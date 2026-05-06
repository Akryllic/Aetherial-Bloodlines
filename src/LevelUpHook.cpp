#include "LevelUpHook.h"
#include "Settings.h"

namespace
{
    using Message = RE::IMessageBoxCallback::Message;

    class ConfirmLevelUpAttributeCallbackLayout : public RE::IMessageBoxCallback
    {
    public:
        ~ConfirmLevelUpAttributeCallbackLayout() override = default;
        void Run(Message) override {}

        RE::LevelUpMenu* menu;
        RE::ActorValue actorValue;
    };

    using Run_t = void (*)(ConfirmLevelUpAttributeCallbackLayout*, Message);

    REL::Relocation<Run_t> g_originalRun;
    bool g_installed = false;

    [[nodiscard]] bool IsHandledAttribute(RE::ActorValue a_actorValue)
    {
        switch (a_actorValue) {
        case RE::ActorValue::kHealth:
        case RE::ActorValue::kMagicka:
        case RE::ActorValue::kStamina:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] const char* ActorValueName(RE::ActorValue a_actorValue)
    {
        switch (a_actorValue) {
        case RE::ActorValue::kHealth:
            return "Health";
        case RE::ActorValue::kMagicka:
            return "Magicka";
        case RE::ActorValue::kStamina:
            return "Stamina";
        default:
            return "Unknown";
        }
    }

    [[nodiscard]] RE::Setting* GetLevelUpGainSetting()
    {
        const auto settings = RE::GameSettingCollection::GetSingleton();
        return settings ? settings->GetSetting("iAVDhmsLevelup") : nullptr;
    }

    [[nodiscard]] std::int32_t GetRaceGain(RE::ActorValue a_actorValue)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return 10;
        }

        const auto race = player->GetRace();
        const auto gains = Settings::GetForRace(race);
        return gains.Get(a_actorValue);
    }

    void RunHook(ConfirmLevelUpAttributeCallbackLayout* a_this, Message a_msg)
    {
        if (!a_this || !IsHandledAttribute(a_this->actorValue)) {
            g_originalRun(a_this, a_msg);
            return;
        }

        const auto setting = GetLevelUpGainSetting();
        if (!setting) {
            logger::warn("iAVDhmsLevelup not found. Running vanilla level-up callback.");
            g_originalRun(a_this, a_msg);
            return;
        }

        const auto oldValue = setting->GetSInt();
        const auto newValue = GetRaceGain(a_this->actorValue);

        // Clean path: change what vanilla reads before it applies the level-up.
        // No cache, no after-the-fact add, no subtract correction.
        setting->data.i = newValue;

        logger::debug("Level-up {} gain override: {} -> {}", ActorValueName(a_this->actorValue), oldValue, newValue);

        g_originalRun(a_this, a_msg);

        setting->data.i = oldValue;
    }
}

namespace LevelUpHook
{
    void Install()
    {
        if (g_installed) {
            return;
        }

        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE___ConfirmLevelUpAttributeCallback[0] };
        g_originalRun = vtbl.write_vfunc(0x1, RunHook);

        g_installed = true;
        logger::info("Installed ConfirmLevelUpAttributeCallback::Run hook.");
    }
}
