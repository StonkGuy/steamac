/*
 * Vertex input layouts that Metal's always-on vertex descriptor validation rejects with an assertion
 * (abort of the whole process, which is the VM): every buffer layout an attribute uses needs a non-zero
 * stride, whatever its step function ("Attribute at index 1 references a buffer at index 29 that has no
 * stride.", STEAMAC-3/X), and per-instance layouts need a non-zero step rate.
 *
 * vi.vert draws one point per vertex (or instance) at location 0 'pos' (binding 0) into a 4x1 target, colored
 * by location 1 'col' (binding 1, 16-byte elements C0, C1, ...): the target shows which element each point
 * read. Each case runs in its own process (a Metal assertion aborts it):
 *
 *   zero_stride            binding 1 stride 0, per vertex: every vertex reads C0
 *   zero_stride_instance   binding 1 stride 0, per instance (4 instances of 1 vertex): C0
 *   zero_divisor           binding 1 stride 16, per instance, divisor 0: C0
 *   dynamic_zero_stride    VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, binding 1 bound with stride 0: C0
 *   dynamic_stride         dynamic stride 16: C0 C1 C2 C3
 *   offset_past_stride     binding 1 stride 16, 'col' at offset 64 (beyond the stride): C4 C5 C6 C7
 *   undescribed_binding    'col' uses binding 1, which has no VkVertexInputBindingDescription (invalid
 *                          usage): pipeline creation must fail, not abort
 *   undescribed_dynamic    the same with dynamic strides
 *   no_bindings            attributes but no binding descriptions at all: must fail, not crash
 *   missing_attribute      only 'pos' described (no attribute or binding 1, invalid usage common in games:
 *                          SCS Truck Simulator, STEAMAC-2Y): Metal rejected the pipeline ("Vertex attribute
 *                          in_var_NORMAL(1) is missing from the vertex descriptor"); 'col' must read zeros
 *   gs_*                   through a passthrough geometry shader (emulated with Metal mesh shaders: the
 *                          object stage fetches the vertices, no Metal vertex descriptor): zero strides,
 *                          static and dynamic, an undescribed binding (the object stage read an unbound
 *                          Metal buffer) and a missing attribute
 *   float*_int*            BLUE REFLECTION (STEAMAC-3N): float vertex formats with integer shader inputs,
 *                          including Float3 -> int4 (Metal supplies w = 1), scalar and truncated inputs
 *   uint_int / int_uint    signedness mismatches, including high-bit values, bits preserved on fetch
 *   uint_float / int_float integer vertex formats with floating-point shader inputs, fetched bits preserved
 *   float_int_array        flattened shader input array, each element converted independently
 *   gs_bgra8 / gs_unorm1010102 / gs_snorm1010102 / gs_rg11b10 / gs_rgb9e5
 *                          packed vertex formats through the geometry shader: the object stage fetched
 *                          them as 0 components ("Invalid component count: 0", pipeline creation failed:
 *                          American Truck Simulator's GL_INT_2_10_10_10_REV normals via zink) or, for
 *                          B8G8R8A8 (DXVK's D3DCOLOR), read float4 from memory; snorm1010102 is the same
 *                          data through Metal's vertex fetch, as a check of the encoding
 *
 *   vertex_input <spv dir> [case]
 */
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

extern char **environ;

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

#define W 4

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue queue;
static VkCommandPool pool;
static const char *dir;

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

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
		if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
			return t;
	printf("FAIL no memory type\n");
	exit(1);
}

static VkBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
	VkBuffer buf;
	CK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VkDeviceMemory mem;
	CK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CK(vkBindBufferMemory(dev, buf, mem, 0));
	CK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, map));
	return buf;
}

static void color(int k, float c[4])
{
	c[0] = 0.0625f * (float)(k + 1);
	c[1] = 0.5f;
	c[2] = 1.0f - 0.0625f * (float)(k + 1);
	c[3] = 1.0f;
}

/* color(k) in a packed format (its values are exact in all of them), 0 for other formats */
static int pack(VkFormat f, const float c[4], uint32_t *w)
{
	uint32_t r, g, b;
	switch (f) {
	case VK_FORMAT_B8G8R8A8_UNORM:
		*w = (uint32_t)(c[2] * 255.0f + 0.5f) | (uint32_t)(c[1] * 255.0f + 0.5f) << 8 |
		     (uint32_t)(c[0] * 255.0f + 0.5f) << 16 | (uint32_t)(c[3] * 255.0f + 0.5f) << 24;
		return 1;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		*w = (uint32_t)(c[0] * 1023.0f + 0.5f) | (uint32_t)(c[1] * 1023.0f + 0.5f) << 10 |
		     (uint32_t)(c[2] * 1023.0f + 0.5f) << 20 | (uint32_t)(c[3] * 3.0f + 0.5f) << 30;
		return 1;
	case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
		*w = ((uint32_t)(int32_t)(c[0] * 511.0f + 0.5f) & 0x3ff) | ((uint32_t)(int32_t)(c[1] * 511.0f + 0.5f) & 0x3ff) << 10 |
		     ((uint32_t)(int32_t)(c[2] * 511.0f + 0.5f) & 0x3ff) << 20 | ((uint32_t)(int32_t)c[3] & 0x3) << 30;
		return 1;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
		/* unsigned 11/10-bit floats: the bits of a half without the sign and low mantissa bits */
		_Float16 h[3] = { (_Float16)c[0], (_Float16)c[1], (_Float16)c[2] };
		uint16_t hb[3];
		memcpy(hb, h, sizeof(hb));
		*w = (uint32_t)(hb[0] >> 4) | (uint32_t)(hb[1] >> 4) << 11 | (uint32_t)(hb[2] >> 5) << 22;
		return 1;
	}
	case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
		/* exponent 16: value = mantissa * 2^(16 - 15 - 9) */
		r = (uint32_t)(c[0] * 256.0f), g = (uint32_t)(c[1] * 256.0f), b = (uint32_t)(c[2] * 256.0f);
		*w = r | g << 9 | b << 18 | 16u << 27;
		return 1;
	default:
		return 0;
	}
}

struct test {
	const char *name;
	VkVertexInputRate rate;			/* of both bindings */
	uint32_t stride1;				/* static stride of binding 1 (dynamic: bound stride) */
	uint32_t offset1;				/* offset of 'col' */
	int dynamic, divisor0, undescribed, no_bindings;
	int missing1;					/* no attribute or binding 1: 'col' reads zeros (expect is ignored) */
	int gs;							/* through vi_points.geom (geometry shader emulation, no Metal vertex descriptor) */
	int expect_fail;
	int expect[W];					/* element read by point i */
	VkFormat format1;				/* zero: the ordinary RGBA32_SFLOAT color input */
	const char *shader;
	int integer_bits;				/* signedness cases use high-bit integer payloads */
	int input_array;				/* second numeric attribute at location 2, same binding */
};

static const struct test tests[] = {
	{ "zero_stride",          VK_VERTEX_INPUT_RATE_VERTEX,   0,  0, .expect = { 0, 0, 0, 0 } },
	{ "zero_stride_instance", VK_VERTEX_INPUT_RATE_INSTANCE, 0,  0, .expect = { 0, 0, 0, 0 } },
	{ "zero_divisor",         VK_VERTEX_INPUT_RATE_INSTANCE, 16, 0, .divisor0 = 1, .expect = { 0, 0, 0, 0 } },
	{ "dynamic_zero_stride",  VK_VERTEX_INPUT_RATE_VERTEX,   0,  0, .dynamic = 1, .expect = { 0, 0, 0, 0 } },
	{ "dynamic_stride",       VK_VERTEX_INPUT_RATE_VERTEX,   16, 0, .dynamic = 1, .expect = { 0, 1, 2, 3 } },
	{ "offset_past_stride",   VK_VERTEX_INPUT_RATE_VERTEX,   16, 64, .expect = { 4, 5, 6, 7 } },
	{ "undescribed_binding",  VK_VERTEX_INPUT_RATE_VERTEX,   16, 0, .undescribed = 1, .expect_fail = 1 },
	{ "undescribed_dynamic",  VK_VERTEX_INPUT_RATE_VERTEX,   16, 0, .dynamic = 1, .undescribed = 1, .expect_fail = 1 },
	{ "no_bindings",          VK_VERTEX_INPUT_RATE_VERTEX,   16, 0, .no_bindings = 1, .expect_fail = 1 },
	{ "missing_attribute",    VK_VERTEX_INPUT_RATE_VERTEX,   16, 0, .missing1 = 1 },
	{ "gs_zero_stride",       VK_VERTEX_INPUT_RATE_VERTEX,   0,  0, .gs = 1, .expect = { 0, 0, 0, 0 } },
	{ "gs_zero_stride_instance", VK_VERTEX_INPUT_RATE_INSTANCE, 0, 0, .gs = 1, .expect = { 0, 0, 0, 0 } },
	{ "gs_dynamic_zero_stride", VK_VERTEX_INPUT_RATE_VERTEX, 0,  0, .gs = 1, .dynamic = 1, .expect = { 0, 0, 0, 0 } },
	{ "gs_dynamic_stride",    VK_VERTEX_INPUT_RATE_VERTEX,   16, 0, .gs = 1, .dynamic = 1, .expect = { 0, 1, 2, 3 } },
	{ "gs_undescribed_binding", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .gs = 1, .undescribed = 1, .expect_fail = 1 },
	{ "gs_missing_attribute", VK_VERTEX_INPUT_RATE_VERTEX,   16, 0, .gs = 1, .missing1 = 1 },
	{ "float3_int4", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32G32B32_SFLOAT, .shader = "vi_int.vert.spv" },
	{ "float4_int4", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32G32B32A32_SFLOAT, .shader = "vi_int.vert.spv" },
	{ "float1_int", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32_SFLOAT, .shader = "vi_int_scalar.vert.spv" },
	{ "float4_int3", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32G32B32A32_SFLOAT, .shader = "vi_int3.vert.spv" },
	{ "uint_int", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32G32B32A32_UINT, .shader = "vi_bits_int.vert.spv", .integer_bits = 1 },
	{ "int_uint", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32G32B32A32_SINT, .shader = "vi_bits_uint.vert.spv", .integer_bits = 1 },
	{ "uint_float", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32G32B32A32_UINT },
	{ "int_float", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32G32B32A32_SINT },
	{ "float_int_array", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_R32G32_SFLOAT, .shader = "vi_int_array.vert.spv", .input_array = 1 },
	{ "snorm1010102", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_A2B10G10R10_SNORM_PACK32 },
	{ "gs_bgra8", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .gs = 1, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_B8G8R8A8_UNORM },
	{ "gs_unorm1010102", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .gs = 1, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
	{ "gs_snorm1010102", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .gs = 1, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_A2B10G10R10_SNORM_PACK32 },
	{ "gs_rg11b10", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .gs = 1, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_B10G11R11_UFLOAT_PACK32 },
	{ "gs_rgb9e5", VK_VERTEX_INPUT_RATE_VERTEX, 16, 0, .gs = 1, .expect = { 0, 1, 2, 3 },
		.format1 = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 },
};
enum { NTESTS = sizeof(tests) / sizeof(tests[0]) };

static int run(const struct test *t)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_4 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no physical device\n"); return 1; }
	VkPhysicalDeviceVulkan14Features sup14 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
	VkPhysicalDeviceFeatures2 sup = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &sup14 };
	vkGetPhysicalDeviceFeatures2(pd, &sup);
	if (t->divisor0 && !sup14.vertexAttributeInstanceRateZeroDivisor) {
		printf("OK   %s: vertexAttributeInstanceRateZeroDivisor not supported, skipped\n", t->name);
		return 0;
	}
	VkPhysicalDeviceVulkan14Features f14 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
		.vertexAttributeInstanceRateDivisor = sup14.vertexAttributeInstanceRateDivisor,
		.vertexAttributeInstanceRateZeroDivisor = sup14.vertexAttributeInstanceRateZeroDivisor };
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f14,
		.features = { .geometryShader = t->gs, .shaderTessellationAndGeometryPointSize = t->gs } };
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));

	/* Render pass: 4x1 RGBA8 target, read back after the pass. */
	VkAttachmentDescription att = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
	VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
		.subpassCount = 1, .pSubpasses = &sub };
	VkRenderPass rp;
	CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { W, 1, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	VkImage img;
	CK(vkCreateImage(dev, &imci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imem;
	CK(vkAllocateMemory(dev, &mai, NULL, &imem));
	CK(vkBindImageMemory(dev, img, imem, 0));
	VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView iv;
	CK(vkCreateImageView(dev, &ivci, NULL, &iv));
	VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
		.pAttachments = &iv, .width = W, .height = 1, .layers = 1 };
	VkFramebuffer fb;
	CK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	/* Pipeline */
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	VkPipelineShaderStageCreateInfo stages[3] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = module(t->shader ? t->shader : "vi.vert.spv"), .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = module("vi.frag.spv"), .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = t->gs ? module("vi_points.geom.spv") : VK_NULL_HANDLE, .pName = "main" },
	};
	VkVertexInputBindingDescription bindings[2] = {
		{ 0, 8, t->rate },
		{ 1, t->stride1, t->rate },
	};
	VkVertexInputAttributeDescription attrs[3] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
		{ 1, 1, t->format1 ? t->format1 : VK_FORMAT_R32G32B32A32_SFLOAT, t->offset1 },
		{ 2, 1, VK_FORMAT_R32G32_SFLOAT, 8 },
	};
	VkVertexInputBindingDivisorDescription div = { 1, 0 };
	VkPipelineVertexInputDivisorStateCreateInfo divci = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO,
		.vertexBindingDivisorCount = 1, .pVertexBindingDivisors = &div };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.pNext = t->divisor0 ? &divci : NULL,
		.vertexBindingDescriptionCount = t->no_bindings ? 0 : t->undescribed || t->missing1 ? 1 : 2, .pVertexBindingDescriptions = bindings,
		.vertexAttributeDescriptionCount = t->missing1 ? 1 : t->input_array ? 3 : 2, .pVertexAttributeDescriptions = attrs };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST };
	VkViewport vp = { 0, 0, W, 1, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, 1 } };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkDynamicState dyn = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
	VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 1, .pDynamicStates = &dyn };
	VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = t->gs ? 3 : 2, .pStages = stages,
		.pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = t->dynamic ? &ds : NULL,
		.layout = layout, .renderPass = rp };
	VkPipeline p = VK_NULL_HANDLE;
	VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &p);
	if (t->expect_fail) {
		printf("%-4s %s: pipeline creation fails cleanly (VkResult %d)\n", r != VK_SUCCESS ? "OK" : "FAIL", t->name, r);
		return r == VK_SUCCESS;
	}
	if (r) { printf("FAIL %s: pipeline creation (VkResult %d)\n", t->name, r); return 1; }

	/* Vertex data: points at the pixel centers, colors C0.. in 16-byte elements. */
	float *pos, *cols;
	uint8_t *px;
	VkBuffer posb = host_buffer(W * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&pos);
	VkBuffer colb = host_buffer(16 * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&cols);
	VkBuffer outb = host_buffer(W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&px);
	for (int i = 0; i < W; i++) {
		pos[2 * i] = (2.0f * (float)i + 1.0f) / (float)W - 1.0f;
		pos[2 * i + 1] = 0.0f;
	}
	for (int k = 0; k < 16; k++) {
		uint32_t w;
		if (t->integer_bits) {
			uint32_t bits[4] = { 0x80000000u | (k + 1), 0xffffffffu, 0x80000000u, 0x7fffffffu };
			memcpy(cols + 4 * k, bits, sizeof(bits));
		} else {
			color(k, cols + 4 * k);
			if (pack(t->format1, cols + 4 * k, &w))
				memcpy(cols + 4 * k, &w, sizeof(w));
		}
	}

	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cmd, &cbbi));
	VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 0 } } };
	VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, 1 } }, .clearValueCount = 1, .pClearValues = &clear };
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	VkBuffer vbs[2] = { posb, colb };
	VkDeviceSize offs[2] = { 0, 0 };
	if (t->dynamic) {
		VkDeviceSize strides[2] = { 8, t->stride1 };
		vkCmdBindVertexBuffers2(cmd, 0, 2, vbs, offs, NULL, strides);
	} else {
		vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
	}
	if (t->rate == VK_VERTEX_INPUT_RATE_INSTANCE)
		vkCmdDraw(cmd, 1, W, 0, 0);
	else
		vkCmdDraw(cmd, W, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, 1, 1 } };
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, outb, 1, &copy);
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	CK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));

	int bad = 0;
	char got[128] = "", want[128] = "";
	for (int i = 0; i < W; i++) {
		size_t gl = strlen(got), wl = strlen(want);
		if (t->missing1) {
			int zero = !px[4 * i] && !px[4 * i + 1] && !px[4 * i + 2] && !px[4 * i + 3];
			bad += !zero;
			snprintf(got + gl, sizeof(got) - gl, " (%u %u %u %u)", px[4 * i], px[4 * i + 1], px[4 * i + 2], px[4 * i + 3]);
			snprintf(want + wl, sizeof(want) - wl, " (0 0 0 0)");
			continue;
		}
		int match = -1;
		for (int k = 0; k < 16 && match < 0; k++) {
			float e[4];
			color(k, e);
			int ok = 1;
			for (int ch = 0; ch < 4; ch++) {
				int d = (int)px[4 * i + ch] - (int)(e[ch] * 255.0f + 0.5f);
				ok &= d >= -1 && d <= 1;
			}
			if (ok) match = k;
		}
		bad += match != t->expect[i];
		if (match >= 0)
			snprintf(got + gl, sizeof(got) - gl, " C%d", match);
		else
			snprintf(got + gl, sizeof(got) - gl, " (%u %u %u %u)", px[4 * i], px[4 * i + 1], px[4 * i + 2], px[4 * i + 3]);
		snprintf(want + wl, sizeof(want) - wl, " C%d", t->expect[i]);
	}
	printf("%-4s %s: points read%s (expected%s)\n", bad ? "FAIL" : "OK", t->name, got, want);
	return bad != 0;
}

int main(int argc, char **argv)
{
	if (argc != 2 && argc != 3) { fprintf(stderr, "usage: %s <spv dir> [case]\n", argv[0]); return 2; }
	dir = argv[1];
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 3) {
		for (int i = 0; i < NTESTS; i++)
			if (!strcmp(argv[2], tests[i].name))
				return run(&tests[i]);
		fprintf(stderr, "unknown case %s\n", argv[2]);
		return 2;
	}
	/* Each case in its own process (a Metal assertion aborts it), spawned rather than forked: the Metal
	 * compiler's XPC connection does not survive fork(). */
	int fails = 0;
	for (int i = 0; i < NTESTS; i++) {
		char *args[] = { argv[0], argv[1], (char *)tests[i].name, NULL };
		pid_t pid;
		int status = 0;
		if (posix_spawn(&pid, argv[0], NULL, NULL, args, environ) || waitpid(pid, &status, 0) != pid) {
			printf("FAIL %s: could not run\n", tests[i].name);
			fails++;
		} else if (WIFSIGNALED(status)) {
			printf("FAIL %s: crashed (signal %d)\n", tests[i].name, WTERMSIG(status));
			fails++;
		} else if (WEXITSTATUS(status)) {
			fails++;
		}
	}
	if (fails) { printf("vertex_input: %d failure(s)\n", fails); return 1; }
	return 0;
}
