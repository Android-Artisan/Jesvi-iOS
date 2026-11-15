/* Device test of the actual selector/delegate, with an isolated control file. */
#define RW_LENS_CONTROL_PATH @"/var/root/native-lens-test-zoom"
#include "camera_lens_switch.m"
#include <dlfcn.h>
#include <assert.h>
#include <stdio.h>
@interface RWTestGraph:NSObject
@property(nonatomic) NSInteger devicePosition;
@end
@implementation RWTestGraph @end
@interface RWTestController:NSObject
@property(nonatomic,strong) RWTestGraph *graph;
@property(nonatomic) NSUInteger forwarded;
@end
@implementation RWTestController
- (id)_currentGraphConfiguration{return self.graph;}
- (void)zoomControl:(id)zoom didChangeSelectedButtonIndex:(NSUInteger)index displayZoomFactor:(double)factor {
    (void)zoom;(void)index;(void)factor;self.forwarded++;
}
@end
int main(void){@autoreleasepool{
    assert(dlopen("/System/Library/PrivateFrameworks/CameraUI.framework/CameraUI",RTLD_NOW));
    UIView *zoom=[[NSClassFromString(@"CAMZoomControl") alloc]initWithFrame:CGRectMake(0,0,375,102)];
    ((void(*)(id,SEL,double,double,double,double,double))objc_msgSend)(zoom,NSSelectorFromString(@"configureForSingleCameraPlusToggleWithZoomFactor:minimumZoomFactor:displayMinimumZoomFactor:maximumZoomFactor:displayMaximumZoomFactor:"),1,1,1,5,5);
    RWTestController *original=[RWTestController new];original.graph=[RWTestGraph new];original.graph.devicePosition=0;
    SEL setter=NSSelectorFromString(@"setDelegate:"),getter=NSSelectorFromString(@"delegate");
    ((void(*)(id,SEL,id))objc_msgSend)(zoom,setter,original);
    [@"1\n" writeToFile:control atomically:YES encoding:NSUTF8StringEncoding error:nil];
    configureZoom(zoom);
    RWNativeLensDelegate *proxy=objc_getAssociatedObject(zoom,&proxyKey);assert(proxy&&proxy.rear&&proxy.original==original);
    assert(([[zoom valueForKey:@"_displayZoomFactors"] isEqual:@[@0.6,@1.0]]));
    [proxy zoomControl:zoom didChangeSelectedButtonIndex:0 displayZoomFactor:0.6];assert(selectedIndex()==0&&original.forwarded==0);
    [proxy zoomControl:zoom didChangeSelectedButtonIndex:1 displayZoomFactor:1];assert(selectedIndex()==1&&original.forwarded==0);
    [proxy zoomControl:zoom didChangeSelectedButtonIndex:2 displayZoomFactor:2];assert(original.forwarded==1);
    original.graph.devicePosition=1;configureZoom(zoom);assert(!proxy.rear);
    assert(((id(*)(id,SEL))objc_msgSend)(zoom,getter)==original);
    original.graph.devicePosition=0;configureZoom(zoom);assert(proxy.rear);
    assert(((id(*)(id,SEL))objc_msgSend)(zoom,getter)==proxy);
    configureZoom(zoom);assert(proxy.original==original);
    [[NSFileManager defaultManager]removeItemAtPath:control error:nil];
    puts("PASS: actual native selector, rear lens writes, callback forwarding, front restoration, rear re-entry");
}}
