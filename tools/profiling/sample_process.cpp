// Standalone, opt-in Windows x64 process sampler. It does not change guest scheduling or runtime code.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct ResumeGuard {
    HANDLE thread;
    ~ResumeGuard() { if (thread) ResumeThread(thread); }
    void resume() { ResumeThread(thread); thread = nullptr; }
};
uint64_t ticks(FILETIME time) { return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
uint64_t cpuTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &exited, &kernel, &user) ? ticks(kernel) + ticks(user) : 0;
}
std::vector<DWORD> threads(DWORD pid) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    std::vector<DWORD> result;
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (snapshot.value == INVALID_HANDLE_VALUE) return result;
    if (Thread32First(snapshot.value, &entry)) do {
        if (entry.th32OwnerProcessID == pid) result.push_back(entry.th32ThreadID);
    } while (Thread32Next(snapshot.value, &entry));
    return result;
}
std::string csv(const std::string& input) {
    std::string result = "\"";
    for (char c : input) { result += c; if (c == '"') result += c; }
    return result + '"';
}
struct Counts { uint64_t wall = 0; };
struct Function { Counts self{}, inclusive{}; };
struct ThreadStats { uint64_t firstCpu = 0, lastCpu = 0, samples = 0; bool initialized = false; };
uint64_t threadCpuTime(HANDLE thread) {
    FILETIME created{}, exited{}, kernel{}, user{};
    return GetThreadTimes(thread, &created, &exited, &kernel, &user) ? ticks(kernel) + ticks(user) : 0;
}

// Used only to validate the sampler against known hot and waiting functions.
volatile uint64_t workloadSink = 1;
__declspec(noinline) void profileBusyLeaf() {
    uint64_t value = workloadSink;
    for (unsigned i = 0; i < 2000000; ++i) value = value * 6364136223846793005ULL + i;
    workloadSink = value;
}
__declspec(noinline) void profileBusyParent() { profileBusyLeaf(); workloadSink = workloadSink + 1; }
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--workload") {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(std::stoul(argv[2]));
        while (std::chrono::steady_clock::now() < end) { profileBusyParent(); Sleep(2); }
        return 0;
    }
    if (argc < 4 || argc > 6) {
        std::cerr << "usage: sample_process PID SECONDS OUTPUT_PREFIX [INTERVAL_MS=10] [DELAY_SECONDS=0]\n";
        return 2;
    }
    const DWORD pid = std::stoul(argv[1]);
    const unsigned seconds = std::stoul(argv[2]);
    const std::string prefix = argv[3];
    const unsigned interval = argc >= 5 ? std::stoul(argv[4]) : 10;
    const unsigned delay = argc == 6 ? std::stoul(argv[5]) : 0;
    if (!seconds || !interval) return 2;
    // Diagnostics are evidence. Refuse to truncate even one output from an earlier run.
    for (const char* suffix : {".functions.csv", ".stacks.csv", ".threads.csv", ".meta.txt"}) {
        std::error_code error;
        const std::string path = prefix + suffix;
        if (std::filesystem::exists(path, error) || error) {
            std::cerr << "Output already exists or cannot be checked: " << path << '\n';
            return 5;
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(delay));
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid));
    if (!process.value) { std::cerr << "OpenProcess failed: " << GetLastError() << '\n'; return 3; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(process.value, nullptr, TRUE)) {
        std::cerr << "SymInitialize failed: " << GetLastError() << '\n'; return 4;
    }
    std::map<std::pair<DWORD, std::vector<DWORD64>>, Counts> stacks;
    std::map<DWORD, ThreadStats> threadStats;
    uint64_t samples = 0, contextFailures = 0, openFailures = 0;
    const uint64_t cpuStart = cpuTime(process.value);
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
        const auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval);
        for (DWORD tid : threads(pid)) {
            Handle thread(OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid));
            if (!thread.value) { ++openFailures; continue; }
            auto& stats = threadStats[tid];
            stats.lastCpu = threadCpuTime(thread.value);
            if (!stats.initialized) { stats.firstCpu = stats.lastCpu; stats.initialized = true; }
            if (SuspendThread(thread.value) == DWORD(-1)) { ++contextFailures; continue; }
            ResumeGuard resume{thread.value};
            CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
            std::vector<DWORD64> stack;
            if (GetThreadContext(thread.value, &context)) {
                STACKFRAME64 frame{};
                frame.AddrPC = {context.Rip, 0, AddrModeFlat};
                frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
                frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
                stack.push_back(context.Rip);
                for (unsigned depth = 1; depth < 64; ++depth) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process.value, thread.value, &frame, &context,
                                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
                        break;
                    // StackWalk's first result can repeat the captured leaf; retain genuine recursion deeper down.
                    if (depth == 1 && frame.AddrPC.Offset == stack.front()) continue;
                    stack.push_back(frame.AddrPC.Offset);
                }
            } else ++contextFailures;
            resume.resume();
            if (!stack.empty()) {
                ++stacks[{tid, stack}].wall;
                ++samples; ++stats.samples;
            }
        }
        std::this_thread::sleep_until(next);
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const uint64_t cpuEnd = cpuTime(process.value);
    std::map<DWORD64, std::string> names;
    auto name = [&](DWORD64 address) -> const std::string& {
        auto found = names.find(address); if (found != names.end()) return found->second;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 2048]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 2047;
        DWORD64 displacement = 0;
        std::string resolved;
        if (SymFromAddr(process.value, address, &displacement, symbol)) resolved = symbol->Name;
        else {
            IMAGEHLP_MODULE64 module{}; module.SizeOfStruct = sizeof(module);
            char raw[64]{};
            if (SymGetModuleInfo64(process.value, address, &module)) {
                snprintf(raw, sizeof(raw), "+0x%llx", address - module.BaseOfImage);
                resolved = std::string(module.ModuleName) + raw;
            } else { snprintf(raw, sizeof(raw), "0x%llx", address); resolved = raw; }
        }
        return names.emplace(address, resolved).first->second;
    };
    std::map<std::pair<DWORD, std::string>, Function> functions;
    std::ofstream raw(prefix + ".stacks.csv"), output(prefix + ".functions.csv"), meta(prefix + ".meta.txt"), threadOutput(prefix + ".threads.csv");
    if (!raw || !output || !meta || !threadOutput) { SymCleanup(process.value); std::cerr << "Cannot write output prefix\n"; return 5; }
    raw << "thread_id,wall_samples,stack_leaf_to_root\n";
    for (const auto& [key, count] : stacks) {
        const auto& [tid, stack] = key;
        std::string path; std::set<std::string> seen;
        for (auto address : stack) {
            const auto& symbol = name(address);
            if (!path.empty()) path += " <- "; path += symbol;
            if (seen.insert(symbol).second) {
                functions[{tid, symbol}].inclusive.wall += count.wall;
            }
        }
        functions[{tid, name(stack.front())}].self.wall += count.wall;
        raw << tid << ',' << count.wall << ',' << csv(path) << '\n';
    }
    std::vector<std::pair<std::pair<DWORD, std::string>, Function>> sorted(functions.begin(), functions.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.self.wall > b.second.self.wall; });
    output << "thread_id,function,wall_self_samples,wall_inclusive_samples\n";
    for (const auto& [key, count] : sorted)
        output << key.first << ',' << csv(key.second) << ',' << count.self.wall << ',' << count.inclusive.wall << '\n';
    threadOutput << "thread_id,observed_cpu_seconds,wall_samples\n";
    for (const auto& [tid, stats] : threadStats)
        threadOutput << tid << ',' << double(stats.lastCpu - stats.firstCpu) / 1e7 << ',' << stats.samples << '\n';
    meta << "pid=" << pid << "\nwall_seconds=" << elapsed << "\nprocess_cpu_seconds=" << double(cpuEnd - cpuStart) / 1e7
         << "\ninterval_ms=" << interval << "\nwall_samples=" << samples
         << "\ncontext_failures=" << contextFailures << "\nopen_thread_failures=" << openFailures
         << "\nmethod=external all-thread suspended stack snapshots; process and thread CPU totals measured independently"
         << "\nlimits=wall samples include waits and are not CPU-cost attribution; per-thread CPU totals span first to last observation;"
            " inclusive counts deduplicate recursion; max depth 64; suspension/unwinding adds overhead; compare performance using unprofiled runs;"
            " CPU seconds include all process threads, not sampler; missing PDBs retain module offsets\n";
    SymCleanup(process.value);
    std::cout << "Wrote " << prefix << ".{functions.csv,stacks.csv,threads.csv,meta.txt}: " << samples << " wall snapshots\n";
    return samples ? 0 : 6;
}
