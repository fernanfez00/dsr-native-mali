// nn::fs — mount points are mapped onto host directories:
//   MountRom(name)      -> RuntimeConfig::game_dir (the extracted romfs: dvdbnd*.bdt, movww/, ...)
//   MountSaveData(name) -> RuntimeConfig::save_dir
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "common/profiler.h"
#include "hle/os/os.h"
#include "hle/os/write_watch.h"
#include "hle/registry.h"
#include "runtime/runtime.h"

namespace {
constexpr const char* kTag = "fs";

// nn::fs results (module 2)
constexpr uint32_t MakeResult(uint32_t desc) { return 2u | (desc << 9); }
constexpr uint32_t kOk = 0;
constexpr uint32_t kPathNotFound = MakeResult(1);
constexpr uint32_t kPathAlreadyExists = MakeResult(2);
constexpr uint32_t kTargetLocked = MakeResult(7);
constexpr uint32_t kDirectoryNotEmpty = MakeResult(8);
constexpr uint32_t kUsableSpaceNotEnough = MakeResult(30);
constexpr uint32_t kMountNameAlreadyExists = MakeResult(60);
constexpr uint32_t kOutOfRange = MakeResult(3005);
constexpr uint32_t kInvalidPath = MakeResult(6001);
constexpr uint32_t kFileExtensionWithoutOpenModeAllowAppend = MakeResult(6201);
constexpr uint32_t kUnexpected = MakeResult(5000);

enum OpenMode { kRead = 1, kWrite = 2, kAllowAppend = 4 };

std::mutex g_mounts_mutex;
std::unordered_map<std::string, std::string> g_mounts;  // name -> host dir (no trailing slash)

struct HostFile {
    int fd;
    int mode;
    std::string path;
    bool dirty = false;  // written since opened (fsynced by CommitSaveData)
};
struct HostDir {
    std::vector<std::pair<std::string, struct stat>> entries;
    size_t pos = 0;
};

struct DirectoryEntry {  // nn::fs::DirectoryEntry, 0x310 bytes
    char name[0x301];
    uint8_t reserved0[3];
    int8_t type;  // 0 = directory, 1 = file
    uint8_t reserved1[3];
    int64_t size;
};
static_assert(sizeof(DirectoryEntry) == 0x310, "DirectoryEntry layout");

uint32_t FromErrno(int e) {
    switch (e) {
        case ENOENT:
        case ENOTDIR: return kPathNotFound;
        case EEXIST: return kPathAlreadyExists;
        case ENOTEMPTY: return kDirectoryNotEmpty;
        case ENOSPC: return kUsableSpaceNotEnough;
        case EBUSY: return kTargetLocked;
        default: return kUnexpected;
    }
}

// "name:/a/b" -> host path. Returns false for unknown mounts.
bool Resolve(const char* guest, std::string* out) {
    if (!guest) return false;
    const char* colon = strchr(guest, ':');
    if (!colon) return false;
    std::string mount(guest, colon - guest);
    const char* rest = colon + 1;
    while (*rest == '/' || *rest == '\\') rest++;
    std::lock_guard<std::mutex> lk(g_mounts_mutex);
    auto it = g_mounts.find(mount);
    if (it == g_mounts.end()) return false;
    *out = it->second + "/" + rest;
    for (char& c : *out)
        if (c == '\\') c = '/';
    return true;
}

#define RESOLVE_OR_FAIL(guest, host)                                      \
    std::string host;                                                     \
    if (!Resolve(guest, &host)) {                                         \
        LOGW(kTag, "%s: unresolvable path '%s'", __func__, guest ? guest : "(null)"); \
        return kPathNotFound;                                             \
    }

uint32_t Mount(const char* name, const std::string& dir) {
    mkdir(dir.c_str(), 0775);
    std::lock_guard<std::mutex> lk(g_mounts_mutex);
    if (!g_mounts.emplace(name, dir).second) return kMountNameAlreadyExists;
    LOGI(kTag, "mounted %s: -> %s", name, dir.c_str());
    return kOk;
}

int RemoveTree(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) return -1;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string child = path + "/" + e->d_name;
        struct stat st;
        if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            RemoveTree(child);
        else
            unlink(child.c_str());
    }
    closedir(d);
    return rmdir(path.c_str());
}

// ---- stutter diagnostics: time spent in file system calls -------------------------------------------
// Every call is timed (two clock reads; calls are thousands per second at most). Each kind is summed for the
// 10 s report; a call of 4 ms or more goes to the profiler, which lists it under a slow frame it overlapped.
enum FsKind { kFsRead, kFsWrite, kFsOpen, kFsFlush, kFsCommit, kFsMeta, kFsKinds };
const char* const kFsKindNames[kFsKinds] = {"reads", "writes", "opens", "flushes", "save commits", "other"};
struct FsStat {
    std::atomic<uint64_t> calls{0}, ns{0}, max_ns{0}, slow{0}, bytes{0};
};
FsStat g_fs_stats[kFsKinds];

int64_t FsNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
const char* BaseName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p.c_str() : p.c_str() + s + 1;
}
// Save loop diagnostics: the calls on the save mount around the 20th to 22nd save commit are logged one by one
// (the game saved ~28 times a second).
std::atomic<int> g_commits{0};
bool TraceSave(const std::string& path) {
    const int c = g_commits.load(std::memory_order_relaxed);
    if (c < 20 || c >= 23) return false;
    const std::string& dir = dsr::GetRuntime().config.save_dir;
    return path.compare(0, dir.size(), dir) == 0;
}
// Save writes: the game rewrites its save slot many times a second. A write of exactly the bytes the last
// write to the same place left there changes nothing and is skipped (no write, no flush: the flash is spared);
// changed writes are counted and the first few logged with where they differ (what makes the game save).
struct SaveCopy {
    std::vector<uint8_t> data;
};
std::mutex g_save_copy_mutex;
std::unordered_map<std::string, SaveCopy> g_save_copies;  // "path@offset"
std::atomic<uint64_t> g_save_same{0}, g_save_changed{0};
// A file replaced, resized, renamed or deleted: what was written before says nothing about it any more.
void ForgetSaveCopies() {
    std::lock_guard<std::mutex> lk(g_save_copy_mutex);
    g_save_copies.clear();
}
bool SaveWriteUnchanged(const std::string& path, int64_t off, const void* buf, size_t size) {
    const std::string& dir = dsr::GetRuntime().config.save_dir;
    if (path.compare(0, dir.size(), dir) != 0 || size > (8u << 20)) return false;
    const std::string key = path + "@" + std::to_string(off);
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    std::lock_guard<std::mutex> lk(g_save_copy_mutex);
    SaveCopy& c = g_save_copies[key];
    if (c.data.size() == size && memcmp(c.data.data(), p, size) == 0) {
        g_save_same.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (c.data.size() == size) {
        size_t first = SIZE_MAX, last = 0, n = 0;
        for (size_t i = 0; i < size; i++)
            if (c.data[i] != p[i]) {
                first = std::min(first, i);
                last = i;
                n++;
            }
        static int logged = 0;
        if (logged++ < 12)
            LOGI(kTag, "save write changed: %s offset %lld: %zu of %zu bytes differ, from byte 0x%zx to 0x%zx", BaseName(path),
                 (long long)off, n, size, first, last);
    }
    g_save_changed.fetch_add(1, std::memory_order_relaxed);
    c.data.assign(p, p + size);
    return false;
}

// Times one call; `detail` (a file name) and `bytes` describe it in the slow-call report.
struct FsTimer {
    FsKind kind;
    uintptr_t caller;
    const char* detail;
    size_t bytes;
    int64_t t0 = FsNow();
    // Save trace (see TraceSave): the call, its offset and result.
    const char* op = nullptr;
    const char* guest = nullptr;  // the game's path, when the call has one
    int64_t off = -1;
    uint32_t rc = 0;
    FsTimer(FsKind k, uintptr_t ret, const char* d = nullptr, size_t b = 0) : kind(k), caller(ret), detail(d), bytes(b) {}
    void Trace(const char* name, const std::string& path, int64_t offset = -1) {
        if (TraceSave(path)) {
            op = name;
            off = offset;
        }
    }
    uint32_t Ret(uint32_t r) {
        rc = r;
        return r;
    }
    ~FsTimer() {
        const int64_t t1 = FsNow(), ns = t1 - t0;
        if (op) {
            const uintptr_t base = dsr::GetRuntime().main.base();
            const char* who = dsr::hle::os::CurrentThreadName();
            LOGI(kTag, "save trace: %s %s %s offset %lld, %zu bytes -> 0x%x (%.2f ms, from main+0x%lx)%s%s%s",
                 who ? who : "host", op, detail ? detail : "", (long long)off, bytes, rc, ns / 1e6,
                 (unsigned long)(caller - base), guest ? " ('" : "", guest ? guest : "", guest ? "')" : "");
        }
        FsStat& st = g_fs_stats[kind];
        st.calls.fetch_add(1, std::memory_order_relaxed);
        st.ns.fetch_add(uint64_t(ns), std::memory_order_relaxed);
        st.bytes.fetch_add(bytes, std::memory_order_relaxed);
        uint64_t m = st.max_ns.load(std::memory_order_relaxed);
        while (uint64_t(ns) > m && !st.max_ns.compare_exchange_weak(m, uint64_t(ns), std::memory_order_relaxed)) {
        }
        if (ns >= 4'000'000) {
            st.slow.fetch_add(1, std::memory_order_relaxed);
            char what[72];
            if (bytes)
                snprintf(what, sizeof(what), "fs %s %s (%zu KB)", kFsKindNames[kind], detail ? detail : "", bytes >> 10);
            else
                snprintf(what, sizeof(what), "fs %s %s", kFsKindNames[kind], detail ? detail : "");
            const char* who = dsr::hle::os::CurrentThreadName();
            dsr::profiler::NoteBlockedSpan(who ? who : "host", what, caller, t0, t1);
        }
    }
};
#define FS_TIMED(...) FsTimer fs_timer_(__VA_ARGS__)
#define FS_CALLER reinterpret_cast<uintptr_t>(__builtin_return_address(0))

void FsReport() {
    std::string line;
    for (int k = 0; k < kFsKinds; k++) {
        FsStat& st = g_fs_stats[k];
        const uint64_t calls = st.calls.exchange(0), ns = st.ns.exchange(0), mx = st.max_ns.exchange(0),
                       slow = st.slow.exchange(0), bytes = st.bytes.exchange(0);
        if (!calls) continue;
        char b[160];
        snprintf(b, sizeof(b), "%s%s %llu (%.0f ms, longest %.1f ms, %llu over 4 ms%s", line.empty() ? "" : "; ",
                 kFsKindNames[k], (unsigned long long)calls, ns / 1e6, mx / 1e6, (unsigned long long)slow,
                 bytes ? "" : ")");
        line += b;
        if (bytes) {
            snprintf(b, sizeof(b), ", %.1f MB)", bytes / 1048576.0);
            line += b;
        }
    }
    if (!line.empty()) LOGI(kTag, "file system calls (last 10 s): %s", line.c_str());
    const uint64_t same = g_save_same.exchange(0), changed = g_save_changed.exchange(0);
    if (same || changed)
        LOGI(kTag, "save writes (last 10 s): %llu changed the save, %llu were identical to what was there (skipped)",
             (unsigned long long)changed, (unsigned long long)same);
}
struct FsReportInstall {
    FsReportInstall() { dsr::profiler::SetReportHook(FsReport); }
} g_fs_report_install;

// ---- save commits ----------------------------------------------------------------------------------
// CommitSaveData flushed the whole device (sync()): every pending write of every app, our texture and shader
// caches included, while the game autosaves every few seconds. It now flushes what the game changed since the
// last commit: the files it wrote (fsync) and the directories whose entries changed.
std::mutex g_dirty_mutex;
std::set<std::string> g_dirty_files, g_dirty_dirs;
void MarkDirtyFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_dirty_mutex);
    g_dirty_files.insert(path);
}
void MarkDirtyDir(const std::string& path) {
    const size_t s = path.find_last_of('/');
    std::lock_guard<std::mutex> lk(g_dirty_mutex);
    g_dirty_dirs.insert(s == std::string::npos ? std::string(".") : path.substr(0, s));
}
int SyncPath(const std::string& path, bool dir) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | (dir ? O_DIRECTORY : 0));
    if (fd < 0) return errno == ENOENT ? 0 : errno;  // deleted since: nothing to keep
    const int r = fsync(fd) == 0 ? 0 : errno;
    close(fd);
    return r;
}
}  // namespace

namespace dsr::hle::fs {
bool ResolveGuestPath(const char* guest, std::string* host) { return Resolve(guest, host); }
}  // namespace dsr::hle::fs

// ---- mounting ------------------------------------------------------------------------------------
HLE_FUNC(_ZN2nn2fs22QueryMountRomCacheSizeEPm, uint32_t, fs_QueryMountRomCacheSize, (size_t * out)) {
    *out = 0x1000;  // no cache needed: reads go straight to the host filesystem
    return kOk;
}
HLE_FUNC(_ZN2nn2fs8MountRomEPKcPvm, uint32_t, fs_MountRom, (const char* name, void*, size_t)) {
    return Mount(name, dsr::GetRuntime().config.game_dir);
}
HLE_FUNC(_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE, uint32_t, fs_EnsureSaveData, (const void*)) {
    mkdir(dsr::GetRuntime().config.save_dir.c_str(), 0775);
    return kOk;
}
HLE_FUNC(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE, uint32_t, fs_MountSaveData, (const char* name, const void*)) {
    return Mount(name, dsr::GetRuntime().config.save_dir);
}
HLE_FUNC(_ZN2nn2fs7UnmountEPKc, void, fs_Unmount, (const char* name)) {
    std::lock_guard<std::mutex> lk(g_mounts_mutex);
    g_mounts.erase(name);
}
HLE_FUNC(_ZN2nn2fs14CommitSaveDataEPKc, uint32_t, fs_CommitSaveData, (const char*)) {
    FS_TIMED(kFsCommit, FS_CALLER);
    fs_timer_.Trace("CommitSaveData", dsr::GetRuntime().config.save_dir);
    g_commits.fetch_add(1, std::memory_order_relaxed);
    std::set<std::string> files, dirs;
    {
        std::lock_guard<std::mutex> lk(g_dirty_mutex);
        files.swap(g_dirty_files);
        dirs.swap(g_dirty_dirs);
    }
    int err = 0;
    for (const std::string& f : files) err = err ? err : SyncPath(f, false);
    for (const std::string& d : dirs) err = err ? err : SyncPath(d, true);
    static int logged = 0;
    if (err && logged++ < 5) LOGW(kTag, "CommitSaveData: flushing the save failed: %s", strerror(err));
    return fs_timer_.Ret(kOk);
}
HLE_FUNC(_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE, void, fs_SetAllocator, (const void*, const void*)) {}

// ---- files ----------------------------------------------------------------------------------------
HLE_FUNC(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, uint32_t, fs_OpenFile, (void** out, const char* path, int mode)) {
    RESOLVE_OR_FAIL(path, host);
    FS_TIMED(kFsOpen, FS_CALLER, BaseName(host));
    fs_timer_.Trace((mode & kWrite) ? "OpenFile (write)" : "OpenFile", host);
    int flags = (mode & kWrite) ? ((mode & kRead) ? O_RDWR : O_WRONLY) : O_RDONLY;
    int fd = open(host.c_str(), flags | O_CLOEXEC);
    if (fd < 0) {
        LOGD(kTag, "OpenFile('%s') failed: %s", path, strerror(errno));
        return fs_timer_.Ret(FromErrno(errno));
    }
    *out = new HostFile{fd, mode, host, false};
    LOGD(kTag, "OpenFile('%s', %d) -> fd %d", path, mode, fd);
    return fs_timer_.Ret(kOk);
}
HLE_FUNC(_ZN2nn2fs9CloseFileENS0_10FileHandleE, void, fs_CloseFile, (HostFile * f)) {
    if (!f) return;
    if (f->dirty) MarkDirtyFile(f->path);
    close(f->fd);
    delete f;
}
// Read failures are logged (the first 50): the game treats a failed read of its data as fatal on the Switch and
// does not always recover here (an area whose models did not load is drawn without them).
static std::atomic<int> g_read_failures{0};
static void LogReadFailure(const HostFile* f, int64_t off, size_t size, size_t done, int err) {
    if (g_read_failures.fetch_add(1) >= 50) return;
    LOGW(kTag, "ReadFile('%s', offset %lld, %zu bytes): %s%s, %zu bytes read", f->path.c_str(), (long long)off, size,
         err ? "failed: " : "short read", err ? strerror(err) : "", done);
}
static uint32_t ReadImpl(size_t* out, HostFile* f, int64_t off, void* buf, size_t size, bool must_fill) {
    size_t done = 0;
    // The kernel writing a page the write tracking made read-only fails (EFAULT) instead of faulting.
    dsr::hle::os::WatchBeforeWrite(buf, size);
    int eio_retries = 0;
    while (done < size) {
        ssize_t r = pread(f->fd, static_cast<char*>(buf) + done, size - done, off + int64_t(done));
        if (r < 0) {
            if (errno == EINTR) continue;
            // An SD card or a slow storage driver can fail a read once and succeed when asked again.
            if ((errno == EIO || errno == EAGAIN) && eio_retries++ < 3) {
                usleep(2000);
                continue;
            }
            LogReadFailure(f, off, size, done, errno);
            return FromErrno(errno);
        }
        if (r == 0) break;
        done += size_t(r);
    }
    if (done != size && must_fill) LogReadFailure(f, off, size, done, 0);
    if (out) *out = done;
    return kOk;
}
HLE_FUNC(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm, uint32_t, fs_ReadFile,
         (HostFile * f, int64_t off, void* buf, size_t size)) {
    FS_TIMED(kFsRead, FS_CALLER, BaseName(f->path), size);
    fs_timer_.Trace("ReadFile", f->path, off);
    size_t n = 0;
    uint32_t r = ReadImpl(&n, f, off, buf, size, true);
    if (r == kOk && n != size) return fs_timer_.Ret(kOutOfRange);
    return fs_timer_.Ret(r);
}
HLE_FUNC(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm, uint32_t, fs_ReadFileOut,
         (size_t * out, HostFile* f, int64_t off, void* buf, size_t size)) {
    FS_TIMED(kFsRead, FS_CALLER, BaseName(f->path), size);
    fs_timer_.Trace("ReadFile", f->path, off);
    return fs_timer_.Ret(ReadImpl(out, f, off, buf, size, false));
}
HLE_FUNC(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE, uint32_t, fs_WriteFile,
         (HostFile * f, int64_t off, const void* buf, size_t size, const int* option)) {
    FS_TIMED(kFsWrite, FS_CALLER, BaseName(f->path), size);
    fs_timer_.Trace((option && (*option & 1)) ? "WriteFile (flush)" : "WriteFile", f->path, off);
    if (SaveWriteUnchanged(f->path, off, buf, size)) return fs_timer_.Ret(kOk);  // already on disk
    f->dirty = true;
    if (!(f->mode & kAllowAppend)) {
        struct stat st;
        if (fstat(f->fd, &st) == 0 && off + int64_t(size) > st.st_size) return fs_timer_.Ret(kFileExtensionWithoutOpenModeAllowAppend);
    }
    size_t done = 0;
    while (done < size) {
        ssize_t r = pwrite(f->fd, static_cast<const char*>(buf) + done, size - done, off + int64_t(done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return fs_timer_.Ret(FromErrno(errno));
        }
        done += size_t(r);
    }
    if (option && (*option & 1)) fsync(f->fd);  // WriteOptionFlag_Flush
    return fs_timer_.Ret(kOk);
}
HLE_FUNC(_ZN2nn2fs9FlushFileENS0_10FileHandleE, uint32_t, fs_FlushFile, (HostFile * f)) {
    FS_TIMED(kFsFlush, FS_CALLER, BaseName(f->path));
    fs_timer_.Trace("FlushFile", f->path);
    fsync(f->fd);
    return fs_timer_.Ret(kOk);
}
HLE_FUNC(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE, uint32_t, fs_GetFileSize, (int64_t * out, HostFile* f)) {
    struct stat st;
    if (fstat(f->fd, &st) != 0) return FromErrno(errno);
    *out = st.st_size;
    return kOk;
}
HLE_FUNC(_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl, uint32_t, fs_SetFileSize, (HostFile * f, int64_t size)) {
    FS_TIMED(kFsMeta, FS_CALLER, BaseName(f->path));
    fs_timer_.Trace("SetFileSize", f->path, size);
    ForgetSaveCopies();
    f->dirty = true;
    return fs_timer_.Ret(ftruncate(f->fd, size) == 0 ? kOk : FromErrno(errno));
}
HLE_FUNC(_ZN2nn2fs10CreateFileEPKcl, uint32_t, fs_CreateFile, (const char* path, int64_t size)) {
    RESOLVE_OR_FAIL(path, host);
    FS_TIMED(kFsMeta, FS_CALLER, BaseName(host));
    fs_timer_.Trace("CreateFile", host);
    ForgetSaveCopies();
    MarkDirtyFile(host);
    MarkDirtyDir(host);
    int fd = open(host.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0664);
    if (fd < 0) return fs_timer_.Ret(FromErrno(errno));
    uint32_t r = ftruncate(fd, size) == 0 ? kOk : FromErrno(errno);
    close(fd);
    return fs_timer_.Ret(r);
}
HLE_FUNC(_ZN2nn2fs10DeleteFileEPKc, uint32_t, fs_DeleteFile, (const char* path)) {
    RESOLVE_OR_FAIL(path, host);
    FS_TIMED(kFsMeta, FS_CALLER, BaseName(host));
    fs_timer_.Trace("DeleteFile", host);
    ForgetSaveCopies();
    MarkDirtyDir(host);
    return fs_timer_.Ret(unlink(host.c_str()) == 0 ? kOk : FromErrno(errno));
}
HLE_FUNC(_ZN2nn2fs10RenameFileEPKcS2_, uint32_t, fs_RenameFile, (const char* from, const char* to)) {
    RESOLVE_OR_FAIL(from, a);
    RESOLVE_OR_FAIL(to, b);
    FS_TIMED(kFsMeta, FS_CALLER, BaseName(b));
    fs_timer_.Trace("RenameFile", b);
    ForgetSaveCopies();
    MarkDirtyDir(a);
    MarkDirtyDir(b);
    struct stat st;
    if (stat(b.c_str(), &st) == 0) return fs_timer_.Ret(kPathAlreadyExists);
    return fs_timer_.Ret(rename(a.c_str(), b.c_str()) == 0 ? kOk : FromErrno(errno));
}

// ---- directories ------------------------------------------------------------------------------------
HLE_FUNC(_ZN2nn2fs15CreateDirectoryEPKc, uint32_t, fs_CreateDirectory, (const char* path)) {
    RESOLVE_OR_FAIL(path, host);
    FS_TIMED(kFsMeta, FS_CALLER, BaseName(host));
    fs_timer_.Trace("CreateDirectory", host);
    MarkDirtyDir(host);
    return fs_timer_.Ret(mkdir(host.c_str(), 0775) == 0 ? kOk : FromErrno(errno));
}
HLE_FUNC(_ZN2nn2fs15DeleteDirectoryEPKc, uint32_t, fs_DeleteDirectory, (const char* path)) {
    RESOLVE_OR_FAIL(path, host);
    FS_TIMED(kFsMeta, FS_CALLER, BaseName(host));
    fs_timer_.Trace("DeleteDirectory", host);
    ForgetSaveCopies();
    MarkDirtyDir(host);
    return fs_timer_.Ret(rmdir(host.c_str()) == 0 ? kOk : FromErrno(errno));
}
HLE_FUNC(_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc, uint32_t, fs_DeleteDirectoryRecursively, (const char* path)) {
    RESOLVE_OR_FAIL(path, host);
    FS_TIMED(kFsMeta, FS_CALLER, BaseName(host));
    fs_timer_.Trace("DeleteDirectoryRecursively", host);
    ForgetSaveCopies();
    MarkDirtyDir(host);
    return fs_timer_.Ret(RemoveTree(host) == 0 ? kOk : FromErrno(errno));
}
HLE_FUNC(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc, uint32_t, fs_GetEntryType,
         (int* out, const char* path)) {
    RESOLVE_OR_FAIL(path, host);
    FS_TIMED(kFsMeta, FS_CALLER, BaseName(host));
    fs_timer_.Trace("GetEntryType", host);
    fs_timer_.guest = path;
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return fs_timer_.Ret(FromErrno(errno));
    *out = S_ISDIR(st.st_mode) ? 0 : 1;
    return fs_timer_.Ret(kOk);
}
HLE_FUNC(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci, uint32_t, fs_OpenDirectory,
         (void** out, const char* path, int mode)) {
    RESOLVE_OR_FAIL(path, host);
    DIR* d = opendir(host.c_str());
    if (!d) return FromErrno(errno);
    auto* hd = new HostDir;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (stat((host + "/" + e->d_name).c_str(), &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);
        if ((is_dir && (mode & 1)) || (!is_dir && (mode & 2))) hd->entries.emplace_back(e->d_name, st);
    }
    closedir(d);
    *out = hd;
    return kOk;
}
HLE_FUNC(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE, void, fs_CloseDirectory, (HostDir * d)) { delete d; }
HLE_FUNC(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl, uint32_t, fs_ReadDirectory,
         (int64_t * out_count, DirectoryEntry* entries, HostDir* d, int64_t max)) {
    int64_t n = 0;
    while (n < max && d->pos < d->entries.size()) {
        const auto& [name, st] = d->entries[d->pos++];
        DirectoryEntry& e = entries[n++];
        memset(&e, 0, sizeof(e));
        strncpy(e.name, name.c_str(), sizeof(e.name) - 1);
        e.type = S_ISDIR(st.st_mode) ? 0 : 1;
        e.size = S_ISDIR(st.st_mode) ? 0 : st.st_size;
    }
    *out_count = n;
    return kOk;
}
HLE_FUNC(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE, uint32_t, fs_GetDirectoryEntryCount,
         (int64_t * out, HostDir* d)) {
    *out = int64_t(d->entries.size());
    return kOk;
}

// Keep unused-constant warnings quiet for results we may need later.
[[maybe_unused]] static const uint32_t kUnusedResults[] = {kInvalidPath};
