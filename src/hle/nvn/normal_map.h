#pragma once
// Tangent-space normal maps in BC1 / BC3 must not go through ETC2: its colour block moves R, G and B together by one
// brightness step per texel, so three independent channels (x, y, z) come out with errors of 7 to 13 degrees in the
// normal, up to 50 on fine detail (skin pores, wrinkles): coloured speckles where the lighting and reflections use them.
// Such textures are kept as decoded RGBA8 instead (settings bc_normals).

#include <cstdint>

#include "hle/nvn/bc_decode.h"
#include "hle/nvn/layout.h"

namespace dsr::hle::nvn {

// Mean R, G, B, A (0..255) of a BC1 / BC3 texture, from one small level of its chain (layer 0: at most 32 texels on the
// long side, at least 4 across). `texture` is the start of the texture's memory. False: not a block-linear BC1 / BC3
// texture, or a chain with no such level (single-level textures are never taken for normal maps).
bool BcMeanColor(const uint8_t* texture, const TextureLayout& layout, BcKind kind, float mean[4]);

// Does a texture with this mean colour look like a normal map? RGB normals (x, y, z = R, G, B: the mean is about
// 128, 128, 255) or DXT5nm (BC3: x in alpha, y in green, red unused: the mean alpha and green are about 128).
bool MeanLooksLikeNormalMap(const float mean[4], BcKind kind);

}  // namespace dsr::hle::nvn
