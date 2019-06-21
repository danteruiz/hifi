#import "CloseInterfaceController.h"
#import "Launcher.h"
#import "CustomUI.h"

@interface CloseInterfaceController()
@property (nonatomic, assign) IBOutlet NSImageView* backgroundImage;
@property (nonatomic, assign) IBOutlet NSImageView* smallLogo;
@property (nonatomic, assign) IBOutlet NSImageView* voxelImage;
@property (nonatomic, assign) IBOutlet NSButton* button;
@property (nonatomic, assign) IBOutlet NSTextField* headerText;
@property (nonatomic, assign) IBOutlet NSTextField* bodyText;
@end

@implementation CloseInterfaceController

- (void) awakeFromNib
{
    [self.backgroundImage setImage:[NSImage imageNamed:hifiBackgroundFilename]];
    [self.smallLogo setImage:[NSImage imageNamed:hifiSmallLogoFilename]];
    [self.voxelImage setImage:[NSImage imageNamed:hifiVoxelFilename]];
}

-(IBAction) closeAndUpdate:(id)sender
{
    Launcher* launcher = [Launcher sharedLauncher];

    if (![launcher isWaitingForInterfaceToTerminate]) {
        [launcher continueWithDownload];
    } else {
        [self.headerText setStringValue:@"Please close HQ"];
        [self.bodyText setStringValue:@""];
        [self.button setHidden: YES];
        [NSTimer scheduledTimerWithTimeInterval:1.0
                                         target:self
                                       selector:@selector(shouldContinue:)
                                       userInfo:nil
                                        repeats:YES];
    }
}

- (void) shouldContinue:(NSTimer*) timer
{
    Launcher* launcher = [Launcher sharedLauncher];
    if (![launcher isWaitingForInterfaceToTerminate]) {
        [timer invalidate];
        [launcher continueWithDownload];
    }
}

@end
