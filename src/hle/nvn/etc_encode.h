#pragma once
// CPU encoder for ETC2 textures, used on GPUs that have no BC support but do support ETC2 (Mali; Adreno 6xx without
// BCeNabler). BC textures are decoded to RGBA8 and re-encoded, so they stay compressed on the GPU (BC1 -> ETC2 is
// 8 bytes per 4x4 block either way: 4x smaller than RGBA8). Based on pull request #13 (fernanfez00), with the
// punch-through (1-bit alpha) format encoded as the specification defines it.
//
// Formats (4x4 blocks, big-endian 64-bit words):
//  - ETC2 RGB8 (VK_FORMAT_ETC2_R8G8B8_*): ETC1 individual and differential modes.
//  - ETC2 RGB8A1 (VK_FORMAT_ETC2_R8G8B8A1_*): differential mode only; the ETC1 "diff" bit is the "opaque" bit.
//    With it clear, pixel index 2 (msb 1, lsb 0) is transparent black and the small modifiers are 0.
//  - ETC2 RGBA8 (VK_FORMAT_ETC2_R8G8B8A8_*): an 8-byte EAC alpha block, then an 8-byte RGB8 block.

#include <cstddef>
#include <cstdint>

namespace dsr::hle::nvn {

// NVN BC1 formats (65, 66, 69, 70) -> ETC2 RGB8 (65, 69) or RGB8A1 (66, 70: BC1 with 1-bit alpha).
bool IsEtcEncodable(int nvn_format);
bool EtcHasPunchThrough(int nvn_format);
// NVN BC3 formats (68, 72) -> ETC2 RGBA8 (lossier: opt-in, settings bc3_etc2).
bool IsEtcRgba8Encodable(int nvn_format);

// Bytes of a level of width x height (x depth) texels: 8 per block (RGB8, RGB8A1) or 16 (RGBA8).
size_t EtcLevelBytes(int width, int height, int depth);
size_t EtcRgba8LevelBytes(int width, int height, int depth);

// Encodes `depth` slices of width x height RGBA8 texels (rows of width * 4 bytes), blocks row by row.
// punch_through: RGB8A1 (alpha < 128 = transparent); otherwise RGB8 (alpha ignored).
void EncodeEtc2Rgb(uint8_t* out, const uint8_t* rgba, int width, int height, int depth, bool punch_through);
void EncodeEtc2Rgba8(uint8_t* out, const uint8_t* rgba, int width, int height, int depth);

// Decoders of one block into 16 RGBA texels, row-major (y * 4 + x): for tests.
void DecodeEtc2RgbBlock(const uint8_t* block, bool punch_through, uint8_t out[16][4]);
void DecodeEacAlphaBlock(const uint8_t* block, uint8_t out[16]);

}  // namespace dsr::hle::nvn
