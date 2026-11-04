// Trace core: no runtime types, so standalone libraries (the GS cache test backend) can link it. The HLE helpers
// that read R5900Context live in zmbor_trace_hle.cpp (ps2_runtime only).
#include "runtime/zmbor_trace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace zmbor_trace
{
    std::atomic<uint32_t> g_enabled{0u};

    namespace
    {
        constexpr size_t kMaxEvents = 20000;

        struct Counter
        {
            uint64_t count = 0;
            uint32_t firstPc = 0;
            uint32_t lastPc = 0;
            uint64_t firstSeq = 0;
            uint64_t lastSeq = 0;
            std::string lastDetail;
            std::map<int64_t, uint64_t> results; // HLE return values (v0) histogram, bounded below
        };

        struct IoRow
        {
            uint64_t count = 0;
            int64_t requestedTotal = 0;
            int64_t resultTotal = 0;
            int64_t failures = 0;
            int64_t firstOffset = -1;
            int64_t lastOffset = -1;
            uint32_t firstPc = 0;
        };

        std::mutex g_mutex;
        std::once_flag g_initOnce;
        uint32_t g_eventsPerKey = 4;
        uint64_t g_seq = 0;
        std::unordered_map<std::string, Counter> g_counters;
        std::vector<std::string> g_events;
        std::map<std::pair<std::string, std::string>, IoRow> g_io; // (resource, op)
        const auto g_start = std::chrono::steady_clock::now();

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && v[0] != '\0' && v[0] != '0';
        }

        double elapsedMs()
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
        }

        void recordLocked(Category c, const std::string &key, uint32_t guestPc, const std::string &detail)
        {
            Counter &counter = g_counters[key];
            ++g_seq;
            if (counter.count == 0)
            {
                counter.firstPc = guestPc;
                counter.firstSeq = g_seq;
            }
            ++counter.count;
            counter.lastPc = guestPc;
            counter.lastSeq = g_seq;
            counter.lastDetail = detail;
            if (counter.count <= g_eventsPerKey && g_events.size() < kMaxEvents)
            {
                char head[96];
                std::snprintf(head, sizeof(head), "#%llu t=%.1fms cat=%u pc=0x%08x ",
                              static_cast<unsigned long long>(g_seq), elapsedMs(), static_cast<unsigned>(c), guestPc);
                g_events.push_back(head + key + " " + detail);
            }
        }
    }

    void initFromEnvironment()
    {
        std::call_once(g_initOnce, []()
                       {
            uint32_t mask = 0;
            const bool all = envOn("ZMBOR_TRACE_ALL");
            if (all || envOn("ZMBOR_TRACE_HLE")) mask |= kHle;
            if (all || envOn("ZMBOR_TRACE_SYSCALL")) mask |= kSyscall;
            if (all || envOn("ZMBOR_TRACE_IO")) mask |= kIo;
            if (all || envOn("ZMBOR_TRACE_GS")) mask |= kGs;
            if (const char *n = std::getenv("ZMBOR_TRACE_EVENTS"))
                g_eventsPerKey = static_cast<uint32_t>(std::max(0L, std::strtol(n, nullptr, 10)));
            g_enabled.store(mask, std::memory_order_relaxed); });
    }

    void record(Category c, const std::string &key, uint32_t guestPc, const std::string &detail)
    {
        if (!enabled(c))
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        recordLocked(c, key, guestPc, detail);
    }

    void recordResult(const std::string &key, int64_t result)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Counter &counter = g_counters[key];
        if (counter.results.size() < 16 || counter.results.count(result))
            ++counter.results[result];
    }

    void ioEvent(const char *op, const std::string &resource, int64_t offset, int64_t requested, int64_t result,
                 uint32_t guestPc)
    {
        if (!enabled(kIo))
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        IoRow &row = g_io[{resource, op}];
        if (row.count == 0)
        {
            row.firstOffset = offset;
            row.firstPc = guestPc;
        }
        ++row.count;
        row.lastOffset = offset;
        if (requested > 0)
            row.requestedTotal += requested;
        if (result >= 0)
            row.resultTotal += result;
        else
            ++row.failures;
        char detail[200];
        std::snprintf(detail, sizeof(detail), "%s off=%lld req=%lld res=%lld", resource.c_str(),
                      static_cast<long long>(offset), static_cast<long long>(requested), static_cast<long long>(result));
        recordLocked(kIo, std::string("io:") + op, guestPc, detail);
    }

    std::string summary(const std::string &extraHeader)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::ostringstream out;
        out << "== zmbor trace summary ==\n" << extraHeader;
        out << "elapsed_ms=" << static_cast<uint64_t>(elapsedMs()) << " enabled_mask=" << g_enabled.load()
            << " keys=" << g_counters.size() << " events=" << g_events.size() << "\n";

        std::vector<std::pair<std::string, const Counter *>> sorted;
        for (const auto &kv : g_counters)
            sorted.emplace_back(kv.first, &kv.second);
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b)
                  { return a.second->firstSeq < b.second->firstSeq; });
        out << "-- counters (in first-seen order): key count first_pc last_pc first_seq last_seq results | last\n";
        for (const auto &[key, c] : sorted)
        {
            char line[200];
            std::snprintf(line, sizeof(line), "%-40s %10llu 0x%08x 0x%08x %8llu %8llu ", key.c_str(),
                          static_cast<unsigned long long>(c->count), c->firstPc, c->lastPc,
                          static_cast<unsigned long long>(c->firstSeq), static_cast<unsigned long long>(c->lastSeq));
            out << line;
            for (const auto &[v, n] : c->results)
                out << v << "x" << n << " ";
            out << "| " << c->lastDetail << "\n";
        }
        out << "-- io audit: resource op count first_pc first_off last_off requested returned failures\n";
        for (const auto &[k, r] : g_io)
        {
            out << k.first << " " << k.second << " " << r.count << " 0x" << std::hex << r.firstPc << std::dec << " "
                << r.firstOffset << " " << r.lastOffset << " " << r.requestedTotal << " " << r.resultTotal << " "
                << r.failures << "\n";
        }
        out << "-- ordered events (first " << g_eventsPerKey << " per key)\n";
        for (const auto &e : g_events)
            out << e << "\n";
        return out.str();
    }

    void writeSummary(const std::string &extraHeader)
    {
        const char *path = std::getenv("ZMBOR_SUMMARY_FILE");
        const bool anyTrace = g_enabled.load() != 0u;
        if (!anyTrace && !(path && path[0]))
            return;
        const std::string text = summary(extraHeader);
        if (path && path[0])
        {
            std::ofstream f(path, std::ios::binary);
            f << text;
        }
        else
        {
            std::cerr << text;
        }
    }
}
