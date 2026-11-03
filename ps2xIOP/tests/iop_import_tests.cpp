#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/imports/iop_imports.h"
#include "emulator/imports/iop_intrman.h"
#include "emulator/imports/iop_loadcore.h"
#include "emulator/imports/iop_stdio.h"
#include "emulator/imports/iop_sysclib.h"
#include "emulator/imports/iop_timrman.h"
#include "emulator/iop_emulator.h"
#include "emulator/services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/sif0_fifo.h"

#include "emulator/iop_emulator_const.h"

#include <cstdint>
#include <chrono>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>
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
            !pollEvent(kernel, eventId, 0x29u, eventResult, -421)) // KE_EVF_COND (real PS2: kernel/iop/eventflag/poll)
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

namespace
{
    // JALR reads its target before writing the link: `jalr $t0, $t0` jumps to the old $t0 (a real PS2 prints
    // "jalr: rs/rd match: 00000002" in ps2autotests cpu/iop/branchdelay). rd = 0 discards the link.
    bool testJalrReadsTargetBeforeLink()
    {
        IopMemory memory;
        IopCpuCore core(memory);
        constexpr uint32_t kPc = 0x1000u, kTarget = 0x2000u;
        memory.write32(kPc, 0x01004009u);      // jalr $t0, $t0  (rs = 8, rd = 8)
        memory.write32(kPc + 4u, 0u);          // nop (delay slot)
        IopCpuState cpu{};
        cpu.pc = kPc;
        cpu.gpr[8] = kTarget;
        (void)core.executeInstruction(cpu);
        (void)core.executeInstruction(cpu);
        if (!expect(cpu.pc == kTarget, "jalr rs == rd did not jump to the old rs value") ||
            !expect(cpu.gpr[8] == kPc + 8u, "jalr rs == rd did not write the link to rd"))
            return false;

        memory.write32(kPc, 0x01000009u);      // jalr $zero, $t0 (rd = 0)
        IopCpuState zero{};
        zero.pc = kPc;
        zero.gpr[8] = kTarget;
        zero.gpr[31] = 0xABCDu;
        (void)core.executeInstruction(zero);
        (void)core.executeInstruction(zero);
        return expect(zero.pc == kTarget, "jalr with rd = 0 did not jump") &&
               expect(zero.gpr[31] == 0xABCDu && zero.gpr[0] == 0u, "jalr with rd = 0 wrote a link register");
    }
}

namespace
{
    // Kernel-level checks of the IOP kernel against what a real PS2 printed for the ps2autotests kernel/iop programs
    // (values transcribed from their .expected files; the third-party programs are not needed here).
    struct KernelRig
    {
        IopMemory memory;
        IopKernel kernel{memory};
        IopThread *running = nullptr;
        KernelRig() { kernel.reset(); }

        int32_t call(bool (IopKernel::*dispatch)(uint16_t, IopCpuState &), uint16_t ordinal, IopCpuState &cpu,
                     std::initializer_list<uint32_t> args)
        {
            uint32_t reg = 4u;
            for (uint32_t a : args)
                cpu.gpr[reg++] = a;
            if (!(kernel.*dispatch)(ordinal, cpu))
                return 0x7FFFFFFF;
            return static_cast<int32_t>(cpu.gpr[2]);
        }
        int32_t thread(uint16_t ordinal, IopCpuState &cpu, std::initializer_list<uint32_t> args)
        {
            uint32_t reg = 4u;
            for (uint32_t a : args)
                cpu.gpr[reg++] = a;
            return kernel.dispatchThreadImport(ordinal, cpu, 0u) ? static_cast<int32_t>(cpu.gpr[2]) : 0x7FFFFFFF;
        }
        int32_t createThread(uint32_t attr, uint32_t entry, uint32_t stack, uint32_t priority, IopCpuState &cpu)
        {
            constexpr uint32_t kParam = 0x30000u;
            memory.write32(kParam + 0u, attr);
            memory.write32(kParam + 4u, 0u);
            memory.write32(kParam + 8u, entry);
            memory.write32(kParam + 12u, stack);
            memory.write32(kParam + 16u, priority);
            return thread(4u, cpu, {kParam});
        }
        // Makes `id` the running thread, as the scheduler would.
        IopThread &run(int id)
        {
            IopThread *next = kernel.beginNextReady(0u);
            if (next == nullptr || next->id != id)
                throw std::runtime_error("the scheduler did not pick the expected thread");
            return *(running = next);
        }
    };

    bool testKernelThreadChecksMatchRealPs2()
    {
        KernelRig rig;
        IopCpuState host{};
        constexpr uint32_t kEntry = 0x40000u;
        // kernel/iop/thread/create
        if (!expect(rig.createThread(0xFFFFFFFFu, kEntry, 0x800u, 10u, host) == -401, "attr ~0 must be KE_ILLEGAL_ATTR") ||
            !expect(rig.createThread(0x00100000u, kEntry, 0x800u, 10u, host) == -401, "TH_NO_FILLSTACK is refused on a real PS2") ||
            !expect(rig.createThread(0x00200000u, kEntry, 0x800u, 10u, host) == -401, "TH_CLEAR_STACK is refused on a real PS2") ||
            !expect(rig.createThread(0x02000000u, 3u, 0x800u, 10u, host) == -402, "entry 3 must be KE_ILLEGAL_ENTRY") ||
            !expect(rig.createThread(0x02000000u, kEntry, 0x800u, 0u, host) == -403, "priority 0 must be KE_ILLEGAL_PRIORITY") ||
            !expect(rig.createThread(0x02000000u, kEntry, 0x800u, 127u, host) == -403, "priority 127 must be KE_ILLEGAL_PRIORITY") ||
            !expect(rig.createThread(0x02000000u, kEntry, 300u, 10u, host) == -404, "stack 300 must be KE_ILLEGAL_SIZE") ||
            !expect(rig.createThread(0x02000000u, kEntry, 0xFFFFFFFFu, 10u, host) == -400, "stack ~0 must be KE_NO_MEMORY") ||
            !expect(rig.createThread(0x00000000u, 0u, 0x800u, 10u, host) > 0, "a NULL entry is accepted"))
            return false;
        const int32_t low = rig.createThread(0x03000008u, kEntry, 0x800u, 9u, host);
        constexpr uint32_t kInfo = 0x31000u;
        if (!expect(low > 0, "TH_UMODE | TH_ASM | TH_C is accepted") ||
            !expect(rig.thread(22u, host, {static_cast<uint32_t>(low), kInfo}) == 0, "ReferThreadStatus of a dormant thread") ||
            !expect(rig.memory.read32(kInfo + 8u) == 0x10u && rig.memory.read32(kInfo + 28u) == 9u && rig.memory.read32(kInfo + 32u) == 0u,
                    "a never-started thread is DORMANT with current priority 0") ||
            !expect(rig.thread(22u, host, {0x7777u, kInfo}) == -407, "stat with invalid thread id must be KE_UNKNOWN_THID") ||
            !expect(rig.thread(5u, host, {0x7777u}) == -407, "delete with invalid tid must be KE_UNKNOWN_THID") ||
            !expect(rig.thread(18u, host, {static_cast<uint32_t>(low)}) == -416, "release-wait of a dormant thread must be KE_NOT_WAIT"))
            return false;

        // A running thread of priority 8 starts the priority-9 one: no preemption, and the new one is READY.
        const int32_t main = rig.createThread(0x02000000u, kEntry, 0x800u, 8u, host);
        if (!expect(rig.thread(6u, host, {static_cast<uint32_t>(main), 0u}) == 0, "StartThread"))
            return false;
        IopThread &self = rig.run(main);
        IopCpuState &cpu = self.cpu;
        if (!expect(rig.thread(6u, cpu, {static_cast<uint32_t>(low), 0u}) == 0 && !cpu.yielded, "a lower-priority start does not preempt") ||
            !expect(rig.thread(22u, cpu, {0u, kInfo}) == 0 && rig.memory.read32(kInfo + 8u) == 0x01u && rig.memory.read32(kInfo + 32u) == 8u,
                    "TH_SELF reads the RUNNING caller at priority 8") ||
            !expect(rig.thread(22u, cpu, {static_cast<uint32_t>(low), kInfo}) == 0 && rig.memory.read32(kInfo + 8u) == 0x02u &&
                        rig.memory.read32(kInfo + 32u) == 9u,
                    "a started thread is READY at its initial priority") ||
            !expect(rig.thread(5u, cpu, {0u}) == -406, "delete with TH_SELF must be KE_ILLEGAL_THID") ||
            !expect(rig.thread(5u, cpu, {static_cast<uint32_t>(main)}) == -414, "delete of the running thread must be KE_NOT_DORMANT") ||
            !expect(rig.thread(5u, cpu, {static_cast<uint32_t>(low)}) == -414, "delete after start must be KE_NOT_DORMANT") ||
            !expect(rig.thread(18u, cpu, {0u}) == -406 && rig.thread(18u, cpu, {static_cast<uint32_t>(main)}) == -406,
                    "release-wait of self must be KE_ILLEGAL_THID") ||
            !expect(rig.thread(18u, cpu, {static_cast<uint32_t>(low)}) == -416, "release-wait of a ready thread must be KE_NOT_WAIT"))
            return false;
        // CancelWakeupThread(TH_SELF) after four wakeup requests returns 4, then 0.
        self.wakeupCount = 4;
        if (!expect(rig.thread(27u, cpu, {0u}) == 4 && rig.thread(27u, cpu, {0u}) == 0, "CancelWakeupThread(TH_SELF) returns the count, then 0") ||
            !expect(rig.thread(27u, cpu, {0x7777u}) == -407, "cancel wakeup of an invalid id must be KE_UNKNOWN_THID"))
            return false;
        // A higher-priority thread started by the running one preempts it.
        const int32_t high = rig.createThread(0x02000000u, kEntry, 0x800u, 7u, host);
        cpu.yielded = false;
        return expect(rig.thread(6u, cpu, {static_cast<uint32_t>(high), 0u}) == 0 && cpu.yielded,
                      "starting a higher-priority thread preempts the caller") &&
               expect(rig.thread(10u, cpu, {static_cast<uint32_t>(low)}) == 0 && rig.thread(10u, cpu, {static_cast<uint32_t>(low)}) == -413,
                      "TerminateThread twice: 0, then KE_DORMANT") &&
               expect(rig.thread(5u, cpu, {static_cast<uint32_t>(low)}) == 0 && rig.thread(5u, cpu, {static_cast<uint32_t>(low)}) == -407,
                      "delete after terminate succeeds, delete after delete is KE_UNKNOWN_THID");
    }

    bool testKernelWaitsAndEventFlagsMatchRealPs2()
    {
        KernelRig rig;
        IopCpuState host{};
        constexpr uint32_t kParam = 0x32000u, kResult = 0x32100u;
        rig.memory.write32(kParam + 0u, 0u);
        rig.memory.write32(kParam + 4u, 0u);
        rig.memory.write32(kParam + 8u, 0xFu);
        const int32_t flag = rig.call(&IopKernel::dispatchEventImport, 4u, host, {kParam});
        rig.memory.write32(kResult, 0xCCCCu);
        // kernel/iop/eventflag/poll
        if (!expect(rig.call(&IopKernel::dispatchEventImport, 11u, host, {static_cast<uint32_t>(flag), 0xF0u, 1u, kResult}) == -421 &&
                        rig.memory.read32(kResult) == 0xCCCCu,
                    "an unmatched poll is KE_EVF_COND and leaves the result untouched") ||
            !expect(rig.call(&IopKernel::dispatchEventImport, 11u, host, {static_cast<uint32_t>(flag), 0u, 1u, kResult}) == -423,
                    "a zero pattern is KE_EVF_ILPAT") ||
            !expect(rig.call(&IopKernel::dispatchEventImport, 11u, host, {0x7777u, 1u, 1u, kResult}) == -409,
                    "an invalid id is KE_UNKNOWN_EVFID") ||
            !expect(rig.call(&IopKernel::dispatchEventImport, 11u, host, {static_cast<uint32_t>(flag), 0x3u, 0x11u, kResult}) == 0 &&
                        rig.memory.read32(kResult) == 0xFu,
                    "OR | CLEAR returns the bits before clearing") ||
            !expect(rig.call(&IopKernel::dispatchEventImport, 11u, host, {static_cast<uint32_t>(flag), 0x3u, 1u, 0u}) == -421,
                    "the second poll after CLEAR fails"))
            return false;

        // A thread waiting on the flag: a single-waiter flag refuses a second waiter, deletion releases the waiter.
        IopCpuState scratch{};
        const int32_t waiter = rig.createThread(0x02000000u, 0x40000u, 0x800u, 7u, scratch);
        (void)rig.thread(6u, scratch, {static_cast<uint32_t>(waiter), 0u});
        IopThread &thread = rig.run(waiter);
        if (!expect(rig.call(&IopKernel::dispatchEventImport, 10u, thread.cpu, {static_cast<uint32_t>(flag), ~0u, 0x11u, 0u}) == 0 &&
                        thread.cpu.yielded,
                    "WaitEventFlag blocks the calling thread"))
            return false;
        rig.kernel.endTimeslice(thread, kThreadReturnSentinel);
        constexpr uint32_t kInfo = 0x32200u;
        if (!expect(rig.thread(22u, host, {static_cast<uint32_t>(waiter), kInfo}) == 0 && rig.memory.read32(kInfo + 8u) == 0x04u &&
                        rig.memory.read32(kInfo + 36u) == 4u,
                    "a thread waiting on an event flag is WAIT with wait type 4") ||
            !expect(rig.call(&IopKernel::dispatchEventImport, 11u, host, {static_cast<uint32_t>(flag), 1u, 1u, kResult}) == -422,
                    "a busy single-waiter flag is KE_EVF_MULTI") ||
            !expect(rig.call(&IopKernel::dispatchEventImport, 5u, host, {static_cast<uint32_t>(flag)}) == 0, "delete while a thread waits") ||
            !expect(static_cast<int32_t>(thread.cpu.gpr[2]) == -425 && thread.state == IopThreadState::Ready,
                    "the waiter returns KE_WAIT_DELETE") ||
            !expect(rig.call(&IopKernel::dispatchEventImport, 5u, host, {static_cast<uint32_t>(flag)}) == -409,
                    "deleting a deleted flag is KE_UNKNOWN_EVFID"))
            return false;

        // kernel/iop/thread/releasewait: a released sleeper returns KE_RELEASE_WAIT.
        IopThread &again = rig.run(waiter);
        (void)rig.thread(24u, again.cpu, {});
        rig.kernel.endTimeslice(again, kThreadReturnSentinel);
        return expect(rig.thread(18u, host, {static_cast<uint32_t>(waiter)}) == 0, "ReleaseWaitThread of a sleeper") &&
               expect(static_cast<int32_t>(again.cpu.gpr[2]) == -418, "the sleeper returns KE_RELEASE_WAIT");
    }

    bool testMessageBoxOrderAndLinkingMatchRealPs2()
    {
        KernelRig rig;
        IopCpuState host{};
        constexpr uint32_t kParam = 0x33000u, kMsg = 0x33100u, kVar = 0x33200u, kInfo = 0x33300u;
        const auto createBox = [&](uint32_t attr)
        {
            rig.memory.write32(kParam + 0u, attr);
            rig.memory.write32(kParam + 4u, 0u);
            return rig.call(&IopKernel::dispatchMessageBoxImport, 4u, host, {kParam});
        };
        const auto message = [&](uint32_t i, uint8_t priority)
        {
            const uint32_t m = kMsg + i * 16u;
            rig.memory.write32(m, 0u);
            rig.memory.write8(m + 4u, priority);
            return m;
        };
        // kernel/iop/messagebox/poll and send
        const int32_t fifo = createBox(0u);
        const uint32_t m0 = message(0u, 3u);
        if (!expect(rig.call(&IopKernel::dispatchMessageBoxImport, 9u, host, {kVar, static_cast<uint32_t>(fifo)}) == -424,
                    "polling an empty box is KE_MBOX_NOMSG") ||
            !expect(rig.call(&IopKernel::dispatchMessageBoxImport, 9u, host, {kVar, 0x7777u}) == -410, "an invalid box is KE_UNKNOWN_MBXID") ||
            !expect(rig.call(&IopKernel::dispatchMessageBoxImport, 6u, host, {static_cast<uint32_t>(fifo), m0}) == 0 &&
                        rig.memory.read32(m0) == m0,
                    "a single message points at itself"))
            return false;
        const int32_t prio = createBox(4u);
        const uint32_t m1 = message(1u, 3u), m2 = message(2u, 2u), m3 = message(3u, 1u);
        for (uint32_t m : {m1, m2, m3})
            (void)rig.call(&IopKernel::dispatchMessageBoxImport, 6u, host, {static_cast<uint32_t>(prio), m});
        (void)rig.call(&IopKernel::dispatchMessageBoxImport, 11u, host, {static_cast<uint32_t>(prio), kInfo});
        if (!expect(rig.memory.read32(kInfo + 12u) == 3u && rig.memory.read32(kInfo + 16u) == m3 &&
                        rig.memory.read32(m3) == m2 && rig.memory.read32(m2) == m1,
                    "MBA_MSPRI keeps messages in ascending priority (3 -> 2 -> 1 sent, 1, 2, 3 queued)") ||
            !expect(rig.call(&IopKernel::dispatchMessageBoxImport, 9u, host, {kVar, static_cast<uint32_t>(prio)}) == 0 &&
                        rig.memory.read32(kVar) == m3,
                    "polling takes the first message"))
            return false;

        // kernel/iop/messagebox/receive: three waiters of priority 7, 6, 5 on a priority-ordered box receive in
        // priority order; with FIFO order they receive in arrival order.
        for (const bool byPriority : {false, true})
        {
            KernelRig r;
            IopCpuState h{};
            r.memory.write32(kParam + 0u, byPriority ? 1u : 0u);
            r.memory.write32(kParam + 4u, 0u);
            const int32_t box = r.call(&IopKernel::dispatchMessageBoxImport, 4u, h, {kParam});
            std::vector<int32_t> ids;
            for (uint32_t i = 0; i < 3u; ++i)
            {
                const int32_t id = r.createThread(0x02000000u, 0x40000u, 0x800u, 7u - i, h);
                (void)r.thread(6u, h, {static_cast<uint32_t>(id), 0u});
                IopThread &t = r.run(id);
                (void)r.call(&IopKernel::dispatchMessageBoxImport, 8u, t.cpu, {kVar + i * 4u, static_cast<uint32_t>(box)});
                r.kernel.endTimeslice(t, kThreadReturnSentinel);
                ids.push_back(id);
            }
            for (uint32_t i = 0; i < 3u; ++i)
                (void)r.call(&IopKernel::dispatchMessageBoxImport, 6u, h, {static_cast<uint32_t>(box), kMsg + i * 16u});
            const bool fifoOrder = r.memory.read32(kVar) == kMsg && r.memory.read32(kVar + 8u) == kMsg + 32u;
            const bool prioOrder = r.memory.read32(kVar + 8u) == kMsg && r.memory.read32(kVar) == kMsg + 32u;
            if (!expect(byPriority ? prioOrder : fifoOrder, byPriority ? "MBA_THPRI wakes the highest-priority waiter first"
                                                                       : "MBA_THFIFO wakes waiters in arrival order"))
                return false;
        }
        return true;
    }

    bool testVariablePoolSizesMatchRealPs2()
    {
        KernelRig rig;
        IopCpuState host{};
        constexpr uint32_t kParam = 0x34000u, kInfo = 0x34100u;
        const auto create = [&](uint32_t attr, uint32_t size)
        {
            rig.memory.write32(kParam + 0u, attr);
            rig.memory.write32(kParam + 4u, 0x1234u);
            rig.memory.write32(kParam + 8u, size);
            return rig.call(&IopKernel::dispatchVariablePoolImport, 4u, host, {kParam});
        };
        const auto sizes = [&](int32_t id)
        {
            (void)rig.call(&IopKernel::dispatchVariablePoolImport, 11u, host, {static_cast<uint32_t>(id), kInfo});
            return std::pair<int32_t, int32_t>(static_cast<int32_t>(rig.memory.read32(kInfo + 8u)), static_cast<int32_t>(rig.memory.read32(kInfo + 12u)));
        };
        // kernel/iop/vpool/create
        if (!expect(create(2u, 0x2000u) == -401 && create(~0u, 0x2000u) == -401 && create(~0u, static_cast<uint32_t>(-0x2000)) == -401,
                    "bad attributes are KE_ILLEGAL_ATTR, checked before the size"))
            return false;
        const int32_t valid = create(0x200u, 0x2000u);
        const int32_t huge = create(0x200u, 0x20000000u);
        if (!expect(sizes(valid) == std::pair<int32_t, int32_t>(8152, 8152), "a 0x2000 pool has 8152 bytes") ||
            !expect(sizes(huge) == std::pair<int32_t, int32_t>(-4, -4), "a pool without a heap reports -4"))
            return false;
        // kernel/iop/vpool/pallocate
        const int32_t pool = create(0u, 0x1000u);
        const auto alloc = [&](uint32_t size) { return rig.call(&IopKernel::dispatchVariablePoolImport, 7u, host, {static_cast<uint32_t>(pool), size}); };
        const auto freeBlock = [&](int32_t address) { (void)rig.call(&IopKernel::dispatchVariablePoolImport, 9u, host, {static_cast<uint32_t>(pool), static_cast<uint32_t>(address)}); };
        struct Row { uint32_t size; int32_t free; };
        for (const Row row : {Row{128u, 3920}, Row{129u, 3912}, Row{136u, 3912}, Row{137u, 3904}, Row{2048u, 2000},
                              Row{static_cast<uint32_t>(-1), 4048}, Row{static_cast<uint32_t>(-5), 4048}})
        {
            const int32_t block = alloc(row.size);
            if (!expect(block > 0 && sizes(pool).second == row.free, "pAllocateVpl block cost differs from the real PS2"))
                return false;
            freeBlock(block);
        }
        // Two blocks freed in address order: the second must merge with the range freed before it, or the whole
        // heap (one block of 4056 - 8 bytes) no longer fits.
        const int32_t first = alloc(0x100u);
        const int32_t second = alloc(0x100u);
        freeBlock(first);
        freeBlock(second);
        const int32_t whole = alloc(4048u);
        if (!expect(first > 0 && second > 0 && whole > 0 && sizes(pool).second == 0, "freed neighbours coalesce in both directions"))
            return false;
        freeBlock(whole);
        return expect(alloc(0x1000u) == -427 && alloc(0x8000u) == -427, "a size above the pool size is KE_ILLEGAL_MEMSIZE") &&
               expect(alloc(static_cast<uint32_t>(-8)) == -400 && alloc(static_cast<uint32_t>(-128)) == -400, "-8 and -128 wrap to huge sizes: KE_NO_MEMORY") &&
               expect(rig.call(&IopKernel::dispatchVariablePoolImport, 7u, host, {~0u, 0x100u}) == -411, "an invalid pool is KE_UNKNOWN_VPLID") &&
               expect(sizes(pool).second == 4056, "freed blocks coalesce back");
    }

    // intrman RegisterIntrHandler/ReleaseIntrHandler results a real PS2 printed (kernel/iop/interrupt).
    bool testIntrHandlerRegistrationMatchesRealPs2()
    {
        struct NoExecutor final : IopGuestExecutor
        {
            uint32_t executeGuestFunction(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { return 0u; }
            uint32_t executeGuestFunctionWithBudget(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { return 0u; }
        } executor;
        IopMemory memory;
        IopIntrman intrman(memory);
        intrman.reset();
        const auto call = [&](uint16_t ordinal, uint32_t irq, uint32_t mode, uint32_t handler)
        {
            IopCpuState cpu{};
            cpu.gpr[4] = irq;
            cpu.gpr[5] = mode;
            cpu.gpr[6] = handler;
            (void)intrman.dispatchImport(ordinal, cpu, executor);
            return static_cast<int32_t>(cpu.gpr[2]);
        };
        constexpr uint32_t kHandler = 0x40000u;
        return expect(call(4u, 0u, 0u, kHandler) == 0 && call(4u, 0u, 0u, kHandler) == -104, "a second handler is KE_FOUND_HANDLER") &&
               expect(call(5u, 0u, 0u, 0u) == 0 && call(5u, 0u, 0u, 0u) == -105, "release after release is KE_NOTFOUND_HANDLER") &&
               expect(call(4u, 0u, 0u, 0u) == 0 && call(4u, 0u, 0u, 0u) == 0, "a NULL handler leaves the line free") &&
               expect(call(5u, 0u, 0u, 0u) == -105, "releasing a NULL handler is KE_NOTFOUND_HANDLER") &&
               expect(call(4u, 0u, 0u, kHandler) == 0 && call(4u, 0u, 0u, 0u) == -104, "NULL after a handler is KE_FOUND_HANDLER") &&
               expect(call(4u, ~0u, 0u, kHandler) == -101 && call(5u, ~0u, 0u, 0u) == -101, "an invalid line is KE_ILLEGAL_INTRCODE") &&
               expect(call(5u, 0u, 0u, 0u) == 0 && call(4u, 0u, 0x80000000u, kHandler) == 0, "any mode value is accepted");
    }

    // SPU2 register file and sound RAM (decision D-SPU-1): values computed by hand from the register layout.
    bool testSpu2RegistersAndSoundRam()
    {
        constexpr uint32_t kSpu = 0x1F900000u, kCore1 = 0x400u;
        IopMemory memory;
        Spu2 &spu = memory.spu2();
        const auto setTsa = [&](uint32_t core, uint32_t address)
        {
            memory.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
            memory.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
        };

        memory.write16(kSpu + 3u * 0x10u, 0x1234u);
        bool ok = expect(memory.read16(kSpu + 3u * 0x10u) == 0x1234u && memory.read16(kSpu + kCore1 + 3u * 0x10u) == 0u,
                         "a voice register reads back as written, per core");
        memory.write16(kSpu + 0x1C0u, 0xFFFFu);
        ok = ok && expect(memory.read16(kSpu + 0x1C0u) == 0x000Fu, "SSA's high half holds address bits 16-19");
        memory.write32(kSpu + 0x10u, 0xBEEF1234u);
        ok = ok && expect(memory.read16(kSpu + 0x10u) == 0x1234u && memory.read16(kSpu + 0x12u) == 0xBEEFu &&
                              memory.read32(kSpu + 0x10u) == 0xBEEF1234u,
                          "a 32-bit access is two 16-bit accesses, low half first");

        setTsa(0u, 0x12345u);
        memory.write16(kSpu + 0x1ACu, 0xAAAAu);
        memory.write16(kSpu + 0x1ACu, 0xBBBBu);
        ok = ok && expect(spu.ram(0x12345u) == 0xAAAAu && spu.ram(0x12346u) == 0xBBBBu && spu.transferAddress(0) == 0x12347u,
                          "data-port writes land at TSA and advance the transfer address");
        ok = ok && expect(memory.read16(kSpu + 0x1A8u) == 0x0001u && memory.read16(kSpu + 0x1AAu) == 0x2345u,
                          "the TSA registers keep the written start address");
        setTsa(0u, 0x12345u);
        ok = ok && expect(memory.read16(kSpu + 0x1ACu) == 0xAAAAu && memory.read16(kSpu + 0x1ACu) == 0xBBBBu,
                          "data-port reads return sound RAM at the advancing address");
        memory.write16(kSpu + 0x1A8u, 0xFFF1u);
        memory.write16(kSpu + 0x1AAu, 0x0000u);
        ok = ok && expect(memory.read16(kSpu + 0x1A8u) == 0x0001u && spu.transferAddress(0) == 0x10000u,
                          "TSA's high half keeps address bits 16-19");
        setTsa(0u, 0xFFFFFu);
        memory.write16(kSpu + 0x1ACu, 0x5A5Au);
        ok = ok && expect(spu.ram(0xFFFFFu) == 0x5A5Au && spu.transferAddress(0) == 0u, "the transfer address wraps in 2 MB");

        // IRQA: core 0 at 0x12346 with ATTR bit 6; a write at 0x12345 does not hit, one at 0x12346 does.
        memory.write16(kSpu + 0x19Cu, 0x0001u);
        memory.write16(kSpu + 0x19Eu, 0x2346u);
        memory.write16(kSpu + 0x19Au, 1u << 6);
        setTsa(0u, 0x12345u);
        memory.write16(kSpu + 0x1ACu, 1u);
        ok = ok && expect((memory.interruptStatus() & (1u << 9)) == 0u, "no interrupt before IRQA is accessed");
        memory.write16(kSpu + 0x1ACu, 2u);
        ok = ok && expect((memory.interruptStatus() & (1u << 9)) != 0u && memory.read16(kSpu + 0x7C2u) == 0x0004u,
                          "an access at IRQA raises IOP INTC 9 and sets IRQINFO bit 2 (core 0)");
        memory.write16(kSpu + 0x7C2u, 0u);
        ok = ok && expect(memory.read16(kSpu + 0x7C2u) == 0x0004u, "IRQINFO is not cleared by a write");
        memory.write16(kSpu + 0x19Au, 0u);
        ok = ok && expect(memory.read16(kSpu + 0x7C2u) == 0u, "clearing ATTR bit 6 clears the core's IRQINFO bit");
        memory.setInterruptStatus(0u);
        memory.write16(kSpu + kCore1 + 0x19Cu, 0x0000u);
        memory.write16(kSpu + kCore1 + 0x19Eu, 0x0040u);
        memory.write16(kSpu + kCore1 + 0x19Au, 1u << 6);
        setTsa(0u, 0x40u);
        memory.write16(kSpu + 0x1ACu, 3u); // core 0's transfer reaches core 1's IRQA
        ok = ok && expect(memory.read16(kSpu + 0x7C2u) == 0x0008u && (memory.interruptStatus() & (1u << 9)) != 0u,
                          "any core's access is checked against both IRQAs (IRQINFO bit 3: core 1)");
        memory.write16(kSpu + kCore1 + 0x19Au, 0u);
        memory.setInterruptStatus(0u);
        setTsa(0u, 0x40u);
        memory.write16(kSpu + 0x1ACu, 3u);
        ok = ok && expect((memory.interruptStatus() & (1u << 9)) == 0u, "without ATTR bit 6 there is no interrupt");

        // KON: voice 5 of core 1 restarts from SSA = 0x34567; KON's high half holds voices 16-23.
        const uint32_t ssa5 = kSpu + kCore1 + 0x1C0u + 5u * 0xCu;
        memory.write16(ssa5, 0x0003u);
        memory.write16(ssa5 + 2u, 0x4567u);
        memory.write16(kSpu + kCore1 + 0x1A0u, 1u << 5);
        ok = ok && expect(memory.read16(ssa5 + 8u) == 0x0003u && memory.read16(ssa5 + 10u) == 0x4567u,
                          "KON loads the voice's NAX from SSA");
        memory.write16(kSpu + kCore1 + 0x1A2u, 0x01FFu);
        ok = ok && expect(spu.pendingKeyOn(1) == (0xFF0000u | (1u << 5)) && memory.read16(kSpu + kCore1 + 0x1A0u) == 0x0020u,
                          "KON latches voices 0-23 (the high half keeps 8 bits) and reads back");

        // IOP DMA channel 4 (core 0) from IOP RAM, channel 7 (core 1) back to IOP RAM.
        const uint16_t samples[4] = {0x1111u, 0x2222u, 0x3333u, 0x4444u};
        (void)memory.writeRam(0x10000u, samples, sizeof(samples));
        setTsa(0u, 0x500u);
        memory.write32(0x1F8010C0u, 0x10000u);
        memory.write32(0x1F8010C4u, (1u << 16) | 2u); // 1 block of 2 words
        memory.write32(0x1F8010C8u, 0x01000201u);     // start, from memory
        ok = ok && expect(spu.ram(0x500u) == 0x1111u && spu.ram(0x503u) == 0x4444u && spu.transferAddress(0) == 0x504u,
                          "channel 4 writes 4 halfwords at core 0's TSA");
        ok = ok && expect((memory.read16(kSpu + 0x344u) & 0x80u) != 0u && (memory.read32(0x1F8010C8u) & (1u << 24)) == 0u,
                          "the transfer sets STAT bit 7 and clears CHCR's start bit");
        const auto dma = memory.takeDmaStart();
        ok = ok && expect(dma.has_value() && dma->irq == 0x24, "the channel 4 completion is scheduled");
        memory.write16(kSpu + 0x19Au, 1u << 15);
        ok = ok && expect((memory.read16(kSpu + 0x344u) & 0x80u) == 0u && (memory.read16(kSpu + 0x19Au) & 0x8000u) == 0u,
                          "ATTR bit 15 clears STAT bit 7 and does not read back");
        setTsa(kCore1, 0x501u);
        memory.write32(0x1F801500u, 0x20000u);
        memory.write32(0x1F801504u, (1u << 16) | 1u);
        memory.write32(0x1F801508u, 0x01000200u); // start, to memory
        uint16_t back[2] = {};
        (void)memory.readRam(0x20000u, back, sizeof(back));
        ok = ok && expect(back[0] == 0x2222u && back[1] == 0x3333u && spu.transferAddress(1) == 0x503u,
                          "channel 7 reads core 1's TSA into IOP RAM");

        memory.write16(kSpu + 0x760u, 0x3FFFu);
        ok = ok && expect(memory.read16(kSpu + 0x760u) == 0x3FFFu && memory.read8(kSpu + 0x761u) == 0x3Fu,
                          "the volume block reads back; byte reads see the register");
        return ok;
    }

    // SPU2 ADPCM block decoding on sound RAM (Stage A, step 1 of the SPU2 voice engine). Expected values are
    // computed by hand from the block format and the prediction coefficients shared with the project's VAG
    // decoder (filter 1: 60/64, filter 2: 115/64 and -52/64, filter 3: 98/64 and -55/64, filter 4: 122/64 and
    // -60/64, each with the decoder's +32/64 rounding term); they are not hardware captures.
    bool testSpu2AdpcmBlockDecoding()
    {
        bool ok = true;
        const auto decode = [](const uint8_t block[16], int16_t &s1, int16_t &s2)
        {
            return Spu2::decodeAdpcmBlock(block, s1, s2);
        };

        // Filter 0, shift 0, low nibble first: 1 -> 4096, -1 -> -4096, 7 -> 28672, -8 -> -32768, then zeros.
        // History is overwritten by the block's trailing zeros.
        {
            int16_t s1 = 999, s2 = -999;
            const uint8_t block[16] = {0x00, 0x00, 0xF1, 0x87, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            const auto decoded = decode(block, s1, s2);
            ok = ok && expect(decoded.samples[0] == 4096 && decoded.samples[1] == -4096 &&
                                  decoded.samples[2] == 28672 && decoded.samples[3] == -32768 &&
                                  decoded.samples[4] == 0 && decoded.samples[27] == 0 && decoded.shift == 0 &&
                                  decoded.filter == 0 && decoded.flags == 0 && s1 == 0 && s2 == 0,
                              "filter 0 decodes low-nibble-first samples at shift 0 and updates history");
        }

        // Filter coefficients, shift 12 (so the shifted nibble is the raw value), all-zero data, history 1000/500.
        const auto filteredFirst = [&](uint8_t header)
        {
            uint8_t block[16] = {};
            block[0] = header;
            int16_t s1 = 1000, s2 = 500;
            return Spu2::decodeAdpcmBlock(block, s1, s2);
        };
        ok = ok && expect(filteredFirst(0x1C).samples[0] == 938, "filter 1: (60*1000 + 32) / 64 == 938");
        {
            const auto decoded = filteredFirst(0x2C);
            ok = ok && expect(decoded.samples[0] == 1391 && decoded.samples[1] == 1687 && decoded.samples[2] == 1901,
                              "filter 2 follows 115/64 and -52/64 through three samples");
        }
        ok = ok && expect(filteredFirst(0x3C).samples[0] == 1102, "filter 3: (98*1000 - 55*500 + 32) / 64 == 1102");
        ok = ok && expect(filteredFirst(0x4C).samples[0] == 1438, "filter 4: (122*1000 - 60*500 + 32) / 64 == 1438");

        // Clamping at both ends, filter 4, shift 12, zero data.
        {
            int16_t s1 = 32767, s2 = -32768;
            const uint8_t block[16] = {0x4C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            ok = ok && expect(decode(block, s1, s2).samples[0] == 32767, "a large positive prediction clamps to 32767");
            s1 = -32768;
            s2 = 32767;
            ok = ok && expect(decode(block, s1, s2).samples[0] == -32768, "a large negative prediction clamps to -32768");
        }

        // Out-of-range header fields follow the VAG decoder's clamping: shift 15 -> 9, filter 7 -> 0.
        {
            int16_t s1 = 0, s2 = 0;
            const uint8_t shiftBlock[16] = {0x0F, 0, 0x87, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            const auto shifted = decode(shiftBlock, s1, s2);
            ok = ok && expect(shifted.shift == 9 && shifted.samples[0] == 56 && shifted.samples[1] == -64,
                              "shift 15 clamps to 9 (7 << 3 == 56, -8 << 3 == -64)");
            s1 = 1000;
            s2 = 500;
            const uint8_t filterBlock[16] = {0x7C, 0, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            const auto unfiltered = decode(filterBlock, s1, s2);
            ok = ok && expect(unfiltered.filter == 0 && unfiltered.samples[0] == 5,
                              "filter 7 clamps to 0, so history does not affect the sample");
        }

        // The same block fetched from sound RAM: byte order, flags, wrap at the end of RAM, and IRQA on a fetch.
        constexpr uint32_t kSpu = 0x1F900000u;
        IopMemory memory;
        Spu2 &spu = memory.spu2();
        const auto setTsa = [&](uint32_t address)
        {
            memory.write16(kSpu + 0x1A8u, static_cast<uint16_t>(address >> 16));
            memory.write16(kSpu + 0x1AAu, static_cast<uint16_t>(address));
        };
        const uint16_t blockHalfwords[8] = {0x0700u, 0x87F1u, 0, 0, 0, 0, 0, 0};
        setTsa(0x500u);
        for (const uint16_t halfword : blockHalfwords)
            memory.write16(kSpu + 0x1ACu, halfword);
        {
            int16_t s1 = 0, s2 = 0;
            const auto decoded = spu.decodeAdpcmBlockAt(0x500u, s1, s2);
            ok = ok && expect(decoded.flags == 0x07 && decoded.samples[0] == 4096 && decoded.samples[1] == -4096 &&
                                  decoded.samples[2] == 28672 && decoded.samples[3] == -32768,
                              "a block fetched from sound RAM decodes with its loop flags");
        }
        setTsa(0xFFFFCu);
        for (const uint16_t halfword : blockHalfwords)
            memory.write16(kSpu + 0x1ACu, halfword);
        {
            int16_t s1 = 0, s2 = 0;
            const auto decoded = spu.decodeAdpcmBlockAt(0xFFFFCu, s1, s2);
            ok = ok && expect(decoded.flags == 0x07 && decoded.samples[0] == 4096 && decoded.samples[27] == 0,
                              "a block straddling the end of sound RAM wraps and still decodes");
        }
        memory.write16(kSpu + 0x19Cu, 0x0000u);
        memory.write16(kSpu + 0x19Eu, 0x0502u);
        memory.write16(kSpu + 0x19Au, 1u << 6);
        {
            int16_t s1 = 0, s2 = 0;
            (void)spu.decodeAdpcmBlockAt(0x500u, s1, s2);
            (void)memory.read16(kSpu + 0x344u); // polls the pending SPU2 interrupt into the IOP status
            ok = ok && expect((memory.interruptStatus() & (1u << 9)) != 0u && memory.read16(kSpu + 0x7C2u) == 0x0004u,
                              "a voice fetch at IRQA raises IOP INTC 9 and sets IRQINFO bit 2");
        }
        return ok;
    }

    // SPU2 voice block progression (Stage A, step 2): KON consumption, NAX advance, LSAX on loop-start,
    // ENDX and loop/stop on loop-end. Expected NAX/LSAX/ENDX values follow directly from the register layout
    // and the flag bits; the stopping case's NAX advance is the model's choice, documented in iop_spu2.h.
    bool testSpu2VoiceBlockProgression()
    {
        constexpr uint32_t kSpu = 0x1F900000u, kCore1 = 0x400u;
        IopMemory memory;
        Spu2 &spu = memory.spu2();
        const auto setTsa = [&](uint32_t core, uint32_t address)
        {
            memory.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
            memory.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
        };
        const auto writeBlock = [&](uint32_t core, uint32_t address, const uint16_t halfwords[8])
        {
            setTsa(core, address);
            for (int i = 0; i < 8; ++i)
                memory.write16(kSpu + core + 0x1ACu, halfwords[i]);
        };
        const auto setSsa = [&](uint32_t core, int voice, uint32_t address)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            memory.write16(base, static_cast<uint16_t>(address >> 16));
            memory.write16(base + 2u, static_cast<uint16_t>(address));
        };
        const auto readNax = [&](uint32_t core, int voice)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            return (static_cast<uint32_t>(memory.read16(base + 8u)) << 16) | memory.read16(base + 10u);
        };
        const auto readLsax = [&](uint32_t core, int voice)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            return (static_cast<uint32_t>(memory.read16(base + 4u)) << 16) | memory.read16(base + 6u);
        };
        const auto readEndx = [&](uint32_t core) { return memory.read16(kSpu + core + 0x342u); };
        const auto keyOn = [&](uint32_t core, int voice)
        {
            memory.write16(kSpu + core + 0x1A0u, static_cast<uint16_t>(1u << voice));
        };

        bool ok = true;
        // Inactive voices and out-of-range arguments produce no block and change nothing.
        ok = ok && expect(!spu.stepVoiceBlock(0, 5).has_value() && !spu.voiceActive(0, 5) &&
                              !spu.stepVoiceBlock(0, 24).has_value() && !spu.stepVoiceBlock(2, 0).has_value(),
                          "an unkeyed or out-of-range voice steps to nothing");

        // One-shot voice 0: block at 0x1000, then a loop-end block without the loop flag at 0x1008.
        const uint16_t oneShot0[8] = {0x0000u, 0x87F1u, 0, 0, 0, 0, 0, 0};
        const uint16_t oneShot1[8] = {0x0100u, 0x0007u, 0, 0, 0, 0, 0, 0};
        writeBlock(0u, 0x1000u, oneShot0);
        writeBlock(0u, 0x1008u, oneShot1);
        setSsa(0u, 0, 0x1000u);
        keyOn(0u, 0);
        ok = ok && expect(!spu.voiceActive(0, 0) && readNax(0u, 0) == 0x1000u && spu.pendingKeyOn(0) == 1u,
                          "before the first step, KON has loaded NAX from SSA but the voice is not yet active");
        const auto first = spu.stepVoiceBlock(0, 0);
        ok = ok && expect(first.has_value() && first->samples[0] == 4096 && spu.voiceActive(0, 0) &&
                              spu.pendingKeyOn(0) == 0u && readNax(0u, 0) == 0x1008u && readEndx(0u) == 0u,
                          "the first step consumes KON, decodes the SSA block and advances NAX");
        const auto second = spu.stepVoiceBlock(0, 0);
        ok = ok && expect(second.has_value() && second->samples[0] == 28672 && !spu.voiceActive(0, 0) &&
                              readEndx(0u) == 1u && readNax(0u, 0) == 0x1010u,
                          "a loop-end block without the loop flag sets ENDX and stops the voice");
        ok = ok && expect(!spu.stepVoiceBlock(0, 0).has_value() && readNax(0u, 0) == 0x1010u,
                          "a stopped voice does not step again");
        // KON restarts the same voice from SSA and clears ENDX (the KON write itself does both).
        keyOn(0u, 0);
        const auto restarted = spu.stepVoiceBlock(0, 0);
        ok = ok && expect(restarted.has_value() && restarted->samples[0] == 4096 && spu.voiceActive(0, 0) &&
                              readEndx(0u) == 0u && readNax(0u, 0) == 0x1008u,
                          "KON restarts a stopped voice from SSA");

        // Looping voice 1: loop-start block at SSA 0x2000, loop-end block at 0x2008, back to LSAX.
        const uint16_t loopStart[8] = {0x0600u, 0x87F1u, 0, 0, 0, 0, 0, 0};
        const uint16_t loopEnd[8] = {0x0300u, 0x0007u, 0, 0, 0, 0, 0, 0};
        writeBlock(0u, 0x2000u, loopStart);
        writeBlock(0u, 0x2008u, loopEnd);
        setSsa(0u, 1, 0x2000u);
        keyOn(0u, 1);
        const auto loopFirst = spu.stepVoiceBlock(0, 1);
        ok = ok && expect(loopFirst.has_value() && loopFirst->samples[0] == 4096 && readLsax(0u, 1) == 0x2000u &&
                              readNax(0u, 1) == 0x2008u && (readEndx(0u) & 2u) == 0u,
                          "a loop-start block records LSAX and advances NAX");
        const auto loopSecond = spu.stepVoiceBlock(0, 1);
        ok = ok && expect(loopSecond.has_value() && loopSecond->samples[0] == 28672 && spu.voiceActive(0, 1) &&
                              (readEndx(0u) & 2u) != 0u && readNax(0u, 1) == 0x2000u,
                          "a looping loop-end block sets ENDX and jumps NAX back to LSAX");
        const auto loopThird = spu.stepVoiceBlock(0, 1);
        ok = ok && expect(loopThird.has_value() && loopThird->samples[0] == 4096 && readNax(0u, 1) == 0x2008u,
                          "after looping, playback resumes at the loop-start block");

        // Voice 2: the loop point is not SSA (plain block, loop-start block, loop-end block).
        const uint16_t plain[8] = {0x0000u, 0x87F1u, 0, 0, 0, 0, 0, 0};
        const uint16_t midLoopStart[8] = {0x0600u, 0x0002u, 0, 0, 0, 0, 0, 0};
        const uint16_t midLoopEnd[8] = {0x0300u, 0x0003u, 0, 0, 0, 0, 0, 0};
        writeBlock(0u, 0x3000u, plain);
        writeBlock(0u, 0x3008u, midLoopStart);
        writeBlock(0u, 0x3010u, midLoopEnd);
        setSsa(0u, 2, 0x3000u);
        keyOn(0u, 2);
        (void)spu.stepVoiceBlock(0, 2);
        ok = ok && expect(readLsax(0u, 2) == 0u && readNax(0u, 2) == 0x3008u,
                          "a plain block leaves LSAX untouched");
        const auto midStart = spu.stepVoiceBlock(0, 2);
        ok = ok && expect(midStart.has_value() && midStart->samples[0] == 8192 && readLsax(0u, 2) == 0x3008u &&
                              readNax(0u, 2) == 0x3010u,
                          "a later loop-start block becomes the loop point");
        const auto midEnd = spu.stepVoiceBlock(0, 2);
        ok = ok && expect(midEnd.has_value() && midEnd->samples[0] == 12288 && readNax(0u, 2) == 0x3008u &&
                              spu.voiceActive(0, 2),
                          "the loop-end block jumps to the mid-sample loop point, not to SSA");
        const auto midAgain = spu.stepVoiceBlock(0, 2);
        ok = ok && expect(midAgain.has_value() && midAgain->samples[0] == 8192,
                          "looping resumes at the recorded loop point");

        // Core 1 follows the same path, and a voice fetch at IRQA raises the interrupt.
        const uint16_t core1Block[8] = {0x0000u, 0x87F1u, 0, 0, 0, 0, 0, 0};
        writeBlock(kCore1, 0x4000u, core1Block);
        setSsa(kCore1, 3, 0x4000u);
        memory.write16(kSpu + kCore1 + 0x19Cu, 0x0000u);
        memory.write16(kSpu + kCore1 + 0x19Eu, 0x4002u);
        memory.write16(kSpu + kCore1 + 0x19Au, 1u << 6);
        keyOn(kCore1, 3);
        const auto core1First = spu.stepVoiceBlock(1, 3);
        (void)memory.read16(kSpu + kCore1 + 0x344u); // polls the pending SPU2 interrupt into the IOP status
        ok = ok && expect(core1First.has_value() && core1First->samples[0] == 4096 && spu.voiceActive(1, 3) &&
                              (memory.interruptStatus() & (1u << 9)) != 0u &&
                              memory.read16(kSpu + 0x7C2u) == 0x0008u,
                          "a core 1 voice fetch at IRQA raises IOP INTC 9 and sets IRQINFO bit 3");
        return ok;
    }

    // SPU2 sample clock (Phase 3, pitch in Phase 7): one ADPCM block is 28 source samples,
    // not one per 48 kHz tick. 768 IOP cycles make exactly one sample, the sub-sample
    // remainder carries between calls, a non-looping voice's final block is still played,
    // and a voice fetch's IRQA hit reaches I_STAT line 9 through advanceSpu2Cycles with no
    // register read in between. These voices run at pitch 1.0 (PITCH written explicitly:
    // since Phase 7 an unwritten pitch of 0 stops the voice) with Gaussian interpolation
    // on, so the first sample is the table-weighted transient (-1), not the raw 4096.
    bool testSpu2SampleClock()
    {
        constexpr uint32_t kSpu = 0x1F900000u;
        IopMemory memory;
        Spu2 &spu = memory.spu2();
        const auto setTsa = [&](uint32_t core, uint32_t address)
        {
            memory.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
            memory.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
        };
        const auto writeBlock = [&](uint32_t core, uint32_t address, const uint16_t halfwords[8])
        {
            setTsa(core, address);
            for (int i = 0; i < 8; ++i)
                memory.write16(kSpu + core + 0x1ACu, halfwords[i]);
        };
        const auto setSsa = [&](uint32_t core, int voice, uint32_t address)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            memory.write16(base, static_cast<uint16_t>(address >> 16));
            memory.write16(base + 2u, static_cast<uint16_t>(address));
        };
        const auto readNax = [&](uint32_t core, int voice)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            return (static_cast<uint32_t>(memory.read16(base + 8u)) << 16) | memory.read16(base + 10u);
        };
        const auto readEndx = [&](uint32_t core) { return memory.read16(kSpu + core + 0x342u); };
        const auto keyOn = [&](uint32_t core, int voice)
        {
            memory.write16(kSpu + core + 0x1A0u, static_cast<uint16_t>(1u << voice));
        };
        const auto setPitch = [&](int voice, uint16_t pitch)
        {
            memory.write16(kSpu + static_cast<uint32_t>(voice) * 0x10u + 4u, pitch);
        };

        bool ok = true;
        ok = ok && expect(!spu.nextSample(0, 5).has_value() && !spu.nextSample(0, 24).has_value() &&
                              !spu.nextSample(2, 0).has_value() && spu.samplesPlayed(0, 5) == 0u &&
                              spu.samplesPlayed(2, 0) == 0u,
                          "an unkeyed or out-of-range voice yields no sample");
        spu.advanceSamples(0, 5, 100u);
        ok = ok && expect(spu.samplesPlayed(0, 5) == 0u,
                          "advancing an inactive voice plays nothing");

        // One-shot voice 0: block at 0x1000, then a loop-end block without the loop flag at 0x1008.
        const uint16_t oneShot0[8] = {0x0000u, 0x87F1u, 0, 0, 0, 0, 0, 0};
        const uint16_t oneShot1[8] = {0x0100u, 0x0007u, 0, 0, 0, 0, 0, 0};
        writeBlock(0u, 0x1000u, oneShot0);
        writeBlock(0u, 0x1008u, oneShot1);
        setSsa(0u, 0, 0x1000u);
        setPitch(0, 0x1000u);
        keyOn(0u, 0);
        ok = ok && expect(!spu.voiceActive(0, 0) && spu.samplesPlayed(0, 0) == 0u,
                          "before the first sample, KON is pending but the voice has played nothing");
        const auto first = spu.nextSample(0, 0);
        ok = ok && expect(first.has_value() && *first == -1 && spu.samplesPlayed(0, 0) == 1u &&
                              readNax(0u, 0) == 0x1008u,
                          "the first sample fetches the SSA block and advances NAX, not one block per sample; "
                          "its value is the Gaussian transient (-1 from gauss[0] * 4096), not the raw 4096 (Phase 7)");
        // The remaining count/timing assertions below are pitch-1.0 values and are unchanged by interpolation.
        spu.advanceSamples(0, 0, 27u);
        ok = ok && expect(spu.samplesPlayed(0, 0) == 28u && readNax(0u, 0) == 0x1008u && spu.voiceActive(0, 0),
                          "28 samples consume exactly one block; the second block is not fetched early");
        // Sub-sample remainder: 767 cycles play nothing, the next cycle completes one sample.
        memory.advanceSpu2Cycles(767u);
        ok = ok && expect(spu.samplesPlayed(0, 0) == 28u,
                          "767 IOP cycles are less than one 48 kHz sample");
        memory.advanceSpu2Cycles(1u);
        ok = ok && expect(spu.samplesPlayed(0, 0) == 29u,
                          "the carried remainder completes one sample on the next cycle");
        // 26 more samples: the loop-end block has been fetched, but one buffered sample remains,
        // so the voice is still playing even though block stepping already stopped it.
        memory.advanceSpu2Cycles(768u * 26u);
        ok = ok && expect(spu.samplesPlayed(0, 0) == 56u - 1u && spu.voiceActive(0, 0) &&
                              (readEndx(0u) & 1u) != 0u,
                          "the final block's last sample is still buffered after ENDX is set");
        memory.advanceSpu2Cycles(768u);
        ok = ok && expect(spu.samplesPlayed(0, 0) == 56u && !spu.voiceActive(0, 0) &&
                              !spu.nextSample(0, 0).has_value(),
                          "after the final sample the voice ends and yields nothing more");
        memory.advanceSpu2Cycles(768u * 100u);
        ok = ok && expect(spu.samplesPlayed(0, 0) == 56u,
                          "advancing a finished voice in guest time plays nothing further");

        // Looping voice 1, with a remainder carried across two advanceSpu2Cycles calls:
        // 100 cycles alone play nothing, then 84 samples (three blocks, A-B-A) play exactly.
        const uint16_t loopStart[8] = {0x0600u, 0x87F1u, 0, 0, 0, 0, 0, 0};
        const uint16_t loopEnd[8] = {0x0300u, 0x0007u, 0, 0, 0, 0, 0, 0};
        writeBlock(0u, 0x2000u, loopStart);
        writeBlock(0u, 0x2008u, loopEnd);
        setSsa(0u, 1, 0x2000u);
        setPitch(1, 0x1000u);
        keyOn(0u, 1);
        memory.advanceSpu2Cycles(100u);
        ok = ok && expect(spu.samplesPlayed(0, 1) == 0u,
                          "a partial sample's cycles carry into the next advance, not lost");
        memory.advanceSpu2Cycles(768u * 84u);
        ok = ok && expect(spu.samplesPlayed(0, 1) == 84u && readNax(0u, 1) == 0x2008u && spu.voiceActive(0, 1),
                          "84 samples loop A-B-A and return NAX to the block after the loop start");

        // IRQA in guest time: voice 2's block sits at IRQA, so its first sample's fetch must
        // latch I_STAT line 9 through advanceSpu2Cycles alone, with no register read polling it.
        const uint16_t irqBlock[8] = {0x0000u, 0x87F1u, 0, 0, 0, 0, 0, 0};
        writeBlock(0u, 0x3000u, irqBlock);
        setSsa(0u, 2, 0x3000u);
        memory.write16(kSpu + 0x19Cu, 0x0000u);
        memory.write16(kSpu + 0x19Eu, 0x3000u);
        memory.write16(kSpu + 0x19Au, 1u << 6);
        memory.setInterruptStatus(0u);
        setPitch(2, 0x1000u);
        keyOn(0u, 2);
        ok = ok && expect((memory.interruptStatus() & (1u << 9)) == 0u,
                          "no SPU2 interrupt is pending before the IRQA fetch");
        memory.advanceSpu2Cycles(768u);
        ok = ok && expect(spu.samplesPlayed(0, 2) == 1u && (memory.interruptStatus() & (1u << 9)) != 0u,
                          "a guest-time voice fetch at IRQA raises IOP INTC 9 without a register poll");
        return ok;
    }

    // SPU2 ADSR envelope (Phase 6). All expected levels are hand-computed from the pinned
    // bitfields and rate table in iop_spu2.h (attack step +14336/sample at shift 0 step 0,
    // sustain level (N+1)*0x800, exponential decay step -8*level/0x8000 at shift 11, and the
    // 16-sample rate at shift 15 step 0), so a wrong rate table or a stuck phase fails these
    // assertions by design; a live mutant check is reported in the ledger. These voices run
    // at pitch 1.0 (Phase 7), and the curves are unit curves against that table,
    // not hardware captures (the hardware ENVX probe remains open).
    bool testSpu2Adsr()
    {
        constexpr uint32_t kSpu = 0x1F900000u;
        IopMemory memory;
        Spu2 &spu = memory.spu2();
        const auto setTsa = [&](uint32_t core, uint32_t address)
        {
            memory.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
            memory.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
        };
        const auto writeBlock = [&](uint32_t core, uint32_t address, const uint16_t halfwords[8])
        {
            setTsa(core, address);
            for (int i = 0; i < 8; ++i)
                memory.write16(kSpu + core + 0x1ACu, halfwords[i]);
        };
        const auto setSsa = [&](uint32_t core, int voice, uint32_t address)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            memory.write16(base, static_cast<uint16_t>(address >> 16));
            memory.write16(base + 2u, static_cast<uint16_t>(address));
        };
        const auto setAdsr = [&](int voice, uint16_t adsr1, uint16_t adsr2)
        {
            const uint32_t base = kSpu + static_cast<uint32_t>(voice) * 0x10u;
            memory.write16(base + 6u, adsr1);
            memory.write16(base + 8u, adsr2);
        };
        const auto setPitch = [&](int voice, uint16_t pitch)
        {
            memory.write16(kSpu + static_cast<uint32_t>(voice) * 0x10u + 4u, pitch);
        };
        // Phase 7: an unwritten pitch of 0 stops a voice, so every voice here runs at
        // pitch 1.0 explicitly; the envelope assertions below are pitch-independent
        // (ADSR steps per output sample, and at 1.0 one output sample is one tick).
        for (int v = 0; v < 8; ++v)
            setPitch(v, 0x1000u);
        const auto readEnvx = [&](int voice)
        {
            return memory.read16(kSpu + static_cast<uint32_t>(voice) * 0x10u + 0x0Au);
        };
        const auto keyOn = [&](int voice) { memory.write16(kSpu + 0x1A0u, static_cast<uint16_t>(1u << voice)); };
        const auto keyOff = [&](int voice) { memory.write16(kSpu + 0x1A4u, static_cast<uint16_t>(1u << voice)); };
        // One looping block (loop-start + loop-end + loop, silence) so the voice plays forever
        // unless the envelope stops it; a zero block (flags 0) plays silence forever without
        // ever setting ENDX, which is what the release test needs.
        const uint16_t loopBlock[8] = {0x0700u, 0, 0, 0, 0, 0, 0, 0};
        const uint16_t zeroBlock[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        bool ok = true;

        // Case A — KON to attack, attack to decay to sustain, then sustain increase to max.
        // ADSR1: attack linear shift 0 step 0, decay shift 0, sustain 7 -> 0x4000. ADSR2: all zero
        // (sustain linear increase shift 0 step 0, release linear shift 0).
        writeBlock(0u, 0x1000u, loopBlock);
        setSsa(0u, 0, 0x1000u);
        setAdsr(0, 0x0007u, 0x0000u);
        keyOn(0);
        ok = ok && expect(spu.envelopePhase(0, 0) == Spu2::AdsrPhase::Idle && readEnvx(0) == 0u,
                          "before the first sample a pending KON has not started the envelope");
        spu.advanceSamples(0, 0, 1u);
        ok = ok && expect(readEnvx(0) == 14336u && spu.envelopeLevel(0, 0) == 14336u &&
                              spu.envelopePhase(0, 0) == Spu2::AdsrPhase::Attack,
                          "attack shift 0 step 0 rises 14336 on the first sample");
        spu.advanceSamples(0, 0, 1u);
        ok = ok && expect(readEnvx(0) == 28672u && spu.envelopePhase(0, 0) == Spu2::AdsrPhase::Attack,
                          "attack rises 14336 again on the second sample");
        spu.advanceSamples(0, 0, 1u);
        ok = ok && expect(readEnvx(0) == 0x7FFFu && spu.envelopePhase(0, 0) == Spu2::AdsrPhase::Decay,
                          "attack clamps at 0x7FFF and moves to decay");
        spu.advanceSamples(0, 0, 1u);
        ok = ok && expect(readEnvx(0) == 0x4000u && spu.envelopePhase(0, 0) == Spu2::AdsrPhase::Sustain,
                          "fast exponential decay reaches the 0x4000 sustain level in one sample");
        spu.advanceSamples(0, 0, 2u);
        ok = ok && expect(readEnvx(0) == 0x7FFFu && spu.envelopePhase(0, 0) == Spu2::AdsrPhase::Sustain,
                          "sustain increase climbs back to 0x7FFF and holds there");

        // Case B — KOFF to release, release to zero stops the voice without setting ENDX.
        // Zero block at 0x2000, ADSR1: attack shift 0 step 0, sustain 15 -> 0x7FFF (attack goes
        // straight to sustain), ADSR2: release linear shift 4 -> -1024 per sample.
        writeBlock(0u, 0x2000u, zeroBlock);
        setSsa(0u, 1, 0x2000u);
        setAdsr(1, 0x000Fu, 0x0004u);
        keyOn(1);
        spu.advanceSamples(0, 1, 3u);
        ok = ok && expect(readEnvx(1) == 0x7FFFu && spu.envelopePhase(0, 1) == Spu2::AdsrPhase::Sustain,
                          "with sustain level 15, attack goes straight to sustain at 0x7FFF");
        keyOff(1);
        ok = ok && expect((spu.pendingKeyOff(0) & (1u << 1)) != 0u, "KOFF latches before the next sample");
        memory.advanceSpu2Cycles(768u * 31u);
        ok = ok && expect(readEnvx(1) == 1023u && spu.envelopePhase(0, 1) == Spu2::AdsrPhase::Release &&
                              spu.voiceActive(0, 1) && (spu.pendingKeyOff(0) & (1u << 1)) == 0u,
                          "guest-time release falls 1024 per sample and consumes the KOFF bit");
        spu.advanceSamples(0, 1, 1u);
        ok = ok && expect(readEnvx(1) == 0u && spu.envelopePhase(0, 1) == Spu2::AdsrPhase::Idle &&
                              !spu.voiceActive(0, 1) && !spu.nextSample(0, 1).has_value() &&
                              (memory.read16(kSpu + 0x342u) & (1u << 1)) == 0u,
                          "release to zero stops the voice and does not set ENDX");

        // Case C — rate table: slow attack steps every 16 samples, all-ones rate never steps.
        // Voice 2: attack linear shift 15 step 0 (+7 every 16 samples). Voice 3: attack shift 31
        // step 3 (all-ones rate), then release shift 31 after KOFF (also all-ones).
        writeBlock(0u, 0x3000u, loopBlock);
        setSsa(0u, 2, 0x3000u);
        setAdsr(2, 0x3C0Fu, 0x0000u);
        keyOn(2);
        spu.advanceSamples(0, 2, 15u);
        ok = ok && expect(readEnvx(2) == 0u && spu.envelopePhase(0, 2) == Spu2::AdsrPhase::Attack,
                          "attack shift 15 has not stepped after 15 samples");
        spu.advanceSamples(0, 2, 1u);
        ok = ok && expect(readEnvx(2) == 7u, "attack shift 15 steps +7 on the 16th sample");
        spu.advanceSamples(0, 2, 16u);
        ok = ok && expect(readEnvx(2) == 14u, "attack shift 15 steps +7 again on the 32nd sample");
        writeBlock(0u, 0x4000u, loopBlock);
        setSsa(0u, 3, 0x4000u);
        setAdsr(3, 0x7F00u, 0x001Fu);
        keyOn(3);
        spu.advanceSamples(0, 3, 100u);
        ok = ok && expect(readEnvx(3) == 0u && spu.envelopePhase(0, 3) == Spu2::AdsrPhase::Attack &&
                              spu.voiceActive(0, 3),
                          "the all-ones attack rate never steps, even after 100 samples");
        keyOff(3);
        spu.advanceSamples(0, 3, 100u);
        ok = ok && expect(readEnvx(3) == 0u && spu.envelopePhase(0, 3) == Spu2::AdsrPhase::Release &&
                              spu.voiceActive(0, 3),
                          "the all-ones release rate never steps either, so the voice keeps playing");

        // Case D — exponential decay shape (linear decay would give different values), then
        // sustain hold with a never-stepping sustain rate. Voice 4: attack shift 0 step 0,
        // decay shift 11, sustain 7 -> 0x4000. Voice 5: same attack/decay, sustain shift 31
        // step 3 (never steps) so the sustain level holds.
        writeBlock(0u, 0x5000u, loopBlock);
        setSsa(0u, 4, 0x5000u);
        setAdsr(4, 0x00B7u, 0x0000u);
        keyOn(4);
        spu.advanceSamples(0, 4, 3u);
        ok = ok && expect(readEnvx(4) == 0x7FFFu && spu.envelopePhase(0, 4) == Spu2::AdsrPhase::Decay,
                          "voice 4 reaches decay at 0x7FFF after its fast attack");
        spu.advanceSamples(0, 4, 1u);
        ok = ok && expect(readEnvx(4) == 32759u, "exponential decay shift 11 falls 8 on the first decay sample (SAR-15 floor, Phase 6b)");
        spu.advanceSamples(0, 4, 1u);
        ok = ok && expect(readEnvx(4) == 32751u && spu.envelopePhase(0, 4) == Spu2::AdsrPhase::Decay,
                          "exponential decay shift 11 falls 8 again on the second decay sample");
        writeBlock(0u, 0x6000u, loopBlock);
        setSsa(0u, 5, 0x6000u);
        setAdsr(5, 0x0007u, 0x1FC0u);
        keyOn(5);
        spu.advanceSamples(0, 5, 4u);
        ok = ok && expect(readEnvx(5) == 0x4000u && spu.envelopePhase(0, 5) == Spu2::AdsrPhase::Sustain,
                          "voice 5 reaches its sustain level after attack and fast decay");
        spu.advanceSamples(0, 5, 100u);
        ok = ok && expect(readEnvx(5) == 0x4000u && spu.envelopePhase(0, 5) == Spu2::AdsrPhase::Sustain,
                          "a never-stepping sustain rate holds the sustain level indefinitely");

        // Case E — ENVX guest writes agree with the internal level, and KON wins over KOFF.
        writeBlock(0u, 0x7000u, loopBlock);
        setSsa(0u, 6, 0x7000u);
        setAdsr(6, 0x0007u, 0x0000u);
        keyOn(6);
        spu.advanceSamples(0, 6, 1u);
        ok = ok && expect(readEnvx(6) == 14336u, "voice 6 attacks to 14336 on its first sample");
        memory.write16(kSpu + 6u * 0x10u + 0x0Au, 0x1000u);
        ok = ok && expect(readEnvx(6) == 0x1000u && spu.envelopeLevel(0, 6) == 0x1000u,
                          "a guest ENVX write is adopted by the internal envelope level");
        spu.advanceSamples(0, 6, 1u);
        ok = ok && expect(readEnvx(6) == 18432u, "attack continues from the guest-written ENVX level");
        memory.write16(kSpu + 6u * 0x10u + 0x0Au, 0xFFFFu);
        ok = ok && expect(readEnvx(6) == 0xFFFFu && spu.envelopeLevel(0, 6) == 0xFFFFu,
                          "a guest ENVX write of FFFFh is sign-extended to -1, not clamped to 0x7FFF (Phase 6b)");
        spu.advanceSamples(0, 6, 1u);
        ok = ok && expect(readEnvx(6) == 14335u,
                          "attack counts up from the guest-written negative level (-1 + 14336)");
        keyOff(6);
        keyOn(6);
        spu.advanceSamples(0, 6, 1u);
        ok = ok && expect(readEnvx(6) == 14336u && spu.envelopePhase(0, 6) == Spu2::AdsrPhase::Attack &&
                              (spu.pendingKeyOff(0) & (1u << 6)) == 0u,
                          "KON restarts the envelope in attack and clears a pending KOFF");

        // Case F — Phase 6b: ADSR parameters are re-read per step, and the rate counter
        // keeps its residue (subtract 0x8000) rather than resetting to zero as in PCSX2.
        // Voice 7: attack shift 13 step 0 (+7 every 4 samples). After 3 samples the counter
        // sits at 0x6000 with no step yet; rewriting ADSR1 to shift 11 steps at once (counter
        // 0xE000, residue 0x6000 kept), and rewriting back to shift 13 steps again on the
        // next sample (0x6000 + 0x2000). A reset-to-zero counter would stay at 7 here.
        writeBlock(0u, 0x8000u, loopBlock);
        setSsa(0u, 7, 0x8000u);
        setAdsr(7, 0x3407u, 0x0000u);
        keyOn(7);
        spu.advanceSamples(0, 7, 3u);
        ok = ok && expect(readEnvx(7) == 0u, "attack shift 13 has not stepped after 3 samples (counter 0x6000)");
        setAdsr(7, 0x2C07u, 0x0000u);
        spu.advanceSamples(0, 7, 1u);
        ok = ok && expect(readEnvx(7) == 7u, "rewriting ADSR1 to shift 11 steps at once from the kept counter");
        setAdsr(7, 0x3407u, 0x0000u);
        spu.advanceSamples(0, 7, 1u);
        ok = ok && expect(readEnvx(7) == 14u,
                          "rewriting ADSR1 back to shift 13 steps again from the kept residue (subtract, not reset)");
        return ok;
    }

    // SPU2 pitch stepping and Gaussian interpolation (Phase 7). All expected outputs are
    // computed from the pinned psx-spx table and the pitch-counter rules in iop_spu2.h
    // (step = PITCH clipped to 0x4000; bits 4..11 of the fractional counter are the
    // Gaussian index; each product is shifted right by 15 before summing), not from the
    // implementation under test: a wrong ratio, a linear blend, or a wrong table entry
    // changes these values by design. The source blocks use filter 0 / shift 0, so the
    // decoded samples are exactly nibble << 12: block A (looping) is
    // [0, 4096, 8192, 12288, 16384, 20480, 24576, 28672, 0 x 20], block A' is the same
    // samples without loop flags, and block B (loop-end, no loop) is [28672, -32768, 0 x 26].
    bool testSpu2Pitch()
    {
        constexpr uint32_t kSpu = 0x1F900000u;
        IopMemory memory;
        Spu2 &spu = memory.spu2();
        const auto setTsa = [&](uint32_t core, uint32_t address)
        {
            memory.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
            memory.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
        };
        const auto writeBlock = [&](uint32_t core, uint32_t address, const uint16_t halfwords[8])
        {
            setTsa(core, address);
            for (int i = 0; i < 8; ++i)
                memory.write16(kSpu + core + 0x1ACu, halfwords[i]);
        };
        const auto setSsa = [&](uint32_t core, int voice, uint32_t address)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            memory.write16(base, static_cast<uint16_t>(address >> 16));
            memory.write16(base + 2u, static_cast<uint16_t>(address));
        };
        const auto readNax = [&](uint32_t core, int voice)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            return (static_cast<uint32_t>(memory.read16(base + 8u)) << 16) | memory.read16(base + 10u);
        };
        const auto readEndx = [&](uint32_t core) { return memory.read16(kSpu + core + 0x342u); };
        const auto keyOn = [&](int voice) { memory.write16(kSpu + 0x1A0u, static_cast<uint16_t>(1u << voice)); };
        const auto setPitch = [&](int voice, uint16_t pitch)
        {
            memory.write16(kSpu + static_cast<uint32_t>(voice) * 0x10u + 4u, pitch);
        };
        const auto next = [&](int voice) { return spu.nextSample(0, voice); };

        bool ok = true;
        // Table guard: spot values from psx-spx, and every quadruple's sum in the
        // documented 0x7F7F..0x7F81 range (psx-spx's "slightly bugged" sum, not 0x8000).
        ok = ok && expect(Spu2::kGaussTable.size() == 512u && Spu2::kGaussTable[0] == -1 &&
                              Spu2::kGaussTable[0xFF] == 0x12C7 && Spu2::kGaussTable[0x100] == 0x1307 &&
                              Spu2::kGaussTable[0x1FF] == 0x59B3,
                          "gauss table spot values match psx-spx");
        bool sumsOk = true;
        for (int i = 0; i < 256; ++i)
        {
            const int32_t sum = Spu2::kGaussTable[static_cast<size_t>(i)] +
                                Spu2::kGaussTable[static_cast<size_t>(0xFF - i)] +
                                Spu2::kGaussTable[static_cast<size_t>(0x100 + i)] +
                                Spu2::kGaussTable[static_cast<size_t>(0x1FF - i)];
            if (sum < 0x7F7F || sum > 0x7F81)
                sumsOk = false;
        }
        ok = ok && expect(sumsOk, "every gauss quadruple sums to 0x7F7F..0x7F81");

        const uint16_t loopA[8] = {0x0700u, 0x3210u, 0x7654u, 0, 0, 0, 0, 0};
        const uint16_t oneShotA[8] = {0x0000u, 0x3210u, 0x7654u, 0, 0, 0, 0, 0};
        const uint16_t oneShotB[8] = {0x0100u, 0x0087u, 0, 0, 0, 0, 0, 0};

        // Pitch 1.0, looping voice 0: one output per source sample, Gaussian values
        // (not the raw samples: at 1.0 the first outputs are the zero-history transient).
        writeBlock(0u, 0x1000u, loopA);
        setSsa(0u, 0, 0x1000u);
        setPitch(0, 0x1000u);
        keyOn(0);
        const int16_t at1x[6] = {0, -1, 607, 4086, 8165, 12246};
        for (int i = 0; i < 6; ++i)
        {
            const auto sample = next(0);
            ok = ok && expect(sample.has_value() && *sample == at1x[i],
                              "pitch 1.0 outputs the Gaussian sequence, not the raw samples");
        }
        ok = ok && expect(spu.samplesPlayed(0, 0) == 6u, "pitch 1.0 plays one output sample per tick");

        // Pitch 0.5, looping voice 1: source samples advance every other tick, and the
        // in-between tick interpolates at index 0x80 (53 = gauss[0x80] * 4096 >> 15).
        writeBlock(0u, 0x1000u, loopA);
        setSsa(0u, 1, 0x1000u);
        setPitch(1, 0x0800u);
        keyOn(1);
        const int16_t atHalf[6] = {0, 0, 0, -1, 53, 607};
        for (int i = 0; i < 6; ++i)
        {
            const auto sample = next(1);
            ok = ok && expect(sample.has_value() && *sample == atHalf[i],
                              "pitch 0.5 advances the source every other tick with index-0x80 interpolation");
        }
        spu.advanceSamples(0, 1, 200u);
        ok = ok && expect(spu.voiceActive(0, 1) && spu.samplesPlayed(0, 1) == 206u,
                          "a looping voice at pitch 0.5 keeps playing past 200 ticks");

        // Pitch 2.0, one-shot voice 2: two source samples per tick, 56 source samples
        // in 28 outputs, then nothing (pitch-2.0 outputs are every other pitch-1.0 output).
        writeBlock(0u, 0x2000u, oneShotA);
        writeBlock(0u, 0x2008u, oneShotB);
        setSsa(0u, 2, 0x2000u);
        setPitch(2, 0x2000u);
        keyOn(2);
        const auto fast1 = next(2);
        const auto fast2 = next(2);
        const auto fast3 = next(2);
        ok = ok && expect(fast1.has_value() && *fast1 == -1 && fast2.has_value() && *fast2 == 4086 &&
                              fast3.has_value() && *fast3 == 12246,
                          "pitch 2.0 consumes two source samples per tick");
        spu.advanceSamples(0, 2, 100u);
        ok = ok && expect(spu.samplesPlayed(0, 2) == 28u && !spu.voiceActive(0, 2) &&
                              !next(2).has_value(),
                          "pitch 2.0 finishes a 56-sample one-shot in 28 outputs");

        // Fractional pitch 1.5, one-shot voice 3: the source crosses the block boundary
        // on tick 20 (NAX advances past block B), and 56 source samples finish in 38
        // outputs, not 37 or 39.
        writeBlock(0u, 0x2000u, oneShotA);
        writeBlock(0u, 0x2008u, oneShotB);
        setSsa(0u, 3, 0x2000u);
        setPitch(3, 0x1800u);
        keyOn(3);
        const int16_t atFrac[4] = {0, 607, 6127, 12246};
        for (int i = 0; i < 4; ++i)
        {
            const auto sample = next(3);
            ok = ok && expect(sample.has_value() && *sample == atFrac[i],
                              "pitch 1.5 outputs the fractional Gaussian sequence");
        }
        spu.advanceSamples(0, 3, 16u);
        ok = ok && expect(spu.samplesPlayed(0, 3) == 20u && readNax(0u, 3) == 0x2010u,
                          "pitch 1.5 fetches the second block on tick 20, crossing the block boundary");
        spu.advanceSamples(0, 3, 100u);
        ok = ok && expect(spu.samplesPlayed(0, 3) == 38u && !next(3).has_value(),
                          "pitch 1.5 finishes a 56-sample one-shot in 38 outputs");

        // Pitch 0, voice 4: stopped, KON stays pending, and writing a pitch resumes it.
        writeBlock(0u, 0x3000u, loopA);
        setSsa(0u, 4, 0x3000u);
        setPitch(4, 0x0000u);
        keyOn(4);
        ok = ok && expect(!next(4).has_value() && spu.samplesPlayed(0, 4) == 0u &&
                              spu.pendingKeyOn(0) == (1u << 4),
                          "pitch 0 stops the voice without consuming KON");
        setPitch(4, 0x1000u);
        const auto resumed = next(4);
        ok = ok && expect(resumed.has_value() && *resumed == 0 && spu.samplesPlayed(0, 4) == 1u,
                          "writing pitch 0x1000 resumes the stopped voice from its first sample");

        // Looping at pitch 2.0, voice 5: ENDX is set by the loop-end fetch and the voice
        // never ends.
        writeBlock(0u, 0x4000u, loopA);
        setSsa(0u, 5, 0x4000u);
        setPitch(5, 0x2000u);
        keyOn(5);
        spu.advanceSamples(0, 5, 100u);
        ok = ok && expect(spu.voiceActive(0, 5) && (readEndx(0u) & (1u << 5)) != 0u &&
                              spu.samplesPlayed(0, 5) == 100u,
                          "a looping voice at pitch 2.0 stays active with ENDX set");

        // Pitch above the clip, voice 6: 0x5000 clips to 0x4000 (4.0x), so the one-shot
        // finishes in 14 outputs, not 12 (unclipped 5x) or 28 (2x).
        writeBlock(0u, 0x5000u, oneShotA);
        writeBlock(0u, 0x5008u, oneShotB);
        setSsa(0u, 6, 0x5000u);
        setPitch(6, 0x5000u);
        keyOn(6);
        spu.advanceSamples(0, 6, 100u);
        ok = ok && expect(spu.samplesPlayed(0, 6) == 14u && !next(6).has_value(),
                          "pitch above 0x3FFF clips to 0x4000, finishing in 14 outputs");

        // Pitch re-read per tick, voice 7: two ticks at 1.0, then 2.0 continues exactly
        // where the pitch-2.0 stream would be (history carries across the change).
        writeBlock(0u, 0x1000u, loopA);
        setSsa(0u, 7, 0x1000u);
        setPitch(7, 0x1000u);
        keyOn(7);
        (void)next(7);
        (void)next(7);
        setPitch(7, 0x2000u);
        const auto changed = next(7);
        ok = ok && expect(changed.has_value() && *changed == 4086,
                          "a mid-playback pitch change takes effect on the next tick");
        return ok;
    }

    // SPU2 mixing and core routing (Phase 9). Expected values are hand-computed
    // from the pinned formula in iop_spu2.h (eff = (ENVX * VOLXL) >> 15,
    // contrib = (sample * eff) >> 15, arithmetic shifts), not copied from the
    // implementation: a wrong volume order, a missing clamp, or a wrong PMON
    // source fails these assertions by design. The loopA pitch-1.0 sample
    // sequence (0, -1, 607, 4086, ...) is the Phase 7 sequence; the ADSR here
    // is attack shift 0 step 0 to sustain level 15, so ENVX is 14336, 28672,
    // then 0x7FFF from tick 3 on (sustain shift 31 step 3 never steps).
    bool testSpu2Mixing()
    {
        constexpr uint32_t kSpu = 0x1F900000u;
        constexpr uint32_t kCore1 = 0x400u;
        IopMemory memory;
        Spu2 &spu = memory.spu2();
        const auto setTsa = [&](uint32_t core, uint32_t address)
        {
            memory.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
            memory.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
        };
        const auto writeBlock = [&](uint32_t core, uint32_t address, const uint16_t halfwords[8])
        {
            setTsa(core, address);
            for (int i = 0; i < 8; ++i)
                memory.write16(kSpu + core + 0x1ACu, halfwords[i]);
        };
        const auto setSsa = [&](uint32_t core, int voice, uint32_t address)
        {
            const uint32_t base = kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu;
            memory.write16(base, static_cast<uint16_t>(address >> 16));
            memory.write16(base + 2u, static_cast<uint16_t>(address));
        };
        const auto setVoice = [&](uint32_t core, int voice, uint32_t address, uint16_t volL, uint16_t volR)
        {
            const uint32_t base = kSpu + core + static_cast<uint32_t>(voice) * 0x10u;
            memory.write16(base + 0u, volL);
            memory.write16(base + 2u, volR);
            memory.write16(base + 4u, 0x1000u); // pitch 1.0
            memory.write16(base + 6u, 0x000Fu); // ADSR1: attack shift 0 step 0, sustain 15
            memory.write16(base + 8u, 0x1FC0u); // ADSR2: sustain shift 31 step 3 (never steps)
            setSsa(core, voice, address);
        };
        const auto setVmix = [&](uint32_t core, uint32_t dryL, uint32_t dryR, uint32_t wetL, uint32_t wetR)
        {
            memory.write16(kSpu + core + 0x188u, static_cast<uint16_t>(dryL));
            memory.write16(kSpu + core + 0x18Au, static_cast<uint16_t>(dryL >> 16));
            memory.write16(kSpu + core + 0x190u, static_cast<uint16_t>(dryR));
            memory.write16(kSpu + core + 0x192u, static_cast<uint16_t>(dryR >> 16));
            memory.write16(kSpu + core + 0x18Cu, static_cast<uint16_t>(wetL));
            memory.write16(kSpu + core + 0x18Eu, static_cast<uint16_t>(wetL >> 16));
            memory.write16(kSpu + core + 0x194u, static_cast<uint16_t>(wetR));
            memory.write16(kSpu + core + 0x196u, static_cast<uint16_t>(wetR >> 16));
        };
        const auto keyOn = [&](uint32_t core, int voice)
        {
            memory.write16(kSpu + core + 0x1A0u, static_cast<uint16_t>(1u << voice));
        };
        const auto volx = [&](uint32_t core, int voice, uint32_t reg)
        {
            return memory.read16(kSpu + core + static_cast<uint32_t>(voice) * 0x10u + reg);
        };

        bool ok = true;
        const uint16_t loopA[8] = {0x0700u, 0x3210u, 0x7654u, 0, 0, 0, 0, 0};

        // Single voice, dry L+R, full volume: tick values are the pinned
        // formula applied to the Phase 7 sample sequence and the ADSR levels
        // above (tick 2: (-1 * 28671) >> 15 = -1; tick 3: (607 * 32766) >> 15
        // = 606; tick 4: (4086 * 32766) >> 15 = 4085).
        writeBlock(0u, 0x1000u, loopA);
        setVoice(0u, 0, 0x1000u, 0x7FFFu, 0x7FFFu);
        setVmix(0u, 1u, 1u, 0u, 0u);
        keyOn(0u, 0);
        const auto m1 = spu.mixTick();
        ok = ok && expect(m1.left == 0 && m1.right == 0 && m1.coreDryL[0] == 0 &&
                              volx(0u, 0, 0x0Cu) == 0x7FFFu && volx(0u, 0, 0x0Eu) == 0x7FFFu,
                          "tick 1 mixes sample 0 to 0 and refreshes VOLXL/VOLXR from VOLL/VOLR");
        const auto m2 = spu.mixTick();
        ok = ok && expect(m2.left == -1 && m2.right == -1 && m2.coreDryL[0] == -1 &&
                              m2.coreWetL[0] == 0 && m2.coreWetR[0] == 0,
                          "tick 2 mixes (-1 * 28671) >> 15 = -1 to dry only, wet stays 0");
        const auto m3 = spu.mixTick();
        ok = ok && expect(m3.left == 606 && m3.right == 606 && m3.coreDryL[0] == 606,
                          "tick 3 mixes (607 * 32766) >> 15 = 606");
        const auto m4 = spu.mixTick();
        ok = ok && expect(m4.left == 4085 && m4.right == 4085,
                          "tick 4 mixes (4086 * 32766) >> 15 = 4085");

        // Two identical voices sum exactly: the two-voice final is twice the
        // single-voice final on the same tick (no rounding is introduced by
        // the bus sum itself).
        {
            IopMemory memory2;
            Spu2 &spu2 = memory2.spu2();
            const auto setTsa2 = [&](uint32_t address)
            {
                memory2.write16(kSpu + 0x1A8u, static_cast<uint16_t>(address >> 16));
                memory2.write16(kSpu + 0x1AAu, static_cast<uint16_t>(address));
            };
            const auto writeBlock2 = [&](uint32_t address)
            {
                setTsa2(address);
                for (int i = 0; i < 8; ++i)
                    memory2.write16(kSpu + 0x1ACu, loopA[i]);
            };
            writeBlock2(0x1000u);
            for (int v = 0; v < 2; ++v)
            {
                const uint32_t base = kSpu + static_cast<uint32_t>(v) * 0x10u;
                memory2.write16(base + 0u, 0x7FFFu);
                memory2.write16(base + 2u, 0x7FFFu);
                memory2.write16(base + 4u, 0x1000u);
                memory2.write16(base + 6u, 0x000Fu);
                memory2.write16(base + 8u, 0x1FC0u);
                memory2.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu,
                                static_cast<uint16_t>(0x1000u >> 16));
                memory2.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu + 2u, 0x1000u);
                memory2.write16(kSpu + 0x1A0u, static_cast<uint16_t>(1u << v));
            }
            memory2.write16(kSpu + 0x188u, 0x0003u);
            memory2.write16(kSpu + 0x190u, 0x0003u);
            (void)spu2.mixTick();
            (void)spu2.mixTick();
            (void)spu2.mixTick();
            const auto two = spu2.mixTick();
            ok = ok && expect(two.left == 2 * 4085 && two.right == 2 * 4085,
                              "two identical voices sum to exactly twice the single-voice tick-4 value");
        }

        // Clamping: two constant-max voices sum to 57340 unclamped, so the
        // final clamps to 32767; two constant-min voices clamp to -32768.
        // The constant block is nibbles of 7 (samples 28672) / 8 (-32768),
        // looping, so the Gaussian output settles at the constant value.
        {
            const uint16_t constMax[8] = {0x0700u, 0x7777u, 0x7777u, 0x7777u, 0x7777u, 0x7777u, 0x7777u, 0x7777u};
            const uint16_t constMin[8] = {0x0700u, 0x8888u, 0x8888u, 0x8888u, 0x8888u, 0x8888u, 0x8888u, 0x8888u};
            IopMemory memory3;
            Spu2 &spu3 = memory3.spu2();
            const auto writeBlock3 = [&](uint32_t address, const uint16_t block[8])
            {
                memory3.write16(kSpu + 0x1A8u, static_cast<uint16_t>(address >> 16));
                memory3.write16(kSpu + 0x1AAu, static_cast<uint16_t>(address));
                for (int i = 0; i < 8; ++i)
                    memory3.write16(kSpu + 0x1ACu, block[i]);
            };
            writeBlock3(0x1000u, constMax);
            for (int v = 0; v < 2; ++v)
            {
                const uint32_t base = kSpu + static_cast<uint32_t>(v) * 0x10u;
                memory3.write16(base + 0u, 0x7FFFu);
                memory3.write16(base + 2u, 0x7FFFu);
                memory3.write16(base + 4u, 0x1000u);
                memory3.write16(base + 6u, 0x000Fu);
                memory3.write16(base + 8u, 0x1FC0u);
                memory3.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu, 0u);
                memory3.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu + 2u, 0x1000u);
                memory3.write16(kSpu + 0x1A0u, static_cast<uint16_t>(1u << v));
            }
            memory3.write16(kSpu + 0x188u, 0x0003u);
            memory3.write16(kSpu + 0x190u, 0x0003u);
            Spu2::MixTick last{};
            for (int i = 0; i < 12; ++i)
                last = spu3.mixTick();
            ok = ok && expect(last.left == 32767 && last.coreDryL[0] > 32767,
                              "two full-volume constant voices clamp the final to 32767 while the dry bus stays unclamped");
            // Negative side, fresh instance.
            IopMemory memory4;
            Spu2 &spu4 = memory4.spu2();
            const auto writeBlock4 = [&](uint32_t address, const uint16_t block[8])
            {
                memory4.write16(kSpu + 0x1A8u, static_cast<uint16_t>(address >> 16));
                memory4.write16(kSpu + 0x1AAu, static_cast<uint16_t>(address));
                for (int i = 0; i < 8; ++i)
                    memory4.write16(kSpu + 0x1ACu, block[i]);
            };
            writeBlock4(0x1000u, constMin);
            for (int v = 0; v < 2; ++v)
            {
                const uint32_t base = kSpu + static_cast<uint32_t>(v) * 0x10u;
                memory4.write16(base + 0u, 0x7FFFu);
                memory4.write16(base + 2u, 0x7FFFu);
                memory4.write16(base + 4u, 0x1000u);
                memory4.write16(base + 6u, 0x000Fu);
                memory4.write16(base + 8u, 0x1FC0u);
                memory4.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu, 0u);
                memory4.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu + 2u, 0x1000u);
                memory4.write16(kSpu + 0x1A0u, static_cast<uint16_t>(1u << v));
            }
            memory4.write16(kSpu + 0x188u, 0x0003u);
            memory4.write16(kSpu + 0x190u, 0x0003u);
            Spu2::MixTick lastNeg{};
            for (int i = 0; i < 12; ++i)
                lastNeg = spu4.mixTick();
            ok = ok && expect(lastNeg.left == -32768 && lastNeg.coreDryL[0] < -32768,
                              "two full-volume constant-min voices clamp the final to -32768");
        }

        // L/R balance and routing: a voice with VOLR 0 contributes nothing
        // right; a voice with no VMIX bits contributes nothing anywhere but
        // still plays; a voice routed wet-only lands in the wet bus, not dry.
        {
            IopMemory memory5;
            Spu2 &spu5 = memory5.spu2();
            const auto writeBlock5 = [&](uint32_t core, uint32_t address)
            {
                memory5.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
                memory5.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
                for (int i = 0; i < 8; ++i)
                    memory5.write16(kSpu + core + 0x1ACu, loopA[i]);
            };
            writeBlock5(0u, 0x1000u);
            // Voice 0: left only by volume. Voice 1: no routing. Voice 2: wet only.
            for (int v = 0; v < 3; ++v)
            {
                const uint32_t base = kSpu + static_cast<uint32_t>(v) * 0x10u;
                memory5.write16(base + 0u, v == 0 ? 0x7FFFu : 0x7FFFu);
                memory5.write16(base + 2u, v == 0 ? 0x0000u : 0x7FFFu);
                memory5.write16(base + 4u, 0x1000u);
                memory5.write16(base + 6u, 0x000Fu);
                memory5.write16(base + 8u, 0x1FC0u);
                memory5.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu, 0u);
                memory5.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu + 2u, 0x1000u);
                memory5.write16(kSpu + 0x1A0u, static_cast<uint16_t>(1u << v));
            }
            memory5.write16(kSpu + 0x188u, 0x0001u); // dry L: voice 0 only
            memory5.write16(kSpu + 0x190u, 0x0001u); // dry R: voice 0 only (VOLR 0)
            memory5.write16(kSpu + 0x18Cu, 0x0004u); // wet L: voice 2 only
            memory5.write16(kSpu + 0x194u, 0x0004u); // wet R: voice 2 only
            (void)spu5.mixTick();
            (void)spu5.mixTick();
            (void)spu5.mixTick();
            const auto routed = spu5.mixTick();
            ok = ok && expect(routed.left == 4085 && routed.right == 0,
                              "VOLR 0 gives a silent right channel while left mixes normally");
            ok = ok && expect(routed.coreWetL[0] == 4085 && routed.coreWetR[0] == 4085 &&
                                  routed.coreDryL[0] == 4085,
                              "a wet-only voice lands in the wet bus (not dry) with the same contribution");
            ok = ok && expect(spu5.samplesPlayed(0, 1) == 4u,
                              "an unrouted voice still plays its samples even though it mixes to nothing");
        }

        // Core routing: a core-1 voice lands in core 1's dry bus only, but
        // still reaches the final output through the core sum.
        {
            IopMemory memory6;
            Spu2 &spu6 = memory6.spu2();
            const auto writeBlock6 = [&](uint32_t core, uint32_t address)
            {
                memory6.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
                memory6.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
                for (int i = 0; i < 8; ++i)
                    memory6.write16(kSpu + core + 0x1ACu, loopA[i]);
            };
            writeBlock6(kCore1, 0x1000u);
            const uint32_t base = kSpu + kCore1 + 0u;
            memory6.write16(base + 0u, 0x7FFFu);
            memory6.write16(base + 2u, 0x7FFFu);
            memory6.write16(base + 4u, 0x1000u);
            memory6.write16(base + 6u, 0x000Fu);
            memory6.write16(base + 8u, 0x1FC0u);
            memory6.write16(kSpu + kCore1 + 0x1C0u, 0u);
            memory6.write16(kSpu + kCore1 + 0x1C0u + 2u, 0x1000u);
            memory6.write16(kSpu + kCore1 + 0x1A0u, 0x0001u);
            memory6.write16(kSpu + kCore1 + 0x188u, 0x0001u);
            memory6.write16(kSpu + kCore1 + 0x190u, 0x0001u);
            (void)spu6.mixTick();
            (void)spu6.mixTick();
            (void)spu6.mixTick();
            const auto core1 = spu6.mixTick();
            ok = ok && expect(core1.coreDryL[0] == 0 && core1.coreDryL[1] == 4085 &&
                                  core1.left == 4085,
                              "a core-1 voice mixes into core 1's dry bus only and reaches the final sum");
        }

        // PMON: core 0 voice 0 is a constant-max modulator (looping, pitch
        // 1.0, fast ADSR); core 0 voice 1 is a one-shot at pitch 0x2000 with
        // PMON bit 1 set; core 1 voice 0 is the same one-shot at pitch 0x2000
        // with no PMON (control). Traced per-tick boundary (modulator OUTX /
        // modulated step / whole samples consumed this tick / cumulative):
        // T1 -1/8191/1/1, T2 3728/9124/3/4, T3 24352/14280/3/7, then OUTX
        // 28558 and step 15331 with wholes 4,4,3,4,4,4,3,4,4,4,3,4,4, giving
        // cumulative 11,15,18,22,26,30,33,37,41,45,48,52,56 at T16. The
        // earlier 19/20-tick expectation was wrong: it summed step>>12 per
        // tick (1 + 2 + 3*18 = 57 at 20 ticks), ignoring the Phase 7
        // pitch-fraction carry, which makes the steady rate 15331/4096 =
        // 3.743 samples/tick, not 3. The voice therefore finishes on tick
        // 16 (its 16th output sample; after tick 15 it has consumed 52 of
        // 56 samples and is still active). The PMON implementation itself
        // is unchanged: each traced step equals floor(0x2000 * (OUTX +
        // 0x8000) / 0x8000) from the documented formula. Note the old
        // 20-tick check never even ran: `ok = ok && expect(...)`
        // short-circuits after the first failure, so both checks below
        // evaluate their expect() first. The control voice, at 2
        // samples/tick, has consumed only 32 after 16 ticks and is still
        // playing.
        {
            const uint16_t constMax[8] = {0x0700u, 0x7777u, 0x7777u, 0x7777u, 0x7777u, 0x7777u, 0x7777u, 0x7777u};
            const uint16_t oneShotA[8] = {0x0000u, 0x3210u, 0x7654u, 0, 0, 0, 0, 0};
            const uint16_t oneShotB[8] = {0x0100u, 0x0087u, 0, 0, 0, 0, 0, 0};
            IopMemory memory7;
            Spu2 &spu7 = memory7.spu2();
            const auto writeBlock7 = [&](uint32_t core, uint32_t address, const uint16_t block[8])
            {
                memory7.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
                memory7.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
                for (int i = 0; i < 8; ++i)
                    memory7.write16(kSpu + core + 0x1ACu, block[i]);
            };
            const auto setVoice7 = [&](uint32_t core, int voice, uint32_t address, uint16_t pitch)
            {
                const uint32_t base = kSpu + core + static_cast<uint32_t>(voice) * 0x10u;
                memory7.write16(base + 0u, 0x7FFFu);
                memory7.write16(base + 2u, 0x7FFFu);
                memory7.write16(base + 4u, pitch);
                memory7.write16(base + 6u, 0x000Fu);
                memory7.write16(base + 8u, 0x1FC0u);
                memory7.write16(kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu, 0u);
                memory7.write16(kSpu + core + 0x1C0u + static_cast<uint32_t>(voice) * 0xCu + 2u,
                                static_cast<uint16_t>(address));
                memory7.write16(kSpu + core + 0x1A0u, static_cast<uint16_t>(1u << voice));
            };
            writeBlock7(0u, 0x5000u, constMax);
            writeBlock7(0u, 0x1000u, oneShotA);
            writeBlock7(0u, 0x1008u, oneShotB);
            setVoice7(0u, 0, 0x5000u, 0x1000u); // modulator
            setVoice7(0u, 1, 0x1000u, 0x2000u); // modulated
            setVoice7(kCore1, 0, 0x1000u, 0x2000u); // control (same RAM content)
            memory7.write16(kSpu + 0x180u, 0x0002u); // PMON bit 1 on core 0
            for (int i = 0; i < 15; ++i)
                (void)spu7.mixTick();
            // Evaluate each expect() before combining, so a failure in the
            // first check cannot short-circuit (and thereby vacate) the
            // second, as the old 19/20-tick pair did.
            const bool pmonStillPlaying = expect(spu7.voiceActive(0, 1) && spu7.voiceActive(1, 0),
                                                 "after 15 ticks neither the PMON voice nor the control has finished");
            ok = ok && pmonStillPlaying;
            (void)spu7.mixTick();
            const bool pmonFinished = expect(!spu7.voiceActive(0, 1) && spu7.voiceActive(1, 0) &&
                                                 spu7.samplesPlayed(0, 1) == 16u,
                                             "after 16 ticks the PMON voice has finished (modulated step) while the control still plays");
            ok = ok && pmonFinished;
        }

        // NON: a noise-flagged voice is muted in the mix (no step, no
        // contribution) while a sibling ADPCM voice on the same core mixes
        // normally; the noise generator itself is not modelled.
        {
            IopMemory memory8;
            Spu2 &spu8 = memory8.spu2();
            const auto writeBlock8 = [&](uint32_t address)
            {
                memory8.write16(kSpu + 0x1A8u, static_cast<uint16_t>(address >> 16));
                memory8.write16(kSpu + 0x1AAu, static_cast<uint16_t>(address));
                for (int i = 0; i < 8; ++i)
                    memory8.write16(kSpu + 0x1ACu, loopA[i]);
            };
            writeBlock8(0x1000u);
            for (int v = 0; v < 2; ++v)
            {
                const uint32_t base = kSpu + static_cast<uint32_t>(v) * 0x10u;
                memory8.write16(base + 0u, 0x7FFFu);
                memory8.write16(base + 2u, 0x7FFFu);
                memory8.write16(base + 4u, 0x1000u);
                memory8.write16(base + 6u, 0x000Fu);
                memory8.write16(base + 8u, 0x1FC0u);
                memory8.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu, 0u);
                memory8.write16(kSpu + 0x1C0u + static_cast<uint32_t>(v) * 0xCu + 2u, 0x1000u);
                memory8.write16(kSpu + 0x1A0u, static_cast<uint16_t>(1u << v));
            }
            memory8.write16(kSpu + 0x184u, 0x0001u); // NON bit 0: voice 0 is noise
            memory8.write16(kSpu + 0x188u, 0x0003u);
            memory8.write16(kSpu + 0x190u, 0x0003u);
            (void)spu8.mixTick();
            (void)spu8.mixTick();
            (void)spu8.mixTick();
            const auto non = spu8.mixTick();
            ok = ok && expect(non.left == 4085 && spu8.samplesPlayed(0, 0) == 0u &&
                                  spu8.samplesPlayed(0, 1) == 4u,
                              "a NON voice contributes nothing and does not step, while its ADPCM sibling mixes normally");
        }
        return ok;
    }

    // SPU2 reverb (Phase 10, D-REV-1..8). The voice below is a constant
    // block — every nibble 1, filter 0, shift 0, so every decoded sample
    // is 4096 — played at pitch 1.0 with ADSR1 0x000F / ADSR2 0x1FC0
    // (fast attack, sustain 15, never-stepping sustain). Its Gaussian
    // transient at pitch 1.0 is -1, 607, 3477, then a steady 4077 (the
    // Phase 7 quadruple at index 0 sums to 0x7F80, not 0x8000, and each
    // term floors), and with full volume the wet bus is -1, 531, 3476,
    // then a steady 4076 (envelope 14336, 28671, 32766, 32766 — the
    // Phase 9 two-stage volume order). Every reverb expectation below
    // is hand-derived from the psx-spx formula with those inputs and
    // cross-checked against an independent Python calculation of the
    // same formula (not against the C++ implementation): pair inputs
    // are (wet[t-1]+wet[t])/2 floored, Lin = (0x7FFF*input)>>15,
    // SAME = ((Lin - prev)*0x4000)>>15 + prev, COMB = SAME/2,
    // EVOL = (COMB*0x7FFF)>>15. Pair 0 input ( -1+531)/2 = 265 gives
    // Lin 264, SAME 132, COMB 66, out 65; pair 1 input 3776 gives
    // Lin 3775, SAME 1953, COMB 976, out 975; and so on. A reverb
    // output computed on a stepping (even) tick is included in that
    // tick and held through the next tick, so each value appears twice.
    bool testSpu2Reverb()
    {
        constexpr uint32_t kSpu = 0x1F900000u;
        constexpr uint32_t kCore1 = 0x400u;
        const auto writeBlock = [&](IopMemory &memory, uint32_t core, uint32_t address, uint16_t header)
        {
            memory.write16(kSpu + core + 0x1A8u, static_cast<uint16_t>(address >> 16));
            memory.write16(kSpu + core + 0x1AAu, static_cast<uint16_t>(address));
            memory.write16(kSpu + core + 0x1ACu, header);
            for (int i = 0; i < 7; ++i)
                memory.write16(kSpu + core + 0x1ACu, 0x1111u);
        };
        const auto setVoice = [&](IopMemory &memory, uint32_t core, uint32_t address)
        {
            const uint32_t base = kSpu + core;
            memory.write16(base + 0u, 0x7FFFu);
            memory.write16(base + 2u, 0x7FFFu);
            memory.write16(base + 4u, 0x1000u); // pitch 1.0
            memory.write16(base + 6u, 0x000Fu);
            memory.write16(base + 8u, 0x1FC0u);
            memory.write16(kSpu + core + 0x1C0u, 0u);
            memory.write16(kSpu + core + 0x1C0u + 2u, static_cast<uint16_t>(address));
            memory.write16(kSpu + core + 0x1A0u, 0x0001u);
        };
        const auto setVmix = [&](IopMemory &memory, uint32_t core, uint32_t dry, uint32_t wet)
        {
            memory.write16(kSpu + core + 0x188u, static_cast<uint16_t>(dry));
            memory.write16(kSpu + core + 0x190u, static_cast<uint16_t>(dry));
            memory.write16(kSpu + core + 0x18Cu, static_cast<uint16_t>(wet));
            memory.write16(kSpu + core + 0x194u, static_cast<uint16_t>(wet));
        };

        bool ok = true;

        // A: one-shot impulse/step response and tail decay (wet only,
        // default params, unity effect volume). The voice ends after
        // tick 28; the tail must NOT cut at the voice end (tick 29 still
        // holds the steady value) but must decay to exactly 0.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            writeBlock(memory, 0u, 0x1000u, 0x0100u); // one-shot: loop-end, no loop
            setVoice(memory, 0u, 0x1000u);
            setVmix(memory, 0u, 0u, 1u);
            spu.setReverbParams(0, Spu2::defaultReverbParams());
            spu.setReverbEnabled(0, true);
            std::array<Spu2::MixTick, 60> ticks{};
            for (auto &tick : ticks)
                tick = spu.mixTick();
            const auto at = [&](int t) { return ticks[static_cast<size_t>(t - 1)]; };
            const bool head = expect(at(1).left == 0 && at(2).left == 65 && at(3).left == 65 &&
                                         at(4).left == 975 && at(5).left == 975 && at(6).left == 1506 &&
                                         at(7).left == 1506 && at(8).left == 1771 && at(9).left == 1771,
                                     "reverb head follows the structure-derived sequence 0, 65, 65, 975, 975, 1506, 1506, 1771, 1771");
            ok = ok && head;
            const bool wetFeed = expect(at(1).coreWetL[0] == -1 && at(2).coreWetL[0] == 531 &&
                                            at(3).coreWetL[0] == 3476 && at(4).coreWetL[0] == 4076 &&
                                            at(4).coreDryL[0] == 0 && at(4).right == at(4).left,
                                        "wet bus feeds reverb (-1, 531, 3476, 4076) while the dry bus stays 0 and L/R agree");
            ok = ok && wetFeed;
            const bool tail = expect(at(28).left == 2036 && at(29).left == 2036 &&
                                         at(30).left == 1017 && at(32).left == 508 && at(48).left == 0,
                                     "voice end does not cut the tail (tick 29 still 2036); it decays 1017, 508, ... to exactly 0 by tick 48");
            ok = ok && tail;
            bool monotone = true;
            for (int t = 30; t < 48; ++t)
                monotone = monotone && at(t).left >= at(t + 1).left;
            const bool mono = expect(monotone, "the reverb tail decreases monotonically to zero");
            ok = ok && mono;
        }

        // B: wet/dry balance via the model effect volume, and the
        // effect-volume gate: half volume halves the reverb output on
        // top of an unchanged dry bus; zero volume removes reverb while
        // the dry bus still mixes. Default-off (never enabled) reproduces
        // the Phase 9 dry values exactly.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            writeBlock(memory, 0u, 0x1000u, 0x0100u);
            setVoice(memory, 0u, 0x1000u);
            setVmix(memory, 0u, 1u, 1u);
            Spu2::ReverbParams half = Spu2::defaultReverbParams();
            half.effectVolumeL = 0x4000;
            half.effectVolumeR = 0x4000;
            spu.setReverbParams(0, half);
            spu.setReverbEnabled(0, true);
            (void)spu.mixTick();
            const auto t2 = spu.mixTick();
            (void)spu.mixTick();
            const auto t4 = spu.mixTick();
            const bool balance = expect(t2.coreDryL[0] == 531 && t2.coreReverbL[0] == 33 && t2.left == 564 &&
                                           t4.coreDryL[0] == 4076 && t4.coreReverbL[0] == 488 &&
                                           t4.left == 4564,
                                       "half effect volume gives reverb 33/488 over dry 531/4076, final 564/4564");
            ok = ok && balance;
        }
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            writeBlock(memory, 0u, 0x1000u, 0x0100u);
            setVoice(memory, 0u, 0x1000u);
            setVmix(memory, 0u, 1u, 1u);
            Spu2::ReverbParams zero = Spu2::defaultReverbParams();
            zero.effectVolumeL = 0;
            zero.effectVolumeR = 0;
            spu.setReverbParams(0, zero);
            spu.setReverbEnabled(0, true);
            (void)spu.mixTick();
            const auto t2 = spu.mixTick();
            (void)spu.mixTick();
            const auto t4 = spu.mixTick();
            const bool gated = expect(t2.coreReverbL[0] == 0 && t2.left == 531 && t4.coreReverbL[0] == 0 &&
                                         t4.left == 4076,
                                     "zero effect volume removes reverb while the dry bus still mixes 531/4076");
            ok = ok && gated;
        }
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            writeBlock(memory, 0u, 0x1000u, 0x0100u);
            setVoice(memory, 0u, 0x1000u);
            setVmix(memory, 0u, 1u, 1u);
            // Reverb never enabled: Phase 9 dry values, reverb fields 0.
            (void)spu.mixTick();
            (void)spu.mixTick();
            (void)spu.mixTick();
            const auto t4 = spu.mixTick();
            const bool off = expect(t4.left == 4076 && t4.coreReverbL[0] == 0 && !spu.reverbEnabled(0),
                                   "reverb off by default reproduces the Phase 9 dry value 4076 with no reverb output");
            ok = ok && off;
        }

        // C: per-core isolation. Core 0 uses the default params; core 1
        // gets the same wet feed but vIIR 0, which zeroes its reflection
        // path (SAME holds its zero history), so core 1 contributes
        // nothing while core 0 outputs 65. A second run proves a core
        // whose reverb was never enabled contributes nothing either.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            writeBlock(memory, 0u, 0x1000u, 0x0100u);
            writeBlock(memory, kCore1, 0x1000u, 0x0100u);
            setVoice(memory, 0u, 0x1000u);
            setVoice(memory, kCore1, 0x1000u);
            setVmix(memory, 0u, 0u, 1u);
            setVmix(memory, kCore1, 0u, 1u);
            spu.setReverbParams(0, Spu2::defaultReverbParams());
            spu.setReverbEnabled(0, true);
            Spu2::ReverbParams flat = Spu2::defaultReverbParams();
            flat.vIir = 0;
            spu.setReverbParams(1, flat);
            spu.setReverbEnabled(1, true);
            (void)spu.mixTick();
            const auto t2 = spu.mixTick();
            const bool isolation = expect(t2.coreReverbL[0] == 65 && t2.coreReverbL[1] == 0 &&
                                             t2.left == 65,
                                         "core 0 reverbs to 65 while core 1 (vIIR 0) contributes 0 to the same final sum");
            ok = ok && isolation;
        }
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            writeBlock(memory, 0u, 0x1000u, 0x0100u);
            writeBlock(memory, kCore1, 0x1000u, 0x0100u);
            setVoice(memory, 0u, 0x1000u);
            setVoice(memory, kCore1, 0x1000u);
            setVmix(memory, 0u, 0u, 1u);
            setVmix(memory, kCore1, 0u, 1u);
            spu.setReverbParams(0, Spu2::defaultReverbParams());
            spu.setReverbEnabled(0, true);
            (void)spu.mixTick();
            const auto t2 = spu.mixTick();
            const bool disabledCore = expect(t2.coreReverbL[1] == 0 && t2.left == 65 && !spu.reverbEnabled(1),
                                            "a core whose reverb was never enabled contributes 0");
            ok = ok && disabledCore;
        }

        // D: feedback stability. A looping constant voice feeds the
        // default reverb for 20000 ticks: the output must converge to
        // the structure's steady state 2036 (SAME fixed point 4074,
        // COMB 2037, EVOL 2036) and never exceed it.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            writeBlock(memory, 0u, 0x1000u, 0x0700u); // looping: loop-end + loop + loop-start replays the block
            setVoice(memory, 0u, 0x1000u);
            setVmix(memory, 0u, 0u, 1u);
            spu.setReverbParams(0, Spu2::defaultReverbParams());
            spu.setReverbEnabled(0, true);
            int32_t maxAbs = 0;
            int32_t last = 0;
            for (int i = 0; i < 20000; ++i)
            {
                const auto tick = spu.mixTick();
                const int32_t v = tick.coreReverbL[0] < 0 ? -tick.coreReverbL[0] : tick.coreReverbL[0];
                if (v > maxAbs)
                    maxAbs = v;
                last = tick.coreReverbL[0];
            }
            const bool stable = expect(maxAbs == 2036 && last == 2036,
                                      "20000 ticks of constant feed converge to steady state 2036 with no overshoot");
            ok = ok && stable;
        }

        // E: reverb never touches sound RAM. A pattern written through
        // the data port and the voice block itself must read back
        // unchanged after 1000 reverb ticks.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            writeBlock(memory, 0u, 0x1000u, 0x0700u);
            memory.write16(kSpu + 0x1A8u, 0u);
            memory.write16(kSpu + 0x1AAu, 0x8000u);
            for (int i = 0; i < 16; ++i)
                memory.write16(kSpu + 0x1ACu, static_cast<uint16_t>(0xBEEFu - i));
            setVoice(memory, 0u, 0x1000u);
            setVmix(memory, 0u, 0u, 1u);
            spu.setReverbParams(0, Spu2::defaultReverbParams());
            spu.setReverbEnabled(0, true);
            for (int i = 0; i < 1000; ++i)
                (void)spu.mixTick();
            bool ramOk = spu.ram(0x1000u) == 0x0700u;
            for (int i = 0; i < 16; ++i)
                ramOk = ramOk && spu.ram(0x8000u + static_cast<uint32_t>(i)) == static_cast<uint16_t>(0xBEEFu - i);
            const bool ram = expect(ramOk, "sound RAM pattern and voice block are unchanged after 1000 reverb ticks");
            ok = ok && ram;
        }

        // F: lifecycle guards. reset() clears the enable flag (and the
        // state with it); out-of-range cores are ignored, never crash.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            spu.setReverbParams(0, Spu2::defaultReverbParams());
            spu.setReverbEnabled(0, true);
            spu.setReverbParams(2, Spu2::defaultReverbParams());
            spu.setReverbEnabled(-1, true);
            const bool guards = expect(spu.reverbEnabled(0) && !spu.reverbEnabled(1) &&
                                          !spu.reverbEnabled(2) && !spu.reverbEnabled(-1),
                                      "enable state tracks only valid cores 0/1");
            ok = ok && guards;
            spu.reset();
            const bool cleared = expect(!spu.reverbEnabled(0), "reset() clears the reverb enable flag");
            ok = ok && cleared;
        }
        return ok;
    }

    // ADMA sound-data input (Phase 11, D-ADMA-1/2/3): a per-core stereo
    // PCM stream fed by DMA and consumed one frame per 48 kHz mix tick.
    // The guest ADMA register offset is unpinned, so the tests drive
    // the model API (setAdmaEnabled/admaWrite) directly; no guest
    // register write is asserted to enable ADMA.
    bool testSpu2Adma()
    {
        constexpr uint32_t kSpu = 0x1F900000u;
        constexpr uint32_t kCore1 = 0x400u;
        bool ok = true;
        const auto feed = [&](Spu2 &spu, int core, const std::vector<std::pair<int16_t, int16_t>> &frames)
        {
            std::vector<uint16_t> halfwords;
            for (const auto &frame : frames)
            {
                halfwords.push_back(static_cast<uint16_t>(frame.first));
                halfwords.push_back(static_cast<uint16_t>(frame.second));
            }
            spu.admaWrite(core, halfwords.data(), halfwords.size());
        };

        // A: disabled by default. A feed is dropped, a bulk dmaWrite
        // still lands in sound RAM at TSA, and the mix has no ADMA.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            ok = ok && expect(!spu.admaEnabled(0) && !spu.admaEnabled(1) &&
                                  spu.admaFramesAvailable(0) == 0u && !spu.admaRefillRequested(0),
                              "ADMA is disabled by default on both cores");
            const uint16_t data[2] = {0x1234u, 0x5678u};
            feed(spu, 0, {{0x1000, -0x1000}});
            ok = ok && expect(spu.admaFramesAvailable(0) == 0u, "a feed into a disabled stream is dropped");
            spu.dmaWrite(0, data, 2u);
            ok = ok && expect(spu.ram(0u) == 0x1234u && spu.ram(1u) == 0x5678u &&
                                  spu.admaFramesAvailable(0) == 0u,
                              "with ADMA disabled, dmaWrite still bulk-writes at TSA (here 0)");
            const auto tick = spu.mixTick();
            ok = ok && expect(tick.left == 0 && tick.right == 0 && tick.coreAdmaL[0] == 0 &&
                                  tick.coreAdmaR[0] == 0,
                              "with ADMA disabled, the mix has no ADMA contribution");
        }

        // B: sequence and underrun. Frames come out in feed order, one
        // per mix tick; once the buffer empties the core is silent.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            spu.setAdmaEnabled(0, true);
            feed(spu, 0, {{1000, -1000}, {2000, -2000}, {3000, -3000}});
            ok = ok && expect(spu.admaEnabled(0) && spu.admaFramesAvailable(0) == 3u &&
                                  spu.admaRefillRequested(0),
                              "three fed frames are buffered and, being far below one half-buffer, request refill");
            const auto t1 = spu.mixTick();
            ok = ok && expect(t1.left == 1000 && t1.right == -1000 && t1.coreAdmaL[0] == 1000 &&
                                  t1.coreAdmaR[0] == -1000 && spu.admaFramesAvailable(0) == 2u,
                              "tick 1 consumes the first ADMA frame (1000, -1000)");
            const auto t2 = spu.mixTick();
            ok = ok && expect(t2.left == 2000 && t2.right == -2000 && spu.admaFramesAvailable(0) == 1u,
                              "tick 2 consumes the second ADMA frame (2000, -2000)");
            const auto t3 = spu.mixTick();
            ok = ok && expect(t3.left == 3000 && t3.right == -3000 && spu.admaFramesAvailable(0) == 0u,
                              "tick 3 consumes the third ADMA frame (3000, -3000)");
            const auto t4 = spu.mixTick();
            ok = ok && expect(t4.left == 0 && t4.right == 0 && t4.coreAdmaL[0] == 0 &&
                                  spu.admaFramesAvailable(0) == 0u,
                              "tick 4 underruns to silence once the buffer is empty");
        }

        // C: cores are independent, and the DMA path routes into ADMA
        // when enabled (TSA does not advance for stream data).
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            memory.write16(kSpu + kCore1 + 0x1A8u, 0u);
            memory.write16(kSpu + kCore1 + 0x1AAu, 0x3000u);
            spu.setAdmaEnabled(1, true);
            const uint16_t data[4] = {0x1111u, 0x2222u, 0x3333u, 0x4444u};
            spu.dmaWrite(1, data, 4u);
            ok = ok && expect(spu.admaFramesAvailable(1) == 2u && spu.admaFramesAvailable(0) == 0u &&
                                  spu.transferAddress(1) == 0x3000u,
                              "dmaWrite into an ADMA-enabled core feeds the stream and leaves TSA alone");
            const auto tick = spu.mixTick();
            ok = ok && expect(tick.coreAdmaL[1] == 0x1111 && tick.coreAdmaR[1] == 0x2222 &&
                                  tick.coreAdmaL[0] == 0 && tick.left == 0x1111 && tick.right == 0x2222,
                              "tick consumes core 1's first frame; core 0 contributes nothing");
        }

        // D: disable stops and clears; re-enable starts fresh.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            spu.setAdmaEnabled(0, true);
            feed(spu, 0, {{500, -500}, {600, -600}});
            (void)spu.mixTick();
            ok = ok && expect(spu.admaFramesAvailable(0) == 1u, "one frame remains after one tick");
            spu.setAdmaEnabled(0, false);
            const auto tick = spu.mixTick();
            ok = ok && expect(tick.coreAdmaL[0] == 0 && spu.admaFramesAvailable(0) == 0u &&
                                  !spu.admaEnabled(0),
                              "disabling ADMA stops the stream and clears the buffer");
            spu.setAdmaEnabled(0, true);
            ok = ok && expect(spu.admaFramesAvailable(0) == 0u, "re-enabling starts with an empty buffer");
        }

        // E: refill request is level-based (fewer than one half-buffer
        // of 256 frames), and a feed clears it again.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            spu.setAdmaEnabled(0, true);
            std::vector<std::pair<int16_t, int16_t>> frames;
            for (int i = 0; i < 512; ++i)
                frames.emplace_back(static_cast<int16_t>(i), static_cast<int16_t>(-i));
            feed(spu, 0, frames);
            ok = ok && expect(spu.admaFramesAvailable(0) == 512u && !spu.admaRefillRequested(0),
                              "a full 512-frame buffer does not request refill");
            for (int i = 0; i < 256; ++i)
                (void)spu.mixTick();
            ok = ok && expect(spu.admaFramesAvailable(0) == 256u && !spu.admaRefillRequested(0),
                              "exactly one half-buffer remaining does not yet request refill");
            (void)spu.mixTick();
            ok = ok && expect(spu.admaFramesAvailable(0) == 255u && spu.admaRefillRequested(0),
                              "dropping below one half-buffer requests refill");
            feed(spu, 0, {{700, -700}});
            ok = ok && expect(spu.admaFramesAvailable(0) == 256u && !spu.admaRefillRequested(0),
                              "a refill feed back to one half-buffer clears the request");
        }

        // F: bounds and odd counts. Over-capacity feeds drop the excess
        // without touching RAM outside the core's input area, and an
        // odd trailing halfword waits for its partner.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            spu.setAdmaEnabled(0, true);
            std::vector<std::pair<int16_t, int16_t>> frames;
            for (int i = 0; i < 600; ++i)
                frames.emplace_back(1, 1);
            feed(spu, 0, frames);
            ok = ok && expect(spu.admaFramesAvailable(0) == 512u,
                              "a 600-frame feed into a 512-frame buffer keeps exactly 512 frames");
            ok = ok && expect(spu.ram(0x1FFFu) == 0u && spu.ram(0x2800u) == 0u && spu.ram(0x2400u) == 0u,
                              "an over-capacity feed touches neither the output area, the waveform area, nor core 1's area");
            IopMemory memory2;
            Spu2 &spu2 = memory2.spu2();
            spu2.setAdmaEnabled(0, true);
            const uint16_t odd[3] = {0x1111u, 0x2222u, 0x3333u};
            spu2.admaWrite(0, odd, 3u);
            ok = ok && expect(spu2.admaFramesAvailable(0) == 1u,
                              "an odd feed completes one frame and holds the trailing halfword");
            const uint16_t partner[1] = {0x4444u};
            spu2.admaWrite(0, partner, 1u);
            ok = ok && expect(spu2.admaFramesAvailable(0) == 2u, "the next halfword completes the pending frame");
            const auto tick = spu2.mixTick();
            ok = ok && expect(tick.coreAdmaL[0] == 0x1111 && tick.coreAdmaR[0] == 0x2222,
                              "the first completed frame reads back as fed");
        }

        // G: no double-signal. An ADMA feed at the IRQA address raises
        // no interrupt and sets no STAT bit (completeDma is not
        // called by the feed itself).
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            memory.write16(kSpu + 0x19Au, 0x0040u); // ATTR bit 6: IRQ enable
            memory.write16(kSpu + 0x19Cu, 0u);      // IRQA high
            memory.write16(kSpu + 0x19Eu, 0x2000u); // IRQA low: input area base
            spu.setAdmaEnabled(0, true);
            feed(spu, 0, {{100, -100}});
            ok = ok && expect(!spu.takeInterrupt() && spu.peek16(0x344u) == 0u,
                              "an ADMA feed at IRQA raises no interrupt and sets no STAT bit");
        }

        // H: voice independence and guest time. A pitch-2.0 looping
        // voice does not change the ADMA rate (one frame per tick),
        // and advanceCycles consumes ADMA in guest time.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            memory.write16(kSpu + 0x1A8u, 0u);
            memory.write16(kSpu + 0x1AAu, 0x1000u);
            const uint16_t loop[8] = {0x0700u, 0x3210u, 0x7654u, 0, 0, 0, 0, 0};
            for (int i = 0; i < 8; ++i)
                memory.write16(kSpu + 0x1ACu, loop[i]);
            memory.write16(kSpu + 0u, 0x7FFFu);
            memory.write16(kSpu + 2u, 0x7FFFu);
            memory.write16(kSpu + 4u, 0x2000u); // pitch 2.0
            memory.write16(kSpu + 6u, 0x000Fu);
            memory.write16(kSpu + 8u, 0x1FC0u);
            memory.write16(kSpu + 0x1C0u, 0u);
            memory.write16(kSpu + 0x1C0u + 2u, 0x1000u);
            memory.write16(kSpu + 0x1A0u, 0x0001u);
            memory.write16(kSpu + 0x188u, 0x0001u);
            memory.write16(kSpu + 0x190u, 0x0001u);
            spu.setAdmaEnabled(0, true);
            feed(spu, 0, {{900, -900}, {800, -800}, {700, -700}, {600, -600}});
            const auto t1 = spu.mixTick();
            ok = ok && expect(t1.coreAdmaL[0] == 900 && spu.voiceActive(0, 0),
                              "tick 1 with a pitch-2.0 voice still consumes exactly one ADMA frame");
            const auto t2 = spu.mixTick();
            ok = ok && expect(t2.coreAdmaL[0] == 800 && spu.admaFramesAvailable(0) == 2u,
                              "tick 2 consumes exactly one more ADMA frame");
            spu.advanceCycles(768u * 2u);
            ok = ok && expect(spu.admaFramesAvailable(0) == 0u,
                              "two guest-time samples consume the last two ADMA frames");
        }

        // I: reset clears ADMA state.
        {
            IopMemory memory;
            Spu2 &spu = memory.spu2();
            spu.setAdmaEnabled(0, true);
            feed(spu, 0, {{100, -100}});
            spu.reset();
            ok = ok && expect(!spu.admaEnabled(0) && spu.admaFramesAvailable(0) == 0u,
                              "reset() clears ADMA enable and buffered frames");
        }
        return ok;
    }

    // IOP fixed allocation (AllocSysMemory mode 2) with guest-controlled address and size: the old
    // range check wrapped in uint32_t, so a fixed address near 0xFFFFFFFF was accepted as an
    // allocation outside IOP RAM, and freeing it would fill m_owned past its end.
    bool testIopFixedAllocationOverflow()
    {
        IopMemory memory;
        bool ok = true;
        ok = ok && expect(memory.allocate(0x20u, 16u, 0xFFFFFFF0u) == 0u,
                          "a fixed allocation wrapping past UINT32_MAX is rejected");
        ok = ok && expect(memory.allocate(0xFFFFFFF0u, 16u, IopMemory::HeapBase) == 0u,
                          "a huge size that wraps on alignment is rejected");
        ok = ok && expect(memory.allocate(0x20u, 16u, IopMemory::HeapLimit - 0x10u) == 0u,
                          "a fixed allocation ending past HeapLimit is rejected");
        ok = ok && expect(memory.allocate(0x20u, 16u, IopMemory::HeapLimit) == 0u,
                          "a fixed allocation starting at HeapLimit is rejected");
        const uint32_t valid = memory.allocate(0x20u, 16u, IopMemory::HeapBase);
        ok = ok && expect(valid == IopMemory::HeapBase && memory.ownsRamRange(valid, 0x20u) &&
                              memory.freeAllocation(valid) && !memory.ownsRamRange(valid, 0x20u),
                          "a valid fixed allocation still works and frees cleanly");
        ok = ok && expect(memory.allocate(0x20u, 16u, IopMemory::HeapBase) == IopMemory::HeapBase,
                          "a freed fixed slot can be allocated again");
        return ok;
    }

    bool testStrtokMatchesRealPs2()
    {
        IopMemory memory;
        IopSysclib sysclib(memory);
        constexpr uint32_t kText = 0x35000u, kDelim = 0x35100u;
        putString(memory, kText, "val1, val2,   val3,val4");
        putString(memory, kDelim, ", ");
        std::vector<std::string> tokens;
        for (uint32_t s = kText;; s = 0u)
        {
            IopCpuState cpu{};
            cpu.gpr[4] = s;
            cpu.gpr[5] = kDelim;
            (void)sysclib.dispatchImport(35u, cpu);
            if (cpu.gpr[2] == 0u)
                break;
            tokens.push_back(memory.readString(cpu.gpr[2], 64u));
        }
        putString(memory, kText, "val1,val2,val3,val4");
        IopCpuState whole{};
        whole.gpr[4] = kText;
        whole.gpr[5] = 0u;
        (void)sysclib.dispatchImport(35u, whole);
        IopCpuState after{};
        (void)sysclib.dispatchImport(35u, after);
        return expect(tokens == std::vector<std::string>{"val1", "val2", "val3", "val4"}, "strtok tokens differ from the real PS2") &&
               expect(whole.gpr[2] == kText && memory.readString(kText, 64u) == "val1,val2,val3,val4", "a NULL delimiter set returns the whole string") &&
               expect(after.gpr[2] == 0u, "strtok(NULL, NULL) after the whole string returns NULL");
    }
}

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;

    // SIF0 IOP-side tests (Phase 4 vertical slice, design §13, IOP side).
    // The 8-slot FIFO capacity, the header-occupies-a-slot rule, the
    // IOP-side QW rounding for partial tags, and the 1/0/-1 DmaStat
    // encodings are labelled MODEL CHOICES (design §§4, 8), not
    // hardware claims, and are commented as such where asserted.
    // DMACEN is not seeded by IopMemory::reset, so every start below
    // writes DMACEN = 1 explicitly; DPCR2's channel-9 enable bit is
    // seeded set, so gating tests write DPCR2 explicitly.
    constexpr uint32_t kSif0DmacEn = 0x1F801578u;
    constexpr uint32_t kSif0Dpcr2 = 0x1F801570u;
    constexpr uint32_t kSif0Dicr = 0x1F8010F4u;
    constexpr uint32_t kSif0Dicr2 = 0x1F801574u;
    constexpr uint32_t kSif0IStat = 0x1F801070u;
    constexpr uint32_t kSif0IMask = 0x1F801074u;
    constexpr uint32_t kSif0ICtrl = 0x1F801078u;
    constexpr uint32_t kSif0Tadr = 0x1F80152Cu;
    constexpr uint32_t kSif0Chcr = 0x1F801528u;
    constexpr uint32_t kSif0ChcrChain = 0x01000701u; // dir | header | chain mode | STR
    constexpr uint32_t kSif0ChcrBurst = 0x01000101u; // dir | header | burst mode | STR
    constexpr uint32_t kSif0DicrMaster = 1u << 23u;
    constexpr uint32_t kSif0Dicr2Mask = 1u << 18u;
    constexpr uint32_t kSif0Dicr2TagIrq = 1u << 9u;
    constexpr uint32_t kSif0Dicr2Flag = 1u << 26u;
    constexpr uint32_t kSif0Dpcr2Ch9Enable = 1u << 11u;
    constexpr uint32_t kSif0ChcrStart = 1u << 24u;

    void sif0WriteWords(IopMemory &memory, uint32_t address, const uint32_t *words, size_t count)
    {
        (void)memory.writeRam(address, words, count * sizeof(uint32_t));
    }

    // IOP chain tag at TADR (4 words, CHCR.8): word0 = src | IRQ bit 30 | EOT bit 31,
    // word1 = size in words, word2/word3 = EE DMAtag header words.
    void sif0WriteTag(IopMemory &memory, uint32_t tadr, uint32_t src, uint32_t sizeWords, bool irq, bool eot,
                      uint32_t lo, uint32_t hi)
    {
        const uint32_t words[4] = {(src & 0x00FFFFFFu) | (irq ? (1u << 30u) : 0u) | (eot ? (1u << 31u) : 0u),
                                   sizeWords & 0x00FFFFFFu, lo, hi};
        sif0WriteWords(memory, tadr, words, 4u);
    }

    Sif0Fifo::Qword sif0MakeQw(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
    {
        Sif0Fifo::Qword qw{};
        const uint32_t words[4] = {w0, w1, w2, w3};
        std::memcpy(qw.bytes.data(), words, sizeof(words));
        return qw;
    }

    uint32_t sif0QwWord(const Sif0Fifo::Qword &qw, size_t index)
    {
        uint32_t word = 0u;
        std::memcpy(&word, qw.bytes.data() + index * 4u, sizeof(word));
        return word;
    }

    bool sif0QwEquals(const Sif0Fifo::Qword &qw, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
    {
        return sif0QwWord(qw, 0u) == w0 && sif0QwWord(qw, 1u) == w1 && sif0QwWord(qw, 2u) == w2 &&
               sif0QwWord(qw, 3u) == w3;
    }

    // DICR master + DICR2 value + I_MASK bit 3 + I_CTRL 1. DICR2 flag bits are
    // write-1-to-clear on guest writes, so a value here never sets flag 26.
    void sif0SetInterrupts(IopMemory &memory, uint32_t dicr2)
    {
        memory.write32(kSif0Dicr, kSif0DicrMaster);
        memory.write32(kSif0Dicr2, dicr2);
        memory.write32(kSif0IMask, 1u << 3u);
        memory.write32(kSif0ICtrl, 1u);
    }

    // Attach the transport, enable it, enable DMAC, point TADR, then write CHCR STR.
    // The CHCR write only starts the engine; progress happens in driveSif0().
    void sif0Start(IopMemory &memory, Sif0Fifo &fifo, uint32_t tadr)
    {
        memory.setSif0Transport(&fifo);
        memory.setTransportOwnsSif0(true);
        memory.write32(kSif0DmacEn, 1u);
        memory.write32(kSif0Tadr, tadr);
        memory.write32(kSif0Chcr, kSif0ChcrChain);
    }

    // Full manual arming for the start-gating tests, where each gate varies independently.
    void sif0ArmManual(IopMemory &memory, Sif0Fifo &fifo, bool owned, uint32_t dmacEn, uint32_t dpcr2,
                       uint32_t tadr, uint32_t chcr)
    {
        memory.setSif0Transport(&fifo);
        memory.setTransportOwnsSif0(owned);
        memory.write32(kSif0DmacEn, dmacEn);
        memory.write32(kSif0Dpcr2, dpcr2);
        memory.write32(kSif0Tadr, tadr);
        memory.write32(kSif0Chcr, chcr);
    }

    // Case 1: one EOT tag, 2 payload QWs. Header first, then payload in order,
    // TADR +4 words, CHCR bit 24 clears on EOT, DICR2 flag 26 + I_STAT bit 3 set,
    // and the DICR read shows derived bit 31 set.
    bool testSif0IopChainBasicEot()
    {
        IopMemory memory;
        Sif0Fifo fifo;
        const uint32_t payload[8] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                     0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};
        sif0WriteWords(memory, 0x1000u, payload, 8u);
        sif0WriteTag(memory, 0x2000u, 0x1000u, 8u, false, true, 0xAABBCCDDu, 0x11223344u);
        sif0SetInterrupts(memory, kSif0Dicr2Mask);
        sif0Start(memory, fifo, 0x2000u);
        if (!expect(memory.sif0Active(), "sif0 basic: engine did not start on CHCR STR"))
            return false;
        memory.driveSif0();
        Sif0Fifo::Header header{};
        if (!expect(fifo.tryPopHeader(header), "sif0 basic: FIFO did not receive the header first"))
            return false;
        Sif0Fifo::Qword qw0{}, qw1{};
        if (!expect(header.lo == 0xAABBCCDDu && header.hi == 0x11223344u, "sif0 basic: header words differ from the tag's extra words") ||
            !expect(fifo.tryPopQw(qw0), "sif0 basic: FIFO did not receive the first payload QW") ||
            !expect(sif0QwEquals(qw0, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u), "sif0 basic: first payload QW is out of order or corrupt") ||
            !expect(fifo.tryPopQw(qw1), "sif0 basic: FIFO did not receive the second payload QW") ||
            !expect(sif0QwEquals(qw1, 0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u), "sif0 basic: second payload QW is out of order or corrupt") ||
            !expect(fifo.isEmpty(), "sif0 basic: FIFO holds units past header + 2 payload QWs"))
            return false;
        return expect(memory.read32(kSif0Tadr) == 0x2010u, "sif0 basic: TADR did not advance by 4 words") &&
               expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) == 0u, "sif0 basic: CHCR bit 24 did not clear on EOT") &&
               expect(!memory.sif0Active(), "sif0 basic: engine is still active after EOT") &&
               expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 basic: DICR2 flag 26 was not set on EOT") &&
               expect((memory.read32(kSif0IStat) & (1u << 3u)) != 0u, "sif0 basic: I_STAT bit 3 was not set on EOT") &&
               expect((memory.read32(kSif0Dicr) & (1u << 31u)) != 0u, "sif0 basic: DICR read did not show derived bit 31 set");
    }

    // Case 2: tag-IRQ independence. (a) Two tags, first IRQ-only, second EOT, with
    // master set, completion mask 18 CLEAR, tag bit 9 set: I_STAT bit 3 is set by the
    // tag-IRQ path alone. (b) One IRQ+EOT tag with both mask and tag bit set: flag 26
    // is set as one bit and I_STAT bit 3 is set (idempotent, not a double assertion).
    // (c) Master clear: no flag, no I_STAT bit 3. (d) I_MASK bit 3 clear: I_STAT bit 3
    // may be set, but (I_STAT & I_MASK) == 0, so delivery is mask-gated.
    bool testSif0IopTagIrqIndependence()
    {
        {
            IopMemory memory;
            Sif0Fifo fifo;
            const uint32_t first[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            const uint32_t second[4] = {0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};
            sif0WriteWords(memory, 0x1000u, first, 4u);
            sif0WriteWords(memory, 0x1010u, second, 4u);
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, true, false, 0x11110000u, 0x22220000u);
            sif0WriteTag(memory, 0x2010u, 0x1010u, 4u, false, true, 0x33330000u, 0x44440000u);
            sif0SetInterrupts(memory, kSif0Dicr2TagIrq); // mask 18 CLEAR, tag bit 9 set
            sif0Start(memory, fifo, 0x2000u);
            memory.driveSif0();
            if (!expect(!memory.sif0Active() && memory.read32(kSif0Tadr) == 0x2020u, "sif0 tag-IRQ: two-tag chain did not run to EOT") ||
                !expect(fifo.count() == 4u, "sif0 tag-IRQ: FIFO did not receive 2 headers + 2 payload QWs") ||
                !expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 tag-IRQ: flag 26 was not set by the tag IRQ with mask 18 clear") ||
                !expect((memory.read32(kSif0IStat) & (1u << 3u)) != 0u, "sif0 tag-IRQ: I_STAT bit 3 was not set by the tag IRQ with mask 18 clear") ||
                !expect((memory.read32(kSif0Dicr) & (1u << 31u)) != 0u, "sif0 tag-IRQ: DICR read did not show derived bit 31 set"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            const uint32_t payload[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            sif0WriteWords(memory, 0x1000u, payload, 4u);
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, true, true, 0x11110000u, 0x22220000u);
            sif0SetInterrupts(memory, kSif0Dicr2Mask | kSif0Dicr2TagIrq);
            sif0Start(memory, fifo, 0x2000u);
            memory.driveSif0();
            if (!expect(!memory.sif0Active(), "sif0 tag-IRQ+EOT: engine is still active after EOT") ||
                !expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) == kSif0Dicr2Flag, "sif0 tag-IRQ+EOT: flag 26 is not set exactly once (a bit, not a count)") ||
                !expect((memory.read32(kSif0IStat) & (1u << 3u)) != 0u, "sif0 tag-IRQ+EOT: I_STAT bit 3 was not set"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            const uint32_t payload[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            sif0WriteWords(memory, 0x1000u, payload, 4u);
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0x11110000u, 0x22220000u);
            sif0SetInterrupts(memory, kSif0Dicr2Mask | kSif0Dicr2TagIrq);
            memory.write32(kSif0Dicr, 0u); // master bit 23 clear
            sif0Start(memory, fifo, 0x2000u);
            memory.driveSif0();
            if (!expect(!memory.sif0Active() && memory.read32(kSif0Tadr) == 0x2010u, "sif0 master-clear: transfer did not complete without the master enable") ||
                !expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) == 0u, "sif0 master-clear: DICR2 flag 26 was set without DICR.23") ||
                !expect((memory.read32(kSif0IStat) & (1u << 3u)) == 0u, "sif0 master-clear: I_STAT bit 3 was set without DICR.23"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            const uint32_t payload[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            sif0WriteWords(memory, 0x1000u, payload, 4u);
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0x11110000u, 0x22220000u);
            sif0SetInterrupts(memory, kSif0Dicr2Mask);
            memory.write32(kSif0IMask, 0u); // I_MASK bit 3 clear: status is not mask-gated, delivery is
            sif0Start(memory, fifo, 0x2000u);
            memory.driveSif0();
            const uint32_t istat = memory.read32(kSif0IStat);
            const uint32_t imask = memory.read32(kSif0IMask);
            return expect((istat & (1u << 3u)) != 0u, "sif0 mask-gated: I_STAT bit 3 was not set") &&
                   expect((istat & imask) == 0u, "sif0 mask-gated: (I_STAT & I_MASK) is nonzero with I_MASK bit 3 clear") &&
                   expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 mask-gated: DICR2 flag 26 was not set");
        }
    }

    // Case 3: acknowledgement semantics, both orders, with the cross-independence check
    // at each clear step. DICR2 flag 26 is write-1-to-clear (a write with bit 26 = 0
    // leaves it); I_STAT bit 3 is write-0-to-clear on the whole register (a write with
    // bit 3 = 1 leaves it, a write with bit 3 = 0 clears only that bit here).
    // Guest DICR2 writes store the mask/tag bits as written, so flag-clear writes
    // re-include the mask bit to preserve it.
    bool testSif0IopAcknowledgement()
    {
        const auto completeTransfer = [](IopMemory &memory, Sif0Fifo &fifo)
        {
            const uint32_t payload[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            sif0WriteWords(memory, 0x1000u, payload, 4u);
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0x11110000u, 0x22220000u);
            sif0SetInterrupts(memory, kSif0Dicr2Mask);
            sif0Start(memory, fifo, 0x2000u);
            memory.driveSif0();
        };
        {
            IopMemory memory;
            Sif0Fifo fifo;
            completeTransfer(memory, fifo);
            if (!expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u && (memory.read32(kSif0IStat) & (1u << 3u)) != 0u,
                        "sif0 ack (flag first): completion did not set both flag 26 and I_STAT bit 3"))
                return false;
            memory.write32(kSif0Dicr2, kSif0Dicr2Mask); // write-0 on flag 26: leaves it
            if (!expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 ack (flag first): DICR2 write-0 cleared flag 26"))
                return false;
            memory.write32(kSif0IStat, 0xFFFFFFFFu); // write-1 on bit 3: leaves it
            if (!expect((memory.read32(kSif0IStat) & (1u << 3u)) != 0u, "sif0 ack (flag first): I_STAT write-1 cleared bit 3"))
                return false;
            memory.write32(kSif0Dicr2, kSif0Dicr2Mask | kSif0Dicr2Flag); // write-1-to-clear flag 26
            if (!expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) == 0u, "sif0 ack (flag first): DICR2 write-1 did not clear flag 26") ||
                !expect((memory.read32(kSif0IStat) & (1u << 3u)) != 0u, "sif0 ack (flag first): clearing flag 26 also cleared I_STAT bit 3"))
                return false;
            memory.write32(kSif0IStat, 0xFFFFFFF7u); // write-0-to-clear bit 3
            if (!expect((memory.read32(kSif0IStat) & (1u << 3u)) == 0u, "sif0 ack (flag first): I_STAT write-0 did not clear bit 3"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            completeTransfer(memory, fifo);
            if (!expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u && (memory.read32(kSif0IStat) & (1u << 3u)) != 0u,
                        "sif0 ack (I_STAT first): completion did not set both flag 26 and I_STAT bit 3"))
                return false;
            memory.write32(kSif0Dicr2, kSif0Dicr2Mask); // write-0 on flag 26: leaves it
            if (!expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 ack (I_STAT first): DICR2 write-0 cleared flag 26"))
                return false;
            memory.write32(kSif0IStat, 0xFFFFFFFFu); // write-1 on bit 3: leaves it
            if (!expect((memory.read32(kSif0IStat) & (1u << 3u)) != 0u, "sif0 ack (I_STAT first): I_STAT write-1 cleared bit 3"))
                return false;
            memory.write32(kSif0IStat, 0xFFFFFFF7u); // write-0-to-clear bit 3 first this time
            if (!expect((memory.read32(kSif0IStat) & (1u << 3u)) == 0u, "sif0 ack (I_STAT first): I_STAT write-0 did not clear bit 3") ||
                !expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 ack (I_STAT first): clearing I_STAT bit 3 also cleared flag 26"))
                return false;
            memory.write32(kSif0Dicr2, kSif0Dicr2Mask | kSif0Dicr2Flag); // write-1-to-clear flag 26
            return expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) == 0u, "sif0 ack (I_STAT first): DICR2 write-1 did not clear flag 26");
        }
    }

    // Case 4: CHCR bit 24 survives repeated reads while paused-on-full, and clears on EOT.
    // No read-clear behaviour for bit 24. The stall setup is the case-5 transfer.
    bool testSif0IopChcrHeldOnFull()
    {
        IopMemory memory;
        Sif0Fifo fifo;
        uint32_t payload[40] = {};
        for (uint32_t i = 0u; i < 40u; ++i)
            payload[i] = 0x10000000u + i;
        sif0WriteWords(memory, 0x1000u, payload, 40u);
        sif0WriteTag(memory, 0x2000u, 0x1000u, 40u, false, true, 0x11110000u, 0x22220000u);
        sif0SetInterrupts(memory, kSif0Dicr2Mask);
        sif0Start(memory, fifo, 0x2000u);
        memory.driveSif0();
        bool ok = expect(memory.sif0PausedOnFull(), "sif0 held: engine did not pause on full") &&
                  expect(fifo.count() == 8u, "sif0 held: FIFO did not fill to 8 slots");
        for (int i = 0; i < 3; ++i)
            ok = ok && expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) != 0u, "sif0 held: a repeated CHCR read cleared bit 24 while paused");
        if (!ok)
            return false;
        Sif0Fifo::Header header{};
        Sif0Fifo::Qword qw{};
        if (!expect(fifo.tryPopHeader(header), "sif0 held: could not drain the header"))
            return false;
        for (int i = 0; i < 7; ++i)
        {
            if (!expect(fifo.tryPopQw(qw), "sif0 held: could not drain a stalled payload QW"))
                return false;
        }
        memory.driveSif0();
        ok = expect(!memory.sif0Active(), "sif0 held: engine is still active after the resumed drive completed") &&
             expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) == 0u, "sif0 held: CHCR bit 24 did not clear on EOT");
        for (int i = 0; i < 2; ++i)
            ok = ok && expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) == 0u, "sif0 held: CHCR bit 24 reappeared after EOT");
        return ok;
    }

    // Cases 5 + 7: FIFO stall/resume with the 8-slot model capacity, plus in-flight
    // DmaStat (model encoding 1/0/-1). A single 10-QW tag cannot fit header + 10 QWs
    // in 8 slots, so the first drive pauses with header + 7 QWs queued. The EE drain
    // must pop the header first: tryPopQw on a header-front slot returns false
    // without consuming, so a drain that only pops QWs can never free the FIFO.
    // After the drain the resumed drive completes byte-exact: header + 10 QWs in order.
    bool testSif0IopFifoStallResume()
    {
        IopMemory memory;
        Sif0Fifo fifo;
        uint32_t payload[40] = {};
        for (uint32_t i = 0u; i < 40u; ++i)
            payload[i] = 0x20000000u + i;
        sif0WriteWords(memory, 0x1000u, payload, 40u);
        sif0WriteTag(memory, 0x2000u, 0x1000u, 40u, false, true, 0x11110000u, 0x22220000u);
        sif0SetInterrupts(memory, kSif0Dicr2Mask);
        sif0Start(memory, fifo, 0x2000u);
        const uint32_t id = memory.sif0CurrentTransferId();
        memory.driveSif0();
        if (!expect(memory.sif0PausedOnFull(), "sif0 stall: engine did not pause on full") ||
            !expect(fifo.count() == 8u, "sif0 stall: FIFO count is not 8 at the stall point") ||
            !expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) != 0u, "sif0 stall: CHCR bit 24 cleared while stalled") ||
            !expect(memory.read32(kSif0Tadr) == 0x2000u, "sif0 stall: TADR advanced before the tag was fully pushed") ||
            !expect(memory.sif0DmaStat(id) == 0, "sif0 stall: DmaStat of the in-flight id is not 0") ||
            !expect(memory.sif0DmaStat(9999u) == -1, "sif0 stall: DmaStat of unknown id 9999 is not -1") ||
            !expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) == 0u, "sif0 stall: DICR2 flag 26 was set before completion") ||
            !expect((memory.read32(kSif0IStat) & (1u << 3u)) == 0u, "sif0 stall: I_STAT bit 3 was set before completion"))
            return false;
        Sif0Fifo::Header header{};
        Sif0Fifo::Qword qw{};
        if (!expect(fifo.tryPopHeader(header) && header.lo == 0x11110000u && header.hi == 0x22220000u,
                    "sif0 stall: drained header differs from the tag's extra words"))
            return false;
        for (uint32_t q = 0u; q < 7u; ++q)
        {
            if (!expect(fifo.tryPopQw(qw), "sif0 stall: could not drain a stalled payload QW") ||
                !expect(sif0QwEquals(qw, payload[q * 4u], payload[q * 4u + 1u], payload[q * 4u + 2u], payload[q * 4u + 3u]),
                        "sif0 stall: a drained payload QW is out of order or corrupt"))
                return false;
        }
        if (!expect(fifo.isEmpty(), "sif0 stall: FIFO was not fully drained"))
            return false;
        memory.driveSif0();
        if (!expect(!memory.sif0Active(), "sif0 resume: engine is still active after the resumed drive") ||
            !expect(memory.read32(kSif0Tadr) == 0x2010u, "sif0 resume: TADR did not advance by 4 words") ||
            !expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) == 0u, "sif0 resume: CHCR bit 24 did not clear on EOT") ||
            !expect(memory.sif0DmaStat(id) == 1, "sif0 resume: DmaStat of the completed id is not 1") ||
            !expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 resume: DICR2 flag 26 was not set on completion") ||
            !expect((memory.read32(kSif0IStat) & (1u << 3u)) != 0u, "sif0 resume: I_STAT bit 3 was not set on completion"))
            return false;
        for (uint32_t q = 7u; q < 10u; ++q)
        {
            if (!expect(fifo.tryPopQw(qw), "sif0 resume: could not drain a resumed payload QW") ||
                !expect(sif0QwEquals(qw, payload[q * 4u], payload[q * 4u + 1u], payload[q * 4u + 2u], payload[q * 4u + 3u]),
                        "sif0 resume: a resumed payload QW is out of order or corrupt"))
                return false;
        }
        return expect(fifo.isEmpty(), "sif0 resume: FIFO holds units past header + 10 payload QWs");
    }

    // Case 6: header-blocked start. The header occupies one FIFO slot (model choice),
    // so a full FIFO blocks the header push: nothing moves, TADR is unchanged, bit 24
    // stays set. Popping one QW lets the header push; draining the prefilled QWs then
    // exposes the header, and a further drive completes the tag.
    bool testSif0IopHeaderBlockedStart()
    {
        IopMemory memory;
        Sif0Fifo fifo;
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            if (!expect(fifo.tryPushQw(sif0MakeQw(0xF0000000u + i, 0xF0000001u + i, 0xF0000002u + i, 0xF0000003u + i)),
                        "sif0 header-blocked: could not prefill the FIFO"))
                return false;
        }
        const uint32_t payload[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        sif0WriteWords(memory, 0x1000u, payload, 4u);
        sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0xDEADBEEFu, 0xCAFEBABEu);
        sif0Start(memory, fifo, 0x2000u);
        memory.driveSif0();
        if (!expect(fifo.count() == 8u, "sif0 header-blocked: FIFO count changed while the header was blocked") ||
            !expect(memory.sif0PausedOnFull(), "sif0 header-blocked: engine did not pause on the blocked header") ||
            !expect(memory.read32(kSif0Tadr) == 0x2000u, "sif0 header-blocked: TADR advanced while the header was blocked") ||
            !expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) != 0u, "sif0 header-blocked: CHCR bit 24 cleared while blocked") ||
            !expect(memory.sif0Active(), "sif0 header-blocked: engine is not active while blocked"))
            return false;
        Sif0Fifo::Qword qw{};
        if (!expect(fifo.tryPopQw(qw), "sif0 header-blocked: could not pop one prefilled QW") ||
            !expect(sif0QwEquals(qw, 0xF0000000u, 0xF0000001u, 0xF0000002u, 0xF0000003u), "sif0 header-blocked: the popped QW is not the first prefilled QW"))
            return false;
        memory.driveSif0();
        if (!expect(fifo.count() == 8u, "sif0 header-blocked: header did not refill the FIFO after one pop") ||
            !expect(memory.sif0PausedOnFull(), "sif0 header-blocked: engine did not pause on full after pushing the header") ||
            !expect(memory.read32(kSif0Tadr) == 0x2000u, "sif0 header-blocked: TADR advanced before the tag was fully pushed"))
            return false;
        for (uint32_t i = 1u; i < 8u; ++i)
        {
            if (!expect(fifo.tryPopQw(qw), "sif0 header-blocked: could not drain a prefilled QW") ||
                !expect(sif0QwEquals(qw, 0xF0000000u + i, 0xF0000001u + i, 0xF0000002u + i, 0xF0000003u + i),
                        "sif0 header-blocked: a prefilled QW moved out of order"))
                return false;
        }
        Sif0Fifo::Header header{};
        if (!expect(fifo.tryPopHeader(header) && header.lo == 0xDEADBEEFu && header.hi == 0xCAFEBABEu,
                    "sif0 header-blocked: the header was not behind the prefilled QWs after one pop"))
            return false;
        memory.driveSif0();
        if (!expect(!memory.sif0Active(), "sif0 header-blocked: engine is still active after the final drive") ||
            !expect(memory.read32(kSif0Tadr) == 0x2010u, "sif0 header-blocked: TADR did not advance after completion") ||
            !expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) == 0u, "sif0 header-blocked: CHCR bit 24 did not clear on EOT") ||
            !expect(fifo.tryPopQw(qw), "sif0 header-blocked: final payload QW was not pushed") ||
            !expect(sif0QwEquals(qw, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u), "sif0 header-blocked: final payload QW is corrupt"))
            return false;
        return expect(fifo.isEmpty(), "sif0 header-blocked: FIFO holds units after the transfer");
    }

    // Case 8: bounds/overflow holds. A TADR outside IOP RAM holds the transfer with
    // bit 24 set and no completion or flag; a source range whose rounded size runs
    // past 2 MB holds before any header or payload is pushed for that tag.
    bool testSif0IopBoundsHold()
    {
        {
            IopMemory memory;
            Sif0Fifo fifo;
            sif0SetInterrupts(memory, kSif0Dicr2Mask);
            sif0Start(memory, fifo, 0x00200000u); // one past the 2 MB IOP RAM
            const uint32_t id = memory.sif0CurrentTransferId();
            memory.driveSif0();
            if (!expect(memory.sif0Held(), "sif0 bounds (TADR): engine did not hold on an out-of-RAM tag read") ||
                !expect(memory.sif0Active(), "sif0 bounds (TADR): engine is not active while held") ||
                !expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) != 0u, "sif0 bounds (TADR): CHCR bit 24 cleared while held") ||
                !expect(fifo.isEmpty(), "sif0 bounds (TADR): FIFO received units from an out-of-RAM tag") ||
                !expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) == 0u, "sif0 bounds (TADR): DICR2 flag 26 was set on a held transfer") ||
                !expect((memory.read32(kSif0IStat) & (1u << 3u)) == 0u, "sif0 bounds (TADR): I_STAT bit 3 was set on a held transfer") ||
                !expect(memory.sif0DmaStat(id) == 0, "sif0 bounds (TADR): DmaStat of the held id is not 0"))
                return false;
            memory.driveSif0();
            if (!expect(memory.sif0Held() && memory.sif0Active(), "sif0 bounds (TADR): a second drive did not stay held"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            // Rounded source range 0x001FFFF0 + 32 bytes runs past 2 MB; the tag itself is in RAM.
            sif0WriteTag(memory, 0x2000u, 0x001FFFF0u, 8u, false, true, 0x11110000u, 0x22220000u);
            sif0SetInterrupts(memory, kSif0Dicr2Mask);
            sif0Start(memory, fifo, 0x2000u);
            memory.driveSif0();
            return expect(memory.sif0Held(), "sif0 bounds (source): engine did not hold on an out-of-RAM source range") &&
                   expect(memory.sif0Active(), "sif0 bounds (source): engine is not active while held") &&
                   expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) != 0u, "sif0 bounds (source): CHCR bit 24 cleared while held") &&
                   expect(memory.read32(kSif0Tadr) == 0x2000u, "sif0 bounds (source): TADR advanced on a held tag") &&
                   expect(fifo.isEmpty(), "sif0 bounds (source): a header or payload was pushed for the out-of-range tag") &&
                   expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) == 0u, "sif0 bounds (source): DICR2 flag 26 was set on a held transfer") &&
                   expect((memory.read32(kSif0IStat) & (1u << 3u)) == 0u, "sif0 bounds (source): I_STAT bit 3 was set on a held transfer");
        }
    }

    // Case 9, CORRECTED RULE: the design §5.2 zero-fill partial-QW rule is replaced by
    // IOP-side rounding — the engine rounds the tag word count up to a QW boundary and
    // reads the padding words from IOP RAM. A 6-word tag therefore pushes 2 whole QWs,
    // and the second QW's last 2 words equal the following IOP RAM words, never zeros.
    bool testSif0IopPartialQwPaddingFromRam()
    {
        IopMemory memory;
        Sif0Fifo fifo;
        const uint32_t words[8] = {0xA0000000u, 0xA0000001u, 0xA0000002u, 0xA0000003u,
                                   0xA0000004u, 0xA0000005u, 0xDEADBEEFu, 0xCAFEBABEu};
        sif0WriteWords(memory, 0x1000u, words, 8u);
        sif0WriteTag(memory, 0x2000u, 0x1000u, 6u, false, true, 0x11110000u, 0x22220000u);
        sif0SetInterrupts(memory, kSif0Dicr2Mask);
        sif0Start(memory, fifo, 0x2000u);
        memory.driveSif0();
        Sif0Fifo::Header header{};
        Sif0Fifo::Qword qw0{}, qw1{};
        if (!expect(fifo.tryPopHeader(header), "sif0 partial-QW: header was not pushed") ||
            !expect(fifo.tryPopQw(qw0), "sif0 partial-QW: first QW was not pushed") ||
            !expect(sif0QwEquals(qw0, 0xA0000000u, 0xA0000001u, 0xA0000002u, 0xA0000003u), "sif0 partial-QW: first QW differs from IOP RAM words 0-3") ||
            !expect(fifo.tryPopQw(qw1), "sif0 partial-QW: second (rounded) QW was not pushed") ||
            !expect(sif0QwEquals(qw1, 0xA0000004u, 0xA0000005u, 0xDEADBEEFu, 0xCAFEBABEu),
                    "sif0 partial-QW: second QW's padding words are not the following IOP RAM words (zero-fill would give 0 here)"))
            return false;
        return expect(fifo.isEmpty(), "sif0 partial-QW: FIFO holds units past header + 2 QWs") &&
               expect(!memory.sif0Active(), "sif0 partial-QW: engine is still active after the rounded tag") &&
               expect(memory.read32(kSif0Tadr) == 0x2010u, "sif0 partial-QW: TADR did not advance by 4 words") &&
               expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) == 0u, "sif0 partial-QW: CHCR bit 24 did not clear on EOT");
    }

    // Case 10: bits 62/63 of the 64-bit tag (word1 bits 30/31) do NOT complete. The tag
    // has neither IRQ nor EOT in word0, so it must be consumed as an ordinary tag and
    // the chain must continue: the tag sits at the last 16 bytes of IOP RAM, so the
    // next tag read is out of RAM and the engine holds — still active, bit 24 set,
    // no flag, no I_STAT bit 3. If bits 62/63 were misread as EOT, the engine would
    // have completed at the first tag instead of holding on the second read.
    bool testSif0IopTagBits62And63DoNotComplete()
    {
        IopMemory memory;
        Sif0Fifo fifo;
        const uint32_t payload[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        sif0WriteWords(memory, 0x1000u, payload, 4u);
        const uint32_t tag[4] = {0x1000u, 4u | (1u << 30u) | (1u << 31u), 0x11110000u, 0x22220000u};
        sif0WriteWords(memory, 0x001FFFF0u, tag, 4u);
        sif0SetInterrupts(memory, kSif0Dicr2Mask);
        sif0Start(memory, fifo, 0x001FFFF0u);
        memory.driveSif0();
        return expect(memory.sif0Active(), "sif0 bits 62/63: engine completed on word1 bits 30/31") &&
               expect(memory.sif0Held(), "sif0 bits 62/63: engine did not continue to the next tag") &&
               expect(memory.read32(kSif0Tadr) == 0x00200000u, "sif0 bits 62/63: TADR did not advance past the non-EOT tag") &&
               expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) != 0u, "sif0 bits 62/63: CHCR bit 24 cleared without EOT") &&
               expect((memory.read32(kSif0Dicr2) & kSif0Dicr2Flag) == 0u, "sif0 bits 62/63: DICR2 flag 26 was set without EOT") &&
               expect((memory.read32(kSif0IStat) & (1u << 3u)) == 0u, "sif0 bits 62/63: I_STAT bit 3 was set without EOT") &&
               expect(fifo.count() == 2u, "sif0 bits 62/63: FIFO did not receive header + 1 payload QW for the tag") &&
               expect(memory.sif0DmaStat(memory.sif0CurrentTransferId()) == 0, "sif0 bits 62/63: DmaStat reports complete without EOT");
    }

    // Case 11: start gating. DPCR2 = 0, DMACEN = 0, burst (non-chain) mode, and
    // transportOwnsSif0 = false each block the start (CHCR is stored but the engine
    // never starts and the FIFO stays empty); DPCR2 enable set starts it.
    bool testSif0IopStartGating()
    {
        {
            IopMemory memory;
            Sif0Fifo fifo;
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0x11110000u, 0x22220000u);
            sif0ArmManual(memory, fifo, true, 1u, 0u, 0x2000u, kSif0ChcrChain); // DPCR2 ch9 enable clear
            memory.driveSif0();
            if (!expect(!memory.sif0Active(), "sif0 gating (DPCR2=0): engine started with the channel-9 enable clear") ||
                !expect(fifo.isEmpty(), "sif0 gating (DPCR2=0): FIFO received units without a start"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            const uint32_t payload[8] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                         0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};
            sif0WriteWords(memory, 0x1000u, payload, 8u);
            sif0WriteTag(memory, 0x2000u, 0x1000u, 8u, false, true, 0x11110000u, 0x22220000u);
            sif0ArmManual(memory, fifo, true, 1u, kSif0Dpcr2Ch9Enable, 0x2000u, kSif0ChcrChain);
            if (!expect(memory.sif0Active(), "sif0 gating (DPCR2 set): engine did not start with the channel-9 enable set"))
                return false;
            memory.driveSif0();
            if (!expect(fifo.count() == 3u, "sif0 gating (DPCR2 set): FIFO did not receive header + 2 payload QWs after the start") ||
                !expect(!memory.sif0Active(), "sif0 gating (DPCR2 set): engine did not complete after the start"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0x11110000u, 0x22220000u);
            sif0ArmManual(memory, fifo, true, 0u, kSif0Dpcr2Ch9Enable, 0x2000u, kSif0ChcrChain); // DMACEN = 0
            memory.driveSif0();
            if (!expect(!memory.sif0Active(), "sif0 gating (DMACEN=0): engine started with DMACEN clear") ||
                !expect(fifo.isEmpty(), "sif0 gating (DMACEN=0): FIFO received units without a start"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0x11110000u, 0x22220000u);
            sif0ArmManual(memory, fifo, true, 1u, kSif0Dpcr2Ch9Enable, 0x2000u, kSif0ChcrBurst); // mode != chain
            memory.driveSif0();
            if (!expect(!memory.sif0Active(), "sif0 gating (burst mode): engine started in non-chain mode") ||
                !expect(fifo.isEmpty(), "sif0 gating (burst mode): FIFO received units without a start"))
                return false;
        }
        {
            IopMemory memory;
            Sif0Fifo fifo;
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0x11110000u, 0x22220000u);
            sif0ArmManual(memory, fifo, false, 1u, kSif0Dpcr2Ch9Enable, 0x2000u, kSif0ChcrChain); // flag false
            memory.driveSif0();
            return expect(!memory.sif0Active(), "sif0 gating (flag false): engine started without transport ownership") &&
                   expect(fifo.isEmpty(), "sif0 gating (flag false): FIFO received units without a start") &&
                   expect((memory.read32(kSif0Chcr) & kSif0ChcrStart) != 0u, "sif0 gating (flag false): CHCR STR was not stored as written");
        }
    }

    // Case 12: scheduler integration via IopEmulator. No direct driveSif0() call is
    // made anywhere in this test: runEeCycles() drives scheduleStep, which services
    // SIF0 at step boundaries. First a small transfer completes; then a stalling
    // transfer pauses on full under the scheduler and completes on a later
    // runEeCycles() after the test drains the FIFO, with no register write in between.
    // (The exact idle nextWake value is internal to the scheduler, so the observable
    // proxy asserted here is completion after runEeCycles with no thread or register
    // action of the test's own; see the report for the nested-call blind spot.)
    bool testSif0IopSchedulerIntegration()
    {
        {
            NullHost host;
            IopEmulator emu(host);
            Sif0Fifo fifo;
            emu.setSif0Transport(&fifo);
            emu.setTransportOwnsSif0(true);
            const uint32_t payload[8] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                         0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};
            const uint32_t tag[4] = {0x1000u | (1u << 31u), 8u, 0xAABBCCDDu, 0x11223344u};
            if (!expect(emu.writeMemory(0x1000u, payload, sizeof(payload)), "sif0 scheduler: could not write the payload") ||
                !expect(emu.writeMemory(0x2000u, tag, sizeof(tag)), "sif0 scheduler: could not write the chain tag"))
                return false;
            emu.writeIopRegister(kSif0DmacEn, 1u);
            emu.writeIopRegister(kSif0Dicr, kSif0DicrMaster);
            emu.writeIopRegister(kSif0Dicr2, kSif0Dicr2Mask);
            emu.writeIopRegister(kSif0IMask, 1u << 3u);
            emu.writeIopRegister(kSif0ICtrl, 1u);
            emu.writeIopRegister(kSif0Tadr, 0x2000u);
            emu.writeIopRegister(kSif0Chcr, kSif0ChcrChain);
            emu.runEeCycles(8000u);
            Sif0Fifo::Header header{};
            Sif0Fifo::Qword qw0{}, qw1{};
            if (!expect(fifo.tryPopHeader(header) && header.lo == 0xAABBCCDDu && header.hi == 0x11223344u,
                        "sif0 scheduler: FIFO did not fill via scheduleStep") ||
                !expect(fifo.tryPopQw(qw0) && sif0QwEquals(qw0, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u),
                        "sif0 scheduler: first payload QW is missing or corrupt") ||
                !expect(fifo.tryPopQw(qw1) && sif0QwEquals(qw1, 0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u),
                        "sif0 scheduler: second payload QW is missing or corrupt") ||
                !expect(fifo.isEmpty(), "sif0 scheduler: FIFO holds units past header + 2 payload QWs"))
                return false;
            if (!expect((emu.readIopRegister(kSif0Chcr) & kSif0ChcrStart) == 0u, "sif0 scheduler: CHCR bit 24 did not clear on EOT") ||
                !expect((emu.readIopRegister(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 scheduler: DICR2 flag 26 was not set") ||
                !expect((emu.readIopRegister(kSif0IStat) & (1u << 3u)) != 0u, "sif0 scheduler: I_STAT bit 3 was not set"))
                return false;
        }
        {
            NullHost host;
            IopEmulator emu(host);
            Sif0Fifo fifo;
            emu.setSif0Transport(&fifo);
            emu.setTransportOwnsSif0(true);
            uint32_t payload[40] = {};
            for (uint32_t i = 0u; i < 40u; ++i)
                payload[i] = 0x30000000u + i;
            const uint32_t tag[4] = {0x1000u | (1u << 31u), 40u, 0x11110000u, 0x22220000u};
            if (!expect(emu.writeMemory(0x1000u, payload, sizeof(payload)), "sif0 scheduler stall: could not write the payload") ||
                !expect(emu.writeMemory(0x2000u, tag, sizeof(tag)), "sif0 scheduler stall: could not write the chain tag"))
                return false;
            emu.writeIopRegister(kSif0DmacEn, 1u);
            emu.writeIopRegister(kSif0Dicr, kSif0DicrMaster);
            emu.writeIopRegister(kSif0Dicr2, kSif0Dicr2Mask);
            emu.writeIopRegister(kSif0IMask, 1u << 3u);
            emu.writeIopRegister(kSif0ICtrl, 1u);
            emu.writeIopRegister(kSif0Tadr, 0x2000u);
            emu.writeIopRegister(kSif0Chcr, kSif0ChcrChain);
            emu.runEeCycles(8000u);
            if (!expect(fifo.count() == 8u, "sif0 scheduler stall: FIFO did not fill to 8 slots under the scheduler") ||
                !expect((emu.readIopRegister(kSif0Chcr) & kSif0ChcrStart) != 0u, "sif0 scheduler stall: CHCR bit 24 cleared while stalled") ||
                !expect((emu.readIopRegister(kSif0Dicr2) & kSif0Dicr2Flag) == 0u, "sif0 scheduler stall: DICR2 flag 26 was set before completion"))
                return false;
            Sif0Fifo::Header header{};
            Sif0Fifo::Qword qw{};
            if (!expect(fifo.tryPopHeader(header), "sif0 scheduler stall: could not drain the header"))
                return false;
            for (uint32_t q = 0u; q < 7u; ++q)
            {
                if (!expect(fifo.tryPopQw(qw), "sif0 scheduler stall: could not drain a stalled payload QW") ||
                    !expect(sif0QwEquals(qw, payload[q * 4u], payload[q * 4u + 1u], payload[q * 4u + 2u], payload[q * 4u + 3u]),
                            "sif0 scheduler stall: a drained payload QW is out of order or corrupt"))
                    return false;
            }
            emu.runEeCycles(8000u); // no driveSif0 call, no register write: the scheduler re-drives it
            if (!expect((emu.readIopRegister(kSif0Chcr) & kSif0ChcrStart) == 0u, "sif0 scheduler stall: CHCR bit 24 did not clear after the resumed scheduler run") ||
                !expect((emu.readIopRegister(kSif0Dicr2) & kSif0Dicr2Flag) != 0u, "sif0 scheduler stall: DICR2 flag 26 was not set after the resumed scheduler run") ||
                !expect((emu.readIopRegister(kSif0IStat) & (1u << 3u)) != 0u, "sif0 scheduler stall: I_STAT bit 3 was not set after the resumed scheduler run") ||
                !expect(fifo.count() == 3u, "sif0 scheduler stall: FIFO did not receive the remaining 3 payload QWs"))
                return false;
            for (uint32_t q = 7u; q < 10u; ++q)
            {
                if (!expect(fifo.tryPopQw(qw), "sif0 scheduler stall: could not drain a resumed payload QW") ||
                    !expect(sif0QwEquals(qw, payload[q * 4u], payload[q * 4u + 1u], payload[q * 4u + 2u], payload[q * 4u + 3u]),
                            "sif0 scheduler stall: a resumed payload QW is out of order or corrupt"))
                    return false;
            }
            return expect(fifo.isEmpty(), "sif0 scheduler stall: FIFO holds units after the transfer");
        }
    }

    // Case 13: HLE gating via IopRpcBridge (NullHost subclass records host writes so the
    // flag-false copy is observable; the stock NullHost's writeGuest always fails, which
    // would make the flag-false path return 0 for an unrelated reason).
    class Sif0RecordingHost final : public NullHost
    {
    public:
        struct Write
        {
            uint32_t address = 0u;
            std::vector<uint8_t> bytes;
        };

        bool readGuest(uint32_t, void *, size_t) const override { return true; }
        bool writeGuest(uint32_t address, const void *data, size_t size) override
        {
            Write write{};
            write.address = address;
            const auto *bytes = static_cast<const uint8_t *>(data);
            write.bytes.assign(bytes, bytes + size);
            writes.push_back(std::move(write));
            return true;
        }

        std::vector<Write> writes;
    };

    bool testSif0IopHleGating()
    {
        {
            Sif0RecordingHost host;
            IopMemory memory;
            IopKernel kernel(memory);
            kernel.reset();
            IopRpcBridge bridge(host, memory, kernel);
            memory.setTransportOwnsSif0(true);
            const uint32_t srcData[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            sif0WriteWords(memory, 0x1000u, srcData, 4u);
            const uint32_t descriptor[4] = {0x1000u, 0x00200000u, 16u, 0u};
            sif0WriteWords(memory, 0x3000u, descriptor, 4u);
            IopCpuState cpu{};
            cpu.gpr[4] = 0x3000u;
            cpu.gpr[5] = 1u;
            if (!expect(bridge.dispatchSifManImport(7u, cpu), "sifman:7 gated was not handled") ||
                !expect(cpu.gpr[2] == 0u, "sifman:7 with transport ownership must return 0, not a DMA id") ||
                !expect(host.writes.empty(), "sifman:7 with transport ownership copied through the HLE path"))
                return false;
        }
        {
            Sif0RecordingHost host;
            IopMemory memory;
            IopKernel kernel(memory);
            kernel.reset();
            IopRpcBridge bridge(host, memory, kernel);
            memory.setTransportOwnsSif0(false);
            const uint32_t srcData[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            sif0WriteWords(memory, 0x1000u, srcData, 4u);
            const uint32_t descriptor[4] = {0x1000u, 0x00200000u, 16u, 0u};
            sif0WriteWords(memory, 0x3000u, descriptor, 4u);
            IopCpuState cpu{};
            cpu.gpr[4] = 0x3000u;
            cpu.gpr[5] = 1u;
            const uint32_t want[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            if (!expect(bridge.dispatchSifManImport(7u, cpu), "sifman:7 ungated was not handled") ||
                !expect(cpu.gpr[2] == 1u, "sifman:7 without transport ownership did not return the first DMA id") ||
                !expect(host.writes.size() == 1u, "sifman:7 without transport ownership did not copy exactly once") ||
                !expect(host.writes[0].address == 0x00200000u && host.writes[0].bytes.size() == 16u &&
                            std::memcmp(host.writes[0].bytes.data(), want, sizeof(want)) == 0,
                        "sifman:7 without transport ownership copied the wrong destination or bytes"))
                return false;
        }
        {
            NullHost host;
            IopMemory memory;
            Sif0Fifo fifo;
            IopKernel kernel(memory);
            kernel.reset();
            IopRpcBridge bridge(host, memory, kernel);
            const uint32_t payload[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            sif0WriteWords(memory, 0x1000u, payload, 4u);
            sif0WriteTag(memory, 0x2000u, 0x1000u, 4u, false, true, 0x11110000u, 0x22220000u);
            sif0Start(memory, fifo, 0x2000u);
            const uint32_t id = memory.sif0CurrentTransferId();
            IopCpuState cpu{};
            cpu.gpr[4] = id;
            if (!expect(bridge.dispatchSifManImport(8u, cpu), "sifman:8 in-flight was not handled") ||
                !expect(static_cast<int32_t>(cpu.gpr[2]) == 0, "sifman:8 with transport ownership did not return 0 for an in-flight id"))
                return false;
            memory.driveSif0();
            cpu.gpr[4] = id;
            if (!expect(bridge.dispatchSifManImport(8u, cpu), "sifman:8 completed was not handled") ||
                !expect(static_cast<int32_t>(cpu.gpr[2]) == 1, "sifman:8 with transport ownership did not return 1 for a completed id"))
                return false;
            cpu.gpr[4] = 9999u;
            if (!expect(bridge.dispatchSifManImport(8u, cpu), "sifman:8 unknown was not handled") ||
                !expect(static_cast<int32_t>(cpu.gpr[2]) == -1, "sifman:8 with transport ownership did not return -1 for an unknown id"))
                return false;
        }
        {
            NullHost host;
            IopMemory memory;
            IopKernel kernel(memory);
            kernel.reset();
            IopRpcBridge bridge(host, memory, kernel);
            IopCpuState cpu{};
            cpu.gpr[4] = 1u;
            return expect(bridge.dispatchSifManImport(8u, cpu), "sifman:8 ungated was not handled") &&
                   expect(cpu.gpr[2] == 0xFFFFFFFFu, "sifman:8 without transport ownership did not return the legacy 0xFFFFFFFF");
        }
    }
}

int main()
{
    if (!testJalrReadsTargetBeforeLink() || !testKernelThreadChecksMatchRealPs2() || !testKernelWaitsAndEventFlagsMatchRealPs2() ||
        !testMessageBoxOrderAndLinkingMatchRealPs2() || !testVariablePoolSizesMatchRealPs2() || !testStrtokMatchesRealPs2() ||
        !testIntrHandlerRegistrationMatchesRealPs2() || !testSpu2RegistersAndSoundRam() ||
        !testSpu2AdpcmBlockDecoding() || !testSpu2VoiceBlockProgression() || !testSpu2SampleClock() ||
        !testSpu2Adsr() || !testSpu2Pitch() || !testSpu2Mixing() || !testSpu2Reverb() || !testSpu2Adma() ||
        !testSif0IopChainBasicEot() || !testSif0IopTagIrqIndependence() || !testSif0IopAcknowledgement() ||
        !testSif0IopChcrHeldOnFull() || !testSif0IopFifoStallResume() || !testSif0IopHeaderBlockedStart() ||
        !testSif0IopBoundsHold() || !testSif0IopPartialQwPaddingFromRam() || !testSif0IopTagBits62And63DoNotComplete() ||
        !testSif0IopStartGating() || !testSif0IopSchedulerIntegration() || !testSif0IopHleGating() ||
        !testIopFixedAllocationOverflow())
        return 1;
    if (!testLoadcoreRebootLibraryMode() || !testCdvdSpecialControl() || !testCdvdSearchFile() ||
        !testTimrmanPeriodicCallback() || !testSprintfMatchesRealPs2Rows() || !testPrintfStackArgumentsAndVaList())
        return 1;
    std::cout << "ps2xIOP import tests passed\n";
    return 0;
}
