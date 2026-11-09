#include "GreenPact.h"
#include "LevelUpHook.h"
#include "Menu.h"
#include "Racials.h"
#include "Settings.h"
#include "logger.h"

namespace
{
    void OnMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message)
            return;

        switch (a_message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            // Forms exist from here on
            // Order matters, the settings decide which forms to look up and the forms decide whether the hooks go in
            Settings::Load();

            LevelUpHook::Install();

            GreenPact::LoadSettings();
            GreenPact::LoadForms();
            GreenPact::Install();

            Menu::LoadForms();

            Racials::LoadForms();

            Racials::LoadShehaiTuning();
            Racials::Register();

            Menu::Register();
            break;

        // Extra value ids are assigned per save, so they have to be resolved again on every load
        case SKSE::MessagingInterface::kPostLoadGame:
            Racials::ResolveExtraValues();
            Racials::OnGameLoaded();
            if (Settings::ForceSettingsOnLoad()) {
                Menu::ApplyDefaults();
            }
            break;

        // The setting globals are saved with the game, so a new character has to get the INI's values here
        case SKSE::MessagingInterface::kNewGame:
            Racials::ResolveExtraValues();
            Racials::OnGameLoaded();
            Menu::ApplyDefaults();
            break;

        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SetupLog();
    logger::info("Plugin loaded");

    // Trampoline gets sized here in Init for every write_call, same as ItzIvy05/GoToBed/src/plugin.cpp:15
    // Asking for more later does nothing, CommonLibSSE-NG only sets it up once
    // log = false keeps our SetupLog logger, Init would replace it otherwise
    SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = 1 << 6 });

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        logger::critical("Failed to register SKSE messaging listener.");
        return false;
    }

    return true;
}
