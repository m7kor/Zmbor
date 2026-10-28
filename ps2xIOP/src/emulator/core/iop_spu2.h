#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2 register file and sound RAM (IOP 0x1F900000-0x1F9007FF): core 0 at offset 0x000, core 1 at 0x400, and the
    // core volume / SPDIF block at 0x760-0x7FF. Registers are 16 bits wide.
    //
    // Per core: 24 voices of 8 registers (VOLL, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLXL, VOLXR) at 0x000-0x17F; PMON,
    // NON, VMIXL, VMIXEL, VMIXR, VMIXER (pairs of halves), MMIX, ATTR, IRQA, KON, KOFF, TSA, the data port and ADMA at
    // 0x180-0x1B0; 24 voices of SSA, LSAX, NAX (high half: 4 address bits, low half: 16) at 0x1C0-0x2DF; ESA, the
    // reverb registers and EEA at 0x2E0-0x33F; ENDX at 0x340/0x342; STAT at 0x344.
    //
    // Modelled here: read-back of every register; the 20-bit halfword addresses; the data port (writes and reads at
    // TSA, which advances and wraps in the 2 MB RAM); IOP DMA channels 4 and 7 through the same transfer address;
    // IRQA (with ATTR bit 6, any RAM access by either core at a core's IRQA sets IRQINFO bit 2 + core and raises IOP
    // INTC line 9; clearing ATTR bit 6 clears the core's IRQINFO bit); KON (clears the voice's ENDX bit and loads NAX
    // from SSA) and the ENDX write rule; STAT bit 7 after a DMA transfer, cleared by ATTR bit 15.
    //
    // Not modelled: the voice engine (ADPCM decoding, ADSR, pitch, mixing, reverb, ADMA), so ENVX, VOLX and NAX change
    // only as described above, and KON/KOFF stay latched (decision D-SPU-1).
    class Spu2
    {
    public:
        static constexpr uint32_t kRamHalfwords = 0x100000u; // 2 MB
        static constexpr uint32_t kRegisterBytes = 0x800u;

        Spu2();
        void reset();

        // `offset` is the byte offset from 0x1F900000 (even, below 0x800).
        uint16_t read16(uint32_t offset);
        void write16(uint32_t offset, uint16_t value);
        // The register value without side effects (no data-port advance).
        [[nodiscard]] uint16_t peek16(uint32_t offset) const;

        // IOP DMA (channel 4: core 0, channel 7: core 1): halfwords at the core's transfer address, which advances.
        void dmaWrite(int core, const uint16_t *data, size_t count);
        void dmaRead(int core, uint16_t *data, size_t count);
        void completeDma(int core);

        // True once per IRQA hit (IOP INTC line 9).
        bool takeInterrupt() noexcept
        {
            const bool pending = m_interrupt;
            m_interrupt = false;
            return pending;
        }

        [[nodiscard]] uint16_t ram(uint32_t address) const { return m_ram[address & (kRamHalfwords - 1u)]; }
        [[nodiscard]] uint32_t transferAddress(int core) const { return m_cores[core & 1].transfer; }
        [[nodiscard]] uint32_t pendingKeyOn(int core) const { return m_cores[core & 1].keyOn; }
        [[nodiscard]] uint32_t pendingKeyOff(int core) const { return m_cores[core & 1].keyOff; }

    private:
        struct Core
        {
            std::array<uint16_t, 0x200> regs{}; // as written, by halfword index
            uint32_t transfer = 0;              // data-port / DMA address (halfwords)
            uint32_t keyOn = 0;
            uint32_t keyOff = 0;
            uint32_t endx = 0;
            uint16_t stat = 0;
        };

        void accessRam(uint32_t address);
        [[nodiscard]] uint32_t address20(const Core &core, uint32_t highReg) const;

        std::array<Core, 2> m_cores{};
        std::array<uint16_t, 0x50> m_shared{}; // 0x760-0x7FF
        std::vector<uint16_t> m_ram;
        uint16_t m_irqInfo = 0;
        bool m_interrupt = false;
    };
}
