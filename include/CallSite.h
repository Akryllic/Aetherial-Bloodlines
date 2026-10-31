#pragma once

#include <cstring>

namespace CallSite
{
    [[nodiscard]] inline std::uintptr_t Destination(std::uintptr_t a_site)
    {
        if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8) return 0;

        std::int32_t rel{};
        std::memcpy(&rel, reinterpret_cast<const void*>(a_site + 1), sizeof(rel));
        return a_site + 5 + static_cast<std::intptr_t>(rel);
    }

    // The global read by a mov rcx, [rip + rel32] that ends exactly at a_end, or 0 when those 7 bytes are not one
    [[nodiscard]] inline std::uintptr_t RcxLoadEndingAt(std::uintptr_t a_end)
    {
        const auto* insn = reinterpret_cast<const std::uint8_t*>(a_end - 7);
        if (insn[0] != 0x48 || insn[1] != 0x8B || insn[2] != 0x0D) return 0;

        std::int32_t rel{};
        std::memcpy(&rel, insn + 3, sizeof(rel));
        return a_end + static_cast<std::intptr_t>(rel);
    }

    [[nodiscard]] inline bool InExe(std::uintptr_t a_address)
    {
        const auto text = REL::Module::get().segment(REL::Segment::textx);
        return a_address >= text.address() && a_address < text.address() + text.size();
    }

    // A plugin that hooked the call first left a trampoline in free memory outside the exe, so only the anchor can vouch for the site
    [[nodiscard]] inline bool Hookable(std::uintptr_t a_dest, std::uintptr_t a_callee, bool a_anchored)
    {
        return a_dest == a_callee || (a_dest && !InExe(a_dest) && a_anchored);
    }
}
