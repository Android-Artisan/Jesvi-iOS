/* iOS 16's home wallpaper is a snapshot in PBUIPosterHomeViewController,
 * not the legacy PBUIWallpaperView. Attach UIKit motion effects only to that
 * snapshot's image view. Icons, lock-screen content, and other windows stay
 * under their native controllers. All calls below run on the main queue. */
#ifndef REDWOOD_WALLPAPER_MOTION_H
#define REDWOOD_WALLPAPER_MOTION_H
#include <math.h>

struct rw_wall_affine { double a,b,c,d,tx,ty; };
struct rw_wall_rect { double x,y,w,h; };
struct rw_wall_point { double x,y; };
#define WM_SEND(type,obj,name) ((type(*)(id,SEL))objc_msgSend)((id)(obj),sel_registerName(name))
#define WM_SEND1(type,obj,name,t,arg) ((type(*)(id,SEL,t))objc_msgSend)((id)(obj),sel_registerName(name),arg)

/* Shared keys also prevent duplicates if this code is loaded as a tweak and
 * later included in the built-in SpringBoard bridge. */
static const void *wallpaper_effect_key(void) { return (const void*)sel_registerName("redwoodWallpaperMotionEffect"); }
static const void *wallpaper_transform_key(void) { return (const void*)sel_registerName("redwoodWallpaperMotionOriginalTransform"); }
static const void *wallpaper_timer_key(void) { return (const void*)sel_registerName("redwoodWallpaperMotionTimerStarted"); }

static bool
wallpaper_kind(id object,const char *name)
{
    Class cls=objc_getClass(name);
    return object && cls && WM_SEND1(BOOL,object,"isKindOfClass:",Class,cls);
}

static id
wallpaper_image_in(id view,unsigned depth)
{
    if(!view || depth>5 || WM_SEND(BOOL,view,"isHidden")) return nil;
    if(wallpaper_kind(view,"UIImageView")) return view;
    id children=WM_SEND(id,view,"subviews");
    for(unsigned long i=0;i<WM_SEND(unsigned long,children,"count");i++) {
        id image=wallpaper_image_in(WM_SEND1(id,children,"objectAtIndex:",unsigned long,i),depth+1);
        if(image) return image;
    }
    return nil;
}

static id
wallpaper_home_image(id view,unsigned depth)
{
    if(!view || depth>18) return nil;
    if(wallpaper_kind(WM_SEND(id,view,"nextResponder"),"PBUIPosterHomeViewController")) {
        id children=WM_SEND(id,view,"subviews");
        for(unsigned long i=0;i<WM_SEND(unsigned long,children,"count");i++) {
            id child=WM_SEND1(id,children,"objectAtIndex:",unsigned long,i);
            if(wallpaper_kind(child,"PBUISnapshotReplicaView")) {
                id image=wallpaper_image_in(child,0);
                if(image) return image;
            }
        }
        return nil;
    }
    id children=WM_SEND(id,view,"subviews");
    for(unsigned long i=0;i<WM_SEND(unsigned long,children,"count");i++) {
        id image=wallpaper_home_image(WM_SEND1(id,children,"objectAtIndex:",unsigned long,i),depth+1);
        if(image) return image;
    }
    return nil;
}

static id
wallpaper_current_image(void)
{
    id app=WM_SEND(id,objc_getClass("UIApplication"),"sharedApplication");
    id windows=WM_SEND(id,app,"windows");
    for(unsigned long i=0;i<WM_SEND(unsigned long,windows,"count");i++) {
        id window=WM_SEND1(id,windows,"objectAtIndex:",unsigned long,i);
        if(!wallpaper_kind(window,"_SBWallpaperSecureWindow")) continue;
        id controller=WM_SEND(id,window,"rootViewController");
        id image=wallpaper_home_image(WM_SEND(id,controller,"view"),0);
        if(image) return image;
    }
    return nil;
}

static bool
wallpaper_motion_enabled(void)
{
    bool (*reduce_motion)(void)=dlsym(RTLD_DEFAULT,"UIAccessibilityIsReduceMotionEnabled");
    if(!reduce_motion || reduce_motion()) return false;
    id process=WM_SEND(id,objc_getClass("NSProcessInfo"),"processInfo");
    if(WM_SEND(BOOL,process,"isLowPowerModeEnabled")) return false;
    CFTypeRef value=CFPreferencesCopyAppValue(CFSTR("WallpaperMotionEnabled"),CFSTR("com.redwood.motion"));
    bool enabled=true;
    if(value) {
        if(CFGetTypeID(value)==CFBooleanGetTypeID()) enabled=CFBooleanGetValue(value);
        else if(CFGetTypeID(value)==CFNumberGetTypeID()) {
            int n=0; enabled=CFNumberGetValue(value,kCFNumberIntType,&n) && n!=0;
        } else enabled=false;
        CFRelease(value);
    }
    return enabled;
}

static void
wallpaper_motion_apply(id image,bool enabled)
{
    if(!image) return;
    id group=objc_getAssociatedObject(image,wallpaper_effect_key());
    if(!enabled) {
        if(!group) return;
        WM_SEND1(void,image,"removeMotionEffect:",id,group);
        CFDataRef saved=(CFDataRef)objc_getAssociatedObject(image,wallpaper_transform_key());
        if(saved && CFDataGetLength(saved)==sizeof(struct rw_wall_affine)) {
            struct rw_wall_affine original;
            CFDataGetBytes(saved,CFRangeMake(0,sizeof(original)),(UInt8*)&original);
            WM_SEND1(void,image,"setTransform:",struct rw_wall_affine,original);
        }
        objc_setAssociatedObject(image,wallpaper_effect_key(),nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(image,wallpaper_transform_key(),nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        fprintf(stderr,"redwood: wallpaper motion: suspended; original image transform restored\n");
        return;
    }
    if(group) return;
    Class effect_class=objc_getClass("UIInterpolatingMotionEffect"),group_class=objc_getClass("UIMotionEffectGroup");
    if(!effect_class || !group_class) return;
    struct rw_wall_rect bounds=WM_SEND(struct rw_wall_rect,image,"bounds");
    if(bounds.w<40 || bounds.h<40) return;
    double amplitude=fmin(9,.02*fmin(bounds.w,bounds.h));
    CFNumberRef low=CFNumberCreate(NULL,kCFNumberDoubleType,&(double){-amplitude});
    CFNumberRef high=CFNumberCreate(NULL,kCFNumberDoubleType,&amplitude);
    id effects[2]={nil,nil};
    for(long axis=0;axis<2;axis++) {
        id allocated=WM_SEND(id,effect_class,"alloc");
        effects[axis]=((id(*)(id,SEL,id,long))objc_msgSend)(allocated,
            sel_registerName("initWithKeyPath:type:"),(id)(axis?CFSTR("center.y"):CFSTR("center.x")),axis);
        WM_SEND1(void,effects[axis],"setMinimumRelativeValue:",id,(id)low);
        WM_SEND1(void,effects[axis],"setMaximumRelativeValue:",id,(id)high);
    }
    group=WM_SEND(id,WM_SEND(id,group_class,"alloc"),"init");
    if(group && effects[0] && effects[1]) {
        CFArrayRef array=CFArrayCreate(NULL,(const void**)effects,2,&kCFTypeArrayCallBacks);
        WM_SEND1(void,group,"setMotionEffects:",id,(id)array);
        CFRelease(array);
        struct rw_wall_affine original=WM_SEND(struct rw_wall_affine,image,"transform"),overscan=original;
        CFDataRef saved=CFDataCreate(NULL,(const UInt8*)&original,sizeof(original));
        objc_setAssociatedObject(image,wallpaper_transform_key(),(id)saved,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        CFRelease(saved);
        /* 3% margin on each edge exceeds the <=2% translation range. */
        overscan.a*=1.06;overscan.b*=1.06;overscan.c*=1.06;overscan.d*=1.06;
        WM_SEND1(void,image,"setTransform:",struct rw_wall_affine,overscan);
        WM_SEND1(void,image,"addMotionEffect:",id,group);
        objc_setAssociatedObject(image,wallpaper_effect_key(),group,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        fprintf(stderr,"redwood: wallpaper motion: attached to home %s, +/-%.2f pt\n",object_getClassName(image),amplitude);
    }
    for(unsigned i=0;i<2;i++) WM_SEND(void,effects[i],"release");
    WM_SEND(void,group,"release");
    CFRelease(low);CFRelease(high);
}

static void
wallpaper_motion_start(void)
{
    static dispatch_source_t timer;
    static int token;
    Class app_class=objc_getClass("UIApplication");
    if(timer || !app_class || objc_getAssociatedObject((id)app_class,wallpaper_timer_key()) ||
        access("/dev/redwood-sensors",F_OK)!=0) return;
    char args[1024]={0};size_t n=sizeof(args)-1;
    if(!sysctlbyname("kern.bootargs",args,&n,NULL,0) && strstr(args,"sens_wallpaper_motion=0")) return;
    timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),2*NSEC_PER_SEC,250*NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer,^{
        wallpaper_motion_apply(wallpaper_current_image(),wallpaper_motion_enabled());
    });
    dispatch_resume(timer);
    objc_setAssociatedObject((id)app_class,wallpaper_timer_key(),(id)kCFBooleanTrue,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    notify_register_dispatch("com.redwood.wallpaper.motion.status",&token,dispatch_get_main_queue(),^(__unused int t){
        id image=wallpaper_current_image();
        id layer=image?WM_SEND(id,image,"layer"):nil;
        id presentation=layer?WM_SEND(id,layer,"presentationLayer"):nil;
        struct rw_wall_point point=presentation?WM_SEND(struct rw_wall_point,presentation,"position"):(struct rw_wall_point){0};
        fprintf(stderr,"redwood: wallpaper motion: image=%p effect=%p enabled=%d presentation=(%.2f,%.2f)\n",
            image,image?objc_getAssociatedObject(image,wallpaper_effect_key()):nil,wallpaper_motion_enabled(),point.x,point.y);
    });
}
#undef WM_SEND
#undef WM_SEND1
#endif
