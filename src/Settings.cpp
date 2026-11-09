#include "PCH.h"
#include "Settings.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "IniText.h"

namespace
{
    constexpr auto INI_PATH = "Data/SKSE/Plugins/AetherialBloodlines.ini";

    struct DefaultRaceSection
    {
        const char* section;
        Settings::AttributeGains gains;
    };

    // What Reset All restores, the same values the shipped INI holds, every race summing to 30.
    // Load never seeds from it, so a vanilla race whose section is deleted falls to [Default] like any other.
    constexpr DefaultRaceSection DEFAULT_RACE_SECTIONS[] = {
        { "Default", { 10, 10, 10 } },
        { "ArgonianRace", { 10, 5, 15 } },
        { "BretonRace", { 5, 15, 10 } },
        { "DarkElfRace", { 10, 10, 10 } },
        { "HighElfRace", { 5, 20, 5 } },
        { "ImperialRace", { 10, 10, 10 } },
        { "KhajiitRace", { 10, 5, 15 } },
        { "NordRace", { 15, 5, 10 } },
        { "OrcRace", { 20, 5, 5 } },
        { "RedguardRace", { 10, 5, 15 } },
        { "WoodElfRace", { 10, 5, 15 } },
    };

    Settings::AttributeGains g_defaultGains{};
    std::unordered_map<std::string, Settings::AttributeGains> g_raceGains;

    // Load() treats an unrecognised section as a race EditorID, so every non-race section has to be listed here
    // "racials" no longer ships a key, but an upgraded install can still have the old section
    const std::unordered_set<std::string> g_nonRaceSections{ "greenpact", "racials", "settings" };

    // What a new game starts each menu setting at, so a list author can ship the values they want
    std::unordered_map<std::string, float> g_menuSettings;

    // Section names in file order and original casing, so the menu can list and label them
    std::vector<std::string> g_gainSections;

    [[nodiscard]] const char* GainKeyName(RE::ActorValue a_actorValue)
    {
        switch (a_actorValue) {
        case RE::ActorValue::kHealth:
            return "Health";
        case RE::ActorValue::kMagicka:
            return "Magicka";
        case RE::ActorValue::kStamina:
            return "Stamina";
        default:
            return nullptr;
        }
    }

    using IniText::StripComment;
    using IniText::ToLower;
    using IniText::Trim;

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

        a_out = std::clamp(value, 0, Settings::MAX_GAIN);
        if (a_out != value) {
            logger::warn("Attribute gain {} is outside 0 to {}, clamped to {}", value, Settings::MAX_GAIN, a_out);
        }
        return true;
    }

    [[nodiscard]] bool TryParseFloat(const std::string& a_text, float& a_out)
    {
        const auto text = Trim(a_text);
        if (text.empty()) {
            return false;
        }

        float value = 0.0f;
        const auto* first = text.data();
        const auto* last = first + text.size();

        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last) {
            return false;
        }

        a_out = value;
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

    float GetSetting(std::string_view a_key, float a_fallback)
    {
        const auto it = g_menuSettings.find(ToLower(std::string(a_key)));
        return it != g_menuSettings.end() ? it->second : a_fallback;
    }

    bool ForceSettingsOnLoad()
    {
        return GetSetting("ForceOnLoad", 0.0f) != 0.0f;
    }

    // Rewrites one line in place, so an author's comments and ordering survive being edited from the menu
    // Shape from ItzIvy05/TearsofKyne/src/Settings.cpp:363, with the section tracked, because every race section repeats Health, Magicka and Stamina
    void WriteKey(std::string_view a_section, std::string_view a_key, const std::string& a_formatted)
    {
        const auto wantedSection = ToLower(std::string(a_section));
        const auto wantedKey = ToLower(std::string(a_key));

        std::vector<std::string> lines;
        {
            std::ifstream in(INI_PATH);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                lines.push_back(std::move(line));
            }
        }

        std::string section;
        bool written = false;
        std::size_t sectionEnd = 0;
        bool sawSection = false;

        for (std::size_t i = 0; i < lines.size() && !written; ++i) {
            const auto bare = StripComment(lines[i]);
            if (!bare.empty() && bare.front() == '[' && bare.back() == ']') {
                section = ToLower(Trim(bare.substr(1, bare.size() - 2)));
                if (section == wantedSection) {
                    sawSection = true;
                    sectionEnd = i + 1;
                }
                continue;
            }
            if (section != wantedSection) {
                continue;
            }
            sectionEnd = i + 1;

            const auto eq = bare.find('=');
            if (eq != std::string::npos && ToLower(Trim(bare.substr(0, eq))) == wantedKey) {
                lines[i] = a_formatted;
                written = true;
            }
        }

        if (!written) {
            if (sawSection) {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), a_formatted);
            } else {
                lines.emplace_back();
                lines.push_back(std::format("[{}]", a_section));
                lines.push_back(a_formatted);
            }
        }

        std::ofstream out(INI_PATH, std::ios::trunc);
        if (!out.is_open()) {
            logger::error("Could not write [{}] {} to {}", a_section, a_key, INI_PATH);
            return;
        }
        for (const auto& line : lines) {
            out << line << '\n';
        }
        logger::info("Saved [{}] {} to {}", a_section, a_formatted, INI_PATH);
    }

    void SaveSetting(std::string_view a_key, float a_value)
    {
        g_menuSettings[ToLower(std::string(a_key))] = a_value;
        WriteKey("Settings", a_key, std::format("{} = {}", a_key, a_value));
    }

    const std::vector<std::string>& GainSections()
    {
        return g_gainSections;
    }

    AttributeGains GetGainsForSection(std::string_view a_section)
    {
        const auto it = g_raceGains.find(ToLower(std::string(a_section)));
        return it != g_raceGains.end() ? it->second : g_defaultGains;
    }

    AttributeGains DefaultGainsForSection(std::string_view a_section)
    {
        auto lower = ToLower(std::string(a_section));
        const auto find = [](std::string_view a_lower) -> const AttributeGains* {
            for (const auto& entry : DEFAULT_RACE_SECTIONS) {
                if (ToLower(std::string(entry.section)) == a_lower) {
                    return &entry.gains;
                }
            }
            return nullptr;
        };
        if (const auto* gains = find(lower)) {
            return *gains;
        }

        // Same rule as GetForRace, so resetting a vampire section gives it its living race's spread
        constexpr std::string_view vampireSuffix = "vampire";
        if (lower.size() > vampireSuffix.size() && lower.ends_with(vampireSuffix)) {
            lower.resize(lower.size() - vampireSuffix.size());
            if (const auto* gains = find(lower)) {
                return *gains;
            }
        }
        return {};
    }

    void SetGain(std::string_view a_section, RE::ActorValue a_actorValue, std::int32_t a_value)
    {
        if (!GainKeyName(a_actorValue)) {
            return;
        }

        const auto lower = ToLower(std::string(a_section));
        auto& gains = g_raceGains[lower];
        const auto clamped = std::clamp(a_value, 0, MAX_GAIN);

        switch (a_actorValue) {
        case RE::ActorValue::kHealth:
            gains.health = clamped;
            break;
        case RE::ActorValue::kMagicka:
            gains.magicka = clamped;
            break;
        default:
            gains.stamina = clamped;
            break;
        }

        // GetForRace falls back to the defaults, so editing Default has to move both or they disagree until the next load
        if (lower == "default") {
            g_defaultGains = gains;
        }
    }

    void PersistGain(std::string_view a_section, RE::ActorValue a_actorValue)
    {
        const auto* key = GainKeyName(a_actorValue);
        if (!key) {
            return;
        }

        const auto gains = GetGainsForSection(a_section);
        WriteKey(a_section, key, std::format("{} = {}", key, gains.Get(a_actorValue)));
    }

    void ResetGainsToDefaults()
    {
        for (const auto& section : g_gainSections) {
            const auto defaults = DefaultGainsForSection(section);
            SetGain(section, RE::ActorValue::kHealth, defaults.health);
            SetGain(section, RE::ActorValue::kMagicka, defaults.magicka);
            SetGain(section, RE::ActorValue::kStamina, defaults.stamina);
            PersistGain(section, RE::ActorValue::kHealth);
            PersistGain(section, RE::ActorValue::kMagicka);
            PersistGain(section, RE::ActorValue::kStamina);
        }

        logger::info("Reset all race attribute gains to the 30-point defaults.");
    }

    void Load()
    {
        g_defaultGains = {};
        g_raceGains.clear();
        g_gainSections.clear();
        g_menuSettings.clear();

        std::ifstream file(INI_PATH);
        if (!file.is_open()) {
            logger::warn("{} not found. Using vanilla +10 defaults.", INI_PATH);
            return;
        }

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            line = StripComment(std::move(line));
            if (!line.empty()) {
                lines.push_back(line);
            }
        }

        {
            AttributeGains defaults = g_defaultGains;
            std::string section;
            for (const auto& l : lines) {
                if (l.front() == '[' && l.back() == ']') {
                    section = Trim(l.substr(1, l.size() - 2));
                    continue;
                }
                if (ToLower(section) == "default") {
                    const auto eq = l.find('=');
                    if (eq != std::string::npos) {
                        ApplyKey(defaults, l.substr(0, eq), l.substr(eq + 1));
                    }
                }
            }
            g_defaultGains = defaults;
            g_raceGains["default"] = g_defaultGains;
        }

        {
            std::string section = "default";
            bool skipSection = false;
            for (const auto& l : lines) {
                if (l.front() == '[' && l.back() == ']') {
                    const auto original = Trim(l.substr(1, l.size() - 2));
                    section = ToLower(original);
                    if (section.empty()) {
                        section = "default";
                    }
                    skipSection = g_nonRaceSections.contains(section);
                    if (!skipSection) {
                        if (!g_raceGains.contains(section)) {
                            g_raceGains[section] = g_defaultGains;
                        }
                        const auto existing = std::ranges::find_if(g_gainSections, [&](const std::string& a_item) {
                            return ToLower(a_item) == section;
                        });
                        if (existing == g_gainSections.end()) {
                            g_gainSections.push_back(original);
                        }
                    }
                    continue;
                }
                if (skipSection) {
                    continue;
                }
                const auto eq = l.find('=');
                if (eq != std::string::npos) {
                    ApplyKey(g_raceGains[section], l.substr(0, eq), l.substr(eq + 1));
                }
            }
        }

        {
            std::string section;
            for (const auto& l : lines) {
                if (l.front() == '[' && l.back() == ']') {
                    section = ToLower(Trim(l.substr(1, l.size() - 2)));
                    continue;
                }
                if (section != "settings") {
                    continue;
                }
                const auto eq = l.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                float value = 0.0f;
                if (TryParseFloat(l.substr(eq + 1), value)) {
                    g_menuSettings[ToLower(Trim(l.substr(0, eq)))] = value;
                } else {
                    logger::warn("Invalid [Settings] value ignored: {}", l);
                }
            }
        }

        logger::info("Loaded {} race attribute sections and {} menu defaults from {}", g_raceGains.size(),
                     g_menuSettings.size(), INI_PATH);
    }

    AttributeGains GetForRace(RE::TESRace* a_race)
    {
        auto editorID = ToLower(std::string(GetRaceEditorID(a_race)));
        if (!editorID.empty()) {
            if (const auto it = g_raceGains.find(editorID); it != g_raceGains.end()) {
                return it->second;
            }

            // Vampire variants share their base race's spread unless the INI names them separately
            constexpr std::string_view vampireSuffix = "vampire";
            if (editorID.size() > vampireSuffix.size() && editorID.ends_with(vampireSuffix)) {
                editorID.resize(editorID.size() - vampireSuffix.size());
                if (const auto it = g_raceGains.find(editorID); it != g_raceGains.end()) {
                    return it->second;
                }
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
