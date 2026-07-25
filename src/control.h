/*
 * hdhr-emu - control.h
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

#ifndef HDHR_CONTROL_H
#define HDHR_CONTROL_H

#include "config.h"
#include "tuner.h"

struct control_ctx {
    struct hdhr_config *cfg;
    struct hdhr_tuner  *tuners; /* array of cfg->tuner_count */
};

/* Runs forever, accepting TCP control connections on port 65001 and
 * servicing GETSET_REQ frames. Intended to be launched in its own
 * thread; `arg` must point to a heap-allocated struct control_ctx that
 * outlives the thread (main.c owns it). */
void *control_thread_main(void *arg);

#endif /* HDHR_CONTROL_H */
