// Runs one IOP module (IRX) on the ps2xIOP emulator and writes the module's console output (stdio printf/puts/
// putchar) to stdout and, when PS2X_TTY_LOG names a file, to that file. Used by tools/hwtest/iop_hwtest.py to run the
// ps2autotests IOP programs, whose .expected files were printed by a real PS2; this program never reads them.
//
// usage: ps2_iop_autotest_runner <module.irx>
// exit: 0 when the module loaded and started, 2 when it did not (the load diagnostic goes to stderr).

#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    using namespace ps2x::iop;

    class FileHost final : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &normalized) const override
        {
            normalized = 0u;
            return false;
        }
        uint32_t allocateIopHandle(IopHandleKind) override { return nextHandle += 0x80u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view path) override
        {
            std::ifstream in(std::string(path), std::ios::binary);
            if (!in)
                return 0u;
            file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return 1u;
        }
        bool hostFileSize(uint64_t handle, uint64_t &size) const override
        {
            size = file.size();
            return handle == 1u;
        }
        bool readHostFile(uint64_t handle, uint64_t offset, void *destination, size_t size, size_t &bytesRead) override
        {
            bytesRead = 0u;
            if (handle != 1u || offset > file.size())
                return false;
            bytesRead = std::min(size, file.size() - static_cast<size_t>(offset));
            std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(offset), bytesRead, static_cast<char *>(destination));
            return true;
        }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return -1; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override
        {
            return false;
        }
        void consoleWrite(std::string_view text) override
        {
            tty.append(text);
            std::cout << text;
        }
        void log(LogLevel level, std::string_view message) override
        {
            if (level >= LogLevel::Warning)
                std::cerr << message << '\n';
        }

        std::vector<char> file;
        std::string tty;
        uint32_t nextHandle = 0x1000u;
    };
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: ps2_iop_autotest_runner <module.irx>\n";
        return 64;
    }
    FileHost host;
    IopSubsystem iop(host);
    const ModuleLoadResult result = iop.loadModule(argv[1]);
    std::cout.flush();
    if (const char *ttyPath = std::getenv("PS2X_TTY_LOG"))
    {
        std::ofstream out(ttyPath, std::ios::binary);
        out << host.tty;
    }
    if (!result.handled || result.moduleId < 0)
    {
        // The emulator's load diagnostics went to stderr through log(Warning/Error).
        std::cerr << "module did not load (id " << result.moduleId << ", start result " << result.startResult << ")\n";
        return 2;
    }
    const DebugSnapshot snapshot = iop.debugSnapshot();
    std::cerr << "module id " << result.moduleId << ", start result " << result.startResult << ", "
              << snapshot.emulatorInstructions << " IOP instructions, " << snapshot.emulatorThreads << " threads\n";
    for (const std::string &line : snapshot.diagnostics)
        std::cerr << "diagnostic: " << line << '\n';
    return 0;
}
