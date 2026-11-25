/* Cocoa notification and Dock-tile integration. All calls are on the UI thread. */
#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>
#include <stdbool.h>

bool desktop_signal_macos_notify(const char *title, const char *body)
{
    @autoreleasepool {
        @try {
            NSString *identifier = [[NSBundle mainBundle] bundleIdentifier];
            NSString *summary = [NSString stringWithUTF8String:title];
            NSString *message = [NSString stringWithUTF8String:body];
            if (![identifier length] || !summary || !message) return false;
            UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
            UNMutableNotificationContent *content = [[[UNMutableNotificationContent alloc] init] autorelease];
            if (!center || !content) return false;
            content.title = summary;
            content.body = message;
            UNNotificationRequest *request = [UNNotificationRequest
                requestWithIdentifier:[[NSUUID UUID] UUIDString] content:content trigger:nil];
            if (!request) return false;
            /* The OS owns the copied completion block. Authorization is only
             * requested after the application's own policy has approved. */
            [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                completionHandler:^(BOOL granted, NSError *error) {
                    (void)error;
                    if (granted) [center addNotificationRequest:request withCompletionHandler:nil];
                }];
            return true;
        } @catch (NSException *exception) {
            (void)exception;
            return false;
        }
    }
}

/* The Dock has no system progress API. A custom Dock tile preserves the app
 * icon and renders the current state; an indeterminate bar animates on the
 * main run loop. */
@interface TermicaDockProgress : NSView {
    int state;
    int percent;
    unsigned phase;
    NSTimer *timer;
}
- (void)setProgressState:(int)newState percent:(int)newPercent;
- (void)stopAnimation;
@end

@implementation TermicaDockProgress
- (void)tick:(NSTimer *)unused
{
    (void)unused;
    phase = (phase + 1) % 40;
    [self setNeedsDisplay:YES];
    [[NSApp dockTile] display];
}
- (void)stopAnimation
{
    [timer invalidate];
    timer = nil;
}
- (void)setProgressState:(int)newState percent:(int)newPercent
{
    state = newState;
    percent = newPercent;
    if (state == 3 && !timer)
        timer = [NSTimer scheduledTimerWithTimeInterval:0.1 target:self
            selector:@selector(tick:) userInfo:nil repeats:YES];
    else if (state != 3)
        [self stopAnimation];
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSRect bounds = [self bounds];
    NSImage *icon = [NSApp applicationIconImage];
    [icon drawInRect:bounds fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
    NSRect bar = NSMakeRect(bounds.size.width * 0.10, bounds.size.height * 0.10,
                            bounds.size.width * 0.80, bounds.size.height * 0.11);
    [[NSColor colorWithWhite:0.0 alpha:0.75] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:bar.size.height / 2
                                    yRadius:bar.size.height / 2] fill];
    NSRect fill = NSInsetRect(bar, 2, 2);
    if (state == 3) {
        CGFloat width = fill.size.width * 0.3;
        fill.origin.x += (fill.size.width - width) * (phase < 20 ? phase : 39 - phase) / 19.0;
        fill.size.width = width;
    } else {
        fill.size.width *= percent / 100.0;
    }
    NSColor *color = state == 2 ? [NSColor systemRedColor] :
                     state == 4 ? [NSColor systemOrangeColor] : [NSColor systemBlueColor];
    [color setFill];
    if (fill.size.width > 0)
        [[NSBezierPath bezierPathWithRoundedRect:fill xRadius:fill.size.height / 2
                                        yRadius:fill.size.height / 2] fill];
}
@end

bool desktop_signal_macos_progress(int state, int progress)
{
    static TermicaDockProgress *view;
    static int last_percent;
    @autoreleasepool {
        @try {
            NSDockTile *dock = [NSApp dockTile];
            if (!dock) return false;
            if (state == 0) {
                [view stopAnimation];
                [dock setContentView:nil];
                [view release];
                view = nil;
            } else {
                if (state != 3 && progress >= 0) last_percent = progress;
                if (!view) {
                    NSSize size = [dock size];
                    view = [[TermicaDockProgress alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
                    if (!view) return false;
                    [dock setContentView:view];
                }
                [view setProgressState:state percent:last_percent];
            }
            [dock display];
            return true;
        } @catch (NSException *exception) {
            (void)exception;
            return false;
        }
    }
}
