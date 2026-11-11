#include "hle/nvn/etc_encode.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dsr::hle::nvn {
namespace {

// ETC1 intensity modifier tables, by pixel index (msb << 1 | lsb): 00 +small, 01 +large, 10 -small, 11 -large.
constexpr int kModifiers[8][4] = {
    {2, 8, -2, -8},       {5, 17, -5, -17},     {9, 29, -9, -29},     {13, 42, -13, -42},
    {18, 60, -18, -60},   {24, 80, -24, -80},   {33, 106, -33, -106}, {47, 183, -47, -183},
};

// How a subblock's pixels may be coded.
enum class Mode {
    Normal,     // RGB8 (individual or differential), or RGB8A1 with the opaque bit set
    NonOpaque,  // RGB8A1, opaque bit clear: index 2 = transparent, indices 0 and 2 have no modifier (0)
};

inline int Modifier(int table, int code, Mode mode) {
    if (mode == Mode::NonOpaque) return (code & 1) ? kModifiers[table][code] : 0;
    return kModifiers[table][code];
}

inline int ClampByte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// One pixel of a 4x4 block. Index i = x * 4 + y (column-major, as in the ETC bitstream).
struct Texel {
    int rgb[3];
    bool transparent;
};

void GatherBlock(const uint8_t* rgba, int w, int h, int bx, int by, bool punch, Texel t[16]) {
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            const int sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
            const uint8_t* p = rgba + (size_t(sy) * size_t(w) + size_t(sx)) * 4;
            Texel& e = t[x * 4 + y];
            e.rgb[0] = p[0];
            e.rgb[1] = p[1];
            e.rgb[2] = p[2];
            e.transparent = punch && p[3] < 128;
        }
}

// Subblock membership: flip 0 splits left/right halves (x), flip 1 splits top/bottom halves (y).
inline bool InSubblock(int i, int flip, int s) { return (flip == 0 ? (i / 4) / 2 : (i % 4) / 2) == s; }

inline int Expand4(int v) { return v * 17; }
inline int Expand5(int v) { return (v << 3) | (v >> 2); }

// Best table and per-pixel codes for one subblock with the given base colour; returns the squared error.
int SolveSubblock(const Texel t[16], int flip, int s, const int base[3], Mode mode, int codes[16], int* table_out) {
    // Only tables whose large modifier matches the subblock's colour range are worth trying.
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (!InSubblock(i, flip, s) || t[i].transparent) continue;
        for (int c = 0; c < 3; c++) {
            lo[c] = std::min(lo[c], t[i].rgb[c]);
            hi[c] = std::max(hi[c], t[i].rgb[c]);
        }
    }
    int range = 0;
    for (int c = 0; c < 3; c++) range = std::max(range, hi[c] - lo[c]);
    int first = 7;
    for (int k = 0; k < 8; k++)
        if (kModifiers[k][1] * 2 >= range) {
            first = k;
            break;
        }
    const int from = std::max(0, first - 1), to = std::min(7, first + 1);

    int best_err = std::numeric_limits<int>::max(), best_table = from;
    int best_codes[16] = {};
    for (int table = from; table <= to; table++) {
        int err = 0;
        int trial[16] = {};
        for (int i = 0; i < 16 && err < best_err; i++) {
            if (!InSubblock(i, flip, s)) continue;
            const Texel& e = t[i];
            if (e.transparent) {  // only in NonOpaque mode
                trial[i] = 2;
                continue;
            }
            int best_pix = std::numeric_limits<int>::max(), best_code = 0;
            for (int code = 0; code < 4; code++) {
                if (mode == Mode::NonOpaque && code == 2) continue;  // transparent
                int pe = 0;
                for (int c = 0; c < 3; c++) {
                    const int d = ClampByte(base[c] + Modifier(table, code, mode)) - e.rgb[c];
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

uint64_t PackCodes(uint64_t v, const int codes[16]) {
    for (int i = 0; i < 16; i++) {
        v |= uint64_t(codes[i] & 1) << i;
        v |= uint64_t(codes[i] >> 1) << (16 + i);
    }
    return v;
}

// One block as a 64-bit word. punch = the RGB8A1 format (differential mode only, bit 33 = opaque).
uint64_t EncodeBlock(const Texel t[16], bool punch) {
    bool any_transparent = false;
    for (int i = 0; i < 16; i++) any_transparent |= t[i].transparent;
    const Mode mode = any_transparent ? Mode::NonOpaque : Mode::Normal;

    uint64_t best_word = 0;
    int best_err = std::numeric_limits<int>::max();
    for (int flip = 0; flip < 2; flip++) {
        // Differential first: 5-bit bases. Plain RGB8 also tries individual mode (4-bit bases) when the two
        // subblocks' colours are too far apart for the 3-bit delta; RGB8A1 has no individual mode, so there the
        // second base is brought within reach of the first.
        for (int diff = 1; diff >= 0; diff--) {
            if (!diff && punch) break;
            int base_code[2][3], base_rgb[2][3];
            bool fits = true;
            for (int s = 0; s < 2; s++) {
                int avg[3];
                SubblockAverage(t, flip, s, avg);
                for (int c = 0; c < 3; c++)
                    base_code[s][c] = diff ? std::min(31, (avg[c] * 31 + 127) / 255) : std::min(15, (avg[c] + 8) / 17);
            }
            if (diff)
                for (int c = 0; c < 3; c++) {
                    const int d = base_code[1][c] - base_code[0][c];
                    if (d < -4 || d > 3) {
                        fits = false;
                        base_code[1][c] = base_code[0][c] + std::clamp(d, -4, 3);
                    }
                }
            if (diff && !fits && !punch) continue;  // RGB8: individual mode instead
            for (int s = 0; s < 2; s++)
                for (int c = 0; c < 3; c++)
                    base_rgb[s][c] = diff ? Expand5(base_code[s][c]) : Expand4(base_code[s][c]);

            int codes[16] = {}, tables[2] = {0, 0}, err = 0;
            for (int s = 0; s < 2; s++) err += SolveSubblock(t, flip, s, base_rgb[s], mode, codes, &tables[s]);
            if (err < best_err) {
                best_err = err;
                uint64_t v = 0;
                if (diff) {
                    for (int c = 0; c < 3; c++) {
                        v |= uint64_t(base_code[0][c]) << (59 - 8 * c);
                        v |= uint64_t((base_code[1][c] - base_code[0][c]) & 7) << (56 - 8 * c);
                    }
                    // Bit 33: "diff" in RGB8; "opaque" in RGB8A1 (clear when the block has transparent pixels).
                    if (!punch || mode == Mode::Normal) v |= uint64_t(1) << 33;
                } else {
                    for (int c = 0; c < 3; c++) {
                        v |= uint64_t(base_code[0][c]) << (60 - 8 * c);
                        v |= uint64_t(base_code[1][c]) << (56 - 8 * c);
                    }
                }
                if (flip) v |= uint64_t(1) << 32;
                v |= uint64_t(tables[0]) << 37;
                v |= uint64_t(tables[1]) << 34;
                best_word = PackCodes(v, codes);
            }
            if (diff && fits) break;  // differential fits: individual mode would only be coarser
        }
    }
    return best_word;
}

void StoreBigEndian(uint8_t* out, uint64_t v) {
    for (int k = 0; k < 8; k++) out[k] = uint8_t(v >> (56 - 8 * k));
}

// ---- EAC alpha (ETC2 RGBA8) -------------------------------------------------------------------------------------
// Modifier tables (8 per table). Block: byte 0 = base, byte 1 = multiplier (high nibble) | table (low nibble), then
// sixteen 3-bit indices (48 bits, big endian, texel order x * 4 + y).
constexpr int kEacMods[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},  {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};
// Table 13 has a zero modifier at index 4: a flat block (opaque textures) is stored exactly.
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
            for (int i = 1; i < 8; i++)
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

// Squared error of one (table, multiplier, base) choice, giving up once it reaches `limit`; fills idx[].
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
        lo = std::min(lo, int(a[i]));
        hi = std::max(hi, int(a[i]));
    }
    int idx[16];
    if (lo == hi) {
        for (int& v : idx) v = kEacFlatIndex;
        StoreEac(out, lo, 1, kEacFlatTable, idx);
        return;
    }
    const int range = hi - lo;
    // Every table, multipliers around the one whose span covers the range, bases around the centred one; then a
    // finer pass on the winner.
    int bt = 0, bm = 1, bb = lo, be = 1 << 30;
    for (int t = 0; t < 16; t++) {
        const int tmin = *std::min_element(kEacMods[t], kEacMods[t] + 8);
        const int tmax = *std::max_element(kEacMods[t], kEacMods[t] + 8);
        const int span = tmax - tmin;
        const int m0 = (range + span - 1) / span;
        for (int m = m0; m <= m0 + 1; m++) {
            if (m < 1 || m > 15) continue;
            const int b0 = ClampByte(int((lo + hi) * 0.5 - m * (tmin + tmax) * 0.5 + 0.5));
            for (int db = -2; db <= 2; db += 2) {
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

size_t EtcLevelBytes(int width, int height, int depth) {
    return size_t((width + 3) / 4) * size_t((height + 3) / 4) * size_t(std::max(1, depth)) * 8;
}
size_t EtcRgba8LevelBytes(int width, int height, int depth) { return EtcLevelBytes(width, height, depth) * 2; }

void EncodeEtc2Rgb(uint8_t* out, const uint8_t* rgba, int width, int height, int depth, bool punch_through) {
    const int bxn = (width + 3) / 4, byn = (height + 3) / 4;
    for (int z = 0; z < std::max(1, depth); z++) {
        const uint8_t* slice = rgba + size_t(z) * size_t(width) * size_t(height) * 4;
        for (int by = 0; by < byn; by++)
            for (int bx = 0; bx < bxn; bx++) {
                Texel t[16];
                GatherBlock(slice, width, height, bx, by, punch_through, t);
                StoreBigEndian(out + ((size_t(z) * byn + by) * bxn + bx) * 8, EncodeBlock(t, punch_through));
            }
    }
}

void EncodeEtc2Rgba8(uint8_t* out, const uint8_t* rgba, int width, int height, int depth) {
    const int bxn = (width + 3) / 4, byn = (height + 3) / 4;
    for (int z = 0; z < std::max(1, depth); z++) {
        const uint8_t* slice = rgba + size_t(z) * size_t(width) * size_t(height) * 4;
        for (int by = 0; by < byn; by++)
            for (int bx = 0; bx < bxn; bx++) {
                Texel t[16];
                GatherBlock(slice, width, height, bx, by, false, t);
                uint8_t a[16];
                for (int x = 0; x < 4; x++)
                    for (int y = 0; y < 4; y++) {
                        const int sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
                        a[x * 4 + y] = slice[(size_t(sy) * size_t(width) + size_t(sx)) * 4 + 3];
                    }
                uint8_t* dst = out + ((size_t(z) * byn + by) * bxn + bx) * 16;
                EncodeEacBlock(dst, a);
                StoreBigEndian(dst + 8, EncodeBlock(t, false));
            }
    }
}

// ---- decoders (tests) -------------------------------------------------------------------------------------------
void DecodeEtc2RgbBlock(const uint8_t* block, bool punch_through, uint8_t out[16][4]) {
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v = (v << 8) | block[k];
    const bool bit33 = (v >> 33) & 1, flip = (v >> 32) & 1;
    const bool diff = punch_through || bit33;  // RGB8A1 has no individual mode
    const bool opaque = !punch_through || bit33;
    const int tables[2] = {int((v >> 37) & 7), int((v >> 34) & 7)};
    int base[2][3];
    for (int c = 0; c < 3; c++) {
        if (diff) {
            const int b0 = int((v >> (59 - 8 * c)) & 31), dd = int((v >> (56 - 8 * c)) & 7);
            const int b1 = b0 + (dd >= 4 ? dd - 8 : dd);
            if (b1 < 0 || b1 > 31) {  // T, H or planar mode: never written by the encoder
                for (int i = 0; i < 16; i++) out[i][0] = 255, out[i][1] = 0, out[i][2] = 255, out[i][3] = 255;
                return;
            }
            base[0][c] = Expand5(b0);
            base[1][c] = Expand5(b1);
        } else {
            base[0][c] = Expand4(int((v >> (60 - 8 * c)) & 15));
            base[1][c] = Expand4(int((v >> (56 - 8 * c)) & 15));
        }
    }
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            const int i = x * 4 + y, s = flip ? y / 2 : x / 2;
            const int code = int(((v >> (16 + i)) & 1) << 1 | ((v >> i) & 1));
            uint8_t* o = out[y * 4 + x];
            if (!opaque && code == 2) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            const Mode mode = opaque ? Mode::Normal : Mode::NonOpaque;
            for (int c = 0; c < 3; c++) o[c] = uint8_t(ClampByte(base[s][c] + Modifier(tables[s], code, mode)));
            o[3] = 255;
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
            out[y * 4 + x] = uint8_t(ClampByte(base + kEacMods[table][k] * (mult ? mult : 1)));
        }
}

}  // namespace dsr::hle::nvn
