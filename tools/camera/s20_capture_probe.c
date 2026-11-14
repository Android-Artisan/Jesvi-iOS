/* Bounded S20 driver check. Explicit binned modes; no full rear or image retention. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "redwood_camera.h"
int main(int argc, char **argv) {
    unsigned sensor = argc == 2 ? (unsigned)atoi(argv[1]) : 1;
    if (sensor != 1 && sensor != 2) return 2;
    int fd = open(REDWOOD_CAMERA_DEVICE, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct redwood_camera_command c = {.command=REDWOOD_CAMERA_SENSOR,.a=sensor,.b=sensor==1?2:1};
    if (write(fd,&c,sizeof(c)) != sizeof(c)) { perror("sensor"); close(fd); return 1; }
    c=(struct redwood_camera_command){.command=REDWOOD_CAMERA_EXPOSURE,.a=1001,.b=32};
    int bad=0;
    for (int i=0;i<36;i++) {
        struct redwood_camera_frame f;
        ssize_t n;
        int retry=0;
        do { n=read(fd,&f,sizeof(f)); } while(n<0 && errno==EAGAIN && ++retry<4);
        if(n!=sizeof(f) || memcmp(f.magic,REDWOOD_CAMERA_MAGIC,8)) { perror("frame"); bad=1; break; }
        if (i==0 && write(fd,&c,sizeof(c)) != sizeof(c)) { perror("exposure"); bad=1; break; }
        printf("%d sensor=%u %ux%u seq=%llu exposure=%u gain=%u line_ns=%u frame=%u\n",i,f.sensor,f.width,f.height,(unsigned long long)f.sequence,f.exposure_lines,f.gain,f.line_ns,f.frame_lines);
        if(f.sensor!=sensor || (sensor==1 && (f.width!=2016 || f.height!=1512 || f.line_ns!=1468 || (i>4 && f.exposure_lines!=1000)))) bad=1;
    }
    close(fd);
    return bad;
}
