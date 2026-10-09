// A still: either captured from the live preview, or loaded from a file on
// disk - the shared "picture with a couple of scope-relevant facts attached"
// concept behind A/B Difference's reference and the Compare panel. Kept
// separate from either panel's own state so both can hold one without
// duplicating the capture/load logic.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scopedeck
{

struct ScopeFrame;

struct StillImage
{
    std::vector<uint8_t> rgb;   // RGB8, row-major, width*height*3 bytes
    int  width  = 0;
    int  height = 0;
    bool valid  = false;

    // Rec.709 by default - a loaded file carries no colour-space metadata
    // this app reads, and a captured-from-source still inherits whatever
    // the frame it was captured from actually used (see CaptureStillFromFrame).
    float lumaCoeff[3] = { 0.2126f, 0.7152f, 0.0722f };

    // What Settings shows next to "Clear" - a filename for a loaded still,
    // "Captured from Source" for a captured one, so a stale still does not
    // read as the live feed by mistake.
    std::string label;

    // Bumped to a fresh value by every successful capture/load - a still
    // never changes on its own once set, so a caller that caches work
    // derived from its pixels (Compare's scope overlay) can compare against
    // this instead of re-deriving it every frame. 0 for a never-set/cleared
    // still (the default-constructed value), never reused for a real one.
    uint64_t generation = 0;

    bool Empty() const { return !valid || width <= 0 || height <= 0 || rgb.empty(); }
};

// Copies the frame's own preview pixels and luma weights - cheap, and the
// only source of "what does this frame actually look like" the app has at
// preview resolution (see ScopeFrame::preview's own comment for why that is
// the honest resolution to capture at, not a limitation particular to this).
void CaptureStillFromFrame(const ScopeFrame& p_Frame, StillImage& p_Out);

// Decodes p_Path (PNG/JPEG/BMP/GIF/... - whatever stb_image reads) into an
// RGB8 still. Returns false and leaves p_Out untouched on failure - a bad
// path, an unreadable file, and a format stb_image does not recognise all
// look the same to the caller: "could not load this".
bool LoadStillFromFile(const char* p_Path, StillImage& p_Out);

// A native "Open" dialog restricted to common image types. Returns the
// chosen path (UTF-8), or an empty string if the user cancelled or the
// dialog failed. GetOpenFileName on Windows, NSOpenPanel on macOS (see
// mac/MacPlatform.mm) - there is no portable file-dialog dependency in this
// tree, and on any other platform the Compare panel's "Load Still..." button
// simply does nothing because this returns "" unconditionally.
std::string OpenStillFileDialog();

} // namespace scopedeck
