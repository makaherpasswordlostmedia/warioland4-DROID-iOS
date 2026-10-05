#import "MenuViewController.h"
#import "GameViewController.h"
#import "WL4Session.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface MenuViewController () <UIDocumentPickerDelegate>
@end

@implementation MenuViewController {
    UILabel *_status;
    UIButton *_play;
}

static UIButton *MakeButton(NSString *title, id target, SEL action) {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
    [b addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
    return b;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    UILabel *title = [UILabel new];
    title.text = @"Wario Land 4";
    title.font = [UIFont systemFontOfSize:34 weight:UIFontWeightHeavy];
    title.textAlignment = NSTextAlignmentCenter;

    _status = [UILabel new];
    _status.numberOfLines = 0;
    _status.textAlignment = NSTextAlignmentCenter;
    _status.textColor = UIColor.secondaryLabelColor;

    _play = MakeButton(@"Play", self, @selector(play));
    UIButton *pick = MakeButton(@"Select ROM…", self, @selector(pickRom:));
    UIButton *logBtn = MakeButton(@"Share log", self, @selector(shareLog:));

    UILabel *note = [UILabel new];
    note.numberOfLines = 0;
    note.textAlignment = NSTextAlignmentCenter;
    note.textColor = UIColor.tertiaryLabelColor;
    note.font = [UIFont systemFontOfSize:13];
    note.text = @"This app ships no game data. Pick your own ROM once, or drop a .gba into the app's folder in Files.";

    UIStackView *st = [[UIStackView alloc] initWithArrangedSubviews:@[title, _status, _play, pick, logBtn, note]];
    st.axis = UILayoutConstraintAxisVertical;
    st.spacing = 18;
    st.alignment = UIStackViewAlignmentFill;
    st.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:st];
    UILayoutGuide *g = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [st.centerYAnchor constraintEqualToAnchor:g.centerYAnchor],
        [st.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
        [st.widthAnchor constraintLessThanOrEqualToConstant:420],
        [st.leadingAnchor constraintGreaterThanOrEqualToAnchor:g.leadingAnchor constant:24],
        [st.trailingAnchor constraintLessThanOrEqualToAnchor:g.trailingAnchor constant:-24],
    ]];
    NSLayoutConstraint *w = [st.widthAnchor constraintEqualToConstant:420];
    w.priority = UILayoutPriorityDefaultHigh;
    w.active = YES;
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refresh];
}

- (void)refresh {
    WL4Session *s = WL4Session.shared;
    [s adoptRomsFromDocuments];
    BOOL has = [s hasRom];
    _play.enabled = has;
    NSString *rom = has ? @"ROM ready." : @"No ROM selected.";
    _status.text = s.started ? [rom stringByAppendingString:@"\nGame is running in the background (paused)."] : rom;
}

- (void)alert:(NSString *)title message:(NSString *)msg {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:msg preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)play {
    NSString *err = [WL4Session.shared startIfNeeded];
    if (err) { [self alert:@"Can't start" message:err]; return; }
    GameViewController *vc = [GameViewController new];
    vc.modalPresentationStyle = UIModalPresentationFullScreen;
    [self presentViewController:vc animated:YES completion:nil];
}

- (void)pickRom:(UIButton *)sender {
    UTType *gba = [UTType typeWithFilenameExtension:@"gba"];
    NSArray<UTType *> *types = gba ? @[gba, UTTypeData] : @[UTTypeData];
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
    p.delegate = self;
    p.allowsMultipleSelection = NO;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *u = urls.firstObject;
    if (!u) return;
    BOOL scoped = [u startAccessingSecurityScopedResource];
    NSData *d = [NSData dataWithContentsOfURL:u];
    if (scoped) [u stopAccessingSecurityScopedResource];
    if (!d) { [self alert:@"Couldn't read the file" message:nil]; return; }
    NSString *err = [WL4Session.shared importRomData:d];
    [self refresh];
    if (err) [self alert:@"ROM" message:err];
}

- (void)shareLog:(UIButton *)sender {
    NSURL *u = [NSURL fileURLWithPath:WL4Session.shared.logPath];
    UIActivityViewController *a = [[UIActivityViewController alloc] initWithActivityItems:@[u] applicationActivities:nil];
    a.popoverPresentationController.sourceView = sender;
    a.popoverPresentationController.sourceRect = sender.bounds;
    [self presentViewController:a animated:YES completion:nil];
}

@end
