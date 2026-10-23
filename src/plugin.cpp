#include "Diagnostics.h"
#include "GreenPact.h"
#include "LevelUpHook.h"
#include "MCP.h"
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
            // First point at which forms exist. Order matters: settings decide which forms to resolve, forms decide whether the hooks install
            Settings::Load();

            LevelUpHook::Install();

            GreenPact::LoadSettings();
            GreenPact::LoadForms();
            GreenPact::Install();

            MCP::LoadForms();

            Racials::LoadForms();

            Racials::LoadShehaiTuning();
            Racials::Register();

            MCP::Register();
            Diagnostics::Register();
            break;

        // Extra value ids are assigned per save, so they have to be resolved again on every load
        case SKSE::MessagingInterface::kPostLoadGame:
            Racials::ResolveExtraValues();
            Racials::OnGameLoaded();
            if (Settings::ForceSettingsOnLoad()) {
                MCP::ApplyDefaults();
            }
            break;

        // The settings globals live in the save, so a new character has to be handed the INI's values
        case SKSE::MessagingInterface::kNewGame:
            Racials::ResolveExtraValues();
            Racials::OnGameLoaded();
            MCP::ApplyDefaults();
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

    // Sized once for every write_call, since a later request is ignored. Logging stays with SetupLog, which Init would otherwise replace. ItzIvy05/GoToBed/src/plugin.cpp:15
    SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = 1 << 6 });

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        logger::critical("Failed to register SKSE messaging listener.");
        return false;
    }

    return true;
}
