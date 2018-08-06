/*
 * Infinity IMM4 decoder
 *
 * Copyright (c) 2018 Paul B Mahol
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "avcodec.h"
#include "bswapdsp.h"
#include "get_bits.h"
#include "internal.h"
#include "idctdsp.h"

typedef struct IMM4Context {
    BswapDSPContext bdsp;
    GetBitContext  gb;

    AVFrame *prev_frame;
    uint8_t *bitstream;
    int bitstream_size;

    unsigned factor;
    unsigned field_2B;
    unsigned field_30;
    unsigned field_28;

    DECLARE_ALIGNED(32, int16_t, block)[6][64];
    IDCTDSPContext idsp;
} IMM4Context;

static uint8_t table_0[] = {
    12, 9, 6,
};

static int16_t table_3[] = {
  -1, 0, 20, 6, 36, 6, 52, 6, 4, 4, 4, 4, 4,
  4, 4, 4, 19, 3, 19, 3, 19, 3, 19, 3, 19, 3, 19,
  3, 19, 3, 19, 3, 35, 3, 35, 3, 35, 3, 35, 3, 35,
  3, 35, 3, 35, 3, 35, 3, 51, 3, 51, 3, 51, 3, 51,
  3, 51, 3, 51, 3, 51, 3, 51, 3, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
};

static int16_t table_5[] = {
  -1, 0, -1, 0, 6, 6, 9, 6, 8, 5, 8, 5, 4, 5, 4, 5, 2, 5, 2,
  5, 1, 5, 1, 5, 0, 4, 0, 4, 0, 4, 0, 4, 12, 4, 12, 4, 12, 4, 12,
  4, 10, 4, 10, 4, 10, 4, 10, 4, 14, 4, 14, 4, 14, 4, 14, 4, 5, 4,
  5, 4, 5, 4, 5, 4, 13, 4, 13, 4, 13, 4, 13, 4, 3, 4, 3, 4, 3, 4,
  3, 4, 11, 4, 11, 4, 11, 4, 11, 4, 7, 4, 7, 4, 7, 4, 7, 4, 15, 2,
  15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15,
  2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2,
};

static uint16_t table_7[304] = {
  0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 16514u, 16514u, 16387u, 16387u,
  11u, 11u, 10u, 10u, 19969u, 19969u, 19969u, 19969u, 19841u,
  19841u, 19841u, 19841u, 19713u, 19713u, 19713u, 19713u, 19585u,
  19585u, 19585u, 19585u, 1154u, 1154u, 1154u, 1154u, 1026u, 1026u,
  1026u, 1026u, 898u, 898u, 898u, 898u, 770u, 770u, 770u, 770u,
  642u, 642u, 642u, 642u, 387u, 387u, 387u, 387u, 259u, 259u, 259u,
  259u, 132u, 132u, 132u, 132u, 12u, 12u, 133u, 133u, 2945u, 2945u,
  3073u, 3073u, 20097u, 20097u, 20225u, 20225u, 20353u, 20353u,
  20481u, 20481u, 134u, 260u, 515u, 643u, 771u, 1282u, 3201u, 3329u,
  20609u, 20737u, 20865u, 20993u, 21121u, 21249u, 21377u, 21505u, 9u,
  8u, 19457u, 19457u, 19329u, 19329u, 19201u, 19201u, 19073u, 19073u,
  18945u, 18945u, 18817u, 18817u, 18689u, 18689u, 18561u, 18561u,
  16386u, 16386u, 2817u, 2817u, 2689u, 2689u, 2561u, 2561u, 2433u,
  2433u, 2305u, 2305u, 2177u, 2177u, 2049u, 2049u, 1921u, 1921u, 514u,
  514u, 386u, 386u, 7u, 7u, 6u, 6u, 18433u, 18433u, 18433u, 18433u,
  18305u, 18305u, 18305u, 18305u, 18177u, 18177u, 18177u, 18177u,
  18049u, 18049u, 18049u, 18049u, 17921u, 17921u, 17921u, 17921u,
  17793u, 17793u, 17793u, 17793u, 17665u, 17665u, 17665u, 17665u,
  17537u, 17537u, 17537u, 17537u, 1793u, 1793u, 1793u, 1793u, 1665u,
  1665u, 1665u, 1665u, 258u, 258u, 258u, 258u, 131u, 131u, 131u, 131u,
  5u, 5u, 5u, 5u, 17409u, 17281u, 17153u, 17025u, 1537u, 1409u, 1281u,
  4u, 16897u, 16897u, 16769u, 16769u, 16641u, 16641u, 16513u, 16513u,
  1153u, 1153u, 1025u, 1025u, 897u, 897u, 769u, 769u, 130u, 130u, 3u,
  3u, 641u, 641u, 641u, 641u, 513u, 513u, 513u, 513u, 385u, 385u, 385u,
  385u, 16385u, 16385u, 16385u, 16385u, 16385u, 16385u, 16385u, 16385u,
  1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
  1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 129u,
  129u, 129u, 129u, 129u, 129u, 129u, 129u, 129u, 129u, 129u, 129u,
  129u, 129u, 129u, 129u, 257u, 257u, 257u, 257u, 257u, 257u, 257u,
  257u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u
};

static uint8_t table_8[] = {
    0,  12,  11,  11,  11,  11,  11,  11,  12,  12,
   13,  13,  22,  22,  22,  22,  11,  10,  10,  10,
   10,  10,  10,  10,  10,  10,  10,  10,  10,  10,
   10,  10,  10,  10,  10,  10,  10,  10,   9,   9,
    9,   9,   9,   9,   9,   9,   9,   9,   9,   9,
    9,   9,   9,   9,   9,   9,   9,   9,   9,   9,
    9,   9,   9,   9,   8,   8,   7,   7,   7,   7,
    7,   6,   6,   6,   5,   5,   3,   3,   3,   3,
    3,   3,   3,   3,   4,   4,   4,   4,   5,   5,
    5,   5,   0,   0,   0,   0
};

static int get_value2(GetBitContext *gb, int x)
{
    int value, skip;

    value = show_bits(gb, 6);
    skip = table_5[2 * value + 1];
    av_assert0(skip > 0);
    skip_bits(gb, skip);

    if (x)
        return table_5[2 * value];
    else
        return 15 - table_5[2 * value];
}

static int decode_block(AVCodecContext *avctx, GetBitContext *gb,
                        int block, unsigned factor, int flag)
{
    IMM4Context *s = avctx->priv_data;
    int i, f = 0, sign, c, d, is_end, len, factor2;

    for (i = 1; i < 64; i++) {
        unsigned bits;

        bits = show_bits_long(gb, 32);

        if (bits >> 27 >= 4) {
            av_assert0(60 + (bits >> 27) < FF_ARRAY_ELEMS(table_8));
            c = table_8[60 + (bits >> 27)];
        } else {
            av_assert0((bits >> 23) < FF_ARRAY_ELEMS(table_8));
            c = table_8[bits >> 23];
        }

        if (!c) {
            f |= 2;
            av_assert0(0);
        }

        d = bits >> 25;
        if (d == 3) {
            is_end = (bits >> 24) & 1;
            len = (bits >> 18) & 0x3F;
            factor2 = (int)(bits << 14) >> 24;
            sign = factor2 < 0;
            skip_bits(gb, c);
        } else {
            int b = table_7[d + 176];
            int e = bits >> 20;
            int v20;

            if (bits >> 27)
                e = (bits >> 22) + 64;
            if ((bits >> 27) < 4) {
                av_assert0(e < FF_ARRAY_ELEMS(table_7));
                b = table_7[e];
            }
            v20 = b & 0x7F;
            is_end = (b >> 14) & 0x3;
            len = (b >> 7) & 0x3F;
            av_assert0(c > 1);
            skip_bits(gb, c);
            sign = bits << (c - 1);
            factor2 = v20;
            if (sign < 0)
                factor2 = -factor2;
        }
        i += len;
        if (i >= 64)
            break;
        s->block[block][i] = factor * factor2;
        if (is_end || f)
            break;
    }

    return 0;
}

static int decode_blocks(AVCodecContext *avctx, GetBitContext *gb,
                         unsigned cbp, int flag)
{
    IMM4Context *s = avctx->priv_data;
    int ret, i;

    memset(s->block, 0, sizeof(s->block));

    for (i = 0; i < 6; i++) {
        int x = get_bits(gb, 8);

        if (x == 255)
            x = 128;
        x *= 8;

        s->block[i][0] = x;
        if (cbp & (1 << (5 - i))) {
            ret = decode_block(avctx, gb, i, s->factor, flag);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int decode_intra(AVCodecContext *avctx, GetBitContext *gb, AVFrame *frame)
{
    IMM4Context *s = avctx->priv_data;
    int ret, x, y;

    s->field_2B = table_0[s->factor];
    s->factor = s->field_2B * 2;
    s->field_30 = 0;

    for (y = 0; y < avctx->height; y += 16) {
        for (x = 0; x < avctx->width; x += 16) {
            unsigned value2, value, skip;
            int flag;

            value = show_bits(gb, 9);
            value >>= 3;
            skip = table_3[2 * value + 1];
            value = table_3[2 * value];
            av_assert0(skip > 0);
            skip_bits(gb, skip);

            s->field_28 = value & 0x07;
            value = value >> 4;
            flag = get_bits1(gb);

            value2 = get_value2(gb, 1);

            value = value | (value2 << 2);
            ret = decode_blocks(avctx, gb, value, flag);
            if (ret < 0)
                return ret;

            s->idsp.idct_put(frame->data[0] + y * frame->linesize[0] + x,
                             frame->linesize[0], s->block[0]);
            s->idsp.idct_put(frame->data[0] + y * frame->linesize[0] + x + 8,
                             frame->linesize[0], s->block[1]);
            s->idsp.idct_put(frame->data[0] + (y + 8) * frame->linesize[0] + x,
                             frame->linesize[0], s->block[2]);
            s->idsp.idct_put(frame->data[0] + (y + 8) * frame->linesize[0] + x + 8,
                             frame->linesize[0], s->block[3]);
            s->idsp.idct_put(frame->data[1] + (y >> 1) * frame->linesize[1] + (x >> 1),
                             frame->linesize[1], s->block[4]);
            s->idsp.idct_put(frame->data[2] + (y >> 1) * frame->linesize[2] + (x >> 1),
                             frame->linesize[2], s->block[5]);
        }
    }

    printf("gb: %d\n", get_bits_left(gb));

    return 0;
}

static int decode_inter(AVCodecContext *avctx, GetBitContext *gb, AVFrame *frame)
{
    IMM4Context *s = avctx->priv_data;
    int ret, x, y;

    s->field_2B = table_0[s->factor];
    s->factor = s->field_2B * 2;
    s->field_30 = 0;

    for (y = 0; y < avctx->height; y += 16) {
        for (x = 0; x < avctx->width; x += 16) {
            unsigned value2, value, skip;
            int flag;

            value = show_bits(gb, 9);
            value >>= 3;
            skip = table_3[2 * value + 1];
            value = table_3[2 * value];
            av_assert0(skip > 0);
            skip_bits(gb, skip);

            s->field_28 = value & 0x07;
            value = value >> 4;
            flag = get_bits1(gb);

            value2 = get_value2(gb, 1);

            value = value | (value2 << 2);
            ret = decode_blocks(avctx, gb, value, flag);
            if (ret < 0)
                return ret;

            s->idsp.idct_put(frame->data[0] + y * frame->linesize[0] + x,
                             frame->linesize[0], s->block[0]);
            s->idsp.idct_put(frame->data[0] + y * frame->linesize[0] + x + 8,
                             frame->linesize[0], s->block[1]);
            s->idsp.idct_put(frame->data[0] + (y + 8) * frame->linesize[0] + x,
                             frame->linesize[0], s->block[2]);
            s->idsp.idct_put(frame->data[0] + (y + 8) * frame->linesize[0] + x + 8,
                             frame->linesize[0], s->block[3]);
            s->idsp.idct_put(frame->data[1] + (y >> 1) * frame->linesize[1] + (x >> 1),
                             frame->linesize[1], s->block[4]);
            s->idsp.idct_put(frame->data[2] + (y >> 1) * frame->linesize[2] + (x >> 1),
                             frame->linesize[2], s->block[5]);
        }
    }

    printf("gb: %d\n", get_bits_left(gb));

    return 0;
}

static int decode_frame(AVCodecContext *avctx, void *data,
                        int *got_frame, AVPacket *avpkt)
{
    IMM4Context *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    AVFrame *frame = data;
    unsigned type;
    int ret, scaled;

    if (avpkt->size <= 32)
        return AVERROR_INVALIDDATA;

    av_fast_malloc(&s->bitstream, &s->bitstream_size,
                   avpkt->size + 3 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!s->bitstream)
        return AVERROR(ENOMEM);

    s->bdsp.bswap_buf((uint32_t *)s->bitstream,
                      (uint32_t *)avpkt->data,
                      (avpkt->size + 3) >> 2);

    if ((ret = init_get_bits8(gb, s->bitstream, avpkt->size + 3)) < 0)
        return ret;

    skip_bits_long(gb, 24 * 8);
    scaled = avpkt->data[8];
    if (scaled < 2) {
        int width, height;
        int mode = avpkt->data[10];

        switch (mode) {
        case 1:
            width = 352;
            height = 240;
            break;
        case 2:
            width = 704;
            height = 240;
            break;
        case 4:
            width = 480;
            height = 704;
            break;
        case 17:
            width = 352;
            height = 288;
            break;
        case 18:
            width = 704;
            height = 288;
            break;
        default:
            width = 704;
            height = 576;
            break;
        }

        avctx->width = width;
        avctx->height = height;
    }

    type = get_bits_long(gb, 32);
    s->factor = get_bits_long(gb, 32);
    if (s->factor > 2)
        return AVERROR_INVALIDDATA;

    if ((ret = ff_get_buffer(avctx, frame, AV_GET_BUFFER_FLAG_REF)) < 0)
        return ret;

    switch (type) {
    case 0x19781977:
        frame->key_frame = 1;
        frame->pict_type = AV_PICTURE_TYPE_I;
        ret = decode_intra(avctx, gb, frame);
        break;
    case 0x12250926:
        frame->key_frame = 0;
        frame->pict_type = AV_PICTURE_TYPE_P;
        ret = decode_inter(avctx, gb, frame);
        break;
    default:
        return AVERROR_INVALIDDATA;
    }

    if (ret < 0)
        return ret;

    av_frame_unref(s->prev_frame);
    if ((ret = av_frame_ref(s->prev_frame, frame)) < 0)
        return ret;

    *got_frame = 1;

    return avpkt->size;
}

static av_cold int decode_init(AVCodecContext *avctx)
{
    IMM4Context *s = avctx->priv_data;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    avctx->idct_algo = FF_IDCT_XVID;
    ff_bswapdsp_init(&s->bdsp);
    ff_idctdsp_init(&s->idsp, avctx);

    s->prev_frame = av_frame_alloc();
    if (!s->prev_frame)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold int decode_close(AVCodecContext *avctx)
{
    IMM4Context *s = avctx->priv_data;

    av_frame_free(&s->prev_frame);
    av_freep(&s->bitstream);
    s->bitstream_size = 0;

    return 0;
}

AVCodec ff_imm4_decoder = {
    .name             = "imm4",
    .long_name        = NULL_IF_CONFIG_SMALL("Infinity IMM4"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_IMM4,
    .priv_data_size   = sizeof(IMM4Context),
    .init             = decode_init,
    .close            = decode_close,
    .decode           = decode_frame,
    .capabilities     = AV_CODEC_CAP_DR1,
    .caps_internal    = FF_CODEC_CAP_INIT_THREADSAFE |
                        FF_CODEC_CAP_INIT_CLEANUP,
};
