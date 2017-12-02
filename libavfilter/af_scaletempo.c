/*
 * scaletempo audio filter
 *
 * scale tempo while maintaining pitch
 * (WSOLA technique with cross correlation)
 * inspired by SoundTouch library by Olli Parviainen
 *
 * basic algorithm
 *   - produce 'stride' output samples per loop
 *   - consume stride*scale input samples per loop
 *
 * to produce smoother transitions between strides, blend next overlap
 * samples from last stride with correlated samples of current input
 *
 * Copyright (c) 2007 Robert Juliano
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"

#include "avfilter.h"
#include "audio.h"
#include "internal.h"

typedef struct ScaleTempoContext
{
    AVClass *class;

    // stride
    float scale;
    float speed;
    int frames_stride;
    float frames_stride_scaled;
    float frames_stride_error;
    int bytes_per_frame;
    int bytes_stride;
    int bytes_queue;
    int bytes_queued;
    int bytes_to_slide;
    int8_t *buf_queue;
    // overlap
    int samples_overlap;
    int samples_standing;
    int bytes_overlap;
    int bytes_standing;
    void *buf_overlap;
    void *table_blend;
    void (*output_overlap)(struct ScaleTempoContext *s, void *out_buf,
                           int bytes_off);
    // best overlap
    int frames_search;
    int num_channels;
    void *buf_pre_corr;
    void *table_window;
    int (*best_overlap_offset)(struct ScaleTempoContext *s);
    // command line
    float scale_nominal;
    float ms_stride;
    float percent_overlap;
    float ms_search;
#define SCALE_TEMPO 1
#define SCALE_PITCH 2
    int speed_opt;

    int64_t pts;
} ScaleTempoContext;

static int query_formats(AVFilterContext *ctx)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats        *formats = NULL;
    static const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_S16,
        AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_NONE
    };
    int ret;

    layouts = ff_all_channel_counts();
    if (!layouts) {
        return AVERROR(ENOMEM);
    }
    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        return ret;

    formats = ff_make_format_list(sample_fmts);
    if (!formats) {
        return AVERROR(ENOMEM);
    }
    ret = ff_set_common_formats(ctx, formats);
    if (ret < 0)
        return ret;

    formats = ff_all_samplerates();
    if (!formats) {
        return AVERROR(ENOMEM);
    }
    return ff_set_common_samplerates(ctx, formats);
}

static int fill_queue(AVFilterContext *ctx, AVFrame *in, int offset)
{
    ScaleTempoContext *s = ctx->priv;
    int bytes_in = in->nb_samples * s->bytes_per_frame - offset;
    int offset_unchanged = offset;

    if (s->bytes_to_slide > 0) {
        if (s->bytes_to_slide < s->bytes_queued) {
            int bytes_move = s->bytes_queued - s->bytes_to_slide;

            memmove(s->buf_queue, s->buf_queue + s->bytes_to_slide, bytes_move);
            s->bytes_to_slide = 0;
            s->bytes_queued = bytes_move;
        } else {
            int bytes_skip;

            s->bytes_to_slide -= s->bytes_queued;
            bytes_skip = FFMIN(s->bytes_to_slide, bytes_in);
            s->bytes_queued = 0;
            s->bytes_to_slide -= bytes_skip;
            offset += bytes_skip;
            bytes_in -= bytes_skip;
        }
    }

    if (bytes_in > 0) {
        int bytes_copy = FFMIN(s->bytes_queue - s->bytes_queued, bytes_in);

        memcpy(s->buf_queue + s->bytes_queued, in->data[0] + offset, bytes_copy);
        s->bytes_queued += bytes_copy;
        offset += bytes_copy;
    }

    return offset - offset_unchanged;
}

#define UNROLL_PADDING (4 * 4)

static int best_overlap_offset_float(ScaleTempoContext *s)
{
    float best_corr = INT_MIN;
    int i, off, best_off = 0;
    float *ppc;
    float *pw  = s->table_window;
    float *po  = s->buf_overlap;
    float *search_start;

    po += s->num_channels;
    ppc = s->buf_pre_corr;
    for (i = s->num_channels; i < s->samples_overlap; i++)
        *ppc++ = *pw++ **po++;

    search_start = (float *)s->buf_queue + s->num_channels;
    for (off = 0; off < s->frames_search; off++) {
        float corr = 0;
        float *ps = search_start;
        ppc = s->buf_pre_corr;
        for (i = s->num_channels; i < s->samples_overlap; i++)
            corr += *ppc++ **ps++;
        if (corr > best_corr) {
            best_corr = corr;
            best_off  = off;
        }
        search_start += s->num_channels;
    }

    return best_off * 4 * s->num_channels;
}

static int best_overlap_offset_s16(ScaleTempoContext *s)
{
    int64_t best_corr = INT64_MIN;
    int i, off, best_off = 0;
    int32_t *ppc;
    int16_t *search_start;

    int32_t *pw  = s->table_window;
    int16_t *po  = s->buf_overlap;
    po += s->num_channels;
    ppc = s->buf_pre_corr;
    for (i = s->num_channels; i < s->samples_overlap; i++)
        *ppc++ = (*pw++ **po++) >> 15;

    search_start = (int16_t *)s->buf_queue + s->num_channels;
    for (off = 0; off < s->frames_search; off++) {
        int64_t corr = 0;
        int16_t *ps = search_start;
        long i;

        ppc = s->buf_pre_corr;
        ppc += s->samples_overlap - s->num_channels;
        ps  += s->samples_overlap - s->num_channels;
        i  = -(s->samples_overlap - s->num_channels);
        do {
            corr += ppc[i + 0] * ps[i + 0];
            corr += ppc[i + 1] * ps[i + 1];
            corr += ppc[i + 2] * ps[i + 2];
            corr += ppc[i + 3] * ps[i + 3];
            i += 4;
        } while (i < 0);
        if (corr > best_corr) {
            best_corr = corr;
            best_off  = off;
        }
        search_start += s->num_channels;
    }

    return best_off * 2 * s->num_channels;
}

static void output_overlap_float(ScaleTempoContext *s, void *buf_out,
                                 int bytes_off)
{
    float *pout = buf_out;
    float *pb   = s->table_blend;
    float *po   = s->buf_overlap;
    float *pin  = (float *)(s->buf_queue + bytes_off);
    int i;

    for (i = 0; i < s->samples_overlap; i++) {
        *pout++ = *po - *pb++ *(*po - *pin++);
        po++;
    }
}

static void output_overlap_s16(ScaleTempoContext *s, void *buf_out,
                               int bytes_off)
{
    int16_t *pout = buf_out;
    int32_t *pb   = s->table_blend;
    int16_t *po   = s->buf_overlap;
    int16_t *pin  = (int16_t *)(s->buf_queue + bytes_off);
    int i;

    for (i = 0; i < s->samples_overlap; i++) {
        *pout++ = *po - ((*pb++ *(*po - *pin++)) >> 16);
        po++;
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext  *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    ScaleTempoContext *s  = ctx->priv;
    int offset_in, in_samples, ret = 0;
    uint8_t *pout;
    AVFrame *out;

    if (s->scale == 1.0) {
        return ff_filter_frame(outlink, in);
    }

    if (s->pts == AV_NOPTS_VALUE)
        s->pts = in->pts;

    in_samples = in->nb_samples;
    out = ff_get_audio_buffer(outlink, ((int)(in_samples / s->frames_stride_scaled) + 1) * s->frames_stride);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    offset_in = fill_queue(ctx, in, 0);
    pout = out->data[0];
    while (s->bytes_queued >= s->bytes_queue) {
        int bytes_off = 0, ti;
        float tf;

        // output stride
        if (s->output_overlap) {
            if (s->best_overlap_offset)
                bytes_off = s->best_overlap_offset(s);
            s->output_overlap(s, pout, bytes_off);
        }
        memcpy(pout + s->bytes_overlap,
               s->buf_queue + bytes_off + s->bytes_overlap,
               s->bytes_standing);
        pout += s->bytes_stride;

        // input stride
        memcpy(s->buf_overlap,
               s->buf_queue + bytes_off + s->bytes_stride,
               s->bytes_overlap);
        tf = s->frames_stride_scaled + s->frames_stride_error;
        ti = (int)tf;
        s->frames_stride_error = tf - ti;
        s->bytes_to_slide = ti * s->bytes_per_frame;

        offset_in += fill_queue(ctx, in, offset_in);
    }

    out->nb_samples = (pout - (uint8_t *)out->data[0]) / (s->bytes_per_frame);
    av_frame_free(&in);
    if (out->nb_samples) {
        out->pts = s->pts;
        s->pts += av_rescale_q(out->nb_samples,
                               (AVRational){1, outlink->sample_rate},
                               outlink->time_base);

        ret = ff_filter_frame(outlink, out);
    } else {
        av_frame_free(&out);
    }
    return ret;
}

static void update_speed(AVFilterContext *ctx, float speed)
{
    ScaleTempoContext *s = ctx->priv;
    double factor;

    s->speed = speed;

    factor = (s->speed_opt & SCALE_PITCH) ? 1.0 / s->speed : s->speed;
    s->scale = factor * s->scale_nominal;

    s->frames_stride_scaled = s->scale * s->frames_stride;
    s->frames_stride_error = FFMIN(s->frames_stride_error, s->frames_stride_scaled);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ScaleTempoContext *s = ctx->priv;
    float srate = inlink->sample_rate / 1000.0;
    int frames_overlap, nch = inlink->channels;
    int bps, use_int = 0;

    if (inlink->format == AV_SAMPLE_FMT_S16) {
        use_int = 1;
        bps = 2;
    } else {
        bps = 4;
    }

    s->frames_stride = srate * s->ms_stride;
    s->bytes_stride  = s->frames_stride * bps * nch;
    s->speed = 1.f;
    s->pts = AV_NOPTS_VALUE;

    update_speed(ctx, s->speed);

    frames_overlap = s->frames_stride * s->percent_overlap;
    if (frames_overlap <= 0) {
        s->bytes_standing   = s->bytes_stride;
        s->samples_standing = s->bytes_standing / bps;
        s->output_overlap   = NULL;
        s->bytes_overlap    = 0;
    } else {
        s->samples_overlap  = frames_overlap * nch;
        s->bytes_overlap    = frames_overlap * nch * bps;
        s->bytes_standing   = s->bytes_stride - s->bytes_overlap;
        s->samples_standing = s->bytes_standing / bps;
        s->buf_overlap      = av_realloc(s->buf_overlap, s->bytes_overlap);
        s->table_blend      = av_realloc(s->table_blend, s->bytes_overlap * 4);
        if (!s->buf_overlap || !s->table_blend) {
            return AVERROR(ENOMEM);
        }
        memset(s->buf_overlap, 0, s->bytes_overlap);
        if (use_int) {
            int32_t *pb = s->table_blend;
            int64_t blend = 0;
            int i, j;

            for (i = 0; i < frames_overlap; i++) {
                int32_t v = blend / frames_overlap;
                for (j = 0; j < nch; j++)
                    *pb++ = v;
                blend += 65536; // 2^16
            }
            s->output_overlap = output_overlap_s16;
        } else {
            float *pb = s->table_blend;
            int i;

            for (i = 0; i < frames_overlap; i++) {
                float v = i / (float)frames_overlap;
                int j;

                for (j = 0; j < nch; j++)
                    *pb++ = v;
            }
            s->output_overlap = output_overlap_float;
        }
    }

    s->frames_search = (frames_overlap > 1) ? srate * s->ms_search : 0;
    if (s->frames_search <= 0) {
        s->best_overlap_offset = NULL;
    } else {
        if (use_int) {
            int64_t t = frames_overlap;
            int32_t n = 8589934588LL / (t * t); // 4 * (2^31 - 1) / t^2
            int32_t *pw;
            int i, j;

            s->buf_pre_corr = av_realloc(s->buf_pre_corr,
                                         s->bytes_overlap * 2 + UNROLL_PADDING);
            s->table_window = av_realloc(s->table_window,
                                         s->bytes_overlap * 2 - nch * bps * 2);
            if (!s->buf_pre_corr || !s->table_window) {
                return AVERROR(ENOMEM);
            }
            memset((char *)s->buf_pre_corr + s->bytes_overlap * 2, 0,
                   UNROLL_PADDING);
            pw = s->table_window;
            for (i = 1; i < frames_overlap; i++) {
                int32_t v = (i * (t - i) * n) >> 15;
                for (j = 0; j < nch; j++)
                    *pw++ = v;
            }
            s->best_overlap_offset = best_overlap_offset_s16;
        } else {
            float *pw;
            int i, j;

            s->buf_pre_corr = av_realloc(s->buf_pre_corr, s->bytes_overlap);
            s->table_window = av_realloc(s->table_window,
                                         s->bytes_overlap - nch * bps);
            if (!s->buf_pre_corr || !s->table_window) {
                return AVERROR(ENOMEM);
            }
            pw = s->table_window;
            for (i = 1; i < frames_overlap; i++) {
                float v = i * (frames_overlap - i);
                for (j = 0; j < nch; j++)
                    *pw++ = v;
            }
            s->best_overlap_offset = best_overlap_offset_float;
        }
    }

    s->bytes_per_frame = bps * nch;
    s->num_channels    = nch;

    s->bytes_queue = (s->frames_search + s->frames_stride + frames_overlap)
                     * bps * nch;
    s->buf_queue = av_realloc(s->buf_queue, s->bytes_queue + UNROLL_PADDING);
    if (!s->buf_queue) {
        return AVERROR(ENOMEM);
    }

    s->bytes_queued = 0;
    s->bytes_to_slide = 0;

    return 0;
}

static void uninit(AVFilterContext *ctx)
{
    ScaleTempoContext *s = ctx->priv;

    av_freep(&s->buf_queue);
    av_freep(&s->buf_overlap);
    av_freep(&s->buf_pre_corr);
    av_freep(&s->table_blend);
    av_freep(&s->table_window);
}

#define OFFSET(x) offsetof(ScaleTempoContext, x)
#define AF        AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption scaletempo_options[] = {
    { "scale", "set nominal amount to scale tempo", OFFSET(scale_nominal), AV_OPT_TYPE_FLOAT, {.dbl=1.0}, 0.01, 10, AF },
    { "stride", "set length in ms to output each stride", OFFSET(ms_stride), AV_OPT_TYPE_FLOAT, {.dbl= 60}, 0.01, 1000, AF },
    { "overlap", "set percentage of stride to overlap", OFFSET(percent_overlap), AV_OPT_TYPE_FLOAT, {.dbl=.2}, 0, 1, AF },
    { "search", "set length in ms to search for best overlap position", OFFSET(ms_search), AV_OPT_TYPE_FLOAT, {.dbl=14}, 0.01, 1000, AF },
    { "speed", "set response to tempo change", OFFSET(speed_opt), AV_OPT_TYPE_INT, {.i64=1}, 0, 3, AF, "speed" },
    { "none",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, AF, "speed" },
    { "tempo",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, AF, "speed" },
    { "pitch",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2}, 0, 0, AF, "speed" },
    { "both",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 3}, 0, 0, AF, "speed" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(scaletempo);

static const AVFilterPad scaletempo_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad scaletempo_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_scaletempo = {
    .name            = "scaletempo",
    .description     = NULL_IF_CONFIG_SMALL("Scale audio tempo while maintaining pitch."),
    .uninit          = uninit,
    .query_formats   = query_formats,
    .priv_size       = sizeof(ScaleTempoContext),
    .priv_class      = &scaletempo_class,
    .inputs          = scaletempo_inputs,
    .outputs         = scaletempo_outputs,
};
