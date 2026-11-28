// Write tracking of guest heap pages: see write_watch.h.
#include "hle/os/write_watch.h"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>

#include "common/log.h"
#include "runtime/runtime.h"

namespace dsr::hle::os {
namespace {
constexpr const char* kTag = "watch";

// Page states.
constexpr uint8_t kWritable = 0;  // not watched
constexpr uint8_t kWatched = 1;   // read-only; a write faults
constexpr uint8_t kBusy = 2;      // written too often: never watched again (stays writable)
constexpr uint8_t kBusyFaults = 6;  // faults after which a page counts as busy

uintptr_t g_base = 0, g_end = 0;
size_t g_page = 4096;
unsigned g_shift = 12;
std::atomic<uint8_t>* g_state = nullptr;
std::atomic<uint8_t>* g_faults = nullptr;
std::atomic<uint32_t>* g_wepoch = nullptr;  // the epoch at the page's last write
std::atomic<uint32_t> g_epoch{1};
std::atomic<uint64_t> g_stat_faults{0}, g_stat_armed{0}, g_stat_busy{0};
struct sigaction g_old_segv;
bool g_enabled = false;

void* MapZeroed(size_t bytes) {
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

// Makes page i writable again after a write (fault, or announced). Async-signal-safe.
void Release(size_t i, bool count_fault) {
    g_wepoch[i].store(g_epoch.load(std::memory_order_acquire), std::memory_order_release);
    uint8_t s = kWatched;
    if (g_state[i].compare_exchange_strong(s, kWritable, std::memory_order_acq_rel)) {
        mprotect(reinterpret_cast<void*>(g_base + (i << g_shift)), g_page, PROT_READ | PROT_WRITE);
        if (count_fault) {
            g_stat_faults.fetch_add(1, std::memory_order_relaxed);
            const uint8_t f = g_faults[i].load(std::memory_order_relaxed);
            if (f < 255) g_faults[i].store(uint8_t(f + 1), std::memory_order_relaxed);
            if (f + 1 >= kBusyFaults) {
                uint8_t w = kWritable;
                if (g_state[i].compare_exchange_strong(w, kBusy)) g_stat_busy.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void SegvHandler(int sig, siginfo_t* info, void* ctx) {
    const int saved_errno = errno;
    // A write to a protected page is SEGV_ACCERR; some kernels (a OnePlus 6T's, Android 11) report it as
    // SEGV_MAPERR, for a page not touched before it was protected. The heap is mapped in full, so either code at a
    // heap address is ours to look at (otherwise the game crashed in its first copy into a watched buffer).
    if (info && (info->si_code == SEGV_ACCERR || info->si_code == SEGV_MAPERR)) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(info->si_addr);
        if (a >= g_base && a < g_end) {
            const size_t i = (a - g_base) >> g_shift;
            const uint8_t s = g_state[i].load(std::memory_order_acquire);
            if (s == kWatched) {
                Release(i, true);
                errno = saved_errno;
                return;  // the write is retried on a writable page
            }
            // Another thread released it a moment ago (its mprotect may still be running): make sure and retry.
            // A heap page we never protected cannot raise an access fault otherwise.
            if (g_wepoch[i].load(std::memory_order_acquire) != 0 || g_faults[i].load(std::memory_order_relaxed)) {
                mprotect(reinterpret_cast<void*>(g_base + (i << g_shift)), g_page, PROT_READ | PROT_WRITE);
                errno = saved_errno;
                return;
            }
        }
    }
    errno = saved_errno;
    // Not ours: the previous handler (the crash report).
    if (g_old_segv.sa_flags & SA_SIGINFO) {
        if (g_old_segv.sa_sigaction) g_old_segv.sa_sigaction(sig, info, ctx);
        return;
    }
    if (g_old_segv.sa_handler == SIG_IGN) return;
    if (g_old_segv.sa_handler != SIG_DFL && g_old_segv.sa_handler) {
        g_old_segv.sa_handler(sig);
        return;
    }
    signal(SIGSEGV, SIG_DFL);  // default action when the fault repeats
}

// The pages wholly inside [p, p + size), as page indices [*lo, *hi). False when there are none or the range is
// not inside the watched heap.
bool Interior(const void* p, size_t size, size_t* lo, size_t* hi) {
    if (!g_enabled) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < g_base || size > g_end - a || a + size > g_end) return false;
    const uintptr_t first = (a + g_page - 1) & ~uintptr_t(g_page - 1);
    const uintptr_t last = (a + size) & ~uintptr_t(g_page - 1);
    if (last <= first) return false;
    *lo = (first - g_base) >> g_shift;
    *hi = (last - g_base) >> g_shift;
    return true;
}

}  // namespace

void WatchSetHeap(uintptr_t base, size_t reserve) {
    if (g_base || !dsr::GetRuntime().config.geo_watch) return;
    const long ps = sysconf(_SC_PAGESIZE);
    g_page = ps > 0 ? size_t(ps) : 4096;
    g_shift = 0;
    while ((size_t(1) << g_shift) < g_page) g_shift++;
    const size_t pages = reserve >> g_shift;
    g_state = static_cast<std::atomic<uint8_t>*>(MapZeroed(pages));
    g_faults = static_cast<std::atomic<uint8_t>*>(MapZeroed(pages));
    g_wepoch = static_cast<std::atomic<uint32_t>*>(MapZeroed(pages * 4));
    if (!g_state || !g_faults || !g_wepoch) {
        LOGW(kTag, "geometry write tracking off: no memory for its page table");
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = SegvHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_old_segv) != 0) {
        LOGW(kTag, "geometry write tracking off: the fault handler could not be installed");
        return;
    }
    g_base = base;
    g_end = base + reserve;
    g_enabled = true;
    LOGI(kTag, "geometry write tracking on: unchanged vertex / index data is not hashed again (%zu-byte pages)",
         g_page);
}

bool WatchEnabled() { return g_enabled; }

uint64_t WatchArm(const void* p, size_t size) {
    size_t lo, hi;
    if (!Interior(p, size, &lo, &hi)) return 0;
    for (size_t i = lo; i < hi; i++)
        if (g_state[i].load(std::memory_order_relaxed) == kBusy) return 0;
    const uint32_t epoch = g_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Runs of writable pages: marked first, then protected (a write in between is caught by the caller's
    // verification after this returns).
    size_t i = lo;
    while (i < hi) {
        if (g_state[i].load(std::memory_order_acquire) != kWritable) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < hi && g_state[j].load(std::memory_order_acquire) == kWritable) {
            g_state[j].store(kWatched, std::memory_order_release);
            j++;
        }
        if (mprotect(reinterpret_cast<void*>(g_base + (i << g_shift)), (j - i) << g_shift, PROT_READ) != 0) {
            for (size_t k = i; k < j; k++) {
                uint8_t s = kWatched;
                g_state[k].compare_exchange_strong(s, kWritable);
            }
            return 0;
        }
        g_stat_armed.fetch_add(j - i, std::memory_order_relaxed);
        i = j;
    }
    return epoch;
}

bool WatchClean(const void* p, size_t size, uint64_t epoch) {
    size_t lo, hi;
    if (!epoch || !Interior(p, size, &lo, &hi)) return false;
    for (size_t i = lo; i < hi; i++) {
        if (g_state[i].load(std::memory_order_acquire) != kWatched) return false;
        if (g_wepoch[i].load(std::memory_order_acquire) >= epoch) return false;
    }
    return true;
}

void WatchBeforeWrite(const void* p, size_t size) {
    if (!g_enabled || size == 0) return;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a + size <= g_base || a >= g_end) return;
    const uintptr_t s = std::max(a, g_base), e = std::min(a + size, g_end);
    for (size_t i = (s - g_base) >> g_shift, n = ((e - 1 - g_base) >> g_shift) + 1; i < n; i++)
        if (g_state[i].load(std::memory_order_acquire) == kWatched) Release(i, false);
}

void WatchEdges(const void* p, size_t size, WatchEdge out[2], int* n) {
    *n = 0;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const uintptr_t first = (a + g_page - 1) & ~uintptr_t(g_page - 1);
    const uintptr_t last = (a + size) & ~uintptr_t(g_page - 1);
    if (last <= first) {  // no whole page: all of it
        out[(*n)++] = {0, size};
        return;
    }
    if (first > a) out[(*n)++] = {0, size_t(first - a)};
    if (a + size > last) out[(*n)++] = {size_t(last - a), size_t(a + size - last)};
}

WatchStats WatchTakeStats() {
    WatchStats s;
    s.faults = g_stat_faults.exchange(0, std::memory_order_relaxed);
    s.armed_pages = g_stat_armed.exchange(0, std::memory_order_relaxed);
    s.busy_pages = g_stat_busy.load(std::memory_order_relaxed);
    return s;
}

}  // namespace dsr::hle::os
