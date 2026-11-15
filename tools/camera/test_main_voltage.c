#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_main_voltage.h"
struct model{uint8_t r[4][256];unsigned ops,fail,writes;bool latch;};
static bool rd(void *ctx,uint8_t p,uint8_t r,uint8_t *v){struct model*m=ctx;if(++m->ops==m->fail)return false;*v=m->r[p][r];return true;}
static bool wr(void *ctx,uint8_t p,uint8_t r,uint8_t v){struct model*m=ctx;m->writes++;bool ok=++m->ops!=m->fail;if(ok||m->latch)m->r[p][r]=v;return ok;}
static struct model initial(void){struct model m={0};m.r[2][0x1e]=0x18;m.r[3][4]=0x40;m.r[3][7]=0x60;return m;}
int main(void){
 for(unsigned fault=1;fault<=9;fault++)for(unsigned latch=0;latch<2;latch++){
  struct model m=initial(),before=m;struct cam_main_voltage s={0};m.fail=fault;m.latch=latch;
  struct cam_main_voltage_io io={&m,rd,wr};assert(!cam_main_voltage_begin(&s,io));
  assert(cam_main_voltage_restore(&s,io));assert(!memcmp(m.r,before.r,sizeof(m.r))&&!s.changed);
 }
 struct model m=initial(),before=m;struct cam_main_voltage s={0};struct cam_main_voltage_io io={&m,rd,wr};
 assert(cam_main_voltage_begin(&s,io));assert(m.r[2][0x1e]==0x24&&m.r[3][4]==0x5c&&m.r[3][7]==0x6e);
 assert(cam_main_voltage_restore(&s,io)&&!memcmp(m.r,before.r,sizeof(m.r)));
 m=initial();s=(struct cam_main_voltage){0};m.r[2][0x1e]|=0x80;assert(!cam_main_voltage_begin(&s,io)&&!m.writes);
 m=initial();s=(struct cam_main_voltage){0};assert(cam_main_voltage_begin(&s,io));m.r[2][0x1e]|=0x80;
 assert(!cam_main_voltage_restore(&s,io));assert(m.r[2][0x1e]==0xa4);m.r[2][0x1e]&=0x7f;assert(cam_main_voltage_restore(&s,io));
 m=initial();s=(struct cam_main_voltage){0};assert(cam_main_voltage_begin(&s,io));m.r[3][7]=0x2f;
 assert(!cam_main_voltage_restore(&s,io)&&m.r[3][7]==0x2f);
 puts("PASS: primary selectors, all startup faults/latched NACKs, reserved bits, live-rail refusal and rollback ownership");
}
