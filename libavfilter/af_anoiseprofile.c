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

#include "libavcodec/avfft.h"
#include "libavutil/opt.h"
#include "audio.h"
#include "avfilter.h"
#include "internal.h"

#define WINSIZE 2048
#define NB_FREQ (WINSIZE / 2 + 1)

typedef struct ChannelData {
    float    sum[NB_FREQ];
    uint64_t profilecount[NB_FREQ];
} ChannelData;

typedef struct NoiseProfileContext {
    const AVClass *class;

    FFTContext *fft;
    FFTComplex *fft_data;

    int channels;
    char *profile_file_str;
    FILE *profile_file;

    ChannelData *chandata;
    float out[NB_FREQ];
} NoiseProfileContext;

#define OFFSET(x) offsetof(NoiseProfileContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption anoiseprofile_options[] = {
    { "file", "set the profile output file", OFFSET(profile_file_str), AV_OPT_TYPE_STRING, {.str = NULL }, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(anoiseprofile);

static av_cold int init(AVFilterContext *ctx)
{
    NoiseProfileContext *s = ctx->priv;

    if (!s->profile_file_str) {
        av_log(ctx, AV_LOG_ERROR, "Profile output file must be set.\n");
        return AVERROR(EINVAL);
    }

    s->profile_file = fopen(s->profile_file_str, "w");
    if (!s->profile_file) {
        int err = AVERROR(errno);
        char buf[128];
        av_strerror(err, buf, sizeof(buf));
        av_log(ctx, AV_LOG_ERROR, "Could not open profile file %s: %s\n",
               s->profile_file_str, buf);
        return err;
    }

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterChannelLayouts *layouts;
    static const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE
    };

    layouts = ff_all_channel_layouts();
    if (!layouts)
        return AVERROR(ENOMEM);
    ff_set_common_channel_layouts(ctx, layouts);

    formats = ff_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    ff_set_common_formats(ctx, formats);

    formats = ff_all_samplerates();
    if (!formats)
        return AVERROR(ENOMEM);
    ff_set_common_samplerates(ctx, formats);

    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    NoiseProfileContext *s = ctx->priv;

    s->channels = inlink->channels;
    s->chandata = av_calloc(s->channels, sizeof(*(s->chandata)));
    if (!s->chandata)
        return AVERROR(ENOMEM);

    av_fft_end(s->fft);
    s->fft = av_fft_init(11, 0);
    if (!s->fft) {
        av_log(ctx, AV_LOG_ERROR, "Unable to create FFT context.\n");
        return AVERROR(ENOMEM);
    }

    av_freep(&s->fft_data);
    s->fft_data = av_calloc(WINSIZE, sizeof(*s->fft_data));
    if (!s->fft_data)
        return AVERROR(ENOMEM);

    inlink->min_samples =
    inlink->max_samples =
    inlink->partial_buf_size = WINSIZE;

    return 0;
}

static void power_spectrum(NoiseProfileContext *s, const float *in)
{
    float *out = s->out;
    int i;

    for (i = 0; i < WINSIZE; i++) {
        s->fft_data[i].re = in[i];
        s->fft_data[i].im = 0;
    }

#define RE(x) s->fft_data[(x)].re
#define IM(x) s->fft_data[(x)].im
#define S(a, b) ((a) * (a) + (b) * (b))

    av_fft_permute(s->fft, s->fft_data);
    av_fft_calc(s->fft, s->fft_data);
    out[0] = S(RE(0), 0);

    for (i = 1; i < NB_FREQ; i++)
        out[i] = S(RE(i), IM(i));
}

static void collect_data(NoiseProfileContext *s, ChannelData *chan, const float *samples)
{
    float *out = s->out;
    int i;

    power_spectrum(s, samples);

    for (i = 0; i < NB_FREQ; i++) {
        if (out[i] > 0) {
            float value = log(out[i]);

            chan->sum[i] += value;
            chan->profilecount[i]++;
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    NoiseProfileContext *s = ctx->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    int i;

    for (i = 0; i < s->channels; i++) {
        ChannelData *chan = &s->chandata[i];
        const float *samples = (const float *)in->extended_data[i];

        collect_data(s, chan, samples);
    }

    return ff_filter_frame(outlink, in);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    NoiseProfileContext *s = ctx->priv;
    int i, j;

    if (s->profile_file) {
        for (i = 0; i < s->channels; i++) {
            ChannelData *chan = &s->chandata[i];

            fprintf(s->profile_file, "Channel %u: ", i);

            for (j = 0; j < NB_FREQ; j++) {
                double r = chan->profilecount[j] != 0 ? chan->sum[j] / chan->profilecount[j] : 0;
                fprintf(s->profile_file, "%s%f", j == 0 ? "" : ", ", r);
            }
            fprintf(s->profile_file, "\n");
        }
        fclose(s->profile_file);
    }

    av_fft_end(s->fft);
    av_freep(&s->fft_data);
    av_freep(&s->chandata);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
        .config_props = config_input,
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

AVFilter ff_af_anoiseprofile = {
    .name          = "anoiseprofile",
    .description   = NULL_IF_CONFIG_SMALL("Calculate a profile of the audio for use in noise reduction."),
    .priv_size     = sizeof(NoiseProfileContext),
    .priv_class    = &anoiseprofile_class,
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = inputs,
    .outputs       = outputs,
};
