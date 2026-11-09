#pragma once
// CPU decoding of BC1-BC5 (S3TC / RGTC) block-compressed textures into RGBA8.
//
// Used when the GPU has no texture compression BC support (most phone GPUs, e.g. Mali): the texture is
// stored as RGBA8 and the game's compressed blocks are decoded at upload time.

#include <cstddef>
#include <cstdint>

namespace dsr::hle::nvn {

// True for the NVN formats this decoder handles (BC1-BC5, UNORM and sRGB). BC4/BC5 SNORM and BC6/BC7
// are not handled and keep their previous behaviour (white stand-in).
bool IsBcDecodable(int nvn_format);

// Decodes a block-compressed image into tightly packed RGBA8 (width * height * depth * 4 bytes).
//   blocks:    `depth` slices; each slice is `rows` block rows of `row_bytes` bytes (row_bytes includes
//              any padding after the blocks). Block rows and slices are tightly packed by the caller.
//   width/height: texel size of the level; blocks at the right/bottom edge are clipped.
void DecodeBcBlocks(uint8_t* rgba, const uint8_t* blocks, int width, int height, int depth, size_t row_bytes,
                    int rows, int nvn_format);

}  // namespace dsr::hle::nvn
