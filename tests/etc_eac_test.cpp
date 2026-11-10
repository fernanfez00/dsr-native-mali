// Host test for the ETC2 RGBA8 (EAC alpha) encoder: g++ -std=c++17 -I src tests/etc_eac_test.cpp src/hle/nvn/etc_encode.cpp
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "hle/nvn/etc_encode.h"

using namespace dsr::hle::nvn;

int main() {
    int failures = 0;
    // 1. flat alpha is exact
    for (int v : {0, 1, 77, 128, 254, 255}) {
        std::vector<uint8_t> rgba(16 * 4, 0);
        for (int i = 0; i < 16; i++) rgba[i * 4 + 3] = uint8_t(v);
        uint8_t out[16];
        EncodeEtc2Rgba8(out, rgba.data(), 4, 4, 1);
        uint8_t a[16];
        DecodeEacAlphaBlock(out, a);
        for (int i = 0; i < 16; i++)
            if (a[i] != v) { printf("FAIL flat %d -> %d\n", v, a[i]); failures++; break; }
    }
    // 2. random / ramp blocks: mean error stays small
    srand(1);
    double total = 0, worst_mean = 0;
    const int N = 20000;
    for (int n = 0; n < N; n++) {
        std::vector<uint8_t> rgba(16 * 4, 0);
        const int kind = n % 3;
        const int a0 = rand() & 255, a1 = rand() & 255;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int v = kind == 0 ? rand() & 255 : kind == 1 ? a0 + (a1 - a0) * (x + y) / 6 : (rand() & 1 ? a0 : a1);
                rgba[(y * 4 + x) * 4 + 3] = uint8_t(v);
            }
        uint8_t out[16];
        EncodeEtc2Rgba8(out, rgba.data(), 4, 4, 1);
        uint8_t a[16];
        DecodeEacAlphaBlock(out, a);
        double e = 0;
        for (int i = 0; i < 16; i++) e += abs(int(a[i]) - int(rgba[i * 4 + 3]));
        e /= 16;
        total += e;
        if (kind != 0 && e > worst_mean) worst_mean = e;
        if (kind == 1 && e > 9) { printf("FAIL smooth ramp mean err %.1f (a0 %d a1 %d)\n", e, a0, a1); failures++; if (failures > 5) return 1; }
    }
    printf("mean abs alpha error over %d blocks: %.2f (worst smooth/two-level block mean %.1f)\n", N, total / N, worst_mean);
    // 3. non-multiple-of-4 size and byte counts
    std::vector<uint8_t> img(7 * 5 * 4, 200);
    std::vector<uint8_t> out(EtcRgba8LevelBytes(7, 5, 1));
    EncodeEtc2Rgba8(out.data(), img.data(), 7, 5, 1);
    if (out.size() != 2 * 2 * 16) { printf("FAIL size %zu\n", out.size()); failures++; }
    // 4. colour half still decodes as a valid ETC1 block of a flat colour
    std::vector<uint8_t> col(16 * 4);
    for (int i = 0; i < 16; i++) { col[i*4] = 100; col[i*4+1] = 150; col[i*4+2] = 50; col[i*4+3] = 255; }
    uint8_t blk[16];
    EncodeEtc2Rgba8(blk, col.data(), 4, 4, 1);
    uint8_t px[16][4];
    DecodeEtc1Block(blk + 8, false, px);
    printf("colour half decodes to (%d,%d,%d), expected near (100,150,50)\n", px[0][0], px[0][1], px[0][2]);
    if (abs(px[0][0] - 100) > 8 || abs(px[0][1] - 150) > 8 || abs(px[0][2] - 50) > 8) failures++;
    // 5. speed: 2048x2048 level
    const int W = 2048;
    std::vector<uint8_t> big(size_t(W) * W * 4);
    for (size_t i = 0; i < big.size(); i++) big[i] = uint8_t((i * 2654435761u) >> 13);
    std::vector<uint8_t> bo(EtcRgba8LevelBytes(W, W, 1));
    auto t0 = std::chrono::steady_clock::now();
    EncodeEtc2Rgba8(bo.data(), big.data(), W, W, 1);
    printf("2048x2048 noise: %.0f ms single thread (worst case; real alpha is mostly flat)\n",
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
