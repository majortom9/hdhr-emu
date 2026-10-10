/*
 * hdhr-emu - http_server.c
 * Copyright (C) 2026  Bill Murphy <gc2majortom@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * http_server.c — the HTTP surface that Plex/Emby/Jellyfin/Channels DVR
 * (and modern SiliconDust firmware itself) actually use day to day.
 * Listens on port 80, matching real hardware.
 *
 * /auto/vX.X doesn't name a specific tuner — real firmware auto-picks
 * one of its N physical tuners for such requests, so we do the same via
 * tuner_pool_claim_free() (see tuner.h), which also protects against
 * double-tuning a physical adapter that a control-plane target= push is
 * already using.
 */
#include "http_server.h"
#include "device_id.h"
#include "dvb_channel.h"
#include "dvb_stream.h"
#include "tuner.h"
#include "control.h"
#include "web_ui.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>

#define HTTP_PORT 80

struct http_ctx {
    int fd;
    struct hdhr_config *cfg;
    struct hdhr_tuner *tuners;
};

static void send_headers(int fd, const char *status, const char *content_type, long content_length)
{
    char hdr[256];
    int n;
    if (content_length >= 0) {
        n = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "Connection: close\r\nServer: hdhr-emu\r\n\r\n",
                      status, content_type, content_length);
    } else {
        n = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                      "Connection: close\r\nServer: hdhr-emu\r\n\r\n",
                      status, content_type);
    }
    if (write(fd, hdr, (size_t)n) < 0) { /* client already gone */ }
}

static void send_json(int fd, const char *json)
{
    send_headers(fd, "200 OK", "application/json", (long)strlen(json));
    if (write(fd, json, strlen(json)) < 0) { /* client already gone */ }
}

static void send_404(int fd)
{
    send_headers(fd, "404 Not Found", "text/plain", 0);
}

static void send_503(int fd, const char *msg)
{
    send_headers(fd, "503 Service Unavailable", "text/plain", (long)strlen(msg));
    if (write(fd, msg, strlen(msg)) < 0) { /* client already gone */ }
}

static int local_ip_for_peer(int connected_fd, char *out, size_t outlen)
{
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    if (getsockname(connected_fd, (struct sockaddr *)&local, &len) < 0) return -1;
    if (!inet_ntop(AF_INET, &local.sin_addr, out, outlen)) return -1;
    return 0;
}

static void handle_discover_json(int fd, const struct hdhr_config *cfg)
{
    char ip[16];
    if (local_ip_for_peer(fd, ip, sizeof(ip)) != 0) snprintf(ip, sizeof(ip), "0.0.0.0");

    char json[768];
    snprintf(json, sizeof(json),
        "{\n"
        "  \"FriendlyName\": \"%s\",\n"
        "  \"ModelNumber\": \"%s\",\n"
        "  \"FirmwareName\": \"%s\",\n"
        "  \"FirmwareVersion\": \"%s\",\n"
        "  \"DeviceID\": \"%08X\",\n"
        "  \"DeviceAuth\": \"\",\n"
        "  \"BaseURL\": \"http://%s:80\",\n"
        "  \"LineupURL\": \"http://%s:80/lineup.json\",\n"
        "  \"TunerCount\": %d\n"
        "}\n",
        cfg->friendly_name, cfg->model, cfg->firmware_name, cfg->firmware_version,
        cfg->device_id, ip, ip, cfg->tuner_count);
    send_json(fd, json);
}

/* Wraps a channel's /auto/vX.X stream URL in a tiny M3U playlist --
 * most browsers can't play a raw MPEG-TS URL directly, but handing
 * off a downloaded .m3u file lets the OS/browser dispatch it to
 * whatever's registered to open one (VLC, mpv, etc.), same trick real
 * HDHomeRun web UIs use. Content-Disposition forces a download rather
 * than an inline text render, since not every browser recognizes the
 * M3U mime type well enough to hand it off on its own. */
static void handle_m3u(int fd, const struct hdhr_config *cfg, int major, int minor)
{
    (void)cfg;
    const struct dvb_channel *ch = dvb_find_channel(major, minor);
    char ip[16];
    if (local_ip_for_peer(fd, ip, sizeof(ip)) != 0) snprintf(ip, sizeof(ip), "0.0.0.0");

    char body[512];
    int blen = snprintf(body, sizeof(body),
        "#EXTM3U\n#EXTINF:-1,%d.%d %s\nhttp://%s:80/auto/v%d.%d\n",
        major, minor, ch ? ch->short_name : "Unknown", ip, major, minor);

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: audio/x-mpegurl\r\n"
        "Content-Disposition: attachment; filename=\"%d.%d.m3u\"\r\n"
        "Content-Length: %d\r\nConnection: close\r\nServer: hdhr-emu\r\n\r\n",
        major, minor, blen);
    if (write(fd, hdr, (size_t)hlen) < 0) { /* client already gone */ }
    if (write(fd, body, (size_t)blen) < 0) { /* client already gone */ }
}

/* The whole lineup as one M3U playlist -- same idea as handle_m3u()
 * above, for the web UI's lineup Export button (and any IPTV-style
 * client that wants a plain playlist URL). tvg-chno/tvg-name are the
 * de facto extended-M3U attributes Jellyfin/Kodi/Channels/VLC read for
 * channel number and name; '"' in a PSIP short name is swapped for '\''
 * inside the attribute so it can't terminate the quoted value early. */
static void handle_lineup_m3u(int fd)
{
    char ip[16];
    if (local_ip_for_peer(fd, ip, sizeof(ip)) != 0) snprintf(ip, sizeof(ip), "0.0.0.0");

    size_t cap = 16384;
    size_t off = 0;
    char *body = malloc(cap);
    if (!body) { send_headers(fd, "500 Internal Server Error", "text/plain", 0); return; }
    off += (size_t)snprintf(body + off, cap - off, "#EXTM3U\n");

    /* Sorted snapshot, major.minor ascending -- see dvb_channels_sorted(). */
    struct dvb_channel *chs = malloc(DVB_CHANNEL_MAX * sizeof(*chs));
    if (!chs) { free(body); send_headers(fd, "500 Internal Server Error", "text/plain", 0); return; }
    int n = dvb_channels_sorted(chs, DVB_CHANNEL_MAX);
    for (int i = 0; i < n; i++) {
        const struct dvb_channel *ch = &chs[i];

        /* one entry is well under 256 bytes (short_name is <16 chars) */
        if (cap - off < 512) {
            cap *= 2;
            char *grown = realloc(body, cap);
            if (!grown) { free(chs); free(body); send_headers(fd, "500 Internal Server Error", "text/plain", 0); return; }
            body = grown;
        }

        char attr_name[sizeof(ch->short_name)];
        snprintf(attr_name, sizeof(attr_name), "%s", ch->short_name);
        for (char *p = attr_name; *p; p++) if (*p == '"') *p = '\'';

        off += (size_t)snprintf(body + off, cap - off,
            "#EXTINF:-1 tvg-chno=\"%d.%d\" tvg-name=\"%s\",%d.%d %s\n"
            "http://%s:80/auto/v%d.%d\n",
            ch->major, ch->minor, attr_name, ch->major, ch->minor, ch->short_name,
            ip, ch->major, ch->minor);
    }
    free(chs);

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: audio/x-mpegurl\r\n"
        "Content-Disposition: attachment; filename=\"lineup.m3u\"\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nServer: hdhr-emu\r\n\r\n", off);
    if (write(fd, hdr, (size_t)hlen) < 0) { /* client already gone */ }
    if (write(fd, body, off) < 0) { /* client already gone */ }
    free(body);
}

static void handle_lineup_status_json(int fd)
{
    send_json(fd,
        "{\n"
        "  \"ScanInProgress\": 0,\n"
        "  \"ScanPossible\": 1,\n"
        "  \"Source\": \"Antenna\",\n"
        "  \"SourceList\": [\"Antenna\"]\n"
        "}\n");
}

static void handle_lineup_json(int fd, const struct hdhr_config *cfg)
{
    (void)cfg; /* not currently needed here — kept for symmetry/future use */
    char ip[16];
    if (local_ip_for_peer(fd, ip, sizeof(ip)) != 0) snprintf(ip, sizeof(ip), "0.0.0.0");

    size_t cap = 65536;
    size_t off = 0;
    char *body = malloc(cap);
    if (!body) { send_headers(fd, "500 Internal Server Error", "text/plain", 0); return; }
    off += (size_t)snprintf(body + off, cap - off, "[\n");

    /* Sorted snapshot, major.minor ascending -- see dvb_channels_sorted().
     * Clients (Plex/Jellyfin/this daemon's own web UI) typically keep
     * lineup.json's order as their channel list. */
    struct dvb_channel *chs = malloc(DVB_CHANNEL_MAX * sizeof(*chs));
    if (!chs) { free(body); send_headers(fd, "500 Internal Server Error", "text/plain", 0); return; }
    int n = dvb_channels_sorted(chs, DVB_CHANNEL_MAX);
    for (int i = 0; i < n; i++) {
        const struct dvb_channel *ch = &chs[i];

        /* worst case a bit under 300 bytes for one entry (long name +
         * URL); keep plenty of headroom before formatting so snprintf
         * never has to truncate. */
        if (cap - off < 512) {
            cap *= 2;
            char *grown = realloc(body, cap);
            if (!grown) { free(chs); free(body); send_headers(fd, "500 Internal Server Error", "text/plain", 0); return; }
            body = grown;
        }

        off += (size_t)snprintf(body + off, cap - off,
            "  {\"GuideNumber\": \"%d.%d\", \"GuideName\": \"%s\", "
            "\"URL\": \"http://%s:80/auto/v%d.%d\"}%s\n",
            ch->major, ch->minor, ch->short_name,
            ip, ch->major, ch->minor,
            (i == n - 1) ? "" : ",");
    }
    free(chs);

    if (cap - off < 8) {
        cap += 8;
        char *grown = realloc(body, cap);
        if (!grown) { free(body); send_headers(fd, "500 Internal Server Error", "text/plain", 0); return; }
        body = grown;
    }
    off += (size_t)snprintf(body + off, cap - off, "]\n");

    send_json(fd, body);
    free(body);
}

/* Ends an HTTP pull that got as far as tuner_bind_channel(): returns the
 * tuner to idle ("ch=none") instead of letting tuner_release() re-open a
 * background hold on the channel just watched. That hold (ARCHITECTURE.md
 * §16) exists for a channel *selected* via the control protocol
 * (vchannel/channel SET -- signal monitoring, antenna aiming); an HTTP
 * pull's binding is only implicit, and real firmware frees an
 * HTTP-streamed tuner once the stream ends. Confirmed live with mpv
 * stepping through /lineup.m3u: it drops the old connection and requests
 * the next channel within milliseconds, before this thread has noticed
 * the disconnect, so /auto/ picks the *other* tuner -- and the first one
 * then stayed held, locked, on the previous channel indefinitely, leaving
 * both tuners engaged with only one viewer. Only clears the selection if
 * it's still the one this stream bound -- a control-plane vchannel/
 * channel SET that landed mid-stream is someone else's newer selection,
 * left for tuner_release() to re-hold as usual. */
static void release_http_pull(struct hdhr_tuner *t, const struct dvb_channel *ch)
{
    tuner_lock(t);
    if (t->vch_resolved && t->vch_major == ch->major && t->vch_minor == ch->minor &&
        t->tuned_frequency_hz == ch->frequency_hz) {
        snprintf(t->channel, sizeof(t->channel), "none");
        t->tuned_frequency_hz = 0;
        t->vch_resolved = false;
        t->program = DVB_PROGRAM_DEFAULT;
        t->filter_override[0] = '\0';
        /* Same idle string as tuner_pool_init()/channel=none -- see the
         * former's comment for why the "ch=" token matters. */
        snprintf(t->status, sizeof(t->status), "ch=none lock=none ss=0 snq=0 seq=0 bps=0 pps=0");
    }
    tuner_unlock(t);
    tuner_release_park(t); /* closes the stream but parks its frontend -- see tuner.h parked_fd */
}

/* How long a /auto/ request waits for a tuner still streaming to the
 * same client before giving up and taking any free tuner. Has to cover
 * the old stream's whole teardown: noticing the disconnect (next write()
 * fails, typically milliseconds), then dvb_stream_close() joining its
 * reader and stats threads -- the stats thread can be mid-way through
 * a frontend stat read that takes ~1s while streaming on this hardware,
 * plus its 500ms refresh sleep. Only ever spent in full when that
 * client genuinely has a second stream still open (e.g. two player
 * windows on one machine), which then falls back to another tuner as
 * before. */
#define AUTO_AFFINITY_WAIT_MS 3000

/* /auto/ client affinity -- see http_peer_addr in tuner.h. If some
 * tuner is still streaming to this same client address, waits (up to
 * AUTO_AFFINITY_WAIT_MS) to take that tuner over once its old stream
 * ends, so a player changing channels keeps using one tuner instead of
 * alternating between them. Returns NULL if there's no such tuner or
 * the wait times out; the caller then falls back to
 * tuner_pool_claim_free(). */
static struct hdhr_tuner *claim_for_peer(struct hdhr_tuner *tuners, int count, uint32_t peer_addr,
                                          const struct dvb_channel *ch, int *out_reused_fd)
{
    if (peer_addr == 0) return NULL;
    for (int i = 0; i < count; i++) {
        struct hdhr_tuner *t = &tuners[i];
        tuner_lock(t);
        bool mine = t->busy && t->http_peer_addr == peer_addr;
        tuner_unlock(t);
        if (!mine) continue;
        if (tuner_try_claim_wait(t, AUTO_AFFINITY_WAIT_MS, ch->frequency_hz, ch->delivery, out_reused_fd)) {
            return t;
        }
        return NULL; /* that client's other stream is still live -- use any free tuner */
    }
    return NULL;
}

/* requested_tuner_idx: -1 to auto-allocate any free tuner (the plain
 * /auto/vX.X path); >=0 to require that specific tuner slot (the
 * /tunerN/vX.X path, e.g. hdhomerun_config's "save /tunerN -"). */
static void stream_channel_to_client(int fd, const struct hdhr_config *cfg,
                                      struct hdhr_tuner *tuners, int requested_tuner_idx,
                                      int major, int minor)
{
    const struct dvb_channel *ch = dvb_find_channel(major, minor);
    if (!ch) {
        send_404(fd);
        return;
    }

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    uint32_t peer_addr = (getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0)
                         ? peer.sin_addr.s_addr : 0;

    int reused_fd = -1;
    struct hdhr_tuner *t;
    if (requested_tuner_idx >= 0) {
        t = &tuners[requested_tuner_idx];
        if (!tuner_try_claim(t, ch->frequency_hz, ch->delivery, &reused_fd)) {
            send_503(fd, "tuner busy\n");
            return;
        }
    } else {
        t = claim_for_peer(tuners, cfg->tuner_count, peer_addr, ch, &reused_fd);
        if (!t) t = tuner_pool_claim_free(tuners, cfg->tuner_count, ch->frequency_hz, ch->delivery, &reused_fd);
        if (!t) {
            send_503(fd, "all tuners busy\n");
            return;
        }
    }
    tuner_lock(t);
    t->http_peer_addr = peer_addr; /* cleared by tuner_release() */
    tuner_unlock(t);

    /* HTTP passthrough doesn't consult t->program or t->filter_override,
     * same established asymmetry as program (see tuner.h) — this path
     * always streams the plain named channel. */
    struct dvb_stream *ds = dvb_stream_open(t->adapter, cfg->dvb_frontend, cfg->dvb_demux,
                                             ch, DVB_PROGRAM_DEFAULT, NULL, reused_fd);
    if (!ds) {
        send_headers(fd, "502 Bad Gateway", "text/plain", 0);
        tuner_release(t);
        return;
    }
    tuner_bind_channel(t, ch, DVB_PROGRAM_DEFAULT);
    tuner_set_stream(t, ds);

    /* no Content-Length — this is a live, unbounded stream */
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: video/mpeg\r\n"
        "Connection: close\r\nServer: hdhr-emu\r\n\r\n");
    if (write(fd, hdr, (size_t)hn) < 0) { release_http_pull(t, ch); return; }

    uint8_t buf[188 * 64];
    for (;;) {
        ssize_t n = dvb_stream_read(ds, buf, sizeof(buf));
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(fd, buf + off, (size_t)(n - off));
            if (w <= 0) goto out;
            off += w;
        }
    }
out:
    release_http_pull(t, ch); /* closes ds internally */
}

/* Only the web UI's /api/tuner<N>/channel route (web_ui.c) accepts
 * POST; every route below stays GET-only, same as before. Small
 * bodies only (a channel string, well under sizeof(body)) -- this
 * server has no chunked-encoding support, matching its existing
 * minimal-HTTP-parser scope. Assumes standard "Content-Length:"
 * casing, which is what every browser's fetch()/XHR actually sends --
 * the only client this route needs to serve. Takes the already-located
 * end-of-headers pointer (see handle_request()'s own header-reading
 * loop -- a single read() is *not* guaranteed to capture the whole
 * header block: confirmed live, a stray ~3KB Cookie header left over
 * from an unrelated earlier service on this same hostname pushed the
 * real POST body past what a fixed single read() ever saw, so the
 * body-extraction below used to silently grab leftover header bytes
 * instead of the actual value). */
static void handle_post_body(int fd, const char *req, size_t req_len, const char *hdr_end,
                              char *body, size_t body_cap, size_t *body_len_out)
{
    size_t body_len = 0;
    const char *body_start = hdr_end + 4;
    size_t already = (size_t)((req + req_len) - body_start);
    if (already > body_cap) already = body_cap;
    memcpy(body, body_start, already);
    body_len = already;

    const char *cl = strstr(req, "Content-Length:");
    long content_length = cl ? atol(cl + 15) : (long)body_len;
    if (content_length < 0) content_length = 0;
    if ((size_t)content_length > body_cap) content_length = (long)body_cap;
    while (body_len < (size_t)content_length) {
        ssize_t got = read(fd, body + body_len, (size_t)content_length - body_len);
        if (got <= 0) break;
        body_len += (size_t)got;
    }
    *body_len_out = body_len;
}

static void handle_request(int fd, struct hdhr_config *cfg, struct hdhr_tuner *tuners)
{
    /* Read until the full header block is in hand ("\r\n\r\n" found),
     * not just one read() -- headers can legitimately span multiple
     * TCP segments (large cookies, many Accept/Sec-prefixed headers
     * from a real browser, etc.), and both the request-line parse and
     * the POST body logic below need the complete, correctly-
     * terminated header block to work from. Bounded so a client that
     * never sends a terminator can't hold a connection thread forever. */
    char req[8192] = {0};
    size_t req_len = 0;
    char *hdr_end = NULL;
    while (req_len < sizeof(req) - 1) {
        ssize_t n = read(fd, req + req_len, sizeof(req) - 1 - req_len);
        if (n <= 0) { if (req_len == 0) return; break; }
        req_len += (size_t)n;
        req[req_len] = '\0';
        hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) break;
    }
    if (!hdr_end) { send_headers(fd, "431 Request Header Fields Too Large", "text/plain", 0); return; }

    char method[8] = {0}, path[512] = {0};
    if (sscanf(req, "%7s %511s", method, path) != 2) return;

    /* strip query string for routing */
    char *qs = strchr(path, '?');
    if (qs) *qs = '\0';

    if (strcmp(method, "POST") == 0) {
        char body[512] = {0};
        size_t body_len = 0;
        handle_post_body(fd, req, req_len, hdr_end, body, sizeof(body), &body_len);
        if (!web_ui_try_handle(fd, cfg, method, path, body, body_len)) {
            send_headers(fd, "405 Method Not Allowed", "text/plain", 0);
        }
        return;
    }
    if (strcmp(method, "GET") != 0) {
        send_headers(fd, "405 Method Not Allowed", "text/plain", 0);
        return;
    }

    if (strcmp(path, "/discover.json") == 0) {
        handle_discover_json(fd, cfg);
    } else if (strcmp(path, "/lineup_status.json") == 0) {
        handle_lineup_status_json(fd);
    } else if (strcmp(path, "/lineup.json") == 0) {
        handle_lineup_json(fd, cfg);
    } else if (strcmp(path, "/lineup.m3u") == 0) {
        handle_lineup_m3u(fd);
    } else if (strncmp(path, "/auto/v", 7) == 0) {
        size_t plen = strlen(path);
        int major = 0, minor = 0;
        sscanf(path + 7, "%d.%d", &major, &minor);
        if (plen > 4 && strcmp(path + plen - 4, ".m3u") == 0) {
            handle_m3u(fd, cfg, major, minor);
        } else {
            stream_channel_to_client(fd, cfg, tuners, -1, major, minor);
        }
    } else {
        int idx = -1, major = 0, minor = 0, consumed = 0;
        if (sscanf(path, "/tuner%d%n", &idx, &consumed) == 1 &&
            sscanf(path + consumed, "/v%d.%d", &major, &minor) == 2) {
            if (idx < 0 || idx >= cfg->tuner_count) {
                send_404(fd);
            } else {
                stream_channel_to_client(fd, cfg, tuners, idx, major, minor);
            }
        } else if (!web_ui_try_handle(fd, cfg, method, path, NULL, 0)) {
            send_404(fd);
        }
    }
}

static void *conn_thread_main(void *arg)
{
    struct http_ctx *hctx = (struct http_ctx *)arg;
    int one = 1;
    setsockopt(hctx->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    handle_request(hctx->fd, hctx->cfg, hctx->tuners);

    close(hctx->fd);
    free(hctx);
    return NULL;
}

void *http_thread_main(void *arg)
{
    struct control_ctx *ctl = (struct control_ctx *)arg; /* reuse: {cfg, tuners} — see control.h */
    struct hdhr_config *cfg = ctl->cfg;
    struct hdhr_tuner *tuners = ctl->tuners;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("http: socket");
        return NULL;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(HTTP_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("http: bind :80 (need root, or setcap CAP_NET_BIND_SERVICE)");
        close(fd);
        return NULL;
    }
    if (listen(fd, 32) < 0) {
        perror("http: listen");
        close(fd);
        return NULL;
    }

    fprintf(stderr, "http: listening on tcp/%d\n", HTTP_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peerlen = sizeof(peer);
        int cfd = accept(fd, (struct sockaddr *)&peer, &peerlen);
        if (cfd < 0) continue;

        struct http_ctx *hctx = malloc(sizeof(*hctx));
        hctx->fd = cfd;
        hctx->cfg = cfg;
        hctx->tuners = tuners;

        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread_main, hctx) != 0) {
            close(cfd);
            free(hctx);
            continue;
        }
        pthread_detach(th);
    }

    close(fd);
    return NULL;
}
