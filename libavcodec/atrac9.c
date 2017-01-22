/*
 * ATRAC9 compatible decoder
 * Copyright (c) 2017 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "libavutil/attributes.h"
#include "libavutil/float_dsp.h"
#include "libavutil/libm.h"
#include "avcodec.h"
#include "bytestream.h"
#include "fft.h"
#include "get_bits.h"
#include "internal.h"

typedef struct ATRAC9Context {
    GetBitContext gb;
} ATRAC9Context;

static int atrac9_decode_frame(AVCodecContext *avctx, void *data,
                               int *got_frame_ptr, AVPacket *avpkt)
{
    ATRAC9Context *s = avctx->priv_data;
    AVFrame *frame = data;
    int ret;

    if (avpkt->size < avctx->block_align) {
        av_log(avctx, AV_LOG_ERROR,
               "Frame too small (%d bytes). Truncated file?\n", avpkt->size);
        return AVERROR_INVALIDDATA;
    }

    frame->nb_samples = 1024;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    *got_frame_ptr = 1;

    return avctx->block_align;
}

static av_cold int atrac9_decode_init(AVCodecContext *avctx)
{
    ATRAC9Context *s = avctx->priv_data;

    if (avctx->channels <= 0 || avctx->channels > 2) {
        av_log(avctx, AV_LOG_ERROR, "Channel configuration error!\n");
        return AVERROR(EINVAL);
    }
    avctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

    return 0;
}

AVCodec ff_atrac9_decoder = {
    .name             = "atrac9",
    .long_name        = NULL_IF_CONFIG_SMALL("ATRAC9 (Adaptive TRansform Acoustic Coding 9)"),
    .type             = AVMEDIA_TYPE_AUDIO,
    .id               = AV_CODEC_ID_ATRAC9,
    .priv_data_size   = sizeof(ATRAC9Context),
    .init             = atrac9_decode_init,
    .decode           = atrac9_decode_frame,
    .capabilities     = AV_CODEC_CAP_SUBFRAMES | AV_CODEC_CAP_DR1,
    .sample_fmts      = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_FLTP,
                                                        AV_SAMPLE_FMT_NONE },
};
