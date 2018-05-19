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

#include <fftw3.h>

#include "libavutil/avassert.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "formats.h"

#define SQR(x) ((x) * (x))

typedef struct NLMeansChannel {
    float *matrix;
    fftwf_plan planf;
    fftwf_plan planb;
    fftwf_complex *matrixc;
} NLMeansChannel;

typedef struct AudioNLMeansContext {
    const AVClass *class;

    float g;
    float B;
    float h;
    int n;
    int K;
    int S;

    int N;
    int hop_size;

    fftwf_complex *kernelc;

    AVFrame *in;
    AVFrame *out;

    NLMeansChannel *chan;
    float *kernel;
    int kernel_size;
    int matrix_size;
    int conv_size;

    int64_t pts;
    int nb_channels;
    uint64_t nb_samples;
    int samples_left;

    AVAudioFifo *fifo;
} AudioNLMeansContext;

#define OFFSET(x) offsetof(AudioNLMeansContext, x)
#define AF AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption anlmeans_options[] = {
    { "n", "set number of patches", OFFSET(n), AV_OPT_TYPE_INT,   {.i64=1},     1,    4, AF },
    { "K", "set patch radius",      OFFSET(K), AV_OPT_TYPE_INT,   {.i64=300},   0, 2024, AF },
    { "S", "set research radius",   OFFSET(S), AV_OPT_TYPE_INT,   {.i64=64},    1, 2024, AF },
    { "h", "set strength" ,         OFFSET(h), AV_OPT_TYPE_FLOAT, {.dbl=1}, .0001, 9999, AF },
    { "B", "set smooth factor",     OFFSET(B), AV_OPT_TYPE_FLOAT, {.dbl=1}, .0001, 9999, AF },
    { "g", "set output gain",       OFFSET(g), AV_OPT_TYPE_FLOAT, {.dbl=1},     0,   10, AF },
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

static void compute_kernel(float *kernel, int K, float B2)
{
    double sum = 0;
    int k;

    for (k = 0; k <= 2 * K; k++) {
        float kk = k - K;

        kernel[k] = exp(-(kk * kk) / B2);
        sum += SQR(kernel[k]);
    }

    sum = sqrt(1. / sum);
    for (k = 0; k <= 2 * K; k++) {
        kernel[k] *= sum;
    }
}

static int get_pos(int i, int j, int N, int S, int K)
{
    return (S - i + j) * (2 * N + 2 * K - S - 1 - i + j) / 2 + j;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioNLMeansContext *s = ctx->priv;
    int i;

    s->pts = AV_NOPTS_VALUE;
    s->N = s->n * s->K * 2 + 1 + s->S * 2;
    s->hop_size = s->n * s->K * 2 + 1;

    av_frame_free(&s->in);
    av_frame_free(&s->out);
    s->in = ff_get_audio_buffer(outlink, s->N);
    s->out = ff_get_audio_buffer(outlink, s->N);
    if (!s->in || !s->out)
        return AVERROR(ENOMEM);

    s->fifo = av_audio_fifo_alloc(outlink->format, outlink->channels, s->N);
    if (!s->fifo)
        return AVERROR(ENOMEM);

    s->nb_channels = outlink->channels;
    s->kernel_size = 2 * s->K + 1;
    s->matrix_size = get_pos(s->N, s->N, s->N, s->S, s->K) + 1;
    s->conv_size = s->N + s->kernel_size;
    s->kernel = av_calloc(s->conv_size, sizeof(*s->kernel));
    s->chan = av_calloc(outlink->channels, sizeof(*s->chan));
    if (!s->chan || !s->kernel)
        return AVERROR(ENOMEM);

    s->kernelc = fftwf_alloc_complex(s->conv_size);

    for (i = 0; i < outlink->channels; i++) {
        NLMeansChannel *c = &s->chan[i];

        c->planf = fftwf_plan_dft_r2c_1d(s->conv_size - 1, s->kernel, s->kernelc, FFTW_ESTIMATE);
        c->planb = fftwf_plan_dft_c2r_1d(s->conv_size - 1, s->kernelc, s->kernel, FFTW_ESTIMATE);

        c->matrix = av_calloc(s->matrix_size, sizeof(*c->matrix));
        c->matrixc = fftwf_alloc_complex(s->conv_size);
        if (!c->matrix)
            return AVERROR(ENOMEM);
    }

    compute_kernel(s->kernel, s->K, s->K * s->B * s->B / 4.f);

    fftwf_execute_dft_r2c(s->chan[0].planf, s->kernel, s->kernelc);

    return 0;
}

typedef struct ThreadData {
    AVFrame *out;
} ThreadData;

static void fcmul(fftwf_complex *r, const fftwf_complex *f, const fftwf_complex *c,
                  ptrdiff_t len)
{
    int n;

    for (n = 0; n <= len / 2; n++) {
        const float cre = c[n][0];
        const float cim = c[n][1];
        const float fre = f[n][0];
        const float fim = f[n][1];

        r[n][0] = fre * cre - fim * cim;
        r[n][1] = fim * cre + fre * cim;
    }
}

static void compute_v(NLMeansChannel *c, const float *f,
                      float *Vt, int conv_size,
                      int N, int K, int S,
                      const fftwf_complex *kernelc,
                      int matrix_size)
{
    int u, v, l = 0;
    float *m;

    m = av_malloc_array(N + 2 * K + 1, sizeof(*m));

    memset(Vt, 0, matrix_size * sizeof(*Vt));

    for (u = 0; u < S + 1; u++) {
        memset(m, 0, (N + 2 * K + 1) * sizeof(*m));

        for (v = 0; v < N - S - 1 + u; v++) {
            const float f1 = f[S + 1 - u + v];
            const float f2 = f[v];

            m[v] = (f1 * f2);
        }

        fftwf_execute_dft_r2c(c->planf, m, c->matrixc);
        fcmul(c->matrixc, c->matrixc, kernelc, conv_size);
        fftwf_execute_dft_c2r(c->planb, c->matrixc, m);

        for (v = 0; v < N - S - 1 + u; v++) {
            Vt[l] = m[K + v];
            l++;
        }

        l += K;
    }

    av_free(m);
}

static int filter_channel(AVFilterContext *ctx, void *arg, int ch, int nb_jobs)
{
    AudioNLMeansContext *s = ctx->priv;
    NLMeansChannel *c = &s->chan[ch];
    ThreadData *td = arg;
    AVFrame *out = td->out;
    const int N = s->N;
    const int S = s->S;
    const int K = s->K;
    const float *f = (const float *)s->in->extended_data[ch];
    const double h2 = 1. / (K * 25. * s->h * s->h);
    const double sd = 1. / (s->conv_size - 1);
    const double g = s->g;
    float *V = c->matrix;
    float *dst = (float *)s->out->extended_data[ch];
    float *ptr = (float *)out->extended_data[ch];
    int i, j;

    compute_v(c, f, V, s->conv_size, N, K, S, s->kernelc,
              s->matrix_size);

    for (i = S; i < N - S; i++) {
        double P = 0., Q = 0.;

        for (j = i - S; j <= i + S; j++) {
            double w, d;

            d = V[get_pos(i, i, N, S, K)] + V[get_pos(j, j, N, S, K)];
            if (i >= j) {
                d -= 2. * V[get_pos(i, j, N, S, K)];
            } else {
                d -= 2. * V[get_pos(j, i, N, S, K)];
            }

            d = d * sd;
            w = exp(-(d) * h2);
            av_log(NULL, AV_LOG_DEBUG, "w: %g\n", w);

            P += w * f[j];
            Q += w;
        }

        dst[i] = g * P / Q;
    }

    for (j = 0; j < s->hop_size; j++) {
        ptr[j] = dst[S + j];
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

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    //AudioNLMeansContext *s = ctx->priv;
    int ret = 0;

    ret = ff_request_frame(ctx->inputs[0]);

    return ret;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioNLMeansContext *s = ctx->priv;
    int i;

    av_audio_fifo_free(s->fifo);
    av_frame_free(&s->in);
    av_frame_free(&s->out);

    if (s->chan) {
        for (i = 0; i < s->nb_channels; i++) {
            NLMeansChannel *c = &s->chan[i];

            av_freep(&c->matrix);

            fftwf_destroy_plan(c->planf);
            fftwf_destroy_plan(c->planb);
        }
    }
    av_freep(&s->chan);
    av_freep(&s->kernel);
    s->kernel_size = 0;
    s->nb_channels = 0;
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
        .request_frame = request_frame,
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
