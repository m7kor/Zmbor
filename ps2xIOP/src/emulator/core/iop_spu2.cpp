#include "iop_spu2.h"

#include <algorithm>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kCoreStride = 0x400u;
        constexpr uint32_t kSharedBase = 0x760u;
        constexpr uint32_t kIrqInfo = 0x7C2u;

        constexpr uint32_t kAttr = 0x19Au;
        constexpr uint32_t kIrqaHigh = 0x19Cu;
        constexpr uint32_t kKeyOn = 0x1A0u;
        constexpr uint32_t kKeyOff = 0x1A4u;
        constexpr uint32_t kTsaHigh = 0x1A8u;
        constexpr uint32_t kTsaLow = 0x1AAu;
        constexpr uint32_t kDataPort = 0x1ACu;
        constexpr uint32_t kVoiceAddresses = 0x1C0u; // 24 x (SSA, LSAX, NAX), 12 bytes each
        constexpr uint32_t kVoiceAddressEnd = kVoiceAddresses + 24u * 0xCu;
        constexpr uint32_t kEndxHigh = 0x340u;
        constexpr uint32_t kEndxLow = 0x342u;
        constexpr uint32_t kStat = 0x344u;

        constexpr uint16_t kAttrIrqEnable = 1u << 6;
        constexpr uint16_t kAttrReset = 1u << 15;
        constexpr uint16_t kStatDmaReady = 1u << 7;
        constexpr uint16_t kStatDmaBusy = 1u << 10;
    }

    Spu2::Spu2()
        : m_ram(kRamHalfwords)
    {
        reset();
    }

    void Spu2::reset()
    {
        m_cores = {};
        m_adma = {};
        m_reverb = {};
        m_reverbSecondTick = false;
        m_shared = {};
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_irqInfo = 0u;
        m_interrupt = false;
        m_sampleCycleRemainder = 0u;
    }

    uint32_t Spu2::address20(const Core &core, uint32_t highReg) const
    {
        return ((static_cast<uint32_t>(core.regs[highReg / 2u]) & 0xFu) << 16) | core.regs[highReg / 2u + 1u];
    }

    // Any sound-RAM access at an enabled core's IRQA raises the SPU2 interrupt (PCSX2 and DobieStation check both
    // cores on every access).
    void Spu2::accessRam(uint32_t address)
    {
        for (int c = 0; c < 2; ++c)
        {
            const Core &core = m_cores[c];
            if ((core.regs[kAttr / 2u] & kAttrIrqEnable) != 0u && address20(core, kIrqaHigh) == address)
            {
                m_irqInfo |= static_cast<uint16_t>(4u << c);
                m_interrupt = true;
            }
        }
    }

    namespace
    {
        int16_t clamp16(int32_t value)
        {
            if (value < -32768)
                return -32768;
            if (value > 32767)
                return 32767;
            return static_cast<int16_t>(value);
        }

        int32_t signExtend4(uint8_t nibble)
        {
            const uint8_t value = static_cast<uint8_t>(nibble & 0x0Fu);
            return (value & 0x08u) != 0u ? static_cast<int32_t>(value) - 16 : static_cast<int32_t>(value);
        }

        // Floor division by 0x8000, i.e. an arithmetic shift right by 15 (SAR 15). Used
        // for the exponential-decrease step (Phase 6b): the hardware envelope unit shifts,
        // it does not truncate toward zero, so -8 * 32767 / 32768 is -8, not -7.
        int32_t floorDivBy32768(int64_t value)
        {
            if (value >= 0)
                return static_cast<int32_t>(value / 0x8000);
            return -static_cast<int32_t>((-value - 1) / 0x8000 + 1);
        }

        // Floor division by 2 (SAR 1), for the reverb pair average (Phase 10):
        // (-1 + 0) / 2 must be -1 (floor), not 0 (C++ truncation).
        int32_t floorDivBy2(int32_t value)
        {
            if (value >= 0)
                return value / 2;
            return -((-value + 1) / 2);
        }

        // One envelope rate step (psx-spx "Envelope Operation", Phase 6, corrected in
        // Phase 6b). `magnitude` is the unscaled step size (7 - step for an increase,
        // 8 - step for a decreasing sustain phase, 8 for decay/release). Returns true when
        // a step was applied this sample. `neverStep` is the all-ones rate (attack/sustain
        // step 3 + shift 31, release shift 31), which never steps; every other rate gets a
        // counter floor of 1. An exponential-decrease step is computed with SAR-15 floor
        // semantics, so it is <= -1 whenever the level is above its target; the forced -1
        // below is retained only as a guard and is inert in that case (both are documented
        // in iop_spu2.h).
        bool stepEnvelopeRate(int shift, int magnitude, bool exponential, bool decreasing, int32_t &level,
                              uint32_t &counter, int32_t target, bool neverStep)
        {
            if (neverStep)
                return false;
            int32_t stepMagnitude = magnitude << std::max(0, 11 - shift);
            uint32_t increment = 0x8000u >> static_cast<uint32_t>(std::max(0, shift - 11));
            if (exponential && !decreasing && level > 0x6000)
            {
                if (shift < 10)
                    stepMagnitude /= 4;
                else if (shift >= 11)
                    increment /= 4;
                else
                {
                    stepMagnitude /= 2;
                    increment /= 2;
                }
            }
            if (increment == 0u)
                increment = 1u;
            counter += increment;
            if (counter < 0x8000u)
                return false;
            counter -= 0x8000u;
            int32_t step = 0;
            if (decreasing)
            {
                if (exponential)
                {
                    // Phase 6b: SAR-15 floor, matching PCSX2's (level_inc * Value) >> 15 and
                    // the shift-based reading of psx-spx's AdsrStep*AdsrLevel/8000h. The old
                    // truncating division gave -7 for -8 * 32767 / 32768; the shift gives -8.
                    step = floorDivBy32768(-static_cast<int64_t>(stepMagnitude) * level);
                    if (step == 0 && level > target)
                        step = -1;
                }
                else
                {
                    step = -stepMagnitude;
                }
            }
            else
            {
                step = stepMagnitude;
            }
            level += step;
            return true;
        }

        int32_t sustainTarget(uint16_t adsr1)
        {
            const int32_t target = (static_cast<int32_t>(adsr1 & 0xFu) + 1) * 0x800;
            return target > 0x7FFF ? 0x7FFF : target;
        }
    }

    Spu2::AdpcmBlock Spu2::decodeAdpcmBlock(const uint8_t block[16], int16_t &previous,
                                            int16_t &previousPrevious)
    {
        AdpcmBlock out{};
        // Header byte 0: shift in bits 0-3, filter in bits 4-6. The out-of-range clamping (shift > 12 -> 9,
        // filter > 4 -> 0) is inherited from the project's VAG decoder, not a hardware claim.
        uint8_t shift = static_cast<uint8_t>(block[0] & 0x0Fu);
        if (shift > 12u)
            shift = 9u;
        uint8_t filter = static_cast<uint8_t>((block[0] >> 4u) & 0x07u);
        if (filter > 4u)
            filter = 0u;
        out.shift = shift;
        out.filter = filter;
        // Header byte 1, i.e. halfword bits 8-10: loop-end, loop, loop-start.
        out.flags = static_cast<uint8_t>(block[1] & 0x07u);

        int32_t s1 = previous;
        int32_t s2 = previousPrevious;
        for (int i = 0; i < 28; ++i)
        {
            const uint8_t byte = block[2 + i / 2];
            const uint8_t nibble = (i & 1) != 0 ? static_cast<uint8_t>(byte >> 4u) : static_cast<uint8_t>(byte & 0x0Fu);
            const int32_t shifted = signExtend4(nibble) << (12 - shift);
            int32_t filtered = shifted;
            switch (filter)
            {
            case 1: filtered = shifted + (60 * s1 + 32) / 64; break;
            case 2: filtered = shifted + (115 * s1 - 52 * s2 + 32) / 64; break;
            case 3: filtered = shifted + (98 * s1 - 55 * s2 + 32) / 64; break;
            case 4: filtered = shifted + (122 * s1 - 60 * s2 + 32) / 64; break;
            default: break;
            }
            const int16_t sample = clamp16(filtered);
            out.samples[static_cast<size_t>(i)] = sample;
            s2 = s1;
            s1 = sample;
        }
        previous = static_cast<int16_t>(s1);
        previousPrevious = static_cast<int16_t>(s2);
        return out;
    }

    Spu2::AdpcmBlock Spu2::decodeAdpcmBlockAt(uint32_t halfwordAddress, int16_t &previous,
                                              int16_t &previousPrevious)
    {
        uint8_t block[16]{};
        uint32_t address = halfwordAddress & (kRamHalfwords - 1u);
        for (int i = 0; i < 8; ++i)
        {
            const uint16_t halfword = m_ram[address];
            block[2 * i] = static_cast<uint8_t>(halfword & 0xFFu);
            block[2 * i + 1] = static_cast<uint8_t>(halfword >> 8u);
            accessRam(address);
            address = (address + 1u) & (kRamHalfwords - 1u);
        }
        return decodeAdpcmBlock(block, previous, previousPrevious);
    }

    bool Spu2::voiceActive(int core, int voice) const
    {
        if (core < 0 || core > 1 || voice < 0 || voice >= 24)
            return false;
        const VoiceState &state = m_cores[static_cast<size_t>(core)].voices[static_cast<size_t>(voice)];
        // A non-looping voice's final block is still playing while its samples remain buffered,
        // even though stepVoiceBlock already cleared `active` at the loop-end block.
        return state.active || state.sampleIndex < 28u;
    }

    std::optional<Spu2::AdpcmBlock> Spu2::stepVoiceBlock(int core, int voice)
    {
        if (core < 0 || core > 1 || voice < 0 || voice >= 24)
            return std::nullopt;
        Core &c = m_cores[static_cast<size_t>(core)];
        VoiceState &state = c.voices[static_cast<size_t>(voice)];
        const uint32_t bit = 1u << voice;

        if ((c.keyOn & bit) != 0u)
        {
            // (Re)start: consume KON, clear ADPCM history. NAX was already loaded from SSA by the KON write
            // and ENDX was already cleared there, so this step only has to start decoding at NAX.
            // KON also restarts the ADSR envelope in attack at level 0 and clears a pending KOFF
            // for this voice (KON wins over KOFF, Phase 6).
            c.keyOn &= ~bit;
            c.keyOff &= ~bit;
            state.active = true;
            state.previous = 0;
            state.previousPrevious = 0;
            state.adsrPhase = AdsrPhase::Attack;
            state.adsrLevel = 0;
            state.adsrCounter = 0;
            c.regs[static_cast<size_t>(voice) * 8u + 5u] = 0;
        }
        if (!state.active)
            return std::nullopt;

        const uint32_t voiceBase = (kVoiceAddresses + static_cast<uint32_t>(voice) * 0xCu) / 2u;
        const uint32_t current = address20(c, kVoiceAddresses + static_cast<uint32_t>(voice) * 0xCu + 8u);
        AdpcmBlock block = decodeAdpcmBlockAt(current, state.previous, state.previousPrevious);

        const uint32_t next = (current + 8u) & (kRamHalfwords - 1u);
        if ((block.flags & 0x04u) != 0u)
        {
            // Loop-start: this block becomes the loop point, kept in LSAX.
            c.regs[voiceBase + 2u] = static_cast<uint16_t>((current >> 16u) & 0xFu);
            c.regs[voiceBase + 3u] = static_cast<uint16_t>(current & 0xFFFFu);
        }
        if ((block.flags & 0x01u) != 0u)
        {
            c.endx |= bit;
            if ((block.flags & 0x02u) != 0u)
            {
                // Looping endpoint: continue at LSAX (which this block may just have set).
                const uint32_t loop = address20(c, kVoiceAddresses + static_cast<uint32_t>(voice) * 0xCu + 4u);
                c.regs[voiceBase + 4u] = static_cast<uint16_t>((loop >> 16u) & 0xFu);
                c.regs[voiceBase + 5u] = static_cast<uint16_t>(loop & 0xFFFFu);
            }
            else
            {
                state.active = false;
                c.regs[voiceBase + 4u] = static_cast<uint16_t>((next >> 16u) & 0xFu);
                c.regs[voiceBase + 5u] = static_cast<uint16_t>(next & 0xFFFFu);
            }
        }
        else
        {
            c.regs[voiceBase + 4u] = static_cast<uint16_t>((next >> 16u) & 0xFu);
            c.regs[voiceBase + 5u] = static_cast<uint16_t>(next & 0xFFFFu);
        }
        return block;
    }

    // 4-point Gaussian interpolation (Phase 7, psx-spx formula, literally): each
    // product is shifted right by 15 (floor, via floorDivBy32768) before summing, and
    // the sum is clamped to int16. `history` is oldest, older, old, new.
    int16_t interpolateGaussian(const std::array<int16_t, 4> &history, uint32_t index)
    {
        const int32_t out = floorDivBy32768(static_cast<int64_t>(Spu2::kGaussTable[0xFFu - index]) * history[0]) +
                            floorDivBy32768(static_cast<int64_t>(Spu2::kGaussTable[0x1FFu - index]) * history[1]) +
                            floorDivBy32768(static_cast<int64_t>(Spu2::kGaussTable[0x100u + index]) * history[2]) +
                            floorDivBy32768(static_cast<int64_t>(Spu2::kGaussTable[index]) * history[3]);
        return clamp16(out);
    }

    std::optional<int16_t> Spu2::nextSample(int core, int voice)
    {
        if (core < 0 || core > 1 || voice < 0 || voice >= 24)
            return std::nullopt;
        // Pitch (Phase 7): PITCH is voice register 2, re-read every tick. A step above
        // 0x3FFF is clipped to 0x4000 (psx-spx); 0 stops the voice without consuming
        // KON or stepping the envelope, so writing a pitch later resumes it. PMON is
        // not applied here (see iop_spu2.h): mixTick supplies the modulated step.
        const uint32_t pitch = m_cores[static_cast<size_t>(core)].regs[static_cast<size_t>(voice) * 8u + 2u];
        const uint32_t step = pitch > 0x3FFFu ? 0x4000u : pitch;
        return nextSampleWithStep(core, voice, step);
    }

    std::optional<int16_t> Spu2::nextSampleWithStep(int core, int voice, uint32_t step)
    {
        if (core < 0 || core > 1 || voice < 0 || voice >= 24)
            return std::nullopt;
        Core &c = m_cores[static_cast<size_t>(core)];
        VoiceState &state = c.voices[static_cast<size_t>(voice)];
        const uint32_t bit = 1u << voice;
        // A pending KON restarts from SSA: discard any buffered block from the previous
        // playback, otherwise stale samples would play before the restart takes effect.
        // The interpolation history and pitch fraction restart too, so the first outputs
        // after KON are the documented transient from a zeroed history.
        if ((c.keyOn & bit) != 0u)
        {
            state.sampleIndex = 28u;
            state.interp = {};
            state.pitchFraction = 0u;
        }
        else if ((c.keyOff & bit) != 0u)
        {
            // KOFF is finally consumed here (Phase 6): attack/decay/sustain move to release.
            // A KOFF on an idle voice, or one already in release, is simply discarded.
            c.keyOff &= ~bit;
            if (state.adsrPhase == AdsrPhase::Attack || state.adsrPhase == AdsrPhase::Decay ||
                state.adsrPhase == AdsrPhase::Sustain)
            {
                state.adsrPhase = AdsrPhase::Release;
                state.adsrCounter = 0;
            }
        }
        // A step of 0 (register pitch 0, or a PMON step of 0) stops the voice
        // without consuming KON or stepping the envelope, so writing a pitch
        // later resumes it.
        if (step == 0u)
            return std::nullopt;
        // KON decodes the first block on the first tick (iris documents KON
        // decoding the first block), even if this tick's pitch step consumes no
        // source sample yet; without this, a very low pitch would delay the KON
        // and the envelope start by thousands of ticks.
        if ((c.keyOn & bit) != 0u && state.sampleIndex >= 28u)
        {
            if (std::optional<AdpcmBlock> first = stepVoiceBlock(core, voice))
            {
                state.currentBlock = *first;
                state.sampleIndex = 0u;
            }
        }
        state.pitchFraction += step;
        const uint32_t whole = state.pitchFraction >> 12;
        const uint32_t gaussIndex = (state.pitchFraction >> 4) & 0xFFu;
        state.pitchFraction &= 0xFFFu;
        // Consume `whole` source samples into the interpolation history, fetching
        // blocks on demand across block boundaries and loop jumps. If the stream ends
        // before any sample is consumed this tick, the voice is over and yields
        // nothing; if at least one sample was consumed, this tick still outputs its
        // interpolated value (so a pitch-2.0 voice's final tick is its last sample).
        uint32_t consumed = 0u;
        for (uint32_t i = 0u; i < whole; ++i)
        {
            if (state.sampleIndex >= 28u)
            {
                std::optional<AdpcmBlock> block = stepVoiceBlock(core, voice);
                if (!block)
                    break;
                state.currentBlock = *block;
                state.sampleIndex = 0u;
            }
            state.interp[0] = state.interp[1];
            state.interp[1] = state.interp[2];
            state.interp[2] = state.interp[3];
            state.interp[3] = state.currentBlock.samples[state.sampleIndex++];
            ++consumed;
        }
        // A tick that consumes nothing still outputs its interpolated value while
        // the voice is playing (pitch < 1.0 advances the source position only on some
        // ticks; a buffered final block still plays after ENDX; a pending KON is the
        // starting transient from a zeroed history). Only a voice that is inactive,
        // has no buffered source sample and is not newly keyed is over.
        if (consumed == 0u && !state.active && state.sampleIndex >= 28u && (c.keyOn & bit) == 0u)
            return std::nullopt;
        // One envelope step per returned output sample (not per source sample). A
        // release that reaches zero stops the voice on this sample: the buffered block
        // is discarded and no sample is returned.
        if (stepEnvelope(c, state, voice))
        {
            state.active = false;
            state.sampleIndex = 28u;
            return std::nullopt;
        }
        ++state.samplesPlayed;
        return interpolateGaussian(state.interp, gaussIndex);
    }

    void Spu2::advanceSamples(int core, int voice, uint64_t count)
    {
        for (uint64_t i = 0u; i < count; ++i)
        {
            // Once a voice has ended (and is not re-keyed from outside during this call)
            // no further sample will appear, so stop instead of spinning on nullopt.
            if (!nextSample(core, voice))
                break;
        }
    }

    void Spu2::setReverbParams(int core, const ReverbParams &params)
    {
        if (core < 0 || core > 1)
            return;
        // Phase 10b (D-REV-10b-4): a parameter rewrite PRESERVES
        // the delay buffers, held output and pair state — the tail
        // must survive reprogramming, exactly as libsd's
        // sceSdSetEffectAttr rewrites params without clearing the
        // work area unless CLEAR is set. The explicit clearing path
        // is resetReverb; Phase 10's zeroing behaviour moved there.
        // Note: changing workSizeSamples here re-sizes the buffers
        // (a model-API reconfiguration, not a guest write — guest
        // writes never change the work size), which necessarily
        // discards buffer contents for the new size.
        ReverbState &state = m_reverb[static_cast<size_t>(core)];
        const uint32_t oldSize = static_cast<uint32_t>(state.bufL.size());
        state.params = params;
        const uint32_t n = params.workSizeSamples == 0u ? 1u : params.workSizeSamples;
        if (n != oldSize)
        {
            state.bufL.assign(n, 0);
            state.bufR.assign(n, 0);
            state.cursor = 0u;
            state.prevWetL = 0;
            state.prevWetR = 0;
            state.heldL = 0;
            state.heldR = 0;
        }
    }

    void Spu2::resetReverb(int core)
    {
        if (core < 0 || core > 1)
            return;
        // The explicit clear (Phase 10's old setReverbParams
        // behaviour): buffers, cursor, pair state and held output
        // are zeroed, params and enable flag untouched.
        ReverbState &state = m_reverb[static_cast<size_t>(core)];
        const uint32_t n = state.params.workSizeSamples == 0u ? 1u : state.params.workSizeSamples;
        state.bufL.assign(n, 0);
        state.bufR.assign(n, 0);
        state.cursor = 0u;
        state.prevWetL = 0;
        state.prevWetR = 0;
        state.heldL = 0;
        state.heldR = 0;
    }

    uint16_t Spu2::sharedPReg(int core, uint32_t pReg) const
    {
        if (core < 0 || core > 1 || pReg > 0x26u)
            return 0u;
        return m_shared[static_cast<size_t>(core) * 20u + pReg / 2u];
    }

    uint32_t Spu2::reverbEsaAddress(int core) const
    {
        if (core < 0 || core > 1)
            return 0u;
        const Core &c = m_cores[static_cast<size_t>(core)];
        const uint64_t hi = c.regs[0x170u];
        const uint64_t lo = c.regs[0x171u];
        return static_cast<uint32_t>((hi << 17) | (lo << 1));
    }

    uint32_t Spu2::reverbEeaAddress(int core) const
    {
        if (core < 0 || core > 1)
            return 0u;
        const Core &c = m_cores[static_cast<size_t>(core)];
        const uint64_t hi = c.regs[0x19Eu];
        return static_cast<uint32_t>((hi << 17) | 0x1FFFFu);
    }

    bool Spu2::reverbWorkWindowValid(int core) const
    {
        if (core < 0 || core > 1)
            return false;
        const Core &c = m_cores[static_cast<size_t>(core)];
        const uint64_t esa = (static_cast<uint64_t>(c.regs[0x170u]) << 17) |
                             (static_cast<uint64_t>(c.regs[0x171u]) << 1);
        uint64_t eea = (static_cast<uint64_t>(c.regs[0x19Eu]) << 17) | 0x1FFFFu;
        if (eea > 0x1FFFFFu)
            eea = 0x1FFFFFu; // truncated to the RAM top (D-REV-10b-3)
        return esa < 0x200000u && esa <= eea;
    }

    bool Spu2::guestEffectsEnabled(int core) const
    {
        if (core < 0 || core > 1)
            return false;
        return m_reverb[static_cast<size_t>(core)].guestEffectsEnabled;
    }

    const Spu2::ReverbParams &Spu2::reverbParams(int core) const
    {
        static const ReverbParams kDefault{};
        if (core < 0 || core > 1)
            return kDefault;
        return m_reverb[static_cast<size_t>(core)].params;
    }

    void Spu2::applyGuestPWrite(int core, uint32_t pReg)
    {
        if (core < 0 || core > 1 || pReg > 0x26u)
            return;
        Core &c = m_cores[static_cast<size_t>(core)];
        ReverbState &rev = m_reverb[static_cast<size_t>(core)];
        const auto v = [&]() { return static_cast<int16_t>(sharedPReg(core, pReg)); };
        // Exactly one field per register (D-REV-10b-7): a partial
        // guest write must not clobber the other coefficients.
        switch (pReg)
        {
        case 0x00u: case 0x02u: c.mvolWritten = true; break;
        case 0x04u: rev.params.effectVolumeL = v(); break;
        case 0x06u: rev.params.effectVolumeR = v(); break;
        case 0x08u: case 0x0Au: break; // AVOL: stored, feeds nothing (D-REV-10b-6)
        case 0x0Cu: case 0x0Eu: c.bvolWritten = true; break;
        case 0x10u: case 0x12u: break; // MVOLXL/XR: mirrors, overwritten by MVOL on mixTick
        case 0x14u: break; // IIR_ALPHA: stored only, ambiguous role (D-REV-10b-7)
        case 0x16u: rev.params.vComb[0] = v(); break;
        case 0x18u: rev.params.vComb[1] = v(); break;
        case 0x1Au: rev.params.vComb[2] = v(); break;
        case 0x1Cu: rev.params.vComb[3] = v(); break;
        case 0x1Eu: rev.params.vIir = v(); break;
        case 0x20u: rev.params.vWall = v(); break;
        case 0x22u: break; // FB_X: stored only, ambiguous role (D-REV-10b-7)
        case 0x24u: rev.params.vLin = v(); break;
        case 0x26u: rev.params.vRin = v(); break;
        default: break;
        }
    }

    void Spu2::applyGuestRWrite(int core, uint32_t rReg)
    {
        if (core < 0 || core > 1 || rReg > 0x58u)
            return;
        const Core &c = m_cores[static_cast<size_t>(core)];
        ReverbState &rev = m_reverb[static_cast<size_t>(core)];
        // Pair reconstruction (libsd SetEffectRegister, D-REV-10b-2):
        // hi at the base halfword, lo at the next; value =
        // (hi << 14) | (lo >> 2). The tap takes the low 16 bits,
        // signed; the engine takes taps modulo the work size.
        const uint32_t base = 0x170u + rReg / 2u;
        const uint32_t value = (static_cast<uint32_t>(c.regs[base]) << 14) |
                               (static_cast<uint32_t>(c.regs[base + 1u]) >> 2);
        const int16_t tap = static_cast<int16_t>(value & 0xFFFFu);
        switch (rReg)
        {
        case 0x00u: case 0x5Cu: break; // ESA / EEA: window is read live, no engine field
        case 0x04u: rev.params.dSameL = tap; break;
        case 0x08u: rev.params.dSameR = tap; break;
        case 0x0Cu: rev.params.mSameL = tap; break;
        case 0x10u: rev.params.mDiffL = tap; break;
        case 0x14u: rev.params.mCombL[0] = tap; break;
        case 0x18u: rev.params.mCombL[1] = tap; break;
        case 0x1Cu: rev.params.mCombL[2] = tap; break;
        case 0x20u: rev.params.mCombL[3] = tap; break;
        case 0x24u: rev.params.dDiffL = tap; break;
        case 0x28u: break; // IIR_SRC_A1: stored only (D-REV-10b-7)
        case 0x2Cu: rev.params.mSameR = tap; break;
        case 0x30u: rev.params.mDiffR = tap; break;
        case 0x34u: rev.params.mCombR[0] = tap; break;
        case 0x38u: rev.params.mCombR[1] = tap; break;
        case 0x3Cu: rev.params.mCombR[2] = tap; break;
        case 0x40u: rev.params.mCombR[3] = tap; break;
        case 0x44u: break; // IIR_SRC_B1: stored only (D-REV-10b-7)
        case 0x48u: rev.params.dDiffR = tap; break;
        case 0x4Cu: rev.params.mApfL[0] = tap; break;
        case 0x50u: rev.params.mApfL[1] = tap; break;
        case 0x54u: rev.params.mApfR[0] = tap; break;
        case 0x58u: rev.params.mApfR[1] = tap; break;
        default: break;
        }
    }

    void Spu2::setReverbEnabled(int core, bool enabled)
    {
        if (core < 0 || core > 1)
            return;
        ReverbState &state = m_reverb[static_cast<size_t>(core)];
        state.enabled = enabled;
        if (!enabled)
        {
            // A disabled core contributes exactly 0 from this tick on.
            state.heldL = 0;
            state.heldR = 0;
        }
    }

    bool Spu2::reverbEnabled(int core) const
    {
        if (core < 0 || core > 1)
            return false;
        return m_reverb[static_cast<size_t>(core)].enabled;
    }

    // One reverb tick (Phase 10, D-REV-1): psx-spx's formula, literally, in
    // its documented order — SAME reflection, DIFF reflection (cross-fed),
    // 4-tap comb, two allpass stages, effect-volume output — with every
    // product SAR-15 floored and every buffer write saturated to int16.
    // Tap offsets are cursor-relative (src/dst relative to the current
    // buffer address, disp relative to src) modulo the work size; the
    // "-1" in a [mX-2] read is one sample back, since addresses count
    // 2-byte halfwords in psx-spx's presentation.
    void Spu2::stepReverbCore(ReverbState &state, int32_t wetL, int32_t wetR)
    {
        const ReverbParams &p = state.params;
        const auto cell = [&](const std::vector<int16_t> &buf, int32_t offset) -> int32_t {
            const int64_t n = static_cast<int64_t>(buf.size());
            int64_t index = (static_cast<int64_t>(state.cursor) + offset) % n;
            if (index < 0)
                index += n;
            return buf[static_cast<size_t>(index)];
        };
        const auto store = [&](std::vector<int16_t> &buf, int32_t offset, int32_t value) {
            const int64_t n = static_cast<int64_t>(buf.size());
            int64_t index = (static_cast<int64_t>(state.cursor) + offset) % n;
            if (index < 0)
                index += n;
            buf[static_cast<size_t>(index)] = clamp16(value);
        };

        const int32_t lin = floorDivBy32768(static_cast<int64_t>(p.vLin) * clamp16(wetL));
        const int32_t rin = floorDivBy32768(static_cast<int64_t>(p.vRin) * clamp16(wetR));

        // Same-side reflection: [mLSAME] = (Lin + [dLSAME]*vWALL -
        // [mLSAME-2])*vIIR + [mLSAME-2], and likewise for R.
        const int32_t lSamePrev = cell(state.bufL, p.mSameL - 1);
        const int32_t rSamePrev = cell(state.bufR, p.mSameR - 1);
        const int32_t lSame =
            floorDivBy32768((static_cast<int64_t>(lin) +
                             floorDivBy32768(static_cast<int64_t>(cell(state.bufL, p.dSameL)) * p.vWall) -
                             lSamePrev) *
                            p.vIir) +
            lSamePrev;
        const int32_t rSame =
            floorDivBy32768((static_cast<int64_t>(rin) +
                             floorDivBy32768(static_cast<int64_t>(cell(state.bufR, p.dSameR)) * p.vWall) -
                             rSamePrev) *
                            p.vIir) +
            rSamePrev;
        store(state.bufL, p.mSameL, lSame);
        store(state.bufR, p.mSameR, rSame);

        // Different-side reflection: L reads the R feedback tap, R reads
        // the L feedback tap.
        const int32_t lDiffPrev = cell(state.bufL, p.mDiffL - 1);
        const int32_t rDiffPrev = cell(state.bufR, p.mDiffR - 1);
        const int32_t lDiff =
            floorDivBy32768((static_cast<int64_t>(lin) +
                             floorDivBy32768(static_cast<int64_t>(cell(state.bufR, p.dDiffR)) * p.vWall) -
                             lDiffPrev) *
                            p.vIir) +
            lDiffPrev;
        const int32_t rDiff =
            floorDivBy32768((static_cast<int64_t>(rin) +
                             floorDivBy32768(static_cast<int64_t>(cell(state.bufL, p.dDiffL)) * p.vWall) -
                             rDiffPrev) *
                            p.vIir) +
            rDiffPrev;
        store(state.bufL, p.mDiffL, lDiff);
        store(state.bufR, p.mDiffR, rDiff);

        // Early echo: 4-tap comb over the reflection buffer.
        int32_t lOut = 0;
        int32_t rOut = 0;
        for (int i = 0; i < 4; ++i)
        {
            lOut += floorDivBy32768(static_cast<int64_t>(p.vComb[static_cast<size_t>(i)]) *
                                    cell(state.bufL, p.mCombL[static_cast<size_t>(i)]));
            rOut += floorDivBy32768(static_cast<int64_t>(p.vComb[static_cast<size_t>(i)]) *
                                    cell(state.bufR, p.mCombR[static_cast<size_t>(i)]));
        }

        // Late reverb: two allpass stages. Each stage subtracts the
        // delayed cell, writes the result, then adds the (possibly
        // just-written) delayed cell back — with dAPF 0 the second read
        // returns the cell just written, so a zero-volume stage passes
        // its input through unchanged.
        for (int stage = 0; stage < 2; ++stage)
        {
            const size_t s = static_cast<size_t>(stage);
            lOut -= floorDivBy32768(static_cast<int64_t>(p.vApf[s]) *
                                    cell(state.bufL, p.mApfL[s] - p.dApf[s]));
            store(state.bufL, p.mApfL[s], lOut);
            lOut = floorDivBy32768(static_cast<int64_t>(p.vApf[s]) * lOut) +
                   cell(state.bufL, p.mApfL[s] - p.dApf[s]);
            rOut -= floorDivBy32768(static_cast<int64_t>(p.vApf[s]) *
                                    cell(state.bufR, p.mApfR[s] - p.dApf[s]));
            store(state.bufR, p.mApfR[s], rOut);
            rOut = floorDivBy32768(static_cast<int64_t>(p.vApf[s]) * rOut) +
                   cell(state.bufR, p.mApfR[s] - p.dApf[s]);
        }

        state.heldL = floorDivBy32768(static_cast<int64_t>(lOut) * p.effectVolumeL);
        state.heldR = floorDivBy32768(static_cast<int64_t>(rOut) * p.effectVolumeR);
        state.cursor = (state.cursor + 1u) % static_cast<uint32_t>(state.bufL.size());
    }

    Spu2::MixTick Spu2::mixTick()
    {
        MixTick out{};
        for (int core = 0; core < 2; ++core)
        {
            Core &c = m_cores[static_cast<size_t>(core)];
            // Flag pairs at local offsets 0x180 (PMON), 0x184 (NON), 0x188
            // (VMIXL), 0x18C (VMIXEL), 0x190 (VMIXR), 0x194 (VMIXER).
            const auto flags = [&](uint32_t lowOffset) {
                return static_cast<uint32_t>(c.regs[lowOffset / 2u]) |
                       ((static_cast<uint32_t>(c.regs[lowOffset / 2u + 1u]) & 0xFFu) << 16);
            };
            const uint32_t pmon = flags(0x180u);
            const uint32_t non = flags(0x184u);
            const uint32_t vmixL = flags(0x188u);
            const uint32_t vmixEL = flags(0x18Cu);
            const uint32_t vmixR = flags(0x190u);
            const uint32_t vmixER = flags(0x194u);
            // OUTX of the previous voice this tick (0 for voice 0 / inactive).
            int32_t prevOutx = 0;
            for (int voice = 0; voice < 24; ++voice)
            {
                const size_t base = static_cast<size_t>(voice) * 8u;
                // No volume sweep (Phase 9): the current volumes track the set
                // volumes immediately, so VOLXL/VOLXR read back as VOLL/VOLR.
                c.regs[base + 6u] = c.regs[base + 0u];
                c.regs[base + 7u] = c.regs[base + 1u];
                const int32_t voll = static_cast<int16_t>(c.regs[base + 6u]);
                const int32_t volr = static_cast<int16_t>(c.regs[base + 7u]);
                int32_t outx = 0;
                if ((non & (1u << voice)) == 0u)
                {
                    uint32_t step = 0u;
                    const uint32_t pitch = c.regs[base + 2u];
                    if (voice > 0 && (pmon & (1u << voice)) != 0u)
                    {
                        // PMON (psx-spx pitch-counter formula, literally):
                        // Factor = OUTX(prev) + 0x8000, Step = (signExpand(PITCH)
                        // * Factor) SAR 15, masked to 16 bits, clipped to 0x4000
                        // when above 0x3FFF.
                        const int32_t signedPitch = static_cast<int16_t>(static_cast<uint16_t>(pitch));
                        const int32_t modulated =
                            floorDivBy32768(static_cast<int64_t>(signedPitch) *
                                            (static_cast<int64_t>(prevOutx) + 0x8000));
                        const uint32_t masked = static_cast<uint32_t>(modulated) & 0xFFFFu;
                        step = masked > 0x3FFFu ? 0x4000u : masked;
                    }
                    else
                    {
                        step = pitch > 0x3FFFu ? 0x4000u : pitch;
                    }
                    if (std::optional<int16_t> sample = nextSampleWithStep(core, voice, step))
                    {
                        // Envelope AFTER this tick's step (stepEnvelope mirrors
                        // it into the ENVX slot); a stopped voice returns
                        // nothing above and contributes nothing here.
                        const int32_t env = c.voices[static_cast<size_t>(voice)].adsrLevel;
                        outx = floorDivBy32768(static_cast<int64_t>(*sample) * env);
                        const int32_t effL = floorDivBy32768(static_cast<int64_t>(env) * voll);
                        const int32_t effR = floorDivBy32768(static_cast<int64_t>(env) * volr);
                        const int32_t contribL = floorDivBy32768(static_cast<int64_t>(*sample) * effL);
                        const int32_t contribR = floorDivBy32768(static_cast<int64_t>(*sample) * effR);
                        const uint32_t bit = 1u << voice;
                        if ((vmixL & bit) != 0u)
                            out.coreDryL[static_cast<size_t>(core)] += contribL;
                        if ((vmixR & bit) != 0u)
                            out.coreDryR[static_cast<size_t>(core)] += contribR;
                        if ((vmixEL & bit) != 0u)
                            out.coreWetL[static_cast<size_t>(core)] += contribL;
                        if ((vmixER & bit) != 0u)
                            out.coreWetR[static_cast<size_t>(core)] += contribR;
                    }
                }
                prevOutx = outx;
            }
        }
        // Phase 10b: MVOLXL/XR track MVOLL/R on each mix tick, like
        // VOLXL/VOLXR track VOLL/VOLR above (only once a guest has
        // written MVOL; unwritten MVOL stays at exact unity).
        for (int core = 0; core < 2; ++core)
        {
            Core &c = m_cores[static_cast<size_t>(core)];
            if (c.mvolWritten)
            {
                const size_t pBase = static_cast<size_t>(core) * 20u;
                m_shared[pBase + 8u] = m_shared[pBase + 0u];
                m_shared[pBase + 9u] = m_shared[pBase + 1u];
            }
        }
        // Phase 11 ADMA (D-ADMA-2): one stereo frame per mix tick per
        // enabled core, consumed from the core's input area in sound
        // RAM; an empty buffer contributes silence. ADMA is not fed
        // to reverb. Since Phase 10b the frame is scaled by BVOL
        // (D-REV-10b-6): unwritten BVOL is exact unity 0x8000, so
        // Phase 11 output reproduces bit-for-bit.
        for (int core = 0; core < 2; ++core)
        {
            AdmaState &adma = m_adma[static_cast<size_t>(core)];
            if (adma.enabled && adma.frames > 0u)
            {
                const Core &c = m_cores[static_cast<size_t>(core)];
                const int32_t bvolL = c.bvolWritten
                                          ? static_cast<int16_t>(sharedPReg(core, 0x0Cu))
                                          : 0x8000;
                const int32_t bvolR = c.bvolWritten
                                          ? static_cast<int16_t>(sharedPReg(core, 0x0Eu))
                                          : 0x8000;
                const uint32_t base = kAdmaAreaBase + static_cast<uint32_t>(core) * kAdmaAreaWords;
                const int16_t frameL = static_cast<int16_t>(m_ram[base + adma.readPos]);
                adma.readPos = (adma.readPos + 1u) % kAdmaAreaWords;
                const int16_t frameR = static_cast<int16_t>(m_ram[base + adma.readPos]);
                adma.readPos = (adma.readPos + 1u) % kAdmaAreaWords;
                --adma.frames;
                out.coreAdmaL[static_cast<size_t>(core)] =
                    floorDivBy32768(static_cast<int64_t>(frameL) * bvolL);
                out.coreAdmaR[static_cast<size_t>(core)] =
                    floorDivBy32768(static_cast<int64_t>(frameR) * bvolR);
            }
        }
        // Phase 10 reverb (D-REV-2): one reverb tick per two output
        // samples (24 kHz). The pair input is the mean of the two ticks'
        // wet buses; the output computed on a stepping tick is included
        // in that tick's final output and held through the next tick.
        // Pairing follows the mixTick count since reset, regardless of
        // when a core is enabled. A disabled core's held output is 0
        // (setReverbEnabled zeroes it), so it contributes nothing.
        for (int core = 0; core < 2; ++core)
        {
            ReverbState &rev = m_reverb[static_cast<size_t>(core)];
            // Phase 10b effective enable (D-REV-10b-5/3): model
            // enable AND the ATTR-bit-7 guest gate AND a valid work
            // window. A gated/invalid core does not step (buffers
            // and held output preserved, tail frozen) and contributes
            // 0 this tick; the pair phase still toggles below.
            const bool effective = rev.enabled && rev.guestEffectsEnabled && reverbWorkWindowValid(core);
            if (m_reverbSecondTick)
            {
                if (effective)
                {
                    const int32_t inL =
                        floorDivBy2(rev.prevWetL + out.coreWetL[static_cast<size_t>(core)]);
                    const int32_t inR =
                        floorDivBy2(rev.prevWetR + out.coreWetR[static_cast<size_t>(core)]);
                    stepReverbCore(rev, inL, inR);
                }
            }
            else
            {
                rev.prevWetL = out.coreWetL[static_cast<size_t>(core)];
                rev.prevWetR = out.coreWetR[static_cast<size_t>(core)];
            }
            out.coreReverbL[static_cast<size_t>(core)] = effective ? rev.heldL : 0;
            out.coreReverbR[static_cast<size_t>(core)] = effective ? rev.heldR : 0;
        }
        m_reverbSecondTick = !m_reverbSecondTick;
        // Clamping happens exactly once, at the final output (D-REV-7):
        // since Phase 10b, each core's dry + reverb + ADMA sum is
        // scaled by that core's MVOL (D-REV-10b-6; unwritten MVOL is
        // exact unity 0x8000, so Phase 9/10/11 values reproduce
        // exactly), the cores are summed, then clamped.
        int32_t sumL = 0;
        int32_t sumR = 0;
        for (int core = 0; core < 2; ++core)
        {
            const Core &c = m_cores[static_cast<size_t>(core)];
            const int32_t mvolL = c.mvolWritten ? static_cast<int16_t>(sharedPReg(core, 0x00u)) : 0x8000;
            const int32_t mvolR = c.mvolWritten ? static_cast<int16_t>(sharedPReg(core, 0x02u)) : 0x8000;
            sumL += floorDivBy32768(static_cast<int64_t>(out.coreDryL[static_cast<size_t>(core)] +
                                                          out.coreReverbL[static_cast<size_t>(core)] +
                                                          out.coreAdmaL[static_cast<size_t>(core)]) *
                                    mvolL);
            sumR += floorDivBy32768(static_cast<int64_t>(out.coreDryR[static_cast<size_t>(core)] +
                                                          out.coreReverbR[static_cast<size_t>(core)] +
                                                          out.coreAdmaR[static_cast<size_t>(core)]) *
                                    mvolR);
        }
        out.left = clamp16(sumL);
        out.right = clamp16(sumR);
        return out;
    }

    void Spu2::advanceCycles(uint64_t cycles)
    {
        const uint64_t total = m_sampleCycleRemainder + cycles;
        const uint64_t samples = total / kCyclesPerSample;
        m_sampleCycleRemainder = total % kCyclesPerSample;
        if (samples == 0u)
            return;
        // Phase 9: one mixed tick per sample (result discarded). This is the
        // same per-voice stepping as before for non-PMON voices, and it is what
        // makes PMON apply in guest time.
        for (uint64_t i = 0u; i < samples; ++i)
            (void)mixTick();
    }

    uint64_t Spu2::samplesPlayed(int core, int voice) const
    {
        if (core < 0 || core > 1 || voice < 0 || voice >= 24)
            return 0u;
        return m_cores[static_cast<size_t>(core)].voices[static_cast<size_t>(voice)].samplesPlayed;
    }

    Spu2::AdsrPhase Spu2::envelopePhase(int core, int voice) const
    {
        if (core < 0 || core > 1 || voice < 0 || voice >= 24)
            return AdsrPhase::Idle;
        return m_cores[static_cast<size_t>(core)].voices[static_cast<size_t>(voice)].adsrPhase;
    }

    uint16_t Spu2::envelopeLevel(int core, int voice) const
    {
        if (core < 0 || core > 1 || voice < 0 || voice >= 24)
            return 0u;
        return static_cast<uint16_t>(m_cores[static_cast<size_t>(core)].voices[static_cast<size_t>(voice)].adsrLevel);
    }

    // ADSR envelope, one step per sample (Phase 6; bitfields and rate table in iop_spu2.h).
    // ADSR1/ADSR2 are re-read from the register file on every step. The computed level is
    // mirrored into the voice's ENVX slot on every call, so a guest read and the internal
    // state cannot disagree. Returns true only when release has just reached zero.
    bool Spu2::stepEnvelope(Core &core, VoiceState &state, int voice)
    {
        const uint16_t adsr1 = core.regs[static_cast<size_t>(voice) * 8u + 3u];
        const uint16_t adsr2 = core.regs[static_cast<size_t>(voice) * 8u + 4u];
        bool stopped = false;
        switch (state.adsrPhase)
        {
        case AdsrPhase::Attack:
        {
            const int shift = (adsr1 >> 10) & 0x1Fu;
            const int stepField = (adsr1 >> 8) & 0x03u;
            if (stepEnvelopeRate(shift, 7 - stepField, (adsr1 & 0x8000u) != 0u, false, state.adsrLevel,
                                 state.adsrCounter, 0x7FFF, stepField == 3 && shift == 31) &&
                state.adsrLevel >= 0x7FFF)
            {
                state.adsrLevel = 0x7FFF;
                state.adsrPhase = sustainTarget(adsr1) >= 0x7FFF ? AdsrPhase::Sustain : AdsrPhase::Decay;
            }
            break;
        }
        case AdsrPhase::Decay:
        {
            const int shift = (adsr1 >> 4) & 0x0Fu;
            const int32_t target = sustainTarget(adsr1);
            if (stepEnvelopeRate(shift, 8, true, true, state.adsrLevel, state.adsrCounter, target, false) &&
                state.adsrLevel <= target)
            {
                state.adsrLevel = target;
                state.adsrPhase = AdsrPhase::Sustain;
            }
            break;
        }
        case AdsrPhase::Sustain:
        {
            const bool decreasing = (adsr2 & 0x4000u) != 0u;
            const int shift = (adsr2 >> 8) & 0x1Fu;
            const int stepField = (adsr2 >> 6) & 0x03u;
            const int magnitude = decreasing ? 8 - stepField : 7 - stepField;
            if (stepEnvelopeRate(shift, magnitude, (adsr2 & 0x8000u) != 0u, decreasing, state.adsrLevel,
                                 state.adsrCounter, decreasing ? 0 : 0x7FFF, stepField == 3 && shift == 31))
            {
                if (decreasing && state.adsrLevel <= 0)
                    state.adsrLevel = 0;
                else if (!decreasing && state.adsrLevel >= 0x7FFF)
                    state.adsrLevel = 0x7FFF;
            }
            break;
        }
        case AdsrPhase::Release:
        {
            const int shift = adsr2 & 0x1Fu;
            if (stepEnvelopeRate(shift, 8, (adsr2 & 0x0020u) != 0u, true, state.adsrLevel, state.adsrCounter, 0,
                                 shift == 31) &&
                state.adsrLevel <= 0)
            {
                state.adsrLevel = 0;
                state.adsrPhase = AdsrPhase::Idle;
                stopped = true;
            }
            break;
        }
        case AdsrPhase::Idle:
            break;
        }
        core.regs[static_cast<size_t>(voice) * 8u + 5u] = static_cast<uint16_t>(state.adsrLevel);
        return stopped;
    }

    uint16_t Spu2::peek16(uint32_t offset) const
    {
        offset &= kRegisterBytes - 2u;
        if (offset >= kSharedBase)
            return offset == kIrqInfo ? m_irqInfo : m_shared[(offset - kSharedBase) / 2u];
        const Core &core = m_cores[offset >= kCoreStride ? 1 : 0];
        const uint32_t local = offset % kCoreStride;
        switch (local)
        {
        case kKeyOn: return static_cast<uint16_t>(core.keyOn);
        case kKeyOn + 2u: return static_cast<uint16_t>(core.keyOn >> 16);
        case kKeyOff: return static_cast<uint16_t>(core.keyOff);
        case kKeyOff + 2u: return static_cast<uint16_t>(core.keyOff >> 16);
        case kDataPort: return m_ram[core.transfer];
        case kEndxHigh: return static_cast<uint16_t>(core.endx >> 16);
        case kEndxLow: return static_cast<uint16_t>(core.endx);
        case kStat: return core.stat;
        default: return core.regs[local / 2u];
        }
    }

    uint16_t Spu2::read16(uint32_t offset)
    {
        offset &= kRegisterBytes - 2u;
        const uint16_t value = peek16(offset);
        if (offset < kSharedBase && offset % kCoreStride == kDataPort)
        {
            Core &core = m_cores[offset >= kCoreStride ? 1 : 0];
            accessRam(core.transfer);
            core.transfer = (core.transfer + 1u) & (kRamHalfwords - 1u);
        }
        return value;
    }

    void Spu2::write16(uint32_t offset, uint16_t value)
    {
        offset &= kRegisterBytes - 2u;
        if (offset >= kSharedBase)
        {
            m_shared[(offset - kSharedBase) / 2u] = value; // IRQINFO reads the interrupt status, not this value
            // Phase 10b: the P-block (0x760 + core*40, regs to 0x26)
            // drives the reverb/volume model; other shared offsets
            // (SPDIF, IRQINFO) have no engine side effect here.
            if (offset < 0x7B0u)
            {
                const int pCore = static_cast<int>((offset - kSharedBase) / 40u);
                const uint32_t pReg = (offset - kSharedBase) % 40u;
                if (pReg <= 0x26u)
                    applyGuestPWrite(pCore, pReg);
            }
            return;
        }
        const int c = offset >= kCoreStride ? 1 : 0;
        Core &core = m_cores[c];
        const uint32_t local = offset % kCoreStride;
        switch (local)
        {
        case kAttr:
            if ((core.regs[kAttr / 2u] & kAttrIrqEnable) != 0u && (value & kAttrIrqEnable) == 0u)
                m_irqInfo &= static_cast<uint16_t>(~(4u << c));
            if ((value & kAttrReset) != 0u)
                core.stat &= static_cast<uint16_t>(~(kStatDmaReady | kStatDmaBusy));
            core.regs[kAttr / 2u] = static_cast<uint16_t>(value & ~kAttrReset);
            // Phase 10b (D-REV-10b-5): ATTR bit 7 (libsd
            // ENABLE_EFFECTS) gates this core's reverb output; it
            // preserves buffers, unlike setReverbEnabled(false).
            m_reverb[static_cast<size_t>(c)].guestEffectsEnabled = (value & 0x0080u) != 0u;
            return;
        case kKeyOn:
        case kKeyOn + 2u:
        {
            // KON: the voice restarts from SSA, so NAX takes SSA and its ENDX bit clears (PCSX2, DobieStation).
            const uint32_t voices = local == kKeyOn ? value : (static_cast<uint32_t>(value & 0xFFu) << 16);
            core.keyOn |= voices;
            core.endx &= ~voices;
            for (uint32_t v = 0; v < 24u; ++v)
            {
                if ((voices & (1u << v)) == 0u)
                    continue;
                const uint32_t base = (kVoiceAddresses + v * 0xCu) / 2u;
                core.regs[base + 4u] = core.regs[base];
                core.regs[base + 5u] = core.regs[base + 1u];
            }
            return;
        }
        case kKeyOff:
        case kKeyOff + 2u:
            core.keyOff |= local == kKeyOff ? value : (static_cast<uint32_t>(value & 0xFFu) << 16);
            return;
        case kTsaHigh:
        case kTsaLow:
            core.regs[local / 2u] = local == kTsaHigh ? static_cast<uint16_t>(value & 0xFu) : value;
            core.transfer = address20(core, kTsaHigh);
            return;
        case kDataPort:
            m_ram[core.transfer] = value;
            accessRam(core.transfer);
            core.transfer = (core.transfer + 1u) & (kRamHalfwords - 1u);
            return;
        case kEndxHigh: // as PCSX2 and DobieStation have it: each half's write clears the other half
            core.endx &= 0xFF0000u;
            return;
        case kEndxLow:
            core.endx &= 0xFFFFu;
            return;
        case kStat:
            return; // read-only
        case kIrqaHigh:
            core.regs[local / 2u] = static_cast<uint16_t>(value & 0xFu);
            return;
        default:
            break;
        }
        if (local >= kVoiceAddresses && local < kVoiceAddressEnd && ((local - kVoiceAddresses) % 0xCu) % 4u == 0u)
            value &= 0xFu; // SSA/LSAX/NAX high halves hold address bits 16-19
        if (local < 0x180u && (local % 0x10u) == 0x0Au)
        {
            // ENVX (voice register 5, Phase 6b): a guest write is sign-extended and adopted
            // as the internal level at once (psx-spx: manual-write range -8000h..+7FFFh,
            // writing lets the generator "jump" to a level; the next applied envelope step
            // recomputes from it). Phase 6 clamped the write to 0..0x7FFF, which turned a
            // written -1 (FFFFh) into full volume; that was wrong. The register slot keeps
            // the raw bits, so read-back shows the written value until the next step.
            core.voices[local / 0x10u].adsrLevel = static_cast<int16_t>(value);
            core.regs[local / 2u] = value;
            return;
        }
        core.regs[local / 2u] = value;
        // Phase 10b: the R-block (local 0x2E0-0x33F) drives the
        // reverb taps / work window; the pair base is the offset
        // rounded down to a multiple of 4, so a write to either
        // half of a pair recomputes that pair (D-REV-10b-2).
        if (local >= 0x2E0u && local < 0x340u)
            applyGuestRWrite(c, (local - 0x2E0u) & ~3u);
    }

    void Spu2::dmaWrite(int core, const uint16_t *data, size_t count)
    {
        // ADMA (Phase 11, D-ADMA-1/2): when the core's ADMA mode is
        // enabled, DMA data streams into the core's input buffer
        // instead of bulk-writing at TSA, and TSA does not advance.
        if (m_adma[core & 1].enabled)
        {
            admaWrite(core, data, count);
            return;
        }
        Core &c = m_cores[core & 1];
        for (size_t i = 0; i < count; ++i)
        {
            m_ram[c.transfer] = data[i];
            accessRam(c.transfer);
            c.transfer = (c.transfer + 1u) & (kRamHalfwords - 1u);
        }
    }

    void Spu2::setAdmaEnabled(int core, bool enabled)
    {
        if (core < 0 || core > 1)
            return;
        AdmaState &state = m_adma[static_cast<size_t>(core)];
        state.enabled = enabled;
        // Both transitions restart the stream deterministically: a
        // fresh enable starts at the area base with an empty buffer,
        // and a disable discards whatever is buffered (D-ADMA-2).
        state.readPos = 0u;
        state.writePos = 0u;
        state.frames = 0u;
        state.pendingHalf = false;
    }

    bool Spu2::admaEnabled(int core) const
    {
        if (core < 0 || core > 1)
            return false;
        return m_adma[static_cast<size_t>(core)].enabled;
    }

    void Spu2::admaWrite(int core, const uint16_t *data, size_t count)
    {
        if (core < 0 || core > 1 || data == nullptr)
            return;
        AdmaState &state = m_adma[static_cast<size_t>(core)];
        if (!state.enabled)
            return; // a feed into a disabled stream is dropped, not buffered
        const uint32_t base = kAdmaAreaBase + static_cast<uint32_t>(core) * kAdmaAreaWords;
        for (size_t i = 0; i < count; ++i)
        {
            // A full buffer drops the excess halfword (D-ADMA-2): it
            // never overwrites an unread frame. A pending L halfword
            // always has room for its R half, because the frame it
            // belongs to was counted as free when its L arrived.
            if (!state.pendingHalf && state.frames >= kAdmaFramesPerCore)
                continue;
            m_ram[base + state.writePos] = data[i];
            state.writePos = (state.writePos + 1u) % kAdmaAreaWords;
            if (state.pendingHalf)
            {
                state.pendingHalf = false;
                ++state.frames;
            }
            else
            {
                state.pendingHalf = true;
            }
        }
        // Deliberately no accessRam call here (D-ADMA-3): stream
        // writes raise no IRQA and set no STAT bit.
    }

    uint32_t Spu2::admaFramesAvailable(int core) const
    {
        if (core < 0 || core > 1)
            return 0u;
        return m_adma[static_cast<size_t>(core)].frames;
    }

    bool Spu2::admaRefillRequested(int core) const
    {
        if (core < 0 || core > 1)
            return false;
        const AdmaState &state = m_adma[static_cast<size_t>(core)];
        return state.enabled && state.frames < kAdmaHalfFrames;
    }

    void Spu2::dmaRead(int core, uint16_t *data, size_t count)
    {
        Core &c = m_cores[core & 1];
        for (size_t i = 0; i < count; ++i)
        {
            data[i] = m_ram[c.transfer];
            accessRam(c.transfer);
            c.transfer = (c.transfer + 1u) & (kRamHalfwords - 1u);
        }
    }

    void Spu2::completeDma(int core)
    {
        Core &c = m_cores[core & 1];
        c.stat = static_cast<uint16_t>((c.stat | kStatDmaReady) & ~kStatDmaBusy);
    }
}
