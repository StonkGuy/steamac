/* vk-minmax: does the Vulkan driver (here: Venus -> KosmicKrisp) really filter with
 * VkSamplerReductionModeCreateInfo (samplerFilterMinmax, D3D12 Tiled Resources Tier 2
 * MINIMUM_/MAXIMUM_ filters)? Samples a 2x2 texture with linear filtering at points whose
 * bilinear footprint is all 4 / 2 / 1 texels, in WEIGHTED_AVERAGE, MIN and MAX modes, and
 * compares with the reference. Built and run by scripts/test/vkd3d-tiled.sh.
 * Exit: 0 all pass (or minmax unsupported: prints UNSUPPORTED), 1 mismatch, 2 setup error. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_minmax_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL %s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mem_props;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    printf("FAIL no memory type for bits %#x flags %#x\n", bits, want);
    exit(2);
}

static VkDeviceMemory alloc_bind_buffer(VkBuffer buf, void **map)
{
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
        mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, map));
    return mem;
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bi, NULL, &buf));
    return buf;
}

struct format_case {
    VkFormat format;
    const char *name;
    VkImageAspectFlags aspect;
    unsigned int texel_size;
    unsigned char data[4][4]; /* raw texels t0 (0,0), t1 (1,0), t2 (0,1), t3 (1,1) */
    float value[4];           /* their normalized values */
};

static const struct format_case formats[] = {
    { VK_FORMAT_R32_SFLOAT, "R32_SFLOAT", VK_IMAGE_ASPECT_COLOR_BIT, 4, { { 0 } }, { 0.25f, 0.75f, 0.5f, 1.0f } },
    { VK_FORMAT_D32_SFLOAT, "D32_SFLOAT", VK_IMAGE_ASPECT_DEPTH_BIT, 4, { { 0 } }, { 0.25f, 0.75f, 0.5f, 1.0f } },
    { VK_FORMAT_R16_SFLOAT, "R16_SFLOAT", VK_IMAGE_ASPECT_COLOR_BIT, 2,
      { { 0x00, 0x34 }, { 0x00, 0x3a }, { 0x00, 0x38 }, { 0x00, 0x3c } }, { 0.25f, 0.75f, 0.5f, 1.0f } },
    { VK_FORMAT_D16_UNORM, "D16_UNORM", VK_IMAGE_ASPECT_DEPTH_BIT, 2,
      { { 0x00, 0x40 }, { 0xff, 0xbf }, { 0x00, 0x80 }, { 0xff, 0xff } },
      { 16384 / 65535.0f, 49151 / 65535.0f, 32768 / 65535.0f, 1.0f } },
    { VK_FORMAT_R8_UNORM, "R8_UNORM", VK_IMAGE_ASPECT_COLOR_BIT, 1,
      { { 64 }, { 191 }, { 128 }, { 255 } }, { 64 / 255.0f, 191 / 255.0f, 128 / 255.0f, 1.0f } },
};

/* sample points and the texels their bilinear footprint covers with non-zero weight */
static const float uvs[4][2] = { { 0.5f, 0.5f }, { 0.5f, 0.25f }, { 0.25f, 0.5f }, { 0.75f, 0.75f } };
static const unsigned int footprint[4] = { 0xf, 0x3, 0x5, 0x8 };

static const VkSamplerReductionMode modes[3] = {
    VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE, VK_SAMPLER_REDUCTION_MODE_MIN, VK_SAMPLER_REDUCTION_MODE_MAX,
};
static const char *mode_names[3] = { "AVERAGE", "MIN", "MAX" };

static float expected(const struct format_case *f, unsigned int mode, unsigned int point)
{
    float sum = 0.0f, mn = INFINITY, mx = -INFINITY;
    unsigned int n = 0;
    for (unsigned int t = 0; t < 4; t++)
    {
        if (!(footprint[point] & (1u << t)))
            continue;
        sum += f->value[t]; n++;
        mn = fminf(mn, f->value[t]); mx = fmaxf(mx, f->value[t]);
    }
    return mode == 0 ? sum / n : mode == 1 ? mn : mx;
}

int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vk-minmax", 1, NULL, 0, VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t count = 1;
    VkPhysicalDevice pd;
    VkResult enum_result = vkEnumeratePhysicalDevices(inst, &count, &pd);
    if ((enum_result != VK_SUCCESS && enum_result != VK_INCOMPLETE) || !count)
    {
        printf("FAIL no physical device\n");
        return 2;
    }

    VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12 };
    vkGetPhysicalDeviceProperties2(pd, &props);
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 feats = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    vkGetPhysicalDeviceFeatures2(pd, &feats);
    printf("device: %s; samplerFilterMinmax %u, filterMinmaxSingleComponentFormats %u, filterMinmaxImageComponentMapping %u\n",
           props.properties.deviceName, f12.samplerFilterMinmax, p12.filterMinmaxSingleComponentFormats,
           p12.filterMinmaxImageComponentMapping);
    if (!f12.samplerFilterMinmax)
    {
        printf("UNSUPPORTED samplerFilterMinmax\n");
        return 0;
    }
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);

    uint32_t qf_count = 0, qf = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, NULL);
    VkQueueFamilyProperties qfp[16];
    if (qf_count > 16)
        qf_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfp);
    for (uint32_t i = 0; i < qf_count && qf == UINT32_MAX; i++)
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            qf = i;

    VkPhysicalDeviceVulkan12Features en12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    en12.samplerFilterMinmax = VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en12, 0, 1, &qci };
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(vk_minmax_spv), vk_minmax_spv };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
    VkDescriptorSetLayoutBinding binds[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, binds };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uvs) };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main" }, pl };
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkSampler samplers[3];
    for (unsigned int m = 0; m < 3; m++)
    {
        VkSamplerReductionModeCreateInfo rci = { VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, NULL, modes[m] };
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &rci };
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        CHECK(vkCreateSampler(dev, &sci, NULL, &samplers[m]));
    }

    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 } };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
        VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 3, 2, sizes };
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf };
    VkCommandPool cmd_pool;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cmd_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));

    const VkDeviceSize stride = 256;
    void *out_map, *up_map;
    VkBuffer out_buf = make_buffer(3 * stride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDeviceMemory out_mem = alloc_bind_buffer(out_buf, &out_map);
    VkBuffer up_buf = make_buffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkDeviceMemory up_mem = alloc_bind_buffer(up_buf, &up_map);
    unsigned int failures = 0, tested = 0;

    for (unsigned int fi = 0; fi < sizeof(formats) / sizeof(*formats); fi++)
    {
        const struct format_case *f = &formats[fi];
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, f->format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT))
        {
            printf("%-11s no SAMPLED_IMAGE_FILTER_MINMAX_BIT (optimal features %#x)%s\n", f->name,
                   fp.optimalTilingFeatures,
                   p12.filterMinmaxSingleComponentFormats && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                   ? " -- FAIL: required by filterMinmaxSingleComponentFormats" : "");
            if (p12.filterMinmaxSingleComponentFormats && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                failures++;
            continue;
        }
        tested++;

        VkImageCreateInfo ici2 = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f->format,
            { 2, 2, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
        VkImage img;
        CHECK(vkCreateImage(dev, &ici2, NULL, &img));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, img, &req);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
            mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        VkDeviceMemory img_mem;
        CHECK(vkAllocateMemory(dev, &ai, NULL, &img_mem));
        CHECK(vkBindImageMemory(dev, img, img_mem, 0));
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
            f->format, { 0 }, { f->aspect, 0, 1, 0, 1 } };
        VkImageView view;
        CHECK(vkCreateImageView(dev, &vci, NULL, &view));

        for (unsigned int t = 0; t < 4; t++)
        {
            if (f->format == VK_FORMAT_R32_SFLOAT || f->format == VK_FORMAT_D32_SFLOAT)
                memcpy((char *)up_map + 4 * t, &f->value[t], 4);
            else
                memcpy((char *)up_map + f->texel_size * t, f->data[t], f->texel_size);
        }

        VkDescriptorSet sets[3];
        VkDescriptorSetLayout layouts[3] = { dsl, dsl, dsl };
        VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 3, layouts };
        CHECK(vkAllocateDescriptorSets(dev, &dsai, sets));
        for (unsigned int m = 0; m < 3; m++)
        {
            VkDescriptorImageInfo dii = { samplers[m], view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo dbi = { out_buf, m * stride, 4 * sizeof(float) };
            VkWriteDescriptorSet w[2] = {
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[m], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[m], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi },
            };
            vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
        }

        VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            img, { f->aspect, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy copy = { 0, 0, 0, { f->aspect, 0, 0, 1 }, { 0, 0, 0 }, { 2, 2, 1 } };
        vkCmdCopyBufferToImage(cmd, up_buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uvs), uvs);
        for (unsigned int m = 0; m < 3; m++)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sets[m], 0, NULL);
            vkCmdDispatch(cmd, 1, 1, 1);
        }
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(cmd));
        memset(out_map, 0xff, 3 * stride);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd };
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(dev, 1, &fence));

        const float tolerance = f->format == VK_FORMAT_R8_UNORM ? 1.5f / 255.0f : 1.0f / 1024.0f;
        for (unsigned int m = 0; m < 3; m++)
        {
            const float *got = (const float *)((const char *)out_map + m * stride);
            int ok = 1;
            for (unsigned int p = 0; p < 4; p++)
                ok &= fabsf(got[p] - expected(f, m, p)) <= tolerance;
            printf("%-11s %-7s %s:", f->name, mode_names[m], ok ? "ok  " : "FAIL");
            for (unsigned int p = 0; p < 4; p++)
                printf(" (%.2f,%.2f) got %.4f want %.4f;", uvs[p][0], uvs[p][1], got[p], expected(f, m, p));
            printf("\n");
            failures += !ok;
        }
        CHECK(vkResetCommandBuffer(cmd, 0));
        CHECK(vkFreeDescriptorSets(dev, pool, 3, sets));
        vkDestroyImageView(dev, view, NULL);
        vkDestroyImage(dev, img, NULL);
        vkFreeMemory(dev, img_mem, NULL);
    }

    printf("vk-minmax: %u formats tested, %u failures\n", tested, failures);
    vkFreeMemory(dev, up_mem, NULL);
    vkFreeMemory(dev, out_mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return failures ? 1 : 0;
}
