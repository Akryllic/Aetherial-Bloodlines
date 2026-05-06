#pragma once

#include <cstdint>

namespace Settings
{
    struct AttributeGains
    {
        std::int32_t health{ 10 };
        std::int32_t magicka{ 10 };
        std::int32_t stamina{ 10 };

        [[nodiscard]] std::int32_t Get(RE::ActorValue a_actorValue) const;
    };

    void Load();

    [[nodiscard]] AttributeGains GetForRace(RE::TESRace* a_race);
    [[nodiscard]] const char* GetRaceEditorID(RE::TESRace* a_race);
}
