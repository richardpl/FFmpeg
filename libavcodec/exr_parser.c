/*
 * EXR parser
 * Copyright (c) 2020 Paul B Mahol
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
 * EXR parser
 */

#include "libavutil/bswap.h"
#include "libavutil/intreadwrite.h"
#include "parser.h"

typedef struct EXRParseContext {
    ParseContext pc;

    int flags;
    uint32_t w, h;
    uint32_t tile_w, tile_h;
    int64_t skip_bytes;
    uint64_t bytes_read;
    int nb_offsets;
    unsigned curr_offset;
    unsigned offset_index;
    uint64_t max_offset;
    int compression;

    int exr_state;
    uint8_t key[256];
    unsigned key_index;
    uint8_t type[256];
    unsigned type_index;
    uint32_t size;
    unsigned size_index;
    uint8_t value[256];
    uint32_t value_index;
} EXRParseContext;

static int exr_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    EXRParseContext *exr = s->priv_data;
    uint64_t state = exr->pc.state64;
    int next = END_NOT_FOUND, i = 0;

    s->pict_type = AV_PICTURE_TYPE_I;
    s->key_frame = 1;

    *poutbuf_size = 0;
    *poutbuf = NULL;

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        for (; i < buf_size; i++) {
            exr->bytes_read++;
            state = (state << 8) | buf[i];

            if (exr->skip_bytes > 0 && exr->exr_state == 0) {
                exr->skip_bytes--;
                if (exr->skip_bytes == 0) {
                    next = i + 1;
                    exr->bytes_read = 0;
                    break;
                }
            } else if (exr->exr_state == 1) {
                exr->key[exr->key_index++] = buf[i];
                if (exr->key_index >= 255) {
                    exr->exr_state = 0;
                    exr->key_index = 0;
                    memset(exr->key, 0, sizeof(exr->key));
                    goto new_state;
                }

                if (!buf[i] && exr->key_index == 1) {
                    exr->exr_state = 5;
                    if (!(exr->flags & 0x020000)) {
                        switch (exr->compression) {
                        case 5:
                        case 3:
                            exr->nb_offsets = (exr->nb_offsets + 15) >> 4;
                            break;
                        case 4:
                        case 6:
                        case 7:
                            exr->nb_offsets = (exr->nb_offsets + 31) >> 5;
                            break;
                        default:
                            break;
                        }
                    } else if (exr->tile_w && exr->tile_h) {
                        exr->nb_offsets = ((exr->w + exr->tile_w - 1) / exr->tile_w) *
                            ((exr->h + exr->tile_h - 1) / exr->tile_h);
                    }
                    exr->curr_offset = 0;
                    exr->offset_index = 0;
                    exr->max_offset = 0;
                } else if (!buf[i]) {
                    exr->exr_state = 2;
                }
            } else if (exr->exr_state == 2) {
                exr->type[exr->type_index++] = buf[i];
                if (exr->type_index >= 255) {
                    exr->exr_state = 0;
                    exr->type_index = 0;
                    memset(exr->type, 0, sizeof(exr->type));
                    exr->key_index = 0;
                    memset(exr->key, 0, sizeof(exr->key));
                    goto new_state;
                }

                if (!buf[i]) {
                    exr->exr_state = 3;
                    exr->size_index = 0;
                    exr->size = 0;
                }
            } else if (exr->exr_state == 3) {
                exr->size = (exr->size << 8) | buf[i];
                exr->size_index++;
                if (exr->size_index == 4) {
                    exr->exr_state = 4;
                }
            } else if (exr->exr_state == 4) {
                if (exr->value_index < 255)
                    exr->value[exr->value_index] = buf[i];
                exr->value_index++;
                if (exr->value_index == av_bswap32(exr->size)) {
                    if (!strcmp(exr->key, "dataWindow") &&
                        !strcmp(exr->type, "box2i")) {
                        exr->w = AV_RL32(exr->value +  8) + 1 - AV_RL32(exr->value + 0);
                        exr->h = AV_RL32(exr->value + 12) + 1 - AV_RL32(exr->value + 4);
                        exr->nb_offsets = exr->h;
                    } else if (!strcmp(exr->key, "compression") &&
                               !strcmp(exr->type, "compression")) {
                        exr->compression = exr->value[0];
                    } else if (!strcmp(exr->key, "tiles") &&
                               !strcmp(exr->type, "tiledesc")) {
                        exr->tile_w = AV_RL32(exr->value + 0);
                        exr->tile_h = AV_RL32(exr->value + 4);
                    }
                    exr->exr_state = 1;
                    exr->size = 0;
                    exr->key_index = 0;
                    exr->type_index = 0;
                    exr->value_index = 0;
                    memset(exr->key, 0, sizeof(exr->key));
                    memset(exr->type, 0, sizeof(exr->type));
                }
            } else if (exr->exr_state == 5) {
                exr->offset_index++;
                if (exr->offset_index == 8) {
                    uint64_t new_offset = av_bswap64(state);

                    exr->curr_offset++;
                    exr->offset_index = 0;
                    if (exr->max_offset < new_offset)
                        exr->max_offset = new_offset;
                    if (exr->curr_offset >= exr->nb_offsets) {
                        exr->curr_offset = 0;
                        exr->exr_state = 6;
                    }
                }
            } else if (exr->exr_state == 6) {
                if (exr->bytes_read == exr->max_offset + 8 + (!!(exr->flags & 0x020000)) * 12) {
                    exr->skip_bytes = av_bswap32(state & 0xFFFFFFFF);
                    exr->bytes_read = 0;
                    exr->exr_state = 0;
                    exr->max_offset = 0;
                    exr->nb_offsets = 0;
                }
            }

new_state:
            if (exr->exr_state == 0 && ((state & 0xFFFFFFFFFF000000LL) == 0x762F310102000000LL)) {
                exr->flags = state & 0xFFFFFF;
                exr->exr_state = 1;
            }
        }

        exr->pc.state64 = state;
        if (ff_combine_frame(&exr->pc, next, &buf, &buf_size) < 0) {
            *poutbuf = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }

    *poutbuf      = buf;
    *poutbuf_size = buf_size;

    return next;
}

AVCodecParser ff_exr_parser = {
    .codec_ids      = { AV_CODEC_ID_EXR },
    .priv_data_size = sizeof(EXRParseContext),
    .parser_parse   = exr_parse,
    .parser_close   = ff_parse_close,
};
