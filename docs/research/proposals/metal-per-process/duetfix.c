// libredwood-duet: duetexpertd traps in ATXEnableMobileAssetDataVault (a data vault this kernel cannot make);
// the folder is left an ordinary directory and the daemon goes on.
#include <dlfcn.h>
#include <stdio.h>
#include <sys/stat.h>
static long skip(void){ return 1; }
__attribute__((constructor)) static void start(void){
  void *h=dlopen("/var/jb/usr/lib/libsubstrate.dylib",RTLD_NOW);
  void (*hook)(void*,void*,void**)=h?dlsym(h,"MSHookFunction"):NULL;
  dlopen("/System/Library/PrivateFrameworks/AppPredictionInternal.framework/AppPredictionInternal",RTLD_NOW);
  void *f=dlsym(RTLD_DEFAULT,"ATXEnableMobileAssetDataVault");
  if(hook&&f){ void *o; hook(f,(void*)skip,&o); fprintf(stderr,"redwood: duet: data vault call skipped\n"); }
  else fprintf(stderr,"redwood: duet: hook not placed (%p %p)\n",(void*)hook,f);
}
