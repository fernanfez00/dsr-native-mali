// Hooks on specific functions inside the game's main module, for the game builds listed in kBuilds (matched by
// build id): 1.0.0 and the 1.0.3 update (two editions of it: title 010088F00820E000's and 01004AB00A260000's, the same
// version with different programs). They replace engine functions whose output is useful to us but goes
// nowhere in a retail build, and patch the frame rate and resolution. The places were found in 1.0.0 and matched
// in 1.0.3 by their code (the same instructions around them, with addresses masked).
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <mutex>
#include <map>

#include "hle/input.h"
#include "common/log.h"
#include "common/profiler.h"
#include "hle/os/heap.h"
#include "hle/os/os.h"
#include <atomic>

#include "runtime/runtime.h"

namespace dsr {
namespace {
float* g_game_step = nullptr;  // the game's per-frame time step (scratch page), when patched

// Where things are in one game build (offsets in main).
struct GameBuild {
    const char* build_id;  // first 16 bytes, hex
    const char* version;
    const char* data_version;  // DataVersion.txt of the RomFS that goes with it
    // Per-frame update: adrp x8 / ldr s8, [x8, #...] loading the constant 1/30 s time step.
    uint64_t step_adrp;
    uint32_t step_adrp_insn, step_ldr_insn;
    // Render thread frame limiter (this, fps) and its state in `this`.
    uint64_t limiter;
    uint64_t limiter_global;  // 1.0.0: skipped while this int (main+) is non-zero; 0 = see limiter_enabled
    uint32_t limiter_enabled;  // 1.0.3: skipped while this byte of `this` is zero
    uint32_t last_tick, skip_active, skip_toggle, skip_time;
    // Resolution defaults (mov w2/w1, #1280 / #720).
    uint64_t width_cfg, width_get, display_width, height_cfg, height_get, display_height;
    // Engine log, log with va_list, DL_PANIC.
    uint64_t log, vlog, panic;
    // Game frame rate while a video plays. The movie player is frame-based: 1.0.0 shows one video frame per game
    // frame (30 fps videos: 30), 1.0.3 one per two game frames (60; at 30 its videos ran at half speed).
    int movie_fps;
    // Performance mode (settings performance_mode): the engine's own quality variables, from its debug menu. (The
    // LOD level bias, 1 on the Switch, is left alone: at 2 nearby walls switched to low-detail models with gaps.)
    //   lowvfx_read:   ldrb w10, [x20, #0x599] reading "Enable Low Quality Vfx" (0) into the effect setup: becomes
    //                  mov w10, #1 (effects through the engine's low-quality resolve).
    //   shadow_size:   the shadow map size global (debug menu "ShadwoMap Size", 512..2048, 2048): set to 1024
    //                  before the game starts; the engine sizes the shadow target and its texel offsets from it.
    uint64_t lowvfx_read, shadow_size;
    // The per-frame dynamic buffer (DLDrawDevice_NXNvn: three, one per frame in flight): its size is
    // `add w8, w8, #0x200, lsl #12` (2 MiB, plus a config value that is 0). A frame that needs more aborts the game
    // ("DL_PANIC: dynamic buffer not big enough"), as in a busy boss fight on a wide screen: made 8 MiB.
    uint64_t dyn_buffer;
    // Lighter post-processing (settings lighter_post_fx): the engine's own switches, from its debug menu, read by
    // the per-frame setup of the renderer (`ldrb` from the graphics settings object, x19):
    //   ao[2]:       "Enable AO" (+0x2ca): the ambient-occlusion switch and the render parameter it sets -> 0
    //   dof:         "Enable DOF" (+0x2e1): passed (inverted) to the image-process manager -> 0 (no depth of field)
    //   camblur[2]:  "Disable Camera Blur" (+0x295), on both paths to the camera motion blur -> 1
    //   lightshaft:  the "LightShaft" draw-group switch (a global, 1) where the pass mask is built -> 0
    uint64_t ao[2], dof, camblur[2], lightshaft;
};
const GameBuild kBuilds[] = {
    {"5a797a3a5da6c4f3468cb7c84e4dde63", "1.0.0", "20180818_462",
     0x66c9c8, 0xB00080A8, 0xBD497108,
     0x70bd90, 0x251fb4c, 0, 0x310, 0x31d, 0x31e, 0x320,
     0x6a170c, 0x6a18f8, 0x716170, 0x6a1758, 0x6a1968, 0x716178,
     0x7fea18, 0x7feb30, 0x7892d8, 30,
     0x3620f4, 0x1aeb890, 0x70b498,
     {0x370014, 0x370088}, 0x370584, {0x36ea4c, 0x36ea5c}, 0x3623e0},
    {"6569410c91c398e78ec76b418ee4cca8", "1.0.3", "20181026_101",
     0x667930, 0xB0008068, 0xBD455908,
     0x706d28, 0, 0x2f7, 0x2e8, 0x2f5, 0x2f6, 0x2f8,
     0x69c638, 0x69c824, 0x711ff0, 0x69c684, 0x69c894, 0x711ff8,
     0x7f71d4, 0x7f72ec, 0x782dac, 60,
     0x356790, 0x1adf890, 0x7065f4,
     {0x364494, 0x364508}, 0x364a04, {0x362f3c, 0x362f4c}, 0x356a7c},
    // 1.0.3 of title 01004AB00A260000 (update v196608, buildinfo region DV): the same game and data as the other
    // 1.0.3 with its code moved; matched from it, and the limiter's fields are the same.
    {"df3766a2bb651a3e822828abf10b6f7b", "1.0.3", "20181026_101",
     0x667900, 0xF0008088, 0xBD46ED08,
     0x707090, 0, 0x2f7, 0x2e8, 0x2f5, 0x2f6, 0x2f8,
     0x69c608, 0x69c7f4, 0x711ec8, 0x69c654, 0x69c864, 0x711ed0,
     0x7f9ec8, 0x7f9fe0, 0x784660, 60,
     0x356790, 0x1ae3890, 0x7067ac,
     {0x364494, 0x364508}, 0x364a04, {0x362f3c, 0x362f4c}, 0x356a7c},
};
const GameBuild* g_build = nullptr;

// Skip intro logos (settings skip_intro). The title screen's step machine (TitleStep / TitleStep_forNS, step
// number at this+0xc) opens the company logo dialog (menu dialog 60, FrpgMenuDlgLogo) by setting its request word
// to 1 (`bl` to set(60, 1)) and then waits until the dialog has cleared it again after its last logo. Both classes'
// open calls become `nop`, so the wait passes at once, as if the logos had finished. TitleStep_forNS also counts
// down a timer (this+0xe0) before closing the dialog: that step branches straight to its close path. Everything
// else (loading, the title movie, press start) runs as before.
struct LogoSkip {
    const char* build_id;  // first hex digits of the build (GameBuild::build_id)
    uint64_t open[2];
    uint32_t open_insn[2];  // bl set_dialog_request (60, 1)
    uint64_t timer;         // ldr s1, [x19, #0xe0]
    uint64_t timer_done;    // the timer-expired path
};
const LogoSkip kLogoSkips[] = {
    {"5a797a3a", {0x6f1c30, 0x6f73e8}, {0x97fa3d6c, 0x97fa277e}, 0x6f7450, 0x6f746c},
    {"6569410c", {0x6ed0e4, 0x6f27fc}, {0x97fa3510, 0x97fa1f4a}, 0x6f2864, 0x6f2880},
    {"df3766a2", {0x6ecf7c, 0x6f268c}, {0x97fa355e, 0x97fa1f9a}, 0x6f26f4, 0x6f2710},
};
constexpr uint32_t kNop = 0xD503201F, kLdrS1Timer = 0xBD40E261;

// Sprint "graze" check (frame-rate fix). Every update the game measures how far the player's character moved since
// the previous update and, if it was less than 1/30 of a unit (`fmov #30`, multiply, compare with 1: below
// 1 unit per second at the game's own 30 updates a second), takes it to be running into a wall and slows the
// movement down: a speed factor (move control +0x1a8, 0.5..1) is multiplied by 0.8 per update, and by 1.2 per update
// while moving again. The test is per update, so at a higher frame rate an update covers less ground: at 60 fps the
// threshold is 2 units per second, at 120 fps 4, which a sprint (once slowed down: for good) no longer reaches -
// the character then sprints at half speed, slower than walking. (Found from the high frame rate fix of
// DS1 FPS Unlock for the PC version, MIT licence, github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered: "FixGraze".)
// The three constants now come from the scratch page and are set every frame from the game's time step (by
// FrameLimiter): 1/step, 0.8^(30 step), 1.2^(30 step): the same test and the same slow-down / recovery per second
// at every frame rate (at 30 fps exactly the game's).
struct GrazeFix {
    const char* build_id;
    // 1.0.3 (optimised code): fmov s0,#30 / fmul s0,s0,v1.s[0] / fmov s1,wzr / tst w8,#1 / add x8,x19,#0x1a8 at
    // `mul`, the result stored by str s0,[x8] at `store`. Rewritten as adrp x9,scratch / ldr s0,[x9,#4] / fmul /
    // fmov / tst, the store as str s0,[x19,#0x1a8] (x8 is only its address, x9 is loaded again before it is used).
    // 1.0.0 (unoptimised): fmov s1,#30 / fmul s0,s0,s1 / stur s0,[x29,#-0x18] / ldur s0,[x29,#-0x18] at `mul`:
    // adrp x9,scratch / ldr s1,[x9,#4] / fmul / stur (the reload of the value just stored is dropped).
    bool optimised;
    uint64_t mul, store;
    uint64_t decay, recover;  // adrp x9,page / ldr s1,[x9,#lo] loading 0.8 and 1.2: now the scratch page's
    uint32_t decay_insn[2], recover_insn[2];
};
const GrazeFix kGrazeFixes[] = {
    {"5a797a3a", false, 0x2a3be8, 0, 0x2a3c48, 0x2a3c90, {0xD0009EE9, 0xBD48CD21}, {0xD0009EE9, 0xBD495521}},
    {"6569410c", true, 0x29c874, 0x29c8d4, 0x29c898, 0x29c8bc, {0x90009EC9, 0xBD44B521}, {0x90009EC9, 0xBD453D21}},
    {"df3766a2", true, 0x29c874, 0x29c8d4, 0x29c898, 0x29c8bc, {0xD0009EE9, 0xBD464921}, {0xD0009EE9, 0xBD46D121}},
};
float* g_graze = nullptr;  // scratch page: [0] 1/step, [1] slow-down, [2] recovery per update

void SetGrazeFactors(float step) {
    if (!g_graze || !(step > 0.0f)) return;
    g_graze[0] = 1.0f / step;
    g_graze[1] = std::pow(0.8f, 30.0f * step);
    g_graze[2] = std::pow(1.2f, 30.0f * step);
}

void ApplyGrazeFix(Runtime& rt, float* graze) {
    const GrazeFix* f = nullptr;
    for (const GrazeFix& g : kGrazeFixes)
        if (rt.main_nso.build_id_hex.rfind(g.build_id, 0) == 0) f = &g;
    if (!f) return;
    const uintptr_t page = reinterpret_cast<uintptr_t>(graze);
    const uint32_t off = uint32_t(page & 0xfff);  // 4 (after the time step)
    auto ldr_s = [&](int rt_, uint32_t byte_off) { return 0xBD400000u | ((byte_off / 4) << 10) | (9u << 5) | uint32_t(rt_); };
    struct P { uint64_t at; uint32_t from, to; };
    std::vector<P> p;
    if (f->optimised) {
        p = {{f->mul, 0x1E27D000, rt.main.EncodeAdrp(f->mul, 9, page)},
             {f->mul + 4, 0x5F819000, ldr_s(0, off)},
             {f->mul + 8, 0x1E2703E1, 0x5F819000},
             {f->mul + 12, 0x7200011F, 0x1E2703E1},
             {f->mul + 16, 0x9106A268, 0x7200011F},
             {f->store, 0xBD000100, 0xBD01AA60}};  // str s0,[x8] -> str s0,[x19,#0x1a8]
    } else {
        p = {{f->mul, 0x1E27D001, rt.main.EncodeAdrp(f->mul, 9, page)},
             {f->mul + 4, 0x1E210800, ldr_s(1, off)},
             {f->mul + 8, 0xBC1E83A0, 0x1E210800},
             {f->mul + 12, 0xBC5E83A0, 0xBC1E83A0}};
    }
    p.push_back({f->decay, f->decay_insn[0], rt.main.EncodeAdrp(f->decay, 9, page)});
    p.push_back({f->decay + 4, f->decay_insn[1], ldr_s(1, off + 4)});
    p.push_back({f->recover, f->recover_insn[0], rt.main.EncodeAdrp(f->recover, 9, page)});
    p.push_back({f->recover + 4, f->recover_insn[1], ldr_s(1, off + 8)});
    // All checked before anything changes.
    for (const P& x : p) {
        const uint32_t* at = reinterpret_cast<const uint32_t*>(rt.main.base() + x.at);
        if (*at != x.from) {
            LOGW("hooks", "sprint frame-rate fix not applied: main+0x%lx is %08x, expected %08x", (unsigned long)x.at,
                 *at, x.from);
            return;
        }
    }
    graze[0] = 30.0f;
    graze[1] = 0.8f;
    graze[2] = 1.2f;
    std::string err;
    size_t done = 0;
    for (const P& x : p) {
        if (!rt.main.PatchInstruction(x.at, x.from, x.to, &err)) break;
        ++done;
    }
    if (done != p.size()) {
        for (size_t i = done; i-- > 0;) rt.main.PatchInstruction(p[i].at, p[i].to, p[i].from, &err);
        LOGW("hooks", "sprint frame-rate fix not applied (%s)", err.c_str());
        return;
    }
    g_graze = graze;
    LOGI("hooks", "sprint frame-rate fix: the wall check and its slow-down follow the time step");
}

// Performance mode: low-quality effects, 1024x1024 shadow maps (see GameBuild); the menu also sets render scale 0.75.
// Each part is checked before it is changed and is skipped (logged) if the game's code or data is not as expected.
void ApplyPerformanceMode(Runtime& rt, const GameBuild& b) {
    constexpr uint32_t kLdrbLowVfx = 0x3956668A;    // ldrb w10, [x20, #0x599]
    constexpr uint32_t kMovLowVfxOn = 0x5280002A;   // mov w10, #1
    std::string err;
    std::string done;
    if (rt.main.PatchInstruction(b.lowvfx_read, kLdrbLowVfx, kMovLowVfxOn, &err))
        done += done.empty() ? "low-quality effects" : ", low-quality effects";
    else
        LOGW("hooks", "performance mode: effects quality not changed (%s)", err.c_str());
    if (rt.config.shadow_quality == 3) {  // shadow_quality 0-2 set their own size (ApplyShadowQuality)
        int32_t* shadow = reinterpret_cast<int32_t*>(rt.main.base() + b.shadow_size);
        if (rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) && *shadow == 2048) {
            *shadow = 1024;
            done += done.empty() ? "shadow maps 1024" : ", shadow maps 1024";
        } else {
            LOGW("hooks", "performance mode: shadow map size not changed (found %d)",
                 rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) ? *shadow : -1);
        }
    }
    LOGI("hooks", "performance mode: %s", done.empty() ? "nothing applied" : done.c_str());
}

// Swipe camera (touch_camera_mode = swipe): the thumb's speed becomes the right stick's tilt, which may go past full
// tilt for fast swipes. The game caps and reshapes the right stick in five places; each is lifted only while the swipe
// camera is in use (the touch right stick is non-zero in that mode), so controllers and the touch stick are unchanged.
//
// 1. The input mapping (df37 main+0x830400) reads each pad channel, applies a dead zone and a scale and clamps it to
//    +-max; the parameters of entry i come from this getter (28-byte entries: +0 channel, +4 paired channel or -1,
//    +8 dead zone, +12 threshold, +16 scale, +20 max, +24 flag). A max of 1 is raised to 8: only the swipe camera sends
//    more than full tilt (controllers and the touch sticks stop at 1).
struct MapEntryHook {
    const char* build_id;
    uint64_t getter;
};
const MapEntryHook kMapEntryHooks[] = {
    {"5a797a3a", 0x821690},
    {"6569410c", 0x819d28},
    {"df3766a2", 0x81ccc0},
};
void InputMapEntry(const uint8_t* self, int i, int32_t* channel, float* dead_zone, int32_t* pair, float* threshold,
                   float* scale, float* max, uint8_t* flag) {
    const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(self + 8) + int64_t(i) * 0x1c;
    memcpy(channel, e + 0, 4);
    memcpy(pair, e + 4, 4);
    memcpy(dead_zone, e + 8, 4);
    memcpy(threshold, e + 12, 4);
    memcpy(scale, e + 16, 4);
    memcpy(max, e + 20, 4);
    *flag = e[24];
    if (*max >= 0.99f && *max <= 1.01f) *max = 8.0f;
}

// 2. The action mapping (df37 main+0x81c848, used by main+0x82ffa8): each action's frame value gets a dead zone and a
//    scale and is clamped to +-max per axis (28-byte entries: +0 action, +4 paired action, +8, +12, +16 scale, +20 max,
//    +24 dead zone; max goes out through the stack argument). A max of 1 goes to 8 while swiping.
struct ActionMapHook {
    const char* build_id;
    uint64_t fn;
};
const ActionMapHook kActionMapHooks[] = {
    {"5a797a3a", 0x821218},
    {"6569410c", 0x8199d0},
    {"df3766a2", 0x81c848},
};
void ActionMapEntry(const uint8_t* self, int i, uint32_t* action, uint32_t* pair, uint32_t* f8, uint32_t* f12,
                    uint32_t* f24, float* scale, float* max) {
    const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(self + 8) + int64_t(i) * 0x1c;
    memcpy(action, e + 0, 4);
    memcpy(pair, e + 4, 4);
    memcpy(f8, e + 8, 4);
    memcpy(f12, e + 12, 4);
    memcpy(scale, e + 16, 4);
    memcpy(max, e + 20, 4);
    memcpy(f24, e + 24, 4);
    if (*max >= 0.99f && *max <= 1.01f && dsr::input::TouchSwipeActive()) *max = 8.0f;
}

// 3. The stick vector (df37 main+0x694d20): actions (x, y) from the published array, a radial dead zone of 0.37, then
//    length = clamp((len - 0.37) * 1.724, 0, 1) along the stick's direction, returned as two floats in x0. Replaced
//    by the same code without the clamp to 1 while swiping (the camera reads actions 18 / 19 through it).
struct StickVecHook {
    const char* build_id;
    uint64_t fn;
};
const StickVecHook kStickVecHooks[] = {
    {"5a797a3a", 0x69a058},
    {"6569410c", 0x694d50},
    {"df3766a2", 0x694d20},
};

// 4. The camera's yaw (df37 main+0xab8c4, in the camera update): past a small horizontal threshold the camera turns at
//    a speed that ramps from its slowest to its fastest over a set time (+312 counts it up, +480 the time), in the
//    input's direction only - how far the stick is pushed does not count; below it the camera turns by input times
//    the slowest speed. While swiping the branch into the ramp becomes a jump to the proportional turn, so the camera
//    turns by exactly what the swipe sends.
// 5. The camera's follow: the angles are worked out again from the camera's position to its target - every frame
//    (df37 main+0xab3f8) and when the character has moved (main+0xab780, cbz w19) - so the camera turns after a
//    moving character (with a stick too: the game's own behaviour). While swiping both are jumped over, so the camera
//    keeps the angle the thumb gave it and only moves along with the character.
// Code patches, written when the swipe camera starts and restored when the thumb lifts.
struct RampPatch {
    const char* build_id;
    uint64_t at;
    uint32_t orig, repl;  // b.le <proportional> / b <proportional>
    uint64_t follow_at;
    uint32_t follow_orig, follow_repl;  // cbz w19, <past> / b <past>
    uint64_t follow2_at;  // ldr q1, [x20, #208] -> b <past the pitch and yaw stores> (+44)
};
constexpr uint32_t kFollow2Orig = 0x3DC03681, kFollow2Repl = 0x1400000B;
const RampPatch kRampPatches[] = {
    {"5a797a3a", 0xae554, 0x5400020D, 0x14000010, 0xae41c, 0x340001B3, 0x1400000D, 0xae094},
    {"6569410c", 0xab8c8, 0x5400022D, 0x14000011, 0xab780, 0x340001D3, 0x1400000E, 0xab3f8},
    {"df3766a2", 0xab8c8, 0x5400022D, 0x14000011, 0xab780, 0x340001D3, 0x1400000E, 0xab3f8},
};
bool g_follow_ok = false, g_follow2_ok = false;
loader::Module* g_ramp_module = nullptr;
const RampPatch* g_ramp = nullptr;
std::atomic<bool> g_ramp_off{false};
void SetCameraProportional(bool on) {
    if (!g_ramp || g_ramp_off.load(std::memory_order_relaxed) == on) return;
    g_ramp_off = on;
    const uint32_t w = on ? g_ramp->repl : g_ramp->orig;
    if (!g_ramp_module->WriteReadOnly(g_ramp->at, &w, 4)) LOGW("hooks", "swipe camera: camera ramp patch failed");
    const uint32_t f = on ? g_ramp->follow_repl : g_ramp->follow_orig;
    if (g_follow_ok && !g_ramp_module->WriteReadOnly(g_ramp->follow_at, &f, 4))
        LOGW("hooks", "swipe camera: camera follow patch failed");
    const uint32_t f2 = on ? kFollow2Repl : kFollow2Orig;
    if (g_follow2_ok && !g_ramp_module->WriteReadOnly(g_ramp->follow2_at, &f2, 4))
        LOGW("hooks", "swipe camera: camera follow patch 2 failed");
}

uint64_t StickVector(const uint8_t* self, int ix, int iy) {
    const float* a = *reinterpret_cast<const float* const*>(*reinterpret_cast<const uint8_t* const*>(self + 8) + 32);
    const float x = a[ix], y = a[iy];
    const float len = std::sqrt(x * x + y * y);
    float rx = 0.0f, ry = 0.0f;
    if (len > 0.37f && std::isfinite(len)) {
        float m = std::max(0.0f, (len - 0.37f) * 1.7241380214691162f);
        if (m > 1.0f && !dsr::input::TouchSwipeActive()) m = 1.0f;
        rx = x / len * m;
        ry = y / len * m;
    }
    if (ix == 18) SetCameraProportional(dsr::input::TouchSwipeActive());  // the camera's actions, once per frame
    uint32_t bx, by;
    memcpy(&bx, &rx, 4);
    memcpy(&by, &ry, 4);
    return uint64_t(bx) | (uint64_t(by) << 32);
}

static void InstallSwipeCamera(Runtime& rt, const GameBuild& b) {
    for (const RampPatch& r : kRampPatches) {
        if (strncmp(r.build_id, b.build_id, strlen(r.build_id))) continue;
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + r.at);
        // fcmp s11, s0 / b.le / ldr s0, [x20, #312]
        if (code[-1] != 0x1E202160 || code[0] != r.orig || code[1] != 0xBD413A80) {
            LOGW("hooks", "swipe camera: camera ramp not found at main+0x%lx", (unsigned long)r.at);
            continue;
        }
        g_ramp_module = &rt.main;
        g_ramp = &r;
        // cbz w19 / ldr q0, [x20, #208] / ldr q1, [x20, #256]
        const uint32_t* fc = reinterpret_cast<const uint32_t*>(rt.main.base() + r.follow_at);
        g_follow_ok = fc[0] == r.follow_orig && fc[1] == 0x3DC03680 && fc[2] == 0x3DC04281;
        // ldr q1, [x20, #208] / fsub v0.4s, v1.4s, v0.4s / stur q0, [x29, #-192] / ldr s0, [x20, #320] ... str s0, [x20, #324]
        const uint32_t* f2 = reinterpret_cast<const uint32_t*>(rt.main.base() + r.follow2_at);
        g_follow2_ok = f2[0] == kFollow2Orig && f2[1] == 0x4EA0D420 && f2[3] == 0xBD414280 && f2[10] == 0xBD014680;
        if (!g_follow_ok || !g_follow2_ok)
            LOGW("hooks", "swipe camera: camera follow not found (main+0x%lx %s, main+0x%lx %s)",
                 (unsigned long)r.follow_at, g_follow_ok ? "ok" : "missing", (unsigned long)r.follow2_at,
                 g_follow2_ok ? "ok" : "missing");
    }

    int hooked = 0;
    for (const StickVecHook& h : kStickVecHooks) {
        if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
        // ldr x8,[x0,#8] / ldr x8,[x8,#0x20] within the first 8 instructions
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.fn);
        bool ok = false;
        for (int k = 0; k < 8; k++) ok |= code[k] == 0xF9400408 && code[k + 1] == 0xF9401108;
        std::string err;
        if (!ok)
            LOGW("hooks", "swipe camera: stick vector not found at main+0x%lx", (unsigned long)h.fn);
        else if (rt.main.HookFunction(h.fn, reinterpret_cast<void*>(&StickVector), &err))
            hooked++;
        else
            LOGW("hooks", "swipe camera: stick vector hook failed: %s", err.c_str());
    }

    for (const MapEntryHook& h : kMapEntryHooks) {
        if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
        // ldr x9,[x0,#8] / orr w10,wzr,#0x1c / smull x11,w1,w10 / ldr w9,[x9,x11]
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.getter);
        std::string err;
        if (code[0] != 0xF9400409 || code[1] != 0x321E0BEA || code[2] != 0x9B2A7C2B)
            LOGW("hooks", "swipe camera: input mapping getter not found at main+0x%lx", (unsigned long)h.getter);
        else if (rt.main.HookFunction(h.getter, reinterpret_cast<void*>(&InputMapEntry), &err))
            hooked++;
        else
            LOGW("hooks", "swipe camera: input mapping hook failed: %s", err.c_str());
    }

    for (const ActionMapHook& h : kActionMapHooks) {
        if (strncmp(h.build_id, b.build_id, strlen(h.build_id))) continue;
        // ldr x9,[x0,#8] / orr w10,wzr,#0x1c / smull x11,w1,w10 / ... ldr x8,[sp] (+0x5c) ... ret
        const uint32_t* code = reinterpret_cast<const uint32_t*>(rt.main.base() + h.fn);
        std::string err;
        if (code[0] != 0xF9400409 || code[1] != 0x321E0BEA || code[2] != 0x9B2A7C2B || code[23] != 0xF94003E8 ||
            code[30] != 0xD65F03C0)
            LOGW("hooks", "swipe camera: action mapping getter not found at main+0x%lx", (unsigned long)h.fn);
        else if (rt.main.HookFunction(h.fn, reinterpret_cast<void*>(&ActionMapEntry), &err))
            hooked++;
        else
            LOGW("hooks", "swipe camera: action mapping hook failed: %s", err.c_str());
    }
    if (g_ramp && hooked == 3)
        LOGI("hooks", "swipe camera: fast swipes turn past the stick's top speed, no turn ramp or follow while swiping");
}

// The engine's screen size block (1.0.0 main+0x1aeba90, 0x200 past GameBuild::shadow_size in every build):
// int width, height; float width, height; int half width, half height; float half width, half height, with
// the defaults 1280x720 / 640x360. The renderer fills it from the resolution getters when it starts
// (main+0x101d80), but a static initializer (main+0x3751c4) copies the half size into two globals before that,
// and the projected-decal pass sizes its colour target (main+0x3611c8) and its 1/size constants (main+0x36e9b4)
// from those copies. Below a 1280x720 render (render scale, performance mode) the target stayed 640x360 beside
// the game's own half-size depth (e.g. 484x272): decals were drawn into its top-left part only and showed in a
// rectangle at the top left of the screen. The defaults are set to the render size before the game starts.
static void ApplyScreenSizeDefaults(Runtime& rt, const GameBuild& b, int width, int height) {
    struct ScreenSize {
        int32_t w, h;
        float fw, fh;
        int32_t hw, hh;
        float fhw, fhh;
    };
    auto* s = reinterpret_cast<ScreenSize*>(rt.main.base() + b.shadow_size + 0x200);
    if (!rt.main.Contains(reinterpret_cast<uintptr_t>(s)) ||
        !rt.main.Contains(reinterpret_cast<uintptr_t>(s) + sizeof(ScreenSize) - 1) || s->w != 1280 || s->h != 720 ||
        s->fw != 1280.0f || s->fh != 720.0f || s->hw != 640 || s->hh != 360 || s->fhw != 640.0f || s->fhh != 360.0f) {
        LOGW("hooks", "screen size defaults not found: decals may show in part of the screen below 1280x720");
        return;
    }
    const int hw = width / 2, hh = height / 2;  // as the engine computes them (float * 0.5, truncated)
    *s = {width, height, float(width), float(height), hw, hh, float(hw), float(hh)};
    LOGI("hooks", "screen size defaults: %dx%d, half %dx%d (decal target)", width, height, hw, hh);
}

// Shadow quality (settings shadow_quality 0-2; from pull request #47): the shadow map size global the engine sizes its
// shadow target from (see GameBuild::shadow_size) at 512 (0 and 1) or 1024 (2). With 0 the renderer also leaves the
// shadow pass out and answers every lookup "lit" (vk_backend.cpp), so the small map only saves memory.
void ApplyShadowQuality(Runtime& rt, const GameBuild& b) {
    const int q = rt.config.shadow_quality;
    if (q >= 3) return;
    const int size = q == 2 ? 1024 : 512;
    int32_t* shadow = reinterpret_cast<int32_t*>(rt.main.base() + b.shadow_size);
    if (rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) && *shadow == 2048) {
        *shadow = size;
        LOGI("hooks", "shadow quality %d: shadow maps %dx%d%s", q, size, size, q == 0 ? ", shadows off" : "");
    } else {
        LOGW("hooks", "shadow quality: shadow map size not changed (found %d)%s",
             rt.main.Contains(reinterpret_cast<uintptr_t>(shadow)) ? *shadow : -1, q == 0 ? "; shadows still off" : "");
    }
}

// Lighter post-processing: no ambient occlusion, depth of field, camera motion blur or light shafts (see GameBuild).
// All or nothing: every instruction is checked first.
void ApplyLighterPostFx(Runtime& rt, const GameBuild& b) {
    struct P {
        uint64_t off;
        uint32_t from, to;
    };
    const P patches[] = {
        {b.ao[0], 0x394B2A60, 0x52800000},       // ldrb w0, [x19, #0x2ca]  -> mov w0, #0
        {b.ao[1], 0x394B2A77, 0x52800017},       // ldrb w23, [x19, #0x2ca] -> mov w23, #0
        {b.dof, 0x394B8668, 0x52800008},         // ldrb w8, [x19, #0x2e1]  -> mov w8, #0
        {b.camblur[0], 0x394A5668, 0x52800028},  // ldrb w8, [x19, #0x295]  -> mov w8, #1
        {b.camblur[1], 0x394A5668, 0x52800028},
        {b.lightshaft, 0x39400108, 0x52800008},  // ldrb w8, [x8]           -> mov w8, #0
    };
    for (const P& p : patches) {
        const uint32_t* at = reinterpret_cast<const uint32_t*>(rt.main.base() + p.off);
        if (!rt.main.Contains(reinterpret_cast<uintptr_t>(at)) || *at != p.from) {
            LOGW("hooks", "lighter post-processing not applied: main+0x%llx is not as expected",
                 (unsigned long long)p.off);
            return;
        }
    }
    std::string err;
    for (const P& p : patches)
        if (!rt.main.PatchInstruction(p.off, p.from, p.to, &err)) {
            LOGW("hooks", "lighter post-processing: %s", err.c_str());
            return;
        }
    LOGI("hooks", "lighter post-processing: no ambient occlusion, depth of field, camera motion blur or light shafts "
                  "(bloom: not drawn by the renderer)");
}

// Effect cluster overflow (a crash in the game itself). The render tasks draw a particle effect cluster
// (FrpgFxClusterAppearance_Model.cpp) by writing each of its pieces into per-thread arrays with room for 384 (16, 4,
// 4 and 64 bytes a piece at buffer+0x210, +0x1a10, +0x2010, +0x2610). The lock guarding the buffer lies right after
// the last 64-byte slot (buffer+0x8618; 1.0.0 +0x8608). With more pieces the copy goes on past the end: piece 385's
// matrix overwrites the lock, and unlocking it at the end of the function calls through that data (SIGSEGV on MTR_n,
// pc main+0x624868 in DV 1.0.3; seen during a boss attack, with 390 pieces). The game does check the count, but only
// to report "error !" (nothing in a release build), and 1.0.3 checks it against a limit at buffer+0x200 that is
// larger than 384 (it let 390 through). The count is now clamped before the loop to 384 (1.0.3: and that limit),
// in the register and in the stack copy the loop and the final draw read back: an oversized cluster shows its first
// 384 pieces for that frame.
struct FxClusterFix {
    const char* build_id;
    uint64_t at;
    int n;
    uint32_t from[9], to[9];
};
// 1.0.3: ldr w8,[x19,#0x200] / cmp w23,w8 / b.ls ok / adrp x0 / adrp x2 / add x0 / add x2 / mov w1,#444 / bl report
//   ->   ldr w8,[x19,#0x200] / mov w9,#384 / cmp w8,w9 / csel w8,w8,w9,lo / cmp w23,w8 / csel w23,w23,w8,ls /
//        str x23,[sp,#0x1e0] / nop / nop   (w9 is written again before it is next read)
// 1.0.0 checks against 384 itself (cmp w23,#0x181); its report call (adrp x0 / adrp x2 / add x0, reached only when
// the count is too large) -> mov w23,#384 / str x23,[sp,#0x308] / b past the report.
#define FX103_TO {0xB9420268, 0x52803009, 0x6B09011F, 0x1A893108, 0x6B0802FF, 0x1A8892F7, 0xF900F3F7, kNop, kNop}
const FxClusterFix kFxClusterFixes[] = {
    {"5a797a3a", 0x628c5c, 3, {0xB0007C20, 0xB0007C22, 0x912BCC00}, {0x52803017, 0xF90187F7, 0x14000004}},
    {"6569410c", 0x62398c, 9,
     {0xB9420268, 0x6B0802FF, 0x540000E9, 0x90007BE0, 0x90007BE2, 0x9131D800, 0x91338C42, 0x52803781, 0x94057D00},
     FX103_TO},
    {"df3766a2", 0x62395c, 9,
     {0xB9420268, 0x6B0802FF, 0x540000E9, 0xD0007C00, 0xD0007C02, 0x9133D800, 0x91358C42, 0x52803781, 0x94058339},
     FX103_TO},
};
#undef FX103_TO

void ApplyFxClusterFix(Runtime& rt) {
    const FxClusterFix* f = nullptr;
    for (const FxClusterFix& x : kFxClusterFixes)
        if (rt.main_nso.build_id_hex.rfind(x.build_id, 0) == 0) f = &x;
    if (!f) return;
    for (int i = 0; i < f->n; ++i) {
        const uint32_t* at = reinterpret_cast<const uint32_t*>(rt.main.base() + f->at + 4 * i);
        if (!rt.main.Contains(reinterpret_cast<uintptr_t>(at)) || *at != f->from[i]) {
            LOGW("hooks", "effect cluster fix not applied: main+0x%llx is not as expected",
                 (unsigned long long)(f->at + 4 * i));
            return;
        }
    }
    std::string err;
    for (int i = 0; i < f->n; ++i)
        if (!rt.main.PatchInstruction(f->at + 4 * i, f->from[i], f->to[i], &err)) {
            LOGW("hooks", "effect cluster fix: %s", err.c_str());
            return;
        }
    LOGI("hooks", "effect cluster fix: effects with more than 384 pieces are drawn in part instead of overwriting memory");
}

void SkipIntroLogos(Runtime& rt, const GameBuild& b) {
    const LogoSkip* p = nullptr;
    for (const LogoSkip& l : kLogoSkips)
        if (!strncmp(l.build_id, b.build_id, strlen(l.build_id))) p = &l;
    if (!p) return;
    struct Edit { uint64_t at; uint32_t from, to; };
    const Edit edits[] = {
        {p->open[0], p->open_insn[0], kNop},
        {p->open[1], p->open_insn[1], kNop},
        {p->timer, kLdrS1Timer, 0x14000000u | uint32_t(((p->timer_done - p->timer) / 4) & 0x3FFFFFF)},  // b
    };
    std::string err;
    size_t done = 0;
    while (done < std::size(edits) && rt.main.PatchInstruction(edits[done].at, edits[done].from, edits[done].to, &err))
        done++;
    if (done == std::size(edits)) {
        LOGI("hooks", "skip_intro: the intro logos are skipped");
        return;
    }
    LOGW("hooks", "skip_intro: patch failed (%s): the logos are shown", err.c_str());
    std::string undo_err;
    while (done-- > 0) rt.main.PatchInstruction(edits[done].at, edits[done].to, edits[done].from, &undo_err);
}

// ---- dynamic frame rate (fps = auto) ---------------------------------------------------------------------------
// After PR #13 ("Dynamic FPS" mods for console ports): the game's time step already follows the real frame time
// (FrameLimiter), so the game keeps its speed at any rate; this half picks the rate. A phone screen shows 60, 30, 20
// and 15 fps with even frame spacing (they divide 60 and 90 Hz), 22-25 fps would alternate short and long frames, so
// the cap moves between those rates with hysteresis: down quickly when frames do not fit their period, up slowly and
// only with clear headroom. Not while a video plays: videos are paced at the game version's movie rate.
std::atomic<int> g_movies{0};
std::atomic<int> g_cap_fps{60};  // the rate the limiter paces to with fps = auto
std::atomic<float> g_gpu_frame_ms{0.0f};
std::atomic<float> g_present_slack_ms{0.0f};  // ReportPresentSlack: smoothed, ms
// Where a paced frame aims to finish against its aim point (ms; ReportPresentSlack: positive = earlier): 1.5 ms
// early on average; early ones are held back to the aim (at most 3 ms, MailboxHold), late ones get the room.
constexpr double kPacingSlackMs = 1.5;
std::atomic<int64_t> g_present_slack_at{0};   // steady_clock ns of the last report
std::atomic<int64_t> g_refresh_last{0}, g_refresh_period{0};  // ReportDisplayRefresh
std::atomic<int64_t> g_limiter_paced_at{0};  // steady_clock ns of the last frame the limiter paced (LimiterPacedRecently)
std::atomic<uint64_t> g_lim_grid{0}, g_lim_other{0}, g_lim_slept{0}, g_lim_sleep_ns{0}, g_lim_missed{0},
    g_lim_resync{0};
std::atomic<int64_t> g_lim_phase_us{0}, g_lim_slack_sum_us{0}, g_lim_miss_sum_us{0};
bool MoviePlaying() { return g_movies.load(std::memory_order_relaxed) > 0; }
int MovieFps() { return g_build ? g_build->movie_fps : 30; }
int MinFps() { return std::clamp(GetRuntime().config.min_fps, 15, 30); }

struct DynamicFps {
    double fast = 0, slow = 0;  // smoothed cost of a frame in ms (fast: ~7 frames, slow: ~90 frames)
    int over = 0, under = 0, hold = 0;
};
DynamicFps g_dyn;

int DynamicRates(int out[6]) {  // highest first: 60, 30, 20, 15 down to min_fps
    static constexpr int kRates[] = {60, 30, 20, 15};
    const int lo = MinFps();
    int n = 0;
    for (int r : kRates)
        if (r >= lo) out[n++] = r;
    if (out[n - 1] > lo) out[n++] = lo;
    return n;
}

void DynamicStep(double work_ms) {
    DynamicFps& s = g_dyn;
    int rates[6];
    const int n = DynamicRates(rates);
    int cap = g_cap_fps.load(std::memory_order_relaxed);
    int idx = n - 1;
    for (int i = 0; i < n; i++)
        if (rates[i] <= cap) {
            idx = i;
            break;
        }
    cap = rates[idx];
    const double period = 1000.0 / cap;
    // A frame's cost: what the game thread took, or the GPU's time if that is longer (with the cap leaving the GPU
    // idle, the game thread alone looks cheap). A loading stall is not the cost of a frame.
    const double gpu = g_gpu_frame_ms.load(std::memory_order_relaxed);
    const double cost = std::min(std::max(work_ms, gpu * 1.03), period * 3.0);
    if (s.fast == 0) s.fast = s.slow = cost;
    s.fast += 0.15 * (cost - s.fast);
    s.slow += (1.0 / 90.0) * (cost - s.slow);
    if (s.hold > 0) s.hold--;
    s.over = s.fast > period * 0.98 ? s.over + 1 : 0;
    if (s.over >= 10 && idx + 1 < n) {
        int next = idx + 1;
        while (next + 1 < n && 1000.0 / rates[next] < s.fast * 1.06) next++;
        LOGI("hooks", "fps auto: %d -> %d fps (frame cost %.1f ms, GPU %.1f ms)", cap, rates[next], s.fast, gpu);
        g_cap_fps.store(rates[next], std::memory_order_relaxed);
        s.over = s.under = 0;
        s.slow = s.fast;
        s.hold = 900;  // ~30-45 s before trying a higher rate again
        return;
    }
    if (idx > 0 && s.hold == 0) {
        s.under = s.slow * 1.15 < 1000.0 / rates[idx - 1] ? s.under + 1 : 0;
        if (s.under >= 240) {
            LOGI("hooks", "fps auto: %d -> %d fps (frame cost %.1f ms, GPU %.1f ms)", cap, rates[idx - 1], s.slow, gpu);
            g_cap_fps.store(rates[idx - 1], std::memory_order_relaxed);
            s.under = 0;
            s.hold = 300;
        }
    } else {
        s.under = 0;
    }
}

// Replacement for the game's frame limiter (1.0.0: main+0x70bd90, 1.0.3: main+0x706d28; this, fps): called once
// per game update on the render thread. Same contract as the original: skip when the engine says so (1.0.0: a
// global flag; 1.0.3: a flag in `this`); sleep until a frame period has passed since the last call; remember the
// tick (1.0.0: this+0x310). Then the game's time step is set to the period that actually passed, and the
// frame-skip state (1.0.0: this+0x31d/0x31e/0x320) is kept clear.
void FrameLimiter(uint8_t* self, int /*fps*/) {
    const Runtime& rt = GetRuntime();
    const GameBuild& b = *g_build;
    if (b.limiter_global ? *reinterpret_cast<const int32_t*>(rt.main.base() + b.limiter_global) != 0
                         : self[b.limiter_enabled] == 0)
        return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    // The frame deadlines: one period after the previous deadline, not after the moment the previous sleep ended.
    // Measured from the wake-up, every frame came out a little late (the sleep overshoots by up to a few tenths of
    // a ms), 16.8 ms instead of 16.67 at 60 fps: on a 120 Hz screen the frames slid against the refreshes and every
    // second or so one was shown a refresh late, or (present_mode mailbox) dropped for the next: a stutter.
    static clock::time_point deadline;
    auto now = clock::now();
    // fps = auto: what this frame cost before any sleeping (videos are paced at their own rate).
    if (rt.config.dynamic_fps && last.time_since_epoch().count() != 0 && !MoviePlaying())
        DynamicStep(std::chrono::duration<double, std::milli>(now - last).count());
    const int fps = FrameRateNow();
    const auto period = std::chrono::nanoseconds(1'000'000'000 / fps);
    // Diagnostics: a game frame that took over 25 ms from its start to here (before any sleep) is logged, with
    // what the game's threads ran on the CPU meanwhile (profiler, within a second). At most 20 per 10 s.
    if (last.time_since_epoch().count() != 0 && !MoviePlaying()) {
        const double work_ms = std::chrono::duration<double, std::milli>(now - last).count();
        static int64_t window_start = 0;
        static int in_window = 0;
        const int64_t tn = now.time_since_epoch().count();
        if (tn - window_start > 10'000'000'000) {
            window_start = tn;
            in_window = 0;
        }
        if (work_ms > 25.0 && work_ms < 2000.0 && in_window++ < 20) {
            LOGI("hooks", "slow frame: %.1f ms from one frame's start to the next (the cap allows %.1f)", work_ms,
                 1000.0 / fps);
            profiler::NoteSlowFrame(last.time_since_epoch().count(), tn, work_ms);
        }
    }
    if (last.time_since_epoch().count() != 0) {
        // present_mode mailbox pacing: frames start on the display's refresh grid (one refresh apart at the cap,
        // so the rate is the screen's exactly: 59.8 Hz on a Thor, where a 60.0 fps schedule drifted against it)
        // plus a phase. Frames ready long before their slot waited there (input read that much earlier), late ones
        // slip a refresh: the phase moves (at most 0.5 ms a frame) until frames are ready about 1.5 ms before
        // their slot. Moving only the phase, the rate cannot drift away from the screen's whatever the readings.
        const int64_t tn = now.time_since_epoch().count();
        const int64_t vs = g_refresh_last.load(std::memory_order_acquire), vp = g_refresh_period.load(std::memory_order_relaxed);
        static int64_t grid = 0;  // the refresh the previous frame started on
        static double phase_ms = 0;
        if (vp > 0 && tn - vs < 8 * vp && tn - g_present_slack_at.load(std::memory_order_relaxed) < 500'000'000) {
            const int64_t spacing = std::max<int64_t>(1, std::llround(double(period.count()) / double(vp)));
            auto on_grid = [&](int64_t t) { return vs + (t > vs ? (t - vs + vp - 1) / vp : 0) * vp; };
            int64_t next = grid ? on_grid(grid + spacing * vp - vp / 2) : 0;
            if (!next || next < tn - 4 * vp) {  // first frame, or after a stall: from now
                next = on_grid(tn);
                g_lim_resync.fetch_add(1, std::memory_order_relaxed);
            }
            phase_ms += std::clamp((g_present_slack_ms.load(std::memory_order_relaxed) - kPacingSlackMs) * 0.15, -0.5, 0.5);
            // The phase stays within one refresh: a refresh more of it is the next refresh with none (the slot chain
            // follows the frames, so a frame a refresh later is just as early for its slot). Wrapped with the grid
            // moved the other way, the deadline does not jump.
            const double vp_ms = double(vp) / 1e6;
            if (phase_ms >= vp_ms) {
                phase_ms -= vp_ms;
                next += vp;
            } else if (phase_ms < 0) {
                phase_ms += vp_ms;
                next -= vp;
            }
            grid = next;
            deadline = clock::time_point(std::chrono::nanoseconds(next + int64_t(phase_ms * 1e6)));
            g_lim_grid.fetch_add(1, std::memory_order_relaxed);
            g_limiter_paced_at.store(tn, std::memory_order_relaxed);
            g_lim_phase_us.store(int64_t(phase_ms * 1000), std::memory_order_relaxed);
            g_lim_slack_sum_us.fetch_add(int64_t(g_present_slack_ms.load(std::memory_order_relaxed) * 1000),
                                         std::memory_order_relaxed);
            if (now > deadline) {
                g_lim_missed.fetch_add(1, std::memory_order_relaxed);
                g_lim_miss_sum_us.fetch_add((now - deadline).count() / 1000, std::memory_order_relaxed);
            }
        } else {
            grid = 0;
            g_lim_other.fetch_add(1, std::memory_order_relaxed);
            deadline += period;
            // More than half a period behind (a hitch, a loading stall, a lower rate than before, the game held by
            // the menu): the schedule starts again from now instead of rushing frames out to catch up.
            if (now > deadline + period / 2) deadline = now;
        }
        if (now < deadline) {
            g_lim_slept.fetch_add(1, std::memory_order_relaxed);
            g_lim_sleep_ns.fetch_add(uint64_t((deadline - now).count()), std::memory_order_relaxed);
            std::this_thread::sleep_until(deadline);
            now = clock::now();
        }
    } else {
        deadline = now;
    }
    double seconds = last.time_since_epoch().count() ? std::chrono::duration<double>(now - last).count() : 1.0 / fps;
    // present_mode mailbox pacing: each frame is shown for whole refreshes (one at the cap), at an even rhythm, but
    // the moment the game thread passes here varies by a few ms (it waits for the render thread, the deadline
    // moves): a time step of exactly what each frame is shown for keeps motion even (the measured one made the
    // game move unevenly against even frames: micro stutter).
    if (std::chrono::steady_clock::now().time_since_epoch().count() - g_present_slack_at.load(std::memory_order_relaxed) <
            500'000'000 &&
        !MoviePlaying()) {
        const double p = 1.0 / fps;
        seconds = std::max(1.0, std::round(seconds / p)) * p;
    }
    last = now;
    // Up to 1/20 s per step (1/min_fps with fps = auto and min_fps 15): slower devices keep the game at normal speed
    // instead of slow motion.
    const double slowest = rt.config.dynamic_fps ? std::min(MinFps(), 20) : 20;
    // Down to 1/60 s (fps = unlocked: one period of the screen's refresh rate).
    const double fastest = rt.config.unlocked_fps && !rt.config.dynamic_fps ? std::max(60, FixedFps(rt.config)) : 60;
    if (g_game_step) {
        *g_game_step = float(std::clamp(seconds, 1.0 / fastest, 1.0 / slowest));
        SetGrazeFactors(*g_game_step);
    }
    *reinterpret_cast<int64_t*>(self + b.last_tick) = hle::os::NowTicks();
    self[b.skip_active] = 0;
    self[b.skip_toggle] = 0;
    *reinterpret_cast<double*>(self + b.skip_time) = 0;
}

void Trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

uintptr_t GuestOffset(const void* p) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const loader::Module& m = GetRuntime().main;
    return m.Contains(a) ? a - m.base() : 0;
}

// Engine debug print (1.0.0: main+0x7fea18), (int level, const char* fmt, ...)
void EngineLog(int level, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    Trim(msg);
    if (msg[0]) LOGI("engine", "[%d] %s", level, msg);
}

// The same with a va_list (1.0.0: main+0x7feb30)
void EngineLogV(int level, const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    Trim(msg);
    if (msg[0]) LOGI("engine", "[%d] %s", level, msg);
}

// DL_PANIC(const char* file, int line, const char* fmt, ...) (1.0.0: main+0x7892d8)
void EnginePanic(const char* file, int line, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    Trim(msg);
    const char* base = file ? strrchr(file, '/') : nullptr;
    FATAL("engine", "DL_PANIC: %s  (%s:%d, called from main+0x%lx)", msg, base ? base + 1 : (file ? file : "?"), line,
          (unsigned long)GuestOffset(__builtin_return_address(0)));
}

// ---- aspect ratio: camera aspect (experimental) --------------------------------------------------------------
// With a wider or narrower render resolution the 3D view still came out stretched: the camera that renders the
// scene keeps a 16:9 aspect ratio. Cameras keep {fov, aspect, near, far} at +0x50..+0x5c (main+0xfa7f4 reads
// fov and aspect for the view frustum). This scans the guest heap and the game's data for such blocks with a
// 16:9 aspect, logs them (with the owner's vtable, to find the code that sets it) and writes the wanted aspect
// into them, again every second (rescanning every 2 s until cameras are found, then every 10 s).
float g_aspect = 0;
void CameraAspectThread() {
    pthread_setname_np(pthread_self(), "dsr-aspect");
    const loader::Module& m = GetRuntime().main;
    std::vector<float*> found;
    int scans = 0;
    auto plausible = [](const float* a) {  // a = &aspect: fov before it, near and far after
        const float fov = a[-1], nearz = a[1], farz = a[2];
        return fov > 0.2f && fov < 2.5f && nearz > 0.001f && nearz < 50.0f && farz > nearz && farz < 1e6f;
    };
    for (;;) {
        const auto scan_start = std::chrono::steady_clock::now();
        // Scan: 16:9 exactly (0x3FE38E39) at a 4-byte boundary.
        uintptr_t hb = 0;
        size_t hs = 0;
        hle::os::GuestHeapRange(&hb, &hs);
        std::vector<std::pair<uintptr_t, size_t>> ranges;
        if (hb && hs) ranges.push_back({hb, hs});
        const loader::NsoImage& nso = GetRuntime().main_nso;  // .data + .bss
        const size_t data = nso.data.mem_offset, data_size = size_t(nso.data.size) + nso.bss_size;
        if (data && m.size() > data) ranges.push_back({m.base() + data, std::min<size_t>(m.size() - data, data_size)});
        int new_found = 0;
        for (const auto& [base, size] : ranges) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(base + 4);
            const uint32_t* end = reinterpret_cast<const uint32_t*>(base + size - 12);
            for (; p < end; p++) {
                if (*p != 0x3FE38E39u) continue;
                float* a = reinterpret_cast<float*>(const_cast<uint32_t*>(p));
                if (!plausible(a)) continue;
                if (std::find(found.begin(), found.end(), a) != found.end()) continue;
                found.push_back(a);
                new_found++;
                const uintptr_t obj = reinterpret_cast<uintptr_t>(a) - 0x54;
                uintptr_t vt = 0;
                if ((obj & 7) == 0) memcpy(&vt, reinterpret_cast<const void*>(obj), 8);
                if (found.size() <= 40)
                    LOGI("hooks", "camera aspect: 16:9 at %p (fov %.3f near %.3f far %.1f), object vtable %s0x%lx", a,
                         a[-1], a[1], a[2], m.Contains(vt) ? "main+" : "", (unsigned long)(m.Contains(vt) ? vt - m.base() : vt));
            }
        }
        scans++;
        const double scan_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scan_start).count();
        if (new_found || scans <= 3 || scans % 60 == 0)
            LOGI("hooks", "camera aspect: scan %d (%.0f ms): %d new 16:9 cameras, %zu in all; setting them to %.3f", scans,
                 scan_ms, new_found, found.size(), g_aspect);
        const int rewrites = found.empty() ? 2 : 10;  // 2 s, then 10 s between scans
        for (int k = 0; k < rewrites; k++) {
            for (float* a : found)
                if (*a > 1.7777f && *a < 1.7779f && plausible(a)) *a = g_aspect;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

struct Hook {
    uint64_t offset;
    void* fn;
    const char* name;
};

}  // namespace

float GameFrameTime() { return g_game_step ? *g_game_step : 1.0f / 30.0f; }
bool LimiterPacedRecently() {
    return std::chrono::steady_clock::now().time_since_epoch().count() - g_limiter_paced_at.load(std::memory_order_relaxed) <
           100'000'000;
}
bool DisplayPacingActive() {
    return std::chrono::steady_clock::now().time_since_epoch().count() - g_present_slack_at.load(std::memory_order_relaxed) <
           500'000'000;
}
std::string TakeLimiterReport() {
    const uint64_t grid = g_lim_grid.exchange(0), other = g_lim_other.exchange(0), slept = g_lim_slept.exchange(0),
                   sleep_ns = g_lim_sleep_ns.exchange(0), missed = g_lim_missed.exchange(0),
                   resync = g_lim_resync.exchange(0);
    const int64_t slack = g_lim_slack_sum_us.exchange(0), miss = g_lim_miss_sum_us.exchange(0);
    char b[256];
    snprintf(b, sizeof(b),
             "frame limiter: %llu frames on the refresh grid (%llu restarts), %llu not; %llu slept %.1f ms on average, "
             "%llu came %.1f ms after their deadline on average; phase %.1f ms, slack followed %.1f ms on average",
             (unsigned long long)grid, (unsigned long long)resync, (unsigned long long)other, (unsigned long long)slept,
             slept ? sleep_ns / 1e6 / double(slept) : 0.0, (unsigned long long)missed,
             missed ? miss / 1e3 / double(missed) : 0.0, g_lim_phase_us.load() / 1e3,
             grid ? slack / 1e3 / double(grid) : 0.0);
    return b;
}
void ReportDisplayRefresh(int64_t last_ns, int64_t period_ns) {
    g_refresh_period.store(period_ns, std::memory_order_relaxed);
    g_refresh_last.store(last_ns, std::memory_order_release);
}
void ReportPresentSlack(double ms) {
    const float prev = g_present_slack_ms.load(std::memory_order_relaxed);
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    const bool fresh = now - g_present_slack_at.load(std::memory_order_relaxed) < 500'000'000;
    g_present_slack_ms.store(fresh ? prev + 0.2f * (float(ms) - prev) : float(ms), std::memory_order_relaxed);
    g_present_slack_at.store(now, std::memory_order_relaxed);
}
void ReportGpuFrameTime(double ms) {
    if (!(ms > 0.0)) return;
    const float prev = g_gpu_frame_ms.load(std::memory_order_relaxed);
    g_gpu_frame_ms.store(prev > 0.0f ? prev + 0.1f * (float(ms) - prev) : float(ms), std::memory_order_relaxed);
}
void SetMoviePlaying(bool playing) {
    playing ? ++g_movies : --g_movies;
    LOGI("hooks", "movie %s: frame rate %d fps", playing ? "started" : "ended", FrameRateNow());
}
bool FrameRateAdjustable() { return g_game_step != nullptr; }
namespace {
std::atomic<int> g_gpu_has_bc{-1};
}
void SetGpuHasBc(bool has) { g_gpu_has_bc = has ? 1 : 0; }
int GpuHasBc() { return g_gpu_has_bc.load(); }
int FrameRateNow() {
    if (MoviePlaying()) return MovieFps();
    const RuntimeConfig& c = GetRuntime().config;
    return c.dynamic_fps ? g_cap_fps.load(std::memory_order_relaxed) : FixedFps(c);
}

const char* GameVersion() { return g_build ? g_build->version : nullptr; }

// ---- save request diagnostics (the game saving ~28 times a second) ----------------------------------------
// The game's save manager (GameDataMan-like object at main+0x22c3930 in 1.0.3) saves when its request byte (+0xb1f)
// is set and no save is running (+0xb2c). Two small functions set it (only while saving is allowed, +0xb50):
// 0x350a0c (no arguments) and 0x3534d4 (bool, also sets +0xb20 / +0xb21); a timer in 0x350820 sets it every 5
// minutes. Both setters are replaced by equivalents that also count their callers, reported every 10 s, to find
// what requests a save every frame. Same code at the same place in both 1.0.3 builds.
struct SaveCaller {
    uintptr_t ret;
    uint64_t calls;
};
std::mutex g_save_req_mutex;
SaveCaller g_save_callers[16];
uint64_t g_save_req_total = 0;
uint8_t** g_save_man = nullptr;
std::chrono::steady_clock::time_point g_save_report;
void NoteSaveRequest(uintptr_t ret) {
    const uintptr_t base = GetRuntime().main.base();
    std::lock_guard<std::mutex> lk(g_save_req_mutex);
    g_save_req_total++;
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (g_save_callers[i].calls && g_save_callers[i].ret == ret) {
            g_save_callers[i].calls++;
            free_slot = -2;
            break;
        }
        if (!g_save_callers[i].calls && free_slot == -1) free_slot = i;
    }
    if (free_slot >= 0) g_save_callers[free_slot] = {ret, 1};
    const auto now = std::chrono::steady_clock::now();
    if (now - g_save_report >= std::chrono::seconds(10)) {
        g_save_report = now;
        std::string line;
        for (SaveCaller& c : g_save_callers) {
            if (!c.calls) continue;
            char b[64];
            snprintf(b, sizeof(b), "%smain+0x%lx x%llu", line.empty() ? "" : ", ", (unsigned long)(c.ret - base),
                     (unsigned long long)c.calls);
            line += b;
            c = {};
        }
        LOGI("hooks", "save requests (last 10 s): %llu, from %s", (unsigned long long)g_save_req_total, line.c_str());
        g_save_req_total = 0;
    }
}
void SaveRequestPlain() {
    NoteSaveRequest(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    uint8_t* m = *g_save_man;
    if (m[0xb50]) m[0xb1f] = 1;
}
void SaveRequestFlagged(bool b) {
    NoteSaveRequest(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    uint8_t* m = *g_save_man;
    if (m[0xb50]) {
        m[0xb20] = 1;
        m[0xb1f] = 1;
        m[0xb21] = b ? 1 : 0;
    }
}
// Finds a small function by its instructions: `adrp x8, page; ldr x8, [x8, #off]` (any page / offset) and then
// `words` exactly. Returns its image offset (0: none or more than one) and the address the pair loads from.
uint64_t FindSaveSetter(const Runtime& rt, const uint32_t* words, size_t n, uintptr_t* global) {
    const uint32_t* text = reinterpret_cast<const uint32_t*>(rt.main.base());
    const size_t count = rt.main.nso().text.size / 4;
    uint64_t found = 0;
    for (size_t i = 0; i + 2 + n < count; i++) {
        if ((text[i] & 0x9f00001f) != 0x90000008 || (text[i + 1] & 0xffc003ff) != 0xf9400108) continue;
        if (memcmp(text + i + 2, words, n * 4) != 0) continue;
        if (found) return 0;  // not unique
        found = i * 4;
        const uint32_t w = text[i];
        int64_t imm = int64_t(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3));
        if (imm & (1 << 20)) imm -= 1 << 21;
        const uintptr_t page = ((rt.main.base() + found) & ~uintptr_t(0xfff)) + uintptr_t(imm << 12);
        *global = page + ((text[i + 1] >> 10) & 0xfff) * 8;
    }
    return found;
}
void InstallSaveRequestDiag(Runtime& rt, const GameBuild& b) {
    if (strncmp(b.build_id, "6569410c", 8) != 0 && strncmp(b.build_id, "df3766a2", 8) != 0) return;
    // After the adrp / ldr pair (1.0.3, build 6569410c: main+0x350a0c and main+0x3534d4).
    static const uint32_t kPlain[5] = {0x396d4109, 0x34000069, 0x320003e9, 0x392c7d09, 0xd65f03c0};
    static const uint32_t kFlagged[6] = {0x396d4109, 0x340000c9, 0x320003e9, 0x1200000a, 0x392c8109, 0x392c7d09};
    uintptr_t g1 = 0, g2 = 0;
    const uint64_t plain = FindSaveSetter(rt, kPlain, 5, &g1);
    const uint64_t flagged = FindSaveSetter(rt, kFlagged, 6, &g2);
    if (!plain || !flagged || g1 != g2) {
        LOGW("hooks", "save request diagnostics: the setters were not found (0x%lx, 0x%lx)", (unsigned long)plain,
             (unsigned long)flagged);
        return;
    }
    g_save_man = reinterpret_cast<uint8_t**>(g1);
    std::string err;
    if (rt.main.HookFunction(plain, reinterpret_cast<void*>(&SaveRequestPlain), &err) &&
        rt.main.HookFunction(flagged, reinterpret_cast<void*>(&SaveRequestFlagged), &err))
        LOGI("hooks", "save request diagnostics on: setters at main+0x%lx, main+0x%lx (callers every 10 s)",
             (unsigned long)plain, (unsigned long)flagged);
    else
        LOGW("hooks", "save request diagnostics: %s", err.c_str());
}

void InstallGameHooks() {
    Runtime& rt = GetRuntime();
    for (const GameBuild& b : kBuilds)
        if (rt.main_nso.build_id_hex.rfind(b.build_id, 0) == 0) g_build = &b;
    if (g_build && !rt.config.game_hooks) {
        LOGW("hooks", "game version %s: engine patches off (settings game_hooks = 0): 30 fps, 16:9, no engine log",
             g_build->version);
        rt.config.target_fps = 30;
        rt.config.dynamic_fps = false;
        rt.config.unlocked_fps = false;
        return;
    }
    if (!g_build) {
        LOGW("hooks", "unknown game build %.32s: engine hooks disabled (supported: 1.0.0, 1.0.3)",
             rt.main_nso.build_id_hex.c_str());
        rt.config.dynamic_fps = false;  // no frame limiter of ours: the game paces itself
        rt.config.unlocked_fps = false;
        return;
    }
    const GameBuild& b = *g_build;
    LOGI("hooks", "game version %s (build %.16s...)", b.version, rt.main_nso.build_id_hex.c_str());
    // The program and the data files must be the same version (e.g. the 1.0.3 exefs over 1.0.0 data is not).
    if (FILE* f = fopen((rt.config.game_dir + "/DataVersion.txt").c_str(), "r")) {
        char data[64] = {};
        if (fgets(data, sizeof(data), f)) {
            Trim(data);
            for (char* e = data + strlen(data); e > data && e[-1] == ' '; *--e = 0) {}
            for (const GameBuild& o : kBuilds)
                if (strcmp(o.data_version, b.data_version) != 0 && strcmp(data, o.data_version) == 0)
                    LOGW("hooks", "the game's data files are version %s (DataVersion %s) but the program is %s: "
                         "install the matching data (the update's RomFS) or the game may misbehave", o.version, data,
                         b.version);
            if (strcmp(data, b.data_version) == 0) LOGI("hooks", "data files: version %s (%s)", b.version, data);
        }
        fclose(f);
    }
    // Frame rate (fps = 60; offsets below are 1.0.0's). The same two places a well-known Switch 60 fps mod changes,
    // done differently:
    // 1. The game's per-frame update (main+0x66c99c) steps every system by a constant 1/30 s loaded from
    //    read-only data (main+0x66c9c8 adrp x8, page; +0x66c9cc ldr s8, [x8, #0x970]). It now loads a float in
    //    the scratch page instead.
    // 2. The render thread's frame limiter (main+0x70bd90, called once per game update with a hard-coded 30) is
    //    replaced by FrameLimiter: it paces to 60 and sets that float to the measured frame period (1/60..1/20 s),
    //    so the game keeps normal speed at 60 fps and when heavy areas run slower. It also keeps the game's
    //    frame-skip mode off (with it, slow frames ran two updates per shown frame).
    // Installed at every frame rate (30 included), so the cap can be changed while the game runs (in-game
    // settings menu): FrameLimiter reads the configured rate every frame.
    {
        std::string err;
        float* step = reinterpret_cast<float*>(rt.main.ScratchPage());
        *step = 1.0f / 30.0f;
        const uint32_t adrp = rt.main.EncodeAdrp(b.step_adrp, 8, reinterpret_cast<uintptr_t>(step));
        const bool adrp_ok = rt.main.PatchInstruction(b.step_adrp, b.step_adrp_insn, adrp, &err);
        bool ok = adrp_ok && rt.main.PatchInstruction(b.step_adrp + 4, b.step_ldr_insn, 0xBD400108, &err);  // ldr s8, [x8]
        if (ok) ok = rt.main.HookFunction(b.limiter, reinterpret_cast<void*>(&FrameLimiter), &err);
        if (ok) {
            g_game_step = step;
            ApplyGrazeFix(rt, step + 1);
            if (rt.config.dynamic_fps)
                LOGI("hooks", "frame rate: own frame limiter, fps auto (60 / 30 / 20 fps, not below %d; changeable in game), "
                     "game time step follows the frame time", MinFps());
            else if (rt.config.unlocked_fps)
                LOGI("hooks", "frame rate: own frame limiter, refresh rate (experimental): up to %d fps, the screen's refresh "
                     "rate (changeable in game); game time step follows the frame time, physics may misbehave above 60",
                     FixedFps(rt.config));
            else
                LOGI("hooks", "frame rate: own frame limiter at %d fps (changeable in game), game time step follows the frame time",
                     FixedFps(rt.config));
        } else {
            LOGW("hooks", "60 fps patches failed (%s): staying at 30 fps", err.c_str());
            rt.main.PatchInstruction(b.step_adrp + 4, 0xBD400108, b.step_ldr_insn, &err);  // undo what applied
            if (adrp_ok) rt.main.PatchInstruction(b.step_adrp, adrp, b.step_adrp_insn, &err);
            rt.config.target_fps = 30;
            rt.config.dynamic_fps = false;
            rt.config.unlocked_fps = false;
        }
    }
    // Aspect ratio (offsets below are 1.0.0's; kBuilds has each build's). Two places set the game's resolution, both
    // with built-in defaults:
    // - the render resolution, read from the game's settings with defaults 1280 / 720 (main+0x6a16b0:
    //   "Normal.Resolution.Width" main+0x6a170c mov w2, #1280; "...Height" main+0x6a1758 mov w2, #720), and
    //   again by the width getter main+0x6a18cc (main+0x6a18f8 mov w2, #1280) whose callers (main+0x101d80,
    //   +0x66e99c, +0x66e9cc) size the post-processing buffers and their copies of the scene. It
    //   also becomes the screen size floats (main+0x1af4a7c / +0x1af4a84) the cameras take their aspect ratio
    //   from (e.g. main+0xbca88), so the 3D view widens (or narrows) instead of stretching;
    // - the handheld display mode (main+0x716158: main+0x716170 mov w1, #1280; +0x716178 mov w2, #720).
    // Frame buffers are allocated at 1920x1080 (main+0x70a20c), the limit. The height always stays 720: the
    // game's half- and quarter-size buffers (640x360, 320x180) keep their 720-line sizes whatever the render
    // resolution, and a 4:3 test at 1280x960 died silently on loading a save. Only the width changes: wider
    // than 16:9 shows more at the sides, narrower (4:3: 960x720) less.
    // Render scale (settings render_scale, 0.5 - 1.0): the same resolution settings at a lower size, height and
    // width together (the height also has a getter with its own default, main+0x6a193c: main+0x6a1968 mov w2, #720).
    // The game draws less and the frame is enlarged to the screen at the end. Heights stay at or below 720 (the
    // fixed half- and quarter-size buffers are then larger than needed). One half-size target is sized from the
    // engine's screen size defaults instead (ApplyScreenSizeDefaults), which follow the render size.
    if (!rt.config.docked) {
        float ar = 16.0f / 9.0f;
        bool ar_ok = true;
        if (rt.config.aspect_ratio == "auto") {
            ar = rt.config.screen_aspect;
        } else if (rt.config.aspect_ratio != "16:9") {
            float w = 0, h = 0;
            if (sscanf(rt.config.aspect_ratio.c_str(), "%f:%f", &w, &h) == 2 && h > 0) ar = w / h;
            else ar = 0;
        }
        if (!(ar > 0.5f && ar < 4.0f)) {
            LOGW("hooks", "aspect_ratio %s: not understood (or screen size unknown), staying at 16:9",
                 rt.config.aspect_ratio.c_str());
            ar = 16.0f / 9.0f;
            ar_ok = false;
        }
        const float scale = std::clamp(rt.config.render_scale, 0.5f, 1.0f);
        const int height = std::clamp(int(std::lround(720.0f * scale / 8.0f)) * 8, 360, 720);
        const int width = std::clamp(int(std::lround(float(height) * ar / 8.0f)) * 8, 320, 1920);
        if (width != 1280 || height != 720) {
            std::string err;
            auto mov = [](int rd, int v) { return 0x52800000u | (uint32_t(v) << 5) | uint32_t(rd); };
            bool ok = true;
            if (width != 1280)
                ok = ok && rt.main.PatchInstruction(b.width_cfg, 0x5280A002, mov(2, width), &err) &&
                     rt.main.PatchInstruction(b.width_get, 0x5280A002, mov(2, width), &err) &&
                     rt.main.PatchInstruction(b.display_width, 0x5280A001, mov(1, width), &err);
            if (height != 720)
                ok = ok && rt.main.PatchInstruction(b.height_cfg, 0x52805A02, mov(2, height), &err) &&
                     rt.main.PatchInstruction(b.height_get, 0x52805A02, mov(2, height), &err) &&
                     rt.main.PatchInstruction(b.display_height, 0x52805A02, mov(2, height), &err);
            if (ok) {
                LOGI("hooks", "aspect ratio %s (%.3f), render scale %.2f: the game renders at %dx%d (was 1280x720)",
                     ar_ok ? rt.config.aspect_ratio.c_str() : "16:9", ar, scale, width, height);
                g_aspect = float(width) / float(height);
                rt.config.render_width = width;
                rt.config.render_height = height;
                ApplyScreenSizeDefaults(rt, b, width, height);
                if (std::fabs(g_aspect - 16.0f / 9.0f) > 0.01f) std::thread(CameraAspectThread).detach();
            } else {
                LOGW("hooks", "resolution patch failed: %s", err.c_str());
            }
        }
    }
    ApplyFxClusterFix(rt);
    if (rt.config.skip_intro) SkipIntroLogos(rt, b);
    if (rt.config.performance_mode) ApplyPerformanceMode(rt, b);
    ApplyShadowQuality(rt, b);
    if (rt.config.lighter_post_fx) ApplyLighterPostFx(rt, b);
    {
        constexpr uint32_t kAdd2MiB = 0x11480108;  // add w8, w8, #0x200, lsl #12
        constexpr uint32_t kAdd8MiB = 0x11600108;  // add w8, w8, #0x800, lsl #12
        std::string err;
        if (rt.main.PatchInstruction(b.dyn_buffer, kAdd2MiB, kAdd8MiB, &err))
            LOGI("hooks", "dynamic buffers: 8 MiB each (were 2 MiB)");
        else
            LOGW("hooks", "dynamic buffer size not changed (%s)", err.c_str());
    }
    InstallSwipeCamera(rt, b);
    InstallSaveRequestDiag(rt, b);
    const Hook hooks[] = {
        {b.log, reinterpret_cast<void*>(&EngineLog), "engine log"},
        {b.vlog, reinterpret_cast<void*>(&EngineLogV), "engine vlog"},
        {b.panic, reinterpret_cast<void*>(&EnginePanic), "DL_PANIC"},
    };
    for (const Hook& h : hooks) {
        std::string err;
        if (rt.main.HookFunction(h.offset, h.fn, &err))
            LOGI("hooks", "hooked %s at main+0x%lx", h.name, (unsigned long)h.offset);
        else
            LOGW("hooks", "could not hook %s: %s", h.name, err.c_str());
    }
}

}  // namespace dsr
