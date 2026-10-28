#include "iop_format.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"

#include <cstddef>

namespace ps2x::iop::detail
{
    namespace
    {
        std::string digits(uint64_t value, unsigned base, bool upper)
        {
            const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            std::string out;
            do
            {
                out.insert(out.begin(), set[value % base]);
                value /= base;
            } while (value != 0u);
            return out;
        }
    }

    std::string formatGuest(std::string_view format, const GuestFormatArgs &args)
    {
        std::string out;
        size_t i = 0u;
        const auto next = [&]() -> uint32_t { return args.nextWord ? args.nextWord() : 0u; };
        while (i < format.size())
        {
            const char c = format[i++];
            if (c != '%')
            {
                out += c;
                continue;
            }
            const size_t specStart = i - 1u;
            bool left = false, plus = false, space = false, zero = false, alt = false;
            for (; i < format.size(); ++i)
            {
                const char f = format[i];
                if (f == '-') left = true;
                else if (f == '+') plus = true;
                else if (f == ' ') space = true;
                else if (f == '0') zero = true;
                else if (f == '#') alt = true;
                else break;
            }
            int width = -1;
            if (i < format.size() && format[i] == '*')
            {
                const int32_t w = static_cast<int32_t>(next());
                if (w < 0)
                {
                    left = true;
                    width = -w;
                }
                else
                {
                    width = w;
                }
                ++i;
            }
            else
            {
                for (; i < format.size() && format[i] >= '0' && format[i] <= '9'; ++i)
                    width = (width < 0 ? 0 : width) * 10 + (format[i] - '0');
            }
            int precision = -1;
            if (i < format.size() && format[i] == '.')
            {
                ++i;
                precision = 0;
                if (i < format.size() && format[i] == '*')
                {
                    const int32_t p = static_cast<int32_t>(next());
                    precision = p < 0 ? -1 : p;
                    ++i;
                }
                else
                {
                    for (; i < format.size() && format[i] >= '0' && format[i] <= '9'; ++i)
                        precision = precision * 10 + (format[i] - '0');
                }
            }
            int longs = 0;
            bool half = false, halfHalf = false;
            while (i < format.size() && (format[i] == 'l' || format[i] == 'h'))
            {
                if (format[i] == 'l')
                    ++longs;
                else if (half)
                    halfHalf = true;
                else
                    half = true;
                ++i;
            }
            if (i >= format.size())
            {
                out.append(format.substr(specStart));
                break;
            }
            const char conv = format[i++];

            const auto pad = [&](std::string body, std::string prefix, bool numeric)
            {
                const size_t len = body.size() + prefix.size();
                const size_t w = width > 0 ? static_cast<size_t>(width) : 0u;
                if (len >= w)
                    return prefix + body;
                const size_t fill = w - len;
                if (left)
                    return prefix + body + std::string(fill, ' ');
                // '0' pads after the sign/prefix, and is ignored when a precision is given for an integer (C).
                if (zero && numeric && precision < 0)
                    return prefix + std::string(fill, '0') + body;
                return std::string(fill, ' ') + prefix + body;
            };
            const auto fetch = [&](bool isSigned) -> uint64_t
            {
                if (longs >= 2)
                {
                    if (args.alignPair)
                        args.alignPair();
                    const uint64_t lo = next();
                    const uint64_t hi = next();
                    return lo | (hi << 32);
                }
                uint32_t v = next();
                if (halfHalf)
                    v = isSigned ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))) : (v & 0xFFu);
                else if (half)
                    v = isSigned ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))) : (v & 0xFFFFu);
                return isSigned ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))) : v;
            };
            const auto integer = [&](uint64_t magnitude, std::string sign, unsigned base, bool upper, std::string prefix)
            {
                std::string body = (precision == 0 && magnitude == 0u) ? std::string() : digits(magnitude, base, upper);
                if (precision > 0 && body.size() < static_cast<size_t>(precision))
                    body.insert(0u, static_cast<size_t>(precision) - body.size(), '0');
                out += pad(body, sign + prefix, true);
            };

            switch (conv)
            {
            case 'd':
            case 'i':
            {
                const int64_t v = static_cast<int64_t>(fetch(true));
                const uint64_t mag = v < 0 ? (0u - static_cast<uint64_t>(v)) : static_cast<uint64_t>(v);
                integer(mag, v < 0 ? "-" : plus ? "+" : space ? " " : "", 10u, false, "");
                break;
            }
            case 'u':
                integer(fetch(false), "", 10u, false, "");
                break;
            case 'x':
            case 'X':
            {
                const uint64_t v = fetch(false);
                integer(v, "", 16u, conv == 'X', alt && v != 0u ? (conv == 'X' ? "0X" : "0x") : "");
                break;
            }
            case 'o':
            {
                const uint64_t v = fetch(false);
                integer(v, "", 8u, false, alt && v != 0u ? "0" : "");
                break;
            }
            case 'p':
                integer(next(), "", 16u, false, "");
                break;
            case 'c':
                out += pad(std::string(1, static_cast<char>(next() & 0xFFu)), "", false);
                break;
            case 's':
            {
                // No "(null)" substitution: IOP address 0 is ordinary RAM and is read like any other address.
                const uint32_t address = next();
                std::string s = args.readString ? args.readString(address) : std::string();
                if (precision >= 0 && s.size() > static_cast<size_t>(precision))
                    s.resize(static_cast<size_t>(precision));
                out += pad(s, "", false);
                break;
            }
            case '%':
                out += '%';
                break;
            default:
                out.append(format.substr(specStart, i - specStart));
                break;
            }
        }
        return out;
    }

    namespace
    {
        constexpr size_t kFormatLimit = 4096u;
    }

    std::string formatVariadic(const IopMemory &memory, const IopCpuState &cpu, uint32_t formatAddress, uint32_t firstSlot)
    {
        uint32_t slot = firstSlot;
        GuestFormatArgs args;
        args.nextWord = [&]() -> uint32_t
        {
            const uint32_t s = slot++;
            return s < 4u ? cpu.gpr[4u + s] : memory.read32(cpu.gpr[29] + 4u * s);
        };
        args.alignPair = [&]() { slot = (slot + 1u) & ~1u; };
        args.readString = [&](uint32_t address) { return memory.readString(address, kFormatLimit); };
        return formatGuest(memory.readString(formatAddress, kFormatLimit), args);
    }

    std::string formatVaList(const IopMemory &memory, uint32_t formatAddress, uint32_t listAddress)
    {
        uint32_t ap = listAddress;
        GuestFormatArgs args;
        args.nextWord = [&]() -> uint32_t
        {
            const uint32_t v = memory.read32(ap);
            ap += 4u;
            return v;
        };
        args.alignPair = [&]() { ap = (ap + 7u) & ~7u; };
        args.readString = [&](uint32_t address) { return memory.readString(address, kFormatLimit); };
        return formatGuest(memory.readString(formatAddress, kFormatLimit), args);
    }
}
