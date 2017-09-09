/*
 * Copyright (c) 2015 Paul B Mahol
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

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define MAX_FRAMES 257

typedef struct TDisplaceContext {
    const AVClass *class;

    int width[4], height[4];
    int nb_planes;

    AVFrame *frames[MAX_FRAMES];
    int nb_frames;
} TDisplaceContext;

#define OFFSET(x) offsetof(TDisplaceContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption tdisplace_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(tdisplace);

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV440P,
        AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P,
        AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRAP,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE
    };

    return ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    TDisplaceContext *s = ctx->priv;
    AVFilterLink *srclink = ctx->inputs[0];
    AVFilterLink *tlink = ctx->inputs[1];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(outlink->format);
    int vsub, hsub;

    if (srclink->format != tlink->format) {
        av_log(ctx, AV_LOG_ERROR, "inputs must be of same pixel format\n");
        return AVERROR(EINVAL);
    }
    if (srclink->w != tlink->w ||
        srclink->h != tlink->h) {
        av_log(ctx, AV_LOG_ERROR, "First input link %s parameters "
               "(size %dx%d) do not match the corresponding "
               "second input link %s parameters (%dx%d)\n",
               ctx->input_pads[0].name, srclink->w, srclink->h,
               ctx->input_pads[1].name, tlink->w, tlink->h);
        return AVERROR(EINVAL);
    }

    outlink->w = srclink->w;
    outlink->h = srclink->h;
    outlink->time_base = srclink->time_base;
    outlink->sample_aspect_ratio = srclink->sample_aspect_ratio;
    outlink->frame_rate = srclink->frame_rate;

    s->nb_planes = av_pix_fmt_count_planes(outlink->format);

    hsub = desc->log2_chroma_w;
    vsub = desc->log2_chroma_h;

    s->height[1] = s->height[2] = AV_CEIL_RSHIFT(outlink->h, vsub);
    s->height[0] = s->height[3] = outlink->h;
    s->width[1]  = s->width[2]  = AV_CEIL_RSHIFT(outlink->w, hsub);
    s->width[0]  = s->width[3]  = outlink->w;

    return 0;
}

static int tdisplace(AVFilterContext *ctx, AVFrame *time, AVFrame **out)
{
    TDisplaceContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int p, y, x;

    *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!*out)
        return AVERROR(ENOMEM);

    for (p = 0; p < s->nb_planes; p++) {
        uint8_t *dst = (*out)->data[p];
        const uint8_t *tsrc = time->data[p];
        const int tlinesize = time->linesize[p];
        const int dlinesize = (*out)->linesize[p];

        for (y = 0; y < s->height[p]; y++) {
            for (x = 0; x < s->width[p]; x++) {
                int frame = tsrc[x] - 128;
                const int linesize = s->frames[128 + frame]->linesize[p];
                dst[x] = s->frames[128 + frame]->data[p][linesize * y + x];
            }

            tsrc += tlinesize;
            dst += dlinesize;
        }
    }

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    TDisplaceContext *s = ctx->priv;
    AVFrame *frame = NULL;
    AVFrame *tframe = NULL;
    int ret, status;
    int64_t pts;

    if (s->nb_frames < 257 && (ret = ff_inlink_consume_frame(ctx->inputs[0], &frame)) > 0) {
        s->frames[s->nb_frames++] = frame;
    }

    if (s->nb_frames == 257 && (ret = ff_inlink_consume_frame(ctx->inputs[1], &tframe)) > 0) {
        AVFrame *out = NULL;

        if ((ret = tdisplace(ctx, tframe, &out)) < 0)
            return ret;

        out->pts = s->frames[0]->pts;
        av_frame_free(&tframe);
        ret = ff_filter_frame(ctx->outputs[0], out);
        av_frame_free(&s->frames[0]);
        memmove(&s->frames[0], &s->frames[1], (s->nb_frames - 1) * sizeof(s->frames[0]));
        s->frames[MAX_FRAMES-1] = 0;
        s->nb_frames--;
    }
    if (ret < 0) {
        return ret;
    } else if (ff_inlink_acknowledge_status(ctx->inputs[0], &status, &pts)) {
        ff_outlink_set_status(ctx->outputs[0], status, pts);
        return 0;
    } else {
        if (ff_outlink_frame_wanted(ctx->outputs[0]))
            ff_inlink_request_frame(ctx->inputs[0]);
        if (ff_outlink_frame_wanted(ctx->outputs[0]) && s->nb_frames == 257)
            ff_inlink_request_frame(ctx->inputs[1]);
        return 0;
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    TDisplaceContext *s = ctx->priv;
    int i;

    for (i = 0; i < MAX_FRAMES; i++) {
        av_frame_free(&s->frames[i]);
    }
}

static const AVFilterPad tdisplace_inputs[] = {
    {
        .name         = "source",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    {
        .name         = "tmap",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

static const AVFilterPad tdisplace_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_vf_tdisplace = {
    .name          = "tdisplace",
    .description   = NULL_IF_CONFIG_SMALL("Temporal pixel displacement."),
    .priv_size     = sizeof(TDisplaceContext),
    .priv_class    = &tdisplace_class,
    .query_formats = query_formats,
    .uninit        = uninit,
    .activate      = activate,
    .inputs        = tdisplace_inputs,
    .outputs       = tdisplace_outputs,
};
