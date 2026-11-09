#include "PCH.h"
#include "GreenPact.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "CallSite.h"
#include "IniText.h"
#include "Menu.h"
#include "Notify.h"
#include "Settings.h"

namespace
{
    constexpr auto INI_PATH = "Data/SKSE/Plugins/AetherialBloodlines.ini";
    constexpr auto INI_SECTION = "greenpact";

    constexpr std::string_view AB_PLUGIN = "Aetherial Bloodlines.esp";
    constexpr std::string_view WT_PLUGIN = "Woodcutting Tweaks.esp";

    // Same text as the perk entry's EPFD label, so the fallback reads like the normal prompt
    constexpr auto FALLBACK_MESSAGE = "The Green Pact Forbids Harming Nature.";

    // A refused activation returns true, as if it was handled
    constexpr bool BLOCK_RETURNS_TRUE = true;

    struct Config {
        bool useChoppableTreeList{ true };
        bool castPenaltySpell{ false };
        float notifyCooldown{ 3.0f };
        std::vector<std::string> extraForbidden;
    };

    Config g_config;
    std::atomic<GreenPact::Mode> g_mode{ GreenPact::Mode::kBlock };

    struct Forms {
        RE::BGSListForm* forbiddenPlants{ nullptr };       // Apo_Bosmer_ForbiddenPlants        0x81E
        RE::BGSListForm* forbiddenTrees{ nullptr };        // Apo_Bosmer_ForbiddenTrees         0x017
        RE::BGSListForm* forbiddenIngredients{ nullptr };  // Apo_Bosmer_ForbiddenIngredients   0x015
        RE::BGSListForm* choppableTrees{ nullptr };        // ANDR_ChoppableTreeList,           optional
        RE::BGSPerk* oathPerk{ nullptr };                  // Apo_GreenPactOathPerk             0x818
        RE::BGSMessage* oathBrokenMsg{ nullptr };          // Apo_GreenPactOathBrokenMsg        0x016
        RE::SpellItem* penaltySpell{ nullptr };            // Apo_GreenPactOathNeg01Spell       0x810
    };

    Forms g_forms;

    std::unordered_set<const RE::TESBoundObject*> g_extraForbidden;
    std::string g_messageText{ FALLBACK_MESSAGE };
    std::chrono::steady_clock::time_point g_lastNotify{};
    bool g_ready{ false };

    // Reads the [GreenPact] section itself with the shared IniText helpers, only the mode comes through Settings
    // This project doesn't pull in an INI library

    using IniText::StripComment;
    using IniText::ToLower;
    using IniText::Trim;

    [[nodiscard]] bool ParseBool(const std::string& a_value, bool a_fallback)
    {
        const auto value = ToLower(Trim(a_value));
        if (value == "1" || value == "true" || value == "yes" || value == "on") {
            return true;
        }
        if (value == "0" || value == "false" || value == "no" || value == "off") {
            return false;
        }
        logger::warn("Green Pact: unreadable boolean '{}', keeping {}.", a_value, a_fallback);
        return a_fallback;
    }

    [[nodiscard]] float ParseFloat(const std::string& a_value, float a_fallback)
    {
        const auto text = Trim(a_value);
        float value = 0.0f;
        const auto* first = text.data();
        const auto* last = first + text.size();

        if (const auto [ptr, ec] = std::from_chars(first, last, value); ec == std::errc{} && ptr == last) {
            return std::clamp(value, 0.0f, 60.0f);
        }

        logger::warn("Green Pact: unreadable number '{}', keeping {}.", a_value, a_fallback);
        return a_fallback;
    }

    [[nodiscard]] std::vector<std::string> SplitList(const std::string& a_value)
    {
        std::vector<std::string> out;
        std::size_t start = 0;

        while (start <= a_value.size()) {
            const auto comma = a_value.find(',', start);
            const auto piece =
                Trim(a_value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!piece.empty()) {
                out.push_back(piece);
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }

        return out;
    }

    // Editor ID first, the plugin and local FormID as the fallback
    // Optional forms just log a miss, same as Seb263/SkyrimSE_UnpoisonedBlocking/src/Initialization.hpp:34-36
    template <class T>
    [[nodiscard]] T* ResolveForm(std::string_view a_editorID, RE::FormID a_localID, std::string_view a_plugin,
                                 bool a_optional)
    {
        if (auto* byEditorID = RE::TESForm::LookupByEditorID<T>(a_editorID)) {
            logger::debug("Green Pact: resolved {} by editor ID -> {:08X}", a_editorID, byEditorID->GetFormID());
            return byEditorID;
        }

        if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
            if (auto* form = dataHandler->LookupForm(a_localID, a_plugin)) {
                if (auto* typed = form->As<T>()) {
                    logger::debug("Green Pact: resolved {} by {}|{:06X} -> {:08X}", a_editorID, a_plugin, a_localID,
                                  typed->GetFormID());
                    return typed;
                }
                logger::error("Green Pact: {}|{:06X} exists but is the wrong form type for {}.", a_plugin, a_localID,
                              a_editorID);
                return nullptr;
            }
        }

        if (a_optional) {
            logger::info("Green Pact: optional form {} not found ({}|{:06X}). Continuing without it.", a_editorID,
                         a_plugin, a_localID);
        } else {
            logger::error("Green Pact: REQUIRED form {} not found ({}|{:06X}). Enforcement disabled.", a_editorID,
                          a_plugin, a_localID);
        }

        return nullptr;
    }

    void CacheMessageText()
    {
        g_messageText = FALLBACK_MESSAGE;

        auto* msg = g_forms.oathBrokenMsg;
        if (!msg) {
            return;
        }

        // BGSMessage is a TESDescription, and GetDescription's default field type 'CSED' is just DESC written backwards
        RE::BSString description;
        msg->GetDescription(description, msg);

        if (description.size() > 0) {
            g_messageText = description.c_str();
            return;
        }

        if (const auto* full = msg->GetFullName(); full && *full) {
            g_messageText = full;
            return;
        }

        logger::warn("Green Pact: Apo_GreenPactOathBrokenMsg has no DESC or FULL text. Using the built-in string.");
    }

    [[nodiscard]] bool InList(const RE::BGSListForm* a_list, const RE::TESForm* a_form)
    {
        // HasForm checks the list's own forms and anything a script added, same use as vinymayan/Dynamic-Forms-Generator/src/Manager.cpp:5104
        return a_list && a_form && a_list->HasForm(a_form);
    }

    [[nodiscard]] bool IsForbidden(const RE::TESBoundObject* a_base, const RE::TESBoundObject* a_produce)
    {
        // 1. base form is a forbidden plant or tree
        if (InList(g_forms.forbiddenPlants, a_base) || InList(g_forms.forbiddenTrees, a_base)) {
            return true;
        }

        // 2. anything on Woodcutting Tweaks' own choppable list, so trees it adds are covered too
        if (g_config.useChoppableTreeList && InList(g_forms.choppableTrees, a_base)) {
            return true;
        }

        // 3. chopping blocks and the sawmill, listed in the INI so they can change without a rebuild
        if (!g_extraForbidden.empty() && g_extraForbidden.contains(a_base)) {
            return true;
        }

        // 4. anything that yields a forbidden ingredient
        if (a_produce && InList(g_forms.forbiddenIngredients, a_produce)) {
            return true;
        }

        // 5. If you're reading this, I forgot to add NOTWL and Aspen Trees....

        return false;
    }

    [[nodiscard]] RE::Actor* GetOathBoundActor(RE::TESObjectREFR* a_activatorRef)
    {
        if (!a_activatorRef || !g_forms.oathPerk) {
            return nullptr;
        }

        auto* actor = a_activatorRef->As<RE::Actor>();
        if (!actor) {
            return nullptr;
        }

        // We test the activator, not the player, so follower Bosmer are bound too
        // Why? Auri. And other bosmer followers
        return actor->HasPerk(g_forms.oathPerk) ? actor : nullptr;
    }

    void CastPenalty(RE::Actor* a_actor)
    {
        if (!g_forms.penaltySpell || !a_actor) {
            return;
        }
        if (auto* caster = a_actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
            caster->CastSpellImmediate(g_forms.penaltySpell, false, a_actor, 1.0f, false, 0.0f, a_actor);
        }
    }

    void AnnounceRefusal(RE::Actor* a_actor, bool a_castSpell)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<float>(now - g_lastNotify).count();

        // The hook fires on every press, so the message needs a cooldown
        if (g_lastNotify.time_since_epoch().count() != 0 && elapsed < g_config.notifyCooldown) {
            return;
        }

        g_lastNotify = now;

        // Notify::Show also passes the engine's cancel-if-already-queued flag, on top of the cooldown
        Notify::Show(g_messageText.c_str());

        if (a_castSpell) {
            CastPenalty(a_actor);
        }
    }

    [[nodiscard]] bool Refuse(const RE::TESBoundObject* a_base, const RE::TESBoundObject* a_produce,
                              RE::TESObjectREFR* a_activatorRef)
    {
        if (!g_ready || !a_base) {
            return false;
        }

        // The other drawbacks check this switch through a record condition, this one has to ask in code
        if (!Menu::RacialNegativesEnabled() || g_mode == GreenPact::Mode::kOff) {
            return false;
        }

        // Rule out most activations early, this runs on every one
        auto* actor = GetOathBoundActor(a_activatorRef);
        if (!actor) {
            return false;
        }

        if (!IsForbidden(a_base, a_produce)) {
            return false;
        }

        if (g_mode == GreenPact::Mode::kPenaltyOnly) {
            logger::debug("Green Pact: penalised {:08X} (produce {:08X}) for {:08X}", a_base->GetFormID(),
                          a_produce ? a_produce->GetFormID() : 0, actor->GetFormID());
            // The harvest goes through here, so the penalty can't wait out the message cooldown or a quick second pick would be free
            CastPenalty(actor);
            AnnounceRefusal(actor, false);
            return false;
        }

        logger::debug("Green Pact: refused {:08X} (produce {:08X}) for {:08X}", a_base->GetFormID(),
                      a_produce ? a_produce->GetFormID() : 0, actor->GetFormID());

        AnnounceRefusal(actor, g_config.castPenaltySpell);
        return true;
    }

    // Activate is vfunc 0x37 on TESFlora, TESObjectTREE, TESObjectACTI and TESFurniture, each with its own vtable
    // Same vtable hook as vinymayan/Book-of-Perks/src/BookManager.cpp:26-43,408-409
    // Not a TESActivateEvent sink, kStop there only stops the other sinks and the activation still happens
    template <class TForm, bool HasProduce>
    struct ActivateHook {
        static bool thunk(TForm* a_self, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef,
                          std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_targetCount)
        {
            // produceItem is a plain member on both TESFlora and TESObjectTREE, so a_self reaches it without a cast
            const RE::TESBoundObject* produce = nullptr;
            if constexpr (HasProduce) {
                produce = a_self ? a_self->produceItem : nullptr;
            }

            if (Refuse(a_self, produce, a_activatorRef)) {
                return BLOCK_RETURNS_TRUE;
            }

            return func(a_self, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        static inline REL::Relocation<decltype(thunk)> func;
    };

    template <class TForm, bool HasProduce>
    void InstallOne(std::string_view a_name)
    {
        // VTABLE[0] is the object's main vtable, the others are for its base classes
        // All four come with SE and AE ids plus a VR offset, so no hand written offsets are needed
        REL::Relocation<std::uintptr_t> vtbl{ TForm::VTABLE[0] };
        ActivateHook<TForm, HasProduce>::func = vtbl.write_vfunc(0x37, ActivateHook<TForm, HasProduce>::thunk);
        logger::info("Green Pact: hooked {}::Activate (vfunc 0x37).", a_name);
    }

    // Would the player be refused this reference? Same rules as Refuse, minus the notification and the cast
    [[nodiscard]] bool WouldRefusePlayer(RE::TESObjectREFR* a_ref)
    {
        if (!g_ready || !a_ref || !Menu::RacialNegativesEnabled() || g_mode != GreenPact::Mode::kBlock) {
            return false;
        }
        if (!GetOathBoundActor(RE::PlayerCharacter::GetSingleton())) {
            return false;
        }

        auto* base = a_ref->GetBaseObject();
        if (!base) {
            return false;
        }

        const RE::TESBoundObject* produce = nullptr;
        if (auto* flora = base->As<RE::TESFlora>()) {
            produce = flora->produceItem;
        } else if (auto* tree = base->As<RE::TESObjectTREE>()) {
            produce = tree->produceItem;
        }

        return IsForbidden(base, produce);
    }

    template <class T>
    void WriteThunkCall(std::uintptr_t a_src)
    {
        auto& trampoline = SKSE::GetTrampoline();
        T::func = trampoline.write_call<5>(a_src, T::thunk);
    }

    // The activation prompt is a HUDData message whose text is the prompt, name and tag on separate lines
    // Done the same way as Styyx1/ImmersiveActivate/src/Manager.cpp:38-117
    struct SendHUDMessage {
        static void thunk(RE::UIMessageQueue* a_this, const RE::BSFixedString& a_menuName, RE::UI_MESSAGE_TYPE a_type,
                          RE::IUIMessageData* a_data)
        {
            auto* data = a_data ? static_cast<RE::HUDData*>(a_data) : nullptr;
            const auto crossHairRef = data ? data->crosshairRef.get() : RE::TESObjectREFRPtr();

            if (data && crossHairRef && WouldRefusePlayer(crossHairRef.get())) {
                // Replace only the prompt line, so the object keeps its name and any owned tag
                std::string text = data->text.c_str() ? data->text.c_str() : "";
                const auto firstBreak = text.find('\n');
                if (firstBreak != std::string::npos) {
                    text = g_messageText + text.substr(firstBreak);
                } else {
                    text = g_messageText;
                }

                data->text = text.c_str();
                data->type = RE::HUD_MESSAGE_TYPE::kSetCrosshairTargetTextOnly;  // no button, because pressing it does nothing
            }

            func(a_this, a_menuName, a_type, a_data);
        }

        static inline REL::Relocation<decltype(thunk)> func;
    };
}

namespace GreenPact
{
    Mode CurrentMode()
    {
        return g_mode;
    }

    void SetMode(Mode a_mode)
    {
        g_mode = a_mode;
    }

    void NotifyOathBroken()
    {
        // Shares AnnounceRefusal's cooldown so eating and harvesting cannot talk over each other
        AnnounceRefusal(nullptr, false);
    }

    std::string DescribeTarget(RE::TESObjectREFR* a_ref)
    {
        if (!g_ready) {
            return "Green Pact not ready (required forms missing).";
        }
        if (!a_ref) {
            return "Nothing under the crosshair.";
        }

        auto* base = a_ref->GetBaseObject();
        if (!base) {
            return "Reference has no base object.";
        }

        // Read produce off the concrete type, as the hook thunks do
        const RE::TESBoundObject* produce = nullptr;
        if (auto* flora = base->As<RE::TESFlora>()) {
            produce = flora->produceItem;
        } else if (auto* tree = base->As<RE::TESObjectTREE>()) {
            produce = tree->produceItem;
        }

        const auto nameOf = [](const RE::TESForm* a_form) -> const char* {
            const auto* n = a_form ? a_form->GetName() : nullptr;
            return (n && *n) ? n : "<no name>";
        };

        const bool inPlants = InList(g_forms.forbiddenPlants, base);
        const bool inTrees = InList(g_forms.forbiddenTrees, base);
        const bool inChoppable = g_config.useChoppableTreeList && InList(g_forms.choppableTrees, base);
        const bool inExtra = g_extraForbidden.contains(base);
        const bool produceForbidden = produce && InList(g_forms.forbiddenIngredients, produce);

        std::vector<std::string> lines;
        lines.push_back(std::format("{} [{:08X}] formType {}", nameOf(base), base->GetFormID(),
                                    static_cast<int>(base->GetFormType())));
        lines.push_back(std::format("  ForbiddenPlants      : {}", inPlants ? "yes" : "no"));
        lines.push_back(std::format("  ForbiddenTrees       : {}", inTrees ? "yes" : "no"));
        lines.push_back(std::format("  ANDR_ChoppableTrees  : {}", inChoppable ? "yes" : "no"));
        lines.push_back(std::format("  ExtraForbiddenForms  : {}", inExtra ? "yes" : "no"));

        if (produce) {
            lines.push_back(std::format("  produce (PFIG)       : {} [{:08X}] -> {}", nameOf(produce),
                                        produce->GetFormID(), produceForbidden ? "FORBIDDEN" : "allowed"));
        } else {
            lines.push_back("  produce (PFIG)       : none");
        }

        const bool forbidden = inPlants || inTrees || inChoppable || inExtra || produceForbidden;
        std::string verdict = forbidden ? "  => would be REFUSED" : "  => would be ALLOWED";
        if (forbidden && !Menu::RacialNegativesEnabled()) {
            verdict += " (but racial drawbacks are off, so it is allowed)";
        } else if (forbidden && g_mode == Mode::kOff) {
            verdict += " (but Green Pact mode is Off, so it is allowed)";
        } else if (forbidden && g_mode == Mode::kPenaltyOnly) {
            verdict = "  => would be ALLOWED with Penalty Spell";
        }
        lines.push_back(verdict);

        std::string out;
        for (const auto& line : lines) {
            out += line;
            out += "\n";
        }
        return out;
    }

    void LoadSettings()
    {
        g_config = {};
        g_extraForbidden.clear();
        const int modeInt = std::clamp(static_cast<int>(Settings::GetSetting("GreenPactMode", 0.0f)), 0, 2);
        g_mode = static_cast<Mode>(modeInt);

        std::ifstream file(INI_PATH);
        if (!file.is_open()) {
            logger::warn("Green Pact: {} not found. Using defaults.", INI_PATH);
            return;
        }

        std::string section;
        std::string line;
        while (std::getline(file, line)) {
            line = StripComment(std::move(line));
            if (line.empty()) {
                continue;
            }

            if (line.front() == '[' && line.back() == ']') {
                section = ToLower(Trim(line.substr(1, line.size() - 2)));
                continue;
            }

            if (section != INI_SECTION) {
                continue;
            }

            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }

            const auto key = ToLower(Trim(line.substr(0, eq)));
            const auto value = Trim(line.substr(eq + 1));

            if (key == "busechoppabletreelist") {
                g_config.useChoppableTreeList = ParseBool(value, g_config.useChoppableTreeList);
            } else if (key == "bcastpenaltyspell") {
                g_config.castPenaltySpell = ParseBool(value, g_config.castPenaltySpell);
            } else if (key == "fnotifycooldown") {
                g_config.notifyCooldown = ParseFloat(value, g_config.notifyCooldown);
            } else if (key == "extraforbiddenforms") {
                g_config.extraForbidden = SplitList(value);
            } else {
                logger::warn("Green Pact: unknown INI key ignored: {}", key);
            }
        }

        logger::info("Green Pact settings: mode={} choppableTreeList={} penaltySpell={} cooldown={}s extraForms={}",
                     modeInt, g_config.useChoppableTreeList, g_config.castPenaltySpell, g_config.notifyCooldown,
                     g_config.extraForbidden.size());
    }

    void LoadForms()
    {
        g_ready = false;
        g_forms = {};
        g_extraForbidden.clear();

        g_forms.forbiddenPlants = ResolveForm<RE::BGSListForm>("Apo_Bosmer_ForbiddenPlants", 0x81E, AB_PLUGIN, false);
        g_forms.forbiddenTrees = ResolveForm<RE::BGSListForm>("Apo_Bosmer_ForbiddenTrees", 0x017, AB_PLUGIN, false);
        g_forms.forbiddenIngredients =
            ResolveForm<RE::BGSListForm>("Apo_Bosmer_ForbiddenIngredients", 0x015, AB_PLUGIN, false);
        g_forms.oathPerk = ResolveForm<RE::BGSPerk>("Apo_GreenPactOathPerk", 0x818, AB_PLUGIN, false);

        g_forms.oathBrokenMsg = ResolveForm<RE::BGSMessage>("Apo_GreenPactOathBrokenMsg", 0x016, AB_PLUGIN, true);
        g_forms.penaltySpell = ResolveForm<RE::SpellItem>("Apo_GreenPactOathNeg01Spell", 0x810, AB_PLUGIN, true);

        if (g_config.useChoppableTreeList) {
            g_forms.choppableTrees = ResolveForm<RE::BGSListForm>("ANDR_ChoppableTreeList", 0x807, WT_PLUGIN, true);
        }

        for (const auto& editorID : g_config.extraForbidden) {
            if (auto* form = RE::TESForm::LookupByEditorID<RE::TESBoundObject>(editorID)) {
                g_extraForbidden.insert(form);
                logger::debug("Green Pact: extra forbidden form {} -> {:08X}", editorID, form->GetFormID());
            } else {
                logger::warn(
                    "Green Pact: extra forbidden form '{}' not found. Requires powerofthree's Tweaks for editor-ID "
                    "lookup.",
                    editorID);
            }
        }

        CacheMessageText();

        const bool haveRequired =
            g_forms.forbiddenPlants && g_forms.forbiddenTrees && g_forms.forbiddenIngredients && g_forms.oathPerk;
        if (!haveRequired) {
            logger::error("Green Pact: required forms missing, enforcement will not run.");
            return;
        }

        g_ready = true;
        logger::info("Green Pact forms loaded: plants={} trees={} ingredients={} choppable={} extra={} msg=\"{}\"",
                     g_forms.forbiddenPlants->forms.size(), g_forms.forbiddenTrees->forms.size(),
                     g_forms.forbiddenIngredients->forms.size(),
                     g_forms.choppableTrees ? g_forms.choppableTrees->forms.size() : 0, g_extraForbidden.size(),
                     g_messageText);
    }

    void Install()
    {
        if (!g_ready) {
            logger::info("Green Pact: not ready, hooks not installed.");
            return;
        }

        InstallOne<RE::TESFlora, true>("TESFlora");
        InstallOne<RE::TESObjectTREE, true>("TESObjectTREE");
        InstallOne<RE::TESObjectACTI, false>("TESObjectACTI");
        InstallOne<RE::TESFurniture, false>("TESFurniture");

        // The published id and offsets only cover SE and AE, and REL::Relocate with two arguments would quietly use the SE offset on VR
        // If you're reading this, feel free to make it compatible with VR
        if (REL::Module::IsVR()) {
            logger::info("Green Pact: crosshair prompt skipped, the call site offset is unverified on VR.");
        } else {
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(39535, 40621), REL::Relocate(0x289, 0x280) };
            const auto site = target.address();
            const auto dest = CallSite::Destination(site);
            // Another plugin may have hooked this call already (Styyx1/ImmersiveActivate/src/Manager.cpp:115), so the UIMessageQueue load right before it is what proves it's the right spot
            const bool anchored = CallSite::RcxLoadEndingAt(site) == REL::ID(400445).address();
            if (CallSite::Hookable(dest, REL::ID(13631).address(), anchored)) {
                const bool chained = !CallSite::InExe(dest);
                WriteThunkCall<SendHUDMessage>(site);
                logger::info("Green Pact: hooked the crosshair prompt{}.",
                             chained ? ", behind another plugin's hook" : "");
            } else {
                logger::error("Green Pact: crosshair prompt skipped, SkyrimSE.exe+{:X} is not a verified HUD "
                              "message call.",
                              site - REL::Module::get().base());
            }
        }

        logger::info("Green Pact enforcement active.");
    }
}
