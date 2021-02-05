/*
 * PhotoCD parser
 * Copyright (c) 2021 Paul B Mahol
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

/**
 * @file
 * PhotoCD parser
 */

#include "libavutil/common.h"

#include "parser.h"

typedef struct PhotoCDParser {
    ParseContext pc;
    int count;
} PhotoCDParser;

#define KEY (((uint64_t)'P' << 56) | ((uint64_t)'C' << 48) | \
             ((uint64_t)'D' << 40) | ((uint64_t)'_' << 32) | \
             ((uint64_t)'I' << 24) | ((uint64_t)'P' << 16) | \
             ((uint64_t)'I' <<  8) | ((uint64_t)'\0'<<  0))

static int photocd_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    PhotoCDParser *bpc = s->priv_data;
    uint64_t state = bpc->pc.state64;
    int next = END_NOT_FOUND, i = 0;

    s->pict_type = AV_PICTURE_TYPE_I;
    s->key_frame = 1;
    s->duration  = 1;

    *poutbuf_size = 0;
    *poutbuf = NULL;

    for (; i < buf_size; i++) {
        state = (state << 8) | buf[i];

        if (state == KEY)
            bpc->count++;

        if (state == KEY && bpc->count >= 2) {
            next = i - (7+2048);
            bpc->count = 0;
            break;
        }
    }

    bpc->pc.state64 = state;
    if (ff_combine_frame(&bpc->pc, next, &buf, &buf_size) < 0) {
        *poutbuf = NULL;
        *poutbuf_size = 0;
        return buf_size;
    }

    *poutbuf      = buf;
    *poutbuf_size = buf_size;

    return next;
}

AVCodecParser ff_photocd_parser = {
    .codec_ids      = { AV_CODEC_ID_PHOTOCD },
    .priv_data_size = sizeof(PhotoCDParser),
    .parser_parse   = photocd_parse,
    .parser_close   = ff_parse_close,
};
