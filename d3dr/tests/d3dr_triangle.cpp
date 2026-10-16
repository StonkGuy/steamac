// d3dr_triangle: renders one red triangle on a blue clear into the swapchain back buffer, copies the back buffer
// to a staging texture and reads it back (exercises device + swapchain creation, buffers with initial data,
// shaders, input layout, dynamic constant buffer Map/Unmap, draw, CopyResource, Map for reading, Present).
// Prints the probed pixels and exits 0 when centre = red, corner = blue. Runs the same against any D3D11:
// direct (oracle) or through d3dr.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

static const char *kShader = R"(
cbuffer C : register(b0) { float4 color; };
struct VSIn { float2 pos : POSITION; };
float4 vs_main(VSIn i) : SV_Position { return float4(i.pos, 0.5, 1); }
float4 ps_main() : SV_Target { return color; }
)";

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "tri", nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
        fprintf(stderr, "compile %s failed: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        exit(2);
    }
    return code;
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 3;
    const UINT W = 256, Hh = 256;
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "d3dr_triangle";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE, "d3dr_triangle", "d3dr_triangle", WS_OVERLAPPEDWINDOW, 0, 0, W, Hh, nullptr, nullptr,
                                wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Width = W; sd.BufferDesc.Height = Hh; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device *dev; ID3D11DeviceContext *ctx; IDXGISwapChain *sc;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev,
                                               nullptr, &ctx);
    if (FAILED(hr)) { fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed 0x%08lx\n", (unsigned long)hr); return 1; }
    ID3DBlob *vsb = compile("vs_main", "vs_5_0"), *psb = compile("ps_main", "ps_5_0");
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *il;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC ied = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    dev->CreateInputLayout(&ied, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);
    float verts[] = {0, 0.8f, 0.8f, -0.8f, -0.8f, -0.8f};
    D3D11_BUFFER_DESC bd = {sizeof verts, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA srd = {verts, 0, 0};
    ID3D11Buffer *vb, *cb; dev->CreateBuffer(&bd, &srd, &vb);
    D3D11_BUFFER_DESC cbd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    dev->CreateBuffer(&cbd, nullptr, &cb);
    ID3D11Texture2D *bb; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    ID3D11RenderTargetView *rtv; dev->CreateRenderTargetView(bb, nullptr, &rtv);
    D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ID3D11Texture2D *staging; dev->CreateTexture2D(&td, nullptr, &staging);

    for (int f = 0; f < frames; f++) {
        float blue[4] = {0, 0, 1, 1};
        ctx->ClearRenderTargetView(rtv, blue);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            float red[4] = {1, 0, 0, 1}; memcpy(ms.pData, red, 16); ctx->Unmap(cb, 0);
        }
        D3D11_VIEWPORT vp = {0, 0, (float)W, (float)Hh, 0, 1};
        ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, nullptr);
        UINT stride = 8, off = 0;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off); ctx->IASetInputLayout(il);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0); ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->Draw(3, 0);
        if (f == frames - 1) ctx->CopyResource(staging, bb);
        sc->Present(0, 0);
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr)) { fprintf(stderr, "Map(READ) failed 0x%08lx\n", (unsigned long)hr); return 1; }
    auto px = [&](UINT x, UINT y) { return *(const unsigned *)((const char *)ms.pData + y * ms.RowPitch + x * 4); };
    unsigned centre = px(W / 2, Hh / 2), corner = px(4, 4);
    ctx->Unmap(staging, 0);
    printf("d3dr_triangle: centre=%08x corner=%08x\n", centre, corner);
    bool ok = centre == 0xff0000ffu && corner == 0xffff0000u; /* RGBA8 little-endian: red = ff0000ff, blue = ffff0000 */
    printf("d3dr_triangle: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
