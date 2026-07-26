/*
 * hdhr-emu - getset_client.h
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
 * getset_client.h — minimal loopback GETSET client, for talking to this
 * same daemon's own control.c server on 127.0.0.1 exactly the way
 * hdhomerun_config does. Used by web_ui.c so the web dashboard never has
 * to duplicate or call into control.c's tuning/locking logic directly —
 * it just becomes another (in-process, low-latency) GETSET client of
 * the already-hardened control server, same as any real client.
 */
#ifndef HDHR_GETSET_CLIENT_H
#define HDHR_GETSET_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* Performs one GET (value == NULL) or SET (value != NULL) against
 * name (e.g. "/tuner0/status"), connecting fresh to
 * 127.0.0.1:HDHR_CONTROL_TCP_PORT for this single request/reply.
 * On success, fills reply_out with the value string and returns true.
 * On failure (connection error, malformed reply, or an ERROR_MESSAGE
 * reply from the server), fills err_out with a human-readable message
 * and returns false. Either out buffer may be NULL if not needed. */
bool getset_client_call(const char *name, const char *value,
                         char *reply_out, size_t reply_len,
                         char *err_out, size_t err_len);

#endif /* HDHR_GETSET_CLIENT_H */
