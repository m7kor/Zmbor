#include "iop_spu2.h"

#include <algorithm>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kCoreStride = 0x400u;
        constexpr uint32_t kSharedBase = 0x760u;
        constexpr uint32_t kIrqInfo = 0x7C2u;

        constexpr uint32_t kAttr = 0x19Au;
        constexpr uint32_t kIrqaHigh = 0x19Cu;
        constexpr uint32_t kKeyOn = 0x1A0u;
        constexpr uint32_t kKeyOff = 0x1A4u;
        constexpr uint32_t kTsaHigh = 0x1A8u;
        constexpr uint32_t kTsaLow = 0x1AAu;
        constexpr uint32_t kDataPort = 0x1ACu;
        constexpr uint32_t kVoiceAddresses = 0x1C0u; // 24 x (SSA, LSAX, NAX), 12 bytes each
        constexpr uint32_t kVoiceAddressEnd = kVoiceAddresses + 24u * 0xCu;
        constexpr uint32_t kEndxHigh = 0x340u;
        constexpr uint32_t kEndxLow = 0x342u;
        constexpr uint32_t kStat = 0x344u;

        constexpr uint16_t kAttrIrqEnable = 1u << 6;
        constexpr uint16_t kAttrReset = 1u << 15;
        constexpr uint16_t kStatDmaReady = 1u << 7;
        constexpr uint16_t kStatDmaBusy = 1u << 10;
    }

    Spu2::Spu2()
        : m_ram(kRamHalfwords)
    {
        reset();
    }

    void Spu2::reset()
    {
        m_cores = {};
        m_shared = {};
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_irqInfo = 0u;
        m_interrupt = false;
    }

    uint32_t Spu2::address20(const Core &core, uint32_t highReg) const
    {
        return ((static_cast<uint32_t>(core.regs[highReg / 2u]) & 0xFu) << 16) | core.regs[highReg / 2u + 1u];
    }

    // Any sound-RAM access at an enabled core's IRQA raises the SPU2 interrupt (PCSX2 and DobieStation check both
    // cores on every access).
    void Spu2::accessRam(uint32_t address)
    {
        for (int c = 0; c < 2; ++c)
        {
            const Core &core = m_cores[c];
            if ((core.regs[kAttr / 2u] & kAttrIrqEnable) != 0u && address20(core, kIrqaHigh) == address)
            {
                m_irqInfo |= static_cast<uint16_t>(4u << c);
                m_interrupt = true;
            }
        }
    }

    uint16_t Spu2::peek16(uint32_t offset) const
    {
        offset &= kRegisterBytes - 2u;
        if (offset >= kSharedBase)
            return offset == kIrqInfo ? m_irqInfo : m_shared[(offset - kSharedBase) / 2u];
        const Core &core = m_cores[offset >= kCoreStride ? 1 : 0];
        const uint32_t local = offset % kCoreStride;
        switch (local)
        {
        case kKeyOn: return static_cast<uint16_t>(core.keyOn);
        case kKeyOn + 2u: return static_cast<uint16_t>(core.keyOn >> 16);
        case kKeyOff: return static_cast<uint16_t>(core.keyOff);
        case kKeyOff + 2u: return static_cast<uint16_t>(core.keyOff >> 16);
        case kDataPort: return m_ram[core.transfer];
        case kEndxHigh: return static_cast<uint16_t>(core.endx >> 16);
        case kEndxLow: return static_cast<uint16_t>(core.endx);
        case kStat: return core.stat;
        default: return core.regs[local / 2u];
        }
    }

    uint16_t Spu2::read16(uint32_t offset)
    {
        offset &= kRegisterBytes - 2u;
        const uint16_t value = peek16(offset);
        if (offset < kSharedBase && offset % kCoreStride == kDataPort)
        {
            Core &core = m_cores[offset >= kCoreStride ? 1 : 0];
            accessRam(core.transfer);
            core.transfer = (core.transfer + 1u) & (kRamHalfwords - 1u);
        }
        return value;
    }

    void Spu2::write16(uint32_t offset, uint16_t value)
    {
        offset &= kRegisterBytes - 2u;
        if (offset >= kSharedBase)
        {
            if (offset != kIrqInfo) // IRQINFO is status, cleared through ATTR
                m_shared[(offset - kSharedBase) / 2u] = value;
            return;
        }
        const int c = offset >= kCoreStride ? 1 : 0;
        Core &core = m_cores[c];
        const uint32_t local = offset % kCoreStride;
        switch (local)
        {
        case kAttr:
            if ((core.regs[kAttr / 2u] & kAttrIrqEnable) != 0u && (value & kAttrIrqEnable) == 0u)
                m_irqInfo &= static_cast<uint16_t>(~(4u << c));
            if ((value & kAttrReset) != 0u)
                core.stat &= static_cast<uint16_t>(~(kStatDmaReady | kStatDmaBusy));
            core.regs[kAttr / 2u] = static_cast<uint16_t>(value & ~kAttrReset);
            return;
        case kKeyOn:
        case kKeyOn + 2u:
        {
            // KON: the voice restarts from SSA, so NAX takes SSA and its ENDX bit clears (PCSX2, DobieStation).
            const uint32_t voices = local == kKeyOn ? value : (static_cast<uint32_t>(value & 0xFFu) << 16);
            core.keyOn |= voices;
            core.endx &= ~voices;
            for (uint32_t v = 0; v < 24u; ++v)
            {
                if ((voices & (1u << v)) == 0u)
                    continue;
                const uint32_t base = (kVoiceAddresses + v * 0xCu) / 2u;
                core.regs[base + 4u] = core.regs[base];
                core.regs[base + 5u] = core.regs[base + 1u];
            }
            return;
        }
        case kKeyOff:
        case kKeyOff + 2u:
            core.keyOff |= local == kKeyOff ? value : (static_cast<uint32_t>(value & 0xFFu) << 16);
            return;
        case kTsaHigh:
        case kTsaLow:
            core.regs[local / 2u] = local == kTsaHigh ? static_cast<uint16_t>(value & 0xFu) : value;
            core.transfer = address20(core, kTsaHigh);
            return;
        case kDataPort:
            m_ram[core.transfer] = value;
            accessRam(core.transfer);
            core.transfer = (core.transfer + 1u) & (kRamHalfwords - 1u);
            return;
        case kEndxHigh: // as PCSX2 and DobieStation have it: each half's write clears the other half
            core.endx &= 0xFF0000u;
            return;
        case kEndxLow:
            core.endx &= 0xFFFFu;
            return;
        case kStat:
            return; // read-only
        case kIrqaHigh:
            core.regs[local / 2u] = static_cast<uint16_t>(value & 0xFu);
            return;
        default:
            break;
        }
        if (local >= kVoiceAddresses && local < kVoiceAddressEnd && ((local - kVoiceAddresses) % 0xCu) % 4u == 0u)
            value &= 0xFu; // SSA/LSAX/NAX high halves hold address bits 16-19
        core.regs[local / 2u] = value;
    }

    void Spu2::dmaWrite(int core, const uint16_t *data, size_t count)
    {
        Core &c = m_cores[core & 1];
        for (size_t i = 0; i < count; ++i)
        {
            m_ram[c.transfer] = data[i];
            accessRam(c.transfer);
            c.transfer = (c.transfer + 1u) & (kRamHalfwords - 1u);
        }
    }

    void Spu2::dmaRead(int core, uint16_t *data, size_t count)
    {
        Core &c = m_cores[core & 1];
        for (size_t i = 0; i < count; ++i)
        {
            data[i] = m_ram[c.transfer];
            accessRam(c.transfer);
            c.transfer = (c.transfer + 1u) & (kRamHalfwords - 1u);
        }
    }

    void Spu2::completeDma(int core)
    {
        Core &c = m_cores[core & 1];
        c.stat = static_cast<uint16_t>((c.stat | kStatDmaReady) & ~kStatDmaBusy);
    }
}
