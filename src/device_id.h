/*
 * hdhr-emu - device_id.h
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

#ifndef HDHR_DEVICE_ID_H
#define HDHR_DEVICE_ID_H

#include <stdint.h>
#include <stdbool.h>

/* Real HDHomeRun device IDs are self-checking: a nibble-XOR checksum over
 * the 8 hex digits must equal zero once each high nibble is passed through
 * a fixed 4-bit lookup table. The official SiliconDust apps (and
 * hdhomerun_config) reject IDs that fail this check as "not a genuine
 * device"; third-party DVR software (Plex/Emby/Jellyfin/Channels DVR)
 * doesn't care either way. We implement it anyway for full fidelity. */

bool hdhr_device_id_is_valid(uint32_t device_id);

/* Takes any 24-bit seed (e.g. derived from the Pi's serial/MAC) and
 * returns a 32-bit device id with a valid checksum, in the same style
 * real devices use: top nibble is a fixed device-class digit (we use 0x1,
 * matching early tuner-class ids), followed by 6 seed nibbles, with the
 * low nibble solved for so the checksum comes out to zero. */
uint32_t hdhr_device_id_generate(uint32_t seed24);

#endif /* HDHR_DEVICE_ID_H */
