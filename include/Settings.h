#pragma once

#include <cstdint>

namespace RE
{
    enum class ActorValue;
    class TESRace;
}

namespace Settings
{
    struct AttributeGains
    {
        std::int32_t health{ 10 };
        std::int32_t magicka{ 10 };
        std::int32_t stamina{ 10 };

        std::int32_t Get(RE::ActorValue a_actorValue) const;
    };

    void Load();
    AttributeGains GetForRace(RE::TESRace* a_race);
    const char* GetRaceEditorID(RE::TESRace* a_race);
}
