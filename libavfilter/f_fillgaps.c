/*
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

#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "formats.h"
#include "framesync.h"
#include "internal.h"
#include "video.h"

typedef struct FillGapsContext {
    const AVClass *class;
    int rounding;

    int64_t delta;
    int64_t pts;
    int64_t prev_pts;
    int is_audio;
    int needs_in;
    int needs_fill;
    AVFrame *in;
    AVFrame *last;
    AVFrame *fill;
} FillGapsContext;

#define OFFSET(x) offsetof(FillGapsContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
static const AVOption fillgaps_options[] = {
    { "round", "set rounding method for timestamps", OFFSET(rounding), AV_OPT_TYPE_INT, { .i64 = AV_ROUND_NEAR_INF }, 0, 5, FLAGS, "round" },
    { "zero", "round towards 0",      OFFSET(rounding), AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_ZERO     }, 0, 5, FLAGS, "round" },
    { "inf",  "round away from 0",    OFFSET(rounding), AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_INF      }, 0, 5, FLAGS, "round" },
    { "down", "round towards -infty", OFFSET(rounding), AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_DOWN     }, 0, 5, FLAGS, "round" },
    { "up",   "round towards +infty", OFFSET(rounding), AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_UP       }, 0, 5, FLAGS, "round" },
    { "near", "round to nearest",     OFFSET(rounding), AV_OPT_TYPE_CONST, { .i64 = AV_ROUND_NEAR_INF }, 0, 5, FLAGS, "round" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(fillgaps);

static int fill_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    FillGapsContext *s = ctx->priv;

    s->fill = in;
    s->needs_fill = 0;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    FillGapsContext *s = ctx->priv;
    int ret = 0;

    if (!s->in) {
        s->in = in;
        s->pts = in->pts;
        s->needs_in = 1;
    } else {
        s->delta = av_rescale_q_rnd(in->pts - s->pts, inlink->time_base,
                                    av_inv_q(inlink->frame_rate), s->rounding);
        if (s->delta <= 1) {
            ret = ff_filter_frame(outlink, s->in);
            s->in = in;
            s->pts = in->pts;
            s->prev_pts = in->pts;
            s->needs_in = 1;
            s->delta--;
        } else {
            if (!s->fill) {
                s->needs_fill = 1;
                s->needs_in = 0;
            } else {
                AVFrame *out;
                int64_t pts = s->pts + av_rescale_q_rnd(1, av_inv_q(inlink->time_base), inlink->frame_rate, s->rounding);
                ret = ff_filter_frame(outlink, s->in);
                s->in = in;
                s->pts = in->pts;
                s->delta--;
                s->prev_pts = pts;

                if (pts < in->pts) {
                    out = s->fill;
                    out->pts = pts;
                    ret = ff_filter_frame(outlink, out);
                    s->pts = in->pts;
                    s->fill = NULL;
                    s->needs_in = 1;
                    s->needs_fill = 1;
                    s->delta--;
                }
            }
        }
    }

    return ret;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FillGapsContext *s = ctx->priv;

    if (!s->fill && s->needs_fill) {
        return ff_request_frame(ctx->inputs[1]);
    }

    if (s->delta > 1 && s->fill) {
        AVFrame *out = s->fill;
        int64_t pts = s->prev_pts + av_rescale_q_rnd(1, av_inv_q(inlink->time_base), inlink->frame_rate, s->rounding);

        out->pts = pts;
        s->prev_pts = pts;
        s->fill = NULL;
        s->needs_fill = 1;
        s->delta--;
        return ff_filter_frame(outlink, out);
    }

    if (s->needs_in) {
        return ff_request_frame(ctx->inputs[0]);
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];

    switch (outlink->type) {
    case AVMEDIA_TYPE_VIDEO:
        outlink->w = inlink->w;
        outlink->h = inlink->h;
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
        outlink->frame_rate = inlink->frame_rate;
        break;
    case AVMEDIA_TYPE_AUDIO:
        outlink->sample_rate    = inlink->sample_rate;
        outlink->channels       = inlink->channels;
        outlink->channel_layout = inlink->channel_layout;
        break;
    }

    outlink->time_base = inlink->time_base;
    outlink->format = inlink->format;

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    FillGapsContext *s = ctx->priv;

    if (!strcmp(ctx->filter->name, "afillgaps"))
        s->is_audio = 1;

    s->needs_fill = 1;
    s->needs_in = 1;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats, *rates = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    int ret, i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        formats = ff_all_formats(ctx->inputs[i]->type);
        if ((ret = ff_set_common_formats(ctx, formats)) < 0)
            return ret;

        if (ctx->inputs[i]->type == AVMEDIA_TYPE_AUDIO) {
            rates = ff_all_samplerates();
            if ((ret = ff_set_common_samplerates(ctx, rates)) < 0)
                return ret;
            layouts = ff_all_channel_counts();
            if ((ret = ff_set_common_channel_layouts(ctx, layouts)) < 0)
                return ret;
        }
    }

    return 0;
}

static const AVFilterPad fillgaps_inputs[] = {
    {
        .name             = "main",
        .type             = AVMEDIA_TYPE_VIDEO,
        .filter_frame     = filter_frame,
    },{
        .name             = "fill",
        .type             = AVMEDIA_TYPE_VIDEO,
        .filter_frame     = fill_frame,
    },
    { NULL }
};

static const AVFilterPad fillgaps_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_fillgaps = {
    .name            = "fillgaps",
    .description     = NULL_IF_CONFIG_SMALL("Fill gaps in video stream"),
    .init            = init,
    .query_formats   = query_formats,
    .uninit          = uninit,
    .outputs         = fillgaps_outputs,
    .inputs          = fillgaps_inputs,
    .priv_size       = sizeof(FillGapsContext),
    .priv_class      = &fillgaps_class,
};

#define afillgaps_options fillgaps_options
AVFILTER_DEFINE_CLASS(afillgaps);

AVFilter ff_af_afillgaps = {
    .name            = "afillgaps",
    .description     = NULL_IF_CONFIG_SMALL("Fill gaps in audio stream"),
    .init            = init,
    .query_formats   = query_formats,
    .uninit          = uninit,
    .outputs         = fillgaps_outputs,
    .inputs          = fillgaps_inputs,
    .priv_size       = sizeof(FillGapsContext),
    .priv_class      = &afillgaps_class,
};
