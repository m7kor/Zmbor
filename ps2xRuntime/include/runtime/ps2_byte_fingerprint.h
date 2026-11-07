#pragma once

#include <cstddef>
#include <cstdint>

// FNV-1a for diagnostic comparisons of guest byte ranges.
inline uint64_t ps2ByteFingerprint(const uint8_t *data, std::size_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
