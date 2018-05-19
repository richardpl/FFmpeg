/*
 * Copyright (c) 2018 Paul B Mahol
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

#include <float.h>

#include "libavutil/avassert.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "formats.h"

#define SQR(x) ((x) * (x))

typedef struct AudioNLMeansContext {
    const AVClass *class;

    double g;
    double B;
    double h;
    double a;
    double m;
    int n;
    int K;
    int S;

    int N;
    int hop_size;

    AVFrame *in;

    double *kernel;

    int64_t pts;
    uint64_t nb_samples;
    int samples_left;

    AVAudioFifo *fifo;
} AudioNLMeansContext;

#define OFFSET(x) offsetof(AudioNLMeansContext, x)
#define AF AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption anlmeans_options[] = {
    { "n", "set number of patches", OFFSET(n), AV_OPT_TYPE_INT,    {.i64=1},        1,         4, AF },
    { "K", "set patch radius",      OFFSET(K), AV_OPT_TYPE_INT,    {.i64=32},       0,      4096, AF },
    { "S", "set research radius",   OFFSET(S), AV_OPT_TYPE_INT,    {.i64=64},       1,      4096, AF },
    { "h", "set strength",          OFFSET(h), AV_OPT_TYPE_DOUBLE, {.dbl=1},        1,   FLT_MAX, AF },
    { "m", "set max allowed diff",  OFFSET(m), AV_OPT_TYPE_DOUBLE, {.dbl=0.1},      0,         2, AF },
    { "a", "set denoising amount",  OFFSET(a), AV_OPT_TYPE_DOUBLE, {.dbl=1},        1,   FLT_MAX, AF },
    { "B", "set smooth factor",     OFFSET(B), AV_OPT_TYPE_DOUBLE, {.dbl=1},        1,   FLT_MAX, AF },
    { "g", "set output gain",       OFFSET(g), AV_OPT_TYPE_DOUBLE, {.dbl=1},        0,        10, AF },
    { NULL }
};

AVFILTER_DEFINE_CLASS(anlmeans);

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    static const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_FLTP,
        AV_SAMPLE_FMT_NONE
    };
    int ret;

    formats = ff_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    ret = ff_set_common_formats(ctx, formats);
    if (ret < 0)
        return ret;

    layouts = ff_all_channel_counts();
    if (!layouts)
        return AVERROR(ENOMEM);

    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        return ret;

    formats = ff_all_samplerates();
    return ff_set_common_samplerates(ctx, formats);
}

static void compute_kernel(double *kernel, int K, double B2)
{
    int k;

    for (k = 0; k <= 2 * K; k++) {
        double kk = k - K;

        kernel[k] = exp(-(kk * kk) / B2);
    }
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioNLMeansContext *s = ctx->priv;

    s->m = SQR(s->m * 65536.);
    s->pts = AV_NOPTS_VALUE;
    s->N = s->n * s->K * 2 + 1 + (s->K + s->S) * 2;
    s->hop_size = s->n * s->K * 2 + 1;

    av_frame_free(&s->in);
    s->in = ff_get_audio_buffer(outlink, s->N);
    if (!s->in)
        return AVERROR(ENOMEM);

    s->fifo = av_audio_fifo_alloc(outlink->format, outlink->channels, s->N);
    if (!s->fifo)
        return AVERROR(ENOMEM);

    s->kernel = av_calloc(s->K * 2 + 1, sizeof(*s->kernel));
    if (!s->kernel)
        return AVERROR(ENOMEM);

    compute_kernel(s->kernel, s->K, 1. / (s->B * s->B));

    return 0;
}

typedef struct ThreadData {
    AVFrame *out;
} ThreadData;

static double compute_distance(const double *kernel,
                               const float *f1, const float *f2, int K)
{
    double sum = 0.;
    int k;

    for (k = -K; k <= K; k++) {
        sum += kernel[k] * SQR(65536. * f1[k] - 65536. * f2[k]);
    }

    return sum;
}

static int filter_channel(AVFilterContext *ctx, void *arg, int ch, int nb_jobs)
{
    AudioNLMeansContext *s = ctx->priv;
    ThreadData *td = arg;
    AVFrame *out = td->out;
    const int S = s->S;
    const int K = s->K;
    const double m = s->m;
    const double g = s->g;
    const double *kernel = s->kernel + K;
    const float *f = (const float *)(s->in->extended_data[ch]) + K;
    const double sw = s->a / (s->h * s->h);
    float *dst = (float *)out->extended_data[ch];
    int i, j;

    for (i = S; i < s->hop_size + S; i++) {
        double P = 0., Q = 0.;

        for (j = i - S; j <= i + S; j++) {
            double w, d2;

            d2 = compute_distance(kernel, f + i, f + j, K);

            if (d2 >= m)
                continue;

            w = exp(-d2 * sw);
            P += w * f[j];
            Q += w;
        }

        dst[i - S] = g * P / Q;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AudioNLMeansContext *s = ctx->priv;
    AVFrame *out = NULL;
    int ret = 0;

    if (s->pts == AV_NOPTS_VALUE)
        s->pts = in->pts;

    ret = av_audio_fifo_write(s->fifo, (void **)in->extended_data,
                              in->nb_samples);
    av_frame_free(&in);

    while (av_audio_fifo_size(s->fifo) >= s->N) {
        ThreadData td;

        out = ff_get_audio_buffer(outlink, s->hop_size);
        if (!out)
            return AVERROR(ENOMEM);

        ret = av_audio_fifo_peek(s->fifo, (void **)s->in->extended_data,
                                 s->N);
        if (ret < 0)
            break;

        td.out = out;
        ctx->internal->execute(ctx, filter_channel, &td, NULL, inlink->channels);

        av_audio_fifo_drain(s->fifo, s->hop_size);

        if (s->samples_left > 0)
            out->nb_samples = FFMIN(s->hop_size, s->samples_left);

        out->pts = s->pts;
        s->pts += s->hop_size;

        ret = ff_filter_frame(outlink, out);
        if (ret < 0)
            break;
    }

    if (ret < 0)
        av_frame_free(&out);
    return ret;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioNLMeansContext *s = ctx->priv;

    av_audio_fifo_free(s->fifo);
    av_frame_free(&s->in);
    av_freep(&s->kernel);
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
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_af_anlmeans = {
    .name          = "anlmeans",
    .description   = NULL_IF_CONFIG_SMALL("Reduce broadband noise from input audio using Non-Local Means."),
    .query_formats = query_formats,
    .priv_size     = sizeof(AudioNLMeansContext),
    .priv_class    = &anlmeans_class,
    .uninit        = uninit,
    .inputs        = inputs,
    .outputs       = outputs,
    .flags         = AVFILTER_FLAG_SLICE_THREADS,
};
