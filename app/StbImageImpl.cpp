// The one translation unit that actually compiles stb_image's implementation
// (single-header library convention - every other includer gets declarations
// only). See vendor/VENDOR.md for provenance.

#ifdef _WIN32
// Lets LoadStillFromFile/OpenStillFileDialog (StillImage.cpp) hand stb_image
// a UTF-8 path and have it reach _wfopen correctly for a non-ASCII filename,
// instead of being silently re-interpreted through the current ANSI code
// page. Must match the same #define this project's other includes of
// stb_image.h use - it gates the declarations too, not just this file's
// definitions.
#define STBI_WINDOWS_UTF8
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
