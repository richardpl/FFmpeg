/*
 * Speech Normalizer
 * Copyright (c) 2020 Paul B Mahol
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

/**
 * @file
 * Speech Normalizer
 */

#include <float.h>

#include "libavutil/avassert.h"
#include "libavutil/opt.h"

#define FF_BUFQUEUE_SIZE (1024)
#include "bufferqueue.h"

#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "internal.h"

#define MAX_ITEMS  882000

typedef struct PeriodItem {
    int size;
    int type;
    double max_peak;
} PeriodItem;

typedef struct SpeechNormalizerContext {
    const AVClass *class;

    double peak_value;
    double max_amplification;
    double threshold_value;
    double feedback;
    double decay;
    int channels;

    int max_period;
    int eof;
    int64_t pts;
    int state[12];

    PeriodItem pi[12][MAX_ITEMS];
    double gain_state[12];
    int pi_start[12];
    int pi_end[12];

    struct FFBufQueue queue;
} SpeechNormalizerContext;

#define OFFSET(x) offsetof(SpeechNormalizerContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption speechnorm_options[] = {
    { "peak",        "set the peak value",          OFFSET(peak_value),          AV_OPT_TYPE_DOUBLE, {.dbl = 0.95},  0.0,     1.0, FLAGS },
    { "p",           "set the peak value",          OFFSET(peak_value),          AV_OPT_TYPE_DOUBLE, {.dbl = 0.95},  0.0,     1.0, FLAGS },
    { "maxgain",     "set the max amplification",   OFFSET(max_amplification),   AV_OPT_TYPE_DOUBLE, {.dbl = 2.0},   1.0,    10.0, FLAGS },
    { "m",           "set the max amplification",   OFFSET(max_amplification),   AV_OPT_TYPE_DOUBLE, {.dbl = 2.0},   1.0,    10.0, FLAGS },
    { "threshold",   "set the threshold value",     OFFSET(threshold_value),     AV_OPT_TYPE_DOUBLE, {.dbl = 0},     0.0,     1.0, FLAGS },
    { "t",           "set the threshold value",     OFFSET(threshold_value),     AV_OPT_TYPE_DOUBLE, {.dbl = 0},     0.0,     1.0, FLAGS },
    { "feedback",    "set the feedback value",      OFFSET(feedback),            AV_OPT_TYPE_DOUBLE, {.dbl = 0.001}, 0.0,     1.0, FLAGS },
    { "f",           "set the feedback value",      OFFSET(feedback),            AV_OPT_TYPE_DOUBLE, {.dbl = 0.001}, 0.0,     1.0, FLAGS },
    { "decay",       "set the decay value",         OFFSET(decay),               AV_OPT_TYPE_DOUBLE, {.dbl = 0.999}, 0.0,     1.0, FLAGS },
    { "d",           "set the decay value",         OFFSET(decay),               AV_OPT_TYPE_DOUBLE, {.dbl = 0.999}, 0.0,     1.0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(speechnorm);

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    AVFilterChannelLayouts *layouts;
    static const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_DBLP,
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
    SpeechNormalizerContext *s = ctx->priv;

    s->max_period = inlink->sample_rate / (2 * 20);
    s->channels = inlink->channels;
    for (int ch = 0; ch < s->channels; ch++)
        s->state[ch] = -1;

    return 0;
}

static int get_pi_samples(PeriodItem *pi, int start, int end, int mode)
{
    int sum;

    if (mode && pi[start].type == 0)
        return 0;

    sum = pi[start].size;
    av_assert0(sum > 0);
    while (start != end) {
        start++;
        if (start >= MAX_ITEMS)
            start = 0;
        if (mode && pi[start].type == 0)
            break;
        av_assert0(pi[start].size > 0);
        sum += pi[start].size;
        if (pi[start].type == 0)
            break;
    }

    return sum;
}

static int consume_pi(PeriodItem *pi, int start, int end, int nb_samples)
{
    int sum;

    sum = pi[start].size;
    av_assert0(pi[start].size > 0);
    while (sum < nb_samples) {
        av_assert0(pi[start].type == 1);
        av_assert0(start != end);
        start++;
        if (start >= MAX_ITEMS)
            start = 0;
        av_assert0(pi[start].size > 0);
        sum += pi[start].size;
    }

    av_assert0(pi[start].size >= sum - nb_samples);
    pi[start].size = sum - nb_samples;
    av_assert0(pi[start].size >= 0);
    if (pi[start].size == 0 && start != end) {
        start++;
        if (start >= MAX_ITEMS)
            start = 0;
    }

    return start;
}

static int get_queued_samples(SpeechNormalizerContext *s)
{
    int sum = 0;

    for (int i = 0; i < s->queue.available; i++) {
        AVFrame *frame = ff_bufqueue_peek(&s->queue, i);
        sum += frame->nb_samples;
    }

    return sum;
}

static int filter_frame(AVFilterContext *ctx)
{
    SpeechNormalizerContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterLink *inlink = ctx->inputs[0];
    int min_pi_nb_samples;
    AVFrame *in = NULL;
    int ret;

    for (int f = 0; f < ff_inlink_queued_frames(inlink); f++) {
        ret = ff_inlink_consume_frame(inlink, &in);
        if (ret < 0)
            return ret;
        if (ret == 0)
            break;

        ff_bufqueue_add(ctx, &s->queue, in);

        for (int ch = 0; ch < inlink->channels; ch++) {
            const double *src = (const double *)in->extended_data[ch];
            int n = 0;

            if (s->state[ch] < 0)
                s->state[ch] = src[0] >= 0.;

            while (n < in->nb_samples) {
                if (s->state[ch] != (src[n] >= 0.) || s->pi[ch][s->pi_end[ch]].size > s->max_period) {
                    s->state[ch] = src[n] >= 0.;
                    av_assert0(s->pi[ch][s->pi_end[ch]].size > 0);
                    s->pi[ch][s->pi_end[ch]].type = 1;
                    s->pi_end[ch]++;
                    if (s->pi_end[ch] >= MAX_ITEMS)
                        s->pi_end[ch] = 0;
                    s->pi[ch][s->pi_end[ch]].max_peak = DBL_MIN;
                    s->pi[ch][s->pi_end[ch]].type = 0;
                    s->pi[ch][s->pi_end[ch]].size = 0;
                    av_assert0(s->pi_end[ch] != s->pi_start[ch]);
                }

                if (src[n] >= 0.) {
                    while (src[n] >= 0.) {
                        s->pi[ch][s->pi_end[ch]].max_peak = FFMAX(s->pi[ch][s->pi_end[ch]].max_peak, FFABS(src[n]));
                        s->pi[ch][s->pi_end[ch]].size++;
                        n++;
                        if (n >= in->nb_samples)
                            break;
                    }
                } else {
                    while (src[n] < 0.) {
                        s->pi[ch][s->pi_end[ch]].max_peak = FFMAX(s->pi[ch][s->pi_end[ch]].max_peak, FFABS(src[n]));
                        s->pi[ch][s->pi_end[ch]].size++;
                        n++;
                        if (n >= in->nb_samples)
                            break;
                    }
                }
            }
        }
    }

    if (s->queue.available > 0) {
        in = ff_bufqueue_peek(&s->queue, 0);
        if (!in)
            return 1;
    } else {
        return 1;
    }

    min_pi_nb_samples = get_pi_samples(s->pi[0], s->pi_start[0], s->pi_end[0], 1);
    for (int ch = 1; ch < inlink->channels; ch++) {
        min_pi_nb_samples = FFMIN(min_pi_nb_samples, get_pi_samples(s->pi[ch], s->pi_start[ch], s->pi_end[ch], 1));
    }

    if (min_pi_nb_samples >= in->nb_samples) {
        int nb_samples = get_queued_samples(s);

        in = ff_bufqueue_get(&s->queue);

        av_frame_make_writable(in);

        nb_samples -= in->nb_samples;

        for (int ch = 0; ch < inlink->channels; ch++) {
            double *src = (double *)in->extended_data[ch];
            int start = s->pi_start[ch];
            int offset = 0;
            double gain;

            for (int n = 0; n < in->nb_samples; n++) {
                if (n >= offset) {
                    if (s->pi[ch][start].max_peak > s->threshold_value)
                        gain = FFMIN(s->max_amplification, s->peak_value / s->pi[ch][start].max_peak);
                    else
                        gain = 1.;
                    av_assert0(s->pi[ch][start].size > 0);
                    offset += s->pi[ch][start++].size;
                    if (start >= MAX_ITEMS)
                        start = 0;
                }
                s->gain_state[ch] = FFMIN(gain, gain * s->feedback + s->gain_state[ch] * s->decay);
                src[n] *= s->gain_state[ch];
            }
        }

        for (int ch = 0; ch < inlink->channels; ch++) {
            s->pi_start[ch] = consume_pi(s->pi[ch], s->pi_start[ch], s->pi_end[ch], in->nb_samples);
        }

        for (int ch = 0; ch < inlink->channels; ch++) {
            int pi_nb_samples = get_pi_samples(s->pi[ch], s->pi_start[ch], s->pi_end[ch], 0);

            if (nb_samples != pi_nb_samples) {
                av_assert0(0);
            }
        }

        return ff_filter_frame(outlink, in);
    }

    return 1;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    SpeechNormalizerContext *s = ctx->priv;
    int ret = 0, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    ret = filter_frame(ctx);
    if (ret <= 0)
        return ret;

    if (!s->eof && ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        if (status == AVERROR_EOF)
            s->eof = 1;
    }

    if (s->eof && ff_inlink_queued_samples(inlink) == 0) {
        ff_outlink_set_status(outlink, AVERROR_EOF, s->pts);
        return 0;
    }

    if (!s->eof)
        FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static av_cold void uninit(AVFilterContext *ctx)
{
}

static const AVFilterPad avfilter_af_speechnorm_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_AUDIO,
        .config_props   = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_af_speechnorm_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_speechnorm = {
    .name          = "speechnorm",
    .description   = NULL_IF_CONFIG_SMALL("Speech Normalizer."),
    .query_formats = query_formats,
    .priv_size     = sizeof(SpeechNormalizerContext),
    .priv_class    = &speechnorm_class,
    .activate      = activate,
    .uninit        = uninit,
    .inputs        = avfilter_af_speechnorm_inputs,
    .outputs       = avfilter_af_speechnorm_outputs,
    .process_command = ff_filter_process_command,
};
