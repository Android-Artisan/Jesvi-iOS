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
#include <notify.h>
#include <stdarg.h>
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

/* every log line about calls carries the time (A, 2026-11-14: late rings could not be measured without it) */
static const char *ts_now(void)
{
    static char b[24];struct timeval tv;gettimeofday(&tv,NULL);struct tm tm;localtime_r(&tv.tv_sec,&tm);
    snprintf(b,sizeof(b),"%02d:%02d:%02d.%03d",tm.tm_hour,tm.tm_min,tm.tm_sec,(int)(tv.tv_usec/1000));return b;
}

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
    if (type==SOCK_STREAM) {struct linger lg={1,0};setsockopt(s,SOL_SOCKET,SO_LINGER,&lg,sizeof(lg));}   /* close = reset: our fixed ports stay free */
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
/* (A, 2026-11-14) bytes after a complete message, per connection: one TCP read can carry two messages (Jio sent 180 and
 * 200 together; the 200 was lost and the call screen stayed on "calling"), so the rest is kept for the next call */
static struct {int fd;size_t n;char b[16384];} recv_left[16];
static int recv_pending(int s)
{
    for (int i=0;i<16;i++) if (recv_left[i].fd==s+1 && recv_left[i].n) return 1;
    return 0;
}
static int recv_msg(int s,char *b,size_t cap,int secs)
{   /* one SIP message from a stream (headers + Content-Length), or a datagram */
    size_t n=0;b[0]=0;
    for (int i=0;i<16;i++) if (recv_left[i].fd==s+1 && recv_left[i].n) {
        n=recv_left[i].n<cap-1?recv_left[i].n:cap-1;memcpy(b,recv_left[i].b,n);b[n]=0;recv_left[i].n=0;break;
    }
    for (;;) {
        char *e=n?strstr(b,"\r\n\r\n"):NULL;
        if (e) {
            /* (keep-alive CRLFs in front of a message are skipped) */
            size_t skip=0;while (skip+1<n && b[skip]=='\r' && b[skip+1]=='\n') skip+=2;
            if (skip) {memmove(b,b+skip,n-skip+1);n-=skip;continue;}
            char cl[32];int body=hdr(b,"Content-Length",cl,sizeof(cl))?atoi(cl):hdr(b,"l",cl,sizeof(cl))?atoi(cl):0;
            size_t len=(size_t)(e+4-b)+(size_t)(body>0?body:0);
            if (n>=len) {
                if (n>len) {   /* the next message(s): kept */
                    int k=-1;for (int i=0;i<16;i++) if (recv_left[i].fd==s+1 || (k<0 && !recv_left[i].n)) {k=i;if (recv_left[i].fd==s+1) break;}
                    if (k>=0) {size_t m=n-len<sizeof(recv_left[k].b)?n-len:sizeof(recv_left[k].b);memcpy(recv_left[k].b,b+len,m);recv_left[k].n=m;recv_left[k].fd=s+1;}
                }
                b[len]=0;return (int)len;
            }
        }
        struct pollfd p={s,POLLIN,0};if (poll(&p,1,secs*1000)<=0) return n?(int)n:-1;
        ssize_t r=recv(s,b+n,cap-1-n,0);if (r<=0) return n?(int)n:-1;
        n+=(size_t)r;b[n]=0;
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

/*
 * Calls, step 1 (2026-11-13, A): answering at the SIP level, behind /var/root/cell-answer (absent: 486 Busy as
 * before). Jio's INVITE (docs/cellular-download-plan.md "Jio's INVITE") has no preconditions and 100rel is only
 * supported, so: 100 Trying, 180 Ringing, after CALL_ANSWER_S 200 OK with an SDP answer (AMR-NB PT 100, DTMF 101) ->
 * ACK; CANCEL -> 200 + 487; BYE -> 200; our own BYE after CALL_MAX_S. No media yet (the CP's CPVE comes next):
 * the caller hears silence. One call at a time; a second INVITE gets 486.
 */
#define CALL_ANSWER_S 3
#define CALL_MAX_S 3600
#define CALL_RTP_PORT 50010
#define IMS_PORT_C 50060              /* port-s is port-c + 2 */
static struct {
    int state;                          /* 0 none, 1 ringing, 2 answered (200 sent), 3 confirmed (ACK) */
    int fd;                             /* the connection the INVITE came on */
    time_t at, answered;
    char via[1200], from[400], to[400], cid[200], cq[80], rr[600], contact[400], sdp_c[96], sdp_m[96];
    unsigned bye_cseq;
    int ui, answer_req;                 /* ui: the phone's call screen decides (RedwoodPhone); answer_req: it said answer */
    unsigned id;                        /* this call's number, for the call screen */
    int out;                            /* we called (state 10 dialling, then 2 alerting/answered, 3 up) */
    char branch[40];unsigned rseq_acked;
    char tag[80];                       /* the call screen's tag of an outgoing call (for an early cancel) */
} call;
/* call numbers unique across restarts (the call screen remembers ended numbers: after a restart "call 1" was taken
 * for an old one and an incoming call never rang, 2026-11-14 05:00) */
static unsigned call_seq;
static char call_ip6[64];
static void call_media(int start);
static void call_bye(void);
static void call_ui_poll(void);
static void call_cancel(int sock);
static void call_sdp_from(const char *r);
static int call_ports, call_sock = -1, call_portc;
static const char *call_user;

/*
 * Calls, step 4 (2026-11-14, A): the native call screen. RedwoodPhone.app (tools/cellular/RedwoodPhone) is the CallKit
 * provider; it and redwood-ims talk through /var/tmp/redwood-cell/call-ui/ (mobile's):
 *   state   (ours)    "ringing ID NUMBER", "connecting ID", "up ID", "ended ID REASON"
 *   alive   (theirs)  touched every few seconds while the app runs
 *   cmd.*   (theirs)  "answer ID" / "end ID", one per file, removed when read
 * With /var/root/cell-answer the old auto-answer stays (tests).
 */
#define CALL_UI_DIR "/var/tmp/redwood-cell/call-ui"
static int
call_ui_alive(void)
{
    struct stat st;
    return !stat(CALL_UI_DIR "/alive",&st) && time(NULL)-st.st_mtime<20;
}
static int
call_auto(void)
{
    struct stat st;
    return !stat("/var/root/cell-answer",&st);
}
static int
call_enabled(void)
{
    return call_auto() || call_ui_alive();
}
/* the other party's number, for RedwoodPhone's own call screen (local only) */
static void
call_ui_number(const char *num)
{
    FILE *f=fopen(CALL_UI_DIR "/number","w");if (!f) return;
    fprintf(f,"%s",num);fclose(f);chmod(CALL_UI_DIR "/number",0644);
}
static void
call_ui_state(const char *fmt,...)
{
    char line[300];va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
    mkdir(CALL_UI_DIR,0755);chown(CALL_UI_DIR,501,501);
    FILE *f=fopen(CALL_UI_DIR "/state.tmp","w");if (!f) return;
    fprintf(f,"%s\n",line);fclose(f);chmod(CALL_UI_DIR "/state.tmp",0644);rename(CALL_UI_DIR "/state.tmp",CALL_UI_DIR "/state");
    printf("%s call: screen <- %s\n",ts_now(),line);
}
/* the caller's number from From (sip:+91...@ or tel:+91...) */
static void
call_number(const char *from,char *out,size_t cap)
{
    const char *p=strstr(from,"sip:");if (!p) p=strstr(from,"tel:");
    out[0]=0;if (!p) return;p+=4;
    size_t o=0;while (*p && *p!='@' && *p!=';' && *p!='>' && o+1<cap) out[o++]=*p++;
    out[o]=0;
}

static void
call_copy_vias(const char *r,char *out,size_t cap)
{
    size_t o=0;out[0]=0;
    for (const char *q=r;(q=strstr(q,"\r\nVia:"));) {
        const char *e=strstr(q+2,"\r\n");if (!e) break;
        o+=(size_t)snprintf(out+o,cap-o,"%.*s\r\n",(int)(e-q-2),q+2);if (o>=cap) {out[cap-1]=0;break;}
        q=e;
    }
}

static void
call_copy_rr(const char *r,char *out,size_t cap)
{
    size_t o=0;out[0]=0;
    for (const char *q=r;(q=strstr(q,"\r\nRecord-Route:"));) {
        const char *e=strstr(q+2,"\r\n");if (!e) break;
        o+=(size_t)snprintf(out+o,cap-o,"%.*s\r\n",(int)(e-q-2),q+2);if (o>=cap) {out[cap-1]=0;break;}
        q=e;
    }
}

/* a response in the call's dialog; body may be NULL */
static void
call_respond(const char *status,const char *body)
{
    char m[4096];int o;
    o=snprintf(m,sizeof(m),"SIP/2.0 %s\r\n%s%sFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n",
        status,call.via,call.rr,call.from,call.to,call.cid,call.cq);
    if (strncmp(status,"100",3)) {
        o+=snprintf(m+o,sizeof(m)-(size_t)o,"Contact: <sip:%s@[%s]:%d;transport=tcp>;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n",
            call_user,call_ip6,call_ports);
        o+=snprintf(m+o,sizeof(m)-(size_t)o,"Allow: INVITE,ACK,CANCEL,BYE,UPDATE,PRACK,OPTIONS,MESSAGE\r\nSupported: replaces\r\n");
    }
    if (body) o+=snprintf(m+o,sizeof(m)-(size_t)o,"Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s",strlen(body),body);
    else o+=snprintf(m+o,sizeof(m)-(size_t)o,"Content-Length: 0\r\n\r\n");
    if (send(call.fd,m,(size_t)o,0)<0) {
        /* the P-CSCF's connection died under the call (2026-11-14 04:40: "Broken pipe", the caller kept ringing): the
         * response goes to the same P-CSCF over our registration connection (port-c -> its port-s, which its Via names) */
        printf("%s call: send %s failed (%s): sent on the registration connection instead\n",ts_now(),status,strerror(errno));
        if (call_sock>=0 && send(call_sock,m,(size_t)o,0)>0) call.fd=call_sock;
    } else printf("%s call: --> %s\n",ts_now(),status);
}

static void
call_end(const char *why)
{
    printf("%s call: ended (%s) after %lds\n",ts_now(),why,(long)(time(NULL)-call.at));
    if (call.ui) call_ui_state("ended %u %s",call.id,why);
    call_media(0);
    call.state=0;
    unlink("/var/tmp/redwood-cell/call");
}


/*
 * Calls, step 2 (2026-11-13, A): the CP's own voice engine (CPVE) carries RTP and the codec. Commands go to the modem
 * through redwood-cellulard's command pipe ("fmt GRP IDX TYPE HEX", /var/tmp/redwood-cell/cmd), as Samsung's
 * libsec-ims sends them (docs/cellular-download-plan.md: 16/06 SetPdnIp, 16/01 SetChannel, 16/02 SetCodec,
 * 16/03 SetRtpMedia; every one acknowledged 0x8000 by this modem, tested without a call 2026-11-13 23:20).
 * Channel 0; AMR-NB 12.2 (PT 100) at 8 kHz, 20 ms. The remote address/port are the INVITE's SDP c=/m=.
 */
static void
call_cmd(const char *grp_idx_type,const uint8_t *p,size_t n)
{
    char line[600];size_t o=(size_t)snprintf(line,sizeof(line),"fmt %s ",grp_idx_type);
    for (size_t i=0;i<n && o+3<sizeof(line);i++) o+=(size_t)snprintf(line+o,sizeof(line)-o,"%02x",p[i]);
    line[o++]='\n';
    int f=open("/var/tmp/redwood-cell/cmd",O_WRONLY|O_NONBLOCK);
    if (f<0) {printf("%s call: media: the cellular command pipe: %s\n",ts_now(),strerror(errno));return;}
    if (write(f,line,o)!=(ssize_t)o) printf("%s call: media: write: %s\n",ts_now(),strerror(errno));
    close(f);
    usleep(300000);                     /* (redwood-cellulard takes one line per poll) */
}

static void
call_put16(uint8_t *p,unsigned v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}

static int
call_channel(int op,const char *remote,unsigned rport)
{
    uint8_t c[47]={0},loc[16],rem[16];
    if (inet_pton(AF_INET6,call_ip6,loc)!=1 || inet_pton(AF_INET6,remote,rem)!=1) {printf("%s call: media: bad address\n",ts_now());return -1;}
    c[0]=0;c[1]=(uint8_t)op;
    call_put16(c+2,CALL_RTP_PORT+1);call_put16(c+4,rport+1);call_put16(c+6,CALL_RTP_PORT);call_put16(c+8,rport);
    c[10]=2;memcpy(c+11,rem,16);c[27]=2;memcpy(c+28,loc,16);
    call_cmd("16 01 03",c,sizeof(c));
    return 0;
}

#include "ims-media.h"

static int
call_cpvoice(void)
{
    struct stat st;
    return !stat("/var/root/cell-cpvoice",&st);   /* the CP's CPVE instead of the AP (it does not take the stream yet) */
}

static void
call_media(int start)
{
    static int open;
    unsigned rport=(unsigned)atoi(call.sdp_m);
    if (!call_cpvoice()) {
        if (start) {
            if (!call.sdp_c[0] || !rport) {printf("%s call: media: no remote address in the SDP\n",ts_now());return;}
            int s=bound(SOCK_DGRAM,CALL_RTP_PORT);
            if (s<0) {printf("%s call: media: RTP socket: %s\n",ts_now(),strerror(errno));return;}
            media_start(s,call.sdp_c,rport);
        } else media_stop();
        return;
    }
    if (start) {
        if (!call.sdp_c[0] || !rport) {printf("%s call: media: no remote address in the SDP\n",ts_now());return;}
        uint8_t pdn[38]={0},ip[16];
        if (inet_pton(AF_INET6,call_ip6,ip)!=1) return;
        pdn[0]=2;memcpy(pdn+1,ip,16);pdn[17]=6;memcpy(pdn+21,"rmnet1",6);
        /* the CP's audio first (IPC group 9, as libsec-ril's IpcProtocol41Sound): the path (09/03 SET, 1 = handset)
         * and the voice clock (09/09 EXEC, 1 = on). Without them 16/03 start answered 100 (2026-11-13 23:08). */
        {uint8_t one=1;call_cmd("09 03 03",&one,1);call_cmd("09 09 01",&one,1);}
        call_cmd("16 06 03",pdn,sizeof(pdn));
        if (call_channel(1,call.sdp_c,rport)) return;
        uint8_t c[58]={0};
        c[0]=0;c[1]=1;                  /* channel 0; 1 codec */
        c[5]=6;c[6]=100;call_put16(c+7,8000);
        c[9]=0x00;c[10]=0x02;           /* 12.2 kbps: 0x200 (convertBitRateToIpc) */
        c[13]=20;c[14]=80;c[15]=1;c[28]=100;
        call_cmd("16 02 03",c,sizeof(c));
        uint8_t m[4]={1,0,0,0};         /* SAE_StartFDSR: 1 start */
        call_cmd("16 03 03",m,sizeof(m));
        open=1;
        printf("%s call: media: CP voice channel 0 up to [%s]:%u, AMR-NB 12.2\n",ts_now(),call.sdp_c,rport);
    } else if (open) {
        uint8_t m[4]={2,0,0,0};         /* SAE_StopFDSR: 2 stop */
        call_cmd("16 03 03",m,sizeof(m));
        call_channel(2,call.sdp_c[0]?call.sdp_c:"::1",rport?rport:9);
        {uint8_t zero=0;call_cmd("09 09 01",&zero,1);}     /* voice clock off */
        open=0;
        printf("%s call: media: CP voice channel 0 closed\n",ts_now());
    }
}


/*
 * A request in the call's dialog that may carry an SDP offer -- a re-INVITE or an UPDATE: the network refreshes the
 * session (session timers) or moves the media. Answered 200 with its own Via/From/To/CSeq and our SDP, the media
 * following a new remote address/port. (Before 2026-11-14 a re-INVITE was taken for the first INVITE again and
 * answered with the first INVITE's CSeq, an UPDATE got 200 without SDP: long calls would be dropped.)
 */
static void call_sdp_answer(char *sdp,size_t cap)
{
    unsigned sid=arc4random();
    snprintf(sdp,cap,
        "v=0\r\no=- %u %u IN IP6 %s\r\ns=-\r\nc=IN IP6 %s\r\nt=0 0\r\n"
        "m=audio %d RTP/AVP 100 101\r\nb=AS:41\r\n"
        "a=rtpmap:100 AMR/8000\r\na=fmtp:100 mode-change-capability=2;max-red=0\r\n"
        "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-15\r\na=ptime:20\r\na=maxptime:80\r\na=sendrecv\r\n",
        sid,sid,call_ip6,call_ip6,CALL_RTP_PORT);
}
static void call_reinvite(int fd,const char *r)
{
    char via[1200],fr[400],to[400],cq[80],ci[200],m[4096],sdp[700];
    call_copy_vias(r,via,sizeof(via));hdr(r,"From",fr,sizeof(fr));hdr(r,"To",to,sizeof(to));hdr(r,"CSeq",cq,sizeof(cq));hdr(r,"Call-ID",ci,sizeof(ci));
    int offer=strstr(r,"\r\n\r\nv=0")!=NULL;
    if (offer) {
        char oc[96]="",om[96]="";snprintf(oc,sizeof(oc),"%s",call.sdp_c);snprintf(om,sizeof(om),"%s",call.sdp_m);
        call_sdp_from(r);
        if ((strcmp(oc,call.sdp_c) || strcmp(om,call.sdp_m)) && med.run) {   /* the media moved: follow it */
            printf("%s call: media moves to [%s]:%s\n",ts_now(),call.sdp_c,call.sdp_m);
            call_media(0);call_media(1);
        }
    }
    call_sdp_answer(sdp,sizeof(sdp));
    int o=snprintf(m,sizeof(m),"SIP/2.0 200 OK\r\n%sFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
        "Contact: <sip:%s@[%s]:%d;transport=tcp>;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n"
        "Allow: INVITE,ACK,CANCEL,BYE,UPDATE,PRACK,OPTIONS,MESSAGE\r\nSupported: replaces,timer\r\n",
        via,fr,to,ci,cq,call_user,call_ip6,call_ports);
    char se[80];
    if (hdr(r,"Session-Expires",se,sizeof(se))) o+=snprintf(m+o,sizeof(m)-(size_t)o,"Session-Expires: %s\r\nRequire: timer\r\n",se);
    o+=snprintf(m+o,sizeof(m)-(size_t)o,"Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s",strlen(sdp),sdp);
    if (send(fd,m,(size_t)o,0)<0 && call_sock>=0) send(call_sock,m,(size_t)o,0);
    printf("%s call: in-dialog %.*s answered 200 (with SDP)\n",ts_now(),(int)(strchr(r,' ')-r),r);
}

/* an INVITE; returns 1 when handled here (answer mode), 0 for the old busy path */
static int
call_invite(int fd,const char *r)
{
    if (!call_enabled()) return 0;
    if (call.state) {
        /* the P-CSCF drops and reopens its connection and resends the INVITE (2026-11-13 23:10: our 100/180 went
         * into the closed one, "Broken pipe"): the same Call-ID moves the call to the new connection */
        char ci[200],tt[300];
        if (hdr(r,"Call-ID",ci,sizeof(ci)) && !strcmp(ci,call.cid) && call.state>=2 && hdr(r,"To",tt,sizeof(tt)) && strstr(tt,"tag=")) {
            call_reinvite(fd,r);return 1;   /* a re-INVITE in the dialog (session refresh, media change) */
        }
        if (hdr(r,"Call-ID",ci,sizeof(ci)) && !strcmp(ci,call.cid)) {
            call.fd=fd;printf("%s call: INVITE again on a new connection: carried on there\n",ts_now());
            if (call.state==1) {call_respond("100 Trying",NULL);call_respond("180 Ringing",NULL);}
            else call.state=1;          /* (answered already: answer again from call_tick) */
            return 1;
        }
        return 0;                       /* (another call: the old path answers 486) */
    }
    memset(&call,0,sizeof(call));
    call.fd=fd;call.at=time(NULL);call.state=1;call.bye_cseq=1;
    call_copy_vias(r,call.via,sizeof(call.via));
    call_copy_rr(r,call.rr,sizeof(call.rr));
    hdr(r,"From",call.from,sizeof(call.from));
    char to[300];hdr(r,"To",to,sizeof(to));
    snprintf(call.to,sizeof(call.to),"%s%s",to,strstr(to,"tag=")?"":";tag=rwc1");
    hdr(r,"Call-ID",call.cid,sizeof(call.cid));
    hdr(r,"CSeq",call.cq,sizeof(call.cq));
    hdr(r,"Contact",call.contact,sizeof(call.contact));
    const char *b=strstr(r,"\r\n\r\n");
    if (b) {
        const char *c=strstr(b,"\nc=IN IP6 "),*mm=strstr(b,"\nm=audio ");
        if (c) sscanf(c+10,"%95s",call.sdp_c);   /* (strlen("\nc=IN IP6 ") = 10: 11 dropped the first digit) */
        if (mm) sscanf(mm+9,"%95s",call.sdp_m);
    }
    {FILE *f=fopen("/var/tmp/redwood-cell/call","w");if (f) {fprintf(f,"ringing %ld\n",(long)call.at);fclose(f);}}
    call.ui=!call_auto();call.id=++call_seq;
    if (call.ui) {
        char num[64];call_number(call.from,num,sizeof(num));
        printf("%s call: INCOMING: remote media port %s; ringing on the call screen\n",ts_now(),call.sdp_m);
        call_ui_number(num[0]?num:"Unknown");
        call_ui_state("ringing %u %s",call.id,num[0]?num:"unknown");
    } else printf("%s call: INCOMING (answer mode): remote media port %s; answering in %ds\n",ts_now(),call.sdp_m,CALL_ANSWER_S);
    call_respond("100 Trying",NULL);
    call_respond("180 Ringing",NULL);
    return 1;
}

/* other requests of the call's dialog; returns 1 when it was ours */
static int
call_request(int fd,const char *r)
{
    char ci[200];
    if (!call.state || !hdr(r,"Call-ID",ci,sizeof(ci)) || strcmp(ci,call.cid)) return 0;
    if (!strncmp(r,"ACK ",4)) {
        if (call.state==2) {call.state=3;printf("%s call: ACK -- the call is up\n",ts_now());fflush(stdout);if (!med.run) call_media(1);
            if (call.ui) call_ui_state("up %u",call.id);fflush(stdout);}
        return 1;
    }
    if (!strncmp(r,"UPDATE ",7)) {call_reinvite(fd,r);return 1;}
    char m[2048],via[1200],fr[400],to[400],cq[80];int o;
    call_copy_vias(r,via,sizeof(via));hdr(r,"From",fr,sizeof(fr));hdr(r,"To",to,sizeof(to));hdr(r,"CSeq",cq,sizeof(cq));
    o=snprintf(m,sizeof(m),"SIP/2.0 200 OK\r\n%sFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\nContent-Length: 0\r\n\r\n",
        via,fr,to,ci,cq);
    send(fd,m,(size_t)o,0);
    if (!strncmp(r,"CANCEL ",7)) {
        printf("%s call: the caller cancelled\n",ts_now());
        if (call.state==1) call_respond("487 Request Terminated",NULL);
        call_end("cancelled");
    } else if (!strncmp(r,"BYE ",4)) {
        call_end("the caller hung up");
    } else {
        printf("%s call: in-dialog request answered 200\n",ts_now());
    }
    return 1;
}

/* once a second from the main loop */
static void
call_tick(void)
{
    call_ui_poll();
    if (call.out && call.state && call.state!=3 && time(NULL)-call.at>=75) {call_cancel(call_sock);call_end("no answer");return;}
    if (call.state==1 && call.ui && !call.answer_req && time(NULL)-call.at>=60) {
        call_respond("480 Temporarily Unavailable",NULL);call_end("not answered in 60 s");return;
    }
    if (!call.out && call.state==1 && (call.ui ? call.answer_req : time(NULL)-call.at>=CALL_ANSWER_S)) {
        char sdp[700];
        unsigned sid=arc4random();
        snprintf(sdp,sizeof(sdp),
            "v=0\r\no=- %u %u IN IP6 %s\r\ns=-\r\nc=IN IP6 %s\r\nt=0 0\r\n"
            "m=audio %d RTP/AVP 100 101\r\nb=AS:41\r\n"
            "a=rtpmap:100 AMR/8000\r\na=fmtp:100 mode-change-capability=2;max-red=0\r\n"
            "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-15\r\na=ptime:20\r\na=maxptime:80\r\na=sendrecv\r\n",
            sid,sid,call_ip6,call_ip6,CALL_RTP_PORT);
        call_respond("200 OK",sdp);
        call.state=2;call.answered=time(NULL);
        if (call.ui) call_ui_state("connecting %u",call.id);
        if (!med.run) {call_media(1);fflush(stdout);}  /* media from our 200 OK on: the ACK can come late or not at all */
        {FILE *f=fopen("/var/tmp/redwood-cell/call","w");if (f) {fprintf(f,"answered %ld\n",(long)call.answered);fclose(f);}}
    } else if (!call.out && call.state==2 && time(NULL)-call.answered>=30 && med.rx_pkts==0) {
        /* answered but never confirmed (A, 2026-11-14 03:25: our 200 OK went unanswered, the call stayed "answered" and
         * every new call got 486 Busy): give it up */
        call_end("no ACK and no voice in 30 s after our 200 OK");
    } else if (call.state>=2 && call.state!=10 && call.answered && time(NULL)-call.answered>=CALL_MAX_S) {
        call_bye();
        call_end("our BYE (an hour reached)");
    }
}

/* our BYE: Route from Record-Route, to the remote Contact */
static void
call_bye(void)
{
    {
        char m[3000],uri[400],route[700]="";int o;
        const char *a=strchr(call.contact,'<'),*e=a?strchr(a,'>'):NULL;
        if (a && e) snprintf(uri,sizeof(uri),"%.*s",(int)(e-a-1),a+1); else snprintf(uri,sizeof(uri),"%s",call.contact);
        for (const char *q=call.rr;(q=strstr(q,"Record-Route:"));q+=13) {
            const char *v=q+13;while (*v==' ') v++;const char *le=strstr(v,"\r\n");
            size_t L=strlen(route);snprintf(route+L,sizeof(route)-L,"Route: %.*s\r\n",(int)(le-v),v);
        }
        /* (a request of ours goes out on our own protected connection, port-c -> the P-CSCF's port-s, as REGISTER and
         * MESSAGE do; sent back on the P-CSCF's connection to our port-s it was dropped silently, 2026-11-13 22:42) */
        o=snprintf(m,sizeof(m),"BYE %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\n%s"
            "Max-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %u BYE\r\nContent-Length: 0\r\n\r\n",
            uri,call_ip6,call_portc,arc4random(),route,call.to,call.from,call.cid,call.bye_cseq++);
        if (send(call_sock,m,(size_t)o,0)<0) printf("%s call: BYE send failed: %s\n",ts_now(),strerror(errno));
        else printf("%s call: --> BYE (on the registration connection)\n",ts_now());
    }
}


/*
 * Calls, step 5 (2026-11-14, A): calling out. The call screen (RedwoodPhone, from the Phone app's dial) writes
 * "dial TAG NUMBER"; we send the INVITE with our AMR offer on the registration connection (port-c, the Service-Route
 * as Route lines, as MESSAGE does), follow 1xx/2xx, ACK the answer (to its Contact, Record-Route reversed), PRACK a
 * reliable 18x, and start the voice on the first answer that has SDP (183 early media or the 200).
 */
static void call_sdp_from(const char *r)
{
    const char *b=strstr(r,"\r\n\r\n");if (!b) return;
    const char *c=strstr(b,"\nc=IN IP6 "),*mm=strstr(b,"\nm=audio ");
    if (c) sscanf(c+10,"%95s",call.sdp_c);
    if (mm) sscanf(mm+9,"%95s",call.sdp_m);
}

/* E.164 country code from the IMSI's MCC (its first three digits); NULL when unknown (then numbers go out as dialled) */
static const char *mcc_country_code(const char *imsi)
{
    static const struct {int mcc;const char *cc;} t[]={
        {404,"91"},{405,"91"},{406,"91"},{310,"1"},{311,"1"},{312,"1"},{313,"1"},{314,"1"},{315,"1"},{316,"1"},{302,"1"},
        {234,"44"},{235,"44"},{262,"49"},{208,"33"},{222,"39"},{214,"34"},{505,"61"},{530,"64"},{440,"81"},{441,"81"},
        {450,"82"},{460,"86"},{454,"852"},{525,"65"},{502,"60"},{510,"62"},{515,"63"},{520,"66"},{452,"84"},{470,"880"},
        {410,"92"},{413,"94"},{429,"977"},{424,"971"},{420,"966"},{427,"974"},{419,"965"},{422,"968"},{426,"973"},
        {602,"20"},{655,"27"},{621,"234"},{639,"254"},{724,"55"},{722,"54"},{334,"52"},{730,"56"},{732,"57"},{716,"51"},
        {250,"7"},{255,"380"},{260,"48"},{286,"90"},{204,"31"},{206,"32"},{228,"41"},{232,"43"},{238,"45"},{240,"46"},
        {242,"47"},{244,"358"},{268,"351"},{272,"353"},{202,"30"},{425,"972"},{416,"962"},{415,"961"},{418,"964"},{432,"98"}};
    if (!imsi || strlen(imsi)<3) return NULL;
    int mcc=(imsi[0]-'0')*100+(imsi[1]-'0')*10+(imsi[2]-'0');
    for (size_t i=0;i<sizeof(t)/sizeof(t[0]);i++) if (t[i].mcc==mcc) return t[i].cc;
    return NULL;
}
static void call_dial(int sock,const char *tag,const char *number)
{
    if (call.state) {call_ui_state("ended 0 busy %s",tag);return;}
    char num[40];size_t n=0;
    for (const char *q=number;*q && n+1<sizeof(num);q++) if ((*q>='0' && *q<='9') || (*q=='+' && !n) || *q=='*' || *q=='#') num[n++]=*q;
    num[n]=0;
    /* the number as the network wants it (A, 2026-11-14): no fixed country. The SIM's MCC gives the country code;
     * 00... and +... are international already, a leading 0 is the national trunk prefix, a national number gets
     * +CC, and short codes (under 7 digits, or * / #) stay local, in the SIM's own IMS domain (phone-context). */
    char tel[96];const char *cc=mcc_country_code(cfg.imsi);
    if (num[0]=='+') snprintf(tel,sizeof(tel),"tel:%s",num);
    else if (!strncmp(num,"00",2) && n>4) snprintf(tel,sizeof(tel),"tel:+%s",num+2);
    else if (n<7 || strchr(num,'*') || strchr(num,'#') || !cc) snprintf(tel,sizeof(tel),"tel:%s;phone-context=%s",num,cfg.realm);
    else if (num[0]=='0') snprintf(tel,sizeof(tel),"tel:+%s%s",cc,num+1);
    else snprintf(tel,sizeof(tel),"tel:+%s%s",cc,num);
    memset(&call,0,sizeof(call));
    call.out=1;call.ui=1;call.id=++call_seq;call.state=10;call.at=time(NULL);call.bye_cseq=2;
    snprintf(call.branch,sizeof(call.branch),"z9hG4bK%08x",arc4random());
    snprintf(call.cid,sizeof(call.cid),"%08x%08x@[%s]",arc4random(),arc4random(),cfg.ip6);
    snprintf(call.to,sizeof(call.to),"<%s>;tag=%08x",sms_impu[0]?sms_impu:"sip:anonymous@anonymous.invalid",arc4random());
    snprintf(call.from,sizeof(call.from),"<%s>",tel);         /* (the far end: To of our requests, its tag added later) */
    snprintf(call.contact,sizeof(call.contact),"<%s>",tel);
    snprintf(call.tag,sizeof(call.tag),"%s",tag);
    call_ui_number(num[0]?num:"Unknown");
    call_ui_state("dialing %u %s",call.id,tag);
    char sdp[700];unsigned sid=arc4random();
    snprintf(sdp,sizeof(sdp),
        "v=0\r\no=- %u %u IN IP6 %s\r\ns=-\r\nc=IN IP6 %s\r\nt=0 0\r\n"
        "m=audio %d RTP/AVP 100 101\r\nb=AS:41\r\n"
        "a=rtpmap:100 AMR/8000\r\na=fmtp:100 mode-change-capability=2;max-red=0\r\n"
        "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-15\r\na=ptime:20\r\na=maxptime:80\r\na=sendrecv\r\n",
        sid,sid,call_ip6,call_ip6,CALL_RTP_PORT);
    char m[4000];int o=snprintf(m,sizeof(m),
        "INVITE %s SIP/2.0\r\n"
        "Via: SIP/2.0/TCP [%s]:%d;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n%s"
        "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 INVITE\r\n"
        "Contact: <sip:%s@[%s]:%d;transport=tcp>;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n"
        "Accept-Contact: *;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n"
        "P-Preferred-Service: urn:urn-7:3gpp-service.ims.icsi.mmtel\r\n"
        "P-Preferred-Identity: <%s>\r\n"
        "Allow: INVITE,ACK,CANCEL,BYE,UPDATE,PRACK,OPTIONS,MESSAGE\r\nSupported: replaces\r\n"
        "Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s",
        tel,call_ip6,call_portc,call.branch,sms_route,call.to,call.from,call.cid,
        call_user,call_ip6,call_ports,sms_impu[0]?sms_impu:"sip:anonymous@anonymous.invalid",strlen(sdp),sdp);
    if (send(sock,m,(size_t)o,0)<0) {printf("%s call: INVITE send failed: %s\n",ts_now(),strerror(errno));call_end("could not send");return;}
    printf("%s call: --> INVITE (outgoing call %u)\n",ts_now(),call.id);
}
/* the ACK (and PRACK) of ours, in the dialog: Route = Record-Route reversed (kept in call.rr already reversed) */
static void call_out_request(int sock,const char *method,unsigned cseq,const char *extra)
{
    char m[3000],uri[400],route[700]="";int o;
    const char *a=strchr(call.contact,'<'),*e=a?strchr(a,'>'):NULL;
    if (a && e) snprintf(uri,sizeof(uri),"%.*s",(int)(e-a-1),a+1); else snprintf(uri,sizeof(uri),"%s",call.contact);
    for (const char *q=call.rr;(q=strstr(q,"Record-Route:"));q+=13) {
        const char *v=q+13;while (*v==' ') v++;const char *le=strstr(v,"\r\n");
        size_t L=strlen(route);snprintf(route+L,sizeof(route)-L,"Route: %.*s\r\n",(int)(le-v),v);
    }
    if (!route[0]) snprintf(route,sizeof(route),"%s",sms_route);
    o=snprintf(m,sizeof(m),"%s %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\n%s"
        "Max-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %u %s\r\n%sContent-Length: 0\r\n\r\n",
        method,uri,call_ip6,call_portc,arc4random(),route,call.to,call.from,call.cid,cseq,method,extra?extra:"");
    if (send(sock,m,(size_t)o,0)<0) printf("%s call: %s send failed: %s\n",ts_now(),method,strerror(errno));
    else printf("%s call: --> %s\n",ts_now(),method);
}
/* a response to our INVITE; returns 1 if it was one */
static int call_out_response(int sock,const char *r)
{
    char ci[200],cq[80];
    if (!hdr(r,"Call-ID",ci,sizeof(ci))) return 0;
    if ((!call.out || !call.state || strcmp(ci,call.cid)) && !strncmp(r,"SIP/2.0 2",9) && hdr(r,"CSeq",cq,sizeof(cq)) && strstr(cq,"INVITE")) {
        /* an answer to an INVITE of ours we have already given up (05:34: the callee picked up after we cancelled and
         * the call ran in the background): ACK it and hang up at once */
        char to[400],fr[400],ct[400],rr[600]="",uri[400]="";
        hdr(r,"To",to,sizeof(to));hdr(r,"From",fr,sizeof(fr));
        if (hdr(r,"Contact",ct,sizeof(ct))) {const char *a=strchr(ct,'<'),*e=a?strchr(a,'>'):NULL;if (a && e) snprintf(uri,sizeof(uri),"%.*s",(int)(e-a-1),a+1);}
        const char *list[8];int nr=0;size_t L=0;
        for (const char *q=r;(q=strstr(q,"\r\nRecord-Route:")) && nr<8;q+=2) list[nr++]=q+2;
        for (int k=nr-1;k>=0;k--) {const char *e=strstr(list[k],"\r\n");const char *v=list[k]+13;while (*v==' ') v++;L+=(size_t)snprintf(rr+L,sizeof(rr)-L,"Route: %.*s\r\n",(int)(e-v),v);}
        if (!uri[0]) return 1;
        char m[3000];
        for (int k=0;k<2;k++) {
            int o=snprintf(m,sizeof(m),"%s %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\n%sMax-Forwards: 70\r\n"
                "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\nContent-Length: 0\r\n\r\n",
                k?"BYE":"ACK",uri,call_ip6,call_portc,arc4random(),rr[0]?rr:sms_route,fr,to,ci,k?"2 BYE":"1 ACK");
            send(sock,m,(size_t)o,0);
        }
        printf("%s call: an answer to a call already ended: ACK + BYE\n",ts_now());
        return 1;
    }
    if (!call.out || !call.state || strcmp(ci,call.cid)) return 0;
    if (!hdr(r,"CSeq",cq,sizeof(cq))) return 1;
    if (!strstr(cq,"INVITE")) {                             /* PRACK/BYE answers */
        int c2=atoi(r+8);
        if (c2==481 && call.state!=3) {                     /* the network lost the call: never stay on "calling" */
            printf("%s call: <-- 481 to our %s: the call is gone\n",ts_now(),strstr(cq,"PRACK")?"PRACK":"request");
            call_cancel(sock);call_end("the network lost the call");
        }
        return 1;
    }
    int code=atoi(r+8);
    printf("%s call: <-- %d to our INVITE\n",ts_now(),code);
    char to[400];
    if (hdr(r,"To",to,sizeof(to)) && strstr(to,"tag=")) snprintf(call.from,sizeof(call.from),"%s",to);
    if (code>=180 && code<300) {
        char ct[400];if (hdr(r,"Contact",ct,sizeof(ct))) snprintf(call.contact,sizeof(call.contact),"%s",ct);
        /* Record-Route reversed for our own requests */
        char rr[600]="";size_t L=0;const char *list[8];int nr=0;
        for (const char *q=r;(q=strstr(q,"\r\nRecord-Route:")) && nr<8;q+=2) list[nr++]=q+2;
        for (int k=nr-1;k>=0;k--) {const char *e=strstr(list[k],"\r\n");L+=(size_t)snprintf(rr+L,sizeof(rr)-L,"%.*s\r\n",(int)(e-list[k]),list[k]);}
        if (nr) snprintf(call.rr,sizeof(call.rr),"%s",rr);
        if (strstr(r,"\r\n\r\nv=0")) {call_sdp_from(r);if (!med.run) call_media(1);}
        char req[80],rs[40];
        if (code<200 && hdr(r,"Require",req,sizeof(req)) && strstr(req,"100rel") && hdr(r,"RSeq",rs,sizeof(rs))) {
            unsigned rseq=(unsigned)atoi(rs);
            if (rseq>call.rseq_acked) {char x[80];snprintf(x,sizeof(x),"RAck: %u 1 INVITE\r\n",rseq);call_out_request(sock,"PRACK",call.bye_cseq++,x);call.rseq_acked=rseq;}
        }
    }
    if (code>=180 && code<200 && call.state==10) {call.state=2;call_ui_state("alerting %u",call.id);}
    else if (code>=200 && code<300) {
        call_out_request(sock,"ACK",1,NULL);
        if (call.state!=3) {call.state=3;call.answered=time(NULL);
            {FILE *f=fopen("/var/tmp/redwood-cell/call","w");if (f) {fprintf(f,"up %ld\n",(long)call.answered);fclose(f);}}
            if (!med.run) call_media(1);
            call_ui_state("up %u",call.id);}
    } else if (code>=300) {
        /* the ACK of a failure goes to the P-CSCF hop with the INVITE's branch */
        char m[1600];int o=snprintf(m,sizeof(m),"ACK %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=%s;rport\r\nMax-Forwards: 70\r\n%s"
            "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n",
            strchr(call.contact,'<')?"":call.contact,call_ip6,call_portc,call.branch,sms_route,call.to,call.from,call.cid);
        {char uri[200];const char *a=strchr(call.contact,'<'),*e=a?strchr(a,'>'):NULL;
         if (a && e) {snprintf(uri,sizeof(uri),"%.*s",(int)(e-a-1),a+1);
           o=snprintf(m,sizeof(m),"ACK %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=%s;rport\r\nMax-Forwards: 70\r\n%s"
            "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n",
            uri,call_ip6,call_portc,call.branch,sms_route,call.to,call.from,call.cid);}}
        send(sock,m,(size_t)o,0);
        char why[64];snprintf(why,sizeof(why),code==486||code==600?"busy":code==487?"cancelled":"failed %d",code);
        call_end(why);
    }
    return 1;
}
/* our CANCEL of a call still ringing out (same branch as the INVITE) */
static void call_cancel(int sock)
{
    char m[1600],uri[200];
    const char *a=strchr(call.contact,'<'),*e=a?strchr(a,'>'):NULL;
    if (a && e) snprintf(uri,sizeof(uri),"%.*s",(int)(e-a-1),a+1); else snprintf(uri,sizeof(uri),"%s",call.contact);
    char to[400];snprintf(to,sizeof(to),"%s",call.from);char *t=strstr(to,";tag=");if (t) *t=0;
    int o=snprintf(m,sizeof(m),"CANCEL %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=%s;rport\r\nMax-Forwards: 70\r\n%s"
        "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 CANCEL\r\nContent-Length: 0\r\n\r\n",
        uri,call_ip6,call_portc,call.branch,sms_route,call.to,to,call.cid);
    send(sock,m,(size_t)o,0);printf("%s call: --> CANCEL\n",ts_now());
}

/* the call screen's commands */
static void
call_ui_poll(void)
{
    /* the call screen's Speaker button (libredwood in InCallService posts these) */
    static int tsp=-1,tea=-1;
    if (tsp<0) {notify_register_check("com.redwood.call.speaker",&tsp);notify_register_check("com.redwood.call.earpiece",&tea);}
    int ch=0;
    if (tsp>=0 && notify_check(tsp,&ch)==0 && ch && call.state) {med.route=0;printf("%s call: screen -> speaker\n",ts_now());}
    ch=0;
    if (tea>=0 && notify_check(tea,&ch)==0 && ch && call.state) {med.route=1;printf("%s call: screen -> earpiece\n",ts_now());}
    DIR *d=opendir(CALL_UI_DIR);if (!d) return;
    struct dirent *e;
    while ((e=readdir(d))) {
        if (strncmp(e->d_name,"cmd.",4)) continue;
        char fn[300],line[100]="";unsigned id=0;struct stat cst;
        snprintf(fn,sizeof(fn),CALL_UI_DIR "/%s",e->d_name);
        int stale=!stat(fn,&cst) && time(NULL)-cst.st_mtime>10;
        FILE *f=fopen(fn,"r");if (f) {if (!fgets(line,sizeof(line),f)) line[0]=0;fclose(f);}
        unlink(fn);
        if (stale && !strncmp(line,"dial ",5)) {   /* (a dial from before we could take it: the user has long given up) */
            char tg[80]="";sscanf(line,"%*s %79s",tg);printf("%s call: screen -> dial (stale, dropped)\n",ts_now());
            call_ui_state("ended 0 stale %s",tg);continue;
        }
        if (!strncmp(line,"cancel ",7)) {           /* the user ended a call still being dialled */
            char tg[80]="";sscanf(line,"%*s %79s",tg);printf("%s call: screen -> cancel\n",ts_now());
            if (call.out && call.state && call.state!=3 && !strcmp(tg,call.tag)) {call_cancel(call_sock);call_end("cancelled here");}
            else if (call.out && call.state==3 && !strcmp(tg,call.tag)) {call_bye();call_end("hung up here");}
            continue;
        }
        char verb[20]="";if (sscanf(line,"%19s %u",verb,&id)<1) continue;
        printf("%s call: screen -> %s\n",ts_now(),verb);
        if (!strcmp(verb,"dial")) {char tag[80],num[64];if (sscanf(line,"%*s %79s %63s",tag,num)==2) call_dial(call_sock,tag,num);continue;}
        if (!call.state || id!=call.id) continue;           /* (an old call's button) */
        if (!strcmp(verb,"answer") && call.state==1) call.answer_req=1;
        else if (!strcmp(verb,"route")) {char w[20]="";sscanf(line,"%*s %*u %19s",w);med.route=strcmp(w,"speaker")!=0;}
        else if (!strcmp(verb,"mute")) {unsigned on=0;sscanf(line,"%*s %*u %u",&on);med.muted=(int)on;}
        else if (!strcmp(verb,"end")) {
            if (call.out && call.state!=3) {call_cancel(call_sock);call_end("cancelled here");}
            else if (call.state==1) {call_respond("603 Decline",NULL);call_end("declined here");}
            else {call_bye();call_end("hung up here");}
        }
    }
    closedir(d);
}


/* leaving with a call up (the registration connection is gone): close it at the network as far as we still can, and
 * on the call screen, so neither side keeps a call nobody has (afterwards: "busy") */
static void call_abort(const char *why)
{
    if (!call.state) return;
    if (call.out && call.state!=3) call_cancel(call_sock);
    else if (call.state==1) call_respond("480 Temporarily Unavailable",NULL);
    else call_bye();
    call_end(why);
}
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
{   /* SIGTERM/SIGINT: our SAs and policies go with us (the next start makes its own). A call in progress is closed
     * first (A, 2026-11-14: a restart mid-call left it open on the network: every later call got "busy") */
    (void)sig;
    if (call.state==1) call_respond("480 Temporarily Unavailable",NULL);
    else if (call.state>=2) call_bye();
    if (call.state && call.ui) call_ui_state("ended %u the phone service restarted",call.id);unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();_exit(0);
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    call_seq=(unsigned)(time(NULL)%1000000)*10;
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
    /* The same ports every time (A, 2026-11-14): our Contact (address + port-s) is then the same after a restart, and
     * the network replaces our registration instead of keeping the old one beside it. With old ones kept, an incoming
     * call was offered to the dead contacts first and reached us only after the caller had heard "busy". */
    struct sockaddr_in6 a={.sin6_len=sizeof(a),.sin6_family=AF_INET6,.sin6_port=htons(5060),.sin6_addr=pc6};
    /* (05:46: a quick restart found port-c still in TIME_WAIT, took a free port, and the old contact stayed at the
     * network: the next call was "busy") -- the fixed port is retried for up to 60 s before any other is taken */
    int s=-1;
    for (int tries=0;tries<30;tries++) {
        s=bound(tcp?SOCK_STREAM:SOCK_DGRAM,IMS_PORT_C);
        if (s>=0 && !connect(s,(struct sockaddr *)&a,sizeof(a))) break;
        if (tries==0 || tries==29) printf("port-c %d: %s%s\n",IMS_PORT_C,strerror(errno),tries==29?"; a free port instead":"; trying again for up to 60 s");
        if (s>=0) close(s);
        s=-1;sleep(2);
    }
    if (s<0) {
        s=bound(tcp?SOCK_STREAM:SOCK_DGRAM,0);if (s<0) {perror("bind");return 1;}
        if (connect(s,(struct sockaddr *)&a,sizeof(a))) {perror("connect");isim_close();return 1;}
    }
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
    /* our previous contact, if it was another (address or port changed): removed in this same REGISTER (expires=0),
     * so the network never offers a call to a dead contact first ("busy", late rings) */
    char my_contact[200],stale_contact[300]="";
    snprintf(my_contact,sizeof(my_contact),"<sip:%s@[%s]:%d;transport=%s>",cfg.imsi,cfg.ip6,ports,tcp?"tcp":"udp");
    {FILE *q=fopen("/var/root/ims-last-contact","r");char old[200]="";
     if (q) {if (!fgets(old,sizeof(old),q)) old[0]=0;fclose(q);old[strcspn(old,"\r\n")]=0;}
     if (old[0] && strcmp(old,my_contact)) {snprintf(stale_contact,sizeof(stale_contact),"Contact: %s;expires=0\r\n",old);printf("    the previous contact is removed in this REGISTER\n");}}
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
        "%s"
        "Content-Length: 0\r\n\r\n",uri,T,cfg.ip6,ports,arc4random(),common,user,cfg.realm,nonce,uri,resp,cnonce,secc,secs,stale_contact);
    printf("--> REGISTER 2 (protected) to port %u\n",pports);
    if (send(s,m,(size_t)n,0)<0) {perror("send (protected)");ipsec_flush();return 1;}
    do n=recv_msg(s,r,sizeof(r),10); while (n>0 && !strncmp(r,"SIP/2.0 1",9));
    if (n<=0) {printf("no answer to the protected REGISTER (the SAs stay for a look: redwood-ims --flush removes them)\n");return 1;}
    printf("<-- %.*s\n",(int)(strstr(r,"\r\n")-r),r);show(r);
    if (strncmp(r,"SIP/2.0 200",11)) {ipsec_flush();return 1;}
    printf("REGISTERED\n");
    {FILE *q=fopen("/var/root/ims-last-contact","w");if (q) {fprintf(q,"%s\n",my_contact);fclose(q);chmod("/var/root/ims-last-contact",0600);}}
    {FILE *q=fopen("/var/tmp/redwood-cell/ims-registered","w");if (q) {fchmod(fileno(q),0644);fprintf(q,"%lld\n",(long long)time(NULL));fclose(q);}}
    sms_keep_registration(r);
    if (flag(argc,argv,"--once")) return 0;

    /* 5. stay: show what the network sends, answer requests with 200 */
    int cs[8];int ncs=0;time_t registered_at=time(NULL);
    snprintf(call_ip6,sizeof(call_ip6),"%s",cfg.ip6);call_ports=ports;call_user=cfg.imsi;call_sock=s;call_portc=portc;
    for (;;) {
        /* a fresh registration well before the 7200 s expiry: leave cleanly, redwood-cellulard starts us again */
        if (time(NULL)-registered_at>6600 && !call.state) {printf("re-registration due: leaving for a fresh start\n");unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}
        struct pollfd p[12];int k=0;p[k++]=(struct pollfd){s,POLLIN,0};p[k++]=(struct pollfd){us,POLLIN,0};if (ls>=0) p[k++]=(struct pollfd){ls,POLLIN,0};
        int base=k;for (int i=0;i<ncs;i++) p[k++]=(struct pollfd){cs[i],POLLIN,0};
        {int pend=0;for (int j=0;j<k;j++) if (j!=1 && j!=2 && recv_pending(p[j].fd)) pend=1;
         if (poll(p,(nfds_t)k,pend?0:call.state?200:1000)<0) break;}
        {   /* keep the registration's connection open: a CRLF CRLF ping every 25 s (RFC 5626 3.5.1). Without it the
             * network dropped the idle TCP connection within minutes, and a text sent into it got no answer at all
             * ("send: failed", 2026-11-13 18:43 and 18:46). */
            static time_t ka;time_t t=time(NULL);
            if (t-ka>=25) {ka=t;if (send(s,"\r\n\r\n",4,0)<0) {printf("keep-alive: the registration connection is gone (%s)\n",strerror(errno));call_abort("connection lost");unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}}
            /* (2026-11-14) the P-CSCF's own connections to our port-s, too: a dead one is closed at once, so the next
             * call comes on a fresh connection instead of waiting out a dead one (late rings, lost CANCELs) */
            static time_t kc;
            if (t-kc>=20) {kc=t;
                for (int j=0;j<ncs;j++) if (send(cs[j],"\r\n\r\n",4,0)<0) {
                    printf("keep-alive: a P-CSCF connection is gone (%s): closed\n",strerror(errno));
                    if (call.state && call.fd==cs[j]) call.fd=s;
                    close(cs[j]);cs[j]=cs[--ncs];j--;
                }
            }
        }
        ims_cmd_poll(s,portc);
        call_tick();
        if (ls>=0 && (p[2].revents&POLLIN) && ncs<8) {int c=accept(ls,NULL,NULL);if (c>=0) {cs[ncs++]=c;printf("the P-CSCF connected to port-s\n");}}
        for (int i=0;i<k;i++) {
            if (i!=1 && i!=2 && !(p[i].revents&(POLLIN|POLLHUP)) && recv_pending(p[i].fd)) p[i].revents|=POLLIN;   /* kept messages */
            if (!(p[i].revents&(POLLIN|POLLHUP)) || (ls>=0 && i==2)) continue;
            struct sockaddr_in6 from;socklen_t fl=sizeof(from);ssize_t g;
            if (i==1) {g=recvfrom(us,r,sizeof(r)-1,0,(struct sockaddr *)&from,&fl);if (g>0) r[g]=0;}
            else g=recv_msg(p[i].fd,r,sizeof(r),2);
            if (g<=0) {if (i>=base) {if (call.state && call.fd==cs[i-base]) call.fd=s;close(cs[i-base]);cs[i-base]=cs[--ncs];} else if (i==0) {printf("the registration connection closed\n");call_abort("connection lost");unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;} break;}
            printf("%s <-- %zd bytes\n",ts_now(),g);show(r);printf("\n");
            if (strncmp(r,"SIP/2.0",7) && !strncmp(r,"INVITE ",7) && i!=1 && call_invite(p[i].fd,r)) {
                char fn[96];snprintf(fn,sizeof(fn),"/var/root/ims-invite-%ld.txt",(long)time(NULL));
                int fd=open(fn,O_WRONLY|O_CREAT|O_TRUNC,0600);if (fd>=0) {(void)write(fd,r,(size_t)g);close(fd);}
                continue;
            }
            if (!strncmp(r,"SIP/2.0",7) && call_out_response(s,r)) continue;
            if (strncmp(r,"SIP/2.0",7) && i!=1 && call_request(p[i].fd,r)) continue;
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
