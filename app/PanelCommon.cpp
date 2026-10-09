#include "PanelCommon.h"

#include "imgui_internal.h"

#include <cmath>
#include <cstdio>

namespace scopedeck
{

namespace
{
// Label column and control widths, in multiples of the font size rather than in
// fixed pixels.
//
// Fixed pixels were wrong: the font scales with the monitor's DPI, so at 150% a
// 130px column clipped "Enhanced Render" and drew its checkbox on top of the
// label. Anything sized against text has to be sized in text units.
float LabelWidth()   { return ImGui::GetFontSize() * 11.0f; }
float ControlWidth() { return ImGui::GetFontSize() * 16.0f; }
}

ImRect ZoomPan::Apply(const ImRect& p_Base) const
{
    const ImVec2 size(p_Base.GetWidth() * zoom, p_Base.GetHeight() * zoom);
    const ImVec2 lo(p_Base.Min.x + pan.x, p_Base.Min.y + pan.y);
    return ImRect(lo, ImVec2(lo.x + size.x, lo.y + size.y));
}

void ZoomPan::HandleInput(const ImRect& p_Base, bool p_Enabled)
{
    if (!p_Enabled)
    {
        // Switching Zoomable off leaves a zoomed panel stuck at whatever it was,
        // with no way to get back - so the checkbox also resets.
        Reset();
        return;
    }

    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    if (io.MouseWheel != 0.0f && p_Base.Contains(mouse))
    {
        // One 1.15x step per wheel notch - by the amount scrolled, not just
        // its direction. Every notch since the last frame arrives summed in
        // io.MouseWheel, and taking only its sign turned two or three notches
        // into one step; on a slow frame (the host busy on the GPU, or the app
        // just brought back to the front) zooming out barely moved, which read
        // as it having stopped working. Fractional on high-resolution wheels
        // and touchpads, which then zoom smoothly instead of in jumps.
        const float oldZoom = zoom;
        const float wanted  = zoom * std::pow(1.15f, io.MouseWheel);
        zoom = ImClamp(wanted, kZoomMin, kZoomMax);

        if (zoom != oldZoom)
        {
            // Cursor-anchored: whatever sits under the pointer stays under it.
            // Zooming about the centre instead makes finding a highlight you were
            // already looking at into a pan-hunt every time.
            const float u = (mouse.x - p_Base.Min.x - pan.x) / (p_Base.GetWidth()  * oldZoom);
            const float v = (mouse.y - p_Base.Min.y - pan.y) / (p_Base.GetHeight() * oldZoom);

            pan.x = mouse.x - p_Base.Min.x - u * p_Base.GetWidth()  * zoom;
            pan.y = mouse.y - p_Base.Min.y - v * p_Base.GetHeight() * zoom;
        }
    }

    // Middle-drag to pan, matching the old app rather than left-drag, which a
    // scope panel needs to keep free for cursor readout and mask editing.
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
        pan.x += delta.x;
        pan.y += delta.y;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) Reset();

    // Keep the content covering the panel: at zoom 1 there is no slack and pan is
    // pinned to zero, and beyond that it can move only as far as its own overhang.
    // Without this a panel can be dragged until it is empty, which reads as a
    // crash rather than as a pan.
    const float slackX = p_Base.GetWidth()  * (zoom - 1.0f);
    const float slackY = p_Base.GetHeight() * (zoom - 1.0f);
    pan.x = ImClamp(pan.x, -slackX, 0.0f);
    pan.y = ImClamp(pan.y, -slackY, 0.0f);
}

ImU32 GraticuleColor(ImU32 p_Base, float p_Brightness)
{
    if (p_Brightness >= 1.0f) return p_Base;

    // Scale RGB and leave alpha alone: the slider is a brightness control, and
    // fading the alpha instead would let the panel background bleed through
    // differently depending on what is behind each line.
    const ImU32 a = (p_Base >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 r = ImU32(float((p_Base >> IM_COL32_R_SHIFT) & 0xFF) * p_Brightness);
    const ImU32 g = ImU32(float((p_Base >> IM_COL32_G_SHIFT) & 0xFF) * p_Brightness);
    const ImU32 b = ImU32(float((p_Base >> IM_COL32_B_SHIFT) & 0xFF) * p_Brightness);
    return IM_COL32(r, g, b, a);
}

bool BeginPanelContextMenu(PanelCommon& p_Common, const ImRect& p_Rect)
{
    const char* popupId = "##panelmenu";

    // Recorded for the Settings window, which is drawn later in the frame when
    // this panel is no longer the current window.
    p_Common.panelRect = p_Rect;

    // Opened by hand rather than with BeginPopupContextWindow, because the click
    // position has to be captured to clamp against - and because the open should
    // be limited to the plot rectangle rather than the whole panel window.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        !ImGui::IsAnyItemHovered() &&
        p_Rect.Contains(ImGui::GetIO().MousePos))
    {
        p_Common.menuPos = ImGui::GetIO().MousePos;
        ImGui::OpenPopup(popupId);
    }

    // Keep the whole menu inside this panel.
    //
    // ImGui clamps popups to the viewport, which is right for a menu bar and wrong
    // here: right-clicking near the bottom of a short scope put the menu over the
    // scope below it, so it read as belonging to the wrong panel. Setting the
    // position explicitly takes over that placement - Begin() only calls
    // FindBestWindowPosForPopup() when no position was set.
    ImVec2 pos = p_Common.menuPos;
    if (p_Common.menuSize.x > 0.0f && p_Common.menuSize.y > 0.0f)
    {
        pos.x = ImClamp(pos.x, p_Rect.Min.x, ImMax(p_Rect.Min.x, p_Rect.Max.x - p_Common.menuSize.x));
        pos.y = ImClamp(pos.y, p_Rect.Min.y, ImMax(p_Rect.Min.y, p_Rect.Max.y - p_Common.menuSize.y));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);

    return ImGui::BeginPopup(popupId);
}

void EndPanelContextMenu(PanelCommon& p_Common)
{
    // Measured for next frame's clamp. On the very first open the size is unknown,
    // so the menu is placed at the pointer and settles on the following frame.
    p_Common.menuSize = ImGui::GetWindowSize();
    ImGui::EndPopup();
}

void PositionSettingsWindow(PanelCommon& p_Common)
{
    // Every frame this is called, not just while placing it: pinning the
    // Settings window to whichever viewport its panel is already drawn into
    // is what stops it becoming its own native OS window the moment ImGui's
    // multi-viewport logic decides its position doesn't fully overlap
    // whatever it happens to be over - if that only ran during the initial
    // placement frames below, dragging the window later, or the panel itself
    // moving to a popped-out window, would be free to detach it again.
    if (p_Common.panelViewportId != 0)
        ImGui::SetNextWindowViewport(p_Common.panelViewportId);

    if (p_Common.settingsPlaceFrames <= 0) return;
    --p_Common.settingsPlaceFrames;

    const ImRect& panel = p_Common.panelRect;
    if (panel.GetWidth() <= 0.0f || panel.GetHeight() <= 0.0f) return;

    const float inset = ImGui::GetFontSize() * 0.5f;
    ImVec2 pos(panel.Min.x + inset, panel.Min.y + inset);

    // Pull it back inside if it would overhang. The size comes from the previous
    // frame, which is why this runs twice: on a first-ever open frame one only has
    // the top-left anchor, and frame two corrects it.
    const ImVec2 size = p_Common.settingsSize;
    if (size.x > 0.0f && size.y > 0.0f)
    {
        pos.x = ImClamp(pos.x, panel.Min.x, ImMax(panel.Min.x, panel.Max.x - size.x));
        pos.y = ImClamp(pos.y, panel.Min.y, ImMax(panel.Min.y, panel.Max.y - size.y));
    }

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
}

void AddDashedLine(ImDrawList* p_Draw, ImVec2 p_A, ImVec2 p_B, ImU32 p_Color,
                   float p_Dash, float p_Gap, float p_Thickness)
{
    const float dx = p_B.x - p_A.x;
    const float dy = p_B.y - p_A.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0f) return;

    const float ux = dx / length;
    const float uy = dy / length;
    const float step = p_Dash + p_Gap;

    for (float t = 0.0f; t < length; t += step)
    {
        const float end = (t + p_Dash < length) ? (t + p_Dash) : length;
        p_Draw->AddLine(ImVec2(p_A.x + ux * t,   p_A.y + uy * t),
                        ImVec2(p_A.x + ux * end, p_A.y + uy * end),
                        p_Color, p_Thickness);
    }
}

bool g_HidePanelChrome = false;

bool DrawPanelCloseButton(const ImRect& p_Rect, bool p_Enabled)
{
    if (g_HidePanelChrome) return false;

    const float size = ImGui::GetFontSize() * 1.15f;
    const float pad  = 4.0f;

    const ImVec2 lo(p_Rect.Max.x - size - pad, p_Rect.Min.y + pad);
    const ImVec2 hi(lo.x + size, lo.y + size);

    // An explicit id, because several panels draw this button and ImGui would
    // otherwise give them all the same one - the first panel's button would
    // swallow every click.
    ImGui::SetCursorScreenPos(lo);
    ImGui::PushID("##panelclose");
    ImGui::InvisibleButton("x", ImVec2(size, size));

    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = p_Enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImGui::PopID();

    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImU32 bg = hovered ? IM_COL32(70, 76, 86, 255) : IM_COL32(44, 48, 55, 190);
    const ImU32 fg = p_Enabled
                     ? (hovered ? IM_COL32(235, 240, 245, 255) : IM_COL32(170, 178, 190, 255))
                     : IM_COL32(90, 95, 102, 255);

    draw->AddRectFilled(lo, hi, bg, 3.0f);
    draw->AddRect(lo, hi, IM_COL32(90, 96, 106, 180), 3.0f);

    const float inset = size * 0.30f;
    draw->AddLine(ImVec2(lo.x + inset, lo.y + inset), ImVec2(hi.x - inset, hi.y - inset), fg, 1.4f);
    draw->AddLine(ImVec2(hi.x - inset, lo.y + inset), ImVec2(lo.x + inset, hi.y - inset), fg, 1.4f);

    if (hovered && !p_Enabled)
        ImGui::SetTooltip("The last section cannot be closed.");

    return pressed;
}

void SettingsRowLabel(const char* p_Label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(p_Label);
    ImGui::SameLine(LabelWidth());
    ImGui::SetNextItemWidth(ControlWidth());
}

bool SettingsSliderFloat(const char* p_Label, float* p_Value,
                         float p_Min, float p_Max, float p_Default,
                         const char* p_Format)
{
    // A caller passing "##id" has already drawn the visible label itself with its
    // own SettingsRowLabel call, right before this one - every call site in the
    // app follows that convention. Either way, the label's Text item is still
    // ImGui's "last item" at this point (SameLine/SetNextItemWidth don't count as
    // items of their own), so hovering it below works whether we drew it just now
    // or the caller did.
    bool hasOwnLabel = !(p_Label[0] == '#' && p_Label[1] == '#');
    if (hasOwnLabel)
        SettingsRowLabel(p_Label);

    bool reset = false;
    if (ImGui::IsItemHovered())
    {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            reset = true;
        ImGui::SetTooltip("Double-click to reset to %g", p_Default);
    }

    char id[128];
    std::snprintf(id, sizeof(id), "##%s", p_Label);

    // ImGuiSliderFlags_NoInput is what makes double-click-to-reset possible.
    //
    // By default ImGui turns a double-click on a slider into a text-entry box
    // (imgui_widgets.cpp: "double_clicked" -> temp_input_is_active), which
    // swallowed the double-click before this function ever saw it. Ctrl+click to
    // type goes away with it; for the one place typing a value really matters -
    // a bounding line code - there is an explicit input box instead.
    bool changed = ImGui::SliderFloat(id, p_Value, p_Min, p_Max, p_Format,
                                      ImGuiSliderFlags_NoInput);

    // Double-click on the bar itself still resets - it's the same control the
    // label sits next to - but no tooltip here: the bar is where you look while
    // dragging, and a "double-click to reset" tooltip popping up mid-drag/hover
    // was getting in the way. The label (above) still explains it.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        reset = true;

    if (reset)
    {
        *p_Value = p_Default;
        changed  = true;
    }

    return changed;
}

bool SettingsSliderInt(const char* p_Label, int* p_Value,
                       int p_Min, int p_Max, int p_Default)
{
    // See SettingsSliderFloat for why checking IsItemHovered() here works whether
    // this label was just drawn or drawn by the caller's own preceding
    // SettingsRowLabel call.
    bool hasOwnLabel = !(p_Label[0] == '#' && p_Label[1] == '#');
    if (hasOwnLabel)
        SettingsRowLabel(p_Label);

    bool reset = false;
    if (ImGui::IsItemHovered())
    {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            reset = true;
        ImGui::SetTooltip("Double-click to reset to %d", p_Default);
    }

    char id[128];
    std::snprintf(id, sizeof(id), "##%s", p_Label);

    bool changed = ImGui::SliderInt(id, p_Value, p_Min, p_Max, "%d",
                                    ImGuiSliderFlags_NoInput);

    // See SettingsSliderFloat: the bar still resets on double-click, just
    // without its own tooltip - the label already explains it.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        reset = true;

    if (reset)
    {
        *p_Value = p_Default;
        changed  = true;
    }

    return changed;
}

void DrawSharedSettings(PanelCommon& p_Common, bool p_WithBoundingLines, bool p_WithGraticule)
{
    ImGui::SeparatorText("Display");

    if (p_WithGraticule) {
        float brightnessPercent = p_Common.graticuleBrightness * 100.0f;
        if (SettingsSliderFloat("Graticule", &brightnessPercent, 0.0f, 100.0f, 100.0f, "%.0f%%"))
            p_Common.graticuleBrightness = brightnessPercent / 100.0f;
    }

    SettingsRowLabel("Zoomable");
    ImGui::Checkbox("##zoomable", &p_Common.zoomable);

    if (!p_WithBoundingLines) return;

    ImGui::SeparatorText("Bounding Lines");

    SettingsRowLabel("Bounding Lines");
    ImGui::Checkbox("##boundingmaster", &p_Common.boundingLinesEnabled);

    // The master switch reveals the four lines, and each line's own switch reveals
    // its code and colour - the same "enable to see more" nesting the old app used,
    // which keeps a settings window that has 20 rows in it from opening with all 20
    // on show.
    if (!p_Common.boundingLinesEnabled) return;

    for (int i = 0; i < 4; ++i)
    {
        BoundingLine& line = p_Common.boundingLines[i];

        ImGui::PushID(i);

        char label[16];
        std::snprintf(label, sizeof(label), "Line %d", i + 1);
        SettingsRowLabel(label);
        ImGui::Checkbox("##enabled", &line.enabled);

        if (line.enabled)
        {
            SettingsRowLabel("  Code");

            // A typed box with step buttons, not a slider: these are exact values a
            // colourist knows by name (940, 64, 512), and hunting for one on a
            // 1400-wide slider is the wrong interaction for the job.
            ImGui::SetNextItemWidth(ControlWidth() - ImGui::GetFontSize() * 3.0f);
            if (ImGui::InputFloat("##code", &line.code, 1.0f, 16.0f, "%.0f"))
            {
                // Clamped past 0-1023 on both sides on purpose: the tap publishes
                // values above white and below black, and a limit line you cannot
                // put out there cannot mark them.
                line.code = ImClamp(line.code, -200.0f, 1200.0f);
            }

            ImGui::SameLine();
            ImGui::ColorEdit3("##color", line.color,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        }

        ImGui::PopID();
    }
}

} // namespace scopedeck
