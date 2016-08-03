/*
 * Copyright (c) 2004 Alex Beregszaszi & Pierre Lombard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"
#include "audio.h"

typedef struct SADRCMem {
    float avg;
    int len;
} SADRCMem;

typedef struct SADRCContext {
    const AVClass *class;
    int method;
    float mul, lastavg;
    int idx;
    int mem_size;
    SADRCMem *mem;
    float mid;
} SADRCContext;

#define OFFSET(x) offsetof(SADRCContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption sadrc_options[] = {
    { "m", "set method", OFFSET(method), AV_OPT_TYPE_INT,   {.i64 = 1.0},       1,   2, FLAGS },
    { "t", "set target", OFFSET(mid),    AV_OPT_TYPE_FLOAT, {.dbl = 0.25}, 0.0001, 1.0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(sadrc);

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    SADRCContext *s = ctx->priv;
    const float *src = (const float *)in->data[0];
    const int channels = inlink->channels;
    const int nb_samples = in->nb_samples;
    const int len = channels * nb_samples;
    AVFrame *out;
    float *dst;

    if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_audio_buffer(inlink, in->nb_samples);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }
    dst = (float *)out->data[0];

    if (s->method == 1) {
        float curavg = 0.0, newavg, neededmul, tmp;
        int i;

        for (i = 0; i < len; i++) {
            tmp = src[i];
            curavg += tmp * tmp;
        }
        curavg = sqrtf(curavg / (float)len);

        if (curavg > 0.01) {
            neededmul = s->mid / (curavg * s->mul);
            s->mul = (1.0 - 0.06) * s->mul + 0.06 * neededmul;
            s->mul = av_clipf(s->mul, 0.1, 5.0);
        }

        for (i = 0; i < len; i++)
            dst[i] = src[i] * s->mul;

        newavg = s->mul * curavg;

        s->lastavg = (1.0 - 0.06) * s->lastavg + 0.06 * newavg;
    } else {
        float curavg = 0.0, newavg, avg = 0.0, tmp;
        int i, totallen = 0;

        for (i = 0; i < len; i++) {
            tmp = src[i];
            curavg += tmp * tmp;
        }
        curavg = sqrtf(curavg / (float)len);

        for (i = 0; i < s->mem_size; i++) {
            avg += s->mem[i].avg * (float)s->mem[i].len;
            totallen += s->mem[i].len;
        }

        if (totallen > 32000) {
            avg /= (float)totallen;
            if (avg >= 0.01) {
                s->mul = s->mid / avg;
                s->mul = av_clipf(s->mul, 0.1, 5.0);
            }
        }

        for (i = 0; i < len; i++)
            dst[i] = src[i] * s->mul;

        newavg = s->mul * curavg;

        s->mem[s->idx].len = len;
        s->mem[s->idx].avg = newavg;
        s->idx = (s->idx + 1) % s->mem_size;
    }

    if (in != out)
        av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterChannelLayouts *layouts;
    static const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_NONE
    };
    int ret;

    layouts = ff_all_channel_counts();
    if (!layouts)
        return AVERROR(ENOMEM);
    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        return ret;

    formats = ff_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    ret = ff_set_common_formats(ctx, formats);
    if (ret < 0)
        return ret;

    formats = ff_all_samplerates();
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_samplerates(ctx, formats);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    SADRCContext *s = ctx->priv;

    s->mul = 1.0;
    s->lastavg = .25;
    s->idx = 0;
    s->mem_size = 128;

    s->mem = av_calloc(s->mem_size, sizeof(*s->mem));
    if (!s->mem)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SADRCContext *s = ctx->priv;

    av_freep(&s->mem);
}

static const AVFilterPad avfilter_af_sadrc_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_af_sadrc_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_sadrc = {
    .name          = "sadrc",
    .description   = NULL_IF_CONFIG_SMALL("Simple audio dynamic range compressor."),
    .priv_size     = sizeof(SADRCContext),
    .priv_class    = &sadrc_class,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = avfilter_af_sadrc_inputs,
    .outputs       = avfilter_af_sadrc_outputs,
};
