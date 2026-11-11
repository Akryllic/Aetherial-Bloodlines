#include "PCH.h"
#include "Menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <FUCK_API.h>

#include "GreenPact.h"
#include "Racials.h"
#include "Settings.h"

namespace
{
    constexpr std::string_view AB_PLUGIN = "Aetherial Bloodlines.esp";

    constexpr std::string_view GLOBAL_RACIAL_NEGATIVES = "Apo_Setting_RacialNegatives";
    constexpr std::string_view GLOBAL_DRAWBACK_MASTERY = "Apo_Setting_DrawbackMastery";
    constexpr std::string_view GLOBAL_THUUM_MELEE = "Apo_Setting_ThuumMelee";
    constexpr std::string_view GLOBAL_THUUM_POWER = "Apo_Setting_ThuumPower";

    constexpr float PYRE_HEALTH_FALLBACK = 30.0f;
    constexpr int PYRE_HEALTH_MAX = 30;

    constexpr float THUUM_CUT_FALLBACK = 33.0f;
    constexpr int THUUM_CUT_MAX = 50;

    // Percent of max health a hit returns at ten stacks, kept small since a fast fighter lands a hit every half second or so
    // With Precision the same number is scaled up into a share of the blow's damage
    constexpr float BLOODTHIRST_FALLBACK = 1.5f;
    constexpr float BLOODTHIRST_MAX = 5.0f;

    constexpr int SHEHAI_HITS_FALLBACK = 3;
    constexpr float SHEHAI_WINDOW_FALLBACK = 4.0f;
    constexpr float SHEHAI_GAP_FALLBACK = 0.25f;

    // Reset to Defaults puts back what the shipped INI holds, which is not always the fallback above
    constexpr float PYRE_HEALTH_SHIPPED = 20.0f;
    constexpr float THUUM_MELEE_SHIPPED = 0.0f;
    constexpr float THUUM_POWER_SHIPPED = 1.0f;

    constexpr ImVec4 GOOD{ 0.35f, 0.78f, 0.35f, 1.0f };
    constexpr ImVec4 WARN{ 0.90f, 0.72f, 0.22f, 1.0f };
    constexpr ImVec4 BAD{ 0.85f, 0.25f, 0.25f, 1.0f };

    // Only settings a record condition reads stay as globals, a global is saved with the game so an INI edit would never reach an existing character
    RE::TESGlobal* g_racialNegatives{ nullptr };
    RE::TESGlobal* g_drawbackMasteryGlobal{ nullptr };
    RE::TESGlobal* g_thuumMelee{ nullptr };
    RE::TESGlobal* g_thuumPower{ nullptr };

    // FLICK draws on its own thread, so anything a page writes and the game reads is atomic
    std::atomic<bool> g_drawbackMastery{ true };
    std::atomic<float> g_pyreHealth{ PYRE_HEALTH_FALLBACK };
    std::atomic<float> g_thuumCooldown{ THUUM_CUT_FALLBACK };
    std::atomic<float> g_bloodthirst{ BLOODTHIRST_FALLBACK };

    int g_orcOverride{ 0 };
    int g_bosmerOverride{ 0 };
    int g_hitLogCount{ 20 };
    int g_animTagLogCount{ 20 };

    struct PerkRow
    {
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

    std::array<RE::BGSPerk*, std::size(PERK_ROWS)> g_racialPerks{};

    // One entry per racial drawback, with the skills and perks that halve it and then remove it.
    struct SkillGate
    {
        RE::ActorValue skill;
        float level;
    };

    // Any one skill gate or perk is enough
    struct Exit
    {
        std::vector<SkillGate> skills;
        std::vector<const char*> perks;
    };

    enum class Accommodation
    {
        kNone,
        kSiphon,       // an Alteration armour spell or a Ward halves it while it holds
        kBoundWeapon,  // a bound weapon in either hand lifts it
    };

    struct DrawbackSpec
    {
        const char* text;
        Exit halve;
        Exit overcome;
        Accommodation accommodation{ Accommodation::kNone };

        // Halves only while the worn cuirass is one of the segmented or lined materials
        const char* materialPerk{ nullptr };

        // A bypass the card cannot test yet, shown as plain text
        const char* note{ nullptr };

        bool greenPact{ false };
    };

    struct RaceSpec
    {
        std::string_view raceEditorID;
        std::vector<DrawbackSpec> drawbacks;
    };

    using AV = RE::ActorValue;

    const std::vector<RaceSpec> RACE_SPECS = {
        { "OrcRace",
            {
                { .text = "Healing spells are 50% less effective, and non-combat spells cost 15% more Magicka.",
                    .halve = { { { AV::kRestoration, 50.0f } }, { "Respite", "PoE_PERK_RES_Paladin" } },
                    .overcome = { { { AV::kRestoration, 75.0f } }, { "PoE_PERK_RES_Urgency" } } },
            } },
        { "RedguardRace",
            {
                { .text = "In Heavy Armour, spells deal 20% more damage to you.",
                    .halve = { {}, { "IWP_PERK_HARM_ReflectiveCarapace" } },
                    .overcome = { {}, { "Conditioning" } } },
                { .text = "Conjuration spells cost 25% more Magicka.",
                    .overcome = { {}, { "PoE_PERK_CON_StudentConjuration2" } } },
                { .text = "Illusion spells cost 25% more Magicka.",
                    .overcome = { {}, { "PoE_PERK_ILL_StudentIllusion2" } } },
            } },
        { "WoodElfRace",
            {
                { .text = "Melee hits on unmarked enemies within reach deal 15% less damage, or 20% less with Two-Handed.",
                    .halve = { { { AV::kOneHanded, 50.0f }, { AV::kTwoHanded, 50.0f } }, {} },
                    .overcome = { { { AV::kOneHanded, 75.0f }, { AV::kTwoHanded, 75.0f } }, {} },
                    .note = "No penalty against a target marked by your arrows." },
                { .text = "The Green Pact: no harvesting or woodcutting, and eating forbidden food cancels food benefits for 90 minutes.",
                    .greenPact = true },
            } },
        { "KhajiitRace",
            {
                { .text = "Heavy Armour gives 35% less armour, and a heavy cuirass slows Stamina regeneration by 25%.",
                    .halve = { {}, { "IWP_PERK_HARM_Trainee2" } },
                    .overcome = { {}, { "Conditioning" } },
                    .materialPerk = "IWP_PERK_HARM_HeavyMaterialMastery" },
                { .text = "Unblocked enemy power attacks deal 15% more damage to you.",
                    .halve = { { { AV::kLightArmor, 50.0f }, { AV::kHeavyArmor, 50.0f } }, {} },
                    .overcome = { {}, { "PoE_PERK_LARM_QuickReflexes", "PoE_PERK_LARM_Evasive", "IWP_PERK_HARM_Bulwark" } } },
            } },
        { "HighElfRace",
            {
                { .text = "Magic Resistance is 25% lower, and below 25% Magicka you take 20% more physical damage.",
                    .overcome = { {}, { "PoE_PERK_ALT_MagicResistance", "PoE_PERK_ALT_AetherialShield" } },
                    .accommodation = Accommodation::kSiphon },
            } },
        { "BretonRace",
            {
                { .text = "Physical armour gives 20% less armour.",
                    .halve = { { { AV::kLightArmor, 50.0f }, { AV::kHeavyArmor, 50.0f } }, {} },
                    .overcome = { {}, { "PoE_PERK_LARM_LightMatchingSet2", "IWP_PERK_HARM_HeavyMatchingSet2" } },
                    .note = "No penalty in robes once you have Mage Armor (Alteration 30)." },
                { .text = "Below 50% Magicka, your weapons deal 15% less physical damage.",
                    .overcome = { {}, { "PoE_PERK_1H_WeavingDance", "PoE_PERK_BLOCK_Conduit" } },
                    .accommodation = Accommodation::kBoundWeapon },
            } },
        { "DarkElfRace",
            {
                { .text = "Frost and Shock spells are 30% weaker, and Frost Resistance is 15% lower.",
                    .halve = { {}, { "PoE_PERK_DES_Elementalist", "AugmentedFrost60", "AugmentedShock60" } },
                    .overcome = { {}, { "PoE_PERK_DES_ForceOfNature" } } },
            } },
        { "NordRace",
            {
                { .text = "Above 85% Health, you take 20% more physical damage.",
                    .overcome = { {}, { "IWP_PERK_HARM_Bulwark", "PoE_PERK_LARM_BattleRush1" } },
                    .note = "Shaken off at 3 stacks of Sovngarde's Mettle." },
                { .text = "Health regeneration pauses above 70% Health.",
                    .overcome = { {}, { "Conditioning", "PoE_PERK_LARM_LightMatchingSet2" } },
                    .note = "Lifted in combat at 3 stacks of Sovngarde's Mettle." },
                { .text = "Magic Resistance is 15% lower.",
                    .overcome = { {}, { "IWP_PERK_HARM_ReflectiveCarapace", "PoE_PERK_LARM_ElusiveZephyr", "ISP_PERK_SPEECH_Dovahziil" } } },
            } },
        { "ArgonianRace",
            {
                { .text = "You take 25% more Frost damage.",
                    .halve = { {}, { "PoE_PERK_LARM_LightMaterialMastery", "IWP_PERK_HARM_HeavyMaterialMastery", "PoE_PERK_RES_AuroraShield" } },
                    .overcome = { {}, { "PoE_PERK_LARM_LightMatchingSet2", "Conditioning" } } },
                { .text = "Stamina potions are 30% weaker.",
                    .overcome = { {}, { "ISP_PERK_ALCH_LongLasting" } } },
            } },
        { "ImperialRace",
            {
                { .text = "Above 50% Health or Stamina, Health and Stamina potions are 40% weaker.",
                    .overcome = { {}, { "ISP_PERK_ALCH_WhiteWitchery", "ISP_PERK_ALCH_LongLasting" } } },
                { .text = "Sneak attacks deal 25% less damage.",
                    .overcome = { { { AV::kSneak, 50.0f } }, {} } },
                { .text = "Bows, not crossbows, draw 20% slower.",
                    .overcome = { { { AV::kArchery, 50.0f } }, { "QuickShot" } } },
            } },
    };

    // Bonemold, Chitin and Nordic heavy count as segmented or lined, and so does Orcish
    constexpr std::array SEGMENTED_MATERIALS{ "DLC2ArmorMaterialBonemoldHeavy", "DLC2ArmorMaterialChitinHeavy",
        "DLC2ArmorMaterialNordicHeavy", "ArmorMaterialOrcish" };

    struct CardForms
    {
        std::unordered_map<std::string_view, RE::BGSPerk*> perks;
        std::vector<RE::BGSKeyword*> segmented;
        RE::BGSKeyword* magicArmorSpell{ nullptr };
        RE::BGSKeyword* magicWard{ nullptr };
    };

    CardForms g_cardForms;

    // What the card shows, built on the main thread and copied out by Draw
    struct CardLine
    {
        std::string text;
        std::string halve;
        std::string overcome;
        std::string note;
        std::string status;
        ImVec4 color{ BAD };
    };

    struct Card
    {
        bool hasPlayer{ false };
        std::string race;
        bool knownRace{ false };
        std::vector<std::string> activePerks;
        std::vector<std::string> missingPerks;
        Racials::Snapshot snap;
        bool shehaiActive{ false };
        bool drawbacksOn{ true };
        bool masteryOn{ true };
        std::vector<CardLine> lines;
        std::string crosshair;
        bool precisionDriving{ false };
        float bloodthirstDamageShare{ 0.0f };
    };

    // FLICK draws on its own thread, so the pages read the game only through this copy, which a game task builds
    std::mutex g_cardLock;
    std::shared_ptr<const Card> g_card;
    std::atomic<bool> g_cardQueued{ false };
    std::atomic<std::uint32_t> g_drawThread{ 0 };
    bool g_threadsLogged{ false };

    constexpr auto CARD_REFRESH = std::chrono::milliseconds(250);

    // TESGlobal::value is a float whatever the declared type
    [[nodiscard]] bool GlobalIsOn(const RE::TESGlobal* a_global)
    {
        return a_global && a_global->value != 0.0f;
    }

    // Anything that changes the game goes through a game task, never straight from Draw
    void RunOnGame(std::function<void()> a_fn)
    {
        if (const auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(std::move(a_fn));
        }
    }

    // A switch kept in a global.
    // The game owns the global, so a page shows what the last card task read.
    // While a change is still queued the card task leaves the shown value alone, so the box never flicks back.
    struct SwitchView
    {
        std::atomic<float> shown{ 0.0f };
        int pending{ 0 };
    };

    SwitchView g_negativesView;
    SwitchView g_masteryView;
    SwitchView g_meleeView;
    SwitchView g_powerView;

    // Held for the check and the store together, or a click landing between them would be overwritten with the old value
    std::mutex g_switchLock;

    void Refresh(SwitchView& a_view, float a_value)
    {
        std::scoped_lock lock(g_switchLock);
        if (a_view.pending == 0) {
            a_view.shown.store(a_value);
        }
    }

    // The global is the live value, the INI is what the next new game starts from, so a change has to reach both
    void Post(RE::TESGlobal* a_global, SwitchView& a_view, std::string_view a_name, std::string_view a_iniKey, float a_value)
    {
        const auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            return;
        }
        {
            std::scoped_lock lock(g_switchLock);
            a_view.shown.store(a_value);
            ++a_view.pending;
        }
        tasks->AddTask([a_global, &a_view, a_name, a_iniKey, a_value]() {
            if (a_global) {
                a_global->value = a_value;
            }
            logger::info("Menu: {} = {}", a_name, a_value);
            Settings::SaveSetting(a_iniKey, a_value);
            std::scoped_lock lock(g_switchLock);
            --a_view.pending;
        });
    }

    // Applies the value while you drag so you see it change, and only writes the INI once you let go
    void SliderSettingLocal(const char* a_label, std::atomic<float>& a_value, std::string_view a_iniKey, float a_min,
                            float a_max, const char* a_format, const char* a_tooltip)
    {
        float value = std::clamp(a_value.load(), a_min, a_max);
        if (FUCK::SliderFloat(a_label, &value, a_min, a_max, a_format)) {
            a_value.store(std::clamp(value, a_min, a_max));
        }
        if (a_tooltip && *a_tooltip) {
            FUCK::SetTooltip(a_tooltip);
        }
        if (FUCK::IsItemDeactivatedAfterEdit()) {
            const float saved = a_value.load();
            logger::info("Menu: {} = {}", a_iniKey, saved);
            Settings::SaveSetting(a_iniKey, saved);
        }
    }

    void SliderSettingInt(const char* a_label, std::atomic<std::int32_t>& a_value, std::string_view a_iniKey, int a_min,
                          int a_max, const char* a_tooltip)
    {
        int value = std::clamp(static_cast<int>(a_value.load()), a_min, a_max);
        if (FUCK::SliderInt(a_label, &value, a_min, a_max, "%d")) {
            a_value.store(std::clamp(value, a_min, a_max));
        }
        if (a_tooltip && *a_tooltip) {
            FUCK::SetTooltip(a_tooltip);
        }
        if (FUCK::IsItemDeactivatedAfterEdit()) {
            const auto saved = a_value.load();
            logger::info("Menu: {} = {}", a_iniKey, saved);
            Settings::SaveSetting(a_iniKey, static_cast<float>(saved));
        }
    }

    // Mod Attack Damage already covers power attacks, so ticking one clears the other
    void ExclusiveCheckbox(const char* a_label, RE::TESGlobal* a_global, SwitchView& a_view, std::string_view a_name,
                           std::string_view a_iniKey, RE::TESGlobal* a_other, SwitchView& a_otherView,
                           std::string_view a_otherName, std::string_view a_otherIniKey, const char* a_tooltip)
    {
        bool on = a_view.shown.load() != 0.0f;
        if (FUCK::Checkbox(a_label, &on)) {
            Post(a_global, a_view, a_name, a_iniKey, on ? 1.0f : 0.0f);
            if (on && a_otherView.shown.load() != 0.0f) {
                Post(a_other, a_otherView, a_otherName, a_otherIniKey, 0.0f);
            }
        }
        if (a_tooltip && *a_tooltip) {
            FUCK::SetTooltip(a_tooltip);
        }
    }

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
        FUCK::SetNextItemWidth(-1.0f);
        if (FUCK::SliderInt(a_label, &value, 0, Settings::MAX_GAIN, "%d")) {
            Settings::SetGain(a_section, a_actorValue, std::clamp(value, 0, Settings::MAX_GAIN));
        }
        if (FUCK::IsItemDeactivatedAfterEdit()) {
            Settings::PersistGain(a_section, a_actorValue);
        }
    }

    [[nodiscard]] std::string_view StripVampireSuffix(std::string_view a_raceEditorID)
    {
        constexpr std::string_view suffix = "Vampire";
        if (a_raceEditorID.size() > suffix.size() && a_raceEditorID.ends_with(suffix)) {
            return a_raceEditorID.substr(0, a_raceEditorID.size() - suffix.size());
        }
        return a_raceEditorID;
    }

    [[nodiscard]] RE::BGSPerk* ExitPerk(const char* a_editorID)
    {
        const auto it = g_cardForms.perks.find(a_editorID);
        return it != g_cardForms.perks.end() ? it->second : nullptr;
    }

    [[nodiscard]] std::string SkillName(RE::ActorValue a_skill)
    {
        if (const auto* info = RE::ActorValueList::GetActorValueInfo(a_skill)) {
            if (const char* name = info->GetFullName(); name && *name) {
                return name;
            }
        }
        const char* name = RE::ActorValueList::GetActorValueName(a_skill);
        return name ? name : "?";
    }

    [[nodiscard]] std::string PerkName(const char* a_editorID)
    {
        if (auto* perk = ExitPerk(a_editorID)) {
            if (const char* name = perk->GetName(); name && *name) {
                return name;
            }
        }
        return a_editorID;
    }

    void AppendPart(std::string& a_out, const std::string& a_part)
    {
        if (!a_out.empty()) {
            a_out += ", ";
        }
        a_out += a_part;
    }

    [[nodiscard]] std::string DescribeExit(const Exit& a_exit)
    {
        std::string out;
        for (const auto& gate : a_exit.skills) {
            AppendPart(out, std::format("{} {:.0f}", SkillName(gate.skill), gate.level));
        }
        for (const auto* perk : a_exit.perks) {
            AppendPart(out, PerkName(perk));
        }
        return out;
    }

    // Trained skill level, without potions or enchantments
    [[nodiscard]] bool MeetsExit(RE::Actor* a_actor, const Exit& a_exit)
    {
        auto* owner = a_actor->AsActorValueOwner();
        for (const auto& gate : a_exit.skills) {
            if (owner && owner->GetBaseActorValue(gate.skill) >= gate.level) {
                return true;
            }
        }
        for (const auto* editorID : a_exit.perks) {
            if (auto* perk = ExitPerk(editorID); perk && a_actor->HasPerk(perk)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool WearsSegmentedCuirass(RE::Actor* a_actor)
    {
        auto* cuirass = a_actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
        if (!cuirass) {
            return false;
        }
        return std::ranges::any_of(g_cardForms.segmented,
            [cuirass](RE::BGSKeyword* a_keyword) { return a_keyword && cuirass->HasKeyword(a_keyword); });
    }

    [[nodiscard]] bool SiphonHolds(RE::Actor* a_actor)
    {
        auto* target = a_actor->AsMagicTarget();
        if (!target) {
            return false;
        }
        return (g_cardForms.magicArmorSpell && target->HasMagicEffectWithKeyword(g_cardForms.magicArmorSpell, nullptr)) ||
               (g_cardForms.magicWard && target->HasMagicEffectWithKeyword(g_cardForms.magicWard, nullptr));
    }

    [[nodiscard]] bool HoldsBoundWeapon(RE::Actor* a_actor)
    {
        for (const bool leftHand : { false, true }) {
            auto* object = a_actor->GetEquippedObject(leftHand);
            auto* weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
            if (weapon && weapon->IsBound()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] CardLine BuildLine(RE::Actor* a_player, const DrawbackSpec& a_spec, bool a_drawbacksOn, bool a_masteryOn)
    {
        CardLine line;
        line.text = a_spec.text;

        if (a_spec.greenPact) {
            line.note = "Set from the Bosmer Green Pact option on the Gameplay & Balance page.";
            if (!a_drawbacksOn) {
                line.status = "Off";
                line.color = GOOD;
                return line;
            }
            switch (GreenPact::CurrentMode()) {
            case GreenPact::Mode::kOff:
                line.status = "Off";
                line.color = GOOD;
                break;
            case GreenPact::Mode::kPenaltyOnly:
                line.status = "Harvesting allowed with the penalty";
                line.color = WARN;
                break;
            default:
                line.status = "Harvesting blocked";
                line.color = BAD;
                break;
            }
            return line;
        }

        line.halve = DescribeExit(a_spec.halve);
        if (a_spec.materialPerk) {
            AppendPart(line.halve, PerkName(a_spec.materialPerk) + " with a segmented or lined heavy cuirass");
        }
        if (a_spec.accommodation == Accommodation::kSiphon) {
            AppendPart(line.halve, "an active Alteration armour spell or Ward");
        }
        line.overcome = DescribeExit(a_spec.overcome);
        if (a_spec.accommodation == Accommodation::kBoundWeapon) {
            line.note = "No penalty while you hold a bound weapon.";
        } else if (a_spec.note) {
            line.note = a_spec.note;
        }

        // The toggle switches off every exit that needs a skill or a perk, a Siphon or a bound weapon still counts without it
        const bool materialHalves = a_masteryOn && a_spec.materialPerk && ExitPerk(a_spec.materialPerk) &&
                                    a_player->HasPerk(ExitPerk(a_spec.materialPerk)) && WearsSegmentedCuirass(a_player);
        const bool siphonHalves = a_spec.accommodation == Accommodation::kSiphon && SiphonHolds(a_player);

        if (!a_drawbacksOn) {
            line.status = "Off";
            line.color = GOOD;
        } else if (a_masteryOn && MeetsExit(a_player, a_spec.overcome)) {
            line.status = "Overcome";
            line.color = GOOD;
        } else if (a_spec.accommodation == Accommodation::kBoundWeapon && HoldsBoundWeapon(a_player)) {
            line.status = "Suppressed";
            line.color = GOOD;
        } else if ((a_masteryOn && MeetsExit(a_player, a_spec.halve)) || materialHalves || siphonHalves) {
            line.status = "Halved";
            line.color = WARN;
        } else {
            line.status = "Unconditioned";
            line.color = BAD;
        }
        return line;
    }

    // Runs as a game task, which SKSE runs on the main thread (alandtse/devbench/src/MainThread.cpp:15-17,38)
    [[nodiscard]] Card BuildCard()
    {
        Card card;
        card.drawbacksOn = Menu::RacialNegativesEnabled();
        card.masteryOn = Menu::DrawbackMasteryEnabled();
        card.precisionDriving = Racials::PrecisionDriving();
        card.bloodthirstDamageShare = Racials::BloodthirstDamageShare();
        card.snap = Racials::GetSnapshot();

        RE::TESObjectREFR* target = nullptr;
        if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
            if (auto ref = pick->GetActiveTarget().get()) {
                target = ref.get();
            }
        }
        card.crosshair = GreenPact::DescribeTarget(target);

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return card;
        }
        card.hasPlayer = true;

        std::string_view baseRace;
        if (auto* race = player->GetRace()) {
            const char* name = race->GetName();
            const char* edid = race->GetFormEditorID();
            const std::string_view rawId = edid ? edid : "";
            baseRace = StripVampireSuffix(rawId);
            card.race = std::format("{} ({}){}", (name && *name) ? name : "Unknown",
                                    rawId.empty() ? "no editor ID" : rawId,
                                    baseRace.size() != rawId.size() ? " [Vampire Variant]" : "");
        }

        for (std::size_t i = 0; i < std::size(PERK_ROWS); ++i) {
            if (!g_racialPerks[i]) {
                card.missingPerks.emplace_back(PERK_ROWS[i].editorID);
            } else if (player->HasPerk(g_racialPerks[i])) {
                card.activePerks.push_back(std::format("{} ({})", PERK_ROWS[i].power, PERK_ROWS[i].race));
            }
        }

        card.shehaiActive = Racials::ShehaiActive();

        const auto spec = std::ranges::find(RACE_SPECS, baseRace, &RaceSpec::raceEditorID);
        if (spec != RACE_SPECS.end()) {
            card.knownRace = true;
            for (const auto& drawback : spec->drawbacks) {
                card.lines.push_back(BuildLine(player, drawback, card.drawbacksOn, card.masteryOn));
            }
        }
        return card;
    }

    // At most one task in flight, and no more than four a second while the page is open
    void RequestCard()
    {
        static std::chrono::steady_clock::time_point lastRequest{};
        const auto now = std::chrono::steady_clock::now();
        if (g_cardQueued.load() || now - lastRequest < CARD_REFRESH) {
            return;
        }

        const auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            return;
        }

        g_drawThread.store(REX::W32::GetCurrentThreadId());
        g_cardQueued.store(true);
        lastRequest = now;
        tasks->AddTask([]() {
            Refresh(g_negativesView, Menu::RacialNegativesEnabled() ? 1.0f : 0.0f);
            Refresh(g_masteryView, Menu::DrawbackMasteryEnabled() ? 1.0f : 0.0f);
            Refresh(g_meleeView, g_thuumMelee ? g_thuumMelee->value : 0.0f);
            Refresh(g_powerView, g_thuumPower ? g_thuumPower->value : 0.0f);
            auto card = std::make_shared<const Card>(BuildCard());
            {
                std::scoped_lock lock(g_cardLock);
                g_card = std::move(card);
            }
            if (!g_threadsLogged) {
                g_threadsLogged = true;
                logger::info("Menu: FLICK draws on thread {}, the card is built on thread {}.", g_drawThread.load(),
                             REX::W32::GetCurrentThreadId());
            }
            g_cardQueued.store(false);
        });
    }

    [[nodiscard]] std::shared_ptr<const Card> LatestCard()
    {
        RequestCard();
        std::scoped_lock lock(g_cardLock);
        return g_card;
    }

    void DrawDrawbackCard(const Card& a_card)
    {
        FUCK::SeparatorText("Drawbacks");
        FUCK::TextDisabled("%s", "Preview of the new drawbacks. The game still runs the current ones until the records that apply these are built.");

        if (!a_card.knownRace) {
            FUCK::TextDisabled("%s", "Unknown or custom race: no drawback set for it.");
            return;
        }
        if (!a_card.drawbacksOn) {
            FUCK::TextColored(GOOD, "%s", "Racial Drawbacks are off.");
        } else if (!a_card.masteryOn) {
            FUCK::TextColored(WARN, "%s", "Drawback Mastery Progression is off, so skills and perks do not reduce these.");
        }

        for (const auto& line : a_card.lines) {
            FUCK::Spacing();
            FUCK::TextColored(line.color, "[%s]", line.status.c_str());
            FUCK::SameLine();
            FUCK::TextWrapped("%s", line.text.c_str());
            FUCK::Indent();
            if (!line.halve.empty()) {
                FUCK::TextWrapped("Halved by: %s", line.halve.c_str());
            }
            if (!line.overcome.empty()) {
                FUCK::TextWrapped("Overcome by: %s", line.overcome.c_str());
            }
            if (!line.note.empty()) {
                FUCK::TextDisabled("%s", line.note.c_str());
            }
            FUCK::Unindent();
        }
    }

    class MyBloodlinePage : public FUCK::ITool
    {
    public:
        const char* Name() const override { return "My Bloodline"; }
        const char* Group() const override { return "Aetherial Bloodlines"; }

        void Draw() override
        {
            const auto shared = LatestCard();
            if (!shared) {
                FUCK::TextDisabled("%s", "Reading your character...");
                return;
            }
            const Card& card = *shared;
            if (!card.hasPlayer) {
                FUCK::TextColored(BAD, "%s", "No player loaded yet.");
                return;
            }

            FUCK::SeparatorText("Detected Bloodline");
            FUCK::Text("Race: %s", card.race.empty() ? "Unknown" : card.race.c_str());

            FUCK::SeparatorText("Active Racial Perks");
            for (const auto& missing : card.missingPerks) {
                FUCK::TextColored(BAD, "  %s is missing from the load order", missing.c_str());
            }
            for (const auto& active : card.activePerks) {
                FUCK::TextColored(GOOD, "  %s", active.c_str());
            }
            if (card.activePerks.empty() && card.missingPerks.empty()) {
                FUCK::TextDisabled("%s", "  No racial power active on this character.");
            }

            const auto& snap = card.snap;
            FUCK::SeparatorText("Live Stacks & Stances");
            if (!snap.avgReady) {
                FUCK::TextColored(BAD, "%s", "Actor Value Generator missing, stack counters inert.");
            } else {
                FUCK::Text("Oath-Forged (Orsimer): %d / 10", snap.orcStacks);
                FUCK::Text("Green Pact Oath (Bosmer): %d / 6", snap.bosmerStacks);
            }

            FUCK::Text("Shehai (Redguard): %d / %d blows%s", snap.shehaiCount, Racials::Shehai().hits.load(),
                       card.shehaiActive ? " [Shehai Active]" : "");
            FUCK::Text("Veloth's Pyre (Dunmer): Tier %d / 3 (%s)", snap.pyreTier,
                       snap.pyreUsedThisFight ? "Spent this fight" : "Ready");
            FUCK::Text("Thu'um Rekindled (Nord): %.2f s recovery (%s)", snap.voiceRecovery,
                       snap.thuumCutThisRecovery ? "Cut used this recovery" : "Cut ready");

            DrawDrawbackCard(card);
        }
    };

    class GameplayBalancePage : public FUCK::ITool
    {
    public:
        const char* Name() const override { return "Gameplay & Balance"; }
        const char* Group() const override { return "Aetherial Bloodlines"; }

        void Draw() override
        {
            const auto card = LatestCard();
            if (!card) {
                FUCK::TextDisabled("%s", "Reading your settings...");
                return;
            }

            FUCK::SeparatorText("Drawback System");

            if (!g_racialNegatives) {
                FUCK::TextColored(BAD, "Apo_Setting_RacialNegatives not found.");
                return;
            }

            bool enabled = g_negativesView.shown.load() != 0.0f;
            if (FUCK::Checkbox("Racial Drawbacks", &enabled)) {
                Post(g_racialNegatives, g_negativesView, GLOBAL_RACIAL_NEGATIVES, "RacialDrawbacks", enabled ? 1.0f : 0.0f);
            }
            FUCK::SetTooltip(
                "Every race trades a strength for a weakness: elemental weaknesses, weaker healing,\n"
                "heavier armour, the Green Pact ban on harvesting, and so on.\n\n"
                "Toggle this to keep every racial strength and disable the negatives.\n"
                "Takes effect immediately.");

            bool mastery = g_masteryView.shown.load() != 0.0f;
            if (FUCK::Checkbox("Drawback Mastery Progression", &mastery)) {
                g_drawbackMastery = mastery;
                Post(g_drawbackMasteryGlobal, g_masteryView, GLOBAL_DRAWBACK_MASTERY, "DrawbackMastery", mastery ? 1.0f : 0.0f);
            }
            FUCK::SetTooltip(
                "Lets the skills and perks named for each drawback halve it and then remove it.\n"
                "The My Bloodline page lists them for your race.\n\n"
                "Turn off to keep every drawback at full strength.");

            int pactMode = static_cast<int>(GreenPact::CurrentMode());
            const char* pactModes[] = { "Block Harvesting", "Allow with Penalty Spell", "Off" };
            if (FUCK::Combo("Bosmer Green Pact", &pactMode, pactModes, static_cast<int>(std::size(pactModes)))) {
                GreenPact::SetMode(static_cast<GreenPact::Mode>(pactMode));
                Settings::SaveSetting("GreenPactMode", static_cast<float>(pactMode));
                logger::info("Menu: GreenPactMode = {}", pactMode);
            }
            FUCK::SetTooltip(
                "Controls how the Green Pact treats flora and wood harvesting for Bosmer.\n\n"
                "Block Harvesting: blocks activation of forbidden plants and trees.\n"
                "Allow with Penalty Spell: allows harvesting but casts the Green Pact Oath penalty spell.\n"
                "Off: no harvesting restriction and no forbidden food penalty, the rest of Bosmer stays.");

            if (!enabled) {
                FUCK::Spacing();
                FUCK::TextDisabled("Racial abilities below are unavailable while drawbacks are off.");
            }
            FUCK::BeginDisabled(!enabled);

            FUCK::SeparatorText("Dunmer");
            SliderSettingLocal("Veloth's Pyre", g_pyreHealth, "PyreHealth", 0.0f,
                               static_cast<float>(PYRE_HEALTH_MAX), "%.0f%% health",
                               "Health a Dunmer must drop to before Veloth's Pyre fires, once per fight.\n"
                               "Set to 0 to disable.");

            FUCK::SeparatorText("Nord");
            SliderSettingLocal("Thu'um Rekindled", g_thuumCooldown, "ThuumCooldown", 0.0f,
                               static_cast<float>(THUUM_CUT_MAX), "%.0f%% off",
                               "Share of the remaining shout cooldown a Nord's kill removes, once per shout.\n"
                               "Set to 0 to disable.");

            ExclusiveCheckbox("Melee & Power Attacks", g_thuumMelee, g_meleeView, GLOBAL_THUUM_MELEE, "ThuumMelee",
                              g_thuumPower, g_powerView, GLOBAL_THUUM_POWER, "ThuumPower",
                              "Enables the Thu'um Rekindled bonus for both melee and power attacks.");

            ExclusiveCheckbox("Power Attacks Only", g_thuumPower, g_powerView, GLOBAL_THUUM_POWER, "ThuumPower",
                              g_thuumMelee, g_meleeView, GLOBAL_THUUM_MELEE, "ThuumMelee",
                              "Enables the Thu'um Rekindled bonus for power attacks only.\n"
                              "Turning one of these on turns the other off so the two never multiply.");

            if (!g_thuumMelee || !g_thuumPower) {
                FUCK::TextColored(BAD, "  Apo_Setting_ThuumMelee/ThuumPower not found.");
            }

            FUCK::SeparatorText("Orsimer");
            SliderSettingLocal("Bloodthirst", g_bloodthirst, "Bloodthirst", 0.0f, BLOODTHIRST_MAX,
                               "%.1f%% a hit",
                               "Health an Orsimer's melee hit returns at ten Oath-Forged stacks.\n"
                               "With Precision installed this is a share of the blow's damage; without it, a share of max health.\n"
                               "Set to 0 to disable.");

            if (card->precisionDriving) {
                FUCK::TextDisabled("  Precision found: returns %.0f%% of a blow's damage at ten stacks.",
                                   card->bloodthirstDamageShare * 100.0f);
            } else {
                FUCK::TextDisabled("  Precision not found: returns a share of max health instead.");
            }

            FUCK::SeparatorText("Redguard");
            auto& shehai = Racials::Shehai();
            SliderSettingInt("Shehai blows", shehai.hits, "ShehaiHits", 2, 6,
                             "Blows on one target, while dual wielding, that summon the Shehai.");

            SliderSettingLocal("Shehai window", shehai.window, "ShehaiWindow", 1.0f, 10.0f, "%.1f s",
                               "How long you have to land them before the combo count resets.");

            SliderSettingLocal("Shehai blow gap", shehai.gap, "ShehaiGap", 0.0f, 1.0f, "%.2f s",
                               "Shortest gap between two blows that both count, so a single dual-wield swing does not count twice.");

            FUCK::EndDisabled();

            FUCK::Spacing();
            if (FUCK::Button("Reset to Defaults")) {
                Post(g_racialNegatives, g_negativesView, GLOBAL_RACIAL_NEGATIVES, "RacialDrawbacks", 1.0f);
                g_drawbackMastery = true;
                Post(g_drawbackMasteryGlobal, g_masteryView, GLOBAL_DRAWBACK_MASTERY, "DrawbackMastery", 1.0f);
                GreenPact::SetMode(GreenPact::Mode::kBlock);
                Settings::SaveSetting("GreenPactMode", 0.0f);

                g_pyreHealth = PYRE_HEALTH_SHIPPED;
                Settings::SaveSetting("PyreHealth", PYRE_HEALTH_SHIPPED);

                g_thuumCooldown = THUUM_CUT_FALLBACK;
                Settings::SaveSetting("ThuumCooldown", THUUM_CUT_FALLBACK);

                Post(g_thuumMelee, g_meleeView, GLOBAL_THUUM_MELEE, "ThuumMelee", THUUM_MELEE_SHIPPED);
                Post(g_thuumPower, g_powerView, GLOBAL_THUUM_POWER, "ThuumPower", THUUM_POWER_SHIPPED);

                g_bloodthirst = BLOODTHIRST_FALLBACK;
                Settings::SaveSetting("Bloodthirst", BLOODTHIRST_FALLBACK);

                auto& s = Racials::Shehai();
                s.hits = SHEHAI_HITS_FALLBACK;
                s.window = SHEHAI_WINDOW_FALLBACK;
                s.gap = SHEHAI_GAP_FALLBACK;
                Settings::SaveSetting("ShehaiHits", static_cast<float>(SHEHAI_HITS_FALLBACK));
                Settings::SaveSetting("ShehaiWindow", SHEHAI_WINDOW_FALLBACK);
                Settings::SaveSetting("ShehaiGap", SHEHAI_GAP_FALLBACK);
            }
        }
    };

    class LevelUpAttributesPage : public FUCK::ITool
    {
    public:
        const char* Name() const override { return "Level-Up Attributes"; }
        const char* Group() const override { return "Aetherial Bloodlines"; }

        void Draw() override
        {
            const auto sections = Settings::GainSections();
            if (sections.empty()) {
                FUCK::TextColored(BAD, "No attribute sections found in AetherialBloodlines.ini.");
                return;
            }

            FUCK::TextDisabled("Points gained when you pick that attribute on levelling up (standard total: 30 pts).");
            FUCK::TextDisabled("Default covers any race the INI does not name, including custom ones.");
            FUCK::Spacing();

            if (!FUCK::BeginTable("levelup", 5, FUCK::TableFlags::kSizingStretchSame)) {
                return;
            }

            FUCK::TableSetupColumn("Race", FUCK::TableColumnFlags::kWidthFixed, 110.0f);
            FUCK::TableSetupColumn("Health", FUCK::TableColumnFlags::kWidthStretch, 1.0f);
            FUCK::TableSetupColumn("Magicka", FUCK::TableColumnFlags::kWidthStretch, 1.0f);
            FUCK::TableSetupColumn("Stamina", FUCK::TableColumnFlags::kWidthStretch, 1.0f);
            FUCK::TableSetupColumn("Total", FUCK::TableColumnFlags::kWidthFixed, 110.0f);
            FUCK::TableHeadersRow();

            for (const auto& section : sections) {
                const auto gains = Settings::GetGainsForSection(section);
                const std::int32_t total = gains.health + gains.magicka + gains.stamina;

                FUCK::PushID(section.c_str());
                FUCK::TableNextRow();

                FUCK::TableNextColumn();
                FUCK::Text("%s", GainSectionLabel(section).c_str());

                FUCK::TableNextColumn();
                GainSlider(section, "##health", RE::ActorValue::kHealth, gains.health);

                FUCK::TableNextColumn();
                GainSlider(section, "##magicka", RE::ActorValue::kMagicka, gains.magicka);

                FUCK::TableNextColumn();
                GainSlider(section, "##stamina", RE::ActorValue::kStamina, gains.stamina);

                FUCK::TableNextColumn();
                FUCK::TextColored(total == Settings::STANDARD_TOTAL ? GOOD : WARN, "Total: %d pts", total);

                FUCK::PopID();
            }

            FUCK::EndTable();

            FUCK::Spacing();
            if (FUCK::Button("Reset All to Defaults")) {
                Settings::ResetGainsToDefaults();
            }
            FUCK::SameLine();
            FUCK::TextDisabled("Saved to the INI as you change them, and applied on your next level up.");
        }
    };

    class DiagnosticsPage : public FUCK::ITool
    {
    public:
        const char* Name() const override { return "Diagnostics"; }
        const char* Group() const override { return "Aetherial Bloodlines"; }

        void Draw() override
        {
            const auto card = LatestCard();
            if (!card) {
                FUCK::TextDisabled("%s", "Reading the game...");
                return;
            }
            const auto& snap = card->snap;

            if (FUCK::CollapsingHeader("Crosshair (Green Pact Inspector)", ImGuiTreeNodeFlags_DefaultOpen)) {
                FUCK::TextUnformatted(card->crosshair.c_str());
            }

            if (FUCK::CollapsingHeader("Shouts (Recovery Inspector)", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!snap.voiceBlockFound) {
                    FUCK::TextColored(BAD, "No high process on the player yet.");
                } else {
                    FUCK::Text("Recovery   %.2f s (record %.2f s)", snap.voiceRecovery, snap.shoutWordRecovery);
                    FUCK::Text("Shout      %s (word %d)", snap.shoutName.c_str(), snap.shoutWord);
                    FUCK::Text("State      %d", snap.voiceState);
                    FUCK::Text("Elapsed    %.2f s", snap.voiceElapsed);
                    FUCK::TextDisabled("Shout and word blank out well before recovery reaches zero.");
                }
            }

            if (FUCK::CollapsingHeader("Stack Override Testers", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!snap.avgReady) {
                    FUCK::TextColored(BAD, "Actor Value Generator missing, counters inert.");
                } else {
                    FUCK::Text("Oath-Forged      %d of 10", snap.orcStacks);
                    FUCK::Text("Green Pact Oath  %d of 6", snap.bosmerStacks);

                    FUCK::SliderInt("Oath-Forged Override", &g_orcOverride, 0, 10);
                    if (FUCK::Button("Apply Oath-Forged")) {
                        RunOnGame([value = g_orcOverride]() { Racials::DebugSetOrcStacks(value); });
                    }

                    FUCK::SliderInt("Green Pact Oath Override", &g_bosmerOverride, 0, 6);
                    if (FUCK::Button("Apply Green Pact Oath")) {
                        RunOnGame([value = g_bosmerOverride]() { Racials::DebugSetBosmerStacks(value); });
                    }
                }
            }

            if (FUCK::CollapsingHeader("Logging", ImGuiTreeNodeFlags_DefaultOpen)) {
                const auto level = spdlog::get_level();
                const auto levelButton = [&](const char* a_label, spdlog::level::level_enum a_level) {
                    if (level == a_level) {
                        FUCK::TextColored(GOOD, "[%s]", a_label);
                    } else if (FUCK::Button(a_label)) {
                        spdlog::set_level(a_level);
                        spdlog::flush_on(a_level);
                        logger::info("Log level set to {}.", a_label);
                    }
                };

                FUCK::Text("Level:");
                FUCK::SameLine();
                levelButton("info", spdlog::level::info);
                FUCK::SameLine();
                levelButton("debug", spdlog::level::debug);
                FUCK::SameLine();
                levelButton("trace", spdlog::level::trace);
                FUCK::TextDisabled("debug logs every stack change, hit classification and combat exit.");

                FUCK::SeparatorText("Hit Log");
                FUCK::SliderInt("Hits to log", &g_hitLogCount, 1, 100);
                if (FUCK::Button(snap.hitLogRemaining > 0 ? "Stop Hit Log" : "Start Hit Log")) {
                    RunOnGame([value = snap.hitLogRemaining > 0 ? 0 : g_hitLogCount]() { Racials::StartHitLog(value); });
                }
                if (snap.hitLogRemaining > 0) {
                    FUCK::SameLine();
                    FUCK::TextColored(GOOD, "%d to go", snap.hitLogRemaining);
                }

                FUCK::SeparatorText("Dodge Tag Log");
                FUCK::SliderInt("Tags to log", &g_animTagLogCount, 1, 100);
                if (FUCK::Button(snap.animTagLogRemaining > 0 ? "Stop Dodge Tag Log" : "Start Dodge Tag Log")) {
                    RunOnGame([value = snap.animTagLogRemaining > 0 ? 0 : g_animTagLogCount]() { Racials::StartAnimTagLog(value); });
                }
                if (snap.animTagLogRemaining > 0) {
                    FUCK::SameLine();
                    FUCK::TextColored(GOOD, "%d to go", snap.animTagLogRemaining);
                }
            }
        }
    };

    MyBloodlinePage g_myBloodlinePage;
    GameplayBalancePage g_gameplayBalancePage;
    LevelUpAttributesPage g_levelUpAttributesPage;
    DiagnosticsPage g_diagnosticsPage;

    void LoadCardForms()
    {
        g_cardForms = {};
        std::vector<std::string_view> missing;

        const auto resolve = [&missing](const char* a_editorID) {
            if (!a_editorID || g_cardForms.perks.contains(a_editorID)) {
                return;
            }
            auto* perk = RE::TESForm::LookupByEditorID<RE::BGSPerk>(a_editorID);
            g_cardForms.perks.emplace(a_editorID, perk);
            if (!perk) {
                missing.emplace_back(a_editorID);
            }
        };

        for (const auto& race : RACE_SPECS) {
            for (const auto& drawback : race.drawbacks) {
                for (const auto* perk : drawback.halve.perks) {
                    resolve(perk);
                }
                for (const auto* perk : drawback.overcome.perks) {
                    resolve(perk);
                }
                resolve(drawback.materialPerk);
            }
        }

        for (const auto* editorID : SEGMENTED_MATERIALS) {
            auto* keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
            g_cardForms.segmented.push_back(keyword);
            if (!keyword) {
                missing.emplace_back(editorID);
            }
        }

        g_cardForms.magicArmorSpell = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("MagicArmorSpell");
        g_cardForms.magicWard = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("MagicWard");
        if (!g_cardForms.magicArmorSpell) {
            missing.emplace_back("MagicArmorSpell");
        }
        if (!g_cardForms.magicWard) {
            missing.emplace_back("MagicWard");
        }

        if (missing.empty()) {
            logger::info("Menu: all {} drawback exit perks and {} keywords found.", g_cardForms.perks.size(),
                         g_cardForms.segmented.size() + 2);
        } else {
            for (const auto name : missing) {
                logger::warn("Menu: drawback card form {} not found, that exit will never show as met.", name);
            }
        }
    }
}

namespace Menu
{
    bool RacialNegativesEnabled()
    {
        // Default on when the record is missing, so it never silently disarms the mod
        return g_racialNegatives ? GlobalIsOn(g_racialNegatives) : true;
    }

    bool DrawbackMasteryEnabled()
    {
        return g_drawbackMasteryGlobal ? GlobalIsOn(g_drawbackMasteryGlobal) : g_drawbackMastery.load();
    }

    float PyreHealthThreshold()
    {
        // Stored as a percent for the slider and the console, used as a fraction
        return std::clamp(g_pyreHealth.load(), 0.0f, 100.0f) * 0.01f;
    }

    float ThuumCutFraction()
    {
        return std::clamp(g_thuumCooldown.load(), 0.0f, static_cast<float>(THUUM_CUT_MAX)) * 0.01f;
    }

    float BloodthirstFraction()
    {
        return std::clamp(g_bloodthirst.load(), 0.0f, BLOODTHIRST_MAX) * 0.01f;
    }

    void LoadForms()
    {
        // Straight from the INI on every load, so an author's edit reaches an existing character
        g_drawbackMastery = Settings::GetSetting("DrawbackMastery", 1.0f) != 0.0f;
        g_pyreHealth = Settings::GetSetting("PyreHealth", PYRE_HEALTH_FALLBACK);
        g_thuumCooldown = Settings::GetSetting("ThuumCooldown", THUUM_CUT_FALLBACK);
        g_bloodthirst = Settings::GetSetting("Bloodthirst", BLOODTHIRST_FALLBACK);
        logger::info("Menu: from the INI, DrawbackMastery {}, PyreHealth {}, ThuumCooldown {}, Bloodthirst {}.",
                     g_drawbackMastery.load(), g_pyreHealth.load(), g_thuumCooldown.load(), g_bloodthirst.load());

        g_thuumMelee = RE::TESForm::LookupByEditorID<RE::TESGlobal>(GLOBAL_THUUM_MELEE);
        g_thuumPower = RE::TESForm::LookupByEditorID<RE::TESGlobal>(GLOBAL_THUUM_POWER);
        if (!g_thuumMelee || !g_thuumPower) {
            logger::warn("Menu: {} / {} not found, the Thu'um damage toggles will not work.",
                         GLOBAL_THUUM_MELEE, GLOBAL_THUUM_POWER);
        }

        g_drawbackMasteryGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>(GLOBAL_DRAWBACK_MASTERY);
        if (g_drawbackMasteryGlobal) {
            logger::info("Menu: resolved {} -> {:08X}, current value {}", GLOBAL_DRAWBACK_MASTERY,
                         g_drawbackMasteryGlobal->GetFormID(), g_drawbackMasteryGlobal->value);
        } else {
            logger::warn("Menu: {} not found in {}, the mastery switch lives in the INI only.",
                         GLOBAL_DRAWBACK_MASTERY, AB_PLUGIN);
        }

        g_racialNegatives = RE::TESForm::LookupByEditorID<RE::TESGlobal>(GLOBAL_RACIAL_NEGATIVES);
        if (g_racialNegatives) {
            logger::info("Menu: resolved {} -> {:08X}, current value {}", GLOBAL_RACIAL_NEGATIVES,
                         g_racialNegatives->GetFormID(), g_racialNegatives->value);
        } else {
            logger::warn(
                "Menu: {} not found in {}. Run the xEdit toggle script, and check that "
                "powerofthree's Tweaks is installed.",
                GLOBAL_RACIAL_NEGATIVES, AB_PLUGIN);
        }

        for (std::size_t i = 0; i < std::size(PERK_ROWS); ++i) {
            g_racialPerks[i] = RE::TESForm::LookupByEditorID<RE::BGSPerk>(PERK_ROWS[i].editorID);
        }

        LoadCardForms();
    }

    void ApplyDefaults()
    {
        struct Entry
        {
            RE::TESGlobal* global;
            std::string_view editorID;
            std::string_view iniKey;
        };

        const Entry entries[]{
            // Only the globals saved with the game need setting here, everything else is read from the INI on every load
            { g_racialNegatives, GLOBAL_RACIAL_NEGATIVES, "RacialDrawbacks" },
            { g_drawbackMasteryGlobal, GLOBAL_DRAWBACK_MASTERY, "DrawbackMastery" },
            { g_thuumMelee, GLOBAL_THUUM_MELEE, "ThuumMelee" },
            { g_thuumPower, GLOBAL_THUUM_POWER, "ThuumPower" },
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
            logger::info("Menu: {} {} -> {} from the INI", entry.editorID, entry.global->value, wanted);
            entry.global->value = wanted;
        }
    }

    void Register()
    {
        // Connect under our plugin name, then register the pages once, same as ItzIvy05/Show-Player-In-Inventory/src/SettingsUI.cpp:436-442
        if (!FUCK::Connect(SKSE::GetPluginName().data())) {
            logger::info("Menu: FLICK not connected, no menu registered.");
            return;
        }

        FUCK::RegisterTool(&g_myBloodlinePage);
        FUCK::RegisterTool(&g_gameplayBalancePage);
        FUCK::RegisterTool(&g_levelUpAttributesPage);
        FUCK::RegisterTool(&g_diagnosticsPage);
        logger::info("Menu: registered 4 FLICK pages under Aetherial Bloodlines.");
    }
}
