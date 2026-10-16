/* d3dr: DXGI format sizes for flattening subresources (numeric DXGI_FORMAT values, so the header also builds
 * natively for the unit tests). */
#ifndef D3DR_FORMAT_H
#define D3DR_FORMAT_H
#include <stdint.h>

/* Block width/height in texels (1 for uncompressed formats) and bytes per block; 0 bytes = unknown format. */
static inline uint32_t d3dr_format_block(uint32_t f, uint32_t *block_dim) {
    *block_dim = 1;
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) { *block_dim = 4; return 8; }    /* BC1, BC4 */
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) {        /* BC2/3, BC5, BC6H/7 */
        *block_dim = 4; return 16;
    }
    if (f >= 1 && f <= 4) return 16;                 /* R32G32B32A32 */
    if (f >= 5 && f <= 8) return 12;                 /* R32G32B32 */
    if (f >= 9 && f <= 22) return 8;                 /* R16G16B16A16, R32G32, R32G8X24 */
    if (f >= 23 && f <= 47) return 4;                /* R10G10B10A2 .. R24G8/D24S8 family, R8G8B8A8, R16G16, R32 */
    if (f >= 48 && f <= 59) return 2;                /* R8G8, R16 */
    if (f >= 60 && f <= 65) return 1;                /* R8, A8 */
    if (f == 67 || f == 68 || f == 69) return 4;     /* R9G9B9E5, R8G8_B8G8, G8R8_G8B8 */
    if (f == 85 || f == 86) return 2;                /* B5G6R5, B5G5R5A1 */
    if (f >= 87 && f <= 93) return 4;                /* B8G8R8A8/X8 (+typeless/srgb), R10G10B10_XR */
    if (f == 115) return 2;                          /* B4G4R4A4 */
    return 0;
}

/* Tightly packed row pitch and row count (in blocks) of mip level `mip` of a w x h texture. */
static inline int d3dr_mip_layout(uint32_t f, uint32_t w, uint32_t h, uint32_t mip, uint32_t *row_pitch, uint32_t *rows) {
    uint32_t bd, bb = d3dr_format_block(f, &bd);
    if (!bb) return 0;
    uint32_t mw = w >> mip, mh = h >> mip;
    if (!mw) mw = 1;
    if (!mh) mh = 1;
    *row_pitch = (mw + bd - 1) / bd * bb;
    *rows = (mh + bd - 1) / bd;
    return 1;
}
#endif
