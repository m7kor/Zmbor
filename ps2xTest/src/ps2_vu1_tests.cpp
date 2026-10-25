#include "MiniTest.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "ps2_vu_fmac.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kVuUpperNop = 0x000002FFu;

    struct Vu1Fixture
    {
        PS2Memory mem;
        GS gs;
        uint8_t *code = nullptr;
        uint8_t *data = nullptr;

        bool initialize()
        {
            if (!mem.initialize())
                return false;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            code = mem.getVU1Code();
            data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);
            return code != nullptr && data != nullptr;
        }
    };

    uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) |
               (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
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

    uint32_t makeVuLowerSpecial(uint8_t specialOp, uint8_t is, uint8_t it = 0u, uint8_t id = 0u, uint8_t dest = 0u)
    {
        return (0x40u << 25) |
               (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(it & 0x1Fu) << 16) |
               (static_cast<uint32_t>(is & 0x1Fu) << 11) |
               (static_cast<uint32_t>(id & 0x1Fu) << 6) |
               (static_cast<uint32_t>(specialOp & 0x7Cu) << 4) |
               static_cast<uint32_t>(specialOp & 0x3u) |
               0x3Cu;
    }

    uint32_t makeVuLowerDirect(uint8_t funct, uint8_t is, uint8_t it = 0u, uint8_t id = 0u, uint8_t dest = 0u)
    {
        return (0x40u << 25) |
               (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(it & 0x1Fu) << 16) |
               (static_cast<uint32_t>(is & 0x1Fu) << 11) |
               (static_cast<uint32_t>(id & 0x1Fu) << 6) |
               static_cast<uint32_t>(funct & 0x3Fu);
    }

    uint32_t makeVuUpper(uint8_t op, uint8_t dest, uint8_t ft, uint8_t fs, uint8_t fd)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(ft & 0x1Fu) << 16) |
               (static_cast<uint32_t>(fs & 0x1Fu) << 11) |
               (static_cast<uint32_t>(fd & 0x1Fu) << 6) |
               static_cast<uint32_t>(op & 0x3Fu);
    }

    uint32_t makeVuUpperSpecial(uint8_t specialOp, uint8_t dest, uint8_t ft, uint8_t fs)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(ft & 0x1Fu) << 16) |
               (static_cast<uint32_t>(fs & 0x1Fu) << 11) |
               (static_cast<uint32_t>(specialOp & 0x7Cu) << 4) |
               static_cast<uint32_t>(specialOp & 0x3u) |
               0x3Cu;
    }

    uint32_t makeVuFlagImmediate(uint8_t opcode, uint8_t targetVi, uint16_t immediate)
    {
        return (static_cast<uint32_t>(opcode & 0x7Fu) << 25) |
               (static_cast<uint32_t>((immediate >> 11) & 0x1u) << 21) |
               (static_cast<uint32_t>(targetVi & 0xFu) << 16) |
               static_cast<uint32_t>(immediate & 0x7FFu);
    }

    uint32_t makeVuFlagRegister(uint8_t opcode, uint8_t targetVi, uint8_t sourceVi)
    {
        return (static_cast<uint32_t>(opcode & 0x7Fu) << 25) |
               (static_cast<uint32_t>(targetVi & 0xFu) << 16) |
               (static_cast<uint32_t>(sourceVi & 0xFu) << 11);
    }

    uint32_t makeVuLq(uint8_t dest, uint8_t targetVf, uint8_t baseVi, int16_t imm)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(targetVf & 0x1Fu) << 16) |
               (static_cast<uint32_t>(baseVi & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuSq(uint8_t dest, uint8_t sourceVf, uint8_t baseVi, int16_t imm)
    {
        return (0x01u << 25) |
               (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(baseVi & 0xFu) << 16) |
               (static_cast<uint32_t>(sourceVf & 0x1Fu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuIaddiu(uint8_t it, uint8_t is, int16_t imm)
    {
        return (0x08u << 25) |
               (static_cast<uint32_t>(it & 0xFu) << 16) |
               (static_cast<uint32_t>(is & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuBranch(int16_t imm)
    {
        return (0x20u << 25) | (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuJr(uint8_t is)
    {
        return (0x24u << 25) |
               (static_cast<uint32_t>(is & 0xFu) << 11);
    }

    uint32_t makeVuIbne(uint8_t is, uint8_t it, int16_t imm)
    {
        return (0x29u << 25) |
               (static_cast<uint32_t>(it & 0xFu) << 16) |
               (static_cast<uint32_t>(is & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuIlw(uint8_t dest, uint8_t targetVi, uint8_t baseVi, int16_t imm)
    {
        return (0x04u << 25) |
               (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(targetVi & 0xFu) << 16) |
               (static_cast<uint32_t>(baseVi & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuDiv(uint8_t fs, uint8_t ft, uint8_t fsf, uint8_t ftf)
    {
        return makeVuLowerSpecial(0x38u, fs, ft, 0u, static_cast<uint8_t>(((ftf & 0x3u) << 2) | (fsf & 0x3u)));
    }

    uint32_t makeVuSqrt(uint8_t ft, uint8_t ftf)
    {
        return makeVuLowerSpecial(0x39u, 0u, ft, 0u, static_cast<uint8_t>((ftf & 0x3u) << 2));
    }

    void writeVuInstructionPair(uint8_t *code, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        std::memcpy(code + pc, &lower, sizeof(lower));
        std::memcpy(code + pc + sizeof(lower), &upper, sizeof(upper));
    }

    uint64_t packVuInstructionPair(uint32_t lower, uint32_t upper)
    {
        return static_cast<uint64_t>(lower) | (static_cast<uint64_t>(upper) << 32);
    }

    void writeTrackedVuInstructionPair(Vu1Fixture &fx, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        fx.mem.write64(PS2_VU1_CODE_BASE + pc, packVuInstructionPair(lower, upper));
    }

    void appendU32(std::vector<uint8_t> &bytes, uint32_t value)
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(&value);
        bytes.insert(bytes.end(), src, src + sizeof(value));
    }

    void uploadVu1Mpg(PS2Memory &mem, uint16_t instructionAddress, uint32_t lower, uint32_t upper)
    {
        std::vector<uint8_t> packet;
        appendU32(packet, makeVifCmd(0x4Au, 1u, instructionAddress));
        appendU32(packet, lower);
        appendU32(packet, upper);
        mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));
    }

    void writeVuQword(uint8_t *data, uint32_t qwordIndex, const float values[4])
    {
        std::memcpy(data + qwordIndex * 16u, values, sizeof(float) * 4u);
    }

    void readVuQword(const uint8_t *data, uint32_t qwordIndex, float values[4])
    {
        std::memcpy(values, data + qwordIndex * 16u, sizeof(float) * 4u);
    }
}

void register_ps2_vu1_tests()
{
    MiniTest::Case("PS2VU1", [](TestCase &tc)
    {
        tc.Run("upper ADD applies the destination mask", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u)); // ADD.xz vf3, vf1, vf2

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[1][3] = 4.0f;
            vu1.state().vf[2][0] = 10.0f;
            vu1.state().vf[2][1] = 20.0f;
            vu1.state().vf[2][2] = 30.0f;
            vu1.state().vf[2][3] = 40.0f;
            vu1.state().vf[3][0] = -1.0f;
            vu1.state().vf[3][1] = -2.0f;
            vu1.state().vf[3][2] = -3.0f;
            vu1.state().vf[3][3] = -4.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);

            t.Equals(vu1.state().vf[3][0], -1.0f,
                     "FMAC destination must remain hidden before its four-cycle writeback");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 3u);
            t.Equals(vu1.state().vf[3][0], 11.0f, "ADD.x should write x");
            t.Equals(vu1.state().vf[3][1], -2.0f, "ADD.xz should preserve y");
            t.Equals(vu1.state().vf[3][2], 33.0f, "ADD.xz should write z");
            t.Equals(vu1.state().vf[3][3], -4.0f, "ADD.xz should preserve w");
        });

        tc.Run("LOI commits the lower immediate after the upper instruction", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            const float newI = 7.0f;
            uint32_t lowerImmediate = 0u;
            std::memcpy(&lowerImmediate, &newI, sizeof(newI));
            const uint32_t upperAddiWithIBit = makeVuUpper(0x22u, 0xFu, 0u, 1u, 2u) | 0x80000000u; // ADDi.xyzw vf2, vf1
            writeVuInstructionPair(fx.code, 0u, lowerImmediate, upperAddiWithIBit);

            VU1Interpreter vu1;
            vu1.state().i = 2.0f;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[1][3] = 4.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);

            t.Equals(vu1.state().vf[2][0], 0.0f,
                     "ADDi result should remain in the FMAC pipeline");
            t.Equals(vu1.state().i, 7.0f,
                     "LOI should become visible after the upper from the same pair");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 3u);
            t.Equals(vu1.state().vf[2][0], 3.0f, "ADDi should use old I for x");
            t.Equals(vu1.state().vf[2][1], 4.0f, "ADDi should use old I for y");
            t.Equals(vu1.state().vf[2][2], 5.0f, "ADDi should use old I for z");
            t.Equals(vu1.state().vf[2][3], 6.0f, "ADDi should use old I for w");
            t.Equals(vu1.state().i, 7.0f, "LOI should commit lower immediate into I after upper execution");
        });

        tc.Run("ITOF converts the raw signed integer bits without float normalization", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpperSpecial(0x10u, 0xFu, 2u, 1u));

            VU1Interpreter vu1;
            const int32_t raw[4] = {1, -16, 4096, -32768};
            std::memcpy(vu1.state().vf[1], raw, sizeof(raw));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vf[2][0], 1.0f,
                     "ITOF0 must not flush an integer bit pattern that resembles a denormal");
            t.Equals(vu1.state().vf[2][1], -16.0f,
                     "ITOF0 must preserve negative integer bit patterns");
            t.Equals(vu1.state().vf[2][2], 4096.0f,
                     "ITOF0 should convert positive fixed-point source bits");
            t.Equals(vu1.state().vf[2][3], -32768.0f,
                     "ITOF0 should convert negative fixed-point source bits");
        });

        tc.Run("MTIR decodes fsf as a component selector", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            for (uint32_t component = 0; component < 4u; ++component)
            {
                writeVuInstructionPair(
                    fx.code, component * 8u,
                    makeVuLowerSpecial(0x3Cu, 1u,
                                       static_cast<uint8_t>(component + 2u),
                                       0u,
                                       static_cast<uint8_t>(component)),
                    kVuUpperNop);
            }

            VU1Interpreter vu1;
            const uint32_t raw[4] = {0x00001111u, 0x00002222u, 0x00003333u, 0x00004444u};
            std::memcpy(vu1.state().vf[1], raw, sizeof(raw));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vi[2], 0x1111,
                     "MTIR fsf=x should read VF.x");
            t.Equals(vu1.state().vi[3], 0x2222,
                     "MTIR fsf=y should read VF.y");
            t.Equals(vu1.state().vi[4], 0x3333,
                     "MTIR fsf=z should read VF.z");
            t.Equals(vu1.state().vi[5], 0x4444,
                     "MTIR fsf=w should read VF.w");
        });

        tc.Run("LQ and SQ use VI qword addressing and destination masks", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            const float sourceQw[4] = {10.0f, 20.0f, 30.0f, 40.0f};
            const float destQw[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
            writeVuQword(fx.data, 3u, sourceQw);
            writeVuQword(fx.data, 5u, destQw);
            writeVuInstructionPair(fx.code, 0u, makeVuLq(0x5u, 4u, 1u, 1), kVuUpperNop); // LQ.yw vf4, 1(vi1)
            writeVuInstructionPair(fx.code, 8u, makeVuSq(0xAu, 4u, 2u, 1), kVuUpperNop); // SQ.xz vf4, 1(vi2)

            VU1Interpreter vu1;
            vu1.state().vi[1] = 2;
            vu1.state().vi[2] = 4;
            vu1.state().vf[4][0] = 100.0f;
            vu1.state().vf[4][1] = 200.0f;
            vu1.state().vf[4][2] = 300.0f;
            vu1.state().vf[4][3] = 400.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 3u);

            t.Equals(vu1.state().vf[4][1], 200.0f,
                     "LQ result should remain hidden before its fourth cycle");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 1u);
            t.Equals(vu1.state().vf[4][0], 100.0f, "LQ.yw should preserve x");
            t.Equals(vu1.state().vf[4][1], 20.0f, "LQ.yw should load y");
            t.Equals(vu1.state().vf[4][2], 300.0f, "LQ.yw should preserve z");
            t.Equals(vu1.state().vf[4][3], 40.0f, "LQ.yw should load w");

            float stored[4] = {};
            readVuQword(fx.data, 5u, stored);
            t.Equals(stored[0], 100.0f, "SQ.xz should store x");
            t.Equals(stored[1], -2.0f, "SQ.xz should preserve y");
            t.Equals(stored[2], 300.0f, "SQ.xz should store z");
            t.Equals(stored[3], -4.0f, "SQ.xz should preserve w");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(4u),
                     "disjoint LQ/SQ lanes should issue without delaying LQ writeback");
        });

        tc.Run("integer lower ops keep VI0 hardwired to zero", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, makeVuIaddiu(2u, 1u, 5), kVuUpperNop);      // IADDIU vi2, vi1, 5
            writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(0u, 2u, 7), kVuUpperNop);      // IADDIU vi0, vi2, 7
            writeVuInstructionPair(fx.code, 16u, makeVuLowerDirect(0x30u, 2u, 1u, 3u), kVuUpperNop); // IADD vi3, vi2, vi1

            VU1Interpreter vu1;
            vu1.state().vi[0] = 99;
            vu1.state().vi[1] = 10;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 3u);

            t.Equals(vu1.state().vi[2], 15, "IADDIU should add signed immediate to VI source");
            t.Equals(vu1.state().vi[3], 25, "IADD should add VI source registers");
            t.Equals(vu1.state().vi[0], 0, "VI0 should remain hardwired to zero");
        });

        tc.Run("XTOP and XITOP expose VIF TOP values to VI registers", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(0x68u, 0u, 2u), kVuUpperNop); // XTOP vi2
            writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x69u, 0u, 3u), kVuUpperNop); // XITOP vi3

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0x123u, 0x2ABu, 2u);

            t.Equals(vu1.state().vi[2], 0x123, "XTOP should move TOP into the target VI register");
            t.Equals(vu1.state().vi[3], 0x2AB, "XITOP should move ITOP into the target VI register");
        });

        tc.Run("lower branch commits after one delay-slot instruction", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, makeVuBranch(2), kVuUpperNop);              // target pc = 24
            writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(1u, 0u, 1), kVuUpperNop);      // delay slot
            writeVuInstructionPair(fx.code, 16u, makeVuIaddiu(2u, 0u, 99), kVuUpperNop);    // skipped
            writeVuInstructionPair(fx.code, 24u, makeVuIaddiu(3u, 0u, 7), kVuUpperNop);     // branch target

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 3u);

            t.Equals(vu1.state().vi[1], 1, "branch delay slot should execute");
            t.Equals(vu1.state().vi[2], 0, "instruction between delay slot and target should be skipped");
            t.Equals(vu1.state().vi[3], 7, "branch target should execute after the delay slot");
        });

        tc.Run("lower side sees old VF value when upper writes the same register", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code,
                                   0u,
                                   makeVuSq(0xFu, 1u, 1u, 0),                 // SQ.xyzw vf1, 0(vi1)
                                   makeVuUpper(0x28u, 0xFu, 3u, 2u, 1u));     // ADD.xyzw vf1, vf2, vf3

            VU1Interpreter vu1;
            vu1.state().vi[1] = 6;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[1][3] = 4.0f;
            vu1.state().vf[2][0] = 10.0f;
            vu1.state().vf[2][1] = 20.0f;
            vu1.state().vf[2][2] = 30.0f;
            vu1.state().vf[2][3] = 40.0f;
            vu1.state().vf[3][0] = 100.0f;
            vu1.state().vf[3][1] = 200.0f;
            vu1.state().vf[3][2] = 300.0f;
            vu1.state().vf[3][3] = 400.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 2u);

            float stored[4] = {};
            readVuQword(fx.data, 6u, stored);
            t.Equals(stored[0], 1.0f, "SQ should observe old VF value for x");
            t.Equals(stored[1], 2.0f, "SQ should observe old VF value for y");
            t.Equals(stored[2], 3.0f, "SQ should observe old VF value for z");
            t.Equals(stored[3], 4.0f, "SQ should observe old VF value for w");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 2u);
            t.Equals(vu1.state().vf[1][0], 110.0f, "upper ADD should write x after lower read");
            t.Equals(vu1.state().vf[1][1], 220.0f, "upper ADD should write y after lower read");
            t.Equals(vu1.state().vf[1][2], 330.0f, "upper ADD should write z after lower read");
            t.Equals(vu1.state().vf[1][3], 440.0f, "upper ADD should write w after lower read");
        });

        tc.Run("upper suppresses only the colliding lower VF write", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            const float loaded[4] = {90.0f, 91.0f, 92.0f, 93.0f};
            writeVuQword(fx.data, 5u, loaded);
            writeVuInstructionPair(
                fx.code,
                0u,
                makeVuLowerSpecial(0x34u, 1u, 1u, 0u, 0xFu),
                makeVuUpper(0x28u, 0x8u, 3u, 2u, 1u));

            VU1Interpreter vu1;
            vu1.state().vi[1] = 5;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[1][3] = 4.0f;
            vu1.state().vf[2][0] = 10.0f;
            vu1.state().vf[3][0] = 100.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 1u);

            t.Equals(vu1.state().vf[1][0], 1.0f,
                     "upper write should remain pending until the FMAC writeback cycle");
            t.Equals(vu1.state().vi[1], 6,
                     "LQI post-increment should commit after one cycle");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 3u);
            t.Equals(vu1.state().vf[1][0], 110.0f,
                     "upper x should be written");
            t.Equals(vu1.state().vf[1][1], 2.0f,
                     "discarded lower must not leak y into the upper result");
            t.Equals(vu1.state().vf[1][2], 3.0f,
                     "discarded lower must not leak z into the upper result");
            t.Equals(vu1.state().vf[1][3], 4.0f,
                      "discarded lower must not leak w into the upper result");
            t.Equals(vu1.state().vi[1], 6,
                     "LQI post-increment must survive suppression of its colliding VF write");
        });

        tc.Run("upper and lower both read the pre-pair VF state", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuLowerSpecial(0x30u, 1u, 2u, 0u, 0x8u),
                makeVuUpper(0x28u, 0x8u, 3u, 2u, 1u));

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 10.0f;
            vu1.state().vf[2][0] = 20.0f;
            vu1.state().vf[3][0] = 1.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 1u);

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 3u);
            t.Equals(vu1.state().vf[1][0], 21.0f,
                     "upper must read vf2 before lower MOVE overwrites it");
            t.Equals(vu1.state().vf[2][0], 10.0f,
                     "lower must read vf1 before upper ADD overwrites it");
        });

        tc.Run("FMAC dependency stalls only the lanes that are read", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpper(0x28u, 0x8u, 2u, 1u, 3u));
            writeVuInstructionPair(
                fx.code, 8u, 0u,
                makeVuUpper(0x28u, 0x8u, 2u, 3u, 4u));

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[2][0] = 2.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().vf[4][0], 0.0f,
                     "the dependent result should remain pending until its own writeback");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 3u);
            t.Equals(vu1.state().vf[4][0], 5.0f,
                     "dependent ADD should consume the completed x lane");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(8u),
                     "dependency stall and the dependent FMAC writeback must both consume cycles");
        });

        tc.Run("ACC forwarding feeds the next upper instruction without a dependency stall", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpperSpecial(0x28u, 0x8u, 2u, 1u)); // ADDA.x acc, vf1, vf2
            writeVuInstructionPair(
                fx.code, 8u, 0u,
                makeVuUpper(0x29u, 0x8u, 4u, 3u, 5u)); // MADD.x vf5, vf3, vf4

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[2][0] = 10.0f;
            vu1.state().vf[3][0] = 2.0f;
            vu1.state().vf[4][0] = 3.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().acc[0], 11.0f,
                     "ADDA should forward ACC to the following upper instruction");
            t.Equals(vu1.state().vf[5][0], 17.0f,
                     "MADD should consume the forwarded ACC value without stalling");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(5u),
                     "ACC forwarding must not introduce a four-cycle dependency stall");
        });

        tc.Run("ILW result becomes visible after four cycles before IALU consumes it", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            const uint32_t source[4] = {0x1234u, 0u, 0u, 0u};
            std::memcpy(fx.data + 2u * 16u, source, sizeof(source));
            writeVuInstructionPair(
                fx.code, 0u, makeVuIlw(0x8u, 2u, 1u, 0), kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIaddiu(3u, 2u, 1), kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 2;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().vi[2], 0x1234,
                     "ILW should commit the selected word after four cycles");
            t.Equals(vu1.state().vi[3], 0x1235,
                     "IADDIU should wait for and consume the ILW result");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(5u),
                     "ILW-to-IALU dependency should account for all stalled cycles");
        });

        tc.Run("DIV and SQRT update the Q register from selected vector components", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, makeVuDiv(1u, 2u, 1u, 2u), kVuUpperNop);              // Q = vf1.y / vf2.z
            writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x3Bu, 0u), kVuUpperNop);          // WAITQ
            writeVuInstructionPair(fx.code, 16u, makeVuSqrt(3u, 3u), kVuUpperNop);                    // Q = sqrt(abs(vf3.w))
            writeVuInstructionPair(fx.code, 24u, makeVuLowerSpecial(0x3Bu, 0u), kVuUpperNop);         // WAITQ

            VU1Interpreter vu1;
            vu1.state().vf[1][1] = 18.0f;
            vu1.state().vf[2][2] = 3.0f;
            vu1.state().vf[3][3] = 25.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 8u);
            t.Equals(vu1.state().q, 6.0f, "WAITQ should expose DIV after its seven-cycle latency");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(8u),
                     "maxCycles should count FDIV stalls as elapsed VU cycles");

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 8u);
            t.Equals(vu1.state().q, 5.0f, "WAITQ should expose SQRT after its seven-cycle latency");
        });

        tc.Run("FDIV resource serializes back-to-back scalar operations", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuDiv(1u, 2u, 0u, 0u),
                kVuUpperNop); // Q = vf1.x / vf2.x
            writeVuInstructionPair(
                fx.code, 8u,
                makeVuSqrt(3u, 0u),
                kVuUpperNop); // Must wait for the shared FDIV unit.
            writeVuInstructionPair(
                fx.code, 16u,
                makeVuLowerSpecial(0x3Bu, 0u),
                kVuUpperNop); // WAITQ

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 18.0f;
            vu1.state().vf[2][0] = 3.0f;
            vu1.state().vf[3][0] = 25.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 15u);

            t.Equals(vu1.state().q, 5.0f,
                     "the second FDIV operation should commit the final Q value");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(15u),
                     "back-to-back FDIV operations should include the resource stall");
        });

        tc.Run("FDIV commits current and sticky divide-invalid status", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, makeVuDiv(1u, 2u, 0u, 0u),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuLowerSpecial(0x3Bu, 0u),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 0.0f;
            vu1.state().vf[2][0] = 0.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 8u);
            t.Equals(vu1.state().status, 0x410u,
                     "zero divided by zero should set current and sticky I");

            vu1.reset();
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[2][0] = 0.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 8u);
            t.Equals(vu1.state().status, 0x820u,
                     "a nonzero numerator divided by zero should set current and sticky D");
        });

        tc.Run("EFU WAITP and RNG execute with architectural latency and state", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuLowerSpecial(0x70u, 1u),
                kVuUpperNop); // ESADD P, vf1
            writeVuInstructionPair(
                fx.code, 8u,
                makeVuLowerSpecial(0x7Bu, 0u),
                kVuUpperNop); // WAITP
            writeVuInstructionPair(
                fx.code, 16u,
                makeVuLowerSpecial(0x42u, 2u, 0u, 0u, 0x8u),
                kVuUpperNop); // RINIT R, vf2.x
            writeVuInstructionPair(
                fx.code, 24u,
                makeVuLowerSpecial(0x40u, 0u, 3u, 0u, 0x8u),
                kVuUpperNop); // RNEXT.x vf3

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[2][0] = 1.5f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 14u);

            t.Equals(vu1.state().p, 14.0f,
                     "WAITP should expose ESADD after eleven cycles");
            t.Equals(vu1.state().vf[3][0], 0.0f,
                     "RNEXT vector result should respect FMAC writeback latency");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 3u);
            t.IsTrue(vu1.state().vf[3][0] >= 1.0f &&
                         vu1.state().vf[3][0] < 2.0f &&
                         vu1.state().vf[3][0] != 1.5f,
                      "RNEXT should advance the 23-bit R LFSR and write a 1.x value");
        });

        tc.Run("EFU sub-opcodes follow the PCSX2 lower-op tables", [](TestCase &t)
        {
            // funct2 = ((instr >> 6) & 0x1F) << 2 | (instr & 3). PCSX2 VUops.cpp LowerOP_T3_00..T3_11:
            // row 0x1E = ESQRT, ERSQRT, ERCPR, WAITP (0x78-0x7B); row 0x1F = ESIN, EATAN, EEXP, - (0x7C-0x7F).
            struct EfuCase
            {
                uint8_t op;
                float input;
                float expected;
                const char *what;
            };
            const EfuCase cases[] = {
                {0x78u, 4.0f, 2.0f, "0x78 is ESQRT"},
                {0x79u, 4.0f, 0.5f, "0x79 is ERSQRT"},
                {0x7Au, 4.0f, 0.25f, "0x7A is ERCPR"},
                {0x7Cu, 1.57079633f, 1.0f, "0x7C is ESIN"},
                // Real PS2 (ps2autotests vu/lower/efu.expected "EATAN CVF_ONE: 3f490fda"): atan(1) = pi/4; the unit
                // forms t = (x - 1) / (x + 1) itself.
                {0x7Du, 1.0f, 0.785398185f, "0x7D is EATAN: atan(x) via t = (x - 1) / (x + 1)"},
                {0x7Eu, 1.0f, 0.367879441f, "0x7E is EEXP (e^-x)"},
            };
            for (const EfuCase &efu : cases)
            {
                Vu1Fixture fx;
                t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
                writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(efu.op, 1u), kVuUpperNop); // op P, vf1.x
                writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x7Bu, 0u), kVuUpperNop);  // WAITP

                VU1Interpreter vu1;
                vu1.state().vf[1][0] = efu.input;
                vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                            fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                            0u, 0u, 0u, 80u);
                t.IsTrue(std::fabs(vu1.state().p - efu.expected) < 1e-4f, efu.what);
            }

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(0x77u, 1u), kVuUpperNop); // reserved
            writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x7Bu, 0u), kVuUpperNop);  // WAITP
            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 4.0f;
            vu1.state().p = 7.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 80u);
            t.Equals(vu1.state().p, 7.0f, "0x77 is reserved and must not compute an EFU result");
        });

        tc.Run("VU ADD drops an operand that is shifted out entirely, whatever its sign", [](TestCase &t)
        {
            // ps2autotests vu/games/triace.expected: ADDi.x of 0xB3E2A618 (-1.05e-7) and I = 0x42546666 reads back
            // 0x42546666; a truncated exact sum would be 0x42546665.
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuInstructionPair(fx.code, 0u, 0x8000033Cu, makeVuUpper(0x22u, 0x8u, 0u, 2u, 3u) | (1u << 30)); // ADDi.x vf3, vf2, I; E
            writeVuInstructionPair(fx.code, 8u, 0x8000033Cu, kVuUpperNop);
            VU1Interpreter vu1;
            const uint32_t i = 0x42546666u, x = 0xB3E2A618u;
            std::memcpy(&vu1.state().i, &i, sizeof(i));
            std::memcpy(&vu1.state().vf[2][0], &x, sizeof(x));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
            uint32_t got = 0u;
            std::memcpy(&got, &vu1.state().vf[3][0], sizeof(got));
            t.Equals(got, 0x42546666u, "the tiny addend must not move the result");
        });

        tc.Run("VU micro ADD/SUB/MUL agree with the FMAC contract that macro mode uses", [](TestCase &t)
        {
            // One FMAC, two implementations: the interpreter (ps2_vu_fmac's double-based lowering) against its integer
            // reference, on operands whose results stay in exponents 1..254. Outside that range the interpreter used to
            // clamp to 0x7F7FFFFF; since Phase 5 it follows the same contract there too.
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuInstructionPair(fx.code, 0u, 0x8000033Cu, makeVuUpper(0x28u, 0xFu, 2u, 1u, 3u)); // ADD vf3, vf1, vf2
            writeVuInstructionPair(fx.code, 8u, 0x8000033Cu, makeVuUpper(0x2Cu, 0xFu, 2u, 1u, 4u)); // SUB vf4, vf1, vf2
            writeVuInstructionPair(fx.code, 16u, 0x8000033Cu, makeVuUpper(0x2Au, 0xFu, 2u, 1u, 5u) | (1u << 30)); // MUL; E
            writeVuInstructionPair(fx.code, 24u, 0x8000033Cu, kVuUpperNop);
            uint64_t s = 0x9E3779B97F4A7C15ull;
            auto next = [&s]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<uint32_t>(s >> 16); };
            uint32_t inRange = 0, mismatches = 0, outOfRange = 0, outOfRangeDiffer = 0;
            std::string first;
            for (int run = 0; run < 3000; ++run)
            {
                VU1Interpreter vu1;
                uint32_t a[4], b[4];
                for (int l = 0; l < 4; ++l)
                {
                    // Mostly normal operands with every exponent gap; some at the ends of the range.
                    const bool edge = (next() & 15u) == 0u;
                    const uint32_t ea = edge ? 250u + next() % 6u : 40u + next() % 170u;
                    const uint32_t gap = next() % 32u;
                    a[l] = (next() & 0x807FFFFFu) | (ea << 23);
                    b[l] = (next() & 0x807FFFFFu) | ((ea - gap) << 23);
                }
                std::memcpy(vu1.state().vf[1], a, sizeof(a));
                std::memcpy(vu1.state().vf[2], b, sizeof(b));
                vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 64u);
                uint32_t got[3][4];
                std::memcpy(got[0], vu1.state().vf[3], sizeof(got[0]));
                std::memcpy(got[1], vu1.state().vf[4], sizeof(got[1]));
                std::memcpy(got[2], vu1.state().vf[5], sizeof(got[2]));
                for (int l = 0; l < 4; ++l)
                {
                    const uint32_t want[3] = {ps2_vu_fmac::addRef(a[l], b[l]), ps2_vu_fmac::subRef(a[l], b[l]),
                                              ps2_vu_fmac::mulRef(a[l], b[l])};
                    for (int op = 0; op < 3; ++op)
                    {
                        const uint32_t e = (want[op] >> 23) & 0xFFu;
                        const bool range = ((a[l] >> 23) & 0xFFu) != 255u && ((b[l] >> 23) & 0xFFu) != 255u && e != 255u;
                        if (!range)
                        {
                            ++outOfRange;
                            outOfRangeDiffer += got[op][l] != want[op];
                            continue;
                        }
                        ++inRange;
                        if (got[op][l] != want[op] && mismatches++ == 0)
                            first = "op " + std::to_string(op) + " " + std::to_string(a[l]) + ", " + std::to_string(b[l]);
                    }
                }
            }
            std::printf("    micro FMAC vs contract: %u in-range results, %u outside (exponent 255), %u of those differ\n",
                        inRange, outOfRange, outOfRangeDiffer);
            t.Equals(mismatches, 0u, "in-range results differing from the contract (first: " + first + ")");
            t.Equals(outOfRangeDiffer, 0u, "exponent-255 results differing from the contract (micro mode no longer clamps)");
            t.IsTrue(inRange > 30000u, "enough in-range results were compared");
        });

        tc.Run("VU micro FMAC broadcast/Q/I/accumulator/outer-product forms follow the FMAC contract", [](TestCase &t)
        {
            // Every FMAC form other than plain ADD/SUB/MUL, one instruction per run, against compositions of the
            // integer reference (MADD/MSUB round the product, then the sum). Operands: vf1 (fs), vf2 (ft), ACC, Q, I.
            enum Src { BC, Q, I, VT };
            enum Kind { ADD, SUB, MUL, MADD, MSUB, OPMUL, OPMSUB };
            struct Form { const char *name; uint8_t op; bool special; Kind kind; Src src; bool toAcc; };
            const Form forms[] = {
                {"ADDbc", 0x00, false, ADD, BC, false}, {"SUBbc", 0x04, false, SUB, BC, false},
                {"MADDbc", 0x08, false, MADD, BC, false}, {"MSUBbc", 0x0C, false, MSUB, BC, false},
                {"MULbc", 0x18, false, MUL, BC, false}, {"MULq", 0x1C, false, MUL, Q, false},
                {"MULi", 0x1E, false, MUL, I, false}, {"ADDq", 0x20, false, ADD, Q, false},
                {"MADDq", 0x21, false, MADD, Q, false}, {"ADDi", 0x22, false, ADD, I, false},
                {"MADDi", 0x23, false, MADD, I, false}, {"SUBq", 0x24, false, SUB, Q, false},
                {"MSUBq", 0x25, false, MSUB, Q, false}, {"SUBi", 0x26, false, SUB, I, false},
                {"MSUBi", 0x27, false, MSUB, I, false}, {"MADD", 0x29, false, MADD, VT, false},
                {"MSUB", 0x2D, false, MSUB, VT, false}, {"OPMSUB", 0x2E, false, OPMSUB, VT, false},
                {"ADDAbc", 0x00, true, ADD, BC, true}, {"SUBAbc", 0x04, true, SUB, BC, true},
                {"MADDAbc", 0x08, true, MADD, BC, true}, {"MSUBAbc", 0x0C, true, MSUB, BC, true},
                {"MULAbc", 0x18, true, MUL, BC, true}, {"MULAq", 0x1C, true, MUL, Q, true},
                {"MULAi", 0x1E, true, MUL, I, true}, {"ADDAq", 0x20, true, ADD, Q, true},
                {"MADDAq", 0x21, true, MADD, Q, true}, {"ADDAi", 0x22, true, ADD, I, true},
                {"MADDAi", 0x23, true, MADD, I, true}, {"SUBAq", 0x24, true, SUB, Q, true},
                {"MSUBAq", 0x25, true, MSUB, Q, true}, {"SUBAi", 0x26, true, SUB, I, true},
                {"MSUBAi", 0x27, true, MSUB, I, true}, {"ADDA", 0x28, true, ADD, VT, true},
                {"MADDA", 0x29, true, MADD, VT, true}, {"MULA", 0x2A, true, MUL, VT, true},
                {"SUBA", 0x2C, true, SUB, VT, true}, {"MSUBA", 0x2D, true, MSUB, VT, true},
                {"OPMULA", 0x2E, true, OPMUL, VT, true},
            };
            uint64_t s = 0xD1B54A32D192ED03ull;
            auto next = [&s]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<uint32_t>(s >> 16); };
            auto operand = [&next](uint32_t exponent) { return (next() & 0x807FFFFFu) | (exponent << 23); };
            uint32_t compared = 0, mismatches = 0;
            std::string first;
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            for (const Form &f : forms)
            {
                for (int run = 0; run < 150; ++run)
                {
                    const uint8_t bc = static_cast<uint8_t>(next() & 3u);
                    const uint8_t op = static_cast<uint8_t>(f.op | (f.src == BC ? bc : 0u));
                    // Special group: selector = bits 1:0 | bits 10:6 << 2, low op 0x3C..0x3F; ACC forms have no vd.
                    const uint32_t upper = f.special
                        ? makeVuUpper(static_cast<uint8_t>(0x3Cu | (op & 3u)), 0xFu, 2u, 1u, static_cast<uint8_t>(op >> 2))
                        : makeVuUpper(op, f.kind == OPMSUB ? 0xEu : 0xFu, 2u, 1u, 3u);
                    writeVuInstructionPair(fx.code, 0u, 0x8000033Cu, upper | (1u << 30));
                    writeVuInstructionPair(fx.code, 8u, 0x8000033Cu, kVuUpperNop);
                    // Exponents base +-15 (accumulator near the product's), so every product and sum stays in 1..254.
                    const uint32_t base = 80u + next() % 90u;
                    uint32_t a[4], b[4], acc[4], dst[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
                    for (int l = 0; l < 4; ++l)
                    {
                        a[l] = operand(base + next() % 16u);
                        b[l] = operand(base - next() % 16u);
                        acc[l] = operand(2u * base - 127u + 16u - next() % 33u);
                    }
                    const uint32_t q = operand(base - next() % 16u), i = operand(base - next() % 16u);
                    VU1Interpreter vu1;
                    std::memcpy(vu1.state().vf[1], a, sizeof(a));
                    std::memcpy(vu1.state().vf[2], b, sizeof(b));
                    std::memcpy(vu1.state().vf[3], dst, sizeof(dst));
                    std::memcpy(vu1.state().acc, acc, sizeof(acc));
                    std::memcpy(&vu1.state().q, &q, sizeof(q));
                    std::memcpy(&vu1.state().i, &i, sizeof(i));
                    vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
                    uint32_t got[4];
                    std::memcpy(got, f.toAcc ? vu1.state().acc : vu1.state().vf[3], sizeof(got));
                    for (int l = 0; l < (f.kind == OPMUL || f.kind == OPMSUB ? 3 : 4); ++l)
                    {
                        const uint32_t rhs = f.src == BC ? b[bc] : f.src == Q ? q : f.src == I ? i : b[l];
                        uint32_t want = 0u;
                        switch (f.kind)
                        {
                        case ADD: want = ps2_vu_fmac::addRef(a[l], rhs); break;
                        case SUB: want = ps2_vu_fmac::subRef(a[l], rhs); break;
                        case MUL: want = ps2_vu_fmac::mulRef(a[l], rhs); break;
                        case MADD: want = ps2_vu_fmac::addRef(acc[l], ps2_vu_fmac::mulRef(a[l], rhs)); break;
                        case MSUB: want = ps2_vu_fmac::subRef(acc[l], ps2_vu_fmac::mulRef(a[l], rhs)); break;
                        case OPMUL: want = ps2_vu_fmac::mulRef(a[(l + 1) % 3], b[(l + 2) % 3]); break;
                        case OPMSUB:
                            want = ps2_vu_fmac::subRef(acc[l], ps2_vu_fmac::mulRef(a[(l + 1) % 3], b[(l + 2) % 3]));
                            break;
                        }
                        ++compared;
                        if (got[l] != want && mismatches++ == 0)
                            first = std::string(f.name) + " lane " + std::to_string(l);
                    }
                    if (f.kind == OPMSUB)
                        t.Equals(got[3], dst[3], "OPMSUB writes only xyz");
                }
            }
            std::printf("    micro FMAC forms vs contract: %u lane results over %zu forms\n", compared,
                        sizeof(forms) / sizeof(forms[0]));
            t.Equals(mismatches, 0u, "FMAC form results differing from the contract (first: " + first + ")");
        });

        tc.Run("EFU results match real-PS2 results (ps2autotests vu/lower/efu)", [](TestCase &t)
        {
            struct EfuHardwareVector
            {
                const char *what;
                uint32_t opcode;
                uint32_t input[4];
                uint32_t result;
            };
            static const EfuHardwareVector vectors[] = {
#include "ps2_efu_hw_vectors.inc"
            };
            uint32_t exact = 0u;
            uint32_t total = 0u;
            std::string differences;
            for (const EfuHardwareVector &v : vectors)
            {
                Vu1Fixture fx;
                if (!fx.initialize())
                    continue;
                // op P, vf1 (scalar forms on field z, as efu.cpp runs them); WAITP.
                writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(static_cast<uint8_t>(v.opcode), 1u, 0u, 0u, 2u), kVuUpperNop);
                writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x7Bu, 0u), kVuUpperNop);
                VU1Interpreter vu1;
                std::memcpy(vu1.state().vf[1], v.input, sizeof(v.input));
                vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 80u);
                uint32_t got = 0u;
                std::memcpy(&got, &vu1.state().p, sizeof(got));
                ++total;
                if (got == v.result)
                    ++exact;
                else
                {
                    char line[96];
                    std::snprintf(line, sizeof(line), "    efu %s: hardware %08x, got %08x\n", v.what, v.result, got);
                    differences += line;
                }
            }
            std::printf("    EFU rows exact: %u of %u\n", exact, total);
            if (exact < 168u)
                std::printf("%s", differences.c_str());
            t.Equals(total, 208u, "all 13 ops x 16 inputs should run");
            // The model reproduces 168 rows bit for bit (167 before Phase 5's FMAC-adder series and 24-bit pi/4); the
            // others differ in the last bits of the EATAN/ESIN/EEXP series. This is a floor: a change that loses a
            // hardware-exact row fails here.
            t.IsTrue(exact >= 168u, "at least 168 of 208 EFU rows should match the hardware exactly, got " + std::to_string(exact));
        });

        tc.Run("EFU resource observes throughput separately from P visibility", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuLowerSpecial(0x70u, 1u),
                kVuUpperNop); // ESADD: result at cycle 11, resource free at 10.
            writeVuInstructionPair(
                fx.code, 8u,
                makeVuLowerSpecial(0x72u, 1u),
                kVuUpperNop); // ELENG: must issue at cycle 10.
            writeVuInstructionPair(
                fx.code, 16u,
                makeVuLowerSpecial(0x7Bu, 0u),
                kVuUpperNop); // WAITP waits for ELENG at cycle 28.

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 29u);

            t.IsTrue(vu1.state().p > 3.7f && vu1.state().p < 3.8f,
                     "WAITP should expose the second EFU result");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(29u),
                     "EFU scheduling should use opcode throughput and result latency");
        });

        tc.Run("all EFU opcodes produce P at their architectural latency", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            struct EfuCase
            {
                uint8_t opcode;
                uint32_t latency;
            };
            constexpr EfuCase cases[] = {
                {0x70u, 11u}, {0x71u, 18u}, {0x72u, 18u}, {0x73u, 24u},
                {0x74u, 54u}, {0x75u, 54u}, {0x76u, 12u},
                {0x78u, 12u}, {0x79u, 18u}, {0x7Au, 12u}, {0x7Cu, 29u},
                {0x7Du, 54u}, {0x7Eu, 44u}}; // ESQRT ERSQRT ERCPR ESIN EATAN EEXP (PCSX2 lower-op tables)

            for (const EfuCase &efu : cases)
            {
                std::memset(fx.code, 0, PS2_VU1_CODE_SIZE);
                writeVuInstructionPair(
                    fx.code, 0u,
                    makeVuLowerSpecial(efu.opcode, 1u, 0u, 0u, 0u),
                    kVuUpperNop);
                writeVuInstructionPair(
                    fx.code, 8u,
                    makeVuLowerSpecial(0x7Bu, 0u),
                    kVuUpperNop);

                VU1Interpreter vu1;
                vu1.state().vf[1][0] = 0.25f;
                vu1.state().vf[1][1] = 0.5f;
                vu1.state().vf[1][2] = 0.75f;
                vu1.state().vf[1][3] = 1.0f;
                vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                            fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                            0u, 0u, 0u, efu.latency + 1u);

                t.Equals(vu1.state().cycles,
                         static_cast<uint64_t>(efu.latency + 1u),
                         "WAITP should count every EFU stall as an elapsed VU cycle");
                t.IsTrue(std::isfinite(vu1.state().p),
                         "architected EFU opcode should commit a finite P result");
            }
        });

        tc.Run("E and enabled D/T stop with their architectural delay rules", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                kVuUpperNop | 0x40000000u);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIaddiu(1u, 0u, 7),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 16u, makeVuIaddiu(2u, 0u, 9),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 32u);
            t.Equals(vu1.state().vi[1], 7,
                     "E should execute exactly one sequential delay slot");
            t.Equals(vu1.state().vi[2], 0,
                     "E should stop before the instruction after its delay slot");

            vu1.reset();
            writeTrackedVuInstructionPair(
                fx, 0u, makeVuIaddiu(1u, 0u, 3),
                kVuUpperNop | 0x08000000u);
            writeTrackedVuInstructionPair(
                fx, 8u, makeVuIaddiu(2u, 0u, 5),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 2u);
            t.Equals(vu1.state().vi[2], 5,
                     "T must be ignored while TE is disabled");

            vu1.reset();
            vu1.state().tBitEnabled = true;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 32u);
            t.Equals(vu1.state().vi[1], 3,
                     "T instruction itself should complete");
            t.Equals(vu1.state().vi[2], 0,
                     "enabled T should stop without an ordinary delay slot");
            t.IsTrue(vu1.state().stoppedByT,
                     "the stop reason should identify T");

            vu1.reset();
            vu1.state().dBitEnabled = true;
            writeTrackedVuInstructionPair(
                fx, 0u, makeVuIaddiu(1u, 0u, 4),
                kVuUpperNop | 0x10000000u);
            writeTrackedVuInstructionPair(
                fx, 8u, makeVuIaddiu(2u, 0u, 6),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 32u);
            t.Equals(vu1.state().vi[1], 4,
                     "D instruction itself should complete");
            t.Equals(vu1.state().vi[2], 0,
                     "enabled D should stop without an ordinary delay slot");
            t.IsTrue(vu1.state().stoppedByD,
                     "the stop reason should identify D");

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 1u);
            t.Equals(vu1.state().vi[2], 6,
                     "MSCNT-style resume should continue at the stopped TPC");
            t.IsTrue(!vu1.state().stoppedByD && !vu1.state().stoppedByT,
                     "resuming should clear the previous D/T stop reason");

            vu1.reset();
            vu1.state().tBitEnabled = true;
            writeTrackedVuInstructionPair(
                fx, 0u, makeVuBranch(1),
                kVuUpperNop | 0x08000000u);
            writeTrackedVuInstructionPair(
                fx, 8u, makeVuIaddiu(2u, 0u, 11),
                kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 16u, makeVuIaddiu(3u, 0u, 13),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 32u);
            t.Equals(vu1.state().vi[2], 11,
                     "T on a branch should still execute its branch delay slot");
            t.Equals(vu1.state().vi[3], 0,
                     "T on a branch should stop before executing the branch target");
            t.Equals(vu1.state().pc, 16u,
                     "the stopped TPC should be the branch destination");
        });

        tc.Run("conditional branch sees the previous VI value for one instruction", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, makeVuIaddiu(1u, 0u, 1),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIbne(1u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 16u, makeVuIaddiu(2u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 24u, makeVuIaddiu(3u, 0u, 3),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 32u, makeVuIaddiu(4u, 0u, 4),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vi[2], 2,
                     "the instruction after a conditional branch remains its delay slot");
            t.Equals(vu1.state().vi[3], 3,
                     "an immediately following branch should see the pre-write VI value");

            vu1.reset();
            writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 16u, makeVuIbne(1u, 0u, 2),
                kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 40u, makeVuIaddiu(5u, 0u, 5),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);
            t.Equals(vu1.state().vi[3], 3,
                     "taken branch should still execute its delay slot");
            t.Equals(vu1.state().vi[4], 0,
                     "after one intervening instruction the branch should observe and branch on the new VI value");
            t.Equals(vu1.state().vi[5], 5,
                     "taken branch should arrive at its target after the delay slot");

            vu1.reset();
            writeTrackedVuInstructionPair(
                fx, 0u, makeVuIaddiu(1u, 0u, 1),
                makeVuUpper(0x28u, 0x8u, 2u, 1u, 3u));
            writeTrackedVuInstructionPair(
                fx, 8u, makeVuIbne(1u, 0u, 2),
                makeVuUpper(0x28u, 0x8u, 0u, 3u, 4u));
            writeTrackedVuInstructionPair(
                fx, 16u, makeVuIaddiu(2u, 0u, 2),
                kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 24u, makeVuIaddiu(3u, 0u, 3),
                kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 32u, makeVuIaddiu(4u, 0u, 4),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 7u);
            t.Equals(vu1.state().vi[2], 2,
                     "a stalled conditional branch should retain its delay slot");
            t.Equals(vu1.state().vi[3], 3,
                     "the VI bypass must survive VF hazard stalls before the next pair issues");
            t.Equals(vu1.state().vi[4], 0,
                     "elapsed stall cycles must not expire the one-instruction VI bypass");
        });

        tc.Run("flag checks are visible to an immediately following branch", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuFlagImmediate(0x16u, 1u, 0x001u),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIbne(1u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 16u, makeVuIaddiu(2u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 24u, makeVuIaddiu(3u, 0u, 3),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 32u, makeVuIaddiu(4u, 0u, 4),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().status = 0x001u;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vi[1], 1,
                     "FSAND should publish its result after one cycle");
            t.Equals(vu1.state().vi[2], 2,
                     "the taken branch should still execute its delay slot");
            t.Equals(vu1.state().vi[3], 0,
                     "the immediately following branch must consume the flag-check result");
            t.Equals(vu1.state().vi[4], 4,
                     "the flag-driven branch should arrive at its target");
        });

        // Ported from Sinan-Karakaya/PS2Recomp 7bb6dbde. PCSX2 VUops.cpp _vuJR/_vuJALR read VI[_Is_] directly; only the
        // conditional branches use the delayed VI value (VIBackupCycles), and microVU's mVUanalyzeJump agrees.
        tc.Run("JR reads the latest VI value after an integer write", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, makeVuIaddiu(1u, 0u, 4),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuJr(1u),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 16u, makeVuIaddiu(2u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 24u, makeVuIaddiu(3u, 0u, 3),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 32u, makeVuIaddiu(4u, 0u, 4),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 3;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vi[1], 4,
                     "the pending IALU write should still commit normally");
            t.Equals(vu1.state().vi[2], 2,
                     "JR should execute exactly one delay-slot pair");
            t.Equals(vu1.state().vi[3], 0,
                     "JR should skip the sequential instruction after its delay slot");
            t.Equals(vu1.state().vi[4], 4,
                     "JR must use the updated target, unlike a conditional branch");
        });

        tc.Run("JALR uses the latest target before writing its link register", [](TestCase &t)
        {
            for (uint8_t link : {uint8_t{15}, uint8_t{1}})
            {
                Vu1Fixture fx;
                t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
                writeVuInstructionPair(fx.code, 0u, makeVuIaddiu(1u, 0u, 4), kVuUpperNop);
                const uint32_t jalr = (0x25u << 25u) | (static_cast<uint32_t>(link) << 16u) | (1u << 11u);
                writeVuInstructionPair(fx.code, 8u, jalr, kVuUpperNop);
                writeVuInstructionPair(fx.code, 16u, makeVuIaddiu(2u, 0u, 2), kVuUpperNop);
                writeVuInstructionPair(fx.code, 24u, makeVuIaddiu(3u, 0u, 3), kVuUpperNop);
                writeVuInstructionPair(fx.code, 32u, makeVuIaddiu(4u, 0u, 4), kVuUpperNop);
                VU1Interpreter vu1;
                vu1.state().vi[1] = 3;
                vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                            fx.gs, &fx.mem, 0u, 0u, 0u, 4u);
                t.Equals(vu1.state().vi[link], 3, "JALR links to the instruction after its delay slot");
                t.Equals(vu1.state().vi[2], 2, "JALR must execute its delay slot");
                t.Equals(vu1.state().vi[3], 0, "JALR must not use the stale target or newly written link");
                t.Equals(vu1.state().vi[4], 4, "JALR must reach the updated target");
            }
        });

        tc.Run("MPG upload invalidates cached VU1 decode before MSCAL", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            VU1Interpreter vu1;
            fx.mem.setVu1MscalCallback([&](uint32_t startPC, uint32_t top, uint32_t itop)
            {
                vu1.execute(fx.code,
                            PS2_VU1_CODE_SIZE,
                            fx.data,
                            PS2_VU1_DATA_SIZE,
                            fx.gs,
                            &fx.mem,
                            startPC,
                            top,
                            itop,
                            1u);
            });

            uploadVu1Mpg(fx.mem, 0u, makeVuIaddiu(1u, 0u, 1), kVuUpperNop);
            const uint32_t firstMscal = makeVifCmd(0x14u, 0u, 0u);
            fx.mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&firstMscal), sizeof(firstMscal));
            t.Equals(vu1.state().vi[1], 1, "first MSCAL should execute the first uploaded program");

            uploadVu1Mpg(fx.mem, 0u, makeVuIaddiu(1u, 0u, 2), kVuUpperNop);
            const uint32_t secondMscal = makeVifCmd(0x14u, 0u, 0u);
            fx.mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&secondMscal), sizeof(secondMscal));
            t.Equals(vu1.state().vi[1], 2, "second MSCAL should see the MPG-updated instruction");
        });

        tc.Run("direct VU1 code writes invalidate cached decode", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            VU1Interpreter vu1;
            fx.mem.write64(PS2_VU1_CODE_BASE, packVuInstructionPair(makeVuIaddiu(1u, 0u, 1), kVuUpperNop));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
            t.Equals(vu1.state().vi[1], 1, "first execution should use the original direct write");

            fx.mem.write64(PS2_VU1_CODE_BASE, packVuInstructionPair(makeVuIaddiu(1u, 0u, 2), kVuUpperNop));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
            t.Equals(vu1.state().vi[1], 2, "second execution should rebuild decode after the direct write");
        });

        tc.Run("XGKICK sends a VU memory GIF packet through PATH1", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            uint8_t *vuCode = mem.getVU1Code();
            uint8_t *vuData = mem.getVU1Data();
            std::memset(vuCode, 0, PS2_VU1_CODE_SIZE);
            std::memset(vuData, 0, PS2_VU1_DATA_SIZE);

            constexpr uint32_t kLastQw = (PS2_VU1_DATA_SIZE / 16u) - 1u;
            const uint32_t tagOffset = kLastQw * 16u;

            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(vuData + tagOffset, &imageTag, sizeof(imageTag));

            for (uint32_t i = 0; i < 16u; ++i)
            {
                vuData[i] = static_cast<uint8_t>(0xC0u + i);
            }

            const uint32_t lower = makeVuLowerSpecial(0x6Cu, 1u);
            std::memcpy(vuCode + 0u, &lower, sizeof(lower));
            const uint32_t upper = 0u;
            std::memcpy(vuCode + 4u, &upper, sizeof(upper));

            VU1Interpreter vu1;
            vu1.state().vi[1] = static_cast<int32_t>(kLastQw);
            vu1.execute(vuCode,
                        PS2_VU1_CODE_SIZE,
                        vuData,
                        PS2_VU1_DATA_SIZE,
                        gs,
                        &mem,
                        0u,
                        0u,
                        0u,
                        3u);

            t.Equals(captured.size(), static_cast<size_t>(1u), "XGKICK should emit one wrapped GIF packet");
            if (!captured.empty())
            {
                t.Equals(captured[0].size(), static_cast<size_t>(32u), "wrapped packet should include tag plus one qword payload");
                bool payloadOk = true;
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    if (captured[0].size() < 32u || captured[0][16u + i] != static_cast<uint8_t>(0xC0u + i))
                    {
                        payloadOk = false;
                        break;
                    }
                }
                t.IsTrue(payloadOk, "wrapped payload should be copied from start of VU1 memory");
            }
        });

        tc.Run("XGKICK observes stores committed before a future PATH1 qword", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *packet, uint32_t sizeBytes)
            {
                captured.emplace_back(packet, packet + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0xFF, PS2_VU1_DATA_SIZE);

            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memset(data, 0, 16u);
            std::memcpy(data, &imageTag, sizeof(imageTag));
            writeVuInstructionPair(
                code, 0u,
                makeVuLowerSpecial(0x6Cu, 1u),
                kVuUpperNop);
            writeVuInstructionPair(
                code, 8u,
                makeVuSq(0xFu, 4u, 2u, 0),
                kVuUpperNop);
            writeVuInstructionPair(code, 16u, 0u, kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 0;
            vu1.state().vi[2] = 1;
            const float replacement[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            std::memcpy(vu1.state().vf[4], replacement, sizeof(replacement));
            vu1.execute(code, PS2_VU1_CODE_SIZE,
                        data, PS2_VU1_DATA_SIZE, gs, &mem,
                        0u, 0u, 0u, 3u);

            t.Equals(captured.size(), static_cast<size_t>(1u),
                     "PATH1 should finish after consuming the updated payload");
            if (!captured.empty())
            {
                t.IsTrue(captured[0].size() >= 32u &&
                             std::memcmp(captured[0].data() + 16u,
                                         replacement,
                                         sizeof(replacement)) == 0,
                         "XGKICK must read each qword in its transfer cycle");
            }
        });

        tc.Run("a second XGKICK stalls until the active PATH1 transfer completes", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *packet, uint32_t sizeBytes)
            {
                captured.emplace_back(packet, packet + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);

            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(data + 0u, &imageTag, sizeof(imageTag));
            std::memcpy(data + 32u, &imageTag, sizeof(imageTag));
            std::memset(data + 16u, 0x11, 16u);
            std::memset(data + 48u, 0x22, 16u);
            writeVuInstructionPair(
                code, 0u,
                makeVuLowerSpecial(0x6Cu, 1u),
                kVuUpperNop);
            writeVuInstructionPair(
                code, 8u,
                makeVuLowerSpecial(0x6Cu, 2u),
                kVuUpperNop);
            writeVuInstructionPair(code, 16u, 0u, kVuUpperNop);
            writeVuInstructionPair(code, 24u, 0u, kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 0;
            vu1.state().vi[2] = 2;
            vu1.execute(code, PS2_VU1_CODE_SIZE,
                        data, PS2_VU1_DATA_SIZE, gs, &mem,
                        0u, 0u, 0u, 6u);

            t.Equals(captured.size(), static_cast<size_t>(2u),
                     "both PATH1 transfers should complete in issue order");
            if (captured.size() == 2u)
            {
                t.IsTrue(captured[0].size() >= 32u && captured[0][16u] == 0x11u,
                         "the first XGKICK payload should be delivered first");
                t.IsTrue(captured[1].size() >= 32u && captured[1][16u] == 0x22u,
                         "the stalled XGKICK should retain its own source address");
            }
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(6u),
                     "XGKICK resource stalls should consume VU cycles");
        });

        tc.Run("synthetic Code Veronica text packet preserves black-frame PATH1 data", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);

            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(data, &imageTag, sizeof(imageTag));
            writeVuInstructionPair(
                code, 0u, 0u,
                makeVuUpper(0x28u, 0xFu, 3u, 2u, 4u));
            writeVuInstructionPair(
                code, 8u,
                makeVuSq(0xFu, 4u, 2u, 0),
                kVuUpperNop);
            writeVuInstructionPair(
                code, 16u,
                makeVuLowerSpecial(0x6Cu, 1u),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 0;
            vu1.state().vi[2] = 1;
            const float a[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const float b[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            std::memcpy(vu1.state().vf[2], a, sizeof(a));
            std::memcpy(vu1.state().vf[3], b, sizeof(b));

            vu1.execute(code, PS2_VU1_CODE_SIZE,
                        data, PS2_VU1_DATA_SIZE, gs, &mem,
                        0u, 0u, 0u, 10u);

            t.Equals(captured.size(), static_cast<size_t>(1u),
                     "the synthetic scene should emit exactly one PATH1 packet");
            if (!captured.empty())
            {
                const float expected[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                t.Equals(captured[0].size(), static_cast<size_t>(32u),
                         "text regression packet should contain one image qword");
                t.IsTrue(captured[0].size() >= 32u &&
                             std::memcmp(captured[0].data() + 16u,
                                         expected,
                                         sizeof(expected)) == 0,
                         "PATH1 must observe the post-FMAC store, not stale blue/magenta data");
            }
        });

        tc.Run("MSCAL can start a VU1 XGKICK program and update GS VRAM", [](TestCase &t)
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

            uint8_t *vuCode = mem.getVU1Code();
            uint8_t *vuData = mem.getVU1Data();
            std::memset(vuCode, 0, PS2_VU1_CODE_SIZE);
            std::memset(vuData, 0, PS2_VU1_DATA_SIZE);

            const uint32_t lower = makeVuLowerSpecial(0x6Cu, 0u);
            std::memcpy(vuCode + 0u, &lower, sizeof(lower));
            const uint32_t upper = 0u;
            std::memcpy(vuCode + 4u, &upper, sizeof(upper));

            const uint64_t gifTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(vuData + 0u, &gifTag, sizeof(gifTag));
            const uint64_t tagHi = 0u;
            std::memcpy(vuData + 8u, &tagHi, sizeof(tagHi));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                vuData[16u + i] = static_cast<uint8_t>(0x90u + i);
            }

            VU1Interpreter vu1;
            mem.setVu1MscalCallback([&](uint32_t startPC, uint32_t top, uint32_t itop)
            {
                vu1.execute(vuCode,
                            PS2_VU1_CODE_SIZE,
                            vuData,
                            PS2_VU1_DATA_SIZE,
                            gs,
                            &mem,
                            startPC,
                            top,
                            itop,
                            3u);
            });

            const uint32_t mscalCmd = makeVifCmd(0x14u, 0u, 0u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mscalCmd), sizeof(mscalCmd));

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0x90u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "MSCAL-triggered XGKICK should route PATH1 packet into GS VRAM");
        });

        tc.Run("standalone VU1 code honors the nullable PS2Memory API", [](TestCase &t)
        {
            std::vector<uint8_t> code(8u, 0u);
            std::vector<uint8_t> data(16u, 0u);
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            VU1Interpreter vu1;
            vu1.execute(code.data(), static_cast<uint32_t>(code.size()),
                        data.data(), static_cast<uint32_t>(data.size()),
                        gs, nullptr, 0u, 0u, 0u, 1u);

            t.Equals(vu1.state().pc, 0u,
                     "external code should execute and wrap without dereferencing a null memory tracker");
        });

        tc.Run("VU1 status-immediate ops decode IMM12 and their target VI", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u,
                                   makeVuFlagImmediate(0x14u, 5u, 0x812u),
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 8u,
                                   makeVuFlagImmediate(0x16u, 6u, 0x810u),
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 16u,
                                   makeVuFlagImmediate(0x17u, 7u, 0x040u),
                                   kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().status = 0x812u;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 3u);

            t.Equals(vu1.state().vi[5], 1,
                     "FSEQ should compare all 12 immediate bits and write IT");
            t.Equals(vu1.state().vi[6], 0x810,
                     "FSAND should return the masked 12-bit status in IT");
            t.Equals(vu1.state().vi[7], 0x852,
                     "FSOR should return the 12-bit OR value rather than a boolean");
            t.Equals(vu1.state().vi[1], 0,
                     "status-immediate ops should not hardcode VI1");
        });

        tc.Run("VU1 FSSET enters the four-cycle flag pipeline", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u,
                                   makeVuFlagImmediate(0x15u, 0u, 0xA80u),
                                   kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().status = 0x015u;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 3u);
            t.Equals(vu1.state().status, 0x015u,
                     "FSSET should not be visible before four cycles elapse");

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 1u);
            t.Equals(vu1.state().status, 0xA95u,
                     "FSSET should replace sticky bits while preserving current and D/I bits");
        });

        tc.Run("VU1 has one flag pipeline and FSSET wins a same-pair conflict", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u,
                                   makeVuFlagImmediate(0x15u, 0u, 0xA80u),
                                   makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u));
            for (uint32_t pc = 8u; pc <= 32u; pc += 8u)
                writeVuInstructionPair(fx.code, pc, 0u, kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][2] = -3.0f;
            vu1.state().vf[2][0] = -1.0f;
            vu1.state().vf[2][2] = 1.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().mac, 0x28u,
                     "same-pair FSSET should suppress only STATUS, not the upper MAC result");
            t.Equals(vu1.state().status, 0xA80u,
                     "the single runtime pipeline should commit FSSET sticky bits");
        });

        tc.Run("VU1 FMAC flags respect destination lanes and become visible after four cycles", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, 0u,
                                   makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u));
            writeVuInstructionPair(fx.code, 8u,
                                   makeVuFlagRegister(0x18u, 6u, 7u),
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 16u,
                                   0u,
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 24u,
                                   0u,
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 32u,
                                   makeVuFlagRegister(0x1Au, 4u, 5u),
                                   kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][2] = -3.0f;
            vu1.state().vf[2][0] = -1.0f;
            vu1.state().vf[2][2] = 1.0f;
            vu1.state().vi[5] = 0xFFFF;
            vu1.state().vi[7] = 0;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 3u);

            t.Equals(vu1.state().mac, 0u,
                     "FMAC flags should remain hidden before four cycles elapse");
            t.Equals(vu1.state().vi[6], 1,
                     "FMEQ before the commit cycle should observe the old MAC flags");

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 2u);

            t.Equals(vu1.state().mac, 0x28u,
                     "ADD.xz should report zero on x and sign on z only");
            t.Equals(vu1.state().status, 0xC3u,
                     "FMAC commit should update current Z/S and accumulate their sticky bits");
            t.Equals(vu1.state().vi[4], 0x28,
                      "FMAND on the commit cycle should observe the new MAC flags");
        });

        tc.Run("VU1 CLIP and FCGET share the four-cycle flag timeline", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpperSpecial(0x1Fu, 0u, 2u, 1u));
            writeVuInstructionPair(
                fx.code, 8u,
                makeVuFlagRegister(0x1Cu, 3u, 0u),
                kVuUpperNop);
            writeVuInstructionPair(fx.code, 16u, 0u, kVuUpperNop);
            writeVuInstructionPair(fx.code, 24u, 0u, kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 32u,
                makeVuFlagRegister(0x1Cu, 4u, 0u),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 2.0f;
            vu1.state().vf[2][3] = 1.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().vi[3], 0,
                     "FCGET before cycle four should observe the previous CLIP value");
            t.Equals(vu1.state().vi[4], 1,
                     "FCGET on cycle four should observe the committed +X CLIP bit");
            t.Equals(vu1.state().clip, 1u,
                     "CLIP should shift and commit the six new comparison bits");
        });

        tc.Run("VU1 FMAC normalizes overflow and underflow before writing results", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, 0u,
                                   makeVuUpper(0x2Au, 0xFu, 2u, 1u, 3u));

            // Phase 5: exponent 255 holds values (evidence register 2.1), so the largest VU value is 0x7FFFFFFF and a
            // result overflows at 2^129. The original operands (FLT_MAX * 2 = 0x7FFFFFFF exactly) no longer overflow;
            // 0x7FFFFFFF * 2 = 2^129 does. Which exact magnitude sets O is provisional (no FMAC flag hardware row).
            VU1Interpreter vu1;
            const uint32_t psMax = 0x7FFFFFFFu;
            std::memcpy(&vu1.state().vf[1][0], &psMax, sizeof(psMax));
            vu1.state().vf[1][1] = std::numeric_limits<float>::min();
            vu1.state().vf[1][2] = -2.0f;
            vu1.state().vf[1][3] = 0.0f;
            vu1.state().vf[2][0] = 2.0f;
            vu1.state().vf[2][1] = 0.5f;
            vu1.state().vf[2][2] = 1.0f;
            vu1.state().vf[2][3] = 1.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().mac, 0x8425u,
                     "MUL should report x overflow, y underflow+zero, z sign and w zero");
            t.Equals(vu1.state().status, 0x3CFu,
                     "current and sticky status should summarize Z/S/U/O");
            uint32_t x = 0u;
            std::memcpy(&x, &vu1.state().vf[3][0], sizeof(x));
            t.Equals(x, 0x7FFFFFFFu, "overflow saturates to the largest VU value, 0x7FFFFFFF");
            t.Equals(vu1.state().vf[3][1], 0.0f,
                     "underflow should flush to signed zero before writeback");

            // FLT_MAX * 2 is (2 - 2^-23) * 2^128: an exponent-255 value, written without O.
            VU1Interpreter vu2;
            vu2.state().vf[1][0] = std::numeric_limits<float>::max();
            vu2.state().vf[2][0] = 2.0f;
            vu2.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 5u);
            std::memcpy(&x, &vu2.state().vf[3][0], sizeof(x));
            t.Equals(x, 0x7FFFFFFFu, "FLT_MAX * 2 is the exponent-255 value 0x7FFFFFFF");
            t.Equals(vu2.state().mac & 0x8000u, 0u, "and it is not an overflow");
        });

        tc.Run("FMAC product contributes Z/S/U/O sticky flags", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpper(0x29u, 0x8u, 2u, 1u, 3u)); // MADD.x

            VU1Interpreter vu1;
            vu1.state().acc[0] = 1.0f;
            vu1.state().vf[1][0] = std::numeric_limits<float>::min();
            vu1.state().vf[2][0] = 0.5f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().vf[3][0], 1.0f,
                     "the accumulated FMAC result should remain normal");
            t.Equals(vu1.state().mac, 0u,
                     "MAC flags should describe the final accumulated value");
            t.Equals(vu1.state().status, 0x140u,
                     "the underflowing product should set sticky Z and U");
        });

        tc.Run("VU micro MADD keeps the two-rounding contract value when the fused exact result would overflow", [](TestCase &t)
        {
            // acc = -2^128, product 2^65 * 2^65 = 2^130. The contract (MA) saturates the product to 0x7FFFFFFF and then
            // adds: (2^129 - 2^105) - 2^128 stays in range. The flag path used to compute the fused exact sum (3 * 2^128,
            // an overflow) and overwrite the value with +max, so micro mode disagreed with macro mode.
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuInstructionPair(fx.code, 0u, 0x8000033Cu, makeVuUpper(0x29u, 0x8u, 2u, 1u, 3u) | (1u << 30)); // MADD.x; E
            writeVuInstructionPair(fx.code, 8u, 0x8000033Cu, kVuUpperNop);
            const uint32_t acc = 0xFF800000u, a = 0x60000000u, b = 0x60000000u;
            VU1Interpreter vu1;
            std::memcpy(&vu1.state().acc[0], &acc, sizeof(acc));
            std::memcpy(&vu1.state().vf[1][0], &a, sizeof(a));
            std::memcpy(&vu1.state().vf[2][0], &b, sizeof(b));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
            uint32_t got = 0u;
            std::memcpy(&got, &vu1.state().vf[3][0], sizeof(got));
            const uint32_t want = ps2_vu_fmac::addRef(acc, ps2_vu_fmac::mulRef(a, b));
            t.Equals(got, want, "micro MADD must write the contract value (macro mode computes the same)");
            t.Equals(vu1.state().mac & 0x8000u, 0u, "the final add does not overflow, so no current x O flag");
        });

        tc.Run("VU micro product sticky flags read exponent-255 operands as values", [](TestCase &t)
        {
            // MADD.x with fs = 0x7F800000 (2^128) and ft = 2: the product is 2^129, an overflow; the accumulator -2^128
            // brings the sum back into range. Clamping fs to 0x7F7FFFFF (the old normalizeOperand) hid the product
            // overflow: (2^129 - 2^105) < 2^129.
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuInstructionPair(fx.code, 0u, 0x8000033Cu, makeVuUpper(0x29u, 0x8u, 2u, 1u, 3u) | (1u << 30)); // MADD.x; E
            writeVuInstructionPair(fx.code, 8u, 0x8000033Cu, kVuUpperNop);
            const uint32_t acc = 0xFF800000u, a = 0x7F800000u, b = 0x40000000u;
            VU1Interpreter vu1;
            std::memcpy(&vu1.state().acc[0], &acc, sizeof(acc));
            std::memcpy(&vu1.state().vf[1][0], &a, sizeof(a));
            std::memcpy(&vu1.state().vf[2][0], &b, sizeof(b));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
            uint32_t got = 0u;
            std::memcpy(&got, &vu1.state().vf[3][0], sizeof(got));
            t.Equals(got, ps2_vu_fmac::addRef(acc, ps2_vu_fmac::mulRef(a, b)), "value follows the contract");
            t.Equals(vu1.state().status & 0x8u, 0u, "the final sum does not overflow (no current O)");
            t.Equals(vu1.state().status & 0x200u, 0x200u, "the product overflow sets sticky O");
        });

        tc.Run("reserved opcodes stop before executing or corrupting state", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, 0u, 0x30u);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIaddiu(1u, 0u, 7),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 8u);

            t.Equals(vu1.state().cycles, static_cast<uint64_t>(0u),
                     "reserved opcode should stop before consuming its issue cycle");
            t.Equals(vu1.state().pc, 0u,
                     "reserved opcode should retain the diagnostic PC");
            t.Equals(vu1.state().vi[1], 0,
                     "instruction following a reserved opcode must not execute");
        });
    });
}
