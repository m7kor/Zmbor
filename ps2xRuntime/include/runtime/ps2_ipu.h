#pragma once

#include <array>
#include <cstdint>
#include <deque>

// EE Image Processing Unit: register file, input/output FIFOs and the command state machine.
//
// Registers (EE 0x10002000-0x10002030): IPU_CMD (write: command in bits 28-31, option below; read: result word, BUSY
// in bit 63), IPU_CTRL (IFC 0-3, OFC 4-7, CBP 8-13, ECD 14, SCD 15, IDP 16-17, AS 20, IVF 21, QST 22, MP1 23, PCT
// 24-26, RST 30 (write), BUSY 31), IPU_BP (BP 0-6, IFC 8-11, FP 16-17), IPU_TOP (next 32 bitstream bits, BUSY 63).
// The input FIFO holds 8 qwords fed by toIPU (DMA channel 4) or the IPU_in_FIFO window; the bitstream is the bytes in
// memory order, each read MSB first (MPEG bit order).
//
// Implemented commands: BCLR (0), FDEC (4), SETIQ (5), SETVQ (6), SETTH (9). IDEC (1), BDEC (2), VDEC (3), CSC (7)
// and PACK (8) are not implemented: such a command is reported through unimplementedCommand() and completes without
// output, so a caller can see that no decoding happened.
//
// Semantics follow the register layout of ps2tek and the behaviour DobieStation models (src/core/ee/ipu, studied, not
// copied); no hardware program checks the IPU yet.
class PS2Ipu
{
public:
    using Qword = std::array<uint8_t, 16>;

    void reset();

    [[nodiscard]] uint64_t readCommand() const;
    [[nodiscard]] uint32_t readControl() const;
    [[nodiscard]] uint32_t readBitPointer() const;
    [[nodiscard]] uint64_t readTop() const;
    // Returns false when a command is written while the IPU is busy (ignored, as the hardware ignores it).
    bool writeCommand(uint32_t value);
    void writeControl(uint32_t value);

    [[nodiscard]] bool canAcceptInput() const noexcept { return m_input.size() < kFifoQwords; }
    bool pushInput(const Qword &qword);
    [[nodiscard]] size_t inputQwords() const noexcept { return m_input.size(); }
    [[nodiscard]] size_t outputQwords() const noexcept { return m_output.size(); }
    bool popOutput(Qword &qword);

    // Advances the current command with the input available.
    void run();
    // True once per completed command (the IPU interrupt, INTC line 8).
    bool takeCompletion() noexcept
    {
        const bool completed = m_completed;
        m_completed = false;
        return completed;
    }
    [[nodiscard]] bool busy() const noexcept { return m_busy; }

    [[nodiscard]] uint8_t intraQuantizer(size_t i) const { return m_intraIq[i & 63u]; }
    [[nodiscard]] uint8_t nonIntraQuantizer(size_t i) const { return m_nonIntraIq[i & 63u]; }
    [[nodiscard]] uint16_t vqClut(size_t i) const { return m_vqClut[i & 15u]; }
    [[nodiscard]] uint16_t threshold0() const noexcept { return m_th0; }
    [[nodiscard]] uint16_t threshold1() const noexcept { return m_th1; }
    // Command number of the last command that was not executed because it is not implemented, or -1.
    [[nodiscard]] int unimplementedCommand() const noexcept { return m_unimplemented; }

    static constexpr size_t kFifoQwords = 8u;

private:
    [[nodiscard]] size_t availableBits() const noexcept { return m_input.size() * 128u - m_bitPointer; }
    [[nodiscard]] bool peekBits(uint32_t count, uint32_t &value) const;
    bool advanceBits(uint32_t count);
    void finish();

    std::deque<Qword> m_input;
    std::deque<Qword> m_output;
    uint32_t m_bitPointer = 0;  // 0-127 within the first input qword
    uint32_t m_command = 0;
    uint32_t m_option = 0;
    uint32_t m_result = 0;
    uint32_t m_remaining = 0;   // bytes still to load (SETIQ/SETVQ)
    bool m_advanced = false;    // FDEC/SETIQ: the option's bit skip was applied
    bool m_busy = false;
    bool m_completed = false;
    bool m_decoding = false;    // FDEC in progress (CMD/TOP bit 63)
    bool m_errorCode = false;
    bool m_startCode = false;
    uint32_t m_controlFields = 0; // IDP, AS, IVF, QST, MP1, PCT as written
    std::array<uint8_t, 64> m_intraIq{};
    std::array<uint8_t, 64> m_nonIntraIq{};
    std::array<uint16_t, 16> m_vqClut{};
    uint16_t m_th0 = 0;
    uint16_t m_th1 = 0;
    int m_unimplemented = -1;
};
