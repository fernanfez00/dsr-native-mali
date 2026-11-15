#pragma once
// CPU encoder for ASTC 4x4 (LDR) textures, an alternative to ETC2 (etc_encode.h) on GPUs that have no BC support but
// do have ASTC (all Mali GPUs; settings bc_astc). BC textures are decoded to RGBA8 and re-encoded, so they stay
// compressed on the GPU.
//
// Every block is a single-partition block with a 4x4 weight grid of 2-bit weights and 8-bit endpoints:
//  - opaque blocks: colour endpoint mode 8 (RGB direct). BC1 has four colours on a line (1/3 steps), which the four
//    ASTC weights (0, 21, 43, 64 of 64) reproduce to within a level or so.
//  - blocks with alpha: colour endpoint mode 12 (RGBA direct), one set of weights for colour and alpha. Colour of
//    (nearly) transparent texels is ignored by the fit. Lossier than the separate EAC alpha of ETC2 RGBA8 where
//    colour and alpha vary independently inside one block.
// 16 bytes per 4x4 block (8 bits per texel), twice a BC1 texture in ETC2 and the same as BC3.

#include <cstddef>
#include <cstdint>

namespace dsr::hle::nvn {

// NVN BC1 (65, 66, 69, 70) and BC3 (68, 72) formats: the ones converted to ASTC.
bool IsAstcEncodable(int nvn_format);

// Bytes of a level of width x height (x depth) texels: 16 per 4x4 block.
size_t AstcLevelBytes(int width, int height, int depth);

// Encodes `depth` slices of width x height RGBA8 texels (rows of width * 4 bytes), blocks row by row (each block's
// rows are row-major, as ASTC wants). Edge texels beyond the image are replicated from the nearest one.
void EncodeAstc4x4(uint8_t* out, const uint8_t* rgba, int width, int height, int depth);

// Decodes one block written by EncodeAstc4x4 into 16 RGBA texels, row-major (y * 4 + x): for tests. Other kinds of
// ASTC block are not understood (returns false).
bool DecodeAstc4x4Block(const uint8_t* block, uint8_t out[16][4]);

}  // namespace dsr::hle::nvn
