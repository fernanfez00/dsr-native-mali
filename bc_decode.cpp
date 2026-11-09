#include "hle/nvn/bc_decode.h"

#include <cstring>
#include <vector>

namespace dsr::hle::nvn {
namespace {

// BC1: 8 bytes = two RGB565 endpoints + 16 two-bit indices.
// BC2: 8 bytes explicit alpha (4 bits per texel) + BC1 colour block (always 4-colour mode).
// BC3: 8 bytes interpolated alpha (like BC4) + BC1 colour block (always 4-colour mode).
// BC4: 8 bytes interpolated single channel. BC5: two BC4 blocks (R, then G).

inline uint8_t Expand5(uint32_t v) { return uint8_t((v << 3) | (v >> 2)); }
inline uint8_t Expand6(uint32_t v) { return uint8_t((v << 2) | (v >> 4)); }

struct Rgb {
    uint8_t r, g, b;
};

inline Rgb Rgb565(uint16_t c) {
    return {Expand5((c >> 11) & 31), Expand6((c >> 5) & 63), Expand5(c & 31)};
}

// Colour block at `p` (8 bytes) -> 16 RGBA texels (row-major 4x4). With `allow_punch_through` (BC1 only),
// c0 <= c1 selects the 3-colour mode whose index 3 is transparent black; alpha is set to 0 there and
// otherwise left for the caller.
void DecodeColorBlock(const uint8_t* p, bool allow_punch_through, uint8_t out[16][4], bool* any_transparent) {
    const uint16_t c0 = uint16_t(p[0] | p[1] << 8);
    const uint16_t c1 = uint16_t(p[2] | p[3] << 8);
    const Rgb a = Rgb565(c0), b = Rgb565(c1);
    Rgb pal[4];
    pal[0] = a;
    pal[1] = b;
    const bool four_colour = !allow_punch_through || c0 > c1;
    if (four_colour) {
        pal[2] = {uint8_t((2 * a.r + b.r) / 3), uint8_t((2 * a.g + b.g) / 3), uint8_t((2 * a.b + b.b) / 3)};
        pal[3] = {uint8_t((a.r + 2 * b.r) / 3), uint8_t((a.g + 2 * b.g) / 3), uint8_t((a.b + 2 * b.b) / 3)};
    } else {
        pal[2] = {uint8_t((a.r + b.r) / 2), uint8_t((a.g + b.g) / 2), uint8_t((a.b + b.b) / 2)};
        pal[3] = {0, 0, 0};
    }
    const uint32_t bits = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
    for (int i = 0; i < 16; i++) {
        const uint32_t idx = (bits >> (2 * i)) & 3;
        const Rgb c = pal[idx];
        out[i][0] = c.r;
        out[i][1] = c.g;
        out[i][2] = c.b;
        out[i][3] = 255;
        if (!four_colour && idx == 3) {
            out[i][3] = 0;
            if (any_transparent) *any_transparent = true;
        }
    }
}

// Interpolated single-channel block (BC3 alpha, BC4, BC5 channels): 8 bytes.
void DecodeAlphaBlock(const uint8_t* p, uint8_t out[16]) {
    const uint32_t a0 = p[0], a1 = p[1];
    uint8_t pal[8];
    pal[0] = uint8_t(a0);
    pal[1] = uint8_t(a1);
    if (a0 > a1) {
        for (int k = 2; k <= 7; k++) pal[k] = uint8_t(((8 - k) * a0 + (k - 1) * a1) / 7);
    } else {
        for (int k = 2; k <= 5; k++) pal[k] = uint8_t(((6 - k) * a0 + (k - 1) * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    // 16 three-bit indices in the 48 bits of bytes 2..7, texel i at bit 3*i.
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(p[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = pal[(bits >> (3 * i)) & 7];
}

}  // namespace

bool IsBcDecodable(int f) {
    switch (f) {
        case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 72: case 73: case 75:
            return true;
        default:
            return false;
    }
}

void DecodeBcBlocks(uint8_t* rgba, const uint8_t* blocks, int width, int height, int depth, size_t row_bytes,
                    int rows, int nvn_format) {
    if (width <= 0 || height <= 0 || depth <= 0) return;
    const int blocks_x = (width + 3) / 4;
    const int blocks_y = rows;
    const size_t slice_bytes = row_bytes * size_t(blocks_y);
    const size_t bpb = (nvn_format == 65 || nvn_format == 66 || nvn_format == 69 || nvn_format == 70 ||
                        nvn_format == 73)
                           ? 8
                           : 16;
    const bool has_alpha = nvn_format == 66 || nvn_format == 67 || nvn_format == 68 || nvn_format == 70 ||
                           nvn_format == 71 || nvn_format == 72;

    for (int z = 0; z < depth; z++) {
        const uint8_t* slice = blocks + size_t(z) * slice_bytes;
        uint8_t* dst_slice = rgba + size_t(z) * size_t(width) * size_t(height) * 4;
        for (int by = 0; by < blocks_y; by++) {
            for (int bx = 0; bx < blocks_x; bx++) {
                const uint8_t* b = slice + size_t(by) * row_bytes + size_t(bx) * bpb;
                uint8_t texels[16][4];
                if (nvn_format == 73) {  // BC4: red only
                    uint8_t r[16];
                    DecodeAlphaBlock(b, r);
                    for (int i = 0; i < 16; i++) {
                        texels[i][0] = r[i];
                        texels[i][1] = 0;
                        texels[i][2] = 0;
                        texels[i][3] = 255;
                    }
                } else if (nvn_format == 75) {  // BC5: red and green
                    uint8_t r[16], g[16];
                    DecodeAlphaBlock(b, r);
                    DecodeAlphaBlock(b + 8, g);
                    for (int i = 0; i < 16; i++) {
                        texels[i][0] = r[i];
                        texels[i][1] = g[i];
                        texels[i][2] = 0;
                        texels[i][3] = 255;
                    }
                } else if (nvn_format == 67 || nvn_format == 71) {  // BC2: explicit alpha + colour
                    DecodeColorBlock(b + 8, false, texels, nullptr);
                    for (int i = 0; i < 16; i++) {
                        const uint8_t byte = b[i / 2];
                        const uint8_t nib = (i & 1) ? uint8_t(byte >> 4) : uint8_t(byte & 15);
                        texels[i][3] = uint8_t(nib * 17);
                    }
                } else if (nvn_format == 68 || nvn_format == 72) {  // BC3: interpolated alpha + colour
                    DecodeColorBlock(b + 8, false, texels, nullptr);
                    uint8_t a[16];
                    DecodeAlphaBlock(b, a);
                    for (int i = 0; i < 16; i++) texels[i][3] = a[i];
                } else {  // BC1 (RGB or RGBA variants)
                    bool transparent = false;
                    DecodeColorBlock(b, true, texels, &transparent);
                    if (!has_alpha)
                        for (int i = 0; i < 16; i++) texels[i][3] = 255;
                }
                // Write the visible part of the 4x4 block.
                for (int ty = 0; ty < 4; ty++) {
                    const int y = by * 4 + ty;
                    if (y >= height) break;
                    for (int tx = 0; tx < 4; tx++) {
                        const int x = bx * 4 + tx;
                        if (x >= width) break;
                        uint8_t* d = dst_slice + (size_t(y) * size_t(width) + size_t(x)) * 4;
                        std::memcpy(d, texels[ty * 4 + tx], 4);
                    }
                }
            }
        }
    }
}

}  // namespace dsr::hle::nvn
