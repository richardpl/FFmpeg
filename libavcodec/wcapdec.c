/*
 * WCAP video decoder
 *
 * Copyright (c) 2012 Paul B Mahol
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

#include <inttypes.h>

#include "libavutil/imgutils.h"
#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"

typedef struct WCAPContext {
    AVFrame *frame;
} WCAPContext;

static av_cold int wcap_decode_init(AVCodecContext *avctx)
{
    WCAPContext *s = avctx->priv_data;
    uint32_t format;

    if (avctx->extradata && avctx->extradata_size >= 4) {
        format = AV_RL32(avctx->extradata);

        switch (format) {
        case 0x34325852: avctx->pix_fmt = AV_PIX_FMT_RGB0; break;
        case 0x34325842: avctx->pix_fmt = AV_PIX_FMT_BGR0; break;
        case 0x34325258: avctx->pix_fmt = AV_PIX_FMT_0RGB; break;
        case 0x34324258: avctx->pix_fmt = AV_PIX_FMT_0BGR; break;
        }
    }

    s->frame = av_frame_alloc();
    if (!s->frame)
        return AVERROR(ENOMEM);

    return 0;
}

static void clear(AVCodecContext *avctx)
{
    WCAPContext *s = avctx->priv_data;
    int y;

    if (!s->frame->buf[0])
        return;

    for (y = 0; y < avctx->height; y++) {
        memset(s->frame->data[0] + y * s->frame->linesize[0], 0, avctx->width * 4);
    }
}

static int wcap_decode_frame(AVCodecContext *avctx, void *data,
                            int *got_frame, AVPacket *avpkt)
{
    WCAPContext *s = avctx->priv_data;
    AVFrame *frame = s->frame;
    uint32_t nrects, x1, y1, x2, y2;
    int ret, n, i, k, x;
    GetByteContext gb;
    uint8_t *dst;

    if ((ret = av_image_check_size(avctx->width, avctx->height, 0, NULL)) < 0)
        return ret;

    bytestream2_init(&gb, avpkt->data, avpkt->size);

    if ((ret = ff_reget_buffer(avctx, frame)) < 0)
        return ret;

    if (avpkt->flags & AV_PKT_FLAG_KEY) {
        clear(avctx);
    }

    bytestream2_skip(&gb, 4);
    nrects = bytestream2_get_le32(&gb);

    for (n = 0; n < nrects; n++) {
        x1 = bytestream2_get_le32(&gb);
        y1 = bytestream2_get_le32(&gb);
        x2 = bytestream2_get_le32(&gb);
        y2 = bytestream2_get_le32(&gb);

        if (x1 >= x2 || y1 >= y2 || x2 > avctx->width || y2 > avctx->height ||
            (x2 - x1) > avctx->width || (y2 - y1) > avctx->height)
            return AVERROR_INVALIDDATA;

        x = x1;
        dst = frame->data[0] + (avctx->height - y1 - 1) * frame->linesize[0];

        for (i = 0; i < (x2 - x1) * (y2 - y1);) {
            unsigned v = bytestream2_get_le32(&gb);
            int run_len = v >> 24;

            if (run_len < 0xE0)
                run_len++;
            else
                run_len = 1 << (run_len - 0xE0 + 7);

            i += run_len;
            for (k = 0; k < run_len; k++) {
                dst[x*4 + 1] += v & 0xFF;
                dst[x*4 + 2] += (v >> 8) & 0xFF;
                dst[x*4 + 3] += (v >> 16) & 0xFF;
                x++;
                if (x == x2) {
                    x = x1;
                    dst -= frame->linesize[0];
                }
            }
        }
    }

    frame->key_frame = (avpkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
    frame->pict_type = (avpkt->flags & AV_PKT_FLAG_KEY) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;

    if ((ret = av_frame_ref(data, s->frame)) < 0)
        return ret;

    *got_frame       = 1;

    return avpkt->size;
}

AVCodec ff_wcap_decoder = {
    .name           = "wcap",
    .long_name      = NULL_IF_CONFIG_SMALL("Weston capture"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_WCAP,
    .priv_data_size = sizeof(WCAPContext),
    .init           = wcap_decode_init,
    .decode         = wcap_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
};
