// d3d12_memory: the Direct3D 12 memory paths a VM transport has to carry (Mullion over wmtr in the steamac guest).
// Each case writes a pattern from the CPU, has the GPU move or produce data, reads the result back on the CPU and
// compares every byte with what the D3D12 specification says it must be:
//   committed   upload buffer -> default buffer -> readback buffer (committed resources)
//   texture     upload buffer -> 64x64 R8G8B8A8 texture (CopyTextureRegion with a placed footprint) -> readback
//   uavclear    ClearUnorderedAccessViewUint on a typed R32_UINT default buffer -> readback
//   placed      buffers placed at offsets 0 and 64 KiB in an upload heap and a readback heap
//               (D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS): under wmtr Mullion needs d3d12.cpuVisibleHeapMemory=True here,
//               an MTLHeap's memory stays on the Mac
//   placednone  the same with D3D12_HEAP_FLAG_NONE heaps (resource heap tier 2; skipped on tier 1)
//   compute     a cs_5_0 shader (D3DCompile at run time, d3dcompiler_47) reads a placed upload buffer through a root
//               SRV (its GPU virtual address) and writes a default buffer through a root UAV -> readback; skipped
//               without a shader compiler
// Prints "INFO ..." lines (adapter, MULLION_CONFIG), "CASE <name> PASS|FAIL|SKIP <detail>" per case and
// "RESULT passed=N failed=N skipped=N"; exit status 0 only when no case failed. A case that faults the process ends
// it: the missing CASE lines show where (run one case alone with --case).
// Usage: d3d12_memory.exe [--out FILE] [--case NAME]...    (stdout is lost under Steam's launch chain: --out)
// Build (llvm-mingw): x86_64-w64-mingw32-clang++ -O2 -static -std=c++17 d3d12_memory.cpp -o d3d12_memory.exe
//                     -ld3d12 -ldxgi
#define WIN32_LEAN_AND_MEAN
// the MSVC ABI for methods returning a struct (GetCPUDescriptorHandleForHeapStart, GetDesc, ...), as games use it
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static FILE *out = stdout;
static int passed, failed, skipped;
static std::vector<std::string> only;

static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(out, fmt, ap);
  va_end(ap);
  fputc('\n', out);
  fflush(out);
}

static void verdict(const char *name, const char *v, const char *fmt, ...) {
  char detail[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof detail, fmt, ap);
  va_end(ap);
  say("CASE %s %s %s", name, v, detail);
  if (!strcmp(v, "PASS"))
    passed++;
  else if (!strcmp(v, "SKIP"))
    skipped++;
  else
    failed++;
}

static bool wanted(const char *name) {
  if (only.empty())
    return true;
  for (auto &o : only)
    if (o == name)
      return true;
  return false;
}

struct Gpu {
  ID3D12Device *dev = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *alloc = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12Fence *fence = nullptr;
  UINT64 value = 0;
  HANDLE event = nullptr;
};

// Record with f, execute, wait for the GPU (10 s: longer is a hang).
template <class F> static HRESULT submit(Gpu &g, F f) {
  HRESULT hr = g.alloc->Reset();
  if (FAILED(hr))
    return hr;
  if (FAILED(hr = g.list->Reset(g.alloc, nullptr)))
    return hr;
  f(g.list);
  if (FAILED(hr = g.list->Close()))
    return hr;
  ID3D12CommandList *lists[] = {g.list};
  g.queue->ExecuteCommandLists(1, lists);
  if (FAILED(hr = g.queue->Signal(g.fence, ++g.value)))
    return hr;
  if (g.fence->GetCompletedValue() < g.value) {
    if (FAILED(hr = g.fence->SetEventOnCompletion(g.value, g.event)))
      return hr;
    if (WaitForSingleObject(g.event, 10000) != WAIT_OBJECT_0)
      return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
  return S_OK;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  return d;
}

static ID3D12Resource *committed(Gpu &g, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state, HRESULT *hr,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = type;
  D3D12_RESOURCE_DESC d = buffer_desc(size, flags);
  ID3D12Resource *r = nullptr;
  *hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, __uuidof(ID3D12Resource),
                                       (void **)&r);
  return SUCCEEDED(*hr) ? r : nullptr;
}

static ID3D12Heap *heap(Gpu &g, D3D12_HEAP_TYPE type, UINT64 size, D3D12_HEAP_FLAGS flags, HRESULT *hr) {
  D3D12_HEAP_DESC d = {};
  d.SizeInBytes = size;
  d.Properties.Type = type;
  d.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  d.Flags = flags;
  ID3D12Heap *h = nullptr;
  *hr = g.dev->CreateHeap(&d, __uuidof(ID3D12Heap), (void **)&h);
  return SUCCEEDED(*hr) ? h : nullptr;
}

static ID3D12Resource *placed(Gpu &g, ID3D12Heap *h, UINT64 offset, UINT64 size, D3D12_RESOURCE_STATES state,
                              HRESULT *hr) {
  D3D12_RESOURCE_DESC d = buffer_desc(size);
  ID3D12Resource *r = nullptr;
  *hr = g.dev->CreatePlacedResource(h, offset, &d, state, nullptr, __uuidof(ID3D12Resource), (void **)&r);
  return SUCCEEDED(*hr) ? r : nullptr;
}

static uint8_t pattern(size_t i, int seed) { return (uint8_t)(i * 31u + (unsigned)seed * 101u + (i >> 8) * 7u); }

// CPU writes through Map (nothing read)
static HRESULT fill(ID3D12Resource *r, size_t size, int seed) {
  void *p = nullptr;
  D3D12_RANGE none = {0, 0};
  HRESULT hr = r->Map(0, &none, &p);
  if (FAILED(hr))
    return hr;
  if (!p)
    return E_POINTER;
  for (size_t i = 0; i < size; i++)
    ((uint8_t *)p)[i] = pattern(i, seed);
  r->Unmap(0, nullptr);
  return S_OK;
}

struct Compared {
  HRESULT hr = S_OK;
  size_t bad = 0, first = 0;
  unsigned got = 0, want = 0;
};

// CPU reads through Map: bytes [0, size) against want(i)
template <class W> static Compared compare(ID3D12Resource *r, size_t size, W want) {
  Compared c;
  void *p = nullptr;
  D3D12_RANGE all = {0, size};
  c.hr = r->Map(0, &all, &p);
  if (SUCCEEDED(c.hr) && !p)
    c.hr = E_POINTER;
  if (FAILED(c.hr))
    return c;
  for (size_t i = 0; i < size; i++) {
    uint8_t w = want(i), got = ((const uint8_t *)p)[i];
    if (got != w && !c.bad++) {
      c.first = i;
      c.got = got;
      c.want = w;
    }
  }
  D3D12_RANGE none = {0, 0};
  r->Unmap(0, &none);
  return c;
}

static void judge(const char *name, const Compared &c, size_t size) {
  if (FAILED(c.hr))
    verdict(name, "FAIL", "readback Map hr=0x%08lx", (unsigned long)c.hr);
  else if (c.bad)
    verdict(name, "FAIL", "%zu of %zu bytes differ, first at %zu: 0x%02x, want 0x%02x", c.bad, size, c.first, c.got,
            c.want);
  else
    verdict(name, "PASS", "%zu bytes", size);
}

static void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  l->ResourceBarrier(1, &b);
}

#define TRY(what, x)                                                                                                   \
  do {                                                                                                                 \
    HRESULT _hr = (x);                                                                                                 \
    if (FAILED(_hr)) {                                                                                                 \
      verdict(name, "FAIL", "%s hr=0x%08lx", what, (unsigned long)_hr);                                                \
      return;                                                                                                          \
    }                                                                                                                  \
  } while (0)

static void case_committed(Gpu &g) {
  const char *name = "committed";
  const size_t n = 256 * 1024 + 123;
  HRESULT hr;
  ID3D12Resource *up = committed(g, D3D12_HEAP_TYPE_UPLOAD, n, D3D12_RESOURCE_STATE_GENERIC_READ, &hr);
  TRY("upload buffer", hr);
  ID3D12Resource *gpu = committed(g, D3D12_HEAP_TYPE_DEFAULT, n, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
  TRY("default buffer", hr);
  ID3D12Resource *rb = committed(g, D3D12_HEAP_TYPE_READBACK, n, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
  TRY("readback buffer", hr);
  TRY("upload Map", fill(up, n, 1));
  TRY("submit", submit(g, [&](ID3D12GraphicsCommandList *l) {
        l->CopyBufferRegion(gpu, 0, up, 0, n);
        barrier(l, gpu, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(rb, 0, gpu, 0, n);
      }));
  judge(name, compare(rb, n, [](size_t i) { return pattern(i, 1); }), n);
  rb->Release();
  gpu->Release();
  up->Release();
}

static void case_texture(Gpu &g) {
  const char *name = "texture";
  const UINT w = 64, h = 64;
  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = w;
  td.Height = h;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  g.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_bytes, &total);
  if (rows != h || row_bytes != w * 4 || fp.Footprint.RowPitch < w * 4) {
    verdict(name, "FAIL", "footprint rows=%u row_bytes=%llu pitch=%u total=%llu", rows, (unsigned long long)row_bytes,
            fp.Footprint.RowPitch, (unsigned long long)total);
    return;
  }
  HRESULT hr;
  D3D12_HEAP_PROPERTIES def = {};
  def.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource *tex = nullptr;
  TRY("texture", g.dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, __uuidof(ID3D12Resource), (void **)&tex));
  ID3D12Resource *up = committed(g, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ, &hr);
  TRY("upload buffer", hr);
  ID3D12Resource *rb = committed(g, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
  TRY("readback buffer", hr);
  const UINT pitch = fp.Footprint.RowPitch;
  // the texel bytes of row y, x: pattern of their index in the tightly packed image
  auto texel = [&](size_t i) -> int {
    size_t y = i / pitch, x = i % pitch;
    return x < w * 4 && y < h ? (int)pattern(y * w * 4 + x, 2) : -1;
  };
  {
    void *p = nullptr;
    D3D12_RANGE none = {0, 0};
    TRY("upload Map", up->Map(0, &none, &p));
    for (size_t i = 0; i < total; i++)
      ((uint8_t *)p)[i] = texel(i) < 0 ? 0xcd : (uint8_t)texel(i);
    up->Unmap(0, nullptr);
  }
  TRY("submit", submit(g, [&](ID3D12GraphicsCommandList *l) {
        D3D12_TEXTURE_COPY_LOCATION t = {}, b = {};
        t.pResource = tex;
        t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        t.SubresourceIndex = 0;
        b.pResource = up;
        b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        b.PlacedFootprint = fp;
        l->CopyTextureRegion(&t, 0, 0, 0, &b, nullptr);
        barrier(l, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        b.pResource = rb;
        l->CopyTextureRegion(&b, 0, 0, 0, &t, nullptr);
      }));
  // compare only the texel bytes (row padding is undefined after the copy)
  Compared c;
  void *p = nullptr;
  D3D12_RANGE all = {0, (SIZE_T)total};
  c.hr = rb->Map(0, &all, &p);
  if (SUCCEEDED(c.hr) && p) {
    for (size_t i = 0; i < total; i++) {
      int want = texel(i);
      uint8_t got = ((const uint8_t *)p)[i];
      if (want >= 0 && got != want && !c.bad++) {
        c.first = i;
        c.got = got;
        c.want = (unsigned)want;
      }
    }
    D3D12_RANGE none = {0, 0};
    rb->Unmap(0, &none);
  } else if (SUCCEEDED(c.hr)) {
    c.hr = E_POINTER;
  }
  judge(name, c, (size_t)w * h * 4);
  rb->Release();
  up->Release();
  tex->Release();
}

static void case_uavclear(Gpu &g) {
  const char *name = "uavclear";
  const UINT count = 4096;
  const UINT value = 0x5a17c0deu;
  HRESULT hr;
  ID3D12Resource *buf = committed(g, D3D12_HEAP_TYPE_DEFAULT, count * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &hr,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  TRY("default buffer", hr);
  ID3D12Resource *rb = committed(g, D3D12_HEAP_TYPE_READBACK, count * 4, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
  TRY("readback buffer", hr);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *cpu = nullptr, *vis = nullptr;
  TRY("CPU descriptor heap", g.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&cpu));
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  TRY("shader-visible descriptor heap", g.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&vis));
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R32_UINT;
  ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = count;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = cpu->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE vis_cpu = vis->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE vis_gpu = vis->GetGPUDescriptorHandleForHeapStart();
  g.dev->CreateUnorderedAccessView(buf, nullptr, &ud, cpu_handle);
  g.dev->CreateUnorderedAccessView(buf, nullptr, &ud, vis_cpu);
  TRY("submit", submit(g, [&](ID3D12GraphicsCommandList *l) {
        l->SetDescriptorHeaps(1, &vis);
        const UINT values[4] = {value, value, value, value};
        l->ClearUnorderedAccessViewUint(vis_gpu, cpu_handle, buf, values, 0, nullptr);
        barrier(l, buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(rb, 0, buf, 0, count * 4);
      }));
  judge(name, compare(rb, count * 4, [&](size_t i) { return (uint8_t)(value >> (8 * (i % 4))); }), count * 4);
  vis->Release();
  cpu->Release();
  rb->Release();
  buf->Release();
}

// two buffers placed at 0 and 64 KiB in an upload heap -> default -> two buffers placed in a readback heap
static void case_placed(Gpu &g, const char *name, D3D12_HEAP_FLAGS flags) {
  const UINT64 span = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, n = span - 77;
  HRESULT hr;
  ID3D12Heap *uh = heap(g, D3D12_HEAP_TYPE_UPLOAD, 4 * span, flags, &hr);
  if (!uh && flags == D3D12_HEAP_FLAG_NONE) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    if (o.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2) {
      verdict(name, "SKIP", "resource heap tier %d (heaps of mixed resources need tier 2)", o.ResourceHeapTier);
      return;
    }
  }
  TRY("upload heap", hr);
  ID3D12Heap *rh = heap(g, D3D12_HEAP_TYPE_READBACK, 4 * span, flags, &hr);
  TRY("readback heap", hr);
  ID3D12Resource *up[2], *rb[2];
  for (int i = 0; i < 2; i++) {
    up[i] = placed(g, uh, i * span, n, D3D12_RESOURCE_STATE_GENERIC_READ, &hr);
    TRY("placed upload buffer", hr);
    rb[i] = placed(g, rh, (i + 1) * span, n, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
    TRY("placed readback buffer", hr);
  }
  ID3D12Resource *gpu = committed(g, D3D12_HEAP_TYPE_DEFAULT, 2 * span, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
  TRY("default buffer", hr);
  for (int i = 0; i < 2; i++)
    TRY("placed upload Map", fill(up[i], n, 10 + i));
  TRY("submit", submit(g, [&](ID3D12GraphicsCommandList *l) {
        for (int i = 0; i < 2; i++)
          l->CopyBufferRegion(gpu, i * span, up[i], 0, n);
        barrier(l, gpu, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (int i = 0; i < 2; i++)
          l->CopyBufferRegion(rb[i], 0, gpu, i * span, n);
      }));
  Compared c0 = compare(rb[0], n, [](size_t i) { return pattern(i, 10); });
  Compared c1 = compare(rb[1], n, [](size_t i) { return pattern(i, 11); });
  if (FAILED(c0.hr) || c0.bad)
    judge(name, c0, n);
  else
    judge(name, c1, 2 * n);
  gpu->Release();
  for (int i = 0; i < 2; i++) {
    rb[i]->Release();
    up[i]->Release();
  }
  rh->Release();
  uh->Release();
}

static const char kShader[] = R"(
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { dst.Store(id.x * 4, src.Load(id.x * 4) * 3u + 1u); }
)";

static void case_compute(Gpu &g) {
  const char *name = "compute";
  HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
  auto compile = dc ? (pD3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : nullptr;
  if (!compile) {
    verdict(name, "SKIP", "no D3DCompile (d3dcompiler_47.dll)");
    return;
  }
  ID3DBlob *cs = nullptr, *err = nullptr;
  HRESULT hr = compile(kShader, sizeof kShader - 1, "d3d12_memory", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
  if (FAILED(hr)) {
    verdict(name, "SKIP", "D3DCompile hr=0x%08lx %s", (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : "");
    return;
  }
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[0].ShaderVisibility = params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *rs_blob = nullptr;
  TRY("D3D12SerializeRootSignature", D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err));
  ID3D12RootSignature *rs = nullptr;
  TRY("CreateRootSignature", g.dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                        __uuidof(ID3D12RootSignature), (void **)&rs));
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = rs;
  pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  ID3D12PipelineState *pso = nullptr;
  TRY("CreateComputePipelineState", g.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso));
  const UINT count = 16384;
  const UINT64 span = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  ID3D12Heap *uh = heap(g, D3D12_HEAP_TYPE_UPLOAD, 2 * span, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, &hr);
  TRY("upload heap", hr);
  // the input is placed at 64 KiB: its GPU address is the heap's plus the offset
  ID3D12Resource *in = placed(g, uh, span, count * 4, D3D12_RESOURCE_STATE_GENERIC_READ, &hr);
  TRY("placed upload buffer", hr);
  ID3D12Resource *res = committed(g, D3D12_HEAP_TYPE_DEFAULT, count * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &hr,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  TRY("default buffer", hr);
  ID3D12Resource *rb = committed(g, D3D12_HEAP_TYPE_READBACK, count * 4, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
  TRY("readback buffer", hr);
  {
    void *p = nullptr;
    D3D12_RANGE none = {0, 0};
    TRY("upload Map", in->Map(0, &none, &p));
    for (UINT i = 0; i < count; i++)
      ((uint32_t *)p)[i] = i * 5u + 7u;
    in->Unmap(0, nullptr);
  }
  TRY("submit", submit(g, [&](ID3D12GraphicsCommandList *l) {
        l->SetComputeRootSignature(rs);
        l->SetPipelineState(pso);
        l->SetComputeRootShaderResourceView(0, in->GetGPUVirtualAddress());
        l->SetComputeRootUnorderedAccessView(1, res->GetGPUVirtualAddress());
        l->Dispatch(count / 64, 1, 1);
        barrier(l, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(rb, 0, res, 0, count * 4);
      }));
  judge(name, compare(rb, count * 4, [](size_t i) {
          uint32_t v = ((uint32_t)(i / 4) * 5u + 7u) * 3u + 1u;
          return (uint8_t)(v >> (8 * (i % 4)));
        }),
        count * 4);
  rb->Release();
  res->Release();
  in->Release();
  uh->Release();
  pso->Release();
  rs->Release();
  rs_blob->Release();
  cs->Release();
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      FILE *f = fopen(argv[++i], "w");
      if (!f) {
        fprintf(stderr, "cannot write %s\n", argv[i]);
        return 2;
      }
      out = f;
    } else if (!strcmp(argv[i], "--case") && i + 1 < argc) {
      only.push_back(argv[++i]);
    } else {
      fprintf(stderr, "usage: d3d12_memory.exe [--out FILE] [--case NAME]...\n");
      return 2;
    }
  }
  char config[256] = "";
  GetEnvironmentVariableA("MULLION_CONFIG", config, sizeof config);
  say("INFO MULLION_CONFIG=%s", config[0] ? config : "(unset)");
  IDXGIFactory4 *factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&factory))) {
    IDXGIAdapter1 *adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
      DXGI_ADAPTER_DESC1 d = {};
      adapter->GetDesc1(&d);
      char desc[256] = "";
      WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, desc, sizeof desc, nullptr, nullptr);
      say("INFO adapter \"%s\" vendor 0x%04x device 0x%04x memory %llu MiB", desc, d.VendorId, d.DeviceId,
          (unsigned long long)(d.DedicatedVideoMemory >> 20));
      adapter->Release();
    }
    factory->Release();
  }
  Gpu g;
  HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&g.dev);
  if (FAILED(hr)) {
    say("RESULT passed=0 failed=1 skipped=0 (D3D12CreateDevice hr=0x%08lx)", (unsigned long)hr);
    return 1;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
  g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
  say("INFO resource heap tier %d, binding tier %d", o.ResourceHeapTier, o.ResourceBindingTier);
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
  g.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(hr = g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&g.queue)) ||
      FAILED(hr = g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                (void **)&g.alloc)) ||
      FAILED(hr = g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr,
                                           __uuidof(ID3D12GraphicsCommandList), (void **)&g.list)) ||
      FAILED(hr = g.list->Close()) ||
      FAILED(hr = g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&g.fence))) {
    say("RESULT passed=0 failed=1 skipped=0 (queue setup hr=0x%08lx)", (unsigned long)hr);
    return 1;
  }
  if (wanted("committed"))
    case_committed(g);
  if (wanted("texture"))
    case_texture(g);
  if (wanted("uavclear"))
    case_uavclear(g);
  if (wanted("placed"))
    case_placed(g, "placed", D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
  if (wanted("placednone"))
    case_placed(g, "placednone", D3D12_HEAP_FLAG_NONE);
  if (wanted("compute"))
    case_compute(g);
  say("RESULT passed=%d failed=%d skipped=%d", passed, failed, skipped);
  return failed ? 1 : 0;
}
