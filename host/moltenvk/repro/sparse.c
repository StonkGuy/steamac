/*
 * D3D12 Tiled Resources Tier 2 as used by vkd3d-proton: sparse image/buffer
 * mappings, strict non-resident reads, residency instructions, minimum LOD,
 * aliasing, and sampler min/max reduction. No unsupported feature is enabled.
 * Run: sparse <geometry-spv directory> [image|buffer|minmax|features]
 * Each independent subtest continues after a failure; API errors abandon only
 * that subtest. The process intentionally leaves remaining handles to exit on
 * an API failure (a device-loss path must not prevent the other diagnostics).
 */
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define PAGE 65536ull
#define SIDE 512u
#define LEVELS 10u
#define LAYERS 2u
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue, sparse_queue;
static uint32_t family, sparse_family;
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties memory_props;
static const char *dir;
static int failures, recovering;
static jmp_buf recovery;

static void abort_test(void)
{
	failures++;
	if (recovering) longjmp(recovery, 1);
	exit(1);
}
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); abort_test(); } } while (0)
static void check(int ok, const char *fmt, ...)
{
	printf("%s ", ok ? "OK  " : "FAIL");
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	failures += !ok;
}
static void require(int ok, const char *what)
{
	if (!ok) { printf("FAIL %s\n", what); abort_test(); }
}
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags)
{
	for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	require(0, "no compatible memory type");
	return 0;
}
static VkDeviceMemory allocate(VkDeviceSize size, uint32_t type)
{
	VkMemoryAllocateInfo ci = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = type };
	VkDeviceMemory mem;
	CK(vkAllocateMemory(dev, &ci, NULL, &mem));
	return mem;
}
struct Buffer { VkBuffer buffer; VkDeviceMemory memory; void *map; VkDeviceSize size; int coherent; };
static VkBuffer buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, VkBufferCreateFlags flags)
{
	VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .flags = flags, .size = size,
		.usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkBuffer b;
	CK(vkCreateBuffer(dev, &ci, NULL, &b));
	return b;
}
static struct Buffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
	struct Buffer b = { .size = size, .buffer = buffer_create(size, usage, 0) };
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, b.buffer, &mr);
	uint32_t type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	b.coherent = !!(memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	b.memory = allocate(mr.size, type);
	CK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
	CK(vkMapMemory(dev, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map));
	return b;
}
static void host_sync(struct Buffer *b, int invalidate)
{
	if (b->coherent) return;
	VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->memory, .size = VK_WHOLE_SIZE };
	if (invalidate) CK(vkInvalidateMappedMemoryRanges(dev, 1, &r));
	else CK(vkFlushMappedMemoryRanges(dev, 1, &r));
}
static void buffer_destroy(struct Buffer *b)
{
	vkUnmapMemory(dev, b->memory);
	vkDestroyBuffer(dev, b->buffer, NULL);
	vkFreeMemory(dev, b->memory, NULL);
}
static VkShaderModule module(const char *name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) { printf("FAIL open %s\n", path); abort_test(); }
	require(!fseek(f, 0, SEEK_END), "seek shader");
	long size = ftell(f);
	require(size > 0 && !(size % 4), "invalid SPIR-V size");
	rewind(f);
	uint32_t *code = malloc((size_t)size);
	require(code != NULL, "allocate shader code");
	require(fread(code, 1, (size_t)size, f) == (size_t)size, "read shader");
	fclose(f);
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code };
	VkShaderModule m;
	CK(vkCreateShaderModule(dev, &ci, NULL, &m));
	free(code);
	return m;
}
struct Pipeline { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkDescriptorPool pool; VkDescriptorSet set; VkPipeline pipeline; };
static struct Pipeline pipeline_create(const char *shader, const VkDescriptorType *types, uint32_t count, uint32_t push_size)
{
	struct Pipeline p = {0};
	VkDescriptorSetLayoutBinding bindings[3];
	VkDescriptorPoolSize sizes[3];
	for (uint32_t i = 0; i < count; i++) {
		bindings[i] = (VkDescriptorSetLayoutBinding){ i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
		sizes[i] = (VkDescriptorPoolSize){ types[i], 1 };
	}
	VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = count, .pBindings = bindings };
	CK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &p.dsl));
	VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size };
	VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p.dsl,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &push };
	CK(vkCreatePipelineLayout(dev, &lci, NULL, &p.layout));
	/* Repeated descriptor types are legal pool entries; their counts are summed. */
	VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = count, .pPoolSizes = sizes };
	CK(vkCreateDescriptorPool(dev, &pci, NULL, &p.pool));
	VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p.pool,
		.descriptorSetCount = 1, .pSetLayouts = &p.dsl };
	CK(vkAllocateDescriptorSets(dev, &ai, &p.set));
	VkShaderModule m = module(shader);
	VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main" },
		.layout = p.layout };
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p.pipeline));
	vkDestroyShaderModule(dev, m, NULL);
	return p;
}
static void pipeline_destroy(struct Pipeline *p)
{
	vkDestroyPipeline(dev, p->pipeline, NULL);
	vkDestroyDescriptorPool(dev, p->pool, NULL);
	vkDestroyPipelineLayout(dev, p->layout, NULL);
	vkDestroyDescriptorSetLayout(dev, p->dsl, NULL);
}
static VkCommandBuffer begin(void)
{
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CK(vkAllocateCommandBuffers(dev, &ai, &cmd));
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CK(vkBeginCommandBuffer(cmd, &bi));
	return cmd;
}
static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags src_access,
	VkPipelineStageFlags dst, VkAccessFlags dst_access)
{
	VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = src_access, .dstAccessMask = dst_access };
	vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}
static void submit(VkCommandBuffer cmd, VkSemaphore wait, uint64_t value, int timeline)
{
	CK(vkEndCommandBuffer(cmd));
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkTimelineSemaphoreSubmitInfo t = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value };
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = timeline ? &t : NULL,
		.waitSemaphoreCount = wait != VK_NULL_HANDLE, .pWaitSemaphores = &wait, .pWaitDstStageMask = &stage,
		.commandBufferCount = 1, .pCommandBuffers = &cmd };
	CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));
	vkFreeCommandBuffers(dev, pool, 1, &cmd);
}
static void dispatch(VkCommandBuffer cmd, struct Pipeline *p, const void *push, uint32_t size, uint32_t x, uint32_t y)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &p->set, 0, NULL);
	vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, push);
	vkCmdDispatch(cmd, x, y, 1);
}
static VkSemaphore semaphore(int timeline)
{
	VkSemaphoreTypeCreateInfo t = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
	VkSemaphoreCreateInfo ci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = timeline ? &t : NULL };
	VkSemaphore s;
	CK(vkCreateSemaphore(dev, &ci, NULL, &s));
	return s;
}
static VkImage image_create(VkFormat format, uint32_t width, uint32_t levels, uint32_t layers, int sparse)
{
	VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.flags = sparse ? VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT : 0,
		.imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {width, width, 1}, .mipLevels = levels, .arrayLayers = layers,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkImage image;
	CK(vkCreateImage(dev, &ci, NULL, &image));
	return image;
}
static VkImageView view_create(VkImage image, VkFormat format, uint32_t base, uint32_t levels, uint32_t layers)
{
	VkImageViewCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
		.viewType = layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = format,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, layers } };
	VkImageView v;
	CK(vkCreateImageView(dev, &ci, NULL, &v));
	return v;
}
static void image_barrier(VkCommandBuffer cmd, VkImage image, uint32_t levels, uint32_t layers,
	VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags src, VkAccessFlags src_access,
	VkPipelineStageFlags dst, VkAccessFlags dst_access)
{
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access, .dstAccessMask = dst_access,
		.oldLayout = old_layout, .newLayout = new_layout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers } };
	vkCmdPipelineBarrier(cmd, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}
static VkSampler nearest_sampler(void)
{
	VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = LEVELS };
	VkSampler s;
	CK(vkCreateSampler(dev, &ci, NULL, &s));
	return s;
}
static float pattern(VkFormat format, uint32_t x, uint32_t y, uint32_t level, uint32_t layer, uint32_t component)
{
	if (format == VK_FORMAT_R32_SFLOAT)
		return component == 0 ? (float)(100 + level * 1000 + layer * 10000 + (x % 128) + (y % 128) * 128) : component == 3 ? 1.0f : 0.0f;
	uint32_t v[] = { 17 + level * 19 + layer * 7, 1 + x % 128, 1 + y % 128, 255 };
	return v[component] / 255.0f;
}
struct Result { float fetch[4], sample[4], load[4]; uint32_t resident[4]; };
struct ImagePush { uint32_t width, height, level, layer, mode; float min_lod; };
static int resident(uint32_t x, uint32_t y, uint32_t level, uint32_t layer, uint32_t tail, int state)
{
	/* state 0: initial image, 1: after relocation, 2: alias image (only tile 0,0). */
	if (state == 2) return level == 0 && layer == 0 && x < 128 && y < 128;
	if (level >= tail) return 1;
	if (level == 1) return layer == 0 && x < 128 && y < 128;
	if (level != 0) return 0;
	if (layer == 1) return x / 128 == 2 && y / 128 == 1;
	return (x / 128 == 1 && y / 128 == 1) ||
		(state == 0 ? x / 128 == 0 && y / 128 == 0 : x / 128 == 3 && y / 128 == 3);
}
static float expected(VkFormat format, uint32_t x, uint32_t y, uint32_t level, uint32_t layer, uint32_t c, int bound)
{
	if (bound) return pattern(format, x, y, level, layer, c);
	return format == VK_FORMAT_R32_SFLOAT && c == 3 ? 1.0f : 0.0f;
}
static void image_descriptors(struct Pipeline *p, VkSampler sampler, VkImageView sampled, VkImageView storage, struct Buffer *output)
{
	VkDescriptorImageInfo images[] = { {sampler, sampled, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL} };
	VkDescriptorBufferInfo buffer = { output->buffer, 0, output->size };
	VkWriteDescriptorSet writes[] = {
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->set, .dstBinding = 0, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[0] },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->set, .dstBinding = 1, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &images[1] },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->set, .dstBinding = 2, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer },
	};
	vkUpdateDescriptorSets(dev, ARRAY_SIZE(writes), writes, 0, NULL);
}
static void image_read(struct Pipeline *p, VkFormat format, VkSampler sampler, VkImageView sampled, VkImageView storage,
	struct Buffer *out, uint32_t level, uint32_t layer, uint32_t tail, int state, uint32_t mode, float min_lod)
{
	uint32_t width = mode ? SIDE : SIDE >> level;
	struct ImagePush push = { width, width, level, layer, mode, min_lod };
	image_descriptors(p, sampler, sampled, storage, out);
	memset(out->map, 0xcd, out->size);
	host_sync(out, 0);
	VkCommandBuffer cmd = begin();
	barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
	dispatch(cmd, p, &push, sizeof(push), (width + 7) / 8, (width + 7) / 8);
	barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
	submit(cmd, VK_NULL_HANDLE, 0, 0);
	host_sync(out, 1);
	struct Result *data = out->map;
	uint32_t bad = 0;
	for (uint32_t y = 0; y < width; y++) for (uint32_t x = 0; x < width; x++) {
		struct Result *r = &data[y * width + x];
		uint32_t lev = mode ? (uint32_t)min_lod : level;
		uint32_t sx = mode ? x >> lev : x, sy = mode ? y >> lev : y;
		int bound = resident(sx, sy, lev, layer, tail, state), mismatch = 0;
		float *values[] = {r->fetch, r->sample, r->load};
		for (uint32_t method = 0; method < 3; method++) {
			uint32_t ml = mode && method == 2 ? 0 : lev;
			uint32_t mx = mode && method == 2 ? x : sx, my = mode && method == 2 ? y : sy;
			int mb = resident(mx, my, ml, layer, tail, state);
			if ((!mode || method == 0) && r->resident[method] != (uint32_t)mb) mismatch = 1;
			for (uint32_t c = 0; c < 4; c++) {
				float want = expected(format, mx, my, ml, layer, c, mb);
				if (!isfinite(values[method][c]) || fabsf(values[method][c] - want) > 0.00001f) mismatch = 1;
			}
		}
		if (mismatch && bad++ < 4)
			printf("     (%u,%u) L%u layer%u expected (%g,%g,%g,%g) resident=%d; fetch=(%g,%g,%g,%g)/%u sample=(%g,%g,%g,%g)/%u load=(%g,%g,%g,%g)/%u\n",
				x, y, lev, layer, expected(format,sx,sy,lev,layer,0,bound), expected(format,sx,sy,lev,layer,1,bound),
				expected(format,sx,sy,lev,layer,2,bound), expected(format,sx,sy,lev,layer,3,bound), bound,
				r->fetch[0],r->fetch[1],r->fetch[2],r->fetch[3],r->resident[0], r->sample[0],r->sample[1],r->sample[2],r->sample[3],r->resident[1],
				r->load[0],r->load[1],r->load[2],r->load[3],r->resident[2]);
	}
	check(!bad, "%s %s L%u layer%u state%d: %u texels, %u mismatches%s",
		format == VK_FORMAT_R32_SFLOAT ? "R32" : "RGBA8", mode ? "min-LOD clamp" : state == 2 ? "image alias fetch/sample/imageLoad" : "fetch/sample/imageLoad",
		mode ? (uint32_t)min_lod : level, layer, state, width * width, bad, mode ? " (textureLod(0) control)" : "");
}

static void image_test(VkFormat format)
{
	const char *name = format == VK_FORMAT_R32_SFLOAT ? "R32" : "RGBA8";
	VkPhysicalDeviceSparseImageFormatInfo2 fi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2,
		.format = format, .type = VK_IMAGE_TYPE_2D, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	uint32_t count = 0;
	vkGetPhysicalDeviceSparseImageFormatProperties2(pd, &fi, &count, NULL);
	require(count != 0, "sparse image format has no properties");
	VkSparseImageFormatProperties2 *properties = calloc(count, sizeof(*properties));
	require(properties != NULL, "allocate sparse format properties");
	for (uint32_t i = 0; i < count; i++) properties[i].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
	vkGetPhysicalDeviceSparseImageFormatProperties2(pd, &fi, &count, properties);
	VkSparseImageFormatProperties fp = {0};
	for (uint32_t i = 0; i < count; i++)
		if (properties[i].properties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) fp = properties[i].properties;
	free(properties);
	int standard = fp.imageGranularity.width == 128 && fp.imageGranularity.height == 128 && fp.imageGranularity.depth == 1;
	check(standard && !(fp.flags & (VK_SPARSE_IMAGE_FORMAT_ALIGNED_MIP_SIZE_BIT | VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT)),
		"%s sparse granularity %ux%ux%u flags 0x%x (expected 128x128x1, no aligned/nonstandard)",
		name, fp.imageGranularity.width, fp.imageGranularity.height, fp.imageGranularity.depth, fp.flags);
	/* Binding incorrect shapes would itself be invalid usage, not a useful repro. */
	require(standard, "cannot safely bind the expected standard tiles");
	VkImage image = image_create(format, SIDE, LEVELS, LAYERS, 1);
	VkImage alias = image_create(format, SIDE, LEVELS, LAYERS, 1);
	VkMemoryRequirements mr, alias_mr;
	vkGetImageMemoryRequirements(dev, image, &mr);
	vkGetImageMemoryRequirements(dev, alias, &alias_mr);
	check(mr.alignment == PAGE && mr.memoryTypeBits != 0, "%s memory size=%llu alignment=%llu memoryTypeBits=0x%x",
		name, (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits);
	require(mr.alignment == PAGE, "64 KiB image alignment required for these binds");
	VkImageSparseMemoryRequirementsInfo2 ri = { VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2, .image = image };
	count = 0;
	vkGetImageSparseMemoryRequirements2(dev, &ri, &count, NULL);
	require(count != 0, "image has no sparse memory requirements");
	VkSparseImageMemoryRequirements2 *requirements = calloc(count, sizeof(*requirements));
	require(requirements != NULL, "allocate sparse image requirements");
	for (uint32_t i = 0; i < count; i++) requirements[i].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2;
	vkGetImageSparseMemoryRequirements2(dev, &ri, &count, requirements);
	VkSparseImageMemoryRequirements sr = {0};
	for (uint32_t i = 0; i < count; i++)
		if (requirements[i].memoryRequirements.formatProperties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT)
			sr = requirements[i].memoryRequirements;
	free(requirements);
	uint32_t first = 0;
	while (first < LEVELS && (SIDE >> first) >= fp.imageGranularity.width && (SIDE >> first) >= fp.imageGranularity.height) first++;
	int single = !!(sr.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT);
	int sane = sr.imageMipTailFirstLod == first && sr.imageMipTailSize && !(sr.imageMipTailSize % PAGE) &&
		!(sr.imageMipTailOffset % PAGE) && sr.imageMipTailOffset <= mr.size && sr.imageMipTailSize <= mr.size - sr.imageMipTailOffset &&
		(single || (sr.imageMipTailStride >= sr.imageMipTailSize && !(sr.imageMipTailStride % PAGE) &&
			sr.imageMipTailStride <= mr.size - sr.imageMipTailOffset - sr.imageMipTailSize));
	check(sane, "%s tail first=%u (expected %u), size=%llu offset=%llu stride=%llu single=%d",
		name, sr.imageMipTailFirstLod, first, (unsigned long long)sr.imageMipTailSize,
		(unsigned long long)sr.imageMipTailOffset, (unsigned long long)sr.imageMipTailStride, single);
	check(sr.formatProperties.imageGranularity.width == fp.imageGranularity.width &&
		sr.formatProperties.imageGranularity.height == fp.imageGranularity.height && sr.formatProperties.imageGranularity.depth == 1 &&
		!(sr.formatProperties.flags & (VK_SPARSE_IMAGE_FORMAT_ALIGNED_MIP_SIZE_BIT | VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT)),
		"%s format query and image requirements agree on standard tiles", name);
	require(sane, "unsafe sparse mip tail requirements");
	uint32_t type = memory_type(mr.memoryTypeBits & alias_mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	uint32_t tails = single ? 1 : LAYERS;
	VkDeviceMemory mem = allocate(4 * PAGE + tails * sr.imageMipTailSize, type);
	VkSparseImageMemoryBind tiles[] = {
		{ .subresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0}, .offset = {0,0,0}, .extent = {128,128,1}, .memory = mem, .memoryOffset = 0 },
		{ .subresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0}, .offset = {128,128,0}, .extent = {128,128,1}, .memory = mem, .memoryOffset = PAGE },
		{ .subresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,1}, .offset = {256,128,0}, .extent = {128,128,1}, .memory = mem, .memoryOffset = 2 * PAGE },
		{ .subresource = {VK_IMAGE_ASPECT_COLOR_BIT,1,0}, .offset = {0,0,0}, .extent = {128,128,1}, .memory = mem, .memoryOffset = 3 * PAGE },
	};
	VkSparseMemoryBind tail_binds[LAYERS];
	for (uint32_t i = 0; i < tails; i++) tail_binds[i] = (VkSparseMemoryBind){
		.resourceOffset = sr.imageMipTailOffset + i * sr.imageMipTailStride, .size = sr.imageMipTailSize,
		.memory = mem, .memoryOffset = 4 * PAGE + i * sr.imageMipTailSize };
	VkSparseImageMemoryBindInfo image_binds[] = { {image, ARRAY_SIZE(tiles), tiles}, {alias, 1, &tiles[0]} };
	VkSparseImageOpaqueMemoryBindInfo opaque = {image, tails, tail_binds};
	VkSemaphore ready = semaphore(0);
	VkBindSparseInfo bi = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .imageBindCount = ARRAY_SIZE(image_binds), .pImageBinds = image_binds,
		.imageOpaqueBindCount = 1, .pImageOpaqueBinds = &opaque, .signalSemaphoreCount = 1, .pSignalSemaphores = &ready };
	CK(vkQueueBindSparse(sparse_queue, 1, &bi, VK_NULL_HANDLE));
	check(1, "%s sparse tiles, per-layer tails and alias bound; binary signal consumed by upload", name);
	VkDeviceSize upload_size = 0;
	VkBufferImageCopy copies[LEVELS * LAYERS];
	for (uint32_t layer = 0; layer < LAYERS; layer++) for (uint32_t level = 0; level < LEVELS; level++) {
		uint32_t w = SIDE >> level;
		copies[layer * LEVELS + level] = (VkBufferImageCopy){ .bufferOffset = upload_size,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 }, .imageExtent = {w,w,1} };
		upload_size += w * w * 4;
	}
	struct Buffer upload = host_buffer(upload_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	for (uint32_t layer = 0; layer < LAYERS; layer++) for (uint32_t level = 0; level < LEVELS; level++) {
		uint32_t w = SIDE >> level;
		unsigned char *bytes = (unsigned char *)upload.map + copies[layer * LEVELS + level].bufferOffset;
		for (uint32_t y = 0; y < w; y++) for (uint32_t x = 0; x < w; x++) {
			if (format == VK_FORMAT_R32_SFLOAT) ((float *)bytes)[y * w + x] = pattern(format,x,y,level,layer,0);
			else for (uint32_t c = 0; c < 4; c++) bytes[(y * w + x) * 4 + c] = (unsigned char)lroundf(pattern(format,x,y,level,layer,c) * 255);
		}
	}
	host_sync(&upload, 0);
	VkCommandBuffer cmd = begin();
	/* Both identical aliases are transitioned before any data is written. */
	image_barrier(cmd, image, LEVELS, LAYERS, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	image_barrier(cmd, alias, LEVELS, LAYERS, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	vkCmdCopyBufferToImage(cmd, upload.buffer, image, VK_IMAGE_LAYOUT_GENERAL, ARRAY_SIZE(copies), copies);
	barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	submit(cmd, ready, 0, 0);
	vkDestroySemaphore(dev, ready, NULL);
	buffer_destroy(&upload);
	struct Buffer out = host_buffer(SIDE * SIDE * sizeof(struct Result), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	struct Pipeline p = pipeline_create(format == VK_FORMAT_R32_SFLOAT ? "sparse_image_r32.comp.spv" : "sparse_image.comp.spv", types, 3, sizeof(struct ImagePush));
	VkSampler sampler = nearest_sampler();
	VkImageView sampled = view_create(image, format, 0, LEVELS, LAYERS);
	VkImageView storage[LEVELS];
	for (uint32_t level = 0; level < LEVELS; level++) storage[level] = view_create(image, format, level, 1, LAYERS);
	for (uint32_t level = 0; level < LEVELS; level++) for (uint32_t layer = 0; layer < LAYERS; layer++)
		image_read(&p, format, sampler, sampled, storage[level], &out, level, layer, first, 0, 0, 0);
	for (uint32_t layer = 0; layer < LAYERS; layer++) for (uint32_t min_lod = 1; min_lod <= 2; min_lod++)
		image_read(&p, format, sampler, sampled, storage[0], &out, 0, layer, first, 0, 1, (float)min_lod);
	VkImageView alias_sampled = view_create(alias, format, 0, LEVELS, LAYERS);
	VkImageView alias_storage = view_create(alias, format, 0, 1, LAYERS);
	image_read(&p, format, sampler, alias_sampled, alias_storage, &out, 0, 0, first, 2, 0, 0);
	/* Host signal -> sparse wait/signal -> compute wait exercises timeline binds. */
	VkSemaphore timeline = semaphore(1);
	VkSemaphoreSignalInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = timeline, .value = 1 };
	CK(vkSignalSemaphore(dev, &signal));
	VkSparseImageMemoryBind relocated[] = {tiles[0], tiles[0]};
	relocated[0].memory = VK_NULL_HANDLE;
	relocated[1].offset = (VkOffset3D){384,384,0};
	VkSparseImageMemoryBindInfo rebinding = {image, ARRAY_SIZE(relocated), relocated};
	uint64_t wait_value = 1, signal_value = 2;
	VkTimelineSemaphoreSubmitInfo ts = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &wait_value, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signal_value };
	bi = (VkBindSparseInfo){ VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pNext = &ts,
		.waitSemaphoreCount = 1, .pWaitSemaphores = &timeline, .imageBindCount = 1, .pImageBinds = &rebinding,
		.signalSemaphoreCount = 1, .pSignalSemaphores = &timeline };
	CK(vkQueueBindSparse(sparse_queue, 1, &bi, VK_NULL_HANDLE));
	cmd = begin();
	barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	submit(cmd, timeline, 2, 1);
	check(1, "%s unbind (0,0), reuse the same physical page at (3,3), timeline wait 1/signal 2", name);
	for (uint32_t layer = 0; layer < LAYERS; layer++)
		image_read(&p, format, sampler, sampled, storage[0], &out, 0, layer, first, 1, 0, 0);
	image_read(&p, format, sampler, alias_sampled, alias_storage, &out, 0, 0, first, 2, 0, 0);
	vkDestroySemaphore(dev, timeline, NULL);
	vkDestroyImageView(dev, alias_storage, NULL);
	vkDestroyImageView(dev, alias_sampled, NULL);
	for (uint32_t level = 0; level < LEVELS; level++) vkDestroyImageView(dev, storage[level], NULL);
	vkDestroyImageView(dev, sampled, NULL);
	vkDestroySampler(dev, sampler, NULL);
	pipeline_destroy(&p);
	buffer_destroy(&out);
	vkDestroyImage(dev, alias, NULL);
	vkDestroyImage(dev, image, NULL);
	vkFreeMemory(dev, mem, NULL);
}

static void buffer_test(void)
{
	const VkDeviceSize size = 1024 * 1024;
	VkBuffer sparse = buffer_create(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT);
	VkBuffer alias = buffer_create(PAGE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0);
	VkMemoryRequirements mr, ar;
	vkGetBufferMemoryRequirements(dev, sparse, &mr);
	vkGetBufferMemoryRequirements(dev, alias, &ar);
	check(mr.alignment == PAGE && mr.memoryTypeBits, "sparse buffer size=%llu alignment=%llu memoryTypeBits=0x%x",
		(unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits);
	require(mr.alignment == PAGE && ar.size <= 2 * PAGE, "unsafe sparse buffer/alias alignment or size");
	uint32_t type = memory_type(mr.memoryTypeBits & ar.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory mem = allocate(2 * PAGE, type);
	CK(vkBindBufferMemory(dev, alias, mem, 0));
	VkSparseMemoryBind pages[] = {
		{ .resourceOffset = PAGE, .size = PAGE, .memory = mem, .memoryOffset = 0 },
		{ .resourceOffset = 3 * PAGE, .size = PAGE, .memory = mem, .memoryOffset = PAGE },
	};
	VkSparseBufferMemoryBindInfo binds = {sparse, ARRAY_SIZE(pages), pages};
	VkSemaphore ready = semaphore(0);
	VkBindSparseInfo bi = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 1, .pBufferBinds = &binds,
		.signalSemaphoreCount = 1, .pSignalSemaphores = &ready };
	CK(vkQueueBindSparse(sparse_queue, 1, &bi, VK_NULL_HANDLE));
	struct Buffer out = host_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	struct Pipeline p = pipeline_create("sparse_buffer.comp.spv", types, 2, sizeof(uint32_t));
	VkDescriptorBufferInfo inputs[] = {{sparse,0,size},{out.buffer,0,size}};
	VkWriteDescriptorSet writes[2];
	for (uint32_t i = 0; i < 2; i++) writes[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = p.set, .dstBinding = i, .descriptorCount = 1, .descriptorType = types[i], .pBufferInfo = &inputs[i] };
	vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);
	VkCommandBuffer cmd = begin();
	vkCmdFillBuffer(cmd, sparse, 0, size, 0x13579bdf);
	barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	uint32_t count = (uint32_t)(size / 4);
	dispatch(cmd, &p, &count, sizeof(count), (count + 63) / 64, 1);
	barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
	submit(cmd, ready, 0, 0);
	vkDestroySemaphore(dev, ready, NULL);
	host_sync(&out, 1);
	uint32_t *words = out.map, bad = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t page = (uint32_t)(i * 4ull / PAGE), want = page == 1 || page == 3 ? 0x13579bdf : 0;
		if (words[i] != want && bad++ < 4) printf("     word%u page%u expected 0x%08x got 0x%08x\n", i, page, want, words[i]);
	}
	check(!bad, "sparse buffer: %u words, bound pages 1/3 contain pattern, unbound pages zero; %u mismatches", count, bad);
	inputs[0] = (VkDescriptorBufferInfo){alias,0,PAGE};
	vkUpdateDescriptorSets(dev, 1, &writes[0], 0, NULL);
	cmd = begin();
	barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	count = PAGE / 4;
	dispatch(cmd, &p, &count, sizeof(count), (count + 63) / 64, 1);
	/* Include the original transfer writes to physical pages for vkMapMemory,
	 * not just the shader's writes to the readback buffer. */
	barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
	submit(cmd, VK_NULL_HANDLE, 0, 0);
	host_sync(&out, 1);
	bad = 0;
	for (uint32_t i = 0; i < count; i++) if (words[i] != 0x13579bdf && bad++ < 4)
		printf("     alias word%u expected 0x13579bdf got 0x%08x\n", i, words[i]);
	check(!bad, "plain buffer aliases sparse page 1 at memory offset 0: %u mismatches", bad);
	if (memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
		uint32_t *mapped;
		CK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
		if (!(memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
			VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem, .size = VK_WHOLE_SIZE };
			CK(vkInvalidateMappedMemoryRanges(dev, 1, &r));
		}
		bad = 0;
		for (uint32_t i = 0; i < 2 * PAGE / 4; i++) if (mapped[i] != 0x13579bdf && bad++ < 4)
			printf("     mapped word%u expected 0x13579bdf got 0x%08x\n", i, mapped[i]);
		check(!bad, "host-visible physical pages agree with sparse/plain buffer reads: %u mismatches", bad);
		vkUnmapMemory(dev, mem);
	}
	pipeline_destroy(&p);
	buffer_destroy(&out);
	vkDestroyBuffer(dev, alias, NULL);
	vkDestroyBuffer(dev, sparse, NULL);
	vkFreeMemory(dev, mem, NULL);
}

static float minmax_value(uint32_t x, uint32_t y, uint32_t level)
{
	/* Deliberately neither monotonic nor symmetric; UNORM data is exactly byte/255. */
	static const uint8_t data[] = { 23,201,67,149, 239,41,113,179, 89,251,5,163, 127,53,223,101 };
	static const uint8_t mip[] = { 211,17,137,73 };
	return (level ? mip[y * 2 + x] : data[y * 4 + x]) / 255.0f;
}
static float reduction_expected(float u, float v, float lod, VkSamplerReductionMode mode, VkSamplerMipmapMode mipmap)
{
	uint32_t start = mipmap == VK_SAMPLER_MIPMAP_MODE_NEAREST ? (uint32_t)floorf(lod + 0.5f) : (uint32_t)floorf(lod);
	uint32_t end = mipmap == VK_SAMPLER_MIPMAP_MODE_LINEAR && lod > floorf(lod) ? start + 1 : start;
	float result = mode == VK_SAMPLER_REDUCTION_MODE_MIN ? INFINITY : mode == VK_SAMPLER_REDUCTION_MODE_MAX ? -INFINITY : 0;
	for (uint32_t l = start; l <= end; l++) {
		int w = l ? 2 : 4;
		float fx = u * w - 0.5f, fy = v * w - 0.5f;
		int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
		float tx = fx - floorf(fx), ty = fy - floorf(fy);
		float lw = start == end ? 1 : l == start ? 1 - (lod - floorf(lod)) : lod - floorf(lod);
		for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
			float weight = (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty) * lw;
			if (weight <= 0) continue;
			int x = x0 + dx, y = y0 + dy;
			if (x < 0) x = 0;
			if (y < 0) y = 0;
			if (x >= w) x = w - 1;
			if (y >= w) y = w - 1;
			float a = minmax_value((uint32_t)x, (uint32_t)y, l);
			if (mode == VK_SAMPLER_REDUCTION_MODE_MIN) result = fminf(result, a);
			else if (mode == VK_SAMPLER_REDUCTION_MODE_MAX) result = fmaxf(result, a);
			else result += weight * a;
		}
	}
	return result;
}
static void minmax_test(VkFormat format)
{
	const char *name = format == VK_FORMAT_R32_SFLOAT ? "R32" : "R8";
	VkFormatProperties props;
	vkGetPhysicalDeviceFormatProperties(pd, format, &props);
	require((props.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT)) == (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT), "single-component format lacks linear/minmax sampling support");
	/* R8 storage is not universally supported, and these images are only sampled. */
	VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
		.extent = {4,4,1}, .mipLevels = 2, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
	VkImage image;
	CK(vkCreateImage(dev, &ici, NULL, &image));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, image, &mr);
	VkDeviceMemory mem = allocate(mr.size, memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
	CK(vkBindImageMemory(dev, image, mem, 0));
	uint32_t bpp = format == VK_FORMAT_R32_SFLOAT ? 4 : 1;
	struct Buffer upload = host_buffer(20 * bpp, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	for (uint32_t l = 0; l < 2; l++) {
		uint32_t w = l ? 2 : 4, base = l ? 16 : 0;
		for (uint32_t y = 0; y < w; y++) for (uint32_t x = 0; x < w; x++) {
			float value = minmax_value(x,y,l);
			if (bpp == 4) ((float *)upload.map)[base + y * w + x] = value;
			else ((uint8_t *)upload.map)[base + y * w + x] = (uint8_t)lroundf(value * 255);
		}
	}
	host_sync(&upload, 0);
	VkBufferImageCopy copies[] = {
		{ .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent = {4,4,1} },
		{ .bufferOffset = 16 * bpp, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,1,0,1}, .imageExtent = {2,2,1} },
	};
	VkCommandBuffer cmd = begin();
	image_barrier(cmd, image, 2, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	vkCmdCopyBufferToImage(cmd, upload.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, copies);
	image_barrier(cmd, image, 2, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	submit(cmd, VK_NULL_HANDLE, 0, 0);
	buffer_destroy(&upload);
	VkImageView view = view_create(image, format, 0, 2, 1);
	struct Buffer out = host_buffer(64 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	struct Pipeline p = pipeline_create("sparse_minmax.comp.spv", types, 2, 16);
	VkDescriptorBufferInfo output = {out.buffer,0,out.size};
	VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p.set, .dstBinding = 1,
		.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &output };
	vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);
	struct Sample {float u,v,lod; uint32_t index;};
	/* Exact texel centers also test exclusion of zero-weight neighbors. */
	const struct Sample samples[] = {
		{0.375f,0.375f,0,0}, {0.4375f,0.5625f,0,0}, {0.5f,0.5f,0,0}, {0.0625f,0.9375f,0,0},
		{0.375f,0.375f,1,0}, {0.5f,0.5f,1,0}, {0.25f,0.25f,1,0},
		/* Avoid the implementation-dependent nearest-mip tie at LOD 0.5. */
		{0.375f,0.375f,0.25f,0}, {0.4375f,0.5625f,0.75f,0}, {0.5f,0.5f,0.625f,0},
	};
	const VkSamplerReductionMode modes[] = {VK_SAMPLER_REDUCTION_MODE_MIN,VK_SAMPLER_REDUCTION_MODE_MAX,VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
	const char *mode_names[] = {"MIN","MAX","WEIGHTED_AVERAGE"};
	for (uint32_t m = 0; m < ARRAY_SIZE(modes); m++) for (uint32_t mip = 0; mip < 2; mip++) {
		VkSamplerMipmapMode mm = mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
		VkSamplerReductionModeCreateInfo reduction = { VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, .reductionMode = modes[m] };
		VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = &reduction,
			.magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = mm,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 1 };
		VkSampler sampler;
		CK(vkCreateSampler(dev, &sci, NULL, &sampler));
		VkDescriptorImageInfo input = {sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		write = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p.set, .dstBinding = 0,
			.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &input };
		vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);
		memset(out.map, 0xff, out.size);
		host_sync(&out, 0);
		cmd = begin();
		barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
		for (uint32_t i = 0; i < ARRAY_SIZE(samples); i++) {
			struct Sample push = samples[i];
			push.index = i;
			dispatch(cmd, &p, &push, sizeof(push), 1, 1);
		}
		barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
		submit(cmd, VK_NULL_HANDLE, 0, 0);
		host_sync(&out, 1);
		float *values = out.map;
		uint32_t bad = 0;
		for (uint32_t i = 0; i < ARRAY_SIZE(samples); i++) {
			float want = reduction_expected(samples[i].u,samples[i].v,samples[i].lod,modes[m],mm);
			/* Allows fixed-point interpolation of the weighted-average control, not a wrong extremum. */
			float tolerance = modes[m] == VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE ? 0.003f : 0.00002f;
			if ((!isfinite(values[i]) || fabsf(values[i] - want) > tolerance) && bad++ < 4)
				printf("     uv=(%g,%g) lod=%g expected=%g got=%g\n", samples[i].u,samples[i].v,samples[i].lod,want,values[i]);
		}
		check(!bad, "%s reduction %s, mip %s: %zu samples, %u mismatches", name, mode_names[m], mip ? "LINEAR" : "NEAREST", ARRAY_SIZE(samples), bad);
		vkDestroySampler(dev, sampler, NULL);
	}
	pipeline_destroy(&p);
	buffer_destroy(&out);
	vkDestroyImageView(dev, view, NULL);
	vkDestroyImage(dev, image, NULL);
	vkFreeMemory(dev, mem, NULL);
}

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2],"image") && strcmp(argv[2],"buffer") && strcmp(argv[2],"minmax") && strcmp(argv[2],"features"))) {
		fprintf(stderr, "usage: %s <spv dir> [image|buffer|minmax|features]\n", argv[0]);
		return 2;
	}
	dir = argv[1];
	const char *selector = argc == 3 ? argv[2] : "all";
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance instance;
	CK(vkCreateInstance(&ici, NULL, &instance));
	uint32_t count = 1;
	VkResult r = vkEnumeratePhysicalDevices(instance, &count, &pd);
	require(r >= 0 && count, "no physical device");
	VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
	vkGetPhysicalDeviceFeatures2(pd, &f);
	VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12 };
	vkGetPhysicalDeviceProperties2(pd, &props);
	VkPhysicalDeviceSparseProperties sp = props.properties.sparseProperties;
	printf("device: %s\n", props.properties.deviceName);
#define PRINT_FEATURE(n) printf("     %s=%u\n", #n, f.features.n)
	PRINT_FEATURE(sparseBinding);
	PRINT_FEATURE(sparseResidencyBuffer);
	PRINT_FEATURE(sparseResidencyImage2D);
	PRINT_FEATURE(sparseResidencyAliased);
	PRINT_FEATURE(shaderResourceResidency);
	PRINT_FEATURE(shaderResourceMinLod);
#undef PRINT_FEATURE
	printf("     residencyStandard2DBlockShape=%u\n     residencyNonResidentStrict=%u\n     residencyAlignedMipSize=%u\n",
		sp.residencyStandard2DBlockShape,sp.residencyNonResidentStrict,sp.residencyAlignedMipSize);
	printf("     filterMinmaxSingleComponentFormats=%u\n     samplerFilterMinmax=%u\n     timelineSemaphore=%u\n",
		p12.filterMinmaxSingleComponentFormats,f12.samplerFilterMinmax,f12.timelineSemaphore);
	count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
	VkQueueFamilyProperties *qp = calloc(count, sizeof(*qp));
	require(qp != NULL, "allocate queue properties");
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, qp);
	family = sparse_family = UINT32_MAX;
	for (uint32_t i = 0; i < count; i++) {
		printf("     queue family %u flags=0x%x count=%u sparse=%u\n", i, qp[i].queueFlags, qp[i].queueCount,
			!!(qp[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT));
		if (qp[i].queueCount && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && family == UINT32_MAX) family = i;
		if (qp[i].queueCount && (qp[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && sparse_family == UINT32_MAX) sparse_family = i;
	}
	free(qp);
	int sparse_present = sparse_family != UINT32_MAX;
	int tier1 = f.features.sparseBinding && f.features.sparseResidencyAliased && f.features.sparseResidencyBuffer &&
		f.features.sparseResidencyImage2D && sp.residencyStandard2DBlockShape && sparse_present;
	int tier2 = tier1 && f.features.shaderResourceResidency && f.features.shaderResourceMinLod && !sp.residencyAlignedMipSize &&
		sp.residencyNonResidentStrict && p12.filterMinmaxSingleComponentFormats;
	printf("tiled resources tier %d\n", tier2 ? 2 : tier1 ? 1 : 0);
	struct Gate { const char *name; int ok; } gates[] = {
		{"sparseBinding",f.features.sparseBinding}, {"sparseResidencyBuffer",f.features.sparseResidencyBuffer},
		{"sparseResidencyImage2D",f.features.sparseResidencyImage2D}, {"sparseResidencyAliased",f.features.sparseResidencyAliased},
		{"shaderResourceResidency",f.features.shaderResourceResidency}, {"shaderResourceMinLod",f.features.shaderResourceMinLod},
		{"residencyStandard2DBlockShape",sp.residencyStandard2DBlockShape}, {"residencyNonResidentStrict",sp.residencyNonResidentStrict},
		{"!residencyAlignedMipSize",!sp.residencyAlignedMipSize}, {"filterMinmaxSingleComponentFormats",p12.filterMinmaxSingleComponentFormats},
		{"samplerFilterMinmax",f12.samplerFilterMinmax}, {"VK_QUEUE_SPARSE_BINDING_BIT",sparse_present},
	};
	uint32_t missing = 0;
	for (uint32_t i = 0; i < ARRAY_SIZE(gates); i++) missing += !gates[i].ok;
	printf("%s feature gate", missing ? "FAIL" : "OK  ");
	if (missing) {
		printf(" missing:");
		for (uint32_t i = 0; i < ARRAY_SIZE(gates); i++) if (!gates[i].ok) printf(" %s", gates[i].name);
	}
	putchar('\n');
	failures += !!missing;
	if (!strcmp(selector,"features")) {
		vkDestroyInstance(instance, NULL);
		return failures != 0;
	}
	require(family != UINT32_MAX, "no compute queue");
	float priority = 1;
	VkDeviceQueueCreateInfo queues[] = {
		{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority },
		{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = sparse_family, .queueCount = 1, .pQueuePriorities = &priority },
	};
	/* Enable only features reported by the device; an honest gate failure must
	 * not turn into vkCreateDevice asking for unsupported features. */
	VkPhysicalDeviceFeatures enabled = {
		.sparseBinding = f.features.sparseBinding, .sparseResidencyBuffer = f.features.sparseResidencyBuffer,
		.sparseResidencyImage2D = f.features.sparseResidencyImage2D, .sparseResidencyAliased = f.features.sparseResidencyAliased,
		.shaderResourceResidency = f.features.shaderResourceResidency, .shaderResourceMinLod = f.features.shaderResourceMinLod,
	};
	VkPhysicalDeviceVulkan12Features enabled12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.timelineSemaphore = f12.timelineSemaphore, .samplerFilterMinmax = f12.samplerFilterMinmax };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enabled12, .pEnabledFeatures = &enabled,
		.queueCreateInfoCount = sparse_present && family != sparse_family ? 2 : 1, .pQueueCreateInfos = queues };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, family, 0, &queue);
	if (sparse_present) vkGetDeviceQueue(dev, sparse_family, 0, &sparse_queue);
	vkGetPhysicalDeviceMemoryProperties(pd, &memory_props);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
	CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	recovering = 1;
	int do_image = !strcmp(selector,"all") || !strcmp(selector,"image");
	int do_buffer = !strcmp(selector,"all") || !strcmp(selector,"buffer");
	int do_minmax = !strcmp(selector,"all") || !strcmp(selector,"minmax");
	int image_supported = f.features.sparseBinding && f.features.sparseResidencyImage2D && f.features.sparseResidencyAliased &&
		f.features.shaderResourceResidency && f.features.shaderResourceMinLod && f12.timelineSemaphore && sparse_present;
	if (do_image) {
		if (!image_supported) check(0, "sparse image/alias/min-LOD/timeline tests unavailable (required features above missing)");
		else {
			if (!setjmp(recovery)) image_test(VK_FORMAT_R8G8B8A8_UNORM);
			if (!setjmp(recovery)) image_test(VK_FORMAT_R32_SFLOAT);
		}
	}
	if (do_buffer) {
		if (!(f.features.sparseBinding && f.features.sparseResidencyBuffer && f.features.sparseResidencyAliased && sparse_present))
			check(0, "sparse buffer/alias tests unavailable (required features above missing)");
		else if (!setjmp(recovery)) buffer_test();
	}
	if (do_minmax) {
		if (!(p12.filterMinmaxSingleComponentFormats && f12.samplerFilterMinmax))
			check(0, "sampler MIN/MAX tests unavailable (filterMinmaxSingleComponentFormats or samplerFilterMinmax missing)");
		else {
			if (!setjmp(recovery)) minmax_test(VK_FORMAT_R32_SFLOAT);
			if (!setjmp(recovery)) minmax_test(VK_FORMAT_R8_UNORM);
		}
	}
	printf("sparse: %d failure(s)\n", failures);
	/* As in the other host repros, process exit tears down the device. */
	return failures != 0;
}
