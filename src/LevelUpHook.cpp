#include "PCH.h"
#include "LevelUpHook.h"
#include "Settings.h"

#include "RE/C/ConfirmLevelUpAttributeCallback.h"

namespace
{
    using Callback = RE::ConfirmLevelUpAttributeCallback;
    using RunFn = void(Callback*, std::uint8_t);

    // The old Message enum had kUnk0 = 0 for confirm
    constexpr std::uint8_t CONFIRM = 0;

    REL::Relocation<RunFn> g_originalRun;

    std::int32_t GetRaceGain(RE::ActorValue a_av)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return 10;
        return Settings::GetForRace(player->GetRace()).Get(a_av);
    }

    void RunHook(Callback* a_this, std::uint8_t a_button)
    {
        const auto av = a_this ? a_this->actorValue : RE::ActorValue::kNone;
        const bool isConfirm = (a_button == CONFIRM);
        const bool handled =
            av == RE::ActorValue::kHealth ||
            av == RE::ActorValue::kMagicka ||
            av == RE::ActorValue::kStamina;

        if (!isConfirm || !handled) {
            g_originalRun(a_this, a_button);
            return;
        }

        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            g_originalRun(a_this, a_button);
            return;
        }

        // Snapshot base value before vanilla runs
        const float before = player->AsActorValueOwner()->GetBaseActorValue(av);

        // Let vanilla run normally so menu closes
        g_originalRun(a_this, a_button);

        // Snapshot after, vanilla gain is after minus before
        const float after = player->AsActorValueOwner()->GetBaseActorValue(av);
        const float vanillaGain = after - before;

        // Undo vanilla gain, apply ours
        const float ourGain = static_cast<float>(GetRaceGain(av));
        player->AsActorValueOwner()->ModBaseActorValue(av, ourGain - vanillaGain);

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
