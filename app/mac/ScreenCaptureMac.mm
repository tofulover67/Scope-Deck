// macOS half of Screen Capture - see ScreenCaptureMac.h.
//
// Built without ARC (the target doesn't enable it), so ownership here is the
// classic retain/release kind: every alloc, retain or copy below has its
// release next to the place the object stops being needed.
#include "ScreenCaptureMac.h"

#include "../ScreenCapture.h"

#import <Cocoa/Cocoa.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <mach/mach_time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>

// ---------------------------------------------------------------------------
// Region picker
// ---------------------------------------------------------------------------
//
// One borderless window per display, above everything, instead of an ImGui
// window: it has to cover every display at once, Scope Deck's own windows
// included - the job the Win32 popup does on Windows. Unlike Windows it shows
// no screenshot. The overlay is a translucent dim with the selection left
// see-through, so the user picks against the live screen, and picking needs
// no Screen Recording permission - only capturing does.
//
// Everything is tracked in global CoreGraphics points (top-left of the main
// display is 0,0, y down), the space ScreenRegion uses on macOS. Cocoa's
// screen space is the same but with y up from the bottom of the main
// display; ToCG flips between them.

namespace
{

struct PickerState
{
    CGFloat primaryHeight = 0;   // the main display's height, for the y flip

    bool    dragging = false;
    bool    done     = false;
    bool    accepted = false;

    // Whole-point cells, like the Windows picker's pixels: a click and drag
    // from cell a to cell b selects both, so the selection is never empty.
    CGPoint start   = CGPointZero;
    CGPoint current = CGPointZero;

    CGRect  screen = CGRectZero;   // the display the drag started on
    CGFloat scale  = 1.0;          // its pixels per point

    CGRect  drawn = CGRectNull;    // what the selection last painted, to repaint it
};

PickerState* g_Picker = nullptr;   // one modal picker at a time

const CGFloat kDimAlpha   = 0.6;
const CGFloat kBorder     = 2.0;
const CGFloat kLabelPadX  = 6.0;
const CGFloat kLabelPadY  = 3.0;
const CGFloat kLabelGap   = 6.0;
const int     kMinPixels  = 8;     // a smaller drag is a click, not a region

CGRect ToCG(NSRect p_Cocoa, CGFloat p_PrimaryHeight)
{
    return CGRectMake(p_Cocoa.origin.x, p_PrimaryHeight - p_Cocoa.origin.y - p_Cocoa.size.height,
                      p_Cocoa.size.width, p_Cocoa.size.height);
}

CGRect PickerSelection(const PickerState& p_State)
{
    const CGFloat x0 = std::min(p_State.start.x, p_State.current.x);
    const CGFloat y0 = std::min(p_State.start.y, p_State.current.y);
    const CGFloat x1 = std::max(p_State.start.x, p_State.current.x) + 1;
    const CGFloat y1 = std::max(p_State.start.y, p_State.current.y) + 1;
    return CGRectMake(x0, y0, x1 - x0, y1 - y0);
}

int ToPixels(CGFloat p_Points, CGFloat p_Scale)
{
    return static_cast<int>(std::lround(p_Points * p_Scale));
}

NSDictionary* LabelAttributes()
{
    // Built once and kept for the process: a font and two colours.
    static NSDictionary* attributes = [@{
        NSFontAttributeName            : [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName : [NSColor colorWithSRGBRed:240 / 255.0 green:240 / 255.0
                                                              blue:240 / 255.0 alpha:1.0],
    } retain];
    return attributes;
}

NSString* SizeText(const PickerState& p_State)
{
    const CGRect sel = PickerSelection(p_State);
    return [NSString stringWithFormat:@"%d x %d", ToPixels(sel.size.width, p_State.scale),
                                                  ToPixels(sel.size.height, p_State.scale)];
}

NSSize LabelSize(NSString* p_Text)
{
    const NSSize text = [p_Text sizeWithAttributes:LabelAttributes()];
    return NSMakeSize(std::ceil(text.width) + 2 * kLabelPadX, std::ceil(text.height) + 2 * kLabelPadY);
}

// Below the selection, or above it when it reaches the bottom of its display,
// and pulled in from the right edge - always on the display being dragged on.
CGRect SizeLabelRect(const PickerState& p_State)
{
    const CGRect sel = PickerSelection(p_State);
    const NSSize size = LabelSize(SizeText(p_State));
    const CGRect& screen = p_State.screen;

    CGFloat y = CGRectGetMaxY(sel) + kLabelGap;
    if (y + size.height > CGRectGetMaxY(screen)) y = CGRectGetMinY(sel) - kLabelGap - size.height;
    y = std::max(y, CGRectGetMinY(screen));

    CGFloat x = std::min(CGRectGetMinX(sel), CGRectGetMaxX(screen) - size.width);
    x = std::max(x, CGRectGetMinX(screen));
    return CGRectMake(x, y, size.width, size.height);
}

// Everything the selection paints - border and size label included.
CGRect SelectionFootprint(const PickerState& p_State)
{
    if (!p_State.dragging) return CGRectNull;
    const CGRect sel = CGRectInset(PickerSelection(p_State), -kBorder, -kBorder);
    return CGRectInset(CGRectUnion(sel, SizeLabelRect(p_State)), -1, -1);
}

void DrawLabel(NSString* p_Text, NSRect p_Box)
{
    [[NSColor colorWithSRGBRed:20 / 255.0 green:20 / 255.0 blue:20 / 255.0 alpha:1.0] set];
    NSRectFillUsingOperation(p_Box, NSCompositingOperationCopy);
    [p_Text drawAtPoint:NSMakePoint(p_Box.origin.x + kLabelPadX, p_Box.origin.y + kLabelPadY)
         withAttributes:LabelAttributes()];
}

NSString* const kPickerHint = @"Drag to select a capture region  -  Esc or right-click to cancel";

} // namespace

@interface SDPickerWindow : NSWindow
@end

@implementation SDPickerWindow
// A borderless window refuses key status by default, and without it Esc
// would go to whatever GLFW window was key before.
- (BOOL)canBecomeKeyWindow { return YES; }
@end

@interface SDPickerView : NSView
{
@public
    CGRect m_ScreenCG;    // this view's display, global CG points
    CGFloat m_Scale;      // and its pixels per point
    BOOL   m_ShowsHint;   // the main display's view carries the instructions
}
@end

@implementation SDPickerView

// Top-left origin, so a view point is a global CG point minus the display's
// origin - no flip on top of the Cocoa/CG one.
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)p_Event { (void)p_Event; return YES; }

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea* area in [[self.trackingAreas copy] autorelease])
        [self removeTrackingArea:area];
    // ActiveAlways: the overlay on a display other than the key one still
    // gets to set the cursor when the mouse crosses onto it.
    NSTrackingArea* area = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingCursorUpdate | NSTrackingMouseMoved | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:area];
    [area release];
}

- (void)cursorUpdate:(NSEvent*)p_Event { (void)p_Event; [[NSCursor crosshairCursor] set]; }
- (void)mouseMoved:(NSEvent*)p_Event   { (void)p_Event; [[NSCursor crosshairCursor] set]; }

- (NSRect)toLocal:(CGRect)p_Global
{
    return NSOffsetRect(NSRectFromCGRect(p_Global), -m_ScreenCG.origin.x, -m_ScreenCG.origin.y);
}

- (void)drawRect:(NSRect)p_Dirty
{
    // Copy, not source-over: the window starts clear and every pass sets
    // each pixel outright, so repainting never darkens what was there.
    [[NSColor colorWithSRGBRed:0 green:0 blue:0 alpha:kDimAlpha] set];
    NSRectFillUsingOperation(p_Dirty, NSCompositingOperationCopy);

    const PickerState* s = g_Picker;
    if (s && s->dragging)
    {
        const NSRect sel = [self toLocal:PickerSelection(*s)];
        if (NSIntersectsRect(NSInsetRect(sel, -kBorder, -kBorder), self.bounds))
        {
            // Not quite zero: a fully clear pixel can let a click fall
            // through to the window below. 1/255 of black is invisible.
            [[NSColor colorWithSRGBRed:0 green:0 blue:0 alpha:1.0 / 255.0] set];
            NSRectFillUsingOperation(sel, NSCompositingOperationCopy);

            [[NSColor colorWithSRGBRed:1.0 green:196 / 255.0 blue:0.0 alpha:1.0] set];
            NSFrameRectWithWidthUsingOperation(NSInsetRect(sel, -kBorder, -kBorder), kBorder,
                                               NSCompositingOperationCopy);
        }

        const NSRect label = [self toLocal:SizeLabelRect(*s)];
        if (NSIntersectsRect(label, self.bounds)) DrawLabel(SizeText(*s), label);
    }

    if (m_ShowsHint)
    {
        NSString* hint = kPickerHint;
        const NSSize size = LabelSize(hint);
        DrawLabel(hint, NSMakeRect(24, 24, size.width, size.height));
    }
}

@end

namespace
{

// Repaints what the selection covered before and what it covers now, on every
// display it touches - not whole displays, which at 5K would lag the drag.
void InvalidateSelection(PickerState& p_State, NSArray<NSWindow*>* p_Windows)
{
    const CGRect now = SelectionFootprint(p_State);
    const CGRect dirty = CGRectUnion(p_State.drawn, now);
    p_State.drawn = now;
    if (CGRectIsNull(dirty)) return;

    for (NSWindow* window in p_Windows)
    {
        SDPickerView* view = static_cast<SDPickerView*>(window.contentView);
        const NSRect local = NSIntersectionRect([view toLocal:dirty], view.bounds);
        if (!NSIsEmptyRect(local)) [view setNeedsDisplayInRect:local];
    }
}

// The whole-point cell under p_Event's mouse, kept on the display the drag
// started on: a region is captured from one display (see ScreenCapture.h),
// so the selection never pretends otherwise.
CGPoint CellAt(const PickerState& p_State, NSEvent* p_Event)
{
    NSPoint p = [NSEvent mouseLocation];
    if (p_Event.window)
        p = [p_Event.window convertPointToScreen:p_Event.locationInWindow];

    CGFloat x = std::floor(p.x);
    CGFloat y = std::floor(p_State.primaryHeight - p.y);
    x = std::clamp(x, CGRectGetMinX(p_State.screen), CGRectGetMaxX(p_State.screen) - 1);
    y = std::clamp(y, CGRectGetMinY(p_State.screen), CGRectGetMaxY(p_State.screen) - 1);
    return CGPointMake(x, y);
}

} // namespace

namespace scopedeck
{
namespace mac
{

bool PickScreenRegion(ScreenRegion& p_Out)
{
    if (g_Picker) return false;

    @autoreleasepool
    {
        NSArray<NSScreen*>* screens = [NSScreen screens];
        if (screens.count == 0) return false;

        PickerState state;
        // screens[0] is the display with the menu bar, whose bottom-left is
        // Cocoa's origin and whose top-left is CoreGraphics'.
        state.primaryHeight = screens[0].frame.size.height;

        NSWindow* previousKey = [[NSApp keyWindow] retain];

        NSMutableArray<NSWindow*>* windows = [NSMutableArray array];
        for (NSScreen* screen in screens)
        {
            const NSRect frame = screen.frame;
            SDPickerWindow* window = [[SDPickerWindow alloc] initWithContentRect:frame
                                                                       styleMask:NSWindowStyleMaskBorderless
                                                                         backing:NSBackingStoreBuffered
                                                                           defer:NO];
            window.releasedWhenClosed = NO;   // the array below owns it
            window.level = NSScreenSaverWindowLevel;
            window.opaque = NO;
            window.backgroundColor = [NSColor clearColor];
            window.hasShadow = NO;
            window.ignoresMouseEvents = NO;
            window.animationBehavior = NSWindowAnimationBehaviorNone;
            // Over a full-screen app's space too, and never in Cmd-` or
            // Mission Control.
            window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                        NSWindowCollectionBehaviorFullScreenAuxiliary |
                                        NSWindowCollectionBehaviorStationary |
                                        NSWindowCollectionBehaviorIgnoresCycle;

            SDPickerView* view = [[SDPickerView alloc]
                initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
            view->m_ScreenCG = ToCG(frame, state.primaryHeight);
            view->m_Scale = screen.backingScaleFactor;
            view->m_ShowsHint = (screen == screens[0]);
            window.contentView = view;
            [view release];

            [window setFrame:frame display:NO];
            [windows addObject:window];
            [window release];
        }

        g_Picker = &state;

        // Normally active already (the request came from this app's menu);
        // this covers a switch away during the frames before the picker.
        [NSApp activate];
        for (NSWindow* window in windows) [window orderFrontRegardless];
        [windows[0] makeKeyWindow];
        [windows[0] makeFirstResponder:windows[0].contentView];
        [[NSCursor crosshairCursor] set];

        // Switching to another app cancels rather than leaving a topmost
        // overlay over the app the user just switched to, as on Windows.
        PickerState* statePtr = &state;
        id resignObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidResignActiveNotification
                        object:NSApp
                         queue:nil
                    usingBlock:^(NSNotification* p_Note) {
                        (void)p_Note;
                        statePtr->done = true;
                    }];

        // Its own loop, like a modal session. Mouse and key events are the
        // picker's and are consumed here; everything else is dispatched, so
        // GLFW's windows still get their window-server traffic and pick up
        // where they left off once the loop returns. The timeout only bounds
        // how late a cancel from the notification above is noticed.
        while (!state.done)
        {
            @autoreleasepool
            {
                NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES];
                if (!event) continue;

                const bool ours = event.window && [windows containsObject:event.window];
                switch (event.type)
                {
                    case NSEventTypeKeyDown:
                        if (event.keyCode == 53) state.done = true;   // kVK_Escape
                        continue;

                    case NSEventTypeKeyUp:
                    case NSEventTypeFlagsChanged:
                        continue;

                    case NSEventTypeRightMouseDown:
                        state.done = true;
                        continue;

                    case NSEventTypeRightMouseUp:
                    case NSEventTypeRightMouseDragged:
                    case NSEventTypeOtherMouseDown:
                    case NSEventTypeOtherMouseUp:
                    case NSEventTypeOtherMouseDragged:
                    case NSEventTypeScrollWheel:
                        continue;

                    case NSEventTypeLeftMouseDown:
                        if (ours)
                        {
                            SDPickerView* view = static_cast<SDPickerView*>(event.window.contentView);
                            state.screen = view->m_ScreenCG;
                            state.scale = view->m_Scale;
                            state.dragging = true;
                            state.start = state.current = CellAt(state, event);
                            [[NSCursor crosshairCursor] set];
                            InvalidateSelection(state, windows);
                        }
                        continue;

                    case NSEventTypeLeftMouseDragged:
                        if (state.dragging)
                        {
                            state.current = CellAt(state, event);
                            [[NSCursor crosshairCursor] set];
                            InvalidateSelection(state, windows);
                        }
                        continue;

                    case NSEventTypeLeftMouseUp:
                        if (state.dragging)
                        {
                            state.current = CellAt(state, event);
                            const CGRect sel = PickerSelection(state);
                            if (ToPixels(sel.size.width, state.scale) >= kMinPixels &&
                                ToPixels(sel.size.height, state.scale) >= kMinPixels)
                            {
                                state.accepted = true;
                                state.done = true;
                            }
                            else
                            {
                                // A click, not a drag - nothing to capture yet, keep waiting.
                                state.dragging = false;
                                InvalidateSelection(state, windows);
                            }
                        }
                        continue;

                    default:
                        break;
                }
                [NSApp sendEvent:event];
            }
        }

        [[NSNotificationCenter defaultCenter] removeObserver:resignObserver];

        for (NSWindow* window in windows)
        {
            [window orderOut:nil];
            [window close];
        }
        [windows removeAllObjects];   // the last reference: the windows go here
        g_Picker = nullptr;

        // Hand key status back to the GLFW window that had it, so keyboard
        // input resumes there without a click.
        if (previousKey && previousKey.visible) [previousKey makeKeyWindow];
        [previousKey release];
        [[NSCursor arrowCursor] set];

        if (!state.accepted) return false;

        const CGRect sel = PickerSelection(state);
        p_Out.x      = static_cast<int>(sel.origin.x);
        p_Out.y      = static_cast<int>(sel.origin.y);
        p_Out.width  = ToPixels(sel.size.width, state.scale);
        p_Out.height = ToPixels(sel.size.height, state.scale);
        p_Out.scale  = static_cast<float>(state.scale);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Display fitting
// ---------------------------------------------------------------------------

bool FitRegionToDisplay(const ScreenRegion& p_In, ScreenRegion& p_Out, uint32_t& p_DisplayId)
{
    if (!p_In.IsValid()) return false;

    const CGRect points = CGRectMake(p_In.x, p_In.y, p_In.width / p_In.scale, p_In.height / p_In.scale);

    // ScreenCaptureKit streams one display, so a region is captured from
    // the display holding its centre and anything hanging off it is cut
    // away. The picker never produces such a region; a remembered one can
    // after the displays are rearranged.
    CGDirectDisplayID display = kCGNullDirectDisplay;
    uint32_t count = 0;
    const CGPoint centre = CGPointMake(CGRectGetMidX(points), CGRectGetMidY(points));
    if (CGGetDisplaysWithPoint(centre, 1, &display, &count) != kCGErrorSuccess || count == 0)
    {
        // The centre fell in a gap between displays: any display it touches.
        if (CGGetDisplaysWithRect(points, 1, &display, &count) != kCGErrorSuccess || count == 0)
            return false;
    }

    const CGRect bounds = CGDisplayBounds(display);
    CGRect fitted = CGRectIntersection(points, bounds);
    if (CGRectIsNull(fitted) || CGRectIsEmpty(fitted)) return false;
    fitted = CGRectIntegral(fitted);
    fitted = CGRectIntersection(fitted, bounds);

    // Pixels per point from the display's current mode rather than the
    // scale stored at pick time: Retina "looks like" modes all render at 2x,
    // and a display may have been switched since.
    CGFloat scale = 1.0;
    if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display))
    {
        const size_t pointWidth = CGDisplayModeGetWidth(mode);
        const size_t pixelWidth = CGDisplayModeGetPixelWidth(mode);
        if (pointWidth > 0 && pixelWidth > 0) scale = static_cast<CGFloat>(pixelWidth) / pointWidth;
        CGDisplayModeRelease(mode);
    }

    ScreenRegion out;
    out.x      = static_cast<int>(fitted.origin.x);
    out.y      = static_cast<int>(fitted.origin.y);
    out.width  = std::max(1, ToPixels(fitted.size.width, scale));
    out.height = std::max(1, ToPixels(fitted.size.height, scale));
    out.scale  = static_cast<float>(scale);

    p_Out = out;
    p_DisplayId = display;
    return true;
}

} // namespace mac
} // namespace scopedeck

// ---------------------------------------------------------------------------
// ScreenCaptureKit stream
// ---------------------------------------------------------------------------

namespace
{

const char* const kPermissionMessage =
    "Screen Recording permission is needed - System Settings > Privacy & Security > "
    "Screen Recording, enable Scope Deck, then quit and reopen it.";

// What the stream's callbacks share with the worker. Held by shared_ptr from
// both sides, so a callback that is already running when Stop() gives up on
// the stream still writes somewhere valid.
struct StreamShared
{
    std::mutex              mutex;
    std::condition_variable cv;
    CVPixelBufferRef        pending     = nullptr;   // newest frame not yet taken
    uint64_t                pendingTime = 0;         // when it arrived here, mach units
    bool                    failed      = false;
    std::string             error;

    ~StreamShared()
    {
        if (pending) CVPixelBufferRelease(pending);
    }
};

double MachToMillis(uint64_t p_Ticks)
{
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return tb;
    }();
    if (timebase.denom == 0) return 0.0;
    return static_cast<double>(p_Ticks) * timebase.numer / timebase.denom / 1.0e6;
}

std::string Describe(NSError* p_Error)
{
    if (!p_Error) return "unknown error";
    NSString* text = p_Error.localizedDescription;
    return text ? std::string(text.UTF8String) : "unknown error";
}

// ScreenCaptureKit reports a missing permission in more than one way - a
// declined code, or just a failure while the process isn't authorised - so
// the TCC preflight decides when the code alone doesn't.
std::string MessageFor(NSError* p_Error, const char* p_Doing)
{
    if ((p_Error && [p_Error.domain isEqualToString:SCStreamErrorDomain] &&
         p_Error.code == SCStreamErrorUserDeclined) ||
        !CGPreflightScreenCaptureAccess())
        return kPermissionMessage;

    if (p_Error && [p_Error.domain isEqualToString:SCStreamErrorDomain] &&
        p_Error.code == SCStreamErrorUserStopped)
        return "Capture was stopped from the macOS menu bar - choose Input > Select Capture Region... "
               "to start again.";

    return std::string(p_Doing) + ": " + Describe(p_Error);
}

// A one-shot answer from one of ScreenCaptureKit's completion handlers.
struct Completion
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done    = false;
    NSError*                error   = nil;   // retained
    SCShareableContent*     content = nil;   // retained

    ~Completion()
    {
        [error release];
        [content release];
    }

    void Finish(NSError* p_Error, SCShareableContent* p_Content)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = [p_Error retain];
            content = [p_Content retain];
            done = true;
        }
        cv.notify_all();
    }

    // Short waits, so a cancel (Stop() on the owning source) is noticed
    // promptly even if the answer never comes.
    enum class Result { Done, Cancelled, TimedOut };
    Result WaitFor(const std::atomic<bool>* p_Cancel, double p_TimeoutSeconds)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration<double>(p_TimeoutSeconds);
        std::unique_lock<std::mutex> lock(mutex);
        while (!done)
        {
            if (p_Cancel && p_Cancel->load()) return Result::Cancelled;
            if (std::chrono::steady_clock::now() >= deadline) return Result::TimedOut;
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return Result::Done;
    }
};

} // namespace

@interface SDStreamSink : NSObject <SCStreamOutput, SCStreamDelegate>
{
@public
    std::shared_ptr<StreamShared> m_Shared;
}
@end

@implementation SDStreamSink

// Runs on the stream's sample queue. Kept to a retain and a pointer swap: the
// conversion and the scopes run on ScreenCaptureSource's worker, and a frame
// the worker hasn't taken yet is simply replaced by this newer one.
- (void)stream:(SCStream*)p_Stream
    didOutputSampleBuffer:(CMSampleBufferRef)p_Sample
                   ofType:(SCStreamOutputType)p_Type
{
    (void)p_Stream;
    if (p_Type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(p_Sample)) return;

    // Idle and blank frames carry no new picture.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(p_Sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return;
    NSDictionary* info = (NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) return;

    CVImageBufferRef pixels = CMSampleBufferGetImageBuffer(p_Sample);
    if (!pixels) return;
    const uint64_t arrived = mach_absolute_time();

    CVPixelBufferRetain(pixels);
    CVPixelBufferRef replaced = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_Shared->mutex);
        replaced = m_Shared->pending;
        m_Shared->pending = pixels;
        m_Shared->pendingTime = arrived;
    }
    m_Shared->cv.notify_one();
    if (replaced) CVPixelBufferRelease(replaced);
}

// The display went away, the user stopped sharing from the menu bar, or the
// permission was revoked: the stream is over either way.
- (void)stream:(SCStream*)p_Stream didStopWithError:(NSError*)p_Error
{
    (void)p_Stream;
    const std::string message = MessageFor(p_Error, "Screen capture stopped");
    {
        std::lock_guard<std::mutex> lock(m_Shared->mutex);
        m_Shared->failed = true;
        m_Shared->error = message;
    }
    m_Shared->cv.notify_all();
}

@end

namespace scopedeck
{
namespace mac
{

struct ScreenStream::Impl
{
    std::shared_ptr<StreamShared> shared = std::make_shared<StreamShared>();
    SCStream*        stream  = nil;       // retained
    SDStreamSink*    sink    = nil;       // retained
    dispatch_queue_t queue   = nullptr;   // retained
    bool             running = false;     // startCapture succeeded
};

ScreenStream::ScreenStream() : m_Impl(new Impl) {}

ScreenStream::~ScreenStream()
{
    Stop();
    delete m_Impl;
}

bool ScreenStream::Start(uint32_t p_DisplayId, const ScreenRegion& p_Region,
                         const std::atomic<bool>& p_Cancel, std::string& p_Error)
{
    Stop();
    p_Error.clear();

    @autoreleasepool
    {
        // The list of displays is also where macOS asks for the Screen
        // Recording permission - once, with its own prompt - and where a
        // refusal shows up. Nothing here waits for the user to answer it:
        // the call fails straight away, and the permission only applies to
        // a fresh launch anyway.
        auto listed = std::make_shared<Completion>();
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* p_Content,
                                                                       NSError* p_Err) {
            listed->Finish(p_Err, p_Content);
        }];
        switch (listed->WaitFor(&p_Cancel, 10.0))
        {
            case Completion::Result::Cancelled: return false;
            case Completion::Result::TimedOut:
                p_Error = "macOS didn't answer the request for the list of displays.";
                return false;
            case Completion::Result::Done: break;
        }
        if (listed->error || !listed->content)
        {
            p_Error = MessageFor(listed->error, "Can't list the displays to capture");
            return false;
        }

        SCDisplay* display = nil;
        for (SCDisplay* d in listed->content.displays)
            if (d.displayID == p_DisplayId) display = d;
        if (!display)
        {
            p_Error = "The display the capture region was on is gone - choose Input > Select Capture Region... "
                      "again.";
            return false;
        }

        // The whole display, nothing excluded - the region is cut out by
        // sourceRect. Scope Deck's own windows are captured if they are in
        // it, as they are by the GDI grab on Windows.
        SCContentFilter* filter = [[[SCContentFilter alloc] initWithDisplay:display
                                                           excludingWindows:@[]] autorelease];

        SCStreamConfiguration* config = [[[SCStreamConfiguration alloc] init] autorelease];
        const CGRect displayFrame = display.frame;   // global CG points, like the region
        config.sourceRect = CGRectMake(p_Region.x - displayFrame.origin.x, p_Region.y - displayFrame.origin.y,
                                       p_Region.width / p_Region.scale, p_Region.height / p_Region.scale);
        config.width  = static_cast<size_t>(p_Region.width);    // pixels: one per backing pixel
        config.height = static_cast<size_t>(p_Region.height);
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.minimumFrameInterval = CMTimeMake(1, 60);
        config.showsCursor = NO;
        // The worker holds at most two frames (one being analysed, one
        // waiting), which leaves the stream room to keep drawing.
        config.queueDepth = 4;
        // sRGB, not the display's own space. The window server has already
        // matched every window into the display's profile (Display P3 on
        // most Macs), so raw display values would read an sRGB red as a
        // desaturated one. Converting back gives the values the content was
        // made with, which is what the scopes are for - and what the GDI grab
        // sees on Windows, where the desktop isn't colour-managed.
        config.colorSpaceName = kCGColorSpaceSRGB;

        m_Impl->shared = std::make_shared<StreamShared>();
        m_Impl->sink = [[SDStreamSink alloc] init];
        m_Impl->sink->m_Shared = m_Impl->shared;

        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        m_Impl->queue = dispatch_queue_create("Scope Deck screen capture", attr);

        m_Impl->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:m_Impl->sink];
        NSError* addError = nil;
        if (![m_Impl->stream addStreamOutput:m_Impl->sink
                                        type:SCStreamOutputTypeScreen
                          sampleHandlerQueue:m_Impl->queue
                                       error:&addError])
        {
            p_Error = MessageFor(addError, "Can't start the screen capture");
            Stop();
            return false;
        }

        auto started = std::make_shared<Completion>();
        [m_Impl->stream startCaptureWithCompletionHandler:^(NSError* p_Err) {
            started->Finish(p_Err, nil);
        }];
        switch (started->WaitFor(&p_Cancel, 10.0))
        {
            case Completion::Result::Cancelled:
                // It may still start; Stop() stops it if so.
                m_Impl->running = true;
                Stop();
                return false;
            case Completion::Result::TimedOut:
                p_Error = "macOS didn't start the screen capture.";
                m_Impl->running = true;
                Stop();
                return false;
            case Completion::Result::Done: break;
        }
        if (started->error)
        {
            p_Error = MessageFor(started->error, "Can't start the screen capture");
            Stop();
            return false;
        }
        m_Impl->running = true;
        return true;
    }
}

ScreenStream::Wait ScreenStream::WaitFrame(int p_TimeoutMs, ScreenFrame& p_Out)
{
    p_Out = ScreenFrame{};

    CVPixelBufferRef pixels = nullptr;
    uint64_t arrived = 0;
    {
        StreamShared& s = *m_Impl->shared;
        std::unique_lock<std::mutex> lock(s.mutex);
        s.cv.wait_for(lock, std::chrono::milliseconds(p_TimeoutMs),
                      [&s] { return s.pending != nullptr || s.failed; });
        if (s.failed) return Wait::Failed;
        if (!s.pending) return Wait::Timeout;
        pixels = s.pending;            // the reference moves to this frame
        arrived = s.pendingTime;
        s.pending = nullptr;
    }

    if (CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA ||
        CVPixelBufferIsPlanar(pixels) ||
        CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
    {
        CVPixelBufferRelease(pixels);
        return Wait::Timeout;
    }

    p_Out.bgra   = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
    p_Out.width  = static_cast<int>(CVPixelBufferGetWidth(pixels));
    p_Out.height = static_cast<int>(CVPixelBufferGetHeight(pixels));
    p_Out.stride = CVPixelBufferGetBytesPerRow(pixels);
    p_Out.buffer = pixels;

    // From the frame arriving in this process to its pixels being readable
    // here - any wait behind the previous frame's scopes plus mapping the
    // buffer. ScreenCaptureKit's own copy off the screen happens on the GPU
    // in another process and can't be timed from here: its
    // SCStreamFrameInfoDisplayTime is the vsync the frame is composited
    // for, which is still ~10 ms in the future when the frame arrives.
    const uint64_t now = mach_absolute_time();
    if (now > arrived) p_Out.msGrab = MachToMillis(now - arrived);

    if (!p_Out.bgra || p_Out.width <= 0 || p_Out.height <= 0)
    {
        Release(p_Out);
        return Wait::Timeout;
    }
    return Wait::Frame;
}

void ScreenStream::Release(ScreenFrame& p_Frame)
{
    if (p_Frame.buffer)
    {
        CVPixelBufferRef pixels = static_cast<CVPixelBufferRef>(p_Frame.buffer);
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(pixels);
    }
    p_Frame = ScreenFrame{};
}

std::string ScreenStream::Error() const
{
    std::lock_guard<std::mutex> lock(m_Impl->shared->mutex);
    return m_Impl->shared->error;
}

void ScreenStream::Stop()
{
    @autoreleasepool
    {
        if (m_Impl->stream)
        {
            if (m_Impl->running)
            {
                // Bounded: a stream that never confirms must not keep
                // ScreenCaptureSource::Stop() - and the UI thread - waiting.
                auto stopped = std::make_shared<Completion>();
                [m_Impl->stream stopCaptureWithCompletionHandler:^(NSError* p_Err) {
                    stopped->Finish(p_Err, nil);
                }];
                stopped->WaitFor(nullptr, 2.0);
            }
            [m_Impl->stream removeStreamOutput:m_Impl->sink type:SCStreamOutputTypeScreen error:nil];
            [m_Impl->stream release];
            m_Impl->stream = nil;
        }
        m_Impl->running = false;

        if (m_Impl->queue)
        {
            // Lets a sample callback already in flight finish before the
            // sink goes; it only touches the shared state, which outlives it.
            dispatch_sync(m_Impl->queue, ^{});
            dispatch_release(m_Impl->queue);
            m_Impl->queue = nullptr;
        }

        [m_Impl->sink release];
        m_Impl->sink = nil;

        std::lock_guard<std::mutex> lock(m_Impl->shared->mutex);
        if (m_Impl->shared->pending)
        {
            CVPixelBufferRelease(m_Impl->shared->pending);
            m_Impl->shared->pending = nullptr;
        }
    }
}

} // namespace mac
} // namespace scopedeck
