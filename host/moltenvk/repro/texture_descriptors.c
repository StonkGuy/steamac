/* STEAMAC-3S/3Z/3J/3F/4B/3K/3G/3R: texel-buffer widths reaching Metal.
 * Run without Vulkan validation: a bad guest view must neither abort the VM nor
 * fail. Under Venus vkCreateBufferView is asynchronous, so a failure is a
 * command-stream error at the view's first use that kills the guest process
 * (Dragon's Dogma 2 at the end of loading a save, KosmicKrisp 0064): a view of
 * zero elements is a null descriptor and an oversized one is narrowed to
 * maxTexelBufferElements. Each case gets its own process so pre-fix Metal
 * assertions name the case.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r = (x); if (r) { fprintf(stderr, "FAIL %s = %d\n", #x, r); exit(1); } } while (0)
static VkInstance instance;
static VkDevice dev;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties mp;
static VkPhysicalDeviceProperties props;

static void setup(void)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CK(vkCreateInstance(&ici, NULL, &instance));
	uint32_t n = 1;
	CK(vkEnumeratePhysicalDevices(instance, &n, &pd));
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	vkGetPhysicalDeviceProperties(pd, &props);
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures(pd, &features);
	float priority = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.pEnabledFeatures = &features };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
}

static void buffer_case(const char *name)
{
	const int sparse = strstr(name, "sparse") != NULL;
	const int zero = strstr(name, "zero") != NULL;
	const int boundary = strstr(name, "boundary") != NULL;
	const int rgb = strstr(name, "rgb") != NULL;
	const int partial = strstr(name, "partial") != NULL;
	const int explicit_range = strstr(name, "explicit") != NULL;
	const int valid = boundary || partial;
	const VkDeviceSize texel = rgb ? 12 : zero ? 8 : partial ? 4 : 1;
	/* Exercise the alignment prefix as well as the advertised maximum. */
	const VkDeviceSize offset = boundary ? (sparse ? 65535 : 12) : partial ? 4 : 0;
	const VkDeviceSize range = boundary ? (VkDeviceSize)props.limits.maxTexelBufferElements * texel :
		explicit_range ? ((VkDeviceSize)props.limits.maxTexelBufferElements + 1) * texel :
		partial ? 11 : zero ? 4 : 536870912;
	if (sparse && !props.limits.sparseAddressSpaceSize) {
		printf("SKIP %s: sparse buffers unsupported\n", name);
		return;
	}
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = offset + range,
		.flags = sparse ? VK_BUFFER_CREATE_SPARSE_BINDING_BIT : 0,
		.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT };
	VkBuffer buffer;
	CK(vkCreateBuffer(dev, &bci, NULL, &buffer));
	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (!sparse) {
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(dev, buffer, &mr);
		uint32_t type = 0;
		while (type < mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << type))) type++;
		if (type == mp.memoryTypeCount) exit(1);
		VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type };
		CK(vkAllocateMemory(dev, &ai, NULL, &memory));
		CK(vkBindBufferMemory(dev, buffer, memory, 0));
	}
	VkBufferViewCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buffer,
		.format = rgb ? VK_FORMAT_R32G32B32_UINT : zero ? VK_FORMAT_R32G32_UINT : partial ? VK_FORMAT_R32_UINT : VK_FORMAT_R8_UINT,
		.offset = offset, .range = explicit_range ? range : VK_WHOLE_SIZE };
	VkBufferView view = VK_NULL_HANDLE;
	VkResult result = vkCreateBufferView(dev, &ci, NULL, &view);
	/* Valid views are created; invalid ones are created too (null or narrowed), never failed */
	if (result != VK_SUCCESS) {
		fprintf(stderr, "FAIL %s returned %d (expected VK_SUCCESS: %s)\n", name, result,
			valid ? "a valid view" : "an invalid view must not fail under Venus");
		exit(1);
	}
	vkDestroyBufferView(dev, view, NULL);
	vkDestroyBuffer(dev, buffer, NULL);
	vkFreeMemory(dev, memory, NULL);
	printf("OK   %s%s (maxTexelBufferElements=%u)\n", name, valid ? "" : " accepted without Metal",
		props.limits.maxTexelBufferElements);
}

int main(int argc, char **argv)
{
	if (argc == 2) {
		setup();
		buffer_case(argv[1]);
		vkDestroyDevice(dev, NULL);
		vkDestroyInstance(instance, NULL);
		return 0;
	}
	const char *cases[] = { "zero", "oversized", "explicit-oversized", "sparse-zero", "sparse-oversized",
		"sparse-explicit-oversized", "boundary", "sparse-boundary", "rgb-boundary", "partial" };
	int failed = 0;
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fflush(NULL);
		pid_t child = fork();
		if (child == 0) { execl(argv[0], argv[0], cases[i], NULL); _exit(127); }
		if (child < 0) return 1;
		int status;
		if (waitpid(child, &status, 0) != child) return 1;
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "FAIL child %s status 0x%x\n", cases[i], status);
			failed++;
		}
	}
	return failed != 0;
}
