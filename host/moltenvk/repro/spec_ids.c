/* STEAMAC-6A: full-width Vulkan SpecIds, duplicate defaults, unused IDs that
 * alias a compact Metal index, and specialization after a serialized cache reload. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkDevice dev;
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

int main(int argc, char **argv)
{
	if (argc != 2) { fprintf(stderr, "usage: %s <spv dir>\n", argv[0]); return 2; }
	dir = argv[1];
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	VkPhysicalDevice pd;
	if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no physical device\n"); return 1; }
	VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .shaderZeroInitializeWorkgroupMemory = VK_TRUE };
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v13 };
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, 0, 0, &queue);

	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout layout;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));

	const VkDeviceSize size = 16;
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buf;
	CK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t t = 0;
	const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	while (!(mr.memoryTypeBits & (1u << t)) || (mp.memoryTypes[t].propertyFlags & hv) != hv) t++;
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
	VkDeviceMemory mem;
	CK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CK(vkBindBufferMemory(dev, buf, mem, 0));
	uint32_t *map;
	CK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));

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
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool;
	CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));

    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VkPipelineCache cache;
    CK(vkCreatePipelineCache(dev, &pci, NULL, &cache));
    int fails = 0;
    for (unsigned test = 0; test < 4; test++) {
        int specialized = test == 1 || test == 2;
        if (test == 2) {
            size_t bytes = 0;
            CK(vkGetPipelineCacheData(dev, cache, &bytes, NULL));
            void *data = malloc(bytes);
            CK(vkGetPipelineCacheData(dev, cache, &bytes, data));
            vkDestroyPipelineCache(dev, cache, NULL);
            pci.initialDataSize = bytes; pci.pInitialData = data;
            CK(vkCreatePipelineCache(dev, &pci, NULL, &cache));
            free(data);
        }
        uint32_t values[] = { 42, 43, 44, 99 };
        VkSpecializationMapEntry entries[] = { { 70000, 0, 4 }, { UINT32_MAX, 4, 4 }, { 0, 8, 4 }, { 1, 12, 4 } };
        VkSpecializationInfo spec = { 4, entries, sizeof(values), values };
        VkShaderModule m = module("spec_ids.comp.spv");
        VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = m, .pName = "main", .pSpecializationInfo = specialized ? &spec : NULL }, .layout = layout };
        VkPipeline p;
        CK(vkCreateComputePipelines(dev, cache, 1, &ci, NULL, &p));
        memset(map, 0, size);
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
        VkCommandBuffer cmd;
        CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
        VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CK(vkBeginCommandBuffer(cmd, &cbbi));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        CK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        CK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CK(vkQueueWaitIdle(queue));
        uint32_t defaults[] = { 7, 9, 11, 3 }, expected[] = { 42, 42, 43, 44 };
        int good = !memcmp(map, specialized ? expected : defaults, sizeof(defaults));
        printf("%s full-width SpecIds %s %s: %u %u %u %u\n", good ? "OK" : "FAIL",
            specialized ? "specialized" : "defaults", test >= 2 ? "disk-cache" : "live-cache", map[0], map[1], map[2], map[3]);
        fails += !good;
        vkFreeCommandBuffers(dev, pool, 1, &cmd);
        vkDestroyPipeline(dev, p, NULL);
        vkDestroyShaderModule(dev, m, NULL);
    }
    vkDestroyPipelineCache(dev, cache, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDescriptorPool(dev, dpool, NULL);
    vkUnmapMemory(dev, mem); vkDestroyBuffer(dev, buf, NULL); vkFreeMemory(dev, mem, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL); vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    return fails ? 1 : 0;
}
