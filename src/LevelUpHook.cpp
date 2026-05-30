#include "PCH.h"
#include "LevelUpHook.h"
#include "Settings.h"

namespace
{
    using Callback = RE::LevelUpMenu::ConfirmLevelUpAttributeCallback;
    using Message = RE::IMessageBoxCallback::Message;
    using RunFn = void(Callback*, Message);

    REL::Relocation<RunFn> g_originalRun;

    std::int32_t GetRaceGain(RE::ActorValue a_av)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return 10;
        return Settings::GetForRace(player->GetRace()).Get(a_av);
    }

    void RunHook(Callback* a_this, Message a_msg)
    {
        const auto av = a_this ? a_this->actorValue : RE::ActorValue::kNone;
        const bool isConfirm = (a_msg == Message::kUnk0);
        const bool handled =
            av == RE::ActorValue::kHealth ||
            av == RE::ActorValue::kMagicka ||
            av == RE::ActorValue::kStamina;

        if (!isConfirm || !handled) {
            g_originalRun(a_this, a_msg);
            return;
        }

        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            g_originalRun(a_this, a_msg);
            return;
        }

        // Snapshot base value before vanilla runs
        const float before = player->AsActorValueOwner()->GetBaseActorValue(av);

        // Let vanilla run normally so menu closes
        g_originalRun(a_this, a_msg);

        // Snapshot after — vanilla gain = after - before
        const float after = player->AsActorValueOwner()->GetBaseActorValue(av);
        const float vanillaGain = after - before;

        // Undo vanilla gain, apply ours
        const float ourGain = static_cast<float>(GetRaceGain(av));
        player->AsActorValueOwner()->ModActorValue(av, ourGain - vanillaGain);

        logger::info("Level-up: vanilla={} ours={} net={}", vanillaGain, ourGain, ourGain - vanillaGain);
    }
}

namespace LevelUpHook
{
    void Install()
    {
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE___ConfirmLevelUpAttributeCallback[0] };
        g_originalRun = vtbl.write_vfunc(0x1, RunHook);
        logger::info("LevelUpHook installed.");
    }
}
