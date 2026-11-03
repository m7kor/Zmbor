#include "Common.h"
#include "Unimplemented.h"

namespace ps2_stubs
{
    void TODO(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("unknown", rdram, ctx, runtime);
    }

    void TODO_NAMED(const char *name, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const std::string stubName = name ? name : "unknown";

        uint32_t callCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_stubWarningMutex);
            callCount = ++g_stubWarningCount[stubName];
        }

        if (callCount > kMaxStubWarningsPerName)
        {
            // Only the *warning* is suppressed past the cap: the stub still
            // fails below. Returning -1 here used to let the guest continue
            // as if an unimplemented function had succeeded, which silently
            // corrupted everything downstream once a stub was called 9+ times
            // (the cap is 8, and the per-name count is never reset).
            if (callCount == (kMaxStubWarningsPerName + 1))
            {
                std::cerr << "Warning: Further warnings for PS2 stub '" << stubName
                          << "' are suppressed after " << kMaxStubWarningsPerName << " warnings"
                          << " (the stub still fails loudly on every call)" << std::endl;
            }
        }
        else
        {
            uint32_t stub_num = getRegU32(ctx, 2);   // $v0
            uint32_t caller_ra = getRegU32(ctx, 31); // $ra

            std::cerr << "Warning: Unimplemented PS2 stub called. name=" << stubName
                      << " PC=0x" << std::hex << ctx->pc
                      << ", RA=0x" << caller_ra
                      << ", Stub# guess (from $v0)=0x" << stub_num << std::dec << std::endl;

            // More context for debugging
            std::cerr << "  Args: $a0=0x" << std::hex << getRegU32(ctx, 4)
                      << ", $a1=0x" << getRegU32(ctx, 5)
                      << ", $a2=0x" << getRegU32(ctx, 6)
                      << ", $a3=0x" << getRegU32(ctx, 7) << std::dec << std::endl;
        }

        throw std::runtime_error("Unimplemented PS2 stub called: " + stubName);
    }
}
