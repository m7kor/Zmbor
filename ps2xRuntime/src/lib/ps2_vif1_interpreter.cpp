// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include <cstring>
#include <vector>
#include <algorithm>

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage || flg == 3u) // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

// VIFcodes whose data spans transfers (ps2autotests dma/vif/unpack "No data UNPACK": VPS reads 1 = waiting for data,
// and the FIFO, written one quadword at a time). Before, the incomplete command was dropped.
void PS2Memory::keepPartialVifCommand(uint32_t vif, const uint8_t *data, uint32_t cmdStart, uint32_t sizeBytes)
{
    std::vector<uint8_t> &partial = m_vifPartialCommand[vif & 1u];
    partial.assign(cmdStart & 15u, 0u); // zero words decode as NOPs and keep the quadword alignment
    partial.insert(partial.end(), data + cmdStart, data + sizeBytes);
    VIFRegisters &regs = vifRegs(vif);
    regs.stat = (regs.stat & ~3u) | 1u; // VPS: waiting for data
}

bool PS2Memory::continuePartialVifCommand(uint32_t vif, const uint8_t *&data, uint32_t &sizeBytes, std::vector<uint8_t> &joined)
{
    VIFRegisters &regs = vifRegs(vif);
    regs.stat &= ~3u; // VPS idle unless a command is left incomplete again
    std::vector<uint8_t> &partial = m_vifPartialCommand[vif & 1u];
    if (partial.empty())
        return false;
    joined.swap(partial);
    partial.clear();
    joined.insert(joined.end(), data, data + sizeBytes);
    data = joined.data();
    sizeBytes = static_cast<uint32_t>(joined.size());
    return true;
}

// UNPACK for either VIF: formats, S-x replication, STMASK, STMOD row add/accumulate and STCYCL skipping/filling
// write to that unit's VU data memory, wrapping at its size (VU0 4 KB, VU1 16 KB). VIF0 used a reduced copy that
// handled only 32-bit formats and never replicated S-32 (ps2autotests dma/vif/basic: VIF0 "S-32 1337c0de" read back
// y = 0). Returns the position after the unpack data.
uint32_t PS2Memory::unpackVif(uint32_t vif, uint8_t opcode, uint8_t num, uint16_t imm, const uint8_t *data, uint32_t pos, uint32_t sizeBytes)
{
    VIFRegisters &regs = vifRegs(vif);
    uint8_t *const vuData = vif != 0u ? m_vu1Data : m_vu0Data;
    const uint32_t vuDataSize = vif != 0u ? PS2_VU1_DATA_SIZE : PS2_VU0_DATA_SIZE;
    const uint32_t addressMask = vuDataSize / 16u - 1u;
    uint8_t vn = (opcode >> 2) & 0x3;
    uint8_t vl = opcode & 0x3;
    const bool maskEnable = (opcode & 0x10u) != 0u;
    int components = vn + 1;
    int bitsPerComponent = 32;
    switch (vl)
    {
    case 0:
        bitsPerComponent = 32;
        break;
    case 1:
        bitsPerComponent = 16;
        break;
    case 2:
        bitsPerComponent = 8;
        break;
    case 3:
        bitsPerComponent = (vn == 3) ? 4 : 16;
        break;
    default:
        break;
    }
    int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
    uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
    // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
    const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

    // STCYCL controls write cycles for UNPACK.
    // WL = 0 means 256 (ps2autotests dma/vif/stcycl "cl 1, wl 0": eight contiguous writes, columns 1 2 3 4 4 4 4 4).
    // CL = 0 behaves as CL = WL: a normal contiguous write ("cl 0, wl 4": columns repeating every 4; "cl 0, wl 0":
    // 1 2 3 4 4 4 4 4).
    uint32_t cl = regs.cycle & 0xFFu;
    uint32_t wl = (regs.cycle >> 8) & 0xFFu;
    if (wl == 0u)
        wl = 256u;
    if (cl == 0u)
        cl = wl;

    uint32_t sourceVectorCount = writeVectorCount;
    if (cl < wl)
    {
        const uint32_t fullBlocks = writeVectorCount / wl;
        uint32_t remainder = writeVectorCount % wl;
        if (remainder > cl)
            remainder = cl;
        sourceVectorCount = fullBlocks * cl + remainder;
    }

    uint32_t totalBytes = sourceVectorCount * bytesPerVector;
    totalBytes = (totalBytes + 3) & ~3u;

    uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
    if (vif != 0u && (imm & 0x8000u) != 0u) // FLG: double buffering, VIF1 only
        vuAddr = (vuAddr + (regs.tops & 0x3FFu)) & 0x3FFu;

    const bool zeroExtend = (imm & 0x4000u) != 0u;

    if (vuData && totalBytes > 0 && pos + totalBytes <= sizeBytes)
    {
        const uint8_t *srcBase = data + pos;
        uint32_t srcIndex = 0u;
        for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
        {
            const uint32_t cyclePos = writeIndex % wl;
            const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

            uint32_t destVec = 0;
            if (cl >= wl)
            {
                destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & addressMask;
            }
            else
            {
                destVec = (vuAddr + writeIndex) & addressMask;
            }

            uint32_t destOff = destVec * 16u;
            if (destOff + 16u > vuDataSize)
            {
                if (sourceAvailable && srcIndex < sourceVectorCount)
                    ++srcIndex;
                continue;
            }

            uint32_t lanes[4] = {0u, 0u, 0u, 0u};
            std::memcpy(lanes, vuData + destOff, sizeof(lanes));
            uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
            bool decoded = false;

            const uint8_t *srcVec = nullptr;
            if (sourceAvailable && srcIndex < sourceVectorCount)
            {
                srcVec = srcBase + srcIndex * bytesPerVector;
                ++srcIndex;
                decoded = true;
            }

            auto extend16 = [&](uint16_t raw) -> uint32_t
            {
                if (zeroExtend)
                    return static_cast<uint32_t>(raw);
                return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
            };

            auto extend8 = [&](uint8_t raw) -> uint32_t
            {
                if (zeroExtend)
                    return static_cast<uint32_t>(raw);
                return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
            };

            // dma/vif/unpack.expected: V2 writes x y x y; V3 takes w from the next element of the stream (decoded
            // like the others; 0 past the end of the data).
            auto completeVector = [&](uint32_t elementBytes)
            {
                if (components == 2)
                {
                    decompressed[2] = decompressed[0];
                    decompressed[3] = decompressed[1];
                }
                else if (components == 3)
                {
                    const uint8_t *next = srcVec + 3u * elementBytes;
                    uint32_t w = 0u;
                    if (next + elementBytes <= data + sizeBytes)
                    {
                        if (elementBytes == 4u)
                            std::memcpy(&w, next, sizeof(w));
                        else if (elementBytes == 2u)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, next, sizeof(raw));
                            w = extend16(raw);
                        }
                        else
                            w = extend8(next[0]);
                    }
                    decompressed[3] = w;
                }
            };

            bool handledFormat = true;
            if (!decoded)
            {
                handledFormat = false;
            }
            else if (vl == 0u)
            {
                if (components == 1)
                {
                    uint32_t scalar = 0;
                    std::memcpy(&scalar, srcVec, sizeof(scalar));
                    decompressed[0] = scalar;
                    decompressed[1] = scalar;
                    decompressed[2] = scalar;
                    decompressed[3] = scalar;
                }
                else
                {
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        decompressed[c] = scalar;
                    }
                    completeVector(4u);
                }
            }
            else if (vl == 1u)
            {
                if (components == 1)
                {
                    uint16_t raw = 0;
                    std::memcpy(&raw, srcVec, sizeof(raw));
                    const uint32_t scalar = extend16(raw);
                    decompressed[0] = scalar;
                    decompressed[1] = scalar;
                    decompressed[2] = scalar;
                    decompressed[3] = scalar;
                }
                else
                {
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint16_t raw = 0;
                        std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                        decompressed[c] = extend16(raw);
                    }
                    completeVector(2u);
                }
            }
            else if (vl == 2u)
            {
                if (components == 1)
                {
                    const uint32_t scalar = extend8(srcVec[0]);
                    decompressed[0] = scalar;
                    decompressed[1] = scalar;
                    decompressed[2] = scalar;
                    decompressed[3] = scalar;
                }
                else
                {
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        decompressed[c] = extend8(srcVec[c]);
                    }
                    completeVector(1u);
                }
            }
            else if (vl == 3u && vn == 3u)
            {
                // V4-5: packed color-like format in a single 16-bit value.
                uint16_t packed = 0;
                std::memcpy(&packed, srcVec, sizeof(packed));
                // Each 5-bit channel lands in the top of a byte and A in bit 7 (dma/vif/unpack "V4-5": 0xFFFF ->
                // f8 f8 f8 80).
                decompressed[0] = (packed & 0x1Fu) << 3;
                decompressed[1] = ((packed >> 5) & 0x1Fu) << 3;
                decompressed[2] = ((packed >> 10) & 0x1Fu) << 3;
                decompressed[3] = ((packed >> 15) & 0x01u) << 7;
            }
            else
            {
                handledFormat = false;
            }

            // Unknown compressed format fallback: preserve legacy raw-copy behavior.
            if (!handledFormat && decoded && !maskEnable && (regs.mode == 0u || regs.mode == 3u))
            {
                uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                std::memcpy(vuData + destOff, srcVec, copyBytes);
                continue;
            }

            const bool canAdd = (vl != 3u || vn != 3u);
            const uint32_t mode = regs.mode & 3u;
            const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
            const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

            for (uint32_t field = 0u; field < 4u; ++field)
            {
                uint32_t maskSpec = 0u;
                if (maskEnable)
                {
                    const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                    maskSpec = (regs.mask >> shift) & 0x3u;
                }

                // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                if (!decoded && maskSpec == 0u)
                    maskSpec = 1u;

                uint32_t writeVal = lanes[field];
                if (maskSpec == 0u)
                {
                    if (handledFormat)
                    {
                        writeVal = decompressed[field];
                        if (canAdd && (mode == 1u || mode == 2u))
                        {
                            writeVal = writeVal + regs.row[field];
                            if (mode == 2u)
                                regs.row[field] = writeVal;
                        }
                        else if (canAdd && mode == 3u)
                        {
                            // dma/vif/stmod "mode 3": the data is written unchanged and saved into ROW.
                            regs.row[field] = writeVal;
                        }
                    }
                }
                else if (maskSpec == 1u)
                {
                    writeVal = regs.row[field];
                }
                else if (maskSpec == 2u)
                {
                    writeVal = regs.col[colIdx];
                }
                else
                {
                    continue; // write-protect
                }

                lanes[field] = writeVal;
            }

            std::memcpy(vuData + destOff, lanes, sizeof(lanes));
        }
    }
    return pos + totalBytes;
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;
    if (bufferIfVifStalled(0u, data, sizeBytes))
        return;
    std::vector<uint8_t> joined;
    continuePartialVifCommand(0u, data, sizeBytes, joined);

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        if (stallVifBeforeNextCode(0u, data, pos, sizeBytes))
            return;

        const uint32_t cmdStart = pos;
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            takeVifInterruptBit(0u, cmd);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
            {
                keepPartialVifCommand(0u, data, cmdStart, sizeBytes);
                return;
            }
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
            {
                keepPartialVifCommand(0u, data, cmdStart, sizeBytes);
                return;
            }
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
            {
                keepPartialVifCommand(0u, data, cmdStart, sizeBytes);
                return;
            }
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (pos + mpgBytes > sizeBytes)
            {
                keepPartialVifCommand(0u, data, cmdStart, sizeBytes);
                return;
            }
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            // Same unpacker as VIF1 (formats, S replication, mask, STMOD, STCYCL), on VU0 data memory.
            const uint32_t next = unpackVif(0u, opcode, num, imm, data, pos, sizeBytes);
            if (next > sizeBytes)
            {
                keepPartialVifCommand(0u, data, cmdStart, sizeBytes);
                return;
            }
            pos = next;
            continue;
        }
        else
        {
            break;
        }
    }
    finishVifCommandStall(0u);
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

// Sends one DIRECT/DIRECTHL payload to GIF PATH2. If PATH2 stopped inside an IMAGE packet, the first qwords of this
// payload are that image's data (re-tagged, since each submitted packet is parsed from a GIFtag); the rest is parsed
// as GIF packets again. VIFcodes between two DIRECTs are never image data (PCSX2 vifCode_Direct / GIF path state).
void PS2Memory::submitPath2DirectPayload(const uint8_t *payload, uint32_t qwCount, bool directHl)
{
    uint32_t imageQw = 0u;
    if (m_vif1PendingPath2ImageQwc != 0u)
    {
        imageQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, qwCount);
        std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(imageQw) * 16u, 0u);
        const uint64_t imageTag =
            static_cast<uint64_t>(imageQw & 0x7FFFu) |
            ((m_vif1PendingPath2ImageQwc == imageQw) ? (1ull << 15) : 0ull) |
            (static_cast<uint64_t>(kGifFmtImage) << 58);
        std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
        std::memcpy(imagePacket.data() + 16u, payload, static_cast<size_t>(imageQw) * 16u);
        submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl);
        m_vif1PendingPath2ImageQwc -= imageQw;
        if (m_vif1PendingPath2ImageQwc == 0u)
            m_vif1PendingPath2DirectHl = false;
    }

    if (imageQw < qwCount)
    {
        const uint8_t *packets = payload + static_cast<size_t>(imageQw) * 16u;
        const uint32_t packetBytes = (qwCount - imageQw) * 16u;
        submitGifPacket(GifPathId::Path2, packets, packetBytes, true, directHl);

        const uint32_t pendingImageQw = pendingGifImageQwc(packets, packetBytes);
        if (pendingImageQw != 0u)
        {
            m_vif1PendingPath2ImageQwc = pendingImageQw;
            m_vif1PendingPath2DirectHl = directHl;
        }
    }
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;
    if (bufferIfVifStalled(1u, data, sizeBytes))
        return;
    std::vector<uint8_t> joined;
    continuePartialVifCommand(1u, data, sizeBytes, joined);

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingDirectQwc != 0u)
        {
            // The DIRECT that ended the previous data is still active; this data continues its payload.
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }
            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingDirectQwc, availableQw);
            submitPath2DirectPayload(data + pos, chunkQw, m_vif1PendingDirectHl);
            pos += chunkQw * 16u;
            m_vif1PendingDirectQwc -= chunkQw;
            continue;
        }

        // An I-bit command has finished (including a DIRECT that continued from an earlier transfer).
        if (stallVifBeforeNextCode(1u, data, pos, sizeBytes))
            return;

        const uint32_t cmdStart = pos;
        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            takeVifInterruptBit(1u, cmd);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
            {
                keepPartialVifCommand(1u, data, cmdStart, sizeBytes);
                return;
            }
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
            {
                keepPartialVifCommand(1u, data, cmdStart, sizeBytes);
                return;
            }
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
            {
                keepPartialVifCommand(1u, data, cmdStart, sizeBytes);
                return;
            }
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (pos + mpgBytes > sizeBytes)
            {
                keepPartialVifCommand(1u, data, cmdStart, sizeBytes);
                return;
            }
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            const bool directHl = (opcode == VIF_DIRECTHL);
            if (qwCount > 0)
            {
                submitPath2DirectPayload(data + pos, qwCount, directHl);
            }

            pos += qwCount * 16;
            if (truncated)
            {
                m_vif1PendingDirectQwc = ((imm == 0u) ? 65536u : imm) - qwCount;
                m_vif1PendingDirectHl = directHl;
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            const uint32_t next = unpackVif(1u, opcode, num, imm, data, pos, sizeBytes);
            if (next > sizeBytes)
            {
                keepPartialVifCommand(1u, data, cmdStart, sizeBytes);
                // dma/vif/unpack "No data UNPACK(NUM: 256)": VIF1_NUM reads 256 while waiting, VIF0_NUM (8 bits) 0.
                if (num == 0u)
                    vif1_regs.num = 256u;
                return;
            }
            pos = next;
            continue;
        }
        else
        {
            continue;
        }
    }
    if (m_vif1PendingDirectQwc == 0u)
        finishVifCommandStall(1u);
}

// ---------------------------------------------------------------------------------------------------------------
// VIF stalls (VSS/VFS/VIS). PCSX2 Vif_Transfer.cpp stops the VIFcode loop once an I-bit command has finished;
// Vif0_Dma.cpp/Vif1_Dma.cpp then set INT (unless ER1), raise INTC VIF0/VIF1, set VIS unless the code was MARK, and
// stall while VSS|VIS|VFS is set. Vif.cpp vifNFBRST: STC clears VSS|VFS|VIS|INT|ER0|ER1 and resumes a stalled VIF.

namespace
{
    constexpr uint32_t kVifStatVss = 1u << 8;
    constexpr uint32_t kVifStatVfs = 1u << 9;
    constexpr uint32_t kVifStatVis = 1u << 10;
    constexpr uint32_t kVifStatInt = 1u << 11;
    constexpr uint32_t kVifStatEr0 = 1u << 12;
    constexpr uint32_t kVifStatEr1 = 1u << 13;
    constexpr uint32_t kVifErrMii = 1u << 0;
}

bool PS2Memory::isVifStalled(uint32_t vif) const noexcept
{
    const VIFRegisters &regs = (vif & 1u) ? vif1_regs : vif0_regs;
    return (regs.stat & (kVifStatVss | kVifStatVfs | kVifStatVis)) != 0u;
}

bool PS2Memory::bufferIfVifStalled(uint32_t vif, const uint8_t *data, uint32_t sizeBytes)
{
    if (!isVifStalled(vif))
        return false;
    std::vector<uint8_t> &held = m_vifStalledData[vif & 1u];
    held.insert(held.end(), data, data + sizeBytes);
    return true;
}

void PS2Memory::takeVifInterruptBit(uint32_t vif, uint32_t cmd)
{
    VIFRegisters &regs = vifRegs(vif);
    if ((regs.err & kVifErrMii) != 0u)
        return; // ERR.MII masks the I bit entirely
    if ((regs.stat & kVifStatEr1) == 0u)
        regs.stat |= kVifStatInt;
    raiseVifInterrupt(vif);
    if (((cmd >> 24) & 0x7Fu) != VIF_MARK)
        m_vifIrqStallPending[vif & 1u] = true;
}

bool PS2Memory::stallVifBeforeNextCode(uint32_t vif, const uint8_t *data, uint32_t pos, uint32_t sizeBytes)
{
    if (!m_vifIrqStallPending[vif & 1u])
        return false;
    m_vifIrqStallPending[vif & 1u] = false;
    // dma/vif/intstall: STAT reads VIS | INT | VPS=2 (the next code is held, decoded) after an I-bit NOP/STMASK.
    vifRegs(vif).stat = (vifRegs(vif).stat & ~3u) | kVifStatVis | 2u;
    std::vector<uint8_t> &held = m_vifStalledData[vif & 1u];
    held.insert(held.end(), data + pos, data + sizeBytes);
    return true;
}

void PS2Memory::finishVifCommandStall(uint32_t vif)
{
    if (m_vifIrqStallPending[vif & 1u])
    {
        m_vifIrqStallPending[vif & 1u] = false;
        vifRegs(vif).stat |= kVifStatVis;
    }
}

void PS2Memory::writeVifFbrst(uint32_t vif, uint32_t value)
{
    VIFRegisters &regs = vifRegs(vif);
    if (value & 0x2u) // FBK
        regs.stat |= kVifStatVfs;
    if (value & 0x4u) // STP
        regs.stat |= kVifStatVss;
    if (value & 0x8u) // STC
    {
        const bool cancel = isVifStalled(vif);
        regs.stat &= ~(kVifStatVss | kVifStatVfs | kVifStatVis | kVifStatInt | kVifStatEr0 | kVifStatEr1);
        if (cancel)
        {
            regs.stat &= ~3u;
            resumeVifAfterStall(vif);
        }
    }
    if (value & 0x1u) // RST: drop whatever the stall was holding; a held DMA has nothing left to wait for
    {
        m_vifStalledData[vif & 1u].clear();
        m_vifPartialCommand[vif & 1u].clear();
        regs.stat &= ~3u; // VPS idle
        m_vifIrqStallPending[vif & 1u] = false;
        if (m_vifDmaHeld[vif & 1u])
        {
            m_vifDmaHeld[vif & 1u] = false;
            completeVifDma(vif);
        }
    }
}

void PS2Memory::resumeVifAfterStall(uint32_t vif)
{
    std::vector<uint8_t> held;
    held.swap(m_vifStalledData[vif & 1u]);
    if (!held.empty())
    {
        if (vif & 1u)
            processVIF1Data(held.data(), static_cast<uint32_t>(held.size()));
        else
            processVIF0Data(held.data(), static_cast<uint32_t>(held.size()));
        if (m_gifArbiter)
            m_gifArbiter->drain();
    }
    if (m_vifDmaHeld[vif & 1u] && m_vifStalledData[vif & 1u].empty())
    {
        m_vifDmaHeld[vif & 1u] = false;
        completeVifDma(vif);
    }
}

void PS2Memory::finishVifDma(uint32_t vif)
{
    // Data held behind a stall sits in the VIF FIFO (EE manual: VIF0 8 qwords, VIF1 16), so the DMA completes when
    // it fits (ps2autotests dma/vif/intstall: a 3-qword packet stalled by an I bit completes, no timeout). Only data
    // the FIFO cannot take keeps the DMA active until the stall releases it.
    const size_t fifoBytes = (vif & 1u) ? 16u * 16u : 8u * 16u;
    // A VIF1 MFIFO drain stopped at the empty ring stays active until fromSPR adds data (resumeMfifoDrain).
    if ((vif & 1u) != 0u && m_mfifoDrainStalled && mfifoDrainChannel() == 0x10009000u)
        return;
    if (isVifStalled(vif) && m_vifStalledData[vif & 1u].size() > fifoBytes)
        m_vifDmaHeld[vif & 1u] = true;
    else
        completeVifDma(vif);
}

uint32_t PS2Memory::readVifRegister(uint32_t vif, uint32_t offset) const
{
    const VIFRegisters &regs = (vif & 1u) ? vif1_regs : vif0_regs;
    switch (offset)
    {
    case 0x00: return regs.stat;
    case 0x10: return 0u; // FBRST is write-only
    case 0x20: return regs.err;
    case 0x30: return regs.mark;
    case 0x40: return regs.cycle;
    case 0x50: return regs.mode;
    case 0x60: return regs.num;
    case 0x70: return regs.mask;
    case 0x80: return regs.code;
    case 0x90: return regs.itops;
    case 0xA0: return regs.base;
    case 0xB0: return regs.ofst;
    case 0xC0: return regs.tops;
    case 0xD0: return regs.itop;
    case 0xE0: return regs.top;
    case 0x100: case 0x110: case 0x120: case 0x130: return regs.row[(offset - 0x100u) >> 4];
    case 0x140: case 0x150: case 0x160: case 0x170: return regs.col[(offset - 0x140u) >> 4];
    default: return 0u;
    }
}
