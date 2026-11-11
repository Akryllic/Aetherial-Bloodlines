#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
    enum class ActorValue;
    class TESRace;
}

namespace Settings
{
    /// @brief Ceiling for a per-level attribute gain, shared by the INI parser and the menu slider so the two cannot disagree about what is a legal value.
    inline constexpr std::int32_t MAX_GAIN = 50;

    /// @brief Standard per-level attribute budget across all three pools.
    inline constexpr std::int32_t STANDARD_TOTAL = 30;

    struct AttributeGains
    {
        std::int32_t health{ 10 };
        std::int32_t magicka{ 10 };
        std::int32_t stamina{ 10 };

        std::int32_t Get(RE::ActorValue a_actorValue) const;
    };

    void Load();
    AttributeGains GetForRace(RE::TESRace* a_race);
    const char* GetRaceEditorID(RE::TESRace* a_race);

    /// @brief A value from the INI's [Settings] section, which holds what a new game starts with.
    /// @return @p a_fallback when the key is absent, so a stale INI never disables anything.
    [[nodiscard]] float GetSetting(std::string_view a_key, float a_fallback);

    /// @brief Whether the INI values are re-applied on every load rather than only on a new game.
    /// @note For a modlist author locking a list down. It makes the in-game menu effectively read only.
    [[nodiscard]] bool ForceSettingsOnLoad();

    /// @brief Write one key into the INI's [Settings] section so the change outlives the save.
    /// @note Rewrites only that key's line. Comments, ordering and every other section survive. Creates the section, or the key inside it, when either is missing.
    void SaveSetting(std::string_view a_key, float a_value);

    /// @brief Every attribute-gain section the INI defines, in file order and original casing.
    /// @note "Default" plus one per race. Driven by the file, so an added section shows up on its own.
    /// @note A copy, since the menu reads it from another thread.
    [[nodiscard]] std::vector<std::string> GainSections();

    /// @brief The gains an INI section currently holds, e.g. "NordRace" or "Default".
    [[nodiscard]] AttributeGains GetGainsForSection(std::string_view a_section);

    /// @brief The built-in 30-point default gains for a race section, or 10/10/10 for unknown sections.
    [[nodiscard]] AttributeGains DefaultGainsForSection(std::string_view a_section);

    /// @brief Set one attribute gain in memory. Takes effect on the next level up.
    /// @note Split from the write so a slider drag can apply live without rewriting the file per step.
    void SetGain(std::string_view a_section, RE::ActorValue a_actorValue, std::int32_t a_value);

    /// @brief Write the in-memory value for one attribute gain to the INI. Call when editing ends.
    void PersistGain(std::string_view a_section, RE::ActorValue a_actorValue);

    /// @brief Restore every race section's level-up gains to the 30-point standard and write them to the INI.
    void ResetGainsToDefaults();
}
