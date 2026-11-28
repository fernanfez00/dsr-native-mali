#include "common/profiler.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "runtime/runtime.h"

#if defined(__aarch64__) && defined(__linux__)
#include <ucontext.h>
#define DSR_PROFILER_SUPPORTED 1
#endif

namespace dsr::profiler {
namespace {
constexpr const char* kTag = "profile";
constexpr int kMaxThreads = 64;
constexpr int kDepth = 16;
constexpr size_t kMaxSamples = 1 << 16;

struct ThreadSlot {
    std::atomic<int> tid{0};
    char name[32] = {};
    uintptr_t stack_lo = 0, stack_hi = 0;
    timer_t timer{};
    bool timer_ok = false;
};
struct Sample {
    int16_t slot;
    uint8_t depth;
    int64_t t_ns;  // CLOCK_MONOTONIC when taken
    uintptr_t pc[kDepth];  // pc, then return addresses (frame-pointer walk)
};

ThreadSlot g_slots[kMaxThreads];
std::atomic<int> g_nslots{0};
Sample* g_samples = nullptr;  // ring written by the signal handler
std::atomic<size_t> g_write{0};
std::atomic<bool> g_enabled{false};
std::mutex g_mutex;

int Gettid() { return int(syscall(SYS_gettid)); }

int SlotOf(int tid) {
    const int n = g_nslots.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_slots[i].tid.load(std::memory_order_relaxed) == tid) return i;
    return -1;
}

#ifdef DSR_PROFILER_SUPPORTED
// Async-signal-safe: no locks, no allocation, no TLS (guest code may be running on this thread).
void OnSignal(int, siginfo_t*, void* uc_) {
    const int slot = SlotOf(Gettid());
    if (slot < 0 || !g_samples) return;
    const auto* uc = static_cast<const ucontext_t*>(uc_);
    const size_t at = g_write.fetch_add(1, std::memory_order_relaxed) & (kMaxSamples - 1);
    Sample& s = g_samples[at];
    s.slot = int16_t(slot);
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);  // async-signal-safe
    s.t_ns = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
    s.pc[0] = uintptr_t(uc->uc_mcontext.pc);
    int d = 1;
    s.pc[d++] = uintptr_t(uc->uc_mcontext.regs[30]);  // lr: the caller, when pc is in a leaf
    uintptr_t fp = uintptr_t(uc->uc_mcontext.regs[29]);
    const uintptr_t lo = g_slots[slot].stack_lo, hi = g_slots[slot].stack_hi;
    while (d < kDepth && fp >= lo && fp + 16 <= hi && (fp & 7) == 0) {
        const uintptr_t next = reinterpret_cast<const uintptr_t*>(fp)[0];
        const uintptr_t ret = reinterpret_cast<const uintptr_t*>(fp)[1];
        if (!ret) break;
        s.pc[d++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    s.depth = uint8_t(d);
}
#endif

// ---- symbolization (reporting thread) ----
struct Where {
    int kind;  // 0 game (main), 1 our library, 2 other library, 3 unknown
    uintptr_t off;
    std::string label;
};
std::unordered_map<uintptr_t, Where> g_where_cache;
uintptr_t g_self_base = 0;

Where Locate(uintptr_t a) {
    auto it = g_where_cache.find(a);
    if (it != g_where_cache.end()) return it->second;
    Where w{3, a, ""};
    const loader::Module& m = GetRuntime().main;
    char buf[160];
    if (m.base() && m.Contains(a)) {
        w = {0, a - m.base(), ""};
        snprintf(buf, sizeof(buf), "main+0x%lx", (unsigned long)(a - m.base()));
        w.label = buf;
    } else {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(a), &info) && info.dli_fname) {
            const uintptr_t base = uintptr_t(info.dli_fbase);
            const char* file = strrchr(info.dli_fname, '/');
            file = file ? file + 1 : info.dli_fname;
            if (base == g_self_base) {
                w = {1, a - base, ""};
                snprintf(buf, sizeof(buf), "dsr+0x%lx", (unsigned long)(a - base));
            } else {
                w = {2, a - base, ""};
                snprintf(buf, sizeof(buf), "%s+0x%lx", file, (unsigned long)(a - base));
            }
            w.label = buf;
            if (info.dli_sname) {
                w.label += " ";
                w.label += std::string(info.dli_sname).substr(0, 60);
            }
        } else {
            snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)a);
            w.label = buf;
        }
    }
    if (g_where_cache.size() > 200000) g_where_cache.clear();
    g_where_cache[a] = w;
    return w;
}

void Report() {
    std::vector<Sample> samples;
    {
        const size_t end = g_write.load();
        static size_t read = 0;
        const size_t begin = end - read > kMaxSamples ? end - kMaxSamples : read;
        for (size_t i = begin; i < end; i++) samples.push_back(g_samples[i & (kMaxSamples - 1)]);
        read = end;
    }
    if (samples.empty()) return;
    const int n = g_nslots.load();
    for (int slot = 0; slot < n; slot++) {
        std::map<std::string, int> self, ours, game, libs, sys;
        int total = 0, sys_total = 0;
        for (const Sample& s : samples) {
            if (s.slot != slot) continue;
            total++;
            // Self: the pc, labelled down to the function where our symbols allow it (rounded to 64 bytes so
            // nearby instructions add up; exact offsets are not needed to find a function).
            Where w = Locate(s.pc[0]);
            {  // by library: the part of the label before '+'
                const size_t plus = w.label.find('+');
                libs[w.kind == 0 ? "game" : w.kind == 1 ? "ours" : plus != std::string::npos ? w.label.substr(0, plus) : "?"]++;
            }
            self[w.kind == 1 || w.kind == 0 ? Locate(s.pc[0] & ~uintptr_t(63)).label : w.label]++;
            // Samples in a system call: which of our functions (or the game's) made it.
            if (w.label.find("syscall") != std::string::npos || w.label.find("ioctl") != std::string::npos ||
                w.label.find("futex") != std::string::npos || w.label.find("sched_yield") != std::string::npos) {
                sys_total++;
                for (int d = 1; d < s.depth; d++) {
                    const Where x = Locate(s.pc[d] - 4);
                    if (x.kind == 0 || x.kind == 1) {
                        sys[Locate((s.pc[d] - 4) & ~uintptr_t(63)).label]++;
                        break;
                    }
                }
            }
            bool got_ours = false, got_game = false;
            for (int d = 0; d < s.depth && (!got_ours || !got_game); d++) {
                const uintptr_t a = d == 0 ? s.pc[0] : s.pc[d] - 4;  // return address -> the call
                const Where x = Locate(a);
                if (x.kind == 1 && !got_ours) {
                    ours[Locate(a & ~uintptr_t(63)).label]++;
                    got_ours = true;
                }
                if (x.kind == 0 && !got_game) {
                    game[Locate(a & ~uintptr_t(63)).label]++;
                    got_game = true;
                }
            }
        }
        if (total < 50) continue;
        // Current name (game threads are named after they start) and exact CPU time since the last report.
        const int tid = g_slots[slot].tid.load();
        char path[64], name[32] = {};
        snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        if (FILE* f = fopen(path, "r")) {
            if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
            fclose(f);
        }
        if (name[0]) snprintf(g_slots[slot].name, sizeof(g_slots[slot].name), "%s", name);
        double cpu_s = -1;
        snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
        if (FILE* f = fopen(path, "r")) {
            unsigned long long ns = 0;
            if (fscanf(f, "%llu", &ns) == 1) {
                static unsigned long long last_ns[kMaxThreads] = {};
                if (last_ns[slot]) cpu_s = (ns - last_ns[slot]) / 1e9;
                last_ns[slot] = ns;
            }
            fclose(f);
        }
        auto top = [&](const char* what, const std::map<std::string, int>& m) {
            std::vector<std::pair<int, std::string>> v;
            for (auto& [k, c] : m) v.push_back({c, k});
            std::sort(v.rbegin(), v.rend());
            std::string line;
            for (size_t i = 0; i < v.size() && i < 14; i++) {
                char b[200];
                snprintf(b, sizeof(b), "%s%.1f%% %s", i ? " | " : "", 100.0 * v[i].first / total, v[i].second.c_str());
                line += b;
            }
            LOGI(kTag, "  %s: %s", what, line.c_str());
        };
        // The CPU it last ran on (field 39 of /proc/.../stat), to see which cores the busy threads get.
        int cpu = -1;
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        if (FILE* f = fopen(path, "r")) {
            char buf[1024];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            const char* p = strrchr(buf, ')');  // the name may contain spaces
            for (int field = 2; p && *p && field < 39; p++)
                if (*p == ' ') field++;
            if (p) cpu = atoi(p);
        }
        LOGI(kTag, "thread '%s' (tid %d, last on CPU %d): %d samples, %.1f s of CPU in the last 10 s", g_slots[slot].name,
             tid, cpu, total, cpu_s);
        top("by library", libs);
        top("self", self);
        top("in our code", ours);
        top("in game code", game);
        if (sys_total >= 10) top("system calls made from", sys);
    }
}

// ---- slow frames (NoteSlowFrame) ----
struct SlowFrame {
    int64_t from, to;
    double ms;
    const char* what;  // a string literal
};
std::mutex g_slow_mutex;
std::vector<SlowFrame> g_slow;

// ---- blocked spans (NoteBlockedSpan) ----
struct BlockedSpan {
    int64_t from, to;
    uintptr_t where;
    char who[24];
    char what[200];
};
constexpr size_t kMaxSpans = 1024;
std::mutex g_span_mutex;
BlockedSpan g_spans[kMaxSpans];
size_t g_span_next = 0;
std::atomic<void (*)()> g_report_hook{nullptr};

// The spans that covered much of [from, to]: a thread waiting through most of the frame (but not a worker that
// idles for seconds anyway), or any slow file system call in it.
std::string BlockedIn(int64_t from, int64_t to) {
    struct Hit {
        int64_t overlap;
        BlockedSpan s;
    };
    std::vector<Hit> hits;
    const int64_t len = to - from;
    {
        std::lock_guard<std::mutex> lk(g_span_mutex);
        const size_t n = std::min(g_span_next, kMaxSpans);
        for (size_t i = 0; i < n; i++) {
            const BlockedSpan& b = g_spans[i];
            const int64_t ov = std::min(b.to, to) - std::max(b.from, from);
            if (ov <= 0) continue;
            const bool fs = strncmp(b.what, "fs ", 3) == 0;
            if (!fs && (ov * 10 < len * 4 || b.to - b.from > len * 4 + 50'000'000)) continue;
            hits.push_back({ov, b});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.overlap > b.overlap; });
    std::string out;
    for (size_t i = 0; i < hits.size() && i < 6; i++) {
        const BlockedSpan& b = hits[i].s;
        char buf[400];
        snprintf(buf, sizeof(buf), "%s%s %.0f ms in %s (from %s, %.0f ms before the frame's end)", out.empty() ? "" : " | ",
                 b.who, (b.to - b.from) / 1e6, b.what, b.where ? Locate(b.where).label.c_str() : "?",
                 (to - b.to) / 1e6);
        out += buf;
    }
    return out;
}

void ReportSlowFrames() {
    std::vector<SlowFrame> todo;
    {
        std::lock_guard<std::mutex> lk(g_slow_mutex);
        todo.swap(g_slow);
    }
    if (todo.empty()) return;
    const size_t end = g_write.load();
    const size_t begin = end > kMaxSamples ? end - kMaxSamples : 0;
    for (const SlowFrame& f : todo) {
        // Per thread: CPU samples in the window (each ~2 ms of its CPU time) and where in the game's code (or
        // ours) they were.
        const int n = g_nslots.load();
        std::vector<int> count(n, 0);
        std::vector<std::map<std::string, int>> where(n);
        for (size_t i = begin; i < end; i++) {
            const Sample& s = g_samples[i & (kMaxSamples - 1)];
            if (s.t_ns < f.from || s.t_ns > f.to || s.slot < 0 || s.slot >= n) continue;
            count[s.slot]++;
            // The game's frame, and when the CPU was outside the game's code at that moment: where (the innermost
            // place) and the outermost frame of ours on the way (which of our functions the game had called).
            std::string label;
            int game_depth = -1;
            for (int d = 0; d < s.depth && label.empty(); d++) {
                const uintptr_t a = d == 0 ? s.pc[0] : s.pc[d] - 4;
                const Where x = Locate(a);
                if (x.kind == 0) {
                    label = Locate(a & ~uintptr_t(63)).label;
                    game_depth = d;
                }
            }
            if (label.empty()) label = Locate(s.pc[0] & ~uintptr_t(63)).label;
            if (game_depth > 0) {
                std::string inner = Locate(s.pc[0] & ~uintptr_t(63)).label, outer;
                for (int d = game_depth - 1; d >= 1 && outer.empty(); d--) {
                    const Where x = Locate(s.pc[d] - 4);
                    if (x.kind == 1) outer = Locate((s.pc[d] - 4) & ~uintptr_t(63)).label;
                }
                label += " (in " + inner + (outer.empty() || outer == inner ? "" : " via " + outer) + ")";
            }
            where[s.slot][label]++;
        }
        std::string line;
        for (int slot = 0; slot < n; slot++) {
            if (count[slot] < 2) continue;
            std::vector<std::pair<int, std::string>> v;
            for (auto& [k, c] : where[slot]) v.push_back({c, k});
            std::sort(v.rbegin(), v.rend());
            char b[1024];
            int len = snprintf(b, sizeof(b), "%s%s ~%d ms CPU [", line.empty() ? "" : " | ", g_slots[slot].name,
                               count[slot] * 2);
            for (size_t i = 0; i < v.size() && i < 3 && len < int(sizeof(b)) - 240; i++)
                len += snprintf(b + len, sizeof(b) - len, "%s%s x%d", i ? ", " : "", v[i].second.c_str(), v[i].first);
            snprintf(b + len, sizeof(b) - len, "]");
            line += b;
        }
        LOGI(kTag, "%s (%.1f ms): %s", f.what, f.ms, line.empty() ? "no thread used the CPU much (waiting?)" : line.c_str());
        const std::string blocked = BlockedIn(f.from, f.to);
        if (!blocked.empty()) LOGI(kTag, "  blocked during it: %s", blocked.c_str());
    }
}

void StartReporter() {
    static bool started = false;
    if (started) return;
    started = true;
    std::thread([] {
        for (int i = 1;; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ReportSlowFrames();
            if (i % 10 == 0) {
                Report();
                if (auto hook = g_report_hook.load()) hook();
            }
        }
    }).detach();
}

void StartTimer(ThreadSlot& t) {
#ifdef DSR_PROFILER_SUPPORTED
    sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
#ifdef sigev_notify_thread_id
    sev.sigev_notify_thread_id = t.tid.load();
#else
    sev._sigev_un._tid = t.tid.load();  // glibc spells the field this way
#endif
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &t.timer) != 0) return;
    itimerspec its{};
    its.it_interval.tv_nsec = 2'000'000;
    its.it_value.tv_nsec = 2'000'000;
    t.timer_ok = timer_settime(t.timer, 0, &its, nullptr) == 0;
#else
    (void)t;
#endif
}
}  // namespace

bool Enabled() { return g_enabled.load(); }

void NoteSlowFrame(int64_t from_ns, int64_t to_ns, double ms, const char* what) {
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lk(g_slow_mutex);
    if (g_slow.size() < 32) g_slow.push_back({from_ns, to_ns, ms, what});
}

void NoteBlockedSpan(const char* who, const char* what, uintptr_t where, int64_t from_ns, int64_t to_ns) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_span_mutex);
    BlockedSpan& b = g_spans[g_span_next++ % kMaxSpans];
    b.from = from_ns;
    b.to = to_ns;
    b.where = where;
    snprintf(b.who, sizeof(b.who), "%s", who ? who : "?");
    snprintf(b.what, sizeof(b.what), "%s", what ? what : "?");
}

void SetReportHook(void (*hook)()) { g_report_hook.store(hook); }

void Enable() {
#ifdef DSR_PROFILER_SUPPORTED
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_enabled.exchange(true)) return;
    g_samples = new Sample[kMaxSamples]();
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&Enable), &info)) g_self_base = uintptr_t(info.dli_fbase);
    struct sigaction sa {};
    sa.sa_sigaction = OnSignal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    StartReporter();
    LOGI(kTag, "sampling profiler on (500 Hz of CPU time per registered thread); our library at %p",
         reinterpret_cast<void*>(g_self_base));
#endif
}

void RegisterCurrentThread(const char* name) {
#ifdef DSR_PROFILER_SUPPORTED
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    const int tid = Gettid();
    if (SlotOf(tid) >= 0) return;
    const int i = g_nslots.load();
    if (i >= kMaxThreads) return;
    ThreadSlot& t = g_slots[i];
    snprintf(t.name, sizeof(t.name), "%s", name ? name : "?");
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* lo = nullptr;
        size_t sz = 0;
        pthread_attr_getstack(&attr, &lo, &sz);
        t.stack_lo = uintptr_t(lo);
        t.stack_hi = uintptr_t(lo) + sz;
        pthread_attr_destroy(&attr);
    }
    t.tid.store(tid);
    g_nslots.store(i + 1, std::memory_order_release);
    StartTimer(t);
#else
    (void)name;
#endif
}

}  // namespace dsr::profiler
