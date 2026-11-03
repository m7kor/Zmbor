#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2 register file and sound RAM (IOP 0x1F900000-0x1F9007FF): core 0 at offset 0x000, core 1 at 0x400, and the
    // core volume / SPDIF block at 0x760-0x7FF. Registers are 16 bits wide.
    //
    // Per core: 24 voices of 8 registers (VOLL, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLXL, VOLXR) at 0x000-0x17F; PMON,
    // NON, VMIXL, VMIXEL, VMIXR, VMIXER (pairs of halves), MMIX, ATTR, IRQA, KON, KOFF, TSA, the data port and ADMA at
    // 0x180-0x1B0; 24 voices of SSA, LSAX, NAX (high half: 4 address bits, low half: 16) at 0x1C0-0x2DF; ESA, the
    // reverb registers and EEA at 0x2E0-0x33F; ENDX at 0x340/0x342; STAT at 0x344.
    //
    // Modelled here: read-back of every register; the 20-bit halfword addresses; the data port (writes and reads at
    // TSA, which advances and wraps in the 2 MB RAM); IOP DMA channels 4 and 7 through the same transfer address;
    // IRQA (with ATTR bit 6, any RAM access by either core at a core's IRQA sets IRQINFO bit 2 + core and raises IOP
    // INTC line 9; clearing ATTR bit 6 clears the core's IRQINFO bit); KON (clears the voice's ENDX bit and loads NAX
    // from SSA) and the ENDX write rule; STAT bit 7 after a DMA transfer, cleared by ATTR bit 15.
    //
    // Modelled in addition to the above: decoding of one 16-byte SPU2 ADPCM block from sound RAM (see
    // decodeAdpcmBlockAt). The block header is the first halfword: bits 0-3 are the shift, bits 4-6 the filter,
    // bit 8 the loop-end flag, bit 9 the loop flag and bit 10 the loop-start flag; the remaining 14 bytes hold
    // 28 four-bit samples, low nibble first. The prediction coefficients and the shift/filter clamping follow the
    // project's existing VAG decoder in ps2xRuntime (ps2_audio_vag.cpp) so the two decoders agree; that rounding
    // is not claimed as hardware-verified.
    //
    // Modelled in addition to the above: per-voice block progression (see stepVoiceBlock). One step consumes a
    // pending KON (restarting the voice from SSA with cleared ADPCM history), decodes the block at NAX, moves
    // NAX to the next block, records a loop-start block in LSAX, and on a loop-end block sets ENDX and either
    // jumps to LSAX (loop flag set) or stops the voice (loop flag clear). This is block-level only: it does not
    // step samples, so pitch, Gaussian interpolation, ADSR and mixing are still separate stages, and KOFF stays
    // latched for the ADSR stage instead of stopping a voice here.
    //
    // ADMA is modelled (Phase 11, see setAdmaEnabled/admaWrite and the
    // D-ADMA decisions below): the sound-data input stream, fed by IOP
    // DMA and consumed one stereo frame per 48 kHz mix tick, mixed at
    // unity into the core output. It is NOT a voice feeder: ADMA data
    // never enters a voice's ADPCM block stream (see D-ADMA-2).
    // Reverb is modelled (Phase 10, see ReverbParams
    // and stepReverbCore): the documented psx-spx reflection/comb/allpass
    // structure, stepped at 24 kHz, fed by the Phase 9 wet buses, with
    // model-level parameters. Since Phase 10b (see D-REV-10b) the guest
    // P-block and R-block registers drive those parameters (addresses
    // source-pinned from ps2sdk spu2regs.h), while the work area stays
    // internal, never sound RAM, so reverb cannot corrupt voice sample
    // data. Mixing is modelled (Phase 9, see mixTick):
    // per-voice volumes, dry/wet routing and core outputs, and PMON pitch modulation.
    // There is still no audio sink: mixTick returns the mixed sample; nothing plays it.
    // Pitch is modelled (Phase 7, see nextSample): the PITCH register steps a 12-bit
    // fractional sample counter and the output is 4-point Gaussian interpolated (below).
    // ADSR is modelled (Phase 6, see stepEnvelope): one envelope step per 48 kHz sample on the
    // sample-clock path (nextSample/advanceSamples/advanceCycles), driven by the voice's ADSR1/ADSR2
    // registers, re-read on every step so a guest write takes effect on the next sample.
    //
    // ADSR decisions (Phase 6, D-ADSR-1/2/3; the tests pin them so a contradicting source falsifies
    // them by design):
    //  1. Bitfields, from psx-spx's SPU Volume/ADSR section (the PS1 SPU whose layout the SPU2
    //     inherits as two cores; the field names match the project's requirements record spu.adsr
    //     exactly). Direct PCSX2 ADSR.cpp / DobieStation spu_envelope.cpp text was not retrieved in
    //     this session (search surfaced only secondary summaries), so no claim is made that those
    //     files were diffed line by line. ADSR1: bit 15 attack mode (0 linear, 1 exponential),
    //     bits 14-10 attack shift, bits 9-8 attack step (+7,+6,+5,+4), bits 7-4 decay shift,
    //     bits 3-0 sustain level, level = (N+1)*0x800 clamped to 0x7FFF. ADSR2: bit 15 sustain mode,
    //     bit 14 sustain direction (0 increase, 1 decrease), bit 13 unused, bits 12-8 sustain shift,
    //     bits 7-6 sustain step (+7..+4 increase, -8..-5 decrease), bit 5 release mode,
    //     bits 4-0 release shift. Decay is always exponential, release step and decay step are
    //     always -8 in magnitude. ENVX range is 0..0x7FFF in normal playback; a manual
    //     guest write may set -8000h..+7FFFh (Phase 6b, see decision 3).
    //  2. Release end-state: when release reaches zero the voice STOPS (voiceActive false, the
    //     buffered block is discarded, no further sample is returned). Release does NOT set ENDX;
    //     ENDX is set only by a loop-end ADPCM block, as before. This is a model choice, not a
    //     hardware claim: psx-spx notes release "ends at 0" and is checked via ENVX for idleness.
    //  3. ENVX read-back: the computed level is written into the stored ENVX register slot
    //     (voice register 5) on every envelope step, so peek16/read16 return it with no special
    //     case, and a guest write to ENVX is sign-extended (Phase 6b: Phase 6 clamped it to
    //     0..0x7FFF, turning a written -1 into full volume) and adopted at once as the
    //     internal level (so internal state and the register can never disagree; the next
    //     applied step recomputes from it, as psx-spx describes). An increasing phase counts
    //     up from a written negative level within -8000h..+7FFFh; a decreasing phase resolves
    //     it to 0 (or the decay target) on its next applied step. A guest write changes
    //     neither the phase nor the rate counter. KON restarts the envelope in attack at level 0 and clears a
    //     pending KOFF for that voice (KON wins); a pending KOFF otherwise moves attack/decay/
    //     sustain to release and is consumed on the sample-clock path, so the latched KOFF bits
    //     from earlier phases are finally used. KOFF on an idle voice is discarded.
    // Rate table (psx-spx "Envelope Operation"): step magnitude = (7 - step) for an increasing
    // phase, (8 - step) for a decreasing sustain phase, 8 for decay/release, scaled left by
    // max(0, 11 - shift); a phase counter adds 0x8000 >> max(0, shift - 11) per sample and steps
    // when it reaches 0x8000, so shift >= 11 steps every 2^(shift-11) samples. Exponential
    // increase above 0x6000 slows by /4 (shift < 10: step, shift > 10: counter, shift == 10:
    // both /2); exponential decrease scales the step by level/0x8000, computed as an
    // arithmetic shift (SAR 15, Phase 6b), not a truncating division.
    // Phase 6b cross-reference review (PCSX2 ADSR.cpp + defs.h read in full, against
    // psx-spx; PCSX2 was NOT copied blindly, and no claim here is hardware-confirmed):
    // (a) counter floor is 1 for every rate except the all-ones rate (attack/sustain
    // step 3 + shift 31, release shift 31), which never steps — this is psx-spx's explicit
    // rule; PCSX2's V_ADSR instead floors unconditionally and doubts itself ("maybe not
    // correct for the 'infinite' settings"), while its V_VolumeSlide implements the
    // exception, so psx-spx wins and the Phase 6 behaviour is kept. Decay shift is only
    // 4 bits, so decay can never be all-ones; its neverStep=false is correct, not an
    // omission. (b) The counter is reduced by 0x8000 on a step, not reset to zero as in
    // PCSX2. With constant rates the two are indistinguishable (increments are powers of
    // two, so the residue at step time is always 0; verified over 2000 samples): they
    // diverge only when the increment changes mid-accumulation — in practice a guest
    // ADSR rewrite, since the exponential-attack threshold crossing cannot overshoot —
    // and a step then strictly overshoots 0x8000. That case is pinned by a test (Case F)
    // and kept as a documented decision; psx-spx's bit-test pseudocode specifies no
    // clearing at all, so neither form is demonstrably wrong. (c) An exponential-decrease
    // step that would be 0 above its target is forced to -1; after the SAR-15 fix this
    // is inert for level > target (a nonzero product floors to <= -1) and is kept only
    // as a guard. (d) Decay landing snaps to the exact sustain target; PCSX2 keeps the
    // undershoot (1 LSB in the shift-11 case). No reference settles this; the snap keeps
    // the documented sustain level exact and is pinned by existing tests. (e) Sustain
    // level 15 goes straight from attack to sustain (zero-length decay); PCSX2 spends
    // one decay sample there. Both converge within one sample; kept. (f) Shift > 26:
    // shifts 26..31 already behave identically here (increment floor 1, same magnitude),
    // verified shift 29 vs 26 over 40000 samples, which is consistent with psx-spx's
    // 0x76-behaves-like-0x6A observation; the exact hardware mechanism ("dependent on
    // Bit15=1") is NOT modelled and remains an explicitly unknown mechanism, not a claim.
    class Spu2
    {
    public:
        // One decoded ADPCM block: 28 PCM samples plus the header fields the voice engine will need.
        struct AdpcmBlock
        {
            std::array<int16_t, 28> samples{};
            uint8_t flags = 0;  // bit 0 loop-end, bit 1 loop, bit 2 loop-start (header bits 8, 9, 10)
            uint8_t shift = 0;
            uint8_t filter = 0;
        };

        // Pure block decode, with no sound-RAM or interrupt side effects. `previous` is the previous decoded
        // sample (s1) and `previousPrevious` the one before it (s2); both are updated to the last two samples
        // of the decoded block, so a caller decodes successive blocks by passing the same variables through.
        // `block` is 16 bytes in sound-RAM byte order: header, flags, then 14 data bytes.
        static AdpcmBlock decodeAdpcmBlock(const uint8_t block[16], int16_t &previous,
                                           int16_t &previousPrevious);
        static constexpr uint32_t kRamHalfwords = 0x100000u; // 2 MB
        static constexpr uint32_t kRegisterBytes = 0x800u;

        Spu2();
        void reset();

        // `offset` is the byte offset from 0x1F900000 (even, below 0x800).
        uint16_t read16(uint32_t offset);
        void write16(uint32_t offset, uint16_t value);
        // The register value without side effects (no data-port advance).
        [[nodiscard]] uint16_t peek16(uint32_t offset) const;

        // IOP DMA (channel 4: core 0, channel 7: core 1): halfwords at the core's transfer address, which advances.
        void dmaWrite(int core, const uint16_t *data, size_t count);
        void dmaRead(int core, uint16_t *data, size_t count);
        void completeDma(int core);

        // Decode the ADPCM block at `halfwordAddress` in sound RAM (wrapping in the 2 MB RAM, so a block may
        // straddle the end of RAM). Each of the block's eight halfwords is checked against both cores' IRQA,
        // exactly like a data-port or DMA access, so a voice fetch at IRQA leaves a pending interrupt for
        // IopMemory to poll (see takeInterrupt). This does not advance any voice's NAX: block progression is
        // the voice engine's job and is not modelled yet.
        AdpcmBlock decodeAdpcmBlockAt(uint32_t halfwordAddress, int16_t &previous,
                                      int16_t &previousPrevious);

        // Advance one voice by one ADPCM block and return the decoded block, or nothing if the voice is
        // neither active nor newly keyed on. A pending KON (re)starts the voice: its bit is consumed, ADPCM
        // history resets, and the block at NAX (loaded from SSA by the KON write) is decoded. Loop-start sets
        // LSAX to the block just decoded; loop-end sets ENDX, then jumps to LSAX if the loop flag is set and
        // stops the voice otherwise (NAX still advances to the next block in the stopping case, which is this
        // model's choice, not a hardware claim). Out-of-range core/voice returns nothing.
        //
        // INTEGRATION GAP, partially closed in Phase 3 (see nextSample/advanceCycles): stepVoiceBlock
        // itself is still a manual primitive, but IopMemory::advanceSpu2Cycles (called by the IOP
        // scheduler) now drives voices in guest time, and a voice fetch's IRQA hit is polled there too.
        // What is still NOT complete: there is no
        // audio output sink yet — samples are mixed and counted, not played. ADSR (Phase 6)
        // steps on the sample-clock path only; a manual stepVoiceBlock call resets the envelope on
        // KON but does not step it per sample. Pitch stepping and Gaussian interpolation
        // (Phase 7) live in nextSample, not here: this primitive still decodes whole blocks.
        std::optional<AdpcmBlock> stepVoiceBlock(int core, int voice);
        [[nodiscard]] bool voiceActive(int core, int voice) const;

        // Sample clock (Phase 3, pitch in Phase 7). One ADPCM block is 28 source samples;
        // nextSample() returns one OUTPUT sample per 48 kHz tick. The voice's PITCH register
        // (voice register 2, re-read on every tick, like ADSR1/ADSR2) steps a fractional
        // source position: step = PITCH, clipped to 0x4000 when PITCH > 0x3FFF (psx-spx
        // "SPU ADPCM Pitch"), so 0x1000 = 48 kHz = 1.0x, 0x0800 = 0.5x, 0x2000 = 2.0x.
        // PITCH = 0 stops the voice (psx-spx "0=stop"): nextSample returns nothing, KON
        // stays pending and the envelope is frozen until a nonzero pitch is written, so a
        // voice that was never given a pitch does not play — guests must write PITCH, as
        // real games do through libsd. The output is 4-point Gaussian interpolated from
        // the four most recent source samples (oldest, older, old, new) with the
        // 512-entry psx-spx table below and bits 4..11 of the fractional counter as the
        // 8-bit interpolation index, each product shifted right by 15 before summing
        // (psx-spx's formula, literally). Gaussian interpolation is always on, even at
        // pitch 1.0: the output at 1.0 is NOT the raw decoded sample (the Phase 3 raw
        // values were the pre-interpolation provisional), it is the table-weighted sum,
        // and the first outputs after KON are a transient from the zeroed history.
        // Source samples are consumed in order across block boundaries, so the history
        // is continuous through blocks and loop jumps; a loop-end block still sets ENDX
        // and jumps to LSAX when its block is fetched, whatever the pitch. The ADSR
        // envelope steps once per returned output sample, not per source sample (the
        // requirements record times ADSR per 48 kHz tick; pitch stretches the source,
        // not the envelope). PMON is NOT applied by this per-voice call: pitch is the
        // register value alone here. PMON is a mixer-level effect (Phase 9, see
        // mixTick), because it needs the previous voice's output from the same tick;
        // evaluating a voice in isolation cannot supply that. Returns nothing for an
        // inactive, unkeyed voice, for a pitch-0 voice, and after the last sample of a
        // non-looping voice (its final block is still played). Do not mix this with
        // manual stepVoiceBlock() calls on the same voice: the sample clock owns block
        // stepping for voices it drives, and a manual step would advance past the
        // buffered block.
        std::optional<int16_t> nextSample(int core, int voice);
        // Advance one voice by `count` samples, discarding the samples (no audio sink yet).
        // This is O(count) sample steps; block fetches happen only at block boundaries.
        void advanceSamples(int core, int voice, uint64_t count);
        // Advance all voices by IOP cycles: 36,864,000 Hz / 48,000 Hz = 768 cycles per sample,
        // exactly, with the sub-sample remainder carried between calls. This is the guest-time
        // hook the IOP scheduler calls; it is deterministic in total IOP cycles.
        // Since Phase 9 it advances through mixTick (one mixed tick per sample), so
        // PMON applies in guest time too; per-voice totals are unchanged for
        // non-PMON voices, and the mixed result is discarded here.
        void advanceCycles(uint64_t cycles);

        // One mixed output sample (Phase 9). Steps every voice of both cores once,
        // in voice order 0..23 within each core, and sums the routed contributions.
        // This is the canonical sample path: nextSample evaluates one voice in
        // isolation (register pitch, no PMON); mixTick is where voices interact.
        //
        // Mixing decisions (Phase 9, D-MIX-1/2/3; the tests pin them so a
        // contradicting source falsifies them by design):
        //  1. Mix structure, from psx-spx's SPU volume section and iris's SPU2
        //     sample path (secondary, allkern/iris ps2_spu2_get_sample): per core,
        //     VMIXL/VMIXR bits route a voice's contribution to the core dry bus and
        //     VMIXEL/VMIXER bits to the core wet bus; since Phase 10 the wet
        //     bus feeds that core's reverb unit (see D-REV below), whose output
        //     is added to the final stereo output scaled by the model-level
        //     effect volume. The final stereo output is the clamped sum of the
        //     two cores' dry buses plus their reverb outputs (iris: final =
        //     CORE0 + CORE1, extended here by the reverb sum).
        //     MMIX is stored and read back like any core register but is NOT
        //     applied to the sample path yet: its hardware-observable effect is
        //     core 0's output feeding core 1's effect path (plus external and
        //     sound-data inputs, neither of which exists in this model), and
        //     the MMIX effect-path bit semantics are not pinned by any
        //     retrieved source, so applying it to the dry sum would
        //     double-count core 0 against the iris final-sum rule; it stays
        //     deferred (see D-REV-4).
        //     MVOL/EVOL/AVOL/BVOL offsets are source-pinned since
        //     Phase 10b (ps2sdk spu2regs.h, see D-REV-10b-1/6): MVOL
        //     scales the per-core final sum, EVOL the effect volume,
        //     BVOL the ADMA feed; AVOL feeds nothing yet.
        //     scope: VOLXL/VOLXR (the current volumes) track VOLL/VOLR
        //     immediately on every mix tick, so a guest write to VOLXL/VOLXR
        //     directly is overwritten on the next tick, as psx-spx notes the
        //     hardware overwrites these internal registers.
        //  2. PMON ordering: voices are evaluated in order 0..23, and voice v's
        //     pitch step (when PMON bit v is set and v > 0) uses voice v-1's
        //     output from THIS tick, computed by psx-spx's pitch-counter formula:
        //     Factor = OUTX(v-1) + 0x8000, Step = (signExpand(PITCH) * Factor)
        //     SAR 15, masked to 16 bits, then clipped to 0x4000 when above
        //     0x3FFF. OUTX is the previous voice's envelope-scaled output,
        //     (interpolated sample * ENVX) >> 15, before voice volume: psx-spx's
        //     PMON example sets the modulator's Left/Right volume to 0 and still
        //     gets modulation, which only makes sense pre-volume, and it cites
        //     the modulator's ADSR volume as the amplitude control. An inactive
        //     previous voice (or voice 0, whose PMON bit is unused per psx-spx)
        //     contributes OUTX 0, i.e. Factor 0x8000, i.e. the register pitch
        //     unchanged. The envelope steps once per returned output sample, as
        //     before; PMON changes only the pitch step, never the envelope rate.
        //     A PMON step of 0 behaves like pitch 0: no sample, envelope frozen.
        //  3. Volume order and clamping: per voice, effective volume first,
        //     effL = (ENVX * VOLXL) >> 15 (psx-spx's final-volume formula,
        //     arithmetic shift, signed so negative volumes phase-invert), then
        //     contribution = (interpolated sample * effL) >> 15, again arithmetic.
        //     ENVX here is the level AFTER this tick's ADSR step (the step runs
        //     before the contribution is computed, so the mix always uses the
        //     current tick's envelope). Buses accumulate in int32 with NO
        //     clamping at the bus stage (24 voices cannot overflow int32);
        //     clamping to int16 happens exactly once, at the final stereo
        //     output. This differs from iris only in rounding: iris applies
        //     voice volume to an already envelope-scaled sample, while this
        //     model follows psx-spx's two-stage formula literally; the two agree
        //     up to the per-stage floor. NON (noise) voices are NOT modelled:
        //     a NON voice is muted in the mix (no step, no contribution, OUTX 0)
        //     rather than playing ADPCM samples through a noise path that does
        //     not exist, which would be fake noise; the noise generator itself
        //     (ATTR shift/step, LFSR) stays a later milestone.
        // Reverb decisions (Phase 10, D-REV-1..8; the tests pin them so a
        // contradicting source falsifies them by design):
        //  1. Structure, from psx-spx's SPU Reverb Formula (the PS1 SPU
        //     formula, applied here as the documented structure — no SPU2
        //     reverb formula has been retrieved, and this is a labelled
        //     model, not a hardware claim): same-side reflection
        //     [mLSAME] = (Lin + [dLSAME]*vWALL - [mLSAME-2])*vIIR +
        //     [mLSAME-2] (and R likewise), different-side reflection
        //     (L reads the R tap, R reads the L tap), early echo as a
        //     4-tap comb Lout = sum vCOMBi*[mLCOMBi], late reverb as two
        //     allpass stages Lout = Lout - vAPF*[mLAPF-dAPF]; [mLAPF] =
        //     Lout; Lout = Lout*vAPF + [mLAPF-dAPF], output scaled by the
        //     effect volume. Every product is divided by 0x8000 as an
        //     arithmetic shift (SAR 15, floor), exactly like the Phase 9
        //     volume order; values written to the delay buffer are
        //     saturated to -8000h..+7FFFh, as psx-spx states.
        //  2. Rate and stepping: the requirements record times SPU2 reverb
        //     at 24 kHz (half the 48 kHz output rate; psx-spx presents the
        //     PS1 unit at 22050 Hz, half of 44100 Hz — same structure, the
        //     SPU2 rate is used here). mixTick steps reverb once per two
        //     output samples: the input is the mean of the pair's wet buses
        //     (SAR-1 floor), a documented stand-in for the 39-tap FIR
        //     downsampler psx-spx lists, which is NOT modelled (its
        //     coefficients are recorded in the ledger for a later
        //     milestone). The output computed on a stepping tick is
        //     included in that tick's final output and held through the
        //     next tick. Single-voice nextSample bypasses reverb, exactly
        //     as it bypasses PMON and mixing: reverb lives on the mixTick
        //     path (and therefore in guest time via advanceCycles).
        //  3. Work area and addressing: each core has its own internal
        //     circular delay buffer (NOT sound RAM — see D-REV-8: guest
        //     ESA/EEA offsets are unpinned, so no guest work area can be
        //     honoured yet). The cursor advances one sample per reverb
        //     tick and wraps within the work area (psx-spx's BufferAddress
        //     advance rule, adapted from bytes to samples). Tap offsets
        //     are cursor-relative (psx-spx: src/dst relative to the
        //     current buffer address, disp relative to src), taken modulo
        //     the work size, so any parameter value is safe. Consequence:
        //     reverb provably cannot corrupt voice sample data, because it
        //     never touches m_ram; guest-visible work-area writes are
        //     future work once ESA/EEA are pinned.
        //  4. Volumes and feeds: vLIN/vRIN, vIIR, vWALL, vCOMB, vAPF and
        //     the effect volume are MODEL parameters (ReverbParams),
        //     driven since Phase 10b by the guest P-block
        //     coefficients (see D-REV-10b-7) and EVOL. Defaults:
        //     vLIN/vRIN 0x7FFF (near unity; avoids psx-spx's -8000h
        //     negation quirk, which is not modelled), effect volume
        //     0x7FFF (near unity). Reverb input is the wet bus only:
        //     sound-data input is the Phase 11 ADMA feed (BVOL-
        //     scaled since Phase 10b, but NOT fed to reverb) and
        //     external input (AVOL) still feeds nothing, and the
        //     MMIX effect-path feed stays deferred because its bit
        //     semantics are not pinned by any retrieved source.
        //  5. Default preset: vIIR 0x4000, vCOMB1 0x4000, all other
        //     volumes 0, APF passthrough (vAPF 0, dAPF 0 — with vAPF 0 an
        //     APF stage passes its input through its own just-written
        //     cell, exactly as psx-spx's disable note describes). The
        //     PS1 game presets (Room, Studio, Hall, ...) are NOT
        //     transcribed: their register values are absolute PS1 buffer
        //     positions that depend on PS1 buffer geometry and do not
        //     transfer to this model without pinned ESA/EEA handling.
        //  6. Lifecycle: reverb state is per core, independent of voices.
        //     KON/KOFF/voice stop do NOT touch it: a stopped voice simply
        //     stops feeding the wet bus, and the tail decays naturally
        //     (psx-spx notes a voice's reverb feed ends with the voice;
        //     the buffer tail is not cut). Spu2::reset restores the
        //     defaults (disabled, zeroed buffers, default params).
        //     Disabling a core zeroes its held output at once, so a
        //     disabled core contributes exactly 0. The ATTR
        //     reverb-write-enable bit is unpinned on SPU2 (psx-spx
        //     documents PS1 ATTR bit 7 only), so it is NOT modelled:
        //     an enabled core always writes its internal buffers.
        //  7. Clamping: buses accumulate in int32 with no clamping at the
        //     bus stage; the reverb output is added to the dry sums and
        //     clamping to int16 happens exactly once, at the final stereo
        //     output, as in Phase 9. Buffer writes saturate (see D-REV-1).
        //  8. Register pinning, Step 1 result (Phase 10 status, now
        //     SUPERSEDED by Phase 10b below — kept for the record): the
        //     SPU2 Overview v6.0 (Sony, April 2002) register list was
        //     retrieved in full and pins the EXISTENCE and semantics of
        //     MVOLL/MVOLR, EVOLL/EVOLR, AVOLL/AVOLR, BVOLL/BVOLR,
        //     MVOLXL/MVOLXR, ESAH/ESAL and EEAH, but it contains NO IOP
        //     addresses anywhere, so it pins no offsets. psx-spx gives
        //     PS1 addresses only, which do not transfer to the SPU2
        //     map. Iris corroborates the dual-core layout and the
        //     final-sum rule but gives no offsets. PCSX2/DobieStation/
        //     ps2sdk source text was NOT retrieved in Phase 10, so NO
        //     guest register was wired to reverb or master volumes
        //     then: wiring one from a guessed offset would be the
        //     pseudo-hardware error Phase 9 refused to make.
        // Phase 10b guest-register integration (D-REV-10b; the tests pin
        // these, so a contradicting source falsifies them by design).
        // Source: ps2sdk spu2regs.h (read in full from an anonymous
        // ps2sdk clone, iop/sound/libsd/include/spu2regs.h), cross-
        // checked against the repo's existing layout (voice/S/A/VA
        // blocks, ENDX 0x340, STAT 0x344 all agree) and libsd
        // effect.c (SetEffectRegister/SetEffectData/sceSdSetEffectAttr)
        // and freesd.c (init sequences). Offsets are from 0x1F900000;
        // ps2sdk's 0xBF900000 is the uncached alias, same offsets.
        //  1. Pinned map. P-block at 0x760 + core*40 + reg: MVOLL/R
        //     0x00/0x02, EVOLL/R 0x04/0x06, AVOLL/R 0x08/0x0A, BVOLL/R
        //     0x0C/0x0E, MVOLXL/R 0x10/0x12, IIR_ALPHA 0x14,
        //     ACC_COEF_A/B/C/D 0x16/0x18/0x1A/0x1C, IIR_COEF 0x1E,
        //     FB_ALPHA 0x20, FB_X 0x22, IN_COEF_L/R 0x24/0x26.
        //     R-block at 0x2E0 + (core<<10) + reg: ESA_HI/LO 0x00/0x02,
        //     then 22 pair parameters stepping 4 bytes each from
        //     0x04 (FB_SRC_A, FB_SRC_B, IIR_DEST_A0/A1, ACC_SRC_A0/A1/
        //     B0/B1, IIR_SRC_A0/A1, IIR_DEST_B0/B1, ACC_SRC_C0/C1/D0/
        //     D1, IIR_SRC_B1/B0, MIX_DEST_A0/A1/B0/B1) to 0x58,
        //     EEA_HI/LO 0x5C/0x5E. ATTR bits from libsd-common.h:
        //     SPU2_ON bit 15, ENABLE_EFFECTS bit 7 (this CORRECTS
        //     Phase 10's "ATTR bit 7 unpinned on SPU2" note).
        //     Addresses are source-pinned; behaviours and timing
        //     below are model decisions, NOT hardware-confirmed.
        //  2. Pair packing (libsd SetEffectRegister, literally):
        //     a pair value V is stored as hi = V >> 14 at the base
        //     halfword and lo = V << 2 (truncated to 16 bits) at the
        //     next halfword, and reconstructed as (hi << 14) |
        //     (lo >> 2). P-block coefficients are single halfwords.
        //     A guest write to one half of a pair updates that half
        //     of the stored value; the engine field follows the
        //     recomputed pair. Read-back is always the stored halves,
        //     so a pair round-trips exactly.
        //  3. ESA/EEA and the work window. ESA byte address =
        //     (ESA_HI << 17) | (ESA_LO << 1) (libsd SetESA);
        //     EEA end = (EEA_HI << 17) | 0x1FFFF (libsd GetEEA:
        //     128 KB-aligned end). The Phase 10 internal buffers stay
        //     the engine: ESA/EEA anchor only a logical window used
        //     for validity, and reverb writes stay internal — they
        //     NEVER become guest-visible RAM writes, so no existing
        //     output changes and voice data cannot be corrupted.
        //     Window validity: ESA < 2 MB and ESA <= EEA (with EEA
        //     truncated to the RAM top 0x1FFFFF for the comparison).
        //     An invalid window mutes that core's reverb output
        //     (held output and buffers preserved, stepping frozen)
        //     and restoring validity resumes the tail; it never
        //     crashes and never touches RAM. Consequences recorded
        //     explicitly: libsd's default EEA_HI 0xF for core 1 gives
        //     EEA 0x1FFFFF — exactly the RAM top, NOT past RAM (an
        //     earlier concern that it overflowed was mistaken);
        //     only EEA_HI >= 0x10 exceeds RAM, and that truncates.
        //     A window overlapping voice data is harmless because
        //     buffers are internal (tested: voice RAM unchanged).
        //     Whether delay lines physically live in sound RAM is
        //     NOT settled by this: libsd clears the work area via
        //     VoiceTrans, which is library behaviour, not a hardware
        //     probe, so it is recorded as an open question.
        //  4. Mid-pair / reprogramming semantics (the sharpest rule).
        //     libsd sceSdSetEffectAttr disables effects (ATTR bit 7
        //     off), writes EVOL + params + ESA, re-enables, and
        //     clears work-area RAM ONLY when the CLEAR flag is set.
        //     Therefore: a guest coefficient/EVOL/R-pair write updates
        //     exactly its engine field and PRESERVES buffers, held
        //     output and pair state, including across a 24 kHz pair
        //     boundary (held output survives the write); an ESA/EEA
        //     re-anchor moves only the logical window and likewise
        //     preserves the tail (the RAM-defined tail of a
        //     RAM-resident engine is future work if buffers ever
        //     migrate to RAM, and is NOT claimed here). The old
        //     setReverbParams behaviour (zero buffers + held) is now
        //     resetReverb only. ATTR bit 7 off gates output but
        //     preserves buffers; disabling via setReverbEnabled
        //     still zeroes held output at once (Phase 10 behaviour,
        //     kept). The 24 kHz pair phase itself is never reset by
        //     a guest write.
        //  5. ATTR gating. Effective reverb for a core =
        //     modelEnabled AND guestGate AND windowValid, where
        //     guestGate starts true (libsd init writes ATTR with
        //     ENABLE_EFFECTS set, so a fresh guest is gated on) and
        //     a guest ATTR write sets it to bit 7. With effects
        //     gated off the core contributes dry only.
        //  6. Volumes. MVOL scales the final sum per core (Overview
        //     §2.6 final volume): final = clamp(sum over cores of
        //     ((dry + reverb + ADMA) * MVOL) >> 15, per side, with
        //     MVOLXL/XR tracking MVOLL/R on each mix tick like
        //     VOLXL/VOLXR track VOLL/VOLR. Until a guest writes
        //     MVOL, the effective master is exactly 0x8000 (unity
        //     in >> 15 arithmetic), so unwritten behaviour is
        //     bit-identical to Phase 9/10/11; a guest MVOL of 0
        //     mutes, 0x4000 halves (floor). This is a model choice:
        //     libsd Reset writes MVOL 0 during init and games then
        //     set it explicitly, so the model cannot distinguish
        //     "init wrote 0" from "guest muted" — power-on is treated
        //     as unity, a guest 0 write as mute. EVOL drives the
        //     effect volume directly (guest write replaces the model
        //     value; unwritten EVOL leaves the Phase 10 default
        //     0x7FFF in place). BVOL scales the ADMA feed: ADMA
        //     contribution = (frame * BVOL) >> 15, unwritten BVOL
        //     = exact unity 0x8000, so Phase 11 ADMA output is
        //     bit-identical until a guest writes BVOL. AVOL feeds
        //     nothing: external input has no source in this model
        //     (freesd notes core 1's external input is core 0's
        //     output, which is the deferred MMIX effect-path feed,
        //     not a standalone input), so AVOL is stored/read back
        //     only and its default is silence.
        //  7. Coefficient/tap mapping (MODEL mapping, not a hardware
        //     claim — derived from psx-spx structure + libsd names,
        //     pinned by tests as chosen). P-block: IN_COEF_L/R ->
        //     vLin/vRin, IIR_COEF -> vIir, FB_ALPHA -> vWall,
        //     ACC_COEF_A..D -> vComb[0..3]. IIR_ALPHA and FB_X are
        //     stored and read back but drive no Phase 10 field:
        //     the single-multiplier reflection stage does not
        //     distinguish an IIR alpha from an IIR coefficient, and
        //     the DIFF path reuses vWall, so their hardware roles
        //     are recorded as ambiguous, not guessed into the
        //     engine. R-block taps: FB_SRC_A/B -> dSameL/R,
        //     IIR_DEST_A0/B0 -> mSameL/R, IIR_DEST_A1/B1 ->
        //     mDiffL/R, ACC_SRC_A0/A1/B0/B1 -> mCombL[0..3],
        //     ACC_SRC_C0/C1/D0/D1 -> mCombR[0..3], IIR_SRC_A0/B0 ->
        //     dDiffL/R, MIX_DEST_A0/A1 -> mApfL[0..1],
        //     MIX_DEST_B0/B1 -> mApfR[0..1]. IIR_SRC_A1/B1 (the
        //     second IIR sources) are stored only. A tap takes the
        //     low 16 bits of the reconstructed pair value, signed;
        //     the engine already takes taps modulo the work size,
        //     so any guest value is safe. A guest write to a P/R
        //     register updates exactly its mapped field (per-
        //     register sync): writing only EVOL does NOT zero the
        //     other coefficients, because libsd always writes the
        //     full set and a partial writer must not clobber it.
        //  8. Open questions, NOT guessed: the unnamed 0x1AE/0x1B0
        //     entries in libsd's ParamRegList (libsd's own comment:
        //     "both related to core attr & dma somehow"; block.c
        //     writes 0x1AE in BlockTransRead and 0x1B0 for DMA
        //     block transfers) are left untouched — Phase 11's
        //     ADMA model seam is unchanged. The ATTR bit 14 MUTE
        //     and bit 15 SPU2_ON semantics are untouched (pre-
        //     existing ATTR handling, including the bit-15 STAT-
        //     clear rule, is out of this milestone's scope).
        struct ReverbParams
        {
            int16_t vLin = 0x7FFF;  // reverb input volume, left (model-level vLIN)
            int16_t vRin = 0x7FFF;  // reverb input volume, right (model-level vRIN)
            int16_t vIir = 0x4000;  // reflection volume 1 (model-level vIIR)
            int16_t vWall = 0;      // reflection volume 2 (model-level vWALL)
            std::array<int16_t, 4> vComb{0x4000, 0, 0, 0}; // early-echo volumes
            std::array<int16_t, 2> vApf{0, 0};            // late-reverb volumes
            std::array<int16_t, 2> dApf{0, 0};            // allpass offsets
            int16_t mSameL = 0;     // same-side reflection write tap, left
            int16_t mSameR = 0;     // same-side reflection write tap, right
            int16_t dSameL = 4;     // same-side feedback read tap, left
            int16_t dSameR = 4;     // same-side feedback read tap, right
            int16_t mDiffL = 32;    // different-side write tap, left
            int16_t mDiffR = 32;    // different-side write tap, right
            int16_t dDiffL = 36;    // different-side feedback read tap, left
            int16_t dDiffR = 36;    // different-side feedback read tap, right
            std::array<int16_t, 4> mCombL{0, 8, 16, 24}; // comb read taps, left
            std::array<int16_t, 4> mCombR{0, 8, 16, 24}; // comb read taps, right
            std::array<int16_t, 2> mApfL{48, 56};        // allpass write taps, left
            std::array<int16_t, 2> mApfR{48, 56};        // allpass write taps, right
            int16_t effectVolumeL = 0x7FFF; // model-level EVOL equivalent, left
            int16_t effectVolumeR = 0x7FFF; // model-level EVOL equivalent, right
            uint32_t workSizeSamples = 256u; // circular delay-buffer size, samples
        };
        static ReverbParams defaultReverbParams() { return ReverbParams{}; }
        // Model-level reverb control. Since Phase 10b, setting params
        // PRESERVES the core's delay buffers, held output and pair
        // state (see D-REV-10b-4: a parameter rewrite must not cut the
        // tail); resetReverb is the explicit clearing path (the old
        // setReverbParams behaviour) and is what Spu2::reset uses.
        // Enabling/disabling never touches the params. Disabling
        // zeroes the held output at once. Out-of-range core is ignored.
        void setReverbParams(int core, const ReverbParams &params);
        void resetReverb(int core);
        void setReverbEnabled(int core, bool enabled);
        [[nodiscard]] bool reverbEnabled(int core) const;
        // Guest-visible reverb state (Phase 10b, D-REV-10b): ESA/EEA
        // byte addresses decoded from the stored R-block halves, and
        // whether the resulting work window is usable (see
        // D-REV-10b-3). guestEffectsEnabled is the ATTR-bit-7 gate
        // (true until a guest ATTR write says otherwise).
        [[nodiscard]] uint32_t reverbEsaAddress(int core) const;
        [[nodiscard]] uint32_t reverbEeaAddress(int core) const;
        [[nodiscard]] bool reverbWorkWindowValid(int core) const;
        [[nodiscard]] bool guestEffectsEnabled(int core) const;
        // Current engine parameters for a core (model defaults until
        // a guest register or setReverbParams changes a field). This
        // is the observable that pins pair packing: the high half of
        // a pair vanishes modulo the 256-sample work buffer, so no
        // engine-output test can distinguish a wrong pair shift —
        // the mapped tap value itself must be asserted.
        [[nodiscard]] const ReverbParams &reverbParams(int core) const;

        // ADMA, sound-data input (Phase 11). Decisions (D-ADMA-1/2/3; the
        // tests pin them so a contradicting source falsifies them by design):
        //  1. Enable/trigger, from the SPU2 Overview v6.0 (Sony, April 2002)
        //     §1.1.2/§2.2 (sound-data input, double-buffered, "requests a
        //     data transfer when half of the area is processed") and iris
        //     (secondary, allkern/iris SPU2 + IOP DMA pages): ADMA is a
        //     per-core mode selected by an ADMAS bit (iris: core 0 bit 0,
        //     core 1 bit 1 of the core's admas value); when set, IOP DMA
        //     channel 4 (core 0) / 7 (core 1) data is streamed into the
        //     core's input buffer instead of bulk-written at TSA, and the
        //     SPU2 consumes it as it plays, rather than all at once.
        //     The ADMA guest register offset is NOT pinned by any
        //     retrieved source (the Overview's register list, as
        //     retrieved, contains no IOP addresses; iris gives no
        //     offset; PCSX2 SPU2 source text was not retrieved), so NO
        //     guest register enables ADMA here: setAdmaEnabled is the
        //     model-level seam, exactly like setReverbEnabled in Phase 10.
        //     The in-repo register map names ADMA in the 0x180-0x1B0
        //     block, but that name alone pins no offset, and wiring the
        //     next free halfword (0x1AE, after the data port) would be
        //     the guessed-offset error Phase 9/10 refused to make.
        //  2. Data path: ADMA is the Overview's sound-data INPUT stream
        //     (stereo PCM from the host), NOT a voice feeder. This
        //     corrects the looser "ADMA feeds voice ADPCM blocks"
        //     framing: the Overview's input area is separate from the
        //     waveform data area, §2.2 mixes input data with voice
        //     output, and iris mixes a per-core ADMA sample into the
        //     core totals alongside voices. So: DMA halfwords feed a
        //     per-core stereo stream (L first, then R — interleave
        //     order is a model choice, unstated in the retrieved
        //     Overview text); mixTick consumes exactly one stereo
        //     frame per 48 kHz tick per enabled core, regardless of
        //     voice pitch (input data has no pitch transformation in
        //     §2.2); voice streams, loop-end/ENDX and ADSR are wholly
        //     unaffected by ADMA. The stream lives in real sound RAM
        //     in the Overview-pinned input area (short-word addresses
        //     0x2000-0x27FF, §1.2.1): core 0 at 0x2000-0x23FF, core 1
        //     at 0x2400-0x27FF — that per-core split of the fixed
        //     area is a model choice, since the Overview fixes only
        //     the total area. Each core's area is a double buffer of
        //     two 0x200-word halves (256 stereo frames each, 512
        //     frames per core). Feed writes sequentially and wraps
        //     within the core's area; consumption reads sequentially
        //     and wraps likewise. A feed beyond capacity drops the
        //     excess (it never overwrites unread frames and never
        //     touches RAM outside the core's area); an odd trailing
        //     halfword is held pending until the next feed completes
        //     the frame. Enabling (re)starts the stream at the area
        //     base with an empty buffer; disabling clears the buffer,
        //     so re-enabling starts fresh. Both are model choices
        //     where the retrieved sources are silent. On underrun
        //     (empty buffer) the core contributes silence (0, 0) for
        //     that tick — no invented noise, no voice disturbance.
        //     ADMA is input-only: dmaRead is unchanged, and a manual
        //     data-port write stays a manual TSA transfer (the DMA
        //     engine's dmaWrite is the ADMA sink, matching iris's
        //     DATA-register routing for DMA data).
        //  3. Completion/signalling: ADMA adds NO signal of its own.
        //     An ADMA feed sets no STAT bit and raises no interrupt:
        //     stream writes bypass accessRam, so they cannot hit IRQA
        //     (whether hardware input-area writes could trigger IRQA
        //     is not in any retrieved source and is left as an open
        //     model choice, not a claim). The IOP DMA engine's
        //     completeDma still sets STAT bit 7 after a transfer
        //     exactly as for bulk DMA — that is the transfer's
        //     completion, not an ADMA event, so nothing is
        //     double-signalled: one transfer, one ready bit, zero
        //     ADMA interrupts. Refill need is level-based, not an
        //     interrupt: admaRefillRequested is true while the
        //     enabled core holds fewer than one half-buffer (256
        //     frames) — the model-level reading of the Overview's
        //     "requests a data transfer when half of the area is
        //     processed". ADMA samples are scaled by BVOL since
        //     Phase 10b (unwritten BVOL = exact unity, so Phase 11
        //     output is bit-identical) and then by that core's MVOL
        //     as part of the final sum, and are NOT fed to reverb
        //     (reverb input is the voice wet bus only, Phase 10);
        //     clamping still happens exactly once, at the final
        //     output.
        void setAdmaEnabled(int core, bool enabled);
        [[nodiscard]] bool admaEnabled(int core) const;
        // DMA-side ADMA feed (halfwords, L/R interleaved). When ADMA is
        // disabled for the core this drops the data. dmaWrite routes
        // here automatically when ADMA is enabled for that core.
        void admaWrite(int core, const uint16_t *data, size_t count);
        [[nodiscard]] uint32_t admaFramesAvailable(int core) const;
        [[nodiscard]] bool admaRefillRequested(int core) const;

        struct MixTick
        {
            std::array<int32_t, 2> coreDryL{}; // per-core dry sums, unclamped
            std::array<int32_t, 2> coreDryR{};
            std::array<int32_t, 2> coreWetL{}; // per-core wet (reverb-send) sums, unclamped
            std::array<int32_t, 2> coreWetR{};
            std::array<int32_t, 2> coreReverbL{}; // per-core reverb outputs (post effect volume), unclamped
            std::array<int32_t, 2> coreReverbR{};
            std::array<int32_t, 2> coreAdmaL{}; // per-core ADMA contributions this tick (post-BVOL), unclamped
            std::array<int32_t, 2> coreAdmaR{};
            int16_t left = 0;  // clamped per-core MVOL-scaled sums of dry + reverb + ADMA (Phase 10b)
            int16_t right = 0; // (unwritten MVOL = exact unity, so Phase 9/10/11 values reproduce exactly)
        };
        MixTick mixTick();
        // ADSR envelope phase (Phase 6). Idle means no envelope is running: either the voice was
        // never keyed, or release has reached zero and stopped the voice.
        enum class AdsrPhase
        {
            Idle,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        [[nodiscard]] AdsrPhase envelopePhase(int core, int voice) const;
        [[nodiscard]] uint16_t envelopeLevel(int core, int voice) const;
        [[nodiscard]] uint64_t samplesPlayed(int core, int voice) const;
        static constexpr uint64_t kCyclesPerSample = 768u; // 36,864,000 / 48,000

        // 4-point Gaussian interpolation table (Phase 7), transcribed from psx-spx's
        // "SPU ADPCM Pitch" section (the PSX table, which psx-spx notes differs from the
        // SNES table and is "slightly bugged": each quadruple
        // (gauss[i], gauss[0xFF-i], gauss[0x100+i], gauss[0x1FF-i]) sums to
        // 0x7F7F..0x7F81, not 0x8000 — the tests assert that range for all 256
        // quadruples, which guards the transcription). Iris (allkern/iris, secondary)
        // corroborates the shape: a 512-entry table, a 16.12 fixed-point counter and a
        // 4-sample history, with 0x1000 = 48 kHz. This is a model of the documented
        // table, not a hardware capture: no PS2-side Gaussian table has been
        // hardware-verified for Zmbor, and whether SPU2's table matches the PS1 table
        // exactly is an open item, not a claim.
        static constexpr std::array<int16_t, 512> kGaussTable = {
            -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1,
            0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
            0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003,
            0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007,
            0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
            0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018,
            0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025,
            0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038,
            0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050,
            0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F,
            0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096,
            0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7,
            0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101,
            0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148,
            0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C,
            0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200,
            0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
            0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9,
            0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392,
            0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441,
            0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506,
            0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4,
            0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC,
            0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF,
            0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E,
            0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C,
            0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8,
            0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63,
            0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F,
            0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB,
            0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7,
            0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4,
            0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700,
            0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B,
            0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
            0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37,
            0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4,
            0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389,
            0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653,
            0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E,
            0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
            0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D,
            0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209,
            0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509,
            0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807,
            0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00,
            0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
            0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0,
            0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C,
            0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651,
            0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9,
            0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F,
            0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
            0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7,
            0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0,
            0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397,
            0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529,
            0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684,
            0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
            0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886,
            0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A,
            0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F,
            0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3,
        };

        // True once per IRQA hit (IOP INTC line 9).
        bool takeInterrupt() noexcept
        {
            const bool pending = m_interrupt;
            m_interrupt = false;
            return pending;
        }

        [[nodiscard]] uint16_t ram(uint32_t address) const { return m_ram[address & (kRamHalfwords - 1u)]; }
        [[nodiscard]] uint32_t transferAddress(int core) const { return m_cores[core & 1].transfer; }
        [[nodiscard]] uint32_t pendingKeyOn(int core) const { return m_cores[core & 1].keyOn; }
        [[nodiscard]] uint32_t pendingKeyOff(int core) const { return m_cores[core & 1].keyOff; }

    private:
        struct VoiceState
        {
            bool active = false;
            int16_t previous = 0;         // ADPCM history s1, reset on KON
            int16_t previousPrevious = 0; // ADPCM history s2, reset on KON
            AdpcmBlock currentBlock{};    // buffered block for the sample clock
            uint32_t sampleIndex = 28u;   // next source sample in currentBlock; 28 means empty
            std::array<int16_t, 4> interp{}; // Gaussian history: oldest, older, old, new (Phase 7)
            uint32_t pitchFraction = 0u;  // fractional part of the pitch counter, < 0x1000
            uint64_t samplesPlayed = 0;   // deterministic guest-time accounting
            AdsrPhase adsrPhase = AdsrPhase::Idle; // envelope phase (Phase 6)
            int32_t adsrLevel = 0;        // envelope level, normally 0..0x7FFF, mirrored in ENVX; a manual ENVX write may set -0x8000..0x7FFF (Phase 6b)
            uint32_t adsrCounter = 0;     // envelope rate counter, < 0x8000
        };

        struct Core
        {
            std::array<uint16_t, 0x200> regs{}; // as written, by halfword index
            std::array<VoiceState, 24> voices{};
            uint32_t transfer = 0;              // data-port / DMA address (halfwords)
            uint32_t keyOn = 0;
            uint32_t keyOff = 0;
            uint32_t endx = 0;
            uint16_t stat = 0;
            bool mvolWritten = false; // a guest MVOL(L/R) write has set the master (Phase 10b)
            bool bvolWritten = false; // a guest BVOL(L/R) write has set the ADMA input volume (Phase 10b)
        };

        // One core's reverb unit (Phase 10, D-REV-3/6). The delay buffers
        // are internal circular buffers in samples, never sound RAM.
        struct ReverbState
        {
            ReverbParams params{};
            bool enabled = false;
            bool guestEffectsEnabled = true; // ATTR bit 7 gate (Phase 10b): true until a guest ATTR write clears it
            std::vector<int16_t> bufL;
            std::vector<int16_t> bufR;
            uint32_t cursor = 0;   // current buffer address, < work size
            int32_t prevWetL = 0;  // previous mixTick's wet bus (pair input)
            int32_t prevWetR = 0;
            int32_t heldL = 0;     // most recent reverb output (post effect volume)
            int32_t heldR = 0;
            ReverbState()
            {
                const uint32_t n = params.workSizeSamples == 0u ? 1u : params.workSizeSamples;
                bufL.assign(n, 0);
                bufR.assign(n, 0);
            }
        };

        // One core's ADMA stream state (Phase 11, D-ADMA-2/3). Positions
        // are word offsets within the core's 0x400-word input area
        // (absolute RAM address = 0x2000 + core * 0x400 + position);
        // `frames` counts complete stereo frames buffered, which is
        // what disambiguates full from empty when the pointers meet.
        struct AdmaState
        {
            bool enabled = false;
            uint32_t readPos = 0;  // next word to consume, < 0x400
            uint32_t writePos = 0; // next word to fill, < 0x400
            uint32_t frames = 0;   // complete stereo frames buffered, <= 512
            bool pendingHalf = false; // an odd L halfword awaits its R half (the L value itself is already in RAM)
        };
        static constexpr uint32_t kAdmaAreaBase = 0x2000u; // Overview §1.2.1 input area, short-word units
        static constexpr uint32_t kAdmaAreaWords = 0x400u; // per core (model split of the 0x800-word area)
        static constexpr uint32_t kAdmaFramesPerCore = 512u;
        static constexpr uint32_t kAdmaHalfFrames = 256u;

        void accessRam(uint32_t address);
        [[nodiscard]] uint32_t address20(const Core &core, uint32_t highReg) const;
        // nextSample with the pitch step supplied by the caller (Phase 9): the
        // public nextSample reads and clips the PITCH register, mixTick supplies
        // the PMON-modulated step instead. Body is the former nextSample.
        std::optional<int16_t> nextSampleWithStep(int core, int voice, uint32_t step);
        // One ADSR step for a voice on the sample-clock path. Returns true when release has
        // just reached zero, i.e. the voice stops on this sample (decision D-ADSR-2).
        bool stepEnvelope(Core &core, VoiceState &state, int voice);
        // One reverb tick for a core (Phase 10, D-REV-1): the psx-spx formula
        // applied to the pair-averaged wet input, updating the core's held
        // output. Callers pass already-saturated int16-range inputs; the
        // method saturates again defensively, since the wet bus is int32.
        void stepReverbCore(ReverbState &state, int32_t wetL, int32_t wetR);
        // Guest P-block / R-block write handlers (Phase 10b, D-REV-10b):
        // `pReg` is the P-block register offset (0x00..0x26), `rReg`
        // the R-block register offset (0x00..0x5E). Both read the
        // stored halves and update exactly the mapped engine field
        // (or, for MVOL/BVOL, the written flag), preserving buffers,
        // held output and pair state. Out-of-range core ignored.
        void applyGuestPWrite(int core, uint32_t pReg);
        void applyGuestRWrite(int core, uint32_t rReg);
        // The P-block halfword value for a core, from shared storage.
        [[nodiscard]] uint16_t sharedPReg(int core, uint32_t pReg) const;

        std::array<Core, 2> m_cores{};
        std::array<AdmaState, 2> m_adma{};
        std::array<ReverbState, 2> m_reverb{};
        bool m_reverbSecondTick = false; // pair phase for the 24 kHz reverb step
        std::array<uint16_t, 0x50> m_shared{}; // 0x760-0x7FF
        std::vector<uint16_t> m_ram;
        uint16_t m_irqInfo = 0;
        bool m_interrupt = false;
        uint64_t m_sampleCycleRemainder = 0; // < kCyclesPerSample, carried between advanceCycles calls
    };
}
