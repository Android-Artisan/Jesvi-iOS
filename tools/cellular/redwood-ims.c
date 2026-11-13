/* MIT. redwood-ims: a small IMS (VoLTE signalling) client for the S20 port: registration on the carrier's
 * P-CSCF with IMS AKA and the IPsec (ESP transport) security agreement of TS 33.203, then it stays and answers.
 * Reads /var/tmp/redwood-cell/ims (interface, IMS address, P-CSCFs, IMSI, IMEI) written by redwood-cellulard and
 * talks to the ISIM through the daemon's command pipe (`apdu HEX` -> /var/tmp/redwood-cell/apdu).
 *
 *   redwood-ims [--pani VALUE] [--pcscf N] [--ua STRING] [--once] [--nullonly] [--udp]
 *   redwood-ims --pfkey          is the kernel's key socket there
 *   redwood-ims --flush          remove our SAs and policies
 *
 * Secrets (RES, CK, IK, the nonce) are never printed. */
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include "sms-pdu.h"
#ifndef IPV6_BOUND_IF
#define IPV6_BOUND_IF 125
#endif

/* ---- PF_KEY v2 as this kernel has it (bsd/net/pfkeyv2.h, bsd/netinet6/ipsec.h of xnu-8796) ---- */
#define PF_KEY_V2 2
#define SADB_ADD 3
#define SADB_FLUSH 9
#define SADB_X_SPDADD 14
#define SADB_X_SPDFLUSH 19
#define SADB_SATYPE_ESP 3
#define SADB_EXT_SA 1
#define SADB_EXT_ADDRESS_SRC 5
#define SADB_EXT_ADDRESS_DST 6
#define SADB_EXT_KEY_AUTH 8
#define SADB_EXT_KEY_ENCRYPT 9
#define SADB_X_EXT_POLICY 18
#define SADB_X_EXT_SA2 19
#define SADB_SASTATE_MATURE 1
#define SADB_AALG_MD5HMAC 1
#define SADB_AALG_SHA1HMAC 2
#define SADB_EALG_NULL 3
#define SADB_X_EALG_AESCBC 12
#define IPSEC_ULPROTO_ANY 255
#define IPSEC_MODE_TRANSPORT 1
#define IPSEC_DIR_INBOUND 1
#define IPSEC_DIR_OUTBOUND 2
#define IPSEC_POLICY_IPSEC 2
#define IPSEC_LEVEL_REQUIRE 2
#define IPSEC_LEVEL_UNIQUE 3
struct sadb_msg {uint8_t version,type,err,satype;uint16_t len,reserved;uint32_t seq,pid;};
struct sadb_sa2k {uint16_t len,exttype;uint32_t spi;uint8_t replay,state,auth,encrypt;uint32_t flags;uint16_t natt_port,natt_interval,natt_offload,natt_src;};
struct sadb_x_sa2 {uint16_t len,exttype;uint8_t mode,r1;uint16_t r2;uint32_t sequence,reqid;};
struct sadb_address {uint16_t len,exttype;uint8_t proto,prefixlen;uint16_t reserved;};
struct sadb_key {uint16_t len,exttype,bits,reserved;};
struct sadb_x_policy {uint16_t len,exttype,type;uint8_t dir,reserved;uint32_t id,reserved2;};
struct sadb_x_ipsecrequest {uint16_t len,proto;uint8_t mode,level;uint16_t reqid;};

static struct {char ifn[16],ip6[64],pcscf[2][64],imsi[16],imei[20],mcc[4],mnc[4],realm[96];int npc;} cfg;
static int load_cfg(void)
{
    FILE *f=fopen("/var/tmp/redwood-cell/ims","r");if (!f) return -1;
    char k[32],v[96];int up=0;
    while (fscanf(f,"%31s %95s",k,v)==2) {
        if (!strcmp(k,"up")) up=atoi(v);
        else if (!strcmp(k,"ifname")) snprintf(cfg.ifn,sizeof(cfg.ifn),"%s",v);
        else if (!strcmp(k,"ip6")) snprintf(cfg.ip6,sizeof(cfg.ip6),"%s",v);
        else if (!strcmp(k,"pcscf") && cfg.npc<2) snprintf(cfg.pcscf[cfg.npc++],64,"%s",v);
        else if (!strcmp(k,"imsi")) snprintf(cfg.imsi,sizeof(cfg.imsi),"%s",v);
        else if (!strcmp(k,"imei")) snprintf(cfg.imei,sizeof(cfg.imei),"%s",v);
    }
    fclose(f);
    if (!up || !cfg.npc || strlen(cfg.imsi)<6) return -1;
    memcpy(cfg.mcc,cfg.imsi,3);memcpy(cfg.mnc,cfg.imsi+3,3);   /* India: 3-digit MNC */
    snprintf(cfg.realm,sizeof(cfg.realm),"ims.mnc%s.mcc%s.3gppnetwork.org",cfg.mnc,cfg.mcc);
    return 0;
}
static void show(const char *m)
{   /* a SIP message with the IMSI, the IMEI, the challenge and the digest masked */
    const char *p=m;size_t L=strlen(cfg.imsi),E=strlen(cfg.imei)>=14?14:0;
    while (*p) {
        if (L && !strncmp(p,cfg.imsi,L)) {printf("%.5s**********",cfg.imsi);p+=L;}
        else if (E && !strncmp(p,cfg.imei,8)) {printf("<imei>");p+=8;while (*p && *p!='>' && *p!='"') p++;}
        else if (!strncmp(p,"nonce=\"",7) || !strncmp(p,"response=\"",10)) {
            const char *q=strchr(p,'"')+1,*e=strchr(q,'"');
            fwrite(p,1,(size_t)(q-p),stdout);if (e>q) printf("<%d chars>",(int)(e-q));p=e?e:q;
        } else putchar(*p++);
    }
}
static const char *opt(int argc,char **argv,const char *k,const char *def)
{for (int i=1;i+1<argc;i++) if (!strcmp(argv[i],k)) return argv[i+1];return def;}
static int flag(int argc,char **argv,const char *k) {for (int i=1;i<argc;i++) if (!strcmp(argv[i],k)) return 1;return 0;}

/* ---- the SIM, through redwood-cellulard ---- */
static int hexval(int c) {return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1;}
static int apdu(const char *hex,uint8_t *out,int cap)
{   /* -> the answer's bytes as the daemon wrote them (the modem's 05 06 payload), or -1 */
    unlink("/var/tmp/redwood-cell/apdu");
    int fd=open("/var/tmp/redwood-cell/cmd",O_WRONLY|O_NONBLOCK);if (fd<0) return -1;
    char line[700];int n=snprintf(line,sizeof(line),"apdu %s\n",hex);
    if (write(fd,line,(size_t)n)!=n) {close(fd);return -1;}
    close(fd);
    for (int t=0;t<150;t++) {
        usleep(20000);
        FILE *f=fopen("/var/tmp/redwood-cell/apdu","r");if (!f) continue;
        char h[1400];int got=0;
        if (fgets(h,sizeof(h),f)) {for (int i=0;h[i] && h[i+1] && got<cap;i+=2) {int a=hexval(h[i]),b=hexval(h[i+1]);if (a<0 || b<0) break;out[got++]=(uint8_t)(a<<4|b);}}
        fclose(f);
        if (got) return got;
    }
    return -1;
}
static int isim_channel;
static int isim_open(void)
{
    uint8_t r[300];int n=apdu("0070000001",r,sizeof(r));if (n<3) return -1;
    int ch=0;for (int i=1;i+1<n;i++) if (r[i]==0x90 && r[i+1]==0x00) {ch=r[i-1];break;}
    if (ch<1 || ch>3) return -1;
    char c[64];snprintf(c,sizeof(c),"%02xa4040407a0000000871004",ch);
    n=apdu(c,r,sizeof(r));if (n<2) return -1;
    isim_channel=ch;return 0;
}
static void isim_close(void)
{uint8_t r[64];char c[32];if (!isim_channel) return;snprintf(c,sizeof(c),"007080%02x00",isim_channel);(void)apdu(c,r,sizeof(r));isim_channel=0;}
static int isim_aka(const uint8_t *rand,const uint8_t *autn,uint8_t *res,int *reslen,uint8_t *ck,uint8_t *ik)
{   /* AUTHENTICATE, 3G context (TS 31.103 7.1.2): DB | L RES | 10 CK | 10 IK.  0 ok, 1 synchronisation failure (DC), -1 */
    char c[160];int o=snprintf(c,sizeof(c),"%02x88008122" "10",isim_channel);
    for (int i=0;i<16;i++) o+=snprintf(c+o,sizeof(c)-(size_t)o,"%02x",rand[i]);
    o+=snprintf(c+o,sizeof(c)-(size_t)o,"10");
    for (int i=0;i<16;i++) o+=snprintf(c+o,sizeof(c)-(size_t)o,"%02x",autn[i]);
    snprintf(c+o,sizeof(c)-(size_t)o,"00");
    uint8_t r[300];int n=apdu(c,r,sizeof(r));if (n<2) return -1;
    for (int pass=0;pass<2;pass++) {
        for (int i=0;i+3<n;i++) {
            if (r[i]==0xdb && r[i+1]>=4 && r[i+1]<=16 && i+2+r[i+1]+17+17<=n && r[i+2+r[i+1]]==0x10 && r[i+2+r[i+1]+17]==0x10) {
                *reslen=r[i+1];memcpy(res,r+i+2,(size_t)*reslen);memcpy(ck,r+i+3+*reslen,16);memcpy(ik,r+i+4+*reslen+16,16);
                memset(r,0,sizeof(r));return 0;
            }
            if (r[i]==0xdc && r[i+1]==0x0e) return 1;
        }
        if (pass) break;
        /* 61 xx: more to fetch */
        int le=-1;for (int i=0;i+1<n;i++) if (r[i]==0x61 && i+2>=n-1) {le=r[i+1];break;}
        if (le<0) break;
        snprintf(c,sizeof(c),"%02xc00000%02x",isim_channel,le);n=apdu(c,r,sizeof(r));if (n<2) break;
    }
    printf("AKA: the SIM's answer is %d bytes and has neither DB nor DC (last two %02x %02x)\n",n,n>=2?r[n-2]:0,n>=1?r[n-1]:0);
    return -1;
}

/* ---- IPsec ---- */
static int pfk=-1;static uint32_t pfseq;
static int pf_send(uint8_t type,uint8_t satype,const uint8_t *ext,size_t n)
{
    uint8_t b[1024];struct sadb_msg *m=(struct sadb_msg *)b;memset(m,0,sizeof(*m));
    m->version=PF_KEY_V2;m->type=type;m->satype=satype;m->len=(uint16_t)((sizeof(*m)+n)/8);m->seq=++pfseq;m->pid=(uint32_t)getpid();
    if (n) memcpy(b+sizeof(*m),ext,n);
    if (write(pfk,b,sizeof(*m)+n)<0) return errno;
    for (int k=0;k<20;k++) {
        struct pollfd p={pfk,POLLIN,0};if (poll(&p,1,1000)<=0) return ETIMEDOUT;
        uint8_t r[2048];ssize_t g=read(pfk,r,sizeof(r));if (g<(ssize_t)sizeof(*m)) return EIO;
        struct sadb_msg *a=(struct sadb_msg *)r;if (a->seq==pfseq && a->pid==(uint32_t)getpid()) return a->err;
    }
    return ETIMEDOUT;
}
static size_t put_addr(uint8_t *b,uint16_t exttype,const struct in6_addr *a,int port)
{
    struct sadb_address *x=(struct sadb_address *)b;memset(b,0,40);x->len=5;x->exttype=exttype;x->proto=IPSEC_ULPROTO_ANY;x->prefixlen=128;
    struct sockaddr_in6 *s=(struct sockaddr_in6 *)(b+8);s->sin6_len=sizeof(*s);s->sin6_family=AF_INET6;s->sin6_port=htons((uint16_t)port);s->sin6_addr=*a;
    return 40;
}
static int sa_add(const struct in6_addr *src,const struct in6_addr *dst,uint32_t spi,uint32_t reqid,int aalg,const uint8_t *ak,int akl,int ealg,const uint8_t *ek,int ekl)
{
    uint8_t b[512];size_t o=0;
    struct sadb_sa2k *sa=(struct sadb_sa2k *)(b+o);memset(sa,0,sizeof(*sa));sa->len=sizeof(*sa)/8;sa->exttype=SADB_EXT_SA;sa->spi=htonl(spi);
    sa->replay=4;sa->state=SADB_SASTATE_MATURE;sa->auth=(uint8_t)aalg;sa->encrypt=(uint8_t)ealg;o+=sizeof(*sa);
    struct sadb_x_sa2 *s2=(struct sadb_x_sa2 *)(b+o);memset(s2,0,sizeof(*s2));s2->len=sizeof(*s2)/8;s2->exttype=SADB_X_EXT_SA2;s2->mode=IPSEC_MODE_TRANSPORT;s2->reqid=reqid;o+=sizeof(*s2);
    o+=put_addr(b+o,SADB_EXT_ADDRESS_SRC,src,0);o+=put_addr(b+o,SADB_EXT_ADDRESS_DST,dst,0);
    for (int k=0;k<2;k++) {
        const uint8_t *key=k?ek:ak;int kl=k?ekl:akl;if (!kl) continue;
        size_t L=(sizeof(struct sadb_key)+(size_t)kl+7)&~7u;struct sadb_key *x=(struct sadb_key *)(b+o);memset(x,0,L);
        x->len=(uint16_t)(L/8);x->exttype=k?SADB_EXT_KEY_ENCRYPT:SADB_EXT_KEY_AUTH;x->bits=(uint16_t)(kl*8);memcpy(b+o+sizeof(*x),key,(size_t)kl);o+=L;
    }
    int e=pf_send(SADB_ADD,SADB_SATYPE_ESP,b,o);memset(b,0,sizeof(b));return e;
}
static int sp_add(const struct in6_addr *src,int sport,const struct in6_addr *dst,int dport,int dir,uint32_t reqid)
{
    uint8_t b[256];size_t o=0;
    o+=put_addr(b+o,SADB_EXT_ADDRESS_SRC,src,sport);o+=put_addr(b+o,SADB_EXT_ADDRESS_DST,dst,dport);
    struct sadb_x_policy *p=(struct sadb_x_policy *)(b+o);memset(p,0,24);p->len=3;p->exttype=SADB_X_EXT_POLICY;p->type=IPSEC_POLICY_IPSEC;p->dir=(uint8_t)dir;
    struct sadb_x_ipsecrequest *q=(struct sadb_x_ipsecrequest *)(b+o+16);q->len=8;q->proto=IPPROTO_ESP;q->mode=IPSEC_MODE_TRANSPORT;
    q->level=reqid?IPSEC_LEVEL_UNIQUE:IPSEC_LEVEL_REQUIRE;q->reqid=(uint16_t)reqid;o+=24;
    return pf_send(SADB_X_SPDADD,0,b,o);
}
static void ipsec_flush(void) {(void)pf_send(SADB_X_SPDFLUSH,0,NULL,0);(void)pf_send(SADB_FLUSH,SADB_SATYPE_ESP,NULL,0);}

/* ---- small helpers ---- */
static int b64(const char *s,size_t n,uint8_t *o,int cap)
{
    int v=0,bits=0,g=0;
    for (size_t i=0;i<n;i++) {
        int c=s[i],d=c>='A' && c<='Z'?c-'A':c>='a' && c<='z'?c-'a'+26:c>='0' && c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;
        if (d<0) continue;
        v=v<<6|d;bits+=6;if (bits>=8) {bits-=8;if (g<cap) o[g++]=(uint8_t)(v>>bits);}
    }
    return g;
}
static void md5hex(const void *d,size_t n,char *hex)
{unsigned char h[16];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
 CC_MD5(d,(CC_LONG)n,h);
#pragma clang diagnostic pop
 for (int i=0;i<16;i++) sprintf(hex+2*i,"%02x",h[i]);}
static int bound(int type,int port)
{
    unsigned idx=if_nametoindex(cfg.ifn);int s=socket(AF_INET6,type,0);int one=1;if (s<0) return -1;
    setsockopt(s,IPPROTO_IPV6,IPV6_BOUND_IF,&idx,sizeof(idx));setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));setsockopt(s,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one));
    struct sockaddr_in6 l={.sin6_len=sizeof(l),.sin6_family=AF_INET6,.sin6_port=htons((uint16_t)port)};inet_pton(AF_INET6,cfg.ip6,&l.sin6_addr);
    if (bind(s,(struct sockaddr *)&l,sizeof(l))) {close(s);return -1;}
    return s;
}
static const char *hdr(const char *msg,const char *name,char *out,size_t cap)
{   /* one header's value, folded lines joined */
    size_t L=strlen(name);out[0]=0;
    for (const char *p=msg;p && *p;) {
        if (!strncasecmp(p,name,L) && p[L]==':') {
            p+=L+1;size_t o=0;
            for (;;) {
                while (*p==' ' || *p=='\t') p++;
                const char *e=strstr(p,"\r\n");if (!e) e=p+strlen(p);
                if (o && o+1<cap) out[o++]=' ';
                size_t n=(size_t)(e-p);if (o+n>=cap) n=cap-1-o;memcpy(out+o,p,n);o+=n;out[o]=0;
                if (e[0] && (e[2]==' ' || e[2]=='\t')) p=e+2; else break;
            }
            return out;
        }
        p=strstr(p,"\r\n");if (p) p+=2;
    }
    return NULL;
}
static int param_u(const char *v,const char *k,uint32_t *out)
{   /* ";k=NUMBER" in a header value */
    size_t L=strlen(k);
    for (const char *p=v;(p=strstr(p,k));p+=L) {
        if ((p==v || p[-1]==';' || p[-1]==' ') && p[L]=='=') {*out=(uint32_t)strtoul(p+L+1,NULL,10);return 0;}
    }
    return -1;
}
static int recv_msg(int s,char *b,size_t cap,int secs)
{   /* one SIP message from a stream (headers + Content-Length), or a datagram */
    size_t n=0;b[0]=0;
    for (;;) {
        struct pollfd p={s,POLLIN,0};if (poll(&p,1,secs*1000)<=0) return n?(int)n:-1;
        ssize_t r=recv(s,b+n,cap-1-n,0);if (r<=0) return n?(int)n:-1;
        n+=(size_t)r;b[n]=0;
        char *e=strstr(b,"\r\n\r\n");if (!e) continue;
        char cl[32];int body=hdr(b,"Content-Length",cl,sizeof(cl))?atoi(cl):hdr(b,"l",cl,sizeof(cl))?atoi(cl):0;
        if (n>=(size_t)(e+4-b)+(size_t)body) return (int)n;
    }
}

/* ---- SMS over IP: MESSAGE with application/vnd.3gpp.sms carries an RP message (TS 24.011): network -> UE RP-DATA
 * {0x01, ref, originator (SMSC) address, empty destination, user data len, TPDU SMS-DELIVER}; we answer the MESSAGE 200
 * and send RP-ACK {0x02, ref, 0x41, 2, SMS-DELIVER-REPORT 00 00} in a MESSAGE of our own back to the sender URI. */
static void sms_job_result(unsigned ref,int ok);
static char sms_route[1600],sms_impu[256],sms_sc_uri[128];static uint8_t sms_sc[12];static unsigned sms_scl,sms_mr,sms_rpref=1;
static void sms_keep_registration(const char *ok)
{   /* Service-Route(s) -> Route lines for our own requests; first P-Associated-URI as our identity */
    size_t o=0;const char *q=ok;sms_route[0]=sms_impu[0]=0;
    while ((q=strstr(q,"\r\nService-Route:")) && o<sizeof(sms_route)-8) {
        const char *v=q+16,*e=strstr(v,"\r\n");while (*v==' ') v++;
        o+=(size_t)snprintf(sms_route+o,sizeof(sms_route)-o,"Route: %.*s\r\n",(int)(e-v),v);q=e;
    }
    const char *a=strstr(ok,"\r\nP-Associated-URI:");
    if (a) {const char *b=strchr(a,'<'),*e=b?strchr(b,'>'):NULL;if (b && e && e-b<(long)sizeof(sms_impu)) snprintf(sms_impu,sizeof(sms_impu),"%.*s",(int)(e-b-1),b+1);}
    printf("SMS: %s Route line(s), identity %s\n",o?"with":"no",sms_impu[0]?"known":"unknown");
}
static void sms_to_inbox(const char *from,unsigned ref,const char *text)
{   /* for the Messages app: /var/tmp/redwood-cell/sms-in/<time>-<ref>.txt = sender, unix time, text (mobile, 0600) */
    mkdir("/var/tmp/redwood-cell/sms-in",0700);chown("/var/tmp/redwood-cell/sms-in",501,501);
    char fn[160],tmp[170];static unsigned seq;snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-in/%lld-%u-%u.txt",(long long)time(NULL),ref,seq++);
    snprintf(tmp,sizeof(tmp),"%s.tmp",fn);
    FILE *q=fopen(tmp,"w");
    if (q) {fchmod(fileno(q),0600);fprintf(q,"%s\n%lld\n%s",from,(long long)time(NULL),text);fclose(q);chown(tmp,501,501);rename(tmp,fn);}
}
/* concatenated texts (TS 23.040 UDH IEI 00 / 08): parts held per sender+reference until complete or 5 minutes old */
static struct {char from[64];unsigned ref,total;time_t first;char *part[32];} sms_cat[8];
static int sms_concat_info(const uint8_t *pdu,unsigned n,unsigned *ref,unsigned *total,unsigned *seq)
{
    unsigned i=1+pdu[0];if (i>=n) return 0;unsigned fo=pdu[i++];if (!(fo&0x40)) return 0;
    if (i+2>n) return 0;unsigned ol=pdu[i];i+=2+(ol+1)/2;i+=2+7;if (i+2>n) return 0;
    i++;const uint8_t *ud=pdu+i;unsigned udhl=ud[0];if (i+1+udhl>n) return 0;
    for (unsigned k=1;k+1<=udhl;) {
        unsigned iei=ud[k],l=ud[k+1];if (k+2+l>udhl+1) break;
        if (iei==0 && l==3) {*ref=ud[k+2];*total=ud[k+3];*seq=ud[k+4];return *total>1 && *seq>=1 && *seq<=*total && *total<=32;}
        if (iei==8 && l==4) {*ref=(ud[k+2]<<8)|ud[k+3];*total=ud[k+4];*seq=ud[k+5];return *total>1 && *seq>=1 && *seq<=*total && *total<=32;}
        k+=2+l;
    }
    return 0;
}
static void sms_cat_flush(int i,int complete)
{
    char all[6000];size_t o=0;all[0]=0;
    for (unsigned k=0;k<sms_cat[i].total;k++) {
        const char *t=sms_cat[i].part[k]?sms_cat[i].part[k]:"…";
        size_t L=strlen(t);if (o+L+1<sizeof(all)) {memcpy(all+o,t,L);o+=L;all[o]=0;}
        free(sms_cat[i].part[k]);sms_cat[i].part[k]=NULL;
    }
    printf("SMS: %u-part text from …%s joined%s\n",sms_cat[i].total,strlen(sms_cat[i].from)>4?sms_cat[i].from+strlen(sms_cat[i].from)-4:sms_cat[i].from,complete?"":" (parts missing after 5 min)");fflush(stdout);
    sms_to_inbox(sms_cat[i].from,sms_cat[i].ref,all);
    sms_cat[i].from[0]=0;
}
static void sms_cat_tick(void)
{
    for (int i=0;i<8;i++) if (sms_cat[i].from[0] && time(NULL)-sms_cat[i].first>300) sms_cat_flush(i,0);
}
static void sms_cat_add(const char *from,unsigned ref,unsigned total,unsigned seq,const char *text)
{
    int i=-1,freei=-1;
    for (int k=0;k<8;k++) {if (sms_cat[k].from[0] && sms_cat[k].ref==ref && sms_cat[k].total==total && !strcmp(sms_cat[k].from,from)) {i=k;break;} if (!sms_cat[k].from[0] && freei<0) freei=k;}
    if (i<0) {
        if (freei<0) {freei=0;for (int k=1;k<8;k++) if (sms_cat[k].first<sms_cat[freei].first) freei=k;sms_cat_flush(freei,0);}
        i=freei;snprintf(sms_cat[i].from,sizeof(sms_cat[i].from),"%s",from);sms_cat[i].ref=ref;sms_cat[i].total=total;sms_cat[i].first=time(NULL);
    }
    if (!sms_cat[i].part[seq-1]) sms_cat[i].part[seq-1]=strdup(text);
    for (unsigned k=0;k<total;k++) if (!sms_cat[i].part[k]) return;
    sms_cat_flush(i,1);
}
static int sms_incoming(int sock,const char *req,size_t len,int portc)
{
    char ct[120],cl[24],fr[300],ci[200];
    if (strncmp(req,"MESSAGE ",8) || !hdr(req,"Content-Type",ct,sizeof(ct)) || !strstr(ct,"vnd.3gpp.sms")) return 0;
    const char *hb=strstr(req,"\r\n\r\n");if (!hb) return 0;
    const uint8_t *b=(const uint8_t *)hb+4;size_t n=hdr(req,"Content-Length",cl,sizeof(cl))?(size_t)atoi(cl):0;
    if ((size_t)(hb+4-req)+n>len || n<2) {printf("SMS: body short\n");return 1;}
    if (b[0]==0x03 || b[0]==0x05) {   /* the result of one of our sends: tell the Messages app if it was its */
        sms_job_result(b[1],b[0]==3);
    }
    if (b[0]==0x03) {printf("SMS: sent -- the SMSC confirmed (RP-ACK, ref %u)\n",b[1]);fflush(stdout);return 1;}
    if (b[0]==0x05) {printf("SMS: NOT sent -- RP-ERROR ref %u, cause %u\n",b[1],n>3?b[3]&0x7f:0);fflush(stdout);return 1;}
    if (b[0]!=0x01) {printf("SMS: RP message type %u from the network, %zu bytes\n",b[0],n);return 1;}
    unsigned ref=b[1],i=2;
    if (i>=n) return 1;
    unsigned ol=b[i];if (ol>=2 && ol<=sizeof(sms_sc) && i+1+ol<=n) {memcpy(sms_sc,b+i+1,ol);sms_scl=ol;}
    i+=1+ol;if (i>=n) return 1;
    unsigned dl=b[i];i+=1+dl;if (i>=n) return 1;
    unsigned ul=b[i++];
    char from[64]="?",text[1600]="",when[32]="";uint8_t pdu[260];int ok=0;
    if (i+ul<=n && ul<sizeof(pdu)-1) {pdu[0]=0;memcpy(pdu+1,b+i,ul);ok=!sms_decode(pdu,ul+1,from,sizeof(from),text,sizeof(text),when,sizeof(when));}
    printf("SMS (IMS) from %s at %s: %s\n",from,when,ok?text:"(undecoded TPDU)");fflush(stdout);
    if (ok) {
        unsigned cr=0,ct=0,cs=0;
        if (sms_concat_info(pdu,ul+1,&cr,&ct,&cs)) sms_cat_add(from,cr,ct,cs,text);
        else sms_to_inbox(from,ref,text);
    }
    FILE *f=fopen("/var/root/cell-sms.txt","a");
    if (f) {fchmod(fileno(f),0600);fprintf(f,"%llu\t%s\t%s\t",(unsigned long long)time(NULL),from,when);
        if (ok) {for (char *c=text;*c;c++) fputc(*c=='\n'||*c=='\t'?' ':*c,f);} else {fputs("tpdu:",f);for (unsigned k=0;k<ul && i+k<n;k++) fprintf(f,"%02x",b[i+k]);}
        fputc('\n',f);fclose(f);}
    /* RP-ACK back to the sender (the SMSC's URI in From) */
    if (!hdr(req,"From",fr,sizeof(fr)) || !hdr(req,"Call-ID",ci,sizeof(ci))) return 1;
    const char *u1=strchr(fr,'<'),*u2=u1?strchr(u1,'>'):NULL;if (!u1 || !u2) return 1;
    if (u2-u1-1<(long)sizeof(sms_sc_uri)) snprintf(sms_sc_uri,sizeof(sms_sc_uri),"%.*s",(int)(u2-u1-1),u1+1);
    {FILE *q=fopen("/var/root/cell-smsc","w");if (q) {fchmod(fileno(q),0600);fprintf(q,"%s\n",sms_sc_uri);for (unsigned k=0;k<sms_scl;k++) fprintf(q,"%02x",sms_sc[k]);fputc('\n',q);fclose(q);}}
    static const uint8_t ackbody_t[]={0x02,0,0x41,0x02,0x00,0x00};uint8_t ack[sizeof(ackbody_t)];memcpy(ack,ackbody_t,sizeof(ack));ack[1]=(uint8_t)ref;
    char m[4000];int o=snprintf(m,sizeof(m),
        "MESSAGE %.*s SIP/2.0\r\n"
        "Via: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "Max-Forwards: 70\r\n"
        "%s"
        "From: <%s>;tag=%08x\r\n"
        "To: <%.*s>\r\n"
        "Call-ID: %08x@[%s]\r\n"
        "CSeq: 1 MESSAGE\r\n"
        "In-Reply-To: %s\r\n"
        "Request-Disposition: no-fork\r\n"
        "Accept-Contact: *;+g.3gpp.smsip\r\n"
        "Content-Type: application/vnd.3gpp.sms\r\n"
        "Content-Length: %zu\r\n\r\n",
        (int)(u2-u1-1),u1+1,cfg.ip6,portc,arc4random(),sms_route,sms_impu[0]?sms_impu:"sip:anonymous@anonymous.invalid",arc4random(),
        (int)(u2-u1-1),u1+1,arc4random(),cfg.ip6,ci,sizeof(ack));
    if (o<0 || (size_t)o+sizeof(ack)>sizeof(m)) return 1;
    memcpy(m+o,ack,sizeof(ack));
    ssize_t w=send(sock,m,(size_t)o+sizeof(ack),0);
    printf("--> RP-ACK (ref %u) %s\n",ref,w>0?"sent":"NOT sent");fflush(stdout);
    return 1;
}

/* text -> parts: GSM 7-bit when every character is in the basic table's ASCII-equal part (160 a text, 153 a part
 * with the concatenation header), else UCS-2 (70 / 67 UTF-16 units, a surrogate pair never split) */
static int sms_is_gsm(const char *t)
{
    static const char ok7[]=" !\"#%&'()*+,-./0123456789:;<=>?ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\n\r";
    for (const char *c=t;*c;c++) if (!strchr(ok7,*c)) return 0;
    return 1;
}
static int sms_utf16(const char *t,uint16_t *u,int cap)
{
    int o=0;const unsigned char *p=(const unsigned char *)t;
    while (*p) {
        unsigned c;
        if (*p<0x80) c=*p++;
        else if ((*p&0xe0)==0xc0 && p[1]) {c=((p[0]&0x1fu)<<6)|(p[1]&0x3fu);p+=2;}
        else if ((*p&0xf0)==0xe0 && p[1] && p[2]) {c=((p[0]&0x0fu)<<12)|((p[1]&0x3fu)<<6)|(p[2]&0x3fu);p+=3;}
        else if ((*p&0xf8)==0xf0 && p[1] && p[2] && p[3]) {c=((p[0]&0x07u)<<18)|((p[1]&0x3fu)<<12)|((p[2]&0x3fu)<<6)|(p[3]&0x3fu);p+=4;}
        else return -1;
        if (c>=0x10000) {if (o+2>cap) return -1;c-=0x10000;u[o++]=(uint16_t)(0xd800+(c>>10));u[o++]=(uint16_t)(0xdc00+(c&0x3ff));}
        else {if (o+1>cap) return -1;u[o++]=(uint16_t)c;}
    }
    return o;
}
/* the Messages app's pending sends: every part's RP ref points at a job; the job answers when all parts did */
static struct {char id[80];int left,failed;} sms_job[32];
static unsigned char sms_ref_job[256];   /* RP ref -> job index + 1 */
static void sms_job_result(unsigned ref,int ok)
{
    unsigned j=sms_ref_job[ref&0xff];if (!j) return;
    sms_ref_job[ref&0xff]=0;j--;
    if (!ok) sms_job[j].failed=1;
    if (--sms_job[j].left>0) return;
    char fn[200];snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-out/%s.%s",sms_job[j].id,sms_job[j].failed?"fail":"ok");
    FILE *q=fopen(fn,"w");if (q) {fclose(q);chown(fn,501,501);}
    sms_job[j].id[0]=0;
}
static int sms_send_tpdu(int sock,int portc,const uint8_t *tp,unsigned t)
{   /* one TPDU in an RP-DATA to the SMSC: the RP ref, or -1 */
    uint8_t rp[260];unsigned r=0;unsigned ref=sms_rpref++&0xff;
    rp[r++]=0x00;rp[r++]=(uint8_t)ref;rp[r++]=0x00;                  /* RP-DATA MS->network, ref, no originator */
    rp[r++]=(uint8_t)sms_scl;memcpy(rp+r,sms_sc,sms_scl);r+=sms_scl;  /* destination: the SMSC */
    rp[r++]=(uint8_t)t;memcpy(rp+r,tp,t);r+=t;
    char m[4000];int o=snprintf(m,sizeof(m),
        "MESSAGE %s SIP/2.0\r\n"
        "Via: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "Max-Forwards: 70\r\n"
        "%s"
        "From: <%s>;tag=%08x\r\n"
        "To: <%s>\r\n"
        "Call-ID: %08x@[%s]\r\n"
        "CSeq: 1 MESSAGE\r\n"
        "Request-Disposition: no-fork\r\n"
        "Accept-Contact: *;+g.3gpp.smsip\r\n"
        "Content-Type: application/vnd.3gpp.sms\r\n"
        "Content-Length: %u\r\n\r\n",
        sms_sc_uri,cfg.ip6,portc,arc4random(),sms_route,sms_impu[0]?sms_impu:"sip:anonymous@anonymous.invalid",arc4random(),
        sms_sc_uri,arc4random(),cfg.ip6,r);
    if (o<0 || (size_t)o+r>sizeof(m)) return -1;
    memcpy(m+o,rp,r);
    return send(sock,m,(size_t)o+r,0)>0?(int)ref:-1;
}
static void sms_smsc_load(void)
{   /* the SMSC learnt from an earlier incoming text, else Jio's (seen as the sender of every incoming text) */
    if (sms_scl && sms_sc_uri[0]) return;
    char uri[128]="tel:+917012075009",hex[64]="";
    FILE *q=fopen("/var/root/cell-smsc","r");
    if (q) {if (fscanf(q,"%127s %63s",uri,hex)<1) uri[0]=0;fclose(q);}
    if (!uri[0]) return;
    snprintf(sms_sc_uri,sizeof(sms_sc_uri),"%s",uri);
    size_t hl=strlen(hex);sms_scl=0;
    for (size_t k=0;k+1<hl && sms_scl<sizeof(sms_sc);k+=2) {int a=hexval(hex[k]),b=hexval(hex[k+1]);if (a<0||b<0) {sms_scl=0;break;}sms_sc[sms_scl++]=(uint8_t)(a<<4|b);}
    if (!sms_scl) {   /* from the URI's digits: TON international, BCD */
        const char *d=strchr(uri,'+');d=d?d+1:uri;char dg[24];size_t nd=0;
        for (;*d && nd<20;d++) if (*d>='0' && *d<='9') dg[nd++]=*d;
        sms_sc[sms_scl++]=0x91;
        for (size_t k=0;k<nd;k+=2) sms_sc[sms_scl++]=(uint8_t)((dg[k]-'0')|((k+1<nd?dg[k+1]-'0':15)<<4));
    }
    printf("SMS: SMSC %s\n",sms_sc_uri);
}
static int sms_send(int sock,int portc,const char *number,const char *text,const char *job_id)
{
    sms_smsc_load();
    if (!sms_scl || !sms_sc_uri[0]) {printf("SMS: no SMSC known yet\n");fflush(stdout);return -1;}
    char dig[24];size_t nd=0;int intl=number[0]=='+';for (const char *c=number;*c && nd<20;c++) if (*c>='0' && *c<='9') dig[nd++]=*c;dig[nd]=0;
    if (nd<3) {printf("SMS: bad number\n");return -1;}
    if (!intl && nd==10 && dig[0]>='6') {memmove(dig+2,dig,11);dig[0]='9';dig[1]='1';nd=12;intl=1;}   /* Indian mobile without +91 */
    else if (!intl && nd==11 && dig[0]=='0') {memmove(dig+2,dig+1,11);dig[0]='9';dig[1]='1';nd=12;intl=1;}
    int gsm=sms_is_gsm(text);
    static uint16_t u[2048];int n=gsm?(int)strlen(text):sms_utf16(text,u,2048);
    if (n<0 || (gsm && n>153*255) || (!gsm && n>67*255)) {printf("SMS: text not encodable or too long\n");fflush(stdout);return -1;}
    int one=gsm?160:70,per=gsm?153:67;
    int parts=n<=one?1:(n+per-1)/per;
    if (!gsm && parts>1) {int k=0,c=0;while (k<n) {int e=k+per;if (e<n && u[e-1]>=0xd800 && u[e-1]<0xdc00) e--;k=e<n?e:n;c++;}parts=c;}
    static uint8_t cref;cref++;
    int j=-1;
    if (job_id) {for (int i=0;i<32;i++) if (!sms_job[i].id[0]) {j=i;break;}
        if (j<0) {printf("SMS: too many sends waiting\n");return -1;}
        snprintf(sms_job[j].id,sizeof(sms_job[j].id),"%s",job_id);sms_job[j].left=parts;sms_job[j].failed=0;}
    int pos=0,sent=0;
    for (int part=1;part<=parts;part++) {
        int len=parts==1?n:per;if (pos+len>n) len=n-pos;
        if (!gsm && parts>1 && pos+len<n && u[pos+len-1]>=0xd800 && u[pos+len-1]<0xdc00) len--;
        uint8_t tp[200];unsigned t=0;
        tp[t++]=(uint8_t)(0x01|(parts>1?0x40:0));tp[t++]=(uint8_t)(sms_mr++);   /* SMS-SUBMIT (UDHI when in parts), TP-MR */
        tp[t++]=(uint8_t)nd;tp[t++]=intl?0x91:0x81;
        for (size_t k=0;k<nd;k+=2) tp[t++]=(uint8_t)((dig[k]-'0')|((k+1<nd?dig[k+1]-'0':15)<<4));
        tp[t++]=0x00;tp[t++]=gsm?0x00:0x08;
        uint8_t udh[6]={5,0,3,cref,(uint8_t)parts,(uint8_t)part};int hl=parts>1?6:0;
        unsigned udlpos=t++;uint8_t *ud=tp+t;memset(ud,0,140);
        if (hl) memcpy(ud,udh,(size_t)hl);
        if (gsm) {   /* septets after the header, which is padded to a septet boundary (6 bytes = 48 bits -> 49) */
            unsigned skip=hl?49:0;
            for (int k=0;k<len;k++) {unsigned bit=skip+7*(unsigned)k;uint8_t c=(uint8_t)text[pos+k];ud[bit/8]|=(uint8_t)(c<<(bit%8));if (bit%8>1) ud[bit/8+1]|=(uint8_t)(c>>(8-bit%8));}
            unsigned septets=(hl?7:0)+(unsigned)len;tp[udlpos]=(uint8_t)septets;t+=(septets*7+7)/8;
        } else {
            for (int k=0;k<len;k++) {ud[hl+2*k]=(uint8_t)(u[pos+k]>>8);ud[hl+2*k+1]=(uint8_t)u[pos+k];}
            tp[udlpos]=(uint8_t)(hl+2*len);t+=(unsigned)(hl+2*len);
        }
        int ref=sms_send_tpdu(sock,portc,tp,t);
        if (ref<0) {if (j>=0) {sms_job[j].failed=1;sms_job[j].left-=(parts-part+1);}break;}
        if (j>=0) sms_ref_job[ref]=(unsigned char)(j+1);
        sent++;pos+=len;
    }
    printf("--> SMS to …%s: %d character(s) in %d part(s), %s, %d handed to the network\n",nd>4?dig+nd-4:dig,n,parts,gsm?"GSM":"UCS-2",sent);fflush(stdout);
    if (j>=0 && sent==0) {sms_job[j].id[0]=0;return -1;}   /* nothing went out: the caller answers "failed" */
    return 0;                                               /* parts out: their RP-ACK/RP-ERROR answer the job */
}
static void sms_outbox(int sock,int portc)
{   /* /var/tmp/redwood-cell/sms-out/<id>.msg from the Messages app: number on the first line, then the text */
    mkdir("/var/tmp/redwood-cell/sms-out",0700);chown("/var/tmp/redwood-cell/sms-out",501,501);
    DIR *d=opendir("/var/tmp/redwood-cell/sms-out");if (!d) return;
    struct dirent *e;
    while ((e=readdir(d))) {
        size_t L=strlen(e->d_name);if (L<5 || strcmp(e->d_name+L-4,".msg") || L-4>=80) continue;
        char fn[300],body[1400];snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-out/%s",e->d_name);
        FILE *q=fopen(fn,"r");if (!q) continue;size_t n=fread(body,1,sizeof(body)-1,q);fclose(q);body[n]=0;unlink(fn);
        char id[80];snprintf(id,sizeof(id),"%.*s",(int)(L-4),e->d_name);
        char *nl=strchr(body,'\n');int rc=-1;
        if (nl) {*nl=0;rc=sms_send(sock,portc,body,nl+1,id);}
        if (rc<0) {snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-out/%s.fail",id);q=fopen(fn,"w");if (q) {fclose(q);chown(fn,501,501);}}
    }
    closedir(d);
}
/* command pipe for the client: "sms NUMBER TEXT" (root only) */
static int ims_cmd_fd=-1;
static void ims_cmd_poll(int sock,int portc)
{
    sms_outbox(sock,portc);
    sms_cat_tick();
    if (ims_cmd_fd<0) {
        unlink("/var/tmp/redwood-cell/ims-cmd");
        if (mkfifo("/var/tmp/redwood-cell/ims-cmd",0600)) return;
        ims_cmd_fd=open("/var/tmp/redwood-cell/ims-cmd",O_RDWR|O_NONBLOCK);
        if (ims_cmd_fd<0) return;
    }
    static char buf[2048];static size_t have;
    ssize_t g=read(ims_cmd_fd,buf+have,sizeof(buf)-1-have);if (g<=0) return;have+=(size_t)g;buf[have]=0;
    char *nl;
    while ((nl=strchr(buf,'\n'))) {
        *nl=0;
        if (!strncmp(buf,"sms ",4)) {char *num=buf+4;while (*num==' ') num++;char *sp=strchr(num,' ');if (sp) {*sp=0;(void)sms_send(sock,portc,num,sp+1,NULL);}}
        size_t used=(size_t)(nl+1-buf);memmove(buf,nl+1,have-used+1);have-=used;
    }
    if (have>=sizeof(buf)-1) have=0;
}

static void ims_stop(int sig)
{   /* SIGTERM/SIGINT: our SAs and policies go with us (the next start makes its own) */
    (void)sig;unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();_exit(0);
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    signal(SIGTERM,ims_stop);signal(SIGINT,ims_stop);signal(SIGPIPE,SIG_IGN);
    pfk=socket(PF_KEY,SOCK_RAW,PF_KEY_V2);
    if (argc>1 && !strcmp(argv[1],"--pfkey")) {printf("PF_KEY socket: %s\n",pfk>=0?"available":strerror(errno));return pfk<0;}
    if (argc>1 && !strcmp(argv[1],"--flush")) {ipsec_flush();printf("SAs and policies flushed\n");return 0;}
    if (load_cfg()) {fprintf(stderr,"redwood-ims: no IMS PDN yet (/var/tmp/redwood-cell/ims)\n");return 1;}
    {   /* one client only: an older one (e.g. left by a restarted redwood-cellulard) is stopped first -- two clients
         * flush each other's SAs and neither can send */
        FILE *q=fopen("/var/tmp/redwood-cell/ims.pid","r");int old=0;
        if (q) {if (fscanf(q,"%d",&old)!=1) old=0;fclose(q);}
        if (old>1 && old!=getpid() && !kill(old,0)) {
            printf("stopping the older client (pid %d)\n",old);kill(old,SIGTERM);
            for (int i=0;i<50 && !kill(old,0);i++) usleep(100000);
            if (!kill(old,0)) kill(old,SIGKILL);
        }
        q=fopen("/var/tmp/redwood-cell/ims.pid","w");if (q) {fprintf(q,"%d\n",getpid());fclose(q);}
    }
    if (pfk<0) {perror("PF_KEY");return 1;}
    int udp=flag(argc,argv,"--udp"),tcp=!udp,pci=atoi(opt(argc,argv,"--pcscf","0"))%cfg.npc;
    if (!opt(argc,argv,"--pcscf",NULL)) {   /* the P-CSCF that answered last time, or the next one after a silent one */
        FILE *q=fopen("/var/tmp/redwood-cell/ims-pcscf","r");int v=0;if (q) {if (fscanf(q,"%d",&v)==1 && v>=0) pci=v%cfg.npc;fclose(q);}
    }
    const char *ua=opt(argc,argv,"--ua","Samsung_SM-G980F_G980FXXSNHYB7_IMS_6.0"),*pani=opt(argc,argv,"--pani","");
    struct in6_addr me6,pc6;inet_pton(AF_INET6,cfg.ip6,&me6);inet_pton(AF_INET6,cfg.pcscf[pci],&pc6);
    ipsec_flush();
    if (flag(argc,argv,"--selftest")) {     /* the kernel's side alone: dummy keys, then removed; and the ISIM opens */
        uint8_t k[20];memset(k,0x5a,sizeof(k));
        int a1=sa_add(&me6,&pc6,0x1001,1,SADB_AALG_MD5HMAC,k,16,SADB_X_EALG_AESCBC,k,16),a2=sa_add(&pc6,&me6,0x1002,0,SADB_AALG_SHA1HMAC,k,20,SADB_EALG_NULL,NULL,0);
        int q1=sp_add(&me6,40001,&pc6,40002,IPSEC_DIR_OUTBOUND,1),q2=sp_add(&pc6,40002,&me6,40001,IPSEC_DIR_INBOUND,0);
        printf("selftest: SA md5/aes %d, SA sha1/null %d, policy out %d, policy in %d (0 = accepted)\n",a1,a2,q1,q2);
        ipsec_flush();
        int io=isim_open();printf("selftest: ISIM %s (channel %d)\n",io?"did NOT open":"opened and selected",isim_channel);isim_close();
        return a1 || a2 || q1 || q2 || io;
    }
    if (isim_open()) {fprintf(stderr,"redwood-ims: the ISIM did not open (is redwood-cellulard running?)\n");return 1;}

    /* 1. the first REGISTER, in the clear, from the port that becomes port-c */
    int s=bound(tcp?SOCK_STREAM:SOCK_DGRAM,0);if (s<0) {perror("bind");return 1;}
    struct sockaddr_in6 a={.sin6_len=sizeof(a),.sin6_family=AF_INET6,.sin6_port=htons(5060),.sin6_addr=pc6};
    if (connect(s,(struct sockaddr *)&a,sizeof(a))) {perror("connect");isim_close();return 1;}
    struct sockaddr_in6 mine;socklen_t ml=sizeof(mine);getsockname(s,(struct sockaddr *)&mine,&ml);
    int portc=ntohs(mine.sin6_port),ports=portc+2;
    int ls=tcp?bound(SOCK_STREAM,ports):-1,us=bound(SOCK_DGRAM,ports);
    if ((tcp && (ls<0 || listen(ls,4))) || us<0) {fprintf(stderr,"redwood-ims: port-s %d is taken\n",ports);isim_close();return 1;}
    uint32_t spic=(arc4random()&0x0fffffff)|0x1000,spis=spic+1;
    char secc[900];
    if (flag(argc,argv,"--nullonly"))   /* (the first form tried: Jio's P-CSCF answers it 403 with no challenge) */
        snprintf(secc,sizeof(secc),"ipsec-3gpp;prot=esp;mod=trans;spi-c=%u;spi-s=%u;port-c=%d;port-s=%d;ealg=null;alg=hmac-md5-96",spic,spis,portc,ports);
    else {
        static const char *const algs[]={"hmac-md5-96","hmac-sha-1-96"},*const ealgs[]={"aes-cbc","null"};size_t o=0;
        for (int i=0;i<2;i++) for (int j=0;j<2;j++)
            o+=(size_t)snprintf(secc+o,sizeof(secc)-o,"%sipsec-3gpp;alg=%s;ealg=%s;spi-c=%u;spi-s=%u;port-c=%d;port-s=%d",o?", ":"",algs[i],ealgs[j],spic,spis,portc,ports);
    }
    char user[128],uri[128],common[1400],m[4000],r[6000];
    snprintf(user,sizeof(user),"%s@%s",cfg.imsi,cfg.realm);snprintf(uri,sizeof(uri),"sip:%s",cfg.realm);
    uint32_t tag=arc4random(),cid=arc4random();const char *T=tcp?"TCP":"UDP";
    snprintf(common,sizeof(common),
        "Max-Forwards: 70\r\n"
        "From: <sip:%s>;tag=%08x\r\n"
        "To: <sip:%s>\r\n"
        "Call-ID: %08x@[%s]\r\n"
        "Contact: <sip:%s@[%s]:%d;transport=%s>;+sip.instance=\"<urn:gsma:imei:%.8s-%.6s-0>\";+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\";+g.3gpp.smsip;audio\r\n"
        "%s%s%s"
        "Require: sec-agree\r\nProxy-Require: sec-agree\r\n"
        "Supported: path,sec-agree\r\n"
        "Allow: INVITE,ACK,CANCEL,BYE,OPTIONS,MESSAGE,NOTIFY\r\n"
        "User-Agent: %s\r\n"
        "Expires: 600000\r\n",
        user,tag,user,cid,cfg.ip6,cfg.imsi,cfg.ip6,ports,tcp?"tcp":"udp",cfg.imei,cfg.imei+8,
        *pani?"P-Access-Network-Info: ":"",pani,*pani?"\r\n":"",ua);
    int n=snprintf(m,sizeof(m),
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/%s [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "CSeq: 1 REGISTER\r\n"
        "%s"
        "Authorization: Digest username=\"%s\",realm=\"%s\",uri=\"%s\",nonce=\"\",response=\"\",algorithm=AKAv1-MD5\r\n"
        "Security-Client: %s\r\n"
        "Content-Length: 0\r\n\r\n",uri,T,cfg.ip6,portc,arc4random(),common,user,cfg.realm,uri,secc);
    printf("--> REGISTER 1 to [%s]:5060 over %s, port-c %d port-s %d\n",cfg.pcscf[pci],T,portc,ports);
    if (send(s,m,(size_t)n,0)<0) {perror("send");isim_close();return 1;}
    do n=recv_msg(s,r,sizeof(r),8); while (n>0 && !strncmp(r,"SIP/2.0 1",9));
    if (n<=0) {   /* this P-CSCF is silent: the next start (redwood-cellulard, 30 s) tries the other */
        FILE *q=fopen("/var/tmp/redwood-cell/ims-pcscf","w");if (q) {fprintf(q,"%d\n",(pci+1)%cfg.npc);fclose(q);}
        printf("no answer from P-CSCF %d; the next start tries P-CSCF %d\n",pci,(pci+1)%cfg.npc);isim_close();return 1;}
    printf("<-- %.*s\n",(int)(strstr(r,"\r\n")-r),r);
    if (strncmp(r,"SIP/2.0 401",11)) {show(r);isim_close();return 1;}
    char www[900],secs[900],nonce[300]={0};
    if (!hdr(r,"WWW-Authenticate",www,sizeof(www)) || !hdr(r,"Security-Server",secs,sizeof(secs))) {printf("401 without a challenge or Security-Server\n");show(r);isim_close();return 1;}
    {const char *p=strstr(www,"nonce=\"");if (p) {p+=7;const char *e=strchr(p,'"');if (e && (size_t)(e-p)<sizeof(nonce)) memcpy(nonce,p,(size_t)(e-p));}}
    uint8_t chal[64];if (b64(nonce,strlen(nonce),chal,sizeof(chal))<32) {printf("the nonce is not RAND|AUTN\n");isim_close();return 1;}
    /* the first offer the P-CSCF lists is the one it wants */
    char first[400];{const char *e=strchr(secs,',');size_t L=e?(size_t)(e-secs):strlen(secs);if (L>=sizeof(first)) L=sizeof(first)-1;memcpy(first,secs,L);first[L]=0;}
    uint32_t pspic=0,pspis=0,pportc=0,pports=0;
    if (param_u(first,"spi-c",&pspic) || param_u(first,"spi-s",&pspis) || param_u(first,"port-c",&pportc) || param_u(first,"port-s",&pports)) {printf("Security-Server not understood: %s\n",first);isim_close();return 1;}
    int aalg=strstr(first,"hmac-sha-1-96")?SADB_AALG_SHA1HMAC:SADB_AALG_MD5HMAC,ealg=strstr(first,"aes-cbc")?SADB_X_EALG_AESCBC:SADB_EALG_NULL;
    printf("    challenge received; the P-CSCF chose %s / %s, its port-c %u port-s %u\n",aalg==SADB_AALG_SHA1HMAC?"hmac-sha-1-96":"hmac-md5-96",ealg==SADB_X_EALG_AESCBC?"aes-cbc":"null",pportc,pports);
    close(s);

    /* 2. the SIM answers the challenge */
    uint8_t res[16],ck[16],ik[20]={0};int resl=0;
    int e=isim_aka(chal,chal+16,res,&resl,ck,ik);isim_close();
    if (e) {printf("AKA: %s\n",e==1?"the SIM reports a sequence (SQN) failure: resynchronisation is not written yet":"failed");return 1;}
    printf("    the SIM answered (RES %d bytes)\n",resl);

    /* 3. four ESP transport SAs and the policies that send the two port pairs through them (TS 33.203 7.1) */
    int akl=aalg==SADB_AALG_SHA1HMAC?20:16,ekl=ealg==SADB_X_EALG_AESCBC?16:0;
    int e1=sa_add(&me6,&pc6,pspis,1,aalg,ik,akl,ealg,ck,ekl);        /* UE port-c -> P port-s */
    int e2=sa_add(&pc6,&me6,spic,0,aalg,ik,akl,ealg,ck,ekl);         /* P port-s  -> UE port-c */
    int e3=sa_add(&pc6,&me6,spis,0,aalg,ik,akl,ealg,ck,ekl);         /* P port-c  -> UE port-s */
    int e4=sa_add(&me6,&pc6,pspic,2,aalg,ik,akl,ealg,ck,ekl);        /* UE port-s -> P port-c */
    int p1=sp_add(&me6,portc,&pc6,(int)pports,IPSEC_DIR_OUTBOUND,1),p2=sp_add(&me6,ports,&pc6,(int)pportc,IPSEC_DIR_OUTBOUND,2);
    int p3=sp_add(&pc6,(int)pports,&me6,portc,IPSEC_DIR_INBOUND,0),p4=sp_add(&pc6,(int)pportc,&me6,ports,IPSEC_DIR_INBOUND,0);
    memset(ck,0,sizeof(ck));memset(ik,0,sizeof(ik));
    printf("    IPsec: SAs %d %d %d %d, policies %d %d %d %d (0 = accepted)\n",e1,e2,e3,e4,p1,p2,p3,p4);
    if (e1 || e2 || e3 || e4 || p1 || p2 || p3 || p4) {ipsec_flush();return 1;}

    /* 4. the second REGISTER, protected: port-c -> the P-CSCF's port-s */
    char ha1[33],ha2[33],resp[33],buf[600],cnonce[20];
    {size_t L=(size_t)snprintf(buf,sizeof(buf),"%s:%s:",user,cfg.realm);memcpy(buf+L,res,(size_t)resl);md5hex(buf,L+(size_t)resl,ha1);memset(res,0,sizeof(res));}
    {size_t L=(size_t)snprintf(buf,sizeof(buf),"REGISTER:%s",uri);md5hex(buf,L,ha2);}
    snprintf(cnonce,sizeof(cnonce),"%08x%08x",arc4random(),arc4random());
    {size_t L=(size_t)snprintf(buf,sizeof(buf),"%s:%s:00000001:%s:auth:%s",ha1,nonce,cnonce,ha2);md5hex(buf,L,resp);}
    memset(ha1,0,sizeof(ha1));
    s=bound(tcp?SOCK_STREAM:SOCK_DGRAM,portc);if (s<0) {perror("bind port-c");ipsec_flush();return 1;}
    a.sin6_port=htons((uint16_t)pports);
    if (connect(s,(struct sockaddr *)&a,sizeof(a))) {perror("connect (protected)");ipsec_flush();return 1;}
    {int on=1,idle=30,intvl=10,cnt=3;setsockopt(s,SOL_SOCKET,SO_KEEPALIVE,&on,sizeof(on));
     setsockopt(s,IPPROTO_TCP,TCP_KEEPALIVE,&idle,sizeof(idle));setsockopt(s,IPPROTO_TCP,TCP_KEEPINTVL,&intvl,sizeof(intvl));
     setsockopt(s,IPPROTO_TCP,TCP_KEEPCNT,&cnt,sizeof(cnt));}
    n=snprintf(m,sizeof(m),
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/%s [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "CSeq: 2 REGISTER\r\n"
        "%s"
        "Authorization: Digest username=\"%s\",realm=\"%s\",nonce=\"%s\",uri=\"%s\",response=\"%s\",algorithm=AKAv1-MD5,qop=auth,nc=00000001,cnonce=\"%s\"\r\n"
        "Security-Client: %s\r\n"
        "Security-Verify: %s\r\n"
        "Content-Length: 0\r\n\r\n",uri,T,cfg.ip6,ports,arc4random(),common,user,cfg.realm,nonce,uri,resp,cnonce,secc,secs);
    printf("--> REGISTER 2 (protected) to port %u\n",pports);
    if (send(s,m,(size_t)n,0)<0) {perror("send (protected)");ipsec_flush();return 1;}
    do n=recv_msg(s,r,sizeof(r),10); while (n>0 && !strncmp(r,"SIP/2.0 1",9));
    if (n<=0) {printf("no answer to the protected REGISTER (the SAs stay for a look: redwood-ims --flush removes them)\n");return 1;}
    printf("<-- %.*s\n",(int)(strstr(r,"\r\n")-r),r);show(r);
    if (strncmp(r,"SIP/2.0 200",11)) {ipsec_flush();return 1;}
    printf("REGISTERED\n");
    {FILE *q=fopen("/var/tmp/redwood-cell/ims-registered","w");if (q) {fchmod(fileno(q),0644);fprintf(q,"%lld\n",(long long)time(NULL));fclose(q);}}
    sms_keep_registration(r);
    if (flag(argc,argv,"--once")) return 0;

    /* 5. stay: show what the network sends, answer requests with 200 */
    int cs[8];int ncs=0;time_t registered_at=time(NULL);
    for (;;) {
        /* a fresh registration well before the 7200 s expiry: leave cleanly, redwood-cellulard starts us again */
        if (time(NULL)-registered_at>6600) {printf("re-registration due: leaving for a fresh start\n");unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}
        struct pollfd p[12];int k=0;p[k++]=(struct pollfd){s,POLLIN,0};p[k++]=(struct pollfd){us,POLLIN,0};if (ls>=0) p[k++]=(struct pollfd){ls,POLLIN,0};
        int base=k;for (int i=0;i<ncs;i++) p[k++]=(struct pollfd){cs[i],POLLIN,0};
        if (poll(p,(nfds_t)k,1000)<0) break;
        {   /* keep the registration's connection open: a CRLF CRLF ping every 25 s (RFC 5626 3.5.1). Without it the
             * network dropped the idle TCP connection within minutes, and a text sent into it got no answer at all
             * ("send: failed", 2026-11-13 18:43 and 18:46). */
            static time_t ka;time_t t=time(NULL);
            if (t-ka>=25) {ka=t;if (send(s,"\r\n\r\n",4,0)<0) {printf("keep-alive: the registration connection is gone (%s)\n",strerror(errno));unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}}
        }
        ims_cmd_poll(s,portc);
        if (ls>=0 && (p[2].revents&POLLIN) && ncs<8) {int c=accept(ls,NULL,NULL);if (c>=0) {cs[ncs++]=c;printf("the P-CSCF connected to port-s\n");}}
        for (int i=0;i<k;i++) {
            if (!(p[i].revents&(POLLIN|POLLHUP)) || (ls>=0 && i==2)) continue;
            struct sockaddr_in6 from;socklen_t fl=sizeof(from);ssize_t g;
            if (i==1) {g=recvfrom(us,r,sizeof(r)-1,0,(struct sockaddr *)&from,&fl);if (g>0) r[g]=0;}
            else g=recv_msg(p[i].fd,r,sizeof(r),2);
            if (g<=0) {if (i>=base) {close(cs[i-base]);cs[i-base]=cs[--ncs];} else if (i==0) {printf("the registration connection closed\n");unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;} break;}
            printf("<-- %zd bytes\n",g);show(r);printf("\n");
            if (strncmp(r,"SIP/2.0",7)) {      /* a request: 200 with its Via/From/To/Call-ID/CSeq */
                /* An INVITE (a call to us) must NOT get a bare 200: the caller would be connected to silence, and
                 * billed. Until calls exist: 100 Trying, the request saved (root-only, for working out the call
                 * flow), then 486 Busy Here. The ACK for the 486 needs no answer. (2026-11-13) */
                int invite=!strncmp(r,"INVITE ",7);
                const char *status=invite?"486 Busy Here":"200 OK";
                if (invite) {
                    char fn[96];snprintf(fn,sizeof(fn),"/var/root/ims-invite-%ld.txt",(long)time(NULL));
                    int fd=open(fn,O_WRONLY|O_CREAT|O_TRUNC,0600);if (fd>=0) {(void)write(fd,r,(size_t)g);close(fd);}
                    printf("INCOMING CALL: saved %s, answered busy (calls not implemented yet)\n",fn);
                }
                char via[900],fr[300],to[300],ci[200],cq[80];int o=0;
                if (invite) {
                    o=snprintf(m,sizeof(m),"SIP/2.0 100 Trying\r\n");
                    const char *q=r;while ((q=strstr(q,"\r\nVia:"))) {const char *e2=strstr(q+2,"\r\n");o+=snprintf(m+o,sizeof(m)-(size_t)o,"%.*s\r\n",(int)(e2-q-2),q+2);q=e2;}
                    if (hdr(r,"From",fr,sizeof(fr))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"From: %s\r\n",fr);
                    if (hdr(r,"To",to,sizeof(to))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"To: %s\r\n",to);
                    if (hdr(r,"Call-ID",ci,sizeof(ci))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"Call-ID: %s\r\n",ci);
                    if (hdr(r,"CSeq",cq,sizeof(cq))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"CSeq: %s\r\n",cq);
                    o+=snprintf(m+o,sizeof(m)-(size_t)o,"Content-Length: 0\r\n\r\n");
                    if (i==1) sendto(us,m,(size_t)o,0,(struct sockaddr *)&from,fl); else send(p[i].fd,m,(size_t)o,0);
                }
                o=snprintf(m,sizeof(m),"SIP/2.0 %s\r\n",status);
                const char *q=r;while ((q=strstr(q,"\r\nVia:"))) {const char *e2=strstr(q+2,"\r\n");o+=snprintf(m+o,sizeof(m)-(size_t)o,"%.*s\r\n",(int)(e2-q-2),q+2);q=e2;}
                (void)via;
                if (hdr(r,"From",fr,sizeof(fr))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"From: %s\r\n",fr);
                if (hdr(r,"To",to,sizeof(to))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"To: %s%s\r\n",to,strstr(to,"tag=")?"":";tag=rw1");
                (void)status;
                if (hdr(r,"Call-ID",ci,sizeof(ci))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"Call-ID: %s\r\n",ci);
                if (hdr(r,"CSeq",cq,sizeof(cq))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"CSeq: %s\r\n",cq);
                o+=snprintf(m+o,sizeof(m)-(size_t)o,"Content-Length: 0\r\n\r\n");
                if (strncmp(r,"ACK",3)) {if (i==1) sendto(us,m,(size_t)o,0,(struct sockaddr *)&from,fl); else send(p[i].fd,m,(size_t)o,0);}
                sms_incoming(s,r,(size_t)g,portc);
            }
        }
    }
    return 0;
}
