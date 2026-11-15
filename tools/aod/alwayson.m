/* Persistent iOS AOD policy on the S20. Native BacklightServices owns the
 * transition, clock, wake, focus and suppression logic. Panel ALPM is separate. */
#import <UIKit/UIKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include "config.h"
void rw_alwayson_settings_start(void);
static BOOL enabled,ready,configured,osSupports,osNativeSupported,osObserved;
static long long lastState;
static int statusToken=-1;
static __weak id platform;
static BOOL (*osSupportOriginal)(id,SEL);
static id (*engineInitOriginal)(id,SEL,id,id,id,id);
static void (*pseudoOriginal)(id,SEL,BOOL);
static void (*bindOriginal)(id,SEL);
static void (*updateOriginal)(id,SEL);
static void (*transitionOriginal)(id,SEL,id,long long,id);
static id object(id obj,const char *name){
    SEL sel=sel_registerName(name);NSMethodSignature *sig=[obj methodSignatureForSelector:sel];
    return sig&&sig.numberOfArguments==2&&sig.methodReturnType[0]=='@'?((id(*)(id,SEL))objc_msgSend)(obj,sel):nil;
}
static BOOL boolean(id obj,const char *name){
    SEL sel=sel_registerName(name);NSMethodSignature *sig=[obj methodSignatureForSelector:sel];
    return sig&&sig.numberOfArguments==2&&(sig.methodReturnType[0]=='B'||sig.methodReturnType[0]=='c')?
        ((BOOL(*)(id,SEL))objc_msgSend)(obj,sel):NO;
}
static BOOL setBool(id obj,const char *name,BOOL value){
    SEL sel=sel_registerName(name);NSMethodSignature *sig=[obj methodSignatureForSelector:sel];
    if(!sig||sig.numberOfArguments!=3||sig.methodReturnType[0]!='v')return NO;
    const char *type=[sig getArgumentTypeAtIndex:2];if(type[0]!='B'&&type[0]!='c')return NO;
    ((void(*)(id,SEL,BOOL))objc_msgSend)(obj,sel,value);return YES;
}
static void publish(void){
    if(statusToken<0)return;
    uint64_t flags=(ready?1:0)|(enabled?2:0)|(boolean(platform,"isAlwaysOnEnabled")?4:0);
    flags|=osSupports?16:0;flags|=osNativeSupported?32:0;flags|=osObserved?64:0;
    flags|=((uint64_t)lastState&15)<<8;notify_set_state(statusToken,flags);
}
static void applyDomain(id domain){
    NSDictionary *config=rw_aod_config();enabled=rw_aod_value(config,@"Enabled");
    BOOL ok=setBool(domain,"setEnableAlwaysOn:",enabled);
    /* Native development support flag; not a global MobileGestalt override. */
    ok=setBool(domain,"setForceAndromedaSupport:",YES)&&ok;
    ok=setBool(domain,"setShowWallpaperInAlwaysOn:",rw_aod_value(config,@"ShowWallpaper"))&&ok;
    ok=setBool(domain,"setShowNotificationsInAlwaysOn:",rw_aod_value(config,@"ShowNotifications"))&&ok;
    configured=ok;
    if(!ok){enabled=NO;setBool(domain,"setEnableAlwaysOn:",NO);}
    publish();
}
static void defaultsBound(id self,SEL cmd){
    bindOriginal(self,cmd);
    @try {applyDomain(self);}@catch(NSException *exception){(void)exception;enabled=NO;}
}
static BOOL observedOSSupport(id self,SEL cmd){
    osNativeSupported=osSupportOriginal(self,cmd);osObserved=YES;
    /* S20 has an AOD-capable panel. Adapt the iPhone-only capability test;
     * this does not claim the panel driver has entered ALPM. */
    osSupports=YES;publish();return YES;
}
/* The port has no CAFlipBook capture backend. Apple's own pseudo-flipbook
 * keeps the native presentation engine on live composition instead. Never
 * suppress the renderer's fatal checks or pretend a missing frame rendered. */
static void useLiveComposition(id self,SEL cmd,BOOL requested){
    (void)requested;pseudoOriginal(self,cmd,YES);
}
static id engineInitialized(id self,SEL cmd,id delegate,id provider,id os,id budget){
    id result=engineInitOriginal(self,cmd,delegate,provider,os,budget);
    if(result)setBool(result,"setUsePseudoFlipbook:",YES);
    return result;
}
static void restoreUnusedOSOverride(void){
    NSUserDefaults *metadata=[[NSUserDefaults alloc]initWithSuiteName:@"com.redwood.alwayson-compatibility"];
    if(![metadata boolForKey:@"SavedOriginalOverride"])return;
    NSUserDefaults *native=[[NSUserDefaults alloc]initWithSuiteName:@"com.apple.BacklightServices"];
    id old=[metadata objectForKey:@"OriginalOverride"];
    if([metadata boolForKey:@"OriginalOverridePresent"]&&[old isKindOfClass:NSNumber.class])
        [native setObject:old forKey:@"deviceSupportsAlwaysOnOverride"];
    else [native removeObjectForKey:@"deviceSupportsAlwaysOnOverride"];
    [native synchronize];
    [metadata removeObjectForKey:@"SavedOriginalOverride"];[metadata synchronize];
}
static BOOL s20Support(id self,SEL cmd){(void)self;(void)cmd;return YES;}
static void policyUpdated(id self,SEL cmd){
    platform=self;updateOriginal(self,cmd);publish();
}
static void revokeForInactive(void){
    id app=UIApplication.sharedApplication;id auth=object(app,"authenticationController");
    SEL revoke=sel_registerName("revokeAuthenticationImmediatelyForPublicReason:");
    NSMethodSignature *sig=[auth methodSignatureForSelector:revoke];
    if(sig&&sig.numberOfArguments==3&&sig.methodReturnType[0]=='v')
        ((void(*)(id,SEL,id))objc_msgSend)(auth,revoke,@"Redwood Always-On: locked display");
}
static void backlightWillChange(id self,SEL cmd,id host,long long state,id event){
    lastState=state;transitionOriginal(self,cmd,host,state,event);publish();
    /* InactiveOn is AOD, not an authenticated screen. Preserve the software
     * passcode policy even if displayStatus stays on in this native mode. */
    if(enabled&&(state==0||state==1))dispatch_async(dispatch_get_main_queue(),^{
        @try {revokeForInactive();}@catch(NSException *exception){(void)exception;}
    });
}
static BOOL pinned(Method method,const char *image,uintptr_t address,unsigned arguments,char result){
    if(!method||method_getNumberOfArguments(method)!=arguments)return NO;
    char type[16]={0};method_getReturnType(method,type,sizeof(type));
    if(type[0]!=result)return NO;
    for(uint32_t i=0;i<_dyld_image_count();i++)if(strstr(_dyld_get_image_name(i),image))
        return (uintptr_t)method_getImplementation(method)==address+_dyld_get_image_vmaddr_slide(i);
    return NO;
}
static void applySettings(void){
    @try {id domain=object(object((id)objc_getClass("SBDefaults"),"localDefaults"),"alwaysOnDefaults");
        if(domain)applyDomain(domain);
    }@catch(NSException *exception){(void)exception;enabled=NO;}
    publish();
}
__attribute__((constructor))static void start(void){
    if(!rw_aod_target())return;
    if(!strcmp(getprogname(),"Preferences")){rw_alwayson_settings_start();return;}
    if(strcmp(getprogname(),"SpringBoard"))return;
    Class sharedGuard=objc_getClass("SBBacklightPlatformProvider");
    const void *guardKey=(const void*)sel_registerName("redwoodAlwaysOnServiceInstalled");
    if(sharedGuard&&objc_getAssociatedObject((id)sharedGuard,guardKey))return;
    enabled=rw_aod_value(rw_aod_config(),@"Enabled");
    notify_register_check(RW_AOD_SERVICE_STATUS,&statusToken);
    Class defaults=objc_getClass("SBAlwaysOnDefaults"),provider=objc_getClass("SBBacklightPlatformProvider"),controller=objc_getClass("SBBacklightController");
    Class engine=objc_getClass("BLSHAlwaysOnPresentationEngine");
    Method engineInit=class_getInstanceMethod(engine,sel_registerName("initWithDelegate:platformProvider:osInterfaceProvider:inactiveBudgetPolicy:"));
    Method pseudo=class_getInstanceMethod(engine,sel_registerName("setUsePseudoFlipbook:"));
    Method osSupport=class_getInstanceMethod(objc_getClass("BLSHBacklightOSInterfaceProvider"),sel_registerName("deviceSupportsAlwaysOn"));
    Method bind=class_getInstanceMethod(defaults,sel_registerName("_bindAndRegisterDefaults"));
    Method support=class_getClassMethod(provider,sel_registerName("deviceSupportsAlwaysOn"));
    Method update=class_getInstanceMethod(provider,sel_registerName("_updateAlwaysOnEnabled"));
    Method transition=class_getInstanceMethod(controller,sel_registerName("backlightHost:willTransitionToState:forEvent:"));
    if(!pinned(engineInit,"/BacklightServicesHost.framework/",0x1eebb29c4,6,'@')||
        !pinned(pseudo,"/BacklightServicesHost.framework/",0x1eebb8dd4,3,'v')||
        !pinned(osSupport,"/BacklightServicesHost.framework/",0x1eeba9734,2,'B')||
        !pinned(bind,"/SpringBoardFoundation.framework/",0x18e609e4c,2,'v')||
        !pinned(support,"/SpringBoard.framework/",0x1c4fb11f0,2,'B')||
        !pinned(update,"/SpringBoard.framework/",0x1c4fb15d0,2,'v')||
        !pinned(transition,"/SpringBoard.framework/",0x1c506650c,5,'v')){publish();return;}
    bindOriginal=(void*)method_setImplementation(bind,(IMP)defaultsBound);
    /* Enforce our default-off setting before making the native path available. */
    applySettings();
    if(!configured){method_setImplementation(bind,(IMP)bindOriginal);publish();return;}
    restoreUnusedOSOverride();
    pseudoOriginal=(void*)method_setImplementation(pseudo,(IMP)useLiveComposition);
    engineInitOriginal=(void*)method_setImplementation(engineInit,(IMP)engineInitialized);
    osSupportOriginal=(void*)method_setImplementation(osSupport,(IMP)observedOSSupport);
    method_setImplementation(support,(IMP)s20Support);
    updateOriginal=(void*)method_setImplementation(update,(IMP)policyUpdated);
    transitionOriginal=(void*)method_setImplementation(transition,(IMP)backlightWillChange);
    ready=YES;objc_setAssociatedObject((id)sharedGuard,guardKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    dispatch_async(dispatch_get_main_queue(),^{applySettings();});
    static int token;notify_register_dispatch(RW_AOD_CHANGED,&token,dispatch_get_main_queue(),^(int t){(void)t;applySettings();});
    publish();
}
