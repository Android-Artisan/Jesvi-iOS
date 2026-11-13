/*
 * Calls, step 3 (2026-11-13, A): the call's voice on the AP. The S5123's own voice engine does not take the RTP stream
 * (the caller's packets, dport 50010/50011, arrive at the AP as ordinary data on the IMS PDN: "SBD RX ch 11"), so the
 * AP plays the part of the voice engine, as any VoIP app does:
 *   in   RTP (AMR-NB, PT 100, RFC 4867 bandwidth-efficient: our SDP answer has no octet-align) -> opencore-amr decoder
 *        -> 8 kHz -> 48 kHz stereo -> /dev/redwood-audio (the speaker driver's raw input, sys/redwood_audio.h)
 *   out  /dev/redwood-mic (48 kHz mono, docs/x1slte-audio.md) -> 8 kHz -> AMR-NB 12.2 -> RTP to the SDP's c=/m=
 * opencore-amr 0.1.6 (Apache-2.0), built static for iOS: work/third_party/amr-build/out (see tools/cellular/build-ims.sh).
 */
#include <math.h>
#include <sys/sysctl.h>
#include <pthread.h>
#include <net/if_dl.h>
#include <opencore-amrnb/interf_dec.h>
#include <opencore-amrnb/interf_enc.h>

#define MEDIA_PT 100
static const int amr_bits[16] = { 95, 103, 118, 134, 148, 159, 204, 244, 39, 0, 0, 0, 0, 0, 0, 0 };

/* the jitter buffer: decoded 8 kHz samples, played out at a steady 48 kHz by media_play (the speaker driver repeats
 * its ring when nothing new comes, so bursts written as they arrived stuttered: the user, 2026-11-14 00:04) */
#define FIFO_N 8000                                         /* 1 s at 8 kHz */
static int16_t fifo[FIFO_N];
static volatile unsigned fifo_w, fifo_r;
static pthread_mutex_t fifo_mx = PTHREAD_MUTEX_INITIALIZER;

static struct {
    volatile int run;
    pthread_t play;
    unsigned underruns, concealed;
    float mic_gain;
    volatile int muted;
    int sock, spk, mic;
    struct sockaddr_in6 peer;
    pthread_t rx, tx;
    unsigned rx_pkts, rx_frames, tx_pkts, rx_bad, tx_err, mic_bytes; int tx_errno;
} med = { .sock = -1, .spk = -1, .mic = -1 };

/* bit reader / writer, MSB first */
static unsigned getbits(const uint8_t *b, size_t *pos, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++, (*pos)++) v = (v << 1) | ((b[*pos >> 3] >> (7 - (*pos & 7))) & 1);
    return v;
}
static void putbits(uint8_t *b, size_t *pos, unsigned v, int n)
{
    for (int i = n - 1; i >= 0; i--, (*pos)++) {
        if ((v >> i) & 1) b[*pos >> 3] |= (uint8_t)(0x80 >> (*pos & 7));
    }
}


/* (A, 2026-11-14, quality) a windowed-sinc low-pass at 3.6 kHz for 48 kHz: the anti-alias filter before 48 -> 8 kHz
 * (was the mean of 6) and the interpolation filter after 8 -> 48 kHz (was linear), 72 taps (12 per phase), Hamming */
#define FIR_N 72
static float fir[FIR_N];
static void fir_init(void)
{
    double sum = 0;
    for (int i = 0; i < FIR_N; i++) {
        double x = i - (FIR_N - 1) / 2.0, fc = 3600.0 / 48000.0;
        double h = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
        h *= 0.54 - 0.46 * cos(2 * M_PI * i / (FIR_N - 1));
        fir[i] = (float)h; sum += h;
    }
    for (int i = 0; i < FIR_N; i++) fir[i] = (float)(fir[i] / sum);
}
static int16_t sat16(float v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)lrintf(v); }
static void rt_thread(int pri)
{   /* real-time priority: the audio threads may not wait behind the rest of the phone */
    struct sched_param sp = { .sched_priority = pri };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

static void *media_rx(void *arg)
{
    (void)arg;
    void *dec = Decoder_Interface_init();
    uint8_t pkt[1500];
    int16_t pcm[160];
    int have_seq = 0; uint16_t next_seq = 0;
    rt_thread(46);
    while (med.run) {
        struct pollfd p = { .fd = med.sock, .events = POLLIN };
        if (poll(&p, 1, 200) <= 0) continue;
        ssize_t n = recv(med.sock, pkt, sizeof(pkt), 0);
        if (n < 13 || (pkt[0] >> 6) != 2) { med.rx_bad++; continue; }
        if ((pkt[1] & 0x7f) != MEDIA_PT) continue;          /* (telephone-event, comfort noise of other types) */
        med.rx_pkts++;
        uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
        if (have_seq) {
            int16_t gap = (int16_t)(seq - next_seq);
            if (gap < 0) continue;                          /* (late or repeated: already concealed) */
            /* lost packets (1..5): the decoder conceals them (a bad-frame call per missing 20 ms), not silence */
            for (int g = 0; g < gap && g < 5; g++) {
                uint8_t nd[1] = { (uint8_t)(15 << 3) };
                Decoder_Interface_Decode(dec, nd, pcm, 1);
                med.concealed++;
                pthread_mutex_lock(&fifo_mx);
                for (int s2 = 0; s2 < 160; s2++) fifo[(fifo_w + (unsigned)s2) % FIFO_N] = pcm[s2];
                fifo_w += 160;
                pthread_mutex_unlock(&fifo_mx);
            }
        }
        have_seq = 1; next_seq = (uint16_t)(seq + 1);
        size_t h = 12 + 4 * (size_t)(pkt[0] & 0x0f);
        if (pkt[0] & 0x10) {                                /* header extension */
            if ((size_t)n < h + 4) continue;
            h += 4 + 4 * (size_t)((pkt[h + 2] << 8) | pkt[h + 3]);
        }
        if ((size_t)n <= h) continue;
        const uint8_t *pl = pkt + h;
        size_t plbits = ((size_t)n - h) * 8, pos = 4;      /* CMR */
        int ft[16], q[16], nf = 0, f;
        do {
            if (pos + 6 > plbits) break;
            f = (int)getbits(pl, &pos, 1); ft[nf] = (int)getbits(pl, &pos, 4); q[nf] = (int)getbits(pl, &pos, 1); nf++;
        } while (f && nf < 16);
        for (int i = 0; i < nf; i++) {
            uint8_t st[40] = { 0 };
            int bits = amr_bits[ft[i]];
            if (pos + (size_t)bits > plbits) break;
            st[0] = (uint8_t)((ft[i] << 3) | (q[i] << 2));
            size_t w = 8;
            for (int k = 0; k < bits; k++) putbits(st, &w, getbits(pl, &pos, 1), 1);
            Decoder_Interface_Decode(dec, st, pcm, ft[i] == 15 || !q[i]);
            med.rx_frames++;
            pthread_mutex_lock(&fifo_mx);
            if (fifo_w - fifo_r > FIFO_N - 160) fifo_r = fifo_w - 1600;    /* (far behind: keep the last 200 ms) */
            for (int s = 0; s < 160; s++) fifo[(fifo_w + (unsigned)s) % FIFO_N] = pcm[s];
            fifo_w += 160;
            pthread_mutex_unlock(&fifo_mx);
        }
    }
    Decoder_Interface_exit(dec);
    return NULL;
}


/* every 10 ms: 80 samples at 8 kHz from the FIFO (silence when it is empty), up to 48 kHz stereo, to the speaker; the
 * clock is the AP's, kept against the elapsed time so it never drifts. Playout starts once 60 ms are buffered. */
static void *media_play(void *arg)
{
    (void)arg;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long played = 0;                               /* 8 kHz samples */
    int primed = 0;
    int16_t pcm[80], out[80 * 6 * 2];
    float hist[FIR_N / 6] = { 0 };                          /* the last 12 input samples (8 kHz) */
    rt_thread(47);
    while (med.run) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        unsigned long due = (unsigned long)((now.tv_sec - t0.tv_sec) * 8000L + (now.tv_nsec - t0.tv_nsec) / 125000L);
        if (due < played + 80) { usleep(2000); continue; }
        pthread_mutex_lock(&fifo_mx);
        unsigned have = fifo_w - fifo_r;
        if (!primed && have >= 480) primed = 1;
        if (primed && have >= 80) {
            for (int s = 0; s < 80; s++) pcm[s] = fifo[(fifo_r + (unsigned)s) % FIFO_N];
            fifo_r += 80;
        } else {
            if (primed) { med.underruns++; primed = 0; }    /* refill 60 ms before going on */
            memset(pcm, 0, sizeof(pcm));
        }
        pthread_mutex_unlock(&fifo_mx);
        for (int s = 0; s < 80; s++) {                      /* 8 -> 48 kHz: polyphase FIR, gain 6 */
            memmove(hist + 1, hist, sizeof(hist) - sizeof(hist[0])); hist[0] = pcm[s];
            for (int k = 0; k < 6; k++) {
                float a = 0;
                for (int j = 0; j < FIR_N / 6; j++) a += hist[j] * fir[k + 6 * j];
                out[2 * (6 * s + k)] = out[2 * (6 * s + k) + 1] = sat16(6 * a);
            }
        }
        if (med.spk >= 0) (void)!write(med.spk, out, sizeof(out));
        played += 80;
        if (played % 40000 == 0)                            /* every 5 s */
            {printf("call: media (AP): in %u pkts, out %u pkts (%u failed: %s), mic %u bytes, %u underruns, %u concealed, mic gain %.1f, buffered %u ms\n",
                med.rx_pkts, med.tx_pkts, med.tx_err, med.tx_err ? strerror(med.tx_errno) : "-", med.mic_bytes, med.underruns,
                med.concealed, med.mic_gain, (fifo_w - fifo_r) / 8); fflush(stdout);}
        if (due > played + 800) played = due;               /* (stalled over 100 ms: skip ahead, do not burst) */
    }
    return NULL;
}

static void *media_tx(void *arg)
{
    (void)arg;
    void *enc = Encoder_Interface_init(0);
    int16_t in[960], pcm[160], prev[FIR_N] = { 0 };
    float dec8[160], gain = 2;
    size_t have = 0;
    rt_thread(45);
    uint16_t seq = (uint16_t)arc4random();
    uint32_t ts = arc4random(), ssrc = arc4random();
    int first = 1;
    while (med.run) {
        ssize_t n = med.mic >= 0 ? read(med.mic, (uint8_t *)in + have, sizeof(in) - have) : -1;
        if (n > 0) { have += (size_t)n; med.mic_bytes += (unsigned)n; }
        if (med.mic < 0) {                                  /* no mic: send silence at 20 ms */
            usleep(20000); memset(in, 0, sizeof(in)); have = sizeof(in);
        } else if (have < sizeof(in)) {
            usleep(4000);
            continue;
        }
        have = 0;
        float peak = 0;
        for (int s = 0; s < 160; s++) {                     /* 48 -> 8 kHz: FIR low-pass, every 6th */
            float a = 0;
            for (int j = 0; j < FIR_N; j++) {
                int idx = 6 * s + 5 - j;
                a += (idx >= 0 ? in[idx] : prev[FIR_N + idx]) * fir[j];
            }
            dec8[s] = a; if (fabsf(a) > peak) peak = fabsf(a);
        }
        memcpy(prev, in + 960 - FIR_N, sizeof(prev));
        /* AGC: speech peaks toward 12000, gain 1..6, fast down, slow up; quiet frames (noise) do not raise it */
        if (peak > 400) {
            float want = 12000.0f / peak; if (want > 6) want = 6; if (want < 1) want = 1;
            gain += (want - gain) * (want < gain ? 0.5f : 0.02f);
        }
        med.mic_gain = gain;
        for (int s = 0; s < 160; s++) pcm[s] = med.muted ? 0 : sat16(dec8[s] * gain);
        uint8_t st[64];
        int len = Encoder_Interface_Encode(enc, MR122, pcm, st, 0);
        if (len < 1) continue;
        int ftv = (st[0] >> 3) & 15, bits = amr_bits[ftv];
        uint8_t pkt[12 + 40] = { 0 };
        pkt[0] = 0x80; pkt[1] = (uint8_t)(MEDIA_PT | (first ? 0x80 : 0)); first = 0;
        pkt[2] = (uint8_t)(seq >> 8); pkt[3] = (uint8_t)seq; seq++;
        pkt[4] = (uint8_t)(ts >> 24); pkt[5] = (uint8_t)(ts >> 16); pkt[6] = (uint8_t)(ts >> 8); pkt[7] = (uint8_t)ts; ts += 160;
        pkt[8] = (uint8_t)(ssrc >> 24); pkt[9] = (uint8_t)(ssrc >> 16); pkt[10] = (uint8_t)(ssrc >> 8); pkt[11] = (uint8_t)ssrc;
        size_t pos = 0, rp = 8;
        putbits(pkt + 12, &pos, 15, 4);                     /* CMR: no request */
        putbits(pkt + 12, &pos, 0, 1); putbits(pkt + 12, &pos, (unsigned)ftv, 4); putbits(pkt + 12, &pos, 1, 1);
        for (int k = 0; k < bits; k++) putbits(pkt + 12, &pos, getbits(st, &rp, 1), 1);
        size_t plen = 12 + (pos + 7) / 8;
        if (sendto(med.sock, pkt, plen, 0, (struct sockaddr *)&med.peer, sizeof(med.peer)) > 0) med.tx_pkts++;
        else { med.tx_err++; med.tx_errno = errno; }
    }
    Encoder_Interface_exit(enc);
    return NULL;
}


/* a host route to the media peer through the IMS interface: redwood-cellulard routes only the P-CSCFs there, and
 * sendto() to the media gateway failed "No route to host" (A, 2026-11-14 03:33). The routing-socket ABI is macOS's
 * (net/route.h, not in the iOS SDK), as in live-modem.c tun_route6. */
struct rw_rt_metrics {uint32_t rmx_locks,rmx_mtu,rmx_hopcount;int32_t rmx_expire;uint32_t rmx_recvpipe,rmx_sendpipe,rmx_ssthresh,rmx_rtt,rmx_rttvar,rmx_pksent,rmx_state,rmx_filler[3];};
struct rw_rt_msghdr {u_short rtm_msglen;u_char rtm_version,rtm_type;u_short rtm_index;int rtm_flags,rtm_addrs;pid_t rtm_pid;int rtm_seq,rtm_errno,rtm_use;uint32_t rtm_inits;struct rw_rt_metrics rtm_rmx;};
static void media_route(const struct in6_addr *dst, const char *ifn)
{
    int rs = socket(PF_ROUTE, SOCK_RAW, 0);
    if (rs < 0) { printf("call: media: route socket: %s\n", strerror(errno)); return; }
    struct {struct rw_rt_msghdr h; struct sockaddr_in6 dst; struct sockaddr_dl gw; struct sockaddr_in6 mask;} m;
    memset(&m, 0, sizeof(m));
    m.h.rtm_msglen = sizeof(m); m.h.rtm_version = 5; m.h.rtm_type = 1;                  /* RTM_VERSION, RTM_ADD */
    m.h.rtm_flags = 0x1 | 0x800 | 0x4;                                                  /* RTF_UP|RTF_STATIC|RTF_HOST */
    m.h.rtm_addrs = 0x1 | 0x2 | 0x4; m.h.rtm_seq = 7;                                   /* DST, GATEWAY, NETMASK */
    m.dst.sin6_len = sizeof(m.dst); m.dst.sin6_family = AF_INET6; m.dst.sin6_addr = *dst;
    m.gw.sdl_len = sizeof(m.gw); m.gw.sdl_family = AF_LINK; m.gw.sdl_index = (u_short)if_nametoindex(ifn);
    m.mask.sin6_len = sizeof(m.mask); m.mask.sin6_family = AF_INET6; memset(&m.mask.sin6_addr, 0xff, 16);
    if (write(rs, &m, sizeof(m)) < 0 && errno != EEXIST) printf("call: media: route via %s: %s\n", ifn, strerror(errno));
    else printf("call: media: host route to the peer via %s\n", ifn);
    close(rs);
}

/* rport: the SDP's m= port; remote: its c= address; sock: a UDP socket bound to our RTP port */
static int media_start(int sock, const char *remote, unsigned rport)
{
    memset(&med.peer, 0, sizeof(med.peer));
    med.peer.sin6_len = sizeof(med.peer); med.peer.sin6_family = AF_INET6; med.peer.sin6_port = htons((uint16_t)rport);
    if (inet_pton(AF_INET6, remote, &med.peer.sin6_addr) != 1) return -1;
    med.sock = sock;
    media_route(&med.peer.sin6_addr, cfg.ifn);
    med.spk = open("/dev/redwood-audio", O_WRONLY | O_NONBLOCK);
    med.mic = open("/dev/redwood-mic", O_RDONLY | O_NONBLOCK);
    med.rx_pkts = med.rx_frames = med.tx_pkts = med.rx_bad = med.tx_err = med.mic_bytes = 0; med.tx_errno = 0;
    {   /* the earpiece (top amplifier only) unless the call screen says speaker; iOS's own audio leaves the device to us */
        int one = 1;
        sysctlbyname("kern.redwood.snd_call", NULL, NULL, &one, sizeof(one));
        sysctlbyname("kern.redwood.snd_route", NULL, NULL, &one, sizeof(one));
    }
    fifo_w = fifo_r = 0; med.underruns = med.concealed = 0; med.mic_gain = 2; med.muted = 0;
    fir_init();
    med.run = 1;
    pthread_create(&med.play, NULL, media_play, NULL);
    pthread_create(&med.rx, NULL, media_rx, NULL);
    pthread_create(&med.tx, NULL, media_tx, NULL);
    printf("call: media (AP): RTP :%d <-> [%s]:%u, speaker %s, mic %s\n", CALL_RTP_PORT, remote, rport,
        med.spk >= 0 ? "open" : strerror(errno), med.mic >= 0 ? "open" : "NOT open");
    return 0;
}

static void media_stop(void)
{
    if (!med.run) return;
    med.run = 0;
    pthread_join(med.rx, NULL); pthread_join(med.tx, NULL); pthread_join(med.play, NULL);
    printf("call: media (AP): stopped: in %u packets / %u frames (%u bad), out %u packets (%u failed: %s), mic %u bytes, %u underruns\n",
        med.rx_pkts, med.rx_frames, med.rx_bad, med.tx_pkts, med.tx_err, strerror(med.tx_errno), med.mic_bytes, med.underruns);
    if (med.spk >= 0) close(med.spk);
    if (med.mic >= 0) close(med.mic);
    if (med.sock >= 0) close(med.sock);
    med.spk = med.mic = med.sock = -1;
    {int zero = 0; sysctlbyname("kern.redwood.snd_call", NULL, NULL, &zero, sizeof(zero));
     sysctlbyname("kern.redwood.snd_route", NULL, NULL, &zero, sizeof(zero));}
}
