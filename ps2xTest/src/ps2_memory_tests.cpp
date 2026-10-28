#include "MiniTest.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "ps2_runtime_macros.h"
#include "Stubs/DMA.h"
#include "Stubs/GS.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) |
               (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
    }

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void appendU32(std::vector<uint8_t> &dst, uint32_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint32_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint32_t));
    }

    void appendU64(std::vector<uint8_t> &dst, uint64_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint64_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint64_t));
    }

    uint64_t makeDmaTag(uint16_t qwc, uint8_t id, uint32_t addr, bool irq = false)
    {
        return static_cast<uint64_t>(qwc) |
               (static_cast<uint64_t>(id & 0x7u) << 28) |
               (irq ? (1ull << 31) : 0ull) |
               (static_cast<uint64_t>(addr & 0x7FFFFFFFu) << 32);
    }

    void writeDmaTag(uint8_t *rdram, uint32_t tagAddr, uint64_t tagLo)
    {
        std::memset(rdram + tagAddr, 0, 16);
        std::memcpy(rdram + tagAddr, &tagLo, sizeof(tagLo));
    }

    struct MfifoRig
    {
        static constexpr uint32_t kRing = 0x00100000u;
        static constexpr uint32_t kGif = 0x1000A000u;
        static constexpr uint32_t kSprFrom = 0x1000D000u;
        static constexpr uint32_t kDStat = 0x1000E010u;
        // received outlives mem: the GIF callback writes into it.
        std::vector<uint8_t> received;
        PS2Memory mem;
        bool ok = false;

        MfifoRig(uint32_t ringMask, uint32_t mfd = 3u)
        {
            ok = mem.initialize();
            mem.setGifPacketCallback([this](const uint8_t *data, uint32_t size) { received.insert(received.end(), data, data + size); });
            mem.writeIORegister(0x1000E050u, kRing);    // D_RBOR
            mem.writeIORegister(0x1000E040u, ringMask); // D_RBSR
            mem.writeIORegister(0x1000E000u, 1u | (mfd << 2u));
            mem.writeIORegister(kSprFrom + 0x10u, kRing); // fromSPR MADR: the write position starts at the base
        }
        // Copies qwords to the scratchpad at offset 0 and runs fromSPR (normal mode) from there.
        void fromSpr(const std::vector<uint64_t> &qwordHalves)
        {
            std::memcpy(mem.getScratchpad(), qwordHalves.data(), qwordHalves.size() * 8u);
            mem.writeIORegister(kSprFrom + 0x80u, 0u);
            mem.writeIORegister(kSprFrom + 0x20u, static_cast<uint32_t>(qwordHalves.size() / 2u));
            mem.writeIORegister(kSprFrom + 0x00u, 0x100u);
        }
        void startGifChain(uint32_t tadr)
        {
            mem.writeIORegister(kGif + 0x30u, tadr);
            mem.writeIORegister(kGif + 0x20u, 0u);
            mem.writeIORegister(kGif + 0x00u, 0x105u);
        }
        bool gifActive() { return (mem.readIORegister(kGif) & 0x100u) != 0u; }
        uint32_t dstat() { return mem.readIORegister(kDStat); }
    };

    // Stall-control tests: D_STADR, the drain's data and its chain.
    constexpr uint32_t kDStadr = 0x1000E060u;
    constexpr uint32_t kStallData = 0x00300000u;
    constexpr uint32_t kStallChain = 0x00200000u;
    constexpr uint32_t kSis = 1u << 13;

    void writeU64(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop);

    uint64_t makeBitbltbuf(uint32_t dbp, uint32_t dbw, uint32_t dpsm)
    {
        return (static_cast<uint64_t>(dbp & 0x3FFFu) << 32) |
               (static_cast<uint64_t>(dbw & 0x3Fu) << 48) |
               (static_cast<uint64_t>(dpsm & 0x3Fu) << 56);
    }

    uint32_t writeGifAd(uint8_t *rdram, uint32_t addr, uint64_t value, uint64_t reg)
    {
        writeU64(rdram, addr + 0u, value);
        writeU64(rdram, addr + 8u, reg);
        return addr + 16u;
    }

    uint32_t writeTextureUploadSetup(uint8_t *rdram, uint32_t addr, uint32_t dbp, uint32_t dpsm)
    {
        writeDmaTag(rdram, addr, makeDmaTag(5u, 1u, 0u, false)); // CNT: setup tag + four A+D writes.
        addr += 16u;
        writeU64(rdram, addr + 0u, makeGifTag(4u, GIF_FMT_PACKED, 1u, false));
        writeU64(rdram, addr + 8u, 0x0Eull); // GIF PACKED A+D descriptor.
        addr += 16u;
        addr = writeGifAd(rdram, addr, makeBitbltbuf(dbp, 1u, dpsm), GS_REG_BITBLTBUF);
        addr = writeGifAd(rdram, addr, 0ull, GS_REG_TRXPOS);
        addr = writeGifAd(rdram, addr, (16ull << 0) | (16ull << 32), GS_REG_TRXREG);
        addr = writeGifAd(rdram, addr, 0ull, GS_REG_TRXDIR);
        return addr;
    }

    uint32_t writeTextureImageRef(uint8_t *rdram, uint32_t addr, uint32_t qwc, uint32_t dataAddr)
    {
        writeDmaTag(rdram, addr, makeDmaTag(1u, 1u, 0u, false)); // CNT: GIF IMAGE tag.
        addr += 16u;
        writeU64(rdram, addr + 0u, makeGifTag(static_cast<uint16_t>(qwc), GIF_FMT_IMAGE, 0u, false));
        writeU64(rdram, addr + 8u, 0ull);
        addr += 16u;
        writeDmaTag(rdram, addr, makeDmaTag(static_cast<uint16_t>(qwc), 3u, dataAddr, false)); // REF: image payload.
        return addr + 16u;
    }

    uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop = true)
    {
        uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu);
        if (eop)
            tag |= (1ull << 15);
        tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
        tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
        return tag;
    }

    uint64_t makeGifTagPrim(uint16_t nloop, uint16_t prim, uint8_t flg, uint8_t nreg, bool eop = true, bool pre = true)
    {
        uint64_t tag = makeGifTag(nloop, flg, nreg, eop);
        if (pre)
            tag |= (1ull << 46);
        tag |= (static_cast<uint64_t>(prim & 0x7FFu) << 47);
        return tag;
    }

    uint64_t makeGsFrame(uint32_t fbp, uint32_t fbw, uint32_t psm, uint32_t mask = 0u)
    {
        return static_cast<uint64_t>(fbp & 0x1FFu) |
               (static_cast<uint64_t>(fbw & 0x3Fu) << 16u) |
               (static_cast<uint64_t>(psm & 0x3Fu) << 24u) |
               (static_cast<uint64_t>(mask) << 32u);
    }

    uint64_t makeGsScissor(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
    {
        return static_cast<uint64_t>(x0 & 0x7FFu) |
               (static_cast<uint64_t>(x1 & 0x7FFu) << 16u) |
               (static_cast<uint64_t>(y0 & 0x7FFu) << 32u) |
               (static_cast<uint64_t>(y1 & 0x7FFu) << 48u);
    }

    void appendPackedRgbaq(std::vector<uint8_t> &packet, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        appendU64(packet, static_cast<uint64_t>(r) | (static_cast<uint64_t>(g) << 32u));
        appendU64(packet, static_cast<uint64_t>(b) | (static_cast<uint64_t>(a) << 32u));
    }

    void appendPackedXyzf2(std::vector<uint8_t> &packet, uint32_t x, uint32_t y, uint32_t z)
    {
        appendU64(packet, static_cast<uint64_t>(x & 0xFFFFu) | (static_cast<uint64_t>(y & 0xFFFFu) << 32u));
        appendU64(packet, static_cast<uint64_t>(z & 0xFFFFFFu) << 4u);
    }

    void appendPackedUv(std::vector<uint8_t> &packet, uint32_t u, uint32_t v)
    {
        appendU64(packet, static_cast<uint64_t>(u & 0x3FFFu) | (static_cast<uint64_t>(v & 0x3FFFu) << 32u));
        appendU64(packet, 0u);
    }

}

void register_ps2_memory_tests()
{
    MiniTest::Case("PS2Memory", [](TestCase &tc)
    {
        tc.Run("uncached aliases map to same RDRAM bytes", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.write32(0x00001000u, 0xDEADBEEFu);
            t.Equals(mem.read32(0x00001000u), 0xDEADBEEFu, "base readback should match");
            t.Equals(mem.read32(0x20001000u), 0xDEADBEEFu, "0x2000_0000 alias should map to RDRAM");

            // 0x3010_0000 maps to physical 0x0010_0000 (AboutPS2 memory map).
            mem.write32(0x00101000u, 0xDEADBEEFu);
            t.Equals(mem.read32(0x30101000u), 0xDEADBEEFu, "0x3010_0000 accelerated alias should map to RDRAM");

            mem.write32(0x20002000u, 0x13579BDFu);
            t.Equals(mem.read32(0x00002000u), 0x13579BDFu, "writes through 0x2000 alias should land in base RDRAM");

            mem.write32(0x30103000u, 0x2468ACE0u);
            t.Equals(mem.read32(0x00103000u), 0x2468ACE0u, "writes through 0x3010 alias should land in base RDRAM");
        });

        tc.Run("translateAddress handles kseg and uncached aliases", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            t.Equals(mem.translateAddress(0x80001234u), 0x00001234u, "KSEG0 should map directly to physical");
            t.Equals(mem.translateAddress(0xA0005678u), 0x00005678u, "KSEG1 should map directly to physical");
            t.Equals(mem.translateAddress(0x20001234u), 0x00001234u, "0x2000 uncached alias should map to RAM");
            t.Equals(mem.translateAddress(0x30105678u), 0x00105678u, "0x3010 accelerated alias should map to RAM");
            t.Equals(mem.translateAddress(PS2_SCRATCHPAD_BASE + 0x123u), 0x123u, "scratchpad base should translate to local offset");
            t.Equals(mem.translateAddress(PS2_SCRATCHPAD_ALIAS_BASE + 0x123u), 0x123u, "0xF000 scratchpad alias should translate to local offset");
        });

        tc.Run("EE timer0 count advances from scheduler cycles and can be reset", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer0Count = 0x10000000u;
            constexpr uint32_t kTimer0Mode = 0x10000010u;
            constexpr uint32_t kTimer0Compare = 0x10000020u;

            t.IsTrue(mem.writeIORegister(kTimer0Count, 0u), "timer count reset write should succeed");
            t.IsTrue(mem.writeIORegister(kTimer0Compare, 0xFFFFu), "timer compare write should succeed");
            t.IsTrue(mem.writeIORegister(kTimer0Mode, 0x82u), "timer mode write should be retained");
            t.Equals(mem.readIORegister(kTimer0Mode), 0x82u, "timer mode should be readable");

            mem.advanceEeTimers(8u * 512u);
            const uint32_t firstCount = mem.readIORegister(kTimer0Count);
            t.Equals(firstCount, 8u, "BUSCLK/256 should increment once per 512 EE cycles");

            t.IsTrue(mem.writeIORegister(kTimer0Count, 0u), "timer count second reset should succeed");
            mem.advanceEeTimers(512u);
            const uint32_t resetCount = mem.readIORegister(kTimer0Count);
            t.Equals(resetCount, 1u, "timer reset should restart the deterministic count window");
        });

        tc.Run("EE timers 0 through 3 expose independent COUNT MODE and COMP registers", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimerBases[] = {
                0x10000000u,
                0x10000800u,
                0x10001000u,
                0x10001800u,
            };
            constexpr uint32_t kBusClockDiv256Cue = 0x82u;
            for (uint32_t index = 0u; index < 4u; ++index)
            {
                const uint32_t base = kTimerBases[index];
                t.IsTrue(mem.writeIORegister(base, 0x100u + index), "timer COUNT write should succeed");
                t.IsTrue(mem.writeIORegister(base + 0x10u, kBusClockDiv256Cue), "timer MODE write should succeed");
                t.IsTrue(mem.writeIORegister(base + 0x20u, 0x200u + index), "timer COMP write should succeed");
            }

            mem.advanceEeTimers(512u);
            for (uint32_t index = 0u; index < 4u; ++index)
            {
                const uint32_t base = kTimerBases[index];
                t.Equals(mem.readIORegister(base), 0x101u + index, "each timer should advance its own COUNT");
                t.Equals(mem.readIORegister(base + 0x10u), kBusClockDiv256Cue, "each timer should retain MODE");
                t.Equals(mem.readIORegister(base + 0x20u), 0x200u + index, "each timer should retain COMP");
            }

            t.IsTrue(mem.writeIORegister(kTimerBases[0] + 0x30u, 0x12345u), "Timer0 HOLD write should succeed");
            t.IsTrue(mem.writeIORegister(kTimerBases[1] + 0x30u, 0x23456u), "Timer1 HOLD write should succeed");
            t.Equals(mem.readIORegister(kTimerBases[0] + 0x30u), 0x2345u, "Timer0 HOLD should be 16-bit");
            t.Equals(mem.readIORegister(kTimerBases[1] + 0x30u), 0x3456u, "Timer1 HOLD should be 16-bit");
        });

        tc.Run("EE Timer2 compare and overflow flags raise INTC_TIM2 and clear on write-one", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer2Count = 0x10001000u;
            constexpr uint32_t kTimer2Mode = 0x10001010u;
            constexpr uint32_t kTimer2Compare = 0x10001020u;
            constexpr uint32_t kCue = 1u << 7u;
            constexpr uint32_t kCmpe = 1u << 8u;
            constexpr uint32_t kOvfe = 1u << 9u;
            constexpr uint32_t kEquf = 1u << 10u;
            constexpr uint32_t kOvff = 1u << 11u;
            constexpr uint32_t kBusClockDiv256 = 2u;

            mem.writeIORegister(kTimer2Count, 0u);
            mem.writeIORegister(kTimer2Compare, 8u);
            mem.writeIORegister(kTimer2Mode, kBusClockDiv256 | kCue | kCmpe | kEquf | kOvff);

            t.Equals(mem.advanceEeTimers(7u * 512u), 0u, "compare should not fire before COUNT reaches COMP");
            t.Equals(mem.readIORegister(kTimer2Count), 7u, "Timer2 should expose its live 16-bit count");
            t.Equals(mem.advanceEeTimers(512u), 1u << 2u, "Timer2 compare should raise the TIM2 interrupt bit");
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kEquf) != 0u, "Timer2 compare should latch EQUF");

            mem.writeIORegister(kTimer2Mode, mem.readIORegister(kTimer2Mode) | kEquf);
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kEquf) == 0u, "writing one should clear EQUF");

            mem.writeIORegister(kTimer2Count, 0xFFFFu);
            mem.writeIORegister(kTimer2Mode, kBusClockDiv256 | kCue | kOvfe | kOvff);
            t.Equals(mem.advanceEeTimers(512u), 1u << 2u, "Timer2 overflow should raise the TIM2 interrupt bit");
            t.Equals(mem.readIORegister(kTimer2Count), 0u, "Timer2 count should wrap at 16 bits");
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kOvff) != 0u, "Timer2 overflow should latch OVFF");

            mem.writeIORegister(kTimer2Mode, mem.readIORegister(kTimer2Mode) | kOvff);
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kOvff) == 0u, "writing one should clear OVFF");
        });

        tc.Run("EE timer zero-return clears COUNT on compare", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer0Count = 0x10000000u;
            constexpr uint32_t kTimer0Mode = 0x10000010u;
            constexpr uint32_t kTimer0Compare = 0x10000020u;
            constexpr uint32_t kZret = 1u << 6u;
            constexpr uint32_t kCue = 1u << 7u;
            constexpr uint32_t kCmpe = 1u << 8u;
            constexpr uint32_t kEquf = 1u << 10u;

            mem.writeIORegister(kTimer0Count, 0u);
            mem.writeIORegister(kTimer0Compare, 3u);
            mem.writeIORegister(kTimer0Mode, kZret | kCue | kCmpe | kEquf);

            t.Equals(mem.advanceEeTimers(6u), 1u, "Timer0 compare should raise TIM0 after three BUSCLK ticks");
            t.Equals(mem.readIORegister(kTimer0Count), 0u, "ZRET should clear COUNT when it equals COMP");
        });

        tc.Run("scratchpad alias accesses the same bytes as base", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kOffset = 0x140u;
            constexpr uint32_t kScratchAddr = PS2_SCRATCHPAD_BASE + kOffset;
            constexpr uint32_t kScratchAliasAddr = PS2_SCRATCHPAD_ALIAS_BASE + kOffset;

            mem.write32(kScratchAliasAddr, 0xCAFEBABEu);
            t.Equals(mem.read32(kScratchAddr), 0xCAFEBABEu, "writes through 0xF000 scratchpad alias should land in scratchpad");
            t.Equals(mem.read32(kScratchAliasAddr), 0xCAFEBABEu, "reads through 0xF000 scratchpad alias should see scratchpad bytes");
        });

        tc.Run("VU0 code and data windows map through EE addresses", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kCodeAddr = PS2_VU0_CODE_BASE + 0x20u;
            mem.write32(kCodeAddr, 0x11223344u);
            t.Equals(mem.read32(kCodeAddr), 0x11223344u, "VU0 code readback should match written word");

            uint32_t codeWord = 0u;
            std::memcpy(&codeWord, mem.getVU0Code() + 0x20u, sizeof(codeWord));
            t.Equals(codeWord, 0x11223344u, "VU0 code write should land in micro memory buffer");

            constexpr uint32_t kDataAddr = PS2_VU0_DATA_BASE + 0x30u;
            const __m128i value = _mm_set_epi32(0x44556677u, 0x01234567u, 0x89ABCDEFu, 0xCAFEBABEu);
            mem.write128(kDataAddr, value);

            alignas(16) uint32_t words[4]{};
            const __m128i readback = mem.read128(kDataAddr);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(words), readback);

            t.Equals(words[0], 0xCAFEBABEu, "VU0 data lane 0 should match");
            t.Equals(words[1], 0x89ABCDEFu, "VU0 data lane 1 should match");
            t.Equals(words[2], 0x01234567u, "VU0 data lane 2 should match");
            t.Equals(words[3], 0x44556677u, "VU0 data lane 3 should match");
        });

        tc.Run("fast memory helpers wrap safely at RAM boundary", [](TestCase &t)
        {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            const uint32_t tail = PS2_RAM_SIZE - 4u;

            // Build a wrapped 64-bit pattern: [tail..tail+3] + [0..3]
            rdram[tail + 0u] = 0xA1u;
            rdram[tail + 1u] = 0xB2u;
            rdram[tail + 2u] = 0xC3u;
            rdram[tail + 3u] = 0xD4u;
            rdram[0u] = 0x11u;
            rdram[1u] = 0x22u;
            rdram[2u] = 0x33u;
            rdram[3u] = 0x44u;

            const uint64_t wrappedRead = Ps2FastRead64(rdram.data(), tail);
            t.Equals(wrappedRead, 0x44332211D4C3B2A1ull,
                     "Ps2FastRead64 should wrap across the 32MB boundary");

            Ps2FastWrite64(rdram.data(), tail, 0x8877665544332211ull);
            t.Equals(static_cast<uint32_t>(rdram[tail + 0u]), 0x11u, "write byte 0 should land at tail+0");
            t.Equals(static_cast<uint32_t>(rdram[tail + 1u]), 0x22u, "write byte 1 should land at tail+1");
            t.Equals(static_cast<uint32_t>(rdram[tail + 2u]), 0x33u, "write byte 2 should land at tail+2");
            t.Equals(static_cast<uint32_t>(rdram[tail + 3u]), 0x44u, "write byte 3 should land at tail+3");
            t.Equals(static_cast<uint32_t>(rdram[0u]), 0x55u, "write byte 4 should wrap to address 0");
            t.Equals(static_cast<uint32_t>(rdram[1u]), 0x66u, "write byte 5 should wrap to address 1");
            t.Equals(static_cast<uint32_t>(rdram[2u]), 0x77u, "write byte 6 should wrap to address 2");
            t.Equals(static_cast<uint32_t>(rdram[3u]), 0x88u, "write byte 7 should wrap to address 3");
        });

        tc.Run("VIF MPG num zero uploads 256 instructions", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            packet.reserve(4u + 2048u);
            appendU32(packet, makeVifCmd(0x4Au, 0u, 0u)); // MPG, num=0 -> 256 instructions (2048 bytes)

            for (uint32_t i = 0; i < 2048u; ++i)
            {
                packet.push_back(static_cast<uint8_t>(i & 0xFFu));
            }

            std::memset(mem.getVU1Code(), 0, PS2_VU1_CODE_SIZE);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1Code = mem.getVU1Code();
            bool matches = true;
            for (uint32_t i = 0; i < 2048u; ++i)
            {
                if (vu1Code[i] != static_cast<uint8_t>(i & 0xFFu))
                {
                    matches = false;
                    break;
                }
            }
            t.IsTrue(matches, "MPG num=0 should copy 2048 bytes into VU1 code memory");
        });

        tc.Run("VIF UNPACK num zero uploads 256 vectors", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            // UNPACK V4_32: opcode 0x6C (vn=3, vl=0), num=0 => 256 vectors, 16 bytes each.
            std::vector<uint8_t> packet;
            packet.reserve(4u + 4096u);
            appendU32(packet, makeVifCmd(0x6Cu, 0u, 0u));
            for (uint32_t i = 0; i < 4096u; ++i)
            {
                packet.push_back(static_cast<uint8_t>((i * 3u) & 0xFFu));
            }

            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1Data = mem.getVU1Data();
            bool matches = true;
            for (uint32_t i = 0; i < 4096u; ++i)
            {
                if (vu1Data[i] != static_cast<uint8_t>((i * 3u) & 0xFFu))
                {
                    matches = false;
                    break;
                }
            }
            t.IsTrue(matches, "UNPACK num=0 should copy 256 V4_32 vectors (4096 bytes)");
        });

        tc.Run("VIF control commands update MARK MASK ROW and COL registers", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x07u, 0u, 0x1234u)); // MARK

            appendU32(packet, makeVifCmd(0x20u, 0u, 0u));      // STMASK
            appendU32(packet, 0x89ABCDEFu);

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u));      // STROW
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, makeVifCmd(0x31u, 0u, 0u));      // STCOL
            appendU32(packet, 0xAAAA0001u);
            appendU32(packet, 0xAAAA0002u);
            appendU32(packet, 0xAAAA0003u);
            appendU32(packet, 0xAAAA0004u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            t.Equals(mem.vif1_regs.mark, 0x1234u, "MARK should set VIF1 MARK register");
            t.Equals(mem.vif1_regs.mask, 0x89ABCDEFu, "STMASK should set VIF1 MASK register");

            t.Equals(mem.vif1_regs.row[0], 0x11111111u, "STROW should set row[0]");
            t.Equals(mem.vif1_regs.row[1], 0x22222222u, "STROW should set row[1]");
            t.Equals(mem.vif1_regs.row[2], 0x33333333u, "STROW should set row[2]");
            t.Equals(mem.vif1_regs.row[3], 0x44444444u, "STROW should set row[3]");

            t.Equals(mem.vif1_regs.col[0], 0xAAAA0001u, "STCOL should set col[0]");
            t.Equals(mem.vif1_regs.col[1], 0xAAAA0002u, "STCOL should set col[1]");
            t.Equals(mem.vif1_regs.col[2], 0xAAAA0003u, "STCOL should set col[2]");
            t.Equals(mem.vif1_regs.col[3], 0xAAAA0004u, "STCOL should set col[3]");
        });

        tc.Run("VIF UNPACK V4-16 sign and zero extension follow immediate bit14", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            // UNPACK V4-16 (opcode 0x6D), num=1, addr=0.
            // Payload components: x=0xFF80, y=0x0001, z=0x7FFF, w=0x8001.
            const uint16_t comps[4] = {0xFF80u, 0x0001u, 0x7FFFu, 0x8001u};

            std::vector<uint8_t> signPacket;
            appendU32(signPacket, makeVifCmd(0x6Du, 1u, 0x0000u)); // sign-extend
            for (uint16_t c : comps)
            {
                const size_t pos = signPacket.size();
                signPacket.resize(pos + sizeof(uint16_t));
                std::memcpy(signPacket.data() + pos, &c, sizeof(uint16_t));
            }
            mem.processVIF1Data(signPacket.data(), static_cast<uint32_t>(signPacket.size()));

            const uint8_t *vu1 = mem.getVU1Data();
            uint32_t sx = 0, sy = 0, sz = 0, sw = 0;
            std::memcpy(&sx, vu1 + 0, 4);
            std::memcpy(&sy, vu1 + 4, 4);
            std::memcpy(&sz, vu1 + 8, 4);
            std::memcpy(&sw, vu1 + 12, 4);
            t.Equals(sx, 0xFFFFFF80u, "sign-extend x");
            t.Equals(sy, 0x00000001u, "sign-extend y");
            t.Equals(sz, 0x00007FFFu, "sign-extend z");
            t.Equals(sw, 0xFFFF8001u, "sign-extend w");

            // Same UNPACK with imm bit14 set => zero-extend.
            std::vector<uint8_t> zeroPacket;
            appendU32(zeroPacket, makeVifCmd(0x6Du, 1u, 0x4000u)); // zero-extend
            for (uint16_t c : comps)
            {
                const size_t pos = zeroPacket.size();
                zeroPacket.resize(pos + sizeof(uint16_t));
                std::memcpy(zeroPacket.data() + pos, &c, sizeof(uint16_t));
            }
            mem.processVIF1Data(zeroPacket.data(), static_cast<uint32_t>(zeroPacket.size()));

            std::memcpy(&sx, vu1 + 0, 4);
            std::memcpy(&sy, vu1 + 4, 4);
            std::memcpy(&sz, vu1 + 8, 4);
            std::memcpy(&sw, vu1 + 12, 4);
            t.Equals(sx, 0x0000FF80u, "zero-extend x");
            t.Equals(sy, 0x00000001u, "zero-extend y");
            t.Equals(sz, 0x00007FFFu, "zero-extend z");
            t.Equals(sw, 0x00008001u, "zero-extend w");
        });

        tc.Run("VIF UNPACK bit15 adds TOPS to destination address", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            mem.vif1_regs.tops = 4u;

            // UNPACK V4-32, num=1, addr=2, bit15 set => effective addr = 6.
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x6Cu, 1u, static_cast<uint16_t>(0x8000u | 0x0002u)));
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1 = mem.getVU1Data();

            uint32_t untouched = 0xDEADBEEFu;
            std::memcpy(&untouched, vu1 + (2u * 16u), 4);
            t.Equals(untouched, 0u, "base addr without TOPS should remain untouched");

            uint32_t x = 0, y = 0, z = 0, w = 0;
            const uint32_t dest = 6u * 16u;
            std::memcpy(&x, vu1 + dest + 0u, 4);
            std::memcpy(&y, vu1 + dest + 4u, 4);
            std::memcpy(&z, vu1 + dest + 8u, 4);
            std::memcpy(&w, vu1 + dest + 12u, 4);
            t.Equals(x, 0x11111111u, "TOPS-adjusted x");
            t.Equals(y, 0x22222222u, "TOPS-adjusted y");
            t.Equals(z, 0x33333333u, "TOPS-adjusted z");
            t.Equals(w, 0x44444444u, "TOPS-adjusted w");
        });

        tc.Run("VIF STCYCL skip mode advances destination by CL when CL>=WL", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x01u, 0u, static_cast<uint16_t>((1u << 8) | 3u))); // STCYCL: WL=1, CL=3
            appendU32(packet, makeVifCmd(0x6Cu, 2u, 0u)); // UNPACK V4-32, NUM=2, ADDR=0

            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, 0xAAAAAAAAu);
            appendU32(packet, 0xBBBBBBBBu);
            appendU32(packet, 0xCCCCCCCCu);
            appendU32(packet, 0xDDDDDDDDu);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();

            uint32_t v0x = 0, v1x = 0, v2x = 0, v3x = 0;
            std::memcpy(&v0x, vu + 0u * 16u + 0u, 4);
            std::memcpy(&v1x, vu + 1u * 16u + 0u, 4);
            std::memcpy(&v2x, vu + 2u * 16u + 0u, 4);
            std::memcpy(&v3x, vu + 3u * 16u + 0u, 4);

            t.Equals(v0x, 0x11111111u, "first vector should write at addr 0");
            t.Equals(v1x, 0u, "skip mode should leave addr 1 untouched when WL=1 CL=3");
            t.Equals(v2x, 0u, "skip mode should leave addr 2 untouched when WL=1 CL=3");
            t.Equals(v3x, 0xAAAAAAAAu, "second vector should write at addr CL (addr 3)");
        });

        tc.Run("VIF UNPACK cut between two transfers continues with the next data", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x6Cu, 2u, 0u)); // UNPACK V4-32, NUM=2, ADDR=0
            for (uint32_t i = 1u; i <= 8u; ++i)
                appendU32(packet, 0x10101010u * i);

            // The first transfer ends in the middle of the second vector.
            mem.processVIF1Data(packet.data(), 4u + 24u);
            t.Equals(mem.vif1_regs.stat & 3u, 1u, "a cut UNPACK should leave VPS waiting for data");
            mem.processVIF1Data(packet.data() + 28u, static_cast<uint32_t>(packet.size() - 28u));
            t.Equals(mem.vif1_regs.stat & 3u, 0u, "the completed UNPACK should return VPS to idle");

            const uint8_t *vu = mem.getVU1Data();
            for (uint32_t i = 0; i < 8u; ++i)
            {
                uint32_t v = 0;
                std::memcpy(&v, vu + i * 4u, 4u);
                t.Equals(v, 0x10101010u * (i + 1u), "both vectors should hold the data of both transfers");
            }
        });

        tc.Run("VIF STMOD mode 3 writes data unchanged and saves it into readable ROW registers", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW 0x1000 x4
            for (uint32_t i = 0; i < 4u; ++i)
                appendU32(packet, 0x1000u);
            appendU32(packet, makeVifCmd(0x20u, 0u, 0u)); // STMASK: cycle 1 y from ROW
            appendU32(packet, 0x00000400u);
            appendU32(packet, makeVifCmd(0x05u, 0u, 3u)); // STMOD 3
            appendU32(packet, makeVifCmd(0x01u, 0u, static_cast<uint16_t>((4u << 8) | 4u))); // STCYCL 4,4
            appendU32(packet, makeVifCmd(0x7Cu, 2u, 0u)); // masked UNPACK V4-32, NUM=2
            for (uint32_t i = 0; i < 8u; ++i)
                appendU32(packet, i);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            uint32_t x0 = 0, y1 = 0;
            std::memcpy(&x0, vu + 0u, 4u);
            std::memcpy(&y1, vu + 16u + 4u, 4u);
            t.Equals(x0, 0u, "mode 3 should not add ROW to the data");
            t.Equals(y1, 1u, "a ROW-masked field should read the ROW saved from the previous vector");
            t.Equals(mem.read32(0x10003D00u), 4u, "VIF1_R0 should read the saved x");
            t.Equals(mem.read32(0x10003D10u), 1u, "VIF1_R1 keeps y because the last y was masked");
            t.Equals(mem.read32(0x10003D30u), 7u, "VIF1_R3 should read the saved w");
        });

        tc.Run("VIF1_NUM reads 256 while a NUM=0 UNPACK waits for data", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x62u, 0u, 0u)); // UNPACK S-8, NUM=0 (256), no data
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));
            t.Equals(mem.read32(0x10003C60u), 256u, "VIF1_NUM");
            t.Equals(mem.read32(0x10003C00u) & 3u, 1u, "VPS should wait for data");
        });

        tc.Run("COP0 condition follows D_PCR.CPC and D_STAT.CIS", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            t.IsTrue(mem.dmacCpcond(), "no channel selected: true");
            t.IsTrue(mem.writeIORegister(0x1000E020u, 1u << 9), "D_PCR.CPC9 write should succeed");
            t.IsFalse(mem.dmacCpcond(), "CPC9 with CIS9 clear: false");

            constexpr uint32_t kToSpr = 0x1000D400u;
            t.IsTrue(mem.writeIORegister(kToSpr + 0x10u, 0x00024000u), "MADR");
            t.IsTrue(mem.writeIORegister(kToSpr + 0x20u, 1u), "QWC");
            t.IsTrue(mem.writeIORegister(kToSpr + 0x00u, 0x101u), "CHCR STR");
            t.IsTrue(mem.dmacCpcond(), "the ch9 DMA sets CIS9: true");
            t.IsTrue(mem.writeIORegister(0x1000E010u, 1u << 9), "D_STAT.CIS9 clear should succeed");
            t.IsFalse(mem.dmacCpcond(), "CIS9 cleared: false");
        });

        tc.Run("toSPR chain stops at a tag IRQ under TIE and still raises CIS9", [](TestCase &t)
        {
            // ps2autotests dma/dmac/tagintr and dma/spr/chain (real PS2).
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            constexpr uint32_t kToSpr = 0x1000D400u;
            constexpr uint32_t kTags = 0x00030000u;
            constexpr uint32_t kData = 0x00031000u;
            const uint32_t data[4] = {0x01234567u, 0x89ABCDEFu, 0xDEADBEEFu, 0x1337C0DEu};
            std::memcpy(mem.getRDRAM() + kData, data, sizeof(data));

            const auto runChain = [&](bool tie, bool irq)
            {
                const uint32_t tags[12] = {0x30000001u | (irq ? 0x80000000u : 0u), kData, 0u, 0u, // REF 1
                                           0x30000001u, kData, 0u, 0u,                             // REF 1
                                           0x70000000u, 0u, 0u, 0u};                               // END 0
                std::memcpy(mem.getRDRAM() + kTags, tags, sizeof(tags));
                mem.writeIORegister(0x1000E010u, mem.readIORegister(0x1000E010u) & (1u << 9));
                mem.writeIORegister(kToSpr + 0x80u, 0x20u);
                mem.writeIORegister(kToSpr + 0x10u, 0u);
                mem.writeIORegister(kToSpr + 0x20u, 0u);
                mem.writeIORegister(kToSpr + 0x30u, kTags);
                mem.writeIORegister(kToSpr + 0x00u, 0x104u | (tie ? 0x80u : 0u));
            };

            runChain(true, true);
            t.Equals(mem.readIORegister(kToSpr + 0x00u) >> 16, 0xB000u, "CHCR[31:16] holds the IRQ REF tag");
            t.Equals(mem.readIORegister(kToSpr + 0x30u), kTags + 0x10u, "TADR points at the next tag");
            t.Equals(mem.readIORegister(kToSpr + 0x80u), 0x30u, "one qword reached the scratchpad");
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 9)) != 0u, "CIS9 is raised");

            runChain(true, false);
            t.Equals(mem.readIORegister(kToSpr + 0x00u) >> 16, 0x7000u, "CHCR[31:16] holds the END tag");
            t.Equals(mem.readIORegister(kToSpr + 0x30u), kTags + 0x20u, "END leaves TADR at the END tag");
            t.Equals(mem.readIORegister(kToSpr + 0x80u), 0x40u, "two qwords reached the scratchpad");
            t.Equals(mem.readIORegister(kToSpr + 0x10u), kTags + 0x30u, "MADR is past the END tag's (empty) data");
            uint32_t first = 0u;
            std::memcpy(&first, mem.getScratchpad() + 0x20u, 4u);
            t.Equals(first, 0x01234567u, "the data lands at SADR");
        });

        tc.Run("toSPR interleave skips SQWC memory qwords after every TQWC", [](TestCase &t)
        {
            // ps2autotests dma/spr/interleave (real PS2): transfer 2, skip 1, count 4 copies memory qwords 0 1 3 4,
            // SADR ends at 0x40 and MADR at +0x60; D_SQWC fields are 8 bits.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            constexpr uint32_t kToSpr = 0x1000D400u;
            constexpr uint32_t kSrc = 0x00040000u;
            for (uint32_t q = 0u; q < 8u; ++q)
                std::memset(mem.getRDRAM() + kSrc + q * 16u, static_cast<int>(q + 1u), 16u);

            const auto upload = [&](uint32_t sqwc)
            {
                std::memset(mem.getScratchpad(), 0, 0x100u);
                mem.writeIORegister(0x1000E030u, sqwc);
                mem.writeIORegister(kToSpr + 0x80u, 0u);
                mem.writeIORegister(kToSpr + 0x10u, kSrc);
                mem.writeIORegister(kToSpr + 0x20u, 4u);
                mem.writeIORegister(kToSpr + 0x00u, 0x108u); // MOD interleave, STR
            };

            upload((2u << 16) | 1u);
            const uint8_t expected[4] = {1u, 2u, 4u, 5u};
            for (uint32_t q = 0u; q < 4u; ++q)
                t.Equals(static_cast<uint32_t>(mem.getScratchpad()[q * 16u]), static_cast<uint32_t>(expected[q]),
                         "scratchpad qword " + std::to_string(q));
            t.Equals(mem.readIORegister(kToSpr + 0x80u), 0x40u, "SADR");
            t.Equals(mem.readIORegister(kToSpr + 0x10u), kSrc + 0x60u, "MADR moves past the last skip");

            upload((1u << 16) | 0x100u); // skip 256 reads as 0
            t.Equals(mem.readIORegister(kToSpr + 0x10u), kSrc + 0x40u, "SQWC 256 behaves as 0");
        });

        tc.Run("VIF UNPACK V2 repeats x and y into z and w", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x64u, 1u, 0u)); // UNPACK V2-32, NUM=1, ADDR=0
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            uint32_t z = 0, w = 0;
            std::memcpy(&z, vu + 8u, 4u);
            std::memcpy(&w, vu + 12u, 4u);
            t.Equals(z, 0x11111111u, "V2 z should repeat x");
            t.Equals(w, 0x22222222u, "V2 w should repeat y");
        });

        tc.Run("VIF masked UNPACK uses data row col and protect selectors", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            // Pre-fill destination W lane for write-protect verification.
            uint32_t preservedW = 0xDEADBEEFu;
            std::memcpy(mem.getVU1Data() + 12u, &preservedW, 4u);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x20u, 0u, 0u)); // STMASK
            appendU32(packet, 0x000000E4u); // m0=0(data), m1=1(row), m2=2(col), m3=3(protect)

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 0xAAAAB001u);
            appendU32(packet, 0xAAAAB002u);
            appendU32(packet, 0xAAAAB003u);
            appendU32(packet, 0xAAAAB004u);

            appendU32(packet, makeVifCmd(0x31u, 0u, 0u)); // STCOL
            appendU32(packet, 0x11110001u);
            appendU32(packet, 0x11110002u);
            appendU32(packet, 0x11110003u);
            appendU32(packet, 0x11110004u);

            appendU32(packet, makeVifCmd(0x7Cu, 1u, 0u)); // UNPACK V4-32 with CMD bit4 (mask enable)
            appendU32(packet, 0x01020304u);
            appendU32(packet, 0x11121314u);
            appendU32(packet, 0x21222324u);
            appendU32(packet, 0x31323334u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            uint32_t x = 0, y = 0, z = 0, w = 0;
            std::memcpy(&x, vu + 0u, 4u);
            std::memcpy(&y, vu + 4u, 4u);
            std::memcpy(&z, vu + 8u, 4u);
            std::memcpy(&w, vu + 12u, 4u);

            t.Equals(x, 0x01020304u, "mask=0 should write decompressed data");
            t.Equals(y, 0xAAAAB002u, "mask=1 should write row register for Y field");
            t.Equals(z, 0x11110001u, "mask=2 should write C0 on first write cycle");
            t.Equals(w, preservedW, "mask=3 should write-protect destination field");
        });

        tc.Run("VIF STMOD offset and difference modes apply to UNPACK data", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 10u);
            appendU32(packet, 20u);
            appendU32(packet, 30u);
            appendU32(packet, 40u);

            appendU32(packet, makeVifCmd(0x05u, 0u, 1u)); // STMOD offset mode
            appendU32(packet, makeVifCmd(0x6Cu, 1u, 0u)); // UNPACK V4-32 -> addr 0
            appendU32(packet, 1u);
            appendU32(packet, 2u);
            appendU32(packet, 3u);
            appendU32(packet, 4u);

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // reset STROW for difference mode
            appendU32(packet, 100u);
            appendU32(packet, 100u);
            appendU32(packet, 100u);
            appendU32(packet, 100u);

            appendU32(packet, makeVifCmd(0x05u, 0u, 2u)); // STMOD difference mode
            appendU32(packet, makeVifCmd(0x6Cu, 2u, 1u)); // UNPACK V4-32 -> addr 1 and 2
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            uint32_t x0 = 0, y0 = 0, z0 = 0, w0 = 0;
            std::memcpy(&x0, vu + 0u * 16u + 0u, 4u);
            std::memcpy(&y0, vu + 0u * 16u + 4u, 4u);
            std::memcpy(&z0, vu + 0u * 16u + 8u, 4u);
            std::memcpy(&w0, vu + 0u * 16u + 12u, 4u);
            t.Equals(x0, 11u, "offset mode X");
            t.Equals(y0, 22u, "offset mode Y");
            t.Equals(z0, 33u, "offset mode Z");
            t.Equals(w0, 44u, "offset mode W");

            uint32_t x1 = 0, x2 = 0;
            std::memcpy(&x1, vu + 1u * 16u + 0u, 4u);
            std::memcpy(&x2, vu + 2u * 16u + 0u, 4u);
            t.Equals(x1, 101u, "difference mode first write should add initial row");
            t.Equals(x2, 103u, "difference mode second write should accumulate updated row");
            t.Equals(mem.vif1_regs.row[0], 103u, "difference mode should update row register");
            t.Equals(mem.vif1_regs.row[1], 103u, "difference mode should update row register for Y");
            t.Equals(mem.vif1_regs.row[2], 103u, "difference mode should update row register for Z");
            t.Equals(mem.vif1_regs.row[3], 103u, "difference mode should update row register for W");
        });

        tc.Run("VIF fill write uses STMASK and STROW when WL>CL", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x01u, 0u, static_cast<uint16_t>((3u << 8) | 1u))); // STCYCL: WL=3, CL=1

            appendU32(packet, makeVifCmd(0x20u, 0u, 0u)); // STMASK
            appendU32(packet, 0x55555555u); // all fields all cycles use row register

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, makeVifCmd(0x7Cu, 3u, 0u)); // masked UNPACK V4-32, NUM=3 writes
            // Only one input vector should be consumed for CL=1, WL=3.
            appendU32(packet, 0xAAAABBBB);
            appendU32(packet, 0xCCCCDDDD);
            appendU32(packet, 0xEEEEFFFF);
            appendU32(packet, 0x12345678);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            for (uint32_t i = 0; i < 3u; ++i)
            {
                uint32_t x = 0, y = 0, z = 0, w = 0;
                std::memcpy(&x, vu + i * 16u + 0u, 4u);
                std::memcpy(&y, vu + i * 16u + 4u, 4u);
                std::memcpy(&z, vu + i * 16u + 8u, 4u);
                std::memcpy(&w, vu + i * 16u + 12u, 4u);
                t.Equals(x, 0x11111111u, "fill write X should use row[0]");
                t.Equals(y, 0x22222222u, "fill write Y should use row[1]");
                t.Equals(z, 0x33333333u, "fill write Z should use row[2]");
                t.Equals(w, 0x44444444u, "fill write W should use row[3]");
            }
        });

        tc.Run("VIF irq command sets STAT.INT and CODE until FBRST.STC clears it", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t irqMarkCmd = 0x80000000u | makeVifCmd(0x07u, 0x12u, 0x3456u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&irqMarkCmd), sizeof(irqMarkCmd));

            t.Equals(mem.vif1_regs.code, irqMarkCmd, "VIF CODE should capture the last processed command");
            t.IsTrue((mem.vif1_regs.stat & (1u << 11)) != 0u, "irq bit should raise VIF1 STAT.INT");
            t.Equals(mem.vif1_regs.mark, 0x3456u, "MARK command should still update MARK register");

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u), "FBRST STC write should succeed");
            t.IsTrue((mem.vif1_regs.stat & (1u << 11)) == 0u, "FBRST.STC should clear VIF1 STAT.INT");
        });

        // VIF stalls, following PCSX2: Vif_Transfer.cpp stops after an I-bit command, Vif1_Dma.cpp/Vif0_Dma.cpp set
        // INT (unless ER1), raise INTC, set VIS unless the code is MARK and stall while VSS|VIS|VFS is set;
        // Vif.cpp vifNFBRST STC clears VSS|VFS|VIS|INT|ER0|ER1 and resumes; ERR.MII masks the I bit.
        tc.Run("VIF1 I bit stalls the VIF after its command until FBRST.STC", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> first;
            appendU32(first, 0x80000000u | makeVifCmd(0x00u, 0u, 0u)); // NOP with the I bit
            appendU32(first, makeVifCmd(0x07u, 0u, 0x1111u));          // MARK, behind the stall
            mem.processVIF1Data(first.data(), static_cast<uint32_t>(first.size()));

            const uint32_t stat = mem.readIORegister(0x10003C00u);
            t.IsTrue((stat & (1u << 10)) != 0u && (stat & (1u << 11)) != 0u, "VIF1_STAT should read VIS and INT");
            t.Equals(mem.takePendingVifInterrupts(), 0x2u, "the I bit should raise INTC_VIF1");
            t.Equals(mem.vif1_regs.mark, 0u, "the code after the I-bit command must wait for STC");

            std::vector<uint8_t> second;
            appendU32(second, makeVifCmd(0x07u, 0u, 0x2222u));         // a later transfer, also held
            mem.processVIF1Data(second.data(), static_cast<uint32_t>(second.size()));
            t.Equals(mem.vif1_regs.mark, 0u, "data arriving during the stall must be held as well");

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u), "FBRST.STC write should succeed");
            t.Equals(mem.vif1_regs.mark, 0x2222u, "STC should run the held data in arrival order");
            t.IsTrue((mem.readIORegister(0x10003C00u) & ((1u << 10) | (1u << 11))) == 0u, "STC should clear VIS and INT");
            t.IsFalse(mem.isVifStalled(1u), "the VIF should run again after STC");
        });

        tc.Run("VIF1 MARK with the I bit interrupts without stalling", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            appendU32(packet, 0x80000000u | makeVifCmd(0x07u, 0u, 0x1234u));
            appendU32(packet, makeVifCmd(0x07u, 0u, 0x5678u));
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            t.Equals(mem.takePendingVifInterrupts(), 0x2u, "MARK with the I bit still raises INTC_VIF1");
            t.IsTrue((mem.vif1_regs.stat & (1u << 11)) != 0u, "and sets STAT.INT");
            t.IsTrue((mem.vif1_regs.stat & (1u << 10)) == 0u, "but not VIS");
            t.Equals(mem.vif1_regs.mark, 0x5678u, "the next code runs immediately");
        });

        tc.Run("VIF1 ERR.MII masks the I bit", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            t.IsTrue(mem.writeIORegister(0x10003C20u, 0x1u), "ERR.MII write should succeed");
            t.Equals(mem.readIORegister(0x10003C20u), 0x1u, "VIF1_ERR should read back");

            std::vector<uint8_t> packet;
            appendU32(packet, 0x80000000u | makeVifCmd(0x00u, 0u, 0u));
            appendU32(packet, makeVifCmd(0x07u, 0u, 0x4321u));
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            t.IsFalse(mem.hasPendingVifInterrupts(), "a masked I bit raises no interrupt");
            t.IsTrue((mem.vif1_regs.stat & ((1u << 10) | (1u << 11))) == 0u, "a masked I bit sets neither INT nor VIS");
            t.Equals(mem.vif1_regs.mark, 0x4321u, "and does not stall");
        });

        tc.Run("VIF1 DMA stalled by an I bit completes when the held data fits the FIFO", [](TestCase &t)
        {
            // ps2autotests dma/vif/intstall (real PS2): 3 qwords, I-bit NOP after STMASK AAAAAAAA; the DMA finishes,
            // MASK stays AAAAAAAA and STAT reads 00000c02.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSrc = 0x00024000u;
            const uint32_t words[12] = {makeVifCmd(0x20u, 0u, 0u), 0xAAAAAAAAu, 0x80000000u, makeVifCmd(0x20u, 0u, 0u),
                                        0xBBBBBBBBu, 0u, 0u, makeVifCmd(0x20u, 0u, 0u), 0xCCCCCCCCu, 0u, 0u, 0u};
            std::memcpy(mem.getRDRAM() + kSrc, words, sizeof(words));

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kSrc), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, 3u), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR should succeed");
            mem.processPendingTransfers();

            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u, "the DMA completes");
            t.Equals(mem.readIORegister(0x10003C70u), 0xAAAAAAAAu, "MASK stops before the held codes");
            t.Equals(mem.readIORegister(0x10003C00u), 0x00000C02u, "STAT reads VIS | INT | VPS=2");

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u), "FBRST.STC write should succeed");
            t.Equals(mem.readIORegister(0x10003C70u), 0xCCCCCCCCu, "the held codes run on STC");
            t.Equals(mem.readIORegister(0x10003C00u) & 3u, 0u, "VPS returns to idle");
        });

        tc.Run("VIF1 DMA behind an I-bit stall completes only after FBRST.STC", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kSrc = 0x00024000u;
            constexpr uint32_t kQwc = 18u; // more held data than the 16-qword VIF1 FIFO takes
            uint8_t *rdram = mem.getRDRAM();
            std::vector<uint32_t> words(kQwc * 4u, 0u);
            words[0] = 0x80000000u;
            words[kQwc * 4u - 1u] = makeVifCmd(0x07u, 0u, 0x0ABCu);
            std::memcpy(rdram + kSrc, words.data(), words.size() * sizeof(uint32_t));

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kSrc), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, kQwc), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR should succeed");
            mem.processPendingTransfers();

            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) != 0u, "CHCR.STR stays set while data is held");
            t.IsTrue((mem.readIORegister(kDStat) & 0x2u) == 0u, "D_STAT.CIS1 is not raised while data is held");
            t.IsTrue(mem.consumeCompletedDmacCauses().empty(), "no DMAC completion while data is held");

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u), "FBRST.STC write should succeed");
            t.Equals(mem.vif1_regs.mark, 0x0ABCu, "the held data runs on STC");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u, "the DMA completes after STC");
            t.IsTrue((mem.readIORegister(kDStat) & 0x2u) != 0u, "D_STAT.CIS1 is raised after STC");
            const std::vector<uint32_t> causes = mem.consumeCompletedDmacCauses();
            t.IsTrue(causes.size() == 1u && causes[0] == 1u, "exactly one VIF1 DMAC completion after STC");
        });

        tc.Run("VIF1 DMA whose last code carries the I bit completes while VIS stays set", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSrc = 0x00024000u;
            const uint32_t words[4] = {0u, 0u, 0u, 0x80000000u}; // the I-bit NOP is the last word
            std::memcpy(mem.getRDRAM() + kSrc, words, sizeof(words));

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kSrc), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, 1u), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR should succeed");
            mem.processPendingTransfers();

            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u, "nothing is held, so the DMA completes");
            t.IsTrue(mem.isVifStalled(1u), "but the VIF stays stalled (VIS) until STC");
        });

        tc.Run("VIF1 FBRST.RST drops held data and releases a held DMA", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSrc = 0x00024000u;
            constexpr uint32_t kQwc = 18u; // more held data than the 16-qword VIF1 FIFO takes, so the DMA is held
            std::vector<uint32_t> words(kQwc * 4u, 0u);
            words[0] = 0x80000000u;
            words[1] = makeVifCmd(0x07u, 0u, 0x0DEFu);
            std::memcpy(mem.getRDRAM() + kSrc, words.data(), words.size() * sizeof(uint32_t));
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kSrc), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, kQwc), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR should succeed");
            mem.processPendingTransfers();

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x1u), "FBRST.RST write should succeed");
            t.IsFalse(mem.isVifStalled(1u), "RST clears the stall");
            t.IsTrue(mem.vifStalledData(1u).empty(), "RST drops the held data");
            t.Equals(mem.vif1_regs.mark, 0u, "the dropped data never runs");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u, "RST releases the held DMA");
        });

        tc.Run("VIF1 FBRST.STP stalls incoming data until STC", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x4u), "FBRST.STP write should succeed");
            t.IsTrue((mem.readIORegister(0x10003C00u) & (1u << 8)) != 0u, "STP sets VSS");

            const uint32_t mark = makeVifCmd(0x07u, 0u, 0x0777u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mark), sizeof(mark));
            t.Equals(mem.vif1_regs.mark, 0u, "a stopped VIF holds its data");
            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u), "FBRST.STC write should succeed");
            t.Equals(mem.vif1_regs.mark, 0x0777u, "STC resumes it");
        });

        tc.Run("VIF0 I bit stalls until VIF0_FBRST.STC", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            appendU32(packet, 0x80000000u | makeVifCmd(0x00u, 0u, 0u));
            appendU32(packet, makeVifCmd(0x07u, 0u, 0x0055u));
            mem.processVIF0Data(packet.data(), static_cast<uint32_t>(packet.size()));

            t.IsTrue((mem.readIORegister(0x10003800u) & (1u << 10)) != 0u, "VIF0_STAT should read VIS");
            t.Equals(mem.takePendingVifInterrupts(), 0x1u, "the I bit should raise INTC_VIF0");
            t.Equals(mem.vif0_regs.mark, 0u, "VIF0 holds the code after the I-bit command");
            t.IsTrue(mem.writeIORegister(0x10003810u, 0x8u), "VIF0_FBRST.STC write should succeed");
            t.Equals(mem.vif0_regs.mark, 0x0055u, "STC resumes VIF0");
        });

        tc.Run("VIF FBRST RST clears VIF1 command state", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.vif1_regs.mark = 0x1234u;
            mem.vif1_regs.cycle = 0x0102u;
            mem.vif1_regs.mode = 2u;
            mem.vif1_regs.num = 7u;
            mem.vif1_regs.mask = 0x89ABCDEFu;
            mem.vif1_regs.code = 0xCAFEBABEu;
            mem.vif1_regs.stat = 0x3F00u;

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x1u), "FBRST RST write should succeed");

            t.Equals(mem.vif1_regs.mark, 0u, "RST should clear MARK");
            t.Equals(mem.vif1_regs.cycle, 0u, "RST should clear CYCLE");
            t.Equals(mem.vif1_regs.mode, 0u, "RST should clear MODE");
            t.Equals(mem.vif1_regs.num, 0u, "RST should clear NUM");
            t.Equals(mem.vif1_regs.mask, 0u, "RST should clear MASK");
            t.Equals(mem.vif1_regs.code, 0u, "RST should clear CODE");
            t.Equals(mem.vif1_regs.stat, 0u, "RST should clear STAT");
        });

        tc.Run("VIF double-buffer OFFSET BASE and MSCAL update TOPS and ITOPS", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.vif1_regs.base = 0x120u;
            mem.vif1_regs.tops = 0x120u;
            mem.vif1_regs.stat = (1u << 7); // DBF=1 before OFFSET

            struct MscalCall
            {
                uint32_t startPC;
                uint32_t top;
                uint32_t itop;
            };
            std::vector<MscalCall> mscalCalls;
            mem.setVu1MscalCallback([&](uint32_t startPC, uint32_t top, uint32_t itop)
            {
                mscalCalls.push_back({startPC, top, itop});
            });

            const uint32_t offsetCmd = makeVifCmd(0x02u, 0u, 0x0022u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&offsetCmd), sizeof(offsetCmd));
            t.Equals(mem.vif1_regs.ofst, 0x22u, "OFFSET should update OFST");
            t.Equals(mem.vif1_regs.base, 0x120u, "OFFSET should copy old TOPS into BASE");
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) == 0u, "OFFSET should clear DBF");
            t.Equals(mem.vif1_regs.tops, 0x120u, "DBF=0 should keep TOPS at BASE");

            const uint32_t baseCmd = makeVifCmd(0x03u, 0u, 0x0030u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&baseCmd), sizeof(baseCmd));
            t.Equals(mem.vif1_regs.base, 0x30u, "BASE should update BASE register");
            t.Equals(mem.vif1_regs.tops, 0x120u, "BASE should not rewrite current TOPS");

            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x0044u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&itopCmd), sizeof(itopCmd));
            t.Equals(mem.vif1_regs.itops, 0x44u, "ITOP VIFcode should update pending ITOPS register");

            const uint32_t mscalCmd = makeVifCmd(0x14u, 0u, 0x0003u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mscalCmd), sizeof(mscalCmd));
            t.Equals(mscalCalls.size(), static_cast<size_t>(1u), "MSCAL should invoke callback once");
            t.Equals(mscalCalls[0].startPC, 0x18u, "MSCAL callback startPC should be IMMEDIATE*8");
            t.Equals(mscalCalls[0].top, 0x120u, "MSCAL callback should receive current TOPS");
            t.Equals(mscalCalls[0].itop, 0x44u, "MSCAL callback should receive pending ITOPS");
            t.Equals(mem.vif1_regs.top, 0x120u, "MSCAL should latch TOP from TOPS");
            t.Equals(mem.vif1_regs.itop, 0x44u, "MSCAL should latch ITOP from ITOPS");
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) != 0u, "MSCAL should toggle DBF");
            t.Equals(mem.vif1_regs.tops, 0x52u, "DBF=1 should set TOPS to BASE+OFST");

            const uint32_t mscntCmd = makeVifCmd(0x17u, 0u, 0u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mscntCmd), sizeof(mscntCmd));
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) == 0u, "MSCNT should toggle DBF again");
            t.Equals(mem.vif1_regs.tops, 0x30u, "DBF=0 should restore TOPS to BASE");
        });

        tc.Run("VIF MSKPATH3 uses immediate bit15", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&setMask), sizeof(setMask));
            t.IsTrue(mem.isPath3Masked(), "MSKPATH3 with imm bit15 set should enable PATH3 mask");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&clearMask), sizeof(clearMask));
            t.IsFalse(mem.isPath3Masked(), "MSKPATH3 with imm bit15 clear should disable PATH3 mask");
        });

        tc.Run("VIF1 FIFO MSKPATH3 is visible through GIF_STAT", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            const __m128i setPacket = _mm_set_epi32(0, 0, 0, static_cast<int32_t>(setMask));
            mem.write128(0x10005000u, setPacket);
            t.IsTrue(mem.isPath3Masked(), "a direct VIF1 FIFO command should execute MSKPATH3");
            t.IsTrue((mem.readIORegister(0x10003020u) & 0x2u) != 0u,
                     "GIF_STAT.M3P should report the VIF1 PATH3 mask");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            const __m128i clearPacket = _mm_set_epi32(0, 0, 0, static_cast<int32_t>(clearMask));
            mem.write128(0x10005000u, clearPacket);
            t.IsFalse(mem.isPath3Masked(), "a direct VIF1 FIFO command should clear MSKPATH3");
            t.IsTrue((mem.readIORegister(0x10003020u) & 0x2u) == 0u,
                     "GIF_STAT.M3P should clear with the VIF1 PATH3 mask");

            mem.writeIORegister(0x10003010u, 0x5u); // GIF_MODE: M3R | IMT
            mem.writeIORegister(0x10003000u, 0x8u); // GIF_CTRL: PSE
            t.Equals(mem.readIORegister(0x10003020u) & 0xDu, 0xDu,
                     "GIF_STAT should mirror GIF_MODE and GIF_CTRL status bits");
        });

        tc.Run("GIF_STAT exposes synchronously drained DMA occupancy for one EE quantum", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifChannel = 0x1000A000u;
            constexpr uint32_t kSource = 0x00020000u;
            mem.setGifPacketCallback([](const uint8_t *, uint32_t) {});

            t.IsTrue(mem.writeIORegister(kGifChannel + 0x10u, kSource),
                     "write GIF MADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifChannel + 0x20u, 1u),
                     "write GIF QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifChannel + 0x00u, 0x100u),
                     "start GIF normal DMA should succeed");

            const uint32_t visibleFqc = (mem.readIORegister(0x10003020u) >> 24u) & 0x1Fu;
            t.Equals(visibleFqc, 1u,
                     "a synchronously consumed qword should remain observable through GIF_STAT.FQC");

            mem.advanceEeTimers(1u);
            const uint32_t drainedFqc = (mem.readIORegister(0x10003020u) >> 24u) & 0x1Fu;
            t.Equals(drainedFqc, 0u,
                     "the synthetic FIFO observation should expire at the next EE scheduling boundary");
        });

        tc.Run("PATH3 mask queues packets until unmask", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            std::vector<uint8_t> packetA(16u);
            std::vector<uint8_t> packetB(16u);
            for (uint32_t i = 0; i < 16u; ++i)
            {
                packetA[i] = static_cast<uint8_t>(0x10u + i);
                packetB[i] = static_cast<uint8_t>(0x40u + i);
            }

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&setMask), sizeof(setMask));
            t.IsTrue(mem.isPath3Masked(), "PATH3 mask should be enabled");

            mem.submitGifPacket(GifPathId::Path3, packetA.data(), static_cast<uint32_t>(packetA.size()));
            mem.submitGifPacket(GifPathId::Path3, packetB.data(), static_cast<uint32_t>(packetB.size()));
            t.Equals(captured.size(), static_cast<size_t>(0u), "masked PATH3 packets should be queued, not dropped/emitted");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&clearMask), sizeof(clearMask));

            t.Equals(captured.size(), static_cast<size_t>(2u), "unmask should flush queued PATH3 packets");
            bool firstOk = true;
            bool secondOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x10u + i))
                    firstOk = false;
                if (captured[1][i] != static_cast<uint8_t>(0x40u + i))
                    secondOk = false;
            }
            t.IsTrue(firstOk, "first queued PATH3 packet should flush in-order");
            t.IsTrue(secondOk, "second queued PATH3 packet should flush in-order");
        });

        tc.Run("GIF arbiter prioritizes PATH1 then PATH2 then PATH3", [](TestCase &t)
        {
            std::vector<uint8_t> order;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                if (data && sizeBytes > 0u)
                    order.push_back(data[0]);
            });

            const std::vector<uint8_t> p1(16u, 0x11u);
            const std::vector<uint8_t> p2(16u, 0x22u);
            const std::vector<uint8_t> p3(16u, 0x33u);

            arbiter.submit(GifPathId::Path3, p3.data(), static_cast<uint32_t>(p3.size()));
            arbiter.submit(GifPathId::Path2, p2.data(), static_cast<uint32_t>(p2.size()));
            arbiter.submit(GifPathId::Path1, p1.data(), static_cast<uint32_t>(p1.size()));
            arbiter.drain();

            t.Equals(order.size(), static_cast<size_t>(3u), "all queued packets should be drained");
            t.Equals(order[0], static_cast<uint8_t>(0x11u), "PATH1 should be drained first");
            t.Equals(order[1], static_cast<uint8_t>(0x22u), "PATH2 should be drained second");
            t.Equals(order[2], static_cast<uint8_t>(0x33u), "PATH3 should be drained third");
        });

        tc.Run("VIF DIRECTHL stalls behind queued PATH3 IMAGE packets", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> firstBytes;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                if (data && sizeBytes > 0u)
                    firstBytes.push_back(data[0]);
            });
            mem.setGifArbiter(&arbiter);

            std::vector<uint8_t> path3Image;
            appendU64(path3Image, makeGifTag(0x00AAu, 2u, 0u, true)); // IMAGE packet marker: first byte 0xAA
            appendU64(path3Image, 0ull);
            mem.submitGifPacket(GifPathId::Path3, path3Image.data(), static_cast<uint32_t>(path3Image.size()), false);

            std::vector<uint8_t> vifPacket;
            appendU32(vifPacket, makeVifCmd(0x51u, 0u, 1u)); // DIRECTHL 1 QW
            for (uint32_t i = 0; i < 16u; ++i)
            {
                vifPacket.push_back(static_cast<uint8_t>(0xD2u + i));
            }
            mem.processVIF1Data(vifPacket.data(), static_cast<uint32_t>(vifPacket.size()));

            t.Equals(firstBytes.size(), static_cast<size_t>(2u), "PATH3 and DIRECTHL packets should both drain");
            t.Equals(firstBytes[0], static_cast<uint8_t>(0xAAu), "DIRECTHL should not preempt queued PATH3 IMAGE packet");
            t.Equals(firstBytes[1], static_cast<uint8_t>(0xD2u), "DIRECTHL packet should drain after PATH3 IMAGE packet");
        });

        tc.Run("GIF DMA mode0 copies RDRAM packet and clears channel", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kSrc = 0x00022000u;
            constexpr uint32_t kQwc = 2u; // 32 bytes

            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < kQwc * 16u; ++i)
            {
                rdram[kSrc + i] = static_cast<uint8_t>((0x40u + i) & 0xFFu);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x10u, kSrc), "write MADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x20u, kQwc), "write QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "write CHCR STR should succeed");

            t.Equals(mem.dmaStartCount(), 1ull, "starting GIF DMA should increment dmaStartCount");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "GIF DMA should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(kQwc * 16u), "GIF packet size should match QWC");

            bool contentOk = true;
            for (uint32_t i = 0; i < kQwc * 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>((0x40u + i) & 0xFFu))
                {
                    contentOk = false;
                    break;
                }
            }
            t.IsTrue(contentOk, "GIF DMA packet bytes should match source RDRAM");
            t.IsTrue(mem.hasSeenGifCopy(), "GIF DMA should mark seen GIF copy");
            t.Equals(mem.gifCopyCount(), 1ull, "GIF DMA should increment gifCopyCount");
            t.IsTrue((mem.readIORegister(kGifCh + 0x00u) & 0x100u) == 0u, "GIF CHCR STR bit should be cleared after drain");
            t.Equals(mem.readIORegister(kGifCh + 0x20u), 0u, "GIF QWC should be cleared after drain");
        });

        tc.Run("GIF DMA can source from scratchpad", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kSrcScratch = PS2_SCRATCHPAD_BASE + 0x80u;
            constexpr uint32_t kQwc = 1u; // 16 bytes

            uint8_t *scratch = mem.getScratchpad();
            for (uint32_t i = 0; i < 16u; ++i)
            {
                scratch[0x80u + i] = static_cast<uint8_t>((0xA0u + i) & 0xFFu);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x10u, kSrcScratch), "write MADR scratchpad should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x20u, kQwc), "write QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "write CHCR STR should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "scratchpad GIF DMA should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "scratchpad GIF DMA packet should be 16 bytes");
            bool contentOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>((0xA0u + i) & 0xFFu))
                {
                    contentOk = false;
                    break;
                }
            }
            t.IsTrue(contentOk, "scratchpad GIF DMA packet bytes should match scratchpad source");
        });

        tc.Run("normal-mode GIF and VIF1 DMA leave MADR past the data and QWC at zero", [](TestCase &t)
        {
            // ps2autotests dma/dmasend.cpp warns "MADR did not increase as expected" unless madr == start + size.
            for (const uint32_t channel : {0x1000A000u, 0x10009000u})
            {
                PS2Memory mem;
                t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
                mem.setGifPacketCallback([](const uint8_t *, uint32_t) {});
                constexpr uint32_t kMadr = 0x00030000u;
                constexpr uint32_t kQwc = 3u;
                t.IsTrue(mem.writeIORegister(channel + 0x10u, kMadr), "write MADR");
                t.IsTrue(mem.writeIORegister(channel + 0x20u, kQwc), "write QWC");
                t.IsTrue(mem.writeIORegister(channel + 0x00u, 0x101u), "start (from memory)");
                mem.processPendingTransfers();
                t.Equals(mem.readIORegister(channel + 0x10u), kMadr + kQwc * 16u, "MADR should advance by QWC * 16");
                t.Equals(mem.readIORegister(channel + 0x20u), 0u, "QWC should reach zero");
            }
        });

        tc.Run("scratchpad DMA SADR and MADR keep only the bits hardware stores", [](TestCase &t)
        {
            // ps2autotests dma/spr/normal.expected: SADR 0x14 reads 0x10, 0x81234028 reads 0x20; MADR drops bit 31.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            mem.writeIORegister(0x1000D480u, 0x14u);
            t.Equals(mem.readIORegister(0x1000D480u), 0x10u, "toSPR SADR low bits");
            mem.writeIORegister(0x1000D480u, 0x81234028u);
            t.Equals(mem.readIORegister(0x1000D480u), 0x20u, "toSPR SADR high bits");
            mem.writeIORegister(0x1000D080u, 0x81234028u);
            t.Equals(mem.readIORegister(0x1000D080u), 0x20u, "fromSPR SADR high bits");
            mem.writeIORegister(0x1000D010u, 0x80030000u);
            t.Equals(mem.readIORegister(0x1000D010u), 0x00030000u, "fromSPR MADR has no SPR bit");
        });

        tc.Run("a source chain that loops without an end tag stops after one lap (ps2x_fuzz_dma regression)", [](TestCase &t)
        {
            // Minimized from tools/fuzz/regressions/dma: a NEXT tag whose ADDR is itself. Before the fix the walk
            // gathered 2^20 laps (~750 MB for QWC=0x2E) before its tag limit stopped it.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTag = 0x00100000u;
            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag, makeDmaTag(0x2Eu, 2u, kTag));
            std::vector<size_t> packetSizes;
            mem.setGifPacketCallback([&](const uint8_t *, uint32_t sizeBytes) { packetSizes.push_back(sizeBytes); });
            mem.writeIORegister(kGifCh + 0x30u, kTag);
            mem.writeIORegister(kGifCh + 0x20u, 0u);
            mem.writeIORegister(kGifCh + 0x00u, 0x105u);
            mem.processPendingTransfers();
            size_t total = 0;
            for (size_t s : packetSizes)
                total += s;
            t.Equals(total, static_cast<size_t>(0x2Eu * 16u), "exactly one lap of payload is delivered");
            t.Equals(mem.readIORegister(kGifCh + 0x30u), kTag, "TADR is left at the repeating tag");
        });

        tc.Run("chain cycle detection does not stop a chain that CALLs the same subroutine twice", [](TestCase &t)
        {
            // CALL sub; CALL sub; END. The subroutine's RET tag is visited twice with the same TADR and ASP but a
            // different return address (ASR0), so it is not a cycle.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kMain = 0x00100000u;
            constexpr uint32_t kSub = 0x00110000u;
            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kMain + 0x00u, makeDmaTag(1u, 5u, kSub)); // CALL, 1 qword at kMain+0x10
            writeDmaTag(rdram, kMain + 0x20u, makeDmaTag(1u, 5u, kSub)); // CALL, 1 qword at kMain+0x30
            writeDmaTag(rdram, kMain + 0x40u, makeDmaTag(1u, 7u, 0u));   // END, 1 qword
            writeDmaTag(rdram, kSub, makeDmaTag(2u, 6u, 0u));            // RET, 2 qwords
            std::vector<size_t> packetSizes;
            mem.setGifPacketCallback([&](const uint8_t *, uint32_t sizeBytes) { packetSizes.push_back(sizeBytes); });
            mem.writeIORegister(kGifCh + 0x30u, kMain);
            mem.writeIORegister(kGifCh + 0x20u, 0u);
            mem.writeIORegister(kGifCh + 0x00u, 0x105u);
            mem.processPendingTransfers();
            size_t total = 0;
            for (size_t s : packetSizes)
                total += s;
            t.Equals(total, static_cast<size_t>((1u + 2u + 1u + 2u + 1u) * 16u), "both calls and the END are transferred");
        });

        tc.Run("chain cycle detection does not stop finite chains that transfer the same data repeatedly", [](TestCase &t)
        {
            // Three REF tags reading one data block, then REFE on the same block: the payload repeats, the tags do not.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTags = 0x00100000u;
            constexpr uint32_t kData = 0x00120000u;
            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < 32u; ++i)
                rdram[kData + i] = static_cast<uint8_t>(0xA0u + i);
            writeDmaTag(rdram, kTags + 0x00u, makeDmaTag(2u, 3u, kData)); // REF
            writeDmaTag(rdram, kTags + 0x10u, makeDmaTag(2u, 3u, kData)); // REF
            writeDmaTag(rdram, kTags + 0x20u, makeDmaTag(2u, 3u, kData)); // REF
            writeDmaTag(rdram, kTags + 0x30u, makeDmaTag(2u, 0u, kData)); // REFE
            std::vector<uint8_t> received;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes) { received.insert(received.end(), data, data + sizeBytes); });
            mem.writeIORegister(kGifCh + 0x30u, kTags);
            mem.writeIORegister(kGifCh + 0x20u, 0u);
            mem.writeIORegister(kGifCh + 0x00u, 0x105u);
            mem.processPendingTransfers();
            t.Equals(received.size(), static_cast<size_t>(4u * 32u), "all four transfers of the shared block arrive");
            bool repeated = received.size() == 128u;
            for (size_t i = 0; repeated && i < received.size(); ++i)
                repeated = received[i] == static_cast<uint8_t>(0xA0u + (i % 32u));
            t.IsTrue(repeated, "each transfer carries the shared block");
            t.Equals(mem.readIORegister(kGifCh + 0x30u), kTags + 0x40u, "REFE leaves TADR past the last tag");
        });

        tc.Run("GIF DMA chain can source tags and payload from 0xF000 scratchpad alias", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTagAlias = PS2_SCRATCHPAD_ALIAS_BASE + 0x100u;

            uint8_t *scratch = mem.getScratchpad();
            std::memset(scratch + 0x100u, 0, 32u);

            const uint64_t endTag = makeDmaTag(1u, 7u, 0u, false);
            std::memcpy(scratch + 0x100u, &endTag, sizeof(endTag));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                scratch[0x110u + i] = static_cast<uint8_t>(0xC0u + i);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kTagAlias), "write TADR scratchpad alias should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write CHCR STR|CHAIN should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "scratchpad alias chain should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "scratchpad alias chain should emit one qword");

            bool contentOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0xC0u + i))
                {
                    contentOk = false;
                    break;
                }
            }
            t.IsTrue(contentOk, "scratchpad alias chain payload should match scratchpad bytes");
        });

        // Ported from LightVelox/PS2Recomp ee8d1604.
        tc.Run("GIF DMA chain longer than 4096 tags is transferred to its END tag", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kChain = 0x00100000u;
            constexpr uint32_t kCntTags = 6000u;

            uint8_t *rdram = mem.getRDRAM();
            uint32_t addr = kChain;
            for (uint32_t i = 0; i <= kCntTags; ++i)
            {
                const bool last = i == kCntTags;
                writeDmaTag(rdram, addr, makeDmaTag(1u, last ? 7u : 1u, 0u, false));
                const uint32_t marker = last ? 0xE0D0E0D0u : i;
                std::memset(rdram + addr + 16u, 0, 16u);
                std::memcpy(rdram + addr + 16u, &marker, sizeof(marker));
                addr += 32u;
            }

            std::vector<uint8_t> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.insert(captured.end(), data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kChain), "write TADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write CHCR STR|CHAIN should succeed");
            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>((kCntTags + 1u) * 16u), "every tag's payload should be transferred");
            uint32_t lastMarker = 0u;
            if (captured.size() >= 16u)
            {
                std::memcpy(&lastMarker, captured.data() + captured.size() - 16u, sizeof(lastMarker));
            }
            t.Equals(lastMarker, 0xE0D0E0D0u, "the END tag's payload should arrive last");
        });

        tc.Run("native GIF image upload recognizes canonical load-image chain", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kChain = 0x00028000u;
            constexpr uint32_t kPixels = 0x00029000u;
            constexpr uint32_t kQwc = 1u;

            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < kQwc * 16u; ++i)
            {
                rdram[kPixels + i] = static_cast<uint8_t>(0x40u + i);
            }

            uint32_t chain = kChain;
            chain = writeTextureUploadSetup(rdram, chain, 0u, GS_PSM_CT32);
            chain = writeTextureImageRef(rdram, chain, kQwc, kPixels);
            writeDmaTag(rdram, chain, makeDmaTag(0u, 7u, 0u, false)); // END.

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kChain), "write GIF TADR should succeed");
            t.IsTrue(mem.tryProcessNativeGifImageUploadChain(gs, kChain, 0x105u),
                     "canonical load-image chain should use the native upload path");

            t.Equals(gs.nativeImageUploadCount(), 1ull, "native GIF DMA chain should upload through GS fast path");
            t.Equals(mem.gifCopyCount(), 1ull, "native GIF DMA chain should still count as a GIF DMA copy");
            t.IsTrue((mem.readIORegister(kDStat) & (1u << 2u)) != 0u,
                     "native GIF DMA chain should raise D_STAT GIF completion");
            t.Equals(mem.readIORegister(kGifCh + 0x20u), 0u, "native GIF DMA chain should clear GIF QWC");
            t.Equals(mem.readIORegister(kGifCh + 0x00u) & 0x100u, 0u,
                     "native GIF DMA chain should clear GIF STR");
            t.Equals(mem.readIORegister(kGifCh + 0x00u) & 0x70000000u, 0x70000000u,
                     "native GIF DMA chain should latch the terminal END tag id");

            bool pixelsOk = true;
            for (uint32_t x = 0; x < 4u && pixelsOk; ++x)
            {
                const uint32_t dstOff = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                const uint32_t srcOff = kPixels + x * 4u;
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (mem.getGSVRAM()[dstOff + c] != rdram[srcOff + c])
                    {
                        pixelsOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(pixelsOk, "native GIF DMA chain should upload image payload into GS VRAM");
        });

        tc.Run("native GIF packed chain matches generic packed primitive packet", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS nativeGs;
            nativeGs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());

            GSRegisters genericRegs{};
            std::vector<uint8_t> genericVram(PS2_GS_VRAM_SIZE, 0u);
            GS genericGs;
            genericGs.init(genericVram.data(), static_cast<uint32_t>(genericVram.size()), &genericRegs);

            std::vector<uint8_t> packet;
            appendU64(packet, makeGifTag(4u, GIF_FMT_PACKED, 1u, false));
            appendU64(packet, 0x0Eull);
            appendU64(packet, makeGsFrame(0u, 1u, GS_PSM_CT32));
            appendU64(packet, GS_REG_FRAME_1);
            appendU64(packet, makeGsScissor(0u, 7u, 0u, 7u));
            appendU64(packet, GS_REG_SCISSOR_1);
            appendU64(packet, 1ull << 17u); // ZTST always.
            appendU64(packet, GS_REG_TEST_1);
            appendU64(packet, 1ull << 32u); // Mask Z writes so the test framebuffer remains visible.
            appendU64(packet, GS_REG_ZBUF_1);

            constexpr uint16_t kSpritePrim = static_cast<uint16_t>(GS_PRIM_SPRITE);
            appendU64(packet, makeGifTagPrim(2u, kSpritePrim, GIF_FMT_PACKED, 3u, true, true));
            appendU64(packet, static_cast<uint64_t>(GS_REG_UV) |
                                  (static_cast<uint64_t>(GS_REG_RGBAQ) << 4u) |
                                  (static_cast<uint64_t>(GS_REG_XYZF2) << 8u));
            appendPackedUv(packet, 0u, 0u);
            appendPackedRgbaq(packet, 0x20u, 0x40u, 0x80u, 0x80u);
            appendPackedXyzf2(packet, 0u, 0u, 0u);
            appendPackedUv(packet, 0u, 0u);
            appendPackedRgbaq(packet, 0xE0u, 0x30u, 0x10u, 0x80u);
            appendPackedXyzf2(packet, 64u, 64u, 0u);

            genericGs.processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kScratchTag = 0xF0000000u;
            uint8_t *scratch = mem.getScratchpad();
            writeDmaTag(scratch, 0u, makeDmaTag(static_cast<uint16_t>(packet.size() / 16u), 7u, 0u, false));
            std::memcpy(scratch + 16u, packet.data(), packet.size());

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kScratchTag), "write GIF TADR scratchpad alias should succeed");
            t.IsTrue(mem.tryProcessNativeGifPackedChain(nativeGs, kScratchTag, 0x105u),
                     "packed primitive chain should use the native packed GIF path");

            t.Equals(nativeGs.nativePackedGIFPacketCount(), 1ull, "native packed GIF packet counter should increment");
            t.Equals(mem.gifCopyCount(), 1ull, "native packed GIF chain should still count as a GIF DMA copy");
            t.IsTrue((mem.readIORegister(kDStat) & (1u << 2u)) != 0u,
                     "native packed GIF chain should raise D_STAT GIF completion");
            t.Equals(mem.readIORegister(kGifCh + 0x20u), 0u, "native packed GIF chain should clear GIF QWC");
            t.Equals(mem.readIORegister(kGifCh + 0x00u) & 0x100u, 0u,
                     "native packed GIF chain should clear GIF STR");

            const uint32_t nativePixel = nativeGs.ReadVram(GS_PSM_CT32, 0u, 1u, 1u, 1u);
            const uint32_t genericPixel = genericGs.ReadVram(GS_PSM_CT32, 0u, 1u, 1u, 1u);
            t.IsTrue(genericPixel != 0u, "generic packed primitive packet should draw a test pixel");
            t.Equals(nativePixel, genericPixel, "native packed GIF chain should match generic GS packet output");
        });

        tc.Run("GIF DMA chain REF keeps CT32 image data after paletted upload", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            std::vector<uint8_t> capturedGifPacket;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                capturedGifPacket.assign(data, data + sizeBytes);
                gs.processGIFPacket(data, sizeBytes);
            });

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kChain = 0x00028000u;
            constexpr uint32_t kT4Data = 0x00029000u;
            constexpr uint32_t kCt32Data = 0x0002A000u;
            constexpr uint32_t kT4Dbp = 0u;
            constexpr uint32_t kCt32Dbp = 32u;
            constexpr uint32_t kWidth = 16u;
            constexpr uint32_t kHeight = 16u;
            constexpr uint32_t kT4Bytes = kWidth * kHeight / 2u;
            constexpr uint32_t kCt32Bytes = kWidth * kHeight * 4u;

            uint8_t *rdram = mem.getRDRAM();
            uint32_t pixel = 0u;
            for (uint32_t i = 0; i < kT4Bytes; ++i)
            {
                const uint8_t lo = static_cast<uint8_t>(pixel & 0xFu);
                const uint8_t hi = static_cast<uint8_t>((pixel + 1u) & 0xFu);
                rdram[kT4Data + i] = static_cast<uint8_t>(lo | (hi << 4));
                pixel += 2u;
                if (pixel > 0xEu)
                {
                    pixel -= 0xEu;
                }
            }

            uint32_t color = 0u;
            for (uint32_t i = 0; i < kCt32Bytes; i += 4u)
            {
                rdram[kCt32Data + i + 0u] = static_cast<uint8_t>((color >> 0) & 0xFFu);
                rdram[kCt32Data + i + 1u] = static_cast<uint8_t>((color >> 8) & 0xFFu);
                rdram[kCt32Data + i + 2u] = static_cast<uint8_t>((color >> 16) & 0xFFu);
                rdram[kCt32Data + i + 3u] = 0x80u;
                color += 0xF1u;
                if (color >= 0xFFFFFFu)
                {
                    color = 0u;
                }
            }

            uint32_t chain = kChain;
            chain = writeTextureUploadSetup(rdram, chain, kT4Dbp, GS_PSM_T4HL);
            chain = writeTextureImageRef(rdram, chain, kT4Bytes / 16u, kT4Data);
            chain = writeTextureUploadSetup(rdram, chain, kCt32Dbp, GS_PSM_CT32);
            chain = writeTextureImageRef(rdram, chain, kCt32Bytes / 16u, kCt32Data);
            writeDmaTag(rdram, chain, makeDmaTag(0u, 7u, 0u, false)); // END.

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kChain), "write GIF TADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write GIF CHCR STR|CHAIN should succeed");

            mem.processPendingTransfers();

            constexpr uint32_t kCt32PayloadOffset = 5u * 16u + 16u + kT4Bytes + 5u * 16u + 16u;
            t.IsTrue(capturedGifPacket.size() >= kCt32PayloadOffset + kCt32Bytes,
                     "flattened GIF chain should contain the full CT32 REF payload");
            bool ct32PayloadOk = capturedGifPacket.size() >= kCt32PayloadOffset + kCt32Bytes;
            for (uint32_t i = 0; i < kCt32Bytes && ct32PayloadOk; ++i)
            {
                if (capturedGifPacket[kCt32PayloadOffset + i] != rdram[kCt32Data + i])
                {
                    ct32PayloadOk = false;
                    break;
                }
            }
            t.IsTrue(ct32PayloadOk, "flattened GIF chain should preserve CT32 REF bytes after the T4 REF payload");

            const uint32_t row1Off = GSPSMCT32::addrPSMCT32(kCt32Dbp, 1u, 0u, 1u);
            uint32_t actualRow1X0 = 0u;
            uint32_t expectedRow1X0 = 0u;
            std::memcpy(&actualRow1X0, mem.getGSVRAM() + row1Off, sizeof(actualRow1X0));
            std::memcpy(&expectedRow1X0, rdram + kCt32Data + kWidth * 4u, sizeof(expectedRow1X0));
            t.Equals(actualRow1X0, expectedRow1X0, "CT32 row 1 must come from CT32 data, not the previous T4 REF payload");

            bool ct32Ok = true;
            for (uint32_t y = 0; y < kHeight && ct32Ok; ++y)
            {
                for (uint32_t x = 0; x < kWidth && ct32Ok; ++x)
                {
                    const uint32_t dstOff = GSPSMCT32::addrPSMCT32(kCt32Dbp, 1u, x, y);
                    const uint32_t srcOff = kCt32Data + ((y * kWidth + x) * 4u);
                    for (uint32_t c = 0; c < 4u; ++c)
                    {
                        if (mem.getGSVRAM()[dstOff + c] != rdram[srcOff + c])
                        {
                            ct32Ok = false;
                            break;
                        }
                    }
                }
            }
            t.IsTrue(ct32Ok, "all CT32 pixels should survive a preceding paletted upload in the same GIF DMA chain");
        });

        tc.Run("VIF1 DMA DIRECT forwards payload to GIF callback and clears channel", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSrc = 0x00024000u;
            constexpr uint32_t kQwc = 2u; // 32 bytes total transport

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kSrc, 0, kQwc * 16u);

            // DIRECT 1 QW.
            const uint32_t cmd = makeVifCmd(0x50u, 0u, 1u);
            std::memcpy(rdram + kSrc, &cmd, sizeof(cmd));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kSrc + 4u + i] = static_cast<uint8_t>((0x11u + i) & 0xFFu);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kSrc), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, kQwc), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "VIF1 DIRECT should emit one GIF packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "VIF1 DIRECT packet should be 1 QW");
            bool contentOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>((0x11u + i) & 0xFFu))
                {
                    contentOk = false;
                    break;
                }
            }
            t.IsTrue(contentOk, "VIF1 DIRECT packet bytes should match payload");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u, "VIF1 CHCR STR bit should be cleared after drain");
            t.Equals(mem.readIORegister(kVif1Ch + 0x20u), 0u, "VIF1 QWC should be cleared after drain");
        });

        tc.Run("VIF1 DMA chain preserves compact tag high bytes for DIRECT packets", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00025000u;

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kTag, 0, 32u);

            const uint64_t endTag = makeDmaTag(1u, 7u, 0u, false);
            std::memcpy(rdram + kTag, &endTag, sizeof(endTag));

            // Compact VIF1 packet helpers place the DIRECT command in the tag's upper 64 bits.
            const uint32_t directCmd = makeVifCmd(0x50u, 0u, 1u);
            std::memcpy(rdram + kTag + 12u, &directCmd, sizeof(directCmd));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kTag + 16u + i] = static_cast<uint8_t>(0x70u + i);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u),
                     "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "compact VIF1 chain should emit one GIF packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "compact VIF1 DIRECT packet should be 1 QW");

            bool payloadOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x70u + i))
                {
                    payloadOk = false;
                    break;
                }
            }
            t.IsTrue(payloadOk, "compact VIF1 chain payload should reach the GIF callback");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u,
                     "compact VIF1 chain should clear the STR bit after drain");
        });

        tc.Run("VIF1 DMA chain preserves compact tag high bytes when qwc is zero", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00025100u;

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kTag, 0, 16u);

            const uint64_t endTag = makeDmaTag(0u, 7u, 0u, false);
            std::memcpy(rdram + kTag, &endTag, sizeof(endTag));

            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x44u);
            std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u),
                     "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(mem.vif1_regs.itops, 0x44u,
                     "qwc-zero compact VIF1 chain should still process high-half VIFcodes");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u,
                     "qwc-zero compact VIF1 chain should clear the STR bit after drain");
        });

        tc.Run("VIF1 DMA chain transfers REF tag high bytes when TTE is enabled", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag0 = 0x00025200u;
            constexpr uint32_t kTag1 = kTag0 + 0x10u;
            constexpr uint32_t kRefPayload = 0x00025300u;

            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag0, makeDmaTag(1u, 3u, kRefPayload, false)); // REF
            writeDmaTag(rdram, kTag1, makeDmaTag(0u, 7u, 0u, false));         // END

            // With CHCR.TTE set, both VIFcodes stored in every DMAtag's upper half
            // precede that tag's payload, including tags whose payload is referenced.
            const uint32_t directCmd = makeVifCmd(0x50u, 0u, 1u);
            std::memcpy(rdram + kTag0 + 12u, &directCmd, sizeof(directCmd));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kRefPayload + i] = static_cast<uint8_t>(0xA0u + i);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag0), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u),
                     "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u),
                     "REF tag high-half DIRECT should emit one GIF packet");
            if (!captured.empty())
            {
                t.Equals(captured[0].size(), static_cast<size_t>(16u),
                         "REF tag high-half DIRECT packet should be 1 QW");

                bool payloadOk = true;
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    if (captured[0][i] != static_cast<uint8_t>(0xA0u + i))
                    {
                        payloadOk = false;
                        break;
                    }
                }
                t.IsTrue(payloadOk, "REF payload should reach the GIF callback without VIF desynchronization");
            }
        });

        tc.Run("VIF1 DMA chain ignores tag high bytes when TTE is disabled", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00025400u;

            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag, makeDmaTag(0u, 7u, 0u, false)); // END
            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x55u);
            std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x104u),
                     "write VIF1 CHCR STR|CHAIN without TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(mem.vif1_regs.itops, 0u,
                     "tag high-half VIFcodes must stay hidden when CHCR.TTE is clear");
        });

        tc.Run("VIF1 packet builders keep chain qwc live before terminate", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kStateAddr = 0x00027000u;
            constexpr uint32_t kBaseAddr = 0x00027100u;

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kStateAddr, 0, 0x200u);

            R5900Context ctx{};
            setRegU32(ctx, 4, kStateAddr);
            setRegU32(ctx, 5, kBaseAddr);
            ps2_stubs::sceVif1PkInit(rdram, &ctx, nullptr);

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kStateAddr);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::sceVif1PkCnt(rdram, &ctx, nullptr);

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kStateAddr);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::sceVif1PkOpenDirectCode(rdram, &ctx, nullptr);

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kStateAddr);
            setRegU32(ctx, 5, 4u); // reserve one qword of DIRECT payload
            ps2_stubs::sceVif1PkReserve(rdram, &ctx, nullptr);
            const uint32_t payloadAddr = ::getRegU32(&ctx, 2);
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[payloadAddr + i] = static_cast<uint8_t>(0x30u + i);
            }

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kStateAddr);
            ps2_stubs::sceVif1PkCloseDirectCode(rdram, &ctx, nullptr);

            uint32_t dmaTagWord = 0u;
            std::memcpy(&dmaTagWord, rdram + kBaseAddr, sizeof(dmaTagWord));
            t.Equals(dmaTagWord & 0xFFFFu, 1u, "live packet head qwc should reflect one qword before terminate");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kBaseAddr), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u),
                     "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "live VIF1 packet should emit one GIF packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "live VIF1 packet should emit one qword");

            bool payloadOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x30u + i))
                {
                    payloadOk = false;
                    break;
                }
            }
            t.IsTrue(payloadOk, "live VIF1 packet payload should reach the GIF callback");
        });

        tc.Run("VIF1 DMA chain latches terminal tag bits in CHCR", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag0 = 0x00027400u;
            constexpr uint32_t kTag1 = kTag0 + 0x20u;

            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag0, makeDmaTag(1u, 1u, 0u, false)); // CNT
            std::memset(rdram + kTag0 + 0x10u, 0, 0x10u);
            writeDmaTag(rdram, kTag1, makeDmaTag(0u, 7u, 0u, false)); // END

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag0), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x185u), "write VIF1 CHCR chain start should succeed");

            mem.processPendingTransfers();

            const uint32_t chcr = mem.readIORegister(kVif1Ch + 0x00u);
            t.Equals(chcr & 0x100u, 0u, "VIF1 STR should clear after DMA chain drain");
            t.Equals(chcr & 0x70000000u, 0x70000000u, "VIF1 CHCR should expose the terminal END tag id");
            t.IsTrue((mem.readIORegister(0x1000E010u) & 0x2u) != 0u, "VIF1 DMA completion should raise D_STAT channel bit");
        });

        tc.Run("GIF DMA chain CALL sources payload from TADR+16", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTag0 = 0x00026000u;
            constexpr uint32_t kTag1 = 0x00026100u;

            uint8_t *rdram = mem.getRDRAM();

            // CALL qwc=1 addr=kTag1
            writeDmaTag(rdram, kTag0, makeDmaTag(1u, 5u, kTag1, false));
            // END qwc=1
            writeDmaTag(rdram, kTag1, makeDmaTag(1u, 7u, 0u, false));

            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kTag0 + 16u + i] = static_cast<uint8_t>(0x40u + i); // CALL payload
                rdram[kTag1 + 16u + i] = static_cast<uint8_t>(0x80u + i); // END payload
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kTag0), "write TADR should succeed");
            // STR + CHAIN mode (MOD=1)
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write CHCR should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "chain CALL should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(32u), "CALL+END should emit two qwords");

            bool firstQwOk = true;
            bool secondQwOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x40u + i))
                    firstQwOk = false;
                if (captured[0][16u + i] != static_cast<uint8_t>(0x80u + i))
                    secondQwOk = false;
            }
            t.IsTrue(firstQwOk, "CALL must transfer from TADR+16, not DMAtag ADDR");
            t.IsTrue(secondQwOk, "END payload should follow CALL payload");
        });

        tc.Run("GIF DMA chain RET transfers payload and resumes after CALL", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTagCall = 0x00026200u;
            constexpr uint32_t kTagRet = 0x00026300u;
            constexpr uint32_t kTagEnd = 0x00026220u;

            uint8_t *rdram = mem.getRDRAM();

            // CALL qwc=1 -> jumps to RET tag
            writeDmaTag(rdram, kTagCall, makeDmaTag(1u, 5u, kTagRet, false));
            // RET qwc=1 -> should return to kTagEnd
            writeDmaTag(rdram, kTagRet, makeDmaTag(1u, 6u, 0u, false));
            // END qwc=1 after CALL payload
            writeDmaTag(rdram, kTagEnd, makeDmaTag(1u, 7u, 0u, false));

            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kTagCall + 16u + i] = static_cast<uint8_t>(0x11u + i); // CALL payload
                rdram[kTagRet + 16u + i] = static_cast<uint8_t>(0x22u + i);  // RET payload
                rdram[kTagEnd + 16u + i] = static_cast<uint8_t>(0x33u + i);  // END payload
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kTagCall), "write TADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write CHCR should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "CALL/RET chain should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(48u), "CALL+RET+END should emit three qwords");

            bool q0 = true;
            bool q1 = true;
            bool q2 = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x11u + i))
                    q0 = false;
                if (captured[0][16u + i] != static_cast<uint8_t>(0x22u + i))
                    q1 = false;
                if (captured[0][32u + i] != static_cast<uint8_t>(0x33u + i))
                    q2 = false;
            }
            t.IsTrue(q0, "CALL payload should be first");
            t.IsTrue(q1, "RET must still transfer its own payload");
            t.IsTrue(q2, "RET must resume after CALL payload and continue chain");
        });

        tc.Run("GIF DMA chain IRQ stops only when TIE is set", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTag0 = 0x00026400u;
            constexpr uint32_t kTag1 = 0x00026410u;
            constexpr uint32_t kRefData = 0x00026500u;

            auto runChain = [&](uint32_t chcrValue, std::vector<uint8_t> &packetOut) -> bool
            {
                uint8_t *rdram = mem.getRDRAM();
                writeDmaTag(rdram, kTag0, makeDmaTag(1u, 3u, kRefData, true)); // REF + IRQ
                writeDmaTag(rdram, kTag1, makeDmaTag(1u, 7u, 0u, false));       // END
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    rdram[kRefData + i] = static_cast<uint8_t>(0x55u + i);
                    rdram[kTag1 + 16u + i] = static_cast<uint8_t>(0x77u + i);
                }

                std::vector<std::vector<uint8_t>> captured;
                mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
                {
                    captured.emplace_back(data, data + sizeBytes);
                });

                if (!mem.writeIORegister(kGifCh + 0x30u, kTag0))
                    return false;
                if (!mem.writeIORegister(kGifCh + 0x00u, chcrValue))
                    return false;
                mem.processPendingTransfers();
                if (captured.empty())
                    return false;
                packetOut = captured[0];
                return true;
            };

            std::vector<uint8_t> packetNoTie;
            t.IsTrue(runChain(0x104u, packetNoTie), "chain run without TIE should succeed");
            t.Equals(packetNoTie.size(), static_cast<size_t>(32u), "IRQ tag should not stop chain when TIE is clear");

            std::vector<uint8_t> packetTie;
            // STR + CHAIN + TIE(bit7)
            t.IsTrue(runChain(0x184u, packetTie), "chain run with TIE should succeed");
            t.Equals(packetTie.size(), static_cast<size_t>(16u), "IRQ tag should stop chain when TIE is set");
        });

        tc.Run("DMAC D_STAT toggles masks and clears channel status on write-one", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kGifMaskBit = (1u << 18); // channel 2 mask
            constexpr uint32_t kGifStatusBit = (1u << 2); // channel 2 status
            constexpr uint32_t kSummaryBit = (1u << 31);

            t.IsTrue(mem.writeIORegister(kDStat, kGifMaskBit), "D_STAT mask toggle write should succeed");
            t.IsTrue((mem.readIORegister(kDStat) & kGifMaskBit) != 0u, "first mask write should enable GIF mask bit");
            t.IsTrue(mem.writeIORegister(kDStat, kGifMaskBit), "D_STAT mask toggle write should succeed");
            t.IsTrue((mem.readIORegister(kDStat) & kGifMaskBit) == 0u, "second mask write should disable GIF mask bit");

            t.IsTrue(mem.writeIORegister(kDStat, kGifMaskBit), "re-enable GIF mask for summary test");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kSrc = 0x00027000u;
            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kSrc + i] = static_cast<uint8_t>(0x90u + i);
            }

            t.IsTrue(mem.writeIORegister(kGifCh + 0x10u, kSrc), "write MADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x20u, 1u), "write QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "write CHCR STR should succeed");

            t.IsTrue((mem.readIORegister(kDStat) & kGifStatusBit) == 0u, "D_STAT status should not set before transfer drain");

            mem.processPendingTransfers();

            const uint32_t dstatAfter = mem.readIORegister(kDStat);
            t.IsTrue((dstatAfter & kGifStatusBit) != 0u, "GIF transfer completion should set D_STAT channel status bit");
            t.IsTrue((dstatAfter & kSummaryBit) != 0u, "status&mask should raise D_STAT summary bit");

            t.IsTrue(mem.writeIORegister(kDStat, kGifStatusBit), "D_STAT status clear write should succeed");
            const uint32_t dstatCleared = mem.readIORegister(kDStat);
            t.IsTrue((dstatCleared & kGifStatusBit) == 0u, "write-one should clear GIF channel status bit");
            t.IsTrue((dstatCleared & kSummaryBit) == 0u, "summary bit should clear after status clear");
        });

        tc.Run("DMAC D_CTRL DMAE gates GIF DMA start", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDctrl = 0x1000E000u;
            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kSrc = 0x00027800u;

            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kSrc + i] = static_cast<uint8_t>(0xE0u + i);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kDctrl, 0u), "clearing D_CTRL.DMAE should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x10u, kSrc), "write MADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x20u, 1u), "write QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "write CHCR STR should succeed");
            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(0u), "DMAE=0 should prevent GIF DMA transfer");
            t.Equals(mem.dmaStartCount(), 0ull, "DMAE=0 should not increment dmaStartCount");

            t.IsTrue(mem.writeIORegister(kDctrl, 1u), "setting D_CTRL.DMAE should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "restarting GIF DMA should succeed");
            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "DMAE=1 should allow GIF DMA transfer");
            if (!captured.empty())
            {
                t.Equals(captured[0].size(), static_cast<size_t>(16u), "GIF DMA transfer should emit one qword");
            }
        });

        tc.Run("DMAC D_ENABLER reads 0x1201 after reset with CPND clear", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDEnableR = 0x1000F520u;
            constexpr uint32_t kCpnd = 1u << 16; // DMAC suspended

            // The hardware reports 0x1201 at boot (PCSX2 hwReset, ps2tek); bit 16 is
            // clear, so the DMAC starts enabled.
            t.Equals(mem.read32(kDEnableR), 0x1201u, "D_ENABLER should read 0x1201 after reset");
            t.IsTrue((mem.read32(kDEnableR) & kCpnd) == 0u, "CPND should be clear after reset");
        });

        tc.Run("DMAC D_ENABLEW writes are visible at D_ENABLER", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDEnableR = 0x1000F520u;
            constexpr uint32_t kDEnableW = 0x1000F590u;
            constexpr uint32_t kCpnd = 1u << 16;

            mem.write32(kDEnableW, kCpnd);
            t.Equals(mem.read32(kDEnableR), kCpnd, "sw D_ENABLEW should be readable at D_ENABLER");

            mem.write32(kDEnableW, 0u);
            t.Equals(mem.read32(kDEnableR), 0u, "clearing D_ENABLEW should clear D_ENABLER");

            // Narrow accesses merge into the same 32-bit register: PCSX2 polls the
            // suspend state with a byte read of D_ENABLER+2.
            mem.write16(kDEnableW + 2u, 0x1u);
            t.Equals(mem.read32(kDEnableR), kCpnd, "sh to the upper half of D_ENABLEW should set CPND at D_ENABLER");
            t.Equals(mem.read8(kDEnableR + 2u), static_cast<uint8_t>(1u), "lb of D_ENABLER byte 2 should expose CPND");
        });

        tc.Run("DMAC suspend and resume polls through D_ENABLEW and D_ENABLER terminate", [](TestCase &t)
        {
            // Ridge Racer V brackets its DMAC interrupt handler and frame kick with
            //   while ((D_ENABLER & 0x10000) == 0) D_ENABLEW = 0x10000;
            //   while ((D_ENABLER & 0x10000) != 0) D_ENABLEW = 0;
            // which spins forever unless the two ports are one register.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDEnableR = 0x1000F520u;
            constexpr uint32_t kDEnableW = 0x1000F590u;
            constexpr uint32_t kCpnd = 1u << 16;
            constexpr uint32_t kMaxPolls = 8u;

            uint32_t suspendPolls = 0u;
            while ((mem.read32(kDEnableR) & kCpnd) == 0u && suspendPolls < kMaxPolls)
            {
                mem.write32(kDEnableW, kCpnd);
                ++suspendPolls;
            }
            t.Equals(suspendPolls, 1u, "the suspend request should be visible on the first poll");

            uint32_t resumePolls = 0u;
            while ((mem.read32(kDEnableR) & kCpnd) != 0u && resumePolls < kMaxPolls)
            {
                mem.write32(kDEnableW, 0u);
                ++resumePolls;
            }
            t.Equals(resumePolls, 1u, "the resume request should be visible on the first poll");
        });

        tc.Run("DMAC SPR_FROM copies scratchpad to RDRAM and completes channel 8", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kChannel = 0x1000D000u;
            constexpr uint32_t kMadr = 0x00028000u;
            constexpr uint32_t kSadr = 0x00000120u;
            constexpr uint32_t kQwc = 2u;
            constexpr uint32_t kBytes = kQwc * 16u;

            for (uint32_t i = 0; i < kBytes; ++i)
                mem.getScratchpad()[kSadr + i] = static_cast<uint8_t>(0x30u + i);

            t.IsTrue(mem.writeIORegister(kChannel + 0x10u, kMadr), "write SPR_FROM MADR should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x20u, kQwc), "write SPR_FROM QWC should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x80u, kSadr), "write SPR_FROM SADR should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x00u, 0x100u), "start SPR_FROM should succeed");

            bool copied = true;
            for (uint32_t i = 0; i < kBytes; ++i)
                copied = copied && mem.getRDRAM()[kMadr + i] == static_cast<uint8_t>(0x30u + i);
            t.IsTrue(copied, "SPR_FROM should copy every qword from scratchpad to RDRAM");
            t.IsTrue((mem.readIORegister(kChannel + 0x00u) & 0x100u) == 0u, "SPR_FROM completion should clear CHCR.STR");
            t.Equals(mem.readIORegister(kChannel + 0x20u), 0u, "SPR_FROM completion should consume QWC");
            t.Equals(mem.readIORegister(kChannel + 0x10u), kMadr + kBytes, "SPR_FROM should advance MADR");
            t.Equals(mem.readIORegister(kChannel + 0x80u), (kSadr + kBytes) & 0x3FFFu, "SPR_FROM should advance SADR");
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 8u)) != 0u, "SPR_FROM should raise D_STAT channel 8");

            const std::vector<uint32_t> causes = mem.consumeCompletedDmacCauses();
            t.Equals(causes.size(), static_cast<size_t>(1u), "SPR_FROM should queue one DMAC completion");
            if (!causes.empty())
                t.Equals(causes[0], 8u, "SPR_FROM completion should use DMAC cause 8");
        });

        tc.Run("DMAC SPR_TO copies RDRAM to scratchpad and completes channel 9", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kChannel = 0x1000D400u;
            constexpr uint32_t kMadr = 0x00028400u;
            constexpr uint32_t kSadr = 0x00000240u;
            constexpr uint32_t kQwc = 2u;
            constexpr uint32_t kBytes = kQwc * 16u;

            for (uint32_t i = 0; i < kBytes; ++i)
                mem.getRDRAM()[kMadr + i] = static_cast<uint8_t>(0x70u + i);

            t.IsTrue(mem.writeIORegister(kChannel + 0x10u, kMadr), "write SPR_TO MADR should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x20u, kQwc), "write SPR_TO QWC should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x80u, kSadr), "write SPR_TO SADR should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x00u, 0x100u), "start SPR_TO should succeed");

            bool copied = true;
            for (uint32_t i = 0; i < kBytes; ++i)
                copied = copied && mem.getScratchpad()[kSadr + i] == static_cast<uint8_t>(0x70u + i);
            t.IsTrue(copied, "SPR_TO should copy every qword from RDRAM to scratchpad");
            t.IsTrue((mem.readIORegister(kChannel + 0x00u) & 0x100u) == 0u, "SPR_TO completion should clear CHCR.STR");
            t.Equals(mem.readIORegister(kChannel + 0x20u), 0u, "SPR_TO completion should consume QWC");
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 9u)) != 0u, "SPR_TO should raise D_STAT channel 9");

            const std::vector<uint32_t> causes = mem.consumeCompletedDmacCauses();
            t.Equals(causes.size(), static_cast<size_t>(1u), "SPR_TO should queue one DMAC completion");
            if (!causes.empty())
                t.Equals(causes[0], 9u, "SPR_TO completion should use DMAC cause 9");
        });

        // MFIFO: fromSPR fills the ring D_RBOR/D_RBSR, the GIF (MFD=3) or VIF1 (MFD=2) source chain drains it.
        // Expected behaviour from PCSX2 (SPR.cpp, Hw.cpp hwMFIFOWrite/Resume) and DobieStation (dmac.cpp
        // mfifo_handler); no hardware program covers MFIFO (docs/engineering/PS2_IMPLEMENTATION_DECISIONS.md D-DMA-1/2).

        tc.Run("MFIFO GIF drain stalls on an empty ring with MEIS and resumes when fromSPR adds data", [](TestCase &t)
        {
            MfifoRig rig(0xFF0u); // 4 KB ring
            t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
            rig.startGifChain(MfifoRig::kRing);
            t.Equals(rig.received.size(), static_cast<size_t>(0u), "nothing is transferred from an empty ring");
            t.IsTrue(rig.gifActive(), "the drain stays active (STR) while the ring is empty");
            t.IsTrue((rig.dstat() & (1u << 14)) != 0u, "D_STAT.MEIS is raised");
            t.IsTrue((rig.dstat() & (1u << 2)) == 0u, "no CIS2 while the drain waits");

            rig.fromSpr({makeDmaTag(1u, 1u, 0u), 0ull, 0x1111111111111111ull, 0x2222222222222222ull}); // CNT qwc 1 + data
            t.Equals(rig.received.size(), static_cast<size_t>(16u), "the CNT data is transferred once it is in the ring");
            t.IsTrue(rig.gifActive(), "the drain waits again for its next tag");
            t.Equals(rig.mem.readIORegister(MfifoRig::kGif + 0x30u), MfifoRig::kRing + 0x20u, "TADR points at the next tag");

            rig.fromSpr({makeDmaTag(1u, 7u, 0u), 0ull, 0x3333333333333333ull, 0x4444444444444444ull}); // END qwc 1 + data
            t.Equals(rig.received.size(), static_cast<size_t>(32u), "the END data is transferred");
            t.IsTrue(!rig.gifActive(), "END completes the channel");
            t.IsTrue((rig.dstat() & (1u << 2)) != 0u, "CIS2 is raised at completion");
            uint64_t words[4] = {};
            std::memcpy(words, rig.received.data(), std::min<size_t>(rig.received.size(), sizeof(words)));
            t.IsTrue(words[0] == 0x1111111111111111ull && words[1] == 0x2222222222222222ull &&
                         words[2] == 0x3333333333333333ull && words[3] == 0x4444444444444444ull,
                     "the payload arrives unchanged and in order");
        });

        tc.Run("MFIFO VIF1 drain (MFD=2) waits for fromSPR and then executes the VIF codes it drains", [](TestCase &t)
        {
            MfifoRig rig(0xFF0u, 2u);
            t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
            constexpr uint32_t kVif1 = 0x10009000u;
            rig.mem.writeIORegister(kVif1 + 0x30u, MfifoRig::kRing);
            rig.mem.writeIORegister(kVif1 + 0x20u, 0u);
            rig.mem.writeIORegister(kVif1 + 0x00u, 0x105u);
            t.IsTrue((rig.mem.readIORegister(kVif1) & 0x100u) != 0u, "the VIF1 drain stays active while the ring is empty");
            // CNT qwc 1 carrying MARK 0x1111 and three NOPs: drained, then the drain waits for its next tag.
            rig.fromSpr({makeDmaTag(1u, 1u, 0u), 0ull, 0x07001111ull, 0ull});
            t.Equals(rig.mem.vif1_regs.mark, 0x1111u, "the first drained MARK reaches VIF1");
            t.IsTrue((rig.dstat() & (1u << 1)) == 0u, "no CIS1 while the drain waits for its next tag");
            t.IsTrue((rig.mem.readIORegister(kVif1) & 0x100u) != 0u, "the VIF1 drain is still active");
            // END qwc 1 carrying MARK 0x1234.
            rig.fromSpr({makeDmaTag(1u, 7u, 0u), 0ull, 0x07001234ull, 0ull});
            t.Equals(rig.mem.vif1_regs.mark, 0x1234u, "the second drained MARK reaches VIF1");
            t.IsTrue((rig.dstat() & (1u << 1)) != 0u, "END raises CIS1");
            t.IsTrue((rig.mem.readIORegister(kVif1) & 0x100u) == 0u, "END completes the VIF1 channel");
        });

        tc.Run("MFIFO tags and following data wrap at the ring end; REF data is read in place", [](TestCase &t)
        {
            MfifoRig rig(0x70u); // 8-qword ring (a completely full ring reads as empty, as on both references)
            t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
            constexpr uint32_t kOutside = 0x00200000u;
            const uint64_t refData[2] = {0x5555555555555555ull, 0x6666666666666666ull};
            std::memcpy(rig.mem.getRDRAM() + kOutside, refData, sizeof(refData));
            // Move the write position to the last ring slot, then write CNT(1) at +0x70 whose data wraps to +0x00,
            // REF(1) at +0x10 pointing outside the ring, and END(0) at +0x20.
            rig.mem.writeIORegister(MfifoRig::kSprFrom + 0x10u, MfifoRig::kRing + 0x70u);
            rig.fromSpr({makeDmaTag(1u, 1u, 0u), 0ull, 0x7777777777777777ull, 0x8888888888888888ull,
                         makeDmaTag(1u, 3u, kOutside), 0ull, makeDmaTag(0u, 7u, 0u), 0ull});
            t.Equals(rig.mem.readIORegister(MfifoRig::kSprFrom + 0x10u), MfifoRig::kRing + 0x30u,
                     "fromSPR MADR wraps around the ring (0x70 + 4 qwords)");
            rig.startGifChain(MfifoRig::kRing + 0x70u);
            t.Equals(rig.received.size(), static_cast<size_t>(32u), "CNT data and REF data are transferred");
            uint64_t words[4] = {};
            std::memcpy(words, rig.received.data(), std::min<size_t>(rig.received.size(), sizeof(words)));
            t.IsTrue(words[0] == 0x7777777777777777ull && words[1] == 0x8888888888888888ull,
                     "the CNT data is read from the wrapped position +0x00");
            t.IsTrue(words[2] == 0x5555555555555555ull && words[3] == 0x6666666666666666ull,
                     "the REF data is read from its own address, outside the ring");
            t.IsTrue(!rig.gifActive(), "END completes the channel");
        });

        tc.Run("fromSPR writes linearly when MFD selects no drain or MADR is outside the ring", [](TestCase &t)
        {
            for (const bool mfdOff : {true, false})
            {
                MfifoRig rig(0x30u, mfdOff ? 0u : 3u);
                t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
                const uint32_t start = mfdOff ? MfifoRig::kRing + 0x30u : 0x00300000u;
                rig.mem.writeIORegister(MfifoRig::kSprFrom + 0x10u, start);
                rig.fromSpr({1ull, 2ull, 3ull, 4ull, 5ull, 6ull});
                t.Equals(rig.mem.readIORegister(MfifoRig::kSprFrom + 0x10u), start + 0x30u, "MADR advances without wrapping");
                uint64_t last = 0u;
                std::memcpy(&last, rig.mem.getRDRAM() + start + 0x20u, sizeof(last));
                t.Equals(last, 5ull, "the third qword lands at MADR + 0x20");
            }
        });

        tc.Run("D_STAT MEIS clears on write-one and raises the INT1 line only when MEIM is set", [](TestCase &t)
        {
            MfifoRig rig(0xFF0u);
            t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
            rig.startGifChain(MfifoRig::kRing);
            t.IsTrue((rig.dstat() & (1u << 14)) != 0u, "MEIS is raised by the empty ring");
            t.IsTrue((rig.dstat() & (1u << 31)) == 0u, "no INT1 while MEIM is clear");
            rig.mem.writeIORegister(MfifoRig::kDStat, 1u << 30); // toggle MEIM on
            t.IsTrue((rig.dstat() & (1u << 30)) != 0u, "MEIM toggles on");
            t.IsTrue((rig.dstat() & (1u << 31)) != 0u, "MEIS with MEIM raises INT1");
            rig.mem.writeIORegister(MfifoRig::kDStat, 1u << 14); // clear MEIS
            t.IsTrue((rig.dstat() & (1u << 14)) == 0u, "MEIS clears on write-one");
            t.IsTrue((rig.dstat() & (1u << 31)) == 0u, "INT1 drops with MEIS cleared");
            t.IsTrue((rig.dstat() & (1u << 30)) != 0u, "MEIM stays set");
        });

        // Stall control: D_CTRL.STS (bits 4-5) names the source whose MADR is copied into D_STADR, D_CTRL.STD
        // (bits 6-7) the drain that may not read past D_STADR. Expected behaviour where PCSX2 (Gif.cpp, Vif1_Dma.cpp,
        // SPR.cpp) and DobieStation (dmac.cpp) agree; no hardware program covers it (D-DMA-4).

        tc.Run("stall control: a GIF REFS drain waits with SIS until fromSPR (STS) moves D_STADR past its data", [](TestCase &t)
        {
            MfifoRig rig(0x30u, 0u);
            t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
            rig.mem.writeIORegister(0x1000E000u, 1u | (2u << 4) | (2u << 6)); // STS = fromSPR, STD = GIF
            rig.mem.writeIORegister(MfifoRig::kSprFrom + 0x10u, kStallData);
            uint8_t *rdram = rig.mem.getRDRAM();
            writeDmaTag(rdram, kStallChain, makeDmaTag(2u, 4u, kStallData));      // REFS qwc 2
            writeDmaTag(rdram, kStallChain + 0x10u, makeDmaTag(0u, 7u, 0u));      // END
            rig.startGifChain(kStallChain);
            t.Equals(rig.received.size(), static_cast<size_t>(0u), "nothing below D_STADR yet: no REFS data is read");
            t.IsTrue(rig.gifActive(), "the drain stays active (STR) at the stall address");
            t.IsTrue((rig.dstat() & kSis) != 0u, "D_STAT.SIS is raised");
            t.IsTrue((rig.dstat() & (1u << 2)) == 0u, "no CIS2 while the drain waits");
            t.Equals(rig.mem.readIORegister(MfifoRig::kGif + 0x30u), kStallChain, "TADR still points at the REFS tag");

            rig.mem.writeIORegister(MfifoRig::kDStat, kSis); // clear SIS: the same stall must not raise it again
            rig.fromSpr({0x1111111111111111ull, 0x2222222222222222ull});
            t.Equals(rig.mem.readIORegister(kDStadr), kStallData + 0x10u, "fromSPR (STS) leaves its MADR in D_STADR");
            t.Equals(rig.received.size(), static_cast<size_t>(0u), "one qword below D_STADR is not enough for REFS qwc 2");
            t.IsTrue(rig.gifActive(), "the drain still waits");
            t.IsTrue((rig.dstat() & kSis) == 0u, "SIS is raised once per stall, not again on each check");

            rig.fromSpr({0x3333333333333333ull, 0x4444444444444444ull});
            t.Equals(rig.mem.readIORegister(kDStadr), kStallData + 0x20u, "D_STADR follows fromSPR's MADR");
            t.Equals(rig.received.size(), static_cast<size_t>(32u), "the REFS data is read once it lies below D_STADR");
            t.IsTrue(!rig.gifActive(), "END completes the channel");
            t.IsTrue((rig.dstat() & (1u << 2)) != 0u, "CIS2 is raised at completion");
            uint64_t words[4] = {};
            std::memcpy(words, rig.received.data(), std::min<size_t>(rig.received.size(), sizeof(words)));
            t.IsTrue(words[0] == 0x1111111111111111ull && words[1] == 0x2222222222222222ull &&
                         words[2] == 0x3333333333333333ull && words[3] == 0x4444444444444444ull,
                     "the drain reads what fromSPR wrote, in order");
        });

        tc.Run("stall control applies only to the STD drain's REFS tags, up to and including D_STADR", [](TestCase &t)
        {
            struct Case
            {
                const char *name;
                uint32_t dctrl;
                uint8_t tagId;
                uint32_t stadr;
            };
            const Case cases[] = {
                {"REFS ending exactly at D_STADR proceeds", 1u | (2u << 6), 4u, kStallData + 0x20u},
                {"REF ignores D_STADR", 1u | (2u << 6), 3u, 0u},
                {"STD = 0 disables stall control", 1u, 4u, 0u},
                {"STD = VIF1 does not stall the GIF", 1u | (1u << 6), 4u, 0u},
            };
            for (const Case &c : cases)
            {
                MfifoRig rig(0x30u, 0u);
                t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
                rig.mem.writeIORegister(0x1000E000u, c.dctrl);
                rig.mem.writeIORegister(kDStadr, c.stadr);
                uint8_t *rdram = rig.mem.getRDRAM();
                writeDmaTag(rdram, kStallChain, makeDmaTag(2u, c.tagId, kStallData));
                writeDmaTag(rdram, kStallChain + 0x10u, makeDmaTag(0u, 7u, 0u));
                rig.startGifChain(kStallChain);
                t.Equals(rig.received.size(), static_cast<size_t>(32u), c.name);
                t.IsTrue(!rig.gifActive(), c.name);
                t.IsTrue((rig.dstat() & kSis) == 0u, c.name);
            }

            // Without STS = fromSPR, a fromSPR transfer leaves D_STADR alone.
            MfifoRig rig(0x30u, 0u);
            rig.mem.writeIORegister(0x1000E000u, 1u | (2u << 6));
            rig.mem.writeIORegister(kDStadr, 0x1230u);
            rig.mem.writeIORegister(MfifoRig::kSprFrom + 0x10u, kStallData);
            rig.fromSpr({1ull, 2ull});
            t.Equals(rig.mem.readIORegister(kDStadr), 0x1230u, "STS = 0: D_STADR is not updated by fromSPR");
        });

        tc.Run("stall control: a VIF1 normal-mode drain resumes on an EE write to D_STADR; SIS reaches INT1 only with SIM", [](TestCase &t)
        {
            MfifoRig rig(0x30u, 0u);
            t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
            constexpr uint32_t kVif1 = 0x10009000u;
            rig.mem.writeIORegister(0x1000E000u, 1u | (1u << 6)); // STD = VIF1
            rig.mem.writeIORegister(kDStadr, kStallData);
            writeU64(rig.mem.getRDRAM(), kStallData, 0x07004321ull); // MARK 0x4321, NOP
            writeU64(rig.mem.getRDRAM(), kStallData + 8u, 0ull);
            rig.mem.writeIORegister(kVif1 + 0x10u, kStallData);
            rig.mem.writeIORegister(kVif1 + 0x20u, 1u);
            rig.mem.writeIORegister(kVif1 + 0x00u, 0x101u); // normal mode, from memory, STR
            t.Equals(rig.mem.vif1_regs.mark, 0u, "the blocked transfer delivers nothing");
            t.IsTrue((rig.mem.readIORegister(kVif1) & 0x100u) != 0u, "the VIF1 drain stays active");
            t.Equals(rig.mem.readIORegister(kVif1 + 0x10u), kStallData, "MADR is untouched while the drain waits");
            t.IsTrue((rig.dstat() & kSis) != 0u, "SIS is raised");
            t.IsTrue((rig.dstat() & (1u << 31)) == 0u, "no INT1 while SIM is clear");
            rig.mem.writeIORegister(MfifoRig::kDStat, 1u << 29); // toggle SIM on
            t.IsTrue((rig.dstat() & (1u << 31)) != 0u, "SIS with SIM raises INT1");

            rig.mem.writeIORegister(kDStadr, kStallData + 0x10u);
            t.Equals(rig.mem.vif1_regs.mark, 0x4321u, "the drain transfers once D_STADR covers its qword");
            t.IsTrue((rig.mem.readIORegister(kVif1) & 0x100u) == 0u, "the transfer completes");
            t.IsTrue((rig.dstat() & (1u << 1)) != 0u, "CIS1 is raised");
            t.Equals(rig.mem.readIORegister(kVif1 + 0x10u), kStallData + 0x10u, "MADR ends past the data");
            rig.mem.writeIORegister(MfifoRig::kDStat, kSis);
            t.IsTrue((rig.dstat() & kSis) == 0u, "SIS clears on write-one");
            t.IsTrue((rig.dstat() & (1u << 31)) == 0u, "INT1 drops with SIS cleared");
        });

        tc.Run("stall control: a GIF drain held after delivering earlier tags stays active; SIS with SIM queues cause 13 once", [](TestCase &t)
        {
            MfifoRig rig(0x30u, 0u);
            t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
            rig.mem.writeIORegister(0x1000E000u, 1u | (2u << 6));   // STD = GIF
            rig.mem.writeIORegister(MfifoRig::kDStat, 1u << 29);    // SIM on before the stall
            (void)rig.mem.consumeCompletedDmacCauses();
            uint8_t *rdram = rig.mem.getRDRAM();
            writeDmaTag(rdram, kStallChain, makeDmaTag(1u, 1u, 0u));                   // CNT qwc 1
            writeU64(rdram, kStallChain + 0x10u, 0x5151515151515151ull);
            writeU64(rdram, kStallChain + 0x18u, 0x5252525252525252ull);
            writeDmaTag(rdram, kStallChain + 0x20u, makeDmaTag(1u, 4u, kStallData));   // REFS qwc 1 (blocked)
            writeDmaTag(rdram, kStallChain + 0x30u, makeDmaTag(0u, 7u, 0u));           // END
            rig.startGifChain(kStallChain);
            t.Equals(rig.received.size(), static_cast<size_t>(16u), "the CNT data before the REFS tag is delivered");
            t.IsTrue(rig.gifActive(), "the drain is still active at the REFS tag");
            t.IsTrue((rig.dstat() & (1u << 2)) == 0u, "no CIS2 for the delivered part");
            t.Equals(rig.mem.readIORegister(MfifoRig::kGif + 0x30u), kStallChain + 0x20u, "TADR points at the REFS tag");
            const std::vector<uint32_t> causes = rig.mem.consumeCompletedDmacCauses();
            t.Equals(static_cast<size_t>(std::count(causes.begin(), causes.end(), 13u)), static_cast<size_t>(1u),
                     "SIS with SIM queues the DMAC handler for cause 13 once");
            t.Equals(static_cast<size_t>(std::count(causes.begin(), causes.end(), 2u)), static_cast<size_t>(0u),
                     "no cause 2 while the GIF drain is held");
            rig.mem.writeIORegister(kDStadr, kStallData + 0x10u);
            t.Equals(rig.received.size(), static_cast<size_t>(32u), "the REFS data follows once D_STADR covers it");
            t.IsTrue(!rig.gifActive(), "END completes the channel");
        });

        // SIF1 (channel 6): IOP packets = 16-byte IOP tag (address bits 0-23, bit 31/30 end, word count) + data
        // (PCSX2 Sif1.cpp and DobieStation iop_dma.cpp agree). No hardware program drives channel 6 directly.
        tc.Run("SIF1 normal mode delivers each IOP packet to the sink and completes with CIS6", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::vector<PS2Memory::Sif1Packet> got;
            mem.setSif1Sink([&](const std::vector<PS2Memory::Sif1Packet> &p) { got.insert(got.end(), p.begin(), p.end()); });
            uint8_t *rdram = mem.getRDRAM();
            constexpr uint32_t kSrc = 0x00400000u;
            const uint32_t stream[] = {0x00012340u, 4u, 0u, 0u, 0xA1u, 0xA2u, 0xA3u, 0xA4u,           // packet 1: 4 words
                                       0x80056780u, 8u, 0u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}; // packet 2, IRQ: end
            std::memcpy(rdram + kSrc, stream, sizeof(stream));
            mem.writeIORegister(0x1000C410u, kSrc);
            mem.writeIORegister(0x1000C420u, sizeof(stream) / 16u);
            mem.writeIORegister(0x1000C400u, 0x101u); // from memory, normal, STR
            t.Equals(got.size(), static_cast<size_t>(2u), "two IOP packets");
            if (got.size() == 2u)
            {
                t.Equals(got[0].iopAddress, 0x00012340u, "packet 1 IOP address");
                t.Equals(got[0].data.size(), static_cast<size_t>(16u), "packet 1 carries 4 words");
                t.IsTrue(!got[0].end && got[1].end, "bit 31 of the IOP tag ends the transfer");
                t.Equals(got[1].iopAddress, 0x00056780u, "packet 2 IOP address (flags masked)");
                uint32_t last = 0u;
                std::memcpy(&last, got[1].data.data() + 28u, 4u);
                t.Equals(last, 8u, "packet 2 data arrives in order");
            }
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 6)) != 0u, "CIS6 is raised");
            t.IsTrue((mem.readIORegister(0x1000C400u) & 0x100u) == 0u, "the channel completes");
            t.Equals(mem.readIORegister(0x1000C410u), kSrc + static_cast<uint32_t>(sizeof(stream)), "MADR ends past the data");
        });

        tc.Run("SIF1 chain stalled at a REFS tag delivers the packet whole once D_STADR moves (STD=3)", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::vector<PS2Memory::Sif1Packet> got;
            mem.setSif1Sink([&](const std::vector<PS2Memory::Sif1Packet> &p) { got.insert(got.end(), p.begin(), p.end()); });
            uint8_t *rdram = mem.getRDRAM();
            mem.writeIORegister(0x1000E000u, 1u | (3u << 6)); // STD = SIF1
            // CNT carries the IOP tag (packet of 4 words, end), REFS carries its data from kStallData.
            writeDmaTag(rdram, kStallChain, makeDmaTag(1u, 1u, 0u));
            const uint32_t header[4] = {0x40002000u, 4u, 0u, 0u};
            std::memcpy(rdram + kStallChain + 16u, header, sizeof(header));
            writeDmaTag(rdram, kStallChain + 32u, makeDmaTag(1u, 4u, kStallData));
            writeDmaTag(rdram, kStallChain + 48u, makeDmaTag(0u, 7u, 0u));
            const uint32_t data[4] = {0xD0u, 0xD1u, 0xD2u, 0xD3u};
            std::memcpy(rdram + kStallData, data, sizeof(data));
            mem.writeIORegister(0x1000C430u, kStallChain);
            mem.writeIORegister(0x1000C420u, 0u);
            mem.writeIORegister(0x1000C400u, 0x105u); // chain, STR
            t.Equals(got.size(), static_cast<size_t>(0u), "nothing reaches the IOP while the packet's data is behind D_STADR");
            t.IsTrue((mem.readIORegister(0x1000C400u) & 0x100u) != 0u, "SIF1 stays active");
            t.IsTrue((mem.readIORegister(0x1000E010u) & kSis) != 0u, "SIS is raised");
            mem.writeIORegister(kDStadr, kStallData + 0x10u);
            t.Equals(got.size(), static_cast<size_t>(1u), "the packet is delivered once");
            if (!got.empty())
            {
                t.Equals(got[0].iopAddress, 0x00002000u, "IOP address");
                t.Equals(got[0].data.size(), static_cast<size_t>(16u), "the whole packet, header and REFS data together");
                t.IsTrue(got[0].end, "bit 30 ends the transfer");
            }
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 6)) != 0u, "CIS6 after the chain's END");
        });

        tc.Run("SIF1 without an IOP sink does not transfer or raise CIS6", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            mem.writeIORegister(0x1000C410u, 0x00400000u);
            mem.writeIORegister(0x1000C420u, 1u);
            mem.writeIORegister(0x1000C400u, 0x101u);
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 6)) == 0u, "no CIS6 without a receiver");
            const std::vector<uint8_t> stream(48u, 0u);
            const auto packets = PS2Memory::splitSif1Stream(stream);
            t.Equals(packets.size(), static_cast<size_t>(3u), "zero-length packets without an end flag continue");
            std::vector<uint8_t> masked(64u, 0u); // room for 7 words, so an unmasked count would take 28 bytes
            masked[4] = 0x07u;                     // 7 words: the IOP channel takes 4 (count & 0xFFFFC)
            const auto one = PS2Memory::splitSif1Stream(masked);
            t.IsTrue(!one.empty() && one[0].data.size() == 16u, "the word count is masked to whole qwords");
        });

        // IPU: register file, input FIFO and the non-decoding commands. Expected values from the register layout
        // (ps2tek) and the MPEG bit order (bytes in memory order, MSB first); no hardware program checks the IPU.
        tc.Run("IPU FDEC returns the next 32 bits after its skip without consuming them; BCLR sets BP", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            const uint8_t bytes[16] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0, 0, 0, 0, 0, 0, 0, 0};
            mem.write128(0x10007010u, _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes)));
            t.Equals(mem.readIORegister(0x10002010u) & 0xFu, 1u, "CTRL.IFC counts the input qword");
            mem.writeIORegister(0x10002000u, (4u << 28) | 4u); // FDEC, skip 4 bits
            t.Equals(mem.readIORegister(0x10002000u), 0x23456789u, "FDEC result: 32 bits after the first 4, MSB first");
            t.IsTrue((mem.readIORegister(0x10002004u) & 0x80000000u) == 0u && (mem.readIORegister(0x10002010u) & 0x80000000u) == 0u,
                     "FDEC completes (CMD bit 63 and CTRL.BUSY clear)");
            t.Equals(mem.readIORegister(0x10002020u) & 0x7Fu, 4u, "FDEC consumed only its skip (BP = 4)");
            t.Equals(mem.readIORegister(0x10002030u), 0x23456789u, "TOP shows the same 32 bits");
            t.IsTrue(mem.takePendingIpuInterrupt(), "a completed command raises the IPU interrupt");

            mem.writeIORegister(0x10002000u, (0u << 28) | 112u); // BCLR, BP = 112
            t.Equals(mem.readIORegister(0x10002010u) & 0xFu, 0u, "BCLR empties the input FIFO");
            mem.write128(0x10007010u, _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes)));
            t.Equals(mem.readIORegister(0x10002020u), 112u | (1u << 16), "BP 112 in a partly consumed qword: FP = 1, IFC = 0");
            // 16 bits left (bytes 14-15 = 0): TOP left-aligns them and sets bit 63 for "fewer than 32 bits".
            t.IsTrue((mem.readIORegister(0x10002034u) & 0x80000000u) != 0u, "TOP bit 63 flags fewer than 32 bits");
        });

        tc.Run("IPU SETIQ loads a quantizer matrix through toIPU DMA, which completes with CIS4", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            constexpr uint32_t kSrc = 0x00420000u;
            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < 64u; ++i)
                rdram[kSrc + i] = static_cast<uint8_t>(i + 1u);
            mem.writeIORegister(0x10002000u, (5u << 28) | (1u << 27)); // SETIQ, non-intra, no skip
            t.IsTrue((mem.readIORegister(0x10002010u) & 0x80000000u) != 0u, "SETIQ waits for data (BUSY)");
            mem.writeIORegister(0x1000B410u, kSrc);
            mem.writeIORegister(0x1000B420u, 4u);
            mem.writeIORegister(0x1000B400u, 0x101u); // toIPU, normal, STR
            t.IsTrue((mem.readIORegister(0x10002010u) & 0x80000000u) == 0u, "SETIQ completes once 64 bytes arrived");
            t.IsTrue(mem.ipu().nonIntraQuantizer(0) == 1u && mem.ipu().nonIntraQuantizer(63) == 64u && mem.ipu().intraQuantizer(0) == 0u,
                     "bit 27 selects the non-intra matrix, filled in stream order");
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 4)) != 0u, "CIS4 is raised");
            t.Equals(mem.readIORegister(0x1000B410u), kSrc + 64u, "toIPU MADR ends past the data");
        });

        tc.Run("IPU toIPU stays active while the input FIFO is full and completes when a command frees it", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            mem.writeIORegister(0x1000B410u, 0x00430000u);
            mem.writeIORegister(0x1000B420u, 10u);
            mem.writeIORegister(0x1000B400u, 0x101u);
            t.Equals(mem.readIORegister(0x10002010u) & 0xFu, 8u, "the FIFO holds 8 qwords");
            t.IsTrue((mem.readIORegister(0x1000B400u) & 0x100u) != 0u, "toIPU waits for FIFO space (STR set)");
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 4)) == 0u, "no CIS4 yet");
            mem.writeIORegister(0x10002000u, 0u); // BCLR frees the FIFO
            t.Equals(mem.readIORegister(0x10002010u) & 0xFu, 2u, "the last 2 qwords enter");
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 4)) != 0u, "CIS4 once all data is in");
            mem.writeIORegister(0x10002010u, 1u << 30); // CTRL.RST
            t.Equals(mem.readIORegister(0x10002010u) & 0xFFu, 0u, "RST empties the FIFOs");
            mem.writeIORegister(0x10002000u, 5u << 28);                   // SETIQ: busy until 64 bytes arrive
            mem.writeIORegister(0x10002000u, (9u << 28) | 0x00050003u);  // SETTH while busy
            t.IsTrue(mem.ipu().threshold0() == 0u && mem.ipu().threshold1() == 0u && mem.ipu().busy(),
                     "a command written while the IPU is busy is ignored");
            mem.writeIORegister(0x10002010u, 1u << 30);
            mem.writeIORegister(0x10002000u, (1u << 28)); // IDEC: not implemented
            t.Equals(mem.ipu().unimplementedCommand(), 1, "an unimplemented command is reported, not decoded");
        });

        // CSC: integer BT.601 (D-IPU-1); expected values computed by hand from the formula.
        tc.Run("IPU CSC converts a macroblock through toIPU and fromIPU DMA (RGB32, RGB16, dither, alpha thresholds)", [](TestCase &t)
        {
            struct Case
            {
                uint32_t option;   // CSC option bits (RGB16 bit 27, dither bit 26)
                uint32_t setth;    // SETTH option
                uint32_t qwc;      // output qwords
                uint32_t firstWord; // first 32 bits of the output
            };
            const Case cases[] = {
                {0u, 0u, 64u, 0x80801980u | 0xFFu},                        // RGBA (255, 25, 128, 0x80)
                {1u << 27, 0u, 32u, 0x407Fu | (0x407Fu << 16)},            // RGB16 31 | 3 << 5 | 16 << 10
                {(1u << 27) | (1u << 26), 0u, 32u, 0x3C5Fu},               // dithered pixel (0,0), d = -4
                {1u << 27, 256u << 16, 32u, 0xC07Fu | (0xC07Fu << 16)},    // TH1 = 256: alpha 0x40 sets bit 15
            };
            for (const Case &c : cases)
            {
                PS2Memory mem;
                t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
                uint8_t *rdram = mem.getRDRAM();
                constexpr uint32_t kIn = 0x00440000u, kOut = 0x00450000u;
                std::memset(rdram + kIn, 126, 256);       // Y
                std::memset(rdram + kIn + 256, 128, 64);  // Cb
                std::memset(rdram + kIn + 320, 255, 64);  // Cr
                std::memset(rdram + kOut, 0xEE, 1024);
                mem.writeIORegister(0x10002000u, (9u << 28) | c.setth);
                mem.writeIORegister(0x10002000u, (7u << 28) | c.option | 1u); // CSC, one macroblock
                mem.writeIORegister(0x1000B010u, kOut);
                mem.writeIORegister(0x1000B020u, c.qwc);
                mem.writeIORegister(0x1000B000u, 0x100u); // fromIPU, normal, STR
                mem.writeIORegister(0x1000B410u, kIn);
                mem.writeIORegister(0x1000B420u, 24u);
                mem.writeIORegister(0x1000B400u, 0x101u); // toIPU, normal, STR
                uint32_t first = 0u;
                std::memcpy(&first, rdram + kOut, 4u);
                const uint32_t mask = (c.option & (1u << 26)) != 0u ? 0xFFFFu : 0xFFFFFFFFu;
                t.Equals(first & mask, c.firstWord & mask, "first output word matches the integer BT.601 result");
                uint32_t last = 0u;
                std::memcpy(&last, rdram + kOut + c.qwc * 16u - 4u, 4u);
                t.IsTrue(last != 0xEEEEEEEEu, "the whole macroblock reached memory");
                t.IsTrue((mem.readIORegister(0x1000E010u) & ((1u << 3) | (1u << 4))) == ((1u << 3) | (1u << 4)), "CIS3 and CIS4 are raised");
                t.IsTrue(!mem.ipu().busy(), "CSC completes once its output has drained");
            }
        });

        tc.Run("stall control: changing D_CTRL.STD releases a waiting drain", [](TestCase &t)
        {
            MfifoRig rig(0x30u, 0u);
            t.IsTrue(rig.ok, "PS2Memory initialize should succeed");
            rig.mem.writeIORegister(0x1000E000u, 1u | (2u << 6));
            uint8_t *rdram = rig.mem.getRDRAM();
            writeDmaTag(rdram, kStallChain, makeDmaTag(1u, 4u, kStallData));
            writeDmaTag(rdram, kStallChain + 0x10u, makeDmaTag(0u, 7u, 0u));
            rig.startGifChain(kStallChain);
            t.IsTrue(rig.gifActive(), "the drain waits at D_STADR = 0");
            rig.mem.writeIORegister(0x1000E000u, 1u); // STD = 0
            t.Equals(rig.received.size(), static_cast<size_t>(16u), "the REFS data is read once stall control is off");
            t.IsTrue(!rig.gifActive(), "the chain completes");
        });

        tc.Run("sceDmaReset re-enables DMAC DMAE", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDctrl = 0x1000E000u;
            constexpr uint32_t kDpcr = 0x1000E020u;
            constexpr uint32_t kDsqwc = 0x1000E030u;
            constexpr uint32_t kDrbor = 0x1000E050u;
            constexpr uint32_t kDrbsr = 0x1000E040u;
            constexpr uint32_t kDstadr = 0x1000E060u;

            PS2Memory &mem = runtime.memory();
            t.IsTrue(mem.writeIORegister(kDctrl, 0u), "clearing D_CTRL should succeed");
            t.IsTrue(mem.writeIORegister(kDpcr, 0x12345678u), "writing D_PCR should succeed");
            t.IsTrue(mem.writeIORegister(kDsqwc, 0x11220044u), "writing D_SQWC should succeed");
            t.IsTrue(mem.writeIORegister(kDrbor, 0x2000u), "writing D_RBOR should succeed");
            t.IsTrue(mem.writeIORegister(kDrbsr, 0x3FFFu), "writing D_RBSR should succeed");
            t.IsTrue(mem.writeIORegister(kDstadr, 0x4567u), "writing D_STADR should succeed");

            R5900Context ctx{};
            ps2_stubs::sceDmaReset(mem.getRDRAM(), &ctx, &runtime);

            t.Equals(static_cast<int32_t>(::getRegU32(&ctx, 2)), 0, "sceDmaReset should return 0");
            t.Equals(mem.readIORegister(kDctrl), 1u, "sceDmaReset should leave D_CTRL DMAE enabled");
            t.Equals(mem.readIORegister(kDpcr), 0u, "sceDmaReset should clear D_PCR");
            t.Equals(mem.readIORegister(kDsqwc), 0u, "sceDmaReset should clear D_SQWC");
            t.Equals(mem.readIORegister(kDrbor), 0u, "sceDmaReset should clear D_RBOR");
            t.Equals(mem.readIORegister(kDrbsr), 0u, "sceDmaReset should clear D_RBSR");
            t.Equals(mem.readIORegister(kDstadr), 0u, "sceDmaReset should clear D_STADR");
        });

        tc.Run("sceDmaSend preserves guest-configured VIF1 TTE", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00028500u;

            PS2Memory &mem = runtime.memory();
            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag, makeDmaTag(0u, 7u, 0u, false)); // END
            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x66u);
            std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));

            // Fatal Frame follows this exact sequence: get channel, set CHCR.TTE,
            // then submit the chain through sceDmaSend.
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x40u),
                     "guest should be able to configure VIF1 CHCR.TTE before submission");

            R5900Context ctx{};
            // sceDmaSend dispatches DMAC completion handlers through the EE scheduler, which must run on its executor
            // thread (asserted in Debug builds); in the runtime that is the game thread, here the test thread.
            runtime.eeScheduler().reset(rdram, ctx);
            setRegU32(ctx, 4, 1u); // sceDmaGetChan(1) / VIF1
            setRegU32(ctx, 5, kTag);
            ps2_stubs::sceDmaSend(rdram, &ctx, &runtime);

            t.Equals(static_cast<int32_t>(::getRegU32(&ctx, 2)), 0,
                     "sceDmaSend should accept the VIF1 chain");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x40u) != 0u,
                     "sceDmaSend must preserve guest-configured CHCR.TTE");
            t.Equals(mem.vif1_regs.itops, 0x66u,
                     "preserved TTE should deliver the tag high-half VIFcode");
        });

        tc.Run("VIF1 DMA DIRECT image packet reaches GS through arbiter", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                gs.processGIFPacket(data, sizeBytes);
            });
            mem.setGifArbiter(&arbiter);

            const uint64_t bitblt =
                (static_cast<uint64_t>(0u) << 0) |
                (static_cast<uint64_t>(1u) << 16) |
                (static_cast<uint64_t>(0u) << 24) |
                (static_cast<uint64_t>(0u) << 32) |
                (static_cast<uint64_t>(1u) << 48) |
                (static_cast<uint64_t>(0u) << 56);
            gs.writeRegister(GS_REG_BITBLTBUF, bitblt);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (1ull << 32));
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSrc = 0x00027C00u;
            constexpr uint32_t kQwc = 3u;

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kSrc, 0, kQwc * 16u);

            const uint32_t directCmd = makeVifCmd(0x50u, 0u, 2u); // DIRECT 2 QW payload.
            std::memcpy(rdram + kSrc, &directCmd, sizeof(directCmd));

            uint8_t *gifPayload = rdram + kSrc + 4u;
            const uint64_t gifTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(gifPayload + 0u, &gifTag, sizeof(gifTag));
            const uint64_t tagHi = 0u;
            std::memcpy(gifPayload + 8u, &tagHi, sizeof(tagHi));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                gifPayload[16u + i] = static_cast<uint8_t>(0x70u + i);
            }

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kSrc), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, kQwc), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR should succeed");

            mem.processPendingTransfers();

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0x70u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "VIF1 DIRECT image should update GS VRAM through GIF path2");
        });

        // Ported from LightVelox/PS2Recomp 56af7ef3 (also fixed in the DQ8, Killzone and SRWZ engines).
        tc.Run("VIF1 DIRECT image tag continues in the next DIRECT payload", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                gs.processGIFPacket(data, sizeBytes);
            });
            mem.setGifArbiter(&arbiter);

            const uint64_t bitblt =
                (static_cast<uint64_t>(0u) << 0) |
                (static_cast<uint64_t>(1u) << 16) |
                (static_cast<uint64_t>(0u) << 24) |
                (static_cast<uint64_t>(0u) << 32) |
                (static_cast<uint64_t>(1u) << 48) |
                (static_cast<uint64_t>(0u) << 56);
            gs.writeRegister(GS_REG_BITBLTBUF, bitblt);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (1ull << 32));
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            std::vector<uint8_t> packet;
            for (uint32_t i = 0; i < 3u; ++i)
                appendU32(packet, makeVifCmd(0x00u, 0u, 0u));
            appendU32(packet, makeVifCmd(0x50u, 0u, 1u)); // DIRECT 1 QW payload: GIF IMAGE tag only.
            appendU64(packet, makeGifTag(1u, GIF_FMT_IMAGE, 0u, true));
            appendU64(packet, 0ull);
            for (uint32_t i = 0; i < 3u; ++i)
                appendU32(packet, makeVifCmd(0x00u, 0u, 0u)); // VIFcodes between the DIRECTs are not pixels.
            appendU32(packet, makeVifCmd(0x50u, 0u, 1u));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                packet.push_back(static_cast<uint8_t>(0xA0u + i));
            }

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0xA0u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "the next DIRECT payload should continue the PATH2 image upload");
        });

        tc.Run("VIF1 DIRECT finds an image continuation after packed setup", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                gs.processGIFPacket(data, sizeBytes);
            });
            mem.setGifArbiter(&arbiter);

            const uint64_t bitblt =
                (static_cast<uint64_t>(1u) << 16) |
                (static_cast<uint64_t>(1u) << 48);
            gs.writeRegister(GS_REG_BITBLTBUF, bitblt);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (1ull << 32));
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            std::vector<uint8_t> packet;
            for (uint32_t i = 0; i < 3u; ++i)
                appendU32(packet, makeVifCmd(0x00u, 0u, 0u));
            appendU32(packet, makeVifCmd(0x50u, 0u, 3u)); // PACKED tag + A+D + IMAGE tag.
            appendU64(packet, makeGifTag(1u, GIF_FMT_PACKED, 1u, false));
            appendU64(packet, 0x0Eull);
            appendU64(packet, 0x8000008000ull); // TEXA, harmless setup preceding the IMAGE tag.
            appendU64(packet, GS_REG_TEXA);
            appendU64(packet, makeGifTag(1u, GIF_FMT_IMAGE, 0u, true));
            appendU64(packet, 0ull);
            for (uint32_t i = 0; i < 3u; ++i)
                appendU32(packet, makeVifCmd(0x00u, 0u, 0u));
            appendU32(packet, makeVifCmd(0x50u, 0u, 1u));
            for (uint32_t i = 0; i < 16u; ++i)
                packet.push_back(static_cast<uint8_t>(0xC0u + i));

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0xC0u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "image continuation after packed setup should come from the next DIRECT payload");
        });

        tc.Run("VIF1 DIRECT cut at the end of the data continues in the next transfer", [](TestCase &t)
        {
            // PCSX2 keeps an unfinished DIRECT active across DMA transfers: the next data is its payload, not VIFcodes.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                gs.processGIFPacket(data, sizeBytes);
            });
            mem.setGifArbiter(&arbiter);

            gs.writeRegister(GS_REG_BITBLTBUF, (static_cast<uint64_t>(1u) << 16) | (static_cast<uint64_t>(1u) << 48));
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (1ull << 32));
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            std::vector<uint8_t> first;
            for (uint32_t i = 0; i < 3u; ++i)
                appendU32(first, makeVifCmd(0x00u, 0u, 0u));
            appendU32(first, makeVifCmd(0x50u, 0u, 2u)); // DIRECT 2 QW, but only the IMAGE tag is in this transfer.
            appendU64(first, makeGifTag(1u, GIF_FMT_IMAGE, 0u, true));
            appendU64(first, 0ull);
            mem.processVIF1Data(first.data(), static_cast<uint32_t>(first.size()));

            std::vector<uint8_t> second;
            for (uint32_t i = 0; i < 16u; ++i)
                second.push_back(static_cast<uint8_t>(0xE0u + i));
            mem.processVIF1Data(second.data(), static_cast<uint32_t>(second.size()));

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0xE0u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "the rest of a cut DIRECT payload should arrive as image data in the next transfer");
        });

        tc.Run("unaligned accesses throw", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            bool threwRead32 = false;
            bool threwWrite64 = false;
            try
            {
                (void)mem.read32(0x00000002u);
            }
            catch (const std::exception &)
            {
                threwRead32 = true;
            }

            try
            {
                mem.write64(0x00000004u + 2u, 0x1122334455667788ull);
            }
            catch (const std::exception &)
            {
                threwWrite64 = true;
            }

            t.IsTrue(threwRead32, "unaligned read32 should throw");
            t.IsTrue(threwWrite64, "unaligned write64 should throw");
        });
    });
}
