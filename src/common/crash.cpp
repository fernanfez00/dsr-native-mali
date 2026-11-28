// Crash reporting. Everything in the signal handler is async-signal-safe: no malloc, no stdio, only
// write() into a stack buffer.
#include "common/crash.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include <atomic>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "common/log.h"

namespace dsr {
namespace {

struct ModuleInfo {
    const char* name;
    uintptr_t base;
    size_t size;
};
constexpr int kMaxModules = 8;
ModuleInfo g_modules[kMaxModules];
volatile int g_num_modules = 0;

// Crash notes (SetCrashNote): two buffers, the handler reads the one published last.
struct Note {
    char path[512];
    char text[4096];
    size_t len;
    int64_t until_ns;
};
Note g_notes[2];
std::atomic<int> g_note{-1};

void WriteNote() {
    const int i = g_note.load();
    if (i < 0) return;
    const Note& n = g_notes[i];
    if (MonotonicNs() >= n.until_ns) return;
    const int fd = open(n.path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    ssize_t r = write(fd, n.text, n.len);
    (void)r;
    close(fd);
}

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
struct sigaction g_old[sizeof(kSignals) / sizeof(kSignals[0])];
volatile sig_atomic_t g_in_handler = 0;

// ---- tiny async-signal-safe string builder ----------------------------------------------------------
struct Buf {
    char data[8192];
    size_t len = 0;
    void Str(const char* s) {
        while (*s && len < sizeof(data) - 1) data[len++] = *s++;
    }
    void Hex(uint64_t v, int digits = 16) {
        char tmp[17];
        for (int i = digits - 1; i >= 0; i--) {
            tmp[i] = "0123456789abcdef"[v & 0xF];
            v >>= 4;
        }
        tmp[digits] = 0;
        Str("0x");
        Str(tmp);
    }
    void Dec(int64_t v) {
        char tmp[24];
        int i = 23;
        tmp[i] = 0;
        bool neg = v < 0;
        uint64_t u = neg ? uint64_t(-v) : uint64_t(v);
        do {
            tmp[--i] = char('0' + u % 10);
            u /= 10;
        } while (u);
        if (neg) tmp[--i] = '-';
        Str(tmp + i);
    }
    // "0x... (main+0x1234)" when the address is inside a registered module
    void Addr(uint64_t a) {
        Hex(a);
        for (int i = 0; i < g_num_modules; i++) {
            const ModuleInfo& m = g_modules[i];
            if (a >= m.base && a < m.base + m.size) {
                Str(" (");
                Str(m.name);
                Str("+");
                Hex(a - m.base, 8);
                Str(")");
                break;
            }
        }
    }
};

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGBUS: return "SIGBUS (bus error / misaligned access)";
        case SIGILL: return "SIGILL (illegal instruction)";
        case SIGFPE: return "SIGFPE (arithmetic error)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGTRAP: return "SIGTRAP (breakpoint / trap)";
        default: return "signal";
    }
}

void Emit(const Buf& b) {
    int fd = LogFileFd();
    if (fd >= 0) {
        ssize_t r = write(fd, b.data, b.len);
        (void)r;
    }
#if defined(__ANDROID__)
    // __android_log_write is not formally async-signal-safe but is what debuggerd-style handlers use.
    char copy[sizeof(b.data)];
    memcpy(copy, b.data, b.len);
    copy[b.len] = 0;
    __android_log_write(ANDROID_LOG_FATAL, "DSR/crash", copy);
#else
    ssize_t r = write(STDERR_FILENO, b.data, b.len);
    (void)r;
#endif
}

// The memory map around the fault address (/proc/self/maps, read with plain system calls): the mapping holding it,
// or the ones on either side of the hole it is in, and how many mappings the process has (Linux refuses new ones
// past ~65530: memory protection changes split them). A SIGSEGV "SEGV_MAPERR" at an address inside the guest heap
// on a OnePlus 6T had no explanation without it.
void MapsAround(Buf& b, uint64_t addr) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char chunk[4096], line[256], prev[256] = "", hit[256] = "", next[256] = "";
    size_t ll = 0;
    long count = 0;
    bool found_hit = false, want_next = false;
    auto hexval = [](const char*& p) {
        uint64_t v = 0;
        for (;; p++) {
            const char c = *p;
            if (c >= '0' && c <= '9') v = v * 16 + uint64_t(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + uint64_t(c - 'a' + 10);
            else break;
        }
        return v;
    };
    auto copy = [](char* dst, const char* src) {
        size_t i = 0;
        for (; src[i] && i < 255; i++) dst[i] = src[i];
        dst[i] = 0;
    };
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            const char c = chunk[i];
            if (c != '\n') {
                if (ll < sizeof(line) - 1) line[ll++] = c;
                continue;
            }
            line[ll] = 0;
            ll = 0;
            count++;
            const char* p = line;
            const uint64_t lo = hexval(p);
            if (*p == '-') p++;
            const uint64_t hi = hexval(p);
            if (want_next) {
                copy(next, line);
                want_next = false;
            }
            if (!found_hit && addr < hi) {
                found_hit = true;
                if (addr >= lo) copy(hit, line);
                else copy(next, line);  // in a hole: this is the mapping after it
                if (addr >= lo) want_next = true;
            } else if (!found_hit) {
                copy(prev, line);
            }
        }
    }
    close(fd);
    b.Str("\nmemory map near the fault (");
    b.Dec(count);
    b.Str(" mappings):\n");
    if (prev[0]) {
        b.Str("  before: ");
        b.Str(prev);
        b.Str("\n");
    }
    if (hit[0]) {
        b.Str("  holding it: ");
        b.Str(hit);
        b.Str("\n");
    } else {
        b.Str("  (not mapped: in the hole between these)\n");
    }
    if (next[0]) {
        b.Str("  after: ");
        b.Str(next);
        b.Str("\n");
    }
}

void Handler(int sig, siginfo_t* info, void* ctx) {
    if (!g_in_handler) {
        g_in_handler = 1;
        Buf b;
        char thread[16] = "?";
        pthread_getname_np(pthread_self(), thread, sizeof(thread));
        b.Str("\n================ CRASH ================\n");
        b.Str("signal: ");
        b.Str(SignalName(sig));
        b.Str("  code ");
        b.Dec(info ? info->si_code : 0);
        b.Str("\nthread: ");
        b.Str(thread);
        b.Str("  tid ");
        b.Dec(gettid());
        if (sig == SIGSEGV || sig == SIGBUS) {
            b.Str("\nfault address: ");
            b.Addr(reinterpret_cast<uint64_t>(info ? info->si_addr : nullptr));
        }
#if defined(__aarch64__)
        const auto* uc = static_cast<const ucontext_t*>(ctx);
        const auto& mc = uc->uc_mcontext;
        b.Str("\npc: ");
        b.Addr(mc.pc);
        b.Str("\nlr: ");
        b.Addr(mc.regs[30]);
        b.Str("\nsp: ");
        b.Hex(mc.sp);
        b.Str("\n");
        for (int i = 0; i < 30; i++) {
            b.Str(i < 10 ? "x" : "x");
            b.Dec(i);
            b.Str(i < 10 ? ":  " : ": ");
            b.Addr(mc.regs[i]);
            b.Str((i % 2) ? "\n" : "    ");
        }
        // Return addresses on the stack that point into guest code: a cheap backtrace for frame-pointer
        // based guest code (the Switch toolchain keeps x29 frame records).
        b.Str("\nframe chain (x29):\n");
        uint64_t fp = mc.regs[29];
        for (int depth = 0; depth < 24 && fp && (fp & 0xF) == 0; depth++) {
            // Only follow frames inside the current stack area to avoid faulting in the handler.
            if (fp < mc.sp || fp - mc.sp > (64u << 20)) break;
            const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
            b.Str("  #");
            b.Dec(depth);
            b.Str(" ");
            b.Addr(rec[1]);
            b.Str("\n");
            if (rec[0] <= fp) break;
            fp = rec[0];
        }
#else
        (void)ctx;
        b.Str("\n");
#endif
        if ((sig == SIGSEGV || sig == SIGBUS) && info) MapsAround(b, reinterpret_cast<uint64_t>(info->si_addr));
        b.Str("=======================================\n");
        Emit(b);
        WriteNote();
    }
    // Hand over to the previous handler (debuggerd tombstone, etc.).
    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++)
        if (kSignals[i] == sig) sigaction(sig, &g_old[i], nullptr);
    // Faults re-trigger when the instruction re-executes; re-raise the others explicitly.
    if (sig == SIGABRT || sig == SIGTRAP || (info && info->si_code <= 0)) raise(sig);
}

}  // namespace

void InstallCrashHandler() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++) sigaction(kSignals[i], &sa, &g_old[i]);
}

int64_t MonotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void SetCrashNote(const char* path, const char* text, size_t len, int64_t until_ns) {
    static std::atomic<int> writers{0};
    if (writers.fetch_add(1) != 0) {  // one at a time (the cheats' lock already ensures it)
        writers.fetch_sub(1);
        return;
    }
    const int cur = g_note.load(), next = cur == 0 ? 1 : 0;
    Note& n = g_notes[next];
    strncpy(n.path, path, sizeof(n.path) - 1);
    n.path[sizeof(n.path) - 1] = 0;
    n.len = len < sizeof(n.text) ? len : sizeof(n.text);
    memcpy(n.text, text, n.len);
    n.until_ns = until_ns;
    g_note.store(next);
    writers.fetch_sub(1);
}

void ClearCrashNote() { g_note.store(-1); }

void RegisterCrashModule(const char* name, uintptr_t base, size_t size) {
    int n = g_num_modules;
    if (n >= kMaxModules) return;
    g_modules[n] = {name, base, size};
    g_num_modules = n + 1;
}

}  // namespace dsr
