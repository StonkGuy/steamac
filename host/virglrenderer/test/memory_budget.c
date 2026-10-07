/* Run inside a throwaway Venus guest, not directly against the host driver.
 * gcc -O2 -std=c11 -Wall -Wextra -Werror memory_budget.c -lvulkan -o memory_budget
 * memory_budget [MiB (0 = reported local heap)] [hold seconds] [memory type] [guest RAM MiB]
 * Allocations are bound and filled by the GPU: uncommitted heaps aren't a
 * meaningful footprint test. Test both private (0) and imported (1) memory.
 */
#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
   fprintf(stderr, "%s: VkResult %d\n", #call, r); exit(1); } } while (0)

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app };
   VkInstance instance;
   CHECK(vkCreateInstance(&ici, NULL, &instance));
   uint32_t n = 1;
   VkPhysicalDevice physical;
   CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
   if (!n) return 1;
   VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
   VkPhysicalDeviceMemoryProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &budget };
   vkGetPhysicalDeviceMemoryProperties2(physical, &props);
   uint64_t total = 0;
   for (uint32_t i = 0; i < props.memoryProperties.memoryHeapCount; i++) {
      VkMemoryHeap heap = props.memoryProperties.memoryHeaps[i];
      printf("heap %u size=%" PRIu64 " budget=%" PRIu64 " usage=%" PRIu64 " local=%u\n",
             i, heap.size, budget.heapBudget[i], budget.heapUsage[i],
             !!(heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT));
      if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) total += heap.size;
   }
   if (argc < 2) { vkDestroyInstance(instance, NULL); return 0; }
   uint64_t mib = strtoull(argv[1], NULL, 10);
   if (mib) total = mib << 20;
   unsigned hold = argc > 2 ? strtoul(argv[2], NULL, 10) : 10;
   uint32_t type = argc > 3 ? strtoul(argv[3], NULL, 10) : 0;
   if (type >= props.memoryProperties.memoryTypeCount || total > (UINT64_C(16384) << 20)) return 2;
   uint64_t ram_mib = argc > 4 ? strtoull(argv[4], NULL, 10) : 0;
   if (ram_mib > 16384) return 2;
   size_t words = (ram_mib << 20) / sizeof(uint32_t);
   volatile uint32_t *ram = words ? malloc(words * sizeof(uint32_t)) : NULL;
   if (words && !ram) return 1;
   uint32_t random = 123456789;
   for (size_t i = 0; i < words; i++) {
      random ^= random << 13; random ^= random >> 17; random ^= random << 5;
      ram[i] = random;
   }
   if (words) printf("guest RAM touched=%" PRIu64 " MiB (incompressible)\n", ram_mib);
   float priority = 1;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   VkDevice device;
   CHECK(vkCreateDevice(physical, &dci, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
   const uint64_t chunk = UINT64_C(64) << 20;
   uint32_t count = (total + chunk - 1) / chunk;
   VkBuffer *buffers = calloc(count, sizeof(*buffers));
   VkDeviceMemory *memories = calloc(count, sizeof(*memories));
   if (!buffers || !memories) return 1;
   uint64_t allocated = 0;
   for (uint32_t i = 0; i < count; i++) {
      uint64_t bytes = total - allocated < chunk ? total - allocated : chunk;
      VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
      CHECK(vkCreateBuffer(device, &bci, NULL, &buffers[i]));
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device, buffers[i], &req);
      if (!(req.memoryTypeBits & (1u << type))) return 2;
      VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size, .memoryTypeIndex = type };
      CHECK(vkAllocateMemory(device, &mai, NULL, &memories[i]));
      CHECK(vkBindBufferMemory(device, buffers[i], memories[i], 0));
      VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdFillBuffer(cmd, buffers[i], 0, bytes, 0x12345678u + i);
      CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1, .pCommandBuffers = &cmd };
      CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(queue));
      CHECK(vkResetCommandBuffer(cmd, 0));
      allocated += bytes;
      if ((i + 1) % 8 == 0) printf("resident=%" PRIu64 " MiB type=%u\n", allocated >> 20, type);
   }
   printf("holding=%" PRIu64 " MiB type=%u seconds=%u\n", allocated >> 20, type, hold);
   sleep(hold);
   for (uint32_t i = 0; i < count; i++) {
      vkDestroyBuffer(device, buffers[i], NULL);
      vkFreeMemory(device, memories[i], NULL);
   }
   free(buffers);
   free(memories);
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyDevice(device, NULL);
   free((void *)ram);
   vkDestroyInstance(instance, NULL);
   puts("PASS");
   return 0;
}
