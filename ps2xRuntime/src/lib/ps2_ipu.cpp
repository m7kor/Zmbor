#include "runtime/ps2_ipu.h"
#include "runtime/ps2_ipu_mpeg_tables.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr uint32_t kBclr = 0u;
    constexpr uint32_t kFdec = 4u;
    constexpr uint32_t kSetiq = 5u;
    constexpr uint32_t kSetvq = 6u;
    constexpr uint32_t kCsc = 7u;
    constexpr uint32_t kPack = 8u;
    constexpr uint32_t kBdec = 2u;
    // Scan position -> raster index (ISO/IEC 13818-2 figures 7-2 and 7-3).
    constexpr uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
                                     41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22,
                                     15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55,
                                     62, 63};
    constexpr uint8_t kAlternate[64] = {0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3,
                                        11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5, 13, 6, 14, 21,
                                        29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39,
                                        47, 55, 63};
    // quantiser_scale for quantiser_scale_code with q_scale_type 1 (table 7-6).
    constexpr uint8_t kNonLinearScale[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22,
                                             24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

    // Finds the code of `table` at the front of the bitstream: 1 found, 0 more data needed, -1 not a code.
    template <typename Entry, size_t N, typename Peek>
    int matchCode(const Entry (&table)[N], Peek peek, const Entry *&found)
    {
        uint32_t maxLength = 0u;
        for (const Entry &entry : table)
            maxLength = std::max<uint32_t>(maxLength, entry.length);
        for (uint32_t length = 1u; length <= maxLength; ++length)
        {
            uint32_t bits = 0u;
            if (!peek(length, bits))
                return 0;
            for (const Entry &entry : table)
            {
                if (entry.length == length && entry.code == bits)
                {
                    found = &entry;
                    return 1;
                }
            }
        }
        return -1;
    }

    // basis[x * 8 + u] = C(u) / 2 * cos((2x + 1) u pi / 16): the separable 8x8 inverse DCT of 13818-2 Annex A.
    const std::array<double, 64> &idctBasis()
    {
        static const std::array<double, 64> basis = []
        {
            std::array<double, 64> b{};
            const double pi = std::acos(-1.0);
            for (uint32_t x = 0; x < 8u; ++x)
                for (uint32_t u = 0; u < 8u; ++u)
                    b[x * 8u + u] = (u == 0u ? std::sqrt(0.5) : 1.0) * std::cos((2.0 * x + 1.0) * u * pi / 16.0) / 2.0;
            return b;
        }();
        return basis;
    }
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
    value |= (m_codedBlockPattern & 0x3Fu) << 8;
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

// One AC coefficient (or EOB) of B-14/B-15. Codes are matched without their sign bit, and a symbol is consumed only
// once all of its bits (sign, escape run and level) are in the FIFO.
int PS2Ipu::decodeCoefficient(bool &endOfBlock)
{
    const bool intra = (m_option & (1u << 27)) != 0u;
    const bool mpeg1 = (m_controlFields & (1u << 23)) != 0u;
    const bool tableB15 = intra && !mpeg1 && (m_controlFields & (1u << 21)) != 0u;
    const auto peek = [this](uint32_t count, uint32_t &value) { return peekBits(count, value); };
    uint32_t bits = 0u;
    if (!intra && m_bdec.index == 0u)
    {
        // The first coefficient of a non-intra block: '1s' is run 0, level 1 (and EOB cannot occur).
        if (!peekBits(1u, bits))
            return 0;
        if (bits == 1u)
        {
            if (!peekBits(2u, bits))
                return 0;
            (void)advanceBits(2u);
            m_bdec.coefficients[m_bdec.index++] = (bits & 1u) != 0u ? -1 : 1;
            return 1;
        }
    }
    else
    {
        const uint32_t eobLength = tableB15 ? 4u : 2u;
        const uint32_t eobCode = tableB15 ? 0b0110u : 0b10u;
        if (peekBits(eobLength, bits) && bits == eobCode)
        {
            (void)advanceBits(eobLength);
            endOfBlock = true;
            return 1;
        }
    }
    const ps2_ipu_tables::DctCode *code = nullptr;
    const int found = tableB15 ? matchCode(ps2_ipu_tables::kDctTableB15, peek, code)
                               : matchCode(ps2_ipu_tables::kDctTableB14, peek, code);
    if (found <= 0)
        return found;
    int32_t run = 0;
    int32_t level = 0;
    uint32_t total = code->length + 1u;
    if (code->run >= 0)
    {
        if (!peekBits(total, bits))
            return 0;
        run = code->run;
        level = (bits & 1u) != 0u ? -static_cast<int32_t>(code->level) : static_cast<int32_t>(code->level);
    }
    else if (!mpeg1)
    {
        // Escape: 6-bit run, 12-bit two's complement level.
        total = code->length + 18u;
        if (!peekBits(total, bits))
            return 0;
        run = static_cast<int32_t>((bits >> 12) & 0x3Fu);
        level = static_cast<int32_t>(bits & 0xFFFu) - ((bits & 0x800u) != 0u ? 0x1000 : 0);
    }
    else
    {
        // MPEG-1 escape: 6-bit run, 8-bit level; 0x00 and 0x80 announce a second byte (levels 128..255, -256..-129).
        total = code->length + 14u;
        if (!peekBits(total, bits))
            return 0;
        run = static_cast<int32_t>((bits >> 8) & 0x3Fu);
        const int32_t first = static_cast<int32_t>(bits & 0xFFu) - ((bits & 0x80u) != 0u ? 0x100 : 0);
        level = first;
        if (first == 0 || first == -128)
        {
            total += 8u;
            if (!peekBits(total, bits))
                return 0;
            level = static_cast<int32_t>(bits & 0xFFu) - (first == 0 ? 0 : 256);
        }
    }
    (void)advanceBits(total);
    m_bdec.index += static_cast<uint32_t>(run);
    if (m_bdec.index > 63u)
        return -1;
    m_bdec.coefficients[m_bdec.index++] = level;
    return 1;
}

// Inverse scan, inverse quantisation (13818-2 7.4; 11172-2 2.4.4 with CTRL.MP1), then the inverse DCT (D-IPU-3).
void PS2Ipu::reconstructBlock()
{
    const bool intra = (m_option & (1u << 27)) != 0u;
    const bool mpeg1 = (m_controlFields & (1u << 23)) != 0u;
    const bool alternate = (m_controlFields & (1u << 20)) != 0u;
    const bool nonLinear = !mpeg1 && (m_controlFields & (1u << 22)) != 0u;
    const uint32_t precision = (m_controlFields >> 16) & 0x3u;
    const uint32_t scaleCode = (m_option >> 16) & 0x1Fu;
    // MPEG-1's quantizer_scale with its /16 is the same as twice the code with MPEG-2's /32.
    const int32_t scale = nonLinear ? kNonLinearScale[scaleCode] : static_cast<int32_t>(2u * scaleCode);
    const std::array<uint8_t, 64> &matrix = intra ? m_intraIq : m_nonIntraIq;
    std::array<int32_t, 64> weight{};
    for (uint32_t k = 0; k < 64u; ++k)
        weight[kZigzag[k]] = matrix[k]; // matrices are loaded in zigzag order whatever the scan
    const uint8_t *scan = alternate ? kAlternate : kZigzag;
    std::array<int32_t, 64> f{};
    int32_t sum = 0;
    for (uint32_t i = 0; i < 64u; ++i)
    {
        const int32_t qf = m_bdec.coefficients[i];
        const uint32_t p = scan[i];
        int32_t value = 0;
        if (intra && i == 0u)
            value = qf * static_cast<int32_t>(8u >> precision);
        else if (qf != 0)
        {
            const int32_t sign = qf > 0 ? 1 : -1;
            value = ((2 * qf + (intra ? 0 : sign)) * weight[p] * scale) / 32;
            if (mpeg1 && (value & 1) == 0 && value != 0)
                value -= value > 0 ? 1 : -1; // oddification
        }
        value = std::max(-2048, std::min(2047, value));
        f[p] = value;
        sum += value;
    }
    if (!mpeg1 && (sum & 1) == 0)
        f[63] += (f[63] & 1) != 0 ? -1 : 1; // mismatch control
    const std::array<double, 64> &basis = idctBasis();
    std::array<double, 64> rows{};
    for (uint32_t v = 0; v < 8u; ++v)
        for (uint32_t x = 0; x < 8u; ++x)
        {
            double acc = 0.0;
            for (uint32_t u = 0; u < 8u; ++u)
                acc += basis[x * 8u + u] * f[v * 8u + u];
            rows[v * 8u + x] = acc;
        }
    std::array<int16_t, 64> &out = m_bdec.blocks[m_bdec.block];
    for (uint32_t y = 0; y < 8u; ++y)
        for (uint32_t x = 0; x < 8u; ++x)
        {
            double acc = 0.0;
            for (uint32_t v = 0; v < 8u; ++v)
                acc += basis[y * 8u + v] * rows[v * 8u + x];
            int32_t sample = std::max(-256, std::min(255, static_cast<int32_t>(std::floor(acc + 0.5))));
            if (intra)
                sample = std::max(0, sample);
            out[y * 8u + x] = static_cast<int16_t>(sample);
        }
}

// RAW16: Y 16x16 (frame blocks in quadrants, field blocks on alternate lines with option DT), Cb 8x8, Cr 8x8.
void PS2Ipu::emitRaw16()
{
    const bool field = (m_option & (1u << 25)) != 0u;
    std::array<int16_t, 384> raw{};
    for (uint32_t b = 0; b < 4u; ++b)
        for (uint32_t r = 0; r < 8u; ++r)
        {
            const uint32_t line = field ? (b >> 1) + 2u * r : (b >> 1) * 8u + r;
            for (uint32_t c = 0; c < 8u; ++c)
                raw[line * 16u + (b & 1u) * 8u + c] = m_bdec.blocks[b][r * 8u + c];
        }
    std::copy(m_bdec.blocks[4].begin(), m_bdec.blocks[4].end(), raw.begin() + 256);
    std::copy(m_bdec.blocks[5].begin(), m_bdec.blocks[5].end(), raw.begin() + 320);
    for (uint32_t q = 0; q < 48u; ++q)
    {
        Qword qword{};
        for (uint32_t i = 0; i < 8u; ++i)
        {
            const uint16_t sample = static_cast<uint16_t>(raw[q * 8u + i]);
            qword[i * 2u] = static_cast<uint8_t>(sample);
            qword[i * 2u + 1u] = static_cast<uint8_t>(sample >> 8);
        }
        m_outputPending.push_back(qword);
    }
    refillOutput();
}

bool PS2Ipu::decodeBlocks()
{
    const bool intra = (m_option & (1u << 27)) != 0u;
    const auto peek = [this](uint32_t count, uint32_t &value) { return peekBits(count, value); };
    const auto fail = [this]
    {
        m_errorCode = true;
        finish();
        return true;
    };
    if (m_bdec.stage == 0u)
    {
        if (!advanceBits(m_option & 0x3Fu))
            return false;
        m_bdec.stage = 1u;
    }
    if (m_bdec.stage == 1u)
    {
        if (intra)
            m_codedBlockPattern = 0x3Fu;
        else
        {
            const ps2_ipu_tables::CodeValue *code = nullptr;
            const int found = matchCode(ps2_ipu_tables::kCodedBlockPattern, peek, code);
            if (found == 0)
                return false;
            if (found < 0)
                return fail();
            (void)advanceBits(code->length);
            m_codedBlockPattern = code->value;
        }
        m_bdec.stage = 2u;
    }
    while (m_bdec.stage == 2u)
    {
        if (m_bdec.block == 6u)
        {
            emitRaw16();
            m_bdec.stage = 3u;
            break;
        }
        if (((m_codedBlockPattern >> (5u - m_bdec.block)) & 1u) != 0u)
        {
            if (intra && !m_bdec.dcDone)
            {
                const ps2_ipu_tables::CodeValue *size = nullptr;
                const int found = m_bdec.block < 4u ? matchCode(ps2_ipu_tables::kDcLuminance, peek, size)
                                                    : matchCode(ps2_ipu_tables::kDcChrominance, peek, size);
                if (found == 0)
                    return false;
                if (found < 0)
                    return fail();
                uint32_t bits = 0u;
                if (!peekBits(size->length + size->value, bits))
                    return false;
                (void)advanceBits(size->length + size->value);
                int32_t differential = 0;
                if (size->value != 0u)
                {
                    // dct_dc_differential: a leading 0 marks a negative value (13818-2 7.2.1).
                    const int32_t raw = static_cast<int32_t>(bits & ((1u << size->value) - 1u));
                    const int32_t half = 1 << (size->value - 1u);
                    differential = raw >= half ? raw : raw - (2 * half - 1);
                }
                const uint32_t component = m_bdec.block < 4u ? 0u : m_bdec.block - 3u;
                m_dcPredictor[component] += differential;
                m_bdec.coefficients[0] = m_dcPredictor[component];
                m_bdec.index = 1u;
                m_bdec.dcDone = true;
            }
            for (;;)
            {
                bool endOfBlock = false;
                const int decoded = decodeCoefficient(endOfBlock);
                if (decoded == 0)
                    return false;
                if (decoded < 0)
                    return fail();
                if (endOfBlock)
                    break;
            }
            reconstructBlock();
        }
        else
            m_bdec.blocks[m_bdec.block].fill(0);
        ++m_bdec.block;
        m_bdec.coefficients.fill(0);
        m_bdec.index = 0u;
        m_bdec.dcDone = false;
    }
    if (m_outputPending.empty())
        finish(); // the whole macroblock has entered the output FIFO
    return true;
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
    case kBdec: // FB 0-5, QSC 16-20, DT 25, DCR 26 (reset the DC predictors), MBI 27 (intra)
        m_bdec = BdecState{};
        if ((m_option & (1u << 26)) != 0u)
            m_dcPredictor.fill(128 << ((m_controlFields >> 16) & 0x3u));
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
    case kBdec:
        (void)decodeBlocks();
        return;
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
