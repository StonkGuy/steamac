/* d3dr client: a d3d11.dll that remotes the Direct3D 11 API to d3dr-server.exe (which runs the real D3D11, e.g.
 * Apple's D3DMetal in a macOS Wine). Objects are client-side shells holding a handle and their description;
 * calls are encoded into a stream (d3dr_proto.h) and sent in batches; only device creation, Map for reading and
 * Present pacing wait for the server.
 *
 * Server address: $D3DR_SERVER (host:port, default 127.0.0.1:47811). Logging: $D3DR_LOG=1.
 * Scope (M1): the D3D11 subset listed in d3dr_proto.h; every other method logs once and fails (stub bases
 * generated from the mingw-w64 headers, tools/gen_stubs.py).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "d3dr_format.h"
#include "d3dr_proto.h"

static bool g_log = false;
#define LOG(...) do { if (g_log) { fprintf(stderr, "d3dr: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define ERR(...) do { fprintf(stderr, "d3dr: " __VA_ARGS__); fputc('\n', stderr); } while (0)

static void d3dr_unimpl(const char *what) {
    static std::mutex m;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> l(m);
    if (seen.insert(what).second) ERR("unimplemented %s", what);
}
#include "stubs.h"

/* ---------------------------------------------------------------- connection */
struct Conn {
    SOCKET s = INVALID_SOCKET;
    std::recursive_mutex mu;
    d3dr_writer w;
    uint64_t presents_sent = 0, presents_done = 0;
    bool dead = false;

    bool connect_server() {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd)) return false;
        char addr[256] = "127.0.0.1";
        unsigned port = D3DR_DEFAULT_PORT;
        char env[256];
        if (GetEnvironmentVariableA("D3DR_SERVER", env, sizeof env) && env[0]) {
            char *c = strrchr(env, ':');
            if (c) { *c = 0; port = (unsigned)atoi(c + 1); }
            snprintf(addr, sizeof addr, "%s", env);
        }
        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char ps[16]; snprintf(ps, sizeof ps, "%u", port);
        if (getaddrinfo(addr, ps, &hints, &res) || !res) { ERR("cannot resolve %s", addr); return false; }
        s = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
        int rc = s == INVALID_SOCKET ? -1 : connect(s, res->ai_addr, (int)res->ai_addrlen);
        freeaddrinfo(res);
        if (rc) { ERR("cannot connect to d3dr server %s:%u (%d)", addr, port, WSAGetLastError()); closesocket(s); s = INVALID_SOCKET; return false; }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        int big = 4 << 20;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof big);
        w.begin(D3DR_HELLO); w.u32(D3DR_VERSION); w.end();
        std::vector<uint8_t> r;
        if (!wait_reply(D3DR_R_HELLO, r) || r.size() < 4) { ERR("no hello from the server"); return false; }
        LOG("connected to %s:%u", addr, port);
        return true;
    }
    bool send_all(const uint8_t *p, size_t n) {
        while (n && !dead) {
            int k = send(s, (const char *)p, (int)std::min<size_t>(n, 1 << 30), 0);
            if (k <= 0) { ERR("send failed (%d); server gone", WSAGetLastError()); dead = true; return false; }
            p += k; n -= (size_t)k;
        }
        return !dead;
    }
    bool recv_all(void *d, size_t n) {
        uint8_t *p = (uint8_t *)d;
        while (n && !dead) {
            int k = recv(s, (char *)p, (int)n, 0);
            if (k <= 0) { ERR("recv failed (%d); server gone", WSAGetLastError()); dead = true; return false; }
            p += k; n -= (size_t)k;
        }
        return !dead;
    }
    void flush() {
        if (w.buf.empty()) return;
        send_all(w.buf.data(), w.buf.size());
        w.buf.clear();
    }
    void maybe_flush() { if (w.buf.size() >= (1u << 20)) flush(); }
    /* Read replies until one of type `op` arrives (present acks on the way are counted). */
    bool wait_reply(uint32_t op, std::vector<uint8_t> &out) {
        flush();
        for (;;) {
            d3dr_msg m;
            if (!recv_all(&m, sizeof m) || m.len > D3DR_MAX_MSG) { dead = true; return false; }
            std::vector<uint8_t> body(m.len);
            if (m.len && !recv_all(body.data(), m.len)) return false;
            if (m.op == D3DR_R_PRESENT) { presents_done++; if (op == D3DR_R_PRESENT) { out.swap(body); return true; } continue; }
            if (m.op == op) { out.swap(body); return true; }
            ERR("unexpected reply 0x%x", m.op);
        }
    }
};
static Conn *g_conn;
static std::atomic<uint32_t> g_next_handle{1};
static uint32_t new_handle() { return g_next_handle++; }

/* ---------------------------------------------------------------- common object plumbing */
class Device;
template <class Base> struct Obj : Base {
    std::atomic<ULONG> refs{1};
    uint32_t h = 0;
    Device *dev = nullptr;
    std::vector<std::pair<GUID, std::vector<uint8_t>>> priv;
    virtual ~Obj() {}
    virtual bool is(REFIID riid) = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) || is(riid)) { *pp = this; this->AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override;
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **pp);
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *size, void *data) {
        for (auto &p : priv) if (p.first == g) {
            if (!data) { *size = (UINT)p.second.size(); return S_OK; }
            if (*size < p.second.size()) { *size = (UINT)p.second.size(); return DXGI_ERROR_MORE_DATA; }
            *size = (UINT)p.second.size(); memcpy(data, p.second.data(), p.second.size()); return S_OK;
        }
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT size, const void *data) {
        for (size_t i = 0; i < priv.size(); i++) if (priv[i].first == g) { priv.erase(priv.begin() + i); break; }
        if (data) priv.push_back({g, std::vector<uint8_t>((const uint8_t *)data, (const uint8_t *)data + size)});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) { return S_OK; }
};
template <class B> ULONG STDMETHODCALLTYPE Obj<B>::Release() {
    ULONG r = --refs;
    if (!r) {
        if (h && g_conn && !g_conn->dead) {
            std::lock_guard<std::recursive_mutex> l(g_conn->mu);
            g_conn->w.begin(D3DR_RELEASE); g_conn->w.u32(h); g_conn->w.end();
        }
        delete this;
    }
    return r;
}
/* handle of a client object through its d3d11 interface (every object is an Obj<Stub_<interface>>) */
#define HANDLE_OF(I) static inline uint32_t H(I *o) { return o ? static_cast<Obj<Stub_##I> *>(static_cast<Stub_##I *>(o))->h : 0; }
HANDLE_OF(ID3D11Buffer) HANDLE_OF(ID3D11ShaderResourceView) HANDLE_OF(ID3D11RenderTargetView) HANDLE_OF(ID3D11DepthStencilView)
HANDLE_OF(ID3D11UnorderedAccessView) HANDLE_OF(ID3D11VertexShader) HANDLE_OF(ID3D11PixelShader) HANDLE_OF(ID3D11GeometryShader)
HANDLE_OF(ID3D11HullShader) HANDLE_OF(ID3D11DomainShader) HANDLE_OF(ID3D11ComputeShader) HANDLE_OF(ID3D11InputLayout)
HANDLE_OF(ID3D11SamplerState) HANDLE_OF(ID3D11BlendState) HANDLE_OF(ID3D11RasterizerState) HANDLE_OF(ID3D11DepthStencilState)

#define CHILD_METHODS(base) \
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **pp) override { Obj::GetDevice(pp); } \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override { return Obj::GetPrivateData(g, s, d); } \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override { return Obj::SetPrivateData(g, s, d); } \
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown *u) override { return Obj::SetPrivateDataInterface(g, u); }

/* handle of any client object through its d3d11 interface (all derive from Obj<Stub_*>) */
struct Resource;
static Resource *as_resource(ID3D11Resource *r);
static uint32_t res_handle(ID3D11Resource *r);

/* ---------------------------------------------------------------- resources */
struct Resource {
    uint32_t handle = 0;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    std::vector<std::vector<uint8_t>> shadow;   /* per-subresource mapped memory */
    std::vector<D3D11_MAP> mapped_as;
    virtual ~Resource() {}
    virtual uint32_t subresources() = 0;
    /* tightly packed layout of a subresource: row pitch, rows, depth slices */
    virtual bool layout(UINT sub, uint32_t *pitch, uint32_t *rows, uint32_t *depth) = 0;
};

struct Buffer : Obj<Stub_ID3D11Buffer>, Resource {
    CHILD_METHODS(Stub_ID3D11Buffer)
    D3D11_BUFFER_DESC desc;
    bool is(REFIID r) override { return r == __uuidof(ID3D11Buffer) || r == __uuidof(ID3D11Resource) || r == __uuidof(ID3D11DeviceChild); }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *d) override { *d = D3D11_RESOURCE_DIMENSION_BUFFER; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC *d) override { *d = desc; }
    uint32_t subresources() override { return 1; }
    bool layout(UINT sub, uint32_t *p, uint32_t *r, uint32_t *d) override {
        if (sub) return false;
        *p = desc.ByteWidth; *r = 1; *d = 1; return true;
    }
};

struct Texture2D : Obj<Stub_ID3D11Texture2D>, Resource {
    CHILD_METHODS(Stub_ID3D11Texture2D)
    D3D11_TEXTURE2D_DESC desc;
    bool is(REFIID r) override { return r == __uuidof(ID3D11Texture2D) || r == __uuidof(ID3D11Resource) || r == __uuidof(ID3D11DeviceChild); }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *d) override { *d = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC *d) override { *d = desc; }
    uint32_t subresources() override { return desc.MipLevels * desc.ArraySize; }
    bool layout(UINT sub, uint32_t *p, uint32_t *r, uint32_t *d) override {
        if (sub >= subresources()) return false;
        *d = 1;
        return d3dr_mip_layout(desc.Format, desc.Width, desc.Height, sub % desc.MipLevels, p, r);
    }
};

static Resource *as_resource(ID3D11Resource *r) {
    if (!r) return nullptr;
    D3D11_RESOURCE_DIMENSION d; r->GetType(&d);
    if (d == D3D11_RESOURCE_DIMENSION_BUFFER) return static_cast<Buffer *>(static_cast<Stub_ID3D11Buffer *>(static_cast<ID3D11Buffer *>(r)));
    if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) return static_cast<Texture2D *>(static_cast<Stub_ID3D11Texture2D *>(static_cast<ID3D11Texture2D *>(r)));
    return nullptr;
}
static uint32_t res_handle(ID3D11Resource *r) { Resource *x = as_resource(r); return x ? x->handle : 0; }

/* views, shaders, states, layouts: a handle and (for views/states) the desc */
#define SIMPLE_CHILD(Name, Iface, ...) \
    struct Name : Obj<Stub_##Iface> { \
        CHILD_METHODS(Stub_##Iface) \
        bool is(REFIID r) override { return r == __uuidof(Iface) || r == __uuidof(ID3D11DeviceChild) __VA_ARGS__; } \
    };
#define VIEW(Name, Iface, Desc) \
    struct Name : Obj<Stub_##Iface> { \
        CHILD_METHODS(Stub_##Iface) \
        Desc desc; ID3D11Resource *res = nullptr; \
        ~Name() { if (res) res->Release(); } \
        bool is(REFIID r) override { return r == __uuidof(Iface) || r == __uuidof(ID3D11View) || r == __uuidof(ID3D11DeviceChild); } \
        void STDMETHODCALLTYPE GetResource(ID3D11Resource **pp) override { *pp = res; if (res) res->AddRef(); } \
        void STDMETHODCALLTYPE GetDesc(Desc *d) override { *d = desc; } \
    };
VIEW(SRV, ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC)
VIEW(RTV, ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC)
VIEW(DSV, ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC)
VIEW(UAV, ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC)
SIMPLE_CHILD(VS, ID3D11VertexShader)
SIMPLE_CHILD(PS, ID3D11PixelShader)
SIMPLE_CHILD(GS, ID3D11GeometryShader)
SIMPLE_CHILD(HS, ID3D11HullShader)
SIMPLE_CHILD(DS, ID3D11DomainShader)
SIMPLE_CHILD(CS, ID3D11ComputeShader)
SIMPLE_CHILD(InputLayout, ID3D11InputLayout)
#define STATE(Name, Iface, Desc) \
    struct Name : Obj<Stub_##Iface> { \
        CHILD_METHODS(Stub_##Iface) \
        Desc desc; \
        bool is(REFIID r) override { return r == __uuidof(Iface) || r == __uuidof(ID3D11DeviceChild); } \
        void STDMETHODCALLTYPE GetDesc(Desc *d) override { *d = desc; } \
    };
STATE(Sampler, ID3D11SamplerState, D3D11_SAMPLER_DESC)
STATE(Blend, ID3D11BlendState, D3D11_BLEND_DESC)
STATE(Raster, ID3D11RasterizerState, D3D11_RASTERIZER_DESC)
STATE(DepthStencil, ID3D11DepthStencilState, D3D11_DEPTH_STENCIL_DESC)

/* ---------------------------------------------------------------- immediate context */
struct Context : Obj<Stub_ID3D11DeviceContext> {
    CHILD_METHODS(Stub_ID3D11DeviceContext)
    bool is(REFIID r) override { return r == __uuidof(ID3D11DeviceContext) || r == __uuidof(ID3D11DeviceChild); }
    d3dr_writer &W() { return g_conn->w; }
    struct Lock { std::lock_guard<std::recursive_mutex> l; Lock() : l(g_conn->mu) {} ~Lock() { g_conn->maybe_flush(); } };

    template <class T> void set_handles(uint32_t op, uint32_t stage, UINT start, UINT n, T *const *objs) {
        Lock l; W().begin(op); W().u32(stage); W().u32(start); W().u32(n);
        for (UINT i = 0; i < n; i++) W().u32(objs ? H(objs[i]) : 0);
        W().end();
    }
    void set_shader(uint32_t stage, uint32_t h) { Lock l; W().begin(D3DR_SET_SHADER); W().u32(stage); W().u32(h); W().end(); }

#define STAGE_METHODS(P, STAGE, ShaderIface) \
    void STDMETHODCALLTYPE P##SetConstantBuffers(UINT s, UINT n, ID3D11Buffer *const *b) override { set_handles(D3DR_SET_CB, STAGE, s, n, b); } \
    void STDMETHODCALLTYPE P##SetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView *const *v) override { set_handles(D3DR_SET_SRV, STAGE, s, n, v); } \
    void STDMETHODCALLTYPE P##SetSamplers(UINT s, UINT n, ID3D11SamplerState *const *v) override { set_handles(D3DR_SET_SAMPLERS, STAGE, s, n, v); } \
    void STDMETHODCALLTYPE P##SetShader(ShaderIface *sh, ID3D11ClassInstance *const *, UINT) override { set_shader(STAGE, H(sh)); }
    STAGE_METHODS(VS, D3DR_STAGE_VS, ID3D11VertexShader)
    STAGE_METHODS(PS, D3DR_STAGE_PS, ID3D11PixelShader)
    STAGE_METHODS(GS, D3DR_STAGE_GS, ID3D11GeometryShader)
    STAGE_METHODS(HS, D3DR_STAGE_HS, ID3D11HullShader)
    STAGE_METHODS(DS, D3DR_STAGE_DS, ID3D11DomainShader)
    STAGE_METHODS(CS, D3DR_STAGE_CS, ID3D11ComputeShader)

    void STDMETHODCALLTYPE Draw(UINT vc, UINT sv) override { Lock l; W().begin(D3DR_DRAW); W().u32(vc); W().u32(sv); W().end(); }
    void STDMETHODCALLTYPE DrawIndexed(UINT ic, UINT si, INT bv) override {
        Lock l; W().begin(D3DR_DRAW_INDEXED); W().u32(ic); W().u32(si); W().u32((uint32_t)bv); W().end();
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT vc, UINT ic, UINT sv, UINT si) override {
        Lock l; W().begin(D3DR_DRAW_INSTANCED); W().u32(vc); W().u32(ic); W().u32(sv); W().u32(si); W().end();
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT ic, UINT n, UINT si, INT bv, UINT sinst) override {
        Lock l; W().begin(D3DR_DRAW_INDEXED_INSTANCED); W().u32(ic); W().u32(n); W().u32(si); W().u32((uint32_t)bv); W().u32(sinst); W().end();
    }
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout *il) override { Lock l; W().begin(D3DR_SET_LAYOUT); W().u32(H(il)); W().end(); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t) override { Lock l; W().begin(D3DR_SET_TOPOLOGY); W().u32(t); W().end(); }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT s, UINT n, ID3D11Buffer *const *b, const UINT *st, const UINT *off) override {
        Lock l; W().begin(D3DR_SET_VB); W().u32(s); W().u32(n);
        for (UINT i = 0; i < n; i++) { W().u32(b ? H(b[i]) : 0); W().u32(st ? st[i] : 0); W().u32(off ? off[i] : 0); }
        W().end();
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer *b, DXGI_FORMAT f, UINT off) override {
        Lock l; W().begin(D3DR_SET_IB); W().u32(H(b)); W().u32(f); W().u32(off); W().end();
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D11_VIEWPORT *v) override {
        Lock l; W().begin(D3DR_SET_VIEWPORTS); W().u32(n); W().put(v, n * sizeof *v); W().end();
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D11_RECT *r) override {
        Lock l; W().begin(D3DR_SET_SCISSORS); W().u32(n); W().put(r, n * sizeof *r); W().end();
    }
    void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState *s) override { Lock l; W().begin(D3DR_SET_RS_STATE); W().u32(H(s)); W().end(); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *ds) override {
        Lock l; W().begin(D3DR_SET_RTV); W().u32(n);
        for (UINT i = 0; i < n; i++) W().u32(rt ? H(rt[i]) : 0);
        W().u32(H(ds)); W().end();
    }
    void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState *b, const FLOAT f[4], UINT mask) override {
        static const FLOAT one[4] = {1, 1, 1, 1};
        Lock l; W().begin(D3DR_SET_BLEND); W().u32(H(b)); W().put(f ? f : one, 16); W().u32(mask); W().end();
    }
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState *s, UINT ref) override {
        Lock l; W().begin(D3DR_SET_DS_STATE); W().u32(H(s)); W().u32(ref); W().end();
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView *v, const FLOAT c[4]) override {
        Lock l; W().begin(D3DR_CLEAR_RTV); W().u32(H(v)); W().put(c, 16); W().end();
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView *v, UINT flags, FLOAT d, UINT8 s) override {
        Lock l; W().begin(D3DR_CLEAR_DSV); W().u32(H(v)); W().u32(flags); W().f32(d); W().u32(s); W().end();
    }
    void STDMETHODCALLTYPE CopyResource(ID3D11Resource *dst, ID3D11Resource *src) override {
        Lock l; W().begin(D3DR_COPY_RESOURCE); W().u32(res_handle(dst)); W().u32(res_handle(src)); W().end();
    }
    void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource *dst, UINT sub, const D3D11_BOX *box, const void *data, UINT rp, UINT dp) override {
        Resource *r = as_resource(dst);
        if (!r || !data) return;
        uint32_t pitch, rows, depth, size;
        if (box) {
            if (r->dim == D3D11_RESOURCE_DIMENSION_BUFFER) { rows = 1; depth = 1; pitch = box->right - box->left; }
            else {
                uint32_t bd; d3dr_format_block(static_cast<Texture2D *>(r)->desc.Format, &bd);
                rows = (box->bottom - box->top + bd - 1) / bd; depth = box->back - box->front; pitch = rp;
            }
        } else if (!r->layout(sub, &pitch, &rows, &depth)) return;
        else if (r->dim != D3D11_RESOURCE_DIMENSION_BUFFER) pitch = rp;
        size = depth > 1 ? dp * (depth - 1) + pitch * rows : pitch * rows;
        if (r->dim == D3D11_RESOURCE_DIMENSION_BUFFER) size = pitch;
        Lock l; W().begin(D3DR_UPDATE_SUBRESOURCE); W().u32(r->handle); W().u32(sub); W().u32(box != nullptr);
        D3D11_BOX b = box ? *box : D3D11_BOX{}; W().pod(b); W().u32(rp); W().u32(dp); W().blob(data, size); W().end();
    }
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource *res, UINT sub, D3D11_MAP type, UINT, D3D11_MAPPED_SUBRESOURCE *out) override {
        Resource *r = as_resource(res);
        uint32_t pitch, rows, depth;
        if (!r || !out || !r->layout(sub, &pitch, &rows, &depth)) return E_INVALIDARG;
        Lock l;
        if (r->shadow.size() < r->subresources()) { r->shadow.resize(r->subresources()); r->mapped_as.resize(r->subresources()); }
        auto &s = r->shadow[sub];
        s.resize((size_t)pitch * rows * depth);
        r->mapped_as[sub] = type;
        if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE) {
            W().begin(D3DR_READ_MAPPED); W().u32(r->handle); W().u32(sub); W().u32(pitch); W().u32(rows); W().u32(depth); W().end();
            std::vector<uint8_t> rep;
            if (!g_conn->wait_reply(D3DR_R_READ, rep) || rep.size() < 4) return DXGI_ERROR_DEVICE_REMOVED;
            int32_t hr; memcpy(&hr, rep.data(), 4);
            if (FAILED(hr)) return hr;
            memcpy(s.data(), rep.data() + 4, std::min(s.size(), rep.size() - 4));
        }
        out->pData = s.data(); out->RowPitch = pitch; out->DepthPitch = pitch * rows;
        return S_OK;
    }
    void STDMETHODCALLTYPE Unmap(ID3D11Resource *res, UINT sub) override {
        Resource *r = as_resource(res);
        uint32_t pitch, rows, depth;
        if (!r || sub >= r->shadow.size() || !r->layout(sub, &pitch, &rows, &depth)) return;
        D3D11_MAP type = r->mapped_as[sub];
        if (type == D3D11_MAP_READ) return;
        Lock l; W().begin(D3DR_WRITE_MAPPED); W().u32(r->handle); W().u32(sub); W().u32(type); W().u32(pitch); W().u32(rows); W().u32(depth);
        W().put(r->shadow[sub].data(), r->shadow[sub].size()); W().end();
    }
    void STDMETHODCALLTYPE Flush() override { Lock l; g_conn->flush(); }
    void STDMETHODCALLTYPE ClearState() override { d3dr_unimpl("ID3D11DeviceContext::ClearState"); }
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override { return D3D11_DEVICE_CONTEXT_IMMEDIATE; }
    UINT STDMETHODCALLTYPE GetContextFlags() override { return 0; }
};

/* ---------------------------------------------------------------- device */
class Device : public Obj<Stub_ID3D11Device> {
public:
    Context *ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
    bool is(REFIID r) override { return r == __uuidof(ID3D11Device); }
    d3dr_writer &W() { return g_conn->w; }
    struct Lock { std::lock_guard<std::recursive_mutex> l; Lock() : l(g_conn->mu) {} ~Lock() { g_conn->maybe_flush(); } };
    template <class T> T *make() { T *o = new T; o->h = new_handle(); o->dev = this; return o; }

    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC *d, const D3D11_SUBRESOURCE_DATA *init, ID3D11Buffer **pp) override {
        if (!d) return E_INVALIDARG;
        if (!pp) return S_FALSE;
        Buffer *b = make<Buffer>(); b->desc = *d; b->handle = b->h; b->dim = D3D11_RESOURCE_DIMENSION_BUFFER;
        Lock l; W().begin(D3DR_CREATE_BUFFER); W().u32(b->h); W().pod(*d); W().u32(init && init->pSysMem);
        if (init && init->pSysMem) W().put(init->pSysMem, d->ByteWidth);
        W().end();
        *pp = b;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **pp) override {
        if (!d) return E_INVALIDARG;
        if (!pp) return S_FALSE;
        Texture2D *t = make<Texture2D>(); t->desc = *d; t->handle = t->h; t->dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
        if (!t->desc.MipLevels) { UINT m = 1, x = std::max(d->Width, d->Height); while (x > 1) { x >>= 1; m++; } t->desc.MipLevels = m; }
        Lock l; W().begin(D3DR_CREATE_TEXTURE2D); W().u32(t->h); W().pod(*d); W().u32(init != nullptr);
        if (init) {
            for (UINT i = 0; i < t->subresources(); i++) {
                uint32_t pitch, rows, depth;
                if (!t->layout(i, &pitch, &rows, &depth)) { ERR("CreateTexture2D: format %u has no layout", d->Format); pitch = rows = 0; }
                W().u32(init[i].SysMemPitch);
                uint32_t size = rows ? init[i].SysMemPitch * (rows - 1) + pitch : 0;
                W().blob(init[i].pSysMem, size);
            }
        }
        W().end();
        *pp = t;
        return S_OK;
    }
    template <class V, class D> HRESULT view(uint32_t kind, ID3D11Resource *res, const D *d, V **pp) {
        if (!res) return E_INVALIDARG;
        if (!pp) return S_FALSE;
        V *v = make<V>(); v->res = res; res->AddRef();
        if (d) v->desc = *d; else memset(&v->desc, 0, sizeof v->desc);
        Lock l; W().begin(D3DR_CREATE_VIEW); W().u32(kind); W().u32(v->h); W().u32(res_handle(res)); W().u32(d != nullptr);
        if (d) W().pod(*d);
        W().end();
        *pp = v;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource *r, const D3D11_SHADER_RESOURCE_VIEW_DESC *d, ID3D11ShaderResourceView **pp) override {
        SRV *v = nullptr; HRESULT hr = view(D3DR_VIEW_SRV, r, d, &v); if (pp) *pp = v; return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource *r, const D3D11_RENDER_TARGET_VIEW_DESC *d, ID3D11RenderTargetView **pp) override {
        RTV *v = nullptr; HRESULT hr = view(D3DR_VIEW_RTV, r, d, &v); if (pp) *pp = v; return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource *r, const D3D11_DEPTH_STENCIL_VIEW_DESC *d, ID3D11DepthStencilView **pp) override {
        DSV *v = nullptr; HRESULT hr = view(D3DR_VIEW_DSV, r, d, &v); if (pp) *pp = v; return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource *r, const D3D11_UNORDERED_ACCESS_VIEW_DESC *d, ID3D11UnorderedAccessView **pp) override {
        UAV *v = nullptr; HRESULT hr = view(D3DR_VIEW_UAV, r, d, &v); if (pp) *pp = v; return hr;
    }
    template <class S, class I> HRESULT shader(uint32_t stage, const void *code, SIZE_T size, I **pp) {
        if (!code) return E_INVALIDARG;
        if (!pp) return S_FALSE;
        S *s = make<S>();
        Lock l; W().begin(D3DR_CREATE_SHADER); W().u32(stage); W().u32(s->h); W().blob(code, (uint32_t)size); W().end();
        *pp = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void *c, SIZE_T n, ID3D11ClassLinkage *, ID3D11VertexShader **pp) override { return shader<VS>(D3DR_STAGE_VS, c, n, pp); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void *c, SIZE_T n, ID3D11ClassLinkage *, ID3D11PixelShader **pp) override { return shader<PS>(D3DR_STAGE_PS, c, n, pp); }
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void *c, SIZE_T n, ID3D11ClassLinkage *, ID3D11GeometryShader **pp) override { return shader<GS>(D3DR_STAGE_GS, c, n, pp); }
    HRESULT STDMETHODCALLTYPE CreateHullShader(const void *c, SIZE_T n, ID3D11ClassLinkage *, ID3D11HullShader **pp) override { return shader<HS>(D3DR_STAGE_HS, c, n, pp); }
    HRESULT STDMETHODCALLTYPE CreateDomainShader(const void *c, SIZE_T n, ID3D11ClassLinkage *, ID3D11DomainShader **pp) override { return shader<DS>(D3DR_STAGE_DS, c, n, pp); }
    HRESULT STDMETHODCALLTYPE CreateComputeShader(const void *c, SIZE_T n, ID3D11ClassLinkage *, ID3D11ComputeShader **pp) override { return shader<CS>(D3DR_STAGE_CS, c, n, pp); }
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *e, UINT n, const void *code, SIZE_T size, ID3D11InputLayout **pp) override {
        if (!pp) return S_FALSE;
        InputLayout *il = make<InputLayout>();
        Lock l; W().begin(D3DR_CREATE_INPUT_LAYOUT); W().u32(il->h); W().u32(n);
        for (UINT i = 0; i < n; i++) {
            D3D11_INPUT_ELEMENT_DESC x = e[i]; x.SemanticName = nullptr; W().pod(x);
            W().blob(e[i].SemanticName, (uint32_t)strlen(e[i].SemanticName));
        }
        W().blob(code, (uint32_t)size); W().end();
        *pp = il;
        return S_OK;
    }
    template <class S, class D, class I> HRESULT state(uint32_t kind, const D *d, I **pp) {
        if (!d) return E_INVALIDARG;
        if (!pp) return S_FALSE;
        S *s = make<S>(); s->desc = *d;
        Lock l; W().begin(D3DR_CREATE_STATE); W().u32(kind); W().u32(s->h); W().pod(*d); W().end();
        *pp = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC *d, ID3D11SamplerState **pp) override { return state<Sampler>(D3DR_STATE_SAMPLER, d, pp); }
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC *d, ID3D11BlendState **pp) override { return state<Blend>(D3DR_STATE_BLEND, d, pp); }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC *d, ID3D11RasterizerState **pp) override { return state<Raster>(D3DR_STATE_RASTERIZER, d, pp); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *d, ID3D11DepthStencilState **pp) override { return state<DepthStencil>(D3DR_STATE_DEPTH_STENCIL, d, pp); }
    void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext **pp) override { ctx->AddRef(); *pp = ctx; }
    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return fl; }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return flags; }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return g_conn->dead ? DXGI_ERROR_DEVICE_REMOVED : S_OK; }
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT) override { return S_OK; }
    UINT STDMETHODCALLTYPE GetExceptionMode() override { return 0; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override { return Obj::GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override { return Obj::SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown *u) override { return Obj::SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT, UINT count, UINT *q) override { *q = count == 1 ? 1 : 0; return S_OK; }
};
template <class B> void STDMETHODCALLTYPE Obj<B>::GetDevice(ID3D11Device **pp) { dev->AddRef(); *pp = dev; }

/* ---------------------------------------------------------------- swapchain */
struct SwapChain : Obj<Stub_IDXGISwapChain> {
    DXGI_SWAP_CHAIN_DESC desc;
    Texture2D *back = nullptr;
    ~SwapChain() { if (back) back->Release(); }
    bool is(REFIID r) override { return r == __uuidof(IDXGISwapChain) || r == __uuidof(IDXGIDeviceSubObject) || r == __uuidof(IDXGIObject); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override { return Obj::SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override { return Obj::GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void **pp) override { return dev->QueryInterface(r, pp); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT idx, REFIID r, void **pp) override {
        if (idx) { d3dr_unimpl("IDXGISwapChain::GetBuffer(index > 0)"); return E_NOTIMPL; }
        if (!back) {
            back = dev->make<Texture2D>(); back->handle = back->h; back->dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
            D3D11_TEXTURE2D_DESC &t = back->desc;
            t = {}; t.Width = desc.BufferDesc.Width; t.Height = desc.BufferDesc.Height; t.MipLevels = 1; t.ArraySize = 1;
            t.Format = desc.BufferDesc.Format; t.SampleDesc = desc.SampleDesc; t.Usage = D3D11_USAGE_DEFAULT;
            t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            std::lock_guard<std::recursive_mutex> l(g_conn->mu);
            g_conn->w.begin(D3DR_SC_GET_BUFFER); g_conn->w.u32(h); g_conn->w.u32(0); g_conn->w.u32(back->h); g_conn->w.end();
        }
        return back->QueryInterface(r, pp);
    }
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override {
        std::lock_guard<std::recursive_mutex> l(g_conn->mu);
        if (g_conn->dead) return DXGI_ERROR_DEVICE_REMOVED;
        uint64_t serial = ++g_conn->presents_sent;
        g_conn->w.begin(D3DR_SC_PRESENT); g_conn->w.u32(h); g_conn->w.u32(sync); g_conn->w.u32(flags); g_conn->w.u64(serial); g_conn->w.end();
        g_conn->flush();
        /* at most two frames in flight */
        std::vector<uint8_t> rep;
        while (g_conn->presents_done + 2 < serial && !g_conn->dead) g_conn->wait_reply(D3DR_R_PRESENT, rep);
        return g_conn->dead ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *d) override { *d = desc; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fs, IDXGIOutput **out) override { if (fs) *fs = FALSE; if (out) *out = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput *) override { return S_OK; }
};

/* ---------------------------------------------------------------- entry points */
static HRESULT create(D3D_DRIVER_TYPE, UINT flags, const D3D_FEATURE_LEVEL *fls, UINT nfl, const DXGI_SWAP_CHAIN_DESC *scd,
                      IDXGISwapChain **sc_out, ID3D11Device **dev_out, D3D_FEATURE_LEVEL *fl_out, ID3D11DeviceContext **ctx_out) {
    static std::mutex init_mu;
    {
        std::lock_guard<std::mutex> l(init_mu);
        if (!g_conn) {
            char e[8];
            g_log = GetEnvironmentVariableA("D3DR_LOG", e, sizeof e) && e[0] == '1';
            Conn *c = new Conn;
            if (!c->connect_server()) { delete c; return E_FAIL; }
            g_conn = c;
        }
    }
    if (g_conn->dead) return DXGI_ERROR_DEVICE_REMOVED;
    Device *dev = new Device; dev->h = new_handle(); dev->dev = dev; dev->flags = flags;
    Context *ctx = new Context; ctx->h = new_handle(); ctx->dev = dev; dev->ctx = ctx;
    SwapChain *sc = nullptr;
    DXGI_SWAP_CHAIN_DESC d = {};
    if (scd && sc_out) {
        d = *scd;
        if (!d.BufferDesc.Width || !d.BufferDesc.Height) {
            RECT r = {0, 0, 1280, 720};
            if (d.OutputWindow) GetClientRect(d.OutputWindow, &r);
            if (!d.BufferDesc.Width) d.BufferDesc.Width = std::max<LONG>(1, r.right - r.left);
            if (!d.BufferDesc.Height) d.BufferDesc.Height = std::max<LONG>(1, r.bottom - r.top);
        }
        sc = new SwapChain; sc->h = new_handle(); sc->dev = dev; sc->desc = d;
    }
    std::vector<uint8_t> rep;
    {
        std::lock_guard<std::recursive_mutex> l(g_conn->mu);
        d3dr_writer &w = g_conn->w;
        w.begin(D3DR_CREATE_DEVICE); w.u32(dev->h); w.u32(ctx->h); w.u32(sc ? sc->h : 0);
        w.u32(fls ? nfl : 0);
        for (UINT i = 0; fls && i < nfl; i++) w.u32(fls[i]);
        w.u32(flags & ~(UINT)D3D11_CREATE_DEVICE_DEBUG);
        DXGI_SWAP_CHAIN_DESC wire = d; wire.OutputWindow = nullptr; w.pod(wire);
        w.end();
        if (!g_conn->wait_reply(D3DR_R_DEVICE, rep) || rep.size() < 8) return DXGI_ERROR_DEVICE_REMOVED;
    }
    int32_t hr; uint32_t fl; memcpy(&hr, rep.data(), 4); memcpy(&fl, rep.data() + 4, 4);
    LOG("device %u: hr 0x%08x feature level 0x%x", dev->h, (unsigned)hr, fl);
    if (FAILED(hr)) { ctx->Release(); if (sc) sc->Release(); dev->ctx = nullptr; dev->Release(); return hr; }
    dev->fl = (D3D_FEATURE_LEVEL)fl;
    if (fl_out) *fl_out = dev->fl;
    if (ctx_out) { ctx->AddRef(); *ctx_out = ctx; }
    if (sc_out) *sc_out = sc;
    if (dev_out) *dev_out = dev; else dev->Release();
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *, D3D_DRIVER_TYPE type, HMODULE, UINT flags,
        const D3D_FEATURE_LEVEL *fls, UINT nfl, UINT, ID3D11Device **dev, D3D_FEATURE_LEVEL *fl, ID3D11DeviceContext **ctx) {
    return create(type, flags, fls, nfl, nullptr, nullptr, dev, fl, ctx);
}

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *, D3D_DRIVER_TYPE type, HMODULE,
        UINT flags, const D3D_FEATURE_LEVEL *fls, UINT nfl, UINT, const DXGI_SWAP_CHAIN_DESC *scd, IDXGISwapChain **sc,
        ID3D11Device **dev, D3D_FEATURE_LEVEL *fl, ID3D11DeviceContext **ctx) {
    return create(type, flags, fls, nfl, scd, sc, dev, fl, ctx);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_conn) { std::lock_guard<std::recursive_mutex> l(g_conn->mu); g_conn->flush(); }
    return TRUE;
}
