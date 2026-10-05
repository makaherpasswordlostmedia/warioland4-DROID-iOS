#import "AppDelegate.h"
#import "MenuViewController.h"
#import "WL4Session.h"
#import <AVFoundation/AVFoundation.h>
#include "WL4Log.h"
#include "audio.h"

@implementation AppDelegate {
    NSTimer *_autosave;
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    WL4Session *s = WL4Session.shared;

    /* keep the previous session's log next to the current one (the one you want after a crash) */
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *prev = [s.documentsPath stringByAppendingPathComponent:@"wl4.prev.log"];
    if ([fm fileExistsAtPath:s.logPath]) { [fm removeItemAtPath:prev error:nil]; [fm moveItemAtPath:s.logPath toPath:prev error:nil]; }
    wl4_log_open(s.logPath.fileSystemRepresentation);

    /* Playback ignores the ring/silent switch so the game is never mysteriously mute.
     * For "respect the switch + mix with other apps" use AVAudioSessionCategoryAmbient instead. */
    AVAudioSession *as = AVAudioSession.sharedInstance;
    [as setCategory:AVAudioSessionCategoryPlayback error:nil];
    [as setActive:YES error:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(audioInterruption:)
                                               name:AVAudioSessionInterruptionNotification object:as];

    [s adoptRomsFromDocuments];
    _autosave = [NSTimer scheduledTimerWithTimeInterval:3.0 repeats:YES block:^(NSTimer *t) { [WL4Session.shared persistSave]; }];

    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [MenuViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)audioInterruption:(NSNotification *)n {
    AVAudioSessionInterruptionType type = (AVAudioSessionInterruptionType)[n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
    if (type == AVAudioSessionInterruptionTypeBegan) {
        WL4Session.shared.interrupted = YES;
    } else {
        [AVAudioSession.sharedInstance setActive:YES error:nil];
        wl4_audio_request_restart();
        WL4Session.shared.interrupted = NO;
    }
}

- (void)applicationDidBecomeActive:(UIApplication *)application {
    [AVAudioSession.sharedInstance setActive:YES error:nil];
    wl4_audio_request_restart();                     /* the unit may have been stopped while we were in the background */
    WL4Session.shared.appActive = YES;
}
- (void)applicationWillResignActive:(UIApplication *)application {
    WL4Session.shared.appActive = NO;
    [WL4Session.shared persistSave];
}
- (void)applicationDidEnterBackground:(UIApplication *)application { [WL4Session.shared persistSave]; }
- (void)applicationWillTerminate:(UIApplication *)application { [WL4Session.shared persistSave]; }

@end
