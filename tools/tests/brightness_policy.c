#include <assert.h>
#include <stdio.h>
#include "../../userland/src/brightness_policy.h"
static void near(double a,double b){if(fabs(a-b)>=.001)fprintf(stderr,"got %.3f expected %.3f\n",a,b);assert(fabs(a-b)<.001);}
int main(void){
 struct rb_policy p={0};rb_wake(&p,0,80);
 for(int i=0;i<50;i++)near(rb_observe(&p,i*.1,10),80);
 near(rb_observe(&p,5.1,10),30);
 /* A short flashlight burst must not alter the accepted target. */
 rb_observe(&p,6,1500);rb_observe(&p,7,10);near(p.target,30);
 rb_observe(&p,8,1500);near(rb_observe(&p,12.9,1500),30);
 near(rb_observe(&p,13.1,1500),420);
 rb_observe(&p,14,0);near(rb_observe(&p,21.9,0),420);
 near(rb_observe(&p,22.1,0),3);
 /* Manual calibration survives small room-light changes, releases across a
  * sustained large transition, and is retained across wake. */
 rb_manual(&p,30,10,90);
 for(int i=0;i<80;i++)near(rb_observe(&p,30+i*.1,14),90);
 rb_observe(&p,40,1500);near(rb_observe(&p,45.1,1500),420);
 rb_wake(&p,50,200);near(rb_observe(&p,51,10),200);
 near(rb_observe(&p,59.1,10),90);
 rb_manual(&p,60,10,420);near(rb_target(&p,1500),420);
 rb_manual(&p,70,150,3);near(rb_target(&p,0),3);
 struct rb_policy wake={0};rb_wake(&wake,0,80);rb_observe(&wake,0,10);near(rb_observe(&wake,5.1,10),30);
 rb_wake(&wake,10,120);near(rb_observe(&wake,10,10),120);near(rb_observe(&wake,17.9,10),120);near(rb_observe(&wake,18.1,10),30);
 double old=0;for(int i=0;i<=100000;i++){double n=rb_base(i/10.);assert(isfinite(n)&&n>=3&&n<=420&&n>=old);old=n;}
 puts("brightness policy: debounce, spikes, manual anchor, wake and bounds passed");
}
