#pragma once
// CPU encoder for ETC1-compatible ETC2 blocks (RGB8 and RGB8A1 "punch-through"), used on GPUs that have
// no BC support but do support ETC2 (e.g. Mali). Input is RGBA8; output is 8 bytes per 4x4 block.
//
// Formats: VK_FORMAT_ETC2_R8G8B8_*_BLOCK (opaque) and VK_FORMAT_ETC2_R8G8B8A1_*_BLOCK (1-bit alpha).
// In the A1 format, a block in individual mode (diff bit 0) whose pixel index is 2 (msb 1, lsb 0) is
// transparent black, so opaque pixels in such blocks never use index 2.

#include <cstddef>
#include <cstdint>

namespace dsr::hle::nvn {

// True for the NVN BC1 formats that map to ETC2 (65, 66, 69, 70). 66/70 carry 1-bit alpha.
bool IsEtcEncodable(int nvn_format);
bool EtcHasPunchThrough(int nvn_format);

// Bytes of ETC data for a level of width x height x depth texels (blocks of 4x4, 8 bytes each).
size_t EtcLevelBytes(int width, int height, int depth);

// Encodes `depth` slices of width x height RGBA8 texels. Pixels with alpha < 128 are transparent when
// `punch_through` is set; otherwise alpha is ignored.
void EncodeEtc1(uint8_t* out, const uint8_t* rgba, int width, int height, int depth, bool punch_through);

// BC3 (NVN 68 UNORM, 72 sRGB) -> ETC2 RGBA8: 16 bytes per 4x4 block (8 bytes EAC alpha, then an 8-byte ETC1-style
// colour block). 4x smaller than the RGBA8 the BC3 decoder produces. Lossy (the colour half is the same ETC1 encoder
// as for BC1; alpha is EAC with 16 tables), so it is opt-in (settings: bc3_etc2).
bool IsEtcRgba8Encodable(int nvn_format);
size_t EtcRgba8LevelBytes(int width, int height, int depth);
void EncodeEtc2Rgba8(uint8_t* out, const uint8_t* rgba, int width, int height, int depth);
// Decodes one 8-byte EAC alpha block (for tests) into 16 alpha values, row-major (y * 4 + x).
void DecodeEacAlphaBlock(const uint8_t* block, uint8_t out[16]);

// Decodes one 8-byte block (for tests) into 16 RGBA texels, row-major (y * 4 + x).
void DecodeEtc1Block(const uint8_t* block, bool punch_through, uint8_t out[16][4]);

}  // namespace dsr::hle::nvn
