#include "hle/nvn/etc_encode.h"

#include <cstring>
#include <limits>

namespace dsr::hle::nvn {
namespace {

// ETC1 intensity modifier tables (per codeword table: {-large, -small, +small, +large}).
constexpr int kModifiers[8][4] = {
    {-8, -2, 2, 8},     {-17, -5, 5, 17},  {-29, -9, 9, 29},    {-42, -13, 13, 42},
    {-60, -18, 18, 60}, {-80, -24, 24, 80}, {-106, -33, 33, 106}, {-183, -47, 47, 183},
};

// 2-bit pixel code -> index into kModifiers[t]. Code = (msb << 1) | lsb:
//   00 -> +small, 01 -> +large, 10 -> -small, 11 -> -large.
constexpr int kCodeToSlot[4] = {2, 3, 1, 0};

inline int Modifier(int table, int code) { return kModifiers[table][kCodeToSlot[code]]; }

inline int ClampByte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// One pixel of a 4x4 block. Index i = x * 4 + y (column-major, as in the ETC bitstream).
struct Texel {
    int rgb[3];
    bool transparent;
};

void GatherBlock(const uint8_t* rgba, int w, int h, int bx, int by, bool punch, Texel t[16]) {
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            const int sx = bx * 4 + x < w ? bx * 4 + x : w - 1;
            const int sy = by * 4 + y < h ? by * 4 + y : h - 1;
            const uint8_t* p = rgba + (size_t(sy) * size_t(w) + size_t(sx)) * 4;
            Texel& e = t[x * 4 + y];
            e.rgb[0] = p[0];
            e.rgb[1] = p[1];
            e.rgb[2] = p[2];
            e.transparent = punch && p[3] < 128;
        }
}

// Subblock membership: flip 0 splits left/right halves (x), flip 1 splits top/bottom halves (y).
inline bool InSubblock(int i, int flip, int s) {
    const int x = i / 4, y = i % 4;
    return (flip == 0 ? x / 2 : y / 2) == s;
}

inline int Expand4(int v) { return v * 17; }
inline int Expand5(int v) { return (v << 3) | (v >> 2); }

// Chooses the best table and per-pixel codes for the pixels of one subblock, given its base colour.
// Returns the squared error; fills codes[i] for the subblock's pixels and *table_out.
int SolveSubblock(const Texel t[16], int flip, int s, const int base[3], bool punch_individual, int codes[16],
                  int* table_out) {
    // Only the tables whose largest modifier matches the subblock's colour range are worth trying.
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (!InSubblock(i, flip, s) || t[i].transparent) continue;
        for (int c = 0; c < 3; c++) {
            if (t[i].rgb[c] < lo[c]) lo[c] = t[i].rgb[c];
            if (t[i].rgb[c] > hi[c]) hi[c] = t[i].rgb[c];
        }
    }
    int range = 0;
    for (int c = 0; c < 3; c++) range = hi[c] - lo[c] > range ? hi[c] - lo[c] : range;
    int first = 7;
    for (int k = 0; k < 8; k++)
        if (kModifiers[k][3] * 2 >= range) {
            first = k;
            break;
        }
    const int from = first > 0 ? first - 1 : 0, to = first < 7 ? first + 1 : 7;

    int best_err = std::numeric_limits<int>::max();
    int best_table = 0;
    int best_codes[16] = {};
    for (int table = from; table <= to; table++) {
        int err = 0;
        int trial[16] = {};
        for (int i = 0; i < 16; i++) {
            if (!InSubblock(i, flip, s)) continue;
            const Texel& e = t[i];
            if (e.transparent) {  // only reachable in punch-through individual mode: code 2, black, alpha 0
                trial[i] = 2;
                continue;
            }
            int best_pix = std::numeric_limits<int>::max(), best_code = 0;
            for (int code = 0; code < 4; code++) {
                if (punch_individual && code == 2) continue;  // reserved for transparency
                int pe = 0;
                for (int c = 0; c < 3; c++) {
                    const int d = ClampByte(base[c] + Modifier(table, code)) - e.rgb[c];
                    pe += d * d;
                }
                if (pe < best_pix) {
                    best_pix = pe;
                    best_code = code;
                }
            }
            trial[i] = best_code;
            err += best_pix;
        }
        if (err < best_err) {
            best_err = err;
            best_table = table;
            std::memcpy(best_codes, trial, sizeof(trial));
        }
    }
    for (int i = 0; i < 16; i++)
        if (InSubblock(i, flip, s)) codes[i] = best_codes[i];
    *table_out = best_table;
    return best_err;
}

// Average of the opaque pixels of one subblock (0 if there are none).
void SubblockAverage(const Texel t[16], int flip, int s, int avg[3]) {
    int sum[3] = {0, 0, 0}, n = 0;
    for (int i = 0; i < 16; i++) {
        if (!InSubblock(i, flip, s) || t[i].transparent) continue;
        for (int c = 0; c < 3; c++) sum[c] += t[i].rgb[c];
        n++;
    }
    for (int c = 0; c < 3; c++) avg[c] = n ? (sum[c] + n / 2) / n : 0;
}

// Encodes one block into a 64-bit ETC1/ETC2 word (see the layout comment in DecodeEtc1Block).
uint64_t EncodeBlock(const Texel t[16], bool punch) {
    bool any_transparent = false;
    for (int i = 0; i < 16; i++) any_transparent |= t[i].transparent;

    uint64_t best_word = 0;
    int best_err = std::numeric_limits<int>::max();

    for (int flip = 0; flip < 2; flip++) {
        bool diff_done = false;
        for (int diff = 1; diff >= 0; diff--) {
            if (diff_done) break;  // differential is good enough when it fits: skip individual mode
            if (diff && any_transparent) continue;  // differential blocks cannot signal transparency

            int base_code[2][3];     // stored base values (4-bit for individual, 5-bit for differential)
            int base_rgb[2][3];      // expanded 8-bit base colours used for the error search
            bool feasible = true;
            for (int s = 0; s < 2 && feasible; s++) {
                int avg[3];
                SubblockAverage(t, flip, s, avg);
                for (int c = 0; c < 3; c++) {
                    if (diff) {
                        base_code[s][c] = ClampByte((avg[c] + 4) / 8) > 31 ? 31 : (avg[c] + 4) / 8;
                        base_rgb[s][c] = Expand5(base_code[s][c]);
                    } else {
                        base_code[s][c] = (avg[c] + 8) / 17 > 15 ? 15 : (avg[c] + 8) / 17;
                        base_rgb[s][c] = Expand4(base_code[s][c]);
                    }
                }
            }
            if (diff) {
                // The second subblock is stored as a 3-bit signed delta from the first: it must fit.
                for (int c = 0; c < 3; c++) {
                    const int d = base_code[1][c] - base_code[0][c];
                    if (d < -4 || d > 3) feasible = false;
                }
            }
            if (!feasible) continue;

            int codes[16] = {};
            int tables[2] = {0, 0};
            int err = 0;
            for (int s = 0; s < 2; s++) {
                err += SolveSubblock(t, flip, s, base_rgb[s], punch && !diff, codes, &tables[s]);
            }
            if (diff) diff_done = true;
            if (err >= best_err) continue;
            best_err = err;

            uint64_t v = 0;
            if (diff) {
                v |= uint64_t(base_code[0][0]) << 59;
                v |= uint64_t((base_code[1][0] - base_code[0][0]) & 7) << 56;
                v |= uint64_t(base_code[0][1]) << 51;
                v |= uint64_t((base_code[1][1] - base_code[0][1]) & 7) << 48;
                v |= uint64_t(base_code[0][2]) << 43;
                v |= uint64_t((base_code[1][2] - base_code[0][2]) & 7) << 40;
                v |= uint64_t(1) << 39;
            } else {
                v |= uint64_t(base_code[0][0]) << 60;
                v |= uint64_t(base_code[1][0]) << 56;
                v |= uint64_t(base_code[0][1]) << 52;
                v |= uint64_t(base_code[1][1]) << 48;
                v |= uint64_t(base_code[0][2]) << 44;
                v |= uint64_t(base_code[1][2]) << 40;
            }
            if (flip) v |= uint64_t(1) << 38;
            v |= uint64_t(tables[0]) << 35;
            v |= uint64_t(tables[1]) << 32;
            for (int i = 0; i < 16; i++) {
                const uint64_t lsb = uint64_t(codes[i] & 1), msb = uint64_t(codes[i] >> 1);
                v |= lsb << i;
                v |= msb << (16 + i);
            }
            best_word = v;
        }
    }
    return best_word;
}

void StoreBigEndian(uint8_t* out, uint64_t v) {
    for (int k = 0; k < 8; k++) out[k] = uint8_t(v >> (56 - 8 * k));
}

}  // namespace

bool IsEtcEncodable(int f) { return f == 65 || f == 66 || f == 69 || f == 70; }

bool EtcHasPunchThrough(int f) { return f == 66 || f == 70; }

size_t EtcLevelBytes(int width, int height, int depth) {
    const size_t bx = size_t((width + 3) / 4), by = size_t((height + 3) / 4);
    return bx * by * size_t(depth > 0 ? depth : 1) * 8;
}

void EncodeEtc1(uint8_t* out, const uint8_t* rgba, int width, int height, int depth, bool punch_through) {
    const int bxn = (width + 3) / 4, byn = (height + 3) / 4;
    for (int z = 0; z < depth; z++) {
        const uint8_t* slice = rgba + size_t(z) * size_t(width) * size_t(height) * 4;
        for (int by = 0; by < byn; by++)
            for (int bx = 0; bx < bxn; bx++) {
                Texel t[16];
                GatherBlock(slice, width, height, bx, by, punch_through, t);
                uint8_t* dst = out + ((size_t(z) * byn + by) * bxn + bx) * 8;
                StoreBigEndian(dst, EncodeBlock(t, punch_through));
            }
    }
}

void DecodeEtc1Block(const uint8_t* block, bool punch_through, uint8_t out[16][4]) {
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v = (v << 8) | block[k];
    const bool diff = (v >> 39) & 1, flip = (v >> 38) & 1;
    const int tables[2] = {int((v >> 35) & 7), int((v >> 32) & 7)};
    int base[2][3];
    if (diff) {
        const int b0[3] = {int((v >> 59) & 31), int((v >> 51) & 31), int((v >> 43) & 31)};
        const int dd[3] = {int((v >> 56) & 7), int((v >> 48) & 7), int((v >> 40) & 7)};
        for (int c = 0; c < 3; c++) {
            const int d = dd[c] >= 4 ? dd[c] - 8 : dd[c];
            base[0][c] = Expand5(b0[c]);
            base[1][c] = Expand5(b0[c] + d);
        }
    } else {
        const int r1 = int((v >> 60) & 15), r2 = int((v >> 56) & 15), g1 = int((v >> 52) & 15),
                  g2 = int((v >> 48) & 15), b1 = int((v >> 44) & 15), b2 = int((v >> 40) & 15);
        base[0][0] = Expand4(r1);
        base[1][0] = Expand4(r2);
        base[0][1] = Expand4(g1);
        base[1][1] = Expand4(g2);
        base[0][2] = Expand4(b1);
        base[1][2] = Expand4(b2);
    }
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            const int i = x * 4 + y;
            const int s = flip ? (y / 2) : (x / 2);
            const int code = int(((v >> (16 + i)) & 1) << 1 | ((v >> i) & 1));
            uint8_t* o = out[y * 4 + x];
            if (punch_through && !diff && code == 2) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            for (int c = 0; c < 3; c++) o[c] = uint8_t(ClampByte(base[s][c] + Modifier(tables[s], code)));
            o[3] = 255;
        }
}

}  // namespace dsr::hle::nvn
