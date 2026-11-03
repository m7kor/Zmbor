#ifndef PS2RECOMP_ELF_PARSER_H
#define PS2RECOMP_ELF_PARSER_H

#include <cstdint>
#include <elfio/elfio.hpp>
#include "ps2recomp/types.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

namespace ps2recomp
{
        struct Relocation;
        struct Section;
        struct Function;
        struct Symbol;
        class RecompilerReporter;

        class ElfParser
        {
        public:
                explicit ElfParser(const std::string &filePath);
                ~ElfParser();

                bool parse();

                bool loadGhidraFunctionMap(const std::string &mapPath);
                std::vector<Function> extractFunctions() const;

                // Phase 12 — recursive-descent function discovery (stripped-ELF front door).
                //
                // discoverFromEntryPoint() walks reachable code from the ELF entry point:
                // direct JAL targets become new functions, direct J branches and
                // conditional branches stay inside the current function, JR $ra ends a
                // function, and JR switches using the LUI/ADDU/SLL/JR pattern have
                // their tables recovered (see discoverJumpTables below). Phase 14
                // adds: JALR through a register holding a block-proven constant
                // becomes a new function (like JAL), JR through such a register
                // becomes an intra-function edge (like J), and — after descent —
                // singleton data words pointing at plausible, not-yet-decoded
                // code addresses become new functions (lowest priority, same
                // merge order as the rest of discovery). The returned
                // functions carry start addresses and end == 0: ends are inferred by
                // extractFunctions() from the next function start / section end with
                // the Phase 3 64-bit clamp helpers, exactly like symbol-less extras.
                //
                // The result is a PROVEN reachable set, deliberately narrower than the
                // ScanFunctionStartsFallback heuristic in loadDebugFunctions(), which
                // proposes a start for every JAL word anywhere in a code section,
                // including dead code. A function called only from dead code is NOT
                // in this set. extractFunctions() merges discovery LAST, so it can
                // never overwrite a symbol-, Ghidra- or extra-derived range.
                struct FunctionDiscovery
                {
                    std::vector<Function> functions;
                    std::vector<JumpTable> jumpTables;
                };
                FunctionDiscovery discoverFromEntryPoint() const;
                std::vector<Function> extractExtraFunctions() const;
                std::vector<Symbol> extractSymbols();
                std::vector<Section> getSections();
                std::vector<Relocation> getRelocations();

                // Helper methods
                bool isValidAddress(uint32_t address) const;
                uint32_t readWord(uint32_t address) const;
                uint8_t *getSectionData(const std::string &sectionName) const;
                uint32_t getSectionAddress(const std::string &sectionName) const;
                uint32_t getSectionSize(const std::string &sectionName) const;
                uint32_t getEntryPoint() const;
                void setReporter(RecompilerReporter *reporter);
                void debugAddress(uint32_t address) const;

        private:
                std::string m_filePath;
                std::unique_ptr<ELFIO::elfio> m_elf;

                std::vector<Section> m_sections;
                std::vector<Symbol> m_symbols;
                std::vector<Relocation> m_relocations;
                std::vector<Function> m_extraFunctions;
                bool m_hasLoadedGhidraMap = false;
                RecompilerReporter *m_reporter = nullptr;
                std::unordered_set<uint32_t> m_ghidraMapStarts;

                void loadSections();
                void loadSymbols();
                void loadRelocations();
                void loadDebugFunctions();
                bool isExecutableSection(const ELFIO::section *section) const;
                bool isDataSection(const ELFIO::section *section) const;
        };

} // namespace ps2recomp

#endif // PS2RECOMP_ELF_PARSER_H
