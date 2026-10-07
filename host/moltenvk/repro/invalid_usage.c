/*
 * Inputs MoltenVK does not advertise support for, which still reach it from guest drivers through Venus,
 * and which must neither crash the renderer nor fail the pipeline:
 *
 * 1. VK_NULL_HANDLE in VkPipelineLayoutCreateInfo::pSetLayouts (legal with graphics pipeline libraries and
 *    VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT): MVKPipelineLayout::Create dereferenced it
 *    (SIGSEGV at address 0x104 on the virglrenderer ring thread, STEAMAC-6). The null sets act as empty
 *    set layouts: a compute shader that only uses set 1 must run with set 0 null, and set 1 null is
 *    accepted too.
 * 2. rasterizationSamples = 8 on Apple GPUs (sample counts 1, 2, 4): the Metal pipeline failed with
 *    "rasterSampleCount (8) is not supported by device" (STEAMAC-F). The pipeline is created with the
 *    largest supported count, and draws with it (render pass without attachments, and with a 4-sample
 *    color attachment) must pass Metal validation and invoke the fragment shader exactly as with 4 samples.
 * 3. VK_NULL_HANDLE in vkCmdBindDescriptorSets::pDescriptorSets (also legal with graphics pipeline libraries):
 *    Counter-Strike 2 binds five sets with the fourth null, and MoltenVK dereferenced it when the command
 *    buffer was submitted (SIGSEGV at address 0x3c in bindDescriptorSets on the virglrenderer ring thread,
 *    STEAMAC-25). A null set binds nothing and takes no dynamic offsets: compute and graphics pipelines using
 *    sets 0 and 4 of five sets with a dynamic storage buffer each must write their own buffer ranges.
 *
 *   invalid_usage <spv dir>
 *   invalid_usage <spv dir> msl-log   a compute shader whose MSL does not compile (double): the pipeline must
 *                                     fail, and MoltenVK logs the MSL as "[mvk-msl] " lines (run.sh checks)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

#define W 16
#define H 16

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

static VkBuffer host_buffer(VkDeviceSize size, uint32_t **map)
{
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buf;
	CK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VkDeviceMemory mem;
	CK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CK(vkBindBufferMemory(dev, buf, mem, 0));
	CK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)map));
	return buf;
}

static VkDescriptorSet storage_set(VkDescriptorSetLayout dsl, VkBuffer buf)
{
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dpool;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet set;
	CK(vkAllocateDescriptorSets(dev, &dsai, &set));
	VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
	vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
	return set;
}

static VkCommandBuffer begin(void)
{
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cmd, &cbbi));
	return cmd;
}

static void submit(VkCommandBuffer cmd)
{
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	CK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));
	vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

static void null_set_layouts(void)
{
	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

	/* set 1 null */
	VkDescriptorSetLayout set1_null[2] = { dsl, VK_NULL_HANDLE };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 2, .pSetLayouts = set1_null };
	VkPipelineLayout layout;
	VkResult r = vkCreatePipelineLayout(dev, &plci, NULL, &layout);
	printf("%-4s pipeline layout { set layout, VK_NULL_HANDLE } (VkResult %d)\n", r ? "FAIL" : "OK", r);
	fails += r != VK_SUCCESS;
	if (!r)
		vkDestroyPipelineLayout(dev, layout, NULL);

	/* set 0 null, the shader uses set 1 */
	VkDescriptorSetLayout set0_null[2] = { VK_NULL_HANDLE, dsl };
	plci.pSetLayouts = set0_null;
	r = vkCreatePipelineLayout(dev, &plci, NULL, &layout);
	printf("%-4s pipeline layout { VK_NULL_HANDLE, set layout } (VkResult %d)\n", r ? "FAIL" : "OK", r);
	if (r) { fails++; return; }
	/* The set layout may go away, the pipeline layout keeps what it needs. */
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

	VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		           .module = module("null_set.comp.spv"), .pName = "main" }, .layout = layout };
	VkPipeline p;
	r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
	printf("%-4s compute pipeline using set 1 of { VK_NULL_HANDLE, set layout } (VkResult %d)\n", r ? "FAIL" : "OK", r);
	if (r) { fails++; return; }

	uint32_t *map;
	VkBuffer buf = host_buffer(256, &map);
	memset(map, 0, 256);
	VkDescriptorSet set = storage_set(dsl, buf);
	VkCommandBuffer cmd = begin();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 1, 1, &set, 0, NULL);
	vkCmdDispatch(cmd, 1, 1, 1);
	submit(cmd);
	printf("%-4s dispatch with set 1 bound: o[0] = %u (expected 42)\n", map[0] == 42 ? "OK" : "FAIL", map[0]);
	fails += map[0] != 42;
}

static void null_set_binds(void)
{
	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1,
		VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkDescriptorSetLayout dsls[5] = { dsl, dsl, dsl, dsl, dsl };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 5, .pSetLayouts = dsls };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));

	/* Sets 0, 1, 2 and 4 all point at buf; their dynamic offsets select 1 KiB ranges 0-3 of it. */
	uint32_t *map;
	VkBuffer buf = host_buffer(4096, &map);
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 4 };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dpool;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
		.descriptorSetCount = 4, .pSetLayouts = dsls };
	VkDescriptorSet s[4];
	CK(vkAllocateDescriptorSets(dev, &dsai, s));
	VkDescriptorBufferInfo dbi = { buf, 0, 256 };
	for (int i = 0; i < 4; i++) {
		VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s[i], .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &dbi };
		vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
	}
	VkDescriptorSet sets[5] = { s[0], s[1], s[2], VK_NULL_HANDLE, s[3] };
	const uint32_t offsets[4] = { 0, 1024, 2048, 3072 };

	/* Compute: set 0 writes 10 to range 0, set 4 writes 40 to range 3. */
	VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		           .module = module("null_set_bind.comp.spv"), .pName = "main" }, .layout = layout };
	VkPipeline cp;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &cp));
	memset(map, 0, 4096);
	VkCommandBuffer cmd = begin();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 5, sets, 4, offsets);
	vkCmdDispatch(cmd, 1, 1, 1);
	submit(cmd);
	int ok = map[0] == 10 && map[256] == 0 && map[512] == 0 && map[768] == 40;
	printf("%-4s dispatch with sets { s0, s1, s2, VK_NULL_HANDLE, s4 }: ranges %u %u %u %u (expected 10 0 0 40)\n",
	       ok ? "OK" : "FAIL", map[0], map[256], map[512], map[768]);
	fails += !ok;

	/* Graphics, render pass without attachments: one count per pixel through set 0, two through set 4. */
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS };
	VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .subpassCount = 1, .pSubpasses = &sub };
	VkRenderPass rp;
	CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fb;
	CK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = module("msaa.vert.spv"), .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = module("null_set_bind.frag.spv"), .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
		.pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &cb, .layout = layout, .renderPass = rp };
	VkPipeline gp;
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &gp));
	memset(map, 0, 4096);
	cmd = begin();
	VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } } };
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 5, sets, 4, offsets);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	submit(cmd);
	ok = map[0] == W * H && map[256] == 0 && map[512] == 0 && map[768] == 2 * W * H;
	printf("%-4s draw with sets { s0, s1, s2, VK_NULL_HANDLE, s4 }: ranges %u %u %u %u (expected %d 0 0 %d)\n",
	       ok ? "OK" : "FAIL", map[0], map[256], map[512], map[768], W * H, 2 * W * H);
	fails += !ok;
}

static void raster_samples(void)
{
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("     sample counts: framebuffer color 0x%x depth 0x%x no-attachments 0x%x, sampled image color 0x%x\n",
	       props.limits.framebufferColorSampleCounts, props.limits.framebufferDepthSampleCounts,
	       props.limits.framebufferNoAttachmentsSampleCounts, props.limits.sampledImageColorSampleCounts);
	if (props.limits.framebufferNoAttachmentsSampleCounts & VK_SAMPLE_COUNT_8_BIT) {
		printf("     8 samples supported by this device, nothing to clamp\n");
		return;
	}

	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	uint32_t *count;
	VkBuffer buf = host_buffer(256, &count);
	VkDescriptorSet set = storage_set(dsl, buf);
	VkShaderModule vs = module("msaa.vert.spv"), fs = module("msaa.frag.spv");

	for (int with_color = 0; with_color < 2; with_color++) {
		/* Render pass without attachments, or with a 4-sample color attachment (what 8 is clamped to). */
		VkAttachmentDescription att = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = (uint32_t)with_color, .pColorAttachments = &ref };
		VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = (uint32_t)with_color,
			.pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
		VkRenderPass rp;
		CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));

		VkImageView iv = VK_NULL_HANDLE;
		if (with_color) {
			VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
				.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_4_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
			VkImage img;
			CK(vkCreateImage(dev, &ici, NULL, &img));
			VkMemoryRequirements mr;
			vkGetImageMemoryRequirements(dev, img, &mr);
			VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
				.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
			VkDeviceMemory mem;
			CK(vkAllocateMemory(dev, &mai, NULL, &mem));
			CK(vkBindImageMemory(dev, img, mem, 0));
			VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
			CK(vkCreateImageView(dev, &ivci, NULL, &iv));
		}
		VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
			.attachmentCount = (uint32_t)with_color, .pAttachments = &iv, .width = W, .height = H, .layers = 1 };
		VkFramebuffer fb;
		CK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

		VkPipelineShaderStageCreateInfo stages[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
		};
		VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		VkViewport vp = { 0, 0, W, H, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { W, H } };
		VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
		VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
		VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
		VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = (uint32_t)with_color, .pAttachments = &cba };
		VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
			.pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
			.pMultisampleState = &ms, .pColorBlendState = &cb, .layout = layout, .renderPass = rp };
		const char *what = with_color ? "4-sample color attachment" : "no attachments";

		/* 4 samples (supported) is the reference: the count is per covered sample on Apple GPUs (observed:
		 * 4 x 256 invocations with 4 samples, the fragment shader has side effects). 8 must draw like 4. */
		uint32_t counts[2] = { 0 };
		VkSampleCountFlagBits samples[2] = { VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT };
		for (int s = 0; s < 2; s++) {
			ms.rasterizationSamples = samples[s];
			VkPipeline p;
			VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &p);
			printf("%-4s graphics pipeline with rasterizationSamples %d, %s (VkResult %d)\n", r ? "FAIL" : "OK",
			       samples[s], what, r);
			if (r) { fails++; break; }

			count[0] = 0;
			VkCommandBuffer cmd = begin();
			VkClearValue clear = { 0 };
			VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
				.renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = (uint32_t)with_color, .pClearValues = &clear };
			vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
			submit(cmd);
			counts[s] = count[0];
			vkDestroyPipeline(dev, p, NULL);
		}
		int ok = counts[0] >= W * H && counts[1] == counts[0];
		printf("%-4s draw, %s: %u fragment shader invocations with 8 samples, %u with 4 (%d pixels)\n",
		       ok ? "OK" : "FAIL", what, counts[1], counts[0], W * H);
		fails += !ok;
	}
}

/* A compute pipeline whose MSL does not compile must fail; run.sh checks the "[mvk-msl] " lines on stderr. */
static void msl_error(void)
{
	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		           .module = module("msl_error.comp.spv"), .pName = "main" }, .layout = layout };
	VkPipeline p;
	VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
	printf("%-4s compute pipeline with MSL that does not compile fails (VkResult %d)\n", r ? "OK" : "FAIL", r);
	fails += r == VK_SUCCESS;
}

int main(int argc, char **argv)
{
	int msl_log = argc == 3 && !strcmp(argv[2], "msl-log");
	if (argc != 2 && !msl_log) { fprintf(stderr, "usage: %s <spv dir> [msl-log]\n", argv[0]); return 2; }
	dir = argv[1];
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no physical device\n"); return 1; }
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.features = { .fragmentStoresAndAtomics = VK_TRUE } };
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));

	if (msl_log) {
		msl_error();
	} else {
		null_set_layouts();
		raster_samples();
		null_set_binds();
	}

	if (fails) { printf("invalid_usage: %d failure(s)\n", fails); return 1; }
	return 0;
}
