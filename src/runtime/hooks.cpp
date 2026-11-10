// Hooks on specific functions inside the game's main module (this build only, matched by build id).
// They replace engine functions whose output is useful to us but goes nowhere in a retail build.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "hle/os/os.h"
#include <atomic>

#include "runtime/runtime.h"

namespace dsr {
std::atomic<float> g_gpu_frame_ms{0.0f};

namespace {
float* g_game_step = nullptr;  // the game's per-frame time step (scratch page), when patched
std::atomic<int> g_cap_fps{0};  // the rate the limiter paces to now (dynamic_fps moves it); 0 = not set yet
std::atomic<int> g_movies{0};

int MaxFps() { return std::clamp(GetRuntime().config.target_fps, 15, 60); }
int MinFps() { return std::min(std::clamp(GetRuntime().config.min_fps, 15, 30), MaxFps()); }

// ---- dynamic frame rate --------------------------------------------------------------------------------------
// The idea of the "Dynamic FPS" mods for console ports (Ocarina of Time remake and others): the game's logic is not
// tied to a fixed frame rate, its time step follows the real frame time, so the game keeps its normal speed when
// frames take longer, instead of slowing down. Here that is half of it (the time step, see FrameLimiter). The other
// half picks the pace: a phone display shows 60/30/20/15 fps with even frame spacing (they divide 60 and 90 Hz),
// whereas 22-25 fps would alternate short and long frames. So the cap moves between those rates, with hysteresis:
// down quickly when frames do not fit their period, up slowly and only with clear headroom.
struct DynamicFps {
    double fast = 0, slow = 0;  // smoothed cost of a frame in ms (fast: ~7 frames, slow: ~90 frames)
    int over = 0, under = 0, hold = 0;
};

// Rates the controller may use, highest first: the configured maximum, then 60/30/20/15 below it, down to min_fps.
int DynamicRates(int out[8]) {
    static constexpr int kRates[] = {60, 30, 20, 15};
    const int hi = MaxFps(), lo = MinFps();
    int n = 0;
    out[n++] = hi;
    for (int r : kRates)
        if (r < hi && r >= lo && n < 7) out[n++] = r;
    if (out[n - 1] > lo) out[n++] = lo;
    return n;
}

void DynamicStep(double work_ms) {
    static DynamicFps s;
    int rates[8];
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
    // A frame's cost: what the game thread took, or the GPU's time if that is longer (when the cap leaves the GPU
    // idle, the game thread alone looks cheap). A loading stall is not the cost of a frame.
    double cost = std::max(work_ms, double(g_gpu_frame_ms.load(std::memory_order_relaxed)) * 1.03);
    cost = std::min(cost, period * 3.0);
    if (s.fast == 0) s.fast = s.slow = cost;
    s.fast += 0.15 * (cost - s.fast);
    s.slow += (1.0 / 90.0) * (cost - s.slow);
    if (s.hold > 0) s.hold--;

    s.over = s.fast > period * 0.98 ? s.over + 1 : 0;
    if (s.over >= 10 && idx + 1 < n) {
        int next = idx + 1;
        while (next + 1 < n && 1000.0 / rates[next] < s.fast * 1.06) next++;
        LOGI("hooks", "dynamic fps: %d -> %d fps (frame cost %.1f ms, GPU %.1f ms)", cap, rates[next], s.fast,
             double(g_gpu_frame_ms.load(std::memory_order_relaxed)));
        g_cap_fps.store(rates[next], std::memory_order_relaxed);
        s.over = s.under = 0;
        s.slow = s.fast;
        s.hold = 900;  // ~30 s before trying a higher rate again
        return;
    }
    if (idx > 0 && s.hold == 0) {
        const double up = 1000.0 / rates[idx - 1];
        s.under = s.slow * 1.15 < up ? s.under + 1 : 0;
        if (s.under >= 240) {
            LOGI("hooks", "dynamic fps: %d -> %d fps (frame cost %.1f ms)", cap, rates[idx - 1], s.slow);
            g_cap_fps.store(rates[idx - 1], std::memory_order_relaxed);
            s.under = 0;
            s.hold = 300;
        }
    } else {
        s.under = 0;
    }
}

// Replacement for the game's frame limiter, main+0x70bd90 (this, fps): called once per game update on the
// render thread. Same contract as the original: skip when the engine's global flag says so; sleep until a
// frame period has passed since the last call; remember the tick at this+0x310. Then the game's time step is
// set to the period that actually passed, and the frame-skip state (this+0x31d/0x31e/0x320) is kept clear.
void FrameLimiter(uint8_t* self, int /*fps*/) {
    const Runtime& rt = GetRuntime();
    if (*reinterpret_cast<const int32_t*>(rt.main.base() + 0x251fb4c) != 0) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    auto now = clock::now();
    const bool have_last = last.time_since_epoch().count() != 0;
    // Dynamic fps: how long this frame took before any sleeping (not while a movie plays: paced at 30 then).
    if (have_last && rt.config.dynamic_fps && g_movies.load(std::memory_order_relaxed) == 0)
        DynamicStep(std::chrono::duration<double, std::milli>(now - last).count());
    const int fps = FrameRateNow();
    const auto period = std::chrono::nanoseconds(1'000'000'000 / fps);
    if (have_last && now < last + period) {
        std::this_thread::sleep_until(last + period);
        now = clock::now();
    }
    const double seconds = have_last ? std::chrono::duration<double>(now - last).count() : 1.0 / fps;
    last = now;
    // The time step is the real frame time, between 1/60 s and the slowest rate allowed (1/30 s as before, or
    // 1/min_fps with dynamic_fps / a cap below 30), so the game keeps its speed when frames are slower.
    const double slowest = rt.config.dynamic_fps ? MinFps() : std::min(30, MaxFps());
    if (g_game_step) *g_game_step = float(std::clamp(seconds, 1.0 / 60.0, 1.0 / slowest));
    *reinterpret_cast<int64_t*>(self + 0x310) = hle::os::NowTicks();
    self[0x31d] = 0;
    self[0x31e] = 0;
    *reinterpret_cast<double*>(self + 0x320) = 0;
}

// Dark Souls Remastered (Switch) 1.0.3, main build id 5a797a3a5da6c4f3468cb7c84e4dde63...
constexpr const char* kBuildId = "5a797a3a5da6c4f3468cb7c84e4dde63";

void Trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

uintptr_t GuestOffset(const void* p) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const loader::Module& m = GetRuntime().main;
    return m.Contains(a) ? a - m.base() : 0;
}

// main+0x7fea18: engine debug print, (int level, const char* fmt, ...)
void EngineLog(int level, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    Trim(msg);
    if (msg[0]) LOGI("engine", "[%d] %s", level, msg);
}

// main+0x7feb30: same with a va_list
void EngineLogV(int level, const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    Trim(msg);
    if (msg[0]) LOGI("engine", "[%d] %s", level, msg);
}

// main+0x7892d8: DL_PANIC(const char* file, int line, const char* fmt, ...)
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

struct Hook {
    uint64_t offset;
    void* fn;
    const char* name;
};
const Hook kHooks[] = {
    {0x7fea18, reinterpret_cast<void*>(&EngineLog), "engine log"},
    {0x7feb30, reinterpret_cast<void*>(&EngineLogV), "engine vlog"},
    {0x7892d8, reinterpret_cast<void*>(&EnginePanic), "DL_PANIC"},
};

}  // namespace

float GameFrameTime() { return g_game_step ? *g_game_step : 1.0f / 30.0f; }
int FrameRateNow() {
    if (g_movies.load(std::memory_order_relaxed) > 0 || !g_game_step) return 30;  // without the patches the game steps 1/30 s
    const int cap = g_cap_fps.load(std::memory_order_relaxed);
    return cap > 0 ? cap : MaxFps();
}
void SetMoviePlaying(bool playing) {
    const int n = playing ? ++g_movies : --g_movies;
    LOGI("hooks", "movie %s: frame rate %d fps", playing ? "started" : "ended", FrameRateNow());
    (void)n;
}

void InstallGameHooks() {
    Runtime& rt = GetRuntime();
    if (rt.main_nso.build_id_hex.rfind(kBuildId, 0) != 0) {
        LOGW("hooks", "unknown game build %.32s: engine hooks disabled", rt.main_nso.build_id_hex.c_str());
        return;
    }
    // Frame rate (fps other than 30, or dynamic_fps). The same two places a well-known Switch 60 fps mod changes, done differently:
    // 1. The game's per-frame update (main+0x66c99c) steps every system by a constant 1/30 s loaded from
    //    read-only data (main+0x66c9c8 adrp x8, page; +0x66c9cc ldr s8, [x8, #0x970]). It now loads a float in
    //    the scratch page instead.
    // 2. The render thread's frame limiter (main+0x70bd90, called once per game update with a hard-coded 30) is
    //    replaced by FrameLimiter: it paces to the current rate and sets that float to the measured frame period
    //    (1/60 s up to 1/30 s, or up to 1/min_fps s with dynamic_fps), so the game keeps normal speed at 60 fps,
    //    at 20 fps and when heavy areas run slower. It also keeps the game's frame-skip mode off (with it, slow
    //    frames ran two updates per shown frame).
    g_cap_fps.store(MaxFps(), std::memory_order_relaxed);
    if (rt.config.target_fps != 30 || rt.config.dynamic_fps) {
        std::string err;
        float* step = reinterpret_cast<float*>(rt.main.ScratchPage());
        *step = 1.0f / 30.0f;
        const uint32_t adrp = rt.main.EncodeAdrp(0x66c9c8, 8, reinterpret_cast<uintptr_t>(step));
        bool ok = rt.main.PatchInstruction(0x66c9c8, 0xB00080A8, adrp, &err) &&
                  rt.main.PatchInstruction(0x66c9cc, 0xBD497108, 0xBD400108, &err);  // ldr s8, [x8]
        if (ok) ok = rt.main.HookFunction(0x70bd90, reinterpret_cast<void*>(&FrameLimiter), &err);
        if (ok) {
            g_game_step = step;
            LOGI("hooks", "own frame limiter, game time step follows the frame time (max %d fps, dynamic fps %s, min %d fps)",
                 MaxFps(), rt.config.dynamic_fps ? "on" : "off", MinFps());
        } else {
            LOGW("hooks", "frame rate patches failed (%s): staying at 30 fps", err.c_str());
            rt.main.PatchInstruction(0x66c9cc, 0xBD400108, 0xBD497108, &err);  // undo, if the first ones applied
            rt.main.PatchInstruction(0x66c9c8, rt.main.EncodeAdrp(0x66c9c8, 8, reinterpret_cast<uintptr_t>(step)),
                                     0xB00080A8, &err);
            rt.config.target_fps = 30;
            rt.config.dynamic_fps = false;
            g_cap_fps.store(30, std::memory_order_relaxed);
        }
    }
    for (const Hook& h : kHooks) {
        std::string err;
        if (rt.main.HookFunction(h.offset, h.fn, &err))
            LOGI("hooks", "hooked %s at main+0x%lx", h.name, (unsigned long)h.offset);
        else
            LOGW("hooks", "could not hook %s: %s", h.name, err.c_str());
    }
}

}  // namespace dsr
