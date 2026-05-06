#include "Settings.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>

namespace
{
    constexpr auto INI_PATH = "Data/SKSE/Plugins/AetherialBloodlines.ini";

    Settings::AttributeGains g_defaultGains{};
    std::unordered_map<std::string, Settings::AttributeGains> g_raceGains;

    [[nodiscard]] std::string Trim(std::string a_text)
    {
        const auto isSpace = [](unsigned char a_ch) {
            return std::isspace(a_ch) != 0;
        };

        a_text.erase(a_text.begin(), std::find_if(a_text.begin(), a_text.end(), [&](unsigned char a_ch) {
            return !isSpace(a_ch);
        }));

        a_text.erase(std::find_if(a_text.rbegin(), a_text.rend(), [&](unsigned char a_ch) {
            return !isSpace(a_ch);
        }).base(), a_text.end());

        return a_text;
    }

    [[nodiscard]] std::string StripComment(std::string a_line)
    {
        const auto semi = a_line.find(';');
        const auto hash = a_line.find('#');
        const auto slash = a_line.find("//");

        auto cut = std::string::npos;
        for (const auto pos : { semi, hash, slash }) {
            if (pos != std::string::npos) {
                cut = cut == std::string::npos ? pos : std::min(cut, pos);
            }
        }

        if (cut != std::string::npos) {
            a_line.resize(cut);
        }

        return Trim(a_line);
    }

    [[nodiscard]] std::string ToLower(std::string a_text)
    {
        std::ranges::transform(a_text, a_text.begin(), [](unsigned char a_ch) {
            return static_cast<char>(std::tolower(a_ch));
        });
        return a_text;
    }

    [[nodiscard]] bool TryParseInt(const std::string& a_text, std::int32_t& a_out)
    {
        const auto text = Trim(a_text);
        if (text.empty()) {
            return false;
        }

        std::int32_t value = 0;
        const auto* first = text.data();
        const auto* last = first + text.size();

        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last) {
            return false;
        }

        a_out = std::clamp(value, 0, 1000);
        return true;
    }

    void ApplyKey(Settings::AttributeGains& a_gains, std::string a_key, const std::string& a_value)
    {
        a_key = ToLower(Trim(std::move(a_key)));

        std::int32_t value = 0;
        if (!TryParseInt(a_value, value)) {
            logger::warn("Invalid INI value ignored: {} = {}", a_key, a_value);
            return;
        }

        if (a_key == "health") {
            a_gains.health = value;
        } else if (a_key == "magicka") {
            a_gains.magicka = value;
        } else if (a_key == "stamina") {
            a_gains.stamina = value;
        } else {
            logger::warn("Unknown INI key ignored: {}", a_key);
        }
    }
}

namespace Settings
{
    std::int32_t AttributeGains::Get(RE::ActorValue a_actorValue) const
    {
        switch (a_actorValue) {
        case RE::ActorValue::kHealth:
            return health;
        case RE::ActorValue::kMagicka:
            return magicka;
        case RE::ActorValue::kStamina:
            return stamina;
        default:
            return 10;
        }
    }

    void Load()
    {
        g_defaultGains = {};
        g_raceGains.clear();

        std::ifstream file(INI_PATH);
        if (!file.is_open()) {
            logger::warn("{} not found. Using vanilla +10 defaults.", INI_PATH);
            return;
        }

        std::string currentSection = "Default";
        g_raceGains[currentSection] = g_defaultGains;

        std::string line;
        while (std::getline(file, line)) {
            line = StripComment(std::move(line));
            if (line.empty()) {
                continue;
            }

            if (line.front() == '[' && line.back() == ']') {
                currentSection = Trim(line.substr(1, line.size() - 2));
                if (currentSection.empty()) {
                    currentSection = "Default";
                }

                if (!g_raceGains.contains(currentSection)) {
                    g_raceGains[currentSection] = g_defaultGains;
                }
                continue;
            }

            const auto equals = line.find('=');
            const auto colon = line.find(':');

            auto split = std::string::npos;
            if (equals != std::string::npos && colon != std::string::npos) {
                split = std::min(equals, colon);
            } else if (equals != std::string::npos) {
                split = equals;
            } else {
                split = colon;
            }

            if (split == std::string::npos) {
                logger::warn("Invalid INI line ignored: {}", line);
                continue;
            }

            ApplyKey(g_raceGains[currentSection], line.substr(0, split), line.substr(split + 1));
        }

        if (const auto it = g_raceGains.find("Default"); it != g_raceGains.end()) {
            g_defaultGains = it->second;
        }

        logger::info("Loaded {} race attribute sections from {}", g_raceGains.size(), INI_PATH);
    }

    AttributeGains GetForRace(RE::TESRace* a_race)
    {
        const auto editorID = GetRaceEditorID(a_race);
        if (editorID && editorID[0] != '\0') {
            if (const auto it = g_raceGains.find(editorID); it != g_raceGains.end()) {
                return it->second;
            }
        }

        return g_defaultGains;
    }

    const char* GetRaceEditorID(RE::TESRace* a_race)
    {
        if (!a_race) {
            return "";
        }

        const auto editorID = a_race->GetFormEditorID();
        return editorID ? editorID : "";
    }
}
