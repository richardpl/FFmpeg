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

#include "avcodec.h"
#include "bswapdsp.h"
#include "copy_block.h"
#include "get_bits.h"
#include "idctdsp.h"
#include "internal.h"

typedef struct IMM4Context {
    BswapDSPContext bdsp;
    GetBitContext  gb;

    AVFrame *prev_frame;
    uint8_t *bitstream;
    int bitstream_size;

    int factor;
    unsigned sindex;

    DECLARE_ALIGNED(32, int16_t, block)[6][64];
    IDCTDSPContext idsp;
} IMM4Context;

static const uint8_t table_0[] = {
    12, 9, 6,
};

static const uint8_t table_10[] = {
    30, 20, 15,
};

static const int16_t table_3[] = {
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

static const int16_t table_5[] = {
  -1, 0, -1, 0, 6, 6, 9, 6, 8, 5, 8, 5, 4, 5, 4, 5, 2, 5, 2,
  5, 1, 5, 1, 5, 0, 4, 0, 4, 0, 4, 0, 4, 12, 4, 12, 4, 12, 4, 12,
  4, 10, 4, 10, 4, 10, 4, 10, 4, 14, 4, 14, 4, 14, 4, 14, 4, 5, 4,
  5, 4, 5, 4, 5, 4, 13, 4, 13, 4, 13, 4, 13, 4, 3, 4, 3, 4, 3, 4,
  3, 4, 11, 4, 11, 4, 11, 4, 11, 4, 7, 4, 7, 4, 7, 4, 7, 4, 15, 2,
  15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2, 15,
  2, 15, 2, 15, 2, 15, 2, 15, 2, 15, 2,
};

static const uint16_t table_7[304] = {
    0, 0, 0, 0, 0, 0, 0, 0, 16514, 16514, 16387, 16387,
    11, 11, 10, 10, 19969, 19969, 19969, 19969, 19841, 19841,
    19841, 19841, 19713, 19713, 19713, 19713, 19585, 19585,
    19585, 19585, 1154, 1154, 1154, 1154, 1026, 1026, 1026,
    1026, 898, 898, 898, 898, 770, 770, 770, 770, 642, 642,
    642, 642, 387, 387, 387, 387, 259, 259, 259, 259, 132,
    132, 132, 132, 12, 12, 133, 133, 2945, 2945, 3073, 3073,
    20097, 20097, 20225, 20225, 20353, 20353, 20481, 20481,
    134, 260, 515, 643, 771, 1282, 3201, 3329, 20609, 20737,
    20865, 20993, 21121, 21249, 21377, 21505, 9, 8, 19457,
    19457, 19329, 19329, 19201, 19201, 19073, 19073, 18945,
    18945, 18817, 18817, 18689, 18689, 18561, 18561, 16386,
    16386, 2817, 2817, 2689, 2689, 2561, 2561, 2433, 2433,
    2305, 2305, 2177, 2177, 2049, 2049, 1921, 1921, 514,
    514, 386, 386, 7, 7, 6, 6, 18433, 18433, 18433, 18433,
    18305, 18305, 18305, 18305, 18177, 18177, 18177, 18177,
    18049, 18049, 18049, 18049, 17921, 17921, 17921, 17921,
    17793, 17793, 17793, 17793, 17665, 17665, 17665, 17665,
    17537, 17537, 17537, 17537, 1793, 1793, 1793, 1793,
    1665, 1665, 1665, 1665, 258, 258, 258, 258, 131, 131,
    131, 131, 5, 5, 5, 5, 17409, 17281, 17153, 17025, 1537,
    1409, 1281, 4, 16897, 16897, 16769, 16769, 16641, 16641,
    16513, 16513, 1153, 1153, 1025, 1025, 897, 897, 769,
    769, 130, 130, 3, 3, 641, 641, 641, 641, 513, 513, 513,
    513, 385, 385, 385, 385, 16385, 16385, 16385, 16385,
    16385, 16385, 16385, 16385, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 129, 129, 129, 129, 129, 129, 129,
    129, 129, 129, 129, 129, 129, 129, 129, 129, 257, 257,
    257, 257, 257, 257, 257, 257, 2, 2, 2, 2, 2, 2, 2, 2,
};

static const uint8_t table_8[] = {
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
    if (skip <= 0)
        return AVERROR_INVALIDDATA;
    skip_bits(gb, skip);

    if (x)
        return table_5[2 * value];
    else
        return 15 - table_5[2 * value];
}

static int decode_block(AVCodecContext *avctx, GetBitContext *gb,
                        int block, int factor, int flag)
{
    IMM4Context *s = avctx->priv_data;
    int i, sign, c, d, is_end, len, factor2;

    for (i = !flag; i < 64; i++) {
        unsigned bits;

        bits = show_bits_long(gb, 32);

        if (bits >> 27 >= 4) {
            if (60 + (bits >> 27) >= FF_ARRAY_ELEMS(table_8))
                return AVERROR_INVALIDDATA;
            c = table_8[60 + (bits >> 27)];
        } else {
            if ((bits >> 23) >= FF_ARRAY_ELEMS(table_8))
                return AVERROR_INVALIDDATA;
            c = table_8[bits >> 23];
        }

        if (!c)
            return AVERROR_INVALIDDATA;

        d = show_bits(gb, 7);
        if (d == 3) {
            skip_bits(gb, 7);
            is_end = get_bits1(gb);
            len = get_bits(gb, 6);
            factor2 = get_sbits(gb, 8);
        } else {
            unsigned b = table_7[d + 176];
            unsigned e = bits >> 20;

            if (show_bits(gb, 5))
                e = (bits >> 22) + 64;
            if (show_bits(gb, 5) < 4) {
                if (e >= FF_ARRAY_ELEMS(table_7))
                    return AVERROR_INVALIDDATA;
                b = table_7[e];
            }
            factor2 = b & 0x7F;
            is_end = (b >> 14) & 1;
            len = (b >> 7) & 0x3F;
            if (c <= 1)
                return AVERROR_INVALIDDATA;
            skip_bits(gb, c - 1);
            sign = get_bits1(gb);
            if (sign)
                factor2 = -factor2;
        }
        i += len;
        if (i >= 64)
            break;
        s->block[block][i] = factor * factor2;
        if (is_end)
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
        if (!flag) {
            int x = get_bits(gb, 8);

            if (x == 255)
                x = 128;
            x *= 8;

            s->block[i][0] = x;
        }

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

    s->factor = table_0[s->sindex] * 2;

    for (y = 0; y < avctx->height; y += 16) {
        for (x = 0; x < avctx->width; x += 16) {
            unsigned value2, value, skip;

            value = show_bits(gb, 9);
            value >>= 3;
            skip = table_3[2 * value + 1];
            value = table_3[2 * value];
            if (skip <= 0)
                return AVERROR_INVALIDDATA;
            skip_bits(gb, skip);

            value = value >> 4;
            skip_bits1(gb);

            value2 = get_value2(gb, 1);

            value = value | (value2 << 2);
            ret = decode_blocks(avctx, gb, value, 0);
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

    return 0;
}

static const uint16_t table_9[] = {
  65535, 0, 255, 9, 52, 9, 36, 9, 20, 9, 49, 9, 35, 8, 35, 8, 19,
  8, 19, 8, 50, 8, 50, 8, 51, 7, 51, 7, 51, 7, 51, 7, 34, 7, 34,
  7, 34, 7, 34, 7, 18, 7, 18, 7, 18, 7, 18, 7, 33, 7, 33, 7, 33,
  7, 33, 7, 17, 7, 17, 7, 17, 7, 17, 7, 4, 6, 4, 6, 4, 6, 4, 6,
  4, 6, 4, 6, 4, 6, 4, 6, 48, 6, 48, 6, 48, 6, 48, 6, 48, 6, 48,
  6, 48, 6, 48, 6, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3,
  5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 3, 5, 32, 4, 32,
  4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32,
  4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32,
  4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32, 4, 32,
  4, 32, 4, 32, 4, 32, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16,
  4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16,
  4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16,
  4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4, 2,
  3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
  3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
  3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
  3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
  3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
  3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2,
  3, 2, 3, 2, 3, 2, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1,
  3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 0, 1, 0, 0
};

static int decode_inter(AVCodecContext *avctx, GetBitContext *gb,
                        AVFrame *frame, AVFrame *prev)
{
    IMM4Context *s = avctx->priv_data;
    int ret, x, y;

    s->factor = table_10[s->sindex];

    for (y = 0; y < avctx->height; y += 16) {
        for (x = 0; x < avctx->width; x += 16) {
            unsigned value2, value, skip;
            int reverse, intra_block;

            if (get_bits1(gb)) {
                copy_block16(frame->data[0] + y * frame->linesize[0] + x,
                             prev->data[0] + y * prev->linesize[0] + x,
                             frame->linesize[0], prev->linesize[0], 16);
                copy_block8(frame->data[1] + (y >> 1) * frame->linesize[1] + (x >> 1),
                            prev->data[1] + (y >> 1) * prev->linesize[1] + (x >> 1),
                            frame->linesize[1], prev->linesize[1], 8);
                copy_block8(frame->data[2] + (y >> 1) * frame->linesize[2] + (x >> 1),
                            prev->data[2] + (y >> 1) * prev->linesize[2] + (x >> 1),
                            frame->linesize[2], prev->linesize[2], 8);
                continue;
            }

            value = show_bits(gb, 9);
            if (value > 256)
                value = 256;
            skip = table_9[2 * value + 1];
            value = table_9[2 * value];
            if (skip <= 0)
                return AVERROR_INVALIDDATA;
            skip_bits(gb, skip);

            intra_block = value & 0x07;
            reverse = intra_block == 3;
            if (reverse)
                skip_bits1(gb);

            value = value >> 4;
            value2 = get_value2(gb, reverse);
            value = value | (value2 << 2);
            if (intra_block) {
                ret = decode_blocks(avctx, gb, value, 0);
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
            } else {
                skip_bits(gb, 2);
                ret = decode_blocks(avctx, gb, value, 1);
                if (ret < 0)
                    return ret;

                copy_block16(frame->data[0] + y * frame->linesize[0] + x,
                             prev->data[0] + y * prev->linesize[0] + x,
                             frame->linesize[0], prev->linesize[0], 16);
                copy_block8(frame->data[1] + (y >> 1) * frame->linesize[1] + (x >> 1),
                            prev->data[1] + (y >> 1) * prev->linesize[1] + (x >> 1),
                            frame->linesize[1], prev->linesize[1], 8);
                copy_block8(frame->data[2] + (y >> 1) * frame->linesize[2] + (x >> 1),
                            prev->data[2] + (y >> 1) * prev->linesize[2] + (x >> 1),
                            frame->linesize[2], prev->linesize[2], 8);

                s->idsp.idct_add(frame->data[0] + y * frame->linesize[0] + x,
                                 frame->linesize[0], s->block[0]);
                s->idsp.idct_add(frame->data[0] + y * frame->linesize[0] + x + 8,
                                 frame->linesize[0], s->block[1]);
                s->idsp.idct_add(frame->data[0] + (y + 8) * frame->linesize[0] + x,
                                 frame->linesize[0], s->block[2]);
                s->idsp.idct_add(frame->data[0] + (y + 8) * frame->linesize[0] + x + 8,
                                 frame->linesize[0], s->block[3]);
                s->idsp.idct_add(frame->data[1] + (y >> 1) * frame->linesize[1] + (x >> 1),
                                 frame->linesize[1], s->block[4]);
                s->idsp.idct_add(frame->data[2] + (y >> 1) * frame->linesize[2] + (x >> 1),
                                 frame->linesize[2], s->block[5]);
            }
        }
    }

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

    av_fast_padded_malloc(&s->bitstream, &s->bitstream_size,
                          avpkt->size + 3);
    if (!s->bitstream)
        return AVERROR(ENOMEM);

    s->bdsp.bswap_buf((uint32_t *)s->bitstream,
                      (uint32_t *)avpkt->data,
                      (avpkt->size + 3) >> 2);

    if ((ret = init_get_bits8(gb, s->bitstream, avpkt->size + 3)) < 0)
        return ret;

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

    skip_bits_long(gb, 24 * 8);
    type = get_bits_long(gb, 32);
    s->sindex = get_bits_long(gb, 32);
    if (s->sindex > 2)
        return AVERROR_INVALIDDATA;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    if ((ret = ff_reget_buffer(avctx, s->prev_frame)) < 0)
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
        ret = decode_inter(avctx, gb, frame, s->prev_frame);
        break;
    default:
        return AVERROR_INVALIDDATA;
    }

    if (ret < 0)
        return ret;

    if (frame->key_frame) {
        if ((ret = av_frame_copy(s->prev_frame, frame)) < 0)
            return ret;
    }

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
