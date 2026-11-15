#include <assert.h>
#include <stdio.h>
#include "../../userland/src/camera_s20_zoom.h"
int main(void){
 assert(s20_zoom_clamp(.6f,false,true)==1); /* front remains >=1 */
 assert(s20_zoom_clamp(.6f,true,false)==1); /* feature disabled */
 assert(s20_zoom_clamp(.5f,true,true)==.6f);
 assert(s20_zoom_clamp(INFINITY,true,true)==1);
 assert(s20_zoom_clamp(NAN,true,true)==1);
 assert(s20_zoom_clamp(20,true,true)==16);
 assert(s20_zoom_mode(.6f,true)==5&&s20_zoom_mode(.99f,true)==5);
 assert(s20_zoom_mode(1,true)==2&&s20_zoom_mode(.6f,false)==2);
 assert(fabsf(s20_zoom_crop(.6f,true)-1)<1e-6f);
 assert(fabsf(s20_zoom_crop(.9f,true)-1.5f)<1e-6f);
 assert(s20_zoom_crop(2,false)==2);
 puts("PASS: front/disabled zoom, finite clamp, lens boundary and per-lens crop");
}
