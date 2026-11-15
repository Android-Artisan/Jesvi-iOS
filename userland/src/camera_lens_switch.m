/* Use Camera's existing lens selector. No overlay window or sensor writes here. */
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <string.h>
#ifndef RW_LENS_CONTROL_PATH
#define RW_LENS_CONTROL_PATH @"/var/mobile/Library/Caches/com.apple.mediaserverd/redwood-rear-zoom"
#endif
static NSString *const control=RW_LENS_CONTROL_PATH;
static char proxyKey;
static SEL selectionSelector(void) {
    return NSSelectorFromString(@"configureForSelectionOnlyWithDisplayZoomFactors:selectedIndex:compactPlatterLayout:animated:");
}
static NSUInteger selectedIndex(void) {
    double z=[NSString stringWithContentsOfFile:control encoding:NSUTF8StringEncoding error:nil].doubleValue;
    return z>0&&z<1?0:1;
}
static void showSelection(id zoom,NSUInteger index,BOOL animated) {
    ((void(*)(id,SEL,id,NSUInteger,BOOL,BOOL))objc_msgSend)(zoom,selectionSelector(),@[@0.6,@1.0],index,NO,animated);
}
@interface RWNativeLensDelegate:NSObject
@property(nonatomic,weak) id original;
@property(nonatomic,weak) id zoom;
@property(nonatomic) BOOL rear;
@property(nonatomic) NSUInteger index;
@end
@implementation RWNativeLensDelegate
- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector]||[self.original respondsToSelector:selector];
}
- (id)forwardingTargetForSelector:(SEL)selector {return self.original;}
- (void)zoomControl:(id)zoom didChangeSelectedButtonIndex:(NSUInteger)index displayZoomFactor:(double)factor {
    if(!self.rear||index>1||(factor!=0.6&&factor!=1.0)) {
        SEL s=_cmd;
        if([self.original respondsToSelector:s])((void(*)(id,SEL,id,NSUInteger,double))objc_msgSend)(self.original,s,zoom,index,factor);
        return;
    }
    NSError *error=nil;
    if(![(index==0?@"0.6\n":@"1\n") writeToFile:control atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
        NSLog(@"redwood native lens write failed: %@",error);showSelection(zoom,selectedIndex(),NO);return;
    }
    self.index=index;showSelection(zoom,index,NO);
}
@end
static void configureZoom(UIView *zoom) {
    SEL getDelegate=NSSelectorFromString(@"delegate"),setDelegate=NSSelectorFromString(@"setDelegate:");
    if(![zoom respondsToSelector:getDelegate]||![zoom respondsToSelector:setDelegate])return;
    id delegate=((id(*)(id,SEL))objc_msgSend)(zoom,getDelegate);
    RWNativeLensDelegate *proxy=objc_getAssociatedObject(zoom,&proxyKey);
    id controller=delegate==proxy?proxy.original:delegate;
    if(!controller)return;
    id graph=nil;
    @try {graph=[controller valueForKey:@"_currentGraphConfiguration"];}@catch(__unused NSException *e){return;}
    SEL position=NSSelectorFromString(@"devicePosition");
    if(![graph respondsToSelector:position])return;
    /* CameraUI uses 0=Back, 1=Front (not AVCaptureDevicePosition). */
    BOOL rear=((NSInteger(*)(id,SEL))objc_msgSend)(graph,position)==0;
    if(!rear) {
        if(delegate==proxy)((void(*)(id,SEL,id))objc_msgSend)(zoom,setDelegate,controller);
        proxy.rear=NO;return;
    }
    if(!proxy){proxy=[RWNativeLensDelegate new];proxy.index=NSNotFound;proxy.zoom=zoom;objc_setAssociatedObject(zoom,&proxyKey,proxy,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
    proxy.original=controller;proxy.rear=YES;
    if(delegate!=proxy)((void(*)(id,SEL,id))objc_msgSend)(zoom,setDelegate,proxy);
    NSUInteger index=selectedIndex();
    NSArray *factors=nil;
    @try{factors=[zoom valueForKey:@"_displayZoomFactors"];}@catch(__unused NSException *e){}
    if(proxy.index!=index||![factors isEqual:@[@0.6,@1.0]]){proxy.index=index;showSelection(zoom,index,NO);}
}
static void walk(UIView *view,Class cls) {
    if([view isKindOfClass:cls]){configureZoom(view);return;}
    for(UIView *child in view.subviews)walk(child,cls);
}
__attribute__((constructor))static void start(void) {
    @autoreleasepool {
        if(![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.camera"])return;
        dispatch_async(dispatch_get_main_queue(),^{
            Class cls=NSClassFromString(@"CAMZoomControl");
            Method method=class_getInstanceMethod(cls,selectionSelector());
            if(!method||strcmp(method_getTypeEncoding(method),"v40@0:8@16Q24B32B36"))return;
            NSTimer *timer=[NSTimer timerWithTimeInterval:.5 repeats:YES block:^(__unused NSTimer *t){
                if(UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)return;
                for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)
                    if([scene isKindOfClass:UIWindowScene.class]&&scene.activationState==UISceneActivationStateForegroundActive)
                        for(UIWindow *window in ((UIWindowScene*)scene).windows)walk(window,cls);
            }];
            [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
        });
    }
}
