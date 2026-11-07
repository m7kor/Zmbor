#pragma once
// Bounded, opt-in runtime diagnostics for bringing up commercial programs (docs/engineering/
// CRASH_TWINSANITY_BLACK_SCREEN.md). Off by default; every hook is one relaxed atomic load when disabled.
//
//   ZMBOR_TRACE_HLE=1      every HLE stub/syscall-wrapper call: counts, first/last caller, args, results
//   ZMBOR_TRACE_SYSCALL=1  numeric EE syscalls (handleSyscall): counts per syscall number
//   ZMBOR_TRACE_IO=1       guest file operations (open/read/seek/close): per-file audit, no file contents
//   ZMBOR_TRACE_GS=1       GS privileged-register and draw counters, display state at exit
//   ZMBOR_TRACE_LOAD=1     guest ELF segment writes through SifLoadElf
//   ZMBOR_TRACE_ALL=1      all of the above
//   ZMBOR_TRACE_EVENTS=N   keep the first N detailed events per key (default 4) in the ordered event log
//   ZMBOR_SUMMARY_FILE=f   write the end-of-run summary to f (otherwise stderr when any trace is on)
//
// Memory is bounded: one counter record per distinct key, at most ZMBOR_TRACE_EVENTS detailed events per key,
// and a hard cap of kMaxEvents ordered events overall.
#include <atomic>
#include <cstdint>
#include <string>

struct R5900Context;

namespace zmbor_trace
{
    enum Category : uint32_t
    {
        kHle = 1u << 0,
        kSyscall = 1u << 1,
        kIo = 1u << 2,
        kGs = 1u << 3,
        kLoad = 1u << 4,
    };

    extern std::atomic<uint32_t> g_enabled;

    inline bool enabled(Category c) noexcept
    {
        return (g_enabled.load(std::memory_order_relaxed) & c) != 0u;
    }

    // Reads the ZMBOR_TRACE_* environment once (idempotent).
    void initFromEnvironment();

    // Generic counted event. key identifies the counter (e.g. "hle:sceMcGetInfo"); detail is kept only for the
    // first ZMBOR_TRACE_EVENTS occurrences of that key.
    void record(Category c, const std::string &key, uint32_t guestPc, const std::string &detail);

    // Adds result to key's result histogram (at most 16 distinct values are kept).
    void recordResult(const std::string &key, int64_t result);

    // HLE call bracket used by generated stub wrappers (PS2X_HLE_TRACE); defined in zmbor_trace_hle.cpp.
    void hleEnter(const char *name, uint32_t stubAddress, const R5900Context *ctx);
    void hleLeave(const char *name, const R5900Context *ctx);

    struct HleScope
    {
        const char *name;
        const R5900Context *ctx;
        bool active;
        HleScope(const char *n, uint32_t addr, const R5900Context *c) noexcept : name(n), ctx(c), active(enabled(kHle))
        {
            if (active)
                hleEnter(name, addr, ctx);
        }
        ~HleScope()
        {
            if (active)
                hleLeave(name, ctx);
        }
        HleScope(const HleScope &) = delete;
        HleScope &operator=(const HleScope &) = delete;
    };

    // File I/O audit: one row per (resource, operation).
    void ioEvent(const char *op, const std::string &resource, int64_t offset, int64_t requested, int64_t result,
                 uint32_t guestPc);

    // Writes the summary (counters sorted by count, the ordered event log, the I/O audit) and returns it.
    std::string summary(const std::string &extraHeader);
    void writeSummary(const std::string &extraHeader);
}

// Emitted by ps2_recomp in every HLE wrapper; compiled to nothing extra but one atomic load when tracing is off.
#define PS2X_HLE_TRACE(name, addr) ::zmbor_trace::HleScope ps2xHleTraceScope_((name), (addr), ctx)
