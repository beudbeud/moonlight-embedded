/*
 * This file is part of Moonlight Embedded.
 *
 * Based on Moonlight Pc implementation
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <libavcodec/avcodec.h>

#define SLICE_THREADING    0x4
#define VDPAU_ACCELERATION 0x40
#define VAAPI_ACCELERATION 0x80

enum decoders { SOFTWARE, VDPAU, VAAPI };
extern enum decoders ffmpeg_decoder;

/*
 * FIX C — Hook de format optionnel.
 * Permet à v4l2drm de demander AV_PIX_FMT_DRM_PRIME avant ffmpeg_init()
 * pour obtenir des frames exportables en DMA-BUF (zero-copy DRM_PRIME).
 * Laisser NULL pour le comportement par défaut.
 */
extern enum AVPixelFormat (*ffmpeg_get_format_cb)(AVCodecContext*,
                                                  const enum AVPixelFormat*);


int      ffmpeg_init(int videoFormat, int width, int height,
                     int perf_lvl, int buffer_count, int thread_count);
void     ffmpeg_destroy(void);
AVFrame* ffmpeg_get_frame(bool native_frame);
int      ffmpeg_decode(unsigned char* indata, int inlen);
