// libFuzzer harnesses for the recompiler's inputs (docs/architecture/fuzzing.md).
//   elf     : ElfParser::parse on an arbitrary file (written to a per-process temporary path), then the section and
//             symbol accessors
//   decoder : R5900Decoder on arbitrary instruction words, with disassembly, and CodeGenerator on the decoded
//             sequence as one function (the recompiler's emitter must not crash on any decodable word sequence)
#include "ps2recomp/elf_parser.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/types.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
#if PS2X_FUZZ_TARGET == 5 // elf
    static const std::string path =
        (std::filesystem::temp_directory_path() / ("ps2x_fuzz_elf_" + std::to_string(reinterpret_cast<uintptr_t>(&path)) + ".elf")).string();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    }
    try
    {
        ps2recomp::ElfParser parser(path);
        if (parser.parse())
        {
            (void)parser.getSections();
        }
    }
    catch (const std::exception &)
    {
        // A rejected file may throw; that is a reported error, not a defect.
    }
#elif PS2X_FUZZ_TARGET == 6 // decoder + emitter
    if (size < 4u || size > 4096u)
        return -1;
    ps2recomp::R5900Decoder decoder;
    std::vector<ps2recomp::Instruction> instructions;
    const uint32_t base = 0x00100000u;
    for (size_t i = 0; i + 4u <= size; i += 4u)
    {
        uint32_t word = 0;
        std::memcpy(&word, data + i, 4);
        instructions.push_back(decoder.decodeInstruction(base + static_cast<uint32_t>(i), word, true));
        (void)ps2recomp::R5900Decoder::disassembleInstruction(instructions.back());
    }
    ps2recomp::Function function;
    function.name = "fuzz_function";
    function.start = base;
    function.end = base + static_cast<uint32_t>(instructions.size() * 4u);
    function.isRecompiled = true;
    try
    {
        ps2recomp::CodeGenerator generator({}, {});
        (void)generator.generateFunction(function, instructions, false);
    }
    catch (const std::exception &)
    {
        // The emitter reports unsupported input by throwing; a crash or sanitizer report is the defect.
    }
#else
#error "PS2X_FUZZ_TARGET must be 5 (elf) or 6 (decoder)"
#endif
    return 0;
}
