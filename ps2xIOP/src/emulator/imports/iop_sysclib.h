#pragma once

#include <cstdint>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopMemory;

    class IopSysclib
    {
    public:
        explicit IopSysclib(IopMemory &memory) noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);

    private:
        [[nodiscard]] uint32_t tokenize(uint32_t s, uint32_t delimiters, uint32_t &next);

        IopMemory &m_memory;
        uint32_t m_strtokNext = 0u;
    };
}
