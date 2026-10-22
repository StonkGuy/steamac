/* Texel buffer views of formats no Metal texture buffer backs (KosmicKrisp 0048, from fxgl/steamac 0042).
 *
 * vkd3d-proton creates R32G32B32_UINT/SFLOAT texel buffer views without checking the format's features and
 * Venus forwards them; KosmicKrisp dereferenced the missing format in vkCreateBufferView (SIGSEGV). Such a view
 * is invalid usage; the driver must survive it and its descriptors read as null descriptors. For each
 * R32G32B32 format: create the view, write it to a uniform texel buffer descriptor, destroy view and set.
 * A supported view (R32_UINT) goes through the same steps. Prints "RESULT: PASS|FAIL ...". Run through
 * tests/run.sh with a KosmicKrisp ICD.
 */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("RESULT: FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

int main(void)
{
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; VkPhysicalDevice pd; VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { printf("RESULT: FAIL no physical device\n"); return 1; }
   float prio = 1;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                             .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT};
   VkBuffer buf; CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
   uint32_t type = 0; while (!(req.memoryTypeBits & (1u << type))) type++;
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
   VkDeviceMemory mem; CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));

   VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_ALL};
   VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
   VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));

   const VkFormat fmts[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_SFLOAT};
   for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
      VkBufferViewCreateInfo bvi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buf, .format = fmts[i], .range = VK_WHOLE_SIZE};
      VkBufferView view; CHECK(vkCreateBufferView(dev, &bvi, NULL, &view));
      VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
      VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &view};
      vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
      CHECK(vkFreeDescriptorSets(dev, dp, 1, &ds));
      vkDestroyBufferView(dev, view, NULL);
   }
   vkDestroyDescriptorPool(dev, dp, NULL); vkDestroyDescriptorSetLayout(dev, dsl, NULL);
   vkDestroyBuffer(dev, buf, NULL); vkFreeMemory(dev, mem, NULL);
   vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
   printf("RESULT: PASS R32_UINT and R32G32B32_{UINT,SINT,SFLOAT} views created, written, destroyed\n");
   return 0;
}
