/*
 * hdhr-emu - getset_client.c
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
#include "getset_client.h"
#include "hdhr_pkt.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* A channel SET can legitimately take up to CHANNEL_SET_WAIT_MS (1.8s,
 * see control.c) before control.c replies, plus scheduling slack under
 * load — matched against real client tolerance (libhdhomerun's own
 * ~2.5s recv timeout, see reference_libhdhomerun_source memory), not
 * just this daemon's own worst case. */
#define GETSET_CLIENT_TIMEOUT_SEC 5

static ssize_t read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    uint8_t *p = buf;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) return -1;
        if (n == 0) return (ssize_t)got; /* peer closed */
        got += (size_t)n;
    }
    return (ssize_t)got;
}

static void set_err(char *err_out, size_t err_len, const char *msg)
{
    if (err_out && err_len > 0) snprintf(err_out, err_len, "%s", msg);
}

bool getset_client_call(const char *name, const char *value,
                         char *reply_out, size_t reply_len,
                         char *err_out, size_t err_len)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_err(err_out, err_len, "socket() failed");
        return false;
    }

    struct timeval tv = { .tv_sec = GETSET_CLIENT_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HDHR_CONTROL_TCP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        set_err(err_out, err_len, "connect to control port failed");
        return false;
    }

    struct hdhr_pkt req;
    hdhr_pkt_start_frame(&req);
    hdhr_pkt_write_tlv_str(&req, HDHR_TAG_GETSET_NAME, name);
    if (value) hdhr_pkt_write_tlv_str(&req, HDHR_TAG_GETSET_VALUE, value);
    size_t req_len = hdhr_pkt_seal_frame(&req, HDHR_TYPE_GETSET_REQ);

    if (write(fd, req.buffer, req_len) != (ssize_t)req_len) {
        close(fd);
        set_err(err_out, err_len, "write to control port failed");
        return false;
    }

    uint8_t header[4];
    if (read_full(fd, header, 4) != 4) {
        close(fd);
        set_err(err_out, err_len, "no reply from control port (timeout?)");
        return false;
    }
    uint16_t paylen = ((uint16_t)header[2] << 8) | header[3];

    uint8_t frame[HDHR_MAX_PACKET_SIZE];
    if (4 + (size_t)paylen + 4 > sizeof(frame)) {
        close(fd);
        set_err(err_out, err_len, "reply too large");
        return false;
    }
    memcpy(frame, header, 4);
    if (read_full(fd, frame + 4, (size_t)paylen + 4) != (ssize_t)paylen + 4) {
        close(fd);
        set_err(err_out, err_len, "truncated reply from control port");
        return false;
    }
    close(fd);

    struct hdhr_pkt pkt;
    uint16_t frame_type;
    if (hdhr_pkt_open_frame(&pkt, frame, 4 + (size_t)paylen + 4, &frame_type) != 0) {
        set_err(err_out, err_len, "malformed reply (bad CRC)");
        return false;
    }
    if (frame_type != HDHR_TYPE_GETSET_RPY) {
        set_err(err_out, err_len, "unexpected reply frame type");
        return false;
    }

    uint8_t tag;
    const uint8_t *tval;
    size_t tlen;
    int r;
    const uint8_t *found_value = NULL, *found_error = NULL;
    size_t found_value_len = 0, found_error_len = 0;
    while ((r = hdhr_pkt_read_tlv(&pkt, &tag, &tval, &tlen)) == 1) {
        if (tag == HDHR_TAG_GETSET_VALUE) { found_value = tval; found_value_len = tlen; }
        else if (tag == HDHR_TAG_ERROR_MESSAGE) { found_error = tval; found_error_len = tlen; }
    }
    if (r < 0) {
        set_err(err_out, err_len, "malformed reply TLVs");
        return false;
    }

    if (found_error) {
        if (err_out && err_len > 0) {
            size_t n = found_error_len < err_len - 1 ? found_error_len : err_len - 1;
            memcpy(err_out, found_error, n);
            err_out[n] = '\0';
        }
        return false;
    }
    if (found_value) {
        if (reply_out && reply_len > 0) {
            size_t n = found_value_len < reply_len - 1 ? found_value_len : reply_len - 1;
            memcpy(reply_out, found_value, n);
            reply_out[n] = '\0';
        }
        return true;
    }

    set_err(err_out, err_len, "reply had neither VALUE nor ERROR_MESSAGE");
    return false;
}
