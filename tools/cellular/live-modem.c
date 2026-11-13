/* MIT. One bounded S5123 BOOT attempt, followed by verified CP-off cleanup.
 * Reuses the live hardware-proven SPI adapter and tested power lifecycle.
 * No DMA, shared-RAM writes, NV, MAIN, calls or network activity. */
#define LIVE_SPI_ADAPTER_ONLY 1
#include "live-spi.c"
#include "s5123-boot.h"
#include "s5123-live-link.h"
static int link_requested;
static int link_delay(uint32_t);

static const struct {uint64_t base;unsigned pin;} outputs[]={
    {0x15c30120,0},{0x15c30280,0},{0x15c30320,0},{0x107300c0,3},{0x10730060,6}
};
static const unsigned controller_regs[]={S5123_CH,S5123_MODE,S5123_CS,S5123_IRQ,
    S5123_PACKET,S5123_SWAP,S5123_FEEDBACK,S5123_USI_CON};
struct modem {
    struct spi_state spi;
    struct s5123_link link;
    uint32_t con[5],dat[5],changed[5];
    uint32_t mux,divider,mode,pins,pulls,pad_dat,saved[8];
    int clock_changed,mode_changed,controller_saved,pads_changed;
};
static void phase(const char *s) {printf("cellular live BOOT: %s\n",s);fflush(stdout);}
static int link_read(uint64_t address,uint32_t *value)
{
    printf("cellular live BOOT: PCIe before read %llx\n",(unsigned long long)address);fflush(stdout);
    return read_word(address,value);
}
static int masked_write(uint64_t address,uint32_t mask,uint32_t value,int verify)
{
    if (link_requested && address==0x14ffe004) {
        /* The sole endpoint write is COMMAND with BME clear. Never echo W1C
         * status bits or enable bus mastering; no arbitrary config writes. */
        if (mask!=0xffff || (value&~0xffffu) || (value&4)) return -1;
        char command[128],response[8192];unsigned long long got;unsigned old,next,back;int used=0;
        snprintf(command,sizeof(command),"w 0x14ffe004 0x%x",value);
        if (request(command,response) || sscanf(response,"%llx: %x -> %x, reads %x %n",
            &got,&old,&next,&back,&used)!=4 || response[used] || got!=address || next!=value ||
            (back&0xffff)!=value) return -1;
        return 0;
    }
    int allowed=address==0x10730068 || (link_requested && s5123_link_allowed(address));
    for (unsigned i=0;i<5;i++)
        if (address==outputs[i].base || address==outputs[i].base+4) allowed=1;
    if (!allowed || (value&~mask)) return -1;
    char command[128],response[8192];unsigned long long got;unsigned old,next,back;int used=0;
    snprintf(command,sizeof(command),"m 0x%llx 0x%x 0x%x",(unsigned long long)address,~mask,value);
    if (request(command,response) || sscanf(response,"%llx: %x -> %x, reads %x %n",
        &got,&old,&next,&back,&used)!=4 || response[used] || got!=address ||
        next!=((old&~mask)|value)) {
        fprintf(stderr,"cellular live BOOT: masked I/O/parse failure %llx\n",(unsigned long long)address);return -1;
    }
    if (verify && (back&mask)!=value) {
        fprintf(stderr,"cellular live BOOT: readback mismatch %llx mask %08x old %08x requested %08x read %08x\n",
            (unsigned long long)address,mask,old,next,back);return -1;
    }
    return 0;
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
    if (link_requested) {
        m->link.io=(struct s5123_link_io){link_read,masked_write,link_delay,phase};
        if (s5123_link_guard(&m->link)) return 0;
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
static int link_delay(uint32_t us) {return delay(NULL,us);}
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

/* ---- firmware download (A, 2026-11-12): legacy RAW queues in the reserved IPC memory, Samsung's
 * link_start_normal_boot + xmit_to_legacy_link + cbd's SIPC UDL. Only with --download TOC. ---- */
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "../../xnu/files/bsd/sys/redwood_cp_ipc.h"
#include "s5123-udl.h"
static const char *download_toc;
static int ipc_mapped;
#define RAW_TX_HEAD 0x18u
#define RAW_TX_TAIL 0x1cu
#define RAW_RX_HEAD 0x20u
#define RAW_RX_TAIL 0x24u
#define RAW_TX_BUF  0x3000u
#define RAW_TX_SIZE 0x1fd000u
#define RAW_RX_BUF  0x200000u
#define RAW_RX_SIZE 0x200000u
static int ipc_op(unsigned op,uint32_t offset,const void *in,uint32_t length,void *out)
{
    struct cp_ipc_request q={.version=CP_IPC_API,.op=op,.offset=offset,.length=length};
    struct cp_ipc_reply r={0};size_t size=sizeof(r),qs=CP_IPC_HEADER;
    if ((op==CP_IPC_WRITE || op==CP_IPC_STORE32) && in) {memcpy(q.data,in,length);qs+=length;}
    if (op<=CP_IPC_UNMAP) q.length=0;
    if (sysctlbyname("kern.redwood.cp_ipc",&r,&size,&q,qs)) return -1;
    if (out && (op==CP_IPC_READ || op==CP_IPC_LOAD32)) memcpy(out,r.data,length);
    return 0;
}
static int ipc_load(uint32_t off,uint32_t *v) {return ipc_op(CP_IPC_LOAD32,off,NULL,4,v);}
static int ipc_store(uint32_t off,uint32_t v) {return ipc_op(CP_IPC_STORE32,off,&v,4,NULL);}
static int ipc_write(uint32_t off,const uint8_t *d,uint32_t n)
{
    while (n) {uint32_t c=n>CP_IPC_CHUNK?CP_IPC_CHUNK:n;if (ipc_op(CP_IPC_WRITE,off,d,c,NULL)) return -1;off+=c;d+=c;n-=c;}
    return 0;
}
static int ipc_read(uint32_t off,uint8_t *d,uint32_t n)
{
    while (n) {uint32_t c=n>CP_IPC_CHUNK?CP_IPC_CHUNK:n;if (ipc_op(CP_IPC_READ,off,NULL,c,d)) return -1;off+=c;d+=c;n-=c;}
    return 0;
}
/* init_legacy_link + link_start_normal_boot: indices zero, access 1, magic BOOT */
static int pktproc_info(void);
static int ipc_boot_init(void)
{
    if (ipc_op(CP_IPC_MAP,0,NULL,0,NULL)) {fprintf(stderr,"cellular live DL: IPC map refused\n");return -1;}
    ipc_mapped=1;
    {   /* the whole IPC area zeroed first: it holds whatever RAM held (SR info, kernel time at 0x424 ...) */
        static const uint8_t zero[2048];uint64_t t0=now_us(NULL);
        for (uint32_t o=0;o<0x800000;o+=sizeof(zero)) if (ipc_write(o,zero,sizeof(zero))) return -1;
        printf("cellular live DL: IPC 8 MB zeroed in %llu ms\n",(unsigned long long)((now_us(NULL)-t0)/1000));fflush(stdout);
    }
    for (uint32_t o=0;o<0x28;o+=4) if (ipc_store(o,0)) return -1;
    for (uint32_t o=0x400;o<0x410;o+=4) if (ipc_store(o,0)) return -1;
    if (pktproc_info()) return -1;   /* as pktproc_create() at probe: before the CP boots */
    if (ipc_store(0x408,2u<<14)) return -1;     /* init_control_messages: ap2cp_united_status ds_det=2 (dual SIM) */
    if (ipc_store(4,1) || ipc_store(0,0x424f4f54u)) return -1;
    uint32_t m=0,a=0;if (ipc_load(0,&m) || ipc_load(4,&a) || m!=0x424f4f54u || a!=1) return -1;
    phase("IPC: queues zeroed, access 1, magic BOOT");return 0;
}
static int raw_send(const uint8_t *frame,uint32_t n)
{
    uint32_t in,out;
    if (ipc_load(RAW_TX_HEAD,&in) || ipc_load(RAW_TX_TAIL,&out) || in>=RAW_TX_SIZE || out>=RAW_TX_SIZE) return -1;
    uint32_t space=(out>in?out-in:RAW_TX_SIZE-in+out)-1;if (space<n) return -2;
    uint32_t first=RAW_TX_SIZE-in<n?RAW_TX_SIZE-in:n;
    if (ipc_write(RAW_TX_BUF+in,frame,first) || (n>first && ipc_write(RAW_TX_BUF,frame+first,n-first))) return -1;
    __sync_synchronize();
    return ipc_store(RAW_TX_HEAD,(in+n)%RAW_TX_SIZE);
}
/* one SIPC5 frame from RAW RX, waiting up to ms; returns length (0 = nothing) */
static int raw_recv(uint8_t *buf,uint32_t cap,unsigned ms)
{
    uint64_t deadline=now_us(NULL)+(uint64_t)ms*1000;      /* (usleep(2000) sleeps far longer here: a clock, not a count) */
    for (;;) {
        if (now_us(NULL)>deadline || stopped) return 0;
        uint32_t in,out;
        if (ipc_load(RAW_RX_HEAD,&in) || ipc_load(RAW_RX_TAIL,&out) || in>=RAW_RX_SIZE || out>=RAW_RX_SIZE) return -1;
        if (in!=out) {
            uint8_t h[4];if (ipc_read(RAW_RX_BUF+out,h,4)) return -1;
            uint32_t len=h[2]|(h[3]<<8),total=(len+7)&~7u;
            if (len<4 || total>cap) {fprintf(stderr,"cellular live DL: RX head %x tail %x bad frame %02x %02x len %u\n",in,out,h[0],h[1],len);return -1;}
            uint32_t first=RAW_RX_SIZE-out<total?RAW_RX_SIZE-out:total;
            if (ipc_read(RAW_RX_BUF+out,buf,first) || (total>first && ipc_read(RAW_RX_BUF,buf+first,total-first))) return -1;
            if (ipc_store(RAW_RX_TAIL,(out+total)%RAW_RX_SIZE)) return -1;
            return (int)len;
        }
        usleep(2000);
    }
    return 0;
}
static int udl_step(unsigned stage,enum s5123_udl_op op,const uint8_t *frame,size_t n)
{
    uint8_t reply[64];
    int e=raw_send(frame,(uint32_t)n);
    if (e) {printf("cellular live DL: stage %u op %u: send error %d\n",stage,op,e);fflush(stdout);return -1;}
    int got=raw_recv(reply,sizeof(reply),stage==1 && op==S5123_UDL_START?10000:3000);
    {
        uint32_t w[10];for (unsigned i=0;i<10;i++) ipc_load(i*4,&w[i]);
        printf("cellular live DL: IPC header after:");for (unsigned i=0;i<10;i++) printf(" %08x",w[i]);printf("\n");fflush(stdout);
    }
    uint32_t th=0,tt=0,rh=0,rt=0;ipc_load(RAW_TX_HEAD,&th);ipc_load(RAW_TX_TAIL,&tt);ipc_load(RAW_RX_HEAD,&rh);ipc_load(RAW_RX_TAIL,&rt);
    printf("cellular live DL: stage %u op %u: reply %d bytes; TX head %x tail %x (tail moves = CP consumed); RX head %x tail %x\n",
        stage,op,got,th,tt,rh,rt);
    if (got>0) {printf("cellular live DL: reply");for (int i=0;i<got && i<16;i++) printf(" %02x",reply[i]);printf("\n");}
    fflush(stdout);
    if (got<=0) return -1;
    enum s5123_udl_ack a=s5123_udl_response(reply,(size_t)((got+7)&~7),stage,op);
    printf("cellular live DL: stage %u op %u: %s\n",stage,op,a==S5123_UDL_ACK?"ACK":a==S5123_UDL_PEER_FAIL?"CP reports FAIL":"unexpected reply");
    fflush(stdout);
    return a==S5123_UDL_ACK?0:-1;
}


/* ---- after FIN: Samsung's shmem_cmd_handler, polled (A, 2026-11-13). cp2ap_msg +404, ap2cp_msg +400,
 * united status +408/+40c (DT, DRAM_V1 at the IPC base). int = 0x80|0x40|cmd. AP->CP doorbell =
 * write 0x10000+int_num into EP BAR0+0x60000 (pci_db_addr), through iATU outbound region 1. ---- */
static int doorbell_ready;
static int doorbell(unsigned num)
{
    char r[8192],c[96];
    if (!doorbell_ready) {
        /* iATU region1 (DBI+300000+0x200): MEM, 14d00000..14dfffff -> PCI 14d00000 (EP BAR0) */
        if (request("w 0x13b00200 0x0",r) || request("w 0x13b00208 0x14d00000",r) || request("w 0x13b0020c 0x0",r) ||
            request("w 0x13b00210 0x14dfffff",r) || request("w 0x13b00214 0x14d00000",r) || request("w 0x13b00218 0x0",r) ||
            request("w 0x13b00204 0x80000000",r)) return -1;
        doorbell_ready=1;
    }
    snprintf(c,sizeof(c),"w 0x14d60000 0x%x",0x10000u+num);
    int e=request(c,r);printf("cellular live DL: doorbell %#x: %s",0x10000u+num,e?"failed\n":r);fflush(stdout);return e;
}
static int ap2cp(unsigned cmd)
{
    if (ipc_store(0x400,0xc0u|cmd)) return -1;__sync_synchronize();
    printf("cellular live DL: AP -> CP cmd %#x (msg %#x)\n",cmd,0xc0u|cmd);fflush(stdout);
    return doorbell(0);
}
static void fmt_dump(void)
{
    uint32_t rh,rt;if (ipc_load(0x10,&rh) || ipc_load(0x14,&rt)) return;
    while (rh!=rt && rt<0x1000 && rh<0x1000) {
        uint8_t h[4],b[64];if (ipc_read(0x2000+rt,h,4)) return;
        uint32_t len=h[2]|(h[3]<<8),total=(len+3)&~3u;if (len<4 || len>0x1000) {printf("cellular live DL: FMT RX bad len %u at %x\n",len,rt);return;}
        uint32_t show=len<64?len:64,first=0x1000-rt<show?0x1000-rt:show;ipc_read(0x2000+rt,b,first);if (show>first) ipc_read(0x2000,b+first,show-first);
        printf("cellular live DL: FMT RX frame (%u bytes):",len);for (unsigned i=0;i<show;i++) printf(" %02x",b[i]);printf("\n");
        rt=(rt+total)%0x1000;ipc_store(0x14,rt);
    }
    fflush(stdout);
}

/* ---- SBD rings (link_device_memory_sbd.c create_sbd_mem_map + init_sbd_link), this board's DT iodevs in
 * order, minus non-IPC/PS channels and the kor_skt/usa_spr option-region ones; MULTI_RAW -> QoS ch 10,11 ---- */
struct sbd_ch {unsigned ch,ul_len,ul_size,dl_len,dl_size;};
static const struct sbd_ch sbd_chs[]={
    {35,16,0x800,16,0x800},{235,16,0x1000,32,0x1000},{236,16,0x1000,32,0x1000},{245,16,0x800,512,0x800},
    {1,32,0x800,64,0x800},{25,16,0x800,16,0x800},{28,16,0x800,128,0x800},{244,16,0x800,32,0x800},
    {10,1024,0x800,256,0x800},{11,512,0x800,1024,0x800}};
#define SBD_N (sizeof(sbd_chs)/sizeof(sbd_chs[0]))
static uint32_t sbd_rpwp,sbd_pos[SBD_N][2],sbd_lenarr[SBD_N][2],sbd_buf[SBD_N][2];
static uint32_t sbd_rp(unsigned id,unsigned dir) {return sbd_rpwp+2*(id+SBD_N*(dir*2));}
static uint32_t sbd_wp(unsigned id,unsigned dir) {return sbd_rpwp+2*(id+SBD_N*(dir*2+1));}
static int ipc_load16(uint32_t off,uint16_t *v) {uint32_t w;if (ipc_load(off&~3u,&w)) return -1;*v=(uint16_t)(w>>((off&2)*8));return 0;}
static int ipc_store16(uint32_t off,uint16_t v)
{uint32_t w;if (ipc_load(off&~3u,&w)) return -1;unsigned sh=(off&2)*8;w=(w&~(0xffffu<<sh))|((uint32_t)v<<sh);return ipc_store(off&~3u,w);}
static int sbd_init(void)
{
    uint8_t zero[2048]={0};
    for (uint32_t o=0x800;o<0x10000;o+=sizeof(zero)) if (ipc_write(o,zero,sizeof(zero))) return -1;
    uint32_t g=0x800,desc=g+12+32*2*8+32*2*20,buff=0x10000;
    sbd_rpwp=desc;desc+=2*2*2*SBD_N;
    for (unsigned i=0;i<SBD_N;i++) for (unsigned dir=0;dir<2;dir++) {
        unsigned len=dir?sbd_chs[i].dl_len:sbd_chs[i].ul_len,size=dir?sbd_chs[i].dl_size:sbd_chs[i].ul_size;
        sbd_pos[i][dir]=desc;desc+=len*4;sbd_lenarr[i][dir]=desc;desc+=len*4;
        sbd_buf[i][dir]=buff;buff+=len*size;
    }
    if (desc>0x10000 || buff>0x800000) {printf("cellular live DL: SBD layout overflow desc %x buff %x\n",desc,buff);return -1;}
    if (ipc_store(g,1) || ipc_store(g+4,SBD_N) || ipc_store(g+8,sbd_rpwp)) return -1;
    for (unsigned i=0;i<SBD_N;i++) for (unsigned dir=0;dir<2;dir++) {
        unsigned len=dir?sbd_chs[i].dl_len:sbd_chs[i].ul_len,size=dir?sbd_chs[i].dl_size:sbd_chs[i].ul_size;
        for (unsigned k=0;k<len;k++) if (ipc_store(sbd_pos[i][dir]+4*k,sbd_buf[i][dir]+k*size)) return -1;
        uint32_t chd=g+12+(i*2+dir)*8,rbd=g+12+32*2*8+(i*2+dir)*20;
        uint8_t d[20];memset(d,0,sizeof(d));
        d[0]=sbd_chs[i].ch;d[4]=dir;d[6]=1;d[8]=0x81;d[12]=len&0xff;d[13]=len>>8;d[14]=i;d[16]=size&0xff;d[17]=size>>8;
        if (ipc_write(rbd,d,20) || ipc_store(chd,rbd) || ipc_store(chd+4,sbd_pos[i][dir])) return -1;
    }
    printf("cellular live DL: SBD: %u channels, rpwp %x, descriptors end %x, buffers end %x\n",(unsigned)SBD_N,sbd_rpwp,desc,buff);fflush(stdout);
    return 0;
}
/* one SIPC5 frame into channel id's UL ring, then MASK_SEND_DATA to the CP */
static int sbd_send(unsigned id,const uint8_t *frame,uint32_t n)
{
    uint16_t rp,wp;unsigned len=sbd_chs[id].ul_len;
    if (ipc_load16(sbd_rp(id,0),&rp) || ipc_load16(sbd_wp(id,0),&wp) || (unsigned)(wp+1)%len==rp) return -1;
    if (n>sbd_chs[id].ul_size || ipc_write(sbd_buf[id][0]+wp*sbd_chs[id].ul_size,frame,n) ||
        ipc_store(sbd_lenarr[id][0]+4*wp,n)) return -1;
    __sync_synchronize();
    if (ipc_store16(sbd_wp(id,0),(uint16_t)((wp+1)%len))) return -1;
    if (ipc_store(0x400,0x81)) return -1;
    return doorbell(0);
}
static int stay_mode;
static int data_off;          /* user switched mobile data off (Settings / Control Center / nodata) */
static int dm_on;
static void cell_ipc(const uint8_t *m,unsigned len);
static void pdn_ipconfig(const uint8_t *d,unsigned n);
static int sbd_send(unsigned id,const uint8_t *frame,uint32_t n);

/* ---- RFS server (umts_rfs0, ch 245, SBD id 3), after mifmon.c (postmarketOS Note 8, decoded from libsec-ril):
 * a byte stream of frames {u32 len (whole frame), u8 cmd, u8 id, data}, carried in SIPC5 frames of up to
 * 2044 payload bytes. NV read/write go to the phone's private NV copy /var/root/cell-nv.bin (never the EFS
 * partition); file requests: OPEN/INFO -> ENOENT, MKDIR -> 0, CLOSE -> 0, others -> ENOSYS. ---- */
#define RFS_MAX (512*1024)
static uint8_t *rfs_buf;static size_t rfs_have;
static void rfs_put32(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint32_t rfs_get32(const uint8_t *p) {return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
static void rfs_send(uint8_t *r,size_t n,uint8_t cmd,uint8_t id)
{
    rfs_put32(r,(uint32_t)n);r[4]=cmd;r[5]=id;
    for (size_t off=0;off<n;) {
        uint8_t f[2048];size_t c=n-off>2044?2044:n-off;
        f[0]=0xf8;f[1]=245;f[2]=(uint8_t)(c+4);f[3]=(uint8_t)((c+4)>>8);memcpy(f+4,r+off,c);
        for (unsigned t=0;t<200 && sbd_send(3,f,(uint32_t)(c+4));t++) usleep(5000);
        off+=c;
    }
}
static void rfs_ret(uint8_t cmd,uint8_t id,int ret,int err)
{
    uint8_t r[14]={0};rfs_put32(r+6,(uint32_t)ret);rfs_put32(r+10,(uint32_t)(ret<0?err:0));rfs_send(r,14,cmd,id);
}
static void rfs_handle(const uint8_t *q,size_t n)
{
    uint8_t cmd=q[4],id=q[5];
    if (cmd==1 || cmd==2) {
        uint32_t off=n>=14?rfs_get32(q+6):0,len=n>=14?rfs_get32(q+10):0;ssize_t done=-1;
        if (len>RFS_MAX) len=RFS_MAX;
        uint8_t *r=calloc(1,15+len);int fd=open("/var/root/cell-nv.bin",O_RDWR);struct stat st;
        if (r && fd>=0 && !fstat(fd,&st) && (uint64_t)off+len<=(uint64_t)st.st_size) {
            if (cmd==1) done=pread(fd,r+15,len,off);
            else if (n>=14+(size_t)len) {done=pwrite(fd,q+14,len,off);fsync(fd);}
        }
        if (fd>=0) close(fd);
        printf("cellular: RFS NV_%s off 0x%x len %u -> %zd\n",cmd==1?"READ":"WRITE",off,len,done);fflush(stdout);
        if (!r) return;
        r[6]=done==(ssize_t)len;rfs_put32(r+7,off);rfs_put32(r+11,done>0?(uint32_t)done:0);
        rfs_send(r,15+(cmd==1 && done>0?(size_t)done:0),cmd,id);free(r);return;
    }
    char path[200]="";
    if (cmd==0x11 && n>=14) {uint32_t pl=rfs_get32(q+10);if (pl>n-14) pl=(uint32_t)(n-14);if (pl>=sizeof(path)) pl=sizeof(path)-1;memcpy(path,q+14,pl);path[pl]=0;}
    if ((cmd==0x0a||cmd==0x0b||cmd==0x0c||cmd==0x14) && n>=10) {uint32_t pl=rfs_get32(q+6);if (pl>n-10) pl=(uint32_t)(n-10);if (pl>=sizeof(path)) pl=sizeof(path)-1;memcpy(path,q+10,pl);path[pl]=0;}
    for (char *c=path;*c;c++) if (*c<32||*c>126) *c='.';
    printf("cellular: RFS cmd 0x%02x id %u %s\n",cmd,id,path);fflush(stdout);
    switch (cmd) {
    case 0x11: rfs_ret(cmd,id,-1,2);break;                       /* OPEN: ENOENT */
    case 0x06: rfs_ret(cmd,id,0,0);break;                        /* CLOSE */
    case 0x0c: rfs_ret(cmd,id,0,0);break;                        /* MKDIR */
    case 0x0a: case 0x13: {uint8_t r[32]={0};rfs_put32(r+6,(uint32_t)-1);rfs_put32(r+28,2);rfs_send(r,32,cmd,id);break;}
    default: rfs_ret(cmd,id,-1,78);break;                        /* ENOSYS */
    }
}
static void rfs_rx(const uint8_t *p,size_t n)
{
    if (!rfs_buf && !(rfs_buf=malloc(RFS_MAX))) return;
    if (rfs_have+n>RFS_MAX) {printf("cellular: RFS overflow, stream reset\n");rfs_have=0;return;}
    memcpy(rfs_buf+rfs_have,p,n);rfs_have+=n;
    while (rfs_have>=6) {
        uint32_t want=rfs_get32(rfs_buf);
        static uint32_t said;
        if (want!=said) {said=want;printf("cellular: RFS request: %u bytes, cmd 0x%02x id %u (have %zu)\n",want,rfs_buf[4],rfs_buf[5],rfs_have);fflush(stdout);}
        if (want<6 || want>RFS_MAX) {printf("cellular: RFS bad length %u, stream reset\n",want);fflush(stdout);rfs_have=0;return;}
        if (rfs_have<want) return;
        rfs_handle(rfs_buf,want);
        memmove(rfs_buf,rfs_buf+want,rfs_have-want);rfs_have-=want;
    }
}
static void sbd_dump_dl(void)
{
    static int said;
    if (!said++) {      /* what the CP sees: g_desc and the ipc0/dm0 descriptors, as written */
        uint8_t g[48];ipc_read(0x800,g,12);
        printf("cellular live DL: g_desc:");for (unsigned k=0;k<12;k++) printf(" %02x",g[k]);printf("\n");
    }
    for (unsigned id=0;id<SBD_N;id++) {
        uint16_t rp,wp;unsigned len=sbd_chs[id].dl_len;
        if (ipc_load16(sbd_rp(id,1),&rp) || ipc_load16(sbd_wp(id,1),&wp)) return;
        if (rp>=len || wp>=len) continue;      /* (not a valid index: reported by the rp/wp dump) */
        for (unsigned guard=0;rp!=wp && guard<len;guard++) {
            uint32_t n=0;uint8_t b[96];ipc_load(sbd_lenarr[id][1]+4*rp,&n);
            uint8_t big[2100];uint32_t show=sbd_chs[id].dl_size<sizeof(big)?sbd_chs[id].dl_size:sizeof(big);(void)b;(void)n;
            ipc_read(sbd_buf[id][1]+rp*sbd_chs[id].dl_size,big,show);
            uint32_t flen=big[2]|(big[3]<<8);if (big[0]>=0xf8 && flen>=4 && flen<show) show=flen;
            if (sbd_chs[id].ch==235 && show>=11) cell_ipc(big,show);
            if (sbd_chs[id].ch==235 && show>=11 && big[8]==0x0d && big[9]==0x09) pdn_ipconfig(big+11,show-11);
            if (sbd_chs[id].ch==245 && show>4) {rfs_rx(big+4,show-4);show=0;}
            if (sbd_chs[id].ch==25 && show>4) {
                printf("cellular: AT <- ");for (unsigned k=4;k<show;k++) putchar(big[k]=='\r'?' ':big[k]=='\n'?'|':(big[k]>=32&&big[k]<127?big[k]:'.'));printf("\n");fflush(stdout);
                show=0;
            }
            if (sbd_chs[id].ch==235 && show>=11) {
                printf("cellular live DL: IPC <- group %02x index %02x type %02x (mseq %u aseq %u, %u bytes):",
                    big[8],big[9],big[10],big[6],big[7],show-11);
                for (unsigned k=11;k<show;k++) printf(" %02x",big[k]);
                printf("  |");for (unsigned k=11;k<show;k++) putchar(big[k]>=32&&big[k]<127?big[k]:'.');printf("|\n");
            } else if (show && sbd_chs[id].ch==28 && stay_mode) {
                if (dm_on) {          /* printable runs of 6+ characters */
                    char run[256];unsigned rl=0;
                    for (unsigned k=0;k<=show;k++) {
                        int c=k<show?big[k]:0;
                        if (c>=32 && c<127 && rl<sizeof(run)-1) run[rl++]=(char)c;
                        else {if (rl>=6) {run[rl]=0;printf("cellular: CP dm: %s\n",run);}rl=0;}
                    }
                    fflush(stdout);
                }
            } else if (show && (!stay_mode || sbd_chs[id].ch!=28)) {     /* (dm0 = the modem's own debug log) */
                printf("cellular live DL: SBD RX ch %u (%u bytes):",sbd_chs[id].ch,show);for (unsigned k=0;k<show && k<48;k++) printf(" %02x",big[k]);printf("\n");
            }
            rp=(uint16_t)((rp+1)%len);ipc_store16(sbd_rp(id,1),rp);
        }
    }
    fflush(stdout);
}

/* pktproc v2 info (link_rx_pktproc.c pktproc_create, CONFIG_CP_PKTPROC_V2): SHMEM_PKTPROC = cp_rmem_2
 * d0000000+64MB, CP view 0x28000000. info 0x100: {num_queues 1, mode 1 SKTBUF, max_packet 2048} +
 * q_info[0] {desc_base cp+0x100, num_desc (0x3f00000/2560)*25/100, data_base cp+0x100000, fore 0, rear 0} */
#define PP_NDESC 256u
#define PP_CELL 2560u
static int pktproc_info(void)
{
    char r[8192],c[96];
    for (unsigned o=0;o<0x100;o+=4) {snprintf(c,sizeof(c),"w 0x%x 0x0",0xd0000000u+o);if (request(c,r)) return -1;}
    /* the descriptor ring (16 B each at +0x100): every entry but the last a free RX buffer, as
     * pktproc_fill_data_addr(): data_addr = CP data base + cell offset + NET_SKB_PAD(64); HEAD on the first */
    for (unsigned i=0;i<PP_NDESC;i++) {
        uint32_t a=0xd0000100u+16*i,addr=i<PP_NDESC-1?0x28100000u+i*PP_CELL+64u:0,ctl=i==0?0x80u:0;
        snprintf(c,sizeof(c),"w 0x%x 0x%x",a,addr);if (request(c,r)) return -1;
        snprintf(c,sizeof(c),"w 0x%x 0x%x",a+4,ctl<<8);if (request(c,r)) return -1;
        snprintf(c,sizeof(c),"w 0x%x 0x0",a+8);if (request(c,r)) return -1;
        snprintf(c,sizeof(c),"w 0x%x 0x0",a+12);if (request(c,r)) return -1;
    }
    /* info: {num_queues 1, mode 1 SKTBUF, max_packet 2048} + q_info[0] {desc_base, num_desc, data_base, fore, rear} */
    const uint32_t w[]={1u|(1u<<4)|(2048u<<8),0x28000100u,PP_NDESC,0x28100000u,PP_NDESC-1,0};
    for (unsigned i=0;i<6;i++) {snprintf(c,sizeof(c),"w 0x%x 0x%x",0xd0000000u+4*i,w[i]);if (request(c,r)) return -1;}
    printf("cellular live DL: pktproc v2 at d0000000: %u descriptors, %u free RX buffers (fore %u)\n",PP_NDESC,PP_NDESC-1,PP_NDESC-1);fflush(stdout);
    return 0;
}
/* ---- stay mode (--stay): the CP stays up; state for iOS in /var/tmp/redwood-cell/state ---- */
static int stay_mode;
static struct {int rssi_dbm,bars,reg_raw,have_rssi;char plmn[8];uint64_t at;} cell;
static void cell_publish(void)
{
    char tmp[]="/var/tmp/redwood-cell/state.tmp";
    mkdir("/var/tmp/redwood-cell",0755);
    FILE *f=fopen(tmp,"w");if (!f) return;
    const char *op=strncmp(cell.plmn,"4058",4)==0||strncmp(cell.plmn,"40586",5)==0?"Jio":cell.plmn[0]?cell.plmn:"";
    /* ios_reg/ios_data: SystemStatusServer's STTelephonyRegistrationStatus (2 = registered, 3 = notRegistered)
     * and STTelephonyDataConnectionType (7 = lteConnection), as its debug-name functions list them */
    fprintf(f,"data_on %d\n",!data_off);
    fprintf(f,"bars %d\nmax 4\ndbm %d\noperator %s\nplmn %s\nreg %d\nrat LTE\nios_reg %d\nios_data %d\nupdated %llu\n",
        cell.bars,cell.rssi_dbm,op,cell.plmn,cell.reg_raw,(cell.plmn[0] && strcmp(cell.plmn,"000000"))?2:3,7,(unsigned long long)time(NULL));
    fclose(f);chmod(tmp,0644);rename(tmp,"/var/tmp/redwood-cell/state");
}
/* ---- SMS (IPC group 04, from stock libsec-ril IpcProtocol41Sms): 04 07 NOTI device ready -> answer SET 04 07;
 * 04 02 NOTI incoming {msg_type, type, u16 sim_index, tpid, len} + PDU (SMSC first) -> EXEC 04 06 {msg_type, u16 0, tpid, 0} */
static void ctl_line(char *l);
static size_t gsm7_utf8(const uint8_t *ud,unsigned septets,unsigned skip_bits,char *o,size_t cap)
{
    static const char *const t[128]={"@","£","$","¥","è","é","ù","ì","ò","Ç","\n","Ø","ø","\r","Å","å","Δ","_","Φ","Γ","Λ","Ω","Π","Ψ","Σ","Θ","Ξ","","Æ","æ","ß","É",
        " ","!","\"","#","¤","%","&","'","(",")","*","+",",","-",".","/","0","1","2","3","4","5","6","7","8","9",":",";","<","=",">","?",
        "¡","A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","Ä","Ö","Ñ","Ü","§",
        "¿","a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z","ä","ö","ñ","ü","à"};
    size_t n=0;int esc=0;
    for (unsigned k=0;k<septets;k++) {
        unsigned bit=skip_bits+7*k,c=((ud[bit/8]|(ud[bit/8+1]<<8))>>(bit%8))&0x7f;
        const char *s;char one[2]={0};
        if (esc) {esc=0;s=c==0x14?"^":c==0x28?"{":c==0x29?"}":c==0x2f?"\\":c==0x3c?"[":c==0x3d?"~":c==0x3e?"]":c==0x40?"|":c==0x65?"€":" ";}
        else if (c==0x1b) {esc=1;continue;} else s=t[c];
        if (!*s) {one[0]='?';s=one;}
        size_t L=strlen(s);if (n+L+1>cap) break;memcpy(o+n,s,L);n+=L;
    }
    o[n]=0;return n;
}
static size_t ucs2_utf8(const uint8_t *u,unsigned bytes,char *o,size_t cap)
{
    size_t n=0;
    for (unsigned k=0;k+1<bytes;k+=2) {
        unsigned c=(u[k]<<8)|u[k+1];
        if (c>=0xd800 && c<0xdc00 && k+3<bytes) {unsigned c2=(u[k+2]<<8)|u[k+3];c=0x10000+((c-0xd800)<<10)+(c2-0xdc00);k+=2;}
        if (n+5>cap) break;
        if (c<0x80) o[n++]=(char)c;
        else if (c<0x800) {o[n++]=(char)(0xc0|c>>6);o[n++]=(char)(0x80|(c&0x3f));}
        else if (c<0x10000) {o[n++]=(char)(0xe0|c>>12);o[n++]=(char)(0x80|((c>>6)&0x3f));o[n++]=(char)(0x80|(c&0x3f));}
        else {o[n++]=(char)(0xf0|c>>18);o[n++]=(char)(0x80|((c>>12)&0x3f));o[n++]=(char)(0x80|((c>>6)&0x3f));o[n++]=(char)(0x80|(c&0x3f));}
    }
    o[n]=0;return n;
}
static int sms_decode(const uint8_t *p,unsigned n,char *from,size_t fcap,char *text,size_t tcap,char *when,size_t wcap)
{
    unsigned i=0;if (n<1) return -1;i=1+p[0];if (i+2>n) return -1;
    unsigned fo=p[i++];if ((fo&3)!=0) return -1;                 /* SMS-DELIVER only */
    unsigned ol=p[i++],toa=p[i++],ob=(ol+1)/2;if (i+ob+9>n) return -1;
    if ((toa&0x70)==0x50) gsm7_utf8(p+i,ol*4/7,0,from,fcap);
    else {size_t k=0;if ((toa&0x70)==0x10 && k+1<fcap) from[k++]='+';
        for (unsigned d=0;d<ol && k+1<fcap;d++) {unsigned v=(p[i+d/2]>>((d&1)*4))&15;from[k++]=v<10?(char)('0'+v):'?';} from[k]=0;}
    i+=ob;i++;unsigned dcs=p[i++];const uint8_t *ts=p+i;i+=7;
    #define SO(b) (((b)&15)*10+((b)>>4))
    snprintf(when,wcap,"20%02u-%02u-%02u %02u:%02u:%02u",SO(ts[0]),SO(ts[1]),SO(ts[2]),SO(ts[3]),SO(ts[4]),SO(ts[5]));
    unsigned udl=p[i++];const uint8_t *ud=p+i;unsigned left=n-i,udh=(fo&0x40)&&left?ud[0]+1u:0;
    int enc=((dcs&0xc0)==0)?((dcs>>2)&3):((dcs&0xf0)==0xf0?((dcs&4)?1:0):((dcs&0xf0)==0xe0?2:0));  /* 0 gsm7 1 8bit 2 ucs2 */
    if (enc==2) ucs2_utf8(ud+udh,udl>udh?udl-udh:0,text,tcap);
    else if (enc==1) {unsigned L=udl>udh?udl-udh:0;if (L>tcap-1) L=(unsigned)tcap-1;for (unsigned k=0;k<L;k++) text[k]=(char)(ud[udh+k]>=32&&ud[udh+k]<127?ud[udh+k]:'.');text[L]=0;}
    else {unsigned skip=udh*8,fill=(7-(skip%7))%7;unsigned sk=(skip+fill)/7;if ((udl*7+7)/8>left) return -1;gsm7_utf8(ud,udl>sk?udl-sk:0,skip+fill,text,tcap);}
    return 0;
}
static void sms_ipc(unsigned i,const uint8_t *d,unsigned n,unsigned type)
{
    char cmd[64];
    static int ready_done;
    if (i==0x07 && type==3) {if (ready_done++) return;ctl_line((char[]){"fmt 04 07 03"});printf("cellular: SMS: device ready answered\n");fflush(stdout);return;}
    if (i!=0x02 || n<6) return;
    unsigned len=d[5];if (6+len>n) len=n-6;
    char from[64]="?",text[1600]="",when[32]="";
    int ok=!sms_decode(d+6,len,from,sizeof(from),text,sizeof(text),when,sizeof(when));
    printf("cellular: SMS from %s at %s: %s%s\n",from,when,ok?text:"(undecoded PDU)",ok?"":"");fflush(stdout);
    FILE *f=fopen("/var/root/cell-sms.txt","a");
    if (f) {fprintf(f,"%llu\t%s\t%s\t",(unsigned long long)time(NULL),from,when);
        if (ok) {for (char *c=text;*c;c++) fputc(*c=='\n'||*c=='\t'?' ':*c,f);}
        else {fputs("pdu:",f);for (unsigned k=0;k<len;k++) fprintf(f,"%02x",d[6+k]);}
        fputc('\n',f);fclose(f);}
    snprintf(cmd,sizeof(cmd),"fmt 04 06 01 %02x 00 00 %02x 00",d[0],d[4]);ctl_line(cmd);
}
static void cell_ipc(const uint8_t *m,unsigned len)
{
    if (len<11) return;
    unsigned g=m[8],i=m[9];const uint8_t *d=m+11;unsigned n=len-11;int changed=0;
    if (g==0x07 && i==0x06 && n>=1) {               /* DISP_RSSI_INFO: first byte = -dBm */
        int dbm=-(int)d[0],bars=dbm>=-90?4:dbm>=-100?3:dbm>=-108?2:dbm>=-115?1:0;
        if (!cell.have_rssi || dbm!=cell.rssi_dbm) {cell.rssi_dbm=dbm;cell.bars=bars;cell.have_rssi=1;changed=1;}
    }
    if (g==0x08 && i==0x03 && n>=9) {               /* NET_CURRENT_PLMN: "...!" then 6 digits */
        char p[8]={0};for (unsigned k=0;k<6 && 3+k<n;k++) p[k]=(char)d[3+k];
        if (p[0]>='0' && p[0]<='9' && strcmp(p,cell.plmn)) {strcpy(cell.plmn,p);changed=1;}
    }
    if (g==0x08 && i==0x05 && n>=2) {if (cell.reg_raw!=d[1]) {cell.reg_raw=d[1];changed=1;}}
    if (g==0x04) sms_ipc(i,d,n,m[10]);
    if (changed) cell_publish();
}

/* ---- control FIFO (stay mode): /var/tmp/redwood-cell/cmd, one command per line:
 *   at <AT command>                  -> umts_router (ch 25, SBD id 5), "\r" appended
 *   fmt <group> <index> <type> [hex] -> umts_ipc0 Samsung IPC (ch 235, SBD id 1)
 * replies are logged ("AT <-" lines for ch 25, "IPC <-" for ch 235) ---- */

/* ---- packets (A, 2026-11-13): data call cid 2 = rmnet1 = PS ch 11. DL through the packet processor's
 * descriptors (rear_ptr written by the CP), UL as raw IP in the multipdp SBD ring (id 9), buff_len|ch<<16 ---- */
static struct {int up;uint8_t ip6[16],dns6[16];unsigned mtu;} pdn;
static unsigned ul_ch=11,ul_ring=8;     /* QoS high-priority multipdp ring carries UL (ring 9 is consumed but not sent) */
static unsigned pp_done;
static uint64_t pp_rx,pp_tx;
static int poke_read1(uint32_t pa,uint32_t *w,unsigned n);
static int poke_read(uint32_t pa,uint32_t *w,unsigned n)
{   /* the kernel peek returns at most 64 words per call */
    for (unsigned o=0;o<n;o+=64) if (poke_read1(pa+4*o,w+o,n-o>64?64:n-o)) return -1;
    return 0;
}
static int poke_read1(uint32_t pa,uint32_t *w,unsigned n)
{
    char c[64],r[8192];snprintf(c,sizeof(c),"r 0x%x %u",pa,n);if (request(c,r)) return -1;
    unsigned got=0;char *save=NULL;
    for (char *t=strtok_r(r," \n",&save);t && got<n;t=strtok_r(NULL," \n",&save)) {
        size_t L=strlen(t);if (L && t[L-1]==':') continue;          /* "0xADDR:" line prefixes */
        w[got++]=(uint32_t)strtoul(t,NULL,16);
    }
    return got==n?0:-1;
}
static int poke_write(uint32_t pa,uint32_t v) {char c[64],r[8192];snprintf(c,sizeof(c),"w 0x%x 0x%x",pa,v);return request(c,r);}
static void pdn_ipconfig(const uint8_t *d,unsigned n)
{
    if (n<0x9a || d[0]!=2) return;
    memcpy(pdn.ip6,d+0x23,16);memcpy(pdn.dns6,d+0x63,16);pdn.mtu=d[0x98]|(d[0x99]<<8);pdn.up=1;
    printf("cellular: data PDN cid 2 up: IPv6 %02x%02x:%02x%02x:..., DNS %02x%02x:%02x%02x:..., MTU %u\n",
        pdn.ip6[0],pdn.ip6[1],pdn.ip6[2],pdn.ip6[3],pdn.dns6[0],pdn.dns6[1],pdn.dns6[2],pdn.dns6[3],pdn.mtu);fflush(stdout);
}
static void pkt_rx(const uint8_t *p,unsigned n);
static void pp_poll(void)
{
    uint32_t rear=0;if (poke_read(0xd0000014,&rear,1) || rear>=PP_NDESC) return;
    while (pp_done!=rear) {
        uint32_t dw[4];uint32_t a=0xd0000100u+16*pp_done;
        if (poke_read(a,dw,4)) return;
        unsigned len=dw[2]&0xffff,ch=(dw[3]>>16)&0xff;uint32_t addr=dw[0];
        if (len && len<=2048 && addr>=0x28100000u && addr<0x2c000000u) {
            uint32_t words[520];unsigned nw=(len+3)/4;
            if (!poke_read(addr-0x28000000u+0xd0000000u,words,nw)) {pp_rx++;pkt_rx((const uint8_t *)words,len);}
        }
        if (pp_rx<20) {printf("cellular: DL desc %u len %u ch %u status %02x\n",pp_done,len,ch,(dw[1]>>16)&0xff);fflush(stdout);}
        /* hand the buffer back: same address, status cleared, fore advanced */
        uint32_t back=0x28100000u+pp_done*PP_CELL+64u;
        poke_write(a,back);poke_write(a+4,pp_done==0?0x8000u:0);poke_write(a+8,0);poke_write(a+12,0);
        uint32_t fore=0;if (!poke_read(0xd0000010,&fore,1)) poke_write(0xd0000010,(fore+1)%PP_NDESC);
        pp_done=(pp_done+1)%PP_NDESC;
    }
}
static int pkt_tx(const uint8_t *p,unsigned n)
{
    unsigned id=ul_ring;uint16_t rp,wp;unsigned len=sbd_chs[id].ul_len;
    if (ipc_load16(sbd_rp(id,0),&rp) || ipc_load16(sbd_wp(id,0),&wp) || (unsigned)(wp+1)%len==rp) return -1;
    if (n>sbd_chs[id].ul_size || ipc_write(sbd_buf[id][0]+wp*sbd_chs[id].ul_size,p,n) ||
        ipc_store(sbd_lenarr[id][0]+4*wp,n|(ul_ch<<16))) return -1;
    __sync_synchronize();
    if (ipc_store16(sbd_wp(id,0),(uint16_t)((wp+1)%len))) return -1;
    pp_tx++;ipc_store(0x400,0x81);return doorbell(0);
}
static uint16_t csum16(const uint8_t *p,size_t n,uint32_t sum)
{
    for (size_t i=0;i+1<n;i+=2) sum+=(p[i]<<8)|p[i+1];if (n&1) sum+=p[n-1]<<8;
    while (sum>>16) sum=(sum&0xffff)+(sum>>16);return (uint16_t)~sum;
}
static void ping6(const char *dst)
{
    uint8_t ip[16];if (!pdn.up || inet_pton(AF_INET6,dst,ip)!=1) {printf("cellular: ping6: no PDN or bad address\n");return;}
    uint8_t pk[48+16];memset(pk,0,sizeof(pk));
    pk[0]=0x60;pk[4]=0;pk[5]=8+16;pk[6]=58;pk[7]=64;memcpy(pk+8,pdn.ip6,16);memcpy(pk+24,ip,16);
    uint8_t *ic=pk+40;ic[0]=128;ic[4]=0x12;ic[5]=0x34;ic[7]=1;memcpy(ic+8,"redwood-s20-lte!",16);
    uint8_t ph[40];memset(ph,0,sizeof(ph));memcpy(ph,pk+8,32);ph[35]=24;ph[39]=58;
    uint32_t s0=0;for (int i=0;i<40;i+=2) s0+=(ph[i]<<8)|ph[i+1];
    uint16_t c=csum16(ic,24,s0);ic[2]=(uint8_t)(c>>8);ic[3]=(uint8_t)c;
    int e=pkt_tx(pk,64);printf("cellular: ping6 %s sent (%s)\n",dst,e?"UL ring refused":"ok");fflush(stdout);
}
static void utun_deliver(const uint8_t *p,unsigned n);
static void pkt_rx(const uint8_t *p,unsigned n)
{
    utun_deliver(p,n);
    if (n>=40 && (p[0]>>4)==6 && p[6]==58 && n>=48 && p[40]==129)
        {printf("cellular: ping6 REPLY from %02x%02x:%02x%02x:... (%u bytes)\n",p[8],p[9],p[10],p[11],n);fflush(stdout);}
    else if (pp_rx<20) {printf("cellular: DL packet %u bytes, IP v%u next %u\n",n,p[0]>>4,p[6]);fflush(stdout);}
}

/* ---- utun: the data PDN as an iOS network interface (IPv6) ---- */
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_dl.h>
/* (not in the iOS SDK; the kernel ABI is macOS's: sys/kern_control.h, sys/sys_domain.h, net/route.h) */
#define AF_SYSTEM 32
#define PF_SYSTEM AF_SYSTEM
#define AF_SYS_CONTROL 2
#define SYSPROTO_CONTROL 2
struct ctl_info {uint32_t ctl_id;char ctl_name[96];};
#define CTLIOCGINFO _IOWR('N',3,struct ctl_info)
struct sockaddr_ctl {u_char sc_len,sc_family;uint16_t ss_sysaddr;uint32_t sc_id,sc_unit,sc_reserved[5];};
struct rt_metrics {uint32_t rmx_locks,rmx_mtu,rmx_hopcount;int32_t rmx_expire;uint32_t rmx_recvpipe,rmx_sendpipe,rmx_ssthresh,rmx_rtt,rmx_rttvar,rmx_pksent,rmx_state,rmx_filler[3];};
struct rt_msghdr {u_short rtm_msglen;u_char rtm_version,rtm_type;u_short rtm_index;int rtm_flags,rtm_addrs;pid_t rtm_pid;int rtm_seq,rtm_errno,rtm_use;uint32_t rtm_inits;struct rt_metrics rtm_rmx;};
#define RTM_VERSION 5
#define RTM_ADD 0x1
#define RTF_UP 0x1
#define RTF_STATIC 0x800
#define RTA_DST 0x1
#define RTA_GATEWAY 0x2
#define RTA_NETMASK 0x4
#include <netinet/in.h>
#ifndef UTUN_OPT_IFNAME
#define UTUN_OPT_IFNAME 2
#endif
struct rw_in6_addrlifetime {time_t ia6t_expire,ia6t_preferred;uint32_t ia6t_vltime,ia6t_pltime;};
struct rw_in6_aliasreq {char ifra_name[IFNAMSIZ];struct sockaddr_in6 ifra_addr,ifra_dstaddr,ifra_prefixmask;int ifra_flags;struct rw_in6_addrlifetime ifra_lifetime;};
#define RW_SIOCAIFADDR_IN6 _IOW('i',26,struct rw_in6_aliasreq)
static int utun_fd=-1;static char utun_name[IFNAMSIZ];
/* configd: publish the cellular service (State:/Network/Service/redwood-cell/{IPv6,DNS}) so IPMonitor
 * installs the DNS resolver and ranks the interface like any other service. Loaded at runtime. */
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
static void sc_publish(void)
{
    void *sc=dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration",RTLD_NOW);
    if (!sc) {printf("cellular: configd: no SystemConfiguration\n");return;}
    void *(*create)(CFAllocatorRef,CFStringRef,void *,void *)=(void *(*)(CFAllocatorRef,CFStringRef,void *,void *))dlsym(sc,"SCDynamicStoreCreate");
    Boolean (*setm)(void *,CFDictionaryRef,CFArrayRef,CFArrayRef)=(Boolean (*)(void *,CFDictionaryRef,CFArrayRef,CFArrayRef))dlsym(sc,"SCDynamicStoreSetMultiple");
    if (!create || !setm) return;
    static void *store;if (!store) store=create(NULL,CFSTR("redwood-cellulard"),NULL,NULL);
    if (!store) {printf("cellular: configd: no store\n");return;}
    char a[64],d[64];inet_ntop(AF_INET6,pdn.ip6,a,sizeof(a));inet_ntop(AF_INET6,pdn.dns6,d,sizeof(d));
    CFStringRef ifn=CFStringCreateWithCString(NULL,utun_name,kCFStringEncodingUTF8);
    CFStringRef sa=CFStringCreateWithCString(NULL,a,kCFStringEncodingUTF8),sd=CFStringCreateWithCString(NULL,d,kCFStringEncodingUTF8);
    int pl=64;CFNumberRef pn=CFNumberCreate(NULL,kCFNumberIntType,&pl);
    const void *av[]={sa},*pv[]={pn},*dv[]={sd};
    CFArrayRef aa=CFArrayCreate(NULL,av,1,&kCFTypeArrayCallBacks),pa=CFArrayCreate(NULL,pv,1,&kCFTypeArrayCallBacks),da=CFArrayCreate(NULL,dv,1,&kCFTypeArrayCallBacks);
    const void *k6[]={CFSTR("Addresses"),CFSTR("PrefixLength"),CFSTR("InterfaceName"),CFSTR("Router")};
    const void *v6[]={aa,pa,ifn,sa};   /* point-to-point: router = our own address (as VPN services do) */
    CFDictionaryRef ip6=CFDictionaryCreate(NULL,k6,v6,4,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    const void *kd[]={CFSTR("ServerAddresses"),CFSTR("InterfaceName")},*vd[]={da,ifn};
    CFDictionaryRef dns=CFDictionaryCreate(NULL,kd,vd,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    const void *ks[]={CFSTR("State:/Network/Service/redwood-cell/IPv6"),CFSTR("State:/Network/Service/redwood-cell/DNS")},*vs[]={ip6,dns};
    CFDictionaryRef all=CFDictionaryCreate(NULL,ks,vs,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    Boolean ok=setm(store,all,NULL,NULL);
    printf("cellular: configd service redwood-cell %s (IPv6 %s, DNS %s on %s)\n",ok?"published":"REFUSED",a,d,utun_name);fflush(stdout);
    CFRelease(all);CFRelease(dns);CFRelease(ip6);CFRelease(aa);CFRelease(pa);CFRelease(da);CFRelease(pn);CFRelease(sa);CFRelease(sd);CFRelease(ifn);
}
static void utun_up(void)
{
    if (utun_fd>=0 || !pdn.up) return;
    int fd=socket(PF_SYSTEM,SOCK_DGRAM,SYSPROTO_CONTROL);if (fd<0) {perror("cellular: utun socket");return;}
    struct ctl_info ci;memset(&ci,0,sizeof(ci));strlcpy(ci.ctl_name,"com.apple.net.utun_control",sizeof(ci.ctl_name));
    if (ioctl(fd,CTLIOCGINFO,&ci)<0) {perror("cellular: CTLIOCGINFO");close(fd);return;}
    struct sockaddr_ctl sc;memset(&sc,0,sizeof(sc));sc.sc_len=sizeof(sc);sc.sc_family=AF_SYSTEM;sc.ss_sysaddr=AF_SYS_CONTROL;sc.sc_id=ci.ctl_id;sc.sc_unit=0;
    if (connect(fd,(struct sockaddr *)&sc,sizeof(sc))<0) {perror("cellular: utun connect");close(fd);return;}
    socklen_t nl=sizeof(utun_name);getsockopt(fd,SYSPROTO_CONTROL,UTUN_OPT_IFNAME,utun_name,&nl);
    fcntl(fd,F_SETFL,O_NONBLOCK);
    int s6=socket(AF_INET6,SOCK_DGRAM,0);
    struct rw_in6_aliasreq ar;memset(&ar,0,sizeof(ar));strlcpy(ar.ifra_name,utun_name,IFNAMSIZ);
    ar.ifra_addr.sin6_len=sizeof(struct sockaddr_in6);ar.ifra_addr.sin6_family=AF_INET6;memcpy(&ar.ifra_addr.sin6_addr,pdn.ip6,16);
    ar.ifra_prefixmask.sin6_len=sizeof(struct sockaddr_in6);ar.ifra_prefixmask.sin6_family=AF_INET6;memset(&ar.ifra_prefixmask.sin6_addr,0xff,8);
    ar.ifra_lifetime.ia6t_vltime=ar.ifra_lifetime.ia6t_pltime=0xffffffff;
    if (ioctl(s6,RW_SIOCAIFADDR_IN6,&ar)<0) perror("cellular: SIOCAIFADDR_IN6");
    struct ifreq ifr;memset(&ifr,0,sizeof(ifr));strlcpy(ifr.ifr_name,utun_name,IFNAMSIZ);ifr.ifr_mtu=(int)(pdn.mtu?pdn.mtu:1500);
    if (ioctl(s6,SIOCSIFMTU,&ifr)<0) perror("cellular: SIOCSIFMTU");
    memset(&ifr,0,sizeof(ifr));strlcpy(ifr.ifr_name,utun_name,IFNAMSIZ);
    if (!ioctl(s6,SIOCGIFFLAGS,&ifr)) {ifr.ifr_flags|=IFF_UP|IFF_RUNNING;ioctl(s6,SIOCSIFFLAGS,&ifr);}
    close(s6);
    /* IPv6 default route through the interface (fails harmlessly if Wi-Fi already holds one) */
    int rs=socket(PF_ROUTE,SOCK_RAW,0);
    if (rs>=0) {
        struct {struct rt_msghdr h;struct sockaddr_in6 dst;struct sockaddr_dl gw;struct sockaddr_in6 mask;} m;memset(&m,0,sizeof(m));
        m.h.rtm_msglen=sizeof(m);m.h.rtm_version=RTM_VERSION;m.h.rtm_type=RTM_ADD;m.h.rtm_flags=RTF_UP|RTF_STATIC;
        m.h.rtm_addrs=RTA_DST|RTA_GATEWAY|RTA_NETMASK;m.h.rtm_seq=1;
        m.dst.sin6_len=sizeof(m.dst);m.dst.sin6_family=AF_INET6;
        m.gw.sdl_len=sizeof(m.gw);m.gw.sdl_family=AF_LINK;m.gw.sdl_index=(u_short)if_nametoindex(utun_name);
        m.mask.sin6_len=sizeof(m.mask);m.mask.sin6_family=AF_INET6;
        if (write(rs,&m,sizeof(m))<0) perror("cellular: default route");
        close(rs);
    }
    utun_fd=fd;
    printf("cellular: %s up (index %u), IPv6 from the modem, MTU %u\n",utun_name,if_nametoindex(utun_name),pdn.mtu);fflush(stdout);
    sc_publish();
}
static void utun_down(void)
{
    if (utun_fd<0) return;
    close(utun_fd);utun_fd=-1;     /* the interface, its address and route go with the socket */
    void *sc=dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration",RTLD_NOW);
    void *(*create)(CFAllocatorRef,CFStringRef,void *,void *)=sc?(void *(*)(CFAllocatorRef,CFStringRef,void *,void *))dlsym(sc,"SCDynamicStoreCreate"):NULL;
    Boolean (*setm)(void *,CFDictionaryRef,CFArrayRef,CFArrayRef)=sc?(Boolean (*)(void *,CFDictionaryRef,CFArrayRef,CFArrayRef))dlsym(sc,"SCDynamicStoreSetMultiple"):NULL;
    void *st=create?create(NULL,CFSTR("redwood-cellulard-off"),NULL,NULL):NULL;
    if (st && setm) {const void *k[]={CFSTR("State:/Network/Service/redwood-cell/IPv6"),CFSTR("State:/Network/Service/redwood-cell/DNS")};
        CFArrayRef rm=CFArrayCreate(NULL,k,2,&kCFTypeArrayCallBacks);setm(st,NULL,rm,NULL);CFRelease(rm);CFRelease(st);}
    printf("cellular: mobile data off (%s closed, configd service removed)\n",utun_name);fflush(stdout);
}
static void utun_poll(void)
{
    if (utun_fd<0) return;
    uint8_t b[2100];
    for (int k=0;k<32;k++) {
        ssize_t n=read(utun_fd,b,sizeof(b));if (n<=4) break;
        if (pkt_tx(b+4,(unsigned)(n-4))) break;
    }
}
static void utun_deliver(const uint8_t *p,unsigned n)
{
    if (utun_fd<0 || n>2048) return;
    uint8_t b[2100];uint32_t af=htonl((p[0]>>4)==6?AF_INET6:AF_INET);memcpy(b,&af,4);memcpy(b+4,p,n);
    (void)write(utun_fd,b,n+4);
}
static int ctl_fd=-1;static uint8_t ipc_mseq=0x40;
static int sbd_send(unsigned id,const uint8_t *frame,uint32_t n);
static void ctl_line(char *l)
{
    uint8_t f[2048];
    while (*l==' ') l++;
    if (!strncmp(l,"at ",3)) {
        size_t n=strlen(l+3);if (n>1000) return;
        f[0]=0xf8;f[1]=25;memcpy(f+4,l+3,n);f[4+n]='\r';n+=5;f[2]=(uint8_t)n;f[3]=(uint8_t)(n>>8);
        int e=sbd_send(5,f,(uint32_t)n);printf("cellular: AT -> %s %s\n",l+3,e?"(send failed)":"");fflush(stdout);
    } else if (!strncmp(l,"nodata",6)) {
        data_off=1;close(open("/var/root/cell-nodata",O_CREAT|O_WRONLY,0644));utun_down();cell_publish();
    } else if (!strncmp(l,"data",4) && data_off) {
        data_off=0;unlink("/var/root/cell-nodata");cell_publish();
        printf("cellular: mobile data on\n");fflush(stdout);
        if (!pdn.up) ctl_line((char[]){"data"});   /* PDN still up: utun_up() follows in the main loop */
    } else if (!strncmp(l,"data",4)) {
        const char *apn=l[4]==' '?l+5:"jionet";char cmd[1400],hex[1100];size_t al=strlen(apn);if (al>99) return;
        uint8_t d[330];
        #define HEXOF(buf,n) do{for(size_t _k=0;_k<(n);_k++) snprintf(hex+2*_k,3,"%02x",(buf)[_k]);}while(0)
        memset(d,0,sizeof(d));d[0]=1;d[1]=1;d[2]=6;memcpy(d+3,apn,al);d[0x148]=6;HEXOF(d,330);snprintf(cmd,sizeof(cmd),"fmt 0d 14 03 %s",hex);ctl_line(cmd);
        ctl_line("fmt 0d 03 03 010000");
        memset(d,0,sizeof(d));d[0]=1;d[1]=2;d[2]=6;memcpy(d+3,apn,al);HEXOF(d,142);snprintf(cmd,sizeof(cmd),"fmt 0d 01 03 %s",hex);ctl_line(cmd);
        memset(d,0,sizeof(d));d[0]=1;d[1]=2;HEXOF(d,241);snprintf(cmd,sizeof(cmd),"fmt 0d 04 03 %s",hex);ctl_line(cmd);
        printf("cellular: data call requested on APN %s\n",apn);fflush(stdout);
    } else if (!strncmp(l,"dm",2)) {
        dm_on=!dm_on;printf("cellular: modem debug strings %s\n",dm_on?"on":"off");fflush(stdout);
    } else if (!strncmp(l,"pch ",4)) {
        unsigned c2=0,r2=ul_ring;sscanf(l+4,"%u %u",&c2,&r2);if (c2) ul_ch=c2;if (r2==8||r2==9) ul_ring=r2;
        printf("cellular: UL channel %u ring %u\n",ul_ch,ul_ring);fflush(stdout);
    } else if (!strncmp(l,"rs",2)) {
        /* ICMPv6 router solicitation from our address to ff02::2 */
        if (!pdn.up) return;
        uint8_t pk[48];memset(pk,0,sizeof(pk));pk[0]=0x60;pk[5]=8;pk[6]=58;pk[7]=255;memcpy(pk+8,pdn.ip6,16);
        pk[24]=0xff;pk[25]=0x02;pk[39]=2;pk[40]=133;
        uint8_t ph[40];memset(ph,0,sizeof(ph));memcpy(ph,pk+8,32);ph[35]=8;ph[39]=58;
        uint32_t s0=0;for (int i=0;i<40;i+=2) s0+=(ph[i]<<8)|ph[i+1];
        uint16_t c=csum16(pk+40,8,s0);pk[42]=(uint8_t)(c>>8);pk[43]=(uint8_t)c;
        printf("cellular: router solicitation %s\n",pkt_tx(pk,48)?"refused":"sent");fflush(stdout);
    } else if (!strncmp(l,"ping6 ",6)) {
        ping6(l+6);
    } else if (!strncmp(l,"stat",4)) {
        printf("cellular: rings id:ch ulrp/ulwp dlrp/dlwp:");
        for (unsigned i=0;i<SBD_N;i++) {uint16_t a1=0,a2=0,a3=0,a4=0;ipc_load16(sbd_rp(i,0),&a1);ipc_load16(sbd_wp(i,0),&a2);ipc_load16(sbd_rp(i,1),&a3);ipc_load16(sbd_wp(i,1),&a4);
            printf(" %u:%u %u/%u %u/%u",i,sbd_chs[i].ch,a1,a2,a3,a4);}
        uint32_t m=0,u=0;ipc_load(0x404,&m);ipc_load(0x40c,&u);printf(" | cp2ap %x united %x\n",m,u);fflush(stdout);
    } else if (!strncmp(l,"fmt ",4)) {
        unsigned g,i,t;int used=0;
        if (sscanf(l+4,"%x %x %x %n",&g,&i,&t,&used)<3) return;
        size_t d=0;const char *h=l+4+used;
        while (*h && d<1900) {while (*h==' ') h++;unsigned b;if (sscanf(h,"%2x",&b)!=1) break;f[11+d++]=(uint8_t)b;h+=2;}
        size_t n=11+d;f[0]=0xf8;f[1]=235;f[2]=(uint8_t)n;f[3]=(uint8_t)(n>>8);
        f[4]=(uint8_t)(n-4);f[5]=(uint8_t)((n-4)>>8);f[6]=ipc_mseq++;f[7]=0;f[8]=(uint8_t)g;f[9]=(uint8_t)i;f[10]=(uint8_t)t;
        int e=sbd_send(1,f,(uint32_t)n);printf("cellular: IPC -> %02x %02x %02x (%zu data bytes) %s\n",g,i,t,d,e?"(send failed)":"");fflush(stdout);
    }
}
static void ctl_poll(void)
{
    static char buf[4096];static size_t have;
    if (ctl_fd<0) {
        mkdir("/var/tmp/redwood-cell",0755);unlink("/var/tmp/redwood-cell/cmd");
        if (mkfifo("/var/tmp/redwood-cell/cmd",0622)) return;
        chmod("/var/tmp/redwood-cell/cmd",0622);
        ctl_fd=open("/var/tmp/redwood-cell/cmd",O_RDWR|O_NONBLOCK);if (ctl_fd<0) return;
    }
    ssize_t r=read(ctl_fd,buf+have,sizeof(buf)-1-have);if (r<=0) return;have+=(size_t)r;buf[have]=0;
    char *nl;
    while ((nl=strchr(buf,'\n'))) {*nl=0;ctl_line(buf);size_t rest=have-(size_t)(nl+1-buf);memmove(buf,nl+1,rest);have=rest;buf[have]=0;}
    if (have>=sizeof(buf)-1) have=0;
}
static int online_handshake(unsigned seconds)
{
    uint32_t last=0xffffffff,lastu=0xffffffff,msi=0,lastmsi=0xffffffff;int online=0;
    uint64_t deadline=now_us(NULL)+(uint64_t)seconds*1000000;
    while (now_us(NULL)<deadline && !stopped) {
        uint32_t m=0,u=0,in=0;
        if (ipc_load(0x404,&m) || ipc_load(0x40c,&u)) return -1;
        read_word(0x13800830,&msi);inputs(&in);
        if (m!=last || u!=lastu || msi!=lastmsi) {
            if (!stay_mode || m!=last) {printf("cellular live DL: cp2ap_msg %08x cp2ap_united %08x MSI status %08x GPIO %x\n",m,u,msi,in);fflush(stdout);}
            if (msi) {char r[8192],c[64];snprintf(c,sizeof(c),"w 0x13800830 0x%x",msi);request(c,r);}
            static unsigned repeats;
            if (m==last && msi && (m&0xff)==0xc8 && online && repeats<5) {
                repeats++;printf("cellular live DL: PHONE_START repeated (MSI) -> INIT_END again (%u)\n",repeats);ap2cp(0x02);
            }
            if (online && !stay_mode) {uint16_t urp=0,uwp=0;ipc_load16(sbd_rp(1,0),&urp);ipc_load16(sbd_wp(1,0),&uwp);
                printf("cellular live DL: umts_ipc0 UL rp %u wp %u (rp moving = CP read our request)\n",urp,uwp);fflush(stdout);}
            if (m!=last && (m&0x80) && (m&0x40)) {
                unsigned cmd=m&0x3f;
                if (cmd==0x01) {phase("CP: INIT_START -> SBD rings, PIF_INIT_DONE");if (pktproc_info() || sbd_init()) return -1;ipc_store(0x408,(2u<<14)|(1u<<1)|(1u<<5));ap2cp(0x0d);}
                else if (cmd==0x08) {
                    phase("CP: PHONE_START -> IPC link (magic AA, queues zero) and INIT_END");
                    for (uint32_t o=8;o<0x28;o+=4) ipc_store(o,0);
                    ipc_store(4,1);ipc_store(0,0xaa);ap2cp(0x02);online=1;
                }
                else if (cmd==0x07 || cmd==0x09) phase("CP: CRASH");
            }
            last=m;lastu=u;lastmsi=msi;
        }
        if (online && stay_mode) ctl_poll();
        if (online && stay_mode && pdn.up) {if (!data_off) utun_up();pp_poll();utun_poll();}
        if (online && stay_mode && !data_off && !pdn.up && cell.plmn[0] && strcmp(cell.plmn,"000000")) {
            static uint64_t tried;static unsigned tries;struct stat nd;
            if (tries<5 && now_us(NULL)-tried>20000000ull && stat("/var/root/cell-nodata",&nd)) {
                tried=now_us(NULL);tries++;printf("cellular: automatic data call, try %u\n",tries);fflush(stdout);ctl_line((char[]){"data"});
            }
        }
        if (online && stay_mode) {      /* heartbeat: SpringBoard treats a state older than two minutes as no modem */
            static uint64_t beat;
            if (now_us(NULL)-beat>20000000ull) {beat=now_us(NULL);if (cell.plmn[0] || cell.have_rssi) cell_publish();}
        }
        if (online) {
            static int asked;static uint64_t shown;
            static int scanned;
            if (!stay_mode && !scanned && now_us(NULL)+8000000ull>deadline) {
                scanned=1;uint8_t c[2048];uint32_t start=0,prev=0;int in_run=0;
                printf("cellular live DL: scan IPC 0x10000..0x800000 for CP writes:\n");
                for (uint32_t o=0x10000;o<0x800000;o+=sizeof(c)) {
                    int nz=0;if (ipc_read(o,c,sizeof(c))) break;
                    for (unsigned k=0;k<sizeof(c);k++) if (c[k]) {nz=1;break;}
                    if (nz && !in_run) {start=o;in_run=1;
                        printf("   %06x:",o);for (unsigned k=0;k<32;k++) printf(" %02x",c[k]);printf("\n");}
                    if (!nz && in_run) {printf("   ... nonzero %06x..%06x\n",start,o);in_run=0;}
                    prev=o;
                }
                if (in_run) printf("   ... nonzero %06x..%06x\n",start,prev+(uint32_t)sizeof(c));
                fflush(stdout);
            }
            static int rawn;
            if (!stay_mode && rawn<3 && now_us(NULL)-shown>1000000) {
                uint8_t r[256];rawn++;
                if (!ipc_read(0xf00,r,sizeof(r))) {printf("cellular live DL: raw IPC+f00:");for (unsigned k=0;k<sizeof(r);k++) printf("%s%02x",k%32?" ":"\n   ",r[k]);printf("\n");}
                if (!ipc_read(0x400,r,32)) {printf("cellular live DL: raw IPC+400:");for (unsigned k=0;k<32;k++) printf(" %02x",r[k]);printf("\n");}
            }
            if (!stay_mode && now_us(NULL)-shown>1000000) {
                shown=now_us(NULL);uint8_t a[2*2*2*SBD_N];
                if (!ipc_read(sbd_rpwp,a,sizeof(a))) {
                    printf("cellular live DL: rpwp ulrp|ulwp|dlrp|dlwp:");
                    for (unsigned k=0;k<sizeof(a)/2;k++) printf("%s%u",k%SBD_N?",":" | ",a[2*k]|(a[2*k+1]<<8));
                    printf("\n");fflush(stdout);
                }
            }
            sbd_dump_dl();
            if (!asked && (stay_mode || now_us(NULL)+35000000ull<deadline)) {
                /* Samsung IPC on umts_ipc0 (ch 235, SBD id 1): IPC_MISC_ME_VERSION GET (0a 01, type 02), data ff */
                uint8_t f[16]={0xf8,235,0,0, 8,0, 1,0, 0x0a,0x01,0x02, 0xff};
                f[2]=12;asked=1;
                int e=sbd_send(1,f,12);
                printf("cellular live DL: IPC MISC_ME_VERSION GET on umts_ipc0: %s\n",e?"send failed":"sent");fflush(stdout);
            }
            static int asked_sim;
            if (!stay_mode && asked && !asked_sim && now_us(NULL)+25000000ull<deadline) {
                /* IPC_SEC_SIM_STATUS (05 01) GET (02), no data */
                uint8_t f[16]={0xf8,235,11,0, 7,0, 2,0, 0x05,0x01,0x02};asked_sim=1;
                int e=sbd_send(1,f,11);
                printf("cellular live DL: IPC SEC_SIM_STATUS GET: %s\n",e?"send failed":"sent");fflush(stdout);
            }
            static int legacy;
            if (0 && asked && !legacy && now_us(NULL)+12000000ull<deadline) {
                /* the same request on the legacy FMT queue: TX buf 0x1000 (4 KB), head 0x8 tail 0xc; SEND_FMT 0x82 */
                uint8_t f[12]={0xf8,235,12,0, 8,0, 2,0, 0x0a,0x01,0x02, 0xff};uint32_t h=0,t=0;legacy=1;
                ipc_load(0x8,&h);ipc_load(0xc,&t);
                if (h<0x1000 && h+12<=0x1000) {ipc_write(0x1000+h,f,12);__sync_synchronize();ipc_store(0x8,h+12);}
                ipc_store(0x400,0x82);doorbell(0);
                printf("cellular live DL: same request on legacy FMT (head %x tail %x -> %x)\n",h,t,h+12);fflush(stdout);
            }
            if (legacy) {
                static uint64_t last;
                if (now_us(NULL)-last>1000000) {
                    last=now_us(NULL);uint32_t th=0,tt=0,rh=0,rt=0;ipc_load(0x8,&th);ipc_load(0xc,&tt);ipc_load(0x10,&rh);ipc_load(0x14,&rt);
                    printf("cellular live DL: legacy FMT TX head %x tail %x, RX head %x tail %x\n",th,tt,rh,rt);fflush(stdout);
                }
                fmt_dump();
            }
        }
        usleep(1000);
    }
    {
        uint8_t r[0x800];
        if (!ipc_read(0,r,sizeof(r))) {
            printf("cellular live DL: IPC 0..800 at the end (nonzero 32-byte rows):\n");
            for (unsigned o=0;o<sizeof(r);o+=32) {int nz=0;for (unsigned k=0;k<32;k++) nz|=r[o+k];
                if (nz) {printf("   %04x:",o);for (unsigned k=0;k<32;k++) printf(" %02x",r[o+k]);printf("\n");}}
        }
    }
    printf("cellular live DL: handshake %s\n",online?"ONLINE (INIT_END sent)":"no PHONE_START seen");fflush(stdout);
    return online?0:-1;
}

/* --download-all PLAN: lines "STAGE FILE OFFSET SIZE CRC" (CRC 0 = no CRC step); "15" = FIN (START only) */
static const char *download_plan;
static int raw_send_wait(const uint8_t *frame,uint32_t n,unsigned ms)
{
    uint64_t deadline=now_us(NULL)+(uint64_t)ms*1000;
    for (;;) {
        int e=raw_send(frame,n);if (e!=-2) return e;
        if (now_us(NULL)>deadline || stopped) return -2;
        usleep(500);
    }
}
static int udl_ack(unsigned stage,enum s5123_udl_op op,unsigned ms)
{
    uint8_t reply[64];int got=raw_recv(reply,sizeof(reply),ms);
    if (got<=0) {printf("cellular live DL: stage %u op %u: no reply in %u ms\n",stage,op,ms);fflush(stdout);return -1;}
    enum s5123_udl_ack a=s5123_udl_response(reply,(size_t)((got+7)&~7),stage,op);
    if (a!=S5123_UDL_ACK) {
        printf("cellular live DL: stage %u op %u: reply",stage,op);for (int i=0;i<got && i<16;i++) printf(" %02x",reply[i]);
        printf(" -> %s\n",a==S5123_UDL_PEER_FAIL?"CP reports FAIL":"unexpected");fflush(stdout);return -1;
    }
    return 0;
}
static int udl_cmd(unsigned stage,enum s5123_udl_op op,uint32_t crc,unsigned ms)
{
    uint8_t frame[64];size_t n=s5123_udl_command(frame,sizeof(frame),stage,op,crc);
    if (!n || raw_send_wait(frame,(uint32_t)n,ms)) return -1;
    return udl_ack(stage,op,ms);
}
static int udl_image(unsigned stage,const char *path,long offset,uint32_t size)
{
    FILE *f=fopen(path,"rb");if (!f || fseek(f,offset,SEEK_SET)) {if (f) fclose(f);return -1;}
    uint32_t frames=size/S5123_UDL_MSS+(size%S5123_UDL_MSS!=0);uint8_t chunk[S5123_UDL_MSS],frame[S5123_UDL_WIRE_MAX];
    uint64_t start=now_us(NULL);
    for (uint32_t i=1;i<=frames;i++) {
        size_t want=size-(i-1)*S5123_UDL_MSS;if (want>S5123_UDL_MSS) want=S5123_UDL_MSS;
        if (fread(chunk,1,want,f)!=want) {fclose(f);return -1;}
        size_t n=s5123_udl_data(frame,sizeof(frame),stage,size,i,chunk,want);
        if (!n || raw_send_wait(frame,(uint32_t)n,5000)) {
            printf("cellular live DL: stage %u: frame %u/%u not taken (ring full 5 s or I/O)\n",stage,i,frames);fflush(stdout);fclose(f);return -1;
        }
    }
    fclose(f);
    printf("cellular live DL: stage %u: %u frames (%u bytes) queued in %llu ms\n",stage,frames,size,
        (unsigned long long)((now_us(NULL)-start)/1000));fflush(stdout);
    return udl_ack(stage,S5123_UDL_SEND,30000);
}
static int download_stages(void)
{
    FILE *p=fopen(download_plan,"r");if (!p) return -1;
    char line[512];int error=0;
    while (!error && fgets(line,sizeof(line),p)) {
        unsigned stage;char path[400];long offset;unsigned long size,crc;
        if (sscanf(line,"%u",&stage)!=1) continue;
        if (stage==15) {
            error=udl_cmd(15,S5123_UDL_START,0,10000);
            printf("cellular live DL: FIN START %s\n",error?"failed":"ACK: all stages loaded");fflush(stdout);
            break;
        }
        if (sscanf(line,"%u %399s %ld %lu %lx",&stage,path,&offset,&size,&crc)!=5) {error=-1;break;}
        printf("cellular live DL: stage %u: %lu bytes from %s+%ld%s\n",stage,size,path,offset,crc?" (with CRC)":"");fflush(stdout);
        error=udl_cmd(stage,S5123_UDL_START,0,5000) || udl_image(stage,path,offset,(uint32_t)size) ||
            (crc && udl_cmd(stage,S5123_UDL_CRC,(uint32_t)crc,10000)) || udl_cmd(stage,S5123_UDL_DONE,0,10000);
        printf("cellular live DL: stage %u %s\n",stage,error?"FAILED":"done (START, SEND, DONE acknowledged)");fflush(stdout);
    }
    fclose(p);
    if (!error) {
        phase("download complete: Samsung's IPC handshake, 20 s");
        error=online_handshake(stay_mode?(unsigned)(10*365*24*3600):40);
        for (unsigned i=0;i<2;i++) {
            uint32_t in=0,w[10];inputs(&in);for (unsigned k=0;k<10;k++) ipc_load(k*4,&w[k]);
            printf("cellular live DL: +%ums GPIO %x IPC",i*500,in);for (unsigned k=0;k<10;k++) printf(" %08x",w[k]);printf("\n");fflush(stdout);
            usleep(500000);
        }
    }
    return error;
}
static int download_run(void)
{
    char response[8192];
    phase("download: S2MPU grant (HSI2) for the CP shared memory");
    if (request("cp.s2mpu",response)) return -1;
    printf("cellular live DL: %s",response);fflush(stdout);
    if (!strstr(response,"ret 0, d0000000+4000000 ret 0") && !strstr(response,"ret 0x0, d0000000+4000000 ret 0x0")) {
        fprintf(stderr,"cellular live DL: hypervisor refused a grant; no DMA enable\n");return -1;
    }
    phase("download: root port + endpoint memory/bus master on (CP may now read/write its shared memory)");
    if (request("m 0x13800004 0xfffffff9 0x6",response)) return -1;
    printf("cellular live DL: root %s",response);
    /* s51xx_pcie_probe: BAR0 = the board's pci_db_addr (DT 0x14d60000), then memory + bus master */
    if (request("w 0x14ffe010 0x14d60000",response)) return -1;
    printf("cellular live DL: endpoint BAR0 %s",response);
    if (request("m 0x14ffe004 0xfff9 0x6",response)) return -1;
    printf("cellular live DL: endpoint %s",response);fflush(stdout);
    /* exynos_pcie_rc_msi_init for EP_SAMSUNG_MODEM: RC MSI target = shm MSI base (f6200000), INTR0 0xf1;
     * then the endpoint's MSI capability (0x50) to the same address, enabled (pci_enable_msi) */
    {
        uint32_t msi=0;
        if (request("w 0x13800820 0xf6200000",response) || request("w 0x13800824 0x0",response) ||
            request("w 0x13800828 0xf1",response)) return -1;
        if (!read_word(0x14ffe050,&msi)) {
            printf("cellular live DL: EP MSI control %08x (%s-bit)\n",msi,(msi&(1u<<23))?"64":"32");
            request("w 0x14ffe054 0xf6200000",response);
            if (msi&(1u<<23)) {request("w 0x14ffe058 0x0",response);request("w 0x14ffe05c 0x0",response);}
            else request("w 0x14ffe058 0x0",response);
            request("m 0x14ffe050 0xff8effff 0x10000",response);   /* enable, one vector */
            read_word(0x14ffe050,&msi);printf("cellular live DL: EP MSI control now %08x\n",msi);fflush(stdout);
        }
    }
    usleep(200000);
    {
        uint32_t w[10];for (unsigned i=0;i<10;i++) ipc_load(i*4,&w[i]);
        printf("cellular live DL: IPC header before:");for (unsigned i=0;i<10;i++) printf(" %08x",w[i]);printf("\n");fflush(stdout);
    }
    uint8_t frame[S5123_UDL_WIRE_MAX];size_t n;int error=-1;
    n=s5123_udl_command(frame,sizeof(frame),1,S5123_UDL_START,0);
    if (n && !udl_step(1,S5123_UDL_START,frame,n)) {
        uint8_t toc[800];size_t length=0;FILE *f=fopen(download_toc,"rb");
        if (f) {length=fread(toc,1,sizeof(toc),f);fclose(f);}
        if (length==800 && (n=s5123_udl_data(frame,sizeof(frame),1,800,1,toc,800)) && !udl_step(1,S5123_UDL_SEND,frame,n) &&
            (n=s5123_udl_command(frame,sizeof(frame),1,S5123_UDL_DONE,0)) && !udl_step(1,S5123_UDL_DONE,frame,n)) {
            phase("download: TOC stage complete (START, SEND, DONE acknowledged)");error=0;
            if (download_plan) error=download_stages();
        }
    }
    request("m 0x14ffe004 0xfffb 0x0",response);request("m 0x13800004 0xfffffffb 0x0",response);
    phase("download: bus master off again on both ends");
    return error;
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
    if (download_toc && ipc_boot_init()) return -1;
    phase("observe CP input transitions for 4000ms; levels do not prove acceptance");
    uint32_t first,previous,changed=0,rising=0;
    if (inputs(&first)) return -1;previous=first;
    for (unsigned i=0;i<200;i++) {
        uint32_t current;if (delay(context,20000) || inputs(&current)) return -1;
        changed|=previous^current;rising|=~previous&current;previous=current;
    }
    printf("cellular live BOOT: GPIO first %x last %x changed %x rising %x (PHONE_ACTIVE,WAKE,PS_HOLD)\n",
        first,previous,changed,rising);fflush(stdout);
    if (link_requested) {
        if (output(context,S5123_AP_WAKE,1) || delay(context,5000)) return -1;
        int link_error=s5123_link_begin(&m->link);
        printf("cellular live BOOT: PCIe root command %08x header %08x endpoint class %08x header %08x\n",
            m->link.root_command,m->link.root_header,m->link.endpoint_class,m->link.endpoint_header);fflush(stdout);
        printf("cellular live BOOT: PCIe error %d link_state %08x endpoint %08x command %08x (endpoint proof requires L0 and valid ID)\n",
            link_error,m->link.link_status,m->link.endpoint_id,m->link.endpoint_command);fflush(stdout);
        if (link_error) return -1;
        if (download_toc) {
            int dl=download_run();
            printf("cellular live DL: result %d\n",dl);fflush(stdout);
        }
    }
    return 0;
}
static int park(void *context)
{
    struct modem *m=context;cleanup=1;phase("park SPI, then hold reset and power low");
    if (!m->controller_saved) return 0;
    int error=0;
    if (link_requested && s5123_link_park(&m->link)) error=1;
    if (restore_word(0x10960000,0,1)) error=1;
    if (restore_word(0x10960020,0,1)) error=1;
    if (restore_word(0x1096000c,1,1)) error=1;
    return error?-1:0;
}
static int restore(void *context)
{
    struct modem *m=context;cleanup=1;int error=0;phase("restore SPI/clock/pad/AP ownership");
    if (ipc_mapped) {ipc_op(CP_IPC_UNMAP,0,NULL,0,NULL);ipc_mapped=0;}
    if (link_requested && s5123_link_restore(&m->link)) error=1;
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
    if ((argc<4 || argc>8) || strcmp(argv[1],"--reserved-base-verified")) return 2;
    if (argc==5) {if (strcmp(argv[4],"--link")) return 2;link_requested=1;}
    if (argc==6) {if (strcmp(argv[4],"--download")) return 2;link_requested=1;download_toc=argv[5];}
    if (argc==8 && !strcmp(argv[7],"--stay")) {
        char args[1024]="";size_t al=sizeof(args)-1;
        if (!sysctlbyname("kern.bootargs",args,&al,NULL,0) && strstr(args,"cellular=0")) {puts("cellular=0: modem left off");return 0;}
        stay_mode=1;{struct stat nd;data_off=!stat("/var/root/cell-nodata",&nd);}argc=7;
        /* a newer copy in /var/root (root only) runs instead of the image's: updates without a new image */
        if (strcmp(argv[0],"/var/root/redwood-cellulard") && access("/var/root/redwood-cellulard",X_OK)==0) {
            argv[0]="/var/root/redwood-cellulard";execv(argv[0],argv);
        }
        unlink("/var/tmp/redwood-cell/state");    /* (/var/tmp survives a restart: no stale bars before the modem is up) */
        /* right after boot the phone is too busy for the SPI BOOT's byte PIO (the CP's ROM times out):
         * wait until 60 s after boot and a load average under 8, at most 4 minutes */
        for (unsigned waited=0;waited<240;waited+=5) {
            struct timeval boot;size_t bl=sizeof(boot);double load[3]={99,99,99};
            int up=!sysctlbyname("kern.boottime",&boot,&bl,NULL,0)?(int)(time(NULL)-boot.tv_sec):999;
            getloadavg(load,3);
            if (up>=60 && load[0]<8.0) break;
            if (waited==0) {printf("cellular: waiting for the system to settle (up %d s, load %.0f)\n",up,load[0]);fflush(stdout);}
            sleep(5);
        }
    }
    if (argc==7) {if (strcmp(argv[4],"--download-all")) return 2;link_requested=1;download_toc=argv[5];download_plan=argv[6];}
    uint8_t image[131073],info[25];size_t length,info_length;uint32_t fields[5];
    if (read_file(argv[2],image,sizeof(image),&length) || read_file(argv[3],info,sizeof(info),&info_length) ||
        info_length!=24 || memcmp(info,"S512",4)) return 2;
    memcpy(fields,info+4,sizeof(fields));
    if (fields[0]!=1 || fields[1]!=length || fields[3]!=0x40000000 || fields[4]!=1) return 2;
    signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);signal(SIGHUP,stop_signal);if (stay_mode) signal(SIGHUP,SIG_IGN);
    char response[8192];
    if (request("spi8.rx 0",response) || strcmp(response,"spi8: invalid byte count/hex\n")) return 1;
    struct modem m={0};struct s5123_boot_result result={0};
    struct s5123_boot_ops ops={&m,ready,prepare,output,delay,route,send_boot,park,restore,quiescent};
    int error=s5123_boot_once(&ops,&result,image,length,fields[2]);
    for (unsigned attempt=2;stay_mode && !stopped && attempt<=3 && error && result.cleanup_safe && result.phase<=5;attempt++) {
        printf("cellular live BOOT: attempt %u in 15 s (error %d at phase %u)\n",attempt,error,result.phase);fflush(stdout);
        sleep(15);
        memset(&m,0,sizeof(m));memset(&result,0,sizeof(result));cleanup=0;ipc_mapped=0;
        error=s5123_boot_once(&ops,&result,image,length,fields[2]);
    }
    printf("cellular live BOOT: error %d attempted %u power_requested %u phase %u completed %u cleanup_verified %u; acceptance unproven\n",
        error,result.attempted,result.power_requested,result.phase,result.spi_completed,result.cleanup_safe);fflush(stdout);
    return result.attempted && result.cleanup_safe ? (error?1:0):2;
}
#endif
