#include <AppKit/AppKit.h>
#include <QWidget>
#import <Cocoa/Cocoa.h>

extern "C" void showLookupForString(WId winId, const char* text, double x, double y) {
    if (winId == 0 || text == nullptr) return;

    NSView* view = (NSView*)winId;
    NSString* nsText = [NSString stringWithUTF8String:text];

    NSPoint point = NSMakePoint(x, y);

    [view showDefinitionForAttributedString:[[NSAttributedString alloc] initWithString:nsText]
                                    atPoint:point];
}

extern "C" void changeTitlebarColor(WId winId, double red, double green, double blue, double alpha){
    if (winId == 0) return;
    NSView* view = (NSView*)winId;
    NSWindow* window = [view window];
    if (!window) return;
    window.titlebarAppearsTransparent = YES;
    window.backgroundColor = [NSColor colorWithRed:red green:green blue:blue alpha: alpha];
    double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
    NSAppearanceName appearanceName = (luminance < 0.5) ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    window.appearance = [NSAppearance appearanceNamed:appearanceName];
}

@interface DraggableTitleView : NSView
@property (nonatomic, assign) BOOL autoHideTrafficLights;
@end

@implementation DraggableTitleView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _autoHideTrafficLights = NO;
        [self initTrackingArea];
    }
    return self;
}

- (NSView *)hitTest:(NSPoint)point {
    NSView *hit = [super hitTest:point];
    if (hit == self && self.window != nil && self.superview != nil) {
        NSPoint windowPoint = [self.superview convertPoint:point toView:nil];
        NSWindowButton buttons[] = { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton };
        for (NSWindowButton btnType : buttons) {
            NSButton *btn = [self.window standardWindowButton:btnType];
            if (btn != nil && !btn.isHidden) {
                NSPoint btnPoint = [btn convertPoint:windowPoint fromView:nil];
                if (NSPointInRect(btnPoint, btn.bounds)) {
                    return nil;
                }
            }
        }
    }
    return hit;
}

// Handle mouse click events
- (void)mouseDown:(NSEvent *)event {
    // double-click to zoom
    if ([event clickCount] == 2) {
        [self.window zoom:nil];
    } else {
        // drag Window
        [self.window performWindowDragWithEvent:event];
    }
}

- (void)mouseMoved:(NSEvent *)event {
    [[self superview] mouseMoved:event];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *area in [self trackingAreas]) {
        [self removeTrackingArea:area];
    }
    [self initTrackingArea];
}

-(void) initTrackingArea {
    NSTrackingAreaOptions options = (NSTrackingActiveAlways | NSTrackingInVisibleRect |
            NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved);

    NSTrackingArea *area = [[NSTrackingArea alloc] initWithRect:[self bounds]
        options:options
        owner:self
        userInfo:nil];

    [self addTrackingArea:area];
}

-(void)mouseEntered:(NSEvent *)event {
    if (self.window != nil && (self.window.styleMask & NSWindowStyleMaskFullScreen) == 0) {
        [[self.window standardWindowButton: NSWindowCloseButton] setHidden:NO];
        [[self.window standardWindowButton: NSWindowMiniaturizeButton] setHidden:NO];
        [[self.window standardWindowButton: NSWindowZoomButton] setHidden:NO];
    }
}

-(void)mouseExited:(NSEvent *)event {
    if (!self.autoHideTrafficLights || self.window == nil) return;
    if ((self.window.styleMask & NSWindowStyleMaskFullScreen) == 0) {
        NSPoint localPoint = [self convertPoint:[self.window mouseLocationOutsideOfEventStream] fromView:nil];
        if (NSPointInRect(localPoint, [self bounds])) {
            return;
        }
        [[self.window standardWindowButton: NSWindowCloseButton] setHidden:YES];
        [[self.window standardWindowButton: NSWindowMiniaturizeButton] setHidden:YES];
        [[self.window standardWindowButton: NSWindowZoomButton] setHidden:YES];
    }
}

@end

extern "C" void configureMacosWindowTitleBar(WId winId, double red, double green, double blue, bool hideTrafficLights) {
    if (winId == 0) return;

    NSView* nativeView = reinterpret_cast<NSView*>(winId);
    NSWindow* nativeWindow = [nativeView window];
    if (nativeWindow == nil) return;

    NSWindowStyleMask desiredMask = [nativeWindow styleMask] | NSWindowStyleMaskFullSizeContentView;
    if ([nativeWindow styleMask] != desiredMask) {
        [nativeWindow setStyleMask:desiredMask];
    }
    [nativeWindow setTitlebarAppearsTransparent:YES];
    [nativeWindow setTitleVisibility:NSWindowTitleHidden];
    [nativeWindow setBackgroundColor:[NSColor colorWithRed:red green:green blue:blue alpha:1.0]];

    double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
    NSAppearanceName appearanceName = (luminance < 0.5) ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    [nativeWindow setAppearance:[NSAppearance appearanceNamed:appearanceName]];

    NSView* contentView = [nativeWindow contentView];
    if (contentView == nil) return;

    DraggableTitleView* titleBarView = nil;
    for (NSView* subview in [contentView subviews]) {
        if ([subview isKindOfClass:[DraggableTitleView class]]) {
            titleBarView = (DraggableTitleView*)subview;
            break;
        }
    }

    NSRect contentViewBounds = contentView.bounds;
    CGFloat barHeight = 28.0;
    BOOL flipped = [contentView isFlipped];
    CGFloat yPos = flipped ? 0.0 : (contentViewBounds.size.height - barHeight);
    NSRect barFrame = NSMakeRect(0, yPos, contentViewBounds.size.width, barHeight);

    if (titleBarView == nil) {
        titleBarView = [[DraggableTitleView alloc] initWithFrame:barFrame];
        titleBarView.autoresizingMask = flipped
            ? (NSViewWidthSizable | NSViewMaxYMargin)
            : (NSViewWidthSizable | NSViewMinYMargin);
        titleBarView.wantsLayer = YES;
        titleBarView.layer.backgroundColor = [[NSColor clearColor] CGColor];
        [contentView setAutoresizesSubviews:YES];
        [contentView addSubview:titleBarView positioned:NSWindowAbove relativeTo:nil];
    } else {
        [titleBarView setFrame:barFrame];
    }

    titleBarView.autoHideTrafficLights = hideTrafficLights ? YES : NO;

    if ((nativeWindow.styleMask & NSWindowStyleMaskFullScreen) == 0) {
        BOOL shouldHide = NO;
        if (hideTrafficLights) {
            NSPoint localPoint = [titleBarView convertPoint:[nativeWindow mouseLocationOutsideOfEventStream] fromView:nil];
            shouldHide = !NSPointInRect(localPoint, [titleBarView bounds]);
        }
        [[nativeWindow standardWindowButton: NSWindowCloseButton] setHidden:shouldHide];
        [[nativeWindow standardWindowButton: NSWindowMiniaturizeButton] setHidden:shouldHide];
        [[nativeWindow standardWindowButton: NSWindowZoomButton] setHidden:shouldHide];
    }
}

extern "C" void hideWindowTitleBar(WId winId) {
    if (winId == 0) return;
    NSView* nativeView = reinterpret_cast<NSView*>(winId);
    NSWindow* nativeWindow = [nativeView window];
    if (nativeWindow == nil) return;
    NSColor* bg = [nativeWindow backgroundColor];
    NSColor* srgb = [bg colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    double r = srgb ? [srgb redComponent] : 0.157;
    double g = srgb ? [srgb greenComponent] : 0.157;
    double b = srgb ? [srgb blueComponent] : 0.157;
    configureMacosWindowTitleBar(winId, r, g, b, true);
}

