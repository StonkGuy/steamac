/* STEAMAC-6H/6G: a shader uses a descriptor absent from its pipeline layout.
 * The driver must not terminate the host process on this invalid Vulkan input:
 * missing descriptors read zero and writes are dropped.
 * The valid case leaves set 0 unused and NULL (independent-set Venus shape).
 *
 * Standalone (KosmicKrisp only):
 * glslangValidator -V shaders/sentry-descriptors.vert -o <dir>/sentry-descriptors.vert.spv
 * glslangValidator -V shaders/sentry-descriptors.frag -o <dir>/sentry-descriptors.frag.spv
 * clang -std=c11 -Wall -Werror sentry_descriptors.c $(pkg-config --cflags --libs vulkan) -o <exe>
 * VK_DRIVER_FILES=<private ICD json> <exe> <dir> <case 0..4>
 * Cases: 0 valid, 1 missing set, 2 NULL set, 3 binding outside, 4 empty binding.
 * Intentionally invalid Vulkan input; do not enable validation layers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r = (x); if (r != VK_SUCCESS) { fprintf(stderr, "FAIL %s: %d\n", #x, r); exit(1); } } while (0)

static VkShaderModule shader(VkDevice dev, const char *dir, const char *stage)
{
	char path[2048];
	snprintf(path, sizeof(path), "%s/sentry-descriptors.%s.spv", dir, stage);
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);
	uint32_t *code = malloc(size);
	if (!code || fread(code, 1, size, f) != (size_t)size) { fprintf(stderr, "FAIL shader read\n"); exit(1); }
	fclose(f);
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
	VkShaderModule module;
	CK(vkCreateShaderModule(dev, &ci, NULL, &module));
	free(code);
	return module;
}

static int null_access(VkDevice dev, VkPhysicalDevice pd, const char *dir)
{
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 40,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buffer;
	CK(vkCreateBuffer(dev, &bci, NULL, &buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, buffer, &req);
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(pd, &props);
	uint32_t type;
	for (type = 0; type < props.memoryTypeCount; type++)
		if ((req.memoryTypeBits & (1u << type)) &&
		    (props.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
	if (type == props.memoryTypeCount) { fprintf(stderr, "FAIL no coherent host memory\n"); exit(1); }
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size, .memoryTypeIndex = type };
	VkDeviceMemory memory;
	CK(vkAllocateMemory(dev, &mai, NULL, &memory));
	CK(vkBindBufferMemory(dev, buffer, memory, 0));
	uint32_t *values;
	CK(vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, (void **)&values));
	for (unsigned i = 0; i < 10; i++) values[i] = 0xffffffffu;
	VkDescriptorSetLayoutBinding binding = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo dsci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &binding };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dsci, NULL, &dsl));
	VkDescriptorSetLayout sets[] = { dsl, VK_NULL_HANDLE };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 2, .pSetLayouts = sets };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size };
	VkDescriptorPool pool;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet set;
	CK(vkAllocateDescriptorSets(dev, &dsai, &set));
	VkDescriptorBufferInfo bi = { buffer, 0, 40 };
	VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
	vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);
	VkShaderModule cs = shader(dev, dir, "comp");
	VkComputePipelineCreateInfo pci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main" }, .layout = layout };
	VkPipeline pipeline;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool commands;
	CK(vkCreateCommandPool(dev, &cpci, NULL, &commands));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cb, &begin));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	vkCmdDispatch(cb, 1, 1, 1);
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 1, &barrier, 0, NULL, 0, NULL);
	CK(vkEndCommandBuffer(cb));
	VkQueue queue;
	vkGetDeviceQueue(dev, 0, 0, &queue);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	CK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));
	int good = 1;
	for (unsigned i = 0; i < 10; i++)
		if (values[i]) { fprintf(stderr, "FAIL null access[%u]=%u\n", i, values[i]); good = 0; }
	printf("%s missing buffer/image/sampler reads and writes\n", good ? "OK" : "FAIL");
	vkDestroyCommandPool(dev, commands, NULL);
	vkDestroyPipeline(dev, pipeline, NULL);
	vkDestroyShaderModule(dev, cs, NULL);
	vkDestroyDescriptorPool(dev, pool, NULL);
	vkDestroyPipelineLayout(dev, layout, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	vkUnmapMemory(dev, memory);
	vkDestroyBuffer(dev, buffer, NULL);
	vkFreeMemory(dev, memory, NULL);
	return good;
}

int main(int argc, char **argv)
{
	if (argc != 3) { fprintf(stderr, "usage: %s <spv dir> <case 0..4>\n", argv[0]); return 2; }
	int test = atoi(argv[2]);
	if (test < 0 || test > 4) return 2;
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "sentry-descriptors", .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t count = 1;
	VkPhysicalDevice pd;
	CK(vkEnumeratePhysicalDevices(inst, &count, &pd));
	if (!count) return 1;
	float priority = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
	VkPhysicalDeviceFeatures features = { 0 };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &features };
	VkDevice dev;
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkDescriptorSetLayoutBinding binding = { .binding = test == 3 ? 0 : 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = test == 4 ? 0 : 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT };
	VkDescriptorSetLayoutCreateInfo dsci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dsci, NULL, &dsl));
	VkDescriptorSetLayout sets[] = { VK_NULL_HANDLE, test == 2 ? VK_NULL_HANDLE : dsl };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = test == 1 ? 1 : 2, .pSetLayouts = sets };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	VkShaderModule vs = shader(dev, argv[1], "vert"), fs = shader(dev, argv[1], "frag");
	VkPipelineShaderStageCreateInfo stages[] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" }
	};
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, 1, 1, 0, 1 };
	VkRect2D scissor = { { 0, 0 }, { 1, 1 } };
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &scissor };
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS };
	VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .subpassCount = 1, .pSubpasses = &subpass };
	VkRenderPass rp;
	CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
		.pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &viewport, .pRasterizationState = &raster,
		.pMultisampleState = &ms, .pColorBlendState = &blend, .layout = layout, .renderPass = rp };
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline);
	int good = result == VK_SUCCESS && pipeline != VK_NULL_HANDLE;
	printf("%s case=%d result=%d pipeline=%p\n", good ? "OK" : "FAIL", test, result, (void *)pipeline);
	if (pipeline) vkDestroyPipeline(dev, pipeline, NULL);
	vkDestroyRenderPass(dev, rp, NULL);
	vkDestroyShaderModule(dev, fs, NULL);
	vkDestroyShaderModule(dev, vs, NULL);
	vkDestroyPipelineLayout(dev, layout, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	if (test == 2) good &= null_access(dev, pd, argv[1]);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	return !good;
}
