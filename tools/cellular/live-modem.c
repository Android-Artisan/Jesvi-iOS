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
static int ipc_boot_init(void)
{
    if (ipc_op(CP_IPC_MAP,0,NULL,0,NULL)) {fprintf(stderr,"cellular live DL: IPC map refused\n");return -1;}
    ipc_mapped=1;
    for (uint32_t o=0;o<0x28;o+=4) if (ipc_store(o,0)) return -1;
    for (uint32_t o=0x400;o<0x410;o+=4) if (ipc_store(o,0)) return -1;
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
static int online_handshake(unsigned seconds)
{
    uint32_t last=0xffffffff,lastu=0xffffffff,msi=0,lastmsi=0xffffffff;int online=0;
    uint64_t deadline=now_us(NULL)+(uint64_t)seconds*1000000;
    while (now_us(NULL)<deadline && !stopped) {
        uint32_t m=0,u=0,in=0;
        if (ipc_load(0x404,&m) || ipc_load(0x40c,&u)) return -1;
        read_word(0x13800830,&msi);inputs(&in);
        if (m!=last || u!=lastu || msi!=lastmsi) {
            printf("cellular live DL: cp2ap_msg %08x cp2ap_united %08x MSI status %08x GPIO %x\n",m,u,msi,in);fflush(stdout);
            if (msi) {char r[8192],c[64];snprintf(c,sizeof(c),"w 0x13800830 0x%x",msi);request(c,r);}
            if (m!=last && (m&0x80) && (m&0x40)) {
                unsigned cmd=m&0x3f;
                if (cmd==0x01) {phase("CP: INIT_START -> PIF_INIT_DONE");ap2cp(0x0d);}
                else if (cmd==0x08) {
                    phase("CP: PHONE_START -> IPC link (magic AA, queues zero) and INIT_END");
                    for (uint32_t o=8;o<0x28;o+=4) ipc_store(o,0);
                    ipc_store(4,1);ipc_store(0,0xaa);ap2cp(0x02);online=1;
                }
                else if (cmd==0x07 || cmd==0x09) phase("CP: CRASH");
            }
            last=m;lastu=u;lastmsi=msi;
        }
        if (online) fmt_dump();
        usleep(1000);
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
        error=online_handshake(20);
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
    if ((argc<4 || argc>7) || strcmp(argv[1],"--reserved-base-verified")) return 2;
    if (argc==5) {if (strcmp(argv[4],"--link")) return 2;link_requested=1;}
    if (argc==6) {if (strcmp(argv[4],"--download")) return 2;link_requested=1;download_toc=argv[5];}
    if (argc==7) {if (strcmp(argv[4],"--download-all")) return 2;link_requested=1;download_toc=argv[5];download_plan=argv[6];}
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
