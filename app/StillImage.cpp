#include "StillImage.h"
#include "ScopeReader.h"

#include <atomic>

// Declares (and, in the STB_IMAGE_IMPLEMENTATION translation unit, defines)
// stbi_convert_wchar_to_utf8 - needed by OpenStillFileDialog below to hand
// stb_image a UTF-8 path it can round-trip through _wfopen. This macro gates
// the declaration itself, not just the implementation, so it has to be
// defined before every #include of this header that calls the function -
// StbImageImpl.cpp defines it the same way, before its own include.
#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#endif
#include "stb_image.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

namespace scopedeck
{

namespace
{
    // One counter for every StillImage in the app, not one per instance -
    // simpler, and uniqueness across the whole process is all a generation
    // comparison actually needs.
    std::atomic<uint64_t> g_NextStillGeneration{1};
}

void CaptureStillFromFrame(const ScopeFrame& p_Frame, StillImage& p_Out)
{
    if (!p_Frame.HasPreview())
    {
        p_Out = StillImage{};
        return;
    }

    p_Out.rgb    = p_Frame.preview;   // a copy - the frame's own buffer is overwritten next frame
    p_Out.width  = int(p_Frame.previewWidth);
    p_Out.height = int(p_Frame.previewHeight);
    p_Out.lumaCoeff[0] = p_Frame.lumaCoeff[0];
    p_Out.lumaCoeff[1] = p_Frame.lumaCoeff[1];
    p_Out.lumaCoeff[2] = p_Frame.lumaCoeff[2];
    p_Out.label      = "Captured from Source";
    p_Out.valid      = true;
    p_Out.generation = g_NextStillGeneration.fetch_add(1, std::memory_order_relaxed);
}

bool LoadStillFromFile(const char* p_Path, StillImage& p_Out)
{
    if (!p_Path || !*p_Path) return false;

    int width = 0, height = 0, channels = 0;
    // Forced to 3 channels (RGB8, alpha dropped) - every other still-backed
    // buffer in this app (ScopeFrame::preview, capture-from-source) is RGB8,
    // and the scope-binning code this feeds reads exactly that layout.
    unsigned char* pixels = stbi_load(p_Path, &width, &height, &channels, 3);
    if (!pixels || width <= 0 || height <= 0) return false;

    StillImage loaded;
    loaded.rgb.assign(pixels, pixels + size_t(width) * size_t(height) * 3);
    loaded.width  = width;
    loaded.height = height;
    loaded.valid  = true;
    // Rec.709 default (StillImage's own) is left as-is: a loaded file has
    // no colour-space metadata this app reads.

    stbi_image_free(pixels);

    // The bare filename, not the full path - a Settings row is not the
    // place to read a long absolute path, and the path itself is not kept
    // anywhere a re-load would need it from.
    std::string path(p_Path);
    const size_t slash = path.find_last_of("\\/");
    loaded.label = (slash == std::string::npos) ? path : path.substr(slash + 1);

    loaded.generation = g_NextStillGeneration.fetch_add(1, std::memory_order_relaxed);

    p_Out = std::move(loaded);
    return true;
}

#ifdef _WIN32
std::string OpenStillFileDialog()
{
    wchar_t path[MAX_PATH] = L"";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = glfwGetWin32Window(glfwGetCurrentContext());
    ofn.lpstrFilter =
        L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.psd;*.hdr;*.pic\0"
        L"All Files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle  = L"Load Still";

    if (!GetOpenFileNameW(&ofn)) return "";

    // stb_image only takes a plain char* path; on Windows that has to be
    // UTF-8 with STBI_WINDOWS_UTF8 defined (see StbImageImpl.cpp) for a
    // non-ASCII path to round-trip through _wfopen correctly rather than
    // being mangled through the current ANSI code page.
    const int len = stbi_convert_wchar_to_utf8(nullptr, 0, path);
    if (len <= 0) return "";

    std::string utf8(size_t(len), '\0');
    stbi_convert_wchar_to_utf8(utf8.data(), utf8.size(), path);
    // stbi_convert_wchar_to_utf8 includes the trailing NUL in its length;
    // std::string should not carry that as part of its own contents.
    if (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();
    return utf8;
}
#else
std::string OpenStillFileDialog()
{
    return "";   // no portable file-dialog dependency in this tree yet
}
#endif

} // namespace scopedeck
