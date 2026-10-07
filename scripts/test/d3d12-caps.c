/* d3d12-caps: create a D3D12 device through native vkd3d-proton (libvkd3d-proton-d3d12.so)
 * at the highest feature level it accepts and print what a game's CheckFeatureSupport sees.
 * Built and run by scripts/test/vkd3d-tiled.sh; exit status 0 = a device was created. */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include "vkd3d_windows.h"
#include "vkd3d_d3d12.h"
#include <stdio.h>

static const D3D_FEATURE_LEVEL levels[] = {
    D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
};

static void print_level(const char *what, D3D_FEATURE_LEVEL level)
{
    printf("%s: %u_%u (0x%x)\n", what, (level >> 12) & 0xf, (level >> 8) & 0xf, level);
}

int main(void)
{
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { (sizeof(levels) / sizeof(*levels)), levels, 0 };
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_8 };
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = { 0 };
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = { 0 };
    ID3D12Device *device = NULL;
    unsigned int i;
    HRESULT hr;

    for (i = 0; i < (sizeof(levels) / sizeof(*levels)); i++)
    {
        hr = D3D12CreateDevice(NULL, levels[i], &IID_ID3D12Device, (void **)&device);
        if (SUCCEEDED(hr))
            break;
        print_level("D3D12CreateDevice refused", levels[i]);
    }
    if (!device)
    {
        printf("D3D12CreateDevice failed at every feature level\n");
        return 1;
    }
    print_level("D3D12CreateDevice accepted", levels[i]);

    if (SUCCEEDED(hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
        print_level("MaxSupportedFeatureLevel", fl.MaxSupportedFeatureLevel);
    else
        printf("FEATURE_LEVELS: hr %#x\n", (unsigned int)hr);

    if (SUCCEEDED(hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))))
    {
        printf("TiledResourcesTier: %u\n", o.TiledResourcesTier);
        printf("ResourceBindingTier: %u\n", o.ResourceBindingTier);
        printf("TypedUAVLoadAdditionalFormats: %u\n", o.TypedUAVLoadAdditionalFormats);
        printf("ResourceHeapTier: %u\n", o.ResourceHeapTier);
        printf("ROVsSupported: %u\n", o.ROVsSupported);
        printf("ConservativeRasterizationTier: %u\n", o.ConservativeRasterizationTier);
        printf("StandardSwizzle64KBSupported: %u\n", o.StandardSwizzle64KBSupported);
        printf("MaxGPUVirtualAddressBitsPerResource: %u\n", o.MaxGPUVirtualAddressBitsPerResource);
    }
    else
        printf("D3D12_OPTIONS: hr %#x\n", (unsigned int)hr);

    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
        printf("SamplerFeedbackTier: %u\nMeshShaderTier: %u\n", o7.SamplerFeedbackTier, o7.MeshShaderTier);
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        printf("HighestShaderModel: %u.%u\n", sm.HighestShaderModel >> 4, sm.HighestShaderModel & 0xf);

    ID3D12Device_Release(device);
    return 0;
}
