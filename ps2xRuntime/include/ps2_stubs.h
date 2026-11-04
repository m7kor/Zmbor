#pragma once

struct R5900Context;
class  PS2Runtime;

#include <cstdint>
#include "ps2_call_list.h"
#include "runtime/ps2_memory.h"

namespace ps2_stubs
{
#define PS2_DECLARE_STUB(name) void name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    PS2_STUB_LIST(PS2_DECLARE_STUB)
#undef PS2_DECLARE_STUB

    // Gate B (S2): these declarations used to arrive via the private
    // src/lib/Kernel/Stubs/Unimplemented.h, which this public header
    // included. Generated code includes only ps2_stubs.h, so the
    // declarations live here now; the private header is a shim.
    void TODO(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void TODO_NAMED(const char *name, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    void resetSifState();
}
