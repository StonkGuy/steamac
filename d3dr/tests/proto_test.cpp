// d3dr protocol unit test (native): message framing, bounds-checked decoding, subresource layouts.
#include <cstdio>
#include "d3dr_format.h"
#include "d3dr_proto.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main() {
    // framing: two messages back to back, lengths patched by end()
    d3dr_writer w;
    w.begin(D3DR_DRAW); w.u32(3); w.u32(0); w.end();
    uint8_t blob[5] = {1, 2, 3, 4, 5};
    w.begin(D3DR_CREATE_SHADER); w.u32(D3DR_STAGE_PS); w.u32(7); w.blob(blob, 5); w.end();
    CHECK(w.buf.size() == 8 + 8 + 8 + 12 + 5);
    d3dr_msg m; memcpy(&m, w.buf.data(), 8);
    CHECK(m.op == D3DR_DRAW && m.len == 8);
    d3dr_reader r(w.buf.data() + 8, m.len);
    CHECK(r.u32() == 3 && r.u32() == 0 && !r.bad && r.left() == 0);
    memcpy(&m, w.buf.data() + 16, 8);
    CHECK(m.op == D3DR_CREATE_SHADER && m.len == 17);
    d3dr_reader r2(w.buf.data() + 24, m.len);
    uint32_t n;
    CHECK(r2.u32() == D3DR_STAGE_PS && r2.u32() == 7);
    const uint8_t *p = r2.blob(&n);
    CHECK(p && n == 5 && p[4] == 5 && !r2.bad);

    // a short payload never reads past the end: zeros + bad
    d3dr_reader r3(w.buf.data() + 24, 10);
    r3.u32(); r3.u32();
    p = r3.blob(&n);
    CHECK(!p && n == 0 && r3.bad);
    CHECK(r3.u32() == 0 && r3.bad);
    // a hostile length is rejected
    uint8_t evil[8] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
    d3dr_reader r4(evil, sizeof evil);
    CHECK(!r4.blob(&n) && r4.bad);

    // layouts (numeric DXGI_FORMAT values)
    uint32_t pitch, rows, bd;
    CHECK(d3dr_mip_layout(28 /* R8G8B8A8_UNORM */, 256, 256, 0, &pitch, &rows) && pitch == 1024 && rows == 256);
    CHECK(d3dr_mip_layout(28, 256, 256, 3, &pitch, &rows) && pitch == 128 && rows == 32);
    CHECK(d3dr_mip_layout(28, 256, 128, 8, &pitch, &rows) && pitch == 4 && rows == 1);
    CHECK(d3dr_mip_layout(71 /* BC1_UNORM */, 256, 256, 0, &pitch, &rows) && pitch == 512 && rows == 64);
    CHECK(d3dr_mip_layout(98 /* BC7_UNORM */, 6, 6, 0, &pitch, &rows) && pitch == 32 && rows == 2);
    CHECK(d3dr_mip_layout(77 /* BC3_UNORM */, 1024, 1024, 10, &pitch, &rows) && pitch == 16 && rows == 1);
    CHECK(d3dr_format_block(2 /* R32G32B32A32_FLOAT */, &bd) == 16 && bd == 1);
    CHECK(d3dr_format_block(41 /* R32_FLOAT */, &bd) == 4);
    CHECK(d3dr_format_block(87 /* B8G8R8A8_UNORM */, &bd) == 4);
    CHECK(d3dr_format_block(56 /* R16_UNORM */, &bd) == 2);
    CHECK(d3dr_format_block(61 /* R8_UNORM */, &bd) == 1);
    CHECK(!d3dr_mip_layout(0 /* UNKNOWN */, 4, 4, 0, &pitch, &rows));

    printf(failures ? "proto_test: %d FAILED\n" : "proto_test: all passed\n", failures);
    return failures != 0;
}
