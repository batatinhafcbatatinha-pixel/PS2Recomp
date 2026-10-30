#pragma once

#include "../core/iop_memory.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace ps2x::iop::detail
{
    // printf-style formatting for IRX code (sysclib sprintf/vsprintf, stdio printf). nextArg yields the 32-bit
    // variadic words in order: o32 passes the first four argument words in a0-a3 and the rest at sp+0x10 onward.
    inline std::string formatIopString(const IopMemory &memory, const std::string &format, const std::function<uint32_t()> &nextArg)
    {
        std::string out;
        for (size_t i = 0; i < format.size(); ++i)
        {
            if (format[i] != '%')
            {
                out.push_back(format[i]);
                continue;
            }
            std::string spec = "%";
            size_t j = i + 1;
            while (j < format.size() && std::string_view("-+ #0").find(format[j]) != std::string_view::npos)
                spec.push_back(format[j++]);
            const auto takeNumber = [&]()
            {
                if (j < format.size() && format[j] == '*')
                {
                    spec += std::to_string(static_cast<int32_t>(nextArg()));
                    ++j;
                    return;
                }
                while (j < format.size() && format[j] >= '0' && format[j] <= '9')
                    spec.push_back(format[j++]);
            };
            takeNumber();
            if (j < format.size() && format[j] == '.')
            {
                spec.push_back(format[j++]);
                takeNumber();
            }
            while (j < format.size() && (format[j] == 'l' || format[j] == 'h'))
                ++j; // IOP ints/longs are 32-bit
            if (j >= format.size())
            {
                out += format.substr(i);
                break;
            }
            const char conversion = format[j];
            char buffer[512];
            switch (conversion)
            {
            case 'd':
            case 'i':
                std::snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), static_cast<int32_t>(nextArg()));
                out += buffer;
                break;
            case 'u':
            case 'x':
            case 'X':
            case 'o':
                std::snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), nextArg());
                out += buffer;
                break;
            case 'p':
                std::snprintf(buffer, sizeof(buffer), "%x", nextArg());
                out += buffer;
                break;
            case 'c':
                std::snprintf(buffer, sizeof(buffer), (spec + "c").c_str(), static_cast<int>(nextArg() & 0xFFu));
                out += buffer;
                break;
            case 's':
            {
                const uint32_t address = nextArg();
                const std::string text = address ? memory.readString(address, 4096u) : std::string("(null)");
                std::snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), text.c_str());
                out += buffer;
                break;
            }
            case '%':
                out.push_back('%');
                break;
            default:
                out += format.substr(i, j - i + 1);
                break;
            }
            i = j;
        }
        return out;
    }
}
