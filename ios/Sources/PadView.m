#import "PadView.h"
#import "WL4Session.h"

@interface PadButton : NSObject
@property (nonatomic) uint16_t key;
@property (nonatomic) CGPoint c;
@property (nonatomic) CGFloat r;
@property (nonatomic, copy) NSString *label;
@end
@implementation PadButton
@end

@implementation PadView {
    NSArray<PadButton *> *_buttons;
    NSMutableSet<UITouch *> *_active;
    uint16_t _mask;
    UIImpactFeedbackGenerator *_haptic;
}

static PadButton *B(uint16_t key, NSString *label) {
    PadButton *b = [PadButton new];
    b.key = key;
    b.label = label;
    return b;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.multipleTouchEnabled = YES;
        self.opaque = NO;
        self.backgroundColor = UIColor.clearColor;
        _active = [NSMutableSet set];
        _haptic = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        _buttons = @[B(WL4KeyUp, @"▲"), B(WL4KeyDown, @"▼"), B(WL4KeyLeft, @"◀"), B(WL4KeyRight, @"▶"),
                     B(WL4KeyA, @"A"), B(WL4KeyB, @"B"), B(WL4KeyL, @"L"), B(WL4KeyR, @"R"),
                     B(WL4KeyStart, @"Start"), B(WL4KeySelect, @"Sel")];
    }
    return self;
}

- (PadButton *)button:(uint16_t)key {
    for (PadButton *b in _buttons) if (b.key == key) return b;
    return nil;
}
- (void)place:(uint16_t)key x:(CGFloat)x y:(CGFloat)y r:(CGFloat)r {
    PadButton *b = [self button:key];
    b.c = CGPointMake(x, y);
    b.r = r;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect bd = self.bounds;
    UIEdgeInsets sa = self.safeAreaInsets;
    CGFloat w = bd.size.width, h = bd.size.height;
    CGFloat u = MIN(w, h) / 7.0;
    CGFloat sc = self.window.screen.scale ?: UIScreen.mainScreen.scale;

    if (w > h) {                                       /* landscape: pad floats over the picture, like the Android port */
        CGFloat l = sa.left, rr = sa.right, bot = sa.bottom * 0.5;
        CGFloat cx = l + 1.9 * u, cy = h - 2.4 * u - bot;
        [self place:WL4KeyUp x:cx y:cy - 1.2 * u r:0.7 * u];
        [self place:WL4KeyDown x:cx y:cy + 1.2 * u r:0.7 * u];
        [self place:WL4KeyLeft x:cx - 1.2 * u y:cy r:0.7 * u];
        [self place:WL4KeyRight x:cx + 1.2 * u y:cy r:0.7 * u];
        [self place:WL4KeyA x:w - rr - 1.2 * u y:h - 2.7 * u - bot r:0.8 * u];
        [self place:WL4KeyB x:w - rr - 2.9 * u y:h - 1.5 * u - bot r:0.8 * u];
        [self place:WL4KeyL x:l + 1.0 * u y:0.8 * u + sa.top r:0.6 * u];
        [self place:WL4KeyR x:w - rr - 1.0 * u y:0.8 * u + sa.top r:0.6 * u];
        [self place:WL4KeySelect x:w / 2 - 1.1 * u y:h - 0.7 * u - bot r:0.5 * u];
        [self place:WL4KeyStart x:w / 2 + 1.1 * u y:h - 0.7 * u - bot r:0.5 * u];
    } else {                                           /* portrait: picture on top, pad underneath */
        CGFloat gameBottom = CGRectGetMaxY(WL4GameRect(bd, sa, _scaleMode, sc));
        CGFloat region = h - sa.bottom - gameBottom;
        CGFloat cx = 2.0 * u, cy = gameBottom + region * 0.55;
        [self place:WL4KeyL x:1.0 * u y:gameBottom + 0.9 * u r:0.6 * u];
        [self place:WL4KeyR x:w - 1.0 * u y:gameBottom + 0.9 * u r:0.6 * u];
        [self place:WL4KeyUp x:cx y:cy - 1.2 * u r:0.7 * u];
        [self place:WL4KeyDown x:cx y:cy + 1.2 * u r:0.7 * u];
        [self place:WL4KeyLeft x:cx - 1.2 * u y:cy r:0.7 * u];
        [self place:WL4KeyRight x:cx + 1.2 * u y:cy r:0.7 * u];
        [self place:WL4KeyA x:w - 1.3 * u y:cy - 0.7 * u r:0.8 * u];
        [self place:WL4KeyB x:w - 3.0 * u y:cy + 0.5 * u r:0.8 * u];
        [self place:WL4KeySelect x:w / 2 - 1.1 * u y:h - sa.bottom - 0.8 * u r:0.5 * u];
        [self place:WL4KeyStart x:w / 2 + 1.1 * u y:h - sa.bottom - 0.8 * u r:0.5 * u];
    }
    [self setNeedsDisplay];
}

- (void)setScaleMode:(WL4ScaleMode)m { _scaleMode = m; [self setNeedsLayout]; }

- (void)drawRect:(CGRect)rect {
    CGContextRef g = UIGraphicsGetCurrentContext();
    for (PadButton *b in _buttons) {
        BOOL down = (_mask & b.key) != 0;
        CGRect cr = CGRectMake(b.c.x - b.r, b.c.y - b.r, 2 * b.r, 2 * b.r);
        CGContextSetFillColorWithColor(g, [UIColor colorWithWhite:1 alpha:down ? 0.55 : 0.22].CGColor);
        CGContextFillEllipseInRect(g, cr);
        CGContextSetStrokeColorWithColor(g, [UIColor colorWithWhite:1 alpha:0.5].CGColor);
        CGContextSetLineWidth(g, 1.5);
        CGContextStrokeEllipseInRect(g, CGRectInset(cr, 0.75, 0.75));
        CGFloat fs = (b.label.length > 2) ? b.r * 0.45 : b.r * 0.7;
        NSDictionary *at = @{NSFontAttributeName: [UIFont boldSystemFontOfSize:fs],
                             NSForegroundColorAttributeName: [UIColor colorWithWhite:1 alpha:0.9]};
        CGSize ts = [b.label sizeWithAttributes:at];
        [b.label drawAtPoint:CGPointMake(b.c.x - ts.width / 2, b.c.y - ts.height / 2) withAttributes:at];
    }
}

- (void)recompute {
    uint16_t m = 0;
    for (UITouch *t in _active) {
        CGPoint p = [t locationInView:self];
        for (PadButton *b in _buttons) {
            CGFloat dx = p.x - b.c.x, dy = p.y - b.c.y, rr = b.r * 1.25;
            if (dx * dx + dy * dy <= rr * rr) m |= b.key;
        }
    }
    if (m == _mask) return;
    if (m & ~_mask) [_haptic impactOccurred];
    _mask = m;
    [self setNeedsDisplay];
    if (_onKeys) _onKeys(m);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [_active unionSet:touches]; [self recompute]; }
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self recompute]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [_active minusSet:touches]; [self recompute]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [_active minusSet:touches]; [self recompute]; }

- (void)setHidden:(BOOL)hidden {
    [super setHidden:hidden];
    if (hidden) { [_active removeAllObjects]; [self recompute]; }
}

@end
