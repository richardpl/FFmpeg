/*
 * Copyright (c) 2017 Ronald S. Bultje <rsbultje@gmail.com>
 * Copyright (c) 2017 Ashish Pratap Singh <ashk43712@gmail.com>
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
 * Calculate Anti-Noise Singnal to Noise Ratio (ANSNR) between two input videos.
 */

#include <inttypes.h>
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "dualinput.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "ansnr.h"
#include "video.h"

typedef struct ANSNRContext {
    const AVClass *class;
    FFDualInputContext dinput;
    int width;
    int height;
    char *format;
    float *data_buf;
    double ansnr_sum;
    uint64_t nb_frames;
} ANSNRContext;

#define OFFSET(x) offsetof(ANSNRContext, x)
#define MAX_ALIGN 32
#define ALIGN_CEIL(x) ((x) + ((x) % MAX_ALIGN ? MAX_ALIGN - (x) % MAX_ALIGN : 0))
#define OPT_RANGE_PIXEL_OFFSET (-128)

const int ansnr_filter2d_ref_width = 3;
const int ansnr_filter2d_dis_width = 5;
const float ansnr_filter2d_ref[3 * 3] = {
    1.0 / 16.0, 2.0 / 16.0, 1.0 / 16.0,
    2.0 / 16.0, 4.0 / 16.0, 2.0 / 16.0,
    1.0 / 16.0, 2.0 / 16.0, 1.0 / 16.0
};
const float ansnr_filter2d_dis[5 * 5] = {
     2.0 / 571.0,  7.0 / 571.0,  12.0 / 571.0,  7.0 / 571.0,  2.0 / 571.0,
     7.0 / 571.0, 31.0 / 571.0,  52.0 / 571.0, 31.0 / 571.0,  7.0 / 571.0,
    12.0 / 571.0, 52.0 / 571.0, 127.0 / 571.0, 52.0 / 571.0, 12.0 / 571.0,
     7.0 / 571.0, 31.0 / 571.0,  52.0 / 571.0, 31.0 / 571.0,  7.0 / 571.0,
     2.0 / 571.0,  7.0 / 571.0,  12.0 / 571.0,  7.0 / 571.0,  2.0 / 571.0
};

static const AVOption ansnr_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(ansnr);

static inline double get_ansnr_avg(double ansnr_sum, uint64_t nb_frames)
{
    return ansnr_sum / nb_frames;
}

static inline float pow_2(float base)
{
    return base*base;
}

static void ansnr_mse(float *ref, float *dis, float *signal, float *noise,
                      int w, int h, int ref_stride, int dis_stride)
{
    int i, j;

    int ref_ind;
    int dis_ind;

    float signal_sum = 0;
    float noise_sum = 0;

    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            ref_ind = i * ref_stride + j;
            dis_ind = i * dis_stride + j;

            signal_sum += pow_2(ref[ref_ind]);
            noise_sum += pow_2(ref[ref_ind] - dis[dis_ind]);
        }
    }

    if (signal) {
        *signal = signal_sum;
    }
    if (noise) {
        *noise = noise_sum;
    }
}

static void ansnr_filter2d(const float *filt, const void *src, float *dst,
                           int w, int h, int src_stride, int dst_stride,
                           int filt_width, ANSNRContext *s)
{
    uint8_t type;
    uint8_t sz;

    uint8_t *src_8bit = (uint8_t *) src;
    uint16_t *src_10bit = (uint16_t *) src;

    int src_px_stride;

    float filt_coeff, img_coeff;
    int i, j, filt_i, filt_j, src_i, src_j;

    if (!strcmp(s->format, "yuv420p") || !strcmp(s->format, "yuv422p") ||
        !strcmp(s->format, "yuv444p")) {
        type = 8;
        sz = sizeof(uint8_t);
    }
    else if (!strcmp(s->format, "yuv420p10le") || !strcmp(s->format,
             "yuv422p10le") || !strcmp(s->format, "yuv444p10le")) {
        type = 10;
        sz = sizeof(uint16_t);
    }

    src_px_stride = src_stride / sizeof(sz);

    for (i = 0; i < h; ++i) {
        for (j = 0; j < w; ++j) {
            float accum = 0;
            for (filt_i = 0; filt_i < filt_width; filt_i++) {
                for (filt_j = 0; filt_j < filt_width; filt_j++) {
                    filt_coeff = filt[filt_i * filt_width + filt_j];

                    src_i = i - filt_width / 2 + filt_i;
                    src_j = j - filt_width / 2 + filt_j;

                    src_i = FFABS(src_i);
                    if (src_i >= h) {
                        src_i = 2 * h - src_i - 1;
                    }
                    src_j = FFABS(src_j);
                    if (src_j >= w) {
                        src_j = 2 * w - src_j - 1;
                    }

                    if (type == 8) {
                        img_coeff = src_8bit[src_i * src_px_stride + src_j] +
                            OPT_RANGE_PIXEL_OFFSET;
                    } else {
                        img_coeff = src_10bit[src_i * src_px_stride + src_j] +
                            OPT_RANGE_PIXEL_OFFSET;
                    }

                    accum += filt_coeff * img_coeff;
                }
            }
            dst[i * dst_stride + j] = accum;
        }
    }
}

static int compute_ansnr(const void *ref, const void *dis, int w, int h,
                         int ref_stride, int dis_stride, double *score,
                         double *score_psnr, double peak, double psnr_max,
                         void *ctx)
{
    ANSNRContext *s = (ANSNRContext *) ctx;

    char *data_top;

    float *ref_filt;
    float *dis_filt;

    float signal, noise;

    int buf_stride = ALIGN_CEIL(w * sizeof(float));
    size_t buf_sz = (size_t) (buf_stride * h);

    double eps = 1e-10;

    data_top = (float *) (s->data_buf);

    ref_filt = (float *) data_top;
    data_top += buf_sz;
    dis_filt = (float *) data_top;
    data_top += buf_sz;

    buf_stride = buf_stride / sizeof(float);

    ansnr_filter2d(ansnr_filter2d_ref, ref, ref_filt, w, h, ref_stride,
                   buf_stride, ansnr_filter2d_ref_width, s);
    ansnr_filter2d(ansnr_filter2d_dis, dis, dis_filt, w, h, dis_stride,
                   buf_stride, ansnr_filter2d_dis_width, s);

    ansnr_mse(ref_filt, dis_filt, &signal, &noise, w, h, buf_stride,
              buf_stride);

    *score = (noise==0) ? (psnr_max) : (10.0 * log10(signal / noise));

    *score_psnr = FFMIN(10.0 * log10(pow_2(peak) * w * h / FFMAX(noise, eps)),
                        psnr_max);

    return 0;
}

static AVFrame *do_ansnr(AVFilterContext *ctx, AVFrame *main, const AVFrame *ref)
{
    ANSNRContext *s = ctx->priv;

    char *format = s->format;

    double score = 0.0;
    double score_psnr = 0.0;

    int w = s->width;
    int h = s->height;

    double stride;

    double max_psnr;
    double peak;

    uint8_t sz;

    if (!strcmp(format, "yuv420p") || !strcmp(format, "yuv422p") ||
        !strcmp(format, "yuv444p")) {
        peak = 255.0;
        max_psnr = 60.0;
        sz = sizeof(uint8_t);
    }
    else if (!strcmp(format, "yuv420p10le") || !strcmp(format, "yuv422p10le") ||
             !strcmp(format, "yuv444p10le")) {
        peak = 255.75;
        max_psnr = 72.0;
        sz = sizeof(uint16_t);
    }

    stride = ALIGN_CEIL(w * sz);

    compute_ansnr(ref->data[0], main->data[0], w, h, stride, stride, &score,
                  &score_psnr, peak, max_psnr, s);

    s->nb_frames++;

    s->ansnr_sum += score;

    return main;
}

static av_cold int init(AVFilterContext *ctx)
{
    ANSNRContext *s = ctx->priv;

    s->dinput.process = do_ansnr;

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV420P10LE,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_input_ref(AVFilterLink *inlink)
{
    AVFilterContext *ctx  = inlink->dst;
    ANSNRContext *s = ctx->priv;
    int buf_stride;
    size_t buf_sz;

    if (ctx->inputs[0]->w != ctx->inputs[1]->w ||
        ctx->inputs[0]->h != ctx->inputs[1]->h) {
        av_log(ctx, AV_LOG_ERROR, "Width and height of input videos must be same.\n");
        return AVERROR(EINVAL);
    }
    if (ctx->inputs[0]->format != ctx->inputs[1]->format) {
        av_log(ctx, AV_LOG_ERROR, "Inputs must be of same pixel format.\n");
        return AVERROR(EINVAL);
    }

    s->width = ctx->inputs[0]->w;
    s->height = ctx->inputs[0]->h;
    s->format = av_get_pix_fmt_name(ctx->inputs[0]->format);

    buf_stride = ALIGN_CEIL(s->width * sizeof(float));
    buf_sz = (size_t)buf_stride * s->height;

    if (SIZE_MAX / buf_sz < 3) {
        av_log(ctx, AV_LOG_ERROR, "insufficient size.\n");
        return AVERROR(EINVAL);
    }

    if (!(s->data_buf = av_malloc(buf_sz * 3))) {
        av_log(ctx, AV_LOG_ERROR, "data_buf allocation failed.\n");
        return AVERROR(EINVAL);
    }

    return 0;
}


static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ANSNRContext *s = ctx->priv;
    AVFilterLink *mainlink = ctx->inputs[0];
    int ret;

    outlink->w = mainlink->w;
    outlink->h = mainlink->h;
    outlink->time_base = mainlink->time_base;
    outlink->sample_aspect_ratio = mainlink->sample_aspect_ratio;
    outlink->frame_rate = mainlink->frame_rate;
    if ((ret = ff_dualinput_init(ctx, &s->dinput)) < 0)
        return ret;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *inpicref)
{
    ANSNRContext *s = inlink->dst->priv;
    return ff_dualinput_filter_frame(&s->dinput, inlink, inpicref);
}

static int request_frame(AVFilterLink *outlink)
{
    ANSNRContext *s = outlink->src->priv;
    return ff_dualinput_request_frame(&s->dinput, outlink);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ANSNRContext *s = ctx->priv;

    ff_dualinput_uninit(&s->dinput);

    av_free(s->data_buf);

    av_log(ctx, AV_LOG_INFO, "ANSNR AVG: %.3f\n", get_ansnr_avg(s->ansnr_sum,
                                                                s->nb_frames));
}

static const AVFilterPad ansnr_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },{
        .name         = "reference",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input_ref,
    },
    { NULL }
};

static const AVFilterPad ansnr_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_ansnr = {
    .name          = "ansnr",
    .description   = NULL_IF_CONFIG_SMALL("Calculate the ANSNR between two video streams."),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(ANSNRContext),
    .priv_class    = &ansnr_class,
    .inputs        = ansnr_inputs,
    .outputs       = ansnr_outputs,
};
