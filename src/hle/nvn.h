#pragma once

#include <atomic>
#include <cstdint>

namespace dsr::hle::nvn {
// An entry point handled as a no-op, reached through the named stub (whose argument points here): its first
// call is logged. `tag` tells it from the plain name strings of unimplemented entry points.
struct NopProc {
    char tag[8] = {'\x01', 'N', 'O', 'P', 'R', 'O', 'C', 0};
    std::atomic<uint64_t> calls{0};
    const char* name = nullptr;
};
// Register a host implementation of an NVN entry point (e.g. "nvnDeviceInitialize").
void RegisterNvnProc(const char* name, void* host_fn);
// Fallback for every entry point starting with `prefix` that has no exact registration.
void RegisterNvnPrefix(const char* prefix, void* host_fn);
// Host implementation registered for `name` (exact or prefix), without a thunk (tests).
void* FindNvnImpl(const char* name);
// Returns a guest-callable pointer for an NVN entry point (implemented or logging stub).
void* GetProc(const char* name);
}  // namespace dsr::hle::nvn
