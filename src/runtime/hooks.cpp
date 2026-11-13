// Hooks on specific functions inside the game's main module, for the game builds listed in kBuilds (matched by
// build id): 1.0.0 and the 1.0.3 update. They replace engine functions whose output is useful to us but goes
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

#include "common/log.h"
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
};
const GameBuild kBuilds[] = {
    {"5a797a3a5da6c4f3468cb7c84e4dde63", "1.0.0", "20180818_462",
     0x66c9c8, 0xB00080A8, 0xBD497108,
     0x70bd90, 0x251fb4c, 0, 0x310, 0x31d, 0x31e, 0x320,
     0x6a170c, 0x6a18f8, 0x716170, 0x6a1758, 0x6a1968, 0x716178,
     0x7fea18, 0x7feb30, 0x7892d8, 30},
    {"6569410c91c398e78ec76b418ee4cca8", "1.0.3", "20181026_101",
     0x667930, 0xB0008068, 0xBD455908,
     0x706d28, 0, 0x2f7, 0x2e8, 0x2f5, 0x2f6, 0x2f8,
     0x69c638, 0x69c824, 0x711ff0, 0x69c684, 0x69c894, 0x711ff8,
     0x7f71d4, 0x7f72ec, 0x782dac, 60},
};
const GameBuild* g_build = nullptr;

// Skip intro logos (settings skip_intro). The title screen's step machine (TitleStep / TitleStep_forNS, step
// number at this+0xc) opens the company logo dialog (menu dialog 60, FrpgMenuDlgLogo) by setting its request word
// to 1 (`bl` to set(60, 1)) and then waits until the dialog has cleared it again after its last logo. Both classes'
// open calls become `nop`, so the wait passes at once, as if the logos had finished. TitleStep_forNS also counts
// down a timer (this+0xe0) before closing the dialog: that step branches straight to its close path. Everything
// else (loading, the title movie, press start) runs as before.
struct LogoSkip {
    const char* version;
    uint64_t open[2];
    uint32_t open_insn[2];  // bl set_dialog_request (60, 1)
    uint64_t timer;         // ldr s1, [x19, #0xe0]
    uint64_t timer_done;    // the timer-expired path
};
const LogoSkip kLogoSkips[] = {
    {"1.0.0", {0x6f1c30, 0x6f73e8}, {0x97fa3d6c, 0x97fa277e}, 0x6f7450, 0x6f746c},
    {"1.0.3", {0x6ed0e4, 0x6f27fc}, {0x97fa3510, 0x97fa1f4a}, 0x6f2864, 0x6f2880},
};
constexpr uint32_t kNop = 0xD503201F, kLdrS1Timer = 0xBD40E261;

void SkipIntroLogos(Runtime& rt, const GameBuild& b) {
    const LogoSkip* p = nullptr;
    for (const LogoSkip& l : kLogoSkips)
        if (!strcmp(l.version, b.version)) p = &l;
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
    const int fps = FrameRateNow();
    const auto period = std::chrono::nanoseconds(1'000'000'000 / fps);
    auto now = clock::now();
    if (last.time_since_epoch().count() != 0 && now < last + period) {
        std::this_thread::sleep_until(last + period);
        now = clock::now();
    }
    const double seconds = last.time_since_epoch().count() ? std::chrono::duration<double>(now - last).count() : 1.0 / fps;
    last = now;
    // Up to 1/20 s per step: slower devices (20-30 fps) keep the game at normal speed instead of slow motion.
    if (g_game_step) *g_game_step = float(std::clamp(seconds, 1.0 / 60.0, 1.0 / 20.0));
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
namespace {
std::atomic<int> g_movies{0};
int MovieFps() { return g_build ? g_build->movie_fps : 30; }
}
void SetMoviePlaying(bool playing) {
    const int n = playing ? ++g_movies : --g_movies;
    LOGI("hooks", "movie %s: frame rate %d fps", playing ? "started" : "ended", n > 0 ? MovieFps() : std::clamp(GetRuntime().config.target_fps, 30, 60));
}
bool FrameRateAdjustable() { return g_game_step != nullptr; }
int FrameRateNow() {
    return g_movies.load(std::memory_order_relaxed) > 0 ? MovieFps() : std::clamp(GetRuntime().config.target_fps, 30, 60);
}

const char* GameVersion() { return g_build ? g_build->version : nullptr; }

void InstallGameHooks() {
    Runtime& rt = GetRuntime();
    for (const GameBuild& b : kBuilds)
        if (rt.main_nso.build_id_hex.rfind(b.build_id, 0) == 0) g_build = &b;
    if (g_build && !rt.config.game_hooks) {
        LOGW("hooks", "game version %s: engine patches off (settings game_hooks = 0): 30 fps, 16:9, no engine log",
             g_build->version);
        rt.config.target_fps = 30;
        return;
    }
    if (!g_build) {
        LOGW("hooks", "unknown game build %.32s: engine hooks disabled (supported: 1.0.0, 1.0.3)",
             rt.main_nso.build_id_hex.c_str());
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
                if (&o != &b && strcmp(data, o.data_version) == 0)
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
            LOGI("hooks", "frame rate: own frame limiter at %d fps (changeable in game), game time step follows the frame time",
                 std::clamp(rt.config.target_fps, 30, 60));
        } else {
            LOGW("hooks", "60 fps patches failed (%s): staying at 30 fps", err.c_str());
            rt.main.PatchInstruction(b.step_adrp + 4, 0xBD400108, b.step_ldr_insn, &err);  // undo what applied
            if (adrp_ok) rt.main.PatchInstruction(b.step_adrp, adrp, b.step_adrp_insn, &err);
            rt.config.target_fps = 30;
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
    // fixed half- and quarter-size buffers are then larger than needed, which is harmless).
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
                if (std::fabs(g_aspect - 16.0f / 9.0f) > 0.01f) std::thread(CameraAspectThread).detach();
            } else {
                LOGW("hooks", "resolution patch failed: %s", err.c_str());
            }
        }
    }
    if (rt.config.skip_intro) SkipIntroLogos(rt, b);
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
