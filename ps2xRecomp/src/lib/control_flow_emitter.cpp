#include "ps2recomp/Emitters/control_flow_emitter.h"

#include "ps2recomp/control_flow_utils.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2_runtime_calls.h"

#include <algorithm>
#include <fmt/format.h>
#include <sstream>
#include <utility>

namespace ps2recomp
{
    ControlFlowEmitter::ControlFlowEmitter(CodeGenerator &generator,
                                           const Instruction &branchInst,
                                           const Instruction &delaySlot,
                                           const Function &function,
                                           const CodeGenerator::AnalysisResult &analysisResult,
                                           std::string delaySlotOverride)
        : m_gen(generator),
          m_branchInst(branchInst),
          m_delaySlot(delaySlot),
          m_function(function),
          m_analysisResult(analysisResult),
          m_delaySlotOverride(std::move(delaySlotOverride))
    {
    }

    uint32_t ControlFlowEmitter::branchPc() const
    {
        return m_branchInst.address;
    }

    uint32_t ControlFlowEmitter::delayPc() const
    {
        return m_branchInst.address + 4u;
    }

    uint32_t ControlFlowEmitter::fallthroughPc() const
    {
        return m_branchInst.address + 8u;
    }

    bool ControlFlowEmitter::hasRealDelaySlot() const
    {
        return !m_delaySlotOverride.empty() || !isGuestNop(m_delaySlot);
    }

    bool ControlFlowEmitter::isCallLikeEdge() const
    {
        return (m_branchInst.opcode == OPCODE_JAL) ||
               (m_branchInst.opcode == OPCODE_SPECIAL && m_branchInst.function == SPECIAL_JALR);
    }

    bool ControlFlowEmitter::isInternalTarget(uint32_t target) const
    {
        return m_analysisResult.entryPoints.contains(target);
    }

    bool ControlFlowEmitter::isLikelyBranch() const
    {
        return (m_branchInst.opcode == OPCODE_BEQL || m_branchInst.opcode == OPCODE_BNEL ||
                m_branchInst.opcode == OPCODE_BLEZL || m_branchInst.opcode == OPCODE_BGTZL ||
                (m_branchInst.opcode == OPCODE_REGIMM &&
                 (m_branchInst.rt == REGIMM_BLTZL || m_branchInst.rt == REGIMM_BGEZL ||
                  m_branchInst.rt == REGIMM_BLTZALL || m_branchInst.rt == REGIMM_BGEZALL)) ||
                (m_branchInst.opcode == OPCODE_COP0 && m_branchInst.rs == COP0_BC &&
                 (m_branchInst.rt == COP0_BC_BCFL || m_branchInst.rt == COP0_BC_BCTL)) ||
                (m_branchInst.opcode == OPCODE_COP1 && m_branchInst.rs == COP1_BC &&
                 (m_branchInst.rt == COP1_BC_BCFL || m_branchInst.rt == COP1_BC_BCTL)) ||
                (m_branchInst.opcode == OPCODE_COP2 && m_branchInst.rs == COP2_BC &&
                 (m_branchInst.rt == COP2_BC_BCFL || m_branchInst.rt == COP2_BC_BCTL)));
    }

    std::vector<uint32_t> ControlFlowEmitter::resolvedLocalIndirectTargets() const
    {
        if (m_branchInst.opcode != OPCODE_SPECIAL)
        {
            return {};
        }

        if (!((m_branchInst.function == SPECIAL_JR && m_branchInst.rs != 31u) ||
              m_branchInst.function == SPECIAL_JALR))
        {
            return {};
        }

        auto jtIt = m_analysisResult.jumpTableTargets.find(m_branchInst.address);
        if (jtIt == m_analysisResult.jumpTableTargets.end())
        {
            return {};
        }

        std::vector<uint32_t> targets = jtIt->second;
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        return targets;
    }

    std::string ControlFlowEmitter::delaySlotCode() const
    {
        if (!hasRealDelaySlot())
        {
            return {};
        }

        if (!m_delaySlotOverride.empty())
        {
            return m_delaySlotOverride;
        }

        std::string code;
        if (m_gen.m_emitInstructionComments)
        {
            code = "// 0x" + fmt::format("{:x}", m_delaySlot.address) + ": 0x" + fmt::format("{:x}", m_delaySlot.raw);
            std::string disassembly = R5900Decoder::disassembleInstruction(m_delaySlot);
            if (!disassembly.empty())
            {
                code += "  " + disassembly;
            }
            code += " (Delay Slot)\n";
        }

        code += m_gen.translateInstruction(m_delaySlot);
        return code;
    }

    void ControlFlowEmitter::emitDelaySlot(std::string_view indent)
    {
        if (!hasRealDelaySlot())
        {
            return;
        }

        m_ss << fmt::format("{}ctx->pc = 0x{:X}u;\n", indent, delayPc());
        m_ss << fmt::format("{}ctx->in_delay_slot = true;\n", indent);
        m_ss << fmt::format("{}ctx->branch_pc = 0x{:X}u;\n", indent, branchPc());

        const std::string code = delaySlotCode();
        std::istringstream lines(code);
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty())
            {
                m_ss << indent << line << "\n";
            }
        }

        m_ss << fmt::format("{}ctx->in_delay_slot = false;\n", indent);
    }

    void ControlFlowEmitter::emitResumeFromDelaySlotEntry()
    {
        if (!isInternalTarget(delayPc()))
        {
            return;
        }

        m_ss << fmt::format("    if (ctx->pc == 0x{:X}u) {{\n", delayPc());
        emitDelaySlot("        ");
        m_ss << fmt::format("        ctx->pc = 0x{:X}u;\n", fallthroughPc());

        if (isInternalTarget(fallthroughPc()))
        {
            m_ss << fmt::format("        goto label_{:x};\n", fallthroughPc());
        }
        else
        {
            m_ss << fmt::format("        goto label_fallthrough_0x{:x};\n", branchPc());
        }

        m_ss << "    }\n";
    }

    void ControlFlowEmitter::emitInternalTarget(uint32_t target, uint32_t sourcePc, std::string_view indent)
    {
        m_ss << fmt::format("{}ctx->pc = 0x{:X}u;\n", indent, target);
        if (target <= sourcePc && !isCallLikeEdge())
        {
            m_ss << fmt::format("{}if (runtime->eeCheckpointDue()) {{\n", indent);
            m_ss << fmt::format("{}    return;\n", indent);
            m_ss << fmt::format("{}}}\n", indent);
        }
        m_ss << fmt::format("{}goto label_{:x};\n", indent, target);
    }

    void ControlFlowEmitter::emitRuntimeBranchDispatch(std::string_view targetExpression,
                                                       uint32_t sourcePc,
                                                       uint32_t returnPc,
                                                       std::string_view runtimeKind,
                                                       std::string_view debugName,
                                                       std::string_view indent,
                                                       bool returnOnTransfer)
    {
        m_ss << fmt::format(
            "{}if (!runtime->dispatchGuestBranch(rdram, ctx, {}, 0x{:X}u, 0x{:X}u, PS2Runtime::GuestBranchKind::{}, \"{}\")) {{\n",
            indent,
            targetExpression,
            sourcePc,
            returnPc,
            runtimeKind,
            debugName);
        if (returnOnTransfer)
        {
            m_ss << fmt::format("{}    return;\n", indent);
        }
        m_ss << fmt::format("{}}}\n", indent);
    }

    bool ControlFlowEmitter::emitDirectFunctionJumpIfAvailable(uint32_t target, StaticBranchKind kind, std::string_view indent)
    {
        if (kind != StaticBranchKind::Jump)
        {
            return false;
        }

        const std::string functionName = m_gen.getFunctionName(target);
        if (functionName.empty())
        {
            return false;
        }

        m_ss << indent << functionName << "(rdram, ctx, runtime); return;\n";
        return true;
    }

    void ControlFlowEmitter::emitExternalJumpDispatch(uint32_t target, StaticBranchKind kind, std::string_view indent)
    {
        const bool isCall = kind == StaticBranchKind::Call;
        emitRuntimeBranchDispatch(fmt::format("0x{:X}u", target),
                                  branchPc(),
                                  isCall ? fallthroughPc() : 0u,
                                  isCall ? "DirectCall" : "DirectJump",
                                  isCall ? "JAL" : "J",
                                  indent,
                                  true);
    }

    void ControlFlowEmitter::emitExternalRegisterCallDispatch(std::string_view jumpTargetExpression, std::string_view indent)
    {
        emitRuntimeBranchDispatch(jumpTargetExpression,
                                  branchPc(),
                                  fallthroughPc(),
                                  "IndirectCall",
                                  "JALR",
                                  indent,
                                  true);
    }

    void ControlFlowEmitter::emitExternalRegisterJumpDispatch(std::string_view jumpTargetExpression,
                                                              RegisterBranchKind kind,
                                                              uint8_t rsReg,
                                                              std::string_view indent)
    {
        const bool isReturn = kind == RegisterBranchKind::Jump && rsReg == 31u;

        if (isReturn)
        {
            m_ss << indent << "#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS\n";
            m_ss << indent << "(void)runtime->dispatchGuestBranch(rdram, ctx, " << jumpTargetExpression
                 << ", 0x" << fmt::format("{:X}", branchPc())
                 << "u, 0u, PS2Runtime::GuestBranchKind::Return, \"JR $ra\");\n";
            m_ss << indent << "return;\n";
            m_ss << indent << "#else\n";
            m_ss << indent << "ctx->pc = " << jumpTargetExpression << ";\n";
            m_ss << indent << "return;\n";
            m_ss << indent << "#endif\n";
            return;
        }

        emitRuntimeBranchDispatch(jumpTargetExpression,
                                  branchPc(),
                                  0u,
                                  "IndirectJump",
                                  "JR",
                                  indent,
                                  true);
    }

    bool ControlFlowEmitter::emitRelocationCallIfAvailable(StaticBranchKind kind, std::string_view indent)
    {
        const auto relocIt = m_gen.m_relocationCallNames.find(m_branchInst.address);
        if (relocIt == m_gen.m_relocationCallNames.end() || relocIt->second.empty())
        {
            return false;
        }

        const std::string_view resolvedSyscallName = ps2_runtime_calls::resolveSyscallName(relocIt->second);
        const std::string_view resolvedStubName = ps2_runtime_calls::resolveStubName(relocIt->second);
        if (resolvedSyscallName.empty() && resolvedStubName.empty())
        {
            return false;
        }

        const bool isSyscall = !resolvedSyscallName.empty();
        const std::string_view handlerName = isSyscall ? resolvedSyscallName : resolvedStubName;

        m_ss << indent << "{\n";
        if (kind == StaticBranchKind::Call)
        {
            m_ss << fmt::format("{}    ctx->pc = 0x{:X}u;\n", indent, fallthroughPc());
        }
        else
        {
            m_ss << indent << "    ctx->pc = getRegU32(ctx, 31);\n";
        }
        m_ss << indent << "    " << (isSyscall ? "ps2_syscalls::" : "ps2_stubs::")
             << handlerName << "(rdram, ctx, runtime);\n";
        m_ss << indent << "}\n";

        if (kind == StaticBranchKind::Jump)
        {
            m_ss << indent << "return;\n";
        }
        else
        {
            m_ss << fmt::format("{}if (ctx->pc != 0x{:X}u) {{ return; }}\n", indent, fallthroughPc());
        }

        return true;
    }

    void ControlFlowEmitter::emitStaticJump(StaticBranchKind kind)
    {
        if (kind == StaticBranchKind::Call)
        {
            m_ss << fmt::format("    SET_GPR_U32(ctx, 31, 0x{:X}u);\n", fallthroughPc());
        }

        emitDelaySlot("    ");

        const uint32_t target = buildAbsoluteJumpTarget(m_branchInst.address, m_branchInst.target);
        if (isInternalTarget(target))
        {
            emitInternalTarget(target, branchPc(), "    ");
            return;
        }

        m_ss << fmt::format("    ctx->pc = 0x{:X}u;\n", target);

        if (kind == StaticBranchKind::Call && emitRelocationCallIfAvailable(kind, "    "))
        {
            return;
        }

        if (emitDirectFunctionJumpIfAvailable(target, kind, "    "))
        {
            return;
        }

        emitExternalJumpDispatch(target, kind, "    ");
    }

    void ControlFlowEmitter::emitRegisterJump(RegisterBranchKind kind)
    {
        const uint8_t rsReg = static_cast<uint8_t>(m_branchInst.rs);
        const uint8_t rdReg = static_cast<uint8_t>(m_branchInst.rd);
        const std::vector<uint32_t> sortedInternalTargets = resolvedLocalIndirectTargets();

        m_ss << "    {\n";
        m_ss << "        const uint32_t jumpTarget = GPR_U32(ctx, " << static_cast<int>(rsReg) << ");\n";

        if (kind == RegisterBranchKind::Call && rdReg != 0u)
        {
            m_ss << fmt::format("        SET_GPR_U32(ctx, {}, 0x{:X}u);\n", rdReg, fallthroughPc());
        }

        emitDelaySlot("        ");
        m_ss << "        ctx->pc = jumpTarget;\n";

        if (!sortedInternalTargets.empty())
        {
            m_ss << "        switch (jumpTarget) {\n";
            for (uint32_t target : sortedInternalTargets)
            {
                m_ss << fmt::format("            case 0x{:X}u: goto label_{:x};\n", target, target);
            }
            m_ss << "            default: break;\n";
            m_ss << "        }\n";
        }

        if (kind == RegisterBranchKind::Jump)
        {
            emitExternalRegisterJumpDispatch("jumpTarget", kind, rsReg, "        ");
        }
        else
        {
            emitExternalRegisterCallDispatch("jumpTarget", "        ");
        }

        m_ss << "    }\n";
    }

    std::string ControlFlowEmitter::conditionalBranchExpression() const
    {
        const uint8_t rsReg = static_cast<uint8_t>(m_branchInst.rs);
        const uint8_t rtReg = static_cast<uint8_t>(m_branchInst.rt);

        switch (m_branchInst.opcode)
        {
        case OPCODE_BEQ:
        case OPCODE_BEQL:
            return fmt::format("GPR_U64(ctx, {}) == GPR_U64(ctx, {})", rsReg, rtReg);
        case OPCODE_BNE:
        case OPCODE_BNEL:
            return fmt::format("GPR_U64(ctx, {}) != GPR_U64(ctx, {})", rsReg, rtReg);
        // The R5900 tests the sign of the full 64-bit GPR (bit 63), not bit 31 (PCSX2 Interpreter.cpp: GPR.SD[0]).
        case OPCODE_BLEZ:
        case OPCODE_BLEZL:
            return fmt::format("GPR_S64(ctx, {}) <= 0", rsReg);
        case OPCODE_BGTZ:
        case OPCODE_BGTZL:
            return fmt::format("GPR_S64(ctx, {}) > 0", rsReg);
        case OPCODE_REGIMM:
            switch (m_branchInst.rt)
            {
            case REGIMM_BLTZ:
            case REGIMM_BLTZL:
            case REGIMM_BLTZAL:
            case REGIMM_BLTZALL:
                return fmt::format("GPR_S64(ctx, {}) < 0", rsReg);
            case REGIMM_BGEZ:
            case REGIMM_BGEZL:
            case REGIMM_BGEZAL:
            case REGIMM_BGEZALL:
                return fmt::format("GPR_S64(ctx, {}) >= 0", rsReg);
            default:
                return "false";
            }
        case OPCODE_COP0:
            if (m_branchInst.rs == COP0_BC)
            {
                // CPCOND0 is the DMAC's D_PCR.CPC / D_STAT.CIS condition (ps2autotests dma/dmac/cpcond).
                const uint8_t bcCond = static_cast<uint8_t>(m_branchInst.rt);
                return (bcCond == COP0_BC_BCF || bcCond == COP0_BC_BCFL)
                           ? "!runtime->memory().dmacCpcond()"
                           : "runtime->memory().dmacCpcond()";
            }
            break;
        case OPCODE_COP1:
            if (m_branchInst.rs == COP1_BC)
            {
                const uint8_t bcCond = static_cast<uint8_t>(m_branchInst.rt);
                return (bcCond == COP1_BC_BCF || bcCond == COP1_BC_BCFL)
                           ? "!(ctx->fcr31 & 0x800000)"
                           : "(ctx->fcr31 & 0x800000)";
            }
            break;
        case OPCODE_COP2:
            if (m_branchInst.rs == COP2_BC)
            {
                const uint8_t bcCond = static_cast<uint8_t>(m_branchInst.rt);
                return (bcCond == COP2_BC_BCF || bcCond == COP2_BC_BCFL)
                           ? "!(ctx->vu0_status & 0x1)"
                           : "(ctx->vu0_status & 0x1)";
            }
            break;
        default:
            break;
        }

        return "false";
    }

    uint32_t ControlFlowEmitter::conditionalBranchTarget() const
    {
        const int32_t offsetBytes = static_cast<int32_t>(static_cast<int16_t>(m_branchInst.simmediate)) << 2;
        return static_cast<uint32_t>(static_cast<int64_t>(m_branchInst.address + 4u) +
                                     static_cast<int64_t>(offsetBytes));
    }

    void ControlFlowEmitter::emitConditionalBranch()
    {
        const uint32_t target = conditionalBranchTarget();
        const bool likely = isLikelyBranch();
        const std::string branchTakenVar = fmt::format("branch_taken_0x{:x}", m_branchInst.address);
        std::string unconditionalLinkCode;

        if (m_branchInst.opcode == OPCODE_REGIMM)
        {
            // The likely forms link even when not taken (ps2autotests cpu/ee/branch: "bgezall -1: skipped, set ra";
            // PCSX2 BGEZALL/BLTZALL also call _SetLink before testing the condition).
            if (m_branchInst.rt == REGIMM_BLTZAL || m_branchInst.rt == REGIMM_BGEZAL ||
                m_branchInst.rt == REGIMM_BLTZALL || m_branchInst.rt == REGIMM_BGEZALL)
            {
                unconditionalLinkCode = fmt::format("SET_GPR_U32(ctx, 31, 0x{:X}u);", fallthroughPc());
            }
        }

        m_ss << "    {\n";
        m_ss << "        const bool " << branchTakenVar << " = (" << conditionalBranchExpression() << ");\n";

        if (!unconditionalLinkCode.empty())
        {
            m_ss << "        " << unconditionalLinkCode << "\n";
        }

        if (likely)
        {
            m_ss << "        if (" << branchTakenVar << ") {\n";
            emitDelaySlot("            ");

            if (isInternalTarget(target))
            {
                emitInternalTarget(target, branchPc(), "            ");
            }
            else
            {
                m_ss << fmt::format("            ctx->pc = 0x{:X}u;\n", target);
                m_ss << "            return;\n";
            }
            m_ss << "        }\n";
        }
        else
        {
            emitDelaySlot("        ");

            m_ss << "        if (" << branchTakenVar << ") {\n";
            if (isInternalTarget(target))
            {
                emitInternalTarget(target, branchPc(), "            ");
            }
            else
            {
                m_ss << fmt::format("            ctx->pc = 0x{:X}u;\n", target);
                m_ss << "            return;\n";
            }
            m_ss << "        }\n";
        }

        m_ss << "    }\n";
    }

    bool ControlFlowEmitter::delaySlotIsOwnTransfer() const
    {
        return m_delaySlotOverride.empty() && m_delaySlot.hasDelaySlot && isInternalTarget(delayPc());
    }

    // The outcome is known when the branch is decoded: jumps, branch-likely (predicted taken), and conditional
    // branches whose encoding fixes the outcome (beq rs,rs; b/bal as bgez/bgezal $0; blez $0).
    bool ControlFlowEmitter::resolvesEarly() const
    {
        const Instruction &b = m_branchInst;
        if (b.opcode == OPCODE_J || b.opcode == OPCODE_JAL || isLikelyBranch())
            return true;
        if (b.opcode == OPCODE_BEQ && b.rs == b.rt)
            return true;
        if (b.opcode == OPCODE_BLEZ && b.rs == 0u)
            return true;
        return b.opcode == OPCODE_REGIMM && b.rs == 0u && (b.rt == REGIMM_BGEZ || b.rt == REGIMM_BGEZAL);
    }

    bool ControlFlowEmitter::linksUnconditionally() const
    {
        return m_branchInst.opcode == OPCODE_REGIMM &&
               (m_branchInst.rt == REGIMM_BLTZAL || m_branchInst.rt == REGIMM_BGEZAL ||
                m_branchInst.rt == REGIMM_BLTZALL || m_branchInst.rt == REGIMM_BGEZALL);
    }

    const Instruction *ControlFlowEmitter::findFunctionInstruction(uint32_t address) const
    {
        const std::vector<Instruction> *instructions = m_gen.m_currentFunctionInstructions;
        if (!instructions)
            return nullptr;
        const auto it = std::lower_bound(instructions->begin(), instructions->end(), address,
                                         [](const Instruction &inst, uint32_t a) { return inst.address < a; });
        return (it != instructions->end() && it->address == address) ? &*it : nullptr;
    }

    void ControlFlowEmitter::emitTransferTo(uint32_t target, std::string_view indent)
    {
        if (isInternalTarget(target))
        {
            emitInternalTarget(target, branchPc(), indent);
            return;
        }
        m_ss << fmt::format("{}ctx->pc = 0x{:X}u;\n", indent, target);
        m_ss << indent << "return;\n";
    }

    void ControlFlowEmitter::warnDelaySlotTransfer(const std::string &message) const
    {
        if (m_gen.m_reporter)
        {
            m_gen.m_reporter->warningAt("codegen", m_function.name, delayPc(), message);
        }
    }

    // A control transfer (inner, at delayPc) in this branch's (outer) delay slot. MIPS leaves it undefined; the
    // EE's results (ps2autotests cpu/ee/branchdelay, cpu/ee_fpu/branchdelay, all branches taken) fit two cases:
    // - the outer outcome is known early (resolvesEarly: `beq $t0,$t0` x4 -> 2, `bc1tl` x4 -> 2): the outer
    //   transfer happens, so the inner branch's delay slot is the instruction at the outer target T1, and the
    //   inner branch then goes to its own target (or continues past T1's instruction when it is not taken);
    // - otherwise (`bc1t` x4 -> 4): a taken inner branch replaces the outer transfer and runs as an ordinary
    //   branch at delayPc (its delay slot is the next instruction); a not-taken one leaves the outer transfer.
    // Not taken, the outer branch runs the inner one as an ordinary branch (likely: annulled, continue at +8).
    // The early/late split is consistent with the EE's static branch prediction (likely and fixed-outcome
    // branches are predicted taken) but only those three hardware rows establish it.
    void ControlFlowEmitter::emitTransferInDelaySlot()
    {
        const Instruction &inner = m_delaySlot;
        const bool outerIsJump = m_branchInst.opcode == OPCODE_J || m_branchInst.opcode == OPCODE_JAL;
        const uint32_t outerTarget = outerIsJump ? buildAbsoluteJumpTarget(m_branchInst.address, m_branchInst.target)
                                                 : conditionalBranchTarget();

        ControlFlowEmitter innerBranch(m_gen, inner, inner, m_function, m_analysisResult);
        const bool innerIsJump = inner.opcode == OPCODE_J || inner.opcode == OPCODE_JAL;
        const bool innerIsConditional = inner.isBranch && !innerIsJump && inner.opcode != OPCODE_SPECIAL;
        const uint32_t innerLink = inner.address + 8u;

        bool early = resolvesEarly();
        const Instruction *targetInst = findFunctionInstruction(outerTarget);
        if (early && (!innerIsConditional && inner.opcode != OPCODE_J))
        {
            warnDelaySlotTransfer("call or register jump in the delay slot of an early-resolved branch; modelled as "
                                  "replacing the outer transfer (no hardware evidence for this case)");
            early = false;
        }
        if (early && (!targetInst || targetInst->hasDelaySlot || !isInternalTarget(outerTarget)))
        {
            warnDelaySlotTransfer("branch in a delay slot whose outer target is outside the function or is itself a "
                                  "branch; modelled as replacing the outer transfer");
            early = false;
        }

        const std::string outerTaken = fmt::format("outer_taken_0x{:x}", branchPc());
        const std::string innerTaken = fmt::format("inner_taken_0x{:x}", inner.address);
        m_ss << "    {\n";
        m_ss << "        const bool " << outerTaken << " = ("
             << (outerIsJump ? std::string("true") : conditionalBranchExpression()) << ");\n";
        if (m_branchInst.opcode == OPCODE_JAL || linksUnconditionally())
        {
            m_ss << fmt::format("        SET_GPR_U32(ctx, 31, 0x{:X}u);\n", fallthroughPc());
        }

        m_ss << "        if (!" << outerTaken << ") {\n";
        if (isLikelyBranch())
        {
            emitTransferTo(fallthroughPc(), "            "); // annulled delay slot
        }
        else
        {
            emitInternalTarget(delayPc(), branchPc(), "            ");
        }
        m_ss << "        }\n";

        // Outer branch taken: the inner transfer executes in its delay slot.
        m_ss << fmt::format("        ctx->pc = 0x{:X}u;\n", delayPc());
        m_ss << "        const bool " << innerTaken << " = ("
             << (innerIsConditional ? innerBranch.conditionalBranchExpression() : std::string("true")) << ");\n";
        if (innerBranch.linksUnconditionally())
        {
            m_ss << fmt::format("        SET_GPR_U32(ctx, 31, 0x{:X}u);\n", innerLink);
        }

        if (early)
        {
            const uint32_t innerTarget = innerIsJump ? buildAbsoluteJumpTarget(inner.address, inner.target)
                                                     : innerBranch.conditionalBranchTarget();
            m_ss << "        if (" << innerTaken << ") {\n";
            m_ss << fmt::format("            ctx->pc = 0x{:X}u;\n", outerTarget);
            m_ss << "            ctx->in_delay_slot = true;\n";
            m_ss << fmt::format("            ctx->branch_pc = 0x{:X}u;\n", inner.address);
            std::istringstream lines(m_gen.translateInstruction(*targetInst));
            for (std::string line; std::getline(lines, line);)
            {
                if (!line.empty())
                    m_ss << "            " << line << "\n";
            }
            m_ss << "            ctx->in_delay_slot = false;\n";
            emitTransferTo(innerTarget, "            ");
            m_ss << "        }\n";
            // Not taken: the instruction at the outer target runs as the inner branch's delay slot (or is annulled
            // for a likely inner branch), then execution continues after it.
            emitTransferTo(innerBranch.isLikelyBranch() ? outerTarget + 4u : outerTarget, "        ");
        }
        else
        {
            m_ss << "        if (" << innerTaken << ") {\n";
            emitInternalTarget(delayPc(), branchPc(), "            ");
            m_ss << "        }\n";
            emitTransferTo(outerTarget, "        ");
        }
        m_ss << "    }\n";
    }

    void ControlFlowEmitter::emitFallbackInstruction()
    {
        m_ss << "    " << m_gen.translateInstruction(m_branchInst) << "\n";
        emitDelaySlot("    ");
    }

    void ControlFlowEmitter::emitFallthroughLabelIfNeeded()
    {
        if (isInternalTarget(delayPc()) && !isInternalTarget(fallthroughPc()))
        {
            m_ss << fmt::format("label_fallthrough_0x{:x}:\n", branchPc());
        }
    }

    void ControlFlowEmitter::emitFinalFallthrough()
    {
        m_ss << fmt::format("    ctx->pc = 0x{:X}u;\n", fallthroughPc());
    }

    std::string ControlFlowEmitter::emit()
    {
        if (delaySlotIsOwnTransfer())
        {
            m_ss << fmt::format("    ctx->pc = 0x{:X}u;\n", branchPc());
            const bool registerJump = m_branchInst.opcode == OPCODE_SPECIAL &&
                                      (m_branchInst.function == SPECIAL_JR || m_branchInst.function == SPECIAL_JALR);
            if (!registerJump && (m_branchInst.isBranch || m_branchInst.opcode == OPCODE_J ||
                                  m_branchInst.opcode == OPCODE_JAL))
            {
                emitTransferInDelaySlot();
                return m_ss.str();
            }
            warnDelaySlotTransfer("control transfer in the delay slot of a register jump; the inner transfer is not "
                                  "executed (no hardware evidence for this case)");
            emitRegisterJump(m_branchInst.function == SPECIAL_JALR ? RegisterBranchKind::Call : RegisterBranchKind::Jump);
            emitFinalFallthrough();
            return m_ss.str();
        }

        emitResumeFromDelaySlotEntry();
        m_ss << fmt::format("    ctx->pc = 0x{:X}u;\n", branchPc());

        if (m_branchInst.opcode == OPCODE_J || m_branchInst.opcode == OPCODE_JAL)
        {
            emitStaticJump(m_branchInst.opcode == OPCODE_JAL ? StaticBranchKind::Call : StaticBranchKind::Jump);
        }
        else if (m_branchInst.opcode == OPCODE_SPECIAL &&
                 (m_branchInst.function == SPECIAL_JR || m_branchInst.function == SPECIAL_JALR))
        {
            emitRegisterJump(m_branchInst.function == SPECIAL_JALR ? RegisterBranchKind::Call : RegisterBranchKind::Jump);
        }
        else if (m_branchInst.isBranch)
        {
            emitConditionalBranch();
        }
        else
        {
            emitFallbackInstruction();
        }

        emitFallthroughLabelIfNeeded();
        emitFinalFallthrough();
        return m_ss.str();
    }
}
