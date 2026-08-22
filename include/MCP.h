#pragma once

namespace MCP
{
    /// @brief Resolve the settings globals. Call on kDataLoaded.
    void LoadForms();

    /// @brief Register the panel section. No-op without SKSE Menu Framework.
    void Register();

    /// @brief Write the INI's [Settings] values into the globals.
    /// @note Call on kNewGame. The globals otherwise live in the save, so a new character would start on whatever is baked into the plugin rather than what the list author chose.
    void ApplyDefaults();

    /// @brief Whether racial drawbacks are switched on.
    /// @return true if the global is missing, so a missing record never disables them silently
    /// @note Record-side drawbacks read the same global through a GetGlobalValue condition.
    [[nodiscard]] bool RacialNegativesEnabled();

    /// @brief Health fraction at or below which Veloth's Pyre answers, from Apo_Setting_PyreHealth.
    /// @return 0.0 to 1.0, where 0 means it never triggers. Falls back to 0.30 if the record is absent.
    [[nodiscard]] float PyreHealthThreshold();

    /// @brief Fraction of a shout's remaining recovery Thu'um Rekindled removes, from Apo_Setting_ThuumCooldown.
    /// @return 0.0 to 0.5, where 0 disables the whole ability. Falls back to 0.33 if absent.
    [[nodiscard]] float ThuumCutFraction();

    /// @brief Fraction of max Health a melee hit returns at *full* Oath-Forged stacks, from Apo_Setting_Bloodthirst.
    /// @return 0.0 to 0.02, where 0 disables the ability. Falls back to 0.005 if absent.
    /// @note Divide by the stack cap for the per-stack share. Stored at full stacks because that is the figure a player judges the ability by, and it survives a change to the cap.
    [[nodiscard]] float BloodthirstFraction();
}
