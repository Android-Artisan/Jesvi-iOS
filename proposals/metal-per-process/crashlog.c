// libredwood-crashlog: on a fatal signal or uncaught ObjC exception, write a backtrace to /var/tmp/crash.<proc>.txt
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <objc/message.h>
static char path[256];
static void dump(const char *why){
  int fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0644); if(fd<0) return;
  dprintf(fd,"=== %s (pid %d)\n",why,getpid());
  void *bt[64]; int n=backtrace(bt,64);
  for(int i=0;i<n;i++){ Dl_info d; if(dladdr(bt[i],&d)&&d.dli_fname){ const char*b=strrchr(d.dli_fname,'/'); dprintf(fd,"%2d %p %s+0x%lx %s\n",i,bt[i],b?b+1:d.dli_fname,(unsigned long)((char*)bt[i]-(char*)d.dli_fbase),d.dli_sname?d.dli_sname:""); } else dprintf(fd,"%2d %p\n",i,bt[i]); }
  close(fd);
}
static void onsig(int s){ char w[32]; snprintf(w,sizeof w,"signal %d",s); dump(w); signal(s,SIG_DFL); raise(s); }
static void onexc(id e){ id r=((id(*)(id,SEL))objc_msgSend)(e,sel_registerName("reason")); const char *c=r?((const char*(*)(id,SEL))objc_msgSend)(r,sel_registerName("UTF8String")):"?"; char w[600]; snprintf(w,sizeof w,"exception: %s",c); dump(w);
  id st=((id(*)(id,SEL))objc_msgSend)(e,sel_registerName("callStackSymbols")); id d=st?((id(*)(id,SEL))objc_msgSend)(st,sel_registerName("description")):0; int fd=open(path,O_WRONLY|O_APPEND); if(fd>=0&&d){dprintf(fd,"%s\n",((const char*(*)(id,SEL))objc_msgSend)(d,sel_registerName("UTF8String")));close(fd);} }
static void atex(void){ dump("exit()"); }
__attribute__((constructor)) static void start(void){
  snprintf(path,sizeof path,"/var/tmp/crash.%s.txt",getprogname());
  int sigs[]={SIGSEGV,SIGBUS,SIGILL,SIGABRT,SIGTRAP,SIGFPE,SIGSYS}; for(unsigned i=0;i<sizeof sigs/sizeof*sigs;i++) signal(sigs[i],onsig);
  void(*set)(void*)=dlsym(RTLD_DEFAULT,"NSSetUncaughtExceptionHandler"); if(!set){dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",RTLD_NOW); set=dlsym(RTLD_DEFAULT,"NSSetUncaughtExceptionHandler");} if(set) set((void*)onexc);
  atexit(atex);
}
