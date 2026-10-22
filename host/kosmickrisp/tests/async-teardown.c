/* MESA_KK_ASYNC_PIPELINES device teardown with compiles in flight.
 *
 * vkDestroyPipeline waits for its program's build, but a pipeline still alive at vkDestroyDevice
 * (an application or a Venus/virglrenderer context torn down without destroying every pipeline)
 * keeps its job on a worker. kk_DestroyDevice released the Metal compiler and only then joined the
 * workers, so a worker still inside newRenderPipelineStateWithDescriptor used a freed compiler
 * (SIGSEGV or pointer-authentication fault in AGXMetal). Each cycle queues builds of distinct
 * render pipeline states, destroys half of the pipelines and destroys the device at once.
 * Prints "RESULT: PASS|FAIL"; a crash is the failure. */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "async-stress-spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("RESULT: FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static const VkFormat formats[] = {
   VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
   VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
   VK_FORMAT_R16G16B16A16_UNORM,
};
#define NFMT (sizeof(formats) / sizeof(formats[0]))

int main(void)
{
   setenv("MESA_KK_ASYNC_PIPELINES", "1", 1);
   int cycles = getenv("STRESS_CYCLES") ? atoi(getenv("STRESS_CYCLES")) : 40;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1; VkPhysicalDevice pd; VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { printf("RESULT: FAIL no physical device\n"); return 1; }

   for (int c = 0; c < cycles; c++) {
      float prio = 1;
      VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
      VkPhysicalDeviceDynamicRenderingFeatures dr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .dynamicRendering = VK_TRUE};
      VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dr, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
      VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

      VkShaderModuleCreateInfo smv = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vs_spv), .pCode = (const uint32_t *)vs_spv};
      VkShaderModuleCreateInfo smf = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(fs_spv), .pCode = (const uint32_t *)fs_spv};
      VkShaderModule vs, fs; CHECK(vkCreateShaderModule(dev, &smv, NULL, &vs)); CHECK(vkCreateShaderModule(dev, &smf, NULL, &fs));
      VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));

      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
      VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
      VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
      VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
      VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
      VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
      VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
      VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};

      /* every (format, write mask, blend) combination is its own render pipeline state */
      VkPipeline p[NFMT * 4];
      unsigned np = 0;
      for (unsigned f = 0; f < NFMT; f++) {
         for (unsigned v = 0; v < 4; v++) {
            VkPipelineRenderingCreateInfo rc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &formats[f]};
            cba.colorWriteMask = (v & 1) ? 0x7 : 0xf;
            cba.blendEnable = (v & 2) != 0;
            cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rc, .stageCount = 2, .pStages = st,
               .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
               .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = layout};
            CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &p[np++]));
         }
      }
      for (unsigned i = 0; i < np; i += 2) vkDestroyPipeline(dev, p[i], NULL);
      vkDestroyPipelineLayout(dev, layout, NULL);
      vkDestroyShaderModule(dev, vs, NULL);
      vkDestroyShaderModule(dev, fs, NULL);
      vkDestroyDevice(dev, NULL);
   }
   vkDestroyInstance(inst, NULL);
   printf("RESULT: PASS %d device teardowns with %u async pipeline builds queued each, half of them still alive\n", cycles, (unsigned)(NFMT * 4));
   return 0;
}
