// SIF0 vertical-slice tests, EE / both-ends side (Phase 4).
//
// TESTS ONLY. This file drives the already-implemented engines
// (PS2Memory channel 5, IopMemory channel 9) through the shared
// Sif0Fifo transport; it does not implement any DMA behaviour.
//
// Labelled model choices asserted here (design
// sif0-architecture-design.md, NOT hardware claims):
//   * the FIFO holds 8 slots, and a header occupies one slot;
//   * the DmaStat encodings 1 = complete / 0 = in-flight /
//     -1 = unknown;
//   * the CORRECTED partial-QW rule: the IOP engine rounds a tag's
//     word count up to a QW boundary and reads padding words
//     from IOP RAM, so the EE side never fabricates a partial
//     QW (the design's original zero-fill rule is NOT tested).
// No test here claims hardware confirmation.

#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/SIF.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "emulator/core/iop_memory.h"
#include "ps2x/iop/sif0_fifo.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
    using IopMemory = ps2x::iop::detail::IopMemory;
    using Sif0Fifo = ps2x::iop::Sif0Fifo;

    // EE DMAC channel 5.
    constexpr uint32_t kEeChcr = 0x1000C000u;
    constexpr uint32_t kEeDStat = 0x1000E010u;
    constexpr uint32_t kEeChcrChainStr = 0x104u;     // MOD chain (bits 2-3 = 1) + STR (bit 8).
    constexpr uint32_t kEeChcrChainStrTie = 0x184u;  // + TIE (bit 7).
    // EE DMAC channel 7 (SIF2) -- must stay off the SIF0 FIFO.
    constexpr uint32_t kEeCh7Chcr = 0x1000C800u;

    // IOP DMA channel 9.
    constexpr uint32_t kIopDmacEn = 0x1F801578u;
    constexpr uint32_t kIopDicr = 0x1F8010F4u;
    constexpr uint32_t kIopDicr2 = 0x1F801574u;
    constexpr uint32_t kIopBcr = 0x1F801524u;
    constexpr uint32_t kIopTadr = 0x1F80152Cu;
    constexpr uint32_t kIopChcr = 0x1F801528u;
    constexpr uint32_t kIopChcrChain = 0x01000701u;  // dir bit 0 | header bit 8 | mode 3<<9 | STR bit 24.
    constexpr uint32_t kIopDicrMaster = 1u << 23u;
    constexpr uint32_t kIopDicr2Sif0Mask = 1u << 18u;
    constexpr uint32_t kIopDicr2Sif0Flag = 1u << 26u;

    // EE DMAtag low word: QWC bits 0-15, Tag ID bits 28-30, IRQ bit 31.
    // EE DMAtag high word: ADDR bits 0-30, scratchpad bit 31.
    uint32_t eeTagLo(uint32_t qwc, uint32_t tagId, bool irq)
    {
        return (qwc & 0xFFFFu) | ((tagId & 0x7u) << 28u) | (irq ? (1u << 31u) : 0u);
    }

    uint32_t eeTagHi(uint32_t dest, bool scratch)
    {
        return (dest & 0x7FFFFFF0u) | (scratch ? (1u << 31u) : 0u);
    }

    std::vector<uint8_t> patternedBytes(size_t count, uint8_t base)
    {
        std::vector<uint8_t> bytes(count);
        for (size_t i = 0; i < count; ++i)
            bytes[i] = static_cast<uint8_t>(base + i);
        return bytes;
    }

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    int32_t getRegS32(const R5900Context &ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(&ctx, reg));
    }

    // One IOP chain tag (4 words when CHCR.8 is set): word 0 is the
    // IOP source address (bits 0-23) with IRQ bit 30 and EOT bit 31,
    // word 1 is the size in words, words 2-3 are the EE DMAtag.
    struct BothEnds
    {
        Sif0Fifo fifo;
        PS2Memory ee;
        IopMemory iop;
        bool initOk = false;

        BothEnds()
        {
            initOk = ee.initialize();
            ee.setSif0Transport(&fifo);
            ee.setTransportOwnsSif0(true);
            iop.setSif0Transport(&fifo);
            iop.setTransportOwnsSif0(true);
        }

        bool putIopBytes(uint32_t addr, const void *data, size_t size)
        {
            return iop.writeRam(addr, data, size);
        }

        bool putIopWords(uint32_t addr, const std::vector<uint32_t> &words)
        {
            return iop.writeRam(addr, words.data(), words.size() * sizeof(uint32_t));
        }

        bool putTag(uint32_t tagAddr, uint32_t src, uint32_t sizeWords, uint32_t tagLo, uint32_t tagHi,
                    bool irq, bool eot)
        {
            const std::vector<uint32_t> words{
                (src & 0x00FFFFFFu) | (irq ? (1u << 30u) : 0u) | (eot ? (1u << 31u) : 0u),
                sizeWords,
                tagLo,
                tagHi,
            };
            return putIopWords(tagAddr, words);
        }

        // Programs the IOP registers for a chain and starts it (the
        // CHCR write starts, but does not drive, the IOP engine).
        // DMACEN and the DICR/DICR2 enables must be set before the
        // CHCR write, or the start preconditions fail silently.
        void startIop(uint32_t tagAddr)
        {
            iop.write32(kIopDmacEn, 1u);
            iop.write32(kIopDicr, kIopDicrMaster);
            iop.write32(kIopDicr2, kIopDicr2Sif0Mask);
            iop.write32(kIopTadr, tagAddr);
            iop.write32(kIopChcr, kIopChcrChain);
        }

        void startEe(uint32_t chcr = kEeChcrChainStr)
        {
            ee.writeIORegister(kEeChcr, chcr);
        }

        [[nodiscard]] bool eeCis5()
        {
            return (ee.readIORegister(kEeDStat) & (1u << 5u)) != 0u;
        }

        [[nodiscard]] bool eeStrSet()
        {
            return (ee.readIORegister(kEeChcr) & 0x100u) != 0u;
        }

        [[nodiscard]] bool iopStrSet() const
        {
            return (iop.read32(kIopChcr) & (1u << 24u)) != 0u;
        }

        [[nodiscard]] bool iopDicr2Flag() const
        {
            return (iop.read32(kIopDicr2) & kIopDicr2Sif0Flag) != 0u;
        }

        bool eeBytesEqual(uint32_t dest, const std::vector<uint8_t> &expected)
        {
            const uint8_t *rdram = ee.getRDRAM();
            if (rdram == nullptr || dest + expected.size() > PS2_RAM_SIZE)
                return false;
            return std::memcmp(rdram + dest, expected.data(), expected.size()) == 0;
        }

        bool eeBytesEqual(uint32_t dest, const uint8_t *expected, size_t size)
        {
            const uint8_t *rdram = ee.getRDRAM();
            if (rdram == nullptr || dest + size > PS2_RAM_SIZE)
                return false;
            return std::memcmp(rdram + dest, expected, size) == 0;
        }
    };

    // ---- Scheduler (delivery-count) fixtures, case 6 only. ----

    constexpr uint32_t kSchedEntryPc = 0x00102000u;
    constexpr uint32_t kSchedResumePc = 0x00102010u;
    constexpr uint32_t kSchedHandlerPc = 0x00102020u;
    constexpr uint32_t kSchedDescAddr = 0x00021300u;
    constexpr uint32_t kSchedHandlerArg = 0x12345678u;
    constexpr uint32_t kSchedEeDest = 0x00050000u;
    constexpr uint32_t kSchedIopTag = 0x00011000u;
    constexpr uint32_t kSchedIopSrc = 0x00021000u;

    int g_sif0SchedHandlerCount = 0;
    bool g_sif0SchedMaskCause = false;
    Sif0Fifo *g_sif0SchedFifo = nullptr;
    IopMemory *g_sif0SchedIop = nullptr;

    void sif0SchedHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_sif0SchedHandlerCount;
        ctx->pc = 0u;
    }

    void sif0SchedResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = 0u;
        runtime->requestStop();
    }

    // Drives one DMAC SIF0 transfer to completion, then drains the
    // queued DMAC causes (which queues the cause-5 handler, if the
    // cause is not masked). Follows the structure of the
    // "sceSifSetDma dispatches enabled DMAC handlers for cause 5"
    // test in ps2_sif_dma_tests.cpp.
    void sif0SchedDmacEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->eeScheduler().addIrqHandler(true, 5u, kSchedHandlerPc, true, kSchedHandlerArg, 0u, 0u);
        PS2Memory &mem = runtime->memory();
        mem.setSif0Transport(g_sif0SchedFifo);
        mem.setTransportOwnsSif0(true);
        IopMemory &iop = *g_sif0SchedIop;
        iop.setSif0Transport(g_sif0SchedFifo);
        iop.setTransportOwnsSif0(true);
        iop.write32(kIopDmacEn, 1u);
        iop.write32(kIopDicr, kIopDicrMaster);
        iop.write32(kIopDicr2, kIopDicr2Sif0Mask);
        iop.write32(kIopTadr, kSchedIopTag);
        iop.write32(kIopChcr, kIopChcrChain);
        iop.driveSif0();
        mem.writeIORegister(kEeChcr, kEeChcrChainStr);
        mem.driveSif0();
        if (g_sif0SchedMaskCause)
            runtime->eeScheduler().setIrqCauseEnabled(true, 5u, false);
        runtime->drainCompletedDmacHandlers(rdram);
        ctx->pc = kSchedResumePc;
    }

    void sif0SchedHleEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->eeScheduler().addIrqHandler(true, 5u, kSchedHandlerPc, true, kSchedHandlerArg, 0u, 0u);
        setRegU32(*ctx, 4, kSchedDescAddr);
        setRegU32(*ctx, 5, 1u);
        ctx->pc = kSchedResumePc;
        ps2_stubs::sceSifSetDma(rdram, ctx, runtime);
    }
}

void register_ps2_sif0_tests()
{
    MiniTest::Case("PS2Sif0", [](TestCase &tc)
    {
        tc.Run("1 both-ends basic transfer is byte-exact and invalidates code regions", [](TestCase &t)
        {
            BothEnds b;
            if (!b.initOk)
            {
                t.Fail("PS2Memory initialize should succeed before a SIF0 transfer");
                return;
            }
            constexpr uint32_t kTag = 0x00010000u;
            constexpr uint32_t kSrc = 0x00020000u;
            constexpr uint32_t kDest = 0x00040000u;
            const std::vector<uint8_t> payload = patternedBytes(32u, 0xA0u);
            t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "test setup should write the IOP payload");
            t.IsTrue(b.putTag(kTag, kSrc, 8u, eeTagLo(2u, 0u, false), eeTagHi(kDest, false), false, true),
                     "test setup should write the IOP chain tag (EOT, cnt header)");
            b.ee.registerCodeRegion(kDest, kDest + 32u);
            t.IsFalse(b.ee.isCodeModified(kDest, 32u), "code region should be unmodified before the transfer");
            b.startEe();
            t.IsTrue(b.ee.sif0WaitingOnEmpty(), "EE engine should wait on the empty FIFO after STR");
            b.startIop(kTag);
            b.iop.driveSif0();
            t.IsFalse(b.iop.sif0Active(), "IOP engine should have finished pushing an EOT tag");
            t.Equals(b.fifo.count(), static_cast<size_t>(3),
                     "FIFO should hold the header plus 2 payload QWs after the IOP drive");
            b.ee.driveSif0();
            // A cnt header does not end a chain: after delivering its
            // 2 QWs the EE engine waits for the next header, so it
            // stays active and sets no CIS5. Payload delivery (not
            // completion) is the basic-transfer check here;
            // completion is asserted in case 2 with an end tag.
            t.IsTrue(b.ee.sif0Active(), "EE engine should stay active after a cnt tag, waiting for the next header");
            t.IsTrue(b.ee.sif0WaitingOnEmpty(), "EE engine should wait on empty after a cnt tag with no following header");
            t.IsFalse(b.eeCis5(), "a cnt tag alone should not complete via CIS5");
            t.IsTrue(b.fifo.isEmpty(), "FIFO should be empty after the cnt tag's QWs are delivered");
            t.IsTrue(b.eeBytesEqual(kDest, payload), "EE RAM at ADDR should hold the IOP payload byte-exact");
            t.IsTrue(b.ee.isCodeModified(kDest, 32u),
                     "SIF0 payload writes must go through the internal DMA path (memcpy + markModified), "
                     "so a registered code region over ADDR must read as modified");
            t.Equals(b.ee.readIORegister(kEeChcr + 0x10u), kDest + 32u,
                     "EE MADR should advance past the 2 delivered QWs");
            t.Equals(b.ee.readIORegister(kEeChcr + 0x20u), 0u, "EE QWC should reach 0 after 2 delivered QWs");
        });

        tc.Run("2 end tag and TIE/IRQ end complete via CIS5 and queued cause 5", [](TestCase &t)
        {
            // (a) end tag.
            {
                BothEnds b;
                if (!b.initOk)
                {
                    t.Fail("PS2Memory initialize should succeed before the end-tag subcase");
                    return;
                }
                constexpr uint32_t kTag = 0x00010000u;
                constexpr uint32_t kSrc = 0x00020000u;
                constexpr uint32_t kDest = 0x00040000u;
                const std::vector<uint8_t> payload = patternedBytes(32u, 0x20u);
                t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "end-tag setup should write the IOP payload");
                t.IsTrue(b.putTag(kTag, kSrc, 8u, eeTagLo(2u, 7u, false), eeTagHi(kDest, false), false, true),
                         "end-tag setup should write the IOP chain tag (EOT, end header)");
                b.startEe();
                b.startIop(kTag);
                b.iop.driveSif0();
                b.ee.driveSif0();
                t.IsTrue(b.eeBytesEqual(kDest, payload), "end tag payload should land byte-exact in EE RAM");
                t.IsTrue(b.eeCis5(), "an end tag should complete via D_STAT CIS5");
                t.IsFalse(b.eeStrSet(), "EE STR should clear after end-tag completion");
                const std::vector<uint32_t> causes = b.ee.consumeCompletedDmacCauses();
                t.IsTrue(causes.size() == 1u && causes[0] == 5u,
                         "end-tag completion should queue exactly one cause 5");
            }
            // (b) non-end IRQ tag with TIE: EE ends after that tag and
            // must NOT consume the following end tag's units.
            {
                BothEnds b;
                if (!b.initOk)
                {
                    t.Fail("PS2Memory initialize should succeed before the TIE subcase");
                    return;
                }
                constexpr uint32_t kTag1 = 0x00010000u;
                constexpr uint32_t kTag2 = 0x00010010u;
                constexpr uint32_t kSrc1 = 0x00020000u;
                constexpr uint32_t kSrc2 = 0x00020100u;
                constexpr uint32_t kDest1 = 0x00040000u;
                constexpr uint32_t kDest2 = 0x00041000u;
                const std::vector<uint8_t> payload1 = patternedBytes(32u, 0x30u);
                const std::vector<uint8_t> payload2 = patternedBytes(32u, 0x50u);
                t.IsTrue(b.putIopBytes(kSrc1, payload1.data(), payload1.size()), "TIE setup should write payload 1");
                t.IsTrue(b.putIopBytes(kSrc2, payload2.data(), payload2.size()), "TIE setup should write payload 2");
                t.IsTrue(b.putTag(kTag1, kSrc1, 8u, eeTagLo(2u, 0u, true), eeTagHi(kDest1, false), true, false),
                         "TIE setup should write tag 1 (IRQ, not EOT, cnt header with IRQ)");
                t.IsTrue(b.putTag(kTag2, kSrc2, 8u, eeTagLo(2u, 7u, false), eeTagHi(kDest2, false), false, true),
                         "TIE setup should write tag 2 (EOT, end header)");
                std::memset(b.ee.getRDRAM() + kDest2, 0x5A, 32u);
                b.startEe(kEeChcrChainStrTie);
                b.startIop(kTag1);
                b.iop.driveSif0();
                b.ee.driveSif0();
                t.IsTrue(b.eeBytesEqual(kDest1, payload1), "TIE/IRQ tag payload should land byte-exact");
                t.IsTrue(b.eeCis5(), "an IRQ tag with TIE should complete via CIS5");
                const std::vector<uint32_t> causes = b.ee.consumeCompletedDmacCauses();
                t.IsTrue(causes.size() == 1u && causes[0] == 5u,
                         "TIE/IRQ completion should queue exactly one cause 5");
                t.Equals(b.fifo.count(), static_cast<size_t>(3),
                         "EE engine should stop after the TIE/IRQ tag, leaving the next header + 2 QWs in the FIFO");
                std::vector<uint8_t> sentinel(32u, 0x5Au);
                t.IsTrue(b.eeBytesEqual(kDest2, sentinel),
                         "the following end tag's destination must stay untouched when TIE ends the transfer early");
            }
            // (c) non-end IRQ tag WITHOUT TIE: EE must continue past
            // it and complete only on the following end tag. The
            // continuation is proven by the final state: had the EE
            // engine (wrongly) ended at tag 1, tag 2's bytes would
            // still be sentinel and its units would still be queued.
            {
                BothEnds b;
                if (!b.initOk)
                {
                    t.Fail("PS2Memory initialize should succeed before the no-TIE subcase");
                    return;
                }
                constexpr uint32_t kTag1 = 0x00010000u;
                constexpr uint32_t kTag2 = 0x00010010u;
                constexpr uint32_t kSrc1 = 0x00020000u;
                constexpr uint32_t kSrc2 = 0x00020100u;
                constexpr uint32_t kDest1 = 0x00040000u;
                constexpr uint32_t kDest2 = 0x00041000u;
                const std::vector<uint8_t> payload1 = patternedBytes(32u, 0x30u);
                const std::vector<uint8_t> payload2 = patternedBytes(32u, 0x50u);
                t.IsTrue(b.putIopBytes(kSrc1, payload1.data(), payload1.size()), "no-TIE setup should write payload 1");
                t.IsTrue(b.putIopBytes(kSrc2, payload2.data(), payload2.size()), "no-TIE setup should write payload 2");
                t.IsTrue(b.putTag(kTag1, kSrc1, 8u, eeTagLo(2u, 0u, true), eeTagHi(kDest1, false), true, false),
                         "no-TIE setup should write tag 1 (IRQ, not EOT, cnt header with IRQ)");
                t.IsTrue(b.putTag(kTag2, kSrc2, 8u, eeTagLo(2u, 7u, false), eeTagHi(kDest2, false), false, true),
                         "no-TIE setup should write tag 2 (EOT, end header)");
                std::memset(b.ee.getRDRAM() + kDest2, 0x5A, 32u);
                b.startEe();  // No TIE.
                b.startIop(kTag1);
                b.iop.driveSif0();
                b.ee.driveSif0();
                t.IsTrue(b.eeBytesEqual(kDest1, payload1), "no-TIE tag 1 payload should land byte-exact");
                t.IsTrue(b.eeBytesEqual(kDest2, payload2),
                         "without TIE the EE engine must continue past the IRQ tag and deliver tag 2's payload");
                t.IsTrue(b.eeCis5(), "completion after the end tag should set CIS5");
                t.IsTrue(b.fifo.isEmpty(), "FIFO should be empty after both tags are delivered");
                const std::vector<uint32_t> causes = b.ee.consumeCompletedDmacCauses();
                t.IsTrue(causes.size() == 1u && causes[0] == 5u,
                         "the no-TIE chain should queue exactly one cause 5, at the end tag");
            }
        });

        tc.Run("3 FIFO stall and resume: IOP full, then EE drain, then completion", [](TestCase &t)
        {
            BothEnds b;
            if (!b.initOk)
            {
                t.Fail("PS2Memory initialize should succeed before the stall test");
                return;
            }
            constexpr uint32_t kTag = 0x00010000u;
            constexpr uint32_t kSrc = 0x00020000u;
            constexpr uint32_t kDest = 0x00040000u;
            const std::vector<uint8_t> payload = patternedBytes(160u, 0x00u);
            t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "stall setup should write the 40-word IOP payload");
            t.IsTrue(b.putTag(kTag, kSrc, 40u, eeTagLo(10u, 7u, false), eeTagHi(kDest, false), false, true),
                     "stall setup should write the IOP chain tag (40 words, EOT, end header QWC 10)");
            std::memset(b.ee.getRDRAM() + kDest, 0x5A, payload.size());
            b.startEe();
            const uint32_t eeId = b.ee.sif0CurrentTransferId();
            t.IsTrue(b.ee.sif0WaitingOnEmpty(), "EE engine should wait on the empty FIFO after STR");
            b.startIop(kTag);
            const uint32_t iopId = b.iop.sif0CurrentTransferId();
            b.iop.driveSif0();
            // Stall point: 8 slots = header + 7 payload QWs; the IOP
            // engine pauses on full (model choice: 8-slot FIFO).
            t.IsTrue(b.iop.sif0PausedOnFull(), "IOP engine should pause on a full FIFO");
            t.Equals(b.fifo.count(), static_cast<size_t>(8), "FIFO should be full (8 slots) at the stall point");
            t.IsTrue(b.iopStrSet(), "IOP CHCR bit 24 should stay set while paused on full");
            t.IsTrue(b.iop.sif0Active(), "IOP engine should still be active while paused on full");
            t.Equals(b.ee.sif0DmaStat(eeId), 0, "EE DmaStat should be 0 (in-flight) at the stall point");
            t.Equals(b.iop.sif0DmaStat(iopId), 0, "IOP DmaStat should be 0 (in-flight) at the stall point");
            t.IsFalse(b.eeCis5(), "CIS5 should stay clear at the stall point");
            t.IsFalse(b.iopDicr2Flag(), "DICR2 SIF0 flag should stay clear at the stall point");
            b.ee.driveSif0();
            // Corrected arithmetic (see report): the EE drive drains
            // the header plus 7 QWs, so QWC 10 falls to 3, not 2;
            // the IOP drive below then pushes the remaining 3 QWs.
            t.IsTrue(b.fifo.isEmpty(), "EE drive should drain the FIFO completely at the stall point");
            t.IsTrue(b.ee.sif0WaitingOnEmpty(), "EE engine should wait on empty again mid-tag after draining 7 QWs");
            t.Equals(b.ee.readIORegister(kEeChcr + 0x20u), 3u,
                     "EE QWC remaining should be 3 after draining 7 of 10 QWs");
            t.IsTrue(b.eeBytesEqual(kDest, payload.data(), 112u),
                     "the first 7 drained QWs should already be byte-exact in EE RAM");
            t.IsTrue(b.eeBytesEqual(kDest + 112u, std::vector<uint8_t>(48u, 0x5Au)),
                     "the not-yet-delivered tail must stay sentinel (no loss, no duplication, no fabrication)");
            b.iop.driveSif0();
            t.IsFalse(b.iop.sif0Active(), "IOP engine should finish once the remaining 3 QWs are pushed");
            t.IsFalse(b.iopStrSet(), "IOP CHCR bit 24 should clear on EOT completion");
            t.Equals(b.iop.sif0DmaStat(iopId), 1, "IOP DmaStat should be 1 after IOP completion");
            t.IsTrue(b.iopDicr2Flag(), "DICR2 SIF0 flag (bit 26) should set on IOP completion");
            t.Equals(b.fifo.count(), static_cast<size_t>(3), "FIFO should hold the remaining 3 payload QWs");
            b.ee.driveSif0();
            t.IsTrue(b.eeBytesEqual(kDest, payload), "the full 160-byte payload should be byte-exact after resume");
            t.IsTrue(b.eeCis5(), "EE completion after resume should set CIS5");
            t.Equals(b.ee.sif0DmaStat(eeId), 1, "EE DmaStat should be 1 after EE completion");
            t.IsFalse(b.eeStrSet(), "EE STR should clear after completion");
            t.IsTrue(b.fifo.isEmpty(), "FIFO should be empty after the completed transfer");
        });

        tc.Run("4 EE empty stall completes with no second EE STR write", [](TestCase &t)
        {
            BothEnds b;
            if (!b.initOk)
            {
                t.Fail("PS2Memory initialize should succeed before the EE empty-stall test");
                return;
            }
            constexpr uint32_t kTag = 0x00010000u;
            constexpr uint32_t kSrc = 0x00020000u;
            constexpr uint32_t kDest = 0x00040000u;
            const std::vector<uint8_t> payload = patternedBytes(32u, 0x60u);
            b.startEe();
            for (int i = 0; i < 3; ++i)
            {
                t.IsTrue(b.eeStrSet(), "repeated EE CHCR reads should keep STR set while waiting on empty");
            }
            t.IsTrue(b.ee.sif0WaitingOnEmpty(), "EE engine should be waiting on empty");
            t.IsFalse(b.eeCis5(), "no CIS5 should be set while the FIFO is empty");
            t.Equals(b.ee.sif0DmaStat(b.ee.sif0CurrentTransferId()), 0,
                     "EE DmaStat should be 0 (in-flight) while waiting on empty");
            t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "empty-stall setup should write the IOP payload");
            t.IsTrue(b.putTag(kTag, kSrc, 8u, eeTagLo(2u, 7u, false), eeTagHi(kDest, false), false, true),
                     "empty-stall setup should write the IOP chain tag (EOT, end header)");
            b.startIop(kTag);
            b.iop.driveSif0();
            // Deliberately NO second EE CHCR write here: the EE
            // engine was already started and waiting, and a push
            // notification (simulated by this drive call) must
            // resume it on its own.
            b.ee.driveSif0();
            t.IsTrue(b.eeBytesEqual(kDest, payload), "EE engine should drain and deliver after the IOP fill, with no second STR");
            t.IsTrue(b.eeCis5(), "EE completion after the IOP fill should set CIS5");
            t.IsFalse(b.eeStrSet(), "EE STR should clear after completion");
        });

        tc.Run("5 header-blocked start waits for FIFO space and then flows in order", [](TestCase &t)
        {
            BothEnds b;
            if (!b.initOk)
            {
                t.Fail("PS2Memory initialize should succeed before the header-blocked test");
                return;
            }
            constexpr uint32_t kTagA = 0x00010000u;
            constexpr uint32_t kTagB = 0x00010100u;
            constexpr uint32_t kSrcA = 0x00020000u;
            constexpr uint32_t kSrcB = 0x00021000u;
            constexpr uint32_t kDestA = 0x00040000u;
            constexpr uint32_t kDestB = 0x00041000u;
            const std::vector<uint8_t> payloadA = patternedBytes(112u, 0x10u);
            const std::vector<uint8_t> payloadB = patternedBytes(32u, 0x90u);
            t.IsTrue(b.putIopBytes(kSrcA, payloadA.data(), payloadA.size()), "header-block setup should write payload A");
            t.IsTrue(b.putIopBytes(kSrcB, payloadB.data(), payloadB.size()), "header-block setup should write payload B");
            t.IsTrue(b.putTag(kTagA, kSrcA, 28u, eeTagLo(7u, 7u, false), eeTagHi(kDestA, false), false, true),
                     "header-block setup should write chain A (7 QWs + header = 8 slots, EOT)");
            t.IsTrue(b.putTag(kTagB, kSrcB, 8u, eeTagLo(2u, 7u, false), eeTagHi(kDestB, false), false, true),
                     "header-block setup should write chain B (EOT, end header)");
            std::memset(b.ee.getRDRAM() + kDestB, 0x5A, 32u);
            b.startEe();
            b.startIop(kTagA);
            b.iop.driveSif0();
            t.Equals(b.fifo.count(), static_cast<size_t>(8), "chain A should fill the FIFO (header + 7 QWs)");
            // Start chain B while the FIFO is still full: its header
            // push must block (header occupies a slot, model choice).
            b.iop.write32(kIopTadr, kTagB);
            b.iop.write32(kIopChcr, kIopChcrChain);
            b.iop.driveSif0();
            t.Equals(b.fifo.count(), static_cast<size_t>(8), "chain B must not push anything into a full FIFO");
            t.IsTrue(b.iop.sif0PausedOnFull(), "chain B should pause on full at its header push");
            t.Equals(b.iop.read32(kIopTadr), kTagB, "chain B TADR should be unchanged while its header is blocked");
            t.IsTrue(b.iopStrSet(), "chain B CHCR bit 24 should stay set while its header is blocked");
            t.IsTrue(b.iop.sif0Active(), "chain B should still be active while blocked");
            b.ee.driveSif0();
            t.IsTrue(b.eeBytesEqual(kDestA, payloadA), "chain A should drain to completion byte-exact");
            t.IsTrue(b.fifo.isEmpty(), "FIFO should be empty after chain A drains");
            const std::vector<uint32_t> causesA = b.ee.consumeCompletedDmacCauses();
            t.IsTrue(causesA.size() == 1u && causesA[0] == 5u, "chain A completion should queue exactly one cause 5");
            b.iop.driveSif0();
            t.Equals(b.fifo.count(), static_cast<size_t>(3),
                     "chain B should flow only after space frees (header + 2 QWs)");
            t.IsFalse(b.iop.sif0Active(), "chain B IOP engine should finish after pushing its units");
            b.startEe();  // New EE STR for chain B.
            t.IsTrue(b.eeBytesEqual(kDestB, payloadB), "chain B payload should arrive byte-exact and in order after chain A");
            t.IsTrue(b.fifo.isEmpty(), "FIFO should be empty after chain B completes");
            const std::vector<uint32_t> causesB = b.ee.consumeCompletedDmacCauses();
            t.IsTrue(causesB.size() == 1u && causesB[0] == 5u, "chain B completion should queue exactly one cause 5");
        });

        tc.Run("6 EE delivery count is exactly one, and HLE under transport ownership queues none", [](TestCase &t)
        {
            // (a) transportOwnsSif0 == true, DMAC completion: exactly 1.
            // (c) is run here too (masked) because it shares the
            // scheduler fixture; (b) follows below.
            auto runDmacScheduler = [&](bool maskCause, int expectedCount)
            {
                g_sif0SchedHandlerCount = 0;
                g_sif0SchedMaskCause = maskCause;
                ps2_stubs::resetSifState();
                PS2Runtime runtime;
                PS2Memory &mem = runtime.memory();
                if (!mem.initialize())
                {
                    t.Fail("PS2Memory initialize should succeed before a scheduler delivery-count run");
                    return;
                }
                Sif0Fifo fifo;
                IopMemory iop;
                g_sif0SchedFifo = &fifo;
                g_sif0SchedIop = &iop;
                mem.setSif0Transport(&fifo);
                mem.setTransportOwnsSif0(true);
                iop.setSif0Transport(&fifo);
                iop.setTransportOwnsSif0(true);
                const std::vector<uint8_t> payload = patternedBytes(32u, 0x70u);
                t.IsTrue(iop.writeRam(kSchedIopSrc, payload.data(), payload.size()),
                         "scheduler setup should write the IOP payload");
                const std::array<uint32_t, 4> schedTag{
                    kSchedIopSrc | (1u << 31u), 8u,
                    eeTagLo(2u, 7u, false), eeTagHi(kSchedEeDest, false)};
                t.IsTrue(iop.writeRam(kSchedIopTag, schedTag.data(), schedTag.size() * sizeof(uint32_t)),
                         "scheduler setup should write the IOP chain tag (EOT, end header)");
                uint8_t *rdram = mem.getRDRAM();
                runtime.registerFunction(kSchedEntryPc, sif0SchedDmacEntry);
                runtime.registerFunction(kSchedResumePc, sif0SchedResume);
                runtime.registerFunction(kSchedHandlerPc, sif0SchedHandler);
                R5900Context dummyCtx{};
                runtime.eeScheduler().reset(rdram, dummyCtx);
                R5900Context mainContext{};
                mainContext.pc = kSchedEntryPc;
                runtime.eeScheduler().reset(rdram, mainContext);
                runtime.eeScheduler().run();
                t.IsTrue(std::memcmp(rdram + kSchedEeDest, payload.data(), payload.size()) == 0,
                         "scheduler DMAC transfer should deliver its payload byte-exact");
                t.IsTrue((mem.readIORegister(kEeDStat) & (1u << 5u)) != 0u,
                         "scheduler DMAC transfer should set CIS5 even when its handler delivery is masked");
                t.Equals(g_sif0SchedHandlerCount, expectedCount,
                         maskCause ? "with cause 5 masked, the DMAC handler should run 0 times"
                                   : "with transport ownership, the DMAC handler should run exactly once");
                g_sif0SchedFifo = nullptr;
                g_sif0SchedIop = nullptr;
            };
            runDmacScheduler(false, 1);
            runDmacScheduler(true, 0);

            // (b) transportOwnsSif0 == false, one HLE sceSifSetDma
            // call: exactly 1 handler run (the HLE path is sole
            // owner when the flag is false).
            {
                g_sif0SchedHandlerCount = 0;
                ps2_stubs::resetSifState();
                std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
                R5900Context ctx{};
                PS2Runtime runtime;
                runtime.eeScheduler().reset(rdram.data(), ctx);
                runtime.registerFunction(kSchedEntryPc, sif0SchedHleEntry);
                runtime.registerFunction(kSchedResumePc, sif0SchedResume);
                runtime.registerFunction(kSchedHandlerPc, sif0SchedHandler);
                constexpr uint32_t kSrc = 0x00020400u;
                constexpr uint32_t kDst = 0x00020500u;
                const std::vector<uint8_t> payload = patternedBytes(16u, 0x40u);
                std::memcpy(rdram.data() + kSrc, payload.data(), payload.size());
                const std::array<uint32_t, 4> desc{kSrc, kDst, 16u, 0u};
                std::memcpy(rdram.data() + kSchedDescAddr, desc.data(), desc.size() * sizeof(uint32_t));
                R5900Context mainContext{};
                mainContext.pc = kSchedEntryPc;
                runtime.eeScheduler().reset(rdram.data(), mainContext);
                runtime.eeScheduler().run();
                t.Equals(g_sif0SchedHandlerCount, 1,
                         "with transport ownership off, one HLE sceSifSetDma call should deliver the handler exactly once");
            }

            // (b2) transportOwnsSif0 == true, one HLE sceSifSetDma
            // call: 0 handler runs. Added 2026-11-03 after mutant
            // (b) (ungated HLE dispatch) survived: the queue-level
            // check below cannot see a direct dispatch, because
            // dispatchDmacHandlersForCause does not go through the
            // completed-causes queue — only a handler-level run
            // under ownership pins the gate.
            {
                g_sif0SchedHandlerCount = 0;
                ps2_stubs::resetSifState();
                std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
                R5900Context ctx{};
                PS2Runtime runtime;
                runtime.memory().setTransportOwnsSif0(true);
                runtime.eeScheduler().reset(rdram.data(), ctx);
                runtime.registerFunction(kSchedEntryPc, sif0SchedHleEntry);
                runtime.registerFunction(kSchedResumePc, sif0SchedResume);
                runtime.registerFunction(kSchedHandlerPc, sif0SchedHandler);
                constexpr uint32_t kSrc = 0x00020400u;
                constexpr uint32_t kDst = 0x00020500u;
                const std::vector<uint8_t> payload = patternedBytes(16u, 0x40u);
                std::memcpy(rdram.data() + kSrc, payload.data(), payload.size());
                const std::array<uint32_t, 4> desc{kSrc, kDst, 16u, 0u};
                std::memcpy(rdram.data() + kSchedDescAddr, desc.data(), desc.size() * sizeof(uint32_t));
                R5900Context mainContext{};
                mainContext.pc = kSchedEntryPc;
                runtime.eeScheduler().reset(rdram.data(), mainContext);
                runtime.eeScheduler().run();
                t.Equals(g_sif0SchedHandlerCount, 0,
                         "with transport ownership on, one HLE sceSifSetDma call should deliver the handler 0 times");
            }

            // Queue level: DMAC completion queues exactly one
            // cause 5, and a subsequent HLE sceSifSetDma call with
            // the flag true queues none (its direct dispatch is
            // gated off, design §7.1).
            {
                ps2_stubs::resetSifState();
                PS2Runtime runtime;
                PS2Memory &mem = runtime.memory();
                if (!mem.initialize())
                {
                    t.Fail("PS2Memory initialize should succeed before the queue-level check");
                    return;
                }
                Sif0Fifo fifo;
                IopMemory iop;
                mem.setSif0Transport(&fifo);
                mem.setTransportOwnsSif0(true);
                iop.setSif0Transport(&fifo);
                iop.setTransportOwnsSif0(true);
                constexpr uint32_t kTag = 0x00010000u;
                constexpr uint32_t kSrc = 0x00020000u;
                constexpr uint32_t kDest = 0x00060000u;
                const std::vector<uint8_t> payload = patternedBytes(32u, 0x80u);
                t.IsTrue(iop.writeRam(kSrc, payload.data(), payload.size()), "queue setup should write the IOP payload");
                t.IsTrue(iop.writeRam(kTag,
                                      std::array<uint32_t, 4>{kSrc | (1u << 31u), 8u,
                                                              eeTagLo(2u, 7u, false), eeTagHi(kDest, false)}.data(),
                                      16u),
                         "queue setup should write the IOP chain tag (EOT, end header)");
                mem.writeIORegister(kEeChcr, kEeChcrChainStr);
                iop.write32(kIopDmacEn, 1u);
                iop.write32(kIopDicr, kIopDicrMaster);
                iop.write32(kIopDicr2, kIopDicr2Sif0Mask);
                iop.write32(kIopTadr, kTag);
                iop.write32(kIopChcr, kIopChcrChain);
                iop.driveSif0();
                mem.driveSif0();
                const std::vector<uint32_t> causes = mem.consumeCompletedDmacCauses();
                t.IsTrue(causes.size() == 1u && causes[0] == 5u,
                         "DMAC completion should queue exactly one cause 5 at queue level");
                // HLE call under transport ownership.
                constexpr uint32_t kHleSrc = 0x00020400u;
                constexpr uint32_t kHleDst = 0x00020500u;
                constexpr uint32_t kHleDesc = 0x00020300u;
                const std::vector<uint8_t> hlePayload = patternedBytes(16u, 0x40u);
                std::memcpy(mem.getRDRAM() + kHleSrc, hlePayload.data(), hlePayload.size());
                const std::array<uint32_t, 4> desc{kHleSrc, kHleDst, 16u, 0u};
                std::memcpy(mem.getRDRAM() + kHleDesc, desc.data(), desc.size() * sizeof(uint32_t));
                R5900Context ctx{};
                setRegU32(ctx, 4, kHleDesc);
                setRegU32(ctx, 5, 1u);
                ps2_stubs::sceSifSetDma(mem.getRDRAM(), &ctx, &runtime);
                t.IsTrue(getRegS32(ctx, 2) > 0, "HLE sceSifSetDma under transport ownership should still report success");
                const std::vector<uint32_t> afterHle = mem.consumeCompletedDmacCauses();
                t.IsTrue(afterHle.empty(),
                         "an HLE sceSifSetDma call with transportOwnsSif0 true should queue no cause 5");
            }
        });

        tc.Run("7 EE CHCR held and STR polling: STR survives reads until completion", [](TestCase &t)
        {
            BothEnds b;
            if (!b.initOk)
            {
                t.Fail("PS2Memory initialize should succeed before the STR polling test");
                return;
            }
            constexpr uint32_t kTag = 0x00010000u;
            constexpr uint32_t kSrc = 0x00020000u;
            constexpr uint32_t kDest = 0x00040000u;
            const std::vector<uint8_t> payload = patternedBytes(16u, 0xA8u);
            b.startEe();
            for (int i = 0; i < 3; ++i)
            {
                t.IsTrue(b.eeStrSet(), "repeated EE CHCR reads while waiting on empty should keep STR set");
            }
            t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "STR polling setup should write the IOP payload");
            t.IsTrue(b.putTag(kTag, kSrc, 4u, eeTagLo(1u, 7u, false), eeTagHi(kDest, false), false, true),
                     "STR polling setup should write the IOP chain tag (EOT, end header QWC 1)");
            b.startIop(kTag);
            b.iop.driveSif0();
            b.ee.driveSif0();
            t.IsTrue(b.eeBytesEqual(kDest, payload), "STR polling transfer should deliver its payload");
            t.IsFalse(b.eeStrSet(), "the first EE CHCR read after completion should show STR clear");
            t.IsFalse(b.eeStrSet(), "a second EE CHCR read after completion should still show STR clear");
        });

        tc.Run("8 in-flight DmaStat on both sides, at the stall point and after completion, plus stub level", [](TestCase &t)
        {
            PS2Runtime runtime;
            PS2Memory &mem = runtime.memory();
            if (!mem.initialize())
            {
                t.Fail("PS2Memory initialize should succeed before the DmaStat test");
                return;
            }
            Sif0Fifo fifo;
            IopMemory iop;
            mem.setSif0Transport(&fifo);
            mem.setTransportOwnsSif0(true);
            iop.setSif0Transport(&fifo);
            iop.setTransportOwnsSif0(true);
            constexpr uint32_t kTag = 0x00010000u;
            constexpr uint32_t kSrc = 0x00020000u;
            constexpr uint32_t kDest = 0x00040000u;
            const std::vector<uint8_t> payload = patternedBytes(160u, 0x00u);
            t.IsTrue(iop.writeRam(kSrc, payload.data(), payload.size()), "DmaStat setup should write the 40-word IOP payload");
            t.IsTrue(iop.writeRam(kTag,
                                  std::array<uint32_t, 4>{kSrc | (1u << 31u), 40u,
                                                          eeTagLo(10u, 7u, false), eeTagHi(kDest, false)}.data(),
                                  16u),
                         "DmaStat setup should write the IOP chain tag (40 words, EOT, end header QWC 10)");
            auto stubStat = [&](uint32_t id)
            {
                R5900Context ctx{};
                setRegU32(ctx, 4, id);
                ps2_stubs::sceSifDmaStat(mem.getRDRAM(), &ctx, &runtime);
                return getRegS32(ctx, 2);
            };
            mem.writeIORegister(kEeChcr, kEeChcrChainStr);
            const uint32_t eeId = mem.sif0CurrentTransferId();
            iop.write32(kIopDmacEn, 1u);
            iop.write32(kIopDicr, kIopDicrMaster);
            iop.write32(kIopDicr2, kIopDicr2Sif0Mask);
            iop.write32(kIopTadr, kTag);
            iop.write32(kIopChcr, kIopChcrChain);
            const uint32_t iopId = iop.sif0CurrentTransferId();
            iop.driveSif0();
            t.IsTrue(iop.sif0PausedOnFull(), "DmaStat stall point should be reached (IOP paused on full)");
            t.Equals(mem.sif0DmaStat(eeId), 0, "EE DmaStat should be 0 (in-flight) at the stall point");
            t.Equals(iop.sif0DmaStat(iopId), 0, "IOP DmaStat should be 0 (in-flight) at the stall point");
            t.Equals(mem.sif0DmaStat(9999u), -1, "EE DmaStat for an unknown id should be -1");
            t.Equals(iop.sif0DmaStat(9999u), -1, "IOP DmaStat for an unknown id should be -1");
            t.Equals(stubStat(eeId), 0, "stub sceSifDmaStat under transport ownership should return 0 in-flight");
            t.Equals(stubStat(9999u), -1, "stub sceSifDmaStat for an unknown id should return -1");
            t.IsTrue((mem.readIORegister(kEeChcr) & 0x100u) != 0u, "EE STR should stay set at the stall point");
            t.IsTrue((iop.read32(kIopChcr) & (1u << 24u)) != 0u, "IOP CHCR bit 24 should stay set at the stall point");
            t.IsFalse((mem.readIORegister(kEeDStat) & (1u << 5u)) != 0u, "CIS5 should stay clear at the stall point");
            t.IsFalse((iop.read32(kIopDicr2) & kIopDicr2Sif0Flag) != 0u, "DICR2 SIF0 flag should stay clear at the stall point");
            mem.driveSif0();
            iop.driveSif0();
            mem.driveSif0();
            t.Equals(mem.sif0DmaStat(eeId), 1, "EE DmaStat should be 1 after completion");
            t.Equals(iop.sif0DmaStat(iopId), 1, "IOP DmaStat should be 1 after completion");
            t.Equals(stubStat(eeId), 1, "stub sceSifDmaStat under transport ownership should return 1 after completion");
            t.IsFalse((mem.readIORegister(kEeChcr) & 0x100u) != 0u, "EE STR should clear after completion");
            t.IsFalse((iop.read32(kIopChcr) & (1u << 24u)) != 0u, "IOP CHCR bit 24 should clear after completion");
            t.IsTrue((mem.readIORegister(kEeDStat) & (1u << 5u)) != 0u, "CIS5 should set after completion");
            t.IsTrue((iop.read32(kIopDicr2) & kIopDicr2Sif0Flag) != 0u, "DICR2 SIF0 flag should set after completion");
        });

        tc.Run("9 bounds and overflow on the EE side hold without writes or completion", [](TestCase &t)
        {
            // Each subcase is a fresh BothEnds: a held transfer must
            // not poison the next case's assertions.
            auto heldChecks = [&](BothEnds &b, const char *what)
            {
                t.IsTrue(b.ee.sif0Held(), what);
                t.IsTrue(b.ee.sif0Active(), what);
                t.IsTrue(b.eeStrSet(), what);
                t.IsFalse(b.eeCis5(), what);
                t.Equals(b.ee.sif0DmaStat(b.ee.sif0CurrentTransferId()), 0, what);
            };
            // (a) ADDR at the top of the tag-addressable range: any
            // QWC pushes it outside EE RAM.
            {
                BothEnds b;
                if (!b.initOk)
                {
                    t.Fail("PS2Memory initialize should succeed before bounds subcase (a)");
                    return;
                }
                constexpr uint32_t kTag = 0x00010000u;
                constexpr uint32_t kSrc = 0x00020000u;
                const std::vector<uint8_t> payload = patternedBytes(32u, 0x11u);
                t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "bounds (a) setup should write the IOP payload");
                t.IsTrue(b.putTag(kTag, kSrc, 8u, eeTagLo(2u, 7u, false), eeTagHi(0x7FFFFFF0u, false), false, true),
                         "bounds (a) setup should write the IOP chain tag");
                std::memset(b.ee.getRDRAM() + 0x00040000u, 0x5A, 32u);
                b.startEe();
                b.startIop(kTag);
                b.iop.driveSif0();
                b.ee.driveSif0();
                heldChecks(b, "an EE destination outside EE RAM should hold (STR stays, no CIS5, DmaStat 0)");
                t.IsTrue(b.eeBytesEqual(0x00040000u, std::vector<uint8_t>(32u, 0x5Au)),
                         "bounds (a) must not write anywhere in EE RAM");
                t.Equals(b.fifo.count(), static_cast<size_t>(2),
                         "bounds (a) should consume the header but no payload QW after holding");
            }
            // (b) ADDR + QWC*16 runs past the end of EE RAM: whole-tag
            // bounds are checked before any write, so no partial write.
            {
                BothEnds b;
                if (!b.initOk)
                {
                    t.Fail("PS2Memory initialize should succeed before bounds subcase (b)");
                    return;
                }
                constexpr uint32_t kTag = 0x00010000u;
                constexpr uint32_t kSrc = 0x00020000u;
                // NOTE (test calibration, fixed 2026-11-03): the first
                // version of this subcase used 0x01FFFF00, but
                // 0x01FFFF00 + 16*16 == 0x02000000 == PS2_RAM_SIZE, so
                // that transfer fits exactly in RAM and the engine
                // rightly completes it. The genuinely overrunning
                // destination is 0x01FFFF10: it ends 16 bytes past RAM
                // end, and only the 240-byte in-RAM prefix is seeded
                // (a 256-byte seed here would write past the RDRAM
                // buffer itself).
                constexpr uint32_t kDest = 0x01FFFF10u;
                const std::vector<uint8_t> payload = patternedBytes(256u, 0x22u);
                t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "bounds (b) setup should write the IOP payload");
                t.IsTrue(b.putTag(kTag, kSrc, 64u, eeTagLo(16u, 7u, false), eeTagHi(kDest, false), false, true),
                         "bounds (b) setup should write the IOP chain tag (QWC 16 from 0x01FFFF10)");
                std::memset(b.ee.getRDRAM() + kDest, 0x5A, 240u);
                b.startEe();
                b.startIop(kTag);
                b.iop.driveSif0();
                b.ee.driveSif0();
                heldChecks(b, "an EE destination running past EE RAM end should hold (STR stays, no CIS5, DmaStat 0)");
                t.IsTrue(b.eeBytesEqual(kDest, std::vector<uint8_t>(240u, 0x5Au)),
                         "bounds (b) must not partially write the in-RAM prefix of an overrunning tag");
            }
            // (c) scratchpad destination (EE DMAtag hi bit 31).
            {
                BothEnds b;
                if (!b.initOk)
                {
                    t.Fail("PS2Memory initialize should succeed before bounds subcase (c)");
                    return;
                }
                constexpr uint32_t kTag = 0x00010000u;
                constexpr uint32_t kSrc = 0x00020000u;
                constexpr uint32_t kDest = 0x00040000u;
                const std::vector<uint8_t> payload = patternedBytes(32u, 0x33u);
                t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "bounds (c) setup should write the IOP payload");
                t.IsTrue(b.putTag(kTag, kSrc, 8u, eeTagLo(2u, 7u, false), eeTagHi(kDest, true), false, true),
                         "bounds (c) setup should write the IOP chain tag with a scratchpad destination");
                std::memset(b.ee.getRDRAM() + kDest, 0x5A, 32u);
                b.startEe();
                b.startIop(kTag);
                b.iop.driveSif0();
                b.ee.driveSif0();
                heldChecks(b, "a scratchpad destination should hold (STR stays, no CIS5, DmaStat 0)");
                t.IsTrue(b.eeBytesEqual(kDest, std::vector<uint8_t>(32u, 0x5Au)),
                         "bounds (c) must not write to the scratchpad-named destination");
            }
            // (d) invalid Tag ID (3 is not cnt/cnts/end).
            {
                BothEnds b;
                if (!b.initOk)
                {
                    t.Fail("PS2Memory initialize should succeed before bounds subcase (d)");
                    return;
                }
                constexpr uint32_t kTag = 0x00010000u;
                constexpr uint32_t kSrc = 0x00020000u;
                constexpr uint32_t kDest = 0x00040000u;
                const std::vector<uint8_t> payload = patternedBytes(32u, 0x44u);
                t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "bounds (d) setup should write the IOP payload");
                t.IsTrue(b.putTag(kTag, kSrc, 8u, eeTagLo(2u, 3u, false), eeTagHi(kDest, false), false, true),
                         "bounds (d) setup should write the IOP chain tag with Tag ID 3");
                std::memset(b.ee.getRDRAM() + kDest, 0x5A, 32u);
                b.startEe();
                b.startIop(kTag);
                b.iop.driveSif0();
                b.ee.driveSif0();
                heldChecks(b, "an invalid Tag ID should hold (STR stays, no CIS5, DmaStat 0)");
                t.IsTrue(b.eeBytesEqual(kDest, std::vector<uint8_t>(32u, 0x5Au)),
                         "bounds (d) must not write for an invalid Tag ID");
            }
        });

        tc.Run("10 partial QW uses IOP RAM padding words, never zero-fill (corrected rule)", [](TestCase &t)
        {
            BothEnds b;
            if (!b.initOk)
            {
                t.Fail("PS2Memory initialize should succeed before the partial-QW test");
                return;
            }
            constexpr uint32_t kTag = 0x00010000u;
            constexpr uint32_t kSrc = 0x00020000u;
            constexpr uint32_t kDest = 0x00040000u;
            // Payload words W0..W5, followed in IOP RAM by nonzero
            // padding words P0,P1. The IOP engine rounds 6 words up
            // to 8 (2 QWs) and reads the padding from IOP RAM.
            const std::vector<uint32_t> iopWords{
                0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                0x55555555u, 0x66666666u, 0xDEADBEEFu, 0xCAFEBABEu,
            };
            t.IsTrue(b.putIopWords(kSrc, iopWords), "partial-QW setup should write W0..W5 plus P0,P1 to IOP RAM");
            t.IsTrue(b.putTag(kTag, kSrc, 6u, eeTagLo(2u, 7u, false), eeTagHi(kDest, false), false, true),
                     "partial-QW setup should write the IOP chain tag (6 words, EOT, end header QWC 2)");
            b.startEe();
            b.startIop(kTag);
            b.iop.driveSif0();
            b.ee.driveSif0();
            // QWC 2 = 2 QWs = 32 bytes = 8 words: W0..W5 then P0,P1.
            const std::vector<uint32_t> expected{
                0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                0x55555555u, 0x66666666u, 0xDEADBEEFu, 0xCAFEBABEu,
            };
            t.IsTrue(b.eeBytesEqual(kDest, reinterpret_cast<const uint8_t *>(expected.data()),
                                    expected.size() * sizeof(uint32_t)),
                     "EE RAM should receive W0..W5 followed by the IOP RAM padding words P0,P1 (32 bytes)");
            uint32_t lastTwo[2] = {};
            std::memcpy(lastTwo, b.ee.getRDRAM() + kDest + 24u, sizeof(lastTwo));
            t.IsTrue(lastTwo[0] == 0xDEADBEEFu && lastTwo[1] == 0xCAFEBABEu,
                     "the last 2 words must equal P0,P1 from IOP RAM");
            t.IsTrue(lastTwo[0] != 0u && lastTwo[1] != 0u,
                     "the last 2 words must NOT be zero-filled (the superseded design rule)");
            t.IsTrue(b.eeCis5(), "partial-QW transfer should still complete via CIS5");
        });

        tc.Run("11 SIF2 isolation: channel 7 and SIF1 HLE traffic stay off the SIF0 FIFO", [](TestCase &t)
        {
            // Channel 7 (SIF2) STR write: nothing through the SIF0
            // FIFO, no CIS5, nothing completed as SIF0.
            {
                BothEnds b;
                if (!b.initOk)
                {
                    t.Fail("PS2Memory initialize should succeed before the SIF2 subcase");
                    return;
                }
                b.ee.writeIORegister(kEeCh7Chcr, kEeChcrChainStr);
                t.Equals(b.fifo.count(), static_cast<size_t>(0),
                         "a channel-7 STR write should move nothing through the SIF0 FIFO");
                t.IsFalse(b.eeCis5(), "a channel-7 STR write should set no CIS5");
                t.IsFalse(b.ee.sif0Active(), "a channel-7 STR write should start no SIF0 transfer");
                t.Equals(b.ee.sif0DmaStat(1u), -1, "a channel-7 STR write should complete nothing as SIF0");
            }
            // SIF1 HLE sceSifSetDma (flag false): FIFO count unchanged.
            {
                ps2_stubs::resetSifState();
                Sif0Fifo fifo;
                std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
                R5900Context ctx{};
                PS2Runtime runtime;
                runtime.eeScheduler().reset(rdram.data(), ctx);
                runtime.memory().setSif0Transport(&fifo);
                runtime.memory().setTransportOwnsSif0(false);
                constexpr uint32_t kSrc = 0x00020400u;
                constexpr uint32_t kDst = 0x00020500u;
                constexpr uint32_t kDesc = 0x00020300u;
                const std::vector<uint8_t> payload = patternedBytes(16u, 0x40u);
                std::memcpy(rdram.data() + kSrc, payload.data(), payload.size());
                const std::array<uint32_t, 4> desc{kSrc, kDst, 16u, 0u};
                std::memcpy(rdram.data() + kDesc, desc.data(), desc.size() * sizeof(uint32_t));
                t.Equals(fifo.count(), static_cast<size_t>(0), "SIF0 FIFO should start empty for the SIF1 HLE check");
                setRegU32(ctx, 4, kDesc);
                setRegU32(ctx, 5, 1u);
                ps2_stubs::sceSifSetDma(rdram.data(), &ctx, &runtime);
                t.IsTrue(getRegS32(ctx, 2) > 0, "SIF1 HLE sceSifSetDma should succeed");
                t.Equals(fifo.count(), static_cast<size_t>(0),
                         "an SIF1 HLE sceSifSetDma transfer should leave the SIF0 FIFO count unchanged");
            }
        });

        tc.Run("12 IOP BCR is ignored in chain mode: transfer size follows the tag", [](TestCase &t)
        {
            BothEnds b;
            if (!b.initOk)
            {
                t.Fail("PS2Memory initialize should succeed before the BCR test");
                return;
            }
            constexpr uint32_t kTag = 0x00010000u;
            constexpr uint32_t kSrc = 0x00020000u;
            constexpr uint32_t kDest = 0x00040000u;
            const std::vector<uint8_t> payload = patternedBytes(32u, 0xB0u);
            t.IsTrue(b.putIopBytes(kSrc, payload.data(), payload.size()), "BCR setup should write the IOP payload");
            t.IsTrue(b.putTag(kTag, kSrc, 8u, eeTagLo(2u, 7u, false), eeTagHi(kDest, false), false, true),
                     "BCR setup should write the IOP chain tag (8 words, EOT, end header QWC 2)");
            b.iop.write32(kIopBcr, 0x00010001u);  // Conflicting BCR: must be ignored (model choice, design §5.1).
            b.startEe();
            b.startIop(kTag);
            b.iop.driveSif0();
            b.ee.driveSif0();
            t.IsTrue(b.eeBytesEqual(kDest, payload),
                     "transfer size should follow the tag (2 QWs), not the conflicting BCR value");
            t.Equals(b.iop.read32(kIopBcr), 0x00010001u,
                     "BCR should still read back as written (ignored, not consumed or cleared)");
            t.IsTrue(b.eeCis5(), "BCR-ignored transfer should still complete via CIS5");
            t.IsFalse(b.iop.sif0Active(), "BCR-ignored transfer should finish on the IOP side");
        });
    });
}
