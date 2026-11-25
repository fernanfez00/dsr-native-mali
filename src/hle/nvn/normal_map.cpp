#include "hle/nvn/normal_map.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dsr::hle::nvn {

bool BcMeanColor(const uint8_t* texture, const TextureLayout& layout, BcKind kind, float mean[4]) {
    if ((kind != BcKind::BC1 && kind != BcKind::BC3) || !texture || layout.linear || layout.levels.size() < 3) return false;
    const LevelLayout* l = nullptr;
    for (const LevelLayout& c : layout.levels)
        if (std::max(c.width, c.height) <= 32 && std::min(c.width, c.height) >= 4) {
            l = &c;
            break;
        }
    if (!l || l->depth != 1) return false;
    const int bpb = kind == BcKind::BC1 ? 8 : 16;
    if (l->row_bytes <= 0 || l->rows <= 0 || l->row_bytes % bpb) return false;
    std::vector<uint8_t> blocks(size_t(l->row_bytes) * l->rows);
    DeswizzleLevel(blocks.data(), texture + l->offset, *l);
    std::vector<uint8_t> rgba(size_t(l->width) * l->height * 4);
    BcDecode(kind, blocks.data(), l->row_bytes / bpb, l->rows, rgba.data(), l->width, l->height);
    double sum[4] = {0, 0, 0, 0};
    const size_t n = size_t(l->width) * l->height;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) sum[c] += rgba[i * 4 + c];
    for (int c = 0; c < 4; c++) mean[c] = float(sum[c] / double(n));
    return true;
}

bool MeanLooksLikeNormalMap(const float mean[4], BcKind kind) {
    const bool rgb = std::fabs(mean[0] - 128.0f) <= 36.0f && std::fabs(mean[1] - 128.0f) <= 36.0f && mean[2] >= 185.0f;
    const bool dxt5nm = kind == BcKind::BC3 && std::fabs(mean[1] - 128.0f) <= 36.0f && std::fabs(mean[3] - 128.0f) <= 36.0f &&
                        (mean[0] >= 224.0f || mean[0] <= 40.0f);
    return rgb || dxt5nm;
}

}  // namespace dsr::hle::nvn
