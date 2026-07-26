/*
 * hdhr-emu - web_ui.h
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
 * web_ui.h — phone/browser-friendly status+control dashboard
 * (web/index.html, style.css, app.js, served from cfg->web_root) plus
 * the small JSON API it talks to (/api/tuners.json,
 * /api/tuner<N>/channel). Talks to tuners exclusively via
 * getset_client.c, i.e. as its own GETSET client of control.c's
 * already-running, already-hardened control server — never touches
 * control.c's tuner structs/locking directly.
 */
#ifndef HDHR_WEB_UI_H
#define HDHR_WEB_UI_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>

/* Tries to handle one request. Returns true (and has already written a
 * full HTTP response to fd) if path matched an /api route or a static
 * file under cfg->web_root; false if http_server.c should fall through
 * to its own routes / a 404. body/body_len are the POST body (may be
 * NULL/0 for GET). */
bool web_ui_try_handle(int fd, const struct hdhr_config *cfg,
                        const char *method, const char *path,
                        const char *body, size_t body_len);

#endif /* HDHR_WEB_UI_H */
