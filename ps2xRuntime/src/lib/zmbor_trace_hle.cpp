// HLE call bracket of the bounded trace (runtime/zmbor_trace.h): reads guest registers, so it is part of
// ps2_runtime only; the trace core (zmbor_trace.cpp) stays free of runtime types.
#include "runtime/zmbor_trace.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <string>

namespace zmbor_trace
{
    namespace
    {
        uint32_t reg32(const R5900Context *ctx, int r)
        {
            return ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0)) : 0u;
        }
    }

    void hleEnter(const char *name, uint32_t stubAddress, const R5900Context *ctx)
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "enter stub=0x%08x ra=0x%08x a0=0x%x a1=0x%x a2=0x%x a3=0x%x",
                      stubAddress, reg32(ctx, 31), reg32(ctx, 4), reg32(ctx, 5), reg32(ctx, 6), reg32(ctx, 7));
        record(kHle, std::string("hle:") + name, reg32(ctx, 31), detail);
    }

    void hleLeave(const char *name, const R5900Context *ctx)
    {
        recordResult(std::string("hle:") + name, static_cast<int32_t>(reg32(ctx, 2)));
    }
}
