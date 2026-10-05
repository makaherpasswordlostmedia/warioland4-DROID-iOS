#import <UIKit/UIKit.h>
#import "GameView.h"

/* Translucent on-screen GBA pad (D-pad as four overlapping circles so diagonals work, A/B, L/R, Start/Select).
 * Multi-touch; sliding a finger between buttons works. */
@interface PadView : UIView
@property (nonatomic, copy) void (^onKeys)(uint16_t keys);
@property (nonatomic) WL4ScaleMode scaleMode;     /* portrait layout depends on where the picture ends */
@end
