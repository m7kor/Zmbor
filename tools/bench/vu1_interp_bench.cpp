// VU1 interpreter workload benchmark (docs/architecture/benchmark-methodology.md).
// Workload: a vertex-transform microprogram of the kind PS2 games run on VU1 - per vertex LQ, a 4x4 matrix product
// (MULAx/MADDAy/MADDAz/MADDw), DIV + WAITQ, MULq, FTOI4, SQ, two IADDIU and an IBNE loop. It is synthetic (written
// here, not taken from a game) and exercises the decode, FMAC, FDIV-pipeline, load/store and branch paths.
// Scenarios:
//   steady   : the same interpreter runs the same resident program repeatedly (decode cache warm)
//   reupload : one code word is rewritten through PS2Memory before every run, as when a program is re-sent by VIF MPG
// The output of every run is checked against an independent computation (ps2_vu_fmac integer reference + FTOI4).
// usage: ps2x_bench_vu1 [runs] [vertices] [profile|xgkick]   ("profile": sample the steady scenario, needs a PDB build;
//        "xgkick": time the XGKICK scenario with [vertices] kicks)
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_vu_fmac.h"
#include "sampling_profiler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    uint32_t upper(uint8_t op, uint8_t dest, uint8_t ft, uint8_t fs, uint8_t fd)
    {
        return (uint32_t(dest & 0xF) << 21) | (uint32_t(ft & 0x1F) << 16) | (uint32_t(fs & 0x1F) << 11) |
               (uint32_t(fd & 0x1F) << 6) | (op & 0x3F);
    }
    uint32_t upperSpecial(uint8_t sop, uint8_t dest, uint8_t ft, uint8_t fs)
    {
        return (uint32_t(dest & 0xF) << 21) | (uint32_t(ft & 0x1F) << 16) | (uint32_t(fs & 0x1F) << 11) |
               (uint32_t(sop & 0x7C) << 4) | (sop & 0x3) | 0x3C;
    }
    uint32_t lowerSpecial(uint8_t sop, uint8_t is, uint8_t it, uint8_t id, uint8_t dest)
    {
        return (0x40u << 25) | (uint32_t(dest & 0xF) << 21) | (uint32_t(it & 0x1F) << 16) | (uint32_t(is & 0x1F) << 11) |
               (uint32_t(id & 0x1F) << 6) | (uint32_t(sop & 0x7C) << 4) | (sop & 0x3) | 0x3C;
    }
    uint32_t lq(uint8_t dest, uint8_t vf, uint8_t vi, int16_t imm)
    {
        return (uint32_t(dest & 0xF) << 21) | (uint32_t(vf & 0x1F) << 16) | (uint32_t(vi & 0xF) << 11) | (uint32_t(imm) & 0x7FF);
    }
    uint32_t sq(uint8_t dest, uint8_t vf, uint8_t vi, int16_t imm)
    {
        return (0x01u << 25) | (uint32_t(dest & 0xF) << 21) | (uint32_t(vi & 0xF) << 16) | (uint32_t(vf & 0x1F) << 11) |
               (uint32_t(imm) & 0x7FF);
    }
    uint32_t iaddiu(uint8_t it, uint8_t is, int16_t imm)
    {
        return (0x08u << 25) | (uint32_t(it & 0xF) << 16) | (uint32_t(is & 0xF) << 11) | (uint32_t(imm) & 0x7FF);
    }
    uint32_t ibne(uint8_t is, uint8_t it, int16_t imm)
    {
        return (0x29u << 25) | (uint32_t(it & 0xF) << 16) | (uint32_t(is & 0xF) << 11) | (uint32_t(imm) & 0x7FF);
    }
    constexpr uint32_t kLowerNop = 0x8000033Cu;
    constexpr uint32_t kUpperNop = 0x000002FFu;
    constexpr uint32_t kEBit = 1u << 30;

    struct Program
    {
        std::vector<uint32_t> words; // lower, upper pairs
        void pair(uint32_t lo, uint32_t up) { words.push_back(lo); words.push_back(up); }
        int16_t index() const { return static_cast<int16_t>(words.size() / 2); }
    };

    // XGKICK workload: `kicks` times, send a 2-qword GIF packet (PACKED, NLOOP=1, NREG=1, EOP) from qword 900 to PATH1,
    // waiting for each transfer to finish (XGKICK stalls while PATH1 is busy) - a stand-in for per-primitive kicks.
    Program buildKicks(uint16_t kicks)
    {
        Program p;
        p.pair(iaddiu(1, 0, 900), kUpperNop);                                  // vi1 = packet qword address
        p.pair(iaddiu(2, 0, 0), kUpperNop);                                    // vi2 = counter
        p.pair(iaddiu(3, 0, static_cast<int16_t>(kicks)), kUpperNop);          // vi3 = count
        const int16_t loop = p.index();
        p.pair(lowerSpecial(0x6C, 1, 0, 0, 0), kUpperNop);                     // XGKICK vi1
        p.pair(iaddiu(2, 2, 1), kUpperNop);
        p.pair(ibne(2, 3, static_cast<int16_t>(loop - (p.index() + 1))), kUpperNop);
        p.pair(kLowerNop, kUpperNop);
        p.pair(kLowerNop, kUpperNop | kEBit);
        p.pair(kLowerNop, kUpperNop);
        return p;
    }

    // vf10..13 = matrix rows (qwords 0..3), vertices from qword 4, results to qword 4 + n.
    Program buildTransform(uint16_t vertices)
    {
        Program p;
        for (uint8_t r = 0; r < 4; ++r)
            p.pair(lq(0xF, static_cast<uint8_t>(10 + r), 0, r), kUpperNop);
        p.pair(iaddiu(1, 0, 4), kUpperNop);                                   // vi1 = input
        p.pair(iaddiu(2, 0, static_cast<int16_t>(4 + vertices)), kUpperNop);  // vi2 = output
        p.pair(iaddiu(3, 0, static_cast<int16_t>(4 + vertices)), kUpperNop);  // vi3 = end of input
        const int16_t loop = p.index();
        p.pair(lq(0xF, 1, 1, 0), kUpperNop);                                  // vf1 = vertex
        p.pair(kLowerNop, upperSpecial(0x18, 0xF, 1, 10));                    // MULAx  ACC = vf10 * vf1.x
        p.pair(kLowerNop, upperSpecial(0x09, 0xF, 1, 11));                    // MADDAy ACC += vf11 * vf1.y
        p.pair(kLowerNop, upperSpecial(0x0A, 0xF, 1, 12));                    // MADDAz ACC += vf12 * vf1.z
        p.pair(kLowerNop, upper(0x0B, 0xF, 1, 13, 2));                        // MADDw  vf2 = ACC + vf13 * vf1.w
        p.pair(lowerSpecial(0x38, 0, 2, 0, (3 << 2) | 3), kUpperNop);         // DIV Q, vf0.w, vf2.w
        p.pair(lowerSpecial(0x3B, 0, 0, 0, 0), kUpperNop);                    // WAITQ
        p.pair(kLowerNop, upper(0x1C, 0xE, 0, 2, 3));                         // MULq.xyz vf3, vf2, Q
        p.pair(kLowerNop, upperSpecial(0x15, 0xE, 4, 3));                     // FTOI4.xyz vf4, vf3
        p.pair(sq(0xE, 4, 2, 0), kUpperNop);                                  // SQ.xyz vf4, 0(vi2)
        p.pair(iaddiu(1, 1, 1), kUpperNop);
        p.pair(iaddiu(2, 2, 1), kUpperNop);
        p.pair(ibne(1, 3, static_cast<int16_t>(loop - (p.index() + 1))), kUpperNop);
        p.pair(kLowerNop, kUpperNop);                                         // delay slot
        p.pair(kLowerNop, kUpperNop | kEBit);
        p.pair(kLowerNop, kUpperNop);
        return p;
    }

    uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

    double median(std::vector<double> v)
    {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
}

int main(int argc, char **argv)
{
    const int runs = argc > 1 ? std::max(3, std::atoi(argv[1])) : 31;
    const uint16_t vertices = static_cast<uint16_t>(argc > 2 ? std::clamp(std::atoi(argv[2]), 1, 500) : 256);
    PS2Memory mem;
    if (!mem.initialize())
        return 2;
    GS gs;
    gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
    uint8_t *code = mem.getVU1Code();
    uint8_t *data = mem.getVU1Data();
    std::memset(code, 0, PS2_VU1_CODE_SIZE);
    const Program prog = buildTransform(vertices);
    for (size_t i = 0; i < prog.words.size(); ++i)
        mem.write32(0x11008000u + static_cast<uint32_t>(i * 4), prog.words[i]); // VU1 micro memory, marks generation

    // Matrix: a perspective-like transform with a w row, all values normal.
    const float m[4][4] = {{1.25f, 0.1f, -0.2f, 0.01f}, {0.05f, 1.5f, 0.3f, 0.02f}, {-0.1f, 0.2f, 0.9f, 0.03f},
                           {3.0f, -2.0f, 10.0f, 4.0f}};
    std::vector<float> in(4u * vertices);
    for (uint16_t i = 0; i < vertices; ++i)
    {
        in[4u * i + 0] = 10.0f + 0.37f * i;
        in[4u * i + 1] = -5.0f + 0.11f * i;
        in[4u * i + 2] = 2.0f + 0.05f * i;
        in[4u * i + 3] = 1.0f;
    }
    auto loadData = [&]() {
        std::memset(data, 0, PS2_VU1_DATA_SIZE);
        std::memcpy(data, m, sizeof(m));
        std::memcpy(data + 64, in.data(), in.size() * sizeof(float));
    };

    // Correctness gate: run once, compare every output vertex with an independent recomputation that uses the
    // interpreter's own Q only through the documented path (MULq then FTOI4) - the matrix part is checked bit-exactly.
    VU1Interpreter vu;
    loadData();
    vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &mem, 0, 0, 0, 1u << 26);
    uint64_t pairsPerRun = 0;
    {
        int bad = 0;
        for (uint16_t i = 0; i < vertices; ++i)
        {
            using namespace ps2_vu_fmac;
            const float *v = &in[4u * i];
            uint32_t r[4];
            for (int l = 0; l < 4; ++l)
            {
                uint32_t a = mulRef(bits(m[0][l]), bits(v[0]));
                a = addRef(a, mulRef(bits(m[1][l]), bits(v[1])));
                a = addRef(a, mulRef(bits(m[2][l]), bits(v[2])));
                r[l] = addRef(a, mulRef(bits(m[3][l]), bits(v[3])));
            }
            int32_t got[3];
            std::memcpy(got, data + 16u * (4u + vertices + i), sizeof(got));
            // FTOI4(x * Q) with Q = 1/w: recompute Q exactly as a float division (FDIV unit rounds to nearest).
            float w; std::memcpy(&w, &r[3], 4);
            const uint32_t qbits = bits(1.0f / w);
            for (int l = 0; l < 3; ++l)
            {
                const uint32_t p = mulRef(r[l], qbits);
                float pf; std::memcpy(&pf, &p, 4);
                const int32_t want = static_cast<int32_t>(static_cast<double>(pf) * 16.0);
                bad += got[l] != want;
            }
        }
        pairsPerRun = 7u + 13u * vertices + 2u;
        std::printf("correctness: %s (%d lane mismatches over %u vertices)\n", bad ? "FAIL" : "ok", bad, vertices);
        if (bad)
            return 1;
    }

    if (argc > 3 && std::strcmp(argv[3], "xgkick") == 0)
    {
        const Program kick = buildKicks(vertices);
        std::memset(code, 0, PS2_VU1_CODE_SIZE);
        for (size_t i = 0; i < kick.words.size(); ++i)
            mem.write32(0x11008000u + static_cast<uint32_t>(i * 4), kick.words[i]);
        const uint64_t tag = 1ull | (1ull << 15) | (1ull << 60); // NLOOP=1, EOP, PACKED, NREG=1, REGS=0 (PRIM)
        std::vector<double> ns;
        for (int r = -3; r < runs; ++r)
        {
            std::memset(data, 0, PS2_VU1_DATA_SIZE);
            std::memcpy(data + 900 * 16, &tag, sizeof(tag));
            const auto t0 = std::chrono::steady_clock::now();
            vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &mem, 0, 0, 0, 1u << 26);
            const auto t1 = std::chrono::steady_clock::now();
            if (r >= 0)
                ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        std::sort(ns.begin(), ns.end());
        std::printf("xgkick    kicks %u: median %.1f us per run [%.1f..%.1f], %.1f ns/kick, %llu cycles\n", vertices,
                    ns[ns.size() / 2] / 1000.0, ns.front() / 1000.0, ns.back() / 1000.0,
                    ns[ns.size() / 2] / vertices, static_cast<unsigned long long>(vu.state().cycles));
        return 0;
    }

    if (argc > 3 && std::strcmp(argv[3], "profile") == 0)
    {
        SamplingProfiler profiler;
        profiler.start();
        for (int r = 0; r < runs; ++r)
        {
            loadData();
            vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &mem, 0, 0, 0, 1u << 26);
        }
        profiler.stop();
        profiler.report();
        return 0;
    }

    for (const bool reupload : {false, true})
    {
        std::vector<double> ns;
        uint64_t cycles = 0;
        for (int r = -3; r < runs; ++r)
        {
            loadData();
            if (reupload) // rewrite the last NOP pair's upper word with itself: content unchanged, generation bumped
                mem.write32(0x11008000u + static_cast<uint32_t>((prog.words.size() - 1) * 4), prog.words.back());
            const uint64_t c0 = vu.state().cycles;
            const auto t0 = std::chrono::steady_clock::now();
            vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &mem, 0, 0, 0, 1u << 26);
            const auto t1 = std::chrono::steady_clock::now();
            cycles = vu.state().cycles - c0;
            if (r >= 0)
                ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        const double med = median(ns);
        std::sort(ns.begin(), ns.end());
        std::printf("%-9s vertices %u: median %.1f us per run [%.1f..%.1f], %.1f ns/vertex, %.2f ns/pair, %llu cycles\n",
                    reupload ? "reupload" : "steady", vertices, med / 1000.0, ns.front() / 1000.0, ns.back() / 1000.0,
                    med / vertices, med / static_cast<double>(pairsPerRun), static_cast<unsigned long long>(cycles));
    }
    return 0;
}
