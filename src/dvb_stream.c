/*
 * hdhr-emu - dvb_stream.c
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
 * dvb_stream.c — tunes a frontend, sets demux PID filters (or full-mux
 * passthrough), and captures the resulting multiplexed TS from
 * /dev/dvb/adapter?/dvr? into a ring buffer, same consumer contract
 * tvh_stream.c used to provide (dvb_stream_read/close mirror
 * tvh_stream_read/close exactly) so http_server.c/udp_stream.c barely
 * had to change.
 */
#include "dvb_stream.h"
#include "dvb_frontend.h"
#include "dvb_channel.h"
#include "pid_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/dvb/dmx.h>

#define RING_CAP (1 << 20) /* 1 MiB, same size tvh_stream used */
#define MAX_DEMUX_FDS 6
#define LOCK_TIMEOUT_MS 2000
#define FULL_MUX_PID 0x2000 /* DVB API wildcard: "all PIDs" full-TS passthrough */
#define MIN_WINDOW_SEC 1.0  /* dvb_stream_get_rate()'s minimum sampling window — see its comment */
/* Real firmware's "packet" unit for /tunerN/status's pps= field: 7
 * 188-byte MPEG-TS packets bundled into one 1316-byte chunk — the
 * classic early-HDHomeRun UDP datagram packing (see udp_stream.c's own
 * identical DATAGRAM_SIZE), used as pps='s unit here even for HTTP
 * passthrough, which has no actual 1316-byte framing of its own. Not
 * shared with udp_stream.c's constant directly — dvb_stream.c
 * shouldn't depend on udp_stream.c, wrong layering direction — but
 * must stay numerically identical to it. */
#define STATUS_PACKET_UNIT_BYTES (188 * 7)

struct dvb_stream {
    int frontend_fd;
    int demux_fds[MAX_DEMUX_FDS];
    uint16_t demux_pids[MAX_DEMUX_FDS]; /* which PID each demux_fds[i] is filtering, for real dedup */
    int demux_fd_count;
    int dvr_fd;
    pthread_t thread;

    uint8_t ring[RING_CAP];
    size_t  head, tail, fill;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty, not_full;

    volatile int closing;
    volatile int eof;

    /* live throughput bookkeeping — see dvb_stream_get_rate(). total_bytes
     * is the lifetime counter (used to detect any activity at all);
     * window_* track a short recent interval so the reported rate is
     * "what's happening now", not a lifetime average that stays skewed
     * by an initial buffer-fill burst for a long time after startup. */
    uint64_t total_bytes;
    struct timespec start_time;
    uint64_t window_start_bytes;
    struct timespec window_start_time;
    double last_bps;
    double last_pps;

    /* legacy symbol-quality fallback (FE_READ_UNCORRECTED_BLOCKS) — see
     * dvb_stream_get_legacy_seq_pct(). Only used for drivers (confirmed:
     * lgdt3306a) that don't populate the modern DVBv5 block-count stats. */
    bool     legacy_seq_have_baseline;
    uint32_t legacy_seq_last_ucblocks;
    struct timespec legacy_seq_last_time;
    int      legacy_seq_last_pct;

    /* Background refresher for this stream's own live frontend stats —
     * same fix as tuner.c's held_stats_thread (see its own comment in
     * tuner.h), applied here too: dvb_frontend_read_stats() can itself
     * block for multiple seconds on this hardware (confirmed live for
     * the held-tuner case, up to 6.8s observed) — calling it directly
     * from a connection thread answering a /tunerN/status GET while
     * this stream is active risked exceeding a real client's own
     * patience. Confirmed live (2026-08-28) this isn't just an
     * occasional held-tuner risk: while actively streaming, the read
     * consistently took just over a second on this hardware — every
     * single status poll during playback, not a rare stall. Owns every
     * read of frontend_fd's stats; dvb_stream_get_cached_stats() only
     * ever reads the cache. Guarded by s->mtx, like the ring buffer
     * fields above, except stats_thread itself (only ever touched by
     * whichever code currently owns starting/joining it). */
    pthread_t stats_thread;
    bool      stats_thread_started;
    volatile bool stats_stop_requested;
    struct dvb_signal_stats stats_cache;
    bool      stats_cache_valid;
};

static int open_pes_pid_filter(int adapter, int demux_num, uint16_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/dvb/adapter%d/demux%d", adapter, demux_num);

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "dvb_stream: open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    struct dmx_pes_filter_params params;
    memset(&params, 0, sizeof(params));
    params.pid = pid;
    params.input = DMX_IN_FRONTEND;
    params.output = DMX_OUT_TS_TAP;
    params.pes_type = DMX_PES_OTHER;

    if (ioctl(fd, DMX_SET_PES_FILTER, &params) < 0) {
        fprintf(stderr, "dvb_stream: DMX_SET_PES_FILTER pid=0x%04X failed: %s\n",
                pid, strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, DMX_START) < 0) {
        fprintf(stderr, "dvb_stream: DMX_START pid=0x%04X failed: %s\n", pid, strerror(errno));
        close(fd);
        return -1;
    }
    fprintf(stderr, "[DEBUG] open_pes_pid_filter: opened demux fd=%d for pid=0x%04X on %s\n", fd, pid, path);
    return fd;
}

/* Adds a PID filter unless it's a duplicate of one already opened — PCR very commonly
 * shares the video PID on ATSC broadcasts, and opening two separate PES
 * filters on the same PID genuinely duplicates every packet on that PID
 * into the shared DVR device (each filter independently matches and
 * emits it), not just a cosmetic double-count — this corrupts the
 * resulting elementary stream (a decoder sees interleaved duplicate PES
 * packets) in addition to inflating the measured bitrate. */
static bool add_pid(struct dvb_stream *s, int adapter, int demux_num, uint16_t pid)
{
    for (int i = 0; i < s->demux_fd_count; i++) {
        if (s->demux_pids[i] == pid) return true; /* already filtering this PID — nothing to do */
    }
    if (s->demux_fd_count >= MAX_DEMUX_FDS) {
        fprintf(stderr, "dvb_stream: too many PID filters requested (max %d)\n", MAX_DEMUX_FDS);
        return false;
    }
    int fd = open_pes_pid_filter(adapter, demux_num, pid);
    if (fd < 0) return false;
    s->demux_pids[s->demux_fd_count] = pid;
    s->demux_fds[s->demux_fd_count] = fd;
    s->demux_fd_count++;
    return true;
}

/* add_pid() for a PID taken from a dvb_channel field, where 0 means "not
 * applicable" (e.g. no audio stream found) rather than a real PID.
 * PAT's genuine PID 0x0000 must go through add_pid() directly instead --
 * an earlier version had this zero check inside the one shared helper,
 * which silently dropped PAT from every program-filtered stream (it was
 * only ever present in full-mux passthrough). Players then couldn't find
 * the PMT and fell back to blind probing: ffmpeg/mpv read their whole
 * 5MB probesize before starting, ~5s on an HD channel and 20-30s on a
 * ~1.3Mbps SD subchannel -- confirmed live by capturing /auto/v2.2 (no
 * PID 0x0000 packets at all) and ffprobe's "bytes read:5013504
 * nb_streams:0" before stream discovery. */
static bool add_pid_if_new(struct dvb_stream *s, int adapter, int demux_num, uint16_t pid)
{
    if (pid == 0) return true;
    return add_pid(s, adapter, demux_num, pid);
}

/* How often stats_thread_main() refreshes this stream's cached signal
 * stats. Same value/rationale as tuner.c's HELD_STATS_REFRESH_MS (see
 * its own comment): short enough that /tunerN/status still looks live,
 * comfortably longer than even the worst confirmed-live stat-read stall
 * (6.8s) so this thread's own loop can't itself be a source of runaway
 * ioctl spam. */
#define STATS_REFRESH_MS 500

/* Owns every read of frontend_fd's live signal stats while this stream
 * is active — see stats_thread's own comment in the struct above. Runs
 * until told to stop (stats_stop_requested set, or the stream is
 * closing) — checked only *between* iterations, since
 * dvb_frontend_read_stats() itself can't be interrupted once it's
 * blocked inside a kernel ioctl on this hardware. Never closes
 * frontend_fd itself; that stays dvb_stream_close()'s job, same as
 * before this thread existed — this thread purely caches. */
static void *stats_thread_main(void *arg)
{
    struct dvb_stream *s = (struct dvb_stream *)arg;

    for (;;) {
        struct dvb_signal_stats stats;
        dvb_frontend_read_stats(s->frontend_fd, &stats); /* the call this
            * whole thread exists to keep off any connection thread —
            * see STATS_REFRESH_MS's own comment above. */
        if (stats.symbol_quality_pct < 0) {
            /* Modern DVBv5 block-count stats unavailable on this driver
             * (confirmed: lgdt3306a) — fall back to the legacy
             * FE_READ_UNCORRECTED_BLOCKS ioctl. Safe to call from this
             * thread specifically because, once this thread exists, it's
             * the sole owner of the legacy_seq_* fields too — no other
             * caller reads live stats for an active stream anymore. */
            int legacy = dvb_stream_get_legacy_seq_pct(s, stats.has_lock);
            if (legacy >= 0) stats.symbol_quality_pct = legacy;
        }

        pthread_mutex_lock(&s->mtx);
        if (s->stats_stop_requested || s->closing) {
            pthread_mutex_unlock(&s->mtx);
            break;
        }
        s->stats_cache = stats;
        s->stats_cache_valid = true;
        pthread_mutex_unlock(&s->mtx);

        usleep(STATS_REFRESH_MS * 1000);

        pthread_mutex_lock(&s->mtx);
        bool stop = (s->stats_stop_requested || s->closing);
        pthread_mutex_unlock(&s->mtx);
        if (stop) break;
    }
    return NULL;
}

static void *reader_thread_main(void *arg)
{
    struct dvb_stream *s = (struct dvb_stream *)arg;
    uint8_t chunk[188 * 64];

    while (!s->closing) {
        struct pollfd pfd = { .fd = s->dvr_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue; /* timeout or interrupted — loop back and re-check closing */

        ssize_t n = read(s->dvr_fd, chunk, sizeof(chunk));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            fprintf(stderr, "[dvb_stream] reader_thread_main: read() failed, dvr_fd=%d, errno=%d %s\n",
                s->dvr_fd, errno, strerror(errno));
            break; /* real error or EOF */
        }

        pthread_mutex_lock(&s->mtx);
        s->total_bytes += (uint64_t)n;
        size_t off = 0;
        while (off < (size_t)n && !s->closing) {
            while (s->fill == RING_CAP && !s->closing) {
                pthread_cond_wait(&s->not_full, &s->mtx);
            }
            if (s->closing) break;

            size_t space = RING_CAP - s->fill;
            size_t remaining = (size_t)n - off;
            size_t chunk_len = remaining < space ? remaining : space;
            size_t first = RING_CAP - s->head;
            if (first > chunk_len) first = chunk_len;
            memcpy(s->ring + s->head, chunk + off, first);
            if (chunk_len > first) memcpy(s->ring, chunk + off + first, chunk_len - first);
            s->head = (s->head + chunk_len) % RING_CAP;
            s->fill += chunk_len;
            off += chunk_len;
            pthread_cond_signal(&s->not_empty);
        }
        pthread_mutex_unlock(&s->mtx);
    }

    pthread_mutex_lock(&s->mtx);
    s->eof = 1;
    pthread_cond_signal(&s->not_empty);
    pthread_mutex_unlock(&s->mtx);
    return NULL;
}

struct dvb_stream *dvb_stream_open(int adapter, int frontend, int demux_num,
                                    const struct dvb_channel *channel,
                                    int program_override, const char *pid_filter,
                                    int adopt_fd)
{
    struct dvb_stream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->not_empty, NULL);
    pthread_cond_init(&s->not_full, NULL);

    /* An adopted fd (see this function's own comment in dvb_stream.h) is
     * already open, and -- for a held or parked fd on this same mux --
     * already tuned and locked, so neither the open nor the tune is
     * repeated. One on a *different* frequency (a parked fd from the
     * previous channel, see tuner.h's parked_fd) is retuned in place:
     * still no close/reopen, which on this driver means a full chip
     * re-init and a relock that can miss LOCK_TIMEOUT_MS outright. */
    bool adopted = (adopt_fd >= 0);
    bool qam = (channel->delivery == HDHR_DELIVERY_QAM);
    if (adopted) {
        s->frontend_fd = adopt_fd;
    } else {
        s->frontend_fd = dvb_frontend_open(adapter, frontend);
        if (s->frontend_fd < 0) { free(s); return NULL; }
    }

    if (!adopted || !dvb_frontend_is_tuned_to(s->frontend_fd, channel->frequency_hz, qam)) {
        /* channel->delivery (stamped at scan time — see dvb_scan.c) tells
         * us which tune function this mux actually needs; a stream open
         * only gets the dvb_channel pointer, not whatever channelmap was
         * active when it was originally scanned. */
        int tune_rc = qam ? dvb_frontend_tune_qam(s->frontend_fd, channel->frequency_hz)
                          : dvb_frontend_tune_8vsb(s->frontend_fd, channel->frequency_hz);
        if (tune_rc != 0) {
            dvb_frontend_close(s->frontend_fd);
            free(s);
            return NULL;
        }
    }
    /* Returns on its first status read if already locked (the common
     * adopted case), so this costs nothing there. */
    struct timespec lock_t0, lock_t1;
    clock_gettime(CLOCK_MONOTONIC, &lock_t0);
    bool locked = dvb_frontend_wait_lock(s->frontend_fd, LOCK_TIMEOUT_MS, NULL, NULL);
    bool retried = false;
    if (!locked) {
        /* One more attempt on a freshly reopened frontend. Measured live
         * (2026-10-10, cold opens with a temporary 6s timeout): a lock
         * either lands in ~650-710ms or not at all -- nothing arrived
         * between 0.7s and 6s -- so waiting longer doesn't help, but an
         * occasional cold open of a strong channel (2.1: -45dBm, 22dB
         * SNR) still failed: dmesg showed lgdt3306a's search loop=0..4
         * all missing right after lgdt3306a_init. Re-issuing the tune on
         * the same fd does nothing -- the driver logs "already set,
         * skipping" for identical parameters and just reruns the failed
         * search -- so the retry closes and reopens the frontend for a
         * genuine re-init. A channel with no signal at all (e.g. a stale
         * cache entry) just takes ~2x LOCK_TIMEOUT_MS to 502. */
        retried = true;
        dvb_frontend_close(s->frontend_fd);
        s->frontend_fd = dvb_frontend_open(adapter, frontend);
        if (s->frontend_fd < 0) { free(s); return NULL; }
        int rc = qam ? dvb_frontend_tune_qam(s->frontend_fd, channel->frequency_hz)
                     : dvb_frontend_tune_8vsb(s->frontend_fd, channel->frequency_hz);
        locked = (rc == 0) && dvb_frontend_wait_lock(s->frontend_fd, LOCK_TIMEOUT_MS, NULL, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &lock_t1);
    /* One line per stream open -- low volume, and the lock timing is the
     * first thing worth knowing when a client reports a slow start. */
    fprintf(stderr, "dvb_stream: %d.%d (%s frontend) lock=%d after %ldms%s\n",
            channel->major, channel->minor, adopted ? "adopted" : "fresh", locked,
            (lock_t1.tv_sec - lock_t0.tv_sec) * 1000L + (lock_t1.tv_nsec - lock_t0.tv_nsec) / 1000000L,
            retried ? " (frontend reopened once)" : "");
    if (!locked) {
        fprintf(stderr, "dvb_stream: no lock on %u Hz for %d.%d%s\n",
                channel->frequency_hz, channel->major, channel->minor,
                adopted ? " (adopted frontend fd)" : "");
        dvb_frontend_close(s->frontend_fd);
        free(s);
        return NULL;
    }

    bool ok = true;
    struct pid_filter pf;
    bool have_filter = pid_filter && pid_filter[0] &&
                        pid_filter_parse(pid_filter, &pf) && pf.count > 0;

    if (have_filter && pid_filter_total_count(&pf) <= MAX_DEMUX_FDS - 1) {
        /* Explicit filter, small enough to enumerate as individual
         * demux PES filters (reserving one slot for PAT below) —
         * takes over PID selection entirely, ignoring program_override,
         * same as real firmware treats an explicit /tunerN/filter as a
         * lower-level override. */
        fprintf(stderr, "dvb_stream: %d.%d: explicit PID filter (%d PIDs)\n",
                channel->major, channel->minor, pid_filter_total_count(&pf));
        ok = add_pid(s, adapter, demux_num, 0x0000); /* PAT, same as the program path below */
        for (int i = 0; i < pf.count && ok; i++) {
            for (uint32_t pid = pf.ranges[i].start; pid <= pf.ranges[i].end && ok; pid++) {
                ok = add_pid_if_new(s, adapter, demux_num, (uint16_t)pid);
            }
        }
    } else if (have_filter || program_override == 0) {
        /* Either program=0's full-mux passthrough, or an explicit
         * filter too wide to enumerate one PID at a time (e.g. the
         * real device's own "0x0000-0x1fff" default) — a hardware
         * demux only has a handful of concurrent PID filter slots
         * (MAX_DEMUX_FDS), nowhere near enough for a range like that
         * one PID per filter, so fall back to the wildcard filter
         * instead of failing outright. */
        fprintf(stderr, "dvb_stream: %d.%d: full-mux passthrough (%s)\n",
                channel->major, channel->minor,
                have_filter ? "filter too wide to enumerate" : "program=0");
        ok = add_pid_if_new(s, adapter, demux_num, FULL_MUX_PID);
    } else {
        const struct dvb_channel *target = channel;
        struct dvb_channel resolved;
        if (program_override > 0 && program_override != channel->program_number) {
            const struct dvb_channel *siblings[DVB_CHANNEL_MAX];
            int n = dvb_channels_on_freq(channel->frequency_hz, siblings, DVB_CHANNEL_MAX);
            bool found = false;
            for (int i = 0; i < n; i++) {
                if (siblings[i]->program_number == (uint16_t)program_override) {
                    resolved = *siblings[i];
                    target = &resolved;
                    found = true;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "dvb_stream: requested program %d not found on this mux, "
                                 "falling back to %d.%d's own program %u\n",
                                 program_override, channel->major, channel->minor,
                                 channel->program_number);
            }
        }

        /* Always include PAT + this program's PMT so a player can
         * actually identify and decode the program, plus PCR/video/audio.
         * add_pid_if_new now genuinely dedups by tracking each demux
         * fd's PID (see its own comment) — this matters because PCR and
         * video very commonly share a PID on ATSC broadcasts, and an
         * earlier version of this code opened a second real filter on
         * the same PID in that case, duplicating packets rather than
         * harmlessly merging them. */
        ok = ok && add_pid(s, adapter, demux_num, 0x0000); /* PAT -- add_pid(), not _if_new: see the latter's comment */
        ok = ok && add_pid_if_new(s, adapter, demux_num, target->pmt_pid);
        ok = ok && add_pid_if_new(s, adapter, demux_num, target->pcr_pid);
        ok = ok && add_pid_if_new(s, adapter, demux_num, target->video_pid);
        ok = ok && add_pid_if_new(s, adapter, demux_num, target->audio_pid);
    }

    if (!ok || s->demux_fd_count == 0) {
        for (int i = 0; i < s->demux_fd_count; i++) close(s->demux_fds[i]);
        dvb_frontend_close(s->frontend_fd);
        free(s);
        return NULL;
    }

    char dvr_path[64];
    snprintf(dvr_path, sizeof(dvr_path), "/dev/dvb/adapter%d/dvr%d", adapter, demux_num);
    s->dvr_fd = open(dvr_path, O_RDONLY);
    if (s->dvr_fd < 0) {
        fprintf(stderr, "dvb_stream: open %s failed: %s\n", dvr_path, strerror(errno));
        for (int i = 0; i < s->demux_fd_count; i++) close(s->demux_fds[i]);
        dvb_frontend_close(s->frontend_fd);
        free(s);
        return NULL;
    }

    if (pthread_create(&s->thread, NULL, reader_thread_main, s) != 0) {
        close(s->dvr_fd);
        for (int i = 0; i < s->demux_fd_count; i++) close(s->demux_fds[i]);
        dvb_frontend_close(s->frontend_fd);
        free(s);
        return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &s->start_time);
    s->window_start_time = s->start_time;
    s->window_start_bytes = 0;

    /* Not fatal if this fails to start — /tunerN/status just falls back
     * to the "still checking" placeholder (dvb_stream_get_cached_stats()
     * returning false) instead of live stats, same degraded-but-safe
     * behavior tuner.c's install_hold_fd() already accepts for the
     * equivalent held-tuner case. */
    if (pthread_create(&s->stats_thread, NULL, stats_thread_main, s) == 0) {
        s->stats_thread_started = true;
    } else {
        fprintf(stderr, "dvb_stream: failed to start stats refresher thread -- "
                         "/tunerN/status will show a placeholder instead of live "
                         "stats for this stream\n");
        s->stats_thread_started = false;
    }

    return s;
}

ssize_t dvb_stream_read(struct dvb_stream *s, void *buf, size_t len)
{
    pthread_mutex_lock(&s->mtx);
    while (s->fill == 0 && !s->eof && !s->closing) {
        pthread_cond_wait(&s->not_empty, &s->mtx);
    }
    if (s->fill == 0) {
        pthread_mutex_unlock(&s->mtx);
        return 0;
    }

    size_t chunk_len = len < s->fill ? len : s->fill;
    size_t first = RING_CAP - s->tail;
    if (first > chunk_len) first = chunk_len;
    memcpy(buf, s->ring + s->tail, first);
    if (chunk_len > first) memcpy((uint8_t *)buf + first, s->ring, chunk_len - first);
    s->tail = (s->tail + chunk_len) % RING_CAP;
    s->fill -= chunk_len;

    pthread_cond_signal(&s->not_full);
    pthread_mutex_unlock(&s->mtx);
    return (ssize_t)chunk_len;
}

/* Shared by dvb_stream_close()/dvb_stream_close_keep_frontend(). */
static int stream_teardown(struct dvb_stream *s, bool keep_frontend)
{

    pthread_mutex_lock(&s->mtx);
    s->closing = 1;
    s->stats_stop_requested = true;
    pthread_cond_broadcast(&s->not_full);
    pthread_cond_broadcast(&s->not_empty);
    pthread_mutex_unlock(&s->mtx);

    pthread_join(s->thread, NULL);
    /* Must finish before frontend_fd is closed below — same requirement
     * as tuner.c's stop_held_stats_thread(), and the same tradeoff: can
     * block briefly (up to however long stats_thread_main's current,
     * possibly slow, stat-read takes), but this only ever runs at
     * stream-teardown time, never on the /tunerN/status GET path itself,
     * which is the entire point of this mechanism. */
    if (s->stats_thread_started) pthread_join(s->stats_thread, NULL);

    close(s->dvr_fd);
    for (int i = 0; i < s->demux_fd_count; i++) close(s->demux_fds[i]);
    int ffd = s->frontend_fd;
    if (!keep_frontend) {
        dvb_frontend_close(ffd);
        ffd = -1;
    }

    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->not_empty);
    pthread_cond_destroy(&s->not_full);
    free(s);
    return ffd;
}

void dvb_stream_close(struct dvb_stream *s)
{
    if (!s) return;
    stream_teardown(s, false);
}

int dvb_stream_close_keep_frontend(struct dvb_stream *s)
{
    if (!s) return -1;
    return stream_teardown(s, true);
}

bool dvb_stream_get_cached_stats(struct dvb_stream *s, struct dvb_signal_stats *out)
{
    if (!s) return false;
    pthread_mutex_lock(&s->mtx);
    bool valid = s->stats_cache_valid;
    if (valid) *out = s->stats_cache;
    pthread_mutex_unlock(&s->mtx);
    return valid;
}

/* ATSC 8VSB segment rate, per ATSC A/53: symbol_rate (10,762,237 Hz,
 * exact) / segment_length (832 symbols, including 4-symbol segment
 * sync) = ~12935.38 segments/sec. One RS(207,187)-coded block per data
 * segment, decoding to one 188-byte TS packet — this ignores the
 * ~0.32% overhead from the once-per-313-segments Field Sync Segment
 * (which carries no data block), a deliberate simplification for a
 * coarse quality indicator rather than an exact accounting. This is a
 * fixed physical constant of the ATSC standard, not a driver-dependent
 * guess like the dBm/dBmV signal-strength scale was. */
#define ATSC_SEGMENTS_PER_SEC 12935.38

int dvb_stream_get_legacy_seq_pct(struct dvb_stream *s, bool has_lock)
{
    if (!s) return -1;

    if (!has_lock) {
        /* See dvb_frontend_legacy_seq_pct()'s identical guard: the
         * uncorrected-blocks counter doesn't move without a lock, which
         * this function would otherwise misread as a perfect "zero
         * errors" window. Reset the baseline too, so a later re-lock
         * doesn't measure across the unlocked gap. */
        s->legacy_seq_have_baseline = false;
        return -1;
    }

    uint32_t ucblocks;
    if (!dvb_frontend_read_legacy_ucblocks(s->frontend_fd, &ucblocks)) {
        return -1; /* driver doesn't support this ioctl either */
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!s->legacy_seq_have_baseline) {
        s->legacy_seq_have_baseline = true;
        s->legacy_seq_last_ucblocks = ucblocks;
        s->legacy_seq_last_time = now;
        s->legacy_seq_last_pct = -1;
        return -1; /* no window yet — next call will have one */
    }

    double elapsed = (now.tv_sec - s->legacy_seq_last_time.tv_sec) +
                      (now.tv_nsec - s->legacy_seq_last_time.tv_nsec) / 1e9;

    /* Same "don't measure a near-zero window" guard as dvb_stream_get_rate(),
     * and for the same reason: return the last computed value rather than
     * a noisy figure from an interval that's too short to be meaningful. */
    if (elapsed < 0.5) {
        return s->legacy_seq_last_pct;
    }

    /* ucblocks is a cumulative counter (confirmed via the kernel DVB API
     * docs: "number of uncorrected blocks detected... during its
     * lifetime"), so guard against a counter reset (channel change,
     * driver quirk) producing a negative delta. */
    uint32_t delta_blocks = (ucblocks >= s->legacy_seq_last_ucblocks)
                             ? (ucblocks - s->legacy_seq_last_ucblocks) : 0;

    double expected_blocks = ATSC_SEGMENTS_PER_SEC * elapsed;
    double error_ratio = expected_blocks > 0 ? (delta_blocks / expected_blocks) : 0.0;
    int pct = (int)((1.0 - error_ratio) * 100.0 + 0.5);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    s->legacy_seq_last_ucblocks = ucblocks;
    s->legacy_seq_last_time = now;
    s->legacy_seq_last_pct = pct;
    return pct;
}

void dvb_stream_get_rate(const struct dvb_stream *s, double *bits_per_second,
                          double *packets_per_second)
{
    *bits_per_second = 0.0;
    *packets_per_second = 0.0;
    if (!s) return;

    /* total_bytes/window_* are written under s->mtx by the reader
     * thread; cast away const to lock the same mutex for a consistent
     * read+update (the struct's mutex itself isn't logically
     * const-violating here). */
    struct dvb_stream *ns = (struct dvb_stream *)s;
    pthread_mutex_lock(&ns->mtx);
    uint64_t bytes_now = s->total_bytes;
    uint64_t window_bytes = s->window_start_bytes;
    struct timespec window_start = s->window_start_time;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - window_start.tv_sec) +
                      (now.tv_nsec - window_start.tv_nsec) / 1e9;

    /* Windowed rate: bytes captured since the *last* status query,
     * divided by the time since that query — reflects current
     * throughput, not a lifetime average that stays distorted by
     * whatever burst happened right after the stream opened (e.g. the
     * ring buffer's initial fill). Require at least MIN_WINDOW_SEC
     * between windows so back-to-back status polls don't divide by
     * near-zero and produce a noisy/inflated instantaneous spike.
     *
     * 200ms used to be the guard here; widened to a full second on the
     * theory that it was too short to average out real DVB delivery's
     * natural burstiness. That theory was wrong, or at least incomplete
     * — confirmed 2026-07-20: hdhomerun_config_gui's "Network Rate"
     * field is computed client-side from *our* pps= alone
     * (packets_per_second * 1316 * 8, see its HDHRConfig.cpp), not from
     * our bps= field at all, and this pps= was counting raw 188-byte
     * MPEG-TS packets instead of 1316-byte UDP datagrams (7 TS packets
     * each — real firmware's actual meaning of "packet" in this field,
     * confirmed by that same GUI formula, and matching this project's
     * own DATAGRAM_SIZE in udp_stream.c). Since 1316 = 7*188, that's a
     * flat, systematic 7x inflation on every single reading, regardless
     * of window length or polling pattern — not a burst-averaging
     * problem, which is exactly why the 1s-window change alone never
     * fixed it, and why plain `hdhomerun_config get /tunerN/status`
     * never surfaced it either (it just prints bps= verbatim, which was
     * already correct; nobody was hand-computing pps*1316*8 to notice
     * pps= itself was wrong). Widening the window was still a
     * legitimate improvement for the bps= field's own real jitter, so
     * left as-is. */
    if (elapsed >= MIN_WINDOW_SEC) {
        uint64_t delta_bytes = bytes_now - window_bytes;
        ns->last_bps = (delta_bytes * 8.0) / elapsed;
        ns->last_pps = (delta_bytes / (double)STATUS_PACKET_UNIT_BYTES) / elapsed;

        ns->window_start_bytes = bytes_now;
        ns->window_start_time = now;
    }
    /* else: too soon since the last window closed (e.g. a GUI polling
     * status faster than 1x/sec) — return the last computed rate rather
     * than flickering to 0 between windows. */
    *bits_per_second = ns->last_bps;
    *packets_per_second = ns->last_pps;

    pthread_mutex_unlock(&ns->mtx);
}
