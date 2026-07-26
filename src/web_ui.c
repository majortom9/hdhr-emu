/*
 * hdhr-emu - web_ui.c
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
#include "web_ui.h"
#include "getset_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* --- tiny local HTTP response helpers ---
 * Deliberately not shared with http_server.c's identical-looking
 * private helpers: reusing them would mean exporting internals across
 * a file boundary for ~15 lines of boilerplate, and this project
 * prefers each .c file self-contained (see e.g. dvb_frontend.c's own
 * copy of a legacy-seq calculation duplicated from dvb_stream.c rather
 * than shared, for the same reason). */
static void send_headers(int fd, const char *status, const char *content_type, long content_length)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "Connection: close\r\nServer: hdhr-emu\r\n\r\n",
                      status, content_type, content_length);
    if (write(fd, hdr, (size_t)n) < 0) { /* client already gone */ }
}

static void send_body(int fd, const char *status, const char *content_type, const char *body, size_t body_len)
{
    send_headers(fd, status, content_type, (long)body_len);
    if (write(fd, body, body_len) < 0) { /* client already gone */ }
}

static void send_404(int fd)
{
    send_headers(fd, "404 Not Found", "text/plain", 0);
}

/* --- plaintext GETSET value parsing ---
 * /tunerN/status and /tunerN/vchannel return simple space-separated
 * "key=value" tokens -- see control.c's handle_tuner_get(). Pulled out
 * one token at a time rather than assuming a fixed field order/count,
 * since e.g. status's bps=/pps= fields are only present while actively
 * streaming. */
static bool token_str(const char *s, const char *key, char *out, size_t outlen)
{
    const char *p = strstr(s, key);
    if (!p) return false;
    p += strlen(key);
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && p[n] != '\n' && n + 1 < outlen) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static int token_int(const char *s, const char *key, int fallback)
{
    char buf[32];
    if (!token_str(s, key, buf, sizeof(buf))) return fallback;
    return atoi(buf);
}

/* Minimal JSON string escaper, appended directly into a caller-owned
 * buffer/offset (same growth discipline as the rest of this file --
 * see append_tuner_json's own comment). ATSC short names are a fixed
 * 7-char PSIP field and essentially never contain '"'/'\\', but channel
 * names and error messages both flow into JSON here, so escape
 * properly rather than assume. Caller must already have enough
 * headroom (worst case ~2x strlen(s) plus a few bytes per control
 * char) -- see PER_TUNER_JSON_MAX's own sizing note. */
static void json_escape_append(char *out, size_t out_cap, size_t *off, const char *s)
{
    for (; *s && *off + 6 < out_cap; s++) {
        if (*s == '"' || *s == '\\') {
            out[(*off)++] = '\\';
            out[(*off)++] = *s;
        } else if ((unsigned char)*s < 0x20) {
            *off += (size_t)snprintf(out + *off, out_cap - *off, "\\u%04x", (unsigned char)*s);
        } else {
            out[(*off)++] = *s;
        }
    }
}

/* Worst-case bytes one tuner's JSON object can take: streaminfo's raw
 * wire value is capped at 512 bytes (control.c's handle_tuner_get()
 * builds every leaf reply, including streaminfo, into a shared
 * char val[512]), so even fully JSON-escaped (~2x) plus per-entry
 * object wrapper text and the other small fixed fields, 4096 bytes is
 * generous headroom -- verified against a live sweep during testing
 * (see plan's verification section), never observed over ~1.2KB. */
#define PER_TUNER_JSON_MAX 4096

/* Appends one tuner's status+vchannel+streaminfo as a JSON object
 * (no trailing comma -- callers join with their own separator) to
 * buf[*off..cap). Caller must guarantee at least PER_TUNER_JSON_MAX
 * bytes of headroom before calling. Talks to control.c purely as a
 * GETSET client (getset_client.c) -- never touches tuner structs
 * directly, so none of control.c's tuning/locking code is duplicated
 * or re-risked here. */
static void append_tuner_json(char *buf, size_t cap, size_t *off, int idx)
{
    char name[32], status[512], vchannel[512], streaminfo[512], err[128];

    snprintf(name, sizeof(name), "/tuner%d/status", idx);
    if (!getset_client_call(name, NULL, status, sizeof(status), err, sizeof(err))) {
        snprintf(status, sizeof(status), "ch=none lock=none ss=0 snq=0 seq=0");
    }
    snprintf(name, sizeof(name), "/tuner%d/vchannel", idx);
    if (!getset_client_call(name, NULL, vchannel, sizeof(vchannel), err, sizeof(err))) {
        snprintf(vchannel, sizeof(vchannel), "none");
    }
    snprintf(name, sizeof(name), "/tuner%d/streaminfo", idx);
    if (!getset_client_call(name, NULL, streaminfo, sizeof(streaminfo), err, sizeof(err))) {
        snprintf(streaminfo, sizeof(streaminfo), "none");
    }

    char physical[64] = "none", lock[16] = "none";
    token_str(status, "ch=", physical, sizeof(physical));
    token_str(status, "lock=", lock, sizeof(lock));
    int ss = token_int(status, "ss=", 0);
    int snq = token_int(status, "snq=", 0);
    int seq = token_int(status, "seq=", 0);
    int bps = token_int(status, "bps=", 0);
    int pps = token_int(status, "pps=", 0);

    *off += (size_t)snprintf(buf + *off, cap - *off, "{\"index\":%d,\"physical_channel\":\"", idx);
    json_escape_append(buf, cap, off, physical);
    *off += (size_t)snprintf(buf + *off, cap - *off, "\",\"vchannel\":\"");
    json_escape_append(buf, cap, off, vchannel);
    *off += (size_t)snprintf(buf + *off, cap - *off, "\",\"lock\":\"");
    json_escape_append(buf, cap, off, lock);
    *off += (size_t)snprintf(buf + *off, cap - *off,
        "\",\"signal_strength_pct\":%d,\"signal_quality_pct\":%d,"
        "\"symbol_quality_pct\":%d,\"bps\":%d,\"pps\":%d,\"streaminfo\":[",
        ss, snq, seq, bps, pps);

    /* streaminfo body: "tsid=0x....\n<program>: <major>.<minor> <name>\n..."
     * or a bare "none". Parse line by line, skipping the tsid= line
     * (and anything else that isn't a "N: M.M name" program line). */
    bool first = true;
    char si_copy[512];
    snprintf(si_copy, sizeof(si_copy), "%s", streaminfo);
    char *saveptr = NULL;
    for (char *line = strtok_r(si_copy, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        unsigned prog, major, minor;
        char cname[64];
        if (sscanf(line, "%u: %u.%u %63[^\n]", &prog, &major, &minor, cname) == 4) {
            *off += (size_t)snprintf(buf + *off, cap - *off,
                "%s{\"program\":%u,\"major\":%u,\"minor\":%u,\"name\":\"",
                first ? "" : ",", prog, major, minor);
            first = false;
            json_escape_append(buf, cap, off, cname);
            *off += (size_t)snprintf(buf + *off, cap - *off, "\"}");
        }
    }
    *off += (size_t)snprintf(buf + *off, cap - *off, "]}");
}

static void handle_tuners_json(int fd, const struct hdhr_config *cfg)
{
    size_t cap = 4096 + (size_t)cfg->tuner_count * PER_TUNER_JSON_MAX;
    char *buf = malloc(cap);
    if (!buf) { send_headers(fd, "500 Internal Server Error", "text/plain", 0); return; }

    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "{\"tuners\":[");
    for (int i = 0; i < cfg->tuner_count; i++) {
        off += (size_t)snprintf(buf + off, cap - off, "%s", i == 0 ? "" : ",");
        append_tuner_json(buf, cap, &off, i);
    }
    off += (size_t)snprintf(buf + off, cap - off, "]}\n");

    send_body(fd, "200 OK", "application/json", buf, off);
    free(buf);
}

/* Lightweight companion to /api/tuners.json, for the web UI's signal
 * trend chart -- that polls much faster (every ~1s vs. the full
 * dashboard's 2s), so it deliberately skips the vchannel/streaminfo
 * GETSET calls append_tuner_json() also makes, which the chart has no
 * use for and which would otherwise multiply the loopback round-trips
 * a fast poll makes for no benefit. Includes physical_channel/lock so
 * the chart can detect a channel change (and reset its history) or an
 * untuned tuner (and hide itself) without needing to correlate against
 * the separate, independently-timed /api/tuners.json poll. */
static void handle_stats_json(int fd, int idx)
{
    char name[32], status[512], err[128];
    snprintf(name, sizeof(name), "/tuner%d/status", idx);
    if (!getset_client_call(name, NULL, status, sizeof(status), err, sizeof(err))) {
        snprintf(status, sizeof(status), "ch=none lock=none ss=0 snq=0 seq=0");
    }

    char physical[64] = "none", lock[16] = "none";
    token_str(status, "ch=", physical, sizeof(physical));
    token_str(status, "lock=", lock, sizeof(lock));
    int ss = token_int(status, "ss=", 0);
    int snq = token_int(status, "snq=", 0);
    int seq = token_int(status, "seq=", 0);

    char json[256];
    size_t off = 0;
    off += (size_t)snprintf(json, sizeof(json), "{\"index\":%d,\"physical_channel\":\"", idx);
    json_escape_append(json, sizeof(json), &off, physical);
    off += (size_t)snprintf(json + off, sizeof(json) - off, "\",\"lock\":\"");
    json_escape_append(json, sizeof(json), &off, lock);
    off += (size_t)snprintf(json + off, sizeof(json) - off,
        "\",\"signal_strength_pct\":%d,\"signal_quality_pct\":%d,\"symbol_quality_pct\":%d}\n",
        ss, snq, seq);

    send_body(fd, "200 OK", "application/json", json, off);
}

/* POST /tuner<N>/channel body -- accepts either a bare value
 * ("us-bcast:25") or "value=us-bcast:25" (a browser
 * <form>/URLSearchParams POST would send the latter); tolerate both
 * rather than force the UI's JS into one specific encoding. */
static void extract_channel_value(const char *body, size_t body_len, char *out, size_t out_len)
{
    char tmp[256];
    size_t n = body_len < sizeof(tmp) - 1 ? body_len : sizeof(tmp) - 1;
    memcpy(tmp, body, n);
    tmp[n] = '\0';
    const char *v = strstr(tmp, "value=");
    if (v) v += 6; else v = tmp;
    snprintf(out, out_len, "%s", v);
}

static void handle_channel_post(int fd, int idx, const char *body, size_t body_len)
{
    char value[256];
    extract_channel_value(body, body_len, value, sizeof(value));

    char name[32], reply[192], err[128];
    snprintf(name, sizeof(name), "/tuner%d/channel", idx);
    if (!getset_client_call(name, value, reply, sizeof(reply), err, sizeof(err))) {
        char json[384];
        size_t off = 0;
        off += (size_t)snprintf(json, sizeof(json), "{\"error\":\"");
        json_escape_append(json, sizeof(json), &off, err);
        off += (size_t)snprintf(json + off, sizeof(json) - off, "\"}\n");
        send_body(fd, "400 Bad Request", "application/json", json, off);
        return;
    }

    /* handle_tuner_get()'s status branch (control.c) has a documented
     * "hold was only just opened" placeholder -- "ch=%s lock=none ss=45
     * snq=0 seq=0" -- published until held_stats_thread_main() (tuner.c,
     * HELD_STATS_REFRESH_MS=500) completes its first real stats read.
     * The channel SET above already blocks until the tune/lock result
     * itself is known, but a status GET fired immediately afterward can
     * still land in this narrow window before that background thread's
     * first reading is published. Retry a few times rather than surface
     * a placeholder as if it were real -- bounded so a tuner that
     * genuinely never locks (no signal) doesn't hang here: at most
     * ~3 * 300ms, well under a browser's own request patience. */
    char status_peek[512], peek_err[128];
    char status_name[32];
    snprintf(status_name, sizeof(status_name), "/tuner%d/status", idx);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!getset_client_call(status_name, NULL, status_peek, sizeof(status_peek), peek_err, sizeof(peek_err))) break;
        bool is_placeholder = strstr(status_peek, "lock=none") && strstr(status_peek, "ss=45") &&
                               strstr(status_peek, "snq=0") && strstr(status_peek, "seq=0");
        if (!is_placeholder) break;
        usleep(300 * 1000);
    }

    char buf[PER_TUNER_JSON_MAX];
    size_t off = 0;
    append_tuner_json(buf, sizeof(buf), &off, idx);
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "\n");

    send_body(fd, "200 OK", "application/json", buf, off);
}

/* --- static file serving --- */
static const char *content_type_for(const char *path)
{
    size_t len = strlen(path);
    if (len >= 5 && strcmp(path + len - 5, ".html") == 0) return "text/html";
    if (len >= 4 && strcmp(path + len - 4, ".css") == 0) return "text/css";
    if (len >= 3 && strcmp(path + len - 3, ".js") == 0) return "application/javascript";
    return "application/octet-stream";
}

static bool serve_static_file(int fd, const struct hdhr_config *cfg, const char *path)
{
    if (strstr(path, "..") != NULL) { send_404(fd); return true; } /* no path traversal */

    if (strcmp(path, "/") == 0) path = "/index.html";

    char full[512];
    snprintf(full, sizeof(full), "%s%s", cfg->web_root, path);

    FILE *f = fopen(full, "rb");
    if (!f) return false; /* let caller fall through to its own 404 */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 4 * 1024 * 1024) { fclose(f); send_404(fd); return true; }

    char *body = malloc((size_t)sz);
    if (!body) { fclose(f); send_headers(fd, "500 Internal Server Error", "text/plain", 0); return true; }
    size_t got = fread(body, 1, (size_t)sz, f);
    fclose(f);

    send_body(fd, "200 OK", content_type_for(full), body, got);
    free(body);
    return true;
}

bool web_ui_try_handle(int fd, const struct hdhr_config *cfg,
                        const char *method, const char *path,
                        const char *body, size_t body_len)
{
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/tuners.json") == 0) {
        handle_tuners_json(fd, cfg);
        return true;
    }

    int idx = -1, consumed = 0;
    if (strcmp(method, "GET") == 0 &&
        sscanf(path, "/api/tuner%d/stats.json%n", &idx, &consumed) == 1 &&
        path[consumed] == '\0') {
        if (idx < 0 || idx >= cfg->tuner_count) { send_404(fd); return true; }
        handle_stats_json(fd, idx);
        return true;
    }

    idx = -1; consumed = 0;
    if (strcmp(method, "POST") == 0 &&
        sscanf(path, "/api/tuner%d/channel%n", &idx, &consumed) == 1 &&
        path[consumed] == '\0') {
        if (idx < 0 || idx >= cfg->tuner_count) { send_404(fd); return true; }
        handle_channel_post(fd, idx, body, body_len);
        return true;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/", 5) != 0) {
        return serve_static_file(fd, cfg, path);
    }

    return false;
}
