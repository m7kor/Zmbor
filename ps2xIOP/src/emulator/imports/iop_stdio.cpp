#include "iop_stdio.h"

#include "iop_format.h"
#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <string>

namespace ps2x::iop::detail
{
    IopStdio::IopStdio(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopStdio::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        // Every write goes to the host console as the module produced it; printf-family calls return the number of
        // characters written, puts its string plus the newline it appends.
        const auto write = [&](const std::string &text)
        {
            m_host.consoleWrite(text);
            setV0(static_cast<uint32_t>(text.size()));
        };

        switch (ordinal)
        {
        case 4: // printf(format, ...)
            write(formatVariadic(m_memory, cpu, a0, 1u));
            return true;
        case 5: // getchar
        case 10:
            setV0(0xFFFFFFFFu);
            return true;
        case 6: // putchar
            m_host.consoleWrite(std::string(1, static_cast<char>(a0 & 0xFFu)));
            setV0(a0 & 0xFFu);
            return true;
        case 7: // puts(s): the string and a newline
            write(m_memory.readString(a0, 4096u) + "\n");
            return true;
        case 8: // gets
        case 13:
            setV0(0u);
            return true;
        case 9: // fdprintf(fd, format, ...)
            write(formatVariadic(m_memory, cpu, a1, 2u));
            return true;
        case 11:
            setV0(a0 & 0xFFu);
            return true;
        case 12: // fdputs(s, fd)
            write(m_memory.readString(a0, 4096u));
            return true;
        case 14: // vfdprintf(fd, format, ap)
            write(formatVaList(m_memory, a1, cpu.gpr[6]));
            return true;
        default:
            return false;
        }
    }
}
