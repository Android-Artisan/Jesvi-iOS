/* MIT. One bounded S5123 BOOT attempt, followed by verified CP-off cleanup.
 * Reuses the live hardware-proven SPI adapter and tested power lifecycle.
 * No DMA, shared-RAM writes, NV, MAIN, calls or network activity. */
#define LIVE_SPI_ADAPTER_ONLY 1
#include "live-spi.c"
#include "s5123-boot.h"

static const struct {uint64_t base;unsigned pin;} outputs[]={
    {0x15c30120,0},{0x15c30280,0},{0x15c30320,0},{0x107300c0,3},{0x10730060,6}
};
static const unsigned controller_regs[]={S5123_CH,S5123_MODE,S5123_CS,S5123_IRQ,
    S5123_PACKET,S5123_SWAP,S5123_FEEDBACK,S5123_USI_CON};
struct modem {
    struct spi_state spi;
    uint32_t con[5],dat[5],changed[5];
    uint32_t mux,divider,mode,pins,pulls,pad_dat,saved[8];
    int clock_changed,mode_changed,controller_saved,pads_changed;
};
static void phase(const char *s) {printf("cellular live BOOT: %s\n",s);fflush(stdout);}
static int masked_write(uint64_t address,uint32_t mask,uint32_t value,int verify)
{
    int allowed=address==0x10730068;
    for (unsigned i=0;i<5;i++)
        if (address==outputs[i].base || address==outputs[i].base+4) allowed=1;
    if (!allowed || (value&~mask)) return -1;
    char command[128],response[8192];unsigned long long got;unsigned old,next,back;int used=0;
    snprintf(command,sizeof(command),"m 0x%llx 0x%x 0x%x",(unsigned long long)address,~mask,value);
    return request(command,response) || sscanf(response,"%llx: %x -> %x, reads %x %n",
        &got,&old,&next,&back,&used)!=4 || response[used] || got!=address ||
        next!=((old&~mask)|value) || (verify && (back&mask)!=value) ? -1:0;
}
static int masked(uint64_t address,uint32_t mask,uint32_t value)
{
    return masked_write(address,mask,value,1);
}
static int restore_mask(uint64_t address,uint32_t mask,uint32_t value)
{
    for (unsigned i=0;i<3;i++) if (!masked(address,mask,value&mask)) return 0;
    return -1;
}
static int ready(void *context)
{
    struct modem *m=context;uint32_t power,active,isolation,g0,g1;
    phase("read CP-off, owner and clock guards");
    if (read_word(0x15860720,&isolation) || read_word(0x15c30284,&power) ||
        read_word(0x15c30384,&active) || read_word(0x10700690,&m->mux) ||
        read_word(0x10701820,&m->divider) || read_word(0x10721048,&m->mode) ||
        read_word(0x10730060,&m->pins) || read_word(0x10730064,&m->pad_dat) ||
        read_word(0x10730068,&m->pulls) || read_word(0x107020fc,&g0) || read_word(0x1070208c,&g1)) return 0;
    if ((isolation&1) || (power&1) || (active&1) || m->mode || (m->pins&0xffff0000u) ||
        (m->mux&(1u<<16)) || (m->divider&(1u<<16)) || !(g0&(1u<<21)) || !(g1&(1u<<21))) return 0;
    for (unsigned i=0;i<5;i++) {
        if (read_word(outputs[i].base,&m->con[i]) || read_word(outputs[i].base+4,&m->dat[i])) return 0;
        unsigned function=(m->con[i]>>(4*outputs[i].pin))&15;
        /* Critical controls may already be held output-low by a prior clean attempt. */
        if (function && !(i<2 && function==1 && !(m->dat[i]&(1u<<outputs[i].pin)))) return 0;
    }
    return 1;
}
static int prepare(void *context)
{
    struct modem *m=context;phase("prepare SPI clocks/controller; pads stay inputs");
    m->clock_changed=1;
    if (write_word(0x10701820,(m->divider&~15u)|3,0) || !clock_wait(0x10701820,(1u<<16)|15,3) ||
        write_word(0x10700690,m->mux&~16u,0) || !clock_wait(0x10700690,(1u<<16)|16,0)) return -1;
    m->mode_changed=1;
    if (write_word(0x10721048,2,1)) return -1;
    for (unsigned i=0;i<8;i++) if (read_word(0x10960000+controller_regs[i],&m->saved[i])) return -1;
    m->controller_saved=1;
    if ((m->saved[0]&3) || m->saved[3] || m->saved[4]) return -1;
    if (write_word(0x10960000,0,1) || write_word(0x10960020,0,1) ||
        write_word(0x10960010,0,1) || write_word(0x1096000c,1,1) || write_word(0x109600c4,0,1)) return -1;
    return 0;
}
static int output(void *context,enum s5123_ap_output signal,int level)
{
    struct modem *m=context;
    if ((unsigned)signal>=5 || (unsigned)level>1) return -1;
    uint64_t base=outputs[signal].base;unsigned pin=outputs[signal].pin;
    m->changed[signal]=1;
    int error=1;
    for (unsigned attempt=0;attempt<(cleanup?3u:1u);attempt++) {
        uint32_t dat;
        /* DAT reads the pad while it is input. Preload the latch without
         * demanding a pad level until CON has selected output mode. */
        if (!masked_write(base+4,1u<<pin,(unsigned)level<<pin,0) &&
            !masked(base,15u<<(4*pin),1u<<(4*pin)) &&
            !read_word(base+4,&dat) && ((dat>>pin)&1)==(unsigned)level) {error=0;break;}
    }
    printf("cellular live BOOT: output %u level %d verified %d\n",signal,level,!error);fflush(stdout);
    return error?-1:0;
}
static int delay(void *context,uint32_t us)
{
    (void)context;
    if (stopped && !cleanup) return -1;
    struct timespec remaining={us/1000000,(long)(us%1000000)*1000};
    while (nanosleep(&remaining,&remaining)) {
        if (errno!=EINTR || (stopped && !cleanup)) return -1;
    }
    return 0;
}
static int route(void *context)
{
    struct modem *m=context;uint32_t con,dat;
    phase("route external SPI16 pads; internal loopback disabled");
    if (read_word(0x10730060,&con) || read_word(0x10730064,&dat) ||
        (con&0xffff0000u)!=0x01000000u || (dat&(1u<<6))) return -1;
    m->pads_changed=1;
    return masked(0x10730068,0xffff0000u,0) || masked(0x10730060,0xffff0000u,0x22220000u) ? -1:0;
}
static int inputs(uint32_t *value)
{
    uint32_t active,wake,hold;
    if (read_word(0x15c30384,&active) || read_word(0x15c303c4,&wake) || read_word(0x15c303e4,&hold)) return -1;
    *value=(active&1)|((wake&1)<<1)|((hold&1)<<2);return 0;
}
static int send_boot(void *context,const uint8_t *data,size_t length,uint32_t *completed)
{
    struct modem *m=context;struct s5123_spi_result result={0};
    struct s5123_spi_ops ops={&m->spi,spi_read32,spi_read8,spi_write32,spi_write8,now_us};
    phase("send BOOT only; byte PIO, no DMA");uint64_t start=now_us(NULL);
    int error=s5123_spi_send_boot(&ops,&result,data,length,1625000,5000000,1);
    *completed=result.completed_bytes;
    printf("cellular live BOOT: PIO error %d io_error %d completed %u received %u packets %u elapsed_us %llu\n",
        error,m->spi.io_error,*completed,m->spi.received,result.packets,(unsigned long long)(now_us(NULL)-start));fflush(stdout);
    if (error || m->spi.io_error || *completed!=length) return -1;
    phase("observe CP input transitions for 4000ms; levels do not prove acceptance");
    uint32_t first,previous,changed=0,rising=0;
    if (inputs(&first)) return -1;previous=first;
    for (unsigned i=0;i<200;i++) {
        uint32_t current;if (delay(context,20000) || inputs(&current)) return -1;
        changed|=previous^current;rising|=~previous&current;previous=current;
    }
    printf("cellular live BOOT: GPIO first %x last %x changed %x rising %x (PHONE_ACTIVE,WAKE,PS_HOLD)\n",
        first,previous,changed,rising);fflush(stdout);return 0;
}
static int park(void *context)
{
    struct modem *m=context;cleanup=1;phase("park SPI, then hold reset and power low");
    if (!m->controller_saved) return 0;
    int error=0;
    if (restore_word(0x10960000,0,1)) error=1;
    if (restore_word(0x10960020,0,1)) error=1;
    if (restore_word(0x1096000c,1,1)) error=1;
    return error?-1:0;
}
static int restore(void *context)
{
    struct modem *m=context;cleanup=1;int error=0;phase("restore SPI/clock/pad/AP ownership");
    if (m->controller_saved) {
        if (restore_word(0x10960000,0,1)) error=1;
        if (restore_word(0x10960020,0,1)) error=1;
        if (restore_word(0x10960000,0x20,0)) error=1;
        if (restore_word(0x10960000,0,1)) error=1;
        uint32_t status;if (read_word(0x10960014,&status) || (status&((0x7fu<<6)|(0x7fu<<15)))) error=1;
        for (unsigned i=1;i<8;i++) if (restore_word(0x10960000+controller_regs[i],m->saved[i],1)) error=1;
        if (restore_word(0x10960000,m->saved[0],1)) error=1;
    }
    if (m->pads_changed || m->changed[S5123_DUMP_NOTI]) {
        if (restore_mask(0x10730060,0xffff0000u,m->pins)) error=1;
        if (restore_mask(0x10730068,0xffff0000u,m->pulls)) error=1;
        if (masked_write(0x10730064,0xf0u,m->pad_dat&0xf0u,0)) error=1;
    }
    for (unsigned i=S5123_AP_STATUS;i<=S5123_AP_WAKE;i++) if (m->changed[i]) {
        unsigned pin=outputs[i].pin;
        if (restore_mask(outputs[i].base,15u<<(4*pin),m->con[i])) error=1;
        if (masked_write(outputs[i].base+4,1u<<pin,m->dat[i]&(1u<<pin),0)) error=1;
    }
    if (m->mode_changed && restore_word(0x10721048,m->mode,1)) error=1;
    if (m->clock_changed) {
        if (restore_word(0x10701820,m->divider,0)) error=1;
        if (!clock_wait(0x10701820,(1u<<16)|15,m->divider&15)) error=1;
        if (restore_word(0x10700690,m->mux,0)) error=1;
        if (!clock_wait(0x10700690,(1u<<16)|16,m->mux&16)) error=1;
    }
    return error?-1:0;
}
static int quiescent(void *context)
{
    (void)context;uint32_t iso,reset_con,reset_dat,power_con,power_dat;
    return !read_word(0x15860720,&iso) && !(iso&1) &&
        !read_word(0x15c30120,&reset_con) && (reset_con&15)==1 &&
        !read_word(0x15c30124,&reset_dat) && !(reset_dat&1) &&
        !read_word(0x15c30280,&power_con) && (power_con&15)==1 &&
        !read_word(0x15c30284,&power_dat) && !(power_dat&1);
}
#ifndef LIVE_MODEM_TEST
static int read_file(const char *path,uint8_t *data,size_t capacity,size_t *length)
{
    FILE *file=fopen(path,"rb");if (!file) return -1;
    *length=fread(data,1,capacity,file);int error=ferror(file)||!feof(file);fclose(file);return error?-1:0;
}
int main(int argc,char **argv)
{
    if (argc!=4 || strcmp(argv[1],"--reserved-base-verified")) return 2;
    uint8_t image[131073],info[25];size_t length,info_length;uint32_t fields[5];
    if (read_file(argv[2],image,sizeof(image),&length) || read_file(argv[3],info,sizeof(info),&info_length) ||
        info_length!=24 || memcmp(info,"S512",4)) return 2;
    memcpy(fields,info+4,sizeof(fields));
    if (fields[0]!=1 || fields[1]!=length || fields[3]!=0x40000000 || fields[4]!=1) return 2;
    signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);signal(SIGHUP,stop_signal);
    char response[8192];
    if (request("spi8.rx 0",response) || strcmp(response,"spi8: invalid byte count/hex\n")) return 1;
    struct modem m={0};struct s5123_boot_result result={0};
    struct s5123_boot_ops ops={&m,ready,prepare,output,delay,route,send_boot,park,restore,quiescent};
    int error=s5123_boot_once(&ops,&result,image,length,fields[2]);
    printf("cellular live BOOT: error %d attempted %u power_requested %u phase %u completed %u cleanup_verified %u; acceptance unproven\n",
        error,result.attempted,result.power_requested,result.phase,result.spi_completed,result.cleanup_safe);fflush(stdout);
    return result.attempted && result.cleanup_safe ? (error?1:0):2;
}
#endif
