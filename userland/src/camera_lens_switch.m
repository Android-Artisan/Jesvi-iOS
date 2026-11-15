/* Camera-only auxiliary lens buttons. No sensor or brightness writes in UI. */
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
static NSString *const control=@"/var/mobile/Library/Caches/com.apple.mediaserverd/redwood-rear-zoom";
@interface RWCameraLensWindow:UIWindow @end
@implementation RWCameraLensWindow
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e {
    UIView *v=[super hitTest:p withEvent:e];
    return [v isKindOfClass:UIButton.class]?v:nil;
}
@end
@interface RWCameraLensControls:NSObject
@property(nonatomic,strong) RWCameraLensWindow *window;
@property(nonatomic,strong) UIButton *wide;
@property(nonatomic,strong) UIButton *main;
- (void)refresh;
@end
@implementation RWCameraLensControls
- (void)select:(UIButton *)sender {
    NSString *value=sender.tag==6?@"0.6\n":@"1\n";
    NSError *error=nil;
    if(![value writeToFile:control atomically:YES encoding:NSUTF8StringEncoding error:&error]){NSLog(@"rear lens switch write failed: %@",error);return;}
    self.wide.selected=sender.tag==6;self.main.selected=sender.tag!=6;
}
- (void)refresh {
    UIApplication *app=UIApplication.sharedApplication;
    if(app.applicationState!=UIApplicationStateActive){self.window.hidden=YES;return;}
    UIWindowScene *scene=nil;
    for(UIScene *s in app.connectedScenes)if([s isKindOfClass:UIWindowScene.class]&&s.activationState==UISceneActivationStateForegroundActive){scene=(UIWindowScene*)s;break;}
    if(!scene)return;
    if(!self.window || self.window.windowScene!=scene){
        self.window=[[RWCameraLensWindow alloc]initWithWindowScene:scene];
        self.window.rootViewController=[UIViewController new];
        self.window.backgroundColor=UIColor.clearColor;self.window.windowLevel=UIWindowLevelNormal+1;
        self.wide=[UIButton buttonWithType:UIButtonTypeSystem];self.main=[UIButton buttonWithType:UIButtonTypeSystem];
        self.wide.tag=6;self.main.tag=10;
        [self.wide setTitle:@"0.6×" forState:UIControlStateNormal];[self.main setTitle:@"1×" forState:UIControlStateNormal];
        for(UIButton *b in @[self.wide,self.main]){
            b.backgroundColor=[UIColor colorWithWhite:0 alpha:.65];b.layer.cornerRadius=21;
            b.titleLabel.font=[UIFont boldSystemFontOfSize:17];
            [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];[b setTitleColor:UIColor.systemYellowColor forState:UIControlStateSelected];
            [b addTarget:self action:@selector(select:) forControlEvents:UIControlEventTouchUpInside];
            [self.window.rootViewController.view addSubview:b];
        }
    }
    CGRect bounds=scene.coordinateSpace.bounds;self.window.frame=bounds;
    CGFloat x=CGRectGetWidth(bounds)/2,y=CGRectGetHeight(bounds)-self.window.safeAreaInsets.bottom-235;
    self.wide.frame=CGRectMake(x-64,y,56,42);self.main.frame=CGRectMake(x+8,y,56,42);
    NSString *value=[NSString stringWithContentsOfFile:control encoding:NSUTF8StringEncoding error:nil];
    self.wide.selected=value.floatValue>0&&value.floatValue<1;self.main.selected=!self.wide.selected;
    self.window.hidden=NO;
}
@end
__attribute__((constructor))static void start(void){
    @autoreleasepool{
        if(![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.camera"])return;
        dispatch_async(dispatch_get_main_queue(),^{
            static RWCameraLensControls *controls;controls=[RWCameraLensControls new];
            NSTimer *timer=[NSTimer timerWithTimeInterval:.5 repeats:YES block:^(__unused NSTimer *t){[controls refresh];}];
            [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];[controls refresh];
        });
    }
}
