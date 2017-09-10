/*
 * WCAP demuxer
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

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "internal.h"
#include "rawdec.h"

static int wcap_probe(AVProbeData *pd)
{
    if (AV_RB32(pd->buf) == MKTAG('W','C','A','P'))
        return AVPROBE_SCORE_MAX;
    else
        return 0;
}

static int wcap_read_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    AVStream *st;

    avio_skip(pb, 4);

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    if (ff_get_extradata(s, st->codecpar, pb, 4) < 0)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_WCAP;
    st->codecpar->width      = avio_rl32(pb);
    st->codecpar->height     = avio_rl32(pb);
    st->need_parsing         = AVSTREAM_PARSE_FULL_RAW;
    avpriv_set_pts_info(st, 64, 1, 1000);

    st->start_time = avio_rl32(pb);
    avio_seek(pb, -4, SEEK_CUR);

    return 0;
}

AVInputFormat ff_wcap_demuxer = {
    .name           = "wcap",
    .long_name      = NULL_IF_CONFIG_SMALL("Weston capture"),
    .read_probe     = wcap_probe,
    .read_header    = wcap_read_header,
    .read_packet    = ff_raw_read_partial_packet,
    .raw_codec_id   = AV_CODEC_ID_WCAP,
    .extensions     = "wcap",
    .flags          = AVFMT_GENERIC_INDEX,
};
