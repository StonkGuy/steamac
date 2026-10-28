/*
 * Texel buffer views at offsets that are not a multiple of 16 bytes (Metal's texture buffer alignment).
 * vkd3d-proton requires single texel alignment (storage/uniformTexelBufferOffsetSingleTexelAlignment); MoltenVK
 * emulates it: the Metal texture starts at the 16-byte aligned offset below the view, and the shader adds the
 * remaining texels, which come from the aux (buffer size) buffer of argument buffer descriptor sets, or from an
 * implicit buffer for sets bound without argument buffers (push descriptors).
 *
 * Source buffer: byte i = i. word(b) = little-endian uint of bytes b..b+3. Robustness2 enabled.
 * 1. tb_read.comp, argument buffer set:
 *      R32_UINT view at 4 (8 texels), R8_UINT view at 3 (10 texels), RGBA8_UINT view at 8, storage R32_UINT view at
 *      byte 20 of a buffer of words 1000 + i (load, store, atomic add, size), an array of R32_UINT views at 68, 72,
 *      76 written one element per update, an array of storage buffers of 16..64 bytes written one element per
 *      update (arrayLength: their sizes are in the same aux buffer), reads past the end of views (bounds of the view,
 *      not of the Metal texture that starts before it): zero, with component substitution for the format, so (0, 0,
 *      0, 0) for RGBA8 (robustBufferAccess2); MoltenVK returns (0, 0, 0, 1).
 * 2. tb_bindless.comp: variable-count update-after-bind array (vkd3d-proton's heaps): element 5 written at 12,
 *    element 6 copied from it with VkCopyDescriptorSet.
 * 3. tb_push.comp: push descriptor set, R32_UINT view at 8; the 256-byte output buffer is written (small buffers with
 *    robustBufferAccess2 were replaced by a temporary copy that lost the writes).
 * 4. tb_rgb32.comp (KosmicKrisp, and drivers with uniform texel buffer features for R32G32B32 formats): vkd3d-proton
 *    views structured buffers of 12-byte strides as R32G32B32_UINT texel buffers without checking format features
 *    (Venus forwards them); KosmicKrisp dereferenced the missing Metal format in vkCreateBufferView and the VM crashed
 *    (STEAMAC-2Z, The Witcher 3). R32G32B32_UINT view at 20 (4 elements), SFLOAT at 264, SINT at 384: texels with
 *    alpha 1, sizes, (0, 0, 0, 1) past the end and at element 0x55555556 (three texels per element would wrap to
 *    texel 2).
 *    KosmicKrisp only: a view of R16G16B16_UINT (no texel buffer features, invalid usage) is created and reads as a
 *    null descriptor.
 *
 * 5. MoltenVK only (STEAMAC-3Q/46/47): unsupported vkCreateBufferView formats must return
 *    VK_ERROR_FORMAT_NOT_SUPPORTED with a null handle, not abort in Metal's
 *    minimumLinearTextureAlignmentForPixelFormat (MTLPixelFormatInvalid), divide by zero,
 *    or accept a depth/compressed/chroma format. Supported controls must still create views.
 *    Run creation alone, without shaders or optional device features:
 *      texel_buffer --format <case>
 *    Cases: rgb32_uint, rgb32_sfloat, rgb32_sint, rgb16_uint, undefined, unknown,
 *           depth, compressed, packed_ycbcr, multiplanar, r32_uint, r8_uint, rgba8_uint.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue queue;
static VkCommandPool pool;
static const char *dir;
static int fails;

static VkShaderModule module(const char *name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) { printf("FAIL open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *code = malloc(n);
	if (fread(code, 1, n, f) != (size_t)n) { printf("FAIL read %s\n", path); exit(1); }
	fclose(f);
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code };
	VkShaderModule m;
	CK(vkCreateShaderModule(dev, &ci, NULL, &m));
	free(code);
	return m;
}

/* Host-visible buffer, mapped. */
static VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
	VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
	VkBuffer b;
	CK(vkCreateBuffer(dev, &ci, NULL, &b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, b, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t t = 0;
	while (!(mr.memoryTypeBits & (1u << t)) || (mp.memoryTypes[t].propertyFlags & want) != want)
		t++;
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
	VkDeviceMemory m;
	CK(vkAllocateMemory(dev, &ai, NULL, &m));
	CK(vkBindBufferMemory(dev, b, m, 0));
	CK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map));
	return b;
}

static VkBufferView view(VkBuffer buf, VkFormat format, VkDeviceSize offset, VkDeviceSize range)
{
	VkBufferViewCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buf, .format = format,
		.offset = offset, .range = range };
	VkBufferView v;
	CK(vkCreateBufferView(dev, &ci, NULL, &v));
	return v;
}

static VkPipeline compute(const char *spv, uint32_t set_count, const VkDescriptorSetLayout *sets, VkPipelineLayout *layout)
{
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = set_count, .pSetLayouts = sets };
	CK(vkCreatePipelineLayout(dev, &plci, NULL, layout));
	VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		           .module = module(spv), .pName = "main" }, .layout = *layout };
	VkPipeline p;
	VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
	printf("%-4s create %s (VkResult %d)\n", r ? "FAIL" : "OK", spv, r);
	if (r) exit(1);
	return p;
}

static VkCommandBuffer begin(void)
{
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CK(vkAllocateCommandBuffers(dev, &ai, &cmd));
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CK(vkBeginCommandBuffer(cmd, &bi));
	return cmd;
}

static void submit(VkCommandBuffer cmd)
{
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	CK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));
}

static uint32_t word(uint32_t b)
{
	return b | (b + 1) << 8 | (b + 2) << 16 | (b + 3) << 24;
}

static void expect(const char *what, const uint32_t *got, const uint32_t *want, uint32_t count)
{
	uint32_t bad = 0;
	for (uint32_t i = 0; i < count; i++)
		if (got[i] != want[i]) {
			if (!bad)
				printf("     %s: [%u] = 0x%08x, want 0x%08x\n", what, i, got[i], want[i]);
			bad++;
		}
	printf("%-4s %s (%u values, %u wrong)\n", bad ? "FAIL" : "OK", what, count, bad);
	fails += bad != 0;
}

static VkDescriptorSetLayout set_layout(const VkDescriptorSetLayoutBinding *b, uint32_t n, const VkDescriptorBindingFlags *flags,
                                        VkDescriptorSetLayoutCreateFlags create_flags)
{
	VkDescriptorSetLayoutBindingFlagsCreateInfo fci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		.bindingCount = n, .pBindingFlags = flags };
	VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, flags ? &fci : NULL,
		.flags = create_flags, .bindingCount = n, .pBindings = b };
	VkDescriptorSetLayout l;
	CK(vkCreateDescriptorSetLayout(dev, &ci, NULL, &l));
	return l;
}

static int format_case(const char *name)
{
	static const struct {
		const char *name;
		VkFormat format;
		VkDeviceSize texel_size;
	} cases[] = {
		{ "rgb32_uint", VK_FORMAT_R32G32B32_UINT, 12 },
		{ "rgb32_sfloat", VK_FORMAT_R32G32B32_SFLOAT, 12 },
		{ "rgb32_sint", VK_FORMAT_R32G32B32_SINT, 12 },
		{ "rgb16_uint", VK_FORMAT_R16G16B16_UINT, 6 },
		{ "undefined", VK_FORMAT_UNDEFINED, 1 },
		{ "unknown", (VkFormat)0x7ffffffe, 1 },
		{ "depth", VK_FORMAT_D32_SFLOAT, 4 },
		{ "compressed", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8 },
		{ "packed_ycbcr", VK_FORMAT_G8B8G8R8_422_UNORM, 4 },
		{ "multiplanar", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 1 },
		{ "r32_uint", VK_FORMAT_R32_UINT, 4 },
		{ "r8_uint", VK_FORMAT_R8_UINT, 1 },
		{ "rgba8_uint", VK_FORMAT_R8G8B8A8_UINT, 4 },
	};
	int found = 0;
	for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		if (!name || !strcmp(name, cases[i].name)) found = 1;
	if (!found) {
		fprintf(stderr, "unknown format case: %s\n", name);
		return 2;
	}

	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
		.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT };
	VkBuffer buf;
	CK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	uint32_t t = 0;
	while (!(mr.memoryTypeBits & (1u << t))) t++;
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
	VkDeviceMemory mem;
	CK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CK(vkBindBufferMemory(dev, buf, mem, 0));
	int bad = 0;
	for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (name && strcmp(name, cases[i].name)) continue;
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(pd, cases[i].format, &props);
		int supported = !!(props.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
		VkResult want = supported ? VK_SUCCESS : VK_ERROR_FORMAT_NOT_SUPPORTED;
		VkBufferViewCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buf,
			.format = cases[i].format, .range = 4 * cases[i].texel_size };
		VkBufferView v = VK_NULL_HANDLE;
		/* Flush so an abort on the baseline identifies the exact format in redirected logs. */
		printf("TEST format %s (%d), buffer features 0x%x, want %d\n",
		       cases[i].name, cases[i].format, props.bufferFeatures, want);
		fflush(stdout);
		VkResult r = vkCreateBufferView(dev, &ci, NULL, &v);
		int ok = r == want && (supported ? v != VK_NULL_HANDLE : v == VK_NULL_HANDLE);
		printf("%-4s format %s: VkResult %d, handle %s\n",
		       ok ? "OK" : "FAIL", cases[i].name, r, v ? "non-null" : "null");
		bad += !ok;
		if (r == VK_SUCCESS && v) vkDestroyBufferView(dev, v, NULL);
	}
	vkDestroyBuffer(dev, buf, NULL);
	vkFreeMemory(dev, mem, NULL);
	return bad != 0;
}

int main(int argc, char **argv)
{
	const char *format = argc == 3 && !strcmp(argv[1], "--format") ? argv[2] : NULL;
	if (argc != 2 && !format) {
		fprintf(stderr, "usage: %s <spv dir> | --format <case>\n", argv[0]);
		return 2;
	}
	dir = argv[1];
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no physical device\n"); return 1; }

	VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	VkPhysicalDeviceVulkan13Properties p13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &drv };
	VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p13 };
	vkGetPhysicalDeviceProperties2(pd, &p2);
	if (format) {
		if (drv.driverID != VK_DRIVER_ID_MOLTENVK) {
			printf("SKIP unsupported-format rejection is a MoltenVK regression\n");
			vkDestroyInstance(inst, NULL);
			return 0;
		}
		float prio = 1;
		VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueCount = 1, .pQueuePriorities = &prio };
		VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
		CK(vkCreateDevice(pd, &dci, NULL, &dev));
		int result = format_case(format);
		vkDestroyDevice(dev, NULL);
		vkDestroyInstance(inst, NULL);
		return result;
	}
	printf("%-4s single texel alignment: storage %u (%llu bytes), uniform %u (%llu bytes)\n",
	       p13.storageTexelBufferOffsetSingleTexelAlignment && p13.uniformTexelBufferOffsetSingleTexelAlignment ? "OK" : "FAIL",
	       p13.storageTexelBufferOffsetSingleTexelAlignment, (unsigned long long)p13.storageTexelBufferOffsetAlignmentBytes,
	       p13.uniformTexelBufferOffsetSingleTexelAlignment, (unsigned long long)p13.uniformTexelBufferOffsetAlignmentBytes);
	if (!p13.storageTexelBufferOffsetSingleTexelAlignment || !p13.uniformTexelBufferOffsetSingleTexelAlignment) return 1;

	VkPhysicalDeviceRobustness2FeaturesEXT rb2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
		.robustBufferAccess2 = VK_TRUE, .robustImageAccess2 = VK_TRUE };
	VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &rb2,
		.descriptorIndexing = VK_TRUE, .runtimeDescriptorArray = VK_TRUE, .descriptorBindingVariableDescriptorCount = VK_TRUE,
		.descriptorBindingPartiallyBound = VK_TRUE, .descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE,
		.shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE, .shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE };
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12,
		.features = { .robustBufferAccess = VK_TRUE, .shaderStorageBufferArrayDynamicIndexing = VK_TRUE } };
	const char *exts[] = { VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = 2, .ppEnabledExtensionNames = exts };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	if (drv.driverID == VK_DRIVER_ID_MOLTENVK) fails += format_case(NULL);
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));

	const VkBufferUsageFlags texel = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
	uint8_t *src;
	uint32_t *wb, *sb, *out;
	VkBuffer srcbuf = buffer(256, texel, (void **)&src);
	VkBuffer wbuf = buffer(256, texel, (void **)&wb);
	VkBuffer sbuf = buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, (void **)&sb);
	VkBuffer obuf = buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, (void **)&out);
	for (uint32_t i = 0; i < 256; i++)
		src[i] = (uint8_t)i;
	for (uint32_t i = 0; i < 64; i++)
		wb[i] = 1000 + i;

	VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 32 }, { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 4 },
	                                 { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 } };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
		.maxSets = 4, .poolSizeCount = 3, .pPoolSizes = sizes };
	VkDescriptorPool dpool;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));

	/* 1. tb_read.comp */
	{
		VkDescriptorSetLayoutBinding b[] = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 4, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		};
		VkDescriptorBindingFlags flags[7] = { [4] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT };
		VkDescriptorSetLayout dsl = set_layout(b, 7, flags, 0);
		VkPipelineLayout layout;
		VkPipeline p = compute("tb_read.comp.spv", 1, &dsl, &layout);
		VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
			.descriptorSetCount = 1, .pSetLayouts = &dsl };
		VkDescriptorSet set;
		CK(vkAllocateDescriptorSets(dev, &dsai, &set));
		VkBufferView views[4] = { view(srcbuf, VK_FORMAT_R32_UINT, 4, 32), view(srcbuf, VK_FORMAT_R8_UINT, 3, 10),
		                          view(srcbuf, VK_FORMAT_R8G8B8A8_UINT, 8, 16), view(wbuf, VK_FORMAT_R32_UINT, 20, 40) };
		for (uint32_t i = 0; i < 4; i++) {
			VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
				.descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
				.pTexelBufferView = &views[i] };
			vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
		}
		for (uint32_t i = 1; i < 4; i++) {
			VkBufferView v = view(srcbuf, VK_FORMAT_R32_UINT, 64 + 4 * i, 16);
			VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 4, .dstArrayElement = i,
				.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &v };
			vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
		}
		for (uint32_t i = 0; i < 4; i++) {
			VkDescriptorBufferInfo bi = { sbuf, 0, 16 * (i + 1) };
			VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 5, .dstArrayElement = i,
				.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
			vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
		}
		VkDescriptorBufferInfo obi = { obuf, 0, VK_WHOLE_SIZE };
		VkWriteDescriptorSet ow = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 6, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &obi };
		vkUpdateDescriptorSets(dev, 1, &ow, 0, NULL);

		memset(out, 0xcd, 256);
		VkCommandBuffer cmd = begin();
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
		vkCmdDispatch(cmd, 1, 1, 1);
		submit(cmd);

		uint32_t want[34];
		for (uint32_t k = 0; k < 8; k++)
			want[k] = word(4 + 4 * k);
		want[8] = 8;
		for (uint32_t k = 0; k < 10; k++)
			want[9 + k] = 3 + k;
		want[19] = 10;
		want[20] = word(12);
		want[21] = 4;
		expect("uniform texel buffers at 4 (R32), 3 (R8), 8 (RGBA8): texels and sizes", out, want, 22);
		want[22] = 1007;
		want[23] = 1009;
		want[24] = 10;
		expect("storage texel buffer at 20 (R32): load, old value of atomic add, size", out + 22, want + 22, 3);
		uint32_t wwant[16];
		for (uint32_t i = 0; i < 16; i++)
			wwant[i] = 1000 + i;
		wwant[8] = 0xabcd0003u;
		wwant[9] = 1009 + 0x100;
		expect("storage texel buffer at 20: store and atomic at their words, other words untouched", wb, wwant, 16);
		for (uint32_t i = 1; i < 4; i++)
			want[24 + i] = word(64 + 4 * i);
		expect("array of texel buffers at 68, 72, 76 written one element per update", out + 25, want + 25, 3);
		for (uint32_t i = 0; i < 4; i++)
			want[28 + i] = 4 * (i + 1);
		expect("array of storage buffers written one element per update: arrayLength 4, 8, 12, 16", out + 28, want + 28, 4);
		want[32] = 0;
		want[33] = drv.driverID == VK_DRIVER_ID_MOLTENVK;
		expect(want[33] ? "texels past the end of views at 4 (R32, texel 8) and 8 (RGBA8, texel 4): x 0, alpha 1 (MoltenVK)"
		                : "texels past the end of views at 4 (R32, texel 8) and 8 (RGBA8, texel 4): x 0, alpha 0",
		       out + 32, want + 32, 2);
	}

	/* 2. tb_bindless.comp */
	{
		VkDescriptorSetLayoutBinding ob = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
		VkDescriptorSetLayoutBinding hb = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 16, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
		VkDescriptorBindingFlags hflags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		                                  VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
		VkDescriptorSetLayout dsl[2] = { set_layout(&ob, 1, NULL, 0),
		                                 set_layout(&hb, 1, &hflags, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) };
		VkPipelineLayout layout;
		VkPipeline p = compute("tb_bindless.comp.spv", 2, dsl, &layout);
		uint32_t var = 8;
		VkDescriptorSetVariableDescriptorCountAllocateInfo vai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
			.descriptorSetCount = 2, .pDescriptorCounts = (uint32_t[]){ 0, var } };
		VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vai, .descriptorPool = dpool,
			.descriptorSetCount = 2, .pSetLayouts = dsl };
		VkDescriptorSet sets[2];
		CK(vkAllocateDescriptorSets(dev, &dsai, sets));
		VkDescriptorBufferInfo obi = { obuf, 0, VK_WHOLE_SIZE };
		VkBufferView v = view(srcbuf, VK_FORMAT_R32_UINT, 12, 16);
		VkWriteDescriptorSet w[2] = {
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &obi },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[1], .dstBinding = 0, .dstArrayElement = 5, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &v },
		};
		VkCopyDescriptorSet c = { VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = sets[1], .srcBinding = 0, .srcArrayElement = 5,
			.dstSet = sets[1], .dstBinding = 0, .dstArrayElement = 6, .descriptorCount = 1 };
		vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
		vkUpdateDescriptorSets(dev, 0, NULL, 1, &c);

		memset(out, 0xcd, 256);
		VkCommandBuffer cmd = begin();
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 0, NULL);
		vkCmdDispatch(cmd, 1, 1, 1);
		submit(cmd);
		uint32_t want[4] = { word(12), 4, word(16), 4 };
		expect("variable-count update-after-bind array: view at 12 in element 5, copied to element 6", out, want, 4);
	}

	/* 3. tb_push.comp */
	{
		VkDescriptorSetLayoutBinding b[] = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		};
		VkDescriptorSetLayout dsl = set_layout(b, 2, NULL, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT);
		VkPipelineLayout layout;
		VkPipeline p = compute("tb_push.comp.spv", 1, &dsl, &layout);
		PFN_vkCmdPushDescriptorSetKHR push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
		VkBufferView v = view(srcbuf, VK_FORMAT_R32_UINT, 8, 16);
		VkDescriptorBufferInfo obi = { obuf, 0, VK_WHOLE_SIZE };
		VkWriteDescriptorSet w[2] = {
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &v },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &obi },
		};
		memset(out, 0xcd, 256);
		VkCommandBuffer cmd = begin();
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
		push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, w);
		vkCmdDispatch(cmd, 1, 1, 1);
		submit(cmd);
		uint32_t want[3] = { word(8), word(16), 4 };
		expect("push descriptor set: view at 8", out, want, 3);
	}

	/* 4. tb_rgb32.comp */
	{
		const VkFormat rgb32[3] = { VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SINT };
		int supported = 1;
		for (uint32_t i = 0; i < 3; i++) {
			VkFormatProperties fp;
			vkGetPhysicalDeviceFormatProperties(pd, rgb32[i], &fp);
			supported &= !!(fp.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
		}
		const int kk = drv.driverID == VK_DRIVER_ID_MESA_KOSMICKRISP;
		if (kk && !supported) {
			printf("FAIL R32G32B32 formats have no uniform texel buffer feature\n");
			fails++;
		}
		if (!supported && !kk) {
			printf("SKIP R32G32B32 uniform texel buffers (no uniform texel buffer feature)\n");
		} else {
			uint32_t *rw;
			VkBuffer rbuf = buffer(512, texel, (void **)&rw);
			for (uint32_t i = 0; i < 64; i++)
				rw[i] = i;
			for (uint32_t i = 0; i < 32; i++) {
				const float v = i + 0.5f;
				memcpy(&rw[64 + i], &v, 4);
				rw[96 + i] = (uint32_t)-(int32_t)(i + 1);
			}
			VkDescriptorSetLayoutBinding b[5];
			for (uint32_t i = 0; i < 5; i++)
				b[i] = (VkDescriptorSetLayoutBinding){ i, i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
				                                       1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
			VkDescriptorBindingFlags flags[5] = { [3] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT };
			VkDescriptorSetLayout dsl = set_layout(b, 5, flags, 0);
			VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
			VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
				.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
			VkPipelineLayout layout;
			CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
			VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
				.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
				           .module = module("tb_rgb32.comp.spv"), .pName = "main" }, .layout = layout };
			VkPipeline p;
			VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
			printf("%-4s create tb_rgb32.comp.spv (VkResult %d)\n", r ? "FAIL" : "OK", r);
			if (r) return 1;
			VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
				.descriptorSetCount = 1, .pSetLayouts = &dsl };
			VkDescriptorSet set;
			CK(vkAllocateDescriptorSets(dev, &dsai, &set));
			VkBufferView views[4] = { view(rbuf, rgb32[0], 20, 48), view(rbuf, rgb32[1], 264, 48), view(rbuf, rgb32[2], 384, 48),
			                          kk ? view(rbuf, VK_FORMAT_R16G16B16_UINT, 0, 48) : VK_NULL_HANDLE };
			printf("OK   views of R32G32B32_UINT/SFLOAT/SINT%s created\n", kk ? " and R16G16B16_UINT (invalid usage)" : "");
			for (uint32_t i = 0; i < (kk ? 4u : 3u); i++) {
				VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &views[i] };
				vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
			}
			VkDescriptorBufferInfo obi = { obuf, 0, VK_WHOLE_SIZE };
			VkWriteDescriptorSet ow = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 4, .descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &obi };
			vkUpdateDescriptorSets(dev, 1, &ow, 0, NULL);

			memset(out, 0xcd, 256);
			const int32_t pc[2] = { 0x55555556, kk };
			VkCommandBuffer cmd = begin();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
			vkCmdDispatch(cmd, 1, 1, 1);
			submit(cmd);

			uint32_t want[36];
			const float one = 1.0f;
			for (uint32_t k = 0; k < 4; k++) {
				for (uint32_t c = 0; c < 3; c++)
					want[4 * k + c] = 5 + 3 * k + c;
				want[4 * k + 3] = 1;
			}
			want[16] = 4;
			expect("R32G32B32_UINT view at 20: elements 0-3 (words 5-16, alpha 1), size", out, want, 17);
			for (uint32_t c = 0; c < 3; c++) {
				const float v = 5 + c + 0.5f;
				memcpy(&want[17 + c], &v, 4);
			}
			memcpy(&want[20], &one, 4);
			want[21] = 4;
			expect("R32G32B32_SFLOAT view at 264: element 1 (5.5, 6.5, 7.5, 1.0), size", out + 17, want + 17, 5);
			want[22] = (uint32_t)-7;
			want[23] = (uint32_t)-8;
			want[24] = (uint32_t)-9;
			want[25] = 1;
			expect("R32G32B32_SINT view at 384: element 2 (-7, -8, -9, 1)", out + 22, want + 22, 4);
			for (uint32_t i = 26; i < 34; i++)
				want[i] = i == 29 || i == 33;
			expect("R32G32B32_UINT elements 4 (past the end) and 0x55555556: (0, 0, 0, 1)", out + 26, want + 26, 8);
			if (kk) {
				want[34] = 0;
				want[35] = 0;
				expect("R16G16B16_UINT view (no texel buffer features): null descriptor (texel 0, size 0)", out + 34, want + 34, 2);
			}
		}
	}

	if (fails) { printf("texel_buffer: %d failure(s)\n", fails); return 1; }
	return 0;
}
