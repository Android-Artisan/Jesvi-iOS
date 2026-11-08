/* Adaptive ambient brightness policy. Original implementation informed by
 * AOSP AutomaticBrightnessController's hysteresis/debounce/short-term-model
 * design; no Android source copied. Times are monotonic seconds, nits nominal. */
#ifndef REDWOOD_BRIGHTNESS_POLICY_H
#define REDWOOD_BRIGHTNESS_POLICY_H
#include <math.h>
#include <stdbool.h>
struct rb_policy {
 double accepted, candidate, mean, candidate_at, hold_until, target;
 double anchor_lux, anchor_nits;
 unsigned samples;
 bool ready, anchored;
};
static double rb_base(double lux)
{
 static const double ambient[] = {0,3,10,150,1500}, nits[] = {3,8,30,150,420};
 if(lux<=0) return 3;
 for(unsigned i=1;i<5;i++) if(lux<=ambient[i]) {
  double t=i<=2?(lux-ambient[i-1])/(ambient[i]-ambient[i-1]):
   (log1p(lux)-log1p(ambient[i-1]))/(log1p(ambient[i])-log1p(ambient[i-1]));
  return nits[i-1]+t*(nits[i]-nits[i-1]);
 }
 return 420;
}
static double rb_target(const struct rb_policy *p,double lux)
{
 double baseline=rb_base(lux);
 if(!p->anchored) return baseline;
 /* Hold the chosen level in similar lighting. Across a large change, blend
  * back toward the ordinary curve rather than carrying an extreme multiplier
  * into every environment (or pinning the screen at its maximum). */
 double distance=fabs(log1p(lux)-log1p(p->anchor_lux));
 double absolute=fabs(lux-p->anchor_lux);
 if(distance<=log(2.) || absolute<=20) return p->anchor_nits;
 double blend=fmax(0,fmin(1,(distance-log(2.))/log(4.)));
 double result=exp(log(p->anchor_nits)*(1-blend)+log(baseline)*blend);
 return lux>p->anchor_lux?fmax(p->anchor_nits,result):fmin(p->anchor_nits,result);
}
static void rb_wake(struct rb_policy *p,double now,double nits)
{
 p->candidate=-1;p->samples=0;p->candidate_at=now;
 p->hold_until=now+5;p->target=fmax(3,fmin(420,nits));
}
static void rb_manual(struct rb_policy *p,double now,double lux,double nits)
{
 p->anchor_lux=fmax(0,lux);p->anchor_nits=fmax(3,fmin(420,nits));p->anchored=true;
 p->accepted=p->anchor_lux;p->ready=true;
 rb_wake(p,now,p->anchor_nits);
}
static double rb_observe(struct rb_policy *p,double now,double lux)
{
 if(!isfinite(lux)||lux<0) return p->target;
 double tolerance=fmax(2,.2*(p->candidate+1));
 if(p->candidate<0||fabs(lux-p->candidate)>tolerance) {
  p->candidate=lux;p->mean=lux;p->candidate_at=now;p->samples=1;
 } else {
  if(p->samples<32)p->samples++;
  p->mean+=(lux-p->mean)/p->samples;
 }
 double delay=!p->ready||p->mean>p->accepted?3:5;
 if(now>=p->hold_until&&now-p->candidate_at>=delay) {
  double threshold=fmax(3,.2*(p->accepted+1));
  if(!p->ready||fabs(p->mean-p->accepted)>threshold) {
   p->accepted=p->mean;p->ready=true;
  }
  if(p->ready) p->target=fmax(3,fmin(420,rb_target(p,p->accepted)));
 }
 return p->target;
}
#endif
