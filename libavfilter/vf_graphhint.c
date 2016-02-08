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
 * graphhint video filter
 */

#include "libavutil/attributes.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavformat/avformat.h"
#include "avfilter.h"
#include "buffersink.h"
#include "buffersrc.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define MAX_LINE_SIZE 8192

typedef struct GraphHintContext {
    const AVClass *class;

    char *file;
    char *filter_graph_str;
    char *current_graph_str;
    char *null_graph_str;

    FILE *f;
    int start, end;

    AVFilterContext *sink;
    AVFilterContext *src;
    AVFilterInOut   *inputs;
    AVFilterInOut   *outputs;
    AVFilterContext *format;
    AVFilterGraph   *graph;
} GraphHintContext;

#define OFFSET(x) offsetof(GraphHintContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption graphhint_options[]= {
    { "file",  "specify the file from which to read graphs", OFFSET(file), AV_OPT_TYPE_STRING, {.str = NULL }, .flags = FLAGS },
    { NULL },
};

static void read_line(AVFilterContext *ctx)
{
    GraphHintContext *s = ctx->priv;
    char line[MAX_LINE_SIZE + 512];

    memcpy(s->filter_graph_str, s->null_graph_str, strlen(s->null_graph_str) + 1);
    while (fgets(line, sizeof(line), s->f)) {
        if (line[0] == '#')
            continue;
        memset(s->filter_graph_str, 0, MAX_LINE_SIZE);
        if (sscanf(line, "%d,%d %8191c", &s->start, &s->end, s->filter_graph_str) != 3) {
            av_log(ctx, AV_LOG_WARNING, "hint syntax error\n");
            continue;
        }
        break;
    }
}

static av_cold int init(AVFilterContext *ctx)
{
    GraphHintContext *s = ctx->priv;
    int ret;

    if (!s->file)
        return AVERROR(EINVAL);

    s->f = fopen(s->file, "r");
    if (!s->f) {
        ret = AVERROR(errno);
        av_log(ctx, AV_LOG_ERROR, "%s: %s\n", s->file, av_err2str(ret));
        return ret;
    }

    s->filter_graph_str = av_mallocz(MAX_LINE_SIZE);
    if (!s->filter_graph_str)
        return AVERROR(ENOMEM);
    s->null_graph_str = av_strdup("null");
    s->current_graph_str = av_strdup("NAN");
    if (!s->null_graph_str || !s->current_graph_str)
        return AVERROR(ENOMEM);
    read_line(ctx);

    return 0;
}

static int recreate_graph(AVFilterContext *ctx, const char *filter_graph_str)
{
    AVFilterLink *outlink = ctx->outputs[0];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(outlink->format);
    GraphHintContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilter *src, *sink, *format;
    int ret;

    avfilter_graph_free(&s->graph);
    avfilter_inout_free(&s->inputs);
    avfilter_inout_free(&s->outputs);
    s->graph = avfilter_graph_alloc();
    if (!s->graph)
        return AVERROR(ENOMEM);

    ret = avfilter_graph_parse_ptr(s->graph, filter_graph_str,
                                   &s->inputs, &s->outputs, ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error parsing graph: %s\n", filter_graph_str);
        return ret;
    }

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

    s->src = avfilter_graph_alloc_filter(s->graph, src, "src");
    if (!s->src) {
        av_log(ctx, AV_LOG_ERROR, "Error allocating src filter\n");
        return AVERROR(ENOMEM);
    }

    av_opt_set_int(s->src, "width",     inlink->w,                   AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(s->src, "height",    inlink->h,                   AV_OPT_SEARCH_CHILDREN);
    av_opt_set_q  (s->src, "time_base", inlink->time_base,           AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(s->src, "pix_fmt",   inlink->format,              AV_OPT_SEARCH_CHILDREN);
    av_opt_set_q  (s->src, "sar",       inlink->sample_aspect_ratio, AV_OPT_SEARCH_CHILDREN);

    ret = avfilter_init_str(s->src, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error initializing src filter\n");
        return ret;
    }

    s->format = avfilter_graph_alloc_filter(s->graph, format, "format");
    if (!s->format) {
        av_log(ctx, AV_LOG_ERROR, "Error allocating format filter\n");
        return AVERROR(ENOMEM);
    }

    av_opt_set(s->format, "pix_fmts", desc->name, AV_OPT_SEARCH_CHILDREN);

    ret = avfilter_init_str(s->format, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error initializing format filter\n");
        return ret;
    }

    s->sink = avfilter_graph_alloc_filter(s->graph, sink, "sink");
    if (!s->sink) {
        av_log(ctx, AV_LOG_ERROR, "Error allocating sink filter\n");
        return AVERROR(ENOMEM);
    }

    ret = avfilter_init_str(s->sink, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error initializing sink filter\n");
        return ret;
    }

    ret = avfilter_link(s->src, 0, s->inputs->filter_ctx, 0);
    if (ret < 0)
        return ret;

    ret = avfilter_link(s->outputs->filter_ctx, 0, s->format, 0);
    if (ret < 0)
        return ret;

    ret = avfilter_link(s->format, 0, s->sink, 0);
    if (ret < 0)
        return ret;

    ret = avfilter_graph_config(s->graph, ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error configuring the filter graph\n");
        return ret;
    }

    av_log(ctx, AV_LOG_DEBUG, "Created graph: %s\n", filter_graph_str);
    av_freep(&s->current_graph_str);
    s->current_graph_str = av_strdup(filter_graph_str);
    if (!s->current_graph_str)
        return AVERROR(ENOMEM);

    return 0;
}

static int try_recreate_graph(AVFilterContext *ctx, int number)
{
    GraphHintContext *s = ctx->priv;

    if (number > s->end)
        read_line(ctx);

    if (number >= s->start && number <= s->end) {
        if (strcmp(s->current_graph_str, s->filter_graph_str))
            return recreate_graph(ctx, s->filter_graph_str);
    } else {
        if (strcmp(s->current_graph_str, "null"))
            return recreate_graph(ctx, "null");
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    GraphHintContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    int ret = 0;

    ret = av_buffersrc_add_frame_flags(s->src, frame,
                                       AV_BUFFERSRC_FLAG_PUSH |
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&frame);

    while (ret >= 0) {
        out = av_frame_alloc();
        if (!out)
            return AVERROR(ENOMEM);

        ret = av_buffersink_get_frame_flags(s->sink, out, 0);
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

    ret = try_recreate_graph(ctx, inlink->frame_count + 1);
    if (!ret)
        return ret;

    return ret;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    GraphHintContext *s = ctx->priv;
    int ret;

    ret = ff_request_frame(ctx->inputs[0]);
    if (ret == AVERROR_EOF) {
        AVFrame *out;

        ret = av_buffersrc_add_frame_flags(s->src, NULL, 0);
        if (ret < 0)
            return ret;

        ret = 0;
        while (ret >= 0) {
            out = av_frame_alloc();
            if (!out)
                return AVERROR(ENOMEM);

            ret = av_buffersink_get_frame_flags(s->sink, out, 0);
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

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    GraphHintContext *s = ctx->priv;
    int ret;

    ret = try_recreate_graph(ctx, 0);
    if (ret < 0)
        return ret;

    outlink->w = s->sink->inputs[0]->w;
    outlink->h = s->sink->inputs[0]->h;
    outlink->time_base = s->sink->inputs[0]->time_base;
    outlink->frame_rate = s->sink->inputs[0]->frame_rate;
    outlink->sample_aspect_ratio = s->sink->inputs[0]->sample_aspect_ratio;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    GraphHintContext *s = ctx->priv;

    avfilter_graph_free(&s->graph);
    avfilter_inout_free(&s->inputs);
    avfilter_inout_free(&s->outputs);
    av_freep(&s->filter_graph_str);
    av_freep(&s->null_graph_str);
    av_freep(&s->current_graph_str);
    fclose(s->f);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
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

AVFILTER_DEFINE_CLASS(graphhint);

AVFilter ff_vf_graphhint = {
    .name        = "graphhint",
    .description = NULL_IF_CONFIG_SMALL("Read custom graphs from file."),
    .priv_size   = sizeof(GraphHintContext),
    .priv_class  = &graphhint_class,
    .init        = init,
    .uninit      = uninit,
    .inputs      = inputs,
    .outputs     = outputs,
};
