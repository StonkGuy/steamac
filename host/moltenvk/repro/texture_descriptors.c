/* STEAMAC-3S/3Z/3J/3F/4B/3K/3G/3R: texel-buffer widths reaching Metal.
 * Run without Vulkan validation: a bad guest view must neither abort the VM nor
 * fail. Under Venus vkCreateBufferView is asynchronous, so a failure is a
 * command-stream error at the view's first use that kills the guest process
 * (Dragon's Dogma 2 at the end of loading a save, KosmicKrisp 0059): a view of
 * zero elements is a null descriptor and an oversized one is narrowed to
 * Metal's per-format capacity after its alignment prefix. The conservative
 * RGB32-derived advertised limit must not truncate an otherwise fitting R32
 * view: a shader checks its full size and fetches beyond that limit. Each case
 * gets its own process so pre-fix Metal assertions name the case.
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

static const uint32_t capacity_spv[] = {
#include "texture_descriptors.comp.inc"
};

/* Creation alone cannot distinguish a full R32 view from a silently clamped one. */
static void check_r32_capacity(VkBufferView view, VkDeviceMemory memory, uint32_t elements)
{
	const uint32_t index = elements - 1, sentinel = 0x51a7cafe;
	uint32_t *data;
	CK(vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, (void **)&data));
	data[index] = sentinel;
	vkUnmapMemory(dev, memory);

	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 8,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer output;
	CK(vkCreateBuffer(dev, &bci, NULL, &output));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, output, &mr);
	uint32_t type = 0;
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	while (type < mp.memoryTypeCount && (!(mr.memoryTypeBits & (1u << type)) ||
		(mp.memoryTypes[type].propertyFlags & host) != host)) type++;
	if (type == mp.memoryTypeCount) exit(1);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type };
	VkDeviceMemory output_memory;
	CK(vkAllocateMemory(dev, &ai, NULL, &output_memory));
	CK(vkBindBufferMemory(dev, output, output_memory, 0));
	uint32_t *result;
	CK(vkMapMemory(dev, output_memory, 0, VK_WHOLE_SIZE, 0, (void **)&result));
	result[0] = result[1] = 0;

	VkDescriptorSetLayoutBinding bindings[] = {
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
	};
	VkDescriptorSetLayoutCreateInfo slci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings };
	VkDescriptorSetLayout set_layout;
	CK(vkCreateDescriptorSetLayout(dev, &slci, NULL, &set_layout));
	VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 4 };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(capacity_spv), .pCode = capacity_spv };
	VkShaderModule shader;
	CK(vkCreateShaderModule(dev, &smci, NULL, &shader));
	VkComputePipelineCreateInfo pci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" } };
	VkPipeline pipeline;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
	VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes };
	VkDescriptorPool pool;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout };
	VkDescriptorSet set;
	CK(vkAllocateDescriptorSets(dev, &dsai, &set));
	VkDescriptorBufferInfo obi = { .buffer = output, .range = 8 };
	VkWriteDescriptorSet writes[] = {
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &view },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &obi },
	};
	vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
	VkCommandPool commands;
	CK(vkCreateCommandPool(dev, &cpci, NULL, &commands));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cb, &begin));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &index);
	vkCmdDispatch(cb, 1, 1, 1);
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	CK(vkEndCommandBuffer(cb));
	VkQueue queue;
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	CK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));
	if (result[0] != elements || result[1] != sentinel) {
		fprintf(stderr, "FAIL R32 capacity: width %u expected %u, last texel 0x%x expected 0x%x\n",
			result[0], elements, result[1], sentinel);
		exit(1);
	}
	vkDestroyCommandPool(dev, commands, NULL);
	vkDestroyDescriptorPool(dev, pool, NULL);
	vkDestroyPipeline(dev, pipeline, NULL);
	vkDestroyShaderModule(dev, shader, NULL);
	vkDestroyPipelineLayout(dev, layout, NULL);
	vkDestroyDescriptorSetLayout(dev, set_layout, NULL);
	vkUnmapMemory(dev, output_memory);
	vkDestroyBuffer(dev, output, NULL);
	vkFreeMemory(dev, output_memory, NULL);
}

static void buffer_case(const char *name)
{
	const int sparse = strstr(name, "sparse") != NULL;
	const int zero = strstr(name, "zero") != NULL;
	const int boundary = strstr(name, "boundary") != NULL;
	const int rgb = strstr(name, "rgb") != NULL;
	const int partial = strstr(name, "partial") != NULL;
	const int explicit_range = strstr(name, "explicit") != NULL;
	const int r32_capacity = strcmp(name, "r32-capacity") == 0;
	const int valid = boundary || partial || r32_capacity;
	const VkDeviceSize texel = rgb ? 12 : zero ? 8 : (partial || r32_capacity) ? 4 : 1;
	/* Exercise the alignment prefix as well as the advertised maximum. */
	const VkDeviceSize offset = boundary ? (sparse ? 65535 : 12) : partial ? 4 : 0;
	const VkDeviceSize range = boundary ? (VkDeviceSize)props.limits.maxTexelBufferElements * texel :
		explicit_range ? ((VkDeviceSize)props.limits.maxTexelBufferElements + 1) * texel :
		partial ? 11 : zero ? 4 : r32_capacity ? (VkDeviceSize)(1u << 27) * texel : 536870912;
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
		const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		while (type < mp.memoryTypeCount && (!(mr.memoryTypeBits & (1u << type)) ||
			(mp.memoryTypes[type].propertyFlags & host) != host)) type++;
		if (type == mp.memoryTypeCount) exit(1);
		VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type };
		CK(vkAllocateMemory(dev, &ai, NULL, &memory));
		CK(vkBindBufferMemory(dev, buffer, memory, 0));
	}
	VkBufferViewCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buffer,
		.format = rgb ? VK_FORMAT_R32G32B32_UINT : zero ? VK_FORMAT_R32G32_UINT : (partial || r32_capacity) ? VK_FORMAT_R32_UINT : VK_FORMAT_R8_UINT,
		.offset = offset, .range = explicit_range ? range : VK_WHOLE_SIZE };
	VkBufferView view = VK_NULL_HANDLE;
	VkResult result = vkCreateBufferView(dev, &ci, NULL, &view);
	/* Valid views are created; invalid ones are created too (null or narrowed), never failed */
	if (result != VK_SUCCESS) {
		fprintf(stderr, "FAIL %s returned %d (expected VK_SUCCESS: %s)\n", name, result,
			valid ? "a valid view" : "an invalid view must not fail under Venus");
		exit(1);
	}
	if (r32_capacity)
		check_r32_capacity(view, memory, range / texel);
	vkDestroyBufferView(dev, view, NULL);
	vkDestroyBuffer(dev, buffer, NULL);
	vkFreeMemory(dev, memory, NULL);
	printf("OK   %s%s (maxTexelBufferElements=%u)\n", name, valid ? "" : " accepted (null or representable width)",
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
		"sparse-explicit-oversized", "boundary", "sparse-boundary", "rgb-boundary", "partial", "r32-capacity" };
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
