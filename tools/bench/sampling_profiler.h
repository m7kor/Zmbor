#pragma once
// Minimal sampling profiler for the benchmarks (Windows; elsewhere a no-op). A helper thread suspends the profiled
// thread at a fixed interval, records its instruction pointer and resumes it; report() resolves the addresses with
// DbgHelp (needs a PDB: compile with /Zi, link with /DEBUG) and prints the functions with the most samples. Self time
// only (no call stacks). The implementation lives in sampling_profiler.cpp so that <windows.h> never meets the
// runtime headers (raylib declares CloseWindow, Rectangle, ...).
#include <memory>

class SamplingProfiler
{
public:
    explicit SamplingProfiler(unsigned intervalMicros = 250);
    ~SamplingProfiler();
    void start();   // profiles the calling thread
    void stop();
    void report(unsigned top = 25) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
