/*
 * hdhr-emu - pid_filter.h
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

#ifndef HDHR_PID_FILTER_H
#define HDHR_PID_FILTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Wire format for /tunerN/filter, confirmed against a genuine
 * HDHomeRun3 (2026-07-19): space-separated PID entries, each either a
 * single "0x%04x" or an inclusive range "0x%04x-0x%04x" — exactly what
 * libhdhomerun's hdhomerun_device_set_tuner_filter_by_array_append()
 * emits. PIDs are 13 bits (0x0000-0x1fff); 0x2000 (this project's
 * FULL_MUX_PID) is a DVB API wildcard, not a real PID, so it never
 * appears in filter text. */

#define PID_FILTER_MAX_RANGES 32

struct pid_range {
    uint16_t start;
    uint16_t end; /* inclusive */
};

struct pid_filter {
    struct pid_range ranges[PID_FILTER_MAX_RANGES];
    int count;
};

/* Parses filter text into ranges. Returns false on malformed input or
 * more ranges than PID_FILTER_MAX_RANGES — caller should reject the
 * SET in that case rather than apply a partial filter. Empty/blank
 * input parses to a zero-range (empty) filter, not an error. */
bool pid_filter_parse(const char *str, struct pid_filter *out);

/* Sum of each range's span — how many individual PIDs this filter
 * actually covers. Used to decide whether it's small enough to
 * enumerate as individual demux PES filters, or wide enough (e.g. the
 * default "0x0000-0x1fff" catch-all) that it has to fall back to
 * full-mux passthrough instead — see dvb_stream.c's MAX_DEMUX_FDS,
 * a hardware demux's number of concurrent PID filters is nowhere near
 * large enough to enumerate a range like that one PID at a time. */
int pid_filter_total_count(const struct pid_filter *f);

/* Formats a PID array back into the same wire format, merging
 * adjacent/equal values into ranges (matching
 * hdhomerun_device_set_tuner_filter_by_array's own merge behavior) —
 * used by control.c's /tunerN/filter GET to describe the PID set a
 * channel/program selection would actually stream. Input does not
 * need to be pre-sorted or pre-deduplicated. */
void pid_filter_format(const uint16_t *pids, int count, char *out, size_t out_len);

#endif /* HDHR_PID_FILTER_H */
