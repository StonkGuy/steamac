/* d3dr-server.exe: replays a d3dr client's Direct3D 11 stream (d3dr_proto.h) against the real D3D11 of the Wine it
 * runs in. On the Mac that is a CrossOver-derived Wine with Apple's D3DMetal enabled (the user's own install; this
 * program only uses the public D3D11 API). Presents into its own window.
 *
 * Usage: d3dr-server.exe [--port N] [--bind ADDR] [--once] [--log]
 *   --once  exit after the first client disconnects (tests)
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include "d3dr_proto.h"

static bool g_log = false;
#define LOG(...) do { if (g_log) { fprintf(stderr, "d3dr-server: " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)
#define ERR(...) do { fprintf(stderr, "d3dr-server: " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) return 0; /* the client owns the lifetime */
    return DefWindowProcW(h, m, w, l);
}

static void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

struct Session {
    SOCKET s;
    std::unordered_map<uint32_t, IUnknown *> objs;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HWND hwnd = nullptr;
    uint64_t ops = 0;

    explicit Session(SOCKET s) : s(s) {}
    ~Session() {
        if (ctx) { ctx->ClearState(); ctx->Flush(); }
        for (auto &o : objs) o.second->Release();
        if (hwnd) DestroyWindow(hwnd);
    }
    template <class T> T *get(uint32_t h) {
        if (!h) return nullptr;
        auto it = objs.find(h);
        return it == objs.end() ? nullptr : static_cast<T *>(it->second);
    }
    void put(uint32_t h, IUnknown *o) {
        if (!o) return;
        auto it = objs.find(h);
        if (it != objs.end()) it->second->Release();
        objs[h] = o;
    }
    bool send_all(const void *d, size_t n) {
        const char *p = (const char *)d;
        while (n) { int k = send(s, p, (int)n, 0); if (k <= 0) return false; p += k; n -= (size_t)k; }
        return true;
    }
    bool reply(uint32_t op, const d3dr_writer &body) {
        d3dr_msg m = {op, (uint32_t)body.buf.size()};
        return send_all(&m, sizeof m) && (body.buf.empty() || send_all(body.buf.data(), body.buf.size()));
    }
    ID3D11Resource *res(uint32_t h) { return get<ID3D11Resource>(h); }

    HWND make_window(UINT w, UINT h) {
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"d3dr";
            RegisterClassW(&wc); reg = true;
        }
        RECT rc = {0, 0, (LONG)w, (LONG)h}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        /* never take focus from the user's foreground app */
        char e[8] = "";
        DWORD ex = GetEnvironmentVariableA("D3DR_ACTIVATE", e, sizeof e) && e[0] == '1' ? 0 : WS_EX_NOACTIVATE;
        HWND hw = CreateWindowExW(ex, L"d3dr", L"d3dr (D3DMetal)", WS_OVERLAPPEDWINDOW, 60, 60,
                                  rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(hw, ex ? SW_SHOWNOACTIVATE : SW_SHOW);
        return hw;
    }

    bool create_device(d3dr_reader &r) {
        uint32_t hdev = r.u32(), hctx = r.u32(), hsc = r.u32(), nfl = r.u32();
        std::vector<D3D_FEATURE_LEVEL> fls;
        for (uint32_t i = 0; i < nfl && !r.bad; i++) fls.push_back((D3D_FEATURE_LEVEL)r.u32());
        uint32_t flags = r.u32();
        DXGI_SWAP_CHAIN_DESC sd = r.pod<DXGI_SWAP_CHAIN_DESC>();
        HRESULT hr = E_INVALIDARG;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device *d = nullptr; ID3D11DeviceContext *c = nullptr; IDXGISwapChain *sc = nullptr;
        if (!r.bad) {
            if (hsc) {
                if (!hwnd) hwnd = make_window(sd.BufferDesc.Width, sd.BufferDesc.Height);
                sd.OutputWindow = hwnd;
                sd.Windowed = TRUE;
                hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls.empty() ? nullptr : fls.data(),
                                                   (UINT)fls.size(), D3D11_SDK_VERSION, &sd, &sc, &d, &fl, &c);
            } else {
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls.empty() ? nullptr : fls.data(),
                                       (UINT)fls.size(), D3D11_SDK_VERSION, &d, &fl, &c);
            }
        }
        LOG("create device: hr 0x%08lx fl 0x%x %ux%u swapchain %u", (unsigned long)hr, fl, sd.BufferDesc.Width, sd.BufferDesc.Height, hsc);
        if (SUCCEEDED(hr)) {
            put(hdev, d); put(hctx, c); if (sc) put(hsc, sc);
            if (!dev) { dev = d; ctx = c; }
        }
        d3dr_writer w; w.u32((uint32_t)hr); w.u32(fl);
        return reply(D3DR_R_DEVICE, w);
    }

    void set_stage(uint32_t op, d3dr_reader &r) {
        uint32_t stage = r.u32(), start = r.u32(), n = r.u32();
        if (n > 128 || r.bad) { r.bad = true; return; }
        void *v[128];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t h = r.u32();
            v[i] = h ? (void *)get<IUnknown>(h) : nullptr;
        }
        typedef void(STDMETHODCALLTYPE ID3D11DeviceContext::*Fn)(UINT, UINT, void *const *);
        static const Fn cb[] = {(Fn)&ID3D11DeviceContext::VSSetConstantBuffers, (Fn)&ID3D11DeviceContext::PSSetConstantBuffers,
                                (Fn)&ID3D11DeviceContext::GSSetConstantBuffers, (Fn)&ID3D11DeviceContext::HSSetConstantBuffers,
                                (Fn)&ID3D11DeviceContext::DSSetConstantBuffers, (Fn)&ID3D11DeviceContext::CSSetConstantBuffers};
        static const Fn srv[] = {(Fn)&ID3D11DeviceContext::VSSetShaderResources, (Fn)&ID3D11DeviceContext::PSSetShaderResources,
                                 (Fn)&ID3D11DeviceContext::GSSetShaderResources, (Fn)&ID3D11DeviceContext::HSSetShaderResources,
                                 (Fn)&ID3D11DeviceContext::DSSetShaderResources, (Fn)&ID3D11DeviceContext::CSSetShaderResources};
        static const Fn smp[] = {(Fn)&ID3D11DeviceContext::VSSetSamplers, (Fn)&ID3D11DeviceContext::PSSetSamplers,
                                 (Fn)&ID3D11DeviceContext::GSSetSamplers, (Fn)&ID3D11DeviceContext::HSSetSamplers,
                                 (Fn)&ID3D11DeviceContext::DSSetSamplers, (Fn)&ID3D11DeviceContext::CSSetSamplers};
        if (stage >= D3DR_STAGE_COUNT) { r.bad = true; return; }
        const Fn *t = op == D3DR_SET_CB ? cb : op == D3DR_SET_SRV ? srv : smp;
        (ctx->*t[stage])(start, n, v);
    }

    void set_shader(uint32_t stage, uint32_t h) {
        IUnknown *o = get<IUnknown>(h);
        switch (stage) {
        case D3DR_STAGE_VS: ctx->VSSetShader((ID3D11VertexShader *)o, nullptr, 0); break;
        case D3DR_STAGE_PS: ctx->PSSetShader((ID3D11PixelShader *)o, nullptr, 0); break;
        case D3DR_STAGE_GS: ctx->GSSetShader((ID3D11GeometryShader *)o, nullptr, 0); break;
        case D3DR_STAGE_HS: ctx->HSSetShader((ID3D11HullShader *)o, nullptr, 0); break;
        case D3DR_STAGE_DS: ctx->DSSetShader((ID3D11DomainShader *)o, nullptr, 0); break;
        case D3DR_STAGE_CS: ctx->CSSetShader((ID3D11ComputeShader *)o, nullptr, 0); break;
        }
    }

    /* copy `rows` x `depth` rows of `pitch` bytes between differently pitched layouts */
    static void copy_rows(uint8_t *dst, UINT drp, UINT ddp, const uint8_t *src, UINT srp, UINT rows, UINT depth, UINT row_bytes) {
        for (UINT z = 0; z < depth; z++)
            for (UINT y = 0; y < rows; y++)
                memcpy(dst + (size_t)z * ddp + (size_t)y * drp, src + ((size_t)z * rows + y) * srp, row_bytes);
    }

    bool handle(uint32_t op, d3dr_reader &r) {
        ops++;
        if (op != D3DR_HELLO && op != D3DR_CREATE_DEVICE && !dev) { ERR("op %u before device creation", op); return false; }
        switch (op) {
        case D3DR_HELLO: {
            uint32_t v = r.u32();
            if (v != D3DR_VERSION) { ERR("client protocol %u, server %u", v, D3DR_VERSION); return false; }
            d3dr_writer w; w.u32(D3DR_VERSION); return reply(D3DR_R_HELLO, w);
        }
        case D3DR_CREATE_DEVICE: return create_device(r);
        case D3DR_SC_GET_BUFFER: {
            uint32_t hsc = r.u32(), idx = r.u32(), ht = r.u32();
            IDXGISwapChain *sc = get<IDXGISwapChain>(hsc);
            ID3D11Texture2D *t = nullptr;
            if (sc && SUCCEEDED(sc->GetBuffer(idx, __uuidof(ID3D11Texture2D), (void **)&t))) put(ht, t);
            else ERR("GetBuffer(%u) failed", idx);
            break;
        }
        case D3DR_SC_PRESENT: {
            uint32_t hsc = r.u32(), sync = r.u32(), flags = r.u32(); uint64_t serial = r.u64();
            IDXGISwapChain *sc = get<IDXGISwapChain>(hsc);
            HRESULT hr = sc ? sc->Present(sync, flags) : E_INVALIDARG;
            if (FAILED(hr)) ERR("Present failed 0x%08lx", (unsigned long)hr);
            pump();
            d3dr_writer w; w.u64(serial);
            return reply(D3DR_R_PRESENT, w);
        }
        case D3DR_RELEASE: {
            uint32_t h = r.u32();
            auto it = objs.find(h);
            if (it != objs.end()) {
                if (it->second == dev || it->second == ctx) break; /* kept for the session */
                it->second->Release(); objs.erase(it);
            }
            break;
        }
        case D3DR_CREATE_BUFFER: {
            uint32_t h = r.u32(); D3D11_BUFFER_DESC d = r.pod<D3D11_BUFFER_DESC>(); uint32_t has = r.u32();
            const uint8_t *init = has ? r.take(d.ByteWidth) : nullptr;
            if (r.bad) return false;
            D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
            ID3D11Buffer *b = nullptr;
            HRESULT hr = dev->CreateBuffer(&d, init ? &sd : nullptr, &b);
            if (FAILED(hr)) ERR("CreateBuffer(%u bytes) failed 0x%08lx", d.ByteWidth, (unsigned long)hr);
            put(h, b);
            break;
        }
        case D3DR_CREATE_TEXTURE2D: {
            uint32_t h = r.u32(); D3D11_TEXTURE2D_DESC d = r.pod<D3D11_TEXTURE2D_DESC>(); uint32_t has = r.u32();
            UINT mips = d.MipLevels;
            if (!mips) { UINT x = d.Width > d.Height ? d.Width : d.Height; mips = 1; while (x > 1) { x >>= 1; mips++; } }
            std::vector<D3D11_SUBRESOURCE_DATA> sd;
            if (has) {
                for (UINT i = 0; i < mips * d.ArraySize && !r.bad; i++) {
                    uint32_t pitch = r.u32(), len; const uint8_t *p = r.blob(&len);
                    sd.push_back({p, pitch, 0});
                }
            }
            if (r.bad) return false;
            ID3D11Texture2D *t = nullptr;
            HRESULT hr = dev->CreateTexture2D(&d, has ? sd.data() : nullptr, &t);
            if (FAILED(hr)) ERR("CreateTexture2D(%ux%u fmt %u) failed 0x%08lx", d.Width, d.Height, d.Format, (unsigned long)hr);
            put(h, t);
            break;
        }
        case D3DR_CREATE_VIEW: {
            uint32_t kind = r.u32(), h = r.u32(), hr_ = r.u32(), has = r.u32();
            ID3D11Resource *rs = res(hr_);
            HRESULT hr = E_INVALIDARG; IUnknown *v = nullptr;
            switch (kind) {
            case D3DR_VIEW_SRV: { D3D11_SHADER_RESOURCE_VIEW_DESC d = {}; if (has) d = r.pod<D3D11_SHADER_RESOURCE_VIEW_DESC>();
                if (rs) hr = dev->CreateShaderResourceView(rs, has ? &d : nullptr, (ID3D11ShaderResourceView **)&v); break; }
            case D3DR_VIEW_RTV: { D3D11_RENDER_TARGET_VIEW_DESC d = {}; if (has) d = r.pod<D3D11_RENDER_TARGET_VIEW_DESC>();
                if (rs) hr = dev->CreateRenderTargetView(rs, has ? &d : nullptr, (ID3D11RenderTargetView **)&v); break; }
            case D3DR_VIEW_DSV: { D3D11_DEPTH_STENCIL_VIEW_DESC d = {}; if (has) d = r.pod<D3D11_DEPTH_STENCIL_VIEW_DESC>();
                if (rs) hr = dev->CreateDepthStencilView(rs, has ? &d : nullptr, (ID3D11DepthStencilView **)&v); break; }
            case D3DR_VIEW_UAV: { D3D11_UNORDERED_ACCESS_VIEW_DESC d = {}; if (has) d = r.pod<D3D11_UNORDERED_ACCESS_VIEW_DESC>();
                if (rs) hr = dev->CreateUnorderedAccessView(rs, has ? &d : nullptr, (ID3D11UnorderedAccessView **)&v); break; }
            }
            if (FAILED(hr)) ERR("create view kind %u failed 0x%08lx", kind, (unsigned long)hr);
            put(h, v);
            break;
        }
        case D3DR_CREATE_SHADER: {
            uint32_t stage = r.u32(), h = r.u32(), n; const uint8_t *code = r.blob(&n);
            if (r.bad) return false;
            HRESULT hr = E_INVALIDARG; IUnknown *s = nullptr;
            switch (stage) {
            case D3DR_STAGE_VS: hr = dev->CreateVertexShader(code, n, nullptr, (ID3D11VertexShader **)&s); break;
            case D3DR_STAGE_PS: hr = dev->CreatePixelShader(code, n, nullptr, (ID3D11PixelShader **)&s); break;
            case D3DR_STAGE_GS: hr = dev->CreateGeometryShader(code, n, nullptr, (ID3D11GeometryShader **)&s); break;
            case D3DR_STAGE_HS: hr = dev->CreateHullShader(code, n, nullptr, (ID3D11HullShader **)&s); break;
            case D3DR_STAGE_DS: hr = dev->CreateDomainShader(code, n, nullptr, (ID3D11DomainShader **)&s); break;
            case D3DR_STAGE_CS: hr = dev->CreateComputeShader(code, n, nullptr, (ID3D11ComputeShader **)&s); break;
            }
            if (FAILED(hr)) ERR("create shader stage %u failed 0x%08lx", stage, (unsigned long)hr);
            put(h, s);
            break;
        }
        case D3DR_CREATE_INPUT_LAYOUT: {
            uint32_t h = r.u32(), n = r.u32();
            if (n > 64) return false;
            std::vector<D3D11_INPUT_ELEMENT_DESC> e(n);
            std::vector<std::string> names(n);
            for (uint32_t i = 0; i < n && !r.bad; i++) {
                e[i] = r.pod<D3D11_INPUT_ELEMENT_DESC>();
                uint32_t len; const uint8_t *s = r.blob(&len);
                names[i].assign((const char *)s, len);
            }
            for (uint32_t i = 0; i < n; i++) e[i].SemanticName = names[i].c_str();
            uint32_t len; const uint8_t *code = r.blob(&len);
            if (r.bad) return false;
            ID3D11InputLayout *il = nullptr;
            HRESULT hr = dev->CreateInputLayout(e.data(), n, code, len, &il);
            if (FAILED(hr)) ERR("CreateInputLayout failed 0x%08lx", (unsigned long)hr);
            put(h, il);
            break;
        }
        case D3DR_CREATE_STATE: {
            uint32_t kind = r.u32(), h = r.u32();
            HRESULT hr = E_INVALIDARG; IUnknown *s = nullptr;
            switch (kind) {
            case D3DR_STATE_SAMPLER: { auto d = r.pod<D3D11_SAMPLER_DESC>(); hr = dev->CreateSamplerState(&d, (ID3D11SamplerState **)&s); break; }
            case D3DR_STATE_BLEND: { auto d = r.pod<D3D11_BLEND_DESC>(); hr = dev->CreateBlendState(&d, (ID3D11BlendState **)&s); break; }
            case D3DR_STATE_RASTERIZER: { auto d = r.pod<D3D11_RASTERIZER_DESC>(); hr = dev->CreateRasterizerState(&d, (ID3D11RasterizerState **)&s); break; }
            case D3DR_STATE_DEPTH_STENCIL: { auto d = r.pod<D3D11_DEPTH_STENCIL_DESC>(); hr = dev->CreateDepthStencilState(&d, (ID3D11DepthStencilState **)&s); break; }
            }
            if (FAILED(hr)) ERR("create state kind %u failed 0x%08lx", kind, (unsigned long)hr);
            put(h, s);
            break;
        }
        case D3DR_CLEAR_RTV: { auto v = get<ID3D11RenderTargetView>(r.u32()); float c[4]; r.get(c, 16); if (v) ctx->ClearRenderTargetView(v, c); break; }
        case D3DR_CLEAR_DSV: {
            auto v = get<ID3D11DepthStencilView>(r.u32()); uint32_t f = r.u32(); float d = r.f32(); uint32_t s = r.u32();
            if (v) ctx->ClearDepthStencilView(v, f, d, (UINT8)s);
            break;
        }
        case D3DR_SET_VIEWPORTS: {
            uint32_t n = r.u32(); if (n > 16) return false;
            D3D11_VIEWPORT v[16]; r.get(v, n * sizeof *v); ctx->RSSetViewports(n, v); break;
        }
        case D3DR_SET_SCISSORS: {
            uint32_t n = r.u32(); if (n > 16) return false;
            D3D11_RECT v[16]; r.get(v, n * sizeof *v); ctx->RSSetScissorRects(n, v); break;
        }
        case D3DR_SET_RTV: {
            uint32_t n = r.u32(); if (n > 8) return false;
            ID3D11RenderTargetView *v[8];
            for (uint32_t i = 0; i < n; i++) v[i] = get<ID3D11RenderTargetView>(r.u32());
            ctx->OMSetRenderTargets(n, v, get<ID3D11DepthStencilView>(r.u32()));
            break;
        }
        case D3DR_SET_BLEND: { auto b = get<ID3D11BlendState>(r.u32()); float f[4]; r.get(f, 16); ctx->OMSetBlendState(b, f, r.u32()); break; }
        case D3DR_SET_DS_STATE: { auto s = get<ID3D11DepthStencilState>(r.u32()); ctx->OMSetDepthStencilState(s, r.u32()); break; }
        case D3DR_SET_RS_STATE: ctx->RSSetState(get<ID3D11RasterizerState>(r.u32())); break;
        case D3DR_SET_VB: {
            uint32_t start = r.u32(), n = r.u32(); if (n > 32) return false;
            ID3D11Buffer *b[32]; UINT st[32], off[32];
            for (uint32_t i = 0; i < n; i++) { b[i] = get<ID3D11Buffer>(r.u32()); st[i] = r.u32(); off[i] = r.u32(); }
            ctx->IASetVertexBuffers(start, n, b, st, off);
            break;
        }
        case D3DR_SET_IB: { auto b = get<ID3D11Buffer>(r.u32()); uint32_t f = r.u32(); ctx->IASetIndexBuffer(b, (DXGI_FORMAT)f, r.u32()); break; }
        case D3DR_SET_LAYOUT: ctx->IASetInputLayout(get<ID3D11InputLayout>(r.u32())); break;
        case D3DR_SET_TOPOLOGY: ctx->IASetPrimitiveTopology((D3D11_PRIMITIVE_TOPOLOGY)r.u32()); break;
        case D3DR_SET_SHADER: { uint32_t st = r.u32(); set_shader(st, r.u32()); break; }
        case D3DR_SET_CB: case D3DR_SET_SRV: case D3DR_SET_SAMPLERS: set_stage(op, r); break;
        case D3DR_DRAW: { uint32_t a = r.u32(); ctx->Draw(a, r.u32()); break; }
        case D3DR_DRAW_INDEXED: { uint32_t a = r.u32(), b = r.u32(); ctx->DrawIndexed(a, b, (INT)r.u32()); break; }
        case D3DR_DRAW_INSTANCED: { uint32_t a = r.u32(), b = r.u32(), c = r.u32(); ctx->DrawInstanced(a, b, c, r.u32()); break; }
        case D3DR_DRAW_INDEXED_INSTANCED: {
            uint32_t a = r.u32(), b = r.u32(), c = r.u32(); INT d = (INT)r.u32(); ctx->DrawIndexedInstanced(a, b, c, d, r.u32()); break;
        }
        case D3DR_WRITE_MAPPED: {
            uint32_t h = r.u32(), sub = r.u32(), type = r.u32(), pitch = r.u32(), rows = r.u32(), depth = r.u32();
            const uint8_t *data = r.take((size_t)pitch * rows * depth);
            ID3D11Resource *rs = res(h);
            if (r.bad || !rs) return !r.bad;
            if (type == D3D11_MAP_READ_WRITE) type = D3D11_MAP_WRITE;
            D3D11_MAPPED_SUBRESOURCE ms;
            HRESULT hr = ctx->Map(rs, sub, (D3D11_MAP)type, 0, &ms);
            if (FAILED(hr)) { ERR("Map(%u, write type %u) failed 0x%08lx", h, type, (unsigned long)hr); break; }
            if (rows == 1 && depth == 1) memcpy(ms.pData, data, pitch);
            else copy_rows((uint8_t *)ms.pData, ms.RowPitch, ms.DepthPitch, data, pitch, rows, depth, pitch);
            ctx->Unmap(rs, sub);
            break;
        }
        case D3DR_UPDATE_SUBRESOURCE: {
            uint32_t h = r.u32(), sub = r.u32(), has_box = r.u32(); D3D11_BOX box = r.pod<D3D11_BOX>();
            uint32_t rp = r.u32(), dp = r.u32(), len; const uint8_t *data = r.blob(&len);
            ID3D11Resource *rs = res(h);
            if (r.bad) return false;
            if (rs && data) ctx->UpdateSubresource(rs, sub, has_box ? &box : nullptr, data, rp, dp);
            break;
        }
        case D3DR_COPY_RESOURCE: { auto d = res(r.u32()); auto s = res(r.u32()); if (d && s) ctx->CopyResource(d, s); break; }
        case D3DR_READ_MAPPED: {
            uint32_t h = r.u32(), sub = r.u32(), pitch = r.u32(), rows = r.u32(), depth = r.u32();
            ID3D11Resource *rs = res(h);
            d3dr_writer w;
            D3D11_MAPPED_SUBRESOURCE ms;
            HRESULT hr = rs ? ctx->Map(rs, sub, D3D11_MAP_READ, 0, &ms) : E_INVALIDARG;
            w.u32((uint32_t)hr);
            if (SUCCEEDED(hr)) {
                size_t at = w.buf.size();
                w.buf.resize(at + (size_t)pitch * rows * depth);
                for (UINT z = 0; z < depth; z++)
                    for (UINT y = 0; y < rows; y++)
                        memcpy(&w.buf[at + ((size_t)z * rows + y) * pitch], (uint8_t *)ms.pData + (size_t)z * ms.DepthPitch + (size_t)y * ms.RowPitch, pitch);
                ctx->Unmap(rs, sub);
            }
            return reply(D3DR_R_READ, w);
        }
        default:
            ERR("unknown op %u", op);
            return false;
        }
        if (r.bad) { ERR("short payload for op %u", op); return false; }
        return true;
    }
};

/* Socket input in large reads: a Wine socket call costs tens of microseconds, and a frame carries hundreds of
 * small messages. */
struct Input {
    SOCKET s;
    std::vector<uint8_t> buf = std::vector<uint8_t>(4 << 20);
    size_t head = 0, tail = 0;
    size_t avail() const { return tail - head; }
    /* read more; false = connection closed */
    bool fill(size_t need) {
        if (head && (buf.size() - head < need || head == tail)) { memmove(buf.data(), buf.data() + head, avail()); tail -= head; head = 0; }
        if (buf.size() < need) buf.resize(need);
        while (avail() < need) {
            int k = recv(s, (char *)buf.data() + tail, (int)(buf.size() - tail), 0);
            if (k <= 0) return false;
            tail += (size_t)k;
        }
        return true;
    }
};

static void serve(SOCKET c) {
    Session ss(c);
    Input in{c};
    for (;;) {
        if (in.avail() < sizeof(d3dr_msg)) {
            /* idle: keep the window responsive while waiting for the client */
            fd_set rf; FD_ZERO(&rf); FD_SET(c, &rf);
            timeval tv = {0, 10000};
            if (!in.avail() && select(0, &rf, nullptr, nullptr, &tv) == 0) { pump(); continue; }
            if (!in.fill(sizeof(d3dr_msg))) break;
        }
        d3dr_msg m;
        memcpy(&m, in.buf.data() + in.head, sizeof m);
        if (m.len > D3DR_MAX_MSG) { ERR("message too large (%u)", m.len); break; }
        if (in.avail() < sizeof m + m.len && !in.fill(sizeof m + m.len)) break;
        d3dr_reader r(in.buf.data() + in.head + sizeof m, m.len);
        in.head += sizeof m + m.len;
        ULONGLONG t0 = GetTickCount64();
        bool ok = ss.handle(m.op, r);
        ULONGLONG dt = GetTickCount64() - t0;
        if (dt >= 500) ERR("op %u took %llu ms", m.op, (unsigned long long)dt);
        if (!ok) { ERR("closing the session after op %u", m.op); break; }
    }
    LOG("client gone after %llu ops", (unsigned long long)ss.ops);
}

int main(int argc, char **argv) {
    unsigned port = D3DR_DEFAULT_PORT;
    const char *bind_addr = "127.0.0.1";
    bool once = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else if (!strcmp(argv[i], "--once")) once = true;
        else if (!strcmp(argv[i], "--log")) g_log = true;
        else { fprintf(stderr, "usage: d3dr-server.exe [--port N] [--bind ADDR] [--once] [--log]\n"); return 2; }
    }
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); inet_pton(AF_INET, bind_addr, &a.sin_addr);
    if (bind(l, (sockaddr *)&a, sizeof a) || listen(l, 1)) { ERR("cannot listen on %s:%u (%d)", bind_addr, port, WSAGetLastError()); return 1; }
    printf("d3dr-server: listening on %s:%u\n", bind_addr, port); fflush(stdout);
    do {
        SOCKET c = accept(l, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        int big = 4 << 20; setsockopt(c, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
        LOG("client connected");
        serve(c);
        closesocket(c);
    } while (!once);
    return 0;
}
