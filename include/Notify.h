#pragma once

/// CommonLibSSE-NG 6.5.0 dropped RE::DebugNotification, so it is declared here from its own relocation.
namespace Notify
{
    inline void Show(const char* a_text)
    {
        using func_t = void(const char*, const char*, bool);
        static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };
        func(a_text, nullptr, true);
    }
}
