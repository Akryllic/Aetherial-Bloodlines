#pragma once

#include <atomic>

/// Racial behaviour: the Orsimer and Bosmer stacking counters, the hit-driven racials, the equip-driven ones.
/// The counters are mirrored into their GlobalVariables because the perk entry-point conditions read those globals

namespace Racials
{
    /// @brief Resolve every form the module needs. Call on kDataLoaded.
    void LoadForms();

    /// @brief Resolve the Actor Value Generator extra values that hold the stacks.
    /// @note Their ids are per save, so call this on every load and new game, not just once.
    void ResolveExtraValues();

    /// @brief Clear the per-session state and re-attach the dodge sink. Call on kPostLoadGame and kNewGame.
    /// @note None of this state belongs to a character, and the animation graph the sink was on is gone.
    void OnGameLoaded();

    /// @brief Register the combat, hit and equip sinks.
    void Register();

    /// @brief Current state, for the My Bloodline and Diagnostics pages.
    struct Snapshot {
        bool ready{ false };
        bool avgReady{ false };
        std::int32_t hitLogRemaining{ 0 };
        std::int32_t animTagLogRemaining{ 0 };

        std::int32_t orcStacks{ 0 };
        std::int32_t bosmerStacks{ 0 };
        std::int32_t shehaiCount{ 0 };
        std::int32_t pyreTier{ 0 };
        bool pyreUsedThisFight{ false };
        bool thuumCutThisRecovery{ false };

        bool voiceBlockFound{ false };
        std::int32_t voiceState{ 0 };
        std::int32_t shoutWord{ -1 };
        float voiceElapsed{ 0.0f };
        float voiceRecovery{ 0.0f };
        float shoutWordRecovery{ 0.0f };
        std::string shoutName;
    };

    [[nodiscard]] Snapshot GetSnapshot();

    /// @brief Is the Shehai lit on the player right now?
    /// @note Walks the player's active effects, so call it from the main thread.
    [[nodiscard]] bool ShehaiActive();

    /// @brief Is Precision installed and driving Bloodthirst's trickle?
    /// @note When true the trickle is a share of the blow's damage; when false, of max Health.
    [[nodiscard]] bool PrecisionDriving();

    /// @brief Share of a blow's damage returned at ten stacks, 0.0 to 1.0. Only meaningful while PrecisionDriving() is true.
    [[nodiscard]] float BloodthirstDamageShare();

    /// @brief Shehai's timing. Owned by the DLL and stored in the INI rather than a GlobalVariable, because no record condition reads it, so a GLOB would cost a CK edit and buy nothing.
    /// @note Atomic because the menu's sliders write it from the menu's thread while hits read it on the game's.
    struct ShehaiTuning
    {
        std::atomic<std::int32_t> hits{ 3 };
        std::atomic<float> window{ 4.0f };
        std::atomic<float> gap{ 0.25f };
    };

    /// @brief The live tuning, mutable so the menu can edit it in place. Loaded from the INI.
    [[nodiscard]] ShehaiTuning& Shehai();

    /// @brief Re-read the Shehai tuning from the INI. Call after Settings::Load.
    void LoadShehaiTuning();

    /// @brief Force a counter, to test the perk tiers without farming a fight.
    void DebugSetOrcStacks(std::int32_t a_value);
    void DebugSetBosmerStacks(std::int32_t a_value);

    /// @brief Log the next @p a_hits hits the player lands, and any counter cleared meanwhile.
    void StartHitLog(std::int32_t a_hits);

    /// @brief Log the next @p a_tags animation tags containing "dodge", from any graph we are sinked to.
    /// @note Matches on substring rather than TKDR_DodgeStart, because the tag that should arrive never has.
    void StartAnimTagLog(std::int32_t a_tags);

}
