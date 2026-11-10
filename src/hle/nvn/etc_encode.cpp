#include "hle/nvn/etc_encode.h"

#include <cstring>
#include <limits>
#include <utility>

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
                v |= uint64_t(1) << 33;
            } else {
                v |= uint64_t(base_code[0][0]) << 60;
                v |= uint64_t(base_code[1][0]) << 56;
                v |= uint64_t(base_code[0][1]) << 52;
                v |= uint64_t(base_code[1][1]) << 48;
                v |= uint64_t(base_code[0][2]) << 44;
                v |= uint64_t(base_code[1][2]) << 40;
            }
            if (flip) v |= uint64_t(1) << 32;
            v |= uint64_t(tables[0]) << 37;
            v |= uint64_t(tables[1]) << 34;
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


// ---- ETC2 RGBA8: EAC alpha block + ETC1-style colour block ---------------------------------------------------------
// EAC alpha modifier tables (8 ascending modifiers each). Block: byte 0 = base codeword, byte 1 = multiplier (high
// nibble) | table (low nibble), then sixteen 3-bit indices (48 bits, big endian, texel order x * 4 + y).
constexpr int kEacMods[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},  {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};
// Table 13 has a zero modifier at index 4: a flat block (very common: opaque textures) is stored exactly.
constexpr int kEacFlatTable = 13, kEacFlatIndex = 4;

// The tables sorted ascending (with each entry's original index), for a 3-step nearest-value search.
struct EacSorted {
    int mod[16][8];
    int index[16][8];
    EacSorted() {
        for (int t = 0; t < 16; t++) {
            for (int k = 0; k < 8; k++) {
                mod[t][k] = kEacMods[t][k];
                index[t][k] = k;
            }
            for (int i = 1; i < 8; i++)  // insertion sort
                for (int j = i; j > 0 && mod[t][j - 1] > mod[t][j]; j--) {
                    std::swap(mod[t][j - 1], mod[t][j]);
                    std::swap(index[t][j - 1], index[t][j]);
                }
        }
    }
};
const EacSorted& Sorted() {
    static const EacSorted s;
    return s;
}

// Squared error of one (table, multiplier, base) choice, giving up once it reaches `limit`; fills idx[] (index into
// the table per texel).
int EacTry(const uint8_t a[16], int table, int mult, int base, int idx[16], int limit = 1 << 30) {
    const EacSorted& s = Sorted();
    int vals[8];
    for (int k = 0; k < 8; k++) vals[k] = ClampByte(base + s.mod[table][k] * mult);  // ascending
    int err = 0;
    for (int i = 0; i < 16; i++) {
        const int v2 = int(a[i]) * 2;
        int pos = v2 > vals[3] + vals[4] ? 4 : 0;
        pos += v2 > vals[pos + 1] + vals[pos + 2] ? 2 : 0;
        pos += v2 > vals[pos] + vals[pos + 1] ? 1 : 0;
        idx[i] = s.index[table][pos];
        const int d = int(a[i]) - vals[pos];
        err += d * d;
        if (err >= limit) return err;
    }
    return err;
}

void StoreEac(uint8_t* out, int base, int mult, int table, const int idx[16]) {
    out[0] = uint8_t(base);
    out[1] = uint8_t((mult << 4) | table);
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) bits |= uint64_t(idx[i] & 7) << (45 - 3 * i);
    for (int k = 0; k < 6; k++) out[2 + k] = uint8_t(bits >> (40 - 8 * k));
}

void EncodeEacBlock(uint8_t* out, const uint8_t a[16]) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) {
        lo = a[i] < lo ? a[i] : lo;
        hi = a[i] > hi ? a[i] : hi;
    }
    int idx[16];
    if (lo == hi) {
        for (int& v : idx) v = kEacFlatIndex;
        StoreEac(out, lo, 1, kEacFlatTable, idx);
        return;
    }
    const int range = hi - lo;
    // The rows are not sorted (indices 0-3 are the negative modifiers, 4-7 the positive ones): take min and max.
    int tmin[16], tmax[16];
    for (int t = 0; t < 16; t++) {
        tmin[t] = tmax[t] = kEacMods[t][0];
        for (int k = 1; k < 8; k++) {
            tmin[t] = kEacMods[t][k] < tmin[t] ? kEacMods[t][k] : tmin[t];
            tmax[t] = kEacMods[t][k] > tmax[t] ? kEacMods[t][k] : tmax[t];
        }
    }
    auto centre = [&](int t, int m) {
        const double c = (lo + hi) * 0.5 - m * (tmin[t] + tmax[t]) * 0.5;
        return ClampByte(int(c + 0.5));
    };
    // Search: every table, multipliers around the one whose span just covers the block's range, a few bases around the
    // centred one (candidates that cannot beat the best so far are dropped early), then a finer pass on the winner.
    int bt = 0, bm = 1, bb = lo, be = 1 << 30;
    for (int t = 0; t < 16; t++) {
        const int span = tmax[t] - tmin[t];
        const int m0 = (range + span - 1) / span;
        for (int m = m0; m <= m0 + 1; m++) {
            if (m < 1 || m > 15) continue;
            const int b0 = centre(t, m);
            for (int db = -3; db <= 3; db += 6) {
                const int b = ClampByte(b0 + db);
                const int e = EacTry(a, t, m, b, idx, be);
                if (e < be) {
                    be = e;
                    bt = t;
                    bm = m;
                    bb = b;
                }
            }
        }
    }
    for (int dm = -1; dm <= 1; dm++) {
        const int m = bm + dm;
        if (m < 1 || m > 15) continue;
        for (int db = -4; db <= 4; db++) {
            const int b = ClampByte(bb + db * (dm == 0 ? 1 : 2));
            const int e = EacTry(a, bt, m, b, idx, be);
            if (e < be) {
                be = e;
                bm = m;
                bb = b;
            }
        }
    }
    EacTry(a, bt, bm, bb, idx);
    StoreEac(out, bb, bm, bt, idx);
}

}  // namespace

bool IsEtcEncodable(int f) { return f == 65 || f == 66 || f == 69 || f == 70; }

bool EtcHasPunchThrough(int f) { return f == 66 || f == 70; }

bool IsEtcRgba8Encodable(int f) { return f == 68 || f == 72; }

size_t EtcRgba8LevelBytes(int width, int height, int depth) {
    const size_t bx = size_t((width + 3) / 4), by = size_t((height + 3) / 4);
    return bx * by * size_t(depth > 0 ? depth : 1) * 16;
}

void EncodeEtc2Rgba8(uint8_t* out, const uint8_t* rgba, int width, int height, int depth) {
    const int bxn = (width + 3) / 4, byn = (height + 3) / 4;
    for (int z = 0; z < depth; z++) {
        const uint8_t* slice = rgba + size_t(z) * size_t(width) * size_t(height) * 4;
        for (int by = 0; by < byn; by++)
            for (int bx = 0; bx < bxn; bx++) {
                Texel t[16];
                GatherBlock(slice, width, height, bx, by, false, t);
                uint8_t a[16];
                for (int x = 0; x < 4; x++)
                    for (int y = 0; y < 4; y++) {
                        const int sx = bx * 4 + x < width ? bx * 4 + x : width - 1;
                        const int sy = by * 4 + y < height ? by * 4 + y : height - 1;
                        a[x * 4 + y] = slice[(size_t(sy) * size_t(width) + size_t(sx)) * 4 + 3];
                    }
                uint8_t* dst = out + ((size_t(z) * byn + by) * bxn + bx) * 16;
                EncodeEacBlock(dst, a);
                StoreBigEndian(dst + 8, EncodeBlock(t, false));
            }
    }
}

void DecodeEacAlphaBlock(const uint8_t* b, uint8_t out[16]) {
    const int base = b[0], mult = b[1] >> 4, table = b[1] & 15;
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++) bits = (bits << 8) | b[2 + k];
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            const int i = x * 4 + y;
            const int k = int((bits >> (45 - 3 * i)) & 7);
            out[y * 4 + x] = uint8_t(ClampByte(base + kEacMods[table][k] * mult));
        }
}

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
    const bool diff = (v >> 33) & 1, flip = (v >> 32) & 1;
    const int tables[2] = {int((v >> 37) & 7), int((v >> 34) & 7)};
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
