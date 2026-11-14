#include <sys/redwood_fp_wire.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t packet[FP_WIRE_SIZE];
static void put(size_t off, uint32_t x) {
 for (unsigned i=0;i<4;i++) packet[off+i]=(uint8_t)(x>>(8*i));
}
static void reset(void) { memset(packet,0,sizeof(packet)); put(0x177004,6); }
static enum fp_wire_result parse(void) {
 return fp_wire_identify(packet,sizeof(packet),0,1);
}
int main(void) {
 assert(fp_wire_id_mask(NULL,2)==0);
 assert(fp_wire_id_mask((const uint8_t *)"1:",2)==1);
 assert(fp_wire_id_mask((const uint8_t *)"1:4:",4)==9);
 assert(fp_wire_id_mask((const uint8_t *)"0:",2)==0);
 assert(fp_wire_id_mask((const uint8_t *)"5:",2)==0);
 assert(fp_wire_id_mask((const uint8_t *)"1:1:",4)==0);
 assert(fp_wire_id_mask((const uint8_t *)"1",1)==0);
 assert(fp_wire_id_mask((const uint8_t *)"1;",2)==0);
 assert(fp_wire_id_mask((const uint8_t *)"1:",400)==0);

 assert(fp_wire_identify(NULL,0,0,1)==FP_WIRE_PROTOCOL_ERROR);
 reset();
 assert(fp_wire_identify(packet,FP_WIRE_SIZE-1,0,1)==FP_WIRE_PROTOCOL_ERROR);
 assert(fp_wire_identify(packet,sizeof(packet),1,1)==FP_WIRE_PROTOCOL_ERROR);
 assert(parse()==FP_WIRE_PROTOCOL_ERROR); /* zero-filled response cannot match */
 put(FP_WIRE_RESPONSE+24,0); /* old r[6] check accepted this */
 assert(parse()==FP_WIRE_PROTOCOL_ERROR);
 put(FP_WIRE_RESPONSE,0x4d);
 assert(parse()==FP_WIRE_PROTOCOL_ERROR);
 put(FP_WIRE_ERROR,0x20); put(FP_WIRE_RESPONSE+4,0x20);
 assert(parse()==FP_WIRE_NO_MATCH); /* op 77 is not success */
 reset(); put(FP_WIRE_ERROR,0x27);put(FP_WIRE_RESPONSE+4,0x27);
 assert(parse()==FP_WIRE_REJECTED);
 reset();
 for (unsigned op=4;op<=7;op++) {
  put(FP_WIRE_RESPONSE,op); assert(parse()==FP_WIRE_PENDING);
 }
 reset();put(FP_WIRE_RESPONSE+16,42);put(FP_WIRE_RESPONSE+20,1);
 assert(parse()==FP_WIRE_PROTOCOL_ERROR); /* no token-present flag */
 packet[FP_WIRE_ID_TOKEN_PRESENT]=1;
 assert(parse()==FP_WIRE_MATCH_CANDIDATE);
 packet[FP_WIRE_ID_TOKEN_PRESENT+1]=1;assert(parse()==FP_WIRE_PROTOCOL_ERROR);
 packet[FP_WIRE_ID_TOKEN_PRESENT+1]=0;
 put(FP_WIRE_RESPONSE,0x4d);assert(parse()==FP_WIRE_PROTOCOL_ERROR);
 put(FP_WIRE_RESPONSE,0);
 assert(fp_wire_identify(packet,sizeof(packet),0,0)==FP_WIRE_PROTOCOL_ERROR);
 assert(fp_wire_identify(packet,sizeof(packet),0,5)==FP_WIRE_PROTOCOL_ERROR);
 put(FP_WIRE_RESPONSE+20,2); assert(parse()==FP_WIRE_PROTOCOL_ERROR);
 put(FP_WIRE_RESPONSE+20,1); put(FP_WIRE_RESPONSE+16,0);
 assert(parse()==FP_WIRE_MATCH_CANDIDATE); /* wrapper does not gate on the score word */
 put(FP_WIRE_RESPONSE+16,42);put(FP_WIRE_RESPONSE+4,0x20);
 assert(parse()==FP_WIRE_PROTOCOL_ERROR); /* conflicting error fields */
 put(FP_WIRE_RESPONSE+4,0);put(0x177004,3);
 assert(parse()==FP_WIRE_PROTOCOL_ERROR); /* old enrollment reply */
 puts("PASS: FINGE response bounds, command/error agreement, no-match, op77, candidate fields");
}
