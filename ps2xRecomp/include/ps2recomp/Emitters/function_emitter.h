#ifndef PS2RECOMP_FUNCTION_EMITTER_H
#define PS2RECOMP_FUNCTION_EMITTER_H

#include <string>
#include <vector>

namespace ps2recomp
{
    struct Function;
    struct Instruction;
    class CodeGenerator;

    class FunctionEmitter
    {
    public:
        explicit FunctionEmitter(CodeGenerator &codeGenerator);

        // Phase 12 — allFunctions lets the emitter's CFA run verify
        // external jump-table targets against known functions, exactly
        // like the resume-target pre-pass does. Without it the emitter
        // analyzed with a null list, so every all-external table looked
        // unresolved and spuriously promoted fallback entries (and warned)
        // even after the table was proven.
        std::string emit(const Function &function, const std::vector<Instruction> &instructions, bool useHeaders,
                         const std::vector<Function> *allFunctions = nullptr);

    private:
        CodeGenerator &m_codeGenerator;
    };
}

#endif // PS2RECOMP_FUNCTION_EMITTER_H
