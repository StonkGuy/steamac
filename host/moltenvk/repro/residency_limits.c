/* STEAMAC-3V/4H: non-sparse texture-buffer views share their buffer's memory.
 * Metal's residency-set hash table cannot grow past 1048576 slots on M1 Max;
 * adding every view separately aborts in IOGPUMetalResidencySet addAllocation:.
 * More than that many simultaneous views must still read their underlying buffer,
 * including after removing the other views. Sparse views keep separate mappings.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
static VkDevice dev;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties mp;

struct buffer { VkBuffer buffer; VkDeviceMemory memory; uint32_t *map; };
static struct buffer make_buffer(VkBufferUsageFlags usage)
{
	struct buffer b;
	VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536, .usage = usage };
	CK(vkCreateBuffer(dev, &ci, NULL, &b.buffer));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, b.buffer, &mr);
	uint32_t t = 0;
	const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	while (t < mp.memoryTypeCount && (!(mr.memoryTypeBits & (1u << t)) || (mp.memoryTypes[t].propertyFlags & flags) != flags)) t++;
	if (t == mp.memoryTypeCount) { fprintf(stderr, "FAIL no coherent host buffer memory\n"); exit(1); }
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
	CK(vkAllocateMemory(dev, &ai, NULL, &b.memory));
	CK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
	CK(vkMapMemory(dev, b.memory, 0, VK_WHOLE_SIZE, 0, (void **)&b.map));
	return b;
}

static void free_buffer(struct buffer b)
{
	vkUnmapMemory(dev, b.memory);
	vkDestroyBuffer(dev, b.buffer, NULL);
	vkFreeMemory(dev, b.memory, NULL);
}

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3) { fprintf(stderr, "usage: %s <shader-dir> [view-count]\n", argv[0]); return 2; }
	uint32_t count = argc == 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1100000;
	if (count < 3) return 2;
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance instance;
	CK(vkCreateInstance(&ici, NULL, &instance));
	uint32_t n = 1;
	CK(vkEnumeratePhysicalDevices(instance, &n, &pd));
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	float priority = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, 0, 0, &queue);

	struct buffer input = make_buffer(VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
	struct buffer output = make_buffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	const uint32_t expected[] = { 0x12345678, 0x87654321, 0xabcdef01 };
	memcpy(input.map, expected, sizeof(expected));
	VkBufferView *views = calloc(count, sizeof(*views));
	if (!views) return 1;
	for (uint32_t i = 0; i < count; i++) {
		VkBufferViewCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = input.buffer,
			.format = VK_FORMAT_R32_UINT, .offset = (i % 3) * 4, .range = 256 };
		CK(vkCreateBufferView(dev, &ci, NULL, &views[i]));
	}
	printf("OK   %u simultaneous buffer-backed texture views\n", count);

	VkDescriptorSetLayoutBinding bindings[] = {
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = 3, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
	};
	VkDescriptorSetLayoutCreateInfo slci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings };
	VkDescriptorSetLayout set_layout;
	CK(vkCreateDescriptorSetLayout(dev, &slci, NULL, &set_layout));
	VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 3 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes };
	VkDescriptorPool descriptor_pool;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &descriptor_pool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descriptor_pool,
		.descriptorSetCount = 1, .pSetLayouts = &set_layout };
	VkDescriptorSet set;
	CK(vkAllocateDescriptorSets(dev, &dsai, &set));
	VkDescriptorBufferInfo out = { output.buffer, 0, 65536 };
	VkWriteDescriptorSet writes[] = {
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 3,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = views },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &out },
	};
	vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	char path[1024];
	snprintf(path, sizeof(path), "%s/residency_limits.comp.spv", argv[1]);
	FILE *file = fopen(path, "rb");
	if (!file) { perror(path); return 1; }
	fseek(file, 0, SEEK_END);
	long bytes = ftell(file);
	rewind(file);
	uint32_t *code = malloc(bytes);
	if (!code || fread(code, 1, bytes, file) != (size_t)bytes) return 1;
	fclose(file);
	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
	VkShaderModule module;
	CK(vkCreateShaderModule(dev, &smci, NULL, &module));
	free(code);
	VkComputePipelineCreateInfo pci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" } };
	VkPipeline pipeline;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	for (unsigned round = 0; round < 2; round++) {
		memset(output.map, 0, sizeof(expected));
		VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
		VkCommandBuffer cmd;
		CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		CK(vkBeginCommandBuffer(cmd, &bi));
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
		vkCmdDispatch(cmd, 1, 1, 1);
		VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
		CK(vkEndCommandBuffer(cmd));
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
		CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
		CK(vkQueueWaitIdle(queue));
		if (memcmp(output.map, expected, sizeof(expected))) {
			fprintf(stderr, "FAIL texel fetch round %u: %08x %08x %08x\n", round, output.map[0], output.map[1], output.map[2]);
			return 1;
		}
		printf("OK   buffer-view fetch %s removing unrelated views\n", round ? "after" : "before");
		vkFreeCommandBuffers(dev, pool, 1, &cmd);
		if (!round) for (uint32_t i = 3; i < count; i++) vkDestroyBufferView(dev, views[i], NULL);
	}
	for (unsigned i = 0; i < 3; i++) vkDestroyBufferView(dev, views[i], NULL);
	free(views);
	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyPipeline(dev, pipeline, NULL);
	vkDestroyShaderModule(dev, module, NULL);
	vkDestroyPipelineLayout(dev, layout, NULL);
	vkDestroyDescriptorPool(dev, descriptor_pool, NULL);
	vkDestroyDescriptorSetLayout(dev, set_layout, NULL);
	free_buffer(output);
	free_buffer(input);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(instance, NULL);
	return 0;
}
