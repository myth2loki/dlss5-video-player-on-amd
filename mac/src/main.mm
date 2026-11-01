// A minimal macOS playback surface, to prove the two risky layers before any
// UI work: the ffmpeg pipe reads identically on POSIX, and a Metal drawable
// presents it at the right cadence.
//
// What this deliberately is not: the Windows player's chrome. No toolbar, no
// timeline, no compare bar, no start screen. Those are phase 2 and they are
// rewrite work, not port work.
//
// Threading, which is the only part worth reading carefully:
//   - the producer thread inside FfmpegPipeSource spawns ffmpeg and fills a
//     bounded queue;
//   - a display link ticks on its own thread and only ever calls TryPop;
//   - every Metal call, and every AppKit call, happens on the main thread.

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CVDisplayLink.h>

#include "AudioPlayer.h"
#include "FfmpegPipeSource.h"
#include "MetalVideoPresenter.h"
#include "PlaybackClock.h"

#include <algorithm>
#include <memory>

#pragma mark - View

// Owns the CAMetalLayer and the display link. The layer must be configured
// once the view has a window, which is why Init is not called from init.
@interface VideoView : NSView {
  @private
    CAMetalLayer* _layer;
    CVDisplayLinkRef _link;
    int _sourceWidth;
    int _sourceHeight;
}
@property(nonatomic, assign) MetalVideoPresenter* presenter;
@property(nonatomic, assign) BOOL fill;
- (void)attachPresenter;
- (void)resizeDrawable;
- (void)setSourceWidth:(int)width height:(int)height;
- (void)drawFrame:(const VideoFrame*)frame;
- (void)startDisplayLink:(void (^)(void))tick;
- (void)stopDisplayLink;
@end

@implementation VideoView

- (instancetype)initWithFrame:(NSRect)frameRect
{
    self = [super initWithFrame:frameRect];
    if (!self) return nil;
    self.wantsLayer = YES;
    _layer = [CAMetalLayer layer];
    _layer.frame = self.bounds;
    _layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [self setLayer:_layer];
    _sourceWidth = 1;
    _sourceHeight = 1;
    return self;
}

- (BOOL)isOpaque { return YES; }

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self resizeDrawable];
}

- (void)setFrameSize:(NSSize)newSize
{
    [super setFrameSize:newSize];
    [self resizeDrawable];
}

- (void)resizeDrawable
{
    if (!_layer) return;
    const CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
    _layer.frame = self.bounds;
    _layer.contentsScale = scale;
    // drawableSize in pixels, not points: a 2x display would otherwise present
    // a half-resolution picture.
    _layer.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                     self.bounds.size.height * scale);
}

- (void)attachPresenter
{
    if (!self.presenter) return;
    const auto result = self.presenter->Init((__bridge void*)_layer);
    if (!result.ok) NSLog(@"Metal init failed: %s", result.error.c_str());
    [self resizeDrawable];
}

- (void)setSourceWidth:(int)width height:(int)height
{
    _sourceWidth = width;
    _sourceHeight = height;
    if (self.presenter) self.presenter->SetSourceSize(width, height);
}

- (void)drawFrame:(const VideoFrame*)frame
{
    if (!frame || !self.presenter) return;
    const CGFloat scale = self.window ? self.window.backingScaleFactor : 2.0;
    self.presenter->Present(frame->bgra.data(), _sourceWidth, _sourceHeight,
                            self.bounds.size.width * scale,
                            self.bounds.size.height * scale,
                            self.fill);
}

- (void)startDisplayLink:(void (^)(void))tick
{
    if (_link) return;
    // CVDisplayLink is deprecated in macOS 15 in favour of
    // NSView.displayLink(target:selector:), but the replacement delivers on a
    // run loop rather than on a dedicated thread and would change where the
    // frame chase runs. Kept for now, deliberately, with the warning silenced
    // here rather than project-wide.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    // The handler runs on the display link's own thread. It is handed a plain
    // block that does no AppKit work, so nothing here needs a lock beyond the
    // source's own.
    CVDisplayLinkSetOutputHandler(_link, ^CVReturn(CVDisplayLinkRef link,
                                                   const CVTimeStamp* now,
                                                   const CVTimeStamp* outputTime,
                                                   CVOptionFlags flagsIn,
                                                   CVOptionFlags* flagsOut) {
        (void)link; (void)now; (void)outputTime; (void)flagsIn; (void)flagsOut;
        tick();
        return kCVReturnSuccess;
    });
    CVDisplayLinkStart(_link);
#pragma clang diagnostic pop
}

- (void)stopDisplayLink
{
    if (!_link) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVDisplayLinkStop(_link);
    CVDisplayLinkRelease(_link);
#pragma clang diagnostic pop
    _link = nullptr;
}

- (void)dealloc
{
    [self stopDisplayLink];
}

@end

#pragma mark - Controller

@interface PlayerController : NSObject
@property(nonatomic, strong) VideoView* view;
- (BOOL)openPath:(NSString*)path;
- (void)tick;
- (void)handleKey:(unichar)character;
- (void)shutdown;
- (MetalVideoPresenter*)presenter;
@end

@implementation PlayerController {
    std::unique_ptr<FfmpegPipeSource> _source;
    std::unique_ptr<MetalVideoPresenter> _presenter;
    std::unique_ptr<AudioPlayer> _audio;
    PlaybackClock _clock;
    VideoFrame _current;
    MediaInfo _info;
    BOOL _hasFrame;
    std::mutex _frameMutex;
    double _deviceCheckAt;
}

- (instancetype)init
{
    self = [super init];
    if (!self) return nil;
    _presenter = std::make_unique<MetalVideoPresenter>();
    _audio = std::make_unique<AudioPlayer>();
    return self;
}

- (MetalVideoPresenter*)presenter { return _presenter.get(); }

// Called by the default-output-device listener in AppDelegate. It only
// latches; the decision to restart is ServiceDeviceChanges', which runs on the
// tick, so the OS's notification thread never touches the pipeline.
- (void)noteOutputDeviceChanged
{
    // Nothing to latch: AudioPlayer polls the device it was opened against and
    // reports the loss itself. This exists so the callback has somewhere
    // obvious to go, and so a future listener-based implementation has a hook.
}

- (BOOL)openPath:(NSString*)path
{
    MediaInfo info;
    if (!FfmpegPipeSource::Probe(path.UTF8String, info)) {
        NSLog(@"probe failed: %@", path);
        return NO;
    }
    _info = info;
    _clock.SetFrameRate(info.fps);
    _clock.SetDuration(info.duration);

    _source = std::make_unique<FfmpegPipeSource>();
    if (!_source->Open(path.UTF8String, info)) return NO;

    // Audio first, because the clock it provides is what the picture follows.
    // A failure here is not fatal: no track or no device means the film plays
    // silently on the fallback timer.
    const bool audioStarted = _audio->Start(path.UTF8String, 0.0, AudioStartState::Playing);
    _clock.SetAudioSource(audioStarted ? _audio.get() : nullptr);
    if (audioStarted) {
        NSLog(@"audio: %s %d Hz %d ch, master clock on",
              _audio->Track().codec.c_str(), _audio->Track().sampleRate, _audio->Track().channels);
    } else {
        NSLog(@"audio: none (silent film, picture on the fallback clock)");
    }

    [self.view setSourceWidth:info.width height:info.height];
    NSLog(@"opened %dx%d %.3f fps %.3f s", info.width, info.height, info.fps, info.duration);
    return YES;
}

// Runs on the display link thread. It drains the queue, keeps only the frame
// the clock is on, and hands the newest one to the main thread to present.
- (void)tick
{
    if (!_source) return;

    const int64_t want = _clock.CurrentFrame();
    // Frames behind the timeline are dropped rather than replayed - the same
    // rule LiveSessionPolicy.h states for the Windows live session.
    if (want > 1) _source->DiscardBefore(want - 1);

    VideoFrame newest;
    bool got = false;
    VideoFrame candidate;
    // Peek first, then pop. The version this replaces popped and then broke on
    // a frame ahead of the clock, believing the comment that said the frame was
    // kept - but TryPop had already taken it out of the queue, so the frame was
    // discarded instead. Dropping one frame per tick is invisible on a short
    // clip and freezes a long one, because the producer outruns the clock and
    // the head is nearly always ahead.
    while (true) {
        int64_t next = 0;
        if (!_source->PeekFrontNumber(next)) break;
        if (next > want) break;
        if (!_source->TryPop(candidate)) break;
        newest = std::move(candidate);
        got = true;
    }

    if (got) {
        {
            std::scoped_lock lock(_frameMutex);
            _current = std::move(newest);
            _hasFrame = YES;
        }
        void (^present)(void) = ^{
            std::scoped_lock lock(self->_frameMutex);
            [self.view drawFrame:&self->_current];
        };
        dispatch_async(dispatch_get_main_queue(), present);
    }

    // Every half second, ask audio whether its endpoint went away. Cheap when
    // nothing happened, which is why the Windows player calls the equivalent
    // from its frame tick rather than from a notification handler.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const double seconds = std::chrono::duration<double>(now).count();
    if (seconds - _deviceCheckAt > 0.5) {
        _deviceCheckAt = seconds;
        if (_audio && _audio->Active()) {
            _audio->ServiceDeviceChanges(_clock.Position(), _clock.IsPlaying());
        }
    }

    if (_clock.AtEnd()) _clock.Pause();
}

- (void)handleKey:(unichar)character
{
    switch (character) {
        case ' ':
            if (_clock.IsPlaying()) {
                _audio->Pause(true);
                _clock.Pause();
            } else {
                _audio->Pause(false);
                _clock.Play();
            }
            break;
        case 'a':
        case 'A':
            _clock.SetFill(!_clock.Fill());
            self.view.fill = _clock.Fill();
            break;
        case 'm':
        case 'M':
            _audio->SetVolume(_audio->Volume() > 0.0f ? 0.0f : 1.0f);
            break;
        case NSLeftArrowFunctionKey:
            [self seekBy:-10.0];
            break;
        case NSRightArrowFunctionKey:
            [self seekBy:10.0];
            break;
        case '.':
            [self seekBy:1.0 / _clock.FrameRate()];
            break;
        default:
            break;
    }
}

- (void)seekBy:(double)delta
{
    const double target = std::max(0.0, _clock.Position() + delta);
    _clock.Seek(target);
    if (_source) _source->Seek(target);
}

- (void)shutdown
{
    [self.view stopDisplayLink];
    if (_source) _source->Close();
    _source.reset();
    _presenter->Shutdown();
}

@end

#pragma mark - App

@interface PlayerWindow : NSWindow
@property(nonatomic, strong) PlayerController* controller;
@end

@implementation PlayerWindow

- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }

- (void)keyDown:(NSEvent*)event
{
    NSString* characters = event.charactersIgnoringModifiers;
    if (characters.length > 0) {
        [self.controller handleKey:[characters characterAtIndex:0]];
    }
}

@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate {
    PlayerWindow* _window;
    PlayerController* _controller;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    const NSRect frame = NSMakeRect(0, 0, 1280, 720);
    _window = [[PlayerWindow alloc] initWithContentRect:frame
                                             styleMask:NSWindowStyleMaskTitled |
                                                       NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskResizable
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    _window.title = @"DLSS 5 Video Player - macOS port (phase 2: picture + sound)";
    _window.backgroundColor = [NSColor colorWithSRGBRed:18.0 / 255.0
                                                  green:19.0 / 255.0
                                                   blue:21.0 / 255.0
                                                  alpha:1.0];
    _window.delegate = self;
    [_window center];

    VideoView* view = [[VideoView alloc] initWithFrame:frame];
    _window.contentView = view;

    _controller = [[PlayerController alloc] init];
    _controller.view = view;
    _window.controller = _controller;

    NSArray<NSString*>* args = [NSProcessInfo processInfo].arguments;
    NSString* path = args.count > 1 ? args[1] : nil;
    if (!path) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        if ([panel runModal] == NSModalResponseOK) path = panel.URL.path;
    }
    if (!path) {
        [NSApp terminate:nil];
        return;
    }

    // The layer is only valid once the window is on screen, so the presenter
    // is attached after the order-front rather than in the view's init.
    [_window makeKeyAndOrderFront:nil];
    view.presenter = [_controller presenter];
    [view attachPresenter];
    [_window makeFirstResponder:view];

    if ([_controller openPath:path]) {
        [view startDisplayLink:^{ [_controller tick]; }];
    }
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)windowWillClose:(NSNotification*)notification
{
    [_controller shutdown];
    [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return YES; }

@end

int main(int argc, const char* argv[])
{
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate* delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
