/*
 * Copyright (c) 2016 Paul B Mahol
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

#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"
#include "avfilter.h"
#include "audio.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "framesync.h"

typedef struct CopyPropsContext {
    const AVClass *class;

    unsigned flags;

    FFFrameSync fs;
} CopyPropsContext;

#define COPY_METADATA          1
#define COPY_TOP_FIELD         2
#define COPY_INTERLACED        4
#define COPY_SAR               8
#define COPY_PRIMARIES        16
#define COPY_TRC              32
#define COPY_COLORSPACE       64
#define COPY_COLOR_RANGE     128
#define COPY_CHROMA_LOCATION 256

#define OFFSET(x) offsetof(CopyPropsContext, x)
#define V AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static int process_frame(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    CopyPropsContext *s = fs->opaque;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out, *first, *second;
    int ret;

    if ((ret = ff_framesync_get_frame(&s->fs, 0, &first,  0)) < 0 ||
        (ret = ff_framesync_get_frame(&s->fs, 1, &second, 0)) < 0)
        return ret;

    out = av_frame_clone(first);
    if (!out)
        return AVERROR(ENOMEM);
    if (!ctx->is_disabled) {
        if (s->flags & COPY_METADATA) {
            av_dict_copy(&out->metadata, second->metadata, 0);
        }
        if (s->flags & COPY_TOP_FIELD)
            out->top_field_first = second->top_field_first;
        if (s->flags & COPY_INTERLACED)
            out->interlaced_frame = second->interlaced_frame;
        if (s->flags & COPY_SAR)
            out->sample_aspect_ratio = second->sample_aspect_ratio;
        if (s->flags & COPY_PRIMARIES)
            out->color_primaries = second->color_primaries;
        if (s->flags & COPY_TRC)
            out->color_trc = second->color_trc;
        if (s->flags & COPY_COLORSPACE)
            out->colorspace = second->colorspace;
        if (s->flags & COPY_COLOR_RANGE)
            out->color_range = second->color_range;
        if (s->flags & COPY_CHROMA_LOCATION)
            out->chroma_location = second->chroma_location;
    }
    out->pts = av_rescale_q(s->fs.pts, s->fs.time_base, outlink->time_base);

    return ff_filter_frame(outlink, out);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *buf)
{
    CopyPropsContext *s = inlink->dst->priv;
    return ff_framesync_filter_frame(&s->fs, inlink, buf);
}

static int request_frame(AVFilterLink *outlink)
{
    CopyPropsContext *s = outlink->src->priv;
    return ff_framesync_request_frame(&s->fs, outlink);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    CopyPropsContext *s = ctx->priv;
    AVFilterLink *first = ctx->inputs[0];
    AVFilterLink *second = ctx->inputs[1];
    FFFrameSyncIn *in;
    int ret;

    outlink->w = first->w;
    outlink->h = first->h;
    outlink->time_base = first->time_base;
    outlink->sample_aspect_ratio = first->sample_aspect_ratio;
    outlink->frame_rate = first->frame_rate;

    if ((ret = ff_framesync_init(&s->fs, ctx, 2)) < 0)
        return ret;

    in = s->fs.in;
    in[0].time_base = first->time_base;
    in[1].time_base = second->time_base;
    in[0].sync   = 1;
    in[0].before = EXT_STOP;
    in[0].after  = EXT_INFINITY;
    in[1].sync   = 1;
    in[1].before = EXT_STOP;
    in[1].after  = EXT_INFINITY;
    s->fs.opaque   = s;
    s->fs.on_event = process_frame;

    return ff_framesync_configure(&s->fs);
}

#if CONFIG_ACOPYPROPS_FILTER

static const AVOption acopyprops_options[] = {
    { "flags", "set a flags of operation", OFFSET(flags), AV_OPT_TYPE_FLAGS, {.i64=0},             0, 0xFFFFFFFF, A, "flags" },
    { "m",     "copy metadata",            0,             AV_OPT_TYPE_CONST, {.i64=COPY_METADATA}, 0, 0,          A, "flags" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(acopyprops);

static const AVFilterPad acopyprops_inputs[] = {
    {
        .name         = "first",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
    {
        .name         = "second",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad acopyprops_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_af_acopyprops = {
    .name          = "acopyprops",
    .description   = NULL_IF_CONFIG_SMALL("Copy audio frames properties from second input to first input."),
    .priv_size     = sizeof(CopyPropsContext),
    .priv_class    = &acopyprops_class,
    .query_formats = ff_query_formats_all,
    .inputs        = acopyprops_inputs,
    .outputs       = acopyprops_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
};
#endif /* CONFIG_ACOPYPROPS_FILTER */

#if CONFIG_COPYPROPS_FILTER

static const AVOption copyprops_options[] = {
    { "flags", "set a flags of operation", OFFSET(flags), AV_OPT_TYPE_FLAGS, {.i64=0}, 0, 0xFFFFFFFF, V, "flags" },
    { "m", "copy metadata", 0, AV_OPT_TYPE_CONST, {.i64=COPY_METADATA}, 0, 0, V, "flags" },
    { "t", "copy top field first flag", 0, AV_OPT_TYPE_CONST, {.i64=COPY_TOP_FIELD}, 0, 0, V, "flags" },
    { "i", "copy interlaced flag", 0, AV_OPT_TYPE_CONST, {.i64=COPY_INTERLACED}, 0, 0, V, "flags" },
    { "sar", "copy sample aspect ratio", 0, AV_OPT_TYPE_CONST, {.i64=COPY_SAR}, 0, 0, V, "flags" },
    { "pri", "copy color primaries", 0, AV_OPT_TYPE_CONST, {.i64=COPY_PRIMARIES}, 0, 0, V, "flags" },
    { "trc", "copy color transfer characteristic", 0, AV_OPT_TYPE_CONST, {.i64=COPY_TRC}, 0, 0, V, "flags" },
    { "cs", "copy colorspace", 0, AV_OPT_TYPE_CONST, {.i64=COPY_COLORSPACE}, 0, 0, V, "flags" },
    { "cr", "copy color range", 0, AV_OPT_TYPE_CONST, {.i64=COPY_COLOR_RANGE}, 0, 0, V, "flags" },
    { "cl", "copy chroma location", 0, AV_OPT_TYPE_CONST, {.i64=COPY_CHROMA_LOCATION}, 0, 0, V, "flags" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(copyprops);

static const AVFilterPad copyprops_inputs[] = {
    {
        .name         = "first",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    {
        .name         = "second",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad copyprops_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_copyprops = {
    .name          = "copyprops",
    .description   = NULL_IF_CONFIG_SMALL("Copy video frames properties from second input to first input."),
    .priv_size     = sizeof(CopyPropsContext),
    .priv_class    = &copyprops_class,
    .query_formats = ff_query_formats_all,
    .inputs        = copyprops_inputs,
    .outputs       = copyprops_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
};
#endif /* CONFIG_COPYPROPS_FILTER */
