#pragma once

#include <algorithm>
#include <cctype>
#include <string>

/// Text helpers shared by the two INI readers. This project pulls no INI library.
namespace IniText
{
    [[nodiscard]] inline std::string Trim(std::string a_text)
    {
        const auto notSpace = [](unsigned char a_ch) {
            return std::isspace(a_ch) == 0;
        };
        a_text.erase(a_text.begin(), std::ranges::find_if(a_text, notSpace));
        a_text.erase(std::find_if(a_text.rbegin(), a_text.rend(), notSpace).base(), a_text.end());
        return a_text;
    }

    [[nodiscard]] inline std::string StripComment(std::string a_line)
    {
        auto cut = std::string::npos;
        for (const auto pos : { a_line.find(';'), a_line.find('#'), a_line.find("//") }) {
            if (pos != std::string::npos) {
                cut = cut == std::string::npos ? pos : std::min(cut, pos);
            }
        }
        if (cut != std::string::npos) {
            a_line.resize(cut);
        }
        return Trim(std::move(a_line));
    }

    [[nodiscard]] inline std::string ToLower(std::string a_text)
    {
        std::ranges::transform(a_text, a_text.begin(), [](unsigned char a_ch) {
            return static_cast<char>(std::tolower(a_ch));
        });
        return a_text;
    }
}
