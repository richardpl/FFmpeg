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

/**
 * @file
 * conditional video filter
 */

#include "libavutil/attributes.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/eval.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"
#include "libavformat/avformat.h"
#include "audio.h"
#include "avfilter.h"
#include "buffersink.h"
#include "buffersrc.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

static const char *const var_names[] = {
    "FRAME_RATE",  ///< defined only for constant frame-rate video
    "INTERLACED",  ///< tell if the current frame is interlaced
    "N_IN",        ///< number of consumed frame, starting at zero
    "N_OUT",       ///< number of returned frame, starting at zero
    "POS",         ///< original position in the file of the frame
    "PTS",         ///< original pts in the file of the frame
    "STARTPTS",    ///< PTS at start of movie
    "STARTT",      ///< time at start of movie
    "T",           ///< original time in the file of the frame
    "TB",          ///< timebase
    "RTCTIME",
    "RTCSTART",
    "KEY",
    NULL
};

enum var_name {
    VAR_FRAME_RATE,
    VAR_INTERLACED,
    VAR_N_IN,
    VAR_N_OUT,
    VAR_POS,
    VAR_PTS,
    VAR_STARTPTS,
    VAR_STARTT,
    VAR_T,
    VAR_TB,
    VAR_RTCTIME,
    VAR_RTCSTART,
    VAR_KEY,
    VAR_VARS_NB
};

typedef struct ConditionalContext {
    const AVClass *class;

    char *expr;
    char *filter_graph_str[2];

    AVExpr *e;
    int cg;
    double var_values[VAR_VARS_NB];

    AVFilterContext *sink[2];
    AVFilterContext *src[2];
    AVFilterInOut   *inputs[2];
    AVFilterInOut   *outputs[2];
    AVFilterContext *format[2];
    AVFilterGraph   *graph[2];
} ConditionalContext;

#define OFFSET(x) offsetof(ConditionalContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption conditional_options[]= {
    { "expr",  "specify the expression",                             OFFSET(expr),                AV_OPT_TYPE_STRING, {.str = "1"    }, .flags = FLAGS },
    { "true",  "specify filtergraph to call if expression is true",  OFFSET(filter_graph_str[0]), AV_OPT_TYPE_STRING, {.str = "null" }, .flags = FLAGS },
    { "false", "specify filtergraph to call if expression is false", OFFSET(filter_graph_str[1]), AV_OPT_TYPE_STRING, {.str = "null" }, .flags = FLAGS },
    { NULL },
};

static av_cold int init(AVFilterContext *ctx)
{
    ConditionalContext *s = ctx->priv;
    int ret, i;

    if (!s->expr || !s->filter_graph_str[0] || !s->filter_graph_str[1])
        return AVERROR(EINVAL);

    ret = av_expr_parse(&s->e, s->expr, var_names,
                        NULL, NULL, NULL, NULL, 0, ctx);
    if (ret < 0)
        return ret;

    s->graph[0] = avfilter_graph_alloc();
    s->graph[1] = avfilter_graph_alloc();
    if (!s->graph[0] || !s->graph[1])
        return AVERROR(ENOMEM);

    for (i = 0; i < 2; i++) {
        ret = avfilter_graph_parse_ptr(s->graph[i], s->filter_graph_str[i],
                                       &s->inputs[i], &s->outputs[i], ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error parsing graph: %s\n", s->filter_graph_str[i]);
            return ret;
        }
    }

    s->var_values[VAR_STARTPTS] = NAN;
    s->var_values[VAR_STARTT]   = NAN;

    return 0;
}

#define TS2D(ts) ((ts) == AV_NOPTS_VALUE ? NAN : (double)(ts))
#define TS2T(ts, tb) ((ts) == AV_NOPTS_VALUE ? NAN : (double)(ts)*av_q2d(tb))

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    ConditionalContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    int ret = 0;

    if (isnan(s->var_values[VAR_STARTPTS])) {
        s->var_values[VAR_STARTPTS] = TS2D(frame->pts);
        s->var_values[VAR_STARTT] = TS2T(frame->pts, inlink->time_base);
    }
    s->var_values[VAR_PTS] = TS2D(frame->pts);
    s->var_values[VAR_T] = TS2T(frame->pts, inlink->time_base);
    s->var_values[VAR_POS] = av_frame_get_pkt_pos(frame) == -1 ? NAN : av_frame_get_pkt_pos(frame);
    s->var_values[VAR_INTERLACED] = frame->interlaced_frame;
    s->var_values[VAR_RTCTIME] = av_gettime();
    s->var_values[VAR_N_IN] = inlink->frame_count;
    s->var_values[VAR_N_OUT] = outlink->frame_count;
    s->var_values[VAR_KEY] = frame->key_frame;

    s->cg = !av_expr_eval(s->e, s->var_values, NULL);

    ret = av_buffersrc_add_frame_flags(s->src[s->cg], frame,
                                       AV_BUFFERSRC_FLAG_PUSH |
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&frame);

    while (ret >= 0) {
        out = av_frame_alloc();
        if (!out)
            return AVERROR(ENOMEM);

        ret = av_buffersink_get_frame_flags(s->sink[s->cg], out, 0);
        if (ret == AVERROR(EAGAIN)) {
            av_frame_free(&out);
            ret = 0;
            break;
        } else if (ret < 0) {
            av_frame_free(&out);
            return ret;
        }
        ret = ff_filter_frame(outlink, out);
    }

    return ret;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ConditionalContext *s = ctx->priv;
    int ret;

    ret = ff_request_frame(ctx->inputs[0]);
    if (ret == AVERROR_EOF) {
        AVFrame *out;

        ret = av_buffersrc_add_frame_flags(s->src[s->cg], NULL, 0);
        if (ret < 0)
            return ret;

        ret = 0;
        while (ret >= 0) {
            out = av_frame_alloc();
            if (!out)
                return AVERROR(ENOMEM);

            ret = av_buffersink_get_frame_flags(s->sink[s->cg], out, 0);
            if (ret == AVERROR(EAGAIN)) {
                av_frame_free(&out);
                ret = 0;
                break;
            } else if (ret < 0) {
                av_frame_free(&out);
                return ret;
            }
            ret = ff_filter_frame(ctx->outputs[0], out);
        }
    }

    return ret;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ConditionalContext *s = ctx->priv;

    s->var_values[VAR_TB] = av_q2d(inlink->time_base);
    s->var_values[VAR_RTCSTART] = av_gettime();
    s->var_values[VAR_FRAME_RATE] = inlink->frame_rate.num && inlink->frame_rate.den ?
                                        av_q2d(inlink->frame_rate) : NAN;

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(outlink->format);
    AVFilterContext *ctx = outlink->src;
    ConditionalContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *tsinklink, *fsinklink;
    AVFilter *src, *sink, *format;
    int i, ret;

    src = avfilter_get_by_name("buffer");
    if (!src) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't find src filter\n");
        return AVERROR(EINVAL);
    }

    sink = avfilter_get_by_name("buffersink");
    if (!sink) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't find sink filter\n");
        return AVERROR(EINVAL);
    }

    format = avfilter_get_by_name("format");
    if (!format) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't find format filter\n");
        return AVERROR(EINVAL);
    }

    for (i = 0; i < 2; i++) {
        const char *sink_name;
        const char *src_name;
        const char *format_name;

        src_name = av_asprintf("src%d", i);
        s->src[i] = avfilter_graph_alloc_filter(s->graph[i], src, src_name);
        av_freep(&src_name);
        if (!s->src[i]) {
            av_log(ctx, AV_LOG_ERROR, "Error allocating src%d filter\n", i);
            return AVERROR(ENOMEM);
        }

        av_opt_set_int(s->src[i], "width",     inlink->w,                   AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(s->src[i], "height",    inlink->h,                   AV_OPT_SEARCH_CHILDREN);
        av_opt_set_q  (s->src[i], "time_base", inlink->time_base,           AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(s->src[i], "pix_fmt",   inlink->format,              AV_OPT_SEARCH_CHILDREN);
        av_opt_set_q  (s->src[i], "sar",       inlink->sample_aspect_ratio, AV_OPT_SEARCH_CHILDREN);

        ret = avfilter_init_str(s->src[i], NULL);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error initializing src%d filter\n", i);
            return ret;
        }

        format_name = av_asprintf("format%d", i);
        s->format[i] = avfilter_graph_alloc_filter(s->graph[i], format, format_name);
        av_freep(&format_name);
        if (!s->format[i]) {
            av_log(ctx, AV_LOG_ERROR, "Error allocating format%d filter\n", i);
            return AVERROR(ENOMEM);
        }

        av_opt_set(s->format[i], "pix_fmts", desc->name, AV_OPT_SEARCH_CHILDREN);

        ret = avfilter_init_str(s->format[i], NULL);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error initializing format%d filter\n", i);
            return ret;
        }

        sink_name = av_asprintf("sink%d", i);
        s->sink[i] = avfilter_graph_alloc_filter(s->graph[i], sink, sink_name);
        av_freep(&sink_name);
        if (!s->sink[i]) {
            av_log(ctx, AV_LOG_ERROR, "Error allocating sink%d filter\n", i);
            return AVERROR(ENOMEM);
        }

        ret = avfilter_init_str(s->sink[i], NULL);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error initializing sink%d filter\n", i);
            return ret;
        }

        ret = avfilter_link(s->src[i], 0, s->inputs[i]->filter_ctx, 0);
        if (ret < 0)
            return ret;

        ret = avfilter_link(s->outputs[i]->filter_ctx, 0, s->format[i], 0);
        if (ret < 0)
            return ret;

        ret = avfilter_link(s->format[i], 0, s->sink[i], 0);
        if (ret < 0)
            return ret;

        ret = avfilter_graph_config(s->graph[i], ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error configuring the filter graph %d\n", i);
            return ret;
        }
    }

    tsinklink = s->sink[0]->inputs[0];
    fsinklink = s->sink[1]->inputs[0];
    if (tsinklink->w != fsinklink->w ||
        tsinklink->h != fsinklink->h) {
        av_log(ctx, AV_LOG_ERROR, "Video sizes of both filters are not same\n");
        return AVERROR(EINVAL);
    }

    outlink->w = tsinklink->w;
    outlink->h = tsinklink->h;
    outlink->time_base = tsinklink->time_base;
    outlink->frame_rate = tsinklink->frame_rate;
    outlink->sample_aspect_ratio = tsinklink->sample_aspect_ratio;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ConditionalContext *s = ctx->priv;

    av_expr_free(s->e);
    avfilter_graph_free(&s->graph[0]);
    avfilter_graph_free(&s->graph[1]);
    avfilter_inout_free(&s->inputs[0]);
    avfilter_inout_free(&s->inputs[1]);
    avfilter_inout_free(&s->outputs[0]);
    avfilter_inout_free(&s->outputs[1]);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame,
        .config_props  = config_output,
    },
    { NULL }
};

AVFILTER_DEFINE_CLASS(conditional);

AVFilter ff_vf_conditional = {
    .name        = "conditional",
    .description = NULL_IF_CONFIG_SMALL("Conditional video filtering."),
    .priv_size   = sizeof(ConditionalContext),
    .priv_class  = &conditional_class,
    .init        = init,
    .uninit      = uninit,
    .inputs      = inputs,
    .outputs     = outputs,
};
