// The dark scope-room palette, in one place.
//
// Scopes are read in a dark grade suite against a reference monitor, so the UI
// stays dark and low-contrast: a bright panel next to the image is not a style
// preference, it actively changes what the colourist sees. Graticules sit only
// just above the background for the same reason.

#pragma once

#include "imgui.h"

namespace scopedeck
{

// Panel and chrome.
constexpr ImU32 kColBackground   = IM_COL32( 18,  19,  22, 255);
constexpr ImU32 kColPanel        = IM_COL32( 24,  26,  30, 255);
constexpr ImU32 kColPanelHeader  = IM_COL32( 32,  35,  40, 255);
constexpr ImU32 kColBorder       = IM_COL32( 52,  56,  63, 255);

// Graticule: the main lines, and the fainter subdivisions between them.
constexpr ImU32 kColGraticule    = IM_COL32( 78,  84,  94, 190);
constexpr ImU32 kColGraticuleDim = IM_COL32( 60,  65,  74, 120);
constexpr ImU32 kColAxisLabel    = IM_COL32(150, 158, 170, 255);

// The legal-range markers - 0 and 100 IRE - which need to stand out from the
// ordinary gridlines without competing with the trace itself.
constexpr ImU32 kColLegalMarker  = IM_COL32(120, 132, 150, 220);

// The graticule proper, shared by the waveform and the histogram so a value reads
// the same on both.
//
// Amber rather than neutral grey: it is a ruler laid over the data, not part of
// the plot frame, and a warm grid separates cleanly from traces that are white,
// red, green or blue. The 0 and 100 lines are the saturated ones - they are the
// limits that matter - and the subdivisions between them are deliberately faint
// enough to read past.
constexpr ImU32 kColGratMajor = IM_COL32(214, 156,  64, 235);
constexpr ImU32 kColGratMinor = IM_COL32(150, 105,  40, 140);
constexpr ImU32 kColGratLabel = IM_COL32(214, 156,  64, 255);

// Cursor readout: the dashed line and its value text.
constexpr ImU32 kColCursor      = IM_COL32(255, 210,  90, 220);
constexpr ImU32 kColCursorLabel = IM_COL32(255, 220, 130, 255);

// Kept as aliases so the histogram scale strip and the graticule cannot drift
// apart - they are the same ruler seen on two axes.
constexpr ImU32 kColHistogramGrid  = kColGratMinor;
constexpr ImU32 kColHistogramScale = kColGratLabel;

// The flesh-tone reference ray on the vectorscope. Warm and dim: it is a
// reference direction, not a measurement, and must not compete with the trace.
constexpr ImU32 kColSkinTone = IM_COL32(214, 176, 150, 210);

// Status, used for the reader states in the footer.
constexpr ImU32 kColLive         = IM_COL32( 92, 200, 128, 255);
constexpr ImU32 kColWaiting      = IM_COL32(190, 170,  90, 255);
constexpr ImU32 kColError        = IM_COL32(220, 110, 110, 255);
constexpr ImU32 kColMuted        = IM_COL32(120, 128, 140, 255);

void ApplyTheme();

} // namespace scopedeck
