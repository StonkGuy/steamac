// d3d11bench: small, deterministic D3D11 load for comparing graphics paths.
// Usage: d3d11bench.exe [--draws N] [--seconds S] [--width W] [--height H] [--vsync 0|1] [--indexed 0|1]
//                       [--overdraw K] [--passes P] [--csv path] [--label text] [--probe MS]
// --probe MS (input-to-photon probe): on every key press or mouse motion the next frame is held back MS
// milliseconds (and the clear colour flips). The host sees the gap in its presented-frame timestamps
// (WMTR_FRAMES / STEAMAC_PERF_FRAMES); the first frame after the gap is the response to the input, so
// latency = its time - input time - MS (wmtr/tools/probe_latency.py). No content readback needed.
// --probe-every SEC: the probe triggers itself every SEC seconds instead of waiting for input and prints
// "probe-trigger <unix ns>" (guest wall clock through Wine) to --probe-log PATH (stdout is not kept under Steam's
// launch chain): app-reaction-to-present latency, no input focus needed.
// Per frame: N draws of an instanced-free textured cube grid, each with its own constant buffer
// update (Map WRITE_DISCARD), plus K full-screen overdraw passes with a moderately heavy pixel
// shader (fill-rate/GPU load). Writes one CSV row per frame: frame,cpu_ms,present_ms,t_s.
// --passes P: the draws are split over P render passes that alternate between an offscreen target and the back
// buffer (the last one), so each frame opens P render encoders, like a game's shadow/post passes (default 1).
// Prints a summary line: frames, avg fps, p50/p95/p99 frame ms, 1% low fps.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_indexed = 0;
static int g_draws = 2000, g_seconds = 20, g_width = 1280, g_height = 800, g_vsync = 0, g_overdraw = 4, g_passes = 1;
static const char *g_csv = "d3d11bench.csv";
static const char *g_label = "run";
static bool g_quit = false;
static int g_probe_ms = 0;
static double g_probe_every = 0;
static const char *g_probe_log = "probe-triggers.txt";
static volatile LONG g_input = 0; /* inputs seen since the last frame (probe mode) */
static void step(const char *s) { fprintf(stderr, "step: %s\n", s); fflush(stderr); }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE || (m == WM_KEYDOWN && w == VK_ESCAPE)) { g_quit = true; return 0; }
  if (g_probe_ms && (m == WM_KEYDOWN || m == WM_MOUSEMOVE || m == WM_INPUT)) InterlockedIncrement(&g_input);
  return DefWindowProcA(h, m, w, l);
}

static const char *kShader = R"(
cbuffer PerDraw : register(b0) { float4x4 mvp; float4 tint; };
Texture2D tex0 : register(t0);
SamplerState smp : register(s0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(VSIn i) { VSOut o; o.pos = mul(mvp, float4(i.pos, 1)); o.uv = i.uv; return o; }
float4 ps_main(VSOut i) : SV_Target { return tex0.Sample(smp, i.uv) * tint; }
struct FSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FSOut fs_vs(uint id : SV_VertexID) {
  FSOut o; float2 p = float2((id << 1) & 2, id & 2); o.uv = p; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0.5, 1); return o; }
// --indexed 1: the fill pass keeps a small local array and indexes it with a per-pixel value
// (fxc emits an indexable temp, x0[r.x]): exercises how the shader compiler lowers dynamic indexing
float4 fs_ps_idx(FSOut i) : SV_Target {
  float4 a[8]; float2 uv = i.uv;
  [unroll] for (int k = 0; k < 8; k++) a[k] = tex0.Sample(smp, frac(uv * (1.0 + 0.13 * k)));
  float4 c = 0;
  [loop] for (int n = 0; n < 24; n++) { uint j = (uint)(frac(uv.x * 7.3 + uv.y * 3.1 + n * 0.37) * 8.0) & 7; c += a[j]; a[(j + 3) & 7] *= 0.97; }
  return float4(c.rgb * tint.rgb * (1.0 / 24), 0.08);
}
float4 fs_ps(FSOut i) : SV_Target {
  float3 c = 0; float2 uv = i.uv;
  [unroll] for (int k = 0; k < 24; k++) { uv = frac(uv * 1.37 + float2(0.11, 0.07) * k); c += tex0.Sample(smp, uv).rgb * (1.0 / 24); }
  return float4(c * tint.rgb, 0.08);
}
)";

static ID3DBlob *compile(const char *entry, const char *target) {
  ID3DBlob *code = nullptr, *err = nullptr;
  HRESULT hr = D3DCompile(kShader, strlen(kShader), "bench", nullptr, nullptr, entry, target, 0, 0, &code, &err);
  if (FAILED(hr)) { fprintf(stderr, "compile %s failed: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?"); exit(2); }
  return code;
}

static void mat_mvp(float *m, float x, float y, float s, float a) {
  float c = cosf(a) * s, sn = sinf(a) * s;
  float r[16] = {c, -sn, 0, x, sn, c, 0, y, 0, 0, s, 0.5f, 0, 0, 0, 1};
  memcpy(m, r, sizeof r);
}

/* --dump FILE: write frame 30's back buffer as a binary PPM and print its mean colour (correctness check:
 * the same picture must come out of every D3D11 implementation). Animation then follows the frame number,
 * not the clock, so the picture is deterministic. */
static const char *g_dump = nullptr;
static void dump_frame(ID3D11Device *dev, ID3D11DeviceContext *ctx, IDXGISwapChain *sc) {
  ID3D11Texture2D *bb; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
  D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
  ID3D11Texture2D *st; dev->CreateTexture2D(&d, nullptr, &st);
  ctx->CopyResource(st, bb);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { fprintf(stderr, "dump: Map failed\n"); return; }
  FILE *f = fopen(g_dump, "wb");
  if (f) fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
  double sum[3] = {0, 0, 0}; unsigned bright = 0;
  bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  for (unsigned y = 0; y < d.Height; y++) {
    const unsigned char *row = (const unsigned char *)m.pData + y * m.RowPitch;
    for (unsigned x = 0; x < d.Width; x++) {
      unsigned char px[3] = {row[4 * x + (bgra ? 2 : 0)], row[4 * x + 1], row[4 * x + (bgra ? 0 : 2)]};
      for (int c = 0; c < 3; c++) sum[c] += px[c];
      if (px[0] + px[1] + px[2] > 96) bright++;
      if (f) fwrite(px, 1, 3, f);
    }
  }
  ctx->Unmap(st, 0); if (f) fclose(f); st->Release(); bb->Release();
  double n = double(d.Width) * d.Height;
  printf("DUMP %s %ux%u format=%d mean_rgb=%.1f,%.1f,%.1f bright_px=%u\n", g_dump, d.Width, d.Height, (int)d.Format,
         sum[0] / n, sum[1] / n, sum[2] / n, bright);
}

int main(int argc, char **argv) {
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--draws")) g_draws = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--seconds")) g_seconds = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--width")) g_width = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--height")) g_height = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--vsync")) g_vsync = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--overdraw")) g_overdraw = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--passes")) g_passes = std::max(1, atoi(argv[i + 1]));
    else if (!strcmp(argv[i], "--indexed")) g_indexed = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--csv")) g_csv = argv[i + 1];
    else if (!strcmp(argv[i], "--label")) g_label = argv[i + 1];
    else if (!strcmp(argv[i], "--dump")) g_dump = argv[i + 1];
    else if (!strcmp(argv[i], "--probe")) g_probe_ms = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--probe-every")) g_probe_every = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "--probe-log")) g_probe_log = argv[i + 1];
  }
  WNDCLASSA wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "d3d11bench";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassA(&wc);
  RECT rc = {0, 0, g_width, g_height}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d3d11bench", "d3d11bench", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, rc.right - rc.left,
                            rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);

  fprintf(stderr, "step: window hwnd=%p err=%lu pid=%lu\n", (void *)hwnd, GetLastError(), GetCurrentProcessId());
  { DWORD wp = 0; DWORD wt = GetWindowThreadProcessId(hwnd, &wp); fprintf(stderr, "step: window tid=%lu pid=%lu GetProcessId(self)=%lu\n", wt, wp, GetProcessId(GetCurrentProcess())); }
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 2; sd.BufferDesc.Width = g_width; sd.BufferDesc.Height = g_height;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr; IDXGISwapChain *sc = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd,
                                             &sc, &dev, nullptr, &ctx);
  if (FAILED(hr)) { fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed 0x%08lx\n", (unsigned long)hr); return 1; }

  step("device");
  ID3DBlob *vsb = compile("vs_main", "vs_5_0"), *psb = compile("ps_main", "ps_5_0");
  ID3DBlob *fvb = compile("fs_vs", "vs_5_0"), *fpb = compile(g_indexed ? "fs_ps_idx" : "fs_ps", "ps_5_0");
  step("compiled");
  ID3D11VertexShader *vs, *fvs; ID3D11PixelShader *ps, *fps;
  dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
  dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
  dev->CreateVertexShader(fvb->GetBufferPointer(), fvb->GetBufferSize(), nullptr, &fvs);
  dev->CreatePixelShader(fpb->GetBufferPointer(), fpb->GetBufferSize(), nullptr, &fps);
  D3D11_INPUT_ELEMENT_DESC ied[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ID3D11InputLayout *il; dev->CreateInputLayout(ied, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);

  // a quad-ish mesh (two triangles) is enough to exercise per-draw state; 6 verts
  float verts[] = {-1, -1, 0, 0, 1, -1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, -1, 0, 0, 1, 1, 1, 0, 1, 0, 1, -1, 0, 1, 1};
  D3D11_BUFFER_DESC bd = {sizeof verts, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA srd = {verts, 0, 0};
  ID3D11Buffer *vb; dev->CreateBuffer(&bd, &srd, &vb);
  D3D11_BUFFER_DESC cbd = {80, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  ID3D11Buffer *cb; dev->CreateBuffer(&cbd, nullptr, &cb);

  const int TW = 256; std::vector<unsigned> texels(TW * TW);
  for (int y = 0; y < TW; y++) for (int x = 0; x < TW; x++) texels[y * TW + x] = ((x ^ y) & 16) ? 0xffd0a040u : 0xff4060c0u;
  D3D11_TEXTURE2D_DESC td = {TW, TW, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA tsd = {texels.data(), TW * 4, 0};
  ID3D11Texture2D *tex; dev->CreateTexture2D(&td, &tsd, &tex);
  ID3D11ShaderResourceView *srv; dev->CreateShaderResourceView(tex, nullptr, &srv);
  D3D11_SAMPLER_DESC smd = {}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; smd.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *smp; dev->CreateSamplerState(&smd, &smp);
  D3D11_BLEND_DESC bld = {}; bld.RenderTarget[0].BlendEnable = TRUE;
  bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState *blend; dev->CreateBlendState(&bld, &blend);

  ID3D11Texture2D *bb; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
  ID3D11RenderTargetView *rtv; dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();
  ID3D11RenderTargetView *off_rtv = nullptr; /* --passes: the offscreen target the other passes draw into */
  if (g_passes > 1) {
    D3D11_TEXTURE2D_DESC od = {(UINT)g_width, (UINT)g_height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *ot; dev->CreateTexture2D(&od, nullptr, &ot);
    dev->CreateRenderTargetView(ot, nullptr, &off_rtv); ot->Release();
  }

  step("resources");
  FILE *csv = fopen(g_csv, "w");
  if (csv) fprintf(csv, "frame,cpu_ms,present_ms,t_s\n");
  LARGE_INTEGER freq, t0, ta, tb, tc; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
  std::vector<double> frame_ms; frame_ms.reserve(100000);
  double prev = 0; int grid = (int)ceil(sqrt((double)std::max(g_draws, 1)));
  for (unsigned frame = 0; !g_quit; frame++) {
    MSG msg; while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    QueryPerformanceCounter(&ta);
    double t = double(ta.QuadPart - t0.QuadPart) / freq.QuadPart;
    if (t > g_seconds) break;
    if (g_dump) t = frame / 60.0;
    if (frame < 3) step("frame");
    static bool flip = false;
    static double next_trigger = 5;
    bool self = g_probe_ms && g_probe_every > 0 && t >= next_trigger;
    if (self) {
      next_trigger = t + g_probe_every;
      FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
      unsigned long long ns = ((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) * 100ULL;
      static FILE *pl = fopen(g_probe_log, "w");
      if (pl) { fprintf(pl, "probe-trigger %llu\n", ns); fflush(pl); }
    }
    if (self || (g_probe_ms && InterlockedExchange(&g_input, 0))) {
      fprintf(stderr, "probe: input at frame %u\n", frame);
      Sleep(g_probe_ms); /* the gap the host looks for */
      flip = !flip;
    }
    float clear[4] = {0.05f, flip ? 0.6f : 0.05f, 0.08f, 1};
    ctx->ClearRenderTargetView(rtv, clear);
    D3D11_VIEWPORT vp = {0, 0, (float)g_width, (float)g_height, 0, 1};
    ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, nullptr);
    UINT stride = 20, off = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off); ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb); ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &srv); ctx->PSSetSamplers(0, 1, &smp);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    for (int d = 0; d < g_draws; d++) {
      if (g_passes > 1 && d % ((g_draws + g_passes - 1) / g_passes) == 0) {
        /* pass k of P: the last one is the back buffer, the ones before alternate with the offscreen target */
        int k = d / ((g_draws + g_passes - 1) / g_passes);
        ID3D11RenderTargetView *target = (g_passes - 1 - k) % 2 ? off_rtv : rtv;
        ctx->OMSetRenderTargets(1, &target, nullptr);
      }
      D3D11_MAPPED_SUBRESOURCE ms;
      if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        float *f = (float *)ms.pData;
        float gx = (d % grid + 0.5f) / grid * 2 - 1, gy = (d / grid + 0.5f) / grid * 2 - 1;
        mat_mvp(f, gx, gy, 0.9f / grid, (float)t + d * 0.01f);
        f[16] = 0.5f + 0.5f * sinf(d * 0.1f); f[17] = 0.8f; f[18] = 1.0f; f[19] = 1.0f;
        ctx->Unmap(cb, 0);
      }
      ctx->Draw(6, 0);
    }
    if (g_overdraw > 0) {
      if (g_passes > 1) ctx->OMSetRenderTargets(1, &rtv, nullptr);
      ctx->IASetInputLayout(nullptr); ctx->VSSetShader(fvs, nullptr, 0); ctx->PSSetShader(fps, nullptr, 0);
      ctx->OMSetBlendState(blend, nullptr, 0xffffffff);
      for (int k = 0; k < g_overdraw; k++) ctx->Draw(3, 0);
    }
    QueryPerformanceCounter(&tb);
    if (g_dump && frame == 30) dump_frame(dev, ctx, sc);
    sc->Present(g_vsync ? 1 : 0, 0);
    QueryPerformanceCounter(&tc);
    double now = double(tc.QuadPart - t0.QuadPart) / freq.QuadPart;
    if (frame > 0) frame_ms.push_back((now - prev) * 1000.0);
    prev = now;
    if (csv) fprintf(csv, "%u,%.3f,%.3f,%.4f\n", frame, double(tb.QuadPart - ta.QuadPart) * 1000.0 / freq.QuadPart,
                     double(tc.QuadPart - tb.QuadPart) * 1000.0 / freq.QuadPart, now);
  }
  if (csv) fclose(csv);
  // skip the first second (warm-up) in the summary
  std::vector<double> v; double acc = 0;
  for (double ms : frame_ms) { acc += ms; if (acc > 1000.0) v.push_back(ms); }
  if (v.empty()) { printf("%s: no frames\n", g_label); return 3; }
  std::vector<double> s = v; std::sort(s.begin(), s.end());
  double sum = 0; for (double x : v) sum += x;
  auto pct = [&](double p) { return s[std::min(s.size() - 1, (size_t)(p * (s.size() - 1)))]; };
  size_t n1 = std::max<size_t>(1, s.size() / 100); double worst = 0;
  for (size_t i = s.size() - n1; i < s.size(); i++) worst += s[i];
  printf("RESULT label=%s draws=%d overdraw=%d passes=%d %dx%d frames=%zu avg_fps=%.1f p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f "
         "low1_fps=%.1f max_ms=%.1f\n", g_label, g_draws, g_overdraw, g_passes, g_width, g_height, v.size(), 1000.0 * v.size() / sum,
         pct(0.5), pct(0.95), pct(0.99), 1000.0 / (worst / n1), s.back());
  fflush(stdout);
  return 0;
}
