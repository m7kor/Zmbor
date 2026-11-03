#include "ps2recomp/analysis_passes.h"

#include "ps2recomp/instructions.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace ps2recomp
{
    bool AnalysisPasses::hasHardwareIOSignal(const std::vector<Instruction> &instructions)
    {
        for (const auto &inst : instructions)
        {
            if (inst.opcode == OPCODE_LUI)
            {
                const uint32_t upperAddr = inst.immediate << 16;
                if ((upperAddr >= 0x10000000 && upperAddr < 0x14000000) || // I/O area
                    (upperAddr >= 0x1F800000 && upperAddr < 0x1F900000))   // Scratchpad RAM
                {
                    return true;
                }
            }
        }

        return false;
    }

    bool AnalysisPasses::hasLargeComplexMMISignal(const std::vector<Instruction> &instructions,
                                                  size_t largeInstructionThreshold)
    {
        if (instructions.size() <= largeInstructionThreshold)
        {
            return false;
        }

        for (const auto &inst : instructions)
        {
            if (inst.isMMI &&
                inst.opcode == OPCODE_MMI &&
                (inst.function == MMI_MMI0 || inst.function == MMI_MMI1 ||
                 inst.function == MMI_MMI2 || inst.function == MMI_MMI3))
            {
                return true;
            }
        }

        return false;
    }

    bool AnalysisPasses::hasSelfModifyingSignal(const std::vector<Instruction> &instructions,
                                                const std::vector<Section> &sections)
    {
        for (size_t i = 0; i < instructions.size(); i++)
        {
            const auto &inst = instructions[i];
            if (!(inst.opcode == OPCODE_SW || inst.opcode == OPCODE_SH ||
                  inst.opcode == OPCODE_SB || inst.opcode == OPCODE_SQ))
            {
                continue;
            }

            uint32_t baseAddr = 0;
            for (int j = static_cast<int>(i) - 1; j >= 0 && j >= static_cast<int>(i) - 5; j--)
            {
                const auto &prevInst = instructions[static_cast<size_t>(j)];
                if (prevInst.opcode == OPCODE_LUI && prevInst.rt == inst.rs)
                {
                    baseAddr = prevInst.immediate << 16;
                    break;
                }
            }

            if (baseAddr == 0)
            {
                continue;
            }

            const uint32_t targetAddr = baseAddr + static_cast<int16_t>(inst.immediate);
            for (const auto &section : sections)
            {
                if (section.isCode && sectionContainsAddress(section, targetAddr))
                {
                    return true;
                }
            }
        }

        return false;
    }

    std::vector<JumpTable> AnalysisPasses::detectJumpTables(
        const std::vector<Instruction> &instructions,
        const std::vector<Section> &sections,
        const std::function<bool(uint32_t, uint32_t &)> &readWord)
    {
        std::vector<JumpTable> jumpTables;

        auto addSignedImm16 = [](uint32_t hiPart, uint16_t imm16) -> uint32_t
        {
            return hiPart + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(imm16)));
        };

        auto orUnsignedImm16 = [](uint32_t hiPart, uint16_t imm16) -> uint32_t
        {
            return hiPart | static_cast<uint32_t>(imm16);
        };

        auto looksLikeCodeTarget = [&sections](uint32_t addr) -> bool
        {
            if (addr == 0)
            {
                return false;
            }

            if (sections.empty())
            {
                return true;
            }

            for (const auto &section : sections)
            {
                if (!section.isCode)
                {
                    continue;
                }

                const uint32_t sectionEnd = section.address + section.size;
                if (addr >= section.address && addr < sectionEnd)
                {
                    return true;
                }
            }

            return false;
        };

        auto readJumpEntryCandidate = [&](uint32_t entryAddr, bool isLoadDouble, uint32_t &outTarget) -> bool
        {
            outTarget = 0;

            uint32_t w0 = 0;
            if (!readWord(entryAddr, w0))
            {
                return false;
            }

            if (!isLoadDouble)
            {
                outTarget = w0;
                return true;
            }

            uint32_t w1 = 0;
            if (!readWord(entryAddr + 4u, w1))
            {
                outTarget = w0;
                return true;
            }

            const bool w0Looks = looksLikeCodeTarget(w0);
            const bool w1Looks = looksLikeCodeTarget(w1);

            if (w0Looks && !w1Looks)
            {
                outTarget = w0;
                return true;
            }
            if (w1Looks && !w0Looks)
            {
                outTarget = w1;
                return true;
            }
            outTarget = w0;
            return true;
        };

        auto tryBuildTable = [&](uint32_t baseAddr, uint32_t baseReg, uint32_t numEntries, uint32_t strideBytes, bool isLoadDouble) -> std::optional<JumpTable>
        {
            JumpTable jumpTable;
            jumpTable.address = baseAddr;
            jumpTable.baseRegister = baseReg;

            uint32_t validCodeTargets = 0;
            uint32_t totalRead = 0;

            for (uint32_t e = 0; e < numEntries; e++)
            {
                const uint32_t entryAddr = baseAddr + (e * strideBytes);

                uint32_t targetAddr = 0;
                if (!readJumpEntryCandidate(entryAddr, isLoadDouble, targetAddr))
                {
                    continue;
                }

                totalRead++;

                if (looksLikeCodeTarget(targetAddr))
                {
                    validCodeTargets++;
                }

                JumpTableEntry entry;
                entry.index = e;
                entry.target = targetAddr;
                jumpTable.entries.push_back(entry);
            }

            if (jumpTable.entries.empty())
            {
                return std::nullopt;
            }

            bool ok = false;
            if (sections.empty())
            {
                ok = (totalRead >= 2);
            }
            else
            {
                ok = (validCodeTargets >= 2) &&
                     (totalRead >= 2) &&
                     (validCodeTargets * 2 >= totalRead);
            }

            if (!ok)
            {
                return std::nullopt;
            }

            return jumpTable;
        };

        for (size_t i = 0; i < instructions.size(); i++)
        {
            const auto &inst = instructions[i];

            if (inst.opcode != OPCODE_SLTIU || i + 2 >= instructions.size())
            {
                continue;
            }

            const auto &nextInst = instructions[i + 1];
            if (nextInst.opcode != OPCODE_BNE && nextInst.opcode != OPCODE_BEQ)
            {
                continue;
            }

            for (size_t j = i + 2; j < std::min(i + 10, instructions.size()); j++)
            {
                const auto &loadInst = instructions[j];
                const bool isLoadWord = (loadInst.opcode == OPCODE_LW);
                const bool isLoadDouble = (loadInst.opcode == OPCODE_LD);

                if ((!isLoadWord && !isLoadDouble) || j + 1 >= instructions.size())
                {
                    continue;
                }

                const auto &jumpInst = instructions[j + 1];
                if (jumpInst.opcode != OPCODE_SPECIAL ||
                    jumpInst.function != SPECIAL_JR ||
                    jumpInst.rs != loadInst.rt)
                {
                    continue;
                }

                const uint32_t numEntries = inst.immediate;
                if (numEntries == 0 || numEntries >= 1000)
                {
                    break;
                }

                uint32_t baseAddr = 0;
                for (int k = static_cast<int>(j) - 1; k >= static_cast<int>(i); k--)
                {
                    const auto &addrInst = instructions[static_cast<size_t>(k)];
                    if (addrInst.opcode != OPCODE_LUI)
                    {
                        continue;
                    }

                    const uint32_t hiPart = (addrInst.immediate << 16);

                    if (static_cast<size_t>(k + 1) < instructions.size())
                    {
                        const auto &offsetInst = instructions[static_cast<size_t>(k + 1)];
                        const bool isAddiuOrOri = (offsetInst.opcode == OPCODE_ADDIU || offsetInst.opcode == OPCODE_ORI);

                        if (isAddiuOrOri &&
                            offsetInst.rs == addrInst.rt &&
                            offsetInst.rt == loadInst.rs)
                        {
                            if (offsetInst.opcode == OPCODE_ADDIU)
                            {
                                baseAddr = addSignedImm16(hiPart, offsetInst.immediate);
                            }
                            else
                            {
                                baseAddr = orUnsignedImm16(hiPart, offsetInst.immediate);
                            }
                            break;
                        }
                    }

                    if (addrInst.rt == loadInst.rs)
                    {
                        baseAddr = addSignedImm16(hiPart, loadInst.immediate);
                        break;
                    }
                }

                if (baseAddr == 0)
                {
                    break;
                }

                const uint32_t preferredStride = isLoadDouble ? 8u : 4u;

                std::optional<JumpTable> table = tryBuildTable(baseAddr, loadInst.rs, numEntries, preferredStride, isLoadDouble);
                if (!table && isLoadDouble)
                {
                    table = tryBuildTable(baseAddr, loadInst.rs, numEntries, 4u, isLoadDouble);
                }

                if (table)
                {
                    jumpTables.push_back(std::move(*table));
                }

                break;
            }
        }

        // Phase 12 — second shape: the standard MIPS switch without an
        // SLTIU bounds check in front of it: LUI table base, SLL index by 2
        // (optional — the index may already be word-scaled), ADDU base +
        // index, LW target, JR target. The existing shape above requires an
        // SLTIU + branch pair and misses switches bounded any other way
        // (ANDI mask, SLTU against a count register, or a constant index).
        // Entry count comes from an SLTIU/SLTI bound on the unshifted index
        // when one is found; otherwise entries are read sequentially while
        // they are valid code targets. A table already found by the first
        // shape (same base address) is not duplicated.
        for (size_t jrIndex = 0; jrIndex < instructions.size(); jrIndex++)
        {
            const auto &jrInst = instructions[jrIndex];
            if (jrInst.opcode != OPCODE_SPECIAL || jrInst.function != SPECIAL_JR || jrInst.rs == 31)
            {
                continue;
            }
            const uint32_t jrReg = jrInst.rs;

            int lwIndex = -1;
            uint32_t baseReg = 0;
            int32_t lwOffset = 0;
            for (int i = static_cast<int>(jrIndex) - 1; i >= 0 && i >= static_cast<int>(jrIndex) - 20; --i)
            {
                const auto &inst = instructions[static_cast<size_t>(i)];
                if ((inst.opcode == OPCODE_LW || inst.opcode == OPCODE_LWU) && inst.rt == jrReg)
                {
                    lwIndex = i;
                    baseReg = inst.rs;
                    lwOffset = static_cast<int32_t>(inst.simmediate);
                    break;
                }
            }
            if (lwIndex == -1)
            {
                continue;
            }

            int adduIndex = -1;
            uint32_t tableBaseReg = 0;
            uint32_t indexReg = 0;
            for (int i = lwIndex - 1; i >= 0 && i >= lwIndex - 10; --i)
            {
                const auto &inst = instructions[static_cast<size_t>(i)];
                if (inst.opcode == OPCODE_SPECIAL && inst.function == SPECIAL_ADDU && inst.rd == baseReg)
                {
                    adduIndex = i;
                    tableBaseReg = inst.rs;
                    indexReg = inst.rt;
                    break;
                }
            }
            if (adduIndex == -1)
            {
                continue;
            }

            uint32_t tableAddress = 0;
            bool foundTableAddress = false;
            for (int i = adduIndex - 1; i >= 0 && i >= adduIndex - 20; --i)
            {
                const auto &inst = instructions[static_cast<size_t>(i)];
                if (inst.opcode != OPCODE_LUI || (inst.rt != tableBaseReg && inst.rt != indexReg))
                {
                    continue;
                }
                const uint32_t hiPart = (inst.immediate << 16);
                uint32_t low = 0;
                bool foundLow = false;
                for (int j = i + 1; j < adduIndex; ++j)
                {
                    const auto &lowInst = instructions[static_cast<size_t>(j)];
                    if (lowInst.rs == inst.rt && lowInst.rt == inst.rt)
                    {
                        if (lowInst.opcode == OPCODE_ADDIU)
                        {
                            low = lowInst.simmediate;
                            foundLow = true;
                        }
                        else if (lowInst.opcode == OPCODE_ORI)
                        {
                            low = lowInst.immediate;
                            foundLow = true;
                        }
                    }
                }
                if (foundLow)
                {
                    tableAddress = hiPart + low;
                    foundTableAddress = true;
                    break;
                }
                if (inst.rt == baseReg)
                {
                    tableAddress = hiPart + static_cast<uint32_t>(lwOffset);
                    foundTableAddress = true;
                    break;
                }
            }
            if (!foundTableAddress)
            {
                continue;
            }

            const bool alreadyFound = std::any_of(
                jumpTables.begin(), jumpTables.end(),
                [tableAddress](const JumpTable &t) { return t.address == tableAddress; });
            if (alreadyFound)
            {
                continue;
            }

            uint32_t numCases = 0;
            for (int i = adduIndex - 1; i >= 0 && i >= adduIndex - 10; --i)
            {
                const auto &inst = instructions[static_cast<size_t>(i)];
                if (inst.opcode == OPCODE_SPECIAL && inst.function == SPECIAL_SLL && inst.rd == indexReg)
                {
                    const uint32_t unshiftedReg = inst.rt;
                    for (int j = adduIndex - 1; j >= 0 && j >= adduIndex - 30; --j)
                    {
                        const auto &boundInst = instructions[static_cast<size_t>(j)];
                        if ((boundInst.opcode == OPCODE_SLTIU || boundInst.opcode == OPCODE_SLTI) &&
                            boundInst.rs == unshiftedReg)
                        {
                            numCases = boundInst.immediate;
                            break;
                        }
                    }
                    break;
                }
            }

            JumpTable table;
            table.address = tableAddress;
            table.baseRegister = baseReg;
            bool tableOk = false;
            if (numCases > 0 && numCases <= 1000)
            {
                uint32_t validCodeTargets = 0;
                for (uint32_t e = 0; e < numCases; ++e)
                {
                    uint32_t target = 0;
                    if (!readWord(tableAddress + e * 4u, target))
                    {
                        break;
                    }
                    JumpTableEntry entry;
                    entry.index = e;
                    entry.target = target;
                    table.entries.push_back(entry);
                    if (looksLikeCodeTarget(target))
                    {
                        validCodeTargets++;
                    }
                }
                tableOk = table.entries.size() == numCases && validCodeTargets >= 2 &&
                          validCodeTargets * 2 >= table.entries.size();
            }
            else if (numCases == 0)
            {
                for (uint32_t e = 0; e < 1024; ++e)
                {
                    uint32_t target = 0;
                    if (!readWord(tableAddress + e * 4u, target) || !looksLikeCodeTarget(target))
                    {
                        break;
                    }
                    JumpTableEntry entry;
                    entry.index = e;
                    entry.target = target;
                    table.entries.push_back(entry);
                }
                tableOk = table.entries.size() >= 2;
            }
            if (tableOk)
            {
                jumpTables.push_back(std::move(table));
            }
        }

        return jumpTables;
    }

    std::unordered_set<std::string> AnalysisPasses::findRecursiveFunctions(
        const std::unordered_map<std::string, std::vector<std::string>> &callGraph)
    {
        std::unordered_set<std::string> nodes;
        for (const auto &[caller, callees] : callGraph)
        {
            nodes.insert(caller);
            for (const auto &callee : callees)
            {
                nodes.insert(callee);
            }
        }

        std::unordered_map<std::string, int> index;
        std::unordered_map<std::string, int> lowlink;
        std::unordered_set<std::string> onStack;
        std::vector<std::string> stack;

        index.reserve(nodes.size());
        lowlink.reserve(nodes.size());
        onStack.reserve(nodes.size());
        stack.reserve(nodes.size());

        int currentIndex = 0;
        std::vector<std::vector<std::string>> sccs;
        sccs.reserve(nodes.size());

        std::function<void(const std::string &)> strongconnect;
        strongconnect = [&](const std::string &v)
        {
            index[v] = currentIndex;
            lowlink[v] = currentIndex;
            currentIndex++;

            stack.push_back(v);
            onStack.insert(v);

            auto it = callGraph.find(v);
            if (it != callGraph.end())
            {
                for (const auto &w : it->second)
                {
                    if (!index.contains(w))
                    {
                        strongconnect(w);
                        lowlink[v] = std::min(lowlink[v], lowlink[w]);
                    }
                    else if (onStack.contains(w))
                    {
                        lowlink[v] = std::min(lowlink[v], index[w]);
                    }
                }
            }

            if (lowlink[v] == index[v])
            {
                std::vector<std::string> scc;
                while (!stack.empty())
                {
                    std::string w = stack.back();
                    stack.pop_back();
                    onStack.erase(w);
                    scc.push_back(w);
                    if (w == v)
                    {
                        break;
                    }
                }

                sccs.push_back(std::move(scc));
            }
        };

        for (const auto &name : nodes)
        {
            if (!index.contains(name))
            {
                strongconnect(name);
            }
        }

        std::unordered_set<std::string> recursive;
        for (const auto &scc : sccs)
        {
            if (scc.size() > 1)
            {
                recursive.insert(scc.begin(), scc.end());
                continue;
            }

            const std::string &name = scc[0];
            auto it = callGraph.find(name);
            if (it == callGraph.end())
            {
                continue;
            }

            if (std::find(it->second.begin(), it->second.end(), name) != it->second.end())
            {
                recursive.insert(name);
            }
        }

        return recursive;
    }
}
