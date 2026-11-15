#include "hle/nvn/astc_encode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dsr::hle::nvn {

bool IsAstcEncodable(int f) { return f == 65 || f == 66 || f == 69 || f == 70 || f == 68 || f == 72; }

size_t AstcLevelBytes(int width, int height, int depth) {
    return size_t((std::max(1, width) + 3) / 4) * size_t((std::max(1, height) + 3) / 4) * size_t(std::max(1, depth)) * 16;
}

namespace {

// ASTC block layout used here (128 bits, bit 0 = least significant bit of byte 0):
//   [0..10]   block mode 0x42: 2D, 4x4 weight grid, one plane, weights of 2 bits (values 0..3)
//   [11..12]  partitions - 1 = 0
//   [13..16]  colour endpoint mode: 8 (RGB direct) or 12 (RGBA direct)
//   [17..]    endpoint values, 8 bits each: r0 r1 g0 g1 b0 b1 (a0 a1), range 256 because 79 bits are free
//   [127..96] the 32 weight bits, in reverse order (weight 0's lowest bit is bit 127)
constexpr uint32_t kBlockMode = 0x42;
constexpr int kWeight[4] = {0, 21, 43, 64};  // the 2-bit weights as the decoder unquantizes them (of 64)

void PutBits(uint8_t* b, int pos, uint32_t v, int n) {
    for (int i = 0; i < n; i++)
        if ((v >> i) & 1) b[(pos + i) >> 3] |= uint8_t(1u << ((pos + i) & 7));
}
uint32_t GetBits(const uint8_t* b, int pos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= uint32_t((b[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i;
    return v;
}

// A decoded channel between endpoints e0 and e1 at weight w (0..64), as the specification's LDR decode does.
inline int Lerp(int e0, int e1, int w) {
    const int c = (e0 * 257 * (64 - w) + e1 * 257 * w + 32) >> 6;
    return c >> 8;
}

inline int ClampByte(float v) {
    const int i = int(v + 0.5f);
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

struct Candidate {
    int e0[4], e1[4];
    uint8_t q[16];
    float err;
};

// Assigns every texel its best weight for the (integer) endpoints; returns the squared error.
float Assign(const float p[16][4], int nc, const float cw[4], const int e0[4], const int e1[4], uint8_t q[16]) {
    int dec[4][4];
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < nc; c++) dec[k][c] = Lerp(e0[c], e1[c], kWeight[k]);
    float total = 0;
    for (int i = 0; i < 16; i++) {
        float best = 1e30f;
        int bk = 0;
        for (int k = 0; k < 4; k++) {
            float d = 0;
            for (int c = 0; c < nc; c++) {
                const float e = p[i][c] - float(dec[k][c]);
                d += e * e * cw[c];
            }
            if (d < best) best = d, bk = k;
        }
        q[i] = uint8_t(bk);
        total += best;
    }
    return total;
}

void EncodeBlock(const uint8_t px[16][4], uint8_t out[16]) {
    int amin = 255;
    for (int i = 0; i < 16; i++) amin = std::min<int>(amin, px[i][3]);
    const bool opaque = amin == 255;
    const int nc = opaque ? 3 : 4;
    const float cw[4] = {1, 1, 1, 4};  // alpha counts more: cut-outs must keep their shape

    float p[16][4];
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) p[i][c] = float(px[i][c]);
    if (!opaque) {  // the colour of (nearly) transparent texels is not worth bits: use the mean of the others
        float sum[3] = {0, 0, 0};
        int n = 0;
        for (int i = 0; i < 16; i++)
            if (px[i][3] >= 16) {
                for (int c = 0; c < 3; c++) sum[c] += p[i][c];
                n++;
            }
        if (n > 0 && n < 16)
            for (int i = 0; i < 16; i++)
                if (px[i][3] < 16)
                    for (int c = 0; c < 3; c++) p[i][c] = sum[c] / float(n);
    }

    // Principal axis of the texels (power iteration on the covariance).
    float mean[4] = {0, 0, 0, 0}, lo[4], hi[4];
    for (int c = 0; c < nc; c++) lo[c] = 255, hi[c] = 0;
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < nc; c++) {
            mean[c] += p[i][c] * (1.0f / 16);
            lo[c] = std::min(lo[c], p[i][c]);
            hi[c] = std::max(hi[c], p[i][c]);
        }
    float cov[4][4] = {};
    for (int i = 0; i < 16; i++)
        for (int a = 0; a < nc; a++)
            for (int b = 0; b < nc; b++) cov[a][b] += (p[i][a] - mean[a]) * (p[i][b] - mean[b]);
    float axis[4] = {0, 0, 0, 0};
    for (int c = 0; c < nc; c++) axis[c] = (hi[c] - lo[c]) * (c == 3 ? 2.0f : 1.0f) + 1e-3f;
    for (int it = 0; it < 8; it++) {
        float v[4] = {0, 0, 0, 0}, len = 0;
        for (int a = 0; a < nc; a++)
            for (int b = 0; b < nc; b++) v[a] += cov[a][b] * axis[b];
        for (int c = 0; c < nc; c++) len += v[c] * v[c];
        len = std::sqrt(len);
        if (len < 1e-6f) break;
        for (int c = 0; c < nc; c++) axis[c] = v[c] / len;
    }
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < 16; i++) {
        float t = 0;
        for (int c = 0; c < nc; c++) t += (p[i][c] - mean[c]) * axis[c];
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }

    float f0[4], f1[4];
    for (int c = 0; c < nc; c++) {
        f0[c] = mean[c] + axis[c] * tmin;
        f1[c] = mean[c] + axis[c] * tmax;
    }

    // Alternate between the best weights for the endpoints and the best endpoints (least squares) for the weights.
    Candidate best;
    best.err = 1e30f;
    for (int c = 0; c < 4; c++) best.e0[c] = best.e1[c] = 255;
    memset(best.q, 0, sizeof(best.q));
    for (int iter = 0; iter < 4; iter++) {
        Candidate cand;
        for (int c = 0; c < 4; c++) cand.e0[c] = cand.e1[c] = 255;
        for (int c = 0; c < nc; c++) cand.e0[c] = ClampByte(f0[c]), cand.e1[c] = ClampByte(f1[c]);
        cand.err = Assign(p, nc, cw, cand.e0, cand.e1, cand.q);
        if (cand.err < best.err) best = cand;
        if (cand.err == 0) break;
        double a00 = 0, a01 = 0, a11 = 0;
        for (int i = 0; i < 16; i++) {
            const double w = kWeight[cand.q[i]] / 64.0;
            a00 += (1 - w) * (1 - w);
            a01 += (1 - w) * w;
            a11 += w * w;
        }
        const double det = a00 * a11 - a01 * a01;
        if (det < 1e-6) break;  // one weight everywhere: nothing to refine
        for (int c = 0; c < nc; c++) {
            double b0 = 0, b1 = 0;
            for (int i = 0; i < 16; i++) {
                const double w = kWeight[cand.q[i]] / 64.0;
                b0 += (1 - w) * p[i][c];
                b1 += w * p[i][c];
            }
            f0[c] = float((a11 * b0 - a01 * b1) / det);
            f1[c] = float((a00 * b1 - a01 * b0) / det);
        }
    }

    // CEM 8 / 12 read the endpoints in swapped, "blue contracted" form when the second one's RGB sum is smaller:
    // keep the first the darker (swapping the weights too; the weights are symmetric).
    if (best.e1[0] + best.e1[1] + best.e1[2] < best.e0[0] + best.e0[1] + best.e0[2]) {
        for (int c = 0; c < 4; c++) std::swap(best.e0[c], best.e1[c]);
        for (int i = 0; i < 16; i++) best.q[i] = uint8_t(3 - best.q[i]);
    }

    memset(out, 0, 16);
    PutBits(out, 0, kBlockMode, 11);
    PutBits(out, 11, 0, 2);
    PutBits(out, 13, opaque ? 8 : 12, 4);
    int pos = 17;
    for (int c = 0; c < nc; c++) {
        PutBits(out, pos, uint32_t(best.e0[c]), 8);
        PutBits(out, pos + 8, uint32_t(best.e1[c]), 8);
        pos += 16;
    }
    for (int i = 0; i < 16; i++)
        for (int b = 0; b < 2; b++)
            if ((best.q[i] >> b) & 1) {
                const int bit = 127 - (i * 2 + b);
                out[bit >> 3] |= uint8_t(1u << (bit & 7));
            }
}

}  // namespace

void EncodeAstc4x4(uint8_t* out, const uint8_t* rgba, int width, int height, int depth) {
    const int bxn = (width + 3) / 4, byn = (height + 3) / 4;
    for (int z = 0; z < std::max(1, depth); z++) {
        const uint8_t* slice = rgba + size_t(z) * size_t(width) * size_t(height) * 4;
        for (int by = 0; by < byn; by++)
            for (int bx = 0; bx < bxn; bx++) {
                uint8_t px[16][4];
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        const int sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
                        memcpy(px[y * 4 + x], slice + (size_t(sy) * size_t(width) + size_t(sx)) * 4, 4);
                    }
                EncodeBlock(px, out + ((size_t(z) * byn + by) * bxn + bx) * 16);
            }
    }
}

bool DecodeAstc4x4Block(const uint8_t* b, uint8_t out[16][4]) {
    if (GetBits(b, 0, 11) != kBlockMode || GetBits(b, 11, 2) != 0) return false;
    const uint32_t cem = GetBits(b, 13, 4);
    if (cem != 8 && cem != 12) return false;
    const int n = cem == 8 ? 6 : 8;
    int v[8];
    for (int i = 0; i < n; i++) v[i] = int(GetBits(b, 17 + 8 * i, 8));
    int e0[4] = {v[0], v[2], v[4], 255}, e1[4] = {v[1], v[3], v[5], 255};
    if (n == 8) e0[3] = v[6], e1[3] = v[7];
    if (v[1] + v[3] + v[5] < v[0] + v[2] + v[4]) {  // blue contraction, endpoints exchanged
        const int a0[4] = {(v[1] + v[5]) >> 1, (v[3] + v[5]) >> 1, v[5], n == 8 ? v[7] : 255};
        const int a1[4] = {(v[0] + v[4]) >> 1, (v[2] + v[4]) >> 1, v[4], n == 8 ? v[6] : 255};
        memcpy(e0, a0, sizeof(e0));
        memcpy(e1, a1, sizeof(e1));
    }
    for (int i = 0; i < 16; i++) {
        int q = 0;
        for (int k = 0; k < 2; k++) {
            const int bit = 127 - (i * 2 + k);
            q |= ((b[bit >> 3] >> (bit & 7)) & 1) << k;
        }
        for (int c = 0; c < 4; c++) out[i][c] = uint8_t(Lerp(e0[c], e1[c], kWeight[q]));
    }
    return true;
}

}  // namespace dsr::hle::nvn
