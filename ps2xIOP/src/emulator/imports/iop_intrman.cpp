#include "iop_intrman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int32_t kIllegalIntrCode = -101;  // KE_ILLEGAL_INTRCODE
        constexpr int32_t kFoundHandler = -104;     // KE_FOUND_HANDLER
        constexpr int32_t kNotFoundHandler = -105;  // KE_NOTFOUND_HANDLER

        // Interrupt lines with a handler slot: VBLANK (0) to DMA SIO2 out (0x2C), and the software lines 0x3E/0x3F.
        bool validLine(int irq)
        {
            return (irq >= 0 && irq <= 0x2C) || irq == 0x3E || irq == 0x3F;
        }
    }

    IopIntrman::IopIntrman(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopIntrman::reset()
    {
        m_handlers.clear();
        m_enabled.clear();
    }

    bool IopIntrman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const uint32_t a3 = cpu.gpr[7];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 3:
            setV0(0u);
            return true;
        case 4: // RegisterIntrHandler(irq, mode, handler, arg)
        {
            // Real PS2 (ps2autotests kernel/iop/interrupt/register): a second handler on a line is refused with
            // KE_FOUND_HANDLER; a NULL handler leaves the line without one; any mode value is accepted.
            const int irq = static_cast<int>(a0);
            const auto existing = m_handlers.find(irq);
            if (!validLine(irq))
                setV0(static_cast<uint32_t>(kIllegalIntrCode));
            else if (existing != m_handlers.end() && existing->second.function != 0u)
                setV0(static_cast<uint32_t>(kFoundHandler));
            else
            {
                m_handlers[irq] = {a2, a3, cpu.gpr[28]};
                setV0(0u);
            }
            return true;
        }
        case 5: // ReleaseIntrHandler(irq)
        {
            const int irq = static_cast<int>(a0);
            const auto existing = m_handlers.find(irq);
            if (!validLine(irq))
                setV0(static_cast<uint32_t>(kIllegalIntrCode));
            else if (existing == m_handlers.end() || existing->second.function == 0u)
                setV0(static_cast<uint32_t>(kNotFoundHandler));
            else
            {
                m_handlers.erase(existing);
                setV0(0u);
            }
            return true;
        }
        case 6: // EnableIntr
            m_enabled[static_cast<int>(a0)] = true;
            if (a0 < 32u)
                m_memory.setInterruptMask(m_memory.interruptMask() | (1u << a0));
            setV0(0u);
            return true;
        case 7: // DisableIntr
            if (a1 != 0u)
                m_memory.write32(a1, a0);
            if (a0 < 32u)
                m_memory.setInterruptMask(m_memory.interruptMask() & ~(1u << a0));
            m_enabled[static_cast<int>(a0)] = false;
            setV0(0u);
            return true;
        case 8: // CpuDisableIntr
            m_memory.setInterruptControl(0u);
            setV0(0u);
            return true;
        case 9: // CpuEnableIntr
            m_memory.setInterruptControl(1u);
            setV0(0u);
            return true;
        case 14:
            setV0(a0 != 0u
                      ? executor.executeGuestFunctionWithBudget(a0, a1, a2, a3, 0u, cpu.gpr[28], 100000u)
                      : 0u);
            return true;
        case 15:
        case 16:
        case 23:
        case 24:
        case 25:
        case 28:
        case 30:
            setV0(0u);
            return true;
        case 17:
            if (a0 != 0u)
                m_memory.write32(a0, m_memory.interruptControl());
            m_memory.setInterruptControl(0u);
            setV0(0u);
            return true;
        case 18:
            m_memory.setInterruptControl(a0 != 0u ? 1u : 0u);
            setV0(0u);
            return true;
        default:
            return false;
        }
    }

    bool IopIntrman::dispatchInterrupt(int irq, IopGuestExecutor &executor) const
    {
        const auto enabled = m_enabled.find(irq);
        if (enabled == m_enabled.end() || !enabled->second)
            return false;
        const auto handler = m_handlers.find(irq);
        if (handler == m_handlers.end() || handler->second.function == 0u)
            return false;

        (void)executor.executeGuestFunctionWithBudget(handler->second.function,
                                                      handler->second.argument,
                                                      0u,
                                                      0u,
                                                      0u,
                                                      handler->second.gp,
                                                      100000u);
        return true;
    }
}
