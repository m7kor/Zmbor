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
#include "emulator/services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

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

int main()
{
    if (!testJalrReadsTargetBeforeLink() || !testKernelThreadChecksMatchRealPs2() || !testKernelWaitsAndEventFlagsMatchRealPs2() ||
        !testMessageBoxOrderAndLinkingMatchRealPs2() || !testVariablePoolSizesMatchRealPs2() || !testStrtokMatchesRealPs2() ||
        !testIntrHandlerRegistrationMatchesRealPs2() || !testSpu2RegistersAndSoundRam())
        return 1;
    if (!testLoadcoreRebootLibraryMode() || !testCdvdSpecialControl() || !testCdvdSearchFile() ||
        !testTimrmanPeriodicCallback() || !testSprintfMatchesRealPs2Rows() || !testPrintfStackArgumentsAndVaList())
        return 1;
    std::cout << "ps2xIOP import tests passed\n";
    return 0;
}
