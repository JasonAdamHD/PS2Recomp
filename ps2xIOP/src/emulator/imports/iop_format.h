#pragma once

#include "../core/iop_memory.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ps2x::iop::detail
{
    // printf-style formatting for IOP guest calls. Arguments are 32-bit words supplied by nextArg (registers,
    // then the stack, or a va_list in guest memory); %s reads a string from guest memory.
    template <typename NextArg>
    std::string formatGuestString(const IopMemory &memory, const std::string &format, NextArg nextArg)
    {
        std::string out;
        out.reserve(format.size() + 32u);
        for (size_t i = 0; i < format.size(); ++i)
        {
            const char ch = format[i];
            if (ch != '%')
            {
                out += ch;
                continue;
            }
            // Collect "%[flags][width][.precision][length]conversion".
            std::string spec = "%";
            size_t j = i + 1u;
            while (j < format.size() && std::string("-+ #0").find(format[j]) != std::string::npos)
                spec += format[j++];
            while (j < format.size() && (std::isdigit(static_cast<unsigned char>(format[j])) || format[j] == '*'))
            {
                if (format[j] == '*')
                    spec += std::to_string(static_cast<int32_t>(nextArg()));
                else
                    spec += format[j];
                ++j;
            }
            if (j < format.size() && format[j] == '.')
            {
                spec += format[j++];
                while (j < format.size() && std::isdigit(static_cast<unsigned char>(format[j])))
                    spec += format[j++];
            }
            while (j < format.size() && (format[j] == 'l' || format[j] == 'h'))
                ++j; // 32-bit target: length modifiers don't change the argument size
            if (j >= format.size())
            {
                out += spec;
                break;
            }
            const char conversion = format[j];
            i = j;
            char buffer[512];
            switch (conversion)
            {
            case '%':
                out += '%';
                break;
            case 'd':
            case 'i':
                std::snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), static_cast<int>(static_cast<int32_t>(nextArg())));
                out += buffer;
                break;
            case 'u':
            case 'x':
            case 'X':
            case 'o':
                std::snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), static_cast<unsigned>(nextArg()));
                out += buffer;
                break;
            case 'p':
                std::snprintf(buffer, sizeof(buffer), "0x%x", static_cast<unsigned>(nextArg()));
                out += buffer;
                break;
            case 'c':
                out += static_cast<char>(nextArg() & 0xFFu);
                break;
            case 's':
            {
                const uint32_t address = nextArg();
                const std::string text = address ? memory.readString(address, 4096u) : std::string("(null)");
                std::snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), text.c_str());
                out += buffer;
                break;
            }
            default:
                out += spec;
                out += conversion;
                break;
            }
        }
        return out;
    }

    // Arguments of a variadic O32 call: registers from firstRegister up to a3, then the stack from sp+16.
    struct IopVarargs
    {
        const IopMemory &memory;
        const std::array<uint32_t, 32> &gpr;
        int nextRegister;
        uint32_t stackSlot = 0;

        uint32_t operator()()
        {
            if (nextRegister <= 7)
                return gpr[nextRegister++];
            return memory.read32(gpr[29] + 16u + 4u * stackSlot++);
        }
    };

    // Arguments from a va_list in guest memory.
    struct IopVaList
    {
        const IopMemory &memory;
        uint32_t address;

        uint32_t operator()()
        {
            const uint32_t value = memory.read32(address);
            address += 4u;
            return value;
        }
    };
}
