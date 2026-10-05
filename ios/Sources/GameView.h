#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, WL4ScaleMode) {
    WL4ScaleSharp = 0,     /* fit the screen, nearest-neighbour */
    WL4ScaleSmooth = 1,    /* fit the screen, bilinear */
    WL4ScaleInteger = 2,   /* largest integer multiple of device pixels, nearest-neighbour (black borders) */
};

/* Where the 240x160 picture goes inside `bounds`: landscape = centred, portrait = full width below the top safe area. */
CGRect WL4GameRect(CGRect bounds, UIEdgeInsets safe, WL4ScaleMode mode, CGFloat screenScale);

/* Shows the core's framebuffer.  A CADisplayLink pulls wl4_copy_frame() and hands it to a CALayer as a CGImage -
 * 240x160 is tiny, so this is cheap and needs no GL/Metal. */
@interface GameView : UIView
@property (nonatomic) WL4ScaleMode scaleMode;     /* persisted in NSUserDefaults */
- (void)start;
- (void)stop;
@end
