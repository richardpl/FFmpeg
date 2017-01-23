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

    unsigned p1, p2, p3, p4, p5;
    unsigned samples;

    int sample_rate;
    unsigned bitrate;

    int some_param;
    int need_this;

    double table0[256];
    double table1[256];
} ATRAC9Context;

static const uint8_t atrac9_params[8] = { 1, 2, 1, };

static const int atrac9_samplerates[] = {
    11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
    44100, 48000, 64000, 88200, 96000, 128000, 176000, 192000,
};

static int atrac9_decode_frame(AVCodecContext *avctx, void *data,
                               int *got_frame_ptr, AVPacket *avpkt)
{
    ATRAC9Context *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    AVFrame *frame = data;
    int v6 = 0;
    int ret;

    if (avpkt->size < avctx->block_align) {
        av_log(avctx, AV_LOG_ERROR,
               "Frame too small (%d bytes). Truncated file?\n", avpkt->size);
        return AVERROR_INVALIDDATA;
    }

    frame->nb_samples = s->samples;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    if ((ret = init_get_bits8(gb, avpkt->data, avctx->block_align)) < 0)
        return ret;

    if (atrac9_params[s->p2] > 0) {
        int v = get_bits1(gb);

        if (v6) {
            if (s->some_param != v)
                return AVERROR_INVALIDDATA;
        } else {
            s->some_param = v;
        }

        if (!s->need_this) {
            if (v)
                return AVERROR_INVALIDDATA;
        }
    } else {
        return AVERROR_INVALIDDATA;
    }

    *got_frame_ptr = 1;

    return avctx->block_align;
}

static void gen_table0(double *a1, int a2)
{
    signed int v2; // esi@1
    int v3; // ebx@1
    int v4; // edx@1
    int v5; // edi@3
    double v6; // st7@4
    int v7; // ecx@6
    double v8; // st7@7
    int v9; // [sp+20h] [bp+8h]@1
    signed int index; // [sp+20h] [bp+8h]@4

    v2 = 1 << a2;
    v3 = (1 << a2) / 2 + 2 * (1 << a2) / 4;
    v4 = ((1 << a2) - 2 * (1 << a2) / 2) / 2;
    v9 = 2 * (1 << a2) / 2;
    if (v4 > 0)
        memset(a1, 0, 8 * v4);
    v5 = v4;
    if ( v4 < v2 ) {
      v6 = (double)v9;
      index = 0;
      do
          a1[v5++] = (sin(((index++ + 0.5) / v6 - 0.5) * M_PI) + 1.0) * 0.5;
      while ( v5 < v2 );
    }
    v7 = 0;
    if ( v2 > 0 ) {
      v8 = 1.0;
      do {
        if ( v7 >= v3 && v8 > a1[v7] ) {
          a1[v7] = v8;
          v8 = 1.0;
        }
        ++v7;
      } while (v7 < v2);
    }
}

static void gen_table1(double *a1, int size)
{
    signed int v2; // esi@1
    int counter; // edx@1
    double *v4; // eax@2
    double *v5; // ecx@2
    double v6; // st7@3
    int v7; // edi@3
    double v8; // st6@3
    int v9; // esi@5
    double *v10; // eax@5
    double *v11; // ecx@5
    double v12; // st7@6
    double v13; // st6@6
    double v14; // [sp+0h] [bp-818h]@3
    double v15[1]; // [sp+8h] [bp-810h]@2
    int v16[1]; // [sp+10h] [bp-808h]@5
    int v17; // [sp+14h] [bp-804h]@2
    double a1a; // [sp+18h] [bp-800h]@1
    double v19[255]; // [sp+20h] [bp-7F8h]@3

    v2 = 1 << size;
    gen_table0(&a1a, size);
    counter = 0;
    if ( 1 << size >= 4 )
    {
      v17 = (char *)&a1a - (char *)a1;
      v4 = a1 + 2;
      v5 = &v15[v2];
      do {
        v6 = v5[1];
        v7 = v17;
        v8 = *(&a1a + counter);
        counter += 4;
        v5 -= 4;
        v4 += 4;
        *(v4 - 6) = v8 / (v6 * v6 + v8 * v8);
        *(v4 - 5) = *(&v14 + counter) / (v5[4] * v5[4] + *(&v14 + counter) * *(&v14 + counter));
        *(v4 - 4) = *(double *)((char *)v4 + v7 - 32)
                  / (v5[3] * v5[3] + *(double *)((char *)v4 + v7 - 32) * *(double *)((char *)v4 + v7 - 32));
        *(v4 - 3) = *(double *)(v19 + (v4 - a1 - 4))
                  / (v5[2] * v5[2]
                   + *(double *)(v19 + (v4 - a1 - 4))
                   * *(double *)(v19 + (v4 - a1 - 4)));
      } while ( counter < v2 - 3 );
    }
    if ( counter < v2 ) {
      v9 = v2 - counter;
      v10 = &a1[counter];
      v11 = (double *)&v16[8 * v9];
      do
      {
        v12 = *v11;
        --v11;
        v13 = *(double *)(&a1a + (v10 - a1));
        ++v10;
        --v9;
        *(v10 - 1) = v13 / (v12 * v12 + v13 * v13);
      } while ( v9 );
    }
}

static av_cold int atrac9_decode_init(AVCodecContext *avctx)
{
    ATRAC9Context *s = avctx->priv_data;
    GetBitContext gb;
    int ret;

    if (avctx->channels <= 0 || avctx->channels > 2) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported number of channels!\n");
        return AVERROR(EINVAL);
    }

    if (avctx->extradata_size < 12) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported extradata size!\n");
        return AVERROR(EINVAL);
    }

    if ((ret = init_get_bits8(&gb, avctx->extradata, avctx->extradata_size)) < 0)
        return ret;
    skip_bits_long(&gb, 32);

    if (get_bits(&gb, 8) != 0xFE)
        return AVERROR_INVALIDDATA;

    s->p1 = get_bits(&gb, 4);
    s->p2 = get_bits(&gb, 3);
    s->p4 = get_bits(&gb, 1);
    s->p3 = get_bits(&gb, 11) + 1;
    s->p5 = 1 << get_bits(&gb, 2);
    s->samples = s->p5 * 256;

    if (s->p4)
        return AVERROR_INVALIDDATA;

    avctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    s->sample_rate = atrac9_samplerates[s->p1];
    s->bitrate = 384000 * s->p3 / 256 / 1000;

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
