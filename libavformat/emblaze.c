/*
 * Emblaze demuxer
 *
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

#include "libavutil/intreadwrite.h"
#include "avio_internal.h"
#include "avformat.h"
#include "internal.h"

typedef struct EmblazeContext {
    int have_audio;
    int stream_index;
} EmblazeContext;

static int emblaze_probe(const AVProbeData *p)
{
    if (memcmp(p->buf, "GEO INTERACTIVE MEDIA GROUP\x1a\x02\00\x00\x00", 32))
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int emblaze_read_header(AVFormatContext *s)
{
    EmblazeContext *em = s->priv_data;
    int fps_num, fps_den;
    AVIOContext *pb = s->pb;
    AVStream *vst, *ast;

    avio_skip(pb, 32);
    em->have_audio = avio_r8(pb);
    fps_num = avio_r8(pb);
    fps_den = avio_r8(pb) + 100;
    avio_skip(pb, 9);

    vst = avformat_new_stream(s, NULL);
    if (!vst)
        return AVERROR(ENOMEM);

    vst->need_parsing = AVSTREAM_PARSE_FULL_RAW;
    vst->start_time = 0;
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_H263;
    avpriv_set_pts_info(vst, 64, fps_den, fps_num * 100);

    if (!em->have_audio)
        return 0;

    ast = avformat_new_stream(s, NULL);
    if (!ast)
        return AVERROR(ENOMEM);

    ast->start_time = 0;
    ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    ast->codecpar->codec_id   = AV_CODEC_ID_GSM;
    avpriv_set_pts_info(ast, 64, fps_den, fps_num * 100);

    return 0;
}

static int emblaze_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    EmblazeContext *em = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t size;
    int64_t pos;
    int ret;

    if (avio_feof(pb))
        return AVERROR_EOF;

    pos = avio_tell(pb);
    size = avio_rl32(pb);

    ret = av_get_packet(pb, pkt, size & 0x7fffffff);
    pkt->pos = pos;
    pkt->duration = 1;
    pkt->stream_index = em->stream_index++;
    if (!em->have_audio)
        em->stream_index = 0;
    else
        em->stream_index &= 1;

    if (size & 0x80000000)
        pkt->flags |= AV_PKT_FLAG_KEY;

    return ret;
}

AVInputFormat ff_emblaze_demuxer = {
    .name           = "emblaze",
    .long_name      = NULL_IF_CONFIG_SMALL("Emblaze"),
    .priv_data_size = sizeof(EmblazeContext),
    .read_probe     = emblaze_probe,
    .read_header    = emblaze_read_header,
    .read_packet    = emblaze_read_packet,
    .extensions     = "ev2",
    .flags          = AVFMT_GENERIC_INDEX,
};
