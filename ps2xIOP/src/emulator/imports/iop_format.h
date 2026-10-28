#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ps2x::iop::detail
{
    // printf-style formatting for IOP library calls (stdio printf/puts family, sysclib sprintf/vsprintf).
    // nextWord returns the next 32-bit argument word in order: for a variadic call the o32 argument slots after the
    // fixed arguments (a0-a3, then the caller's stack at sp + 16 onwards), for a va_list consecutive words from the
    // list pointer. readString reads a NUL-terminated guest string.
    //
    // Conversions: d i u x X o c s p %, flags - + space 0 #, width and precision (numbers or *), length modifiers
    // hh h l (32-bit on the IOP) and ll (two words, low first, the pair aligned to an even slot as o32 requires).
    // %p prints lowercase hex without a 0x prefix, as the IOP's sprintf does on a real PS2 (ps2autotests
    // kernel/iop/libc/sprintf: "%p" of 0x01234567 prints 1234567). An unknown conversion is copied as written.
    struct GuestFormatArgs
    {
        std::function<uint32_t()> nextWord;
        // Called before a 64-bit argument; aligns the next slot to an even index (o32). May be empty (va_list).
        std::function<void()> alignPair;
        std::function<std::string(uint32_t)> readString;
    };

    std::string formatGuest(std::string_view format, const GuestFormatArgs &args);

    struct IopCpuState;
    class IopMemory;

    // Arguments of a variadic call whose first variadic argument is o32 slot firstSlot (a0-a3 = slots 0-3, slot n >= 4
    // at the caller's sp + 4n).
    std::string formatVariadic(const IopMemory &memory, const IopCpuState &cpu, uint32_t formatAddress, uint32_t firstSlot);
    // Arguments read from a va_list (consecutive words from listAddress; 64-bit values 8-byte aligned).
    std::string formatVaList(const IopMemory &memory, uint32_t formatAddress, uint32_t listAddress);
}
