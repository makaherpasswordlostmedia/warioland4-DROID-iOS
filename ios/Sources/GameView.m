#import "GameView.h"
#import <QuartzCore/QuartzCore.h>
#include "host.h"

CGRect WL4GameRect(CGRect b, UIEdgeInsets safe, WL4ScaleMode mode, CGFloat sc) {
    BOOL land = b.size.width > b.size.height;
    CGFloat availW = b.size.width;
    CGFloat availH = land ? b.size.height : b.size.width * 160.0 / 240.0;
    CGFloat s = MIN(availW / 240.0, availH / 160.0);
    if (mode == WL4ScaleInteger && sc > 0) s = MAX(1.0, floor(s * sc)) / sc;
    CGFloat w = 240.0 * s, h = 160.0 * s;
    CGFloat x = b.origin.x + (b.size.width - w) / 2.0;
    CGFloat y = land ? b.origin.y + (b.size.height - h) / 2.0 : b.origin.y + safe.top;
    return CGRectMake(x, y, w, h);
}

@implementation GameView {
    CALayer *_screen;
    CGContextRef _ctx;
    CADisplayLink *_link;
    int _lastFrame;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = UIColor.blackColor;
        self.opaque = YES;
        _screen = [CALayer layer];
        _screen.opaque = YES;
        _screen.backgroundColor = UIColor.blackColor.CGColor;
        _screen.contentsGravity = kCAGravityResize;
        [self.layer addSublayer:_screen];

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        /* core framebuffer is XRGB8888 in a uint32: bytes B,G,R,X in memory */
        _ctx = CGBitmapContextCreate(NULL, 240, 160, 8, 240 * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGColorSpaceRelease(cs);

        _lastFrame = -1;
        _scaleMode = (WL4ScaleMode)[NSUserDefaults.standardUserDefaults integerForKey:@"wl4.scale"];
        [self applyFilter];
    }
    return self;
}

- (void)dealloc {
    [_link invalidate];
    if (_ctx) CGContextRelease(_ctx);
}

- (void)applyFilter {
    NSString *f = (_scaleMode == WL4ScaleSmooth) ? kCAFilterLinear : kCAFilterNearest;
    _screen.magnificationFilter = f;
    _screen.minificationFilter = f;
}

- (void)setScaleMode:(WL4ScaleMode)m {
    _scaleMode = m;
    [NSUserDefaults.standardUserDefaults setInteger:m forKey:@"wl4.scale"];
    [self applyFilter];
    [self setNeedsLayout];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat sc = self.window.screen.scale ?: UIScreen.mainScreen.scale;
    CGRect r = WL4GameRect(self.bounds, self.safeAreaInsets, _scaleMode, sc);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _screen.frame = r;
    [CATransaction commit];
}

- (void)start {
    if (_link) return;
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    _link.preferredFramesPerSecond = 60;
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)stop {
    [_link invalidate];
    _link = nil;
}

- (void)tick:(CADisplayLink *)link {
    uint32_t *px = (uint32_t *)CGBitmapContextGetData(_ctx);
    if (!px) return;
    int f = wl4_copy_frame(px);
    if (f == _lastFrame) return;
    _lastFrame = f;
    CGImageRef img = CGBitmapContextCreateImage(_ctx);   /* copy-on-write snapshot of the context */
    if (!img) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _screen.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img);
}

@end
