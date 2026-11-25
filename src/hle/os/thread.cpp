// nn::os threads and thread-local storage.
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <chrono>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/platform.h"
#include "arch/arch.h"
#include "common/log.h"
#include "common/profiler.h"
#include "hle/os/os.h"
#include "hle/registry.h"
#include "runtime/runtime.h"

namespace dsr::hle::os {

namespace {
constexpr const char* kTag = "os.thread";
constexpr uint32_t kResultSuccess = 0;
constexpr uint32_t kResultOutOfResource = 0x1003;  // module 3 (os), desc 8 (approximation)
constexpr size_t kHostStackExtra = 256 * 1024;      // headroom for host code (drivers, logging) on guest threads
constexpr int kTlsSlots = 32;

enum class State : int { Created, Started, Exited };

struct HostThread {
    GuestThread* guest = nullptr;
    const void* entry = nullptr;
    void* arg = nullptr;
    size_t stack_size = 0;
    int priority = 16;
    int ideal_core = 0;
    uint64_t id = 0;
    std::string name;
    const char* name_ptr = nullptr;  // guest-provided pointer (SetThreadNamePointer)
    pthread_t pthread{};
    uintptr_t stack_lo = 0, stack_hi = 0;
    std::mutex m;
    std::atomic<uint32_t> state{0};
    std::atomic<bool> poller{false};  // see MarkPoller
};

std::mutex g_threads_mutex;
std::unordered_map<GuestThread*, std::unique_ptr<HostThread>> g_threads;
std::atomic<uint64_t> g_next_id{1};

thread_local HostThread* t_current = nullptr;
thread_local uintptr_t t_tls_values[kTlsSlots];

struct TlsSlotInfo {
    bool used = false;
    const void* destructor = nullptr;  // guest function void(uintptr_t)
};
std::mutex g_tls_mutex;
TlsSlotInfo g_tls_slots[kTlsSlots];

HostThread* Lookup(GuestThread* t) {
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    auto it = g_threads.find(t);
    return it == g_threads.end() ? nullptr : it->second.get();
}

void QueryStack(HostThread* ht) {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* lo;
        size_t sz;
        pthread_attr_getstack(&attr, &lo, &sz);
        ht->stack_lo = reinterpret_cast<uintptr_t>(lo);
        ht->stack_hi = ht->stack_lo + sz;
        pthread_attr_destroy(&attr);
    }
}

void RunTlsDestructors() {
    for (int i = 0; i < kTlsSlots; i++) {
        const void* dtor;
        {
            std::lock_guard<std::mutex> lk(g_tls_mutex);
            dtor = g_tls_slots[i].used ? g_tls_slots[i].destructor : nullptr;
        }
        if (dtor && t_tls_values[i]) {
            uintptr_t v = t_tls_values[i];
            t_tls_values[i] = 0;
            arch::GuestCall<void>(dtor, v);
        }
    }
}

// The online-services scheduler thread ("NexusRevolution") polls its job list in a loop of very short sleeps: about a
// third of a core all the time (Helio G80), for services that are never reached here. Its sleeps are stretched to
// kPollerSleepNs and it runs at a low priority, so it stays off the cores and power the game needs.
constexpr int64_t kPollerSleepNs = 5'000'000;
// The online-services library names its threads "NexusRevolution Event", "... Socket", "... Surveillance" (an exact
// "NexusRevolution" never matched, so this was never on). Logs from a Mali phone with two fast cores: the Socket
// thread never sleeps (no SleepThread call: it loops over its timers reading the clock) and took 4-6 s of CPU per
// 10 s; on a slower core its clock reads are too far apart for GetSystemTick's back-to-back rule to catch it.
void MarkPoller(HostThread* ht) {
    if (ht->name.rfind("NexusRevolution", 0) == 0) ht->poller.store(true, std::memory_order_relaxed);
}
thread_local int64_t t_poller_run_start = 0;  // poller thread: when it last slept or napped (ticks)
// On the poller thread itself, before it sleeps: lowers its priority once, returns the sleep to use.
int64_t PollerSleep(int64_t ns) {
    thread_local bool lowered = false;
    if (!lowered) {
        lowered = true;
        setpriority(PRIO_PROCESS, 0, 10);
        LOGI(kTag, "'%s': low priority, sleeps of under %.0f ms stretched to %.0f ms (online-services polling)",
             t_current->name.c_str(), kPollerSleepNs / 1e6, kPollerSleepNs / 1e6);
    }
    return std::max(ns, kPollerSleepNs);
}

void* ThreadTrampoline(void* p) {
    auto* ht = static_cast<HostThread*>(p);
    t_current = ht;
    QueryStack(ht);
    pthread_setname_np(pthread_self(), ht->name.empty() ? "guest" : ht->name.substr(0, 15).c_str());
    MarkPoller(ht);
    dsr::platform::ReleaseInheritedPin();  // created by the pinned main thread (few fast cores: not pinned)
    dsr::profiler::RegisterCurrentThread(ht->name.empty() ? "guest" : ht->name.c_str());
    LOGD(kTag, "thread %llu '%s' start entry=%p arg=%p", (unsigned long long)ht->id, ht->name.c_str(), ht->entry,
         ht->arg);
    arch::GuestCall<void>(ht->entry, ht->arg);
    RunTlsDestructors();
    ht->state.store(uint32_t(State::Exited), std::memory_order_release);
    FutexWake(&ht->state, INT32_MAX);
    LOGD(kTag, "thread %llu exited", (unsigned long long)ht->id);
    return nullptr;
}

uint32_t CreateThreadImpl(GuestThread* t, const void* entry, void* arg, void* stack, size_t stack_size, int prio,
                          int core) {
    (void)stack;  // the guest stack is not used: bionic needs its own guard/TLS layout
    auto ht = std::make_unique<HostThread>();
    ht->guest = t;
    ht->entry = entry;
    ht->arg = arg;
    ht->stack_size = stack_size;
    ht->priority = prio;
    ht->ideal_core = core;
    ht->id = g_next_id.fetch_add(1);
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    g_threads[t] = std::move(ht);
    return kResultSuccess;
}
}  // namespace

GuestThread* CurrentThread() {
    if (!t_current) {
        // Adopt foreign threads lazily (e.g. host callback threads that call into guest code).
        auto* buf = calloc(1, 0x200);
        auto ht = std::make_unique<HostThread>();
        ht->guest = buf;
        ht->id = g_next_id.fetch_add(1);
        ht->name = "adopted";
        ht->state = uint32_t(State::Started);
        QueryStack(ht.get());
        t_current = ht.get();
        std::lock_guard<std::mutex> lk(g_threads_mutex);
        g_threads[buf] = std::move(ht);
    }
    return t_current->guest;
}

void AdoptMainThread(const char* name, size_t stack_size, int priority) {
    dsr::profiler::RegisterCurrentThread(name);
    GuestThread* t = CurrentThread();
    t_current->name = name;
    t_current->stack_size = stack_size;
    t_current->priority = priority;
    (void)t;
}

}  // namespace dsr::hle::os

using namespace dsr::hle::os;
using dsr::hle::os::CurrentThread;

// ------------------------------------------------------------------------------------------------
HLE_FUNC(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi, uint32_t, nn_os_CreateThread,
         (GuestThread * t, const void* entry, void* arg, void* stack, size_t size, int prio)) {
    return CreateThreadImpl(t, entry, arg, stack, size, prio, 0);
}

HLE_FUNC(_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii, uint32_t, nn_os_CreateThreadCore,
         (GuestThread * t, const void* entry, void* arg, void* stack, size_t size, int prio, int core)) {
    return CreateThreadImpl(t, entry, arg, stack, size, prio, core);
}

HLE_FUNC(nnosCreateThread, uint32_t, nnosCreateThread,
         (GuestThread * t, const void* entry, void* arg, void* stack, size_t size, int prio, int core)) {
    return CreateThreadImpl(t, entry, arg, stack, size, prio, core);
}

HLE_FUNC(_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE, void, nn_os_StartThread, (GuestThread * t)) {
    HostThread* ht = Lookup(t);
    if (!ht) FATAL("os.thread", "StartThread on unknown thread %p", t);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::max<size_t>(ht->stack_size, 64 * 1024) + kHostStackExtra);
    ht->state.store(uint32_t(State::Started));
    if (pthread_create(&ht->pthread, &attr, ThreadTrampoline, ht) != 0)
        FATAL("os.thread", "pthread_create failed for '%s'", ht->name.c_str());
    pthread_attr_destroy(&attr);
}
HLE_ALIAS(nnosStartThread, nn_os_StartThread);

HLE_FUNC(_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE, void, nn_os_WaitThread, (GuestThread * t)) {
    HostThread* ht = Lookup(t);
    if (!ht) return;
    uint32_t s;
    while ((s = ht->state.load(std::memory_order_acquire)) != uint32_t(State::Exited)) FutexWait(&ht->state, s, -1);
}
HLE_ALIAS(nnosWaitThread, nn_os_WaitThread);

HLE_FUNC(_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE, void, nn_os_DestroyThread, (GuestThread * t)) {
    HostThread* ht = Lookup(t);
    if (!ht) return;
    if (ht->state.load() != uint32_t(State::Created)) {
        nn_os_WaitThread(t);
        pthread_join(ht->pthread, nullptr);
    }
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    g_threads.erase(t);
}
HLE_ALIAS(nnosDestroyThread, nn_os_DestroyThread);

HLE_FUNC(_ZN2nn2os16GetCurrentThreadEv, GuestThread*, nn_os_GetCurrentThread, ()) { return CurrentThread(); }
HLE_ALIAS(nnosGetCurrentThread, nn_os_GetCurrentThread);

// Each guest thread name, once (which of the game's worker threads exist; diagnostics).
static void LogThreadName(const char* name) {
    static std::mutex m;
    static std::vector<std::string> seen;
    const std::string n = name ? name : "";
    std::lock_guard<std::mutex> lk(m);
    if (seen.size() >= 128 || std::find(seen.begin(), seen.end(), n) != seen.end()) return;
    seen.push_back(n);
    LOGI("os", "guest thread named \"%s\"", n.c_str());
}

HLE_FUNC(_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc, void, nn_os_SetThreadName,
         (GuestThread * t, const char* name)) {
    LogThreadName(name);
    if (HostThread* ht = Lookup(t)) {
        ht->name = name ? name : "";
        ht->name_ptr = nullptr;
        MarkPoller(ht);
        if (ht == t_current) pthread_setname_np(pthread_self(), ht->name.substr(0, 15).c_str());
    }
}

HLE_FUNC(nnosSetThreadNamePointer, void, nnosSetThreadNamePointer, (GuestThread * t, const char* name)) {
    LogThreadName(name);
    if (HostThread* ht = Lookup(t)) {
        ht->name = name ? name : "";
        ht->name_ptr = name;
        MarkPoller(ht);
    }
}

HLE_FUNC(_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE, const char*, nn_os_GetThreadNamePointer,
         (GuestThread * t)) {
    HostThread* ht = Lookup(t);
    if (!ht) return "";
    return ht->name_ptr ? ht->name_ptr : ht->name.c_str();
}

HLE_FUNC(_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE, int, nn_os_GetThreadPriority, (GuestThread * t)) {
    HostThread* ht = Lookup(t);
    return ht ? ht->priority : 16;
}
HLE_ALIAS(nnosGetThreadPriority, nn_os_GetThreadPriority);

HLE_FUNC(_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi, int, nn_os_ChangeThreadPriority,
         (GuestThread * t, int prio)) {
    HostThread* ht = Lookup(t);
    if (!ht) return 16;
    int old = ht->priority;
    ht->priority = prio;
    return old;
}

HLE_FUNC(_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim, void, nn_os_SetThreadCoreMask,
         (GuestThread * t, int ideal, uint64_t mask)) {
    if (HostThread* ht = Lookup(t)) ht->ideal_core = ideal;
    (void)mask;
}

HLE_FUNC(_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE, uint64_t, nn_os_GetThreadId, (GuestThread * t)) {
    HostThread* ht = Lookup(t);
    return ht ? ht->id : 0;
}

HLE_FUNC(_ZN2nn2os19GetCurrentStackInfoEPmS1_, void, nn_os_GetCurrentStackInfo, (uintptr_t * stack, size_t* size)) {
    CurrentThread();
    if (stack) *stack = t_current->stack_lo;
    if (size) *size = t_current->stack_hi - t_current->stack_lo;
}

namespace dsr::hle::os {
namespace {
// Diagnostics: who sleeps how long (finds the game's frame limiter). Per caller (guest return address and the
// caller's own return address, both as main+offset), every 10 s, the ones that slept the most.
struct SleepSite {
    uint64_t calls = 0;
    int64_t total_ns = 0, min_ns = INT64_MAX, max_ns = 0;
    std::string thread;
};
std::mutex g_sleep_mutex;
std::map<std::pair<uintptr_t, uintptr_t>, SleepSite> g_sleep_sites;
std::chrono::steady_clock::time_point g_sleep_report = std::chrono::steady_clock::now();

uintptr_t MainOffset(uintptr_t a) {
    const dsr::loader::Module& m = dsr::GetRuntime().main;
    return m.Contains(a) ? a - m.base() : 0;
}

void RecordSleep(uintptr_t ret, uintptr_t frame, int64_t ns) {
    // `frame` is the calling guest function's frame record (its x29): [frame + 8] is where it returns to.
    uintptr_t ret2 = 0;
    if (t_current && frame >= t_current->stack_lo && frame + 16 <= t_current->stack_hi)
        ret2 = *reinterpret_cast<const uintptr_t*>(frame + 8);
    const auto key = std::make_pair(MainOffset(ret), MainOffset(ret2));
    std::lock_guard<std::mutex> lk(g_sleep_mutex);
    SleepSite& site = g_sleep_sites[key];
    site.calls++;
    site.total_ns += ns;
    site.min_ns = std::min(site.min_ns, ns);
    site.max_ns = std::max(site.max_ns, ns);
    if (site.thread.empty() && t_current) site.thread = t_current->name;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_sleep_report < std::chrono::seconds(10)) return;
    g_sleep_report = now;
    std::vector<std::pair<int64_t, std::pair<std::pair<uintptr_t, uintptr_t>, SleepSite>>> top;
    for (auto& [k, v] : g_sleep_sites) top.push_back({v.total_ns, {k, v}});
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < top.size() && i < 6; i++) {
        const auto& [k, v] = top[i].second;
        if (v.total_ns < 50'000'000) break;
        LOGI(kTag, "sleep: main+0x%lx (from main+0x%lx) thread '%s': %llu calls, %.1f s total, %.2f-%.2f ms",
             (unsigned long)k.first, (unsigned long)k.second, v.thread.c_str(), (unsigned long long)v.calls,
             v.total_ns / 1e9, v.min_ns / 1e6, v.max_ns / 1e6);
    }
    g_sleep_sites.clear();
}
}  // namespace
}  // namespace dsr::hle::os

namespace dsr::hle::os {
// From GetSystemTick: a poller thread (MarkPoller; the game is offline, so its network timing does not matter) that
// has run 0.5 ms without sleeping naps 5 ms. That bounds it to about a tenth of a core (1.5 ms naps still left it
// 1.5-2.4 s of CPU per 10 s on a Mali phone); the nap is short enough in case it holds a lock the game takes.
void PollerClockRead() {
    if (!t_current || !t_current->poller.load(std::memory_order_relaxed)) return;
    const int64_t now = NowTicks();
    if (!t_poller_run_start) t_poller_run_start = now;
    if (now - t_poller_run_start < kTickFrequency / 2000) return;  // 0.5 ms
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        LOGI(kTag, "'%s' reads the clock without sleeping: it naps 5 ms after each 0.5 ms of running "
             "(offline network polling)", t_current->name.c_str());
    PollerSleep(0);  // lowers its priority once
    timespec ts{0, 5'000'000};
    nanosleep(&ts, nullptr);
    t_poller_run_start = NowTicks();
}
}  // namespace dsr::hle::os

HLE_FUNC(_ZN2nn2os11SleepThreadENS_8TimeSpanE, void, nn_os_SleepThread, (int64_t ns)) {
    dsr::hle::os::RecordSleep(reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
                *static_cast<const uintptr_t*>(__builtin_frame_address(0)), ns);  // saved x29: the caller's frame
    // In the background the game's polling threads (network, streaming) are held at their next sleep, after a
    // grace period so a save being written finishes first.
    dsr::platform::WaitWhilePaused(3000);
    if (t_current && t_current->poller.load(std::memory_order_relaxed)) ns = PollerSleep(ns);
    if (ns <= 0) {
        sched_yield();
        return;
    }
    timespec ts{time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
    t_poller_run_start = 0;
}
HLE_ALIAS(nnosSleepThread, nn_os_SleepThread);

HLE_FUNC(_ZN2nn2os11YieldThreadEv, void, nn_os_YieldThread, ()) {
    if (t_current && t_current->poller.load(std::memory_order_relaxed)) {
        const int64_t ns = PollerSleep(0);
        timespec ts{0, long(ns)};
        nanosleep(&ts, nullptr);
        t_poller_run_start = 0;
        return;
    }
    sched_yield();
}

HLE_FUNC(_ZN2nn2os20GetCurrentCoreNumberEv, int, nn_os_GetCurrentCoreNumber, ()) {
    int c = sched_getcpu();
    return c < 0 ? 0 : c % 3;  // applications see cores 0-2
}

HLE_FUNC(_ZN2nn2os26GetThreadAvailableCoreMaskEv, uint64_t, nn_os_GetThreadAvailableCoreMask, ()) { return 0b111; }

// ---- TLS ----------------------------------------------------------------------------------------
static uint32_t AllocTls(uint32_t* out, const void* dtor) {
    std::lock_guard<std::mutex> lk(g_tls_mutex);
    for (int i = 0; i < kTlsSlots; i++) {
        if (!g_tls_slots[i].used) {
            g_tls_slots[i] = {true, dtor};
            *out = uint32_t(i);
            return kResultSuccess;
        }
    }
    return kResultOutOfResource;
}

HLE_FUNC(_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE, uint32_t, nn_os_AllocateTlsSlot,
         (uint32_t * out, const void* dtor)) {
    return AllocTls(out, dtor);
}
HLE_FUNC(_ZN2nn2os18SdkAllocateTlsSlotEPNS0_7TlsSlotEPFvmE, uint32_t, nn_os_SdkAllocateTlsSlot,
         (uint32_t * out, const void* dtor)) {
    return AllocTls(out, dtor);
}
HLE_FUNC(_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE, void, nn_os_FreeTlsSlot, (uint32_t slot)) {
    std::lock_guard<std::mutex> lk(g_tls_mutex);
    if (slot < kTlsSlots) g_tls_slots[slot] = {};
}
HLE_FUNC(_ZN2nn2os11GetTlsValueENS0_7TlsSlotE, uintptr_t, nn_os_GetTlsValue, (uint32_t slot)) {
    return slot < kTlsSlots ? t_tls_values[slot] : 0;
}
HLE_FUNC(_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm, void, nn_os_SetTlsValue, (uint32_t slot, uintptr_t v)) {
    if (slot < kTlsSlots) t_tls_values[slot] = v;
}

// ---- host process arguments ---------------------------------------------------------------------
static char* g_argv[] = {const_cast<char*>("DarkSoulsRemastered"), nullptr};
HLE_FUNC(_ZN2nn2os11GetHostArgcEv, int, nn_os_GetHostArgc, ()) { return 1; }
HLE_FUNC(_ZN2nn2os11GetHostArgvEv, char**, nn_os_GetHostArgv, ()) { return g_argv; }
