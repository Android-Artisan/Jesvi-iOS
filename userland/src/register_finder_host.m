/* Register the local Files host as a containerized user app, so Finder's
 * user-app sharing query includes it. Keep its existing identifier and data.
 * Copy the immutable system bundle once; never replace user documents. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <unistd.h>
@interface MCMAppContainer : NSObject
+ (id)containerWithIdentifier:(NSString *)identifier createIfNecessary:(BOOL)create
                     existed:(BOOL *)existed error:(NSError **)error;
- (NSURL *)url;
@end
@interface LSApplicationWorkspace : NSObject
+ (id)defaultWorkspace;
- (BOOL)registerApplication:(NSURL *)url;
@end
int main(void) {
    alarm(30);
    @autoreleasepool {
        NSString *identifier = @"org.hakios.LocalFilesHost";
        dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
        dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
        Class containers = NSClassFromString(@"MCMAppContainer");
        Class workspace = NSClassFromString(@"LSApplicationWorkspace");
        if (!containers || !workspace) return 1;
        NSError *error = nil; BOOL existed = NO;
        id container = [containers containerWithIdentifier:identifier createIfNecessary:YES existed:&existed error:&error];
        NSURL *url = [container url];
        if (!url) { NSLog(@"Finder host container unavailable: %@", error); return 1; }
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *destination = [url.path stringByAppendingPathComponent:@"LocalFilesHost.app"];
        if (![fm fileExistsAtPath:destination]) {
            NSString *source = nil;
            for (NSString *candidate in @[@"/Applications/HakiosLocalFiles.app",
                 @"/var/mobile/Library/HakiosLocalFiles/LocalFilesHost.app"]) {
                NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[candidate stringByAppendingPathComponent:@"Info.plist"]];
                if ([info[@"CFBundleIdentifier"] isEqual:identifier]) { source = candidate; break; }
            }
            NSString *staging = [url.path stringByAppendingPathComponent:
                [@".jesvi-host-" stringByAppendingString:NSUUID.UUID.UUIDString]];
            BOOL copied = source && [fm copyItemAtPath:source toPath:staging error:&error];
            if (!copied || ![fm moveItemAtPath:staging toPath:destination error:&error]) {
                if (copied) [fm removeItemAtPath:staging error:nil];
                NSLog(@"Finder host copy unavailable: %@", error); return 1;
            }
        }
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[destination stringByAppendingPathComponent:@"Info.plist"]];
        if (![info[@"CFBundleIdentifier"] isEqual:identifier] || ![info[@"UIFileSharingEnabled"] boolValue]) return 1;
        BOOL ok = [[workspace defaultWorkspace] registerApplication:[NSURL fileURLWithPath:destination]];
        NSLog(@"Jesvi Files user registration: %@", ok ? @"complete" : @"retry needed");
        return ok ? 0 : 1;
    }
}
