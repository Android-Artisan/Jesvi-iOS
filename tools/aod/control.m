/* Root command arms one short appearance test. Does not lock, wake or reboot. */
#import <Foundation/Foundation.h>
#include <notify.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "preview_token.h"
int main(int argc,char **argv){@autoreleasepool{
    if(getuid()!=0||argc!=2||(strcmp(argv[1],"begin")&&strcmp(argv[1],"end"))){fprintf(stderr,"usage (root): aod-preview begin|end\n");return 1;}
    umask(0077);
    if(mkdir(RW_AOD_DIRECTORY,0755)&&errno!=EEXIST)return 1;
    struct stat st;if(lstat(RW_AOD_DIRECTORY,&st)||!S_ISDIR(st.st_mode)||st.st_uid||(st.st_mode&0022))return 1;
    if(chmod(RW_AOD_DIRECTORY,0755))return 1;
    int parent=open(RW_AOD_DIRECTORY,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK);if(parent<0)return 1;
    if(!strcmp(argv[1],"end")){unlinkat(parent,"arm",0);close(parent);return notify_post(RW_AOD_END)==NOTIFY_STATUS_OK?0:1;}
    int fd=openat(parent,"arm",O_WRONLY|O_CREAT|O_NOFOLLOW|O_NONBLOCK,0644);close(parent);if(fd<0)return 1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid||st.st_nlink!=1||fchmod(fd,0644)||ftruncate(fd,0)){close(fd);return 1;}
    struct rw_aod_token token={.magic=RW_AOD_MAGIC,.expires=NSProcessInfo.processInfo.systemUptime+30};
    do {arc4random_buf(&token.nonce,sizeof(token.nonce));}while(!token.nonce);
    BOOL ok=write(fd,&token,sizeof(token))==sizeof(token)&&!fsync(fd);close(fd);
    if(!ok)return 1;
    if(notify_post(RW_AOD_BEGIN)!=NOTIFY_STATUS_OK)return 1;
    puts("Requested one 12-second native appearance preview; this does not confirm it rendered. Touch or 'end' restores normal appearance.");
}return 0;}
