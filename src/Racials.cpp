#include "PCH.h"
#include "Racials.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

// The AVG API reaches for GetModuleHandle and GetProcAddress, which CommonLibSSE-NG does not pull in
#include <Windows.h>

#pragma warning(push)
#pragma warning(disable : 4100)  // unreferenced parameter, third-party header at /W4
#include "AVG/API_ActorValueGenerator.h"
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable : 4189)  // unreferenced local, third-party header at /W4
#include "Precision-API.h"
#pragma warning(pop)

#include "GreenPact.h"
#include "Notify.h"
#include "MCP.h"
#include "Settings.h"

namespace
{
    constexpr std::string_view AB_PLUGIN = "Aetherial Bloodlines.esp";
    constexpr std::string_view SKYRIM_ESM = "Skyrim.esm";

    // Lifted verbatim from the Papyrus originals so behaviour does not drift
    constexpr std::int32_t ORC_MAX_STACKS = 10;
    constexpr std::int32_t BOSMER_MAX_STACKS = 6;
    constexpr float IMPERIAL_PROC_CHANCE = 0.15f;
    constexpr float KHAJIIT_BLEED_CHANCE = 0.50f;
    constexpr float ALTMER_BURN_THRESHOLD = 0.33f;
    constexpr float ALTMER_BURN_DEEP = 0.15f;
    constexpr float ALTMER_PROC_COOLDOWN = 0.75f;

    constexpr std::int32_t DODGE_ATTACH_TRIES = 60;

    // TK Dodge Addon ships DodgeCostModifier defaulting to 1.0, so this is a straight quarter off
    constexpr float REDGUARD_DODGE_COST = 0.75f;
    constexpr float DODGE_COST_NEUTRAL = 1.0f;

    // Stacks drain rather than snap once the fight is over
    constexpr float DECAY_DELAY = 3.0f;     // seconds out of combat before the fall starts
    constexpr float DECAY_INTERVAL = 1.5f;  // seconds per stack lost after that

    // Longer than any alt-death calm, or StopCombat mid-encounter re-arms the pyre on the same bandits
    constexpr float PYRE_REARM_DELAY = 30.0f;

    // Kills renew the pyre, so it needs a cap or a long dungeon fight holds it up indefinitely
    constexpr std::int32_t PYRE_MAX_TIER = 3;

    // Share of the wound closed at ten stacks, scaling down with the stacks actually held
    constexpr float BLOODTHIRST_SPEND_MISSING = 0.4f;

    // Same target only, so swinging into a crowd never builds the rhythm
    Racials::ShehaiTuning g_shehai;

    // Converts the slider's max-Health share into a share of the blow's damage, for the Precision path
    constexpr float BLOODTHIRST_DAMAGE_SCALE = 6.67f;

    bool g_ready{ false };

    std::int32_t g_hitLogRemaining{ 0 };

    std::size_t g_resolved{ 0 };
    std::size_t g_missing{ 0 };

    struct Forms {
        RE::TESGlobal* orcCounter{ nullptr };
        RE::TESGlobal* bosmerCounter{ nullptr };

        RE::BGSPerk* orcPerk{ nullptr };
        RE::BGSPerk* bosmerPerk{ nullptr };
        RE::BGSPerk* imperialPerk{ nullptr };
        RE::BGSPerk* khajiitPerk{ nullptr };
        RE::BGSPerk* altmerPerk{ nullptr };
        RE::BGSPerk* argonianPerk{ nullptr };
        RE::BGSPerk* dunmerPerk{ nullptr };

        RE::SpellItem* imperialBuff{ nullptr };
        RE::SpellItem* khajiitBleed{ nullptr };
        RE::SpellItem* argonianBuff{ nullptr };
        RE::SpellItem* greenPactPenalty{ nullptr };
        RE::SpellItem* searLight{ nullptr };
        RE::SpellItem* searHeavy{ nullptr };
        RE::SpellItem* bareHands{ nullptr };
        RE::SpellItem* gloved{ nullptr };

        RE::BGSListForm* veggieFoods{ nullptr };
        RE::BGSListForm* bloodthirstExclusions{ nullptr };
        RE::BGSKeyword* vendorItemPotion{ nullptr };
        RE::BGSKeyword* weapTypeBow{ nullptr };
        RE::BGSKeyword* weapTypeSword{ nullptr };

        RE::SpellItem* velothsPyre{ nullptr };
        RE::SpellItem* velothsPyre2{ nullptr };
        RE::SpellItem* velothsPyre3{ nullptr };

        RE::BGSPerk* nordPerk{ nullptr };
        RE::SpellItem* thuumRekindled{ nullptr };

        RE::BGSPerk* redguardPerk{ nullptr };
        RE::SpellItem* shehai{ nullptr };
    };

    Forms g_forms;

    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastSear;

    // Switched to AVG::ExtraValue for the stacks, because the Globals could only be read for the player
    // Hooray for AVG!
    RE::ActorValue g_orcStacksAV{ RE::ActorValue::kNone };
    RE::ActorValue g_bosmerStacksAV{ RE::ActorValue::kNone };
    bool g_avgReady{ false };

    // TK Dodge Addon's own extra value, kNone when the addon is absent, which is the soft dependency
    RE::ActorValue g_dodgeCostAV{ RE::ActorValue::kNone };

    float g_outOfCombatFor{ 0.0f };
    float g_decayAccum{ 0.0f };
    bool g_pyreUsedThisFight{ false };

    // Separate from g_outOfCombatFor, which bails on empty stacks and so never runs for a Dunmer
    float g_peaceFor{ 0.0f };
    std::int32_t g_pyreTier{ 0 };

    // Defined below with the sink they attach, but the tick and the load reset call them long before that
    void TryAttachDodgeSink();
    bool AttachDodgeSink();

    // The graph is not ready at kDataLoaded, so the attach is retried from the tick until it takes
    bool g_dodgeSinkAttached{ false };
    std::int32_t g_dodgeAttachTries{ 0 };
    std::int32_t g_animTagLogRemaining{ 0 };

    bool TagLooksLikeDodge(const RE::BSFixedString& a_tag)
    {
        const char* tag = a_tag.c_str();
        if (!tag) return false;
        for (const char* p = tag; *p; ++p) {
            if ((*p | 0x20) != 'd') continue;
            const char* want = "odge";
            const char* q = p + 1;
            for (; *want && *q && (*q | 0x20) == *want; ++q, ++want) {}
            if (!*want) return true;
        }
        return false;
    }

    // Shehai's running combo. The target is held so hits scattered across a crowd never stack
    RE::FormID g_shehaiTarget{ 0 };
    std::int32_t g_shehaiCount{ 0 };
    std::chrono::steady_clock::time_point g_shehaiLast{};

    // Thu'um Rekindled, tracked across kills so one shout's recovery is only ever cut once
    constexpr const char* THUUM_MESSAGE = "Kyne returns your breath.";
    constexpr const char* PYRE_RENEWED_MESSAGE = "The Prophet's flame rises.";
    constexpr const char* BLOODTHIRST_SPEND_MESSAGE = "Mauloch keeps the oath.";
    constexpr const char* SHEHAI_MESSAGE = "The Shehai takes shape.";
    float g_voiceLastSeen{ 0.0f };
    bool g_thuumCutThisRecovery{ false };

    // Editor ID first, load-order fallback
    template <class T>
    [[nodiscard]] T* Resolve(std::string_view a_editorID, RE::FormID a_localID, std::string_view a_plugin)
    {
        T* out = RE::TESForm::LookupByEditorID<T>(a_editorID);
        std::string how;

        if (out) {
            how = std::format("{:08X} (editor ID)", out->GetFormID());
        } else if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
            if (auto* form = dataHandler->LookupForm(a_localID, a_plugin)) {
                out = form->As<T>();
                how = out ? std::format("{:08X} ({}|{:06X})", out->GetFormID(), a_plugin, a_localID)
                          : std::string("WRONG FORM TYPE");
            }
        }

        if (out) {
            ++g_resolved;
        } else {
            ++g_missing;
            if (how.empty()) {
                how = "MISSING";
            }
            logger::warn("Racials: could not resolve {} ({}|{:06X})", a_editorID, a_plugin, a_localID);
        }

        logger::debug("form: {} -> {}", a_editorID, how);
        return out;
    }

    [[nodiscard]] RE::Actor* Player()
    {
        return RE::PlayerCharacter::GetSingleton();
    }

    [[nodiscard]] bool HasPerk(RE::Actor* a_actor, RE::BGSPerk* a_perk)
    {
        if (!a_actor) return false;
        if (!a_perk) return false;
        return a_actor->HasPerk(a_perk);
    }

    // A real spell belongs to one of the five schools; other mods' utility spells arrive flagged hostile with the player as the cause
    [[nodiscard]] bool IsRealSpell(RE::SpellItem* a_spell)
    {
        if (!a_spell || !a_spell->IsHostile()) return false;
        switch (a_spell->GetAssociatedSkill()) {
        case RE::ActorValue::kAlteration:
        case RE::ActorValue::kConjuration:
        case RE::ActorValue::kDestruction:
        case RE::ActorValue::kIllusion:
        case RE::ActorValue::kRestoration:
            return true;
        default:
            return false;
        }
    }

    // One timestamp per caster, because the sear is no longer the player's alone
    [[nodiscard]] bool SearOffCooldown(RE::Actor* a_actor)
    {
        const auto it = g_lastSear.find(a_actor->GetFormID());
        if (it == g_lastSear.end()) return true;
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second).count() >=
               ALTMER_PROC_COOLDOWN;
    }

    void MarkSear(RE::Actor* a_actor)
    {
        const auto now = std::chrono::steady_clock::now();

        // Prune rather than grow forever, since every Altmer that ever seared would otherwise stay in the map
        if (g_lastSear.size() > 32) {
            std::erase_if(g_lastSear, [&](const auto& a_entry) {
                return std::chrono::duration<float>(now - a_entry.second).count() >= ALTMER_PROC_COOLDOWN;
            });
        }

        g_lastSear[a_actor->GetFormID()] = now;
    }

    [[nodiscard]] bool IsBowWeapon(RE::TESObjectWEAP* a_weapon)
    {
        return a_weapon && g_forms.weapTypeBow && a_weapon->HasKeyword(g_forms.weapTypeBow);
    }

    // Only used by the hit log, to settle what source actually holds on a bow hit
    [[nodiscard]] const char* SourceKind(RE::TESForm* a_form)
    {
        if (!a_form) return "none";
        if (a_form->As<RE::TESObjectWEAP>()) return "weapon";
        if (a_form->As<RE::TESAmmo>()) return "ammo";
        if (a_form->As<RE::SpellItem>()) return "spell";
        if (a_form->As<RE::EnchantmentItem>()) return "enchantment";
        return "other";
    }

    // The native Papyrus GetActorValuePercentage resolves to (Styyx1/StyyxUtils/include/st-actor.h:701)
    [[nodiscard]] float GetActorValuePercentage(RE::Actor* a_actor, RE::ActorValue a_av)
    {
        using func_t = float (*)(RE::Actor*, RE::ActorValue);
        static REL::Relocation<func_t> func{ RELOCATION_ID(36347, 37337) };
        return func(a_actor, a_av);
    }

    [[nodiscard]] float RandomUnit()
    {
        static thread_local std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(rng);
    }

    void CastOn(RE::SpellItem* a_spell, RE::Actor* a_caster, RE::TESObjectREFR* a_target)
    {
        if (!a_spell) return;
        if (!a_caster) return;

        // DoCombatSpellApply has no CommonLibSSE-NG equivalent afaik, CastSpellImmediate replaces it
        if (auto* caster = a_caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
            caster->CastSpellImmediate(a_spell, false, a_target ? a_target : a_caster, 1.0f, false, 0.0f, a_caster);
        }
    }

    [[nodiscard]] const char* Who(RE::Actor* a_actor)
    {
        if (!a_actor) return "<none>";
        const auto* name = a_actor->GetName();
        return (name && *name) ? name : "<unnamed>";
    }

    [[nodiscard]] std::int32_t GetStacks(RE::Actor* a_actor, RE::ActorValue a_av)
    {
        if (!g_avgReady || !a_actor) return 0;
        return static_cast<std::int32_t>(a_actor->AsActorValueOwner()->GetActorValue(a_av));
    }

    // Every stack change goes through here, so the log always shows who, which counter, and why
    void SetStacks(RE::Actor* a_actor, RE::ActorValue a_av, std::int32_t a_value, std::int32_t a_max,
                   const char* a_counter, const char* a_why)
    {
        if (!a_actor) return;
        if (!g_avgReady) {
            logger::debug("stack: {} {} ignored, AVG not ready", Who(a_actor), a_counter);
            return;
        }

        const auto before = GetStacks(a_actor, a_av);
        const auto after = std::clamp(a_value, 0, a_max);
        if (before == after) {
            logger::debug("stack: {} {} already {}, {}", Who(a_actor), a_counter, after, a_why);
            return;
        }

        a_actor->AsActorValueOwner()->SetActorValue(a_av, static_cast<float>(after));

        const auto readback = GetStacks(a_actor, a_av);
        if (readback != after) {
            logger::error("stack: {} {} write failed, asked {} got {} ({})", Who(a_actor), a_counter, after, readback,
                          a_why);
            return;
        }
        logger::debug("stack: {} {} {} -> {}, {}", Who(a_actor), a_counter, before, after, a_why);
    }

    void BumpStacks(RE::Actor* a_actor, RE::ActorValue a_av, std::int32_t a_max, const char* a_counter,
                    const char* a_why)
    {
        SetStacks(a_actor, a_av, GetStacks(a_actor, a_av) + 1, a_max, a_counter, a_why);
    }

    // Direct membership first because it is a lookup; keyword and faction entries are walked
    [[nodiscard]] bool ActorMatchesList(RE::Actor* a_actor, RE::BGSListForm* a_list)
    {
        if (!a_actor || !a_list) return false;

        if (auto* race = a_actor->GetRace(); race && a_list->HasForm(race)) return true;
        if (auto* base = a_actor->GetActorBase(); base && a_list->HasForm(base)) return true;
        if (a_list->HasForm(a_actor)) return true;

        bool matched = false;
        a_list->ForEachForm([&](RE::TESForm* a_form) {
            if (auto* keyword = a_form ? a_form->As<RE::BGSKeyword>() : nullptr) {
                // Actor overrides HasKeywordHelper, so this sees keywords a runtime race swap added
                if (a_actor->HasKeyword(keyword)) matched = true;
            } else if (auto* faction = a_form ? a_form->As<RE::TESFaction>() : nullptr) {
                if (a_actor->IsInFaction(faction)) matched = true;
            }
            return matched ? RE::BSContainer::ForEachResult::kStop
                           : RE::BSContainer::ForEachResult::kContinue;
        });
        return matched;
    }

    // Shared by the trickle and the spend so the two can never drift apart
    [[nodiscard]] float BloodthirstAmount(RE::Actor* a_orc, std::int32_t a_stacks, float a_multiplier)
    {
        if (!a_orc || a_stacks <= 0) return 0.0f;

        const float atFullStacks = MCP::BloodthirstFraction();
        if (atFullStacks <= 0.0f) return 0.0f;  // slider at 0 turns it off

        const float maxHealth = a_orc->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth);
        return maxHealth * atFullStacks * static_cast<float>(a_stacks) * a_multiplier / ORC_MAX_STACKS;
    }

    // Both hands holding a weapon. A spell or an empty hand is not a dance
    [[nodiscard]] bool IsDualWielding(RE::Actor* a_actor)
    {
        if (!a_actor) return false;

        auto* right = a_actor->GetEquippedObject(false);
        auto* left = a_actor->GetEquippedObject(true);
        return right && left && right->As<RE::TESObjectWEAP>() && left->As<RE::TESObjectWEAP>();
    }

    // The Gambit's own perk entries all gate on WeapTypeSword, so the discount asks for the same commitment
    [[nodiscard]] bool IsDualWieldingSwords(RE::Actor* a_actor)
    {
        if (!a_actor || !g_forms.weapTypeSword) return false;

        auto* right = a_actor->GetEquippedObject(false);
        auto* left = a_actor->GetEquippedObject(true);
        if (!right || !left) return false;

        auto* rightWeap = right->As<RE::TESObjectWEAP>();
        auto* leftWeap = left->As<RE::TESObjectWEAP>();
        if (!rightWeap || !leftWeap) return false;

        return rightWeap->HasKeyword(g_forms.weapTypeSword) && leftWeap->HasKeyword(g_forms.weapTypeSword);
    }

    void RefreshDodgeCost(RE::Actor* a_actor)
    {
        if (!a_actor || g_dodgeCostAV == RE::ActorValue::kNone) return;
        if (!HasPerk(a_actor, g_forms.redguardPerk)) return;

        const float wanted = IsDualWieldingSwords(a_actor) ? REDGUARD_DODGE_COST : DODGE_COST_NEUTRAL;
        auto* owner = a_actor->AsActorValueOwner();
        if (std::abs(owner->GetActorValue(g_dodgeCostAV) - wanted) < 0.001f) return;

        owner->SetActorValue(g_dodgeCostAV, wanted);

        const float readback = owner->GetActorValue(g_dodgeCostAV);
        if (std::abs(readback - wanted) > 0.001f) {
            logger::error("dodge: cost modifier write failed, asked {} got {}", wanted, readback);
            return;
        }
        logger::debug("dodge: cost modifier now {}", wanted);
    }

    // A new target or too long a pause starts the count again at one
    void AdvanceShehai(RE::Actor* a_actor, RE::FormID a_target, const char* a_why)
    {
        if (!g_forms.shehai || !MCP::RacialNegativesEnabled()) return;
        if (!HasPerk(a_actor, g_forms.redguardPerk) || !IsDualWielding(a_actor)) return;

        const auto now = std::chrono::steady_clock::now();
        const float since = std::chrono::duration<float>(now - g_shehaiLast).count();

        // The off hand landing on the same swing is not a second blow
        if (g_shehaiCount > 0 && since < g_shehai.gap) {
            logger::debug("shehai: {} ignored, {:.3f}s after the last", a_why, since);
            return;
        }

        const bool sameTarget = a_target != 0 && a_target == g_shehaiTarget;
        const bool inTime = g_shehaiCount > 0 && since <= g_shehai.window;

        // A dodge carries the count on whatever you were hitting, so it extends rather than retargets
        if (a_target != 0) g_shehaiTarget = a_target;
        g_shehaiCount = (sameTarget || (a_target == 0 && inTime)) && inTime ? g_shehaiCount + 1 : 1;
        g_shehaiLast = now;

        logger::debug("shehai: {} of {}, {}", g_shehaiCount, g_shehai.hits, a_why);

        if (g_shehaiCount < g_shehai.hits) return;

        g_shehaiCount = 0;
        g_shehaiTarget = 0;
        CastOn(g_forms.shehai, a_actor, a_actor);
        if (a_actor == Player()) {
            Notify::Show(SHEHAI_MESSAGE);
        }
        logger::debug("power: Shehai answered");
    }

    // Precision carries the real damage figure and drives the melee branch outright, because the order the two fire in is unverified
    bool g_precisionDriving{ false };

    // Both counters only move while Precision drives, so a growing gap means it is not seeing some melee hits
    std::uint64_t g_precisionHits{ 0 };
    std::uint64_t g_vanillaMeleeSeen{ 0 };

    [[nodiscard]] float BloodthirstFromDamage(std::int32_t a_stacks, float a_damage)
    {
        if (a_stacks <= 0 || a_damage <= 0.0f) return 0.0f;

        const float share = MCP::BloodthirstFraction() * BLOODTHIRST_DAMAGE_SCALE;
        if (share <= 0.0f) return 0.0f;  // slider at 0 turns it off
        return a_damage * share * static_cast<float>(a_stacks) / ORC_MAX_STACKS;
    }

    // Reads the wound rather than the pool, floored at a normal hit's return
    [[nodiscard]] float BloodthirstSpendAmount(RE::Actor* a_orc, std::int32_t a_stacks)
    {
        if (!a_orc || a_stacks <= 0) return 0.0f;

        auto* values = a_orc->AsActorValueOwner();
        const float maxHealth = values->GetPermanentActorValue(RE::ActorValue::kHealth);
        const float missing = std::max(0.0f, maxHealth - values->GetActorValue(RE::ActorValue::kHealth));

        const float fromWound =
            missing * BLOODTHIRST_SPEND_MISSING * static_cast<float>(a_stacks) / ORC_MAX_STACKS;
        return std::max(fromWound, BloodthirstAmount(a_orc, a_stacks, 1.0f));
    }

    // Matched on ActiveEffect::spell, so the C++ never restates a duration the record already owns
    [[nodiscard]] bool HasActiveEffectFrom(RE::Actor* a_actor, RE::MagicItem* a_spell)
    {
        if (!a_actor || !a_spell) return false;

        auto* magicTarget = a_actor->AsMagicTarget();
        if (!magicTarget) return false;

        auto* effects = magicTarget->GetActiveEffectList();
        if (!effects) return false;

        for (auto* effect : *effects) {
            if (!effect) continue;
            if (effect->flags.any(RE::ActiveEffect::Flag::kInactive)) continue;
            if (effect->spell == a_spell) return true;
        }
        return false;
    }

    // Recasting restarts the record's own duration, which is why nothing here knows how long it is
    [[nodiscard]] RE::SpellItem* PyreSpellForTier(std::int32_t a_tier)
    {
        switch (a_tier) {
        case 1: return g_forms.velothsPyre;
        case 2: return g_forms.velothsPyre2;
        case 3: return g_forms.velothsPyre3;
        default: return nullptr;
        }
    }

    // Collected before dispelling, because Dispel mutates the list being walked
    void DispelEffectsFrom(RE::Actor* a_actor, RE::MagicItem* a_spell)
    {
        if (!a_actor || !a_spell) return;

        auto* magicTarget = a_actor->AsMagicTarget();
        if (!magicTarget) return;

        auto* effects = magicTarget->GetActiveEffectList();
        if (!effects) return;

        std::vector<RE::ActiveEffect*> doomed;
        for (auto* effect : *effects) {
            if (effect && effect->spell == a_spell) doomed.push_back(effect);
        }
        for (auto* effect : doomed) {
            effect->Dispel(true);
        }
    }

    void TryPyreRenewal(RE::Actor* a_player)
    {
        if (!MCP::RacialNegativesEnabled()) return;
        if (!HasPerk(a_player, g_forms.dunmerPerk)) return;
        if (g_pyreTier <= 0) return;

        auto* current = PyreSpellForTier(g_pyreTier);
        if (!current || !HasActiveEffectFrom(a_player, current)) return;

        const std::int32_t nextTier = std::min(g_pyreTier + 1, PYRE_MAX_TIER);
        auto* next = PyreSpellForTier(nextTier);
        if (!next) return;

        // The running tier goes first, or its cloak would burn alongside the new one
        DispelEffectsFrom(a_player, current);
        g_pyreTier = nextTier;
        CastOn(next, a_player, a_player);
        Notify::Show(PYRE_RENEWED_MESSAGE);
        logger::debug("power: Veloth's Pyre fed by a kill, now tier {} of {}", g_pyreTier, PYRE_MAX_TIER);
    }

    // A script restore is the point, not an implementation detail: it bypasses the Orsimer healing drawback
    void TryBloodthirst(RE::Actor* a_orc, RE::Actor* a_victim)
    {
        if (!MCP::RacialNegativesEnabled()) return;

        // An empty or missing list excludes nobody, so a bad record never silently disables the ability
        if (ActorMatchesList(a_victim, g_forms.bloodthirstExclusions)) {
            logger::debug("power: Bloodthirst declined, {} is excluded", Who(a_victim));
            return;
        }

        const auto stacks = GetStacks(a_orc, g_orcStacksAV);
        const float restore = BloodthirstAmount(a_orc, stacks, 1.0f);
        if (restore <= 0.0f) return;

        // Health damage lives in the kDamage modifier, so restoring it is the heal
        a_orc->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIERS::kDamage,
                                                      RE::ActorValue::kHealth, restore);
        logger::debug("power: Bloodthirst, {} stacks returned {:.1f} health", stacks, restore);
    }

    // Normal hits build the counter, power attacks empty it, and the two stay exclusive
    void TryBloodthirstSpend(RE::Actor* a_orc, RE::Actor* a_victim)
    {
        if (!MCP::RacialNegativesEnabled()) return;
        if (ActorMatchesList(a_victim, g_forms.bloodthirstExclusions)) return;

        const auto stacks = GetStacks(a_orc, g_orcStacksAV);
        const float restore = BloodthirstSpendAmount(a_orc, stacks);
        if (restore <= 0.0f) return;

        SetStacks(a_orc, g_orcStacksAV, 0, ORC_MAX_STACKS, "Oath-Forged", "spent on a power attack");
        a_orc->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIERS::kDamage,
                                                      RE::ActorValue::kHealth, restore);

        if (a_orc == Player()) {
            Notify::Show(BLOODTHIRST_SPEND_MESSAGE);
        }
        logger::debug("power: Bloodthirst spent {} stacks for {:.1f} health", stacks, restore);
    }

    // Fires itself once per fight when the player is nearly down, checked from both the tick and the hit sink because the tick's hook is skipped on VR
    void TryVelothsPyre(RE::Actor* a_player)
    {
        if (g_pyreUsedThisFight || !g_forms.velothsPyre) return;
        if (!MCP::RacialNegativesEnabled()) return;
        if (!HasPerk(a_player, g_forms.dunmerPerk)) return;
        if (!a_player->IsInCombat()) return;
        const float threshold = MCP::PyreHealthThreshold();
        if (threshold <= 0.0f) return;  // slider at 0 turns it off
        if (GetActorValuePercentage(a_player, RE::ActorValue::kHealth) >= threshold) return;

        g_pyreUsedThisFight = true;
        g_pyreTier = 1;
        CastOn(g_forms.velothsPyre, a_player, a_player);
        logger::debug("power: Veloth's Pyre answered, health below {:.0f}%", threshold * 100.0f);
    }

    // voiceRecoveryTime is the live countdown in seconds and the engine gates shouting on it

    [[nodiscard]] RE::HighProcessData* VoiceBlock(RE::Actor* a_actor)
    {
        if (!a_actor) return nullptr;

        // currentProcess moved between 1.5 and 1.6, so it is only reachable through the accessor
        auto* process = a_actor->GetActorRuntimeData().currentProcess;
        return process ? process->high : nullptr;
    }

    [[nodiscard]] std::string ShoutName(RE::HighProcessData* a_high)
    {
        if (!a_high || !a_high->currentShout) return "<none>";
        const auto* name = a_high->currentShout->GetName();
        return (name && *name) ? name : "<unnamed>";
    }

    // What the SHOU record says this word costs, to compare against what the engine is holding
    [[nodiscard]] float RecordRecoveryTime(RE::HighProcessData* a_high)
    {
        if (!a_high || !a_high->currentShout) return 0.0f;
        const auto word = static_cast<std::int32_t>(a_high->currentShoutVariation);
        if (word < 0 || word >= RE::TESShout::VariationIDs::kTotal) return 0.0f;
        return a_high->currentShout->variations[word].recoveryTime;
    }

    void LogVoice(const char* a_why, RE::HighProcessData* a_high)
    {
        if (!a_high) {
            logger::info("voice: {} | no high process, the player is not in high AI", a_why);
            return;
        }
        logger::info("voice: {} | state={} shout=\"{}\" word={} elapsed={:.2f} recovery={:.2f} record={:.2f}",
                     a_why, static_cast<int>(a_high->voiceState.underlying()), ShoutName(a_high),
                     static_cast<std::int32_t>(a_high->currentShoutVariation), a_high->voiceTimeElapsed,
                     a_high->voiceRecoveryTime, RecordRecoveryTime(a_high));
    }

    // Once per recovery: repeated cuts compound, and 14 of them took a 17s wait to 0.04s
    void TryThuumRekindled(RE::Actor* a_player)
    {
        if (!g_forms.thuumRekindled) return;
        if (!MCP::RacialNegativesEnabled()) return;
        if (!HasPerk(a_player, g_forms.nordPerk)) return;

        const float fraction = MCP::ThuumCutFraction();
        if (fraction <= 0.0f) return;  // slider at 0 turns it off

        auto* high = VoiceBlock(a_player);
        if (!high) return;

        const float recovery = high->voiceRecoveryTime;
        if (recovery <= 0.0f) {
            g_voiceLastSeen = 0.0f;
            g_thuumCutThisRecovery = false;
            return;
        }

        // Recovery only ever rises when a shout fires, so a rise is a new cooldown to spend a cut on
        if (recovery > g_voiceLastSeen) {
            g_thuumCutThisRecovery = false;
        }

        if (!g_thuumCutThisRecovery) {
            high->voiceRecoveryTime = recovery * (1.0f - fraction);
            g_thuumCutThisRecovery = true;
            logger::debug("power: Thu'um Rekindled, recovery {:.2f} -> {:.2f}", recovery,
                          high->voiceRecoveryTime);

            // The only feedback either half of this gets, and the cut is already once per recovery
            Notify::Show(THUUM_MESSAGE);
        }
        g_voiceLastSeen = high->voiceRecoveryTime;

        CastOn(g_forms.thuumRekindled, a_player, a_player);
    }

    // Stacks fall on a tick, because the player almost never fires a combat event
    void TickDecay(float a_delta)
    {
        if (!g_ready || !g_avgReady) return;

        auto* player = Player();
        if (!player) return;

        TryAttachDodgeSink();

        if (player->IsInCombat()) {
            g_outOfCombatFor = 0.0f;
            g_decayAccum = 0.0f;
            g_peaceFor = 0.0f;
            TryVelothsPyre(player);
            return;
        }

        // First frame out of combat, so it reports once per fight; a gap means Precision missed qualifying hits
        if (g_precisionDriving && g_peaceFor == 0.0f && g_vanillaMeleeSeen > 0) {
            logger::debug("bloodthirst driver: Precision {} of {} eligible melee hits", g_precisionHits,
                          g_vanillaMeleeSeen);
        }

        // Only a real lull re-arms the pyre, never the bare combat flag
        g_peaceFor += a_delta;
        if (g_peaceFor >= PYRE_REARM_DELAY && g_pyreUsedThisFight) {
            g_pyreUsedThisFight = false;
            g_pyreTier = 0;
            logger::debug("power: Veloth's Pyre re-armed after {:.0f}s out of combat", g_peaceFor);
        }

        const auto orc = GetStacks(player, g_orcStacksAV);
        const auto bosmer = GetStacks(player, g_bosmerStacksAV);
        if (orc <= 0 && bosmer <= 0) {
            g_outOfCombatFor = 0.0f;
            g_decayAccum = 0.0f;
            return;
        }

        g_outOfCombatFor += a_delta;
        if (g_outOfCombatFor < DECAY_DELAY) return;

        g_decayAccum += a_delta;
        if (g_decayAccum < DECAY_INTERVAL) return;
        g_decayAccum = 0.0f;

        if (orc > 0) {
            SetStacks(player, g_orcStacksAV, orc - 1, ORC_MAX_STACKS, "Oath-Forged", "cooling out of combat");
        }
        if (bosmer > 0) {
            SetStacks(player, g_bosmerStacksAV, bosmer - 1, BOSMER_MAX_STACKS, "Green Pact", "cooling out of combat");
        }
    }

    // Main update call site (NoahBoddie/poison-aid/src/Hooks.h:341-352, Styyx1/STweaks-Rewrite/src/hooks.h:81)
    struct MainUpdateHook {
        static void thunk()
        {
            func();
            TickDecay(RE::GetSecondsSinceLastFrame());
        }

        static inline REL::Relocation<decltype(thunk)> func;
    };

    class CombatSink : public RE::BSTEventSink<RE::TESCombatEvent>
    {
    public:
        static CombatSink* GetSingleton()
        {
            static CombatSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* a_event,
                                              RE::BSTEventSource<RE::TESCombatEvent>*) override
        {
            if (!a_event || !g_ready) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* actor = a_event->actor ? a_event->actor->As<RE::Actor>() : nullptr;
            if (!actor) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // "The stacks did not reset" and "combat never ended" look identical in game, and only this tells them apart
            logger::debug("combat: {} state -> {}", Who(actor), static_cast<int>(a_event->newState.get()));

            // The player's own fall is handled by the update tick; the player barely ever appears in a combat event

            // Reset whoever left combat, not just the player
            if (a_event->newState != RE::ACTOR_COMBAT_STATE::kNone) {
                return RE::BSEventNotifyControl::kContinue;
            }

            SetStacks(actor, g_orcStacksAV, 0, ORC_MAX_STACKS, "Oath-Forged", "left combat");
            SetStacks(actor, g_bosmerStacksAV, 0, BOSMER_MAX_STACKS, "Green Pact", "left combat");

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
    {
    public:
        static HitSink* GetSingleton()
        {
            static HitSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
                                              RE::BSTEventSource<RE::TESHitEvent>*) override
        {
            if (!a_event || !g_ready) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* aggressor = a_event->cause ? a_event->cause->As<RE::Actor>() : nullptr;
            if (!aggressor) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const bool byPlayer = aggressor == Player();

            const bool blocked = a_event->flags.all(RE::TESHitEvent::Flag::kHitBlocked);
            const bool bashed = a_event->flags.all(RE::TESHitEvent::Flag::kBashAttack);
            auto* source = a_event->source ? RE::TESForm::LookupByID(a_event->source) : nullptr;
            auto* weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;

            // projectile reads 0 on almost every arrow hit, so the weapon type is the only reliable discriminator
            const bool meleeWeaponHit = weapon && weapon->IsMelee();
            const bool bowHit = weapon && !bashed && IsBowWeapon(weapon);

            auto* victim = a_event->target ? a_event->target->As<RE::Actor>() : nullptr;

            // The countdown forces info, for when the level is left at info and only a short capture is wanted
            if (byPlayer && (g_hitLogRemaining > 0 || spdlog::should_log(spdlog::level::debug))) {
                const auto line =
                    std::format("hit: source {:08X} {} \"{}\" | projectile {:08X} | melee={} bow={} bash={} blocked={}",
                                a_event->source, SourceKind(source), source ? source->GetName() : "",
                                a_event->projectile, meleeWeaponHit, bowHit, bashed, blocked);
                if (g_hitLogRemaining > 0) {
                    --g_hitLogRemaining;
                    logger::info("{} | {} left", line, g_hitLogRemaining);
                } else {
                    logger::debug("{}", line);
                }
            }

            // A corpse is not a fight, so it feeds neither stacks nor lifesteal
            const bool liveVictim = victim && victim != aggressor && !victim->IsDead();

            // Counted so a coverage gap between the two drivers shows up in the log
            if (g_precisionDriving && meleeWeaponHit && !blocked && liveVictim &&
                HasPerk(aggressor, g_forms.orcPerk)) {
                ++g_vanillaMeleeSeen;
            }

            // Blocked blows do not wound, ranged and magic are not melee. IncomingHitSink carries the mirror of this for hits taken
            if (g_precisionDriving) {
                // Precision owns the Orsimer branch outright, see OnPrecisionHit
            } else if (meleeWeaponHit && !blocked && liveVictim && HasPerk(aggressor, g_forms.orcPerk)) {
                if (a_event->flags.all(RE::TESHitEvent::Flag::kPowerAttack)) {
                    TryBloodthirstSpend(aggressor, victim);
                } else {
                    BumpStacks(aggressor, g_orcStacksAV, ORC_MAX_STACKS, "Oath-Forged", "melee hit dealt");

                    // After the bump, so the first swing of a fight already returns its one stack
                    TryBloodthirst(aggressor, victim);
                }
            } else if (byPlayer && meleeWeaponHit && !blocked && HasPerk(aggressor, g_forms.orcPerk)) {
                // Which arm of liveVictim refused; "nothing happened" reads identically for a corpse, a barrel or the player
                logger::debug("stack: no Oath-Forged, victim {}",
                              !victim               ? "is not an actor"
                              : victim == aggressor ? "is the aggressor"
                                                    : "is already dead");
            }

            // Outside the Precision branch, because the Shehai counts blows and never needs a damage figure
            if (meleeWeaponHit && !blocked && liveVictim) {
                AdvanceShehai(aggressor, victim->GetFormID(), "melee hit");
            }

            if (HasPerk(aggressor, g_forms.bosmerPerk)) {
                if (bowHit) {
                    BumpStacks(aggressor, g_bosmerStacksAV, BOSMER_MAX_STACKS, "Green Pact", "bow hit");
                } else if (weapon) {
                    // Any other weapon breaks the run, a spell leaves the count alone
                    SetStacks(aggressor, g_bosmerStacksAV, 0, BOSMER_MAX_STACKS, "Green Pact", "hit with a non-bow weapon");
                }
            }

            if (!victim || victim == aggressor || victim->IsDead()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Khajiit bleed, 50% on a weapon hit, for any Khajiit rather than the player alone
            if (meleeWeaponHit && HasPerk(aggressor, g_forms.khajiitPerk)) {
                if (RandomUnit() < KHAJIIT_BLEED_CHANCE) {
                    CastOn(g_forms.khajiitBleed, aggressor, victim);
                }
            }

            // Sear, on a hostile spell the caster lands, never feeding itself
            auto* spell = source ? source->As<RE::SpellItem>() : nullptr;
            if (spell && spell != g_forms.searLight && spell != g_forms.searHeavy && IsRealSpell(spell) &&
                HasPerk(aggressor, g_forms.altmerPerk) && SearOffCooldown(aggressor)) {
                const float pct = GetActorValuePercentage(aggressor, RE::ActorValue::kMagicka);
                if (pct <= ALTMER_BURN_THRESHOLD) {
                    MarkSear(aggressor);
                    CastOn(pct <= ALTMER_BURN_DEEP ? g_forms.searHeavy : g_forms.searLight, aggressor, victim);
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // The mirror of HitSink: whoever took the blow, rather than whoever landed it
    class IncomingHitSink : public RE::BSTEventSink<RE::TESHitEvent>
    {
    public:
        static IncomingHitSink* GetSingleton()
        {
            static IncomingHitSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
                                              RE::BSTEventSource<RE::TESHitEvent>*) override
        {
            if (!a_event || !g_ready) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* victim = a_event->target ? a_event->target->As<RE::Actor>() : nullptr;
            if (!victim || victim->IsDead()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (a_event->cause && a_event->cause.get() == victim) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* source = a_event->source ? RE::TESForm::LookupByID(a_event->source) : nullptr;
            auto* weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
            if (!weapon || !weapon->IsMelee()) {
                return RE::BSEventNotifyControl::kContinue;  // magic and ranged are not melee
            }

            if (!a_event->flags.all(RE::TESHitEvent::Flag::kHitBlocked) && HasPerk(victim, g_forms.orcPerk)) {
                BumpStacks(victim, g_orcStacksAV, ORC_MAX_STACKS, "Oath-Forged", "melee hit taken");
            }

            if (victim == Player()) {
                TryVelothsPyre(victim);
            }

            if (HasPerk(victim, g_forms.imperialPerk) && RandomUnit() < IMPERIAL_PROC_CHANCE) {
                CastOn(g_forms.imperialBuff, victim, victim);
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // A greater power with an unmet condition fires and quietly does nothing, and this is the only thing that says why
    class PowerCastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
    {
    public:
        static PowerCastSink* GetSingleton()
        {
            static PowerCastSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
                                              RE::BSTEventSource<RE::TESSpellCastEvent>*) override
        {
            if (!a_event || !g_ready || !a_event->spell) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* player = Player();
            if (!player || a_event->object.get() != player) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Nothing to refuse yet; every racial ability so far fires itself rather than being cast
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Fires twice per death, so only the settled one is acted on, and actorKiller is null on an unattributed kill
    class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
    {
    public:
        static DeathSink* GetSingleton()
        {
            static DeathSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
                                              RE::BSTEventSource<RE::TESDeathEvent>*) override
        {
            if (!a_event || !g_ready || !a_event->dead) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* killer = a_event->actorKiller ? a_event->actorKiller->As<RE::Actor>() : nullptr;
            if (!killer) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* dying = a_event->actorDying ? a_event->actorDying->As<RE::Actor>() : nullptr;
            logger::debug("kill: \"{}\" killed by \"{}\"", Who(dying), Who(killer));

            if (killer == Player()) {
                TryThuumRekindled(killer);
                TryPyreRenewal(killer);
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // A dodge carries the Shehai's count rather than breaking it. The tag belongs to TK Dodge RE Addon (Styyx1/TKDodgeAddon/src/AnimationEvents.cpp:40), and never arrives without it
    class DodgeSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
    {
    public:
        static DodgeSink* GetSingleton()
        {
            static DodgeSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                              RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
        {
            if (!a_event || !g_ready) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Any dodge tag, not TKDR_DodgeStart alone: the tag we are hunting has never been seen, so naming it would hide it
            if (g_animTagLogRemaining > 0 && TagLooksLikeDodge(a_event->tag)) {
                --g_animTagLogRemaining;
                logger::info("anim: \"{}\" | {} left", a_event->tag.c_str(), g_animTagLogRemaining);
            }

            // The graph takes our registration down with it, so arm the tick to attach to the next one
            if (a_event->tag == "GraphDeleting") {
                g_dodgeSinkAttached = false;
                g_dodgeAttachTries = 0;
                logger::info("Racials: animation graph deleted, dodge sink will reattach.");
                return RE::BSEventNotifyControl::kContinue;
            }

            if (a_event->tag != "TKDR_DodgeStart") {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Target 0 keeps whoever the combo was already on, so a dodge extends rather than retargets
            AdvanceShehai(Player(), 0, "dodge");
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Per graph rather than through Actor::AddAnimationGraphEventSink, which only ever adds to graphs.front() and returns false when already registered (NoahBoddie/CommonLibSSE/src/RE/A/Actor.cpp:49)
    bool AttachDodgeSink()
    {
        auto* player = Player();
        if (!player) return false;

        RE::BSAnimationGraphManagerPtr manager;
        player->GetAnimationGraphManager(manager);
        if (!manager) return false;

        std::size_t attached = 0;
        for (const auto& graph : manager->graphs) {
            if (!graph) continue;
            auto* source = graph->GetEventSource<RE::BSAnimationGraphEvent>();
            if (!source) continue;
            source->RemoveEventSink(DodgeSink::GetSingleton());
            source->AddEventSink(DodgeSink::GetSingleton());
            ++attached;
        }

        if (attached == 0) return false;

        g_dodgeSinkAttached = true;
        logger::info("Racials: dodge sink attached to {} graph(s).", attached);
        return true;
    }

    // The graph is not ready at kDataLoaded, so the tick keeps asking until one exists
    void TryAttachDodgeSink()
    {
        if (g_dodgeSinkAttached || g_dodgeAttachTries >= DODGE_ATTACH_TRIES) return;

        auto* player = Player();
        // A load screen has no player 3D to attach to, so those ticks are not failed attempts
        if (!player || !player->Is3DLoaded()) return;

        ++g_dodgeAttachTries;
        if (!AttachDodgeSink() && g_dodgeAttachTries >= DODGE_ATTACH_TRIES) {
            logger::warn("Racials: dodge sink never attached, the Shehai will not see dodges.");
        }
    }

    class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        static EquipSink* GetSingleton()
        {
            static EquipSink instance;
            return &instance;
        }

        // Queried fresh each time so it cannot desync from what is worn
        static void RefreshHands(RE::Actor* a_player)
        {
            if (!a_player || !g_forms.bareHands || !g_forms.gloved) {
                return;
            }
            if (!HasPerk(a_player, g_forms.altmerPerk)) {
                return;
            }

            const bool bare = a_player->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kHands) == nullptr;

            if (bare) {
                if (a_player->HasSpell(g_forms.gloved)) a_player->RemoveSpell(g_forms.gloved);
                if (!a_player->HasSpell(g_forms.bareHands)) a_player->AddSpell(g_forms.bareHands);
            } else {
                if (a_player->HasSpell(g_forms.bareHands)) a_player->RemoveSpell(g_forms.bareHands);
                if (!a_player->HasSpell(g_forms.gloved)) a_player->AddSpell(g_forms.gloved);
            }
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event,
                                              RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (!a_event || !g_ready) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* actor = a_event->actor ? a_event->actor->As<RE::Actor>() : nullptr;
            if (!actor) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const bool isPlayer = actor == Player();

            // baseObject is a FormID, not a pointer
            auto* base = a_event->baseObject ? RE::TESForm::LookupByID(a_event->baseObject) : nullptr;

            // Gloves on and off both matter, so this runs for unequip too. Player only, or every Altmer in a city churns on every equip
            if (isPlayer) {
                RefreshHands(actor);
                RefreshDodgeCost(actor);
            }

            if (!base || !a_event->equipped) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* potion = base->As<RE::AlchemyItem>();
            if (!potion) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Eating cannot be refused after the fact, so the penalty always fires, routed through GreenPact for one notification path
            if (isPlayer && potion->IsFood() && g_forms.veggieFoods && g_forms.veggieFoods->HasForm(potion) &&
                HasPerk(actor, g_forms.bosmerPerk) && MCP::RacialNegativesEnabled()) {
                GreenPact::NotifyOathBroken();
                CastOn(g_forms.greenPactPenalty, actor, actor);
            }

            if (g_forms.vendorItemPotion && potion->HasKeyword(g_forms.vendorItemPotion) &&
                HasPerk(actor, g_forms.argonianPerk)) {
                CastOn(g_forms.argonianBuff, actor, actor);
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

namespace Racials
{
    void LoadForms()
    {
        g_ready = false;
        g_forms = {};
        g_resolved = 0;
        g_missing = 0;

        g_forms.orcPerk = Resolve<RE::BGSPerk>("Apo_OathForgedPerk", 0x824, AB_PLUGIN);
        g_forms.bosmerPerk = Resolve<RE::BGSPerk>("Apo_GreenPactOathPerk", 0x818, AB_PLUGIN);
        g_forms.imperialPerk = Resolve<RE::BGSPerk>("Apo_SpearoftheEmperorPerk", 0x802, AB_PLUGIN);
        g_forms.khajiitPerk = Resolve<RE::BGSPerk>("Apo_JoneJodeRhythmPerk", 0x00A, AB_PLUGIN);
        g_forms.altmerPerk = Resolve<RE::BGSPerk>("Apo_BindingofAetheriusPerk", 0x808, AB_PLUGIN);
        g_forms.argonianPerk = Resolve<RE::BGSPerk>("Apo_HistsCallingPerk", 0x835, AB_PLUGIN);
        g_forms.dunmerPerk = Resolve<RE::BGSPerk>("Apo_FlameofVelothPerk", 0x80F, AB_PLUGIN);

        g_forms.imperialBuff = Resolve<RE::SpellItem>("Apo_SpearoftheEmperorBuffSpell", 0x803, AB_PLUGIN);
        g_forms.khajiitBleed = Resolve<RE::SpellItem>("Apo_JoneJodeRhythmBuffSpell01", 0x008, AB_PLUGIN);
        g_forms.argonianBuff = Resolve<RE::SpellItem>("Apo_HistsCallingBuff02Spell", 0x834, AB_PLUGIN);
        g_forms.greenPactPenalty = Resolve<RE::SpellItem>("Apo_GreenPactOathNeg01Spell", 0x810, AB_PLUGIN);
        g_forms.searLight = Resolve<RE::SpellItem>("Apo_BindingSearLightSpell", 0x010, AB_PLUGIN);
        g_forms.searHeavy = Resolve<RE::SpellItem>("Apo_BindingSearHeavySpell", 0x011, AB_PLUGIN);
        g_forms.bareHands = Resolve<RE::SpellItem>("Apo_BindingBareHandsAb", 0x00C, AB_PLUGIN);
        g_forms.gloved = Resolve<RE::SpellItem>("Apo_BindingGlovedAb", 0x00D, AB_PLUGIN);

        g_forms.veggieFoods = Resolve<RE::BGSListForm>("Apo_Bosmer_VeggieFoods", 0x81D, AB_PLUGIN);
        g_forms.bloodthirstExclusions =
            Resolve<RE::BGSListForm>("Apo_Bloodthirst_Exclusions", 0x1C, AB_PLUGIN);
        g_forms.vendorItemPotion = Resolve<RE::BGSKeyword>("VendorItemPotion", 0x8CDEC, SKYRIM_ESM);
        g_forms.weapTypeSword = Resolve<RE::BGSKeyword>("WeapTypeSword", 0x1E711, SKYRIM_ESM);
        g_forms.weapTypeBow = Resolve<RE::BGSKeyword>("WeapTypeBow", 0x1E715, SKYRIM_ESM);

        g_forms.nordPerk = Resolve<RE::BGSPerk>("Apo_AncestralValorPerk", 0x82A, AB_PLUGIN);
        g_forms.redguardPerk = Resolve<RE::BGSPerk>("Apo_AnseisGambitPerk", 0x80C, AB_PLUGIN);

        // Optional: the racial abilities are being built one race at a time, so a missing one is not an error
        g_forms.velothsPyre = RE::TESForm::LookupByEditorID<RE::SpellItem>("Apo_VelothsPyrePower");
        g_forms.velothsPyre2 = RE::TESForm::LookupByEditorID<RE::SpellItem>("Apo_VelothsPyrePower2");
        g_forms.velothsPyre3 = RE::TESForm::LookupByEditorID<RE::SpellItem>("Apo_VelothsPyrePower3");
        g_forms.thuumRekindled = RE::TESForm::LookupByEditorID<RE::SpellItem>("Apo_ThuumRekindledSpell");
        g_forms.shehai = RE::TESForm::LookupByEditorID<RE::SpellItem>("Apo_ShehaiSpell");

        g_ready = true;
        logger::info("Racials: {} forms resolved, {} missing.", g_resolved, g_missing);
    }

    void ResolveExtraValues()
    {
        g_avgReady = false;

        // Ask for the lowest interface carrying ResolveExtraValue. Requesting a higher version than the installed build returns null with nothing logged either side (API_ActorValueGenerator.cpp:89)
        auto* avg = static_cast<AVG::API::InterfaceVersion1*>(AVG::API::RequestInterface<AVG::API::InterfaceVersion3>(false));
        if (!avg) avg = AVG::API::RequestInterface<AVG::API::InterfaceVersion2>(false);
        if (!avg) avg = AVG::API::RequestInterface<AVG::API::InterfaceVersion1>(false);

        if (!avg) {
            logger::warn("Racials: Actor Value Generator did not hand out an interface, counters are inert.");
            return;
        }
        logger::info("Racials: AVG interface version {}.", static_cast<int>(avg->GetVersion()));

        // Resolve by name. ExtraValue::Resolve only remaps an id already held and returns anything <= kTotal untouched (API_ActorValueGenerator.cpp:18), so a name-built ExtraValue never resolves
        g_orcStacksAV = RE::GetActorValueIDFromName("Apo_OathForgedStacks");
        g_bosmerStacksAV = RE::GetActorValueIDFromName("Apo_GreenPactStacks");
        g_dodgeCostAV = RE::GetActorValueIDFromName("DodgeCostModifier");

        if (g_dodgeCostAV == RE::ActorValue::kNone || g_dodgeCostAV == RE::ActorValue::kTotal) {
            g_dodgeCostAV = RE::ActorValue::kNone;
            logger::info("Racials: no DodgeCostModifier value, Redguard dodges cost the usual.");
        } else {
            logger::info("Racials: dodge cost value resolved, av {}.", static_cast<int>(g_dodgeCostAV));
        }

        g_avgReady = g_orcStacksAV != RE::ActorValue::kNone && g_orcStacksAV != RE::ActorValue::kTotal &&
                     g_bosmerStacksAV != RE::ActorValue::kNone && g_bosmerStacksAV != RE::ActorValue::kTotal;

        if (g_avgReady) {
            logger::info("Racials: stacks resolved, Oath-Forged av {}, Green Pact av {}.",
                         static_cast<int>(g_orcStacksAV), static_cast<int>(g_bosmerStacksAV));
        } else {
            logger::error("Racials: AVG did not know Apo_OathForgedStacks / Apo_GreenPactStacks (got {} and {}). "
                          "Is AetherialBloodlines_AVG.toml being loaded?",
                          static_cast<int>(g_orcStacksAV), static_cast<int>(g_bosmerStacksAV));
        }
    }

    // Precision's figure is the engine's computed damage, not health removed, so an overkill still pays out in full (Precision/src/PendingHit.cpp:213-231)
    void OnPrecisionHit(const PRECISION_API::PrecisionHitData& a_hit, const RE::HitData& a_hitData)
    {
        if (!g_ready) return;

        auto* aggressor = a_hit.attacker;
        if (!aggressor || !HasPerk(aggressor, g_forms.orcPerk)) return;
        if (a_hitData.flags.any(RE::HitData::Flag::kBlocked)) return;

        auto* victim = a_hit.target ? a_hit.target->As<RE::Actor>() : nullptr;
        if (!victim || victim == aggressor || victim->IsDead()) return;

        ++g_precisionHits;

        if (a_hitData.flags.any(RE::HitData::Flag::kPowerAttack)) {
            TryBloodthirstSpend(aggressor, victim);
            return;
        }

        BumpStacks(aggressor, g_orcStacksAV, ORC_MAX_STACKS, "Oath-Forged", "melee hit dealt");

        if (!MCP::RacialNegativesEnabled()) return;
        if (ActorMatchesList(victim, g_forms.bloodthirstExclusions)) {
            logger::debug("power: Bloodthirst declined, {} is excluded", Who(victim));
            return;
        }

        const auto stacks = GetStacks(aggressor, g_orcStacksAV);
        const float restore = BloodthirstFromDamage(stacks, a_hitData.totalDamage);
        if (restore <= 0.0f) return;

        aggressor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIERS::kDamage,
                                                          RE::ActorValue::kHealth, restore);
        logger::debug("power: Bloodthirst, {} stacks returned {:.1f} health from {:.1f} damage", stacks,
                      restore, a_hitData.totalDamage);
    }

    void RegisterPrecision()
    {
        auto* api = static_cast<PRECISION_API::IVPrecision4*>(
            PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V4));
        if (!api) {
            logger::info("Racials: Precision not found, Bloodthirst uses the max health fallback.");
            return;
        }

        const auto result = api->AddPostHitCallback(SKSE::GetPluginHandle(), OnPrecisionHit);
        if (result != PRECISION_API::APIResult::OK) {
            logger::warn("Racials: Precision refused the post hit callback, staying on the fallback.");
            return;
        }

        g_precisionDriving = true;
        logger::info("Racials: Precision drives Bloodthirst, trickle scales with damage dealt.");
    }

    void OnGameLoaded()
    {
        g_outOfCombatFor = 0.0f;
        g_decayAccum = 0.0f;
        g_peaceFor = 0.0f;
        g_pyreUsedThisFight = false;
        g_pyreTier = 0;
        g_shehaiTarget = 0;
        g_shehaiCount = 0;
        g_shehaiLast = {};
        g_thuumCutThisRecovery = false;
        g_voiceLastSeen = 0.0f;
        g_lastSear.clear();

        // The load rebuilds the player's graph, so the sink is gone and the flag that says otherwise is stale (ersh1/Precision/src/main.cpp:25-30)
        g_dodgeSinkAttached = false;
        g_dodgeAttachTries = 0;
        AttachDodgeSink();

        RefreshDodgeCost(Player());
        logger::info("Racials: per-session state cleared for the loaded game.");
    }

    void Register()
    {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            logger::error("Racials: ScriptEventSourceHolder unavailable, no sinks registered.");
            return;
        }

        // Registered unconditionally, each sink early-outs on its own INI switch
        holder->AddEventSink<RE::TESCombatEvent>(CombatSink::GetSingleton());
        holder->AddEventSink<RE::TESHitEvent>(HitSink::GetSingleton());
        holder->AddEventSink<RE::TESHitEvent>(IncomingHitSink::GetSingleton());
        holder->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
        holder->AddEventSink<RE::TESSpellCastEvent>(PowerCastSink::GetSingleton());
        holder->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());

        RegisterPrecision();

        TryAttachDodgeSink();

        logger::info("Racials: event sinks registered (combat, hit x2, equip, power cast, death).");

        // The published offsets are SE and AE only, and REL::Relocate's two argument form would hand the SE one to VR unchecked
        if (REL::Module::IsVR()) {
            logger::info("Racials: stack decay skipped, the update call site is unverified on VR.");
            return;
        }

        REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(35565, 36564), REL::Relocate(0x748, 0xC26) };
        MainUpdateHook::func = SKSE::GetTrampoline().write_call<5>(target.address(), MainUpdateHook::thunk);
        logger::info("Racials: stack decay hooked onto the main update.");
    }

    Snapshot GetSnapshot()
    {
        Snapshot s;
        s.ready = g_ready;
        s.avgReady = g_avgReady;
        s.hitLogRemaining = g_hitLogRemaining;
        s.animTagLogRemaining = g_animTagLogRemaining;
        s.orcStacks = GetStacks(Player(), g_orcStacksAV);
        s.bosmerStacks = GetStacks(Player(), g_bosmerStacksAV);

        if (auto* high = VoiceBlock(Player())) {
            s.voiceBlockFound = true;
            s.voiceState = static_cast<std::int32_t>(high->voiceState.underlying());
            s.shoutWord = static_cast<std::int32_t>(high->currentShoutVariation);
            s.voiceElapsed = high->voiceTimeElapsed;
            s.voiceRecovery = high->voiceRecoveryTime;
            s.shoutWordRecovery = RecordRecoveryTime(high);
            s.shoutName = ShoutName(high);
        }
        return s;
    }

    bool PrecisionDriving()
    {
        return g_precisionDriving;
    }

    float BloodthirstDamageShare()
    {
        return MCP::BloodthirstFraction() * BLOODTHIRST_DAMAGE_SCALE;
    }

    ShehaiTuning& Shehai()
    {
        return g_shehai;
    }

    void LoadShehaiTuning()
    {
        g_shehai.hits = static_cast<std::int32_t>(Settings::GetSetting("ShehaiHits", 3.0f));
        g_shehai.window = Settings::GetSetting("ShehaiWindow", 4.0f);
        g_shehai.gap = Settings::GetSetting("ShehaiGap", 0.25f);
        logger::info("Racials: Shehai tuning, {} hits in {:.1f}s, {:.2f}s between blows.", g_shehai.hits,
                     g_shehai.window, g_shehai.gap);
    }

    void DebugSetOrcStacks(std::int32_t a_value)
    {
        SetStacks(Player(), g_orcStacksAV, a_value, ORC_MAX_STACKS, "Oath-Forged", "set from the Debug page");
    }
    void DebugSetBosmerStacks(std::int32_t a_value)
    {
        SetStacks(Player(), g_bosmerStacksAV, a_value, BOSMER_MAX_STACKS, "Green Pact", "set from the Debug page");
    }

    void StartHitLog(std::int32_t a_hits)
    {
        g_hitLogRemaining = a_hits;
        logger::info("Hit logging armed for the next {} hits.", a_hits);
    }

    void StartAnimTagLog(std::int32_t a_tags)
    {
        g_animTagLogRemaining = a_tags;
        logger::info("Dodge tag logging armed for the next {} tags.", a_tags);
    }

}
