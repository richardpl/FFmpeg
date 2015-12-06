/*
 * Copyright (c) 2010 Fons Adriaensen <fons@kokkinizita.net>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <float.h>
#include <math.h>

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/xga_font_data.h"
#include "libavutil/opt.h"
#include "audio.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"

typedef struct ITUR468Filter {
    double whp;
    double a11, a12;
    double a21, a22;
    double a31, a32;
    double b30, b31, b32;
    double zhp;
    double z11, z12;
    double z21, z22;
    double z31, z32;

    double a1, b1;
    double a2, b2;
    double z1, z2;
} ITUR468Filter;

typedef struct {
    const AVClass *class;

    ITUR468Filter *filter;
} ITUR468Context;

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    ITUR468Context *s = ctx->priv;
    int c;

    s->filter = av_calloc(inlink->channels, sizeof(*s->filter));
    if (!s->filter)
        return AVERROR(ENOMEM);

    for (c = 0; c < inlink->channels; c++) {
        ITUR468Filter *f = &s->filter[c];

        f->whp =  3.8715217e-01;
        f->a11 = -8.4163201e-01;
        f->a12 =  3.0498350e-01;
        f->a21 = -6.5680242e-01;
        f->a22 =  2.3733993e-01;
        f->a31 = -3.3843556e-01;
        f->a32 =  4.3756709e-01;
        f->b30 =  9.8607997e-01;
        f->b31 =  5.4846389e-01;
        f->b32 = -8.2465158e-02;
        f->a1  = 670. / inlink->sample_rate;
        f->b1  = 3.5  / inlink->sample_rate;
        f->a2  = 6.6  / inlink->sample_rate;
        f->b2  = 0.65 / inlink->sample_rate;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ITUR468Context *s = ctx->priv;
    AVDictionary **metadata;
    metadata = avpriv_frame_get_metadatap(in);
    int n, c;

    for (c = 0; c < inlink->channels; c++) {
        ITUR468Filter *f = &s->filter[c];
        double *src = (double *)in->extended_data[c];
        double out, x, zhp, z11, z12, z21, z22, z31, z32, z1, z2;

        zhp = f->zhp;
        z11 = f->z11;
        z12 = f->z12;
        z21 = f->z21;
        z22 = f->z22;
        z31 = f->z31;
        z32 = f->z32;
        z1  = f->z1;
        z2  = f->z2;

        for (n = 0; n < in->nb_samples; n++) {
            x = src[n];
            zhp += f->whp * (x - zhp) + 1e-20;
            x -= zhp;
            x -= f->a11 * z11 + f->a12 * z12;
            z12 = z11;
            z11 = x;
            x -= f->a21 * z21 + f->a22 * z22;
            z22 = z21;
            z21 = x;
            x -= f->a31 * z31 + f->a32 * z32;
            out = f->b30 * x + f->b31 * z31 + f->b32 * z32;
            z32 = z31;
            z31 = x;
            x = out;
            x = fabs(x) + 1e-30;
            z1 -= z1 * f->b1;
            if (x > z1)
                z1 += f->a1 * (x - z1);

            z2 -= z2 * f->b2;
            if (z1 > z2)
                z2 += f->a2 * (z1 - z2);
        }

        f->zhp = zhp;
        f->z11 = z11;
        f->z12 = z12;
        f->z21 = z21;
        f->z22 = z22;
        f->z31 = z31;
        f->z32 = z32;
        f->z1 = z1;
        f->z2 = z2;
        if (metadata) {
            uint8_t value[128];
            uint8_t key[128];

            snprintf(key, sizeof(key), "lavfi.itur468.%d.noise", c + 1);
            snprintf(value, sizeof(value), "%+1.1lf", 20 * log10(1.1453 * f->z2));
            av_dict_set(metadata, key, value, 0);
        }
    }

    return ff_filter_frame(ctx->outputs[0], in);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterChannelLayouts *layouts;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;

    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_DBLP, AV_SAMPLE_FMT_NONE };
    static const int input_srate[] = {48000, -1};

    formats = ff_make_format_list(sample_fmts);
    if ((ret = ff_formats_ref(formats, &inlink->out_formats)) < 0 ||
        (ret = ff_formats_ref(formats, &outlink->in_formats)) < 0)
        return ret;

    layouts = ff_all_channel_counts();
    if ((ret = ff_channel_layouts_ref(layouts, &inlink->out_channel_layouts)) < 0 ||
        (ret = ff_channel_layouts_ref(layouts, &outlink->in_channel_layouts)) < 0)
        return ret;

    formats = ff_make_format_list(input_srate);
    if ((ret = ff_formats_ref(formats, &inlink->out_samplerates)) < 0 ||
        (ret = ff_formats_ref(formats, &outlink->in_samplerates)) < 0)
        return ret;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ITUR468Context *s = ctx->priv;

    av_freep(&s->filter);
}

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
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_af_itur468 = {
    .name          = "itur468",
    .description   = NULL_IF_CONFIG_SMALL("ITU-R 468 noise meter."),
    .priv_size     = sizeof(ITUR468Context),
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = inputs,
    .outputs       = outputs,
};
