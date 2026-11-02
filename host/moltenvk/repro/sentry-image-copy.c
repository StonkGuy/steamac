/* STEAMAC-5E/5F: exercise kk_CmdCopyImage2's texture->buffer->texture path.
 * Build: xcrun clang -std=c11 -Wall -Werror -I/opt/homebrew/include
 *   host/moltenvk/repro/sentry-image-copy.c -L/opt/homebrew/lib -lvulkan
 *   -Wl,-rpath,/opt/homebrew/lib -o <private-dir>/sentry-image-copy
 * Run with VK_DRIVER_FILES=<private ICD JSON> MTL_DEBUG_LAYER=1
 *   MTL_DEBUG_LAYER_ERROR_MODE=assert <program> [color|compressed]
 * Uses 8-byte compatible formats, including compressed mip tails. Each byte is
 * checked after a round trip; no shader or game installation is required.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(x) do { VkResult r = (x); if (r != VK_SUCCESS) { fprintf(stderr, "FAIL %s: %d at %d\n", #x, r, __LINE__); exit(1); } } while (0)
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;
struct image { VkImage handle; VkDeviceMemory memory; };
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fprintf(stderr, "FAIL memory type\n"); exit(1);
}
static struct image image(VkFormat format, uint32_t w, uint32_t h, uint32_t d) {
    struct image out;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = format, .extent = {w,h,d}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    CK(vkCreateImage(dev, &ci, NULL, &out.handle));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, out.handle, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = memory_type(mr.memoryTypeBits, 0) };
    CK(vkAllocateMemory(dev, &ai, NULL, &out.memory));
    CK(vkBindImageMemory(dev, out.handle, out.memory, 0));
    return out;
}
static void barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
int main(int argc, char **argv) {
    int compressed = argc < 2 || !strcmp(argv[1], "compressed");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "sentry-image-copy", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    CK(vkCreateInstance(&ici, NULL, &inst));
    VkPhysicalDevice pd; uint32_t count = 1;
    CK(vkEnumeratePhysicalDevices(inst, &count, &pd));
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(pd, &features);
    if (compressed && !features.textureCompressionBC) { puts("SKIP BC unsupported"); return 0; }
    float priority = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .pEnabledFeatures = &features };
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool; CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
    VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 8 * 1024 * 1024,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buffer; CK(vkCreateBuffer(dev, &bci, NULL, &buffer));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buffer, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory memory; CK(vkAllocateMemory(dev, &ai, NULL, &memory)); CK(vkBindBufferMemory(dev, buffer, memory, 0));
    unsigned char *bytes; CK(vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, (void **)&bytes));
    for (uint32_t shape = 0; shape < 4; ++shape) {
        uint32_t w = compressed ? (1u << shape) : shape ? 256 : 32;
        uint32_t h = w;
        uint32_t d = !compressed && shape ? 8 : 1;
        uint32_t bw = compressed ? (w + 3) / 4 : w;
        uint32_t bh = compressed ? (h + 3) / 4 : h;
        size_t size = (size_t)bw * bh * d * 8;
        struct image a = image(VK_FORMAT_R32G32_UINT, bw, bh, d);
        struct image b = image(compressed ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_R16G16B16A16_UINT, w, h, d);
        struct image c = image(VK_FORMAT_R32G32_UINT, bw, bh, d);
        for (size_t j = 0; j < size; ++j) bytes[j] = (unsigned char)(j * 17 + 31);
        memset(bytes + 4 * 1024 * 1024, 0, size);
        CK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CK(vkBeginCommandBuffer(cmd, &begin));
        VkImageMemoryBarrier ib[3]; memset(ib, 0, sizeof(ib));
        VkImage images[] = {a.handle, b.handle, c.handle};
        for (unsigned j = 0; j < 3; ++j) ib[j] = (VkImageMemoryBarrier) { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[j], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 3, ib);
        VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = {bw,bh,d} };
        vkCmdCopyBufferToImage(cmd, buffer, a.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
        barrier(cmd);
        VkImageCopy copy = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = {bw,bh,d} };
        fprintf(stderr, "CASE %s %ux%ux%u begin\n", compressed ? "BC1" : "color", w, h, d);
        vkCmdCopyImage(cmd, a.handle, VK_IMAGE_LAYOUT_GENERAL, b.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        barrier(cmd);
        copy.extent = (VkExtent3D){w,h,d};
        vkCmdCopyImage(cmd, b.handle, VK_IMAGE_LAYOUT_GENERAL, c.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        barrier(cmd);
        bic.bufferOffset = 4 * 1024 * 1024;
        vkCmdCopyImageToBuffer(cmd, c.handle, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &bic);
        VkMemoryBarrier host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
        CK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        CK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(queue));
        for (size_t j = 0; j < size; ++j) if (bytes[j] != bytes[4 * 1024 * 1024 + j]) {
            fprintf(stderr, "FAIL byte %zu expected %u got %u\n", j, bytes[j], bytes[4 * 1024 * 1024 + j]); return 1;
        }
        puts("PASS image-copy round trip");
        for (unsigned j = 0; j < 3; ++j) vkDestroyImage(dev, images[j], NULL);
        vkFreeMemory(dev, a.memory, NULL); vkFreeMemory(dev, b.memory, NULL); vkFreeMemory(dev, c.memory, NULL);
    }
    vkUnmapMemory(dev, memory); vkDestroyBuffer(dev, buffer, NULL); vkFreeMemory(dev, memory, NULL);
    vkDestroyCommandPool(dev, pool, NULL); vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    return 0;
}
