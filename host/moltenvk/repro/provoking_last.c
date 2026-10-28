/*
 * Flat varyings with the last vertex provoking (KosmicKrisp only; STEAMAC-G).
 *
 * zink keeps GL's last provoking vertex (VK_EXT_provoking_vertex), and Metal only has the first. KosmicKrisp unrolls a
 * draw whose fragment shader reads a flat varying into indices that rotate the last vertex first; the unroll kernel
 * ended the render pass around every such draw: its attachments stored, a compute pass, the attachments loaded again.
 * Steam's web helper composites its first frame like this, 12 rectangles into its LINEAR window buffer (host
 * memory): 13 render passes over the whole 2560x1600 window took 43 ms of GPU time on an M1 Max, and M3 and later GPUs
 * lost the device right there (the GPU's recovery from a command buffer it could not preempt).
 *
 * Draws flat-shaded strips (also an instanced one), fans and lists into a 2560x1600 B8G8R8A8 LINEAR host pointer import
 * like zink's window buffer, one render pass, and checks the last vertex's color in both triangles of each cell:
 *   1. direct draws: the record (MESA_KK_DEBUG=cbs, child process) must show one render pass and no compute encoder
 *      (the indices of direct draws without an index buffer are known when recording);
 *   2. the same as indexed draws, 16-bit indices written by vkCmdUpdateBuffer earlier in the command buffer: the unroll
 *      kernel needs them, and its compute pass must run before the render pass (one render pass in the record) and
 *      after the update;
 *   3. two command buffers of one device unrolling indexed draws with different indices: A waits for a timeline
 *      semaphore at its submission, B is submitted after it without a wait, then the host signals; both images must
 *      hold their own command buffer's colors (the unroll kernels allocated their indices from one device-wide heap
 *      that every command buffer reset to its start);
 *   4. indexed indirect draws (their unrolls allocate from that heap) with a geometry shader draw among them: 2 render
 *      passes (the GS's compute pass ends the first and allocates from the heap too), all colors right;
 *   5. a clear, 8 blits and 8 render passes of direct draws in one command buffer (no unrolls): one Metal command
 *      buffer in the record (with one per render pass, Steam's web helper's command buffer of 77 blits took twice the
 *      GPU time). The indexed draws of 2. and 3. take 2 Metal command buffers: the update and the unrolls, the render
 *      pass.
 * The GPU times come from the records.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define W 2560
#define H 1600

static VkDevice dev;
static VkPipelineLayout pl;
static VkPipeline pipes[4];

static VkShaderModule load_shader(const char *dir, const char *name)
{
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s.spv", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) { printf("FAIL open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *code = malloc(size);
	if (fread(code, 1, size, f) != (size_t)size) { printf("FAIL read %s\n", path); exit(1); }
	fclose(f);
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
	VkShaderModule m;
	CK(vkCreateShaderModule(dev, &ci, NULL, &m));
	free(code);
	return m;
}

/* topology, cell sample points (cell-local, y up as in the vertex shader) and the vertex provoking there */
static const struct {
	VkPrimitiveTopology topology;
	float x[2], y[2];
	uint32_t k[2];
} topologies[3] = {
	{ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, { 0.2f, 0.8f }, { 0.2f, 0.8f }, { 2, 3 } },
	{ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, { 0.8f, 0.2f }, { 0.2f, 0.8f }, { 2, 3 } },
	{ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, { 0.2f, 0.8f }, { 0.2f, 0.8f }, { 2, 5 } },
};

/* cells 0-5 strips, 6-7 one instanced strip draw, 8-9 fans, 10-11 lists */
static uint32_t cell_topology(uint32_t cell)
{
	return cell < 8 ? 0 : cell < 10 ? 1 : 2;
}

/* Index sets of the indexed draws (vertex offset cell * 8): strips/fans from 0, lists from 4. Set 1 takes vertices 4-7
 * of the strips and fans: the same positions (gl_VertexIndex % 4), other colors. */
static const uint16_t index_sets[2][10] = {
	{ 0, 1, 2, 3, 0, 1, 2, 3, 4, 5 },
	{ 4, 5, 6, 7, 0, 1, 2, 3, 4, 5 },
};

/* the window buffer: LINEAR B8G8R8A8 in imported host memory, as virglrenderer gives zink */
struct target {
	VkImage image;
	VkImageView view;
	void *host;
	VkSubresourceLayout layout;
};

static void make_target(struct target *t, VkPhysicalDevice pd)
{
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
	VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hostp };
	vkGetPhysicalDeviceProperties2(pd, &p2);
	PFN_vkGetMemoryHostPointerPropertiesEXT get_host_ptr_props =
		(PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");

	VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
	VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_B8G8R8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	CK(vkCreateImage(dev, &ici, NULL, &t->image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, t->image, &req);
	VkDeviceSize align = hostp.minImportedHostPointerAlignment;
	VkDeviceSize host_size = (req.size + align - 1) / align * align;
	t->host = mmap(NULL, host_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (t->host == MAP_FAILED) { printf("FAIL mmap\n"); exit(1); }
	VkMemoryHostPointerPropertiesEXT hp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
	CK(get_host_ptr_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, t->host, &hp));
	uint32_t type = __builtin_ctz(req.memoryTypeBits & hp.memoryTypeBits);
	VkImportMemoryHostPointerInfoEXT import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = t->host };
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, .allocationSize = host_size,
		.memoryTypeIndex = type };
	VkDeviceMemory mem;
	CK(vkAllocateMemory(dev, &ai, NULL, &mem));
	CK(vkBindImageMemory(dev, t->image, mem, 0));
	VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
	vkGetImageSubresourceLayout(dev, t->image, &sub, &t->layout);
	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	CK(vkCreateImageView(dev, &vci, NULL, &t->view));
}

/* Draws the 12 cells into t in one render pass: direct draws (index_set < 0), or indexed draws of index_set from
 * index_buf. With indirect_buf, the indexed draws are indirect (one command per cell there), and cell 2 is drawn
 * through the geometry shader. */
static void draw_cells(VkCommandBuffer cmd, const struct target *t, int index_set, VkBuffer index_buf,
		       VkBuffer indirect_buf)
{
	VkDeviceSize index_offset = index_set > 0 ? sizeof(index_sets[0]) * 2 : 0;
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = t->image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL,
			     0, NULL, 1, &b);
	VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t->view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE };
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1,
		.colorAttachmentCount = 1, .pColorAttachments = &att };
	vkCmdBeginRendering(cmd, &ri);
	if (index_set >= 0)
		vkCmdBindIndexBuffer(cmd, index_buf, index_offset, VK_INDEX_TYPE_UINT16);
	for (uint32_t cell = 0; cell < 12; cell++) {
		uint32_t t = cell_topology(cell);
		uint32_t count = t == 2 ? 6 : 4, instances = cell == 6 ? 2 : 1; /* cells 6 and 7: one instanced draw */
		int gs = indirect_buf && cell == 2;
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[gs ? 3 : t]);
		vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &t);
		if (gs)
			vkCmdDraw(cmd, count, instances, cell * 8, 0);
		else if (indirect_buf)
			vkCmdDrawIndexedIndirect(cmd, indirect_buf, cell * sizeof(VkDrawIndexedIndirectCommand), 1,
						 sizeof(VkDrawIndexedIndirectCommand));
		else if (index_set >= 0)
			vkCmdDrawIndexed(cmd, count, instances, t == 2 ? 4 : 0, (int32_t)cell * 8, 0);
		else
			vkCmdDraw(cmd, count, instances, cell * 8, 0);
		cell += instances - 1;
	}
	vkCmdEndRendering(cmd);
}

/* Records the 12 cells into t (draw_cells), the indices of index_set written into index_buf by vkCmdUpdateBuffer first
 * (its memory holds zeros: degenerate triangles) */
static void record_cells(VkCommandBuffer cmd, const struct target *t, int index_set, VkBuffer index_buf,
			 VkBuffer indirect_buf)
{
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CK(vkBeginCommandBuffer(cmd, &bi));
	VkDeviceSize index_offset = index_set > 0 ? sizeof(index_sets[0]) * 2 : 0;
	if (index_set >= 0) {
		vkCmdUpdateBuffer(cmd, index_buf, index_offset, sizeof(index_sets[0]), index_sets[index_set]);
		VkBufferMemoryBarrier ib = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_INDEX_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = index_buf, .offset = index_offset,
			.size = sizeof(index_sets[0]) };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
				     &ib, 0, NULL);
	}
	draw_cells(cmd, t, index_set, index_buf, indirect_buf);
	CK(vkEndCommandBuffer(cmd));
}

/* 5. BLITS blits and render passes of direct draws taking turns in one command buffer, no unrolls (the shape of Steam's
 * web helper's command buffers of 77 blits): a clear of src, then each blit src -> dst (vk_meta: a render pass each)
 * followed by the cells drawn into t */
#define BLITS 8
static void record_blits(VkCommandBuffer cmd, const struct target *t, VkImage src, VkImage dst)
{
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CK(vkBeginCommandBuffer(cmd, &bi));
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageMemoryBarrier b[2] = {
		{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = src,
		  .subresourceRange = range },
		{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = dst,
		  .subresourceRange = range },
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
	VkClearColorValue color = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
	vkCmdClearColorImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
	b[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, b);
	for (uint32_t i = 0; i < BLITS; i++) {
		VkImageBlit region = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.srcOffsets = { { 0, 0, 0 }, { 512, 512, 1 } }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.dstOffsets = { { 0, 0, 0 }, { 256 + 32 * (int32_t)i, 256, 1 } } };
		vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
			       &region, VK_FILTER_LINEAR);
		VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL,
				     0, NULL);
		draw_cells(cmd, t, -1, VK_NULL_HANDLE, VK_NULL_HANDLE);
	}
	CK(vkEndCommandBuffer(cmd));
}

/* The last vertex's color in both triangles of each cell (index set 1: vertices 4-7 of strips and fans) */
static int check_cells(const struct target *t, int index_set, const char *what)
{
	int fails = 0;
	for (uint32_t cell = 0; cell < 12; cell++) {
		uint32_t topo = cell_topology(cell);
		for (uint32_t tri = 0; tri < 2; tri++) {
			float u = (cell % 4 + 0.05f + topologies[topo].x[tri] * 0.9f) / 4.0f;
			float v = (cell / 4 + 0.05f + topologies[topo].y[tri] * 0.9f) / 3.0f;
			uint32_t x = (uint32_t)(u * W), y = (uint32_t)(v * H);
			uint32_t got = *(uint32_t *)((char *)t->host + t->layout.offset + y * t->layout.rowPitch + x * 4);
			uint32_t k = topologies[topo].k[tri] + (index_set == 1 && topo < 2 ? 4 : 0);
			uint32_t want = 0xff000000u | (cell * 16 + k + 1) << 8 | (topo + 1) * 40;
			if (got != want) {
				printf("FAIL %s: cell %u (%s) triangle %u: color 0x%08x, want vertex %u's 0x%08x\n", what, cell,
				       topo == 0 ? "strip" : topo == 1 ? "fan" : "list", tri, got, k, want);
				fails++;
			}
		}
	}
	printf("%-4s %s: last vertex provoking in 24 triangles (strips, an instanced strip, fans, lists)\n",
	       fails ? "FAIL" : "OK", what);
	return fails;
}

static int child(const char *spv_dir)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "provoking_last",
		.apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	VkPhysicalDevice pd;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no physical device\n"); return 1; }

	VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT };
	VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &pvf };
	VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	if (!pvf.provokingVertexLast || !v13.dynamicRendering || !v12.timelineSemaphore || !f2.features.geometryShader) {
		printf("SKIP provoking_last: no provokingVertexLast\n");
		return 0;
	}

	VkPhysicalDeviceProvokingVertexFeaturesEXT pv_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
		.provokingVertexLast = VK_TRUE };
	VkPhysicalDeviceVulkan13Features v13_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &pv_on,
		.dynamicRendering = VK_TRUE };
	VkPhysicalDeviceVulkan12Features v12_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13_on,
		.timelineSemaphore = VK_TRUE };
	const char *exts[] = { VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkPhysicalDeviceFeatures2 f2_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12_on,
		.features = { .geometryShader = VK_TRUE } };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2_on, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci, .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, 0, 0, &queue);

	struct target targets[2];
	make_target(&targets[0], pd);
	make_target(&targets[1], pd);

	/* host-visible index memory, zeros until a command buffer writes its index set */
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(index_sets[0]) * 4,
		.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer index_buf;
	CK(vkCreateBuffer(dev, &bci, NULL, &index_buf));
	VkMemoryRequirements breq;
	vkGetBufferMemoryRequirements(dev, index_buf, &breq);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t btype = 0;
	while (!(breq.memoryTypeBits & (1u << btype)) ||
	       !(mp.memoryTypes[btype].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
		btype++;
	VkMemoryAllocateInfo bai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = breq.size,
		.memoryTypeIndex = btype };
	VkDeviceMemory bmem;
	CK(vkAllocateMemory(dev, &bai, NULL, &bmem));
	CK(vkBindBufferMemory(dev, index_buf, bmem, 0));
	void *bmap;
	CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &bmap));
	memset(bmap, 0, breq.size);

	/* 4.: one indexed indirect draw per cell */
	VkBufferCreateInfo ibci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 12 * sizeof(VkDrawIndexedIndirectCommand),
		.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT };
	VkBuffer indirect_buf;
	CK(vkCreateBuffer(dev, &ibci, NULL, &indirect_buf));
	vkGetBufferMemoryRequirements(dev, indirect_buf, &breq);
	bai.allocationSize = breq.size;
	VkDeviceMemory imem;
	CK(vkAllocateMemory(dev, &bai, NULL, &imem));
	CK(vkBindBufferMemory(dev, indirect_buf, imem, 0));
	VkDrawIndexedIndirectCommand *indirect;
	CK(vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, (void **)&indirect));
	for (uint32_t cell = 0; cell < 12; cell++) {
		uint32_t t = cell_topology(cell);
		indirect[cell] = (VkDrawIndexedIndirectCommand){ .indexCount = t == 2 ? 6 : 4,
			.instanceCount = cell == 6 ? 2 : 1, .firstIndex = t == 2 ? 4 : 0, .vertexOffset = (int32_t)cell * 8 };
	}

	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 4 };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &pcr };
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkShaderModule vs = load_shader(spv_dir, "flat_last.vert"), fs = load_shader(spv_dir, "flat_last.frag");
	VkShaderModule gs = load_shader(spv_dir, "flat_last.geom");
	VkPipelineShaderStageCreateInfo stages[3] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = gs, .pName = "main" },
	};
	/* strips, fans, lists, strips through the geometry shader */
	for (uint32_t p = 0; p < 4; p++) {
		uint32_t t = p == 3 ? 0 : p;
		VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = topologies[t].topology };
		VkViewport vp = { 0, 0, W, H, 0, 1 };
		VkRect2D sc = { { 0, 0 }, { W, H } };
		VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
		VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
			VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
			.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT };
		VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, &pvs,
			.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
		VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
		VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
		VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1, .pAttachments = &cba };
		VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
		VkPipelineRenderingCreateInfo prci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1, .pColorAttachmentFormats = &format };
		VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prci,
			.stageCount = p == 3 ? 3 : 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
			.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
			.layout = pl };
		CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipes[p]));
	}

	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool;
	CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 6 };
	VkCommandBuffer cmds[6];
	CK(vkAllocateCommandBuffers(dev, &cbai, cmds));
	int fails = 0;

	/* 1. direct draws, 2. indexed draws */
	for (int set = -1; set <= 0; set++) {
		VkCommandBuffer cmd = cmds[set + 1];
		record_cells(cmd, &targets[0], set, index_buf, VK_NULL_HANDLE);
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
		CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
		CK(vkQueueWaitIdle(queue));
		fails += check_cells(&targets[0], set, set < 0 ? "direct draws" : "indexed draws");
	}

	/* 3. A waits for the semaphore at its submission, B (other indices) is submitted after it, then the host signals */
	VkSemaphoreTypeCreateInfo stci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
	VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &stci };
	VkSemaphore sem;
	CK(vkCreateSemaphore(dev, &sci, NULL, &sem));
	memset(targets[0].host, 0, targets[0].layout.offset + targets[0].layout.size);
	record_cells(cmds[2], &targets[0], 0, index_buf, VK_NULL_HANDLE);
	record_cells(cmds[3], &targets[1], 1, index_buf, VK_NULL_HANDLE);
	uint64_t one = 1;
	VkTimelineSemaphoreSubmitInfo tsi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &one };
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo sa = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsi, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem,
		.pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cmds[2] };
	CK(vkQueueSubmit(queue, 1, &sa, VK_NULL_HANDLE));
	VkSubmitInfo sb = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmds[3] };
	CK(vkQueueSubmit(queue, 1, &sb, VK_NULL_HANDLE));
	usleep(50000);
	VkSemaphoreSignalInfo ssi = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = sem, .value = 1 };
	CK(vkSignalSemaphore(dev, &ssi));
	CK(vkQueueWaitIdle(queue));
	fails += check_cells(&targets[0], 0, "command buffer A (waited for a semaphore, indexed)");
	fails += check_cells(&targets[1], 1, "command buffer B (submitted after A, other indices)");

	/* 4. indexed indirect draws (their unrolls allocate from the device heap), a geometry shader draw among them (its
	 * compute pass ends the render pass and allocates from the heap too) */
	memset(targets[0].host, 0, targets[0].layout.offset + targets[0].layout.size);
	record_cells(cmds[4], &targets[0], 0, index_buf, indirect_buf);
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmds[4] };
	CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));
	fails += check_cells(&targets[0], 0, "indexed indirect draws, a geometry shader draw among them");

	/* 5. blits between two 512x512 device-local images and render passes of direct draws taking turns */
	VkImage blit_images[2];
	for (uint32_t i = 0; i < 2; i++) {
		VkImageCreateInfo bici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_B8G8R8A8_UNORM, .extent = { 512, 512, 1 }, .mipLevels = 1, .arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
		CK(vkCreateImage(dev, &bici, NULL, &blit_images[i]));
		VkMemoryRequirements ireq;
		vkGetImageMemoryRequirements(dev, blit_images[i], &ireq);
		uint32_t itype = 0;
		while (!(ireq.memoryTypeBits & (1u << itype)) ||
		       !(mp.memoryTypes[itype].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			itype++;
		VkMemoryAllocateInfo iai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = ireq.size,
			.memoryTypeIndex = itype };
		VkDeviceMemory blit_mem;
		CK(vkAllocateMemory(dev, &iai, NULL, &blit_mem));
		CK(vkBindImageMemory(dev, blit_images[i], blit_mem, 0));
	}
	memset(targets[0].host, 0, targets[0].layout.offset + targets[0].layout.size);
	record_blits(cmds[5], &targets[0], blit_images[0], blit_images[1]);
	si.pCommandBuffers = &cmds[5];
	CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));
	fails += check_cells(&targets[0], -1, "direct draws after blits");
	fflush(stdout);
	/* KosmicKrisp logs the records when the command buffers' feedback comes, the device's destruction waits for it */
	vkDestroyDevice(dev, NULL);
	return fails != 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: %s <spv dir> [child]\n", argv[0]); return 2; }
	if (argc > 2 && !strcmp(argv[2], "child"))
		return child(argv[1]);

	/* The child logs its command buffers' records (MESA_KK_DEBUG=cbs) on stderr, in submission order */
	char err_path[] = "/tmp/provoking_last.XXXXXX";
	int fd = mkstemp(err_path);
	if (fd < 0) { printf("FAIL mkstemp\n"); return 1; }
	close(fd);
	char cmd[8192];
	snprintf(cmd, sizeof(cmd), "MESA_KK_DEBUG=cbs '%s' '%s' child 2> '%s'", argv[0], argv[1], err_path);
	fflush(stdout);
	int status = system(cmd);
	FILE *f = fopen(err_path, "r");
	char line[4096], records[6][4096] = { "", "", "", "", "", "" };
	unsigned count = 0;
	while (f && fgets(line, sizeof(line), f)) {
		if (strstr(line, "kk: command buffer") && strstr(line, "of provoking_last") && strstr(line, "draws") &&
		    count < 6) {
			line[strcspn(line, "\n")] = '\0';
			snprintf(records[count++], sizeof(records[0]), "%s", line);
		}
	}
	if (f)
		fclose(f);
	unlink(err_path);
	int fails = status != 0;
	if (status)
		printf("FAIL child exit status %d\n", status);
	/* 1. direct draws: one render pass, no compute pass; 2. and 3. indexed draws: the unroll kernels in a compute pass
	 * at the end of the Metal command buffer of the update, the one render pass in a Metal command buffer of its own;
	 * 4. the geometry shader's compute pass ends the render pass once; 5. no unrolls: one Metal command buffer for the
	 * clear, the blits and the render passes (a Metal command buffer costs GPU time of its own, ~55 us on an M1 Max) */
	static const struct {
		const char *name, *want, *draws, *metal;
	} cases[6] = {
		{ "direct draws in one render pass, no compute pass", ": 1 render passes, 0 compute encoders, 11 draws",
		  " 11 draws", ", 1 Metal command buffers" },
		{ "indexed draws in one render pass", ": 1 render passes, ", " 11 draws", ", 2 Metal command buffers" },
		{ "A in one render pass", ": 1 render passes, ", " 11 draws", ", 2 Metal command buffers" },
		{ "B in one render pass", ": 1 render passes, ", " 11 draws", ", 2 Metal command buffers" },
		{ "indexed indirect draws and a geometry shader draw in 2 render passes", ": 2 render passes, ", " 11 draws",
		  ", 3 Metal command buffers" },
		{ "a clear, 8 blits and 8 render passes of direct draws in one Metal command buffer",
		  ": 17 render passes, ", " 96 draws", ", 1 Metal command buffers" },
	};
	for (unsigned i = 0; i < 6; i++) {
		const char *shown = strstr(records[i], "provoking_last: ");
		int ok = strstr(records[i], cases[i].want) && strstr(records[i], cases[i].draws) &&
			 strstr(records[i], cases[i].metal);
		printf("%-4s %s (MESA_KK_DEBUG=cbs: %s)\n", ok ? "OK" : "FAIL", cases[i].name,
		       shown ? shown + strlen("provoking_last: ") : "no record");
		fails += !ok;
	}
	if (fails) { printf("provoking_last: %d failure(s)\n", fails); return 1; }
	return 0;
}
