/*
 * DHAV demuxer
 *
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

#include "avformat.h"
#include "internal.h"

typedef struct DHAVContext {
    int64_t pts;
} DHAVContext;

static int dhav_probe(AVProbeData *p)
{
    if (memcmp(p->buf, "DHAV", 4))
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int dhav_read_header(AVFormatContext *s)
{
    AVStream *st, *ast;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type  = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id    = AV_CODEC_ID_H264;
    st->need_parsing          = AVSTREAM_PARSE_FULL_RAW;
    st->internal->avctx->framerate = (AVRational){25, 1};
    avpriv_set_pts_info(st, 64, 1, 1200000);

    ast = avformat_new_stream(s, NULL);
    if (!ast)
        return AVERROR(ENOMEM);

    ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    ast->codecpar->codec_id    = AV_CODEC_ID_PCM_ALAW;
    ast->codecpar->channels    = 1;
    ast->codecpar->sample_rate = 8000;
    avpriv_set_pts_info(ast, 64, 1, 8000);

    s->ctx_flags |= AVFMTCTX_NOHEADER;

    return 0;
}

static int dhav_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    unsigned packet, type, size, timestamp;
    int64_t pos;
    int ret;

    if (avio_feof(s->pb))
        return AVERROR_EOF;

    pos = avio_tell(s->pb);
    if (avio_rl32(s->pb) != MKTAG('D','H','A','V'))
        return AVERROR_INVALIDDATA;
    type = avio_rl32(s->pb);
    packet = avio_rl32(s->pb);
    size = avio_rl32(s->pb);
    timestamp = avio_rl32(s->pb);
    avio_skip(s->pb, 24);

    ret = av_get_packet(s->pb, pkt, size - 52);
    if (ret < 0)
        return ret;
    pkt->stream_index = type == 0xF0 ? 1 : 0;
    avio_skip(s->pb, 8);

    return ret;
}

AVInputFormat ff_dhav_demuxer = {
    .name           = "dhav",
    .long_name      = NULL_IF_CONFIG_SMALL("DHAV"),
    .priv_data_size = sizeof(DHAVContext),
    .read_probe     = dhav_probe,
    .read_header    = dhav_read_header,
    .read_packet    = dhav_read_packet,
    .extensions     = "dav",
    .flags          = AVFMT_GENERIC_INDEX,
};
