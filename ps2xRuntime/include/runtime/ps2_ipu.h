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
// Implemented commands: BCLR (0), IDEC (1), BDEC (2), VDEC (3), FDEC (4), SETIQ (5), SETVQ (6), CSC (7), PACK (8),
// SETTH (9). A command number above 9 is reported through unimplementedCommand() and completes without output.
//
// IDEC decodes an intra slice from its first macroblock_type to its end (ISO/IEC 13818-2 6.2.5, 7.2): for each
// macroblock the macroblock_type (B-2), dct_type when option DTD is set, quantiser_scale_code when the type has
// macroblock_quant (otherwise the QSC of the option or of the previous macroblock), the six intra blocks as BDEC
// decodes them (DC predictors reset to 128 << IDP at the start and after a skipped address), then CSC to RGB32 or
// RGB16 (option OFM, DTE dither; SGN subtracts 128 from each colour, wrapping). The next macroblock follows once the
// converted one has entered the output FIFO; macroblock_address_increment (B-1, with escape and stuffing) separates
// macroblocks, and the slice ends at the next start code: zero bits to the byte boundary, zero bytes, then 0x000001,
// which stays in the FIFO and sets CTRL.SCD. Anything else where an address increment is due sets CTRL.ECD. Motion
// compensation is not an IPU function: IDEC accepts intra macroblocks only (decision D-IPU-4).
//
// BDEC decodes one macroblock's block layer (ISO/IEC 13818-2 7.2-7.6, MPEG-1 rules with CTRL.MP1): for a non-intra
// macroblock (option MBI clear) the coded_block_pattern (B-9) first, then for each coded block of Y0-Y3, Cb, Cr the
// intra DC (B-12/B-13, predictors reset to 128 << IDP by option DCR) and the AC coefficients (B-14, or B-15 for intra
// with CTRL.IVF), inverse scan by CTRL.AS, inverse quantisation (QSC option bits 16-20, linear or non-linear by
// CTRL.QST, matrices from SETIQ in zigzag order), mismatch control (MPEG-2) or oddification (MPEG-1), saturation and
// an IEEE 1180 reference IDCT (decision D-IPU-3). The output is RAW16: Y as 16x16 (frame or, option DT, field block
// order), Cb 8x8, Cr 8x8, 48 qwords of little-endian int16; intra samples are clamped to 0-255. A bitstream error
// sets CTRL.ECD and ends the command.
//
// VDEC decodes one MPEG-2 VLC symbol (ISO/IEC 13818-2 tables B-1, B-2..B-4 by CTRL.PCT, B-10, B-11) and returns
// (code length << 16) | value (16-bit two's complement); a code not in the table sets CTRL.ECD.
//
// CSC converts YCbCr 4:2:0 macroblocks (Y 16x16, then Cb 8x8, Cr 8x8: 384 bytes) to RGB32 (64 qwords) or RGB16
// (32 qwords, optional 4x4 dither) with the integer BT.601 method PCSX2 documents as the hardware's (decision D-IPU-1;
// DobieStation uses full-range float coefficients instead); alpha is 0, 0x40 or 0x80 by TH0/TH1. The 8-qword output
// FIFO refills from the converted macroblock as it drains.
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
    [[nodiscard]] uint32_t idecMacroblocks() const noexcept { return m_idec.macroblocks; }
    [[nodiscard]] int32_t dcPredictor(size_t component) const { return m_dcPredictor[component % 3u]; }
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
    void convertMacroblock();
    void packMacroblock();
    void emitRgb16(const uint8_t *rgb32, bool dither);
    [[nodiscard]] static std::array<uint16_t, 256> toRgb16(const uint8_t *rgb32, bool dither);
    void refillOutput();
    // Block layer of the macroblock in m_bdec: 1 decoded, 0 more data needed, -1 bitstream error.
    int decodeBlocks();
    void runBdec();
    void runIdec();
    // Ends the current command with CTRL.ECD.
    void fail();
    [[nodiscard]] std::array<int16_t, 384> assembleMacroblock() const;
    // One coefficient symbol of the current block: 1 decoded, 0 more data needed, -1 bitstream error.
    int decodeCoefficient(bool &endOfBlock);
    void reconstructBlock();
    void emitRaw16();
    struct BdecState
    {
        uint32_t stage = 0;       // 0 CBP, 1 blocks, 2 decoded, 3 output emitted
        bool intra = false;
        bool field = false;       // field DCT: Y blocks on alternate lines
        uint32_t scaleCode = 0;   // quantiser_scale_code
        uint32_t block = 0;       // 0-5
        bool dcDone = false;
        uint32_t index = 0;       // next scan position
        std::array<int32_t, 64> coefficients{}; // QF by scan position
        std::array<std::array<int16_t, 64>, 6> blocks{};
    } m_bdec;
    struct IdecState
    {
        uint32_t stage = 0;       // 0 skip FB, 1 macroblock modes, 2 blocks, 3 output, 4 address increment, 5 start code
        uint32_t scaleCode = 0;
        uint32_t increment = 0;   // macroblock_address_increment so far (escapes)
        uint32_t macroblocks = 0; // decoded in this command
    } m_idec;
    std::array<int32_t, 3> m_dcPredictor{128, 128, 128};
    uint32_t m_codedBlockPattern = 0;
    std::array<uint8_t, 1024> m_rgbBlock{};

    std::deque<Qword> m_outputPending; // converted data not yet in the 8-qword output FIFO
    std::array<uint8_t, 384> m_macroblock{};
    uint32_t m_macroblockBytes = 0;
    uint32_t m_macroblocksLeft = 0;

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
