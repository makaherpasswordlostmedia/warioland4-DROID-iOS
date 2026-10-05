#import "GameViewController.h"
#import "GameView.h"
#import "PadView.h"
#import "WL4Session.h"
#import <GameController/GameController.h>

@implementation GameViewController {
    GameView *_gv;
    PadView *_pad;
    UIButton *_menu;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;

    _gv = [[GameView alloc] initWithFrame:self.view.bounds];
    _gv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_gv];

    _pad = [[PadView alloc] initWithFrame:self.view.bounds];
    _pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _pad.scaleMode = _gv.scaleMode;
    _pad.onKeys = ^(uint16_t k) { WL4Session.shared.touchKeys = k; };
    [self.view addSubview:_pad];

    _menu = [UIButton buttonWithType:UIButtonTypeSystem];
    [_menu setTitle:@"☰" forState:UIControlStateNormal];
    _menu.titleLabel.font = [UIFont systemFontOfSize:24 weight:UIFontWeightBold];
    _menu.tintColor = [UIColor colorWithWhite:1 alpha:0.7];
    [_menu addTarget:self action:@selector(showMenu) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:_menu];

    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:self selector:@selector(controllerConnected:) name:GCControllerDidConnectNotification object:nil];
    [nc addObserver:self selector:@selector(controllerDisconnected:) name:GCControllerDidDisconnectNotification object:nil];
    for (GCController *c in GCController.controllers) [self bindController:c];
    [self updatePadVisibility];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGFloat w = self.view.bounds.size.width;
    _menu.frame = CGRectMake((w - 44) / 2, self.view.safeAreaInsets.top + 2, 44, 44);
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    [_gv start];
    WL4Session.shared.gameVisible = YES;
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    WL4Session.shared.gameVisible = NO;
    [WL4Session.shared persistSave];
    [_gv stop];
    UIApplication.sharedApplication.idleTimerDisabled = NO;
}

#pragma mark - in-game menu

- (void)showMenu {
    WL4Session *s = WL4Session.shared;
    s.userPaused = YES;
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Paused" message:nil preferredStyle:UIAlertControllerStyleAlert];
    NSArray<NSString *> *names = @[@"Sharp (fit)", @"Smooth (fit)", @"Integer scale"];
    for (NSInteger i = 0; i < 3; i++) {
        NSString *t = [NSString stringWithFormat:@"%@%@", names[i], _gv.scaleMode == i ? @"  ✓" : @""];
        [a addAction:[UIAlertAction actionWithTitle:t style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
            self->_gv.scaleMode = (WL4ScaleMode)i;
            self->_pad.scaleMode = (WL4ScaleMode)i;
            s.userPaused = NO;
        }]];
    }
    [a addAction:[UIAlertAction actionWithTitle:@"Back to menu" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
        s.userPaused = NO;
        [self dismissViewControllerAnimated:YES completion:nil];
    }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Resume" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { s.userPaused = NO; }]];
    [self presentViewController:a animated:YES completion:nil];
}

#pragma mark - game controllers (MFi / Xbox / DualShock / DualSense)

- (void)bindController:(GCController *)c {
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g) return;
    g.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el) {
        uint16_t k = 0;
        if (gp.buttonA.pressed) k |= WL4KeyA;                              /* same mapping as the Android port: A=A, B/X=B */
        if (gp.buttonB.pressed || gp.buttonX.pressed) k |= WL4KeyB;
        if (gp.rightShoulder.pressed) k |= WL4KeyR;
        if (gp.leftShoulder.pressed) k |= WL4KeyL;
        if (gp.buttonMenu.pressed) k |= WL4KeyStart;
        if (gp.buttonOptions && gp.buttonOptions.pressed) k |= WL4KeySelect;
        float x = gp.leftThumbstick.xAxis.value, y = gp.leftThumbstick.yAxis.value;
        if (gp.dpad.left.pressed || x < -0.5f) k |= WL4KeyLeft;
        if (gp.dpad.right.pressed || x > 0.5f) k |= WL4KeyRight;
        if (gp.dpad.up.pressed || y > 0.5f) k |= WL4KeyUp;
        if (gp.dpad.down.pressed || y < -0.5f) k |= WL4KeyDown;
        WL4Session.shared.padKeys = k;
    };
}

- (void)controllerConnected:(NSNotification *)n {
    [self bindController:(GCController *)n.object];
    [self updatePadVisibility];
}
- (void)controllerDisconnected:(NSNotification *)n {
    WL4Session.shared.padKeys = 0;
    [self updatePadVisibility];
}
- (void)updatePadVisibility {
    BOOL haveGamepad = NO;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad) haveGamepad = YES;
    _pad.hidden = haveGamepad;                      /* a real controller is connected: get the overlay out of the way */
}

#pragma mark - chrome

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (BOOL)shouldAutorotate { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskAllButUpsideDown; }

@end
