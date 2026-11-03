#pragma once

#include "iop_spu2.h"
#include "ps2x/iop/sif0_fifo.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory
    {
    public:
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        static constexpr uint32_t HeapBase = 0x00120000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
        };

        IopMemory();

        void reset();

        [[nodiscard]] uint8_t read8(uint32_t address) const;
        [[nodiscard]] uint16_t read16(uint32_t address) const;
        [[nodiscard]] uint32_t read32(uint32_t address) const;
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;
        // Guest-time SPU2 advance: steps all voices by `cycles` of IOP time (768 cycles per
        // 48 kHz sample) and then polls the interrupt, so a voice fetch's IRQA hit reaches
        // I_STAT line 9 in guest time instead of waiting for the next register/DMA access.
        void advanceSpu2Cycles(uint64_t cycles) const;
        [[nodiscard]] Spu2 &spu2() noexcept { return m_spu2; }
        [[nodiscard]] const Spu2 &spu2() const noexcept { return m_spu2; }
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }

        // SIF0 DMA (IOP side, channel 9 chain mode) — Phase 4 vertical
        // slice, labelled model per sif0-architecture-design.md §11.1.
        // The transport object is owned by the caller (the host/runtime
        // layer that bridges the processors, design §3.4); IopMemory only
        // holds a pointer to it and never owns it. transportOwnsSif0 is
        // an explicit activation flag, default false, never inferred
        // from CHCR STR: when false the channel-9 registers are stored
        // but the engine never starts and the HLE SIF paths stay sole
        // owner, exactly as before this slice.
        void setSif0Transport(ps2x::iop::Sif0Fifo *fifo) noexcept { m_sif0Fifo = fifo; }
        void setTransportOwnsSif0(bool owned) noexcept { m_transportOwnsSif0 = owned; }
        [[nodiscard]] bool transportOwnsSif0() const noexcept { return m_transportOwnsSif0; }
        // Drive the channel-9 engine until FIFO full, chain EOT, or a
        // hold. Called by IopEmulator::serviceSif0 at scheduleStep
        // boundaries and directly by tests; never called from inside
        // runCpu slices (the documented nested-runCpu blind spot).
        void driveSif0();
        // True while a channel-9 transfer is started and not ended
        // (in-flight, paused-on-full, or held on a bounds failure).
        // Used by the scheduler idle nextWake computation.
        [[nodiscard]] bool sif0Pending() const noexcept { return m_sif0Active; }
        [[nodiscard]] bool sif0Active() const noexcept { return m_sif0Active; }
        [[nodiscard]] bool sif0PausedOnFull() const noexcept { return m_sif0PausedOnFull; }
        [[nodiscard]] bool sif0Held() const noexcept { return m_sif0Held; }
        // In-flight DmaStat model (design §8), under transport
        // ownership only: 1 = that id's transfer completed (DICR2 flag
        // set), 0 = in-flight, -1 = unknown id. The HLE stub keeps its
        // legacy return while transportOwnsSif0 is false.
        [[nodiscard]] int32_t sif0DmaStat(uint32_t id) const;
        [[nodiscard]] uint32_t sif0CurrentTransferId() const noexcept { return m_sif0CurrentId; }

        [[nodiscard]] static uint32_t physicalAddress(uint32_t address) noexcept;

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);
        [[nodiscard]] static bool isSpu2Register(uint32_t phys) noexcept
        {
            return phys >= Spu2Base && phys < Spu2Base + Spu2::kRegisterBytes;
        }
        // Latches an SPU2 IRQA hit into I_STAT (line 9).
        void pollSpu2() const;
        void transferSpu2Dma(bool secondCore, uint32_t chcr, uint32_t halfwords);

        // SIF0 channel-9 engine (private state, design §3.2). The DMA
        // engine sets the DICR2 flag and derives DICR.31; it never
        // writes m_interruptStatus directly and never calls
        // dispatchInterrupt (Path A only, design §7.2).
        [[nodiscard]] uint32_t sif0HwReg(uint32_t address) const noexcept
        {
            const auto it = m_hardware.find(address);
            return it != m_hardware.end() ? it->second : 0u;
        }
        void sif0SignalCompletion(bool tagIrq);
        void sif0StartIfReady(uint32_t chcrValue);
        ps2x::iop::Sif0Fifo *m_sif0Fifo = nullptr;
        bool m_transportOwnsSif0 = false;
        bool m_sif0Active = false;
        bool m_sif0TagLatched = false;
        bool m_sif0HeaderPushed = false;
        bool m_sif0PausedOnFull = false;
        bool m_sif0Held = false;
        bool m_sif0TagIrq = false;
        bool m_sif0TagEot = false;
        uint32_t m_sif0HeaderWords = 2u;
        uint32_t m_sif0HeaderLo = 0u;
        uint32_t m_sif0HeaderHi = 0u;
        uint32_t m_sif0SrcCursor = 0u;
        uint32_t m_sif0WordsRemaining = 0u;
        uint32_t m_sif0CurrentId = 0u;
        uint32_t m_sif0NextId = 1u;
        std::unordered_map<uint32_t, int32_t> m_sif0IdState;

        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_owned;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<Allocation> m_allocations;
        uint32_t m_heapCursor = HeapBase;
        mutable uint32_t m_interruptStatus = 0; // the SPU2 interrupt can latch during a register read
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        mutable Spu2 m_spu2; // data-port reads advance the transfer address
    };
}
