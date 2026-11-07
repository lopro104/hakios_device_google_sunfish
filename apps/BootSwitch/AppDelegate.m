#import "AppDelegate.h"
#import <notify.h>

@interface ViewController : UIViewController

@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *subtitleLabel;
@property (nonatomic, strong) UIButton *androidButton;
@property (nonatomic, strong) UIButton *rebootButton;
@property (nonatomic, strong) UILabel *statusLabel;

@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.distribution = UIStackViewDistributionEqualSpacing;
    stack.spacing = 16.0;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.leadingAnchor constant:24.0],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.trailingAnchor constant:-24.0]
    ]];

    self.titleLabel = [[UILabel alloc] init];
    self.titleLabel.text = @"Boot Switch";
    self.titleLabel.font = [UIFont boldSystemFontOfSize:32.0];
    self.titleLabel.textColor = [UIColor whiteColor];
    self.titleLabel.textAlignment = NSTextAlignmentCenter;
    [stack addArrangedSubview:self.titleLabel];

    self.subtitleLabel = [[UILabel alloc] init];
    self.subtitleLabel.text = @"This is iOS (HakiOS). Android is in the other slot.";
    self.subtitleLabel.font = [UIFont systemFontOfSize:16.0 weight:UIFontWeightRegular];
    self.subtitleLabel.textColor = [UIColor lightGrayColor];
    self.subtitleLabel.textAlignment = NSTextAlignmentCenter;
    self.subtitleLabel.numberOfLines = 0;
    [stack addArrangedSubview:self.subtitleLabel];

    UIView *spacer = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 10, 16)];
    [stack addArrangedSubview:spacer];

    // Button: Restart into Android
    self.androidButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.androidButton setTitle:@"Restart into Android" forState:UIControlStateNormal];
    self.androidButton.titleLabel.font = [UIFont boldSystemFontOfSize:18.0];
    [self.androidButton setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    self.androidButton.backgroundColor = [UIColor systemRedColor];
    self.androidButton.layer.cornerRadius = 14.0;
    self.androidButton.contentEdgeInsets = UIEdgeInsetsMake(16, 28, 16, 28);
    [self.androidButton addTarget:self action:@selector(handleAndroidRestartTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:self.androidButton];

    // Button: Normal Reboot
    self.rebootButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.rebootButton setTitle:@"Reboot" forState:UIControlStateNormal];
    self.rebootButton.titleLabel.font = [UIFont boldSystemFontOfSize:18.0];
    [self.rebootButton setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    self.rebootButton.backgroundColor = [UIColor colorWithWhite:0.25 alpha:1.0];
    self.rebootButton.layer.cornerRadius = 14.0;
    self.rebootButton.contentEdgeInsets = UIEdgeInsetsMake(16, 28, 16, 28);
    [self.rebootButton addTarget:self action:@selector(handleNormalRebootTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:self.rebootButton];

    // Keep button widths uniform
    [self.rebootButton.widthAnchor constraintEqualToAnchor:self.androidButton.widthAnchor].active = YES;

    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.font = [UIFont systemFontOfSize:18.0 weight:UIFontWeightMedium];
    self.statusLabel.textColor = [UIColor systemOrangeColor];
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.hidden = YES;
    [stack addArrangedSubview:self.statusLabel];
}

- (void)handleAndroidRestartTapped {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Restart into Android?"
                                                                   message:@"iOS will shut down."
                                                            preferredStyle:UIAlertControllerStyleAlert];

    UIAlertAction *cancelAction = [UIAlertAction actionWithTitle:@"Cancel"
                                                           style:UIAlertActionStyleCancel
                                                         handler:nil];

    UIAlertAction *restartAction = [UIAlertAction actionWithTitle:@"Restart"
                                                            style:UIAlertActionStyleDestructive
                                                          handler:^(UIAlertAction * _Nonnull action) {
        [self performAndroidRestart];
    }];

    [alert addAction:cancelAction];
    [alert addAction:restartAction];

    [self presentViewController:alert animated:YES completion:nil];
}

- (void)performAndroidRestart {
    // Notify privileged daemon to reboot to Android
    notify_post("com.hakios.next-boot-android");

    // Show "Restarting…" and do nothing else
    self.androidButton.hidden = YES;
    self.rebootButton.hidden = YES;
    self.statusLabel.text = @"Restarting…";
    self.statusLabel.hidden = NO;
}

- (void)handleNormalRebootTapped {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Reboot Phone?"
                                                                   message:@"iOS will restart."
                                                            preferredStyle:UIAlertControllerStyleAlert];

    UIAlertAction *cancelAction = [UIAlertAction actionWithTitle:@"Cancel"
                                                           style:UIAlertActionStyleCancel
                                                         handler:nil];

    UIAlertAction *rebootAction = [UIAlertAction actionWithTitle:@"Reboot"
                                                           style:UIAlertActionStyleDefault
                                                         handler:^(UIAlertAction * _Nonnull action) {
        [self performNormalReboot];
    }];

    [alert addAction:cancelAction];
    [alert addAction:rebootAction];

    [self presentViewController:alert animated:YES completion:nil];
}

- (void)performNormalReboot {
    // Notify privileged daemon for regular reboot
    notify_post("com.hakios.reboot");
    notify_post("com.hakios.restart");

    // Show "Rebooting…" and do nothing else
    self.androidButton.hidden = YES;
    self.rebootButton.hidden = YES;
    self.statusLabel.text = @"Rebooting…";
    self.statusLabel.hidden = NO;
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    return UIStatusBarStyleLightContent;
}

@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [[ViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
