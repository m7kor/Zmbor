#pragma once

// SIF0 DMA transport (labelled model, Phase 4 SIF0 vertical slice).
//
// This is the shared FIFO contract from the SIF0 architecture design
// (sif0-architecture-design.md §4). It is a MODEL, not a hardware claim:
// the 8-slot capacity is adopted from the sourced "128-byte SIF FIFO"
// figure (Sony EE Overview) with an explicit per-direction split that no
// source states, and the header-occupies-a-slot rule is a slice model
// choice. If a hardware depth/split is later sourced, only the capacity
// constant and the full-threshold tests change; no engine logic changes.
//
// The FIFO is header-only so both processor sides can use it without a
// new link dependency: ps2xIOP owns it, and ps2xRuntime already includes
// ps2x/iop headers. It owns NO CHCR, D_STAT, DICR, interrupt, or
// transfer-id state. Those stay with their owners (PS2Memory on the EE
// side, IopMemory on the IOP side); the transport only moves units and
// reports full/empty. It never interprets tags, never completes a
// transfer, never grows, never reorders, and never carries SIF1, SIF2,
// SIFCMD, or SIFRPC traffic.
//
// Units: payload is one 128-bit quadword (16 bytes). The EE DMAtag
// header is pushed as one 64-bit header unit (the 2 extra words of the
// IOP chain tag when CHCR.8 is set) and occupies one FIFO slot, so a
// full FIFO blocks a header push too and a header can never overtake
// payload. Slots are kind-tagged: popping a header when the front slot
// is payload (or vice versa) returns false WITHOUT consuming anything,
// so a read on empty (or a kind mismatch) never fabricates data.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace ps2x::iop
{

class Sif0Fifo
{
public:
    // Bounded capacity (slots), one slot per header unit or payload QW.
    static constexpr size_t kCapacitySlots = 8u;

    struct Qword
    {
        std::array<uint8_t, 16u> bytes{};
    };

    struct Header
    {
        uint32_t lo = 0u; // EE DMAtag low word: QWC bits 0-15, Tag ID 28-30, IRQ 31
        uint32_t hi = 0u; // EE DMAtag high word: ADDR bits 0-30, scratchpad bit 31
    };

    // False (no state change) when full. Never overwrites.
    [[nodiscard]] bool tryPushHeader(Header header)
    {
        if (m_slots.size() >= kCapacitySlots)
            return false;
        Slot slot{};
        slot.kind = SlotKind::Header;
        slot.header = header;
        m_slots.push_back(slot);
        return true;
    }

    // False (nothing consumed) when empty or when the front slot is not
    // a header. A read on empty never fabricates data.
    [[nodiscard]] bool tryPopHeader(Header &header)
    {
        if (m_slots.empty() || m_slots.front().kind != SlotKind::Header)
            return false;
        header = m_slots.front().header;
        m_slots.pop_front();
        return true;
    }

    // False (no state change) when full. Never overwrites.
    [[nodiscard]] bool tryPushQw(const Qword &qw)
    {
        if (m_slots.size() >= kCapacitySlots)
            return false;
        Slot slot{};
        slot.kind = SlotKind::Payload;
        slot.qw = qw;
        m_slots.push_back(slot);
        return true;
    }

    // False (nothing consumed) when empty or when the front slot is not
    // a payload QW.
    [[nodiscard]] bool tryPopQw(Qword &qw)
    {
        if (m_slots.empty() || m_slots.front().kind != SlotKind::Payload)
            return false;
        qw = m_slots.front().qw;
        m_slots.pop_front();
        return true;
    }

    [[nodiscard]] bool isEmpty() const noexcept { return m_slots.empty(); }
    [[nodiscard]] bool isFull() const noexcept { return m_slots.size() >= kCapacitySlots; }
    [[nodiscard]] size_t count() const noexcept { return m_slots.size(); }

    // Cursors to 0; buffer contents undefined thereafter. Guest code
    // never sees count(); it exists for tests and stall assertions.
    void reset() noexcept { m_slots.clear(); }

private:
    enum class SlotKind
    {
        Header,
        Payload,
    };

    struct Slot
    {
        SlotKind kind = SlotKind::Payload;
        Header header{};
        Qword qw{};
    };

    std::deque<Slot> m_slots;
};

} // namespace ps2x::iop
