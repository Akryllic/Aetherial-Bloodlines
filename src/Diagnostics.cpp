#include "PCH.h"
#include "Diagnostics.h"

#include <array>
#include <string>

#include "GreenPact.h"
#include "MCP.h"
#include "PanelUI.h"
#include "Racials.h"
#include "SKSEMCP/SKSEMenuFramework.hpp"

namespace
{
    // Power names are the ability records' FULL names, the perks carry none
    struct PerkRow {
        const char* race;
        const char* power;
        const char* editorID;
    };

    constexpr PerkRow PERK_ROWS[] = {
        { "Imperial", "Spear of the Emperor", "Apo_SpearoftheEmperorPerk" },
        { "Nord", "Ancestral Valor", "Apo_AncestralValorPerk" },
        { "Breton", "Wyrd Bound", "Apo_WyrdBoundPerk" },
        { "Redguard", "Ansei's Gambit", "Apo_AnseisGambitPerk" },
        { "Altmer", "Binding of Aetherius", "Apo_BindingofAetheriusPerk" },
        { "Dunmer", "Flame of Veloth", "Apo_FlameofVelothPerk" },
        { "Bosmer", "Green Pact Oath", "Apo_GreenPactOathPerk" },
        { "Orsimer", "Oath-Forged", "Apo_OathForgedPerk" },
        { "Khajiit", "Jone and Jode's Rhythm", "Apo_JoneJodeRhythmPerk" },
        { "Argonian", "Hist's Calling", "Apo_HistsCallingPerk" },
    };

    int g_orcOverride{ 0 };
    int g_bosmerOverride{ 0 };
    int g_hitLogCount{ 20 };

    void DrawPlayerState()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            ImGuiMCP::TextColored(PanelUI::BAD, "No player loaded yet.");
            return;
        }

        if (auto* race = player->GetRace()) {
            const auto* name = race->GetName();
            ImGuiMCP::Text("Playing as: %s", (name && *name) ? name : "unknown race");
        }

        // Optimization: It will only run once and cache the results for subsequent calls
        static const auto perks = [] {
            std::array<RE::BGSPerk*, std::size(PERK_ROWS)> out{};
            for (std::size_t i = 0; i < std::size(PERK_ROWS); ++i) {
                out[i] = RE::TESForm::LookupByEditorID<RE::BGSPerk>(PERK_ROWS[i].editorID);
            }
            return out;
        }();

        // Only what is actually on this character. Nine greyed out rows for races you are not was noise
        int active = 0;
        int missing = 0;
        for (std::size_t i = 0; i < std::size(PERK_ROWS); ++i) {
            if (!perks[i]) {
                ++missing;
                ImGuiMCP::TextColored(PanelUI::BAD, "  %s is missing from the load order", PERK_ROWS[i].editorID);
                continue;
            }
            if (player->HasPerk(perks[i])) {
                ++active;
                ImGuiMCP::TextColored(PanelUI::GOOD, "  %s", PERK_ROWS[i].power);
            }
        }

        if (active == 0 && missing == 0) {
            ImGuiMCP::TextDisabled("  No racial power active on this character.");
        }
    }

    void DrawCounters()
    {
        const auto snap = Racials::GetSnapshot();

        if (!snap.avgReady) {
            ImGuiMCP::TextColored(PanelUI::BAD, "Actor Value Generator missing, counters inert.");
            return;
        }

        ImGuiMCP::Text("Oath-Forged      %d of 10", snap.orcStacks);
        ImGuiMCP::Text("Green Pact Oath  %d of 6", snap.bosmerStacks);

        ImGuiMCP::SetNextItemWidth(180.0f);
        ImGuiMCP::SliderInt("Oath-Forged", &g_orcOverride, 0, 10); // Counter range for Osimer
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Apply##orc")) {
            Racials::DebugSetOrcStacks(g_orcOverride);
        }

        ImGuiMCP::SetNextItemWidth(180.0f);
        ImGuiMCP::SliderInt("Green Pact Oath", &g_bosmerOverride, 0, 6); // Counter range for Bosmer
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Apply##bosmer")) {
            Racials::DebugSetBosmerStacks(g_bosmerOverride);
        }
    }

    void DrawGreenPactInspector()
    {
        RE::TESObjectREFR* target = nullptr;
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            if (auto ref = pick->GetActiveTarget().get()) {
                target = ref.get();
            }
        }

        const auto text = GreenPact::DescribeTarget(target);
        ImGuiMCP::TextUnformatted(text.c_str());
    }

    // Raised at runtime so a bug can be chased without a rebuild; the release build starts at info
    void DrawLogging()
    {
        const auto snap = Racials::GetSnapshot();
        const auto level = spdlog::get_level();

        const auto levelButton = [&](const char* a_label, spdlog::level::level_enum a_level) {
            const bool on = level == a_level;
            if (on) {
                ImGuiMCP::TextColored(PanelUI::GOOD, "%s", a_label);
            } else if (ImGuiMCP::Button(a_label)) {
                spdlog::set_level(a_level);
                spdlog::flush_on(a_level);
                logger::info("Log level set to {}.", a_label);
            }
        };

        ImGuiMCP::Text("Level");
        ImGuiMCP::SameLine();
        levelButton("info", spdlog::level::info);
        ImGuiMCP::SameLine();
        levelButton("debug", spdlog::level::debug);
        ImGuiMCP::SameLine();
        levelButton("trace", spdlog::level::trace);

        ImGuiMCP::TextDisabled("debug logs every stack change, hit classification and combat exit.");

        ImGuiMCP::SeparatorText("Hit log");
        ImGuiMCP::SetNextItemWidth(180.0f);
        ImGuiMCP::SliderInt("Hits to log", &g_hitLogCount, 1, 100);
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(snap.hitLogRemaining > 0 ? "Stop##hitlog" : "Start##hitlog")) {
            Racials::StartHitLog(snap.hitLogRemaining > 0 ? 0 : g_hitLogCount);
        }
        if (snap.hitLogRemaining > 0) {
            ImGuiMCP::SameLine();
            ImGuiMCP::TextColored(PanelUI::GOOD, "%d to go", snap.hitLogRemaining);
        }
        ImGuiMCP::TextDisabled("Forces the next N hits you land to log at info, whatever the level above.");
    }

    void DrawVoice()
    {
        const auto snap = Racials::GetSnapshot();

        if (!snap.voiceBlockFound) {
            ImGuiMCP::TextColored(PanelUI::BAD, "No high process on the player yet.");
            return;
        }

        ImGuiMCP::Text("Recovery   %.2f", snap.voiceRecovery);
        ImGuiMCP::Text("Shout      %s (word %d)", snap.shoutName.c_str(), snap.shoutWord);
        ImGuiMCP::Text("State      %d", snap.voiceState);
        ImGuiMCP::Text("Elapsed    %.2f", snap.voiceElapsed);

        // Both clear about a second after the shout resolves while recovery keeps running
        ImGuiMCP::TextDisabled("Shout and word blank out well before recovery reaches zero.");
    }

    void __stdcall RenderDebug()
    {
        if (ImGuiMCP::CollapsingHeader("Character", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawPlayerState();
        }
        if (ImGuiMCP::CollapsingHeader("Counters")) {
            DrawCounters();
        }
        if (ImGuiMCP::CollapsingHeader("Crosshair")) {  // Used for Green Pact target inspection
            DrawGreenPactInspector();
        }
        if (ImGuiMCP::CollapsingHeader("Shouts")) {
            DrawVoice();
        }
        if (ImGuiMCP::CollapsingHeader("Logging")) {
            DrawLogging();
        }
    }
}

namespace Diagnostics
{
    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            return;
        }

        SKSEMenuFramework::SetSection("Aetherial Bloodlines");
        SKSEMenuFramework::AddSectionItem("Debug", RenderDebug);

        logger::info("Diagnostics: panel registered.");
    }
}
