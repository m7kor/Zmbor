#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu_fmac.h"

#include <cmath>
#include <cstring>
#include <limits>

// FMAC ADD/SUB/MUL/MADD/MSUB/OPMULA/OPMSUB use ps2_vu_fmac, the contract VU0 macro mode uses too: exponent-gap rule
// (vu/games/triace), truncation, exponent 255 as a value. It does not depend on the interpreter's FE_TOWARDZERO.

namespace
{
    // Four FMAC lanes at once through ps2_vu_fmac's SSE2 kernels, the code VU0 macro mode runs. With a per-lane loop
    // over the scalar kernel, the MSVC 14.51 CI build wrote lane w's result into every lane (14.44 and GCC/Clang
    // builds were correct). The cause is not identified: review found no aliasing or UB, and the same loop shape in
    // isolation (tools/fpenv/lane_loop_probe.cpp) keeps its lanes on 14.51. The four-lane form is the shared lowering
    // anyway; the "lanes stay independent" unit test guards the observed pattern.
    inline __m128i lanes(const float *v) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(v)); }
    inline __m128i splat(float v) { return _mm_set1_epi32(std::bit_cast<int32_t>(v)); }
    inline void store(float *result, __m128i v) { _mm_storeu_si128(reinterpret_cast<__m128i *>(result), v); }

    int32_t vuFloatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }
}

// ============================================================================
// Upper instructions (FMAC pipeline)
// ============================================================================
void VU1Interpreter::execUpper(uint32_t instr)
{
    m_currentUpperInstruction = instr;
    uint8_t dest = DEST(instr);
    uint8_t ft = FT(instr);
    uint8_t fs = FS(instr);
    uint8_t fd = FD(instr);
    uint8_t op = instr & 0x3F;

    float *vd = m_state.vf[fd];
    float normalizedVs[4];
    float normalizedVt[4];
    float normalizedAcc[4];
    for (uint32_t component = 0; component < 4u; ++component)
    {
        normalizedVs[component] = normalizeOperand(m_state.vf[fs][component]);
        normalizedVt[component] = normalizeOperand(m_state.vf[ft][component]);
        normalizedAcc[component] = normalizeOperand(m_state.acc[component]);
    }
    const float *vs = normalizedVs;
    const float *vt = normalizedVt;
    const float *acc = normalizedAcc;
    const float q = normalizeOperand(m_state.q);
    const float i = normalizeOperand(m_state.i);
    // FMAC arithmetic reads the raw registers: ps2_vu_fmac treats exponent 0 as zero and exponent 255 as a value
    // (docs/architecture/evidence-register.md 2.1). The normalized copies above stay for MAX/MINI/ABS/conversions.
    const float *rvs = m_state.vf[fs];
    const float *rvt = m_state.vf[ft];
    const float *racc = m_state.acc;
    const float rq = m_state.q;
    const float ri = m_state.i;
    float result[4];

    // Upper opcode decoding (bits 5:0 of upper word)
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: // ADDbc
    {
        const float rbc = rvt[op & 3];
        store(result, ps2_vu_fmac::add4(lanes(rvs), splat(rbc)));
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: // SUBbc
    {
        const float rbc = rvt[op & 3];
        store(result, ps2_vu_fmac::sub4(lanes(rvs), splat(rbc)));
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B: // MADDbc
    {
        const float rbc = rvt[op & 3];
        store(result, ps2_vu_fmac::add4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(rbc))));
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: // MSUBbc
    {
        const float rbc = rvt[op & 3];
        store(result, ps2_vu_fmac::sub4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(rbc))));
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: // MAXbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: // MINIbc
    {
        float bc = broadcast(vt, op & 3);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: // MULbc
    {
        const float rbc = rvt[op & 3];
        store(result, ps2_vu_fmac::mul4(lanes(rvs), splat(rbc)));
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x1C: // MULq
        store(result, ps2_vu_fmac::mul4(lanes(rvs), splat(rq)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x1D: // MAXi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
    case 0x1E: // MULi
        store(result, ps2_vu_fmac::mul4(lanes(rvs), splat(ri)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x1F: // MINIi
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
    case 0x20: // ADDq
        store(result, ps2_vu_fmac::add4(lanes(rvs), splat(rq)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x21: // MADDq
        store(result, ps2_vu_fmac::add4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(rq))));
        applyFmacDest(vd, result, dest);
        return;
    case 0x22: // ADDi
        store(result, ps2_vu_fmac::add4(lanes(rvs), splat(ri)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x23: // MADDi
        store(result, ps2_vu_fmac::add4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(ri))));
        applyFmacDest(vd, result, dest);
        return;
    case 0x24: // SUBq
        store(result, ps2_vu_fmac::sub4(lanes(rvs), splat(rq)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x25: // MSUBq
        store(result, ps2_vu_fmac::sub4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(rq))));
        applyFmacDest(vd, result, dest);
        return;
    case 0x26: // SUBi
        store(result, ps2_vu_fmac::sub4(lanes(rvs), splat(ri)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x27: // MSUBi
        store(result, ps2_vu_fmac::sub4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(ri))));
        applyFmacDest(vd, result, dest);
        return;
    case 0x28: // ADD
        store(result, ps2_vu_fmac::add4(lanes(rvs), lanes(rvt)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x29: // MADD
        store(result, ps2_vu_fmac::add4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), lanes(rvt))));
        applyFmacDest(vd, result, dest);
        return;
    case 0x2A: // MUL
        store(result, ps2_vu_fmac::mul4(lanes(rvs), lanes(rvt)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x2B: // MAX
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;
    case 0x2C: // SUB
        store(result, ps2_vu_fmac::sub4(lanes(rvs), lanes(rvt)));
        applyFmacDest(vd, result, dest);
        return;
    case 0x2D: // MSUB
        store(result, ps2_vu_fmac::sub4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), lanes(rvt))));
        applyFmacDest(vd, result, dest);
        return;
    case 0x2E: // OPMSUB
        result[0] = ps2_vu_fmac::subf(racc[0], ps2_vu_fmac::mulf(rvs[1], rvt[2]));
        result[1] = ps2_vu_fmac::subf(racc[1], ps2_vu_fmac::mulf(rvs[2], rvt[0]));
        result[2] = ps2_vu_fmac::subf(racc[2], ps2_vu_fmac::mulf(rvs[0], rvt[1]));
        result[3] = 0.0f;
        applyFmacDest(vd, result, dest);
        return;
    case 0x2F: // MINI
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;

    // Upper special group (low op 0x3C..0x3F).
    // Like lower1 special, the real selector is not just bits 5:0.  Dobie decodes:
    //   op = (instr & 0x3) | ((instr >> 4) & 0x7C)
    // Several instructions in this group also use FT as the destination, not FD.
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    {
        const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        float *vtDest = m_state.vf[ft];

        switch (specialOp)
        {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03: // ADDAbc
        {
            const float rbc = rvt[specialOp & 3];
            store(result, ps2_vu_fmac::add4(lanes(rvs), splat(rbc)));
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07: // SUBAbc
        {
            const float rbc = rvt[specialOp & 3];
            store(result, ps2_vu_fmac::sub4(lanes(rvs), splat(rbc)));
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: // MADDAbc
        {
            const float rbc = rvt[specialOp & 3];
            store(result, ps2_vu_fmac::add4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(rbc))));
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F: // MSUBAbc
        {
            const float rbc = rvt[specialOp & 3];
            store(result, ps2_vu_fmac::sub4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(rbc))));
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x10: // ITOF0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x11: // ITOF4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 16.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x12: // ITOF12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 4096.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x13: // ITOF15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 32768.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x14: // FTOI0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 1.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x15: // FTOI4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 16.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x16: // FTOI12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 4096.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x17: // FTOI15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 32768.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B: // MULAbc
        {
            const float rbc = rvt[specialOp & 3];
            store(result, ps2_vu_fmac::mul4(lanes(rvs), splat(rbc)));
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x1C: // MULAq
            store(result, ps2_vu_fmac::mul4(lanes(rvs), splat(rq)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x1D: // ABS
            for (int c = 0; c < 4; c++)
                result[c] = std::fabs(vs[c]);
            applyDest(vtDest, result, dest);
            return;
        case 0x1E: // MULAi
            store(result, ps2_vu_fmac::mul4(lanes(rvs), splat(ri)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x1F: // CLIP
        {
            uint32_t wBits = 0u;
            std::memcpy(&wBits, &m_state.vf[ft][3], sizeof(wBits));
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;

            const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                bits ^= signMask;
                int32_t orderedBits = 0;
                std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
                return orderedBits > limit;
            };

            uint32_t flags = 0u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x00000000u))
                flags |= 0x01u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x80000000u))
                flags |= 0x02u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x00000000u))
                flags |= 0x04u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x80000000u))
                flags |= 0x08u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x00000000u))
                flags |= 0x10u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x80000000u))
                flags |= 0x20u;
            queueClip(flags);
            return;
        }
        case 0x20: // ADDAq
            store(result, ps2_vu_fmac::add4(lanes(rvs), splat(rq)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x21: // MADDAq
            store(result, ps2_vu_fmac::add4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(rq))));
            applyFmacDestAcc(result, dest);
            return;
        case 0x22: // ADDAi
            store(result, ps2_vu_fmac::add4(lanes(rvs), splat(ri)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x23: // MADDAi
            store(result, ps2_vu_fmac::add4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(ri))));
            applyFmacDestAcc(result, dest);
            return;
        case 0x24: // SUBAq
            store(result, ps2_vu_fmac::sub4(lanes(rvs), splat(rq)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x25: // MSUBAq
            store(result, ps2_vu_fmac::sub4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(rq))));
            applyFmacDestAcc(result, dest);
            return;
        case 0x26: // SUBAi
            store(result, ps2_vu_fmac::sub4(lanes(rvs), splat(ri)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x27: // MSUBAi
            store(result, ps2_vu_fmac::sub4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), splat(ri))));
            applyFmacDestAcc(result, dest);
            return;
        case 0x28: // ADDA
            store(result, ps2_vu_fmac::add4(lanes(rvs), lanes(rvt)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x29: // MADDA
            store(result, ps2_vu_fmac::add4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), lanes(rvt))));
            applyFmacDestAcc(result, dest);
            return;
        case 0x2A: // MULA
            store(result, ps2_vu_fmac::mul4(lanes(rvs), lanes(rvt)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x2C: // SUBA
            store(result, ps2_vu_fmac::sub4(lanes(rvs), lanes(rvt)));
            applyFmacDestAcc(result, dest);
            return;
        case 0x2D: // MSUBA
            store(result, ps2_vu_fmac::sub4(lanes(racc), ps2_vu_fmac::mul4(lanes(rvs), lanes(rvt))));
            applyFmacDestAcc(result, dest);
            return;
        case 0x2E: // OPMULA
            result[0] = ps2_vu_fmac::mulf(rvs[1], rvt[2]);
            result[1] = ps2_vu_fmac::mulf(rvs[2], rvt[0]);
            result[2] = ps2_vu_fmac::mulf(rvs[0], rvt[1]);
            result[3] = 0.0f;
            applyFmacDestAcc(result, dest);
            return;
        case 0x2F:
        case 0x30: // NOP
            return;
        default:
            reportReservedInstruction(true, instr);
            return;
        }
    }

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    default:
        reportReservedInstruction(true, instr);
        return;
    }
}
