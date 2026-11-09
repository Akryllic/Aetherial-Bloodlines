#pragma once

namespace Menu
{
    void LoadForms();
    void Register();
    void ApplyDefaults();

    [[nodiscard]] bool RacialNegativesEnabled();
    [[nodiscard]] bool DrawbackMasteryEnabled();
    [[nodiscard]] float PyreHealthThreshold();
    [[nodiscard]] float ThuumCutFraction();
    [[nodiscard]] float BloodthirstFraction();
}
