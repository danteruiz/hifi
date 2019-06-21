#import "CloseInterfaceController.h"
#import "Launcher.h"
#import "CustomUI.h"

@interface CloseInterfaceController()
@property (nonatomic, assign) IBOutlet NSImageView* backgroundImage;
@property (nonatomic, assign) IBOutlet NSImageView* smallLogo;
@property (nonatomic, assign) IBOutlet NSImageView* voxelImage;
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
    NSLog(@"pressed button");
}

@end
