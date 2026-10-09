#include "MacPlatform.h"

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <mach-o/dyld.h>
#include <sys/stat.h>

#include <cstdint>
#include <vector>

namespace scopedeck
{
namespace mac
{

std::string AppSupportDir()
{
    @autoreleasepool
    {
        NSArray<NSURL*>* urls = [[NSFileManager defaultManager]
            URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask];
        NSURL* base = urls.count ? urls[0] : nil;
        if (!base)
        {
            const char* home = std::getenv("HOME");
            return std::string(home ? home : ".") + "/Library/Application Support/Scope Deck";
        }
        NSURL* dir = [base URLByAppendingPathComponent:@"Scope Deck" isDirectory:YES];
        // Harmless if it exists already, like CreateDirectoryA on Windows.
        [[NSFileManager defaultManager] createDirectoryAtURL:dir
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil];
        return std::string(dir.fileSystemRepresentation);
    }
}

std::string ExecutableDir()
{
    // _NSGetExecutablePath, not NSBundle: a bare build binary is not inside a
    // bundle, and this has to work for both.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return ".";

    char resolved[PATH_MAX];
    const char* path = realpath(buf.data(), resolved) ? resolved : buf.data();
    std::string s(path);
    const size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? "." : s.substr(0, slash);
}

std::string ResourcePath(const char* p_Name)
{
    const std::string exeDir = ExecutableDir();
    const std::string candidates[] = {
        exeDir + "/../Resources/" + p_Name,   // inside Scope Deck.app
        exeDir + "/" + p_Name,                // a bare build binary
    };
    for (const std::string& c : candidates)
    {
        struct stat st;
        if (::stat(c.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return c;
    }
    return candidates[0];
}

std::string OpenImageFileDialog(const char* p_Title)
{
    @autoreleasepool
    {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = [NSString stringWithUTF8String:p_Title] ?: @"Load Still";
        panel.message = panel.title;
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        // Everything stb_image decodes (see StillImage.cpp's Windows filter):
        // the image type covers png/jpg/bmp/tga/gif/psd/hdr/pic.
        panel.allowedContentTypes = @[ UTTypeImage ];

        if ([panel runModal] != NSModalResponseOK) return "";
        NSURL* url = panel.URLs.firstObject;
        if (!url) return "";
        return std::string(url.fileSystemRepresentation);
    }
}

static NSRunningApplication* RunningApp(const char* p_BundleId)
{
    NSString* bundleId = [NSString stringWithUTF8String:p_BundleId];
    if (!bundleId) return nil;
    NSArray<NSRunningApplication*>* apps =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleId];
    return apps.count ? apps[0] : nil;
}

bool ActivateApp(const char* p_BundleId)
{
    @autoreleasepool
    {
        NSRunningApplication* app = RunningApp(p_BundleId);
        if (!app) return false;
        // macOS 14's cooperative activation: this app hands its active status
        // over, which is what lets the other app actually come forward
        // rather than merely being asked to.
        [NSApp yieldActivationToApplication:app];
        if (app.hidden) [app unhide];
        return [app activateWithOptions:NSApplicationActivateAllWindows];
    }
}

int RunningAppPid(const char* p_BundleId)
{
    @autoreleasepool
    {
        NSRunningApplication* app = RunningApp(p_BundleId);
        return app ? int(app.processIdentifier) : 0;
    }
}

} // namespace mac
} // namespace scopedeck
