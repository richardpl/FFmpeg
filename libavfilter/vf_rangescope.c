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

#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/xga_font_data.h"
#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct Range {
    int min;
    int max;
    int imin;
    int imax;
} Range;

typedef struct RangescopeContext {
    const AVClass *class;
    int ow, oh;

    int planewidth[4];
    int planeheight[4];
    int nb_planes;
    int depth;
    FFDrawContext draw;
    FFDrawColor black;
    uint8_t map[4];

    Range range[4];
    float *history[4];
    int max;
    void (*get_ranges)(AVFilterContext *ctx, AVFrame *in);
} RangescopeContext;

#define OFFSET(x) offsetof(RangescopeContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption rangescope_options[] = {
    { "width", "set output width",    OFFSET(ow),        AV_OPT_TYPE_INT,   {.i64=640}, 9, 4000, FLAGS },
    { "w",     "set output width",    OFFSET(ow),        AV_OPT_TYPE_INT,   {.i64=640}, 9, 4000, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(rangescope);

static const enum AVPixelFormat in_pix_fmts[] = {
    AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUV440P,
    AV_PIX_FMT_YUV411P,  AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_YUV444P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV420P9,
    AV_PIX_FMT_YUVA444P9, AV_PIX_FMT_YUVA422P9, AV_PIX_FMT_YUVA420P9,
    AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV420P10,
    AV_PIX_FMT_YUVA444P10, AV_PIX_FMT_YUVA422P10, AV_PIX_FMT_YUVA420P10,
    AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV440P12,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat out_pix_fmts[] = {
    AV_PIX_FMT_GBRP,
    AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    int ret;

    if ((ret = ff_formats_ref(ff_make_format_list(in_pix_fmts), &ctx->inputs[0]->out_formats)) < 0)
        return ret;
    return ff_formats_ref(ff_make_format_list(out_pix_fmts), &ctx->outputs[0]->in_formats);
}

static void get_ranges8(AVFilterContext *ctx, AVFrame *in)
{
    RangescopeContext *s = ctx->priv;
    int p, x, y;

    for (p = 0; p < s->nb_planes; p++) {
        uint8_t *value = in->data[p];
        Range *range = &s->range[p];

        memset(s->history[p], 0, s->max * sizeof(*s->history[p]));
        range->max = range->min = value[0];
        for (y = 0; y < s->planeheight[p]; y++) {
            value = in->data[p] + y * in->linesize[p];
            for (x = 0; x < s->planewidth[p]; x++) {
                range->max = FFMAX(value[x], range->max);
                range->min = FFMIN(value[x], range->min);
                s->history[p][value[x]]++;
            }
        }
    }
}

static void get_ranges16(AVFilterContext *ctx, AVFrame *in)
{
    RangescopeContext *s = ctx->priv;
    int p, x, y;

    for (p = 0; p < s->nb_planes; p++) {
        uint16_t *value = (uint16_t *)in->data[p];
        Range *range = &s->range[p];

        memset(s->history[p], 0, s->max * sizeof(*s->history[p]));
        range->max = range->min = value[0];
        for (y = 0; y < s->planeheight[p]; y++) {
            value = (uint16_t *)(in->data[p] + y * in->linesize[p]);
            for (x = 0; x < s->planewidth[p]; x++) {
                range->max = FFMAX(value[x], range->max);
                range->min = FFMIN(value[x], range->min);
                s->history[p][value[x]]++;
            }
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx  = inlink->dst;
    RangescopeContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int width = outlink->w / (s->nb_planes * 2);
    AVFrame *out;
    int c;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    out->pts = in->pts;

    ff_fill_rectangle(&s->draw, &s->black, out->data, out->linesize,
                      0, 0, outlink->w, outlink->h);

    if (av_frame_get_color_range(in) == AVCOL_RANGE_MPEG) {
        s->range[0].imin = 16  * (1 << (s->depth - 8));
        s->range[0].imax = 235 * (1 << (s->depth - 8));
        s->range[1].imin = 16  * (1 << (s->depth - 8));
        s->range[1].imax = 240 * (1 << (s->depth - 8));
        s->range[2].imin = 16  * (1 << (s->depth - 8));
        s->range[2].imax = 240 * (1 << (s->depth - 8));
        s->range[3].imin = 0;
        s->range[3].imax = 255 * (1 << (s->depth - 8));
    } else {
        for (c = 0; c < 4; c++) {
            s->range[c].imin = 0;
            s->range[c].imax = 255 * (1 << (s->depth - 8));
        }
    }

    s->get_ranges(ctx, in);
    av_frame_free(&in);

    for (c = 0; c < s->nb_planes; c++) {
        int y, x, i;
        float max = 1;

        for (i = 0; i < s->max; i++) {
            max = FFMAX(s->history[c][i], max);
        }

        for (y = 0; y < outlink->h; y++) {
            for (x = width / 2 + c * outlink->w / s->nb_planes; x < width / 2 + c * outlink->w / s->nb_planes + width; x++) {
                out->data[c][y * out->linesize[c] + x] = s->history[c][y] / max * s->max;
            }
        }
    }

    return ff_filter_frame(outlink, out);
}

static int config_input(AVFilterLink *inlink)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    RangescopeContext *s = inlink->dst->priv;
    int i;

    s->planeheight[1] = s->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;
    s->planewidth[1]  = s->planewidth[2]  = AV_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->planewidth[0]  = s->planewidth[3]  = inlink->w;

    s->nb_planes = av_pix_fmt_count_planes(inlink->format);
    ff_draw_init(&s->draw, inlink->format, 0);
    if (desc->flags & AV_PIX_FMT_FLAG_RGB) {
        ff_fill_rgba_map(s->map, inlink->format);
    } else {
        s->map[1] = 1; s->map[2] = 2; s->map[3] = 3;
    }

    s->depth = desc->comp[0].depth;
    s->max = (1 << s->depth) - 1;
    if (s->depth == 8)
        s->get_ranges = get_ranges8;
    else
        s->get_ranges = get_ranges16;

    for (i = 0; i < s->nb_planes; i++) {
        s->history[i] = av_malloc_array(s->max, sizeof(*s->history[i]));
        if (!s->history[i])
            return AVERROR(ENOMEM);
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    RangescopeContext *s = ctx->priv;

    outlink->h = s->max;
    outlink->w = s->ow;
    outlink->sample_aspect_ratio = (AVRational){1,1};

    ff_draw_init(&s->draw, outlink->format, 0);
    ff_draw_color(&s->draw, &s->black, (uint8_t[]){   0,   0,   0,   0} );

    return 0;
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
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

static av_cold void uninit(AVFilterContext *ctx)
{
    RangescopeContext *s = ctx->priv;
    int i;

    for (i = 0; i < s->nb_planes; i++) {
        av_freep(&s->history[i]);
    }
}

AVFilter ff_vf_rangescope = {
    .name          = "rangescope",
    .description   = NULL_IF_CONFIG_SMALL("Video pixel component range analysis."),
    .priv_size     = sizeof(RangescopeContext),
    .priv_class    = &rangescope_class,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = inputs,
    .outputs       = outputs,
};
