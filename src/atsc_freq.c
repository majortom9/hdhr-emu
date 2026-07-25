/*
 * hdhr-emu - atsc_freq.c
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

#include "atsc_freq.h"

const struct atsc_channel_freq atsc_freq_table[ATSC_FREQ_TABLE_COUNT] = {
    {  2,   57000000 },
    {  3,   63000000 },
    {  4,   69000000 },
    {  5,   79000000 },
    {  6,   85000000 },
    {  7,  177000000 },
    {  8,  183000000 },
    {  9,  189000000 },
    { 10,  195000000 },
    { 11,  201000000 },
    { 12,  207000000 },
    { 13,  213000000 },
    { 14,  473000000 },
    { 15,  479000000 },
    { 16,  485000000 },
    { 17,  491000000 },
    { 18,  497000000 },
    { 19,  503000000 },
    { 20,  509000000 },
    { 21,  515000000 },
    { 22,  521000000 },
    { 23,  527000000 },
    { 24,  533000000 },
    { 25,  539000000 },
    { 26,  545000000 },
    { 27,  551000000 },
    { 28,  557000000 },
    { 29,  563000000 },
    { 30,  569000000 },
    { 31,  575000000 },
    { 32,  581000000 },
    { 33,  587000000 },
    { 34,  593000000 },
    { 35,  599000000 },
    { 36,  605000000 },
};

int atsc_freq_to_channel(uint32_t freq_hz)
{
    for (int i = 0; i < ATSC_FREQ_TABLE_COUNT; i++) {
        if (atsc_freq_table[i].frequency_hz == freq_hz) {
            return atsc_freq_table[i].channel;
        }
    }
    return 0;
}

uint32_t atsc_channel_to_freq(int channel)
{
    for (int i = 0; i < ATSC_FREQ_TABLE_COUNT; i++) {
        if (atsc_freq_table[i].channel == channel) {
            return atsc_freq_table[i].frequency_hz;
        }
    }
    return 0;
}
