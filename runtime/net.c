/* WSOCK32 for online play: IPX sockets on a virtual LAN.
 *
 * NFS4's "IPX" network mode (LAN game browser, datagrams) runs on top of a
 * room on the relay server (web/relay.mjs): every room member has a 6-byte
 * node address, broadcasts reach the whole room, unicast reaches one peer.
 * In the browser the page carries packets (WebRTC peer-to-peer, relay as
 * fallback); natively a TCP connection to the relay does (RT_NET=host:port,
 * RT_ROOM=name, RT_NAME=player).
 *
 * Packet on the wire (after the 6-byte node header the relay adds/strips):
 *   u16 dst socket (BE) | u16 src socket (BE) | data
 *
 * TCP/IP (AF_INET) sockets are refused, so the game's TCP mode reports no network.
 */
#define _GNU_SOURCE
#include "rt.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#endif

#define WSAEWOULDBLOCK 10035
#define WSAEMSGSIZE 10040
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAENOTSOCK 10038
#define WSAENETDOWN 10050
#define WSAEINVAL 10022
#define AF_IPX_ 6
#define MAXPKT 2048
#define QLEN 256
#define NSOCK 16
#define SOCK_BASE 0x4000

static __thread uint32_t last_err;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static uint8_t self_node[6];
static int online;                       /* joined a room */
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

typedef struct { uint16_t len, sport; uint8_t src[6]; uint8_t data[MAXPKT]; } Pkt;
typedef struct {
    int used, nonblock, bound;
    uint32_t gen;                        /* bumped on close: wakes blocked readers */
    uint16_t port;                       /* host order */
    Pkt *q; int qh, qt;                  /* ring of QLEN */
} VSock;
static VSock socks[NSOCK];

static struct { uint64_t tx, rx, txb, rxb, drop; } stats;
static int net_trace = -1;
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
static void trace_pkt(const char *dir, const uint8_t node[6], uint16_t dport, uint16_t sport, const uint8_t *d, int n) {
    if (net_trace < 0) net_trace = getenv("RT_NET_TRACE") != NULL;
    if (!net_trace) return;
    char h[100]; int k = 0;
    for (int i = 0; i < n && i < 24; i++) k += snprintf(h + k, sizeof h - k, "%02x", d[i]);
    rt_log("pkt %.3f %s %02x%02x %04x->%04x %4d %s", fmod(tnow(), 1000.0), dir, node[4], node[5], sport, dport, n, h);
}

/* --------------------------------------------------------------- transport */
static void transport_send(const uint8_t dst[6], const uint8_t *pkt, int len);

/* a packet from the network (any thread) */
void net_deliver(const uint8_t src[6], const uint8_t *p, int len) {
    if (len < 4) return;
    static double loss = -1;            /* RT_NET_LOSS=percent: drop incoming packets (testing) */
    if (loss < 0) { const char *e = getenv("RT_NET_LOSS"); loss = e ? atof(e) / 100.0 : 0; }
    if (loss > 0 && (double)rand() / RAND_MAX < loss) { stats.drop++; return; }
    uint16_t dport = (uint16_t)(p[0] << 8 | p[1]), sport = (uint16_t)(p[2] << 8 | p[3]);
    trace_pkt("in ", src, dport, sport, p + 4, len - 4);
    pthread_mutex_lock(&mu);
    stats.rx++; stats.rxb += (uint64_t)len;
    for (int i = 0; i < NSOCK; i++) {
        VSock *s = &socks[i];
        if (!s->used || !s->bound || s->port != dport) continue;
        if (((s->qh + 1) % QLEN) == s->qt) { stats.drop++; break; }
        Pkt *k = &s->q[s->qh];
        k->len = (uint16_t)(len - 4 > MAXPKT ? MAXPKT : len - 4); k->sport = sport; memcpy(k->src, src, 6);
        memcpy(k->data, p + 4, k->len);
        s->qh = (s->qh + 1) % QLEN;
        pthread_cond_broadcast(&cv);
        break;
    }
    pthread_mutex_unlock(&mu);
}

#ifdef __EMSCRIPTEN__
/* the page owns the connection: packets out go to Module.netSend on the main
 * thread; packets in arrive via rt_net_packet */
/* Network worker (web/net-worker.js, when the page turns it on): packets for peers without a
 * direct WebRTC link go through two rings in this memory to a Web Worker that owns its own
 * relay socket, so they never wait for the busy main thread. Each record: u32 len, then
 * [node 6][packet]. Out: game threads -> worker (the worker sleeps on `head`). In: worker ->
 * net_in_main() below (sleeps on `head`). Packets still go to the main thread as well; it sends
 * only to peers with an open direct link, the worker only to the others. */
#define NRING (256u << 10)
typedef struct { volatile uint32_t head, tail; uint32_t cap, dropped; uint8_t data[NRING]; } NetRing;
static NetRing ring_out = { 0, 0, NRING, 0 }, ring_in = { 0, 0, NRING, 0 };
static volatile int use_worker;
static pthread_mutex_t out_mu = PTHREAD_MUTEX_INITIALIZER;
static void ring_put(NetRing *r, const uint8_t dst[6], const uint8_t *pkt, int len) {
    uint32_t n = (uint32_t)len + 6, h = r->head, t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if (NRING - (h - t) < n + 4) { r->dropped++; return; }
    uint8_t hdr[10]; memcpy(hdr, &n, 4); memcpy(hdr + 4, dst, 6);
    for (uint32_t i = 0; i < 10; i++) r->data[(h + i) % NRING] = hdr[i];
    for (uint32_t i = 0; i < (uint32_t)len; i++) r->data[(h + 10 + i) % NRING] = pkt[i];
    __atomic_store_n(&r->head, h + 4 + n, __ATOMIC_RELEASE);
    __builtin_wasm_memory_atomic_notify((int *)&r->head, 1);
}
static void *net_in_main(void *unused) {
    (void)unused;
    static uint8_t buf[2048];
    for (;;) {
        uint32_t t = ring_in.tail, h = __atomic_load_n(&ring_in.head, __ATOMIC_ACQUIRE);
        if (t == h) { __builtin_wasm_memory_atomic_wait32((int *)&ring_in.head, (int)h, -1); continue; }
        uint32_t n = 0;
        for (uint32_t i = 0; i < 4; i++) n |= (uint32_t)ring_in.data[(t + i) % NRING] << (8 * i);
        if (n <= sizeof buf) {
            for (uint32_t i = 0; i < n; i++) buf[i] = ring_in.data[(t + 4 + i) % NRING];
            if (n > 6) net_deliver(buf, buf + 6, (int)n - 6);
        }
        __atomic_store_n(&ring_in.tail, t + 4 + n, __ATOMIC_RELEASE);
    }
    return NULL;
}
/* the page: [out ring, in ring] addresses; on = 0 turns the worker path off again (fallback) */
EMSCRIPTEN_KEEPALIVE uint32_t *rt_net_worker(int on) {
    static uint32_t addr[2]; static int started;
    addr[0] = (uint32_t)(uintptr_t)&ring_out; addr[1] = (uint32_t)(uintptr_t)&ring_in;
    if (on && !started) { started = 1; pthread_t th; pthread_create(&th, NULL, net_in_main, NULL); }
    use_worker = on;
    rt_log("net: network worker %s", on ? "on" : "off");
    return addr;
}
static void transport_send(const uint8_t dst[6], const uint8_t *pkt, int len) {
    if (use_worker) { pthread_mutex_lock(&out_mu); ring_put(&ring_out, dst, pkt, len); pthread_mutex_unlock(&out_mu); }
    uint8_t *b = malloc((size_t)len + 6);
    memcpy(b, dst, 6); memcpy(b + 6, pkt, (size_t)len);
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module.netSend) Module.netSend($0, $1); else _free($0); }, b, len + 6);
}
EMSCRIPTEN_KEEPALIVE void rt_net_packet(const uint8_t *buf, int len) {   /* [src 6][packet] */
    if (len > 6) net_deliver(buf, buf + 6, len - 6);
}
/* the page calls this after joining a room (before the game opens a socket) */
EMSCRIPTEN_KEEPALIVE void rt_net_set_node(const char *n) {
    if (!n || strlen(n) != 12) return;
    for (int i = 0; i < 6; i++) { unsigned v; sscanf(n + 2 * i, "%2x", &v); self_node[i] = (uint8_t)v; }
    online = 1;
    rt_log("net: joined room as node %s", n);
}
static void transport_init(void) {}
#else
static int relay_fd = -1;
static pthread_mutex_t tx_mu = PTHREAD_MUTEX_INITIALIZER;
static void raw_send(int kind, const void *body, size_t n) {
    uint8_t h[5]; uint32_t len = (uint32_t)n + 1;
    memcpy(h, &len, 4); h[4] = (uint8_t)kind;
    pthread_mutex_lock(&tx_mu);
    if (relay_fd >= 0) { (void)!write(relay_fd, h, 5); (void)!write(relay_fd, body, n); }
    pthread_mutex_unlock(&tx_mu);
}
static void transport_send(const uint8_t dst[6], const uint8_t *pkt, int len) {
    uint8_t b[MAXPKT + 16];
    memcpy(b, dst, 6); memcpy(b + 6, pkt, (size_t)len);
    raw_send(1, b, (size_t)len + 6);
}
static int read_full(int fd, void *p, size_t n) {
    for (size_t o = 0; o < n;) { ssize_t r = read(fd, (char *)p + o, n - o); if (r <= 0) return -1; o += (size_t)r; }
    return 0;
}
static volatile int welcomed;
static void *relay_rx(void *unused) {
    (void)unused;
    static uint8_t body[1 << 16];
    for (;;) {
        uint32_t n; if (read_full(relay_fd, &n, 4) || n < 1 || n > sizeof body) break;
        if (read_full(relay_fd, body, n)) break;
        if (body[0] == 1) { if (n > 7) net_deliver(body + 1, body + 7, (int)n - 7); continue; }
        body[n] = 0;
        const char *j = (const char *)body + 1;
        if (strstr(j, "\"t\":\"welcome\"")) {
            const char *id = strstr(j, "\"id\":\"");
            if (id) for (int i = 0; i < 6; i++) { unsigned v; sscanf(id + 6 + 2 * i, "%2x", &v); self_node[i] = (uint8_t)v; }
            online = 1; welcomed = 1;
        }
        rt_log("net: %s", j);
    }
    rt_log("net: relay connection lost");
    online = 0;
    return NULL;
}
static int connect_timeout(int fd, const struct sockaddr *sa, socklen_t len, int ms) {
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, sa, len);
    if (r && errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, ms) == 1) { int e = 0; socklen_t l = sizeof e; getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l); r = e ? -1 : 0; }
        else r = -1;
    }
    fcntl(fd, F_SETFL, fl);
    return r;
}
static void transport_init(void) {
    /* RT_NET=host:port or unix:/path/to/socket */
    const char *addr = getenv("RT_NET"); if (!addr) return;
    int fd = -1;
    if (!strncmp(addr, "unix:", 5)) {
        struct sockaddr_un un = { .sun_family = AF_UNIX };
        snprintf(un.sun_path, sizeof un.sun_path, "%s", addr + 5);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect_timeout(fd, (struct sockaddr *)&un, sizeof un, 3000)) { rt_log("net: cannot connect to relay %s", addr); if (fd >= 0) close(fd); return; }
    } else {
        char host[256]; snprintf(host, sizeof host, "%s", addr);
        char *colon = strrchr(host, ':'); if (!colon) return;
        *colon = 0;
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai;
        if (getaddrinfo(host, colon + 1, &hints, &ai)) { rt_log("net: cannot resolve %s", addr); return; }
        fd = socket(ai->ai_family, SOCK_STREAM, 0);
        if (fd < 0 || connect_timeout(fd, ai->ai_addr, ai->ai_addrlen, 3000)) { rt_log("net: cannot connect to relay %s", addr); freeaddrinfo(ai); if (fd >= 0) close(fd); return; }
        freeaddrinfo(ai);
    }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* fails harmlessly on Unix sockets */
    relay_fd = fd;
    char hello[256];
    snprintf(hello, sizeof hello, "{\"t\":\"hello\",\"room\":\"%s\",\"name\":\"%s\"}",
             getenv("RT_ROOM") ? getenv("RT_ROOM") : "test", getenv("RT_NAME") ? getenv("RT_NAME") : "native");
    raw_send(0, hello, strlen(hello));
    pthread_t t; pthread_create(&t, NULL, relay_rx, NULL);
    for (int i = 0; i < 200 && !welcomed; i++) usleep(10000);
}
#endif

static void *stats_main(void *unused);
static int net_inited;
static void net_init(void) {
    if (net_inited) return;
    net_inited = 1;
    transport_init();
    if (getenv("RT_NET_STATS")) { pthread_t t; pthread_create(&t, NULL, stats_main, NULL); }
    rt_log("net: %s, node %02x:%02x:%02x:%02x:%02x:%02x", online ? "online" : "offline",
           self_node[0], self_node[1], self_node[2], self_node[3], self_node[4], self_node[5]);
}

/* ------------------------------------------------------------------ WSOCK32 */
static VSock *vs(uint32_t s) { return s >= SOCK_BASE && s < SOCK_BASE + NSOCK && socks[s - SOCK_BASE].used ? &socks[s - SOCK_BASE] : NULL; }
static uint32_t fail(uint32_t e) { last_err = e; return 0xFFFFFFFFu; }

static void write_ipx_addr(uint32_t p, const uint8_t node[6], uint16_t port) {
    WR16(p, AF_IPX_); memset(G + p + 2, 0, 4); memcpy(G + p + 6, node, 6);
    G[p + 12] = (uint8_t)(port >> 8); G[p + 13] = (uint8_t)port;
}

static uint32_t ws_WSAStartup(CPU *c, const uint32_t *a) {
    (void)c;
    uint32_t p = A(1);
    if (p) { memset(G + p, 0, 400); WR16(p, 0x0101); WR16(p + 2, 0x0101); strcpy(HSTR(p + 4), "nfs4-webgl virtual LAN"); }
    net_init();
    return 0;
}
static uint32_t ws_WSACleanup(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t ws_WSAGetLastError(CPU *c, const uint32_t *a) { (void)c; (void)a; return last_err; }

static uint32_t ws_socket(CPU *c, const uint32_t *a) {
    (void)c;
    net_init();
    if (A(0) != AF_IPX_ || A(1) != 2) { rt_log("socket(af=%u, type=%u, proto=%u): only IPX datagrams are supported", A(0), A(1), A(2)); return fail(WSAEAFNOSUPPORT); }
    if (!online) { rt_log("socket(IPX): not in an online room"); return fail(WSAENETDOWN); }
    pthread_mutex_lock(&mu);
    for (int i = 0; i < NSOCK; i++) if (!socks[i].used) {
        Pkt *q = socks[i].q; uint32_t g = socks[i].gen;
        socks[i] = (VSock){ .used = 1, .q = q, .gen = g + 1 };
        if (!socks[i].q) socks[i].q = malloc(sizeof(Pkt) * QLEN);
        pthread_mutex_unlock(&mu);
        rt_log("socket(IPX) -> %#x", SOCK_BASE + i);
        return SOCK_BASE + (uint32_t)i;
    }
    pthread_mutex_unlock(&mu);
    return fail(WSAEINVAL);
}
static uint32_t ws_closesocket(CPU *c, const uint32_t *a) {
    (void)c;
    VSock *s = vs(A(0)); if (!s) return fail(WSAENOTSOCK);
    pthread_mutex_lock(&mu); Pkt *q = s->q; uint32_t g = s->gen; *s = (VSock){0}; s->q = q; s->gen = g + 1;
    pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    rt_log("closesocket(%#x)", A(0));
    return 0;
}
static uint32_t ws_bind(CPU *c, const uint32_t *a) {
    (void)c;
    VSock *s = vs(A(0)); if (!s) return fail(WSAENOTSOCK);
    uint32_t p = A(1);
    uint16_t port = (uint16_t)(G[p + 12] << 8 | G[p + 13]);
    pthread_mutex_lock(&mu);
    if (!port) { static uint16_t next = 0x4000; port = next++; }
    for (int i = 0; i < NSOCK; i++) if (socks[i].used && socks[i].bound && socks[i].port == port) { pthread_mutex_unlock(&mu); return fail(WSAEADDRINUSE); }
    s->port = port; s->bound = 1;
    pthread_mutex_unlock(&mu);
    rt_log("bind(%#x, IPX socket %#06x)", A(0), port);
    return 0;
}
static uint32_t ws_getsockname(CPU *c, const uint32_t *a) {
    (void)c;
    VSock *s = vs(A(0)); if (!s) return fail(WSAENOTSOCK);
    write_ipx_addr(A(1), self_node, s->port);
    if (A(2)) WR32(A(2), 14);
    return 0;
}
static uint32_t ws_setsockopt(CPU *c, const uint32_t *a) { (void)c; if (!vs(A(0))) return fail(WSAENOTSOCK); return 0; }
static uint32_t ws_ioctlsocket(CPU *c, const uint32_t *a) {
    (void)c;
    VSock *s = vs(A(0)); if (!s) return fail(WSAENOTSOCK);
    if (A(1) == 0x8004667eu) { s->nonblock = RD32(A(2)) != 0; return 0; }           /* FIONBIO */
    if (A(1) == 0x4004667fu) {                                                    /* FIONREAD */
        pthread_mutex_lock(&mu); WR32(A(2), s->qh != s->qt ? s->q[s->qt].len : 0); pthread_mutex_unlock(&mu);
        return 0;
    }
    return fail(WSAEINVAL);
}
static uint32_t ws_sendto(CPU *c, const uint32_t *a) {
    (void)c;
    VSock *s = vs(A(0)); if (!s) return fail(WSAENOTSOCK);
    uint32_t len = A(2), to = A(4);
    if (len > MAXPKT) return fail(WSAEMSGSIZE);
    if (!s->bound) { uint32_t tmp[3] = { A(0), 0, 0 }; (void)tmp; static uint16_t next = 0x5000; s->port = next++; s->bound = 1; }
    const uint8_t *node = G + to + 6;
    uint16_t dport = (uint16_t)(G[to + 12] << 8 | G[to + 13]);
    uint8_t pkt[MAXPKT + 4];
    pkt[0] = (uint8_t)(dport >> 8); pkt[1] = (uint8_t)dport; pkt[2] = (uint8_t)(s->port >> 8); pkt[3] = (uint8_t)s->port;
    memcpy(pkt + 4, G + A(1), len);
    stats.tx++; stats.txb += len;
    trace_pkt("out", node, dport, s->port, G + A(1), (int)len);
    if (!memcmp(node, self_node, 6)) net_deliver(self_node, pkt, (int)len + 4);   /* to ourselves */
    else transport_send(node, pkt, (int)len + 4);
    if (rt_verbose > 1) rt_log("sendto(%#x, %u bytes -> %02x..%02x:%04x)", A(0), len, node[0], node[5], dport);
    return len;
}
static uint32_t ws_recvfrom(CPU *c, const uint32_t *a) {
    (void)c;
    VSock *s = vs(A(0)); if (!s) return fail(WSAENOTSOCK);
    pthread_mutex_lock(&mu);
    uint32_t gen = s->gen;
    while (s->qh == s->qt) {
        if (s->nonblock) { pthread_mutex_unlock(&mu); return fail(WSAEWOULDBLOCK); }
        pthread_cond_wait(&cv, &mu);
        if (s->gen != gen || !s->used) { pthread_mutex_unlock(&mu); return fail(10004 /* WSAEINTR */); }
    }
    Pkt *k = &s->q[s->qt];
    uint32_t n = k->len < A(2) ? k->len : A(2);
    memcpy(G + A(1), k->data, n);
    if (A(4)) write_ipx_addr(A(4), k->src, k->sport);
    if (A(5)) WR32(A(5), 14);
    int trunc = k->len > A(2);
    s->qt = (s->qt + 1) % QLEN;
    pthread_mutex_unlock(&mu);
    if (trunc) return fail(WSAEMSGSIZE);
    return n;
}

/* fd_set: u32 count, u32 fds[64] */
static int fdset_has(uint32_t set, uint32_t fd) {
    if (!set) return 0;
    uint32_t n = RD32(set);
    for (uint32_t i = 0; i < n && i < 64; i++) if (RD32(set + 4 + i * 4) == fd) return 1;
    return 0;
}
static uint32_t ws_select(CPU *c, const uint32_t *a) {
    (void)c;
    uint32_t rd = A(1), wr = A(2), ex = A(3), tv = A(4);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    long ms = tv ? (long)RD32(tv) * 1000 + (long)RD32(tv + 4) / 1000 : -1;
    if (ms > 0) { dl.tv_sec += ms / 1000; dl.tv_nsec += (ms % 1000) * 1000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; } }
    uint32_t rin[65] = {0};
    if (rd) { rin[0] = RD32(rd); if (rin[0] > 64) rin[0] = 64; for (uint32_t i = 0; i < rin[0]; i++) rin[1 + i] = RD32(rd + 4 + i * 4); }
    uint32_t total;
    uint32_t gens[65] = {0};
    pthread_mutex_lock(&mu);
    for (uint32_t i = 0; i < rin[0]; i++) { VSock *s = vs(rin[1 + i]); gens[1 + i] = s ? s->gen : 0; }
    for (;;) {
        total = 0;
        uint32_t keep = 0;
        if (rd) {
            for (uint32_t i = 0; i < rin[0]; i++) {
                uint32_t fd = rin[1 + i];
                VSock *s = vs(fd);
                /* readable: data waiting, or closed meanwhile (the next recvfrom then fails, as on Windows) */
                int closed = !s || (fd >= SOCK_BASE && fd < SOCK_BASE + NSOCK && socks[fd - SOCK_BASE].gen != gens[1 + i]);
                if (closed || s->qh != s->qt) WR32(rd + 4 + keep++ * 4, fd);
            }
            WR32(rd, keep); total += keep;
        }
        if (total || ms == 0) break;
        if (wr && RD32(wr)) break;
        if (ms < 0) pthread_cond_wait(&cv, &mu);
        else if (pthread_cond_timedwait(&cv, &mu, &dl)) break;
    }
    pthread_mutex_unlock(&mu);
    if (wr) total += RD32(wr);         /* datagram sockets are always writable */
    if (ex) WR32(ex, 0);
    return total;
}
static uint32_t ws_FDIsSet(CPU *c, const uint32_t *a) { (void)c; return (uint32_t)fdset_has(A(1), A(0)); }

static uint32_t ws_htons(CPU *c, const uint32_t *a) { (void)c; return __builtin_bswap16((uint16_t)A(0)); }
static uint32_t ws_gethostname(CPU *c, const uint32_t *a) { (void)c; snprintf(HSTR(A(0)), A(1), "nfs4web"); return 0; }
static uint32_t ws_inet_addr(CPU *c, const uint32_t *a) {
    (void)c; unsigned b[4];
    if (sscanf(HSTR(A(0)), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) return 0xFFFFFFFFu;
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}
static uint32_t ntoa_buf;
static uint32_t ws_inet_ntoa(CPU *c, const uint32_t *a) {
    (void)c;
    if (!ntoa_buf) ntoa_buf = galloc(32);
    uint32_t v = A(0);
    snprintf(HSTR(ntoa_buf), 32, "%u.%u.%u.%u", v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
    return ntoa_buf;
}
static uint32_t ws_unsupported(CPU *c, const uint32_t *a) { (void)c; (void)a; return fail(WSAEAFNOSUPPORT); }
static uint32_t ws_null(CPU *c, const uint32_t *a) { (void)c; (void)a; last_err = WSAENETDOWN; return 0; }

#define W(n, args, fn) {"WSOCK32.DLL", #n, args, fn}
const ImportDef imp_wsock32[] = {
    W(WSAStartup, 2, ws_WSAStartup), W(WSACleanup, 0, ws_WSACleanup), W(WSAGetLastError, 0, ws_WSAGetLastError),
    W(socket, 3, ws_socket), W(closesocket, 1, ws_closesocket), W(bind, 3, ws_bind), W(getsockname, 3, ws_getsockname),
    W(setsockopt, 5, ws_setsockopt), W(ioctlsocket, 3, ws_ioctlsocket), W(sendto, 6, ws_sendto), W(recvfrom, 6, ws_recvfrom),
    W(select, 5, ws_select), W(__WSAFDIsSet, 2, ws_FDIsSet),
    W(htons, 1, ws_htons), W(ntohs, 1, ws_htons), W(gethostname, 2, ws_gethostname), W(inet_addr, 1, ws_inet_addr),
    W(inet_ntoa, 1, ws_inet_ntoa), W(gethostbyname, 1, ws_null),
    /* stream sockets (the game's TCP/IP mode) aren't available */
    W(listen, 2, ws_unsupported), W(shutdown, 2, ws_unsupported), W(connect, 3, ws_unsupported), W(accept, 3, ws_unsupported),
    W(send, 4, ws_unsupported), W(recv, 4, ws_unsupported),
    {0},
};

/* RT_NET_STATS=1: one traffic line per second */
static void *stats_main(void *unused) {
    (void)unused;
    uint64_t ptx = 0, prx = 0, ptxb = 0, prxb = 0;
    for (;;) {
        sleep(1);
        rt_log("net/s: tx %llu pkts %llu B, rx %llu pkts %llu B, drops %llu",
               (unsigned long long)(stats.tx - ptx), (unsigned long long)(stats.txb - ptxb),
               (unsigned long long)(stats.rx - prx), (unsigned long long)(stats.rxb - prxb), (unsigned long long)stats.drop);
        ptx = stats.tx; prx = stats.rx; ptxb = stats.txb; prxb = stats.rxb;
    }
    return NULL;
}
