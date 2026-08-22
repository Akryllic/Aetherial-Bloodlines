#pragma once

#include "SKSEMCP/SKSEMenuFramework.hpp"

/// Shared bits for the two SKSE Menu Framework pages.
namespace PanelUI
{
    inline constexpr ImGuiMCP::ImVec4 GOOD{ 0.35f, 0.75f, 0.40f, 1.0f };
    inline constexpr ImGuiMCP::ImVec4 BAD{ 0.85f, 0.25f, 0.25f, 1.0f };

    /// @brief Dimmed "(?)" after the previous control, showing @p a_text on hover.
    /// @note Follows ItzIvy05/IvysAltDeathMod/src/MCP.cpp:29-33.
    inline void HelpMarker(const char* a_text)
    {
        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip("%s", a_text);
        }
    }
}
