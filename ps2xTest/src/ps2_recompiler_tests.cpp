#include "MiniTest.h"
#include "ps2recomp/ps2_recompiler.h"
#include "ps2recomp/config_manager.h"
#include "ps2recomp/control_flow_analyzer.h"
#include "ps2recomp/elf_parser.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/Translators/cop0_translator.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/types.h"
#include "ps2_runtime_calls.h"
#include <elfio/elfio.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace ps2recomp;

static Instruction makeNopLike(uint32_t address)
{
    Instruction inst{};
    inst.address = address;
    inst.opcode = OPCODE_ADDIU;
    inst.rt = 0;
    inst.raw = 0;
    return inst;
}

static Instruction makeAbsJump(uint32_t address, uint32_t target, uint32_t opcode)
{
    Instruction inst{};
    inst.address = address;
    inst.opcode = opcode;
    inst.target = (target >> 2) & 0x03FFFFFFu;
    inst.hasDelaySlot = true;
    inst.raw = (opcode << 26) | inst.target;
    return inst;
}

static Instruction makeJrRa(uint32_t address)
{
    Instruction inst{};
    inst.address = address;
    inst.opcode = OPCODE_SPECIAL;
    inst.function = SPECIAL_JR;
    inst.rs = 31;
    inst.hasDelaySlot = true;
    inst.raw = 0x03E00008u;
    return inst;
}

static Function makeFunction(const std::string &name, uint32_t start, uint32_t end)
{
    Function fn{};
    fn.name = name;
    fn.start = start;
    fn.end = end;
    fn.isRecompiled = true;
    fn.isStub = false;
    fn.isSkipped = false;
    return fn;
}

static bool writeMinimalMipsElfWithCodeAndDataFunctionSymbols(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);
    const char textBytes[] = {0x08, 0x00, static_cast<char>(0xE0), 0x03, 0x00, 0x00, 0x00, 0x00};
    text->set_data(textBytes, sizeof(textBytes));

    ELFIO::section *data = writer.sections.add(".data");
    data->set_type(ELFIO::SHT_PROGBITS);
    data->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    data->set_addr_align(4);
    data->set_address(0x00200000u);
    const char dataBytes[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, static_cast<char>(0x88)};
    data->set_data(dataBytes, sizeof(dataBytes));

    ELFIO::section *strtab = writer.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    strtab->set_addr_align(1);

    ELFIO::section *symtab = writer.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_info(1);
    symtab->set_link(strtab->get_index());
    symtab->set_addr_align(4);
    symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));

    ELFIO::symbol_section_accessor symbols(writer, symtab);
    ELFIO::string_section_accessor strings(strtab);
    symbols.add_symbol(strings, "", 0, 0, ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
    symbols.add_symbol(strings, "code_func", text->get_address(), text->get_size(),
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());
    symbols.add_symbol(strings, "data_func", data->get_address(), data->get_size(),
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, data->get_index());

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    ELFIO::segment *dataSegment = writer.segments.add();
    dataSegment->set_type(ELFIO::PT_LOAD);
    dataSegment->set_flags(ELFIO::PF_R | ELFIO::PF_W);
    dataSegment->set_align(0x1000);
    dataSegment->add_section_index(data->get_index(), data->get_addr_align());

    return writer.save(elfPath.string());
}

// A 64-bit MIPS ELF whose .text sits above the 32-bit address space. Before the ELFCLASS32
// check in ElfParser::parse, this parsed and its section/symbol addresses truncated to 0,
// overlapping the real low sections; after the check, parse() must reject it outright.
static bool writeMinimalMipsElf64WithHighText(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS64, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x100000000ull);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x100000000ull);
    const char textBytes[] = {0x08, 0x00, static_cast<char>(0xE0), 0x03, 0x00, 0x00, 0x00, 0x00};
    text->set_data(textBytes, sizeof(textBytes));

    ELFIO::section *strtab = writer.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    strtab->set_addr_align(1);

    ELFIO::section *symtab = writer.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_info(1);
    symtab->set_link(strtab->get_index());
    symtab->set_addr_align(8);
    symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));

    ELFIO::symbol_section_accessor symbols(writer, symtab);
    ELFIO::string_section_accessor strings(strtab);
    symbols.add_symbol(strings, "", 0, 0, ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
    symbols.add_symbol(strings, "high_func", text->get_address(), text->get_size(),
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());

    return writer.save(elfPath.string());
}

static bool writeMinimalMipsElfWithJalFallbackTarget(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    const std::array<uint32_t, 6> textWords = {
        0x0C040004u, // jal 0x00100010
        0x00000000u, // nop
        0x03E00008u, // jr $ra
        0x00000000u, // nop
        0x03E00008u, // jr $ra
        0x00000000u  // nop
    };
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

// Phase 12 — stripped synthetic ELF with an exactly known function set.
// .text at 0x00100000 (42 words, 0xA8 bytes):
//   entry [0x100000,0x100020): jal f1, jal f2, jal switch_fn, jr $ra
//   f1    [0x100020,0x100028): jr $ra
//   f2    [0x100028,0x100034): addiu v0, 7; jr $ra
//   switch[0x100034,0x100088): addiu a0, 1; andi t1, a0, 3; lui t2, 0x10;
//                              ori t2, t2, 0x200; sll t1, t1, 2;
//                              addu t3, t2, t1; lw t4, 0(t3); jr t4;
//                              cases 10/11/12/13 at 0x100058/64/70/7C
//                              (each: addiu v0, N; jr $ra)
//   dead_caller [0x100088,0x1000A0): jal f2, jal dead_func, jr $ra —
//                              UNREACHABLE from entry (no edge leads here)
//   dead_func   [0x1000A0,0x1000A8): jr $ra — called only by dead_caller
// .data at 0x00100200: the 4-entry switch table (case addresses).
// Deliberately NO SLTIU anywhere: the switch bound is an ANDI mask, so the
// CFA's unconfigured SLTIU recovery cannot resolve the JR — only a
// discovered table can. cutEntryJalF2 replaces entry's `jal f2` with nop
// (the call-edge-deletion mutant); withMergedSymbol adds a sized symbol
// "merged_fn" covering [f1, f2) to test merge order.
static const std::array<uint32_t, 42> kPhase12TextWords = {
    0x0C040008u, // jal f1 (0x100020)
    0x00000000u, // nop
    0x0C04000Au, // jal f2 (0x100028)
    0x00000000u, // nop
    0x0C04000Du, // jal switch_fn (0x100034)
    0x00000000u, // nop
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x03E00008u, // f1: jr $ra
    0x00000000u, // nop
    0x24020007u, // f2: addiu v0, zero, 7
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x24040001u, // switch: addiu a0, zero, 1
    0x30890003u, // andi t1, a0, 3
    0x3C0A0010u, // lui t2, 0x10
    0x354A0200u, // ori t2, t2, 0x200
    0x00094880u, // sll t1, t1, 2
    0x01495821u, // addu t3, t2, t1
    0x8D6C0000u, // lw t4, 0(t3)
    0x01800008u, // jr t4
    0x00000000u, // nop
    0x2402000Au, // case0: addiu v0, zero, 10
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x2402000Bu, // case1: addiu v0, zero, 11
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x2402000Cu, // case2: addiu v0, zero, 12
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x2402000Du, // case3: addiu v0, zero, 13
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x0C04000Au, // dead_caller: jal f2
    0x00000000u, // nop
    0x0C040028u, // dead_caller: jal dead_func (0x1000A0)
    0x00000000u, // nop
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x03E00008u, // dead_func: jr $ra
    0x00000000u, // nop
};

static bool writePhase12DiscoveryElf(const std::filesystem::path &elfPath, bool cutEntryJalF2,
                                     bool withMergedSymbol)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    std::array<uint32_t, 42> textWords = kPhase12TextWords;
    if (cutEntryJalF2)
    {
        textWords[2] = 0x00000000u; // entry's `jal f2` becomes nop
    }
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::section *data = writer.sections.add(".data");
    data->set_type(ELFIO::SHT_PROGBITS);
    data->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    data->set_addr_align(4);
    data->set_address(0x00100200u);
    const std::array<uint32_t, 4> tableWords = {0x00100058u, 0x00100064u, 0x00100070u, 0x0010007Cu};
    data->set_data(reinterpret_cast<const char *>(tableWords.data()),
                   static_cast<ELFIO::Elf_Word>(tableWords.size() * sizeof(uint32_t)));

    if (withMergedSymbol)
    {
        ELFIO::section *strtab = writer.sections.add(".strtab");
        strtab->set_type(ELFIO::SHT_STRTAB);
        strtab->set_addr_align(1);

        ELFIO::section *symtab = writer.sections.add(".symtab");
        symtab->set_type(ELFIO::SHT_SYMTAB);
        symtab->set_info(1);
        symtab->set_link(strtab->get_index());
        symtab->set_addr_align(4);
        symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));

        ELFIO::symbol_section_accessor symbols(writer, symtab);
        ELFIO::string_section_accessor strings(strtab);
        symbols.add_symbol(strings, "", 0, 0, ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
        symbols.add_symbol(strings, "merged_fn", 0x00100020u, 0x14u, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC,
                           0, text->get_index());
    }

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    ELFIO::segment *dataSegment = writer.segments.add();
    dataSegment->set_type(ELFIO::PT_LOAD);
    dataSegment->set_flags(ELFIO::PF_R | ELFIO::PF_W);
    dataSegment->set_align(0x1000);
    dataSegment->add_section_index(data->get_index(), data->get_addr_align());

    return writer.save(elfPath.string());
}

// Phase 14 — JALR / JR-constant synthetic ELF (no .data section).
// .text at 0x00100000 (50 words):
//   entry [0x100000,0x100040): lui $t9,0x10; addiu $t9,$t9,0x40;
//                              jalr $t9 (funcA); lui $t9,0x10;
//                              ori $t9,$t9,0x60; jalr $t9 (funcB);
//                              jalr $t8 (never defined: unknown target);
//                              lui $t0,0x10; addiu $t0,$t0,0xA0;
//                              jr $t0 (computed jump to jrBlock); jr $ra
//   funcA [0x100040,0x10004C): addiu $v0, 41; jr $ra
//   funcB [0x100060,0x10006C): addiu $v0, 42; jr $ra
//   jrBlock [0x1000A0,0x1000B0): jal funcD; jr $ra — reached ONLY
//                              through the entry's constant jr $t0, so it
//                              is an entry block, NOT a function start
//   funcD [0x1000C0,0x1000C8): jr $ra — reached ONLY through jrBlock's
//                              jal, so it is discovered ONLY if the
//                              constant-jr edge is followed.
// cutFirstAddiu replaces entry's first `addiu $t9,$t9,0x40` with nop, so
// the first jalr targets the bare LUI value 0x100000 (entry itself) and
// funcA must drop out. clobberDelay replaces the first jalr's delay-slot
// nop with `addiu $t9,$zero,0x99`, so the target register is rewritten
// in the delay slot (architecturally unpredictable) and funcA must NOT
// be recovered from that jalr.
static const std::array<uint32_t, 50> kPhase14JalrTextWords = {
    0x3C190010u, // entry: lui $t9, 0x10
    0x27390040u, // addiu $t9, $t9, 0x40 -> 0x100040
    0x0320F809u, // jalr $t9 (funcA)
    0x00000000u, // nop (delay)
    0x3C190010u, // lui $t9, 0x10
    0x37390060u, // ori $t9, $t9, 0x60 -> 0x100060
    0x0320F809u, // jalr $t9 (funcB)
    0x00000000u, // nop (delay)
    0x0300F809u, // jalr $t8 (unknown: $t8 never defined)
    0x00000000u, // nop (delay)
    0x3C080010u, // lui $t0, 0x10
    0x210800A0u, // addiu $t0, $t0, 0xA0 -> 0x1000A0
    0x01000008u, // jr $t0 (computed jump to jrBlock)
    0x00000000u, // nop (delay)
    0x03E00008u, // jr $ra (unreached fallthrough words)
    0x00000000u, // nop
    0x24020029u, // funcA: addiu $v0, zero, 41
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x2402002Au, // funcB: addiu $v0, zero, 42
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x0C040030u, // jrBlock: jal funcD (0x1000C0)
    0x00000000u, // nop (delay)
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x03E00008u, // funcD: jr $ra
    0x00000000u, // nop
};

static bool writePhase14JalrElf(const std::filesystem::path &elfPath, bool cutFirstAddiu,
                                bool clobberDelay)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    std::array<uint32_t, 50> textWords = kPhase14JalrTextWords;
    if (cutFirstAddiu)
    {
        textWords[1] = 0x00000000u; // entry's first `addiu $t9,$t9,0x40` becomes nop
    }
    if (clobberDelay)
    {
        textWords[3] = 0x24190099u; // first jalr's delay slot: addiu $t9, $zero, 0x99
    }
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

// Phase 14 — singleton data-pointer synthetic ELF.
// .text at 0x00100000 (26 words):
//   entry [0x100000,0x100010): jal plainFunc; jr $ra
//   plainFunc [0x100020,0x10002C): addiu $v0, 51; jr $ra
//   dataptrFunc [0x100040,0x100054): addiu $v0, 52; jal dataptrCallee;
//                                    jr $ra — reachable ONLY through the
//                                    .data pointer below
//   dataptrCallee [0x100060,0x100068): jr $ra — reachable ONLY through
//                                    dataptrFunc's jal, so it is discovered
//                                    ONLY if dataptrFunc is descended into.
// .data at 0x00100200 (14 words): a SINGLETON pointer to dataptrFunc at
// offset 0 (no conservative neighbour within 32 bytes, so the fallback
// scan's clustering rule must miss it), 9 junk words, a decoy pointer to
// plainFunc's jr $ra (mid-function, offset 0x28 = 40 bytes away, so it
// clusters with nothing either), junk, a self pointer, and an unaligned
// code address. zeroDataPointer replaces the singleton word with 0, so
// dataptrFunc (and hence dataptrCallee) must drop out of discovery.
static const std::array<uint32_t, 26> kPhase14DataTextWords = {
    0x0C040008u, // entry: jal plainFunc (0x100020)
    0x00000000u, // nop (delay)
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x24020033u, // plainFunc: addiu $v0, zero, 51
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x24020034u, // dataptrFunc: addiu $v0, zero, 52
    0x0C040018u, // jal dataptrCallee (0x100060)
    0x00000000u, // nop (delay)
    0x03E00008u, // jr $ra
    0x00000000u, // nop
    0x00000000u, // pad
    0x00000000u, // pad
    0x00000000u, // pad
    0x03E00008u, // dataptrCallee: jr $ra
    0x00000000u, // nop
};

static bool writePhase14DataElf(const std::filesystem::path &elfPath, bool zeroDataPointer)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);
    text->set_data(reinterpret_cast<const char *>(kPhase14DataTextWords.data()),
                   static_cast<ELFIO::Elf_Word>(kPhase14DataTextWords.size() * sizeof(uint32_t)));

    ELFIO::section *data = writer.sections.add(".data");
    data->set_type(ELFIO::SHT_PROGBITS);
    data->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    data->set_addr_align(4);
    data->set_address(0x00100200u);
    std::array<uint32_t, 14> dataWords = {
        0x00100040u, // singleton pointer to dataptrFunc
        0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu,
        0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu,
        0x00100024u, // decoy: plainFunc's jr $ra (mid-function)
        0xDEADBEEFu,
        0x00100200u, // self pointer (data, not code)
        0x00100042u, // unaligned code address
    };
    if (zeroDataPointer)
    {
        dataWords[0] = 0x00000000u;
    }
    data->set_data(reinterpret_cast<const char *>(dataWords.data()),
                   static_cast<ELFIO::Elf_Word>(dataWords.size() * sizeof(uint32_t)));

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    ELFIO::segment *dataSegment = writer.segments.add();
    dataSegment->set_type(ELFIO::PT_LOAD);
    dataSegment->set_flags(ELFIO::PF_R | ELFIO::PF_W);
    dataSegment->set_align(0x1000);
    dataSegment->add_section_index(data->get_index(), data->get_addr_align());

    return writer.save(elfPath.string());
}

static bool writeMinimalMipsElfWithVuMicroprogramSection(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);
    const std::array<uint32_t, 2> textWords{0x03E00008u, 0x00000000u};
    text->set_data(reinterpret_cast<const char *>(textWords.data()), sizeof(textWords));

    ELFIO::section *vuText = writer.sections.add(".vutext");
    vuText->set_type(ELFIO::SHT_PROGBITS);
    vuText->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    vuText->set_addr_align(16);
    vuText->set_address(0x00250000u);
    const std::array<uint32_t, 4> vuWords{0x01EC48BDu, 0u, 0x01FA717Du, 0u};
    vuText->set_data(reinterpret_cast<const char *>(vuWords.data()), sizeof(vuWords));

    ELFIO::section *strtab = writer.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    strtab->set_addr_align(1);

    ELFIO::section *symtab = writer.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_info(1);
    symtab->set_link(strtab->get_index());
    symtab->set_addr_align(4);
    symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));

    ELFIO::symbol_section_accessor symbols(writer, symtab);
    ELFIO::string_section_accessor strings(strtab);
    symbols.add_symbol(strings, "", 0, 0, ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
    symbols.add_symbol(strings, "ee_entry", text->get_address(), text->get_size(),
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());
    symbols.add_symbol(strings, "vu_program", vuText->get_address(), vuText->get_size(),
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, vuText->get_index());

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    ELFIO::segment *vuSegment = writer.segments.add();
    vuSegment->set_type(ELFIO::PT_LOAD);
    vuSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    vuSegment->set_align(0x1000);
    vuSegment->add_section_index(vuText->get_index(), vuText->get_addr_align());

    return writer.save(elfPath.string());
}

static bool writeMinimalMipsElfWithUnmappedEntryHint(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);
    const std::array<uint32_t, 10> textWords = {
        0x03E00008u, 0x00000000u, // known function at 0x00100000
        0x00000000u, 0x00000000u,
        0x03E00008u, 0x00000000u, // omitted entry at 0x00100010
        0x00000000u, 0x00000000u,
        0x03E00008u, 0x00000000u, // next known function at 0x00100020
    };
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::section *strtab = writer.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    strtab->set_addr_align(1);

    ELFIO::section *symtab = writer.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_info(1);
    symtab->set_link(strtab->get_index());
    symtab->set_addr_align(4);
    symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));

    ELFIO::symbol_section_accessor symbols(writer, symtab);
    ELFIO::string_section_accessor strings(strtab);
    symbols.add_symbol(strings, "", 0, 0,
                       ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
    symbols.add_symbol(strings, "known_before", 0x00100000u, 8u,
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());
    symbols.add_symbol(strings, "stubbed_owner", 0x00100008u, 0x18u,
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());
    symbols.add_symbol(strings, "known_after", 0x00100020u, 8u,
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

static bool writeMinimalMipsElfWithStandaloneEntryBlock(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    const std::array<uint32_t, 8> textWords = {
        0x0C040004u, // entry_00100000: jal 0x00100010
        0x00000000u, // nop
        0x03E00008u, // jr $ra (the JAL return address, 0x00100008, is a resume point)
        0x00000000u, // nop
        0x03E00008u, // callee at 0x00100010: jr $ra
        0x00000000u, // nop
        0x03E00008u, // other at 0x00100018: jr $ra
        0x00000000u  // nop
    };
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::section *strtab = writer.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    strtab->set_addr_align(1);

    ELFIO::section *symtab = writer.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_info(1);
    symtab->set_link(strtab->get_index());
    symtab->set_addr_align(4);
    symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));

    ELFIO::symbol_section_accessor symbols(writer, symtab);
    ELFIO::string_section_accessor strings(strtab);
    symbols.add_symbol(strings, "", 0, 0,
                       ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
    symbols.add_symbol(strings, "entry_00100000", 0x00100000u, 0x10u,
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());
    symbols.add_symbol(strings, "callee", 0x00100010u, 8u,
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());
    symbols.add_symbol(strings, "other", 0x00100018u, 8u,
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

// A leaf whose symbol is one byte long, as some disassemblers export it, so its
// "jr $ra" delay slot lies past the symbol's end and outside every function.
static bool writeMinimalMipsElfWithDelaySlotPastFunctionEnd(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    const std::array<uint32_t, 4> textWords = {
        0x03E00008u, // short_leaf: jr $ra
        0x8C820064u, // lw $v0, 0x64($a0) (delay slot, past the symbol's end)
        0x03E00008u, // next: jr $ra
        0x00000000u  // nop
    };
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::section *strtab = writer.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    strtab->set_addr_align(1);

    ELFIO::section *symtab = writer.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_info(1);
    symtab->set_link(strtab->get_index());
    symtab->set_addr_align(4);
    symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));

    ELFIO::symbol_section_accessor symbols(writer, symtab);
    ELFIO::string_section_accessor strings(strtab);
    symbols.add_symbol(strings, "", 0, 0,
                       ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
    symbols.add_symbol(strings, "short_leaf", 0x00100000u, 1u,
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());
    symbols.add_symbol(strings, "next", 0x00100008u, 8u,
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

static bool writeMinimalMipsElfWithAddressTakenCallbacks(const std::filesystem::path &elfPath,
                                                         bool includePartialDwarf = false)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    std::array<uint32_t, 384> textWords{};
    textWords[0] = 0x3C040010u;  // lui a0,0x10
    textWords[1] = 0xAC800000u;  // sw zero,0(a0)
    textWords[2] = 0x0C040008u;  // jal 0x00100020 (callback registrar)
    textWords[3] = 0x24840040u;  // addiu a0,a0,0x40 (delay slot)
    textWords[4] = 0x03E00008u;  // jr ra
    textWords[5] = 0x00000000u;  // nop
    textWords[6] = 0x3C080010u;  // lui t0,0x10
    textWords[7] = 0x25080300u;  // addiu t0,t0,0x300 (code label, not a callback argument)

    textWords[8] = 0x03E00008u;  // registrar at 0x00100020
    textWords[9] = 0x00000000u;

    textWords[16] = 0x27BDFFF0u; // callback at 0x00100040: addiu sp,sp,-0x10
    textWords[17] = 0xFFBF0000u; // sd ra,0(sp)
    textWords[18] = 0xDFBF0000u; // ld ra,0(sp)
    textWords[19] = 0x03E00008u; // jr ra
    textWords[20] = 0x27BD0010u; // addiu sp,sp,0x10

    textWords[24] = 0x08040008u; // table leaf thunk at 0x00100060: j 0x00100020
    textWords[25] = 0x00000000u; // nop (delay slot)
    textWords[26] = 0x03E00008u; // table leaf at 0x00100068
    textWords[27] = 0x00000000u;
    textWords[28] = 0x03E00008u; // adjacent leaf thunk at 0x00100070
    textWords[29] = 0x00000000u;

    // Address-taken initializer at 0x00100080 with a long constant-setup
    // preamble before its stack frame, matching retail constructor tables.
    textWords[32] = 0x3C020010u; // lui v0,0x10
    textWords[33] = 0x3C030010u; // lui v1,0x10
    textWords[34] = 0x3C050010u; // lui a1,0x10
    textWords[35] = 0x3C060010u; // lui a2,0x10
    textWords[36] = 0x3C070010u; // lui a3,0x10
    textWords[37] = 0x3C080010u; // lui t0,0x10
    textWords[38] = 0x3C090010u; // lui t1,0x10
    textWords[39] = 0x3C0A0010u; // lui t2,0x10
    textWords[40] = 0x3C0B0010u; // lui t3,0x10
    textWords[41] = 0x27BDFFF0u; // addiu sp,sp,-0x10
    textWords[42] = 0x03E00008u; // jr ra
    textWords[43] = 0x27BD0010u; // addiu sp,sp,0x10 (delay slot)

    // A callback address completed three instructions before the registrar call.
    // Its leaf body is deliberately longer than a small thunk and begins after a
    // preceding function's return, matching stripped retail ELF callback ranges.
    textWords[44] = 0x3C060010u; // lui a2,0x10
    textWords[45] = 0x7FB00010u; // sq s0,0x10(sp)
    textWords[46] = 0xFFBF0000u; // sd ra,0(sp)
    textWords[47] = 0x24C600E0u; // addiu a2,a2,0xE0 (callback at 0x001000E0)
    textWords[48] = 0x24040008u; // addiu a0,zero,8
    textWords[49] = 0x2405040Fu; // addiu a1,zero,0x40F
    textWords[50] = 0x0C040008u; // jal 0x00100020 (callback registrar)
    textWords[51] = 0x00000000u; // nop (delay slot)

    textWords[56] = 0x3C020010u; // long leaf callback at 0x001000E0
    textWords[57] = 0x8C420200u;
    textWords[58] = 0x3C030020u;
    textWords[59] = 0x24630100u;
    textWords[60] = 0x3C068000u;
    textWords[61] = 0x24420001u;
    textWords[62] = 0x00A31821u;
    textWords[63] = 0x3C010010u;
    textWords[64] = 0xAC420200u;
    textWords[65] = 0xAC660004u;
    textWords[66] = 0x0080102Du;
    textWords[67] = 0x3C010010u;
    textWords[68] = 0xAC450204u;
    textWords[69] = 0x03E00008u; // jr ra, beyond the old eight-word leaf window
    textWords[70] = 0xAC600000u; // sw zero,0(v1) (delay slot)

    // A long leaf method referenced only by a clustered descriptor table. Its
    // return is deliberately beyond the materialized-callback probe distance.
    textWords[72] = 0x8C850014u; // lw a1,0x14(a0), method at 0x00100120
    for (size_t index = 73; index < 121; ++index)
    {
        textWords[index] = 0x24420001u; // addiu v0,v0,1
    }
    textWords[121] = 0x03E00008u; // jr ra at method instruction 49
    textWords[122] = 0x00000000u; // nop (delay slot)

    // Some retail callback registrars keep an address in a saved register while
    // assembling the remaining arguments, then copy it into a2 immediately before
    // the JAL. The call is deliberately well beyond any small lookahead window.
    textWords[128] = 0x3C140010u; // lui s4,0x10
    textWords[129] = 0x26940280u; // addiu s4,s4,0x280 (callback at 0x00100280)
    textWords[130] = 0x7FB00060u; // sq s0,0x60(sp)
    textWords[131] = 0x7FB10050u; // sq s1,0x50(sp)
    textWords[132] = 0x24070001u; // addiu a3,zero,1
    textWords[133] = 0x7FB20040u; // sq s2,0x40(sp)
    textWords[134] = 0x0000202Du; // daddu a0,zero,zero
    textWords[135] = 0x7FB30030u; // sq s3,0x30(sp)
    textWords[136] = 0xFFBF0000u; // sd ra,0(sp)
    textWords[137] = 0x2405011Fu; // addiu a1,zero,0x11F
    textWords[138] = 0x0C040008u; // setup call; s4 must preserve the incomplete address
    textWords[139] = 0x00000000u; // nop (delay slot)
    textWords[144] = 0x0280302Du; // daddu a2,s4,zero
    textWords[146] = 0x0C040008u; // jal 0x00100020 (callback registrar)
    textWords[147] = 0x00000000u; // nop (delay slot)

    // A retail-style conditional initializes a callback register in its delay
    // slot, then completes the address only in the taken successor block. A
    // linear lookahead cannot connect these two halves; CFG traversal must.
    textWords[148] = 0x04410004u; // bgez v0,0x00100264
    textWords[149] = 0x3C060010u; // lui a2,0x10 (delay slot)
    textWords[150] = 0x10000008u; // b 0x0010027C (not-taken path)
    textWords[151] = 0x00000000u; // nop (delay slot)
    textWords[153] = 0x24C602C0u; // addiu a2,a2,0x2C0
    textWords[154] = 0x0C040008u; // jal 0x00100020 (callback registrar)
    textWords[155] = 0x24040008u; // addiu a0,zero,8 (delay slot)

    textWords[160] = 0x3C030010u; // callback at 0x00100280
    textWords[161] = 0x8C630200u;
    textWords[162] = 0x24630001u;
    textWords[163] = 0xAC630200u;
    textWords[164] = 0x03E00008u; // jr ra
    textWords[165] = 0x0080102Du; // daddu v0,a0,zero (delay slot)

    textWords[176] = 0x8F830000u; // callback at 0x001002C0: lw v1,0(gp)
    textWords[177] = 0x0080102Du; // daddu v0,a0,zero
    textWords[178] = 0x2405FFFFu; // addiu a1,zero,-1
    textWords[179] = 0x00832021u; // addu a0,a0,v1
    textWords[180] = 0x03E00008u; // jr ra
    textWords[181] = 0xAC850000u; // sw a1,0(a0) (delay slot)

    // Retail class constructors often build their method tables in writable
    // memory instead of shipping literal function pointers in .rodata. The
    // materialized code address is never passed to a registrar; storing it in
    // the descriptor is the only address-taken evidence.
    textWords[184] = 0x3C040010u; // lui a0,0x10
    textWords[185] = 0x24840340u; // addiu a0,a0,0x340 (method at 0x00100340)
    textWords[186] = 0xAE44001Cu; // sw a0,0x1c(s2)
    textWords[187] = 0x0000202Du; // daddu a0,zero,zero (clobber)

    textWords[208] = 0x3C020020u; // stored leaf method at 0x00100340
    textWords[209] = 0x03E00008u; // jr ra
    textWords[210] = 0x24420100u; // addiu v0,v0,0x100 (delay slot)

    // Long leaf in an alternating (function pointer, numeric id) table. This
    // is a common stripped retail dispatch-table layout and provides strong
    // address evidence through the adjacent ordinary function pointer.
    textWords[224] = 0x3C010020u; // long leaf at 0x00100380
    for (size_t index = 225; index < 235; ++index)
    {
        textWords[index] = 0x24420001u;
    }
    textWords[235] = 0x03E00008u;
    textWords[236] = 0x00000000u;

    // A stripped function map may merge the middle member of a run of trivial
    // leaf accessors into its predecessor. Only the first accessor is reached by
    // a direct call; the second still needs its own callable entry.
    textWords[188] = 0x0C0400F0u; // jal 0x001003C0
    textWords[189] = 0x00000000u; // nop (delay slot)
    textWords[192] = 0x03E00008u; // isolated pointer target at 0x00100300
    textWords[193] = 0x00000000u; // nop (delay slot)
    textWords[240] = 0x03E00008u; // known leaf at 0x001003C0: jr ra
    textWords[241] = 0x0080102Du; // daddu v0,a0,zero (delay slot)
    textWords[242] = 0x03E00008u; // merged leaf at 0x001003C8: jr ra
    textWords[243] = 0x0080102Du; // daddu v0,a0,zero (delay slot)

    // Some retail registrars take more than four register arguments. The fifth
    // callback is passed in physical t0, followed by unrelated argument setup
    // before the call. It must remain distinguishable from the dead t0 code
    // materialization at 0x00100018 above.
    textWords[196] = 0x3C080010u; // lui t0,0x10
    textWords[197] = 0x250803E0u; // addiu t0,t0,0x3E0 (callback at 0x001003E0)
    textWords[198] = 0x24040014u; // addiu a0,zero,0x14
    textWords[199] = 0x0C040008u; // jal 0x00100020 (callback registrar)
    textWords[200] = 0x24050A0Bu; // addiu a1,zero,0xA0B (delay slot)

    textWords[248] = 0x03E00008u; // extended-argument leaf at 0x001003E0: jr ra
    textWords[249] = 0x0080102Du; // daddu v0,a0,zero (delay slot)

    // The fifth register argument can also reference a substantial leaf body.
    // Its return deliberately lies beyond the old fixed 64-instruction scan
    // window, so discovery must follow the candidate's reachable control flow.
    textWords[201] = 0x3C080010u; // lui t0,0x10
    textWords[202] = 0x25080400u; // addiu t0,t0,0x400 (callback at 0x00100400)
    textWords[203] = 0x24040030u; // addiu a0,zero,0x30
    textWords[204] = 0x0C040008u; // jal 0x00100020 (callback registrar)
    textWords[205] = 0x24050A06u; // addiu a1,zero,0xA06 (delay slot)

    textWords[256] = 0x3C080048u; // long extended-argument leaf at 0x00100400
    for (size_t index = 257; index < 336; ++index)
    {
        textWords[index] = 0x24420001u; // addiu v0,v0,1
    }
    textWords[336] = 0x03E00008u; // jr ra at instruction 80
    textWords[337] = 0x00000000u; // nop (delay slot)

    // Stripped function maps can merge a normal non-leaf function into the
    // preceding function even though the boundary is unambiguous in the bytes:
    // `jr ra`, its delay slot, then a fresh stack allocation.
    textWords[338] = 0x27BDFFF0u; // post-return function at 0x00100548
    textWords[339] = 0xFFBF0000u; // sd ra,0(sp)
    textWords[340] = 0x03E00008u; // jr ra
    textWords[341] = 0x27BD0010u; // addiu sp,sp,0x10 (delay slot)

    // The target at 0x00100580 has only a singleton initialized-data pointer.
    // A known function loads that slot and invokes it through JALR, matching
    // retail callback slots that are not large enough to look like a table.
    textWords[30] = 0x0C0400D4u;  // jal 0x00100350
    textWords[31] = 0x00000000u;  // nop (delay slot)
    textWords[212] = 0x27BDFFF0u; // indirect caller at 0x00100350
    textWords[213] = 0xFFBF0000u; // sd ra,0(sp)
    textWords[214] = 0x3C100020u; // lui s0,0x20
    textWords[215] = 0x8E021000u; // lw v0,0x1000(s0) -> [0x00201000]
    textWords[216] = 0x00000000u; // nop
    textWords[217] = 0x0040F809u; // jalr v0
    textWords[218] = 0x00000000u; // nop (delay slot)
    textWords[219] = 0xDFBF0000u; // ld ra,0(sp)
    textWords[220] = 0x03E00008u; // jr ra
    textWords[221] = 0x27BD0010u; // addiu sp,sp,0x10 (delay slot)
    textWords[352] = 0x27BDFFF0u; // singleton data target at 0x00100580
    textWords[353] = 0x0320F809u; // jalr t9
    textWords[354] = 0x0200202Du; // daddu a0,s0,zero (delay slot at 0x00100588)
    textWords[355] = 0x24420001u; // addiu v0,v0,1
    textWords[356] = 0x24420001u; // addiu v0,v0,1
    textWords[357] = 0x03E00008u; // jr ra
    textWords[358] = 0x27BD0010u; // addiu sp,sp,0x10 (delay slot)

    // A second initialized-data word deliberately points at 0x00100588, the
    // delay slot of the JALR above. Its following body can look callable to a
    // reachability probe, but splitting there would truncate the real owner.
    textWords[12] = 0x0C040170u;  // jal 0x001005C0
    textWords[13] = 0x00000000u;  // nop (delay slot)
    textWords[368] = 0x27BDFFF0u; // delay-slot pointer caller at 0x001005C0
    textWords[369] = 0x3C100020u; // lui s0,0x20
    textWords[370] = 0x8E021040u; // lw v0,0x1040(s0) -> [0x00201040]
    textWords[371] = 0x0040F809u; // jalr v0
    textWords[372] = 0x00000000u; // nop (delay slot)
    textWords[373] = 0x03E00008u; // jr ra
    textWords[374] = 0x27BD0010u; // addiu sp,sp,0x10 (delay slot)

    // As in ps2autotests kernel/ee/thread/delete.elf: the LUI of a thread entry sits in a JAL delay slot, the
    // ADDIU completes it in callee-saved s0 after the call, and s0 is later passed as an argument. The entry is a
    // `jr ra` leaf placed right after a `syscall; nop` with no return of its own before it.
    textWords[300] = 0x27BDFFF0u; // caller at 0x001004B0: addiu sp,sp,-0x10
    textWords[301] = 0xFFBF0000u; // sd ra,0(sp)
    textWords[302] = 0x0C040008u; // jal 0x00100020
    textWords[303] = 0x3C100010u; // lui s0,0x10 (delay slot)
    textWords[304] = 0x261004F0u; // addiu s0,s0,0x4F0 -> 0x001004F0
    textWords[305] = 0x0C040008u; // jal 0x00100020
    textWords[306] = 0x00000000u; // nop (delay slot)
    textWords[307] = 0x0200202Du; // daddu a0,s0,zero
    textWords[308] = 0x0C040008u; // jal 0x00100020
    textWords[309] = 0x00000000u; // nop (delay slot)
    textWords[310] = 0xDFBF0000u; // ld ra,0(sp)
    textWords[311] = 0x03E00008u; // jr ra
    textWords[312] = 0x27BD0010u; // addiu sp,sp,0x10 (delay slot)
    textWords[313] = 0x24030023u; // addiu v1,zero,0x23
    textWords[314] = 0x0000000Cu; // syscall (ExitThread, does not return)
    textWords[315] = 0x00000000u; // nop
    textWords[316] = 0x03E00008u; // thread entry at 0x001004F0: jr ra
    textWords[317] = 0x00000000u; // nop (delay slot)

    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::section *rodata = writer.sections.add(".rodata");
    rodata->set_type(ELFIO::SHT_PROGBITS);
    rodata->set_flags(ELFIO::SHF_ALLOC);
    rodata->set_addr_align(4);
    rodata->set_address(0x00200000u);

    std::array<uint32_t, 48> tableWords{};
    tableWords[1] = 0x00100060u;
    tableWords[3] = 0x00100068u;
    // Retail class descriptor: name pointer, ordinary method, two reserved
    // words, then a long leaf method.
    tableWords[5] = 0x0020004Cu;
    tableWords[6] = 0x00100080u;
    tableWords[7] = 0;
    tableWords[8] = 0;
    tableWords[9] = 0x00100120u;
    tableWords[16] = 0x00100300u; // plausible entry, but not part of a pointer cluster
    // A pointer-shaped data word naming the delay slot of `jal` at 0x00100008 (as in ps2autotests
    // dma/vif/stcycl.elf), clustered with a real leaf so the table heuristic would accept it.
    tableWords[42] = 0x0010000Cu; // far from tableWords[16], which must stay an isolated pointer
    tableWords[44] = 0x00100068u;
    tableWords[28] = 0x00100080u; // ordinary function, followed by a numeric id
    tableWords[29] = 0x0000000Du;
    tableWords[30] = 0x00100380u; // long leaf, followed by a numeric id
    tableWords[31] = 0x0000000Bu;
    rodata->set_data(reinterpret_cast<const char *>(tableWords.data()),
                     static_cast<ELFIO::Elf_Word>(tableWords.size() * sizeof(uint32_t)));

    ELFIO::section *data = writer.sections.add(".data");
    data->set_type(ELFIO::SHT_PROGBITS);
    data->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    data->set_addr_align(4);
    data->set_address(0x00201000u);
    std::array<uint32_t, 17> singletonCallbacks{};
    singletonCallbacks[0] = 0x00100580u;
    singletonCallbacks[16] = 0x00100588u;
    data->set_data(reinterpret_cast<const char *>(singletonCallbacks.data()),
                   static_cast<ELFIO::Elf_Word>(singletonCallbacks.size() * sizeof(uint32_t)));

    if (includePartialDwarf)
    {
        // A retail ELF can retain debug information for only part of its code.
        // The parser must still supplement that incomplete map with static
        // address-taken discovery instead of treating any DWARF as exhaustive.
        const std::array<uint8_t, 19> abbrevBytes = {
            0x01, 0x11, 0x01, // abbrev 1: compile_unit, has children
            0x03, 0x08,       // DW_AT_name, DW_FORM_string
            0x00, 0x00,
            0x02, 0x2E, 0x00, // abbrev 2: subprogram, no children
            0x03, 0x08,       // DW_AT_name, DW_FORM_string
            0x11, 0x01,       // DW_AT_low_pc, DW_FORM_addr
            0x12, 0x06,       // DW_AT_high_pc, DW_FORM_data4
            0x00, 0x00,       // end of attribute list
            0x00};             // end of abbreviation table

        ELFIO::section *debugAbbrev = writer.sections.add(".debug_abbrev");
        debugAbbrev->set_type(ELFIO::SHT_PROGBITS);
        debugAbbrev->set_addr_align(1);
        debugAbbrev->set_data(reinterpret_cast<const char *>(abbrevBytes.data()),
                              static_cast<ELFIO::Elf_Word>(abbrevBytes.size()));

        std::vector<uint8_t> infoBytes(sizeof(uint32_t), 0);
        auto appendU8 = [&infoBytes](uint8_t value)
        { infoBytes.push_back(value); };
        auto appendU16 = [&infoBytes](uint16_t value)
        {
            const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
            infoBytes.insert(infoBytes.end(), bytes, bytes + sizeof(value));
        };
        auto appendU32 = [&infoBytes](uint32_t value)
        {
            const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
            infoBytes.insert(infoBytes.end(), bytes, bytes + sizeof(value));
        };
        auto appendString = [&infoBytes](std::string_view value)
        {
            infoBytes.insert(infoBytes.end(), value.begin(), value.end());
            infoBytes.push_back(0);
        };

        appendU16(4);          // DWARF version
        appendU32(0);          // abbreviation table offset
        appendU8(4);           // address size
        appendU8(1);           // compile-unit DIE
        appendString("partial-unit");
        appendU8(2);           // subprogram DIE
        appendString("known_partial_function");
        appendU32(0x00100000u);
        appendU32(0x20u);      // DWARF 4 high_pc offset
        appendU8(2);           // a later known subprogram bounds fallback ranges
        appendString("known_tail_function");
        appendU32(0x001003F0u);
        appendU32(0x10u);
        appendU8(0);           // end compile-unit children

        const uint32_t unitLength = static_cast<uint32_t>(infoBytes.size() - sizeof(uint32_t));
        std::memcpy(infoBytes.data(), &unitLength, sizeof(unitLength));

        ELFIO::section *debugInfo = writer.sections.add(".debug_info");
        debugInfo->set_type(ELFIO::SHT_PROGBITS);
        debugInfo->set_addr_align(1);
        debugInfo->set_data(reinterpret_cast<const char *>(infoBytes.data()),
                            static_cast<ELFIO::Elf_Word>(infoBytes.size()));
    }

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    ELFIO::segment *dataSegment = writer.segments.add();
    dataSegment->set_type(ELFIO::PT_LOAD);
    dataSegment->set_flags(ELFIO::PF_R);
    dataSegment->set_align(0x1000);
    dataSegment->add_section_index(rodata->get_index(), rodata->get_addr_align());
    dataSegment->add_section_index(data->get_index(), data->get_addr_align());

    return writer.save(elfPath.string());
}

static bool writeMinimalMipsElfWithInitializer(const std::filesystem::path &elfPath,
                                               const std::string &functionName,
                                               uint32_t initializerTarget)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);
    const std::array<uint32_t, 2> textWords = {
        0x03E00008u, // jr $ra
        0x00000000u, // nop
    };
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::section *ctors = writer.sections.add(".ctors");
    ctors->set_type(ELFIO::SHT_PROGBITS);
    ctors->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    ctors->set_addr_align(4);
    ctors->set_address(0x00200000u);
    ctors->set_data(reinterpret_cast<const char *>(&initializerTarget),
                    static_cast<ELFIO::Elf_Word>(sizeof(initializerTarget)));

    ELFIO::section *strtab = writer.sections.add(".strtab");
    strtab->set_type(ELFIO::SHT_STRTAB);
    strtab->set_addr_align(1);

    ELFIO::section *symtab = writer.sections.add(".symtab");
    symtab->set_type(ELFIO::SHT_SYMTAB);
    symtab->set_info(1);
    symtab->set_link(strtab->get_index());
    symtab->set_addr_align(4);
    symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));

    ELFIO::symbol_section_accessor symbols(writer, symtab);
    ELFIO::string_section_accessor strings(strtab);
    symbols.add_symbol(strings, "", 0, 0,
                       ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
    symbols.add_symbol(strings, functionName.c_str(), text->get_address(), text->get_size(),
                       ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    ELFIO::segment *dataSegment = writer.segments.add();
    dataSegment->set_type(ELFIO::PT_LOAD);
    dataSegment->set_flags(ELFIO::PF_R | ELFIO::PF_W);
    dataSegment->set_align(0x1000);
    dataSegment->add_section_index(ctors->get_index(), ctors->get_addr_align());

    return writer.save(elfPath.string());
}

static bool writeRecompilerTestConfig(const std::filesystem::path &configPath,
                                      const std::filesystem::path &elfPath,
                                      const std::filesystem::path &outputPath,
                                      const std::vector<std::string> &skip,
                                      const std::vector<std::string> &stubs = {},
                                      const std::vector<std::string> &entryPoints = {})
{
    std::ofstream config(configPath);
    if (!config)
        return false;

    config << "[general]\n";
    config << "input = \"" << elfPath.generic_string() << "\"\n";
    config << "output = \"" << outputPath.generic_string() << "\"\n";
    config << "skip = [";
    for (size_t i = 0; i < skip.size(); ++i)
    {
        if (i != 0u)
            config << ", ";
        config << '"' << skip[i] << '"';
    }
    config << "]\n";
    config << "stubs = [";
    for (size_t i = 0; i < stubs.size(); ++i)
    {
        if (i != 0u)
            config << ", ";
        config << '"' << stubs[i] << '"';
    }
    config << "]\n";
    config << "entry_points = [";
    for (size_t i = 0; i < entryPoints.size(); ++i)
    {
        if (i != 0u)
            config << ", ";
        config << '"' << entryPoints[i] << '"';
    }
    config << "]\n";
    return static_cast<bool>(config);
}

// Phase 19 — Ghidra oracle completion. Minimal ELF for the oracle tests:
// .text at 0x00100000 (82 words, 0x148 bytes, covering the POC 1 synthetic
// fixture's 0x144-byte text span) plus a non-executable .data at 0x00100200
// (8 words).
// .text word 0 is jal 0x00100028 so pre-map discovery finds a second start,
// exactly like the existing ghidra-merge test's fallback pattern; the rest
// is jr $ra / nop pairs (no further edges).
static bool writeGhidraOracleElf(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    std::array<uint32_t, 82> textWords{};
    textWords[0] = (0x03u << 26) | ((0x00100028u >> 2) & 0x03FFFFFFu); // jal 0x00100028
    textWords[1] = 0x00000000u;                                     // nop (delay slot)
    for (size_t i = 2; i < textWords.size(); i += 2)
    {
        textWords[i] = 0x03E00008u;     // jr $ra
        textWords[i + 1] = 0x00000000u; // nop
    }
    // Gate D — the tile layout models real function boundaries: every
    // oracle row start follows a non-branch word, so no row starts in
    // a delay slot. The POC 1 row starts sit at indices 0, 10, 17, 45,
    // 55, 58, 69, 72; their predecessors are indices 9, 16, 44, 54, 57,
    // 68, 71. Indices 16, 44, 54, 68 would be jr $ra under the plain
    // tiling, which would make those rows delay-slot starts; they are
    // nop instead, matching real code where a function starts after
    // the previous function's delay slot, not in it. Span decoding is
    // unaffected (nop decodes).
    textWords[16] = 0x00000000u; // nop, not jr: predecessor of FUN_00100044
    textWords[44] = 0x00000000u; // nop, not jr: predecessor of FUN_001000b4
    textWords[54] = 0x00000000u; // nop, not jr: predecessor of FUN_001000dc
    textWords[68] = 0x00000000u; // nop, not jr: predecessor of FUN_00100114
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::section *data = writer.sections.add(".data");
    data->set_type(ELFIO::SHT_PROGBITS);
    data->set_flags(ELFIO::SHF_ALLOC);
    data->set_addr_align(4);
    data->set_address(0x00100200u);
    const std::array<uint32_t, 8> dataWords = {0x00100084u, 0x00100000u, 0u, 0u, 0u, 0u, 0u, 0u};
    data->set_data(reinterpret_cast<const char *>(dataWords.data()),
                   static_cast<ELFIO::Elf_Word>(dataWords.size() * sizeof(uint32_t)));

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    ELFIO::segment *dataSegment = writer.segments.add();
    dataSegment->set_type(ELFIO::PT_LOAD);
    dataSegment->set_flags(ELFIO::PF_R);
    dataSegment->set_align(0x1000);
    dataSegment->add_section_index(data->get_index(), data->get_addr_align());

    return writer.save(elfPath.string());
}

static const Function *findFunctionByStart(const std::vector<Function> &functions, uint32_t start)
{
    const auto it = std::find_if(functions.begin(), functions.end(),
                                 [start](const Function &fn) { return fn.start == start; });
    return it == functions.end() ? nullptr : &*it;
}

static bool writeGhidraOracleCsv(const std::filesystem::path &mapPath, const char *rows)
{
    std::ofstream mapFile(mapPath);
    if (!mapFile)
    {
        return false;
    }
    mapFile << "Name,Start,End,Size\n";
    mapFile << rows;
    mapFile.close();
    return static_cast<bool>(mapFile);
}

static void cleanupGhidraOracleFiles(const std::filesystem::path &elfPath,
                                     const std::filesystem::path &mapPath)
{
    std::error_code removeError;
    std::filesystem::remove(elfPath, removeError);
    std::filesystem::remove(mapPath, removeError);
}

static bool loadGhidraOracleElf(ElfParser &parser, const std::filesystem::path &elfPath, TestCase &t)
{
    const bool parseOk = parser.parse();
    t.IsTrue(parseOk, "generated ELF should parse");
    return parseOk;
}

static bool loadGhidraOracleMap(ElfParser &parser, const std::filesystem::path &mapPath, TestCase &t)
{
    const bool mapLoaded = parser.loadGhidraFunctionMap(mapPath.string());
    t.IsTrue(mapLoaded, "ghidra map should load");
    return mapLoaded;
}

static std::filesystem::path ghidraOracleTempPath(const std::string &stem, const std::string &suffix)
{
    const auto uniqueSuffix = std::to_string(
        static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return std::filesystem::temp_directory_path() / (stem + "-" + uniqueSuffix + suffix);
}

// Phase 19 — LOAD: a real POC 1 result CSV flows through the real loader.
// The 8 rows below are copied VERBATIM from the POC 1 stored results
// (research/poc1/results/synthetic_stripped.ghidra.csv), produced by
// Ghidra 12.1.4 + EE extension headless analysis of the synthetic
// stripped fixture and re-verified byte-identical on a fresh 2026-11-03
// re-run. Ends are Ghidra body-max+1; the Size column is the body
// address count, NOT a byte size (f_switch spans 112 bytes but reports
// 76 — its body excludes the jump-table cases), and the loader ignores
// it. This test proves CSV -> map load -> merge -> function list with
// the expected ranges, with no re-implementation or mock in between.
static void runGhidraOracleLoadTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-ghidra-oracle", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-ghidra-oracle", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "entry,0x00100000,0x00100028,40\n"
                                  "FUN_00100028,0x00100028,0x00100044,28\n"
                                  "FUN_00100044,0x00100044,0x001000B4,76\n"
                                  "FUN_001000b4,0x001000B4,0x001000DC,40\n"
                                  "FUN_001000dc,0x001000DC,0x001000E8,12\n"
                                  "FUN_001000e8,0x001000E8,0x00100114,44\n"
                                  "FUN_00100114,0x00100114,0x00100120,12\n"
                                  "FUN_00100120,0x00100120,0x0010013C,28\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.Equals(functions.size(), static_cast<size_t>(8),
             "oracle CSV should yield exactly the 8 exported functions");

    const std::array<std::tuple<const char *, uint32_t, uint32_t>, 8> expected = {{
        {"entry", 0x00100000u, 0x00100028u},
        {"FUN_00100028", 0x00100028u, 0x00100044u},
        {"FUN_00100044", 0x00100044u, 0x001000B4u},
        {"FUN_001000b4", 0x001000B4u, 0x001000DCu},
        {"FUN_001000dc", 0x001000DCu, 0x001000E8u},
        {"FUN_001000e8", 0x001000E8u, 0x00100114u},
        {"FUN_00100114", 0x00100114u, 0x00100120u},
        {"FUN_00100120", 0x00100120u, 0x0010013Cu},
    }};
    for (const auto &[name, start, end] : expected)
    {
        const Function *fn = findFunctionByStart(functions, start);
        t.IsTrue(fn != nullptr, std::string("oracle function should exist at ") + name);
        if (fn != nullptr)
        {
            t.Equals(fn->name, std::string(name), "oracle function name should match the CSV row");
            t.Equals(fn->end, end, "uncontested oracle end should survive the merge");
        }
    }
    const auto &loadStats = parser.ghidraMapLoadStats();
    t.Equals(loadStats.loaded, 8, "oracle load keeps eight rows");
    t.Equals(loadStats.skippedNonExecutable, 0, "oracle load skips nothing non-executable");
    t.Equals(loadStats.skippedInvalidRange, 0, "oracle load skips nothing invalid-range");
    t.Equals(loadStats.skippedMisaligned, 0, "oracle load skips nothing misaligned");
    t.Equals(loadStats.skippedUnalignedEnd, 0, "oracle load skips nothing unaligned-end");
    t.Equals(loadStats.skippedOverlapping, 0, "oracle load skips nothing overlapping");
    t.Equals(loadStats.skippedUndecodable, 0, "oracle load skips nothing undecodable");
    t.Equals(loadStats.skippedDelaySlot, 0, "oracle load skips nothing delay-slot");
    t.Equals(loadStats.skippedMalformed, 0, "oracle load skips nothing malformed");
    t.IsTrue(loadStats.headerSeen, "oracle load sees the exporter header");
    t.IsFalse(loadStats.rowLimitExceeded, "oracle load does not hit the row cap");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Phase 19 — MUTANT: CSV entries failing Zmbor's validation must NOT
// become functions. Non-executable-section and out-of-range rows were
// already skipped by the loader; misaligned rows were NOT (no alignment
// check existed anywhere in the loader or addOrMerge path) until the
// Phase 19 loader fix — this test pins all three.
static void runGhidraOracleMutantTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-ghidra-oracle-mutant", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-ghidra-oracle-mutant", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "entry,0x00100000,0x00100028,40\n"
                                  "FUN_00100028,0x00100028,0x00100044,28\n"
                                  "FUN_00100044,0x00100044,0x001000B4,76\n"
                                  "BAD_MISALIGNED,0x00100002,0x00100010,0x0E\n"
                                  "BAD_DATA,0x00100200,0x00100210,0x10\n"
                                  "BAD_FAR,0x00200000,0x00200010,0x10\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.Equals(functions.size(), static_cast<size_t>(3),
             "only the valid oracle rows should become functions");
    t.IsTrue(findFunctionByStart(functions, 0x00100000u) != nullptr,
             "valid entry row should survive");
    t.IsTrue(findFunctionByStart(functions, 0x00100002u) == nullptr,
             "misaligned CSV row must NOT become a function");
    t.IsTrue(findFunctionByStart(functions, 0x00100200u) == nullptr,
             "non-executable-section CSV row must NOT become a function");
    t.IsTrue(findFunctionByStart(functions, 0x00200000u) == nullptr,
             "out-of-range CSV row must NOT become a function");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Phase 19 — LABEL PROMOTION (documented hazard, not a feature), as
// redesigned by Gate D: the label-collection port emits rows in the
// SAME Name,Start,End,Size format with entry_* names, and entry_ is
// NOT an auto-generated name per IsAutoGeneratedName — so the loader
// promotes them as named extras with authoritative ends. Gate D's
// overlap rule now rejects label rows that fall INSIDE an accepted
// function row (both synthetic in-body labels below sit inside
// FUN_00100044's [0x44, 0xB4) span), which is a behaviour improvement,
// not a filter: the rule is name-blind, it rejects any overlapping
// row. The hazard itself persists and is re-pinned by the gap label
// at 0x140, which overlaps nothing and still promotes — that is
// exactly why the label export must NOT be a pipeline input, only
// the functions-only export.
static void runGhidraOracleLabelPromotionTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-ghidra-oracle-label", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-ghidra-oracle-label", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "entry,0x00100000,0x00100028,40\n"
                                  "FUN_00100028,0x00100028,0x00100044,28\n"
                                  "FUN_00100044,0x00100044,0x001000B4,76\n"
                                  "entry_00100084,0x00100084,0x001000A8,36\n"
                                  "entry_001000a8,0x001000A8,0x001000B4,12\n"
                                  "FUN_001000b4,0x001000B4,0x001000DC,40\n"
                                  "FUN_001000dc,0x001000DC,0x001000E8,12\n"
                                  "FUN_001000e8,0x001000E8,0x00100114,44\n"
                                  "FUN_00100114,0x00100114,0x00100120,12\n"
                                  "FUN_00100120,0x00100120,0x0010013C,28\n"
                                  "entry_00100140,0x00100140,0x00100144,4\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.Equals(functions.size(), static_cast<size_t>(9),
             "8 function rows plus the non-overlapping gap label should survive");
    t.IsTrue(findFunctionByStart(functions, 0x00100084u) == nullptr,
             "in-body switch-case label row must be rejected by the overlap rule");
    t.IsTrue(findFunctionByStart(functions, 0x001000A8u) == nullptr,
             "in-body switch-default label row must be rejected by the overlap rule");
    const Function *gapLabel = findFunctionByStart(functions, 0x00100140u);
    t.IsTrue(gapLabel != nullptr, "non-overlapping gap label row still promotes (hazard pinned)");
    if (gapLabel != nullptr)
    {
        t.Equals(gapLabel->end, 0x00100144u, "gap label row end should survive the merge");
    }
    const auto &labelStats = parser.ghidraMapLoadStats();
    t.Equals(labelStats.loaded, 9, "label load keeps nine rows");
    t.Equals(labelStats.skippedOverlapping, 2, "exactly two in-body label rows counted overlapping");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

static void registerGhidraOracleTests(TestCase &tc)
{
    tc.Run("ghidra oracle CSV loads end to end with verbatim POC 1 synthetic rows",
           [](TestCase &t) { runGhidraOracleLoadTest(t); });
    tc.Run("ghidra map rejects misaligned, non-executable, and out-of-range rows",
           [](TestCase &t) { runGhidraOracleMutantTest(t); });
    tc.Run("ghidra label rows promote as functions (label-feed hazard pinned)",
           [](TestCase &t) { runGhidraOracleLabelPromotionTest(t); });
}

// Gate D (S6) — Ghidra loader validation. All tests below reuse the
// oracle ELF (0x00100000 tiles, Gate D nop layout) unless they build
// their own ELF. Discovery from the oracle entry finds exactly
// {0x00100000, 0x00100028} (entry jal + jr $ra), so any other start in
// the merged output comes from the map under test, never discovery.
// Fail-before note: every rejection test below fails against the
// Phase 19 loader (the rejected row promotes there) and passes after
// the Gate D loader fixes, except the symbol-precedence guard, which
// passes in both (it pins downstream behaviour the loader must not
// disturb).

// Verbatim CSV writer: unlike writeGhidraOracleCsv it does NOT inject
// a header line, so headerless-file tests control the first line.
static bool writeGhidraOracleCsvRaw(const std::filesystem::path &mapPath, const std::string &content)
{
    std::ofstream mapFile(mapPath);
    if (!mapFile)
    {
        return false;
    }
    mapFile << content;
    mapFile.close();
    return static_cast<bool>(mapFile);
}

// The 8 verbatim POC 1 rows, shared by the Gate D tests that need the
// full accepted set (overlap and unaligned-end tests need the
// neighbouring rows present for span claims).
static const char *kGateDOracleRows =
    "entry,0x00100000,0x00100028,40\n"
    "FUN_00100028,0x00100028,0x00100044,28\n"
    "FUN_00100044,0x00100044,0x001000B4,76\n"
    "FUN_001000b4,0x001000B4,0x001000DC,40\n"
    "FUN_001000dc,0x001000DC,0x001000E8,12\n"
    "FUN_001000e8,0x001000E8,0x00100114,44\n"
    "FUN_00100114,0x00100114,0x00100120,12\n"
    "FUN_00100120,0x00100120,0x0010013C,28\n";

// Gate D gap 1 — unaligned end. The 0x44 row's end is made unaligned
// (0xB3); every other row is verbatim, so the only change is the end.
static void runGateDUnalignedEndTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-end", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-end", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "entry,0x00100000,0x00100028,40\n"
                                  "FUN_00100028,0x00100028,0x00100044,28\n"
                                  "FUN_00100044,0x00100044,0x001000B3,76\n"
                                  "FUN_001000b4,0x001000B4,0x001000DC,40\n"
                                  "FUN_001000dc,0x001000DC,0x001000E8,12\n"
                                  "FUN_001000e8,0x001000E8,0x00100114,44\n"
                                  "FUN_00100114,0x00100114,0x00100120,12\n"
                                  "FUN_00100120,0x00100120,0x0010013C,28\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x00100044u) == nullptr,
             "row with unaligned end must NOT become a function");
    t.IsTrue(findFunctionByStart(functions, 0x001000B4u) != nullptr,
             "neighbouring valid row must still promote");
    t.IsTrue(findFunctionByStart(functions, 0x00100000u) != nullptr,
             "entry row must still promote");
    const auto &endStats = parser.ghidraMapLoadStats();
    t.Equals(endStats.loaded, 7, "unaligned-end load keeps seven rows");
    t.Equals(endStats.skippedUnalignedEnd, 1, "exactly one unaligned-end row counted");
    t.IsTrue(endStats.headerSeen, "exporter header recognized");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D gap 2 — overlapping ranges. The full 8-row set plus one row
// strictly inside FUN_00100044's span; adjacency (0xB4 touching 0x44's
// end) is not overlap and must keep promoting.
static void runGateDOverlapTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-overlap", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-overlap", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    std::string rows = kGateDOracleRows;
    rows += "FUN_OVERLAP,0x00100084,0x001000A8,36\n";
    t.IsTrue(writeGhidraOracleCsv(mapPath, rows.c_str()), "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.Equals(functions.size(), static_cast<size_t>(8),
             "overlapping row must not add a ninth function");
    t.IsTrue(findFunctionByStart(functions, 0x00100084u) == nullptr,
             "overlapping row must NOT become a function");
    const Function *covered = findFunctionByStart(functions, 0x00100044u);
    t.IsTrue(covered != nullptr, "first-wins row must survive the overlap");
    if (covered != nullptr)
    {
        t.Equals(covered->end, 0x001000B4u, "first-wins row keeps its end");
    }
    t.IsTrue(findFunctionByStart(functions, 0x001000B4u) != nullptr,
             "adjacent row touching the first-wins end must still promote");
    const auto &overlapStats = parser.ghidraMapLoadStats();
    t.Equals(overlapStats.loaded, 8, "overlap load keeps eight rows");
    t.Equals(overlapStats.skippedOverlapping, 1, "exactly one overlapping row counted");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D gap 3 — decode check, plus R5900-class coverage. .text at
// 0x00100000, 12 words: entry jr/nop, then an INVALID word
// (opcode 0x1D, unassigned — rabbitizer R5900 marks it
// RABBITIZER_INSTR_ID_r5900_INVALID), then MMI/LQ/SQ/COP2 words that
// must NOT be falsely rejected.
static bool writeGateDUndecodableElf(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    const std::array<uint32_t, 12> textWords = {
        0x03E00008u, // 0x100000: jr $ra (entry, no edges: discovery finds only this)
        0x00000000u, // 0x100004: nop
        0x03E00008u, // 0x100008: jr $ra
        0x00000000u, // 0x10000C: nop
        0x74000000u, // 0x100010: opcode 0x1D, unassigned -> INVALID
        0x00000000u, // 0x100014: nop
        0x70000008u, // 0x100018: paddw (MMI, must decode)
        0x78000000u, // 0x10001C: lq (R5900, must decode)
        0x7C000000u, // 0x100020: sq (R5900, must decode)
        0x4A000000u, // 0x100024: vaddx (COP2, must decode)
        0x03E00008u, // 0x100028: jr $ra
        0x00000000u, // 0x10002C: nop
    };
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

static void runGateDUndecodableTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-decode", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-decode", ".csv");

    t.IsTrue(writeGateDUndecodableElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "GOOD,0x00100000,0x00100008,8\n"
                                  "BAD,0x00100010,0x00100018,8\n"
                                  "R5900ROW,0x00100018,0x00100028,16\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x00100000u) != nullptr,
             "decodable entry row must promote");
    t.IsTrue(findFunctionByStart(functions, 0x00100010u) == nullptr,
             "row spanning an undecodable word must NOT become a function");
    const Function *r5900 = findFunctionByStart(functions, 0x00100018u);
    t.IsTrue(r5900 != nullptr, "row of valid MMI/LQ/SQ/COP2 words must promote");
    if (r5900 != nullptr)
    {
        t.Equals(r5900->end, 0x00100028u, "R5900 row keeps its end");
    }
    const auto &decodeStats = parser.ghidraMapLoadStats();
    t.Equals(decodeStats.loaded, 2, "decode load keeps two rows");
    t.Equals(decodeStats.skippedUndecodable, 1, "exactly one undecodable row counted");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D gap 4 — delay-slot start. .text at 0x00100000, 12 words:
// entry jal to the branch function, then a beq at 0x100020 whose
// delay slot is 0x100024. A row starting at the delay slot (with no
// row covering the branch) must be rejected; the row after the delay
// slot must promote.
static bool writeGateDDelaySlotElf(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    const std::array<uint32_t, 12> textWords = {
        0x0C040008u, // 0x100000: jal 0x100020
        0x00000000u, // 0x100004: nop
        0x03E00008u, // 0x100008: jr $ra
        0x00000000u, // 0x10000C: nop
        0x03E00008u, // 0x100010: jr $ra
        0x00000000u, // 0x100014: nop
        0x03E00008u, // 0x100018: jr $ra
        0x00000000u, // 0x10001C: nop
        0x10000001u, // 0x100020: beq $zero, $zero, 1 (target 0x100028)
        0x00000000u, // 0x100024: nop (the beq delay slot)
        0x03E00008u, // 0x100028: jr $ra
        0x00000000u, // 0x10002C: nop
    };
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

static void runGateDDelaySlotTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-delayslot", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-delayslot", ".csv");

    t.IsTrue(writeGateDDelaySlotElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "DELAYSLOT,0x00100024,0x00100028,4\n"
                                  "AFTER,0x00100028,0x00100030,8\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x00100024u) == nullptr,
             "row starting in a branch delay slot must NOT become a function");
    t.IsTrue(findFunctionByStart(functions, 0x00100028u) != nullptr,
             "row starting after the delay slot must promote");
    // Gate D correction: the branch function at 0x100020 is NOT a map
    // row, so the loaded-map set rule (extractFunctions: a discovered
    // start is admitted only if it is in m_ghidraMapStarts) drops it.
    // That is pre-existing Phase 19 merge behavior, unchanged by
    // Gate D — not a consequence of the delay-slot rejection. The
    // original "discovery must survive" assertion contradicted the
    // set rule and could never pass with any map loaded; asserting
    // the set-rule outcome here instead. (The same rule explains the
    // pre-fix row-cap anomaly: uncapped, the trailing TAIL row loaded
    // and the set rule dropped the discovery entry; capped, the load
    // fails and discovery survives.)
    t.IsTrue(findFunctionByStart(functions, 0x00100020u) == nullptr,
             "discovered branch function not listed in the map is dropped by the loaded-map set rule");
    const auto &delayStats = parser.ghidraMapLoadStats();
    t.Equals(delayStats.loaded, 1, "delay-slot load keeps one row");
    t.Equals(delayStats.skippedDelaySlot, 1, "exactly one delay-slot row counted");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D gap 5 — malformed-row accounting. Four malformed rows (short
// row, non-numeric start, extra fifth field, empty start) plus two
// valid rows. The valid rows must promote; the malformed rows must
// produce a warning (observed via a capturing reporter), not silence.
static void runGateDMalformedTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-malformed", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-malformed", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "entry,0x00100000,0x00100028,40\n"
                                  "SHORT,0x00100044,0x001000B4\n"
                                  "BADNUM,XYZ,0x001000B4,10\n"
                                  "EXTRA,0x00100044,0x001000B4,76,EXTRA\n"
                                  "EMPTYNUM,,0x001000B4,10\n"
                                  "VALID,0x001000B4,0x001000DC,40\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    RecompilerReporter reporter;
    parser.setReporter(&reporter);
    if (!loadGhidraOracleElf(parser, elfPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }
    t.IsTrue(parser.loadGhidraFunctionMap(mapPath.string()), "ghidra map should load");

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x001000B4u) != nullptr,
             "valid row must promote despite malformed neighbours");
    t.IsTrue(findFunctionByStart(functions, 0x00100044u) == nullptr,
             "malformed rows must NOT become functions");
    t.IsTrue(reporter.hasWarnings(),
             "malformed rows must produce a warning, not silent skips");
    const auto &malformedStats = parser.ghidraMapLoadStats();
    t.Equals(malformedStats.loaded, 2, "malformed load keeps two valid rows");
    t.Equals(malformedStats.skippedMalformed, 4, "exactly four malformed rows counted");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D gap 6 — headerless file. The first line is a valid data row
// with no header above it; the Phase 19 loader discarded it as the
// assumed header. Both rows must promote now.
static void runGateDHeaderTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-header", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-header", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsvRaw(mapPath,
                                     "FUN_00100044,0x00100044,0x001000B4,76\n"
                                     "FUN_001000B4,0x001000B4,0x001000DC,40\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    RecompilerReporter reporter;
    parser.setReporter(&reporter);
    if (!loadGhidraOracleElf(parser, elfPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }
    t.IsTrue(parser.loadGhidraFunctionMap(mapPath.string()), "ghidra map should load");

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x00100044u) != nullptr,
             "first data row of a headerless file must promote, not be dropped");
    t.IsTrue(findFunctionByStart(functions, 0x001000B4u) != nullptr,
             "second row must promote");
    t.IsTrue(reporter.hasWarnings(),
             "a headerless file must warn that its first line is not the known header");
    const auto &headerStats = parser.ghidraMapLoadStats();
    t.Equals(headerStats.loaded, 2, "headerless load keeps both data rows");
    t.IsFalse(headerStats.headerSeen, "headerless file reports no header seen");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D gap 7 — stoul base-0 octal parsing. .text at 0x00000040,
// 64 jr/nop tile words, entry at 0x100 (a jr word: discovery finds
// only the entry). The row start "0104" must parse as decimal 104
// (0x68), not octal 68 (0x44); the hex row proves 0x-prefixed hex
// still parses.
static bool writeGateDOctalElf(const std::filesystem::path &elfPath)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00000100u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00000040u);

    std::array<uint32_t, 64> textWords{};
    for (size_t i = 0; i < textWords.size(); i += 2)
    {
        textWords[i] = 0x03E00008u;     // jr $ra
        textWords[i + 1] = 0x00000000u; // nop
    }
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

static void runGateDOctalTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-octal", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-octal", ".csv");

    t.IsTrue(writeGateDOctalElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "OCTAL,0104,0x80,28\n"
                                  "HEXROW,0x90,0xA0,16\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x00000068u) != nullptr,
             "\"0104\" must parse as decimal 104 (0x68), not octal 68");
    t.IsTrue(findFunctionByStart(functions, 0x00000044u) == nullptr,
             "octal interpretation (0x44) must NOT appear");
    t.IsTrue(findFunctionByStart(functions, 0x00000090u) != nullptr,
             "0x-prefixed hex row must still promote");
    const auto &octalStats = parser.ghidraMapLoadStats();
    t.Equals(octalStats.loaded, 2, "octal test load keeps both rows");
    t.Equals(octalStats.skippedMalformed, 0, "no malformed rows in the octal test");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D gap 8 — row-count bound. 262144 non-executable filler rows
// (the cap) plus one valid trailing row. The loader must stop at the
// cap, so the trailing row is never read and never promotes.
static void runGateDRowCapTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-rowcap", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-rowcap", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    {
        std::ofstream mapFile(mapPath);
        t.IsTrue(static_cast<bool>(mapFile), "ghidra map file should be writable");
        if (!mapFile)
        {
            cleanupGhidraOracleFiles(elfPath, mapPath);
            return;
        }
        mapFile << "Name,Start,End,Size\n";
        for (int i = 0; i < 262144; ++i)
        {
            mapFile << "FILL,0x00200000,0x00200010,16\n";
        }
        mapFile << "TAIL,0x00100044,0x001000B4,76\n";
        mapFile.close();
        t.IsTrue(static_cast<bool>(mapFile), "ghidra map file should be writable");
    }

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }
    t.IsFalse(parser.loadGhidraFunctionMap(mapPath.string()),
              "all-cap filler rows skipped and tail unread means no map loads");

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x00100044u) == nullptr,
             "trailing row past the row cap must NOT promote");
    t.IsTrue(findFunctionByStart(functions, 0x00100000u) != nullptr,
             "discovery entry must survive a capped map load");
    const auto &rowCapStats = parser.ghidraMapLoadStats();
    t.Equals(rowCapStats.loaded, 0, "capped load keeps no rows");
    t.Equals(rowCapStats.skippedNonExecutable, 262144, "all cap filler rows counted non-executable");
    t.IsTrue(rowCapStats.rowLimitExceeded, "row cap exceeded flag set");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D gap 8 (line-length bound). A row whose line exceeds the
// 4096-character cap is malformed, however valid its addresses are.
static void runGateDLineLengthTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-linelen", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-linelen", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    {
        std::ofstream mapFile(mapPath);
        t.IsTrue(static_cast<bool>(mapFile), "ghidra map file should be writable");
        if (!mapFile)
        {
            cleanupGhidraOracleFiles(elfPath, mapPath);
            return;
        }
        mapFile << "Name,Start,End,Size\n";
        mapFile << std::string(5000, 'X') << ",0x00100044,0x001000B4,76\n";
        mapFile << "VALID,0x001000B4,0x001000DC,40\n";
        mapFile.close();
        t.IsTrue(static_cast<bool>(mapFile), "ghidra map file should be writable");
    }

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x00100044u) == nullptr,
             "over-long line must NOT become a function");
    t.IsTrue(findFunctionByStart(functions, 0x001000B4u) != nullptr,
             "valid row after the over-long line must promote");
    const auto &lineLengthStats = parser.ghidraMapLoadStats();
    t.Equals(lineLengthStats.loaded, 1, "line-length load keeps one row");
    t.Equals(lineLengthStats.skippedMalformed, 1, "exactly one over-long line counted malformed");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D independence — a fully rejected CSV must not poison the
// function set: load returns false and only discovery survives. The
// inverted-end row is the sharp edge: the Phase 19 clamp repaired it
// to [start, start+4) and promoted it; the unaligned-end row likewise
// promoted. Both must now be rejected.
static void runGateDRejectedCsvTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-rejected", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-rejected", ".csv");

    t.IsTrue(writeGhidraOracleElf(elfPath), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath,
                                  "INV_END,0x00100044,0x00100040,0\n"
                                  "UNAL_END,0x001000B4,0x001000DB,0\n"
                                  "MISAL,0x00100002,0x00100010,0\n"
                                  "NONEXEC,0x00100200,0x00100210,0\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }
    t.IsFalse(parser.loadGhidraFunctionMap(mapPath.string()),
              "a CSV with zero acceptable rows must not load");

    const auto functions = parser.extractFunctions();
    t.IsTrue(findFunctionByStart(functions, 0x00100000u) != nullptr,
             "discovery entry must survive a rejected map");
    t.IsTrue(findFunctionByStart(functions, 0x00100028u) != nullptr,
             "discovery jal target must survive a rejected map");
    t.IsTrue(findFunctionByStart(functions, 0x00100044u) == nullptr,
             "repaired inverted-end row must NOT poison the set");
    t.IsTrue(findFunctionByStart(functions, 0x001000B4u) == nullptr,
             "unaligned-end row must NOT poison the set");
    t.IsTrue(findFunctionByStart(functions, 0x00100002u) == nullptr,
             "misaligned row must NOT poison the set");
    t.IsTrue(findFunctionByStart(functions, 0x00100200u) == nullptr,
             "non-executable row must NOT poison the set");
    const auto &rejectedStats = parser.ghidraMapLoadStats();
    t.Equals(rejectedStats.loaded, 0, "rejected load keeps no rows");
    t.Equals(rejectedStats.skippedInvalidRange, 1, "exactly one invalid-range row counted");
    t.Equals(rejectedStats.skippedUnalignedEnd, 1, "exactly one unaligned-end row counted");
    t.Equals(rejectedStats.skippedMisaligned, 1, "exactly one misaligned row counted");
    t.Equals(rejectedStats.skippedNonExecutable, 1, "exactly one non-executable row counted");

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

// Gate D independence — a bogus map row must never override a
// verified ELF symbol (regression guard for merge precedence: the
// loader changes must not disturb symbols > map). The Phase 12 ELF
// carries sized symbol merged_fn [0x20, 0x34); the map row claims
// the same start with a shorter end.
static void runGateDSymbolPrecedenceTest(TestCase &t)
{
    const auto elfPath = ghidraOracleTempPath("ps2recomp-gated-symbol", ".elf");
    const auto mapPath = ghidraOracleTempPath("ps2recomp-gated-symbol", ".csv");

    t.IsTrue(writePhase12DiscoveryElf(elfPath, false, true), "temporary ELF should be generated");
    t.IsTrue(writeGhidraOracleCsv(mapPath, "BOGUS,0x00100020,0x00100028,8\n"),
             "ghidra map file should be writable");

    ElfParser parser(elfPath.string());
    if (!loadGhidraOracleElf(parser, elfPath, t) || !loadGhidraOracleMap(parser, mapPath, t))
    {
        cleanupGhidraOracleFiles(elfPath, mapPath);
        return;
    }

    const auto functions = parser.extractFunctions();
    const Function *fn = findFunctionByStart(functions, 0x00100020u);
    t.IsTrue(fn != nullptr, "symbol address must yield a function");
    if (fn != nullptr)
    {
        t.Equals(fn->name, std::string("merged_fn"), "verified symbol name must win over map row");
        t.Equals(fn->end, 0x00100034u, "verified symbol end must win over map row end");
    }

    cleanupGhidraOracleFiles(elfPath, mapPath);
}

static void registerGateDTests(TestCase &tc)
{
    tc.Run("ghidra map rejects a row with an unaligned end",
           [](TestCase &t) { runGateDUnalignedEndTest(t); });
    tc.Run("ghidra map rejects overlapping rows first-wins",
           [](TestCase &t) { runGateDOverlapTest(t); });
    tc.Run("ghidra map rejects rows spanning undecodable words, keeps valid R5900 rows",
           [](TestCase &t) { runGateDUndecodableTest(t); });
    tc.Run("ghidra map rejects a row starting in a branch delay slot",
           [](TestCase &t) { runGateDDelaySlotTest(t); });
    tc.Run("ghidra map counts malformed rows with a warning",
           [](TestCase &t) { runGateDMalformedTest(t); });
    tc.Run("ghidra map treats a headerless first line as data",
           [](TestCase &t) { runGateDHeaderTest(t); });
    tc.Run("ghidra map parses leading-zero numbers as decimal, not octal",
           [](TestCase &t) { runGateDOctalTest(t); });
    tc.Run("ghidra map stops at the row cap and never reads past it",
           [](TestCase &t) { runGateDRowCapTest(t); });
    tc.Run("ghidra map rejects over-long lines as malformed",
           [](TestCase &t) { runGateDLineLengthTest(t); });
    tc.Run("ghidra map fully rejected CSV leaves discovery intact",
           [](TestCase &t) { runGateDRejectedCsvTest(t); });
    tc.Run("ghidra map row never overrides a verified symbol",
           [](TestCase &t) { runGateDSymbolPrecedenceTest(t); });
}

// Gate C (S3) — SLTIU sign-extension synthetic ELF (no .data section).
// .text at 0x00100000 (20 words). The entry materializes $s0, applies
// SLTIU (or SLTI) against an immediate, scales the 0/1 result by 64,
// adds it to 0x100000, and JALRs through the result:
//   entry: <materialize $s0>; sltiu/slti $t0,$s0,imm; sll $t0,$t0,6;
//          lui $t1,0x10; addu $t1,$t1,$t0; jalr $t1; nop; jr $ra; nop
//   target [0x100040): addiu $v0, 7; jr $ra; nop — discovered ONLY if
//          the comparison yields 1.
// MIPS/R5900 SLTIU sign-extends the immediate FIRST, then compares
// unsigned: with $s0 = 0x10000 and imm = -1, the correct result is 1
// (0x10000 < 0xFFFFFFFF) while a zero-extended comparison gives 0
// (0x10000 < 0xFFFF). The buggy code therefore resolves the JALR to
// the entry itself (rejected as the current function start) and the
// target drops out of discovery; the fixed code discovers it.
enum class GateCSltiuVariant
{
    NegOne,
    Neg32768,
    Positive,
    RsZero,
    RsAllOnes,
    Slti,
    LoadVoid,
    MmiVoid,
};

static bool writeGateCSltiuElf(const std::filesystem::path &elfPath, GateCSltiuVariant variant)
{
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x00100000u);

    ELFIO::section *text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4);
    text->set_address(0x00100000u);

    // Word 0-1: materialize $s0 (two slots so every variant lines up).
    uint32_t word0 = 0x3C100001u; // lui $s0, 0x1 -> 0x10000
    uint32_t word1 = 0x00000000u; // nop
    if (variant == GateCSltiuVariant::RsZero)
    {
        word0 = 0x34100000u; // ori $s0, $zero, 0 -> 0
    }
    else if (variant == GateCSltiuVariant::RsAllOnes)
    {
        word0 = 0x3C10FFFFu; // lui $s0, 0xFFFF
        word1 = 0x3610FFFFu; // ori $s0, $s0, 0xFFFF -> 0xFFFFFFFF
    }
    else if (variant == GateCSltiuVariant::LoadVoid)
    {
        word1 = 0x8C100000u; // lw $s0, 0($zero): voids the $s0 proof
    }
    else if (variant == GateCSltiuVariant::MmiVoid)
    {
        word1 = 0x70000000u; // MMI opcode: unmodelled, voids all proofs
    }

    // Word 2: the comparison under test.
    uint32_t compare = 0x2E08FFFFu; // sltiu $t0, $s0, -1
    if (variant == GateCSltiuVariant::Neg32768)
    {
        compare = 0x2E088000u; // sltiu $t0, $s0, -32768
    }
    else if (variant == GateCSltiuVariant::Positive)
    {
        compare = 0x2E087FFFu; // sltiu $t0, $s0, 0x7FFF
    }
    else if (variant == GateCSltiuVariant::Slti)
    {
        compare = 0x2A08FFFFu; // slti $t0, $s0, -1 (signed: 0, unlike SLTIU)
    }

    const std::array<uint32_t, 20> textWords = {
        word0,
        word1,
        compare,
        0x00084180u, // sll $t0, $t0, 6
        0x3C090010u, // lui $t1, 0x10
        0x01284821u, // addu $t1, $t1, $t0
        0x0120F809u, // jalr $t1
        0x00000000u, // nop (delay)
        0x03E00008u, // jr $ra
        0x00000000u, // nop
        0x00000000u, // pad
        0x00000000u, // pad
        0x00000000u, // pad
        0x00000000u, // pad
        0x00000000u, // pad
        0x00000000u, // pad
        0x24020007u, // target (0x100040): addiu $v0, zero, 7
        0x03E00008u, // jr $ra
        0x00000000u, // nop
        0x00000000u, // pad
    };
    text->set_data(reinterpret_cast<const char *>(textWords.data()),
                   static_cast<ELFIO::Elf_Word>(textWords.size() * sizeof(uint32_t)));

    ELFIO::segment *textSegment = writer.segments.add();
    textSegment->set_type(ELFIO::PT_LOAD);
    textSegment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    textSegment->set_align(0x1000);
    textSegment->add_section_index(text->get_index(), text->get_addr_align());

    return writer.save(elfPath.string());
}

static bool gateCSltiuStarts(const std::filesystem::path &elfPath, GateCSltiuVariant variant,
                   TestCase &t, std::set<uint32_t> &starts)
{
    const bool writeOk = writeGateCSltiuElf(elfPath, variant);
    t.IsTrue(writeOk, "temporary SLTIU ELF should be generated");
    if (!writeOk)
    {
        return false;
    }
    ElfParser parser(elfPath.string());
    const bool parseOk = parser.parse();
    t.IsTrue(parseOk, "generated SLTIU ELF should parse");
    if (!parseOk)
    {
        std::error_code removeError;
        std::filesystem::remove(elfPath, removeError);
        return false;
    }
    const auto discovery = parser.discoverFromEntryPoint();
    for (const auto &fn : discovery.functions)
    {
        starts.insert(fn.start);
    }
    std::error_code removeError;
    std::filesystem::remove(elfPath, removeError);
    return true;
}

static std::filesystem::path gateCSltiuPath()
{
    const auto uniqueSuffix = std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    return std::filesystem::temp_directory_path() /
           ("ps2recomp-gatec-sltiu-" + uniqueSuffix + ".elf");
}

void register_ps2_recompiler_tests()
{
    MiniTest::Case("PS2Recompiler", [](TestCase &tc)
                   {
        tc.Run("game helpers are not classified as runtime stubs", [](TestCase &t) {
            t.IsFalse(ps2_runtime_calls::isStubName("Pad_init"),
                      "Pad_init should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("Pad_set"),
                      "Pad_set should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("pdInitPeripheral"),
                      "pdInitPeripheral should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("pdGetPeripheral"),
                      "pdGetPeripheral should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("InitThread"),
                      "InitThread should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("syFree"),
                      "syFree should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("syMallocInit"),
                      "syMallocInit should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("syHwInit"),
                      "syHwInit should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("syHwInit2"),
                      "syHwInit2 should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("syRtcInit"),
                      "syRtcInit should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("sdDrvInit"),
                      "sdDrvInit should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("sdSndStopAll"),
                      "sdSndStopAll should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("sdSysFinish"),
                      "sdSysFinish should be recompiled as game code");
            t.IsFalse(ps2_runtime_calls::isStubName("iopGetArea"),
                      "iopGetArea should be recompiled as game code");
            t.IsTrue(ps2_runtime_calls::isStubName("builtin_set_imask"),
                     "builtin_set_imask should remain a runtime helper");
            t.IsTrue(ps2_runtime_calls::isStubName("getpid"),
                     "getpid should remain a runtime helper");
            t.IsTrue(ps2_runtime_calls::isStubName("scePadRead"),
                     "scePadRead should remain a runtime pad stub");
        });

        tc.Run("additional entries split at nearest discovered boundary", [](TestCase &t) {
            std::vector<Section> sections = {
                {".text", 0x1000u, 0x3000u, 0u, true, false, false, true, nullptr}
            };

            std::vector<Function> functions = {
                makeFunction("container", 0x1000u, 0x1018u),
                makeFunction("caller", 0x2000u, 0x2010u)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x1000u] = {
                makeNopLike(0x1000u),
                makeNopLike(0x1004u),
                makeNopLike(0x1008u),
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u)
            };
            decodedFunctions[0x2000u] = {
                makeAbsJump(0x2000u, 0x1008u, OPCODE_JAL),
                makeNopLike(0x2004u),
                makeAbsJump(0x2008u, 0x100Cu, OPCODE_J),
                makeNopLike(0x200Cu)
            };

            size_t discovered = PS2Recompiler::DiscoverAdditionalEntryPoints(
                functions, decodedFunctions, sections);

            t.Equals(discovered, static_cast<size_t>(3),
                     "expected two mid-function targets plus the JAL return entry to be discovered");

            auto findByStart = [&](uint32_t start) -> const Function* {
                auto it = std::find_if(functions.begin(), functions.end(),
                                       [&](const Function &fn) { return fn.start == start; });
                if (it == functions.end())
                {
                    return nullptr;
                }
                return &(*it);
            };

            const Function *entry1008 = findByStart(0x1008u);
            const Function *entry100C = findByStart(0x100Cu);
            const Function *entry2008 = findByStart(0x2008u);
            t.IsNotNull(entry1008, "entry at 0x1008 should exist");
            t.IsNotNull(entry100C, "entry at 0x100C should exist");
            t.IsNotNull(entry2008, "JAL return address entry at 0x2008 should exist");
            if (entry1008 && entry100C)
            {
                t.Equals(entry1008->end, 0x100Cu,
                         "entry 0x1008 should end at nearest discovered start 0x100C");
                t.Equals(entry100C->end, 0x1018u,
                         "entry 0x100C should end at containing function end");
            }
            if (entry2008)
            {
                t.Equals(entry2008->end, 0x2010u,
                         "return entry 0x2008 should slice through the caller tail");
            }

            auto decoded1008It = decodedFunctions.find(0x1008u);
            auto decoded100CIt = decodedFunctions.find(0x100Cu);
            auto decoded2008It = decodedFunctions.find(0x2008u);
            t.IsTrue(decoded1008It != decodedFunctions.end(), "decoded slice for 0x1008 should exist");
            t.IsTrue(decoded100CIt != decodedFunctions.end(), "decoded slice for 0x100C should exist");
            t.IsTrue(decoded2008It != decodedFunctions.end(), "decoded slice for 0x2008 should exist");
            if (decoded1008It != decodedFunctions.end())
            {
                t.Equals(decoded1008It->second.size(), static_cast<size_t>(1),
                         "entry 0x1008 slice should stop before 0x100C");
                if (!decoded1008It->second.empty())
                {
                    t.Equals(decoded1008It->second.front().address, 0x1008u,
                             "entry 0x1008 slice should begin at 0x1008");
                }
            }
            if (decoded100CIt != decodedFunctions.end() && !decoded100CIt->second.empty())
            {
                t.Equals(decoded100CIt->second.front().address, 0x100Cu,
                         "entry 0x100C slice should begin at 0x100C");
            }
            if (decoded2008It != decodedFunctions.end())
            {
                t.Equals(decoded2008It->second.size(), static_cast<size_t>(2),
                         "return entry 0x2008 slice should keep the jump and its delay slot");
                if (!decoded2008It->second.empty())
                {
                    t.Equals(decoded2008It->second.front().address, 0x2008u,
                             "return entry 0x2008 slice should begin at the JAL fallthrough");
                }
            }
        });

        tc.Run("entry reslice trims earlier entries after late discovery", [](TestCase &t) {
            std::vector<Function> functions = {
                makeFunction("container", 0x1000u, 0x1018u),
                makeFunction("entry_1008", 0x1008u, 0x1018u),
                makeFunction("entry_100c", 0x100Cu, 0x1018u)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x1000u] = {
                makeNopLike(0x1000u),
                makeNopLike(0x1004u),
                makeNopLike(0x1008u),
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u)
            };
            decodedFunctions[0x1008u] = {
                makeNopLike(0x1008u),
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u)
            };
            decodedFunctions[0x100Cu] = {
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u)
            };

            size_t resliced = PS2Recompiler::ResliceEntryFunctions(functions, decodedFunctions);
            t.Equals(resliced, static_cast<size_t>(1),
                     "expected only the earlier entry to be resliced");

            auto findByStart = [&](uint32_t start) -> const Function* {
                auto it = std::find_if(functions.begin(), functions.end(),
                                       [&](const Function &fn) { return fn.start == start; });
                if (it == functions.end())
                {
                    return nullptr;
                }
                return &(*it);
            };

            const Function *entry1008 = findByStart(0x1008u);
            const Function *entry100C = findByStart(0x100Cu);
            t.IsNotNull(entry1008, "entry at 0x1008 should exist");
            t.IsNotNull(entry100C, "entry at 0x100C should exist");
            if (entry1008)
            {
                t.Equals(entry1008->end, 0x100Cu,
                         "entry 0x1008 should be trimmed to next entry start");
            }
            if (entry100C)
            {
                t.Equals(entry100C->end, 0x1018u,
                         "entry 0x100C should still end at containing end");
            }

            auto decoded1008It = decodedFunctions.find(0x1008u);
            auto decoded100CIt = decodedFunctions.find(0x100Cu);
            t.IsTrue(decoded1008It != decodedFunctions.end(), "decoded slice for 0x1008 should exist");
            t.IsTrue(decoded100CIt != decodedFunctions.end(), "decoded slice for 0x100C should exist");
            if (decoded1008It != decodedFunctions.end())
            {
                t.Equals(decoded1008It->second.size(), static_cast<size_t>(1),
                         "entry 0x1008 slice should stop before 0x100C");
                if (!decoded1008It->second.empty())
                {
                    t.Equals(decoded1008It->second.front().address, 0x1008u,
                             "entry 0x1008 slice should begin at 0x1008");
                }
            }
            if (decoded100CIt != decodedFunctions.end())
            {
                t.Equals(decoded100CIt->second.size(), static_cast<size_t>(3),
                         "entry 0x100C slice should keep remaining instructions");
            }
        });

        tc.Run("same-function JAL return addresses get entry wrappers but targets stay labels", [](TestCase &t) {
            std::vector<Section> sections = {
                {".text", 0x1000u, 0x40u, 0u, true, false, false, true, nullptr}
            };

            std::vector<Function> functions = {
                makeFunction("container", 0x1000u, 0x101Cu)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x1000u] = {
                makeAbsJump(0x1000u, 0x100Cu, OPCODE_JAL),
                makeNopLike(0x1004u),
                makeAbsJump(0x1008u, 0x1014u, OPCODE_J),
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u),
                makeJrRa(0x1018u)
            };

            size_t discovered = PS2Recompiler::DiscoverAdditionalEntryPoints(
                functions, decodedFunctions, sections);

            t.Equals(discovered, static_cast<size_t>(1),
                     "same-function JAL should create only the resume entry while plain J stays internal");

            const bool hasResumeEntry = std::any_of(
                functions.begin(), functions.end(),
                [](const Function &fn) { return fn.start == 0x1008u; });
            const bool hasCallEntry = std::any_of(
                functions.begin(), functions.end(),
                [](const Function &fn) { return fn.start == 0x100Cu; });
            const bool hasJumpEntry = std::any_of(
                functions.begin(), functions.end(),
                [](const Function &fn) { return fn.start == 0x1014u && fn.name.rfind("entry_", 0) == 0; });

            t.IsTrue(hasResumeEntry, "same-function JAL return address should be promoted to a resumable entry");
            t.IsFalse(hasCallEntry, "same-function JAL target should remain an internal label");
            t.IsFalse(hasJumpEntry, "same-function J target should remain an internal label only");
        });

        tc.Run("JAL return addresses get resumable entry wrappers", [](TestCase &t) {
            std::vector<Section> sections = {
                {".text", 0x1000u, 0x2000u, 0u, true, false, false, true, nullptr}
            };

            std::vector<Function> functions = {
                makeFunction("caller", 0x1000u, 0x1018u),
                makeFunction("callee", 0x2000u, 0x2008u)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x1000u] = {
                makeAbsJump(0x1000u, 0x2000u, OPCODE_JAL),
                makeNopLike(0x1004u),
                makeNopLike(0x1008u),
                makeNopLike(0x100Cu),
                makeJrRa(0x1010u),
                makeNopLike(0x1014u)
            };
            decodedFunctions[0x2000u] = {
                makeJrRa(0x2000u),
                makeNopLike(0x2004u)
            };

            size_t discovered = PS2Recompiler::DiscoverAdditionalEntryPoints(
                functions, decodedFunctions, sections);
            t.Equals(discovered, static_cast<size_t>(1),
                     "external JAL should create one resumable entry at the caller return address");

            auto entryIt = std::find_if(functions.begin(), functions.end(),
                                        [](const Function &fn) { return fn.start == 0x1008u; });
            t.IsTrue(entryIt != functions.end(), "return address 0x1008 should be promoted to an entry wrapper");
            if (entryIt != functions.end())
            {
                t.Equals(entryIt->end, 0x1018u,
                         "return-address entry should slice through the remainder of the caller");
            }

            auto decodedEntryIt = decodedFunctions.find(0x1008u);
            t.IsTrue(decodedEntryIt != decodedFunctions.end(),
                     "decoded entry slice for the caller return address should exist");
            if (decodedEntryIt != decodedFunctions.end())
            {
                t.Equals(decodedEntryIt->second.size(), static_cast<size_t>(4),
                         "return-address entry slice should keep the caller tail");
                if (!decodedEntryIt->second.empty())
                {
                    t.Equals(decodedEntryIt->second.front().address, 0x1008u,
                             "return-address entry slice should begin at the JAL fallthrough");
                }
            }
        });

        tc.Run("JAL to an already-known function still discovers the return entry", [](TestCase &t) {
            std::vector<Section> sections = {
                {".text", 0x1000u, 0x2000u, 0u, true, false, false, true, nullptr}
            };

            std::vector<Function> functions = {
                makeFunction("caller", 0x1000u, 0x1020u),
                makeFunction("callee", 0x1100u, 0x1108u)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x1000u] = {
                makeNopLike(0x1000u),
                makeNopLike(0x1004u),
                makeAbsJump(0x1008u, 0x1100u, OPCODE_JAL),
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u),
                makeJrRa(0x1018u),
                makeNopLike(0x101Cu)
            };
            decodedFunctions[0x1100u] = {
                makeJrRa(0x1100u),
                makeNopLike(0x1104u)
            };

            size_t discovered = PS2Recompiler::DiscoverAdditionalEntryPoints(
                functions, decodedFunctions, sections);
            t.Equals(discovered, static_cast<size_t>(1),
                     "return entry should still be discovered even when the JAL target is already registered");

            auto entryIt = std::find_if(functions.begin(), functions.end(),
                                        [](const Function &fn) { return fn.start == 0x1010u; });
            t.IsTrue(entryIt != functions.end(),
                     "return address 0x1010 should be emitted as a resumable entry");
            if (entryIt != functions.end())
            {
                t.Equals(entryIt->end, 0x1020u,
                         "return entry should cover the remaining caller tail");
            }
        });

        tc.Run("discovery ignores synthetic entry wrappers", [](TestCase &t) {
            std::vector<Section> sections = {
                {".text", 0x1000u, 0x2000u, 0u, true, false, false, true, nullptr}
            };

            std::vector<Function> functions = {
                makeFunction("entry_1008", 0x1008u, 0x1020u),
                makeFunction("callee", 0x1100u, 0x1108u)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x1008u] = {
                makeAbsJump(0x1008u, 0x1100u, OPCODE_JAL),
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u),
                makeJrRa(0x1018u),
                makeNopLike(0x101Cu)
            };
            decodedFunctions[0x1100u] = {
                makeJrRa(0x1100u),
                makeNopLike(0x1104u)
            };

            size_t discovered = PS2Recompiler::DiscoverAdditionalEntryPoints(
                functions, decodedFunctions, sections);
            t.Equals(discovered, static_cast<size_t>(0),
                     "synthetic entry wrappers should not recursively produce more entries");

            const bool hasRecursiveResumeEntry = std::any_of(
                functions.begin(), functions.end(),
                [](const Function &fn) { return fn.start == 0x1010u; });
            t.IsFalse(hasRecursiveResumeEntry,
                      "discovery should not promote a return entry out of an existing entry wrapper");
        });

        tc.Run("entry reslice handles entries without containing function", [](TestCase &t) {
            std::vector<Function> functions = {
                makeFunction("entry_1008", 0x1008u, 0x1018u),
                makeFunction("entry_100c", 0x100Cu, 0x1018u)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x1008u] = {
                makeNopLike(0x1008u),
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u)
            };
            decodedFunctions[0x100Cu] = {
                makeNopLike(0x100Cu),
                makeNopLike(0x1010u),
                makeNopLike(0x1014u)
            };

            size_t resliced = PS2Recompiler::ResliceEntryFunctions(functions, decodedFunctions);
            t.Equals(resliced, static_cast<size_t>(1),
                     "expected only the earlier entry to be resliced");

            auto findByStart = [&](uint32_t start) -> const Function* {
                auto it = std::find_if(functions.begin(), functions.end(),
                                       [&](const Function &fn) { return fn.start == start; });
                if (it == functions.end())
                {
                    return nullptr;
                }
                return &(*it);
            };

            const Function *entry1008 = findByStart(0x1008u);
            const Function *entry100C = findByStart(0x100Cu);
            t.IsNotNull(entry1008, "entry at 0x1008 should exist");
            t.IsNotNull(entry100C, "entry at 0x100C should exist");
            if (entry1008)
            {
                t.Equals(entry1008->end, 0x100Cu,
                         "entry 0x1008 should be trimmed to next entry start");
            }
            if (entry100C)
            {
                t.Equals(entry100C->end, 0x1018u,
                         "entry 0x100C should keep original end");
            }

            auto decoded1008It = decodedFunctions.find(0x1008u);
            auto decoded100CIt = decodedFunctions.find(0x100Cu);
            t.IsTrue(decoded1008It != decodedFunctions.end(), "decoded slice for 0x1008 should exist");
            t.IsTrue(decoded100CIt != decodedFunctions.end(), "decoded slice for 0x100C should exist");
            if (decoded1008It != decodedFunctions.end())
            {
                t.Equals(decoded1008It->second.size(), static_cast<size_t>(1),
                         "entry 0x1008 slice should stop before 0x100C");
            }
            if (decoded100CIt != decodedFunctions.end())
            {
                t.Equals(decoded100CIt->second.size(), static_cast<size_t>(3),
                         "entry 0x100C slice should keep remaining instructions");
            }
        });

        tc.Run("non-executable section targets are ignored", [](TestCase &t) {
            std::vector<Section> sections = {
                {".text", 0x1000u, 0x2000u, 0u, true, false, false, true, nullptr},
                {".data", 0x3000u, 0x1000u, 0u, false, true, false, false, nullptr}
            };

            std::vector<Function> functions = {
                makeFunction("data_container", 0x3000u, 0x3010u),
                makeFunction("caller", 0x1800u, 0x1810u)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x3000u] = {
                makeNopLike(0x3000u),
                makeNopLike(0x3004u),
                makeNopLike(0x3008u),
                makeNopLike(0x300Cu)
            };
            decodedFunctions[0x1800u] = {
                makeAbsJump(0x1800u, 0x3004u, OPCODE_J),
                makeNopLike(0x1804u)
            };

            size_t discovered = PS2Recompiler::DiscoverAdditionalEntryPoints(
                functions, decodedFunctions, sections);
            t.Equals(discovered, static_cast<size_t>(0),
                     "non-executable targets should not produce additional entries");

            const bool hasDataEntry = std::any_of(functions.begin(), functions.end(),
                                                  [](const Function &fn) { return fn.start == 0x3004u; });
            t.IsFalse(hasDataEntry, "target in data section must not produce entry wrapper");
        });

        tc.Run("entry starting at jr ra is capped to return thunk", [](TestCase &t) {
            std::vector<Section> sections = {
                {".text", 0x1000u, 0x2000u, 0u, true, false, false, true, nullptr}
            };

            std::vector<Function> functions = {
                makeFunction("container", 0x1000u, 0x1200u),
                makeFunction("caller", 0x1300u, 0x1310u)
            };

            std::unordered_map<uint32_t, std::vector<Instruction>> decodedFunctions;
            decodedFunctions[0x1000u] = {
                makeNopLike(0x1000u),
                makeNopLike(0x1004u),
                makeNopLike(0x1008u),
                makeJrRa(0x10A0u),
                makeNopLike(0x10A4u),
                makeNopLike(0x10A8u),
                makeNopLike(0x10ACu)
            };
            decodedFunctions[0x1300u] = {
                makeAbsJump(0x1300u, 0x10A0u, OPCODE_J),
                makeNopLike(0x1304u)
            };

            size_t discovered = PS2Recompiler::DiscoverAdditionalEntryPoints(
                functions, decodedFunctions, sections);
            t.Equals(discovered, static_cast<size_t>(1),
                     "expected one additional entry from cross-function jump");

            auto entryIt = std::find_if(functions.begin(), functions.end(),
                                        [](const Function &fn) { return fn.start == 0x10A0u; });
            t.IsTrue(entryIt != functions.end(), "entry wrapper at 0x10A0 should exist");
            if (entryIt != functions.end())
            {
                t.Equals(entryIt->end, 0x10A8u,
                         "jr ra entry should end after delay slot, not at container end");
            }

            auto decodedEntryIt = decodedFunctions.find(0x10A0u);
            t.IsTrue(decodedEntryIt != decodedFunctions.end(),
                     "decoded entry slice for 0x10A0 should exist");
            if (decodedEntryIt != decodedFunctions.end())
            {
                t.Equals(decodedEntryIt->second.size(), static_cast<size_t>(2),
                         "jr ra entry slice should contain exactly jr+delay");
                if (!decodedEntryIt->second.empty())
                {
                    t.Equals(decodedEntryIt->second.front().address, 0x10A0u,
                             "entry slice should start at 0x10A0");
                }
            }
        });

        tc.Run("config manager parses jump_tables table entries", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path configPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-jump-table-" + uniqueSuffix + ".toml");

            std::ofstream configFile(configPath);
            t.IsTrue(static_cast<bool>(configFile), "temp config file should be writable");
            if (!configFile)
            {
                return;
            }

            configFile << "[general]\n";
            configFile << "input = \"dummy.elf\"\n";
            configFile << "output = \"out\"\n\n";
            configFile << "[jump_tables]\n";
            configFile << "[[jump_tables.table]]\n";
            configFile << "address = \"0x200000\"\n";
            configFile << "base_register = 9\n";
            configFile << "entries = [\n";
            configFile << "  { index = 0, target = \"0x1620\" },\n";
            configFile << "  { index = 1, target = \"0x1630\" },\n";
            configFile << "]\n";
            configFile.close();

            ConfigManager manager(configPath.string());
            RecompilerConfig config = manager.loadConfig();

            t.Equals(config.jumpTables.size(), static_cast<size_t>(1),
                     "one configured jump table should be loaded");
            if (!config.jumpTables.empty())
            {
                const JumpTable &table = config.jumpTables.front();
                t.Equals(table.address, 0x200000u, "table address should parse from hex string");
                t.Equals(table.baseRegister, 9u, "base register should parse");
                t.Equals(table.entries.size(), static_cast<size_t>(2),
                         "two jump table entries should parse");
                if (table.entries.size() >= 2)
                {
                    t.Equals(table.entries[0].index, 0u, "first entry index should parse");
                    t.Equals(table.entries[0].target, 0x1620u, "first entry target should parse");
                    t.Equals(table.entries[1].index, 1u, "second entry index should parse");
                    t.Equals(table.entries[1].target, 0x1630u, "second entry target should parse");
                }
            }

            std::error_code removeError;
            std::filesystem::remove(configPath, removeError);
        });

        tc.Run("config manager loads modern and legacy guest entry hints", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path configPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-entry-hints-" + uniqueSuffix + ".toml");

            std::ofstream configFile(configPath);
            t.IsTrue(static_cast<bool>(configFile), "temp config file should be writable");
            if (!configFile)
            {
                return;
            }

            configFile << "[general]\n";
            configFile << "input = \"dummy.elf\"\n";
            configFile << "output = \"out\"\n";
            configFile << "entry_points = [\"callback@0x7008\"]\n";
            configFile << "untracked_stubs = [\"legacy_callback@0x7018\"]\n";
            configFile.close();

            ConfigManager manager(configPath.string());
            const RecompilerConfig config = manager.loadConfig();

            t.Equals(config.entryPointHints.size(), static_cast<size_t>(2),
                     "modern and legacy entry metadata should be merged");
            t.IsTrue(std::find(config.entryPointHints.begin(), config.entryPointHints.end(),
                               "callback@0x7008") != config.entryPointHints.end(),
                     "modern entry_points metadata should load");
            t.IsTrue(std::find(config.entryPointHints.begin(), config.entryPointHints.end(),
                               "legacy_callback@0x7018") != config.entryPointHints.end(),
                     "legacy untracked_stubs metadata should remain compatible");

            std::error_code removeError;
            std::filesystem::remove(configPath, removeError);
        });

        tc.Run("configured entry hint synthesizes an omitted standalone function", [](TestCase &t) {
            const std::string uniqueSuffix =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            const std::filesystem::path tempRoot =
                std::filesystem::temp_directory_path() / ("ps2recomp-entry-synthesis-" + uniqueSuffix);
            const std::filesystem::path elfPath = tempRoot / "entry-hint.elf";
            const std::filesystem::path configPath = tempRoot / "entry-hint.toml";
            const std::filesystem::path outputPath = tempRoot / "output";
            std::filesystem::create_directories(tempRoot);

            const bool elfWritten = writeMinimalMipsElfWithUnmappedEntryHint(elfPath);
            const bool configWritten = writeRecompilerTestConfig(
                configPath,
                elfPath,
                outputPath,
                {},
                {"InitAlarm@0x00100008"},
                {"omitted_callback@0x00100010"});
            t.IsTrue(elfWritten && configWritten,
                     "standalone entry regression inputs should be generated");

            if (elfWritten && configWritten)
            {
                PS2Recompiler recompiler(configPath.string());
                t.IsTrue(recompiler.initialize(),
                         "standalone entry regression config should initialize");
                t.IsTrue(recompiler.recompile(),
                         "configured executable entry should be decoded even without a symbol");
                recompiler.generateOutput();

                const std::filesystem::path registrationPath = outputPath / "register_functions.cpp";
                std::ifstream registrationFile(registrationPath);
                const std::string registration{
                    std::istreambuf_iterator<char>(registrationFile),
                    std::istreambuf_iterator<char>()};
                t.IsTrue(registration.find("// 0x100010") != std::string::npos,
                         "synthesized entry address should be registered for guest dispatch");
                t.IsTrue(recompiler.reportCounters().additionalEntryPoints >= 1u,
                         "synthesized entry should be visible in the report");
            }

            std::error_code removeError;
            std::filesystem::remove_all(tempRoot, removeError);
        });

        tc.Run("standalone entry_ blocks register resume points for their JAL return addresses", [](TestCase &t) {
            const std::string uniqueSuffix =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            const std::filesystem::path tempRoot =
                std::filesystem::temp_directory_path() / ("ps2recomp-standalone-entry-" + uniqueSuffix);
            const std::filesystem::path elfPath = tempRoot / "standalone-entry.elf";
            const std::filesystem::path configPath = tempRoot / "standalone-entry.toml";
            const std::filesystem::path outputPath = tempRoot / "output";
            std::filesystem::create_directories(tempRoot);

            const bool elfWritten = writeMinimalMipsElfWithStandaloneEntryBlock(elfPath);
            const bool configWritten = writeRecompilerTestConfig(configPath, elfPath, outputPath, {});
            t.IsTrue(elfWritten && configWritten,
                     "standalone entry block regression inputs should be generated");

            if (elfWritten && configWritten)
            {
                PS2Recompiler recompiler(configPath.string());
                t.IsTrue(recompiler.initialize(),
                         "standalone entry block config should initialize");
                t.IsTrue(recompiler.recompile(),
                         "standalone entry block should recompile");
                recompiler.generateOutput();

                // entry_00100000 is not nested inside any other function, so a thread switched out inside its
                // JAL must be able to resume at the return address (0x100008) through the registered owner.
                const std::filesystem::path registrationPath = outputPath / "register_functions.cpp";
                std::ifstream registrationFile(registrationPath);
                const std::string registration{
                    std::istreambuf_iterator<char>(registrationFile),
                    std::istreambuf_iterator<char>()};
                t.IsTrue(registration.find("// 0x100008") != std::string::npos,
                         "JAL return address inside a standalone entry_ block should be registered as a resume point");
            }

            std::error_code removeError;
            std::filesystem::remove_all(tempRoot, removeError);
        });

        tc.Run("a delay slot past the function's end is still translated", [](TestCase &t) {
            const std::string uniqueSuffix =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            const std::filesystem::path tempRoot =
                std::filesystem::temp_directory_path() / ("ps2recomp-delay-slot-end-" + uniqueSuffix);
            const std::filesystem::path elfPath = tempRoot / "delay-slot-end.elf";
            const std::filesystem::path configPath = tempRoot / "delay-slot-end.toml";
            const std::filesystem::path outputPath = tempRoot / "output";
            std::filesystem::create_directories(tempRoot);

            const bool elfWritten = writeMinimalMipsElfWithDelaySlotPastFunctionEnd(elfPath);
            const bool configWritten = writeRecompilerTestConfig(configPath, elfPath, outputPath, {});
            t.IsTrue(elfWritten && configWritten, "delay slot regression inputs should be generated");

            if (elfWritten && configWritten)
            {
                PS2Recompiler recompiler(configPath.string());
                t.IsTrue(recompiler.initialize(), "delay slot config should initialize");
                t.IsTrue(recompiler.recompile(), "delay slot ELF should recompile");
                recompiler.generateOutput();

                std::string leaf;
                for (const auto &entry : std::filesystem::directory_iterator(outputPath))
                {
                    if (entry.path().filename().string().rfind("short_leaf", 0) != 0)
                        continue;
                    std::ifstream file(entry.path());
                    leaf.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                }
                t.IsTrue(!leaf.empty(), "short_leaf should be emitted");
                t.IsTrue(leaf.find("READ32(ADD32(GPR_U32(ctx, 4), 100))") != std::string::npos,
                         "the lw in jr $ra's delay slot must be translated, not replaced by a NOP");
            }

            std::error_code removeError;
            std::filesystem::remove_all(tempRoot, removeError);
        });

        tc.Run("elf parser skips relocation sections whose data did not load (ps2x_fuzz_elf regression)", [](TestCase &t) {
            // tools/fuzz/regressions/elf/null_reloc_data.bin, found by the CI fuzz smoke job: a REL section whose
            // sh_size runs past the end of the file. ELFIO leaves its data null but keeps sh_size, and its relocation
            // accessor dereferenced the null data. The parser must skip the section instead of crashing.
            static const unsigned char kInput[] = {
                0x7F, 0x45, 0x4C, 0x46, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x02, 0xCF, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x34, 0x00, 0x00, 0x00,
                0x78, 0x00, 0x00, 0x00, 0x01, 0x40, 0x92, 0x20, 0x34, 0x00, 0x20, 0x00, 0x01, 0x00, 0x28, 0x00,
                0x03, 0x00, 0x00, 0x00, 0x2E, 0x34, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x54, 0x00, 0x27,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
                0x00, 0x05, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xF0, 0xFF, 0xBD, 0x27, 0x08, 0x00, 0xE0,
                0x03, 0x10, 0x00, 0xBD, 0x27, 0x00, 0x00, 0x5B, 0x00, 0x00, 0x2E, 0x74, 0x65, 0x78, 0x74, 0x00,
                0x2E, 0x73, 0x68, 0x73, 0x74, 0x72, 0x74, 0xE1, 0x62, 0x00, 0x00, 0x00, 0x0E, 0x0E, 0x0E, 0x0E,
                0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                0x54, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCB, 0xE6,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x9D,
            };
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-nullreloc-" + uniqueSuffix + ".elf");
            {
                std::ofstream out(elfPath, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(kInput), sizeof(kInput));
            }
            bool threw = false;
            try
            {
                ElfParser parser(elfPath.string());
                (void)parser.parse();
            }
            catch (const std::exception &)
            {
                threw = true; // a reported rejection is acceptable; a crash is not
            }
            t.IsTrue(true, threw ? "parser rejected the file" : "parser returned without crashing");
            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("elf parser ignores STT_FUNC symbols in non-executable sections", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-parser-" + uniqueSuffix + ".elf");

            const bool writeOk = writeMinimalMipsElfWithCodeAndDataFunctionSymbols(elfPath);
            t.IsTrue(writeOk, "temporary ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            const auto functions = parser.extractFunctions();
            const bool hasCodeFunction = std::any_of(functions.begin(), functions.end(),
                                                     [](const Function &fn)
                                                     { return fn.start == 0x00100000u; });
            const bool hasDataFunction = std::any_of(functions.begin(), functions.end(),
                                                     [](const Function &fn)
                                                     { return fn.start == 0x00200000u; });

            t.IsTrue(hasCodeFunction, "function in executable section should be retained");
            t.IsFalse(hasDataFunction, "STT_FUNC symbol in .data must be ignored");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("elf parser keeps VU microprograms out of EE code discovery", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-vutext-" + uniqueSuffix + ".elf");

            const bool writeOk = writeMinimalMipsElfWithVuMicroprogramSection(elfPath);
            t.IsTrue(writeOk, "temporary ELF with .vutext should be generated");
            if (!writeOk)
                return;

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "ELF with .vutext should parse");
            if (parseOk)
            {
                const auto sections = parser.getSections();
                const auto vuSection = std::find_if(sections.begin(), sections.end(),
                                                    [](const Section &section)
                                                    { return section.name == ".vutext"; });
                t.IsTrue(vuSection != sections.end(), ".vutext bytes should remain available");
                if (vuSection != sections.end())
                    t.IsFalse(vuSection->isCode, ".vutext must not be decoded as R5900 code");

                const auto functions = parser.extractFunctions();
                const bool hasVuFunction = std::any_of(functions.begin(), functions.end(),
                                                       [](const Function &function)
                                                       { return function.start == 0x00250000u; });
                t.IsFalse(hasVuFunction, "VU symbol must not become an EE function");
            }

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("ghidra map replaces JAL fallback-only auto starts", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-ghidra-merge-" + uniqueSuffix + ".elf");
            const std::filesystem::path mapPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-ghidra-merge-" + uniqueSuffix + ".csv");

            const bool writeOk = writeMinimalMipsElfWithJalFallbackTarget(elfPath);
            t.IsTrue(writeOk, "temporary ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            const auto fallbackExtras = parser.extractExtraFunctions();
            const bool hasFallbackStart = std::any_of(
                fallbackExtras.begin(), fallbackExtras.end(),
                [](const Function &fn)
                { return fn.start == 0x00100010u; });
            t.IsTrue(hasFallbackStart, "JAL fallback should discover secondary start before map load");

            std::ofstream mapFile(mapPath);
            t.IsTrue(static_cast<bool>(mapFile), "ghidra map file should be writable");
            if (!mapFile)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }
            mapFile << "name,start,end,size\n";
            mapFile << "FUN_00100000,0x00100000,0x00100010,0x10\n";
            mapFile.close();

            const bool mapLoaded = parser.loadGhidraFunctionMap(mapPath.string());
            t.IsTrue(mapLoaded, "ghidra map should load");

            const auto functions = parser.extractFunctions();
            const auto entryIt = std::find_if(
                functions.begin(), functions.end(),
                [](const Function &fn)
                { return fn.start == 0x00100000u; });
            t.IsTrue(entryIt != functions.end(), "ghidra entry should exist");
            if (entryIt != functions.end())
            {
                t.Equals(entryIt->name, std::string("FUN_00100000"),
                         "ghidra name should win over fallback auto-name");
            }

            const bool stillHasFallbackOnlyStart = std::any_of(
                functions.begin(), functions.end(),
                [](const Function &fn)
                { return fn.start == 0x00100010u; });
            t.IsFalse(stillHasFallbackOnlyStart,
                      "fallback-only function starts should be removed once ghidra map is loaded");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
            std::filesystem::remove(mapPath, removeError);
        });

        registerGhidraOracleTests(tc);
        registerGateDTests(tc);


        tc.Run("Phase 12 discovery finds exactly the reachable set and switch table", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-phase12-base-" + uniqueSuffix + ".elf");

            const bool writeOk = writePhase12DiscoveryElf(elfPath, false, false);
            t.IsTrue(writeOk, "temporary stripped ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            // The discovery pass itself: proven reachable set, nothing more.
            const auto discovery = parser.discoverFromEntryPoint();
            std::set<uint32_t> starts;
            for (const auto &fn : discovery.functions)
            {
                starts.insert(fn.start);
            }
            t.Equals(starts.size(), static_cast<size_t>(4), "discovery should find exactly 4 reachable functions");
            for (const uint32_t expected :
                 {0x00100000u, 0x00100020u, 0x00100028u, 0x00100034u})
            {
                t.IsTrue(starts.count(expected) == 1, "reachable function should be discovered");
            }
            t.IsFalse(starts.count(0x00100088u) == 1,
                      "dead_caller is unreachable and must not be discovered");
            t.IsFalse(starts.count(0x001000A0u) == 1,
                      "dead_func is called only by dead code and must not be discovered");

            t.Equals(discovery.jumpTables.size(), static_cast<size_t>(1),
                     "exactly one switch table should be recovered");
            if (discovery.jumpTables.size() == 1)
            {
                const auto &table = discovery.jumpTables[0];
                t.Equals(table.address, 0x00100200u, "switch table address should be the .data table");
                t.Equals(table.entries.size(), static_cast<size_t>(4), "switch table should have 4 entries");
                const std::array<uint32_t, 4> cases = {0x00100058u, 0x00100064u, 0x00100070u, 0x0010007Cu};
                for (size_t i = 0; i < 4 && i < table.entries.size(); ++i)
                {
                    t.Equals(table.entries[i].target, cases[i], "switch table entry should match its case");
                }
            }

            // The merged output still carries the fallback-scan extras, so
            // it is a SUPERSET of discovery: the case targets appear as
            // functions (the scan proposes materialized code addresses from
            // the .data table words) and dead_func appears (its JAL word
            // sits in dead code, which the scan cannot distinguish), while
            // dead_caller appears in neither (nothing takes its address).
            // Discovery, by contrast, treats the cases as intra-function
            // blocks of the switch function — the 4-function set above.
            // Ends are inferred from the next merged start / section end,
            // so the merged switch end is case0, not the switch's own
            // decoded extent.
            const auto functions = parser.extractFunctions();
            const auto findFn = [&functions](uint32_t start) {
                return std::find_if(functions.begin(), functions.end(),
                                    [start](const Function &fn) { return fn.start == start; });
            };
            const auto entryIt = findFn(0x00100000u);
            t.IsTrue(entryIt != functions.end(), "merged entry should exist");
            if (entryIt != functions.end())
            {
                t.Equals(entryIt->end, 0x00100020u, "merged entry end should be f1 start");
            }
            const auto f1It = findFn(0x00100020u);
            t.IsTrue(f1It != functions.end(), "merged f1 should exist");
            if (f1It != functions.end())
            {
                t.Equals(f1It->end, 0x00100028u, "merged f1 end should be f2 start");
            }
            const auto f2It = findFn(0x00100028u);
            t.IsTrue(f2It != functions.end(), "merged f2 should exist");
            if (f2It != functions.end())
            {
                t.Equals(f2It->end, 0x00100034u, "merged f2 end should be switch start");
            }
            const auto switchIt = findFn(0x00100034u);
            t.IsTrue(switchIt != functions.end(), "merged switch should exist");
            if (switchIt != functions.end())
            {
                t.Equals(switchIt->end, 0x00100058u,
                         "merged switch end should be the next merged start (case0, via the scan)");
            }
            t.IsTrue(findFn(0x00100058u) != functions.end(),
                     "merged output contains case0 via the fallback scan (not via discovery)");
            t.IsTrue(findFn(0x001000A0u) != functions.end(),
                     "merged output still contains dead_func via the fallback scan");
            t.IsTrue(findFn(0x00100088u) == functions.end(),
                     "dead_caller appears in neither discovery nor merged output");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("Phase 12 merge order keeps sized symbol ranges over discovery", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-phase12-merge-" + uniqueSuffix + ".elf");

            const bool writeOk = writePhase12DiscoveryElf(elfPath, false, true);
            t.IsTrue(writeOk, "temporary ELF with sized symbol should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            // Discovery itself is symbol-blind: it still finds f1 and f2
            // from code alone. The merge, not discovery, enforces priority.
            const auto discovery = parser.discoverFromEntryPoint();
            std::set<uint32_t> starts;
            for (const auto &fn : discovery.functions)
            {
                starts.insert(fn.start);
            }
            t.IsTrue(starts.count(0x00100020u) == 1, "discovery still finds f1 from code");
            t.IsTrue(starts.count(0x00100028u) == 1, "discovery still finds f2 from code");

            const auto functions = parser.extractFunctions();
            const auto findFn = [&functions](uint32_t start) {
                return std::find_if(functions.begin(), functions.end(),
                                    [start](const Function &fn) { return fn.start == start; });
            };
            const auto mergedIt = findFn(0x00100020u);
            t.IsTrue(mergedIt != functions.end(), "symbol range should survive the merge");
            if (mergedIt != functions.end())
            {
                t.Equals(mergedIt->name, std::string("merged_fn"), "symbol name should win over auto name");
                t.Equals(mergedIt->end, 0x00100034u,
                         "symbol end should be exactly start + size, not extended by discovery");
            }
            t.IsTrue(findFn(0x00100028u) == functions.end(),
                     "f2 must not appear as a separate function inside the symbol range");
            const auto entryIt = findFn(0x00100000u);
            t.IsTrue(entryIt != functions.end(), "merged entry should exist");
            if (entryIt != functions.end())
            {
                t.Equals(entryIt->end, 0x00100020u, "merged entry end should be the symbol start");
            }
            t.IsTrue(findFn(0x00100034u) != functions.end(), "switch should survive the merge");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("Phase 12 call-edge deletion removes only the orphaned callee", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-phase12-mutant-" + uniqueSuffix + ".elf");

            // Entry's `jal f2` replaced by nop; dead_caller still contains
            // `jal f2`, so a scan-based pass would still report f2.
            const bool writeOk = writePhase12DiscoveryElf(elfPath, true, false);
            t.IsTrue(writeOk, "temporary mutant ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            const auto discovery = parser.discoverFromEntryPoint();
            std::set<uint32_t> starts;
            for (const auto &fn : discovery.functions)
            {
                starts.insert(fn.start);
            }
            t.Equals(starts.size(), static_cast<size_t>(3),
                     "deleting the only live call edge should leave exactly 3 functions");
            t.IsTrue(starts.count(0x00100000u) == 1, "entry should remain discovered");
            t.IsTrue(starts.count(0x00100020u) == 1, "f1 should remain discovered");
            t.IsTrue(starts.count(0x00100034u) == 1, "switch should remain discovered");
            t.IsFalse(starts.count(0x00100028u) == 1,
                      "f2 must drop out of discovery once its only live caller is cut");
            t.Equals(discovery.jumpTables.size(), static_cast<size_t>(1),
                     "the switch table should still be recovered in the mutant");

            // Contrast: the merged output DOES still contain f2, because
            // the fallback scan sees dead_caller's `jal f2` word. This is
            // what makes the test non-vacuous — it measures descent, not
            // scanning.
            const auto functions = parser.extractFunctions();
            t.IsTrue(std::any_of(functions.begin(), functions.end(),
                                 [](const Function &fn) { return fn.start == 0x00100028u; }),
                     "merged output still contains f2 via the fallback scan");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("Phase 12 discovered tables resolve JR switches without fallback promotion", [](TestCase &t) {
            // Decode the switch function's own words (indices 13..33 of the
            // Phase 12 text) with the real decoder, so the CFA sees exactly
            // what the recompiler would feed it.
            R5900Decoder decoder;
            std::vector<Instruction> instructions;
            for (size_t i = 13; i < 34; ++i)
            {
                instructions.push_back(
                    decoder.decodeInstruction(0x00100000u + static_cast<uint32_t>(i * 4), kPhase12TextWords[i]));
            }
            t.Equals(instructions.size(), static_cast<size_t>(21), "switch function should decode to 21 instructions");

            Section textSec{};
            textSec.name = ".text";
            textSec.address = 0x00100000u;
            textSec.size = 0xA8u;
            textSec.isCode = true;
            Section dataSec{};
            dataSec.name = ".data";
            dataSec.address = 0x00100200u;
            dataSec.size = 16u;
            dataSec.isData = true;
            const std::vector<Section> sections{textSec, dataSec};

            Function switchFn{};
            switchFn.name = "switch_fn";
            switchFn.start = 0x00100034u;
            switchFn.end = 0x00100088u;
            switchFn.isRecompiled = true;
            const std::vector<Function> allFunctions{switchFn};

            const std::unordered_map<uint32_t, std::vector<uint32_t>> discoveredMap{
                {0x00100200u, {0x00100058u, 0x00100064u, 0x00100070u, 0x0010007Cu}}};
            ControlFlowAnalyzer withTable(sections, discoveredMap, nullptr);
            const auto result = withTable.analyze(switchFn, instructions, &allFunctions);
            const auto jrIt = result.jumpTableTargets.find(0x00100050u);
            t.IsTrue(jrIt != result.jumpTableTargets.end(),
                     "JR should resolve through the discovered table");
            if (jrIt != result.jumpTableTargets.end())
            {
                t.Equals(jrIt->second.size(), static_cast<size_t>(4),
                         "JR should resolve to all 4 case targets");
                for (const uint32_t target : {0x00100058u, 0x00100064u, 0x00100070u, 0x0010007Cu})
                {
                    t.IsTrue(std::find(jrIt->second.begin(), jrIt->second.end(), target) != jrIt->second.end(),
                             "each case target should be a proven CFG edge");
                }
            }
            t.IsTrue(result.indirectFallbackEntryPoints.empty(),
                     "a resolved JR must not trigger indirect-fallback promotion");

            // Negative control: the same function with NO discovered table
            // must fall back (the switch bound is an ANDI mask, not SLTIU,
            // so unconfigured recovery cannot save it).
            const std::unordered_map<uint32_t, std::vector<uint32_t>> emptyMap;
            ControlFlowAnalyzer withoutTable(sections, emptyMap, nullptr);
            const auto fallbackResult = withoutTable.analyze(switchFn, instructions, &allFunctions);
            t.IsFalse(fallbackResult.indirectFallbackEntryPoints.empty(),
                      "without the discovered table the JR must fall back to promotion");
            t.IsTrue(fallbackResult.jumpTableTargets.find(0x00100050u) ==
                         fallbackResult.jumpTableTargets.end(),
                     "without the discovered table the JR should have no proven targets");

            std::error_code removeError;
            (void)removeError;
        });

        tc.Run("Phase 12 CFA resolves tables whose cases are separate functions", [](TestCase &t) {
            // The real-pipeline shape: the fallback scan turns each case
            // into its own function, so the merged switch range ends at
            // case0 and the CFA sees only the 8-word prologue. Every case
            // target is then "external" — but each is a known function
            // entry, so the JR must resolve without fallback promotion.
            R5900Decoder decoder;
            std::vector<Instruction> instructions;
            for (size_t i = 13; i < 21; ++i)
            {
                instructions.push_back(
                    decoder.decodeInstruction(0x00100000u + static_cast<uint32_t>(i * 4), kPhase12TextWords[i]));
            }
            t.Equals(instructions.size(), static_cast<size_t>(8), "prologue should decode to 8 instructions");

            Section textSec{};
            textSec.name = ".text";
            textSec.address = 0x00100000u;
            textSec.size = 0xA8u;
            textSec.isCode = true;
            Section dataSec{};
            dataSec.name = ".data";
            dataSec.address = 0x00100200u;
            dataSec.size = 16u;
            dataSec.isData = true;
            const std::vector<Section> sections{textSec, dataSec};

            Function switchFn{};
            switchFn.name = "switch_fn";
            switchFn.start = 0x00100034u;
            switchFn.end = 0x00100058u; // ends at case0, as the scan-split merged output does
            switchFn.isRecompiled = true;
            std::vector<Function> allFunctions{switchFn};
            for (size_t c = 0; c < 4; ++c)
            {
                Function caseFn{};
                caseFn.name = "case_fn";
                caseFn.start = 0x00100058u + static_cast<uint32_t>(c * 12);
                caseFn.end = caseFn.start + 12u;
                caseFn.isRecompiled = true;
                allFunctions.push_back(caseFn);
            }

            const std::unordered_map<uint32_t, std::vector<uint32_t>> discoveredMap{
                {0x00100200u, {0x00100058u, 0x00100064u, 0x00100070u, 0x0010007Cu}}};
            ControlFlowAnalyzer withTable(sections, discoveredMap, nullptr);
            const auto result = withTable.analyze(switchFn, instructions, &allFunctions);
            t.IsTrue(result.indirectFallbackEntryPoints.empty(),
                     "all-external but known case targets must not trigger fallback promotion");

            // Guard: a table with a target contained in NO known function
            // must still fall back — the accounting rule must not blindly
            // suppress promotion for bogus tables.
            const std::unordered_map<uint32_t, std::vector<uint32_t>> bogusMap{
                {0x00100200u, {0x00100058u, 0x00100064u, 0x00100070u, 0x00200000u}}};
            ControlFlowAnalyzer withBogus(sections, bogusMap, nullptr);
            const auto bogusResult = withBogus.analyze(switchFn, instructions, &allFunctions);
            t.IsFalse(bogusResult.indirectFallbackEntryPoints.empty(),
                      "a table with an unaccounted target must still fall back");
        });

        tc.Run("Phase 14 JALR constant targets are proven discovery edges", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-phase14-jalr-" + uniqueSuffix + ".elf");

            const bool writeOk = writePhase14JalrElf(elfPath, false, false);
            t.IsTrue(writeOk, "temporary JALR ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            // Discovery is the proven set: both constant JALR targets and
            // the function reached only through the constant JR edge.
            const auto discovery = parser.discoverFromEntryPoint();
            std::set<uint32_t> starts;
            for (const auto &fn : discovery.functions)
            {
                starts.insert(fn.start);
            }
            t.Equals(starts.size(), static_cast<size_t>(4),
                     "discovery should find exactly entry, funcA, funcB, funcD");
            for (const uint32_t expected :
                 {0x00100000u, 0x00100040u, 0x00100060u, 0x001000C0u})
            {
                t.IsTrue(starts.count(expected) == 1, "JALR/JR-constant reachable function should be discovered");
            }
            t.IsFalse(starts.count(0x001000A0u) == 1,
                      "jrBlock is an entry block reached by a constant jump, not a function start");
            t.IsTrue(discovery.jumpTables.empty(), "no switch table should be recovered from this ELF");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("Phase 14 JALR mutants drop only the affected callee", [](TestCase &t) {
            for (const auto &[name, cutAddiu, clobber] :
                 {std::tuple{"cut-addiu", true, false}, std::tuple{"clobber-delay", false, true}})
            {
                const auto uniqueSuffix = std::to_string(static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
                const std::filesystem::path elfPath = std::filesystem::temp_directory_path() /
                                                      ("ps2recomp-phase14-jalr-mutant-" + uniqueSuffix + ".elf");

                const bool writeOk = writePhase14JalrElf(elfPath, cutAddiu, clobber);
                t.IsTrue(writeOk, "temporary JALR mutant ELF should be generated");
                if (!writeOk)
                {
                    continue;
                }

                ElfParser parser(elfPath.string());
                const bool parseOk = parser.parse();
                t.IsTrue(parseOk, "generated mutant ELF should parse");
                if (!parseOk)
                {
                    std::error_code removeError;
                    std::filesystem::remove(elfPath, removeError);
                    continue;
                }

                const auto discovery = parser.discoverFromEntryPoint();
                std::set<uint32_t> starts;
                for (const auto &fn : discovery.functions)
                {
                    starts.insert(fn.start);
                }
                t.Equals(starts.size(), static_cast<size_t>(3),
                         "mutant discovery should find exactly entry, funcB, funcD");
                t.IsFalse(starts.count(0x00100040u) == 1,
                          "funcA must drop out of discovery once its JALR edge is cut or clobbered");
                t.IsTrue(starts.count(0x00100060u) == 1, "funcB should remain discovered in the mutant");
                t.IsTrue(starts.count(0x001000C0u) == 1, "funcD should remain discovered in the mutant");

                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                (void)name;
            }
        });

        tc.Run("GateC SLTIU negative immediate proves the unsigned comparison", [](TestCase &t) {
            // Discriminating cases: old code compares against the
            // zero-extended field (0xFFFF / 0x8000) and resolves 0, so the
            // JALR targets the entry itself and the target is lost. Fixed
            // code sign-extends first (0xFFFFFFFF / 0x80000000) and resolves
            // 1, so the target is discovered.
            for (const auto variant : {GateCSltiuVariant::NegOne, GateCSltiuVariant::Neg32768})
            {
                std::set<uint32_t> starts;
                if (!gateCSltiuStarts(gateCSltiuPath(), variant, t, starts))
                {
                    continue;
                }
                t.IsTrue(starts.count(0x00100000u) == 1, "entry should be discovered");
                t.IsTrue(starts.count(0x00100040u) == 1,
                         "SLTIU target must be discovered when the sign-extended comparison is 1");
            }
        });

        tc.Run("GateC SLTIU agreement controls and SLTI divergence", [](TestCase &t) {
            // Positive immediate: both old and new code resolve 0.
            {
                std::set<uint32_t> starts;
                if (gateCSltiuStarts(gateCSltiuPath(), GateCSltiuVariant::Positive, t, starts))
                {
                    t.IsFalse(starts.count(0x00100040u) == 1,
                              "positive-immediate SLTIU must resolve 0 in both old and new code");
                }
            }
            // rs = 0 with imm = -1: 0 < 0xFFFFFFFF and 0 < 0xFFFF are both 1.
            {
                std::set<uint32_t> starts;
                if (gateCSltiuStarts(gateCSltiuPath(), GateCSltiuVariant::RsZero, t, starts))
                {
                    t.IsTrue(starts.count(0x00100040u) == 1,
                             "rs=0 SLTIU against -1 must resolve 1 in both old and new code");
                }
            }
            // rs = 0xFFFFFFFF with imm = -1: both comparisons give 0.
            {
                std::set<uint32_t> starts;
                if (gateCSltiuStarts(gateCSltiuPath(), GateCSltiuVariant::RsAllOnes, t, starts))
                {
                    t.IsFalse(starts.count(0x00100040u) == 1,
                              "rs=0xFFFFFFFF SLTIU against -1 must resolve 0 in both old and new code");
                }
            }
            // SLTI on the same operands as the NegOne case: signed
            // 0x10000 < -1 is 0, while SLTIU gives 1. The two opcodes must
            // differ exactly where the semantics say they must.
            {
                std::set<uint32_t> starts;
                if (gateCSltiuStarts(gateCSltiuPath(), GateCSltiuVariant::Slti, t, starts))
                {
                    t.IsFalse(starts.count(0x00100040u) == 1,
                              "SLTI with a negative immediate must resolve 0 where SLTIU resolves 1");
                }
            }
        });

        tc.Run("GateC SLTIU proof invalidated by load and unmodelled opcode", [](TestCase &t) {
            // A load into $s0 voids its constant, and an unmodelled MMI
            // opcode voids every proof: no JALR target may be proven from
            // either block, before or after the fix.
            for (const auto variant : {GateCSltiuVariant::LoadVoid, GateCSltiuVariant::MmiVoid})
            {
                std::set<uint32_t> starts;
                if (!gateCSltiuStarts(gateCSltiuPath(), variant, t, starts))
                {
                    continue;
                }
                t.IsFalse(starts.count(0x00100040u) == 1,
                          "a voided constant must never yield a proven JALR target");
            }
        });

        tc.Run("GateC S14 ERET emitter restores from ErrorEPC or EPC", [](TestCase &t)
        {
            // ERET lives in the emitter (cop0_translator), not the runtime:
            // ERL set -> pc = ErrorEPC and clear ERL; otherwise pc = EPC and
            // clear EXL; the LL bit is cleared and the block stops. Neither
            // ERL nor EXL set is undefined on hardware; the emitter uses EPC,
            // which is the common behavior.
            std::vector<Symbol> symbols;
            std::vector<Section> sections;
            CodeGenerator generator(symbols, sections);
            Cop0Translator translator(generator);
            R5900Decoder decoder;
            const uint32_t raw = (OPCODE_COP0 << 26) | (COP0_CO << 21) | COP0_CO_ERET;
            const Instruction inst = decoder.decodeInstruction(0x1000u, raw);
            const std::string emitted = translator.translate(inst);
            t.IsTrue(emitted.find("cop0_errorepc") != std::string::npos,
                     "ERET must restore pc from ErrorEPC when ERL is set");
            t.IsTrue(emitted.find("cop0_epc") != std::string::npos,
                     "ERET must restore pc from EPC when ERL is clear");
            t.IsTrue(emitted.find("~0x4") != std::string::npos, "ERET must clear ERL");
            t.IsTrue(emitted.find("~0x2") != std::string::npos, "ERET must clear EXL");
            t.IsTrue(emitted.find("clearLLBit") != std::string::npos, "ERET must clear the LL bit");
            t.IsTrue(emitted.find("return;") != std::string::npos, "ERET must stop the block");
        });

        tc.Run("Phase 14 singleton data pointer discovers its function, decoy excluded", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-phase14-data-" + uniqueSuffix + ".elf");

            const bool writeOk = writePhase14DataElf(elfPath, false);
            t.IsTrue(writeOk, "temporary data-pointer ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            const auto discovery = parser.discoverFromEntryPoint();
            std::set<uint32_t> starts;
            for (const auto &fn : discovery.functions)
            {
                starts.insert(fn.start);
            }
            t.Equals(starts.size(), static_cast<size_t>(4),
                     "discovery should find exactly entry, plainFunc, dataptrFunc, dataptrCallee");
            for (const uint32_t expected :
                 {0x00100000u, 0x00100020u, 0x00100040u, 0x00100060u})
            {
                t.IsTrue(starts.count(expected) == 1, "data-pointer reachable function should be discovered");
            }
            t.IsFalse(starts.count(0x00100024u) == 1,
                      "decoy pointer to plainFunc's jr $ra is mid-function and must not be discovered");

            // The merged output must carry dataptrFunc through the
            // discovery merge: the fallback scan's clustering rule misses
            // a singleton pointer with no conservative neighbour.
            const auto functions = parser.extractFunctions();
            t.IsTrue(std::any_of(functions.begin(), functions.end(),
                                 [](const Function &fn) { return fn.start == 0x00100040u; }),
                     "merged output should contain dataptrFunc via discovery");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("Phase 14 data-pointer deletion removes the function and its callee", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-phase14-data-mutant-" + uniqueSuffix + ".elf");

            const bool writeOk = writePhase14DataElf(elfPath, true);
            t.IsTrue(writeOk, "temporary data-pointer mutant ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            const auto discovery = parser.discoverFromEntryPoint();
            std::set<uint32_t> starts;
            for (const auto &fn : discovery.functions)
            {
                starts.insert(fn.start);
            }
            t.Equals(starts.size(), static_cast<size_t>(2),
                     "deleting the data pointer should leave exactly entry and plainFunc");
            t.IsFalse(starts.count(0x00100040u) == 1,
                      "dataptrFunc must drop out of discovery once its data pointer is zeroed");
            t.IsFalse(starts.count(0x00100060u) == 1,
                      "dataptrCallee must drop out too: its only caller was dataptrFunc");

            // Contrast: the merged output DOES still contain dataptrCallee,
            // because the fallback scan sees dataptrFunc's `jal` word in
            // dead code. This is what makes the test non-vacuous — it
            // measures descent from the data pointer, not scanning.
            const auto functions = parser.extractFunctions();
            t.IsTrue(std::any_of(functions.begin(), functions.end(),
                                 [](const Function &fn) { return fn.start == 0x00100060u; }),
                     "merged output still contains dataptrCallee via the fallback scan");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("elf parser discovers address-taken callbacks in stripped ELFs", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-address-taken-" + uniqueSuffix + ".elf");

            const bool writeOk = writeMinimalMipsElfWithAddressTakenCallbacks(elfPath);
            t.IsTrue(writeOk, "temporary stripped ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF should parse");
            if (!parseOk)
            {
                std::error_code removeError;
                std::filesystem::remove(elfPath, removeError);
                return;
            }

            const auto functions = parser.extractFunctions();
            auto hasStart = [&functions](uint32_t start)
            {
                return std::any_of(functions.begin(), functions.end(),
                                   [start](const Function &function)
                                   { return function.start == start; });
            };

            t.IsTrue(hasStart(0x00100040u),
                     "LUI plus delay-slot ADDIU should discover the callback entry");
            t.IsTrue(hasStart(0x00100060u),
                     "clustered rodata pointers should discover the first leaf callback");
            t.IsTrue(hasStart(0x00100068u),
                     "clustered rodata pointers should discover the second leaf callback");
            t.IsTrue(hasStart(0x00100080u),
                     "clustered pointers should discover an initializer with a delayed stack prologue");
            t.IsTrue(hasStart(0x001000E0u),
                     "a nearby registrar call should discover a longer leaf callback");
            t.IsTrue(hasStart(0x00100120u),
                     "a clustered descriptor should discover a long leaf method");
            t.IsTrue(hasStart(0x00100280u),
                     "a callback should flow through a saved register into a call argument");
            t.IsTrue(hasStart(0x001002C0u),
                     "a delay-slot LUI should flow into the taken branch successor");
            t.IsTrue(hasStart(0x00100340u),
                     "a materialized method stored into a runtime descriptor should be discovered");
            t.IsTrue(hasStart(0x00100380u),
                     "an alternating pointer/id table should discover a neighboring long leaf");
            t.IsTrue(hasStart(0x001003C8u),
                     "an adjacent two-instruction leaf thunk should be split from a known thunk");
            t.IsTrue(hasStart(0x001003E0u),
                     "a callback passed as the fifth register argument should be discovered");
            t.IsTrue(hasStart(0x001004F0u),
                     "a code address whose LUI sits in a JAL delay slot of a callee-saved register should be discovered");
            t.IsTrue(!hasStart(0x0010000Cu),
                     "a delay slot must never become a function start, whatever proposes it");
            t.IsTrue(hasStart(0x00100400u),
                     "a long callback passed as the fifth register argument should be discovered");
            t.IsFalse(hasStart(0x00100548u),
                      "a post-return prologue without a cross-reference must remain only a hint");
            t.IsTrue(hasStart(0x00100580u),
                     "a singleton data pointer loaded and consumed by JALR should be discovered");
            t.IsFalse(hasStart(0x00100588u),
                      "a data pointer must not split a function at a control-transfer delay slot");
            t.IsFalse(hasStart(0x00100300u),
                      "an isolated data pointer or non-callback code materialization must not become a function");

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("elf parser supplements partial DWARF with address-taken callbacks", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-partial-dwarf-" + uniqueSuffix + ".elf");

            const bool writeOk = writeMinimalMipsElfWithAddressTakenCallbacks(elfPath, true);
            t.IsTrue(writeOk, "temporary ELF with partial DWARF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            const bool parseOk = parser.parse();
            t.IsTrue(parseOk, "generated ELF with partial DWARF should parse");
            if (parseOk)
            {
                const auto functions = parser.extractFunctions();
                const auto callbackIt = std::find_if(
                    functions.begin(), functions.end(),
                    [](const Function &function)
                    { return function.start == 0x00100040u; });
                t.IsTrue(callbackIt != functions.end(),
                         "partial DWARF must not suppress static callback discovery");

                const auto lastFallbackIt = std::find_if(
                    functions.begin(), functions.end(),
                    [](const Function &function)
                    { return function.start == 0x001003E0u; });
                t.IsTrue(lastFallbackIt != functions.end(),
                         "the last inferred callback should still be discovered");
                if (lastFallbackIt != functions.end())
                {
                    t.Equals(0x001003F0u, lastFallbackIt->end,
                             "later partial DWARF should bound an inferred callback");
                }
            }

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("runtime call resolution includes Veronica compatibility aliases", [](TestCase &t) {
            t.Equals(ps2_runtime_calls::resolveSyscallName("ReleaseAlarm"), std::string_view{"ReleaseAlarm"},
                     "ReleaseAlarm should resolve as a syscall name");
            t.Equals(ps2_runtime_calls::resolveSyscallName("_ReleaseAlarm"), std::string_view{"ReleaseAlarm"},
                     "underscore ReleaseAlarm alias should resolve to ReleaseAlarm");
            t.Equals(ps2_runtime_calls::resolveSyscallName("EnableCache"), std::string_view{"EnableCache"},
                     "EnableCache should resolve as a syscall name");
            t.Equals(ps2_runtime_calls::resolveSyscallName("DisableCache"), std::string_view{"DisableCache"},
                     "DisableCache should resolve as a syscall name");
            t.Equals(ps2_runtime_calls::resolveStubName("isceSifSetDma"), std::string_view{"isceSifSetDma"},
                     "isceSifSetDma should resolve as a stub name");
            t.Equals(ps2_runtime_calls::resolveStubName("isceSifSetDChain"), std::string_view{"isceSifSetDChain"},
                     "isceSifSetDChain should resolve as a stub name");
            t.Equals(ps2_runtime_calls::resolveStubName("memalign"), std::string_view{"memalign"},
                     "memalign should resolve as a stub name");
            t.Equals(ps2_runtime_calls::resolveStubName("_memalign_r"), std::string_view{"memalign_r"},
                     "_memalign_r should resolve to the memalign_r stub");
            t.Equals(ps2_runtime_calls::resolveStubName("_realloc_r"), std::string_view{"realloc_r"},
                     "_realloc_r should resolve to the realloc_r stub");
            t.Equals(ps2_runtime_calls::resolveStubName("malloc_extend_top"), std::string_view{"malloc_extend_top"},
                     "malloc_extend_top should resolve as an allocator compatibility stub");
            t.Equals(ps2_runtime_calls::resolveStubName("__malloc_lock"), std::string_view{"__malloc_lock"},
                     "__malloc_lock should resolve as an allocator compatibility stub");
            t.Equals(ps2_runtime_calls::resolveStubName("__malloc_unlock"), std::string_view{"__malloc_unlock"},
                     "__malloc_unlock should resolve as an allocator compatibility stub");
            t.Equals(ps2_runtime_calls::resolveStubName("memclr"), std::string_view{"memclr"},
                     "memclr should resolve as a runtime stub");
            t.Equals(ps2_runtime_calls::resolveStubName("__divdi3"), std::string_view{"__divdi3"},
                     "__divdi3 should resolve as a runtime stub");
            t.Equals(ps2_runtime_calls::resolveStubName("__mcmp"), std::string_view{},
                     "__mcmp should be left for recompilation");
            t.Equals(ps2_runtime_calls::resolveStubName("__sprint"), std::string_view{},
                     "__sprint should be left for recompilation");
            t.Equals(ps2_runtime_calls::resolveStubName("__sprint_r"), std::string_view{},
                     "__sprint_r should be left for recompilation");
            t.Equals(ps2_runtime_calls::resolveStubName("__sbprintf"), std::string_view{},
                     "__sbprintf should be left for recompilation");
        });

        tc.Run("initializer skips fall back to guest recompilation", [](TestCase &t) {
            const std::string uniqueSuffix =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            const std::filesystem::path tempRoot =
                std::filesystem::temp_directory_path() / ("ps2recomp-initializer-" + uniqueSuffix);
            const std::filesystem::path elfPath = tempRoot / "initializer.elf";
            const std::filesystem::path configPath = tempRoot / "initializer.toml";
            const std::filesystem::path outputPath = tempRoot / "output";
            std::filesystem::create_directories(tempRoot);

            const bool elfWritten =
                writeMinimalMipsElfWithInitializer(elfPath, "__sinit_test.cpp", 0x00100000u);
            const bool configWritten =
                writeRecompilerTestConfig(configPath, elfPath, outputPath, {"__sinit_test.cpp"});
            t.IsTrue(elfWritten && configWritten,
                     "initializer regression inputs should be generated");

            if (elfWritten && configWritten)
            {
                PS2Recompiler recompiler(configPath.string());
                t.IsTrue(recompiler.initialize(),
                         "initializer regression config should initialize");
                t.IsTrue(recompiler.recompile(),
                         "a decodable skipped initializer should use guest fallback");
                const RecompilerReporter::Counters &counters = recompiler.reportCounters();
                t.Equals(counters.correctnessCriticalGuestFallbacks, static_cast<size_t>(1u),
                         "the ignored initializer skip should be reported");
                t.Equals(counters.correctnessCriticalFailures, static_cast<size_t>(0u),
                         "guest fallback should avoid a correctness-critical failure");
                t.Equals(counters.functionsSkipped, static_cast<size_t>(0u),
                         "the initializer should not remain skipped");
                t.Equals(counters.functionsRecompiled, static_cast<size_t>(1u),
                         "the original initializer body should be recompiled");
            }

            std::error_code removeError;
            std::filesystem::remove_all(tempRoot, removeError);
        });

        tc.Run("missing constructor-table targets fail recompilation", [](TestCase &t) {
            const std::string uniqueSuffix =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            const std::filesystem::path tempRoot =
                std::filesystem::temp_directory_path() / ("ps2recomp-missing-initializer-" + uniqueSuffix);
            const std::filesystem::path elfPath = tempRoot / "initializer.elf";
            const std::filesystem::path configPath = tempRoot / "initializer.toml";
            const std::filesystem::path outputPath = tempRoot / "output";
            std::filesystem::create_directories(tempRoot);

            const bool elfWritten =
                writeMinimalMipsElfWithInitializer(elfPath, "ordinary_entry", 0x00100040u);
            const bool configWritten =
                writeRecompilerTestConfig(configPath, elfPath, outputPath, {});
            t.IsTrue(elfWritten && configWritten,
                     "missing-initializer regression inputs should be generated");

            if (elfWritten && configWritten)
            {
                {
                    PS2Recompiler recompiler(configPath.string());
                    t.IsTrue(recompiler.initialize(),
                             "missing-initializer regression config should initialize");
                    t.IsFalse(recompiler.recompile(),
                              "an unresolved .ctors target should be correctness-fatal");
                    t.Equals(recompiler.reportCounters().correctnessCriticalFailures,
                             static_cast<size_t>(1u),
                             "the unresolved constructor target should appear in the report");
                }

                const bool overrideWritten =
                    writeRecompilerTestConfig(
                        configPath, elfPath, outputPath, {},
                        {"memclr@0x00100040"});
                t.IsTrue(overrideWritten,
                         "manual initializer override config should be generated");
                if (overrideWritten)
                {
                    PS2Recompiler overridden(configPath.string());
                    t.IsTrue(overridden.initialize(),
                             "manual initializer override should initialize");
                    t.IsTrue(overridden.recompile(),
                             "a resolved address-bound handler should satisfy the constructor target");
                    t.Equals(overridden.reportCounters().functionsStubbed,
                             static_cast<size_t>(1u),
                             "the resolved manual initializer should be emitted as a stub binding");
                }
            }

            std::error_code removeError;
            std::filesystem::remove_all(tempRoot, removeError);
        });

        tc.Run("section range checks do not wrap at the top of the address space", [](TestCase &t) {
            Section top{};
            top.address = 0xFFFFFFF0u;
            top.size = 0x20u;
            top.isCode = true;
            // Old code computed the end in uint32_t (0xFFFFFFF0 + 0x20 -> 0x10) and missed every
            // address in this section, including its own start.
            t.IsTrue(sectionContainsAddress(top, 0xFFFFFFF0u), "a section contains its own start");
            t.IsTrue(sectionContainsAddress(top, 0xFFFFFFFCu), "a section contains its last word");
            t.IsTrue(sectionEnd(top) == 0x100000010ull, "the section end is computed in 64 bits");
            t.IsFalse(sectionContainsAddress(top, 0x00000010u),
                      "the wrapped uint32_t end must not make low addresses look inside");
            t.IsFalse(sectionContainsAddress(top, 0xFFFFFFEFu), "an address below the section is outside");
            t.IsTrue(sectionContainsAddress(top, 0xFFFFFFFFu),
                     "the 64-bit range check covers the byte at 0xFFFFFFFF, which a clamped function end cannot name");
            t.Equals(clampFunctionEnd(0x100000000ull), 0xFFFFFFFFu,
                     "a function end past the address space clamps to 0xFFFFFFFF, not to a wrapped 0");
            t.Equals(clampFunctionEnd(0x00100100ull), 0x00100100u,
                     "a normal function end is unchanged by clamping");
            Section normal{};
            normal.address = 0x00100000u;
            normal.size = 0x100u;
            t.IsTrue(sectionContainsAddress(normal, 0x001000FCu), "a normal section still contains its tail");
            t.IsFalse(sectionContainsAddress(normal, 0x00100100u), "a normal section ends at address + size");
        });

        tc.Run("elf parser rejects a 64-bit MIPS ELF instead of truncating its addresses", [](TestCase &t) {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-parser64-" + uniqueSuffix + ".elf");

            const bool writeOk = writeMinimalMipsElf64WithHighText(elfPath);
            t.IsTrue(writeOk, "temporary 64-bit ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfParser parser(elfPath.string());
            t.IsFalse(parser.parse(),
                      "a 64-bit ELF must be rejected, not parsed with its 0x100000000 addresses truncated to 0");
            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError);
        });

        tc.Run("respect max length for .cpp filenames", [](TestCase& t) {
            
            t.IsTrue(PS2Recompiler::ClampFilenameLength("ReallyLongFunctionNameReallyLongFunctionNameReallyLongFunctionName_0x12345678",".cpp",50).length() <= 50,"Function name must be max 50 characters");

            t.IsTrue(PS2Recompiler::ClampFilenameLength("ReallyLongFunctionNameReallyLongFunctionNameReallyLongFunctionName_0x12345678", ".cpp", 50).rfind("0x12345678") != std::string::npos, "Function name must mantain the function address at the end, if present");
            
        });
    });
}
