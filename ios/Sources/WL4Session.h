#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/* GBA KEYINPUT bit order, same as host.h / the Android port */
enum {
    WL4KeyA = 1 << 0, WL4KeyB = 1 << 1, WL4KeySelect = 1 << 2, WL4KeyStart = 1 << 3,
    WL4KeyRight = 1 << 4, WL4KeyLeft = 1 << 5, WL4KeyUp = 1 << 6, WL4KeyDown = 1 << 7,
    WL4KeyR = 1 << 8, WL4KeyL = 1 << 9
};

/* Owns everything that outlives a single screen: ROM + save files, the "should the core run right now" decision,
 * and the merged key state.  The core (host.c) is started once and then lives until the app is killed - the wasm2c
 * instance is never freed, so stop/start cycles would leak ~225 MB each. */
@interface WL4Session : NSObject
+ (instancetype)shared;

@property (nonatomic, readonly) NSString *documentsPath, *romPath, *savePath, *logPath;
@property (nonatomic, readonly) BOOL started;

/* pause inputs - the core runs only while the app is active, the game screen is visible, and nothing else pauses it */
@property (nonatomic) BOOL appActive, gameVisible, userPaused, interrupted;
/* key sources, merged into one mask for the core */
@property (nonatomic) uint16_t touchKeys, padKeys;

- (BOOL)hasRom;
- (void)adoptRomsFromDocuments;                              /* *.gba dropped into Documents via Files/iTunes -> wl4.gba */
- (nullable NSString *)importRomData:(NSData *)data;         /* nil on success, otherwise a user-visible reason */
- (nullable NSString *)startIfNeeded;                        /* nil on success, otherwise a user-visible reason */
- (void)persistSave;                                         /* writes SRAM to Documents/wl4.sav if it changed */
@end

NS_ASSUME_NONNULL_END
