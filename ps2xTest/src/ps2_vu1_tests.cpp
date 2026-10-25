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
            if (exact < 167u)
                std::printf("%s", differences.c_str());
            t.Equals(total, 208u, "all 13 ops x 16 inputs should run");
            // The model reproduces 167 rows bit for bit; the others differ in the last bits of the EATAN/ESIN/EEXP
            // series. This is a floor: a change that loses a hardware-exact row fails here. A higher count is not by
            // itself a reason to change the model (evidence register 2.5).
            t.IsTrue(exact >= 167u, "at least 167 of 208 EFU rows should match the hardware exactly, got " + std::to_string(exact));
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

        tc.Run("consecutive XGKICKs deliver exactly their own bytes (long packet, then a short one)", [](TestCase &t)
        {
            // The XGKICK buffer is reused without clearing; each delivered packet must still be exactly the source
            // qwords of its own transfer: a 4-qword packet followed by a 2-qword one must not carry stale bytes.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *packet, uint32_t sizeBytes) { captured.emplace_back(packet, packet + sizeBytes); });
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);
            const uint64_t longTag = makeGifTag(3u, GIF_FMT_IMAGE, 0u, true);  // tag + 3 qwords
            const uint64_t shortTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true); // tag + 1 qword
            std::memcpy(data + 0u, &longTag, sizeof(longTag));
            for (uint32_t i = 16u; i < 64u; ++i)
                data[i] = static_cast<uint8_t>(0xA0u + i);
            std::memcpy(data + 64u, &shortTag, sizeof(shortTag));
            std::memset(data + 80u, 0x5Au, 16u);
            writeVuInstructionPair(code, 0u, makeVuLowerSpecial(0x6Cu, 1u), kVuUpperNop);  // XGKICK vi1 (qword 0)
            writeVuInstructionPair(code, 8u, makeVuLowerSpecial(0x6Cu, 2u), kVuUpperNop);  // XGKICK vi2 (qword 4)
            writeVuInstructionPair(code, 16u, 0x8000033Cu, kVuUpperNop | (1u << 30));
            writeVuInstructionPair(code, 24u, 0x8000033Cu, kVuUpperNop);
            VU1Interpreter vu1;
            vu1.state().vi[1] = 0;
            vu1.state().vi[2] = 4;
            vu1.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &mem, 0u, 0u, 0u, 200u);
            t.Equals(captured.size(), static_cast<size_t>(2u), "two PATH1 packets");
            if (captured.size() == 2u)
            {
                t.Equals(captured[0].size(), static_cast<size_t>(64u), "first packet is tag + 3 qwords");
                t.IsTrue(std::equal(captured[0].begin(), captured[0].end(), data + 0u), "first packet bytes are its source");
                t.Equals(captured[1].size(), static_cast<size_t>(32u), "second packet is tag + 1 qword");
                t.IsTrue(std::equal(captured[1].begin(), captured[1].end(), data + 64u), "second packet bytes are its source");
            }
        });

        tc.Run("XGKICK buffer: poisoned history, wrap-around, longest packet, oversized packet", [](TestCase &t)
        {
            // The packet buffer is reused between transfers. Each case checks the delivered bytes against an
            // independent construction from VU data memory (source qwords, modulo the 16 KB data memory), after a
            // large transfer has filled the buffer with a poison pattern that must never reappear.
            constexpr size_t kXgkickBufferBytes = 0x10000u; // XgkickPipeline::kBufferSize
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *packet, uint32_t sizeBytes) { captured.emplace_back(packet, packet + sizeBytes); });
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            const auto expectedBytes = [&](uint32_t qword, uint32_t bytes) {
                std::vector<uint8_t> out(bytes);
                for (uint32_t i = 0; i < bytes; ++i)
                    out[i] = data[(qword * 16u + i) % PS2_VU1_DATA_SIZE];
                return out;
            };
            // One program: XGKICK vi1, XGKICK vi2, XGKICK vi3, E bit. Each test case sets the tags and addresses.
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            writeVuInstructionPair(code, 0u, makeVuLowerSpecial(0x6Cu, 1u), kVuUpperNop);
            writeVuInstructionPair(code, 8u, makeVuLowerSpecial(0x6Cu, 2u), kVuUpperNop);
            writeVuInstructionPair(code, 16u, makeVuLowerSpecial(0x6Cu, 3u), kVuUpperNop);
            writeVuInstructionPair(code, 24u, 0x8000033Cu, kVuUpperNop | (1u << 30));
            writeVuInstructionPair(code, 32u, 0x8000033Cu, kVuUpperNop);
            VU1Interpreter vu1; // reused across cases: state must not leak between executions
            const auto run = [&](uint32_t q1, uint32_t q2, uint32_t q3) {
                captured.clear();
                vu1.state().vi[1] = static_cast<int32_t>(q1);
                vu1.state().vi[2] = static_cast<int32_t>(q2);
                vu1.state().vi[3] = static_cast<int32_t>(q3);
                vu1.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &mem, 0u, 0u, 0u, 40000u);
            };
            const auto setTag = [&](uint32_t qword, uint16_t nloop) {
                const uint64_t tag = makeGifTag(nloop, GIF_FMT_IMAGE, 0u, true);
                std::memcpy(data + qword * 16u, &tag, sizeof(tag));
                std::memset(data + qword * 16u + 8u, 0, 8u);
            };

            // Case 1: a 512-qword poison packet (0xCC), then a 2-qword and a 1-qword-payload packet.
            std::memset(data, 0xCC, PS2_VU1_DATA_SIZE);
            setTag(0u, 511u);
            setTag(600u, 1u);
            std::memset(data + 601u * 16u, 0x31, 16u);
            setTag(610u, 2u);
            std::memset(data + 611u * 16u, 0x42, 32u);
            run(0u, 600u, 610u);
            t.Equals(captured.size(), static_cast<size_t>(3u), "three packets after the poison packet");
            if (captured.size() == 3u)
            {
                t.IsTrue(captured[0] == expectedBytes(0u, 512u * 16u), "poison packet delivered whole");
                t.IsTrue(captured[1] == expectedBytes(600u, 32u), "short packet: exact bytes, no poison");
                t.IsTrue(captured[2] == expectedBytes(610u, 48u), "second short packet: exact bytes, no poison");
            }

            // Case 2: a packet that starts at the last qword of data memory wraps to qword 0.
            std::memset(data, 0x00, PS2_VU1_DATA_SIZE);
            for (uint32_t i = 0; i < 64u; ++i)
                data[i] = static_cast<uint8_t>(0x80u + i);
            setTag(1023u, 3u); // tag at the last qword, payload qwords 0..2
            setTag(100u, 1u);
            setTag(200u, 1u);
            run(1023u, 100u, 200u);
            t.Equals(captured.size(), static_cast<size_t>(3u), "wrap-around case delivers three packets");
            if (!captured.empty())
                t.IsTrue(captured[0] == expectedBytes(1023u, 64u), "wrapped packet = last qword + qwords 0..2");

            // Case 3: the longest legal packet, tag + 4095 qwords = the whole 64 KB buffer (source wraps four times),
            // right before the E bit: the pipeline drain must deliver it completely within its bound.
            for (uint32_t i = 0; i < PS2_VU1_DATA_SIZE; ++i)
                data[i] = static_cast<uint8_t>(i * 7u + 3u);
            setTag(0u, 4095u);
            setTag(10u, 1u);
            setTag(20u, 1u);
            run(0u, 10u, 20u);
            t.Equals(captured.size(), static_cast<size_t>(3u), "longest packet and two short ones delivered");
            if (!captured.empty())
            {
                t.Equals(captured[0].size(), static_cast<size_t>(kXgkickBufferBytes), "the full buffer is delivered");
                t.IsTrue(captured[0] == expectedBytes(0u, kXgkickBufferBytes), "longest packet bytes are its wrapped source");
            }

            // Case 4: one qword more than the buffer is refused and nothing is delivered for it.
            setTag(0u, 4096u);
            run(0u, 10u, 20u);
            t.IsTrue(captured.empty() || captured[0].size() != static_cast<size_t>(kXgkickBufferBytes + 16u),
                     "an oversized packet is never delivered");
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

        tc.Run("VU micro FMAC lanes stay independent (MSVC 14.51 lane-w regression)", [](TestCase &t)
        {
            // Observed on the MSVC 14.51 CI build (PR #3, a465d72): with a per-lane loop over the scalar kernel, every
            // lane received lane w's result: ADD of (1,2,3,4) + (10,20,30,40) wrote 44 everywhere. Distinct values per
            // lane for one form of each operand kind (vt, bc, q, i, ACC) catch that pattern directly.
            struct Case { const char *name; uint32_t upper; bool toAcc; float want[4]; };
            const Case cases[] = {
                {"ADD", makeVuUpper(0x28u, 0xFu, 2u, 1u, 3u), false, {11.0f, 22.0f, 33.0f, 44.0f}},
                {"ADDbc.y", makeVuUpper(0x01u, 0xFu, 2u, 1u, 3u), false, {21.0f, 22.0f, 23.0f, 24.0f}},
                {"MULq", makeVuUpper(0x1Cu, 0xFu, 2u, 1u, 3u), false, {0.5f, 1.0f, 1.5f, 2.0f}},
                {"SUBi", makeVuUpper(0x26u, 0xFu, 2u, 1u, 3u), false, {-2.0f, -1.0f, 0.0f, 1.0f}},
                {"MADD", makeVuUpper(0x29u, 0xFu, 2u, 1u, 3u), false, {110.0f, 240.0f, 390.0f, 560.0f}},
                {"MULA", makeVuUpper(0x3Eu, 0xFu, 2u, 1u, 0x0Au), true, {10.0f, 40.0f, 90.0f, 160.0f}},
            };
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            for (const Case &c : cases)
            {
                writeVuInstructionPair(fx.code, 0u, 0x8000033Cu, c.upper | (1u << 30));
                writeVuInstructionPair(fx.code, 8u, 0x8000033Cu, kVuUpperNop);
                VU1Interpreter vu1;
                for (int l = 0; l < 4; ++l)
                {
                    vu1.state().vf[1][l] = static_cast<float>(l + 1);
                    vu1.state().vf[2][l] = static_cast<float>(10 * (l + 1));
                    vu1.state().acc[l] = static_cast<float>(100 * (l + 1));
                }
                vu1.state().q = 0.5f;
                vu1.state().i = 3.0f;
                vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
                const float *got = c.toAcc ? vu1.state().acc : vu1.state().vf[3];
                for (int l = 0; l < 4; ++l)
                    t.Equals(got[l], c.want[l], std::string(c.name) + " lane " + "xyzw"[l]);
            }
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

        tc.Run("VU1 commit index matches the reference cycle by cycle (same-cycle events, reloads, restarts)", [](TestCase &t)
        {
            // Lockstep: the indexed interpreter and the full-scan reference advance one cycle at a time (resume with a
            // budget of 1) and the complete architectural state plus data memory is compared after every cycle, so an
            // event committed one cycle early or late, twice, or out of order shows at the cycle it happens.
            constexpr uint32_t kLowerNop = 0x8000033Cu;
            struct Program { const char *name; std::vector<std::pair<uint32_t, uint32_t>> pairs; };
            std::vector<Program> programs;
            // Four pipelines due on the same cycle 11: ESADD (P, 11) at 0, DIV (Q, 7) at 4, ADD (VF + flags, 4) at 7,
            // SQ (store, 1) at 10.
            programs.push_back({"same-cycle commits", {
                {makeVuLowerSpecial(0x70u, 1u, 0u, 0u, 0xEu), kVuUpperNop},                      // 0  ESADD P
                {kLowerNop, kVuUpperNop}, {kLowerNop, kVuUpperNop}, {kLowerNop, kVuUpperNop},
                {makeVuDiv(1u, 2u, 0u, 1u), kVuUpperNop},                                       // 4  DIV Q
                {kLowerNop, kVuUpperNop}, {kLowerNop, kVuUpperNop},
                {kLowerNop, makeVuUpper(0x28u, 0xFu, 2u, 1u, 3u)},                              // 7  ADD vf3
                {kLowerNop, kVuUpperNop}, {kLowerNop, kVuUpperNop},
                {makeVuSq(0xFu, 1u, 0u, 5), kVuUpperNop},                                       // 10 SQ vf1
                {kLowerNop, kVuUpperNop}, {kLowerNop, kVuUpperNop | (1u << 30)}, {kLowerNop, kVuUpperNop}}});
            // Repeated writes to one register from different pipelines: the latest-sequence rule decides.
            programs.push_back({"overlapping VF writes", {
                {makeVuLq(0xFu, 4u, 0u, 1), makeVuUpper(0x2Au, 0xFu, 2u, 1u, 4u)},             // LQ vf4 | MUL vf4
                {makeVuLq(0xFu, 4u, 0u, 2), kVuUpperNop},
                {kLowerNop, makeVuUpper(0x28u, 0xFu, 4u, 4u, 4u)},                              // ADD vf4 = vf4 + vf4
                {makeVuLq(0xAu, 4u, 0u, 3), kVuUpperNop},
                {kLowerNop, makeVuUpper(0x2Cu, 0x5u, 1u, 4u, 4u)},
                {kLowerNop, kVuUpperNop | (1u << 30)}, {kLowerNop, kVuUpperNop}}});
            // A random program: generated below.
            uint64_t s = 0x9E3779B97F4A7C15ull;
            auto next = [&s]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<uint32_t>(s >> 16); };
            for (int p = 0; p < 150; ++p)
            {
                Program prog{"random", {}};
                const uint32_t count = 6u + next() % 30u;
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint32_t lower = kLowerNop, upper = kVuUpperNop;
                    switch (next() % 6u)
                    {
                    case 0: lower = makeVuLq(0xFu, static_cast<uint8_t>(1u + next() % 8u), 0u, static_cast<int16_t>(next() % 32u)); break;
                    case 1: lower = makeVuSq(0xFu, static_cast<uint8_t>(next() % 8u), 0u, static_cast<int16_t>(next() % 32u)); break;
                    case 2: lower = makeVuDiv(static_cast<uint8_t>(1u + next() % 7u), static_cast<uint8_t>(1u + next() % 7u), static_cast<uint8_t>(next() % 4u), static_cast<uint8_t>(next() % 4u)); break;
                    case 3: lower = makeVuLowerSpecial(static_cast<uint8_t>(0x70u + next() % 3u), static_cast<uint8_t>(1u + next() % 7u), 0u, 0u, 0xEu); break;
                    case 4: lower = makeVuLowerSpecial(static_cast<uint8_t>(next() % 2u ? 0x3Bu : 0x7Bu), 0u); break; // WAITQ/WAITP
                    default: break;
                    }
                    if (next() % 2u)
                        upper = makeVuUpper(static_cast<uint8_t>(0x28u + next() % 6u), static_cast<uint8_t>(1u + next() % 15u),
                                            static_cast<uint8_t>(next() % 8u), static_cast<uint8_t>(next() % 8u),
                                            static_cast<uint8_t>(1u + next() % 7u));
                    prog.pairs.emplace_back(lower, upper);
                }
                prog.pairs.emplace_back(kLowerNop, kVuUpperNop | (1u << 30));
                prog.pairs.emplace_back(kLowerNop, kVuUpperNop);
                programs.push_back(std::move(prog));
            }

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            uint32_t comparedCycles = 0, mismatches = 0;
            std::string first;
            for (size_t index = 0; index < programs.size(); ++index)
            {
                const Program &prog = programs[index];
                std::memset(fx.code, 0, PS2_VU1_CODE_SIZE);
                for (size_t i = 0; i < prog.pairs.size(); ++i)
                    writeVuInstructionPair(fx.code, static_cast<uint32_t>(i * 8u), prog.pairs[i].first, prog.pairs[i].second);
                std::vector<uint8_t> dataA(PS2_VU1_DATA_SIZE), dataB(PS2_VU1_DATA_SIZE);
                for (size_t i = 0; i < dataA.size(); ++i)
                    dataA[i] = static_cast<uint8_t>((i * 37u + index) ^ (i >> 4));
                dataB = dataA;
                VU1Interpreter indexed, reference;
                reference.setReferenceCommitMode(true);
                for (VU1Interpreter *vu : {&indexed, &reference})
                    for (int r = 1; r < 8; ++r)
                        for (int l = 0; l < 4; ++l)
                            vu->state().vf[r][l] = static_cast<float>(r * 4 + l + 1) * (l % 2 ? -0.75f : 1.25f);
                // Restart check: a partial run, then execute() again from the start (resets the pipelines).
                indexed.execute(fx.code, PS2_VU1_CODE_SIZE, dataA.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 3u);
                reference.execute(fx.code, PS2_VU1_CODE_SIZE, dataB.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 3u);
                indexed.execute(fx.code, PS2_VU1_CODE_SIZE, dataA.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
                reference.execute(fx.code, PS2_VU1_CODE_SIZE, dataB.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
                for (int cycle = 0; cycle < 600; ++cycle)
                {
                    const VU1State &p = indexed.state(), &q = reference.state();
                    const bool same = std::memcmp(p.vf, q.vf, sizeof(p.vf)) == 0 && std::memcmp(p.vi, q.vi, sizeof(p.vi)) == 0 &&
                                      std::memcmp(p.acc, q.acc, sizeof(p.acc)) == 0 && std::memcmp(&p.q, &q.q, 4) == 0 &&
                                      std::memcmp(&p.p, &q.p, 4) == 0 && p.mac == q.mac && p.status == q.status &&
                                      p.clip == q.clip && p.pc == q.pc && p.cycles == q.cycles && dataA == dataB;
                    ++comparedCycles;
                    if (!same && mismatches++ == 0)
                        first = std::string(prog.name) + " #" + std::to_string(index) + " cycle " + std::to_string(p.cycles);
                    indexed.resume(fx.code, PS2_VU1_CODE_SIZE, dataA.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 1u);
                    reference.resume(fx.code, PS2_VU1_CODE_SIZE, dataB.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 1u);
                }
                if (index == 0)
                {
                    // All four same-cycle commits happened: P, Q, vf3 and the stored qword are no longer defaults.
                    t.IsTrue(indexed.state().p != 0.0f, "ESADD committed P");
                    t.IsTrue(indexed.state().q != 1.0f, "DIV committed Q");
                    t.IsTrue(indexed.state().vf[3][0] != 0.0f, "ADD committed vf3");
                    t.IsTrue(std::memcmp(dataA.data() + 5 * 16, indexed.state().vf[1], 16) == 0, "SQ committed the store");
                }
            }
            std::printf("    lockstep: %zu programs, %u compared cycles, %u mismatches\n", programs.size(), comparedCycles, mismatches);
            t.Equals(mismatches, 0u, "indexed and reference must agree after every cycle (first: " + first + ")");
        });

        tc.Run("VU1 commit index matches the full-scan reference on random programs (state, cycles, memory)", [](TestCase &t)
        {
            // Differential test of the pipeline commit index (m_nextReady): each random straight-line program (with
            // forward branches) runs twice from the same state - once indexed, once in reference mode, which scans every
            // pipeline on every call as the interpreter did before the index - and the complete architectural state,
            // the cycle count and VU data memory must be identical, both at an interrupted point (cycle budget) and at
            // the end. The instruction mix covers FMAC (all operand kinds, ACC forms), FDIV/EFU with WAITQ/WAITP,
            // flag instructions, integer ops, loads/stores (immediate, post-increment, pre-decrement) and I-bit pairs.
            constexpr uint32_t kLowerNop = 0x8000033Cu;
            uint64_t s = 0x2545F4914F6CDD1Dull;
            auto next = [&s]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<uint32_t>(s >> 16); };
            auto pick = [&next](uint32_t n) { return next() % n; };
            const auto upperOp = [&]() -> uint32_t {
                const uint8_t dest = static_cast<uint8_t>(1u + pick(15u));
                const uint8_t ft = static_cast<uint8_t>(pick(32u)), fs = static_cast<uint8_t>(pick(32u));
                const uint8_t fd = static_cast<uint8_t>(1u + pick(31u));
                switch (pick(4u))
                {
                case 0: return makeVuUpper(static_cast<uint8_t>(pick(0x30u)), dest, ft, fs, fd);            // ADD..MINI
                case 1:
                {
                    static const uint8_t specials[] = {0x00, 0x01, 0x04, 0x05, 0x08, 0x0A, 0x0C, 0x0F, 0x10, 0x11, 0x14,
                                                       0x15, 0x18, 0x19, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
                                                       0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2C, 0x2D, 0x2E};
                    return makeVuUpperSpecial(specials[pick(sizeof(specials))], dest, ft, fs);
                }
                default: return kVuUpperNop;
                }
            };
            const auto lowerOp = [&](uint32_t index, uint32_t count) -> uint32_t {
                const uint8_t dest = static_cast<uint8_t>(1u + pick(15u));
                const uint8_t a = static_cast<uint8_t>(1u + pick(31u)), b = static_cast<uint8_t>(pick(32u));
                const uint8_t vi = static_cast<uint8_t>(1u + pick(4u)), vj = static_cast<uint8_t>(1u + pick(15u));
                const uint8_t fsf = static_cast<uint8_t>(pick(4u)), ftf = static_cast<uint8_t>(pick(4u));
                switch (pick(22u))
                {
                case 0: return makeVuLq(dest, a, vi, static_cast<int16_t>(pick(48u)));
                case 1: return makeVuSq(dest, b, vi, static_cast<int16_t>(pick(48u)));
                case 2: return makeVuIaddiu(vj, static_cast<uint8_t>(pick(16u)), static_cast<int16_t>(pick(32u)));
                case 3: return makeVuLowerDirect(0x30u + static_cast<uint8_t>(pick(2u)), vj, vi, vj);  // IADD/ISUB
                case 4: return makeVuLowerDirect(0x34u + static_cast<uint8_t>(pick(2u)), vj, vi, vj);  // IAND/IOR
                case 5: return makeVuLowerSpecial(0x30u + static_cast<uint8_t>(pick(2u)), b, a, 0u, dest); // MOVE/MR32
                case 6: return makeVuDiv(b, static_cast<uint8_t>(pick(32u)), fsf, ftf);
                case 7: return makeVuSqrt(b, ftf);
                case 8: return makeVuLowerSpecial(0x3Au, b, static_cast<uint8_t>(pick(32u)), 0u, static_cast<uint8_t>((ftf << 2) | fsf)); // RSQRT
                case 9: return makeVuLowerSpecial(0x3Bu, 0u);                                          // WAITQ
                case 10: return makeVuLowerSpecial(0x7Bu, 0u);                                         // WAITP
                case 11: return makeVuLowerSpecial(static_cast<uint8_t>(0x70u + pick(7u)), b, 0u, 0u, 0xEu); // ESADD..ESUM
                case 12: return makeVuLowerSpecial(static_cast<uint8_t>(0x78u + pick(3u)), b, 0u, 0u, static_cast<uint8_t>(fsf)); // ESQRT/ERSQRT/ERCPR
                case 13: return makeVuLowerSpecial(0x64u, 0u, a, 0u, dest);                            // MFP
                case 14: return makeVuFlagImmediate(static_cast<uint8_t>(0x10u + pick(4u)), vj, static_cast<uint16_t>(next() & 0xFFFu)); // FC*
                case 15: return makeVuFlagImmediate(static_cast<uint8_t>(0x14u + pick(4u)), vj, static_cast<uint16_t>(next() & 0xFFFu)); // FS*
                case 16: { static const uint8_t fm[] = {0x18u, 0x1Au, 0x1Bu, 0x1Cu}; return makeVuFlagRegister(fm[pick(4u)], vj, vi); } // FMEQ/FMAND/FMOR/FCGET
                case 17: return makeVuLowerSpecial(static_cast<uint8_t>(0x34u + pick(4u)), vi, a, 0u, dest); // LQI/SQI/LQD/SQD
                case 18: return makeVuIlw(dest, vj, vi, static_cast<int16_t>(pick(48u)));
                case 19:
                {
                    if (index + 3u >= count)
                        return kLowerNop;
                    return makeVuBranch(static_cast<int16_t>(pick(3u))); // forward, lands inside the program
                }
                case 20: return makeVuLowerSpecial(static_cast<uint8_t>(0x3Cu + pick(2u)), vj, b, 0u, static_cast<uint8_t>(8u >> pick(4u))); // MTIR/MFIR
                default: return kLowerNop;
                }
            };

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            std::vector<uint8_t> dataA(PS2_VU1_DATA_SIZE), dataB(PS2_VU1_DATA_SIZE);
            uint32_t programs = 0, mismatched = 0, ended = 0;
            std::string first;
            const auto compare = [&](const VU1Interpreter &x, const VU1Interpreter &y, const char *where, uint32_t program) {
                const VU1State &p = x.state(), &q = y.state();
                const bool same = std::memcmp(p.vf, q.vf, sizeof(p.vf)) == 0 && std::memcmp(p.vi, q.vi, sizeof(p.vi)) == 0 &&
                                  std::memcmp(p.acc, q.acc, sizeof(p.acc)) == 0 && std::memcmp(&p.q, &q.q, 4) == 0 &&
                                  std::memcmp(&p.p, &q.p, 4) == 0 && std::memcmp(&p.i, &q.i, 4) == 0 && p.r == q.r &&
                                  p.pc == q.pc && p.mac == q.mac && p.clip == q.clip && p.status == q.status &&
                                  p.cycles == q.cycles && p.ebit == q.ebit && p.branchPending == q.branchPending &&
                                  dataA == dataB;
                if (!same && mismatched++ == 0)
                    first = "program " + std::to_string(program) + " at " + where + ": cycles " + std::to_string(p.cycles) +
                            "/" + std::to_string(q.cycles) + " status " + std::to_string(p.status) + "/" +
                            std::to_string(q.status) + " mac " + std::to_string(p.mac) + "/" + std::to_string(q.mac);
            };
            for (uint32_t program = 0; program < 3000u; ++program)
            {
                const uint32_t count = 8u + pick(40u);
                std::memset(fx.code, 0, PS2_VU1_CODE_SIZE);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const bool iBit = pick(16u) == 0u;
                    const uint32_t upper = upperOp() | (iBit ? 0x80000000u : 0u);
                    const uint32_t lower = iBit ? ((next() & 0x807FFFFFu) | ((100u + pick(56u)) << 23)) : lowerOp(i, count);
                    writeVuInstructionPair(fx.code, i * 8u, lower, upper);
                }
                writeVuInstructionPair(fx.code, count * 8u, kLowerNop, kVuUpperNop | (1u << 30));
                writeVuInstructionPair(fx.code, count * 8u + 8u, kLowerNop, kVuUpperNop);

                VU1Interpreter indexed, reference;
                reference.setReferenceCommitMode(true);
                for (VU1Interpreter *vu : {&indexed, &reference})
                {
                    VU1State &st = vu->state();
                    uint64_t seed = s;
                    auto value = [&seed]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return static_cast<uint32_t>(seed >> 16); };
                    for (int r = 1; r < 32; ++r)
                        for (int l = 0; l < 4; ++l)
                        {
                            const uint32_t bits = (value() & 0x807FFFFFu) | ((value() % 8u == 0u ? value() % 256u : 100u + value() % 56u) << 23);
                            std::memcpy(&st.vf[r][l], &bits, 4);
                        }
                    for (int r = 1; r < 16; ++r)
                        st.vi[r] = static_cast<int32_t>(value() % 64u);
                    for (int l = 0; l < 4; ++l)
                    {
                        const uint32_t bits = (value() & 0x807FFFFFu) | ((100u + value() % 56u) << 23);
                        std::memcpy(&st.acc[l], &bits, 4);
                    }
                    const uint32_t ib = (value() & 0x807FFFFFu) | (127u << 23);
                    std::memcpy(&st.i, &ib, 4);
                }
                for (size_t i = 0; i < dataA.size(); ++i)
                    dataA[i] = static_cast<uint8_t>(next());
                dataB = dataA;
                next(); // advance the shared stream past the register seed
                const uint32_t budget = 20u + pick(200u);
                indexed.execute(fx.code, PS2_VU1_CODE_SIZE, dataA.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, budget);
                reference.execute(fx.code, PS2_VU1_CODE_SIZE, dataB.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, budget);
                compare(indexed, reference, "budget", program);
                indexed.resume(fx.code, PS2_VU1_CODE_SIZE, dataA.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 20000u);
                reference.resume(fx.code, PS2_VU1_CODE_SIZE, dataB.data(), PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 20000u);
                compare(indexed, reference, "end", program);
                ended += indexed.state().cycles < 20000u + budget ? 1u : 0u;
                ++programs;
            }
            std::printf("    commit index vs reference: %u programs, %u finished within budget, %u mismatches\n", programs,
                        ended, mismatched);
            t.Equals(mismatched, 0u, "indexed and reference runs must agree (first: " + first + ")");
            t.IsTrue(ended > programs / 2u, "most random programs must run to their E bit");
        });
    });
}
