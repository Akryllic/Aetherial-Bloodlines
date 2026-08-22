#include "PCH.h"
#include "MCP.h"

#include <algorithm>

#include "PanelUI.h"
#include "SKSEMCP/SKSEMenuFramework.hpp"
#include "Racials.h"
#include "Settings.h"

namespace
{
    constexpr std::string_view AB_PLUGIN = "Aetherial Bloodlines.esp";

    // Needs powerofthree's Tweaks
    constexpr std::string_view GLOBAL_RACIAL_NEGATIVES = "Apo_Setting_RacialNegatives";
    constexpr std::string_view GLOBAL_THUUM_MELEE = "Apo_Setting_ThuumMelee";
    constexpr std::string_view GLOBAL_THUUM_POWER = "Apo_Setting_ThuumPower";

    // Health percentage at or below which Veloth's Pyre activates
    constexpr float PYRE_HEALTH_FALLBACK = 30.0f;
    constexpr int PYRE_HEALTH_MAX = 30;

    // Percentage of the remaining recovery a kill removes
    constexpr float THUUM_CUT_FALLBACK = 33.0f;
    constexpr int THUUM_CUT_MAX = 50;

    // Tenths, not whole points: at about five hits in two seconds, 3% of max health returned a sixth of the pool per swing
    constexpr float BLOODTHIRST_FALLBACK = 1.5f;
    constexpr float BLOODTHIRST_MAX = 5.0f;

    // AlwaysClamp, because ctrl+click turns a slider into an unchecked text field
    constexpr auto CLAMPED = ImGuiMCP::ImGuiSliderFlags_AlwaysClamp;

    // Only the globals a record condition reads are still globals; a GLOB lives in the save, so an author's edit would never reach an existing character
    RE::TESGlobal* g_racialNegatives{ nullptr };
    RE::TESGlobal* g_thuumMelee{ nullptr };
    RE::TESGlobal* g_thuumPower{ nullptr };

    float g_pyreHealth{ PYRE_HEALTH_FALLBACK };
    float g_thuumCooldown{ THUUM_CUT_FALLBACK };
    float g_bloodthirst{ BLOODTHIRST_FALLBACK };

    // TESGlobal::value is a float whatever the declared type
    [[nodiscard]] bool GlobalIsOn(const RE::TESGlobal* a_global)
    {
        return a_global && a_global->value != 0.0f;
    }

    // The global is the live value, the INI is what the next new game starts from, so a change has to reach both
    void Apply(RE::TESGlobal* a_global, float a_value)
    {
        if (a_global) {
            a_global->value = a_value;
        }
    }

    void Commit(RE::TESGlobal* a_global, std::string_view a_name, std::string_view a_iniKey, float a_value)
    {
        Apply(a_global, a_value);
        logger::info("MCP: {} = {}", a_name, a_value);
        Settings::SaveSetting(a_iniKey, a_value);
    }

    // Applies every step so the game responds under the cursor, writes once when the drag ends
    void SliderSetting(const char* a_label, RE::TESGlobal* a_global, std::string_view a_name,
                       std::string_view a_iniKey, float a_fallback, int a_max, const char* a_format)
    {
        int value = std::clamp(static_cast<int>(a_global ? a_global->value : a_fallback), 0, a_max);

        ImGuiMCP::SetNextItemWidth(180.0f);
        if (ImGuiMCP::SliderInt(a_label, &value, 0, a_max, a_format, CLAMPED)) {
            Apply(a_global, static_cast<float>(value));
        }
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
            logger::info("MCP: {} = {}", a_name, value);
            Settings::SaveSetting(a_iniKey, static_cast<float>(value));
        }
    }

    // Same split as SliderSetting. ImGui rounds to the format's precision, so "%.1f" is what makes the step a tenth
    void SliderSettingFloat(const char* a_label, RE::TESGlobal* a_global, std::string_view a_name,
                            std::string_view a_iniKey, float a_fallback, float a_max, const char* a_format)
    {
        float value = std::clamp(a_global ? a_global->value : a_fallback, 0.0f, a_max);

        ImGuiMCP::SetNextItemWidth(180.0f);
        if (ImGuiMCP::SliderFloat(a_label, &value, 0.0f, a_max, a_format, CLAMPED)) {
            Apply(a_global, value);
        }
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
            logger::info("MCP: {} = {}", a_name, value);
            Settings::SaveSetting(a_iniKey, value);
        }
    }

    // For values the DLL owns outright, with no GLOB behind them because no record condition reads these
    void SliderSettingLocal(const char* a_label, float* a_value, std::string_view a_iniKey, float a_min,
                            float a_max, const char* a_format)
    {
        float value = std::clamp(*a_value, a_min, a_max);

        ImGuiMCP::SetNextItemWidth(180.0f);
        if (ImGuiMCP::SliderFloat(a_label, &value, a_min, a_max, a_format, CLAMPED)) {
            *a_value = value;
        }
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
            logger::info("MCP: {} = {}", a_iniKey, value);
            Settings::SaveSetting(a_iniKey, value);
        }
    }

    // Mod Attack Damage already covers power attacks, so ticking one clears the other
    void ExclusiveCheckbox(const char* a_label, RE::TESGlobal* a_global, std::string_view a_name,
                           std::string_view a_iniKey, RE::TESGlobal* a_other, std::string_view a_otherName,
                           std::string_view a_otherIniKey)
    {
        bool on = GlobalIsOn(a_global);
        if (!ImGuiMCP::Checkbox(a_label, &on)) {
            return;
        }

        Commit(a_global, a_name, a_iniKey, on ? 1.0f : 0.0f);
        if (on && GlobalIsOn(a_other)) {
            Commit(a_other, a_otherName, a_otherIniKey, 0.0f);
        }
    }

    // "NordRace" reads as "Nord" in a list of races
    [[nodiscard]] std::string GainSectionLabel(const std::string& a_section)
    {
        constexpr std::string_view suffix = "Race";
        if (a_section.size() > suffix.size() && a_section.ends_with(suffix)) {
            return a_section.substr(0, a_section.size() - suffix.size());
        }
        return a_section;
    }

    void GainSlider(const std::string& a_section, const char* a_label, RE::ActorValue a_actorValue,
                    std::int32_t a_current)
    {
        int value = std::clamp(static_cast<int>(a_current), 0, Settings::MAX_GAIN);
        ImGuiMCP::SetNextItemWidth(-1.0f);
        if (ImGuiMCP::SliderInt(a_label, &value, 0, Settings::MAX_GAIN, "%d", CLAMPED)) {
            Settings::SetGain(a_section, a_actorValue, value);
        }
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
            Settings::PersistGain(a_section, a_actorValue);
        }
    }

    void __stdcall RenderLevelUp()
    {
        const auto& sections = Settings::GainSections();
        if (sections.empty()) {
            ImGuiMCP::TextColored(PanelUI::BAD, "No attribute sections found in AetherialBloodlines.ini.");
            return;
        }

        ImGuiMCP::TextDisabled("Points gained when you pick that attribute on levelling up.");
        ImGuiMCP::TextDisabled("Default covers any race the INI does not name, including custom ones.");
        ImGuiMCP::Spacing();

        // StretchProp sizes by content width, so a slider asking for the rest of the row makes that circular and swallows the panel
        if (!ImGuiMCP::BeginTable("levelup", 4, ImGuiMCP::ImGuiTableFlags_SizingStretchSame)) {
            return;
        }

        ImGuiMCP::TableSetupColumn("Race", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGuiMCP::TableSetupColumn("Health", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGuiMCP::TableSetupColumn("Magicka", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGuiMCP::TableSetupColumn("Stamina", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGuiMCP::TableHeadersRow();

        for (const auto& section : sections) {
            const auto gains = Settings::GetGainsForSection(section);

            // Every row uses the same three labels, so they need scoping or ImGui merges them
            ImGuiMCP::PushID(section.c_str());
            ImGuiMCP::TableNextRow();

            ImGuiMCP::TableNextColumn();
            ImGuiMCP::Text("%s", GainSectionLabel(section).c_str());

            ImGuiMCP::TableNextColumn();
            GainSlider(section, "##health", RE::ActorValue::kHealth, gains.health);

            ImGuiMCP::TableNextColumn();
            GainSlider(section, "##magicka", RE::ActorValue::kMagicka, gains.magicka);

            ImGuiMCP::TableNextColumn();
            GainSlider(section, "##stamina", RE::ActorValue::kStamina, gains.stamina);

            ImGuiMCP::PopID();
        }

        ImGuiMCP::EndTable();

        ImGuiMCP::Spacing();
        ImGuiMCP::TextDisabled("Saved to the INI as you change them, and applied on your next level up.");
    }

    // RenderFunction is void(__stdcall*)() (SKSEMenuFramework.hpp:44), inert on x64
    void __stdcall RenderSettings()
    {
        if (!g_racialNegatives) {
            ImGuiMCP::TextColored(PanelUI::BAD, "Apo_Setting_RacialNegatives not found.");
            return;
        }

        // Read live, the global already lives in the save so there is nothing to persist
        bool enabled = GlobalIsOn(g_racialNegatives);

        if (ImGuiMCP::Checkbox("Racial Drawbacks", &enabled)) {
            Commit(g_racialNegatives, GLOBAL_RACIAL_NEGATIVES, "RacialDrawbacks", enabled ? 1.0f : 0.0f);
        }
        PanelUI::HelpMarker("Every race trades a strength for a weakness: elemental weaknesses, weaker healing,\n"
                            "heavier armour, the Green Pact ban on harvesting, and so on.\n\n"
                            "Toggle this to keep every racial strength and disable the negatives.\n"
                            "Takes effect immediately.");

        // The abilities are the bargain the drawbacks pay for, so they go too. Greyed rather than hidden, so the dependency is visible
        if (!enabled) {
            ImGuiMCP::Spacing();
            ImGuiMCP::TextDisabled("Racial abilities below are unavailable while drawbacks are off.");
        }
        ImGuiMCP::BeginDisabled(!enabled);

        ImGuiMCP::SeparatorText("Dunmer");

        // Read live off the global, which already lives in the save, so there is nothing to persist
        SliderSettingLocal("Veloth's Pyre", &g_pyreHealth, "PyreHealth", 0.0f,
                           static_cast<float>(PYRE_HEALTH_MAX), "%.0f%% health");
        PanelUI::HelpMarker("Health a Dunmer must drop to before Veloth's Pyre fires, once per fight.\n\n"
                            "Set to 0 to disable");

        ImGuiMCP::SeparatorText("Nord");

        SliderSettingLocal("Thu'um Rekindled", &g_thuumCooldown, "ThuumCooldown", 0.0f,
                           static_cast<float>(THUUM_CUT_MAX), "%.0f%% off");
        PanelUI::HelpMarker("Share of the remaining shout cooldown a Nord's kill removes, once per shout.\n\n"
                            "Set to 0 to disable");

        // Both read by perk entry conditions, so the DLL holds no part of the damage rule
        ExclusiveCheckbox("Melee & Power Attacks", g_thuumMelee, GLOBAL_THUUM_MELEE, "ThuumMelee",
                          g_thuumPower, GLOBAL_THUUM_POWER, "ThuumPower");
        PanelUI::HelpMarker("Enables the bonus for both melee and power attacks.");

        ExclusiveCheckbox("Power Attacks Only", g_thuumPower, GLOBAL_THUUM_POWER, "ThuumPower",
                          g_thuumMelee, GLOBAL_THUUM_MELEE, "ThuumMelee");
        PanelUI::HelpMarker("Enables the bonus for power attacks only.\n\n"
                            "Turning one of these on turns the other off, since the first already\n"
                            "covers power attacks and the two would otherwise multiply.");

        if (!g_thuumMelee || !g_thuumPower) {
            ImGuiMCP::TextColored(PanelUI::BAD, "  Apo_Setting_ThuumMelee/ThuumPower not found.");
        }

        ImGuiMCP::SeparatorText("Orsimer");

        SliderSettingLocal("Bloodthirst", &g_bloodthirst, "Bloodthirst", 0.0f, BLOODTHIRST_MAX,
                           "%.1f%% a hit");
        PanelUI::HelpMarker("Health an Orsimer's melee hit returns at ten Oath-Forged stacks. One stack\n"
                            "returns a tenth of it.\n\n"
                            "With Precision installed this is a share of the damage the blow dealt, so a\n"
                            "heavy weapon returns more than a light one. Without it, a share of your max\n"
                            "health, the same whatever you swing.\n\n"
                            "The two scale differently, so the same number is stronger without Precision\n"
                            "than with it. Tune by feel rather than by the figure.\n\n"
                            "Set to 0 to disable");

        // Which driver is live changes what the slider above means, so state it rather than imply it
        if (Racials::PrecisionDriving()) {
            ImGuiMCP::TextDisabled("  Precision found: returns %.0f%% of a blow's damage at ten stacks.",
                                   Racials::BloodthirstDamageShare() * 100.0f);
        } else {
            ImGuiMCP::TextDisabled("  Precision not found: returns a share of max health instead.");
        }

        ImGuiMCP::SeparatorText("Redguard");

        auto& shehai = Racials::Shehai();
        float hits = static_cast<float>(shehai.hits);
        SliderSettingLocal("Shehai blows", &hits, "ShehaiHits", 2.0f, 6.0f, "%.0f");
        shehai.hits = static_cast<std::int32_t>(hits);
        PanelUI::HelpMarker("Blows on one target, while dual wielding, that summon the Shehai.");

        SliderSettingLocal("Shehai window", &shehai.window, "ShehaiWindow", 1.0f, 10.0f, "%.1f s");
        PanelUI::HelpMarker("How long you have to land them. The count resets if you take longer, or\n"
                            "switch target.");

        SliderSettingLocal("Shehai blow gap", &shehai.gap, "ShehaiGap", 0.0f, 1.0f, "%.2f s");
        PanelUI::HelpMarker("Shortest gap between two blows that both count.\n\n"
                            "Dual wielding lands each weapon as a separate hit a millisecond apart, so\n"
                            "without this a single swing counts twice and the combo is half as long as\n"
                            "it reads. Raise it if the Shehai fires too easily, lower it if a genuine\n"
                            "flurry is being ignored.");

        ImGuiMCP::EndDisabled();
    }
}

namespace MCP
{
    bool RacialNegativesEnabled()
    {
        // Default on when the record is missing, so it never silently disarms the mod
        return g_racialNegatives ? GlobalIsOn(g_racialNegatives) : true;
    }

    float PyreHealthThreshold()
    {
        // Stored as a percentage so the slider and the console read naturally, used as a fraction
        return std::clamp(g_pyreHealth, 0.0f, 100.0f) * 0.01f;
    }

    float ThuumCutFraction()
    {
        return std::clamp(g_thuumCooldown, 0.0f, static_cast<float>(THUUM_CUT_MAX)) * 0.01f;
    }

    float BloodthirstFraction()
    {
        return std::clamp(g_bloodthirst, 0.0f, BLOODTHIRST_MAX) * 0.01f;
    }

    void LoadForms()
    {
        // Straight from the INI on every load, so an author's edit reaches an existing character
        g_pyreHealth = Settings::GetSetting("PyreHealth", PYRE_HEALTH_FALLBACK);
        g_thuumCooldown = Settings::GetSetting("ThuumCooldown", THUUM_CUT_FALLBACK);
        g_bloodthirst = Settings::GetSetting("Bloodthirst", BLOODTHIRST_FALLBACK);
        logger::info("MCP: from the INI, PyreHealth {}, ThuumCooldown {}, Bloodthirst {}.", g_pyreHealth,
                     g_thuumCooldown, g_bloodthirst);

        g_thuumMelee = RE::TESForm::LookupByEditorID<RE::TESGlobal>(GLOBAL_THUUM_MELEE);
        g_thuumPower = RE::TESForm::LookupByEditorID<RE::TESGlobal>(GLOBAL_THUUM_POWER);
        if (!g_thuumMelee || !g_thuumPower) {
            logger::warn("MCP: {} / {} not found, the Thu'um damage toggles will not work.",
                         GLOBAL_THUUM_MELEE, GLOBAL_THUUM_POWER);
        }

        g_racialNegatives = RE::TESForm::LookupByEditorID<RE::TESGlobal>(GLOBAL_RACIAL_NEGATIVES);

        if (g_racialNegatives) {
            logger::info("MCP: resolved {} -> {:08X}, current value {}", GLOBAL_RACIAL_NEGATIVES,
                         g_racialNegatives->GetFormID(), g_racialNegatives->value);
        } else {
            logger::warn(
                "MCP: {} not found in {}. Run the xEdit toggle script, and check that "
                "powerofthree's Tweaks is installed.",
                GLOBAL_RACIAL_NEGATIVES, AB_PLUGIN);
        }
    }

    void ApplyDefaults()
    {
        struct Entry {
            RE::TESGlobal* global;
            std::string_view editorID;
            std::string_view iniKey;
            float fallback;
        };

        const Entry entries[]{
            // Only the save-resident globals need seeding; the rest are read from the INI on every load
            { g_racialNegatives, GLOBAL_RACIAL_NEGATIVES, "RacialDrawbacks", 1.0f },
            { g_thuumMelee, GLOBAL_THUUM_MELEE, "ThuumMelee", 1.0f },
            { g_thuumPower, GLOBAL_THUUM_POWER, "ThuumPower", 0.0f },
        };

        for (const auto& entry : entries) {
            if (!entry.global) {
                continue;
            }

            // Falling back to the record's own value keeps a partial INI from resetting anything
            const float wanted = Settings::GetSetting(entry.iniKey, entry.global->value);
            if (entry.global->value == wanted) {
                continue;
            }
            logger::info("MCP: {} {} -> {} from the INI", entry.editorID, entry.global->value, wanted);
            entry.global->value = wanted;
        }
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::info("MCP: SKSE Menu Framework not installed, panel skipped.");
            return;
        }

        SKSEMenuFramework::SetSection("Aetherial Bloodlines");
        SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
        SKSEMenuFramework::AddSectionItem("Level Up", RenderLevelUp);

        logger::info("MCP: registered SKSE Menu Framework section.");
    }
}
