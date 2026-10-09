// What every scope panel has in common: the right-click menu, its own Settings
// window, and the settings the base ScopePanel owned in the old app (Graticule
// brightness, Zoomable, Bounding Lines).
//
// State is per panel *instance*, not per panel type, so two Waveforms can carry
// different bounding lines and different graticule brightness - which is what the
// old app did and the reason its settings lived on the widget rather than in a
// global dict.

#pragma once

#include "imgui.h"
#include "imgui_internal.h"   // ImRect

#include <cstdint>

namespace scopedeck
{

// --- IRE scales -------------------------------------------------------------
//
// Two conventions, and it matters which one a readout is on: a frame peaking at
// 0.97 reads 97.0 IRE on the full scale and 105.9 on the legal one. Neither is
// wrong - they answer different questions - so this is a switch, not a guess.

constexpr float kLegalBlack10Bit = 64.0f;
constexpr float kLegalWhite10Bit = 940.0f;
constexpr float kCodeMax10Bit    = 1023.0f;

inline float IreOfValue(float p_Value, bool p_Legal)
{
    if (p_Legal)
        return (p_Value * kCodeMax10Bit - kLegalBlack10Bit)
               / (kLegalWhite10Bit - kLegalBlack10Bit) * 100.0f;
    return p_Value * 100.0f;
}

inline float ValueOfIre(float p_Ire, bool p_Legal)
{
    if (p_Legal)
        return (kLegalBlack10Bit + p_Ire / 100.0f * (kLegalWhite10Bit - kLegalBlack10Bit))
               / kCodeMax10Bit;
    return p_Ire / 100.0f;
}

// 10-bit code value, the unit both conventions are defined against - and the unit
// a colourist actually types when placing a limit line ("940", not "0.919").
inline float CodeOfValue(float p_Value) { return p_Value * kCodeMax10Bit; }
inline float ValueOfCode(float p_Code)  { return p_Code / kCodeMax10Bit; }

// --- Bounding lines ---------------------------------------------------------
//
// Four fixed references at a code value and colour the user picks. Unlike the
// built-in IRE marks there is no single right set of these, so they are all off
// until both the master switch and the line's own switch are on.

struct BoundingLine
{
    bool  enabled = false;
    float code    = 512.0f;
    float color[3] = { 1.0f, 1.0f, 0.5f };
};

// Scroll to zoom, middle-drag to pan, Z to reset - the interaction the old app's
// _ZoomPan provided, and what the Zoomable checkbox switches on.
//
// The graticule is transformed along with the trace, not left fixed. A zoomed
// trace against an unzoomed grid would be unreadable in the specific way that
// matters most here: every value read off it would be wrong.
struct ZoomPan
{
    float  zoom = 1.0f;
    ImVec2 pan  = ImVec2(0.0f, 0.0f);

    void Reset() { zoom = 1.0f; pan = ImVec2(0.0f, 0.0f); }
    bool Active() const { return zoom != 1.0f || pan.x != 0.0f || pan.y != 0.0f; }

    // The rectangle content should be drawn into, given the panel's own rectangle.
    ImRect Apply(const ImRect& p_Base) const;

    // Consumes scroll/drag/keys while the panel is hovered. Does nothing when
    // p_Enabled is false, so the Zoomable checkbox genuinely gates it.
    void HandleInput(const ImRect& p_Base, bool p_Enabled);
};

constexpr float kZoomMin = 1.0f;
constexpr float kZoomMax = 32.0f;

struct PanelCommon
{
    // Dims every reference-grid colour - rules, rings, target boxes, tick labels -
    // and never the trace, which is what the grid exists to be read against.
    float graticuleBrightness = 1.0f;

    // On by default, unlike the old app. Scroll-to-zoom is non-destructive and
    // reversible with Z, and a scope you cannot zoom into is the common complaint;
    // having to find a checkbox first is a worse default than the occasional
    // unintended scroll.
    bool  zoomable            = true;

    bool  boundingLinesEnabled = false;
    BoundingLine boundingLines[4] = {
        { false, 512.0f, { 1.000f, 1.000f, 0.498f } },
        { false, 384.0f, { 1.000f, 1.000f, 0.498f } },
        { false, 940.0f, { 0.667f, 0.000f, 0.000f } },
        { false,  64.0f, { 0.667f, 0.000f, 0.000f } },
    };

    // Whether this panel's own Settings window is open. Per instance, so opening
    // one Waveform's settings does not open another's.
    bool  showSettings = false;

    // Where the right-click menu was opened, and how big it measured last frame.
    // The size is needed to keep the menu inside the panel: a popup's extent is
    // only known once it has been laid out, so the clamp uses the previous
    // frame's measurement.
    ImVec2 menuPos  = ImVec2(0.0f, 0.0f);
    ImVec2 menuSize = ImVec2(0.0f, 0.0f);

    // This panel's plot rectangle, refreshed every frame. The Settings window is
    // drawn after every panel has been laid out, so it needs the rectangle
    // recorded rather than passed down.
    ImRect panelRect = ImRect(ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f));

    // Which OS window this panel is currently drawn into - the main app window
    // normally, or a popped-out panel's own window, refreshed every frame right
    // after the panel's own ImGui::Begin. The Settings window pins itself to this
    // (see PositionSettingsWindow) rather than letting ImGui's own multi-viewport
    // logic decide: a Settings popup is meant to be an ordinary floating window
    // that lives with whatever it settings for, never a separate, natively-
    // decorated OS window of its own.
    ImGuiID panelViewportId = 0;

    // Counts down the frames over which the Settings window is placed on its own
    // panel; zero afterwards, so dragging it somewhere else sticks.
    //
    // Two frames, not one: an auto-resizing window has no size until it has been
    // laid out once, so the first frame can only anchor it and the second is what
    // pulls it back inside a panel too short to hold it.
    int    settingsPlaceFrames = 0;
    ImVec2 settingsSize        = ImVec2(0.0f, 0.0f);

    ZoomPan zoom;
};

// `base` dimmed by the panel's Graticule slider. Every grid colour should be built
// through this rather than used directly, so one slider covers all of them.
ImU32 GraticuleColor(ImU32 p_Base, float p_Brightness);


// The right-click menu. Returns true if the menu was shown this frame.
//
// Call this immediately after a panel draws its content, while the panel window is
// still current. p_Rect is the panel's own plot rectangle: the menu opens at the
// pointer but is kept wholly inside it, rather than spilling across neighbouring
// scopes the way a viewport-clamped popup does.
// Split into Begin/End so the caller can compose the menu items - Content, Add
// Section and Solo all need to reach the panel list, which lives above this file.
// Between them, submit menu items exactly as inside any ImGui popup.
bool BeginPanelContextMenu(PanelCommon& p_Common, const ImRect& p_Rect);
void EndPanelContextMenu(PanelCommon& p_Common);

// A labelled row, matching the old app's _add_menu_row: label on the left at a
// fixed width, control on the right. Call before the control.
void SettingsRowLabel(const char* p_Label);

// A slider that resets to its default on double-click, as _ResettableSlider did.
// Double-clicking either the slider or its label resets it.
bool SettingsSliderFloat(const char* p_Label, float* p_Value,
                         float p_Min, float p_Max, float p_Default,
                         const char* p_Format = "%.0f");

// The int equivalent, for the handful of settings (dot density, cloud spread)
// that are step counts rather than continuous values.
bool SettingsSliderInt(const char* p_Label, int* p_Value,
                       int p_Min, int p_Max, int p_Default);

// Opens a panel's Settings window over that panel. Call immediately before the
// window's ImGui::Begin(); it only acts on the frame the window was opened.
void PositionSettingsWindow(PanelCommon& p_Common);

// A dashed line, for the cursor readout. ImDrawList has no dashed primitive, so
// this walks the span drawing short segments.
void AddDashedLine(ImDrawList* p_Draw, ImVec2 p_A, ImVec2 p_B, ImU32 p_Color,
                   float p_Dash = 6.0f, float p_Gap = 4.0f, float p_Thickness = 1.0f);

// A close button in the panel's top-right corner. Returns true on the frame it is
// clicked.
//
// Drawn into the plot area rather than relying on ImGui's own title-bar close
// button, because a docked panel shows a tab rather than a title bar - the X would
// end up on the tab strip, away from the section it closes.
//
// Draws nothing and returns false while g_HidePanelChrome is set - a full-screen
// Solo, where the panel is meant to be nothing but its picture.
bool DrawPanelCloseButton(const ImRect& p_Rect, bool p_Enabled);

extern bool g_HidePanelChrome;

// Graticule + Zoomable, and the Bounding Lines group when p_WithBoundingLines.
// Panels call this at the end of their own settings so the shared rows land below
// the panel's own controls.
//
// Bounding lines are a level reference and only mean something on a scope with a
// level axis. The Vectorscope has none - its axes are Cb and Cr - so it opts out
// rather than showing a control that could only draw a line at a meaningless place.
void DrawSharedSettings(PanelCommon& p_Common, bool p_WithBoundingLines = true, bool p_WithGraticule = true);

} // namespace scopedeck
