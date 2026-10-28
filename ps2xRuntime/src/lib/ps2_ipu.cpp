#include "runtime/ps2_ipu.h"

#include <algorithm>

namespace
{
    constexpr uint32_t kBclr = 0u;
    constexpr uint32_t kFdec = 4u;
    constexpr uint32_t kSetiq = 5u;
    constexpr uint32_t kSetvq = 6u;
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
        m_decoding = true;
        break;
    case kSetiq: // skip option bits 0-5, then 64 bytes into the intra (bit 27 = 0) or non-intra matrix
        m_remaining = 64u;
        break;
    case kSetvq: // 16 halfwords of the VQ CLUT
        m_remaining = 32u;
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
