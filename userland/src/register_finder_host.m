/* Register the local Files host as a containerized user app, so Finder's
 * user-app sharing query includes it. Keep its existing identifier and data.
 * Copy the immutable system bundle once; never replace user documents. */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <sys/stat.h>
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
@interface MCMContainer : NSObject
+ (id)containerWithIdentifier:(NSString *)identifier createIfNecessary:(BOOL)create
    existed:(BOOL *)existed error:(NSError **)error;
- (NSURL *)url;
@end
@interface LSApplicationWorkspace (Record)
- (BOOL)registerApplicationDictionary:(NSDictionary *)record;
@end
extern int SecStaticCodeCreateWithPathAndAttributes(CFURLRef, uint32_t, CFDictionaryRef, void **);
extern int SecCodeCopySigningInformation(void *, uint32_t, CFDictionaryRef *);

static NSString *mcm_path(NSString *cls, NSString *identifier) {
    NSError *error = nil; BOOL existed = NO;
    return [[[NSClassFromString(cls) containerWithIdentifier:identifier createIfNecessary:YES
        existed:&existed error:&error] url] path];
}

static NSDictionary *entitlements(NSString *executable) {
    void *code = NULL; CFDictionaryRef info = NULL; NSDictionary *found = nil;
    if (SecStaticCodeCreateWithPathAndAttributes((__bridge CFURLRef)[NSURL fileURLWithPath:executable], 0, NULL, &code) == 0 && code) {
        if (SecCodeCopySigningInformation(code, 1u << 2, &info) == 0 && info) {
            found = [(__bridge NSDictionary *)info objectForKey:@"entitlements-dict"];
            CFRelease(info);
        }
        CFRelease(code);
    }
    return found ?: @{};
}

/* The record of a bundle for LaunchServices: an app (owner nil) or one of its extensions. */
static NSDictionary *record(NSString *path, NSString *owner) {
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
    NSString *identifier = info[@"CFBundleIdentifier"], *executable = info[@"CFBundleExecutable"];
    if (!identifier || !executable) return nil;
    NSDictionary *rights = entitlements([path stringByAppendingPathComponent:executable]);
    NSMutableDictionary *r = [@{
        @"ApplicationType": owner ? @"PluginKitPlugin" : @"User",
        @"CFBundleIdentifier": identifier, @"CodeInfoIdentifier": identifier, @"CompatibilityState": @0,
        @"Path": path, @"Entitlements": rights, @"IsDeletable": @NO,
        @"SignerOrganization": @"Apple Inc.", @"SignatureVersion": @132352,
        @"SignerIdentity": @"Apple iPhone OS Application Signing", @"IsAdHocSigned": @YES,
        @"LSInstallType": @1, @"HasMIDBasedSINF": @0, @"MissingSINF": @0, @"FamilyID": @0,
        @"IsOnDemandInstallCapable": @0 } mutableCopy];
    NSString *home = mcm_path(owner ? @"MCMPluginKitPluginDataContainer" : @"MCMAppDataContainer", identifier);
    if (home) {
        r[@"Container"] = home; r[@"IsContainerized"] = @YES;
        r[@"EnvironmentVariables"] = @{ @"CFFIXED_USER_HOME": home, @"HOME": home,
            @"TMPDIR": [home stringByAppendingPathComponent:@"tmp"] };
    }
    NSMutableDictionary *groups = [NSMutableDictionary dictionary];
    id names = rights[@"com.apple.security.application-groups"];
    if ([names isKindOfClass:NSArray.class]) for (NSString *name in names) {
        NSString *shared = mcm_path(@"MCMSharedDataContainer", name);
        if (shared) groups[name] = shared;
    }
    r[@"GroupContainers"] = groups;
    /* (SpringBoard takes the tags from the record, not from Info.plist: without them the hidden
     * host gets an icon on the home screen) */
    if (info[@"SBAppTags"]) r[@"SBAppTags"] = info[@"SBAppTags"];
    if (owner) { r[@"PluginOwnerBundleID"] = owner; return r; }
    NSMutableDictionary *plugins = [NSMutableDictionary dictionary];
    NSString *dir = [path stringByAppendingPathComponent:@"PlugIns"];
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil] ?: @[]) {
        if (![name.pathExtension isEqual:@"appex"]) continue;
        NSDictionary *plugin = record([dir stringByAppendingPathComponent:name], identifier);
        if (plugin) plugins[plugin[@"CFBundleIdentifier"]] = plugin;
    }
    r[@"_LSBundlePlugins"] = plugins;
    return r;
}

/* Finder's file sharing shows one folder: the app's Documents. Make that folder the phone's own
 * storage, "On My iPhone" (the local provider's File Provider Storage), so a Mac sees Downloads and
 * the rest and not an empty folder of this hidden app. afcd follows the link (checked 2026-11-13).
 * What was in a real Documents folder is moved into the storage first; nothing is deleted. */
static void share_storage(NSString *home) {
    NSString *group = mcm_path(@"MCMSharedDataContainer", @"group.com.apple.FileProvider.LocalStorage");
    if (!home || !group) return;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *storage = [group stringByAppendingPathComponent:@"File Provider Storage"];
    NSString *documents = [home stringByAppendingPathComponent:@"Documents"];
    if (![fm fileExistsAtPath:storage]) {       /* (the provider makes it at its first start) */
        if (mkdir(storage.fileSystemRepresentation, 0755)) return;
        chown(storage.fileSystemRepresentation, 501, 501);
    }
    NSDictionary *is = [fm attributesOfItemAtPath:documents error:nil];
    if ([is.fileType isEqual:NSFileTypeSymbolicLink]) {
        if ([[fm destinationOfSymbolicLinkAtPath:documents error:nil] isEqual:storage]) return;
        unlink(documents.fileSystemRepresentation);
    } else if (is) {
        for (NSString *name in [fm contentsOfDirectoryAtPath:documents error:nil] ?: @[])
            [fm moveItemAtPath:[documents stringByAppendingPathComponent:name]
                        toPath:[storage stringByAppendingPathComponent:name] error:nil];
        if (rmdir(documents.fileSystemRepresentation)) return;      /* a name was taken: left as it is */
    }
    if (symlink(storage.fileSystemRepresentation, documents.fileSystemRepresentation) == 0)
        lchown(documents.fileSystemRepresentation, 501, 501);
    NSLog(@"Jesvi Files: Finder shares %@", storage);
}

/*
 * Native Files (A, 2026-11-20; the default image): Apple's local provider (On My iPhone) and the two document pickers
 * stay in their frameworks, as on an iPhone, with no host app and so no "Jesvi Files" icon. LaunchServices usually
 * finds them itself; any it does not know are registered here, and the provider's app-group and data containers are
 * made (a fresh data volume has no installer pass that would make them). An earlier install's host app is unregistered.
 */
static int native_files(Class workspace) {
    NSArray *paths = @[
        @"/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex",
        @"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/SaveToFiles.appex",
        @"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/com.apple.DocumentManager.Service.appex" ];
    Class plugins = NSClassFromString(@"LSPlugInKitProxy"), apps = NSClassFromString(@"LSApplicationProxy");
    id ws = [workspace defaultWorkspace];
    id host = [apps performSelector:@selector(applicationProxyForIdentifier:) withObject:@"org.hakios.LocalFilesHost"];
    NSURL *hostURL = [host respondsToSelector:@selector(bundleURL)] ? [host performSelector:@selector(bundleURL)] : nil;
    if (hostURL && [ws respondsToSelector:@selector(unregisterApplication:)]) {
        BOOL gone = ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, @selector(unregisterApplication:), hostURL);
        NSLog(@"Files: the old host app (Jesvi Files) unregistered: %d", gone);
    }
    NSString *group = mcm_path(@"MCMSharedDataContainer", @"group.com.apple.FileProvider.LocalStorage");
    mcm_path(@"MCMPluginKitPluginDataContainer", @"com.apple.FileProvider.LocalStorage");
    if (group) {
        NSString *storage = [group stringByAppendingPathComponent:@"File Provider Storage"];
        if (mkdir(storage.fileSystemRepresentation, 0755) == 0) chown(storage.fileSystemRepresentation, 501, 501);
    }
    int missing = 0;
    for (NSString *path in paths) {
        NSString *pid = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"];
        if (!pid) { NSLog(@"Files: %@ is not in this image", path); missing++; continue; }
        if ([plugins performSelector:@selector(pluginKitProxyForIdentifier:) withObject:pid]) continue;
        BOOL ok = [ws respondsToSelector:@selector(registerPlugin:)] &&
            ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, @selector(registerPlugin:), [NSURL fileURLWithPath:path]);
        BOOL known = [plugins performSelector:@selector(pluginKitProxyForIdentifier:) withObject:pid] != nil;
        NSLog(@"Files: %@ registered: %d, known now: %d", pid, ok, known);
        if (!known) missing++;
    }
    NSLog(@"Files (native): %s", missing ? "some extensions missing" : "On My iPhone and the pickers ready");
    return missing ? 1 : 0;
}

int main(void) {
    alarm(30);
    @autoreleasepool {
        NSString *identifier = @"org.hakios.LocalFilesHost";
        if (access("/Applications/HakiosLocalFiles.app/Info.plist", F_OK) != 0 &&
            access("/var/mobile/Library/HakiosLocalFiles/LocalFilesHost.app/Info.plist", F_OK) != 0) {
            dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
            dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
            return native_files(NSClassFromString(@"LSApplicationWorkspace"));
        }
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
        /* From a clean LaunchServices database registerApplication: refuses an identifier it has never
         * seen (-9499 at _LSRegisterBundleNode), and the host with its three extensions stays unknown:
         * no pickers, no On My iPhone, no Finder sharing. Then give LaunchServices the whole record, as
         * installd would: the app, its data container and each extension with its own. (2026-11-13) */
        /* (A, 2026-11-20) on a clean data volume registerApplication: SUCCEEDED for the app in /Applications but left
         * its extensions out: LaunchServices knew no com.apple.FileProvider.LocalStorage, fileproviderd never started
         * it, Files had no On My iPhone. So check every extension and give the whole record when one is missing. */
        if (ok) {
            NSString *plugins = [destination stringByAppendingPathComponent:@"PlugIns"];
            for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:plugins error:nil] ?: @[]) {
                NSString *pid = [NSDictionary dictionaryWithContentsOfFile:[[plugins stringByAppendingPathComponent:name]
                    stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"];
                Class proxy = NSClassFromString(@"LSPlugInKitProxy");
                if (pid && proxy && ![proxy performSelector:@selector(pluginKitProxyForIdentifier:) withObject:pid]) {
                    NSLog(@"Jesvi Files: extension %@ not registered: the whole record", pid);
                    ok = NO;
                    break;
                }
            }
        }
        if (!ok) {
            NSDictionary *whole = record(destination, nil);
            ok = whole && [[workspace defaultWorkspace] registerApplicationDictionary:whole];
            NSLog(@"Jesvi Files record: data %@, %lu extensions", whole[@"Container"],
                (unsigned long)[whole[@"_LSBundlePlugins"] count]);
        }
        NSLog(@"Jesvi Files user registration: %@", ok ? @"complete" : @"retry needed");
        if (ok) share_storage(mcm_path(@"MCMAppDataContainer", identifier));
        return ok ? 0 : 1;
    }
}
