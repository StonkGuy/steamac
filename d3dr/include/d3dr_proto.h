/* d3dr: Direct3D 11 remoting wire protocol (client = guest d3d11.dll, server = d3dr-server.exe on the Mac).
 *
 * Both ends are x86-64 Windows code (the client runs in Proton under FEX, the server in a macOS Wine), so plain
 * D3D11 descriptor structs travel as their raw bytes; only pointers (initial data, semantic names, bytecode) are
 * flattened. A message is a d3dr_msg header followed by `len` payload bytes. Objects are named by handles the
 * client assigns (non-zero u32), so creation never waits for the server; only calls that return data to the
 * application (device creation, Map for reading, Present pacing) get a reply.
 */
#ifndef D3DR_PROTO_H
#define D3DR_PROTO_H
#include <stdint.h>
#include <string.h>

#define D3DR_VERSION 1
#define D3DR_DEFAULT_PORT 47811
#define D3DR_MAX_MSG (256u << 20)

struct d3dr_msg { uint32_t op, len; };

enum d3dr_op {
    D3DR_HELLO = 1,             /* u32 version                                  -> R_HELLO u32 version */
    D3DR_CREATE_DEVICE,         /* u32 dev, ctx, sc; u32 fl_count; u32 fl[]; u32 flags;
                                   DXGI_SWAP_CHAIN_DESC (OutputWindow ignored)  -> R_DEVICE i32 hr, u32 fl */
    D3DR_SC_GET_BUFFER,         /* u32 sc, index, tex */
    D3DR_SC_PRESENT,            /* u32 sc, sync, flags; u64 serial              -> R_PRESENT u64 serial */
    D3DR_RELEASE,               /* u32 handle */
    D3DR_CREATE_BUFFER,         /* u32 h; D3D11_BUFFER_DESC; u32 has_init; [bytes ByteWidth] */
    D3DR_CREATE_TEXTURE2D,      /* u32 h; D3D11_TEXTURE2D_DESC; u32 has_init; per subresource: u32 pitch, u32 size, bytes */
    D3DR_CREATE_VIEW,           /* u32 kind(D3DR_VIEW_*), h, resource, has_desc; [desc bytes] */
    D3DR_CREATE_SHADER,         /* u32 stage(D3DR_STAGE_*), h, size; bytes */
    D3DR_CREATE_INPUT_LAYOUT,   /* u32 h, n; n x {D3D11_INPUT_ELEMENT_DESC (name ptr zeroed), u32 namelen, name};
                                   u32 size; bytecode */
    D3DR_CREATE_STATE,          /* u32 kind(D3DR_STATE_*), h; desc bytes */
    D3DR_CLEAR_RTV,             /* u32 rtv; float[4] */
    D3DR_CLEAR_DSV,             /* u32 dsv, flags; float depth; u32 stencil */
    D3DR_SET_VIEWPORTS,         /* u32 n; D3D11_VIEWPORT[n] */
    D3DR_SET_SCISSORS,          /* u32 n; D3D11_RECT[n] */
    D3DR_SET_RTV,               /* u32 n; u32 rtv[n]; u32 dsv */
    D3DR_SET_BLEND,             /* u32 h; float[4]; u32 mask */
    D3DR_SET_DS_STATE,          /* u32 h, ref */
    D3DR_SET_RS_STATE,          /* u32 h */
    D3DR_SET_VB,                /* u32 start, n; n x {u32 h, stride, offset} */
    D3DR_SET_IB,                /* u32 h, format, offset */
    D3DR_SET_LAYOUT,            /* u32 h */
    D3DR_SET_TOPOLOGY,          /* u32 topology */
    D3DR_SET_SHADER,            /* u32 stage, h */
    D3DR_SET_CB,                /* u32 stage, start, n; u32 h[n] */
    D3DR_SET_SRV,               /* u32 stage, start, n; u32 h[n] */
    D3DR_SET_SAMPLERS,          /* u32 stage, start, n; u32 h[n] */
    D3DR_DRAW,                  /* u32 vertex_count, start_vertex */
    D3DR_DRAW_INDEXED,          /* u32 index_count, start_index; i32 base_vertex */
    D3DR_DRAW_INSTANCED,        /* u32 vcount, icount, svert, sinst */
    D3DR_DRAW_INDEXED_INSTANCED,/* u32 icount, instcount, sindex; i32 base; u32 sinst */
    D3DR_WRITE_MAPPED,          /* u32 res, sub, map_type, row_pitch, rows, depth; bytes (row_pitch*rows*depth) */
    D3DR_UPDATE_SUBRESOURCE,    /* u32 res, sub, has_box; D3D11_BOX; u32 row_pitch, depth_pitch, size; bytes */
    D3DR_COPY_RESOURCE,         /* u32 dst, src */
    D3DR_READ_MAPPED,           /* u32 res, sub, row_pitch, rows, depth     -> R_READ i32 hr; bytes */
    D3DR_OP_COUNT,

    D3DR_R_HELLO = 0x1000,
    D3DR_R_DEVICE,
    D3DR_R_PRESENT,
    D3DR_R_READ,
};

enum { D3DR_VIEW_SRV, D3DR_VIEW_RTV, D3DR_VIEW_DSV, D3DR_VIEW_UAV };
enum { D3DR_STATE_SAMPLER, D3DR_STATE_BLEND, D3DR_STATE_RASTERIZER, D3DR_STATE_DEPTH_STENCIL };
enum { D3DR_STAGE_VS, D3DR_STAGE_PS, D3DR_STAGE_GS, D3DR_STAGE_HS, D3DR_STAGE_DS, D3DR_STAGE_CS, D3DR_STAGE_COUNT };

#ifdef __cplusplus
#include <vector>
/* Append-only message writer. */
struct d3dr_writer {
    std::vector<uint8_t> buf;
    size_t start = 0;
    void begin(uint32_t op) { start = buf.size(); d3dr_msg m = {op, 0}; put(&m, sizeof m); }
    void end() { uint32_t len = (uint32_t)(buf.size() - start - sizeof(d3dr_msg)); memcpy(&buf[start + 4], &len, 4); }
    void put(const void *p, size_t n) { if (n) buf.insert(buf.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
    void u32(uint32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void f32(float v) { put(&v, 4); }
    template <class T> void pod(const T &v) { put(&v, sizeof v); }
    void blob(const void *p, uint32_t n) { u32(n); put(p, n); }
};

/* Bounds-checked payload reader: any short read sets `bad` and yields zeros. */
struct d3dr_reader {
    const uint8_t *p, *e;
    bool bad = false;
    d3dr_reader(const void *d, size_t n) : p((const uint8_t *)d), e((const uint8_t *)d + n) {}
    const uint8_t *take(size_t n) {
        if (bad || (size_t)(e - p) < n) { bad = true; return nullptr; }
        const uint8_t *r = p; p += n; return r;
    }
    void get(void *d, size_t n) { const uint8_t *s = take(n); if (s) memcpy(d, s, n); else memset(d, 0, n); }
    uint32_t u32() { uint32_t v; get(&v, 4); return v; }
    uint64_t u64() { uint64_t v; get(&v, 8); return v; }
    float f32() { float v; get(&v, 4); return v; }
    template <class T> T pod() { T v; get(&v, sizeof v); return v; }
    /* u32 length + bytes; returns nullptr (and len 0) when short */
    const uint8_t *blob(uint32_t *len) { *len = u32(); const uint8_t *r = take(*len); if (!r) *len = 0; return r; }
    size_t left() const { return (size_t)(e - p); }
};
#endif
#endif
