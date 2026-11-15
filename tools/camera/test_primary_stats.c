#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "camera_isp.h"
int main(void){
 struct redwood_camera_frame f={.width=2016,.height=1512,.stride=2528,.bayer=1,.black=64};
 uint8_t *raw=calloc(f.stride,f.height);assert(raw);
 for(unsigned y=0;y<f.height;y++)for(unsigned x=0;x<f.width;x+=4){uint8_t*g=raw+(size_t)y*f.stride+(x/4)*5;
  for(unsigned k=0;k<4;k++){unsigned v=180+(x+k)/8+y/16+((y&1)*2+((x+k)&1))*13;g[k]=v>>2;g[4]|=(v&3)<<(2*k);}}
 struct isp isp;isp_init(&isp);struct isp_stats s;
 for(unsigned sensor=1;sensor<=3;sensor++){
  f.sensor=sensor;clock_t start=clock();for(unsigned n=0;n<40;n++)isp_measure(&isp,&f,raw,&s);
  printf("%u %u %.9g %.9g %.9g %.6f\n",sensor,s.cells,s.mean[0],s.mean[1],s.mean[2],(double)(clock()-start)/CLOCKS_PER_SEC);
 }
 free(raw);
}
