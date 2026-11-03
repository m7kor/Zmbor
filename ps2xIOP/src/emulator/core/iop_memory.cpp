#include "iop_memory.h"

#include <algorithm>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        // SIF0 DMA channel 9 (derived bases, design §2.2) and the
        // DICR/DICR2/DPCR/DMACEN registers (sourced, design §2.2-2.3).
        constexpr uint32_t kSif0Madr = 0x1F801520u;
        constexpr uint32_t kSif0Bcr = 0x1F801524u;
        constexpr uint32_t kSif0Chcr = 0x1F801528u;
        constexpr uint32_t kSif0Tadr = 0x1F80152Cu;
        constexpr uint32_t kDpcr = 0x1F8010F0u;
        constexpr uint32_t kDicr = 0x1F8010F4u;
        constexpr uint32_t kDpcr2 = 0x1F801570u;
        constexpr uint32_t kDicr2 = 0x1F801574u;
        constexpr uint32_t kDmacEn = 0x1F801578u;
        // DICR2 derived bit positions for channel 9 (design §2.3):
        // mask 16 + (9-7) = 18, flag 24 + (9-7) = 26, tag-IRQ bit 9.
        constexpr uint32_t kDicr2Sif0Mask = 1u << 18u;
        constexpr uint32_t kDicr2Sif0Flag = 1u << 26u;
        constexpr uint32_t kDicr2Sif0TagIrq = 1u << 9u;
        // DPCR2 channel-9 enable: the nibble's bit 3 (bits 8-11, design
        // §5.1 step 3, model choice that DPCR enable gates start).
        constexpr uint32_t kDpcr2Sif0Enable = 1u << 11u;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    }

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        reset();
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        m_spu2.reset();
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        m_allocations.clear();
        m_heapCursor = HeapBase;
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
        // Sourced DPCR reset value (design §2.2). DPCR2 has no sourced
        // reset in the design; only its channel-9 enable nibble is
        // seeded set (MODEL CHOICE) so an unwritten DPCR2 does not
        // block every SIF0 start, while a guest writing DPCR2 with
        // the enable bit clear does gate start (design §5.1 step 3).
        m_hardware[kDpcr] = 0x07777777u;
        m_hardware[kDpcr2] = kDpcr2Sif0Enable;
        // SIF0 engine state is guest-transfer state: cleared on reset.
        // The transport pointer and the transportOwnsSif0 activation
        // flag are integration state, not guest state, and survive
        // reset (mirroring how the Spu2 object itself survives).
        m_sif0Active = false;
        m_sif0TagLatched = false;
        m_sif0HeaderPushed = false;
        m_sif0PausedOnFull = false;
        m_sif0Held = false;
        m_sif0TagIrq = false;
        m_sif0TagEot = false;
        m_sif0CurrentId = 0u;
        m_sif0NextId = 1u;
        m_sif0IdState.clear();
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        if (isSpu2Register(phys)) // byte reads see the register without side effects
            return static_cast<uint8_t>(m_spu2.peek16((phys - Spu2Base) & ~1u) >> ((phys & 1u) * 8u));
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if (isSpu2Register(phys) && (phys & 1u) == 0u)
        {
            const uint16_t value = m_spu2.read16(phys - Spu2Base);
            pollSpu2();
            return value;
        }
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && isSpu2Register(phys)) // two 16-bit accesses, low half first
            return static_cast<uint32_t>(read16(address)) | (static_cast<uint32_t>(read16(address + 2u)) << 16u);
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        if (isSpu2Register(phys))
        {
            const uint32_t half = (phys - Spu2Base) & ~1u;
            const uint32_t shift = (phys & 1u) * 8u;
            const uint16_t merged = static_cast<uint16_t>((m_spu2.peek16(half) & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift));
            write16(Spu2Base + half, merged);
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if (isSpu2Register(phys) && (phys & 1u) == 0u)
        {
            m_spu2.write16(phys - Spu2Base, value);
            pollSpu2();
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && isSpu2Register(phys))
        {
            write16(address, static_cast<uint16_t>(value));
            write16(address + 2u, static_cast<uint16_t>(value >> 16u));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        // SIF0 CHCR bit 24 is start/busy: it survives repeated reads
        // while the engine is in-flight or paused-on-full, and clears
        // only on EOT completion (design §6). No read-clears behaviour
        // is implemented for it — that semantic is not sourced for
        // the IOP side and would be an invention.
        if (address == kSif0Chcr && m_sif0Active)
        {
            const auto value = m_hardware.find(address);
            return (value != m_hardware.end() ? value->second : 0u) | kDmaStart;
        }
        // DICR bit 31 is derived, never stored (sourced formula,
        // design §2.3): DICR.15 | (DICR.23 && (DICR.24-30 ||
        // DICR2.24-29)). When set, IRQ3 is sent to the IOP INTC.
        if (address == kDicr)
        {
            const uint32_t dicr = sif0HwReg(kDicr);
            const uint32_t dicr2 = sif0HwReg(kDicr2);
            const bool master = (dicr & (1u << 23u)) != 0u;
            const bool anyFlag = (dicr & 0x7F000000u) != 0u || (dicr2 & 0x3F000000u) != 0u;
            const uint32_t derived = (dicr & ~(1u << 31u)) |
                                     (((dicr & (1u << 15u)) != 0u || (master && anyFlag)) ? (1u << 31u) : 0u);
            return derived;
        }
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        switch (address)
        {
        case 0x1F801070u:
            // I_STAT is write-0-to-clear (sourced IOP semantics,
            // design §2.3): a write with bit 3 = 0 clears bit 3, a
            // write with bit 3 = 1 leaves it unchanged. The EE
            // write-1-to-clear convention is NOT imported here.
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        case kDicr:
        {
            // DICR flags (bits 24-30) are write-1-to-clear (sourced);
            // bit 31 is derived on read and is not stored.
            const uint32_t stored = sif0HwReg(kDicr);
            m_hardware[kDicr] = (value & 0x00FFFFFFu) | (stored & 0x7F000000u & ~value);
            return;
        }
        case kDicr2:
        {
            // DICR2 flags (bits 24-29) are write-1-to-clear (sourced);
            // tag-IRQ enables (bits 0-12) and masks (bits 16-21) are
            // stored as written. This is a guest write; the DMA
            // engine sets flags by direct OR, never through here.
            const uint32_t stored = sif0HwReg(kDicr2);
            m_hardware[kDicr2] = (value & ~0xFF000000u) | (stored & 0x3F000000u & ~value);
            return;
        }
        case kSif0Chcr:
            m_hardware[kSif0Chcr] = value;
            sif0StartIfReady(value);
            return;
        case kDpcr:
        case kDpcr2:
        case kDmacEn:
        case 0x1F80157Cu: // DMACINTEN: stored, never a completion gate (design §12 UNKNOWN 4)
        case kSif0Madr:
        case kSif0Bcr:
        case kSif0Tadr:
            m_hardware[address] = value;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        const bool secondCore = address == kDmaSpu1Chcr;
        m_hardware[address] = value & ~kDmaStart;

        const uint32_t blockControlAddress = address - sizeof(uint32_t);
        uint32_t blockControl = 0u;
        if (const auto current = m_hardware.find(blockControlAddress); current != m_hardware.end())
            blockControl = current->second;
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t transferWords = static_cast<uint64_t>(wordsPerBlock) * blockCount;
        transferSpu2Dma(secondCore, value, static_cast<uint32_t>(std::min<uint64_t>(transferWords * 2u, 0xFFFFFFFFu)));
        m_dmaStart = DmaStart{
            secondCore ? kDmaSpu1Irq : kDmaSpu0Irq,
            std::max<uint64_t>(transferWords * 2u, 64u),
        };
    }

    void IopMemory::pollSpu2() const
    {
        if (m_spu2.takeInterrupt())
            m_interruptStatus |= 1u << 9u;
    }

    void IopMemory::advanceSpu2Cycles(uint64_t cycles) const
    {
        m_spu2.advanceCycles(cycles);
        pollSpu2();
    }

    // IOP DMA channel 4 (core 0) / 7 (core 1): CHCR bit 0 set moves IOP RAM at MADR to sound RAM at the core's
    // transfer address, clear moves sound RAM to IOP RAM; MADR wraps in the 2 MB IOP RAM. The data moves when the
    // channel starts; STAT bit 7 is set at the end.
    void IopMemory::transferSpu2Dma(bool secondCore, uint32_t chcr, uint32_t halfwords)
    {
        const int core = secondCore ? 1 : 0;
        uint32_t madr = 0u;
        if (const auto current = m_hardware.find((secondCore ? kDmaSpu1Chcr : kDmaSpu0Chcr) - 8u); current != m_hardware.end())
            madr = current->second & (RamSize - 2u);
        for (uint32_t i = 0; i < halfwords; ++i)
        {
            const uint32_t at = (madr + i * 2u) & (RamSize - 1u);
            uint16_t sample = 0u;
            if ((chcr & 1u) != 0u)
            {
                std::memcpy(&sample, m_ram.data() + at, sizeof(sample));
                m_spu2.dmaWrite(core, &sample, 1u);
            }
            else
            {
                m_spu2.dmaRead(core, &sample, 1u);
                std::memcpy(m_ram.data() + at, &sample, sizeof(sample));
                markOwned(at, sizeof(sample));
            }
        }
        m_spu2.completeDma(core);
        pollSpu2();
    }

    // SIF0 completion signalling (design §7.2, Path A only). The
    // engine sets the DICR2 flag and derives DICR.31; DICR.31 is
    // reflected into I_STAT bit 3, and the existing checkInterrupt()
    // exception path is what the CPU sees. This function never calls
    // dispatchInterrupt and never writes m_interruptStatus except
    // through the I_STAT bit-3 reflection below.
    void IopMemory::sif0SignalCompletion(bool tagIrq)
    {
        const uint32_t dicr = sif0HwReg(kDicr);
        if ((dicr & (1u << 23u)) == 0u)
            return; // DICR.23 master enable clear: no flag, no IRQ3.
        const uint32_t dicr2 = sif0HwReg(kDicr2);
        // EOT completion requires the channel mask (bit 18); a tag
        // IRQ fires independently of that mask (sourced, design
        // §2.3) but still through the same DICR.31 -> IRQ3 route.
        // Both paths set the same flag, so both enabled for one tag
        // still assert IRQ3 exactly once (a bit, not a count).
        if (!tagIrq && (dicr2 & kDicr2Sif0Mask) == 0u)
            return;
        m_hardware[kDicr2] = sif0HwReg(kDicr2) | kDicr2Sif0Flag;
        // DICR.31 is now set (master on + flag 26 on): reflect IRQ3.
        m_interruptStatus |= 1u << 3u;
    }

    void IopMemory::sif0StartIfReady(uint32_t chcrValue)
    {
        if (!m_transportOwnsSif0 || m_sif0Active)
            return; // Gated off, or already in-flight: no restart.
        // Preconditions (design §5.1, all must hold; otherwise the
        // engine does nothing and CHCR bit 24 stays as written):
        // DMACEN bit 0, chain mode (bits 9-10 = 3), CHCR.8 header,
        // direction bit 0 = from RAM, STR bit 24, DPCR2 ch9 enable.
        if ((sif0HwReg(kDmacEn) & 1u) == 0u)
            return;
        if (((chcrValue >> 9u) & 0x3u) != 3u || (chcrValue & (1u << 8u)) == 0u ||
            (chcrValue & 1u) == 0u || (chcrValue & kDmaStart) == 0u)
            return;
        if ((sif0HwReg(kDpcr2) & kDpcr2Sif0Enable) == 0u)
            return;
        m_sif0Active = true;
        m_sif0TagLatched = false;
        m_sif0HeaderPushed = false;
        m_sif0PausedOnFull = false;
        m_sif0Held = false;
        m_sif0TagIrq = false;
        m_sif0TagEot = false;
        m_sif0HeaderWords = 4u; // CHCR.8 set (precondition above): 2 tag words + 2 header words.
        m_sif0CurrentId = m_sif0NextId++;
        if (m_sif0NextId == 0u)
            m_sif0NextId = 1u;
        m_sif0IdState[m_sif0CurrentId] = 0;
        // The transfer is started, not yet driven: progress happens
        // in driveSif0() at scheduleStep boundaries (design §9).
    }

    void IopMemory::driveSif0()
    {
        if (!m_sif0Active || m_sif0Fifo == nullptr)
            return;
        // BCR is ignored for chain transfers (MODEL CHOICE, design
        // §5.1 step 6): transfer size comes solely from the tag.
        // A cyclic chain (no EOT) is bounded per drive call so one
        // drive can never hang the scheduler; progress resumes on
        // the next drive. This guard is a safety bound, not hardware.
        for (int guard = 0; guard < 1024 && m_sif0Active; ++guard)
        {
            if (!m_sif0TagLatched)
            {
                const uint32_t tadr = sif0HwReg(kSif0Tadr);
                uint32_t tagWords[4] = {};
                const size_t tagBytes = static_cast<size_t>(m_sif0HeaderWords) * 4u;
                if (!readRam(tadr, tagWords, tagBytes))
                {
                    // Out-of-range tag read: hold, no completion,
                    // CHCR bit 24 stays set, no DICR flag (§10).
                    m_sif0Held = true;
                    return;
                }
                const uint32_t srcAddr = tagWords[0] & 0x00FFFFFFu;
                const bool tagIrq = (tagWords[0] & (1u << 30u)) != 0u;
                const bool tagEot = (tagWords[0] & (1u << 31u)) != 0u;
                const uint32_t sizeWords = tagWords[1] & 0x00FFFFFFu;
                // Partial-QW rule, CORRECTED after corroboration
                // (Phase 4): the design's zero-fill rule for a final
                // partial QW is NOT used — the PCSX2 blog documents
                // zero-fill padding as breaking a real game (True
                // Crime reinitialising its DVD library), and the
                // iris-consistent model rounds the tag word count
                // UP to a QW boundary and reads the padding words
                // from IOP RAM, so the EE side never fabricates a
                // partial QW at all. This rounding is still a
                // labelled model (no hardware probe confirms it),
                // but it replaces zero-fill everywhere: the
                // rounded size is what is validated, pushed, and
                // counted below.
                const uint32_t roundedWords = (sizeWords + 3u) & ~3u;
                // Whole-tag source bounds, checked before any header
                // push or payload byte moves (§10): the ROUNDED
                // range must fit in IOP RAM, no partial write past
                // the boundary, no completion on failure.
                const uint64_t srcBytes = static_cast<uint64_t>(roundedWords) * 4u;
                if (static_cast<uint64_t>(srcAddr) + srcBytes > RamSize)
                {
                    m_sif0Held = true;
                    return;
                }
                m_sif0Held = false;
                m_sif0TagIrq = tagIrq;
                m_sif0TagEot = tagEot;
                m_sif0HeaderLo = tagWords[2];
                m_sif0HeaderHi = tagWords[3];
                m_sif0SrcCursor = srcAddr;
                m_sif0WordsRemaining = roundedWords;
                m_sif0TagLatched = true;
                m_sif0HeaderPushed = false;
            }

            if (!m_sif0HeaderPushed)
            {
                if (!m_sif0Fifo->tryPushHeader(ps2x::iop::Sif0Fifo::Header{m_sif0HeaderLo, m_sif0HeaderHi}))
                {
                    // Header occupies a FIFO slot (§4.1): a full FIFO
                    // blocks the header, TADR and header preserved.
                    m_sif0PausedOnFull = true;
                    return;
                }
                m_sif0HeaderPushed = true;
                m_sif0PausedOnFull = false;
            }

            while (m_sif0WordsRemaining > 0u)
            {
                if (m_sif0Fifo->isFull())
                {
                    m_sif0PausedOnFull = true;
                    return;
                }
                // After rounding, every push is a whole QW read
                // from IOP RAM (padding words are real RAM words,
                // never zero-filled — see the correction above).
                const uint32_t take = 4u;
                ps2x::iop::Sif0Fifo::Qword qw{};
                for (uint32_t i = 0u; i < take; ++i)
                {
                    uint32_t word = 0u;
                    std::memcpy(&word, m_ram.data() + m_sif0SrcCursor + i * 4u, sizeof(word));
                    std::memcpy(qw.bytes.data() + i * 4u, &word, sizeof(word));
                }
                if (!m_sif0Fifo->tryPushQw(qw))
                {
                    m_sif0PausedOnFull = true;
                    return;
                }
                m_sif0SrcCursor += take * 4u;
                m_sif0WordsRemaining -= take;
            }

            // Tag fully pushed. A tag IRQ signals when that tag's
            // words are fully pushed, independently of the channel
            // mask (design §5.1 step 5).
            if (m_sif0TagIrq && (sif0HwReg(kDicr2) & kDicr2Sif0TagIrq) != 0u)
                sif0SignalCompletion(true);
            // TADR advances only when the tag's payload is fully
            // pushed (tag size 2 or 4 words, sourced, design §2.6).
            m_hardware[kSif0Tadr] = sif0HwReg(kSif0Tadr) + m_sif0HeaderWords * 4u;
            m_sif0TagLatched = false;
            m_sif0PausedOnFull = false;
            if (m_sif0TagEot)
            {
                m_sif0Active = false;
                m_hardware[kSif0Chcr] = sif0HwReg(kSif0Chcr) & ~kDmaStart;
                m_sif0IdState[m_sif0CurrentId] = 1;
                sif0SignalCompletion(false);
                return;
            }
        }
    }

    int32_t IopMemory::sif0DmaStat(uint32_t id) const
    {
        const auto it = m_sif0IdState.find(id);
        return it != m_sif0IdState.end() ? it->second : -1;
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        // Align in 64 bits: alignUp() wraps for a guest size near UINT32_MAX (e.g. 0xFFFFFFF0
        // aligns to 0), which would otherwise turn a huge request into a tiny one.
        const uint64_t alignedSize64 = (static_cast<uint64_t>(std::max(size, 1u)) + 15u) & ~15ull;
        if (alignedSize64 > static_cast<uint64_t>(HeapLimit - HeapBase))
            return 0u;
        size = static_cast<uint32_t>(alignedSize64);
        alignment = std::max<uint32_t>(alignment, 4u);
        if (fixed)
        {
            const uint32_t address = *fixed;
            // Overflow-safe range check. The old check `address + size > HeapLimit` wrapped for
            // an address near 0xFFFFFFFF (0xFFFFFFF0 + 0x20 -> 0x10), so AllocSysMemory with a
            // guest-controlled fixed address could record an allocation outside IOP RAM; freeing
            // that allocation would then fill m_owned far past its end. All comparisons here are
            // in 64 bits, and the recorded range is guaranteed inside [HeapBase, HeapLimit].
            const uint64_t address64 = address;
            if (address64 < HeapBase || address64 + size > HeapLimit)
                return 0u;
            for (const auto &block : m_allocations)
                if (address64 < static_cast<uint64_t>(block.address) + block.size &&
                    static_cast<uint64_t>(block.address) < address64 + size)
                    return 0u;
            m_allocations.push_back({address, size});
            markOwned(address, size);
            return address;
        }

        uint32_t candidate = alignUp(m_heapCursor, alignment);
        for (;;)
        {
            bool overlap = false;
            for (const auto &block : m_allocations)
            {
                if (candidate < block.address + block.size && block.address < candidate + size)
                {
                    candidate = alignUp(block.address + block.size, alignment);
                    overlap = true;
                    break;
                }
            }
            if (!overlap)
                break;
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        m_allocations.push_back({candidate, size});
        markOwned(candidate, size);
        m_heapCursor = std::max(m_heapCursor, candidate + size);
        return candidate;
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        // Defensive: never fill past m_owned, even for a malformed allocation recorded by
        // an older version. allocate() now guarantees in-range blocks, so this is a guard,
        // not a second range model.
        if (block->address <= RamSize && block->size <= RamSize - block->address)
            std::fill(m_owned.begin() + block->address,
                      m_owned.begin() + block->address + block->size,
                      uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        return m_heapCursor < HeapLimit ? HeapLimit - m_heapCursor : 0u;
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}
