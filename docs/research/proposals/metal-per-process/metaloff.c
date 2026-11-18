/* libredwood-metaloff: the reverse of metalon -- in the processes named in kern.exec_inject, Metal's
 * two discovery properties read as absent, so Metal stays off there while it is on system-wide
 * (metal_props on). mediaserverd aborted with a Metal device ("may not be called outside of
 * mediaserverd", 2026-11-12). */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static CFTypeRef (*o_create)(io_registry_entry_t,CFStringRef,CFAllocatorRef,IOOptionBits);
static CFTypeRef (*o_search)(io_registry_entry_t,const io_name_t,CFStringRef,CFAllocatorRef,IOOptionBits);
static kern_return_t (*o_props)(io_registry_entry_t,CFMutableDictionaryRef*,CFAllocatorRef,IOOptionBits);
static bool hidden(CFStringRef key)
{
    return key && CFGetTypeID(key)==CFStringGetTypeID() &&
           (CFEqual(key,CFSTR("MetalPluginName")) || CFEqual(key,CFSTR("MetalPluginClassName")));
}
static CFTypeRef my_create(io_registry_entry_t e,CFStringRef key,CFAllocatorRef a,IOOptionBits o)
{ return hidden(key)?NULL:o_create(e,key,a,o); }
static CFTypeRef my_search(io_registry_entry_t e,const io_name_t plane,CFStringRef key,CFAllocatorRef a,IOOptionBits o)
{ return hidden(key)?NULL:o_search(e,plane,key,a,o); }
static kern_return_t my_props(io_registry_entry_t e,CFMutableDictionaryRef *p,CFAllocatorRef a,IOOptionBits o)
{
    kern_return_t k=o_props(e,p,a,o);
    if(k==KERN_SUCCESS && p && *p) {
        CFDictionaryRemoveValue(*p,CFSTR("MetalPluginName"));
        CFDictionaryRemoveValue(*p,CFSTR("MetalPluginClassName"));
    }
    return k;
}
__attribute__((constructor)) static void start(void)
{
    void *h=dlopen("/var/jb/usr/lib/libsubstrate.dylib",RTLD_NOW);
    void (*hook)(void*,void*,void**)=h?(void(*)(void*,void*,void**))dlsym(h,"MSHookFunction"):NULL;
    if(!hook) { fprintf(stderr,"redwood: metaloff: no MSHookFunction\n"); return; }
    hook((void*)IORegistryEntryCreateCFProperty,(void*)my_create,(void**)&o_create);
    hook((void*)IORegistryEntrySearchCFProperty,(void*)my_search,(void**)&o_search);
    hook((void*)IORegistryEntryCreateCFProperties,(void*)my_props,(void**)&o_props);
    fprintf(stderr,"redwood: metaloff: Metal hidden in %s\n",getprogname());
}
