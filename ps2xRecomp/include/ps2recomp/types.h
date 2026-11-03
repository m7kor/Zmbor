#ifndef PS2RECOMP_TYPES_H
#define PS2RECOMP_TYPES_H

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <map>

namespace ps2recomp
{

    // Instruction representation
    struct Instruction
    {
        uint32_t address;
        uint32_t opcode;
        uint32_t rs;         // Source register
        uint32_t rt;         // Target register
        uint32_t rd;         // Destination register
        uint32_t sa;         // Shift amount
        uint32_t function;   // Function code for R-type instructions
        uint32_t immediate;  // Immediate value for I-type instructions
        uint32_t simmediate; // Sign-extended immediate value (extended to 32 bits)
        uint32_t target;     // Jump target for J-type instructions
        uint32_t raw;        // Raw instruction value

        // Instruction type flags
        bool isMMI;        // Is MMI instruction (PS2 specific)
        bool isVU;         // Is VU instruction (PS2 specific)
        bool isBranch;     // Is branch instruction
        bool isJump;       // Is jump instruction
        bool isCall;       // Is function call
        bool isReturn;     // Is return instruction
        bool hasDelaySlot; // Has delay slot
        bool isMultimedia; // PS2-specific multimedia operations
        bool isStore;      // Is store instruction
        bool isLoad;       // Is load instruction

        // Additional PS2-specific fields
        uint8_t mmiType;        // 0=MMI0, 1=MMI1, 2=MMI2, 3=MMI3
        uint8_t mmiFunction;    // Function within MMI type
        uint8_t pmfhlVariation; // For PMFHL instructions
        uint8_t vuFunction;     // For VU instructions

        bool isMmio = false;
        uint32_t mmioAddress = 0;
        std::string disassembly;

        struct
        {
            bool isVector;       // Uses vector operations
            bool usesQReg;       // Uses Q register
            bool usesPReg;       // Uses P register
            bool modifiesMAC;    // Modifies MAC flags
            uint8_t vectorField; // xyzw field mask
            uint8_t fsf;         // Field select for FS reg (bits 10-11)
            uint8_t ftf;         // Source Field select for FT reg (bits 8-9)
        } vectorInfo;

        struct
        {
            bool modifiesGPR;     // Modifies general purpose register
            bool modifiesFPR;     // Modifies floating point register
            bool modifiesVFR;     // Modifies vector float register
            bool modifiesVIR;     // Modifies vector integer register
            bool modifiesVIC;     // Modifies vector integer control register
            bool modifiesMemory;  // Modifies memory
            bool modifiesControl; // Modifies control register
        } modificationInfo;

        Instruction() : address(0), opcode(0), rs(0), rt(0), rd(0), sa(0), function(0),
                        immediate(0), simmediate(0), target(0), raw(0),
                        isMMI(false), isVU(false), isBranch(false), isJump(false), isCall(false),
                        isReturn(false), hasDelaySlot(false), isMultimedia(false), isStore(false), isLoad(false),
                        mmiType(0), mmiFunction(0), pmfhlVariation(0), vuFunction(0), isMmio(false), mmioAddress(0)
        {
            vectorInfo = {};
            modificationInfo = {};
        }
    };

    struct MemoryAccessHint
    {
        bool hasAddress = false;
        uint32_t address = 0;
    };

    // Function information
    struct Function
    {
        std::string name;
        uint32_t start;
        uint32_t end;
        std::vector<Instruction> instructions;
        std::vector<uint32_t> callers;
        std::vector<uint32_t> callees;
        bool isRecompiled = false;
        bool isStub = false;
        bool isSkipped = false;
    };

    // Symbol information
    struct Symbol
    {
        std::string name;
        uint32_t address;
        uint32_t size;
        bool isFunction;
        bool isImported;
        bool isExported;
    };

    // Section information
    struct Section
    {
        std::string name;
        uint32_t address;
        uint32_t size;
        uint32_t offset;
        bool isCode;
        bool isData;
        bool isBSS;
        bool isReadOnly;
        uint8_t *data;
    };

    // Section end in 64 bits. A section near the top of the 32-bit address space wraps if its end is
    // computed in uint32_t (e.g. base 0xFFFFFFF0 + size 0x20 -> 0x10), which made every range check that
    // used `address + size` miss the section's own addresses. Use this helper for all section range
    // checks; the plain uint32_t end does not exist, on purpose.
    inline uint64_t sectionEnd(const Section &section) noexcept
    {
        return static_cast<uint64_t>(section.address) + static_cast<uint64_t>(section.size);
    }

    inline bool sectionContainsAddress(const Section &section, uint32_t address) noexcept
    {
        return static_cast<uint64_t>(address) >= static_cast<uint64_t>(section.address) &&
               static_cast<uint64_t>(address) < sectionEnd(section);
    }

    // Clamp a 64-bit exclusive end to a uint32_t function end. This is a lossy representation,
    // used only where a function end must stay uint32_t: an end past 0xFFFFFFFF becomes 0xFFFFFFFF.
    // That still contains every 4-aligned instruction start in the section (the last one is
    // 0xFFFFFFFC < 0xFFFFFFFF), and it is never a wrapped small value that would make the function
    // end before it starts. The single byte at 0xFFFFFFFF itself is not an instruction start and is
    // still covered by the unclamped sectionContainsAddress() for data ranges.
    inline uint32_t clampFunctionEnd(uint64_t end64) noexcept
    {
        return (end64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(end64);
    }

    // Relocation information
    struct Relocation
    {
        uint32_t offset;
        uint32_t info;
        uint32_t symbol;
        std::string symbolName;
        uint32_t type;
        int32_t addend;
    };

    // Jump table entry
    struct JumpTableEntry
    {
        uint32_t index;
        uint32_t target;
    };

    // Jump table
    struct JumpTable
    {
        uint32_t address;
        uint32_t baseRegister;
        std::vector<JumpTableEntry> entries;
    };

    // Control flow graph
    struct CFGNode
    {
        uint32_t startAddress;
        uint32_t endAddress;
        std::vector<Instruction> instructions;
        std::vector<uint32_t> predecessors;
        std::vector<uint32_t> successors;
        bool isJumpTarget;
        bool hasJumpTable;
        JumpTable jumpTable;
    };

    // Function call
    struct FunctionCall
    {
        uint32_t callerAddress;
        uint32_t calleeAddress;
        std::string calleeName;
    };

    // Recompiler configuration
    struct RecompilerConfig
    {
        std::string inputPath;
        std::string outputPath;
        std::string ghidraMapPath;
        bool singleFileOutput = false;
        bool lowMemoryMode = false;
        uint32_t outputWorkerThreads = 0;
        bool patchSyscalls = false;
        bool patchCop0 = true;
        bool patchCache = true;
        std::vector<std::string> skipFunctions;
        std::unordered_map<uint32_t, std::string> patches;
        std::vector<std::string> stubImplementations;
        std::vector<std::string> entryPointHints;
        std::unordered_map<uint32_t, uint32_t> mmioByInstructionAddress;
        std::vector<JumpTable> jumpTables;
    };

} // namespace ps2recomp

#endif // PS2RECOMP_TYPES_H
