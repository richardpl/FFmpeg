/*
 * Copyright (c) 2019 Paul B Mahol
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
#include "libavutil/samplefmt.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "internal.h"

typedef struct AudioSamplingContext {
    const AVClass *class;

    int mode;
    int factor;

    int min_in_samples;
    int64_t next_pts;

    void (*filter)(AVFrame *out, AVFrame *in, int mode, int factor);
} AudioSamplingContext;

#define OFFSET(x) offsetof(AudioSamplingContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption asampling_options[] = {
    { "mode",   "set sampling mode",       OFFSET(mode),   AV_OPT_TYPE_INT,   {.i64=1}, 0, 1, A, "mode" },
    {   "down", 0,                         0,              AV_OPT_TYPE_CONST, {.i64=0}, 0, 0, A, "mode" },
    {   "up",   0,                         0,              AV_OPT_TYPE_CONST, {.i64=1}, 0, 0, A, "mode" },
    { "factor", "set downsampling factor", OFFSET(factor), AV_OPT_TYPE_INT,   {.i64=1}, 1, 64, A },
    { NULL }
};

AVFILTER_DEFINE_CLASS(asampling);

static int query_formats(AVFilterContext *ctx)
{
    AudioSamplingContext *s = ctx->priv;
    AVFilterChannelLayouts *layouts;
    AVFilterFormats *formats;
    int sample_rates[] = { 44100, -1 };
    static const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
        AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_DBLP,
        AV_SAMPLE_FMT_NONE
    };
    AVFilterFormats *avff;
    int ret;

    if (!ctx->inputs[0]->in_samplerates ||
        !ctx->inputs[0]->in_samplerates->nb_formats) {
        return AVERROR(EAGAIN);
    }

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

    avff = ctx->inputs[0]->in_samplerates;
    sample_rates[0] = avff->formats[0];
    if (!ctx->inputs[0]->out_samplerates)
        if ((ret = ff_formats_ref(ff_make_format_list(sample_rates),
                                  &ctx->inputs[0]->out_samplerates)) < 0)
            return ret;

    if (s->mode)
        sample_rates[0] = avff->formats[0] * s->factor;
    else
        sample_rates[0] = avff->formats[0] / s->factor;

    return ff_formats_ref(ff_make_format_list(sample_rates),
                         &ctx->outputs[0]->in_samplerates);
}

static void filter_dbl(AVFrame *out, AVFrame *in, int mode, int factor)
{
    const double *src = (const double *)in->extended_data[0];
    double *dst = (double *)out->extended_data[0];
    const int channels = in->channels;

    if (mode) {
        for (int n = 0; n < in->nb_samples; n++) {
            for (int c = 0; c < in->channels; c++)
                dst[c] = *src++;
            dst += factor * channels;
        }
    } else {
        for (int n = 0; n < out->nb_samples; n++) {
            for (int c = 0; c < in->channels; c++)
                *dst++ = src[c];
            src += factor * channels;
        }
    }
}

static void filter_flt(AVFrame *out, AVFrame *in, int mode, int factor)
{
    const float *src = (const float *)in->extended_data[0];
    float *dst = (float *)out->extended_data[0];
    const int channels = in->channels;

    if (mode) {
        for (int n = 0; n < in->nb_samples; n++) {
            for (int c = 0; c < in->channels; c++)
                dst[c] = *src++;
            dst += factor * channels;
        }
    } else {
        for (int n = 0; n < out->nb_samples; n++) {
            for (int c = 0; c < in->channels; c++)
                *dst++ = src[c];
            src += factor * channels;
        }
    }
}

static void filter_dblp(AVFrame *out, AVFrame *in, int mode, int factor)
{
    if (mode) {
        for (int c = 0; c < in->channels; c++) {
            const double *src = (const double *)in->extended_data[c];
            double *dst = (double *)out->extended_data[c];

            for (int n = 0; n < in->nb_samples; n++)
                dst[n * factor] = src[n];
        }
    } else {
        for (int c = 0; c < in->channels; c++) {
            const double *src = (const double *)in->extended_data[c];
            double *dst = (double *)out->extended_data[c];

            for (int n = 0; n < out->nb_samples; n++)
                dst[n] = src[n * factor];
        }
    }
}

static void filter_fltp(AVFrame *out, AVFrame *in, int mode, int factor)
{
    if (mode) {
        for (int c = 0; c < in->channels; c++) {
            const float *src = (const float *)in->extended_data[c];
            float *dst = (float *)out->extended_data[c];

            for (int n = 0; n < in->nb_samples; n++)
                dst[n * factor] = src[n];
        }
    } else {
        for (int c = 0; c < in->channels; c++) {
            const float *src = (const float *)in->extended_data[c];
            float *dst = (float *)out->extended_data[c];

            for (int n = 0; n < out->nb_samples; n++)
                dst[n] = src[n * factor];
        }
    }
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AudioSamplingContext *s = ctx->priv;

    s->next_pts = AV_NOPTS_VALUE;

    if (s->mode)
        s->min_in_samples = 1;
    else
        s->min_in_samples = s->factor;

    switch (inlink->format) {
    case AV_SAMPLE_FMT_FLT:  s->filter = filter_flt;  break;
    case AV_SAMPLE_FMT_FLTP: s->filter = filter_fltp; break;
    case AV_SAMPLE_FMT_DBL:  s->filter = filter_dbl;  break;
    case AV_SAMPLE_FMT_DBLP: s->filter = filter_dblp; break;
    }

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AudioSamplingContext *s = ctx->priv;
    const int factor = s->factor;
    AVFrame *in, *out;
    int nb_samples, nb_out_samples;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    nb_samples = ff_inlink_queued_samples(inlink);

    if (nb_samples >= s->min_in_samples) {
        if (!s->mode)
            nb_samples = (nb_samples / factor) * factor;
        ff_inlink_consume_samples(inlink, nb_samples, nb_samples, &in);

        if (s->mode)
            nb_out_samples = in->nb_samples * s->factor;
        else
            nb_out_samples = in->nb_samples / s->factor;

        out = ff_get_audio_buffer(outlink, nb_out_samples);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }

        if (s->next_pts == AV_NOPTS_VALUE)
            s->next_pts = in->pts;

        s->filter(out, in, s->mode, s->factor);

        out->pts = s->next_pts;
        s->next_pts += av_rescale_q(out->nb_samples, (AVRational){1, outlink->sample_rate}, outlink->time_base);
        av_frame_free(&in);
        return ff_filter_frame(outlink, out);
    }

    FF_FILTER_FORWARD_STATUS(inlink, outlink);
    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static const AVFilterPad asampling_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad asampling_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_asampling = {
    .name          = "asampling",
    .description   = NULL_IF_CONFIG_SMALL("Upsample or downsample audio by integer factor."),
    .query_formats = query_formats,
    .priv_size     = sizeof(AudioSamplingContext),
    .priv_class    = &asampling_class,
    .activate      = activate,
    .inputs        = asampling_inputs,
    .outputs       = asampling_outputs,
};
