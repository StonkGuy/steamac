/* Device-local vkAllocateMemory cost with and without sparseBinding (KosmicKrisp 0028 + fix).
 *
 * A device created without the sparseBinding feature cannot make sparse resources, so its device-local
 * allocations must be plain Metal heaps: N small allocations must not each be rounded up to the 64 KiB a
 * sparse heap takes. With sparseBinding on they must stay sparse-capable (a sparse buffer binds).
 *
 *   alloc-heap sparse-off   per-allocation heap usage must be below 64 KiB
 *   alloc-heap sparse-on    a sparse buffer bound to a type-0 allocation must succeed
 * Prints "RESULT: PASS|FAIL ...". Usage is read from VK_EXT_memory_budget (KosmicKrisp reports Metal's
 * currentAllocatedSize). Run through tests/run.sh with a KosmicKrisp ICD.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define N 256
#define SZ 4096
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("RESULT: FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

int main(int argc, char **argv)
{
   int sparse = argc > 1 && !strcmp(argv[1], "sparse-on");
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; VkPhysicalDevice pd; VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { printf("RESULT: FAIL no physical device\n"); return 1; }

   VkPhysicalDeviceFeatures supported; vkGetPhysicalDeviceFeatures(pd, &supported);
   if (sparse && !supported.sparseBinding) { printf("RESULT: SKIP sparseBinding not supported\n"); return 0; }
   VkPhysicalDeviceFeatures feat = {.sparseBinding = sparse};
   uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties qp[8]; if (nq > 8) nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
   uint32_t qf = 0; for (; qf < nq; qf++) if (!sparse || (qp[qf].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) break;
   float prio = 1;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf,
                                  .queueCount = 1, .pQueuePriorities = &prio};
   const char *dext[] = {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci, .pEnabledFeatures = &feat,
                             .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext};
   VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

   /* the plain DEVICE_LOCAL type (not host visible) */
   VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
   VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bud};
   vkGetPhysicalDeviceMemoryProperties2(pd, &mp);
   int type = -1;
   for (uint32_t i = 0; i < mp.memoryProperties.memoryTypeCount; i++) {
      VkMemoryPropertyFlags f = mp.memoryProperties.memoryTypes[i].propertyFlags;
      if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = i; break; }
   }
   if (type < 0) { printf("RESULT: FAIL no device-local-only memory type\n"); return 1; }
   uint32_t heap = mp.memoryProperties.memoryTypes[type].heapIndex;

   if (sparse) {
      VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536,
                                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT};
      VkBuffer buf; CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
      VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
      if (!(req.memoryTypeBits & (1u << type))) { printf("RESULT: FAIL sparse buffer does not accept type %d\n", type); return 1; }
      VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
      VkDeviceMemory mem; CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
      VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
      VkSparseMemoryBind bind = {.resourceOffset = 0, .size = req.size, .memory = mem};
      VkSparseBufferMemoryBindInfo bb = {.buffer = buf, .bindCount = 1, .pBinds = &bind};
      VkBindSparseInfo bsi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 1, .pBufferBinds = &bb};
      CHECK(vkQueueBindSparse(q, 1, &bsi, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(q));
      printf("sparse buffer of %llu bytes bound to type %d\n", (unsigned long long)req.size, type);
      printf("RESULT: PASS sparse binding still works with sparseBinding on\n");
      return 0;
   }

   vkGetPhysicalDeviceMemoryProperties2(pd, &mp);
   VkDeviceSize before = bud.heapUsage[heap];
   VkDeviceMemory mem[N];
   for (int i = 0; i < N; i++) {
      VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = SZ, .memoryTypeIndex = type};
      CHECK(vkAllocateMemory(dev, &mai, NULL, &mem[i]));
   }
   vkGetPhysicalDeviceMemoryProperties2(pd, &mp);
   VkDeviceSize after = bud.heapUsage[heap];
   double per = (double)(after - before) / N;
   printf("%d allocations of %d B from type %d: heap usage +%llu B = %.0f B each\n", N, SZ, type,
          (unsigned long long)(after - before), per);
   if (per < 65536) printf("RESULT: PASS %.0f B per allocation (below the 64 KiB sparse page)\n", per);
   else { printf("RESULT: FAIL %.0f B per allocation: allocations are rounded to 64 KiB sparse heaps\n", per); return 1; }
   return 0;
}
