#include "sampling_profiler.h"

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

struct SamplingProfiler::Impl
{
    unsigned interval = 250;
    HANDLE target = nullptr;
    std::atomic<bool> running{false};
    std::thread thread;
    std::map<uint64_t, uint64_t> samples;
    uint64_t total = 0;

    void loop()
    {
        while (running)
        {
            if (SuspendThread(target) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(target, &context))
                {
                    ++samples[context.Rip];
                    ++total;
                }
                ResumeThread(target);
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(interval);
            while (running && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
        }
    }
};

SamplingProfiler::SamplingProfiler(unsigned intervalMicros) : m_impl(std::make_unique<Impl>()) { m_impl->interval = intervalMicros; }
SamplingProfiler::~SamplingProfiler() { stop(); }

void SamplingProfiler::start()
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_impl->target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    m_impl->running = true;
    m_impl->thread = std::thread([impl = m_impl.get()] { impl->loop(); });
}

void SamplingProfiler::stop()
{
    if (!m_impl->running)
        return;
    m_impl->running = false;
    if (m_impl->thread.joinable())
        m_impl->thread.join();
    CloseHandle(m_impl->target);
}

void SamplingProfiler::report(unsigned top) const
{
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    std::map<std::string, uint64_t> byFunction;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (const auto &[address, count] : m_impl->samples)
    {
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const std::string name = SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : "<unknown>";
        byFunction[name] += count;
    }
    SymCleanup(process);
    std::vector<std::pair<std::string, uint64_t>> sorted(byFunction.begin(), byFunction.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    std::printf("profile: %llu samples every %u us (self time)\n", static_cast<unsigned long long>(m_impl->total),
                m_impl->interval);
    for (unsigned i = 0; i < sorted.size() && i < top; ++i)
        std::printf("  %5.1f%%  %s\n", 100.0 * static_cast<double>(sorted[i].second) / static_cast<double>(m_impl->total),
                    sorted[i].first.c_str());
}
#else
struct SamplingProfiler::Impl {};
SamplingProfiler::SamplingProfiler(unsigned) : m_impl(std::make_unique<Impl>()) {}
SamplingProfiler::~SamplingProfiler() = default;
void SamplingProfiler::start() {}
void SamplingProfiler::stop() {}
void SamplingProfiler::report(unsigned) const { std::printf("profile: the sampling profiler is Windows-only\n"); }
#endif
