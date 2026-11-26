// Regression and measurement for mpv patch 0318 (stream/http_parallel.c:
// one http(s) file through several connections), linked by
// test_http_parallel.sh against a minimal host build of the patched FFmpeg
// (http and tcp, with patches 0080/0081). The servers run on loopback in this
// process and model a slow network: every connection is limited to a rate
// and every reply waits a round trip before its first byte, as a single TCP
// connection over a long path is.
//
// What must hold:
//   - every byte read is the byte the resource has at that offset, through
//     the walk mpv's stream layer makes (headers, the index at the end, the
//     middle, back, the very end), at 2, 4 and 8 connections;
//   - sequential reading over rate-limited connections is faster than one
//     connection: printed, and at least 2x with 4 connections;
//   - a connection cut in the middle of a reply is retried, and the bytes are
//     still right;
//   - a server that refuses connections beyond two is read through the two;
//   - a seek away stops fetches outside the new window: what was received
//     and never read stays small.
#define _GNU_SOURCE // memmem, strcasestr
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>

#include "stream/http_parallel.h"

#define MiB (1024 * 1024)
#define SIZE (96ll * MiB + 12345)

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t byte_at(int64_t off) { return (uint8_t)((off * 2654435761u) >> 13); }

struct server {
    int fd, port;
    int64_t rate;          // bytes per second per connection, 0: unlimited
    int latency_ms;        // before each reply's first byte
    int max_connections;   // refuse beyond this many at once, 0: any
    atomic_int cut_once;   // cut the next reply after 300 KiB
    atomic_int open_now, connections, requests;
};

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_s(double s)
{
    if (s <= 0) return;
    struct timespec ts = {(time_t)s, (long)((s - (time_t)s) * 1e9)};
    nanosleep(&ts, NULL);
}

struct conn { struct server *s; int fd; };

static void *serve_conn(void *arg)
{
    struct conn *c = arg;
    struct server *s = c->s;
    char req[4096];
    int have = 0;
    while (1) {
        char *end;
        while (!(end = memmem(req, have, "\r\n\r\n", 4))) {
            if (have == sizeof(req)) goto done;
            int r = recv(c->fd, req + have, sizeof(req) - have, 0);
            if (r <= 0) goto done;
            have += r;
        }
        int hlen = end + 4 - req;
        atomic_fetch_add(&s->requests, 1);
        int64_t from = 0, to = SIZE - 1;
        char *range = strcasestr(req, "\r\nRange: bytes=");
        if (range && range < end) {
            from = strtoll(range + 15, &range, 10);
            if (*range == '-' && range[1] >= '0' && range[1] <= '9')
                to = strtoll(range + 1, NULL, 10);
        }
        if (to > SIZE - 1) to = SIZE - 1;
        bool close_after = strcasestr(req, "Connection: close") && strcasestr(req, "Connection: close") < end;
        memmove(req, req + hlen, have - hlen);
        have -= hlen;
        sleep_s(s->latency_ms / 1000.0);
        char head[512];
        int n = snprintf(head, sizeof(head),
            "HTTP/1.1 206 Partial Content\r\nContent-Length: %" PRId64 "\r\nAccept-Ranges: bytes\r\n"
            "Content-Range: bytes %" PRId64 "-%" PRId64 "/%lld\r\n%s\r\n",
            to - from + 1, from, to, (long long)SIZE, close_after ? "Connection: close\r\n" : "");
        if (send(c->fd, head, n, 0) != n) goto done;
        bool cut = atomic_exchange(&s->cut_once, 0);
        double t0 = now();
        int64_t sent = 0;
        uint8_t buf[16384];
        while (from + sent <= to) {
            if (cut && sent >= 300 * 1024) goto done;
            int64_t k = to - from - sent + 1;
            if (k > (int64_t)sizeof(buf)) k = sizeof(buf);
            for (int i = 0; i < k; i++) buf[i] = byte_at(from + sent + i);
            ssize_t w = send(c->fd, buf, k, 0);
            if (w <= 0) goto done;
            sent += w;
            if (s->rate)
                sleep_s(t0 + (double)sent / s->rate - now());
        }
        if (close_after) goto done;
    }
done:
    close(c->fd);
    atomic_fetch_sub(&s->open_now, 1);
    free(c);
    return NULL;
}

static void *accept_loop(void *arg)
{
    struct server *s = arg;
    while (1) {
        int fd = accept(s->fd, NULL, NULL);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        // A small send buffer, so the rate limit is what the client sees.
        int sndbuf = 65536;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        if (s->max_connections && atomic_load(&s->open_now) >= s->max_connections) {
            const char *busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(fd, busy, strlen(busy), 0);
            close(fd);
            continue;
        }
        atomic_fetch_add(&s->open_now, 1);
        atomic_fetch_add(&s->connections, 1);
        struct conn *c = malloc(sizeof(*c));
        c->s = s;
        c->fd = fd;
        pthread_t t;
        pthread_create(&t, NULL, serve_conn, c);
        pthread_detach(t);
    }
    return NULL;
}

static struct server *start_server(int64_t rate, int latency_ms, int max_connections)
{
    struct server *s = calloc(1, sizeof(*s));
    s->rate = rate;
    s->latency_ms = latency_ms;
    s->max_connections = max_connections;
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(s->fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(s->fd, (struct sockaddr *)&a, &len);
    s->port = ntohs(a.sin_port);
    listen(s->fd, 64);
    pthread_t t;
    pthread_create(&t, NULL, accept_loop, s);
    pthread_detach(t);
    return s;
}

// As stream_lavf.c opens a file.
static AVDictionary *options(void)
{
    AVDictionary *d = NULL;
    av_dict_set(&d, "reconnect", "1", 0);
    av_dict_set(&d, "reconnect_delay_max", "7", 0);
    av_dict_set(&d, "multiple_requests", "1", 0);
    av_dict_set(&d, "short_seek_size", "262144", 0);
    return d;
}

static void url_of(struct server *s, char *buf, size_t size)
{
    snprintf(buf, size, "http://127.0.0.1:%d/file.mkv", s->port);
}

static volatile int adopt_abort;
static int primary_interrupt(void *ctx) { return adopt_abort; }

static struct mp_http_parallel *open_parallel(struct server *s, int connections, int64_t chunk)
{
    char url[128];
    url_of(s, url, sizeof(url));
    AVDictionary *d = options(), *copy = NULL;
    av_dict_copy(&copy, d, 0);
    AVIOContext *avio = NULL;
    adopt_abort = 0;
    AVIOInterruptCB cb = {.callback = primary_interrupt};
    if (avio_open2(&avio, url, AVIO_FLAG_READ, &cb, &d) < 0) {
        printf("cannot open %s\n", url);
        exit(2);
    }
    av_dict_free(&d);
    struct mp_http_parallel_params params = {
        .url = url, .options = copy, .size = avio_size(avio), .connections = connections,
        .chunk_size = chunk, .sequential_threshold = 512 * 1024, .adopt = avio,
        .adopt_abort = &adopt_abort,
    };
    struct mp_http_parallel *p = mp_http_parallel_create(&params);
    av_dict_free(&copy);
    return p;
}

// Reads [pos, pos + len) and checks every byte.
static bool read_check(struct mp_http_parallel *p, int64_t pos, int64_t len)
{
    mp_http_parallel_seek(p, pos);
    static uint8_t buf[1 << 20];
    while (len > 0) {
        int want = len > (int64_t)sizeof(buf) ? (int)sizeof(buf) : (int)len;
        int r = mp_http_parallel_read(p, buf, want);
        if (r <= 0) {
            printf("read at %" PRId64 " returned %d\n", pos, r);
            return false;
        }
        for (int i = 0; i < r; i++) {
            if (buf[i] != byte_at(pos + i)) {
                printf("wrong byte at %" PRId64 "\n", pos + i);
                return false;
            }
        }
        pos += r;
        len -= r;
    }
    return true;
}

static double read_plain(struct server *s, int64_t len)
{
    char url[128];
    url_of(s, url, sizeof(url));
    AVDictionary *d = options();
    AVIOContext *avio = NULL;
    double t0 = now();
    if (avio_open2(&avio, url, AVIO_FLAG_READ, NULL, &d) < 0) exit(2);
    av_dict_free(&d);
    static uint8_t buf[1 << 17];
    int64_t got = 0;
    while (got < len) {
        int r = avio_read_partial(avio, buf, sizeof(buf));
        if (r <= 0) break;
        got += r;
    }
    avio_closep(&avio);
    return now() - t0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    av_log_set_level(AV_LOG_QUIET);
    avformat_network_init();

    // 1. Every byte right through the stream layer's walk.
    struct server *fast = start_server(0, 0, 0);
    for (int connections = 2; connections <= 8; connections *= 2) {
        struct mp_http_parallel *p = open_parallel(fast, connections, 2 * MiB);
        bool ok = read_check(p, 0, 300 * 1024) &&              // headers
                  read_check(p, SIZE - 200 * 1024, 200 * 1024) && // index at the end
                  read_check(p, 0, 9 * MiB + 77) &&             // the start again, on
                  read_check(p, 40 * MiB + 3, 21 * MiB) &&      // a seek into the middle
                  read_check(p, 7 * MiB - 5, 3 * MiB) &&        // back
                  read_check(p, SIZE - 1, 1);                   // the very end
        uint8_t b;
        mp_http_parallel_seek(p, SIZE);
        ok = ok && mp_http_parallel_read(p, &b, 1) == 0;
        CHECK(ok, "%d connections: every byte of the walk is right, and the end reads as the end", connections);
        mp_http_parallel_destroy(p);
    }

    // 2. A slow network: 2 MB/s per connection, 40 ms before each reply.
    struct server *slow = start_server(2000000, 40, 0);
    int64_t len = 24 * MiB;
    double plain = read_plain(slow, len);
    for (int connections = 2; connections <= 8; connections *= 2) {
        int before = atomic_load(&slow->connections);
        double t0 = now();
        struct mp_http_parallel *p = open_parallel(slow, connections, 2 * MiB);
        bool ok = read_check(p, 0, len);
        double t = now() - t0;
        struct mp_http_parallel_stats st;
        mp_http_parallel_get_stats(p, &st);
        mp_http_parallel_destroy(p);
        printf("info: %d MiB sequential at 2 MB/s per connection, 40 ms per reply: one connection %.2f s "
               "(%.1f MB/s), %d connections %.2f s (%.1f MB/s), %.1fx; %d connections opened, %" PRId64
               " requests, %" PRId64 " KiB unread\n", (int)(len / MiB), plain, len / plain / 1e6, connections,
               t, len / t / 1e6, plain / t, atomic_load(&slow->connections) - before, st.requests,
               st.wasted / 1024);
        CHECK(ok, "%d connections over the slow network read every byte right", connections);
        if (connections == 4)
            CHECK(plain / t >= 2.0, "4 connections are at least twice as fast (%.1fx)", plain / t);
    }

    // 3. Start of playback: the first 4 MiB after a resume, as the demuxer
    // asks for them (time to first bytes and to a filled start).
    {
        struct mp_http_parallel *p = open_parallel(slow, 4, 2 * MiB);
        int requests = atomic_load(&slow->requests);
        double t0 = now();
        bool ok = read_check(p, 30 * MiB, 1);
        double first = now() - t0;
        requests = atomic_load(&slow->requests) - requests;
        ok = ok && read_check(p, 30 * MiB + 1, 8 * MiB + 64 * 1024 - 1);
        double filled = now() - t0;
        mp_http_parallel_destroy(p);
        char url[128];
        url_of(slow, url, sizeof(url));
        AVDictionary *d = options();
        AVIOContext *avio = NULL;
        avio_open2(&avio, url, AVIO_FLAG_READ, NULL, &d);
        av_dict_free(&d);
        static uint8_t buf[1 << 17];
        t0 = now();
        avio_seek(avio, 30 * MiB, SEEK_SET);
        int64_t got = 0;
        double pfirst = 0;
        while (got < 8 * MiB + 64 * 1024) {
            int r = avio_read_partial(avio, buf, sizeof(buf));
            if (r <= 0) break;
            if (!got) pfirst = now() - t0;
            got += r;
        }
        double pfilled = now() - t0;
        avio_closep(&avio);
        printf("info: resume mid-file, slow network: first bytes %.0f ms after %d request(s) (one connection "
               "%.0f ms), 8 MiB in %.2f s (one connection %.2f s)\n", first * 1000, requests, pfirst * 1000,
               filled, pfilled);
        CHECK(ok && first < pfirst + 0.1, "a resume's first bytes come no later than with one connection");
    }

    // 4. A connection cut mid-reply is retried.
    {
        struct mp_http_parallel *p = open_parallel(fast, 4, 2 * MiB);
        atomic_store(&fast->cut_once, 1);
        bool ok = read_check(p, 10 * MiB, 20 * MiB);
        struct mp_http_parallel_stats st;
        mp_http_parallel_get_stats(p, &st);
        mp_http_parallel_destroy(p);
        CHECK(ok, "a reply cut short is fetched again and every byte is right (%" PRId64 " retries)", st.retries);
    }

    // 5. A server that takes two connections at once.
    {
        struct server *capped = start_server(4000000, 10, 2);
        struct mp_http_parallel *p = open_parallel(capped, 4, 1 * MiB);
        bool ok = read_check(p, 0, 16 * MiB);
        mp_http_parallel_destroy(p);
        CHECK(ok, "a server limited to two connections is read through them");
    }

    // 6. Seeking away stops what the reader left.
    {
        struct mp_http_parallel *p = open_parallel(slow, 4, 2 * MiB);
        bool ok = read_check(p, 0, 3 * MiB);  // the window opens
        ok = ok && read_check(p, 60 * MiB, 100 * 1024);
        sleep_s(0.5);
        struct mp_http_parallel_stats st;
        mp_http_parallel_get_stats(p, &st);
        mp_http_parallel_destroy(p);
        CHECK(ok && st.wasted < 6 * MiB, "a seek away leaves %" PRId64 " KiB unread", st.wasted / 1024);
    }

    printf(failures ? "%d FAILED\n" : "http parallel: all passed\n", failures);
    return failures ? 1 : 0;
}
