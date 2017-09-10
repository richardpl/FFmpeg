/*
 * WCAP parser
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

#include "libavutil/bswap.h"
#include "parser.h"

typedef struct WCAPParseContext {
    ParseContext pc;
    uint32_t pos;
    uint32_t nrects;
    int got_msec;
    int got_nrects;
    int got_rectangle;
    int got_frame;
    uint32_t rect_counter;
    uint32_t current_rect;
    uint32_t x1, y1, x2, y2;
    int first;
    int64_t pts;
} WCAPParseContext;

static int wcap_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    WCAPParseContext *ppc = s->priv_data;
    uint32_t state = ppc->pc.state;
    int i, next = END_NOT_FOUND;

    *poutbuf_size = 0;

    for (i = 0; i < buf_size; i++) {
        state = (state << 8) | buf[i];

        if (((ppc->pc.index + i) % 4) != 3)
            continue;

        if (!ppc->got_msec) {
            ppc->got_msec = 1;
            ppc->pts = av_bswap32(state);
            ppc->current_rect = 0;
        } else if (ppc->got_msec && !ppc->got_nrects) {
            ppc->got_nrects = 1;
            ppc->nrects = av_bswap32(state);
        } else if (!ppc->got_rectangle && ppc->got_msec && ppc->got_nrects) {
            ppc->rect_counter++;
            if (ppc->rect_counter == 4) {
                ppc->got_rectangle = 1;
                ppc->y2 = av_bswap32(state);
            } else if (ppc->rect_counter == 3) {
                ppc->x2 = av_bswap32(state);
            } else if (ppc->rect_counter == 2) {
                ppc->y1 = av_bswap32(state);
            } else if (ppc->rect_counter == 1) {
                ppc->x1 = av_bswap32(state);
            }
        } else if (ppc->got_rectangle && ppc->got_nrects && ppc->got_msec) {
            if (!ppc->got_frame) {
                int l = state & 0xFF;

                if (l < 0xE0)
                    l = l + 1;
                else
                    l = 1 << (l - 0xE0 + 7);
                ppc->pos += l;
                if (ppc->pos >= (ppc->x2 - ppc->x1) * (ppc->y2 - ppc->y1)) {
                    ppc->got_frame = 1;
                }
            }
            if (ppc->got_frame) {
                ppc->current_rect++;
                ppc->got_rectangle = 0;
                ppc->rect_counter = 0;
                ppc->got_frame = 0;
                ppc->pos = 0;
                if (ppc->current_rect == ppc->nrects) {
                    ppc->current_rect = 0;
                    ppc->nrects = 0;
                    ppc->got_msec = 0;
                    ppc->got_nrects = 0;
                    next = i + 1;
                    s->key_frame = !ppc->first;
                    s->pict_type = s->key_frame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
                    s->pts = ppc->pts;
                    ppc->first++;
                    break;
                }
            }
        }
    }
    ppc->pc.state = state;

    if (ff_combine_frame(&ppc->pc, next, &buf, &buf_size) < 0)
        return buf_size;

    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return next;
}

AVCodecParser ff_wcap_parser = {
    .codec_ids      = { AV_CODEC_ID_WCAP },
    .priv_data_size = sizeof(WCAPParseContext),
    .parser_parse   = wcap_parse,
    .parser_close   = ff_parse_close,
};
