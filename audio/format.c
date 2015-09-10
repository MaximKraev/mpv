/*
 * Copyright (C) 2005 Alex Beregszaszi
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>

#include "common/common.h"
#include "audio/filter/af.h"

// number of bytes per sample, 0 if invalid/unknown
int af_fmt_to_bytes(int format)
{
    switch (af_fmt_from_planar(format)) {
    case AF_FORMAT_U8:      return 1;
    case AF_FORMAT_S16:     return 2;
    case AF_FORMAT_S24:     return 3;
    case AF_FORMAT_S32:     return 4;
    case AF_FORMAT_FLOAT:   return 4;
    case AF_FORMAT_DOUBLE:  return 8;
    }
    if (af_fmt_is_spdif(format))
        return 2;
    return 0;
}

int af_fmt_change_bytes(int format, int bytes)
{
    if (!af_fmt_is_valid(format) || !bytes)
        return 0;
    for (int fmt = 1; fmt < AF_FORMAT_COUNT; fmt++) {
        if (af_fmt_to_bytes(fmt) == bytes &&
            af_fmt_is_float(fmt) == af_fmt_is_float(format) &&
            af_fmt_is_planar(fmt) == af_fmt_is_planar(format) &&
            af_fmt_is_spdif(fmt) == af_fmt_is_spdif(format))
            return fmt;
    }
    return 0;
}

// All formats are considered signed, except explicitly unsigned int formats.
bool af_fmt_is_unsigned(int format)
{
    return format == AF_FORMAT_U8 || format == AF_FORMAT_U8P;
}

bool af_fmt_is_float(int format)
{
    format = af_fmt_from_planar(format);
    return format == AF_FORMAT_FLOAT || format == AF_FORMAT_DOUBLE;
}

// true for both unsigned and signed ints
bool af_fmt_is_int(int format)
{
    return format && !af_fmt_is_spdif(format) && !af_fmt_is_float(format);
}

// false for interleaved and AF_FORMAT_UNKNOWN
bool af_fmt_is_planar(int format)
{
    return format && af_fmt_to_planar(format) == format;
}

bool af_fmt_is_spdif(int format)
{
    return af_format_sample_alignment(format) > 1;
}

bool af_fmt_is_pcm(int format)
{
    return af_fmt_is_valid(format) && !af_fmt_is_spdif(format);
}

static const int planar_formats[][2] = {
    {AF_FORMAT_U8P,     AF_FORMAT_U8},
    {AF_FORMAT_S16P,    AF_FORMAT_S16},
    {AF_FORMAT_S32P,    AF_FORMAT_S32},
    {AF_FORMAT_FLOATP,  AF_FORMAT_FLOAT},
    {AF_FORMAT_DOUBLEP, AF_FORMAT_DOUBLE},
};

// Return the planar format corresponding to the given format.
// If the format is already planar, return it.
// Return 0 if there's no equivalent.
int af_fmt_to_planar(int format)
{
    for (int n = 0; n < MP_ARRAY_SIZE(planar_formats); n++) {
        if (planar_formats[n][1] == format)
            return planar_formats[n][0];
        if (planar_formats[n][0] == format)
            return format;
    }
    return 0;
}

// Return the interleaved format corresponding to the given format.
// If the format is already interleaved, return it.
// Always succeeds if format is actually planar; otherwise return 0.
int af_fmt_from_planar(int format)
{
    for (int n = 0; n < MP_ARRAY_SIZE(planar_formats); n++) {
        if (planar_formats[n][0] == format)
            return planar_formats[n][1];
    }
    return format;
}

bool af_fmt_is_valid(int format)
{
    return format > 0 && format < AF_FORMAT_COUNT;
}

const char *af_fmt_to_str(int format)
{
    switch (format) {
    case AF_FORMAT_U8:          return "u8";
    case AF_FORMAT_S16:         return "s16";
    case AF_FORMAT_S24:         return "s24";
    case AF_FORMAT_S32:         return "s32";
    case AF_FORMAT_FLOAT:       return "float";
    case AF_FORMAT_DOUBLE:      return "double";
    case AF_FORMAT_U8P:         return "u8p";
    case AF_FORMAT_S16P:        return "s16p";
    case AF_FORMAT_S32P:        return "s32p";
    case AF_FORMAT_FLOATP:      return "floatp";
    case AF_FORMAT_DOUBLEP:     return "doublep";
    case AF_FORMAT_S_AAC:       return "spdif-aac";
    case AF_FORMAT_S_AC3:       return "spdif-ac3";
    case AF_FORMAT_S_DTS:       return "spdif-dts";
    case AF_FORMAT_S_DTSHD:     return "spdif-dtshd";
    case AF_FORMAT_S_EAC3:      return "spdif-eac3";
    case AF_FORMAT_S_MP3:       return "spdif-mp3";
    case AF_FORMAT_S_TRUEHD:    return "spdif-truehd";
    }
    return "??";
}

int af_fmt_seconds_to_bytes(int format, float seconds, int channels, int samplerate)
{
    assert(!af_fmt_is_planar(format));
    int bps      = af_fmt_to_bytes(format);
    int framelen = channels * bps;
    int bytes    = seconds  * bps * samplerate;
    if (bytes % framelen)
        bytes += framelen - (bytes % framelen);
    return bytes;
}

void af_fill_silence(void *dst, size_t bytes, int format)
{
    memset(dst, af_fmt_is_unsigned(format) ? 0x80 : 0, bytes);
}

// Returns a "score" that serves as heuristic how lossy or hard a conversion is.
// If the formats are equal, 1024 is returned. If they are gravely incompatible
// (like s16<->ac3), INT_MIN is returned. If there is implied loss of precision
// (like s16->s8), a value <0 is returned.
static int af_format_conversion_score(int dst_format, int src_format)
{
    if (dst_format == AF_FORMAT_UNKNOWN || src_format == AF_FORMAT_UNKNOWN)
        return INT_MIN;
    if (dst_format == src_format)
        return 1024;
    // Can't be normally converted
    if (!af_fmt_is_pcm(dst_format) || !af_fmt_is_pcm(src_format))
        return INT_MIN;
    int score = 1024;
    if (af_fmt_is_planar(dst_format) != af_fmt_is_planar(src_format))
        score -= 1;     // has to (de-)planarize
    if (af_fmt_is_float(dst_format) != af_fmt_is_float(src_format)) {
        int dst_bytes = af_fmt_to_bytes(dst_format);
        if (af_fmt_is_float(dst_format)) {
            // For int->float, always prefer 32 bit float.
            score -= dst_bytes == 4 ? 0 : 1;
        } else {
            // For float->int, always prefer highest bit depth int
            score -= 8 - dst_bytes;
        }
        // Has to convert float<->int - Consider this the worst case.
        score -= 2048;
    } else {
        int bytes = af_fmt_to_bytes(dst_format) - af_fmt_to_bytes(src_format);
        if (bytes > 0) {
            score -= 1 + bytes;         // has to add padding
        } else if (bytes < 0) {
            score -= 1024 - bytes;      // has to reduce bit depth
        }
    }
    return score;
}

struct entry {
    int fmt;
    int score;
};

static int cmp_entry(const void *a, const void *b)
{
#define CMP_INT(a, b) (a > b ? 1 : (a < b ? -1 : 0))
    return -CMP_INT(((struct entry *)a)->score, ((struct entry *)b)->score);
}

// Return a list of sample format compatible to src_format, sorted by order
// of preference. out_formats[0] will be src_format (as long as it's valid),
// and the list is terminated with 0 (AF_FORMAT_UNKNOWN).
// Keep in mind that this also returns formats with flipped interleaving
// (e.g. for s16, it returns [s16, s16p, ...]).
void af_get_best_sample_formats(int src_format, int out_formats[AF_FORMAT_COUNT])
{
    int num = 0;
    struct entry e[AF_FORMAT_COUNT];
    for (int fmt = 1; fmt < AF_FORMAT_COUNT; fmt++) {
        int score = af_format_conversion_score(fmt, src_format);
        if (score > INT_MIN)
            e[num++] = (struct entry){fmt, score};
    }
    qsort(e, num, sizeof(e[0]), cmp_entry);
    for (int n = 0; n < num; n++)
        out_formats[n] = e[n].fmt;
    out_formats[num] = 0;
}

// Return the number of samples that make up one frame in this format.
// You get the byte size by multiplying them with sample size and channel count.
int af_format_sample_alignment(int format)
{
    switch (format) {
    case AF_FORMAT_S_AAC:       return 16384 / 4;
    case AF_FORMAT_S_AC3:       return 6144 / 4;
    case AF_FORMAT_S_DTSHD:     return 32768 / 16;
    case AF_FORMAT_S_DTS:       return 2048 / 4;
    case AF_FORMAT_S_EAC3:      return 24576 / 4;
    case AF_FORMAT_S_MP3:       return 4608 / 4;
    case AF_FORMAT_S_TRUEHD:    return 61440 / 16;
    default:                    return 1;
    }
}
