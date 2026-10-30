/* STEAMAC-3W: a sampler2D passed to a helper becomes spvDynamicImageSampler.
 * nullDescriptor checks must test its underlying texture, not the wrapper.
 * Run: texture_helpers <spv dir>. A null texture must return four zeros.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CK(x) do { VkResult r = (x); if (r) { fprintf(stderr, "FAIL %s: %d at %d\n", #x, r, __LINE__); exit(1); } } while (0)

int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
	VkPhysicalDevice pd; uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	VkPhysicalDeviceRobustness2FeaturesEXT robust = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES };
	robust.pNext = &ycbcr;
	VkPhysicalDeviceFeatures2 feats = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &robust };
	vkGetPhysicalDeviceFeatures2(pd, &feats);
	if (!robust.nullDescriptor || !robust.robustImageAccess2 || !ycbcr.samplerYcbcrConversion) {
		puts("SKIP texture_helpers: robust YCbCr sampler features unsupported");
		vkDestroyInstance(inst, NULL);
		return 0;
	}
	robust.robustBufferAccess2 = VK_FALSE;
	float priority = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
	const char *ext = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &robust, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext };
	VkDevice dev; CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
	VkSamplerYcbcrConversionCreateInfo yci = { VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
		.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL, .xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN,
		.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN, .chromaFilter = VK_FILTER_NEAREST };
	VkSamplerYcbcrConversion conversion; CK(vkCreateSamplerYcbcrConversion(dev, &yci, NULL, &conversion));
	VkSamplerYcbcrConversionInfo yinfo = { VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .conversion = conversion };
	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = &yinfo,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
	VkSampler sampler; CK(vkCreateSampler(dev, &sci, NULL, &sampler));
	VkDescriptorSetLayoutBinding bindings[] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }
	};
	VkDescriptorSetLayoutCreateInfo slci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings };
	VkDescriptorSetLayout sl; CK(vkCreateDescriptorSetLayout(dev, &slci, NULL, &sl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl };
	VkPipelineLayout layout; CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
	char path[1024]; snprintf(path, sizeof(path), "%s/dynamic_sampler.comp.spv", argv[1]);
	FILE *f = fopen(path, "rb"); if (!f) return 2;
	fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
	uint32_t *code = malloc(len); if (fread(code, 1, len, f) != (size_t)len) return 2; fclose(f);
	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = len, .pCode = code };
	VkShaderModule module; CK(vkCreateShaderModule(dev, &smci, NULL, &module)); free(code);
	VkComputePipelineCreateInfo pci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" } };
	VkPipeline pipeline; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buffer; CK(vkCreateBuffer(dev, &bci, NULL, &buffer));
	VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buffer, &req);
	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t type;
	for (type = 0; type < mp.memoryTypeCount; type++)
		if ((req.memoryTypeBits & (1u << type)) && (mp.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
	if (type == mp.memoryTypeCount) return 2;
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type };
	VkDeviceMemory memory; CK(vkAllocateMemory(dev, &mai, NULL, &memory)); CK(vkBindBufferMemory(dev, buffer, memory, 0));
	float *values; CK(vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, (void **)&values));
	memset(values, 0xff, 16);
	VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes };
	VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &sl };
	VkDescriptorSet set; CK(vkAllocateDescriptorSets(dev, &dsai, &set));
	VkDescriptorImageInfo image = { sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorBufferInfo data = { buffer, 0, 16 };
	VkWriteDescriptorSet writes[] = {
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image },
		{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &data }
	};
	vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
	VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cmd, &cbbi));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	vkCmdDispatch(cmd, 1, 1, 1);
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	CK(vkEndCommandBuffer(cmd));
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(queue));
	int bad = values[0] != 0 || values[1] != 0 || values[2] != 0 || values[3] != 0;
	printf("%s dynamic sampler helper: null descriptor returns (%g, %g, %g, %g)\n", bad ? "FAIL" : "OK", values[0], values[1], values[2], values[3]);
	vkDestroyCommandPool(dev, pool, NULL); vkDestroyDescriptorPool(dev, dp, NULL); vkDestroySampler(dev, sampler, NULL);
	vkDestroySamplerYcbcrConversion(dev, conversion, NULL);
	vkUnmapMemory(dev, memory); vkDestroyBuffer(dev, buffer, NULL); vkFreeMemory(dev, memory, NULL);
	vkDestroyPipeline(dev, pipeline, NULL); vkDestroyShaderModule(dev, module, NULL); vkDestroyPipelineLayout(dev, layout, NULL);
	vkDestroyDescriptorSetLayout(dev, sl, NULL); vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
	return bad;
}
