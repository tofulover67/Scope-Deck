// The macOS platform layer the C++ side calls into: paths, the file dialog and
// talking to other running apps. Objective-C++ lives only in MacPlatform.mm;
// this header is plain C++ so main.cpp, StillImage.cpp and the two Python
// bridges include it without becoming Objective-C++ themselves.
//
// Windows has no counterpart file: those callers use Win32 directly under
// #ifdef _WIN32. macOS keeps its Cocoa behind these few functions instead.

#pragma once

#include <string>

namespace scopedeck
{
namespace mac
{

// ~/Library/Application Support/Scope Deck, created if it does not exist yet.
// Where layout.ini and presets/ live - the counterpart of %LOCALAPPDATA%\ScopeDeck.
std::string AppSupportDir();

// The directory holding the running executable: Contents/MacOS inside the
// .app, or the build directory for a bare binary.
std::string ExecutableDir();

// A file shipped with the app: Contents/Resources/<name> inside the .app, or
// next to the executable for a bare build (where the post-build copy puts
// the worker scripts on Windows). Returns the first of the two that exists,
// else the bundle path.
std::string ResourcePath(const char* p_Name);

// A modal Open panel filtered to images. Empty if cancelled. Main thread only.
std::string OpenImageFileDialog(const char* p_Title);

// Brings the running app with this bundle id to the front (its windows, not a
// new one). False when it is not running.
bool ActivateApp(const char* p_BundleId);

// Process id of the running app with this bundle id, or 0.
int RunningAppPid(const char* p_BundleId);

// Bundle ids of the hosts the app reads from.
constexpr const char* kResolveBundleId  = "com.blackmagic-design.DaVinciResolve";
constexpr const char* kPremiereBundleId = "com.adobe.PremierePro.CC";

} // namespace mac
} // namespace scopedeck
