#pragma once

/// Green Pact harvest ban, hooked on TESBoundObject::Activate (vfunc 0x37) so it sits beneath every activation path rather than just the first-person prompt.

namespace GreenPact
{
    enum class Mode : int
    {
        kBlock = 0,
        kPenaltyOnly = 1,
        kOff = 2,
    };

    /// @brief Read the [GreenPact] INI section. Safe to call before forms exist.
    void LoadSettings();

    /// @brief Resolve the form lists, perk, message and penalty spell. Call on kDataLoaded.
    void LoadForms();

    /// @brief Install the Activate hooks on TESFlora, TESObjectTREE, TESObjectACTI, TESFurniture.
    void Install();

    [[nodiscard]] Mode CurrentMode();
    void SetMode(Mode a_mode);

    /// @brief Show the oath-broken message, sharing the refusal cooldown.
    /// @note Called when a forbidden food is eaten, so both halves of the oath use one voice.
    void NotifyOathBroken();

    /// @brief Explain what the harvest rule would decide about a reference, and why.
    /// @param a_ref the reference to test, usually the crosshair target
    /// @return which list matched, the produce read, and the verdict
    [[nodiscard]] std::string DescribeTarget(RE::TESObjectREFR* a_ref);
}
