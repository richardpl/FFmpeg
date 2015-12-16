/*
 * Copyright (c) 2015 Paul B. Mahol
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

#include <math.h>

#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"
#include "audio.h"

enum AmbisonicMatrix {
    SQUARE,
    NB_AMATRIX,
};

static const struct {
    int channels;
    float matrix[8][4];
} ambisonic_matrix[] = {
    [SQUARE] = {
        .channels = 4,
        .matrix = {
            { M_SQRT2/4,  M_SQRT2/4,  M_SQRT2/4,     0 },
            { M_SQRT2/4,  M_SQRT2/4, -M_SQRT2/4,     0 },
            { M_SQRT2/4, -M_SQRT2/4, -M_SQRT2/4,     0 },
            { M_SQRT2/4, -M_SQRT2/4,  M_SQRT2/4,     0 },
        },
    },
};

typedef struct AmbisonicContext {
    const AVClass *class;

    int matrix;
} AmbisonicContext;

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AmbisonicContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    int n, ic, oc;

    out = ff_get_audio_buffer(outlink, in->nb_samples);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    for (oc = 0; oc < outlink->channels; oc++) {
        float *dst = (float *)out->extended_data[oc];
        float *src = (float *)in->extended_data[0];

        for (n = 0; n < in->nb_samples; n++) {
            dst[n] = src[n] * ambisonic_matrix[s->matrix].matrix[oc][0];
        }

        for (ic = 1; ic < inlink->channels; ic++) {
            src = (float *)in->extended_data[ic];

            for (n = 0; n < in->nb_samples; n++) {
                dst[n] += src[n] * ambisonic_matrix[s->matrix].matrix[oc][ic];
            }
        }
    }

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int query_formats(AVFilterContext *ctx)
{
    struct AmbisonicContext *s = ctx->priv;
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    int och = ambisonic_matrix[s->matrix].channels;
    int ret;

    ret = ff_add_format(&formats, AV_SAMPLE_FMT_FLTP);
    if (ret)
        return ret;
    ret = ff_set_common_formats(ctx, formats);
    if (ret)
        return ret;

    ret = ff_add_channel_layout(&layouts, FF_COUNT2LAYOUT(4));
    if (!layouts)
        return AVERROR(ENOMEM);

    ret = ff_channel_layouts_ref(layouts, &ctx->inputs[0]->out_channel_layouts);
    if (ret)
        return ret;

    layouts = NULL;
    ret = ff_add_channel_layout(&layouts, FF_COUNT2LAYOUT(och));
    if (ret)
        return ret;
    ret = ff_channel_layouts_ref(layouts, &ctx->outputs[0]->in_channel_layouts);
    if (ret)
        return ret;

    formats = ff_all_samplerates();
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_samplerates(ctx, formats);
}

#define OFFSET(x) offsetof(AmbisonicContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption ambisonic_options[] = {
    { "layout", "set output layout", OFFSET(matrix), AV_OPT_TYPE_INT, {.i64=0}, 0, 32, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(ambisonic);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_ambisonic = {
    .name          = "ambisonic",
    .description   = NULL_IF_CONFIG_SMALL("Ambisonic decoder."),
    .priv_size     = sizeof(AmbisonicContext),
    .priv_class    = &ambisonic_class,
    .query_formats = query_formats,
    .inputs        = inputs,
    .outputs       = outputs,
};
