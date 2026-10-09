#include "Theme.h"

namespace scopedeck
{

namespace
{
inline ImVec4 Col(ImU32 p_Packed)
{
    return ImGui::ColorConvertU32ToFloat4(p_Packed);
}
}

void ApplyTheme()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    // Square corners throughout. A scope deck is a grid of instruments; rounded
    // panels waste edge pixels and make adjacent scopes harder to compare across
    // a shared boundary.
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = 2.0f;
    style.PopupRounding     = 2.0f;
    style.TabRounding       = 0.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding      = 2.0f;

    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;

    // Tight padding: every pixel spent on chrome is a pixel not spent on trace.
    style.WindowPadding     = ImVec2(6.0f, 6.0f);
    style.FramePadding      = ImVec2(6.0f, 3.0f);
    style.ItemSpacing       = ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing  = ImVec2(4.0f, 4.0f);

    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]            = Col(kColPanel);
    c[ImGuiCol_ChildBg]             = Col(kColBackground);
    c[ImGuiCol_PopupBg]             = Col(kColPanelHeader);
    c[ImGuiCol_Border]              = Col(kColBorder);

    c[ImGuiCol_FrameBg]             = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.19f, 0.21f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.23f, 0.26f, 0.30f, 1.00f);

    c[ImGuiCol_TitleBg]             = Col(kColPanelHeader);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]    = Col(kColPanelHeader);

    c[ImGuiCol_MenuBarBg]           = Col(kColPanelHeader);

    c[ImGuiCol_Tab]                 = Col(kColPanelHeader);
    c[ImGuiCol_TabHovered]          = ImVec4(0.24f, 0.28f, 0.33f, 1.00f);
    c[ImGuiCol_TabSelected]         = ImVec4(0.20f, 0.23f, 0.27f, 1.00f);
    c[ImGuiCol_TabDimmed]           = Col(kColPanelHeader);
    c[ImGuiCol_TabDimmedSelected]   = ImVec4(0.17f, 0.19f, 0.22f, 1.00f);

    c[ImGuiCol_Header]              = ImVec4(0.20f, 0.23f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.25f, 0.29f, 0.34f, 1.00f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.28f, 0.33f, 0.39f, 1.00f);

    c[ImGuiCol_Button]              = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.26f, 0.29f, 0.34f, 1.00f);
    c[ImGuiCol_ButtonActive]        = ImVec4(0.31f, 0.35f, 0.41f, 1.00f);

    c[ImGuiCol_SliderGrab]          = ImVec4(0.45f, 0.52f, 0.62f, 1.00f);
    c[ImGuiCol_SliderGrabActive]    = ImVec4(0.55f, 0.63f, 0.74f, 1.00f);
    c[ImGuiCol_CheckMark]           = ImVec4(0.60f, 0.72f, 0.90f, 1.00f);

    c[ImGuiCol_Separator]           = Col(kColBorder);
    c[ImGuiCol_ResizeGrip]          = ImVec4(0.30f, 0.34f, 0.40f, 0.50f);
    c[ImGuiCol_ResizeGripHovered]   = ImVec4(0.40f, 0.46f, 0.54f, 0.70f);

    c[ImGuiCol_Text]                = ImVec4(0.86f, 0.88f, 0.91f, 1.00f);
    c[ImGuiCol_TextDisabled]        = ImVec4(0.45f, 0.48f, 0.53f, 1.00f);

    c[ImGuiCol_DockingPreview]      = ImVec4(0.35f, 0.45f, 0.60f, 0.60f);
    c[ImGuiCol_DockingEmptyBg]      = Col(kColBackground);
}

} // namespace scopedeck
