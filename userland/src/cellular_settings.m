/* MIT. S20 Cellular Settings UI, independent of the radio driver.
 * Loaded only in Preferences by the port's existing tweak loader. No fake
 * CommCenter replies, SIM state, registration, carrier name or signal bars.
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <notify.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Small declarations for the runtime API; no extracted Apple headers. */
@interface PSSpecifier : NSObject
+ (instancetype)preferenceSpecifierNamed:(NSString *)name target:(id)target
    set:(SEL)set get:(SEL)get detail:(Class)detail cell:(NSInteger)cell edit:(Class)edit;
- (id)propertyForKey:(NSString *)key;
- (void)setProperty:(id)value forKey:(NSString *)key;
@end

@interface PSListController : UIViewController
- (NSArray *)specifiers;
- (void)reloadSpecifiers;
@end

static NSString *const RWCellularID = @"MOBILE_DATA_SETTINGS_ID";
static CFStringRef const RWDomain = CFSTR("com.redwood.cellular");
static CFStringRef const RWDataKey = CFSTR("CellularDataEnabled");

static void diagnostic(const char *event)
{
    FILE *file = fopen("/var/mobile/Library/Preferences/com.redwood.cellular-ui.log", "a");
    if (file) { fprintf(file, "[%d] %s\n", getpid(), event); fclose(file); }
}

/* PSListController's table helpers consult its backing array as well as the
 * getter. Resolve the ivar by name, never by a private-layout offset. */
static void store_specifiers(id controller, NSArray *items)
{
    Ivar ivar = class_getInstanceVariable(PSListController.class, "_specifiers");
    if (ivar && ivar_getTypeEncoding(ivar)[0] == '@') {
        object_setIvarWithStrongDefault(controller, ivar, items);
    }
}

@interface RWCellularSettingsController : PSListController
@end

@implementation RWCellularSettingsController
- (NSArray *)specifiers
{
    static dispatch_once_t page_once;
    dispatch_once(&page_once, ^{ diagnostic("cellular-page-opened"); });
    PSSpecifier *group = [PSSpecifier preferenceSpecifierNamed:nil target:nil
        set:NULL get:NULL detail:Nil cell:0 edit:Nil];
    [group setProperty:@"Cellular service is not available yet. This setting will apply when it is ready."
        forKey:@"footerText"];
    PSSpecifier *data = [PSSpecifier preferenceSpecifierNamed:@"Cellular Data" target:self
        set:@selector(setCellularData:specifier:) get:@selector(cellularData:)
        detail:Nil cell:6 edit:Nil];
    [data setProperty:@"redwood.cellular.data" forKey:@"id"];
    PSSpecifier *network = [PSSpecifier preferenceSpecifierNamed:@"Network" target:self
        set:NULL get:@selector(network:) detail:Nil cell:4 edit:Nil];
    PSSpecifier *calls = [PSSpecifier preferenceSpecifierNamed:@"Calls" target:self
        set:NULL get:@selector(calls:) detail:Nil cell:4 edit:Nil];
    self.title = @"Cellular";
    NSArray *items = @[group, data, network, calls];
    store_specifiers(self, items);
    return items;
}

- (NSNumber *)cellularData:(PSSpecifier *)specifier
{
    (void)specifier;
    Boolean valid = false;
    Boolean enabled = CFPreferencesGetAppBooleanValue(RWDataKey, RWDomain, &valid);
    return @(valid && enabled);
}

- (void)setCellularData:(NSNumber *)value specifier:(PSSpecifier *)specifier
{
    (void)specifier;
    CFPreferencesSetAppValue(RWDataKey, value.boolValue ? kCFBooleanTrue : kCFBooleanFalse, RWDomain);
    if (!CFPreferencesAppSynchronize(RWDomain)) {
        NSLog(@"RedwoodCellular: could not save Cellular Data preference");
    }
    /* A future Samsung session service consumes this preference and event.
     * No modem command or route change is implied before that service exists. */
    notify_post("com.redwood.cellular.data-changed");
    diagnostic(value.boolValue ? "data-preference-on" : "data-preference-off");
}

- (NSString *)network:(PSSpecifier *)specifier
{
    (void)specifier;
    return @"No Service";
}

- (NSString *)calls:(PSSpecifier *)specifier
{
    (void)specifier;
    return @"Unavailable";
}
@end

static NSArray *(*root_specifiers)(id, SEL);
static Class hooked_class;
static BOOL hooked;
static atomic_bool install_queued;

static void refresh_root(UIViewController *controller, Class cls, unsigned depth)
{
    if (!controller || depth > 16) return;
    if ([controller isKindOfClass:cls] && [controller respondsToSelector:@selector(reloadSpecifiers)]) {
        [(PSListController *)controller reloadSpecifiers];
        return;
    }
    for (UIViewController *child in controller.childViewControllers) refresh_root(child, cls, depth + 1);
}

static UIImage *cellular_icon(void)
{
    static UIImage *icon;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc]
            initWithSize:CGSizeMake(29, 29)];
        icon = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
            (void)context;
            [UIColor.systemGreenColor setFill];
            [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, 29, 29) cornerRadius:6] fill];
            UIImageSymbolConfiguration *configuration = [UIImageSymbolConfiguration
                configurationWithPointSize:20 weight:UIImageSymbolWeightRegular];
            UIImage *symbol = [[UIImage systemImageNamed:@"antenna.radiowaves.left.and.right"
                withConfiguration:configuration] imageWithTintColor:UIColor.whiteColor
                renderingMode:UIImageRenderingModeAlwaysOriginal];
            [symbol drawInRect:CGRectMake((29 - symbol.size.width) / 2,
                (29 - symbol.size.height) / 2, symbol.size.width, symbol.size.height)];
        }];
    });
    return icon;
}

static NSArray *cellular_root_specifiers(id self, SEL command)
{
    static dispatch_once_t entered;
    dispatch_once(&entered, ^{ diagnostic("root-specifiers-entered"); });
    NSArray *original = root_specifiers(self, command);
    if (![original isKindOfClass:NSArray.class]) return original;
    NSMutableArray *items = [original mutableCopy];
    NSUInteger insert = NSNotFound;
    for (NSUInteger i = 0; i < items.count; i++) {
        id item = items[i];
        if (![item respondsToSelector:@selector(propertyForKey:)]) continue;
        NSString *identifier = [item propertyForKey:@"id"];
        if ([identifier isEqual:RWCellularID]) {
            insert = i;
            [items removeObjectAtIndex:i];
            break;
        }
        if ([identifier isEqual:@"Bluetooth"]) insert = i + 1;
    }
    if (insert == NSNotFound) {
        /* Avoid an unexpected layout when the root list isn't initialized yet. */
        return original;
    }
    PSSpecifier *cell = [PSSpecifier preferenceSpecifierNamed:@"Cellular" target:nil
        set:NULL get:NULL detail:RWCellularSettingsController.class cell:1 edit:Nil];
    [cell setProperty:RWCellularID forKey:@"id"];
    [cell setProperty:@YES forKey:@"enabled"];
    [cell setProperty:cellular_icon() forKey:@"iconImage"];
    [items insertObject:cell atIndex:MIN(insert, items.count)];
    store_specifiers(self, items);
    static dispatch_once_t row_once;
    dispatch_once(&row_once, ^{ diagnostic("cellular-row-ready"); });
    return items;
}

static void install_root_hook(void)
{
    if (hooked) return;
    /* iOS 16.5 uses PSUIPrefsListController for the visible iPhone root list.
     * PSUIPrefsRootController exists too, but isn't the displayed controller. */
    Class cls = objc_getClass("PSUIPrefsListController");
    SEL selector = @selector(specifiers);
    Method method = cls ? class_getInstanceMethod(cls, selector) : NULL;
    if (!method || method_getNumberOfArguments(method) != 2) return;
    char type[16] = {0};
    method_getReturnType(method, type, sizeof(type));
    if (type[0] != '@') return;
    IMP previous = method_getImplementation(method);
    /* A subclass may inherit this method. Add an override instead of replacing
     * PSListController's implementation for every Settings page. */
    root_specifiers = (void *)previous;
    if (!class_addMethod(cls, selector, (IMP)cellular_root_specifiers, method_getTypeEncoding(method))) {
        method_setImplementation(class_getInstanceMethod(cls, selector), (IMP)cellular_root_specifiers);
    }
    hooked = YES;
    hooked_class = cls;
    diagnostic("menu-hook-installed");
    NSLog(@"RedwoodCellular: Settings Cellular menu installed (radio unavailable)");
    /* The first root list may already exist by the time this main-queue task
     * runs. Refresh only Settings' own root, not every preferences controller. */
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            refresh_root(window.rootViewController, cls, 0);
        }
    }
}

static void image_added(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    if (atomic_exchange(&install_queued, true)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&install_queued, false);
        install_root_hook();
    });
}

__attribute__((constructor)) static void cellular_settings_init(void)
{
    if (strcmp(getprogname(), "Preferences")) return;
    /* PreferencesUI may load later. No UIKit/Objective-C work under dyld's lock. */
    _dyld_register_func_for_add_image(image_added);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (!hooked_class) return;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow *window in ((UIWindowScene *)scene).windows) {
                refresh_root(window.rootViewController, hooked_class, 0);
            }
        }
    });
}
