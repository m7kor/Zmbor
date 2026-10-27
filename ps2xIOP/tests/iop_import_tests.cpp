#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/imports/iop_imports.h"
#include "emulator/imports/iop_loadcore.h"
#include "emulator/imports/iop_stdio.h"
#include "emulator/imports/iop_sysclib.h"
#include "emulator/imports/iop_timrman.h"
#include "emulator/services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

#include <cstdint>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;

    constexpr uint32_t kExportMagic = 0x41C00000u;
    constexpr int32_t kLibraryNotFound = -213;
    constexpr int32_t kIllegalLibrary = -214;

    class NullHost : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    class CdRootHost final : public NullHost
    {
    public:
        explicit CdRootHost(std::filesystem::path rootPath)
            : root(std::move(rootPath))
        {
        }

        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdRoot ? root.string() : std::string{};
        }

    private:
        std::filesystem::path root;
    };

    class RecordingExecutor final : public IopGuestExecutor
    {
    public:
        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t gp) override
        {
            ++calls;
            lastAddress = address;
            lastArgument = a0;
            lastGp = gp;
            return callbackResult;
        }

        uint32_t callbackResult = 0u;
        uint32_t calls = 0u;
        uint32_t lastAddress = 0u;
        uint32_t lastArgument = 0u;
        uint32_t lastGp = 0u;
    };

    bool expect(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    bool testLoadcoreRebootLibraryMode()
    {
        IopMemory memory;
        IopImportRegistry imports(memory);
        IopLoadcore loadcore(memory, imports);

        IopCpuState cpu{};
        cpu.gpr[4] = 0u;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kIllegalLibrary,
                    "loadcore:27 did not reject a null export table"))
            return false;

        constexpr uint32_t table = 0x1000u;
        memory.write32(table, kExportMagic);
        memory.write16(table + 8u, 0x0101u);
        memory.write16(table + 10u, 0x1234u);
        const char name[8] = {'t', 'e', 's', 't', 'l', 'i', 'b', '\0'};
        (void)memory.writeRam(table + 12u, name, sizeof(name));
        memory.write32(table + 20u, 0u);

        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a valid export table") ||
            !expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a valid export table") ||
            !expect(memory.read16(table + 10u) == 0x1232u,
                    "loadcore:27 did not replace only export mode bits 1 and 2"))
            return false;

        constexpr uint32_t invalidTable = 0x1100u;
        memory.write32(invalidTable, 0xDEADBEEFu);
        cpu = {};
        cpu.gpr[4] = invalidTable;
        cpu.gpr[5] = 6u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 did not consume an invalid-table call") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kLibraryNotFound,
                    "loadcore:27 returned the wrong invalid-table error"))
            return false;

        if (!expect(imports.registerExportTable(table), "test export table did not register"))
            return false;
        memory.write32(table, 0u);
        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 6u;
        return expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a registered table") &&
               expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a registered table") &&
               expect(memory.read16(table + 10u) == 0x1236u,
                      "loadcore:27 did not update a registered table's mode");
    }

    bool pollEvent(IopKernel &kernel, int eventId, uint32_t bits, uint32_t resultAddress, int32_t expected)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(eventId);
        cpu.gpr[5] = bits;
        cpu.gpr[6] = 0u; // WEF_AND
        cpu.gpr[7] = resultAddress;
        return expect(kernel.dispatchEventImport(11u, cpu), "PollEventFlag was not handled") &&
               expect(static_cast<int32_t>(cpu.gpr[2]) == expected, "PollEventFlag returned an unexpected result");
    }

    bool testCdvdSpecialControl()
    {
        NullHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t param = 0x2000u;
        constexpr uint32_t eventResult = 0x2010u;
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(-11); // sceCdSC: return cdvdman interrupt event flag
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "cdvdman:50 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "sceCdSC(-11) did not return a valid event flag"))
            return false;
        const int eventId = static_cast<int>(cpu.gpr[2]);

        if (!pollEvent(kernel, eventId, 0x29u, eventResult, 0) ||
            !expect(memory.read32(eventResult) == 0x29u, "cdvdman event flag did not start with bits 0x29"))
            return false;

        IopCpuState clear{};
        clear.gpr[4] = static_cast<uint32_t>(eventId);
        clear.gpr[5] = ~0x29u;
        if (!expect(kernel.dispatchEventImport(8u, clear), "ClearEventFlag was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, -418))
            return false;

        cpu = {};
        cpu.gpr[4] = 0x12345u;
        if (!expect(cdvd.dispatchImport(7u, cpu), "sceCdSeek was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, 0))
            return false;

        memory.write8(param, 0x30u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-2);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-2) was not handled") ||
            !expect(cpu.gpr[2] == 0x30u, "sceCdSC(-2) did not store the low-byte error"))
            return false;

        memory.write32(param, 0u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-1) was not handled") ||
            !expect(cpu.gpr[2] == 0u, "sceCdSC(-1) returned the wrong initial stream state") ||
            !expect(memory.read32(param) == 0x30u, "sceCdSC(-1) did not publish the last error"))
            return false;

        cpu = {};
        cpu.gpr[4] = 2u;
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(2) was not handled") ||
            !expect(cpu.gpr[2] == 2u, "sceCdSC(2) did not update the stream state"))
            return false;

        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        return expect(cdvd.dispatchImport(50u, cpu), "second sceCdSC(-1) was not handled") &&
               expect(cpu.gpr[2] == 2u, "sceCdSC(-1) did not preserve the stream state");
    }

    bool testCdvdSearchFile()
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("ps2x-iop-cdvd-search-" + suffix);
        const std::filesystem::path movieDirectory = root / "MOVIE";
        const std::filesystem::path moviePath = movieDirectory / "OPENING.PSS";
        std::error_code error;
        std::filesystem::create_directories(movieDirectory, error);
        if (!expect(!error, "could not create the temporary CD root"))
            return false;
        {
            std::ofstream movie(moviePath, std::ios::binary);
            movie.write("PSS!", 4);
        }

        CdRootHost host(root);
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t resultAddress = 0x2400u;
        constexpr uint32_t pathAddress = 0x2480u;
        const char path[] = "cdrom0:\\movie\\opening.pss;1";
        (void)memory.writeRam(pathAddress, path, sizeof(path));

        IopCpuState cpu{};
        cpu.gpr[4] = resultAddress;
        cpu.gpr[5] = pathAddress;
        const bool handled = cdvd.dispatchImport(10u, cpu);
        const bool passed =
            expect(handled, "cdvdman:10 was not handled") &&
            expect(cpu.gpr[2] == 1u, "sceCdSearchFile did not find a case-insensitive ISO path") &&
            expect(memory.read32(resultAddress) >= 20u, "sceCdSearchFile returned an invalid LSN") &&
            expect(memory.read32(resultAddress + 4u) == 4u, "sceCdSearchFile returned the wrong size") &&
            expect(memory.readString(resultAddress + 8u, 16u) == "OPENING.PSS",
                   "sceCdSearchFile returned the wrong file name");

        std::filesystem::remove_all(root, error);
        return passed;
    }

    bool testTimrmanPeriodicCallback()
    {
        IopTimrman timrman;
        timrman.reset();
        IopCpuState cpu{};

        cpu.gpr[4] = 1u; // SYSCLK
        cpu.gpr[5] = 32u;
        cpu.gpr[6] = 1u;
        if (!expect(timrman.dispatchImport(4u, cpu, 100u), "AllocHardTimer was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "AllocHardTimer did not allocate a 32-bit timer"))
            return false;
        const uint32_t timerId = cpu.gpr[2];

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 100u;
        cpu.gpr[6] = 0x12340u;
        cpu.gpr[7] = 0x45670u;
        cpu.gpr[28] = 0x89AB0u;
        if (!expect(timrman.dispatchImport(20u, cpu, 100u), "SetTimerHandler was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetTimerHandler failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = 0u;
        cpu.gpr[7] = 1u;
        if (!expect(timrman.dispatchImport(22u, cpu, 100u), "SetupHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetupHardTimer failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        if (!expect(timrman.dispatchImport(23u, cpu, 100u), "StartHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "StartHardTimer failed") ||
            !expect(timrman.nextEventCycle(1000u) == 200u, "timer compare was scheduled at the wrong cycle"))
            return false;

        RecordingExecutor executor;
        executor.callbackResult = 100u;
        timrman.serviceDue(199u, executor);
        if (!expect(executor.calls == 0u, "timer callback ran too early"))
            return false;
        timrman.serviceDue(200u, executor);
        return expect(executor.calls == 1u, "timer callback did not run") &&
               expect(executor.lastAddress == 0x12340u, "timer called the wrong handler") &&
               expect(executor.lastArgument == 0x45670u, "timer passed the wrong common argument") &&
               expect(executor.lastGp == 0x89AB0u, "timer callback lost the registering module GP") &&
               expect(timrman.nextEventCycle(1000u) == 300u, "timer callback return did not rearm compare");
    }
}

namespace
{
    class ConsoleHost final : public NullHost
    {
    public:
        void consoleWrite(std::string_view text) override { tty.append(text); }
        std::string tty;
    };

    void putString(IopMemory &memory, uint32_t address, std::string_view s)
    {
        (void)memory.writeRam(address, s.data(), s.size());
        memory.write8(address + static_cast<uint32_t>(s.size()), 0u);
    }

    // sysclib sprintf against the output a real PS2 printed for the same calls (ps2autotests
    // tests/kernel/iop/libc/sprintf.expected, transcribed row by row; the third-party file is not needed here).
    bool testSprintfMatchesRealPs2Rows()
    {
        struct Row
        {
            const char *format;
            uint32_t arg;
            const char *expected;
        };
        const Row rows[] = {
            {"%d", 32u, "32"}, {"%03d", 45u, "045"}, {"%3d", 12u, " 12"}, {"%3.3d", 87u, "087"},
            {"%+6d", 948u, "  +948"}, {"%+6d", static_cast<uint32_t>(-948), "  -948"}, {"%6d", static_cast<uint32_t>(-948), "  -948"},
            {"%06d", static_cast<uint32_t>(-948), "-00948"}, {"%+06d", 948u, "+00948"}, {"%u", static_cast<uint32_t>(-948), "4294966348"},
            {"%x", 0xDEADBEEFu, "deadbeef"}, {"%X", 0xDEADBEEFu, "DEADBEEF"}, {"%08x", 23u, "00000017"}, {"%8x", 12u, "       c"},
            {"%p", 0x01234567u, "1234567"}, {"%c", 'j', "j"},
        };
        IopMemory memory;
        IopSysclib sysclib(memory);
        constexpr uint32_t kBuffer = 0x10000u, kFormat = 0x11000u, kString = 0x12000u;
        for (const Row &row : rows)
        {
            putString(memory, kFormat, row.format);
            IopCpuState cpu{};
            cpu.gpr[4] = kBuffer;
            cpu.gpr[5] = kFormat;
            cpu.gpr[6] = row.arg;
            if (!expect(sysclib.dispatchImport(19u, cpu), "sysclib:19 sprintf not handled"))
                return false;
            const std::string got = memory.readString(kBuffer, 256u);
            if (!expect(got == row.expected, std::string("sprintf(\"") + row.format + "\") gave \"" + got + "\", real PS2 \"" + row.expected + "\"") ||
                !expect(cpu.gpr[2] == got.size(), "sprintf returned the wrong length"))
                return false;
        }
        // "%s%3.3d" with "myfile", 0: two arguments (a2, a3).
        putString(memory, kFormat, "%s%3.3d");
        putString(memory, kString, "myfile");
        IopCpuState cpu{};
        cpu.gpr[4] = kBuffer;
        cpu.gpr[5] = kFormat;
        cpu.gpr[6] = kString;
        cpu.gpr[7] = 0u;
        return expect(sysclib.dispatchImport(19u, cpu), "sysclib:19 sprintf not handled") &&
               expect(memory.readString(kBuffer, 256u) == "myfile000", "sprintf(\"%s%3.3d\") differs from the real PS2 row");
    }

    // stdio printf reads variadic arguments past a3 from the caller's stack (o32: slot n at sp + 4n) and writes the
    // formatted text to the host console; vsprintf reads a va_list. Expected values computed by hand.
    bool testPrintfStackArgumentsAndVaList()
    {
        IopMemory memory;
        ConsoleHost host;
        IopStdio stdio(host, memory);
        constexpr uint32_t kFormat = 0x11000u, kSp = 0x20000u, kList = 0x21000u, kBuffer = 0x10000u;
        putString(memory, kFormat, "%d %d %d %d %08x|%-4s|%5.2s|%%\n");
        putString(memory, 0x12000u, "ab");
        putString(memory, 0x12100u, "xyz");
        IopCpuState cpu{};
        cpu.gpr[4] = kFormat;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = 2u;
        cpu.gpr[7] = 3u;
        cpu.gpr[29] = kSp;
        memory.write32(kSp + 16u, 4u);          // slot 4
        memory.write32(kSp + 20u, 0xBEEFu);     // slot 5
        memory.write32(kSp + 24u, 0x12000u);    // slot 6
        memory.write32(kSp + 28u, 0x12100u);    // slot 7
        const std::string want = "1 2 3 4 0000beef|ab  |   xy|%\n";
        if (!expect(stdio.dispatchImport(4u, cpu), "stdio:4 printf not handled") ||
            !expect(host.tty == want, "printf output \"" + host.tty + "\", expected \"" + want + "\"") ||
            !expect(cpu.gpr[2] == want.size(), "printf returned the wrong length"))
            return false;

        putString(memory, kFormat, "%u-%lld-%x");
        memory.write32(kList + 0u, 7u);
        memory.write32(kList + 8u, 0x00000001u); // 64-bit value aligned to 8: low word first
        memory.write32(kList + 12u, 0x00000002u);
        memory.write32(kList + 16u, 0xABu);
        IopSysclib sysclib(memory);
        IopCpuState v{};
        v.gpr[4] = kBuffer;
        v.gpr[5] = kFormat;
        v.gpr[6] = kList;
        return expect(sysclib.dispatchImport(42u, v), "sysclib:42 vsprintf not handled") &&
               expect(memory.readString(kBuffer, 256u) == "7-8589934593-ab", "vsprintf of a va_list with an aligned 64-bit value");
    }
}

int main()
{
    if (!testLoadcoreRebootLibraryMode() || !testCdvdSpecialControl() || !testCdvdSearchFile() ||
        !testTimrmanPeriodicCallback() || !testSprintfMatchesRealPs2Rows() || !testPrintfStackArgumentsAndVaList())
        return 1;
    std::cout << "ps2xIOP import tests passed\n";
    return 0;
}
