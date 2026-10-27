// d3d11_shared: a D3D11_RESOURCE_MISC_SHARED texture made by one device and opened by another, as games do for
// video (Media Foundation), web UI (ANGLE/CEF) and D3D11-on-12 surfaces. THE FINALS creates one (272x272
// B8G8R8A8_UNORM_SRGB, render target + shader resource) when it enters its practice range, and stops with a
// LowLevelFatalError when CreateTexture2D fails.
// Checks: CreateTexture2D with MISC_SHARED, IDXGIResource::GetSharedHandle, OpenSharedResource on a second device
// (same description), and that the second device reads the colour the first one cleared the texture to.
// Prints one line per check and "RESULT: PASS" or "RESULT: FAIL"; exit status = number of failed checks.
// Build: x86_64-w64-mingw32-g++ -O1 -static -o d3d11_shared.exe d3d11_shared.cpp -ld3d11 -ldxgi -luuid
// Run in the guest: wmtr/guest/lean-run.sh wmtr d3d11_shared.exe (or dxvk for the Vulkan path).
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>

static int failures;
static void check(bool ok, const char *what, HRESULT hr = S_OK) {
  std::printf("%s %s (hr 0x%08lx)\n", ok ? "ok  " : "FAIL", what, (unsigned long)hr);
  if (!ok) failures++;
}

static ID3D11Device *device(ID3D11DeviceContext **ctx) {
  ID3D11Device *d = nullptr;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                                 nullptr, ctx);
  check(SUCCEEDED(hr) && d, "D3D11CreateDevice", hr);
  return d;
}

int main() {
  ID3D11DeviceContext *ctx1 = nullptr, *ctx2 = nullptr;
  ID3D11Device *dev1 = device(&ctx1), *dev2 = device(&ctx2);
  if (!dev1 || !dev2) { std::printf("RESULT: FAIL\n"); return 1; }

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = 272; desc.Height = 272; desc.MipLevels = 1; desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  ID3D11Texture2D *tex = nullptr;
  HRESULT hr = dev1->CreateTexture2D(&desc, nullptr, &tex);
  check(SUCCEEDED(hr) && tex, "CreateTexture2D 272x272 B8G8R8A8_UNORM_SRGB, MISC_SHARED", hr);
  if (!tex) { std::printf("RESULT: FAIL\n"); return failures; }

  IDXGIResource *res = nullptr;
  HANDLE shared = nullptr;
  hr = tex->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
  if (SUCCEEDED(hr)) hr = res->GetSharedHandle(&shared);
  check(SUCCEEDED(hr) && shared, "IDXGIResource::GetSharedHandle", hr);

  // the first device clears it to a known colour
  ID3D11RenderTargetView *rtv = nullptr;
  hr = dev1->CreateRenderTargetView(tex, nullptr, &rtv);
  check(SUCCEEDED(hr), "CreateRenderTargetView", hr);
  const float colour[4] = {0.0f, 1.0f, 0.0f, 1.0f}; // green: B8G8R8A8 = 00 ff 00 ff in memory
  if (rtv) ctx1->ClearRenderTargetView(rtv, colour);
  ctx1->Flush();

  ID3D11Texture2D *opened = nullptr;
  hr = shared ? dev2->OpenSharedResource(shared, __uuidof(ID3D11Texture2D), (void **)&opened) : E_FAIL;
  check(SUCCEEDED(hr) && opened, "OpenSharedResource on a second device", hr);
  if (opened) {
    D3D11_TEXTURE2D_DESC od = {};
    opened->GetDesc(&od);
    check(od.Width == desc.Width && od.Height == desc.Height && od.Format == desc.Format, "opened texture: same description");
    // the second device reads it back through a staging copy
    D3D11_TEXTURE2D_DESC sd = od;
    sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    hr = dev2->CreateTexture2D(&sd, nullptr, &staging);
    if (staging) {
      ctx2->CopyResource(staging, opened);
      D3D11_MAPPED_SUBRESOURCE m = {};
      hr = ctx2->Map(staging, 0, D3D11_MAP_READ, 0, &m);
      unsigned char px[4] = {0};
      if (SUCCEEDED(hr)) { std::memcpy(px, (unsigned char *)m.pData + 100 * m.RowPitch + 100 * 4, 4); ctx2->Unmap(staging, 0); }
      std::printf("     pixel (100,100) via the second device: %02x %02x %02x %02x\n", px[0], px[1], px[2], px[3]);
      check(SUCCEEDED(hr) && px[0] < 8 && px[1] > 247 && px[2] < 8 && px[3] > 247, "second device sees the first one's clear", hr);
      staging->Release();
    } else check(false, "staging texture on the second device", hr);
    opened->Release();
  }
  std::printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
  return failures;
}
