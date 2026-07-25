/*
 * hdhr-emu - http_server.h
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

#ifndef HDHR_HTTP_SERVER_H
#define HDHR_HTTP_SERVER_H

#include "config.h"

/* Serves the HTTP surface real HDHomeRun firmware (and every modern DVR
 * client — Plex, Emby, Jellyfin, Channels DVR) actually talks to day to
 * day: discover.json, lineup.json, lineup_status.json, and the
 * /auto/v<channel> stream endpoint. A plain /auto/vX.X request doesn't
 * name a tuner slot, so it auto-allocates any currently-idle one via
 * tuner_pool_claim_free() (see tuner.h) — same arbitration real firmware
 * does internally across its N physical tuners, needed here because we
 * now own the DVB hardware directly instead of delegating to TVheadend.
 * Runs forever; launch in its own thread. `arg` must point to a
 * heap-allocated struct control_ctx (see control.h) that outlives it —
 * reused here rather than a bare struct hdhr_config* because HTTP pulls
 * need access to the same tuner pool control.c's target= pushes do. */
void *http_thread_main(void *arg);

#endif /* HDHR_HTTP_SERVER_H */
