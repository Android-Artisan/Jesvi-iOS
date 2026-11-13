/* MIT. redwood-ims: a small IMS (VoLTE signalling) client for SMS over IP on the S20 port.
 * Reads /var/tmp/redwood-cell/ims (interface, IMS address, P-CSCFs, IMSI) written by redwood-cellulard.
 * Phase 1 (this file so far): initial REGISTER with a Security-Client offer; prints the 401 challenge. */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef IPV6_BOUND_IF
#define IPV6_BOUND_IF 125
#endif
#define PF_KEY_V2 2
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
{   /* print a SIP message with the IMSI masked */
    const char *p=m;size_t L=strlen(cfg.imsi),E=strlen(cfg.imei)>=14?14:0;
    while (*p) {if (L && !strncmp(p,cfg.imsi,L)) {printf("%.5s**********",cfg.imsi);p+=L;}
        else if (E && !strncmp(p,cfg.imei,8)) {printf("<imei>");p+=8;} else putchar(*p++);}
}
static int sock_bound(int type,int port)
{
    unsigned idx=if_nametoindex(cfg.ifn);int s=socket(AF_INET6,type,0);int one=1;
    setsockopt(s,IPPROTO_IPV6,IPV6_BOUND_IF,&idx,sizeof(idx));setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in6 l={.sin6_len=sizeof(l),.sin6_family=AF_INET6,.sin6_port=htons((uint16_t)port)};inet_pton(AF_INET6,cfg.ip6,&l.sin6_addr);
    if (bind(s,(struct sockaddr *)&l,sizeof(l))) {perror("bind");close(s);return -1;}
    return s;
}
static const char *opt(int argc,char **argv,const char *k,const char *def)
{for (int i=1;i+1<argc;i++) if (!strcmp(argv[i],k)) return argv[i+1];return def;}
static int flag(int argc,char **argv,const char *k) {for (int i=1;i<argc;i++) if (!strcmp(argv[i],k)) return 1;return 0;}
int main(int argc,char **argv)
{
    if (argc>1 && !strcmp(argv[1],"--pfkey")) {
        int k=socket(PF_KEY,SOCK_RAW,PF_KEY_V2);printf("PF_KEY socket: %s\n",k>=0?"available":strerror(errno));return k<0;
    }
    if (load_cfg()) {fprintf(stderr,"redwood-ims: no IMS PDN yet (/var/tmp/redwood-cell/ims)\n");return 1;}
    uint32_t spic=arc4random()|0x1000,spis=spic+1;int portc=5100,ports=5101;
    int tcp=flag(argc,argv,"--tcp"),pci=atoi(opt(argc,argv,"--pcscf","0"))%cfg.npc;
    const char *ua=opt(argc,argv,"--ua","Samsung_SM-G980F_G980FXXSNHYB7_IMS_6.0"),*alg=opt(argc,argv,"--alg","hmac-md5-96"),*pani=opt(argc,argv,"--pani","");
    int s=sock_bound(tcp?SOCK_STREAM:SOCK_DGRAM,tcp?0:5060);if (s<0) return 1;
    struct sockaddr_in6 a={.sin6_len=sizeof(a),.sin6_family=AF_INET6,.sin6_port=htons(5060)};inet_pton(AF_INET6,cfg.pcscf[pci],&a.sin6_addr);
    if (tcp && connect(s,(struct sockaddr *)&a,sizeof(a))) {perror("connect");return 1;}
    struct sockaddr_in6 me;socklen_t ml=sizeof(me);getsockname(s,(struct sockaddr *)&me,&ml);int myport=ntohs(me.sin6_port);
    char m[3000];uint32_t br=arc4random(),tag=arc4random(),cid=arc4random();
    int n=snprintf(m,sizeof(m),
        "REGISTER sip:%s SIP/2.0\r\n"
        "Via: SIP/2.0/%s [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%08x\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %08x@[%s]\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Contact: <sip:%s@[%s]:%d>;+sip.instance=\"<urn:gsma:imei:%.8s-%.6s-0>\";+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\";+g.3gpp.smsip;audio;expires=600000\r\n"
        "Authorization: Digest username=\"%s@%s\",realm=\"%s\",uri=\"sip:%s\",nonce=\"\",response=\"\",algorithm=AKAv1-MD5\r\n"
        "Security-Client: ipsec-3gpp;prot=esp;mod=trans;spi-c=%u;spi-s=%u;port-c=%d;port-s=%d;ealg=null;alg=%s\r\n"
        "%s%s%s"
        "Require: sec-agree\r\nProxy-Require: sec-agree\r\n"
        "Supported: path,sec-agree\r\n"
        "Allow: MESSAGE,OPTIONS,NOTIFY\r\n"
        "User-Agent: %s\r\n"
        "Expires: 600000\r\n"
        "Content-Length: 0\r\n\r\n",
        cfg.realm,tcp?"TCP":"UDP",cfg.ip6,tcp?myport:5060,br,cfg.imsi,cfg.realm,tag,cfg.imsi,cfg.realm,cid,cfg.ip6,cfg.imsi,cfg.ip6,ports,cfg.imei,cfg.imei+8,
        cfg.imsi,cfg.realm,cfg.realm,cfg.realm,spic,spis,portc,ports,alg,*pani?"P-Access-Network-Info: ":"",pani,*pani?"\r\n":"",ua);
    printf("--> REGISTER to [%s] over %s (%d bytes)\n",cfg.pcscf[pci],tcp?"TCP":"UDP",n);show(m);
    struct timeval tv={4,0};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    for (int t=0;t<3;t++) {
        if ((tcp?send(s,m,(size_t)n,0):sendto(s,m,(size_t)n,0,(struct sockaddr *)&a,sizeof(a)))<0) {perror("send");return 1;}
        if (tcp) break;
        char pk[8];if (recv(s,pk,sizeof(pk),MSG_PEEK)>0) break;printf("(no answer, resend)\n");
    }
    tv.tv_sec=10;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    for (int k=0;k<3;k++) {char b[4096];ssize_t r=recv(s,b,sizeof(b)-1,0);if (r<=0) {printf("no answer\n");break;}
        b[r]=0;printf("<-- %zd bytes\n",r);show(b);if (strncmp(b,"SIP/2.0 1",9)) break;}
    return 0;
}
