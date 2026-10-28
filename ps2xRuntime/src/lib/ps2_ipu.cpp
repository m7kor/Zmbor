#include "runtime/ps2_ipu.h"

#include <algorithm>

namespace
{
    constexpr uint32_t kBclr = 0u;
    constexpr uint32_t kFdec = 4u;
    constexpr uint32_t kSetiq = 5u;
    constexpr uint32_t kSetvq = 6u;
    constexpr uint32_t kCsc = 7u;
    constexpr uint32_t kPack = 8u;
    // Integer BT.601 conversion (1/64 units), rounded by a final shift with +1 (PCSX2 yuv2rgb_reference, D-IPU-1).
    constexpr int32_t kYBias = 16;
    constexpr int32_t kYCoeff = 0x95;
    constexpr int32_t kRCrCoeff = 0xCC;
    constexpr int32_t kGCrCoeff = -0x68;
    constexpr int32_t kGCbCoeff = -0x32;
    constexpr int32_t kBCbCoeff = 0x102;
    constexpr int kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

    int32_t clampByte(int32_t v)
    {
        return std::max(0, std::min(255, v));
    }

    constexpr uint32_t kVdec = 3u;

    // MPEG-2 VLC tables used by VDEC (ISO/IEC 13818-2 Annex B). Codes are written MSB first.
    struct Vlc
    {
        uint16_t code;
        uint8_t length;
        int16_t value;
    };

    // B-1 macroblock_address_increment; stuffing decodes to 34 and escape to 35 (the IPU's convention as the
    // references model it).
    constexpr Vlc kMbai[] = {
        {0b1, 1, 1}, {0b011, 3, 2}, {0b010, 3, 3}, {0b0011, 4, 4}, {0b0010, 4, 5}, {0b00011, 5, 6}, {0b00010, 5, 7},
        {0b0000111, 7, 8}, {0b0000110, 7, 9}, {0b00001011, 8, 10}, {0b00001010, 8, 11}, {0b00001001, 8, 12},
        {0b00001000, 8, 13}, {0b00000111, 8, 14}, {0b00000110, 8, 15}, {0b0000010111, 10, 16},
        {0b0000010110, 10, 17}, {0b0000010101, 10, 18}, {0b0000010100, 10, 19}, {0b0000010011, 10, 20},
        {0b0000010010, 10, 21}, {0b00000100011, 11, 22}, {0b00000100010, 11, 23}, {0b00000100001, 11, 24},
        {0b00000100000, 11, 25}, {0b00000011111, 11, 26}, {0b00000011110, 11, 27}, {0b00000011101, 11, 28},
        {0b00000011100, 11, 29}, {0b00000011011, 11, 30}, {0b00000011010, 11, 31}, {0b00000011001, 11, 32},
        {0b00000011000, 11, 33}, {0b00000001111, 11, 34}, {0b00000001000, 11, 35},
    };
    // B-2/B-3/B-4 macroblock_type as flag bits: quant 0x10, motion forward 0x08, motion backward 0x04,
    // pattern 0x02, intra 0x01.
    constexpr Vlc kMbTypeI[] = {{0b1, 1, 0x01}, {0b01, 2, 0x11}};
    constexpr Vlc kMbTypeP[] = {{0b1, 1, 0x0A}, {0b01, 2, 0x02}, {0b001, 3, 0x08}, {0b00011, 5, 0x01},
                                {0b00010, 5, 0x1A}, {0b00001, 5, 0x12}, {0b000001, 6, 0x11}};
    constexpr Vlc kMbTypeB[] = {{0b10, 2, 0x0C}, {0b11, 2, 0x0E}, {0b010, 3, 0x04}, {0b011, 3, 0x06},
                                {0b0010, 4, 0x08}, {0b0011, 4, 0x0A}, {0b00011, 5, 0x01}, {0b00010, 5, 0x1E},
                                {0b000011, 6, 0x1A}, {0b000010, 6, 0x16}, {0b000001, 6, 0x11}};
    // B-10 motion_code: the last bit is the sign (0 positive).
    constexpr Vlc kMotionCode[] = {
        {0b1, 1, 0}, {0b010, 3, 1}, {0b011, 3, -1}, {0b0010, 4, 2}, {0b0011, 4, -2}, {0b00010, 5, 3},
        {0b00011, 5, -3}, {0b0000110, 7, 4}, {0b0000111, 7, -4}, {0b00001010, 8, 5}, {0b00001011, 8, -5},
        {0b00001000, 8, 6}, {0b00001001, 8, -6}, {0b00000110, 8, 7}, {0b00000111, 8, -7},
        {0b0000010110, 10, 8}, {0b0000010111, 10, -8}, {0b0000010100, 10, 9}, {0b0000010101, 10, -9},
        {0b0000010010, 10, 10}, {0b0000010011, 10, -10}, {0b00000100010, 11, 11}, {0b00000100011, 11, -11},
        {0b00000100000, 11, 12}, {0b00000100001, 11, -12}, {0b00000011110, 11, 13}, {0b00000011111, 11, -13},
        {0b00000011100, 11, 14}, {0b00000011101, 11, -14}, {0b00000011010, 11, 15}, {0b00000011011, 11, -15},
        {0b00000011000, 11, 16}, {0b00000011001, 11, -16},
    };
    // B-11 dmvector.
    constexpr Vlc kDmVector[] = {{0b0, 1, 0}, {0b10, 2, 1}, {0b11, 2, -1}};

    struct VlcTable
    {
        const Vlc *codes;
        size_t count;
    };

    template <size_t N>
    constexpr VlcTable table(const Vlc (&codes)[N])
    {
        return {codes, N};
    }
    constexpr uint32_t kSetth = 9u;
    // IPU_CTRL fields the EE writes: IDP 16-17, AS 20, IVF 21, QST 22, MP1 23, PCT 24-26.
    constexpr uint32_t kControlWritable = 0x07F30000u;
    constexpr uint32_t kControlReset = 1u << 30;
}

void PS2Ipu::reset()
{
    *this = PS2Ipu{};
}

uint64_t PS2Ipu::readCommand() const
{
    return static_cast<uint64_t>(m_result) | (static_cast<uint64_t>(m_decoding) << 63);
}

uint32_t PS2Ipu::readControl() const
{
    uint32_t value = static_cast<uint32_t>(std::min<size_t>(m_input.size(), 15u));
    value |= static_cast<uint32_t>(std::min<size_t>(m_output.size(), 15u)) << 4;
    value |= m_errorCode ? (1u << 14) : 0u;
    value |= m_startCode ? (1u << 15) : 0u;
    value |= m_controlFields;
    value |= m_busy ? (1u << 31) : 0u;
    return value;
}

// BP is the bit position in the first input qword; with a partly consumed first qword, FP is 1 and IFC counts the
// whole qwords after it.
uint32_t PS2Ipu::readBitPointer() const
{
    const bool partial = m_bitPointer != 0u && !m_input.empty();
    const uint32_t whole = static_cast<uint32_t>(m_input.size()) - (partial ? 1u : 0u);
    return (m_bitPointer & 0x7Fu) | ((whole & 0xFu) << 8) | ((partial ? 1u : 0u) << 16);
}

// The next 32 bitstream bits, left-aligned; with fewer than 32 bits in the FIFO bit 63 is set as well (DobieStation
// notes FMV libraries use it to see how much data is left).
uint64_t PS2Ipu::readTop() const
{
    const uint32_t bits = static_cast<uint32_t>(std::min<size_t>(availableBits(), 32u));
    uint32_t value = 0u;
    if (bits != 0u)
    {
        (void)peekBits(bits, value);
        value = bits == 32u ? value : value << (32u - bits);
    }
    return static_cast<uint64_t>(value) | (static_cast<uint64_t>(m_decoding || bits < 32u) << 63);
}

bool PS2Ipu::peekBits(uint32_t count, uint32_t &value) const
{
    if (count == 0u || count > 32u || availableBits() < count)
        return false;
    uint64_t acc = 0u;
    uint32_t position = m_bitPointer;
    for (uint32_t i = 0; i < count; ++i, ++position)
    {
        const size_t qword = position / 128u;
        const uint32_t bit = position % 128u;
        const uint8_t byte = m_input[qword][bit / 8u];
        acc = (acc << 1) | ((byte >> (7u - (bit % 8u))) & 1u);
    }
    value = static_cast<uint32_t>(acc);
    return true;
}

bool PS2Ipu::advanceBits(uint32_t count)
{
    if (availableBits() < count)
        return false;
    m_bitPointer += count;
    while (m_bitPointer >= 128u)
    {
        m_bitPointer -= 128u;
        m_input.pop_front();
    }
    return true;
}

bool PS2Ipu::pushInput(const Qword &qword)
{
    if (!canAcceptInput())
        return false;
    m_input.push_back(qword);
    return true;
}

bool PS2Ipu::popOutput(Qword &qword)
{
    if (m_output.empty())
        return false;
    qword = m_output.front();
    m_output.pop_front();
    refillOutput();
    return true;
}

void PS2Ipu::refillOutput()
{
    while (m_output.size() < kFifoQwords && !m_outputPending.empty())
    {
        m_output.push_back(m_outputPending.front());
        m_outputPending.pop_front();
    }
}

void PS2Ipu::convertMacroblock()
{
    const bool rgb16 = (m_option & (1u << 27)) != 0u;
    const bool dither = (m_option & (1u << 26)) != 0u;
    std::array<uint8_t, 256u * 4u> rgb32{};
    for (uint32_t y = 0; y < 16u; ++y)
    {
        for (uint32_t x = 0; x < 16u; ++x)
        {
            const uint32_t chroma = (y >> 1) * 8u + (x >> 1);
            const int32_t cb = static_cast<int32_t>(m_macroblock[256u + chroma]) - 128;
            const int32_t cr = static_cast<int32_t>(m_macroblock[320u + chroma]) - 128;
            const int32_t lum = (kYCoeff * std::max(0, static_cast<int32_t>(m_macroblock[y * 16u + x]) - kYBias)) >> 6;
            const int32_t r = clampByte((lum + ((kRCrCoeff * cr) >> 6) + 1) >> 1);
            const int32_t g = clampByte((lum + ((kGCrCoeff * cr) >> 6) + ((kGCbCoeff * cb) >> 6) + 1) >> 1);
            const int32_t b = clampByte((lum + ((kBCbCoeff * cb) >> 6) + 1) >> 1);
            uint8_t alpha = 0x80u;
            if (r < m_th0 && g < m_th0 && b < m_th0)
                alpha = 0u;
            else if (r < m_th1 && g < m_th1 && b < m_th1)
                alpha = 0x40u;
            uint8_t *p = &rgb32[(y * 16u + x) * 4u];
            p[0] = static_cast<uint8_t>(r);
            p[1] = static_cast<uint8_t>(g);
            p[2] = static_cast<uint8_t>(b);
            p[3] = alpha;
        }
    }
    if (!rgb16)
    {
        for (uint32_t q = 0; q < 64u; ++q)
        {
            Qword qword{};
            std::copy_n(rgb32.begin() + q * 16u, 16u, qword.begin());
            m_outputPending.push_back(qword);
        }
    }
    else
        emitRgb16(rgb32.data(), dither);
    refillOutput();
}

// RGB32 -> RGB16 (5:5:5, top bits, optional 4x4 dither; bit 15 for alpha 0x40), shared by CSC and PACK.
std::array<uint16_t, 256> PS2Ipu::toRgb16(const uint8_t *rgb32, bool dither)
{
    std::array<uint16_t, 256> out{};
    for (uint32_t pixel = 0; pixel < 256u; ++pixel)
    {
        const uint32_t y = pixel / 16u, x = pixel % 16u;
        const int d = dither ? kDither[y & 3u][x & 3u] : 0;
        const uint8_t *p = &rgb32[pixel * 4u];
        const uint32_t r = static_cast<uint32_t>(clampByte(p[0] + d)) >> 3;
        const uint32_t g = static_cast<uint32_t>(clampByte(p[1] + d)) >> 3;
        const uint32_t b = static_cast<uint32_t>(clampByte(p[2] + d)) >> 3;
        out[pixel] = static_cast<uint16_t>(r | (g << 5) | (b << 10) | ((p[3] == 0x40u ? 1u : 0u) << 15));
    }
    return out;
}

void PS2Ipu::emitRgb16(const uint8_t *rgb32, bool dither)
{
    const std::array<uint16_t, 256> rgb = toRgb16(rgb32, dither);
    for (uint32_t q = 0; q < 32u; ++q)
    {
        Qword qword{};
        for (uint32_t i = 0; i < 8u; ++i)
        {
            qword[i * 2u] = static_cast<uint8_t>(rgb[q * 8u + i]);
            qword[i * 2u + 1u] = static_cast<uint8_t>(rgb[q * 8u + i] >> 8);
        }
        m_outputPending.push_back(qword);
    }
}

// PACK: an RGB32 macroblock (1024 bytes) to RGB16, or (OFM = 0) to 4-bit indices of the nearest VQ CLUT entry by
// squared 5:5:5 distance, first minimum wins, two pixels per byte with the even pixel in the low nibble (decision
// D-IPU-2, DobieStation process_PACK; single source).
void PS2Ipu::packMacroblock()
{
    const bool rgb16 = (m_option & (1u << 27)) != 0u;
    const bool dither = (m_option & (1u << 26)) != 0u;
    if (rgb16)
    {
        emitRgb16(m_rgbBlock.data(), dither);
        refillOutput();
        return;
    }
    const std::array<uint16_t, 256> rgb = toRgb16(m_rgbBlock.data(), dither);
    const auto nearest = [&](uint16_t color)
    {
        const int r = color & 0x1F, g = (color >> 5) & 0x1F, b = (color >> 10) & 0x1F;
        uint8_t best = 0u;
        int bestDistance = 1 << 30;
        for (uint8_t i = 0; i < 16u; ++i)
        {
            const int dr = r - (m_vqClut[i] & 0x1F), dg = g - ((m_vqClut[i] >> 5) & 0x1F), db = b - ((m_vqClut[i] >> 10) & 0x1F);
            const int distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance)
            {
                best = i;
                bestDistance = distance;
            }
        }
        return best;
    };
    for (uint32_t q = 0; q < 8u; ++q)
    {
        Qword qword{};
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const uint32_t pixel = q * 32u + i * 2u;
            qword[i] = static_cast<uint8_t>(nearest(rgb[pixel]) | (nearest(rgb[pixel + 1u]) << 4));
        }
        m_outputPending.push_back(qword);
    }
    refillOutput();
}

void PS2Ipu::finish()
{
    m_busy = false;
    m_decoding = false;
    m_completed = true;
}

bool PS2Ipu::writeCommand(uint32_t value)
{
    if (m_busy)
        return false;
    m_command = value >> 28;
    m_option = value & 0x0FFFFFFFu;
    m_errorCode = false;
    m_startCode = false;
    m_busy = true;
    m_advanced = false;
    switch (m_command)
    {
    case kBclr: // clear the input FIFO and set the bit pointer
        m_input.clear();
        m_bitPointer = m_option & 0x7Fu;
        finish();
        return true;
    case kFdec: // skip option bits 0-5, then the next 32 bits are the result (not consumed)
    case kVdec: // skip option bits 0-5, then one VLC symbol of table TBL (option bits 26-27)
        m_decoding = true;
        break;
    case kSetiq: // skip option bits 0-5, then 64 bytes into the intra (bit 27 = 0) or non-intra matrix
        m_remaining = 64u;
        break;
    case kSetvq: // 16 halfwords of the VQ CLUT
        m_remaining = 32u;
        break;
    case kCsc: // option bits 0-10 macroblock count, bit 26 dither (RGB16), bit 27 RGB16
    case kPack:
        m_macroblocksLeft = m_option & 0x7FFu;
        m_macroblockBytes = 0u;
        break;
    case kSetth: // TH0 bits 0-8, TH1 bits 16-24
        m_th0 = static_cast<uint16_t>(m_option & 0x1FFu);
        m_th1 = static_cast<uint16_t>((m_option >> 16) & 0x1FFu);
        finish();
        return true;
    default:
        m_unimplemented = static_cast<int>(m_command);
        finish();
        return true;
    }
    run();
    return true;
}

void PS2Ipu::writeControl(uint32_t value)
{
    m_controlFields = value & kControlWritable;
    if ((value & kControlReset) != 0u)
    {
        m_input.clear();
        m_output.clear();
        m_outputPending.clear();
        m_macroblockBytes = 0u;
        m_macroblocksLeft = 0u;
        m_bitPointer = 0u;
        m_command = 0u;
        m_busy = false;
        m_decoding = false;
        m_remaining = 0u;
    }
}

void PS2Ipu::run()
{
    if (!m_busy)
        return;
    switch (m_command)
    {
    case kFdec:
        if (!m_advanced)
        {
            if (!advanceBits(m_option & 0x3Fu))
                return;
            m_advanced = true;
        }
        if (peekBits(32u, m_result))
            finish();
        return;
    case kVdec:
    {
        if (!m_advanced)
        {
            if (!advanceBits(m_option & 0x3Fu))
                return;
            m_advanced = true;
        }
        // TBL 0 address increment, 1 macroblock type of the picture type in CTRL.PCT (1 I, 2 P, 3 B), 2 motion
        // code, 3 dmvector.
        VlcTable codes{nullptr, 0u};
        const uint32_t pictureType = (m_controlFields >> 24) & 0x7u;
        switch ((m_option >> 26) & 0x3u)
        {
        case 0u: codes = table(kMbai); break;
        case 1u:
            codes = pictureType == 1u ? table(kMbTypeI) : pictureType == 2u ? table(kMbTypeP)
                                                         : pictureType == 3u ? table(kMbTypeB) : VlcTable{nullptr, 0u};
            break;
        case 2u: codes = table(kMotionCode); break;
        default: codes = table(kDmVector); break;
        }
        if (codes.codes == nullptr)
        {
            m_errorCode = true; // no macroblock-type table for this picture type
            finish();
            return;
        }
        uint32_t maxLength = 0u;
        for (size_t i = 0; i < codes.count; ++i)
            maxLength = std::max<uint32_t>(maxLength, codes.codes[i].length);
        for (uint32_t length = 1u; length <= maxLength; ++length)
        {
            uint32_t bits = 0u;
            if (!peekBits(length, bits))
                return; // wait for more data
            for (size_t i = 0; i < codes.count; ++i)
            {
                if (codes.codes[i].length == length && codes.codes[i].code == bits)
                {
                    (void)advanceBits(length);
                    m_result = (length << 16) | static_cast<uint16_t>(codes.codes[i].value);
                    finish();
                    return;
                }
            }
        }
        m_errorCode = true; // not a code of the table
        finish();
        return;
    }
    case kSetiq:
        if (!m_advanced)
        {
            if (!advanceBits(m_option & 0x3Fu))
                return;
            m_advanced = true;
        }
        while (m_remaining != 0u)
        {
            uint32_t byte = 0u;
            if (!peekBits(8u, byte))
                return;
            (void)advanceBits(8u);
            const size_t index = 64u - m_remaining;
            ((m_option & (1u << 27)) != 0u ? m_nonIntraIq : m_intraIq)[index] = static_cast<uint8_t>(byte);
            --m_remaining;
        }
        finish();
        return;
    case kCsc:
        for (;;)
        {
            if (!m_outputPending.empty())
                return; // the previous macroblock is still entering the output FIFO
            if (m_macroblocksLeft == 0u)
            {
                finish();
                return;
            }
            while (m_macroblockBytes < m_macroblock.size())
            {
                uint32_t byte = 0u;
                if (!peekBits(8u, byte))
                    return;
                (void)advanceBits(8u);
                m_macroblock[m_macroblockBytes++] = static_cast<uint8_t>(byte);
            }
            convertMacroblock();
            m_macroblockBytes = 0u;
            --m_macroblocksLeft;
        }
    case kPack:
        for (;;)
        {
            if (!m_outputPending.empty())
                return;
            if (m_macroblocksLeft == 0u)
            {
                finish();
                return;
            }
            while (m_macroblockBytes < m_rgbBlock.size())
            {
                uint32_t byte = 0u;
                if (!peekBits(8u, byte))
                    return;
                (void)advanceBits(8u);
                m_rgbBlock[m_macroblockBytes++] = static_cast<uint8_t>(byte);
            }
            packMacroblock();
            m_macroblockBytes = 0u;
            --m_macroblocksLeft;
        }
    case kSetvq:
        while (m_remaining != 0u)
        {
            uint32_t half = 0u;
            if (!peekBits(16u, half))
                return;
            (void)advanceBits(16u);
            // CLUT entries are little-endian halfwords in memory order.
            m_vqClut[(32u - m_remaining) / 2u] = static_cast<uint16_t>(((half & 0xFFu) << 8) | (half >> 8));
            m_remaining -= 2u;
        }
        finish();
        return;
    default:
        return;
    }
}
