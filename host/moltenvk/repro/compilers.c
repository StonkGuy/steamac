/*
 * KosmicKrisp only (STEAMAC-31/39): Vulkan devices created at once on several threads, each with an instance of its
 * own (virglrenderer: one instance and device per Venus context, a ring thread each), then compiling compute
 * pipelines at the same time, must share one MTL4Compiler (KK_WORKAROUND_11). With a compiler per device, concurrent
 * compiles on several compilers corrupted the heap (FB22683138, fixed in macOS 27 beta 5): KosmicKrisp turned the
 * workaround off from macOS 27.0.0, which betas 1-4 report too, and virglrenderer crashed in libmalloc's heap
 * corruption trap under -[MTL4Compiler newComputePipelineStateWithDescriptor:...] when a guest session started
 * (beta 1 on an M4 Pro, beta 4 on an M3 Max).
 *
 * The child process logs its Metal compiles (MESA_KK_DEBUG=compiles) on stderr: one MTL4Compiler must be created and
 * every compile must use it, while compiles still overlap on it (the shared compiler compiles concurrently). While a
 * compile runs, its thread is named after it ("kk <kind> <MSL hashes> compiler <n>: <k> in flight on <m>", what crash
 * reports show of every thread): a watcher thread must see such a name.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

#define THREADS   8u
#define PIPELINES 16u

static const char *dir;
static uint32_t salt;
static atomic_uint finished;

static struct {
	pthread_mutex_t m;
	pthread_cond_t c;
	unsigned count, gen;
} bar = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 };

/* All workers reach each step together, like contexts created in the same moment of a session start */
static void barrier(void)
{
	pthread_mutex_lock(&bar.m);
	unsigned gen = bar.gen;
	if (++bar.count == THREADS) {
		bar.count = 0;
		bar.gen++;
		pthread_cond_broadcast(&bar.c);
	} else {
		while (gen == bar.gen)
			pthread_cond_wait(&bar.c, &bar.m);
	}
	pthread_mutex_unlock(&bar.m);
}

static void *worker(void *arg)
{
	uint32_t tid = (uint32_t)(uintptr_t)arg;
	barrier();
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	VkPhysicalDevice pd;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no physical device\n"); exit(1); }
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev;
	barrier();
	CK(vkCreateDevice(pd, &dci, NULL, &dev));

	char path[1024];
	snprintf(path, sizeof(path), "%s/compilers.comp.spv", dir);
	FILE *f = fopen(path, "rb");
	if (!f) { printf("FAIL open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *code = malloc(size);
	if (fread(code, 1, size, f) != (size_t)size) { printf("FAIL read %s\n", path); exit(1); }
	fclose(f);
	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
	VkShaderModule module;
	CK(vkCreateShaderModule(dev, &smci, NULL, &module));
	free(code);
	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));

	for (uint32_t i = 0; i < PIPELINES; i++) {
		/* A constant no earlier run used: a new Metal compile, not one from Metal's shader cache */
		uint32_t k = salt + tid * PIPELINES + i;
		VkSpecializationMapEntry me = { 0, 0, sizeof(k) };
		VkSpecializationInfo si = { 1, &me, sizeof(k), &k };
		VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
				   .module = module, .pName = "main", .pSpecializationInfo = &si },
			.layout = layout };
		VkPipeline pipeline;
		barrier();
		CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
		vkDestroyPipeline(dev, pipeline, NULL);
	}
	vkDestroyPipelineLayout(dev, layout, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	vkDestroyShaderModule(dev, module, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	atomic_fetch_add(&finished, 1u);
	return NULL;
}

static int child(void)
{
	pthread_t t[THREADS];
	for (uint32_t i = 0; i < THREADS; i++)
		pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)i);
	/* Sample the workers' names while they compile */
	char seen[64] = "";
	while (!seen[0] && atomic_load(&finished) < THREADS) {
		for (uint32_t i = 0; i < THREADS && !seen[0]; i++) {
			char name[64];
			if (pthread_getname_np(t[i], name, sizeof(name)) == 0 && !strncmp(name, "kk compute ", 11))
				snprintf(seen, sizeof(seen), "%s", name);
		}
		if (!seen[0])
			usleep(100);
	}
	for (uint32_t i = 0; i < THREADS; i++)
		pthread_join(t[i], NULL);
	/* The parent reads this from stdout */
	printf("thread name: %s\n", seen[0] ? seen : "(none seen)");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: %s <spv dir> [child]\n", argv[0]); return 2; }
	dir = argv[1];
	if (argc > 2 && !strcmp(argv[2], "child")) {
		salt = (uint32_t)strtoul(argv[3], NULL, 10);
		return child();
	}

	char err_path[] = "/tmp/compilers.XXXXXX", out_path[] = "/tmp/compilers-out.XXXXXX";
	int fd = mkstemp(err_path), fd2 = mkstemp(out_path);
	if (fd < 0 || fd2 < 0) { printf("FAIL mkstemp\n"); return 1; }
	close(fd);
	close(fd2);
	char cmd[8192];
	snprintf(cmd, sizeof(cmd), "MESA_KK_DEBUG=compiles '%s' '%s' child %u > '%s' 2> '%s'", argv[0], argv[1],
		 arc4random() & 0x3fffffffu, out_path, err_path);
	fflush(stdout);
	int status = system(cmd);
	unsigned created = 0, compiles = 0, other = 0, peak = 0, peak_compilers = 0, kernels = 0;
	char line[4096], name[256] = "";
	FILE *f = fopen(err_path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		unsigned serial, k, m, up, on;
		if (sscanf(line, "kk: MTL4Compiler %u created", &serial) == 1) {
			created++;
			continue;
		}
		const char *with = strstr(line, " with MTL4Compiler ");
		const char *upto = strstr(line, ", up to ");
		if (strncmp(line, "kk: compile ", 12) || !with || !upto || sscanf(with, " with MTL4Compiler %u", &serial) != 1 ||
		    sscanf(strstr(line, "; "), "; %u compiles in flight at its start on %u compilers", &k, &m) != 2 ||
		    sscanf(upto, ", up to %u on %u", &up, &on) != 2)
			continue;
		compiles++;
		other += serial != 1;
		kernels += !strncmp(line, "kk: compile compute ", 20);
		if (up > peak)
			peak = up;
		if (on > peak_compilers)
			peak_compilers = on;
	}
	if (f)
		fclose(f);
	f = fopen(out_path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "thread name: ", 13)) {
			line[strcspn(line, "\n")] = '\0';
			snprintf(name, sizeof(name), "%s", line + 13);
		}
	}
	if (f)
		fclose(f);
	unlink(err_path);
	unlink(out_path);
	int fails = status != 0;
	if (status)
		printf("FAIL child exit status %d\n", status);
	int shared = created == 1 && other == 0 && kernels >= THREADS * PIPELINES && peak_compilers == 1;
	printf("%-4s %u devices created at once share one MTL4Compiler (MESA_KK_DEBUG=compiles: %u created, %u compiles, "
	       "%u of them on another compiler, at most %u compilers in flight at once)\n",
	       shared ? "OK" : "FAIL", THREADS, created, compiles, other, peak_compilers);
	fails += !shared;
	printf("%-4s compiles overlap on the shared compiler (up to %u in flight at once)\n", peak >= 2 ? "OK" : "FAIL", peak);
	fails += peak < 2;
	int named = !strncmp(name, "kk compute ", 11);
	printf("%-4s a compiling thread is named after its compile (\"%s\")\n", named ? "OK" : "FAIL", name);
	fails += !named;
	if (fails) { printf("compilers: %d failure(s)\n", fails); return 1; }
	return 0;
}
