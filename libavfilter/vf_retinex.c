/*
 * Copyright (c) 2014 mawen1250
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FFmpeg.  If not, see <http://www.gnu.org/licenses/>.
 */

/* TODO: hightbit depth support */

#include <float.h>

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct RetinexContext {
    const AVClass *class;

    int mode;
    float lower_thr;
    float upper_thr;
    int full;
    float chroma_protect;
    float restore;

    float *in_data[3];
    float *out_data[3];
    float *gauss;

    void (*retinex)(struct RetinexContext *s, AVFrame *in, AVFrame *out);
} RetinexContext;

#define OFFSET(x) offsetof(RetinexContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define CONST(name, help, val, unit) { name, help, 0, AV_OPT_TYPE_CONST, {.i64=val}, 0, 0, FLAGS, unit }

static const AVOption retinex_options[] = {
    { "mode",  "set filter mode",     OFFSET(mode), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "mode" },
    CONST("msrcp",  NULL, 0, "mode"),
    CONST("msrcr",  NULL, 1, "mode"),
    { "lower", "set lower threshold", OFFSET(lower_thr), AV_OPT_TYPE_FLOAT, {.dbl=0.001}, 0, 1, FLAGS },
    { "upper", "set upper threshold", OFFSET(upper_thr), AV_OPT_TYPE_FLOAT, {.dbl=0.001}, 0, 1, FLAGS },
    { "full",  "set full range",      OFFSET(full), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS },
    { "chroma_protect", "set chroma protect", OFFSET(chroma_protect), AV_OPT_TYPE_FLOAT, {.dbl=1.2}, 1, FLT_MAX, FLAGS },
    { "restore", "set restore", OFFSET(restore), AV_OPT_TYPE_FLOAT, {.dbl=125}, FLT_EPSILON, FLT_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(retinex);

static int query_formats(AVFilterContext *ctx)
{
    RetinexContext *s = ctx->priv;

    static const enum AVPixelFormat msrcp_fmts[] = {
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat msrcr_fmts[] = {
        AV_PIX_FMT_GBRP,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *formats = ff_make_format_list(s->mode ? msrcr_fmts : msrcp_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, formats);
}

static void recursive_gaussian_parameters(const double sigma, float *B, float *B1,
                                          float *B2, float *B3)
{
    const double q = sigma < 2.5 ? 3.97156 - 4.14554*sqrt(1 - 0.26891*sigma) : 0.98711*sigma - 0.96330;

    const double b0 = 1.57825 + 2.44413*q + 1.4281*q*q + 0.422205*q*q*q;
    const double b1 = 2.44413*q + 2.85619*q*q + 1.26661*q*q*q;
    const double b2 = -(1.4281*q*q + 1.26661*q*q*q);
    const double b3 = 0.422205*q*q*q;

    *B = 1. - (b1 + b2 + b3) / b0;
    *B1 = b1 / b0;
    *B2 = b2 / b0;
    *B3 = b3 / b0;
}

static void recursive_gaussian2d_horizontal(float *output, const float *input,
                                            int h, int w, int stride,
                                            const float B, const float B1,
                                            const float B2, const float B3)
{
    int i, j, lower, upper;
    float p0, p1, p2, p3;

    for (j = 0; j < h; j++) {
        lower = stride * j;
        upper = lower + w;

        i = lower;
        output[i] = p3 = p2 = p1 = input[i];

        for (i++; i < upper; i++) {
            p0 = B*input[i] + B1*p1 + B2*p2 + B3*p3;
            p3 = p2;
            p2 = p1;
            p1 = p0;
            output[i] = p0;
        }

        i--;
        p3 = p2 = p1 = output[i];

        for (i--; i >= lower; i--) {
            p0 = B*output[i] + B1*p1 + B2*p2 + B3*p3;
            p3 = p2;
            p2 = p1;
            p1 = p0;
            output[i] = p0;
        }
    }
}

static void recursive_gaussian2d_vertical(float *output, const float *input,
                                          int h, int w, int stride, const float B,
                                          const float B1, const float B2, const float B3)
{
    int i0, i1, i2, i3, j, lower, upper;
    float p0, p1, p2, p3;

    if (output != input)
        memcpy(output, input, sizeof(float) * w);

    for (j = 0; j < h; j++) {
        lower = stride * j;
        upper = lower + w;

        i0 = lower;
        i1 = j < 1 ? i0 : i0 - stride;
        i2 = j < 2 ? i1 : i1 - stride;
        i3 = j < 3 ? i2 : i2 - stride;

        for (; i0 < upper; i0++, i1++, i2++, i3++) {
            p3 = output[i3];
            p2 = output[i2];
            p1 = output[i1];
            p0 = input[i0];
            output[i0] = B*p0 + B1*p1 + B2*p2 + B3*p3;
        }
    }

    for (j = h - 1; j >= 0; j--) {
        lower = stride * j;
        upper = lower + w;

        i0 = lower;
        i1 = j >= h - 1 ? i0 : i0 + stride;
        i2 = j >= h - 2 ? i1 : i1 + stride;
        i3 = j >= h - 3 ? i2 : i2 + stride;

        for (; i0 < upper; i0++, i1++, i2++, i3++) {
            p3 = output[i3];
            p2 = output[i2];
            p1 = output[i1];
            p0 = output[i0];
            output[i0] = B*p0 + B1*p1 + B2*p2 + B3*p3;
        }
    }
}

static const uint8_t sigma[3] = { 20, 80, 250 };

static void msr_kernel(float *out_data, float *in_data, float *gauss, int w, int h)
{
    int x, y, s, upper;
    float *out, b, b1, b2, b3;

    for (y = 0; y < h; y++) {
        out = out_data + y * w;

        for (x = 0; x < w; x++)
            out[x] = 1;
    }

    for (s = 0; s < sizeof(sigma); s++) {
        recursive_gaussian_parameters(sigma[s], &b, &b1, &b2, &b3);
        recursive_gaussian2d_horizontal(gauss, in_data, h, w, w, b, b1, b2, b3);
        recursive_gaussian2d_vertical(gauss, gauss, h, w, w, b, b1, b2, b3);

        for (y = 0; y < h; y++) {
            x = y * w;
            for (upper = x + w; x < upper; x++)
                out_data[x] *= gauss[x] <= 0 ? 1 : in_data[x] / gauss[x] + 1;
        }
    }

    for (y = 0; y < h; y++) {
        x = y * w;
        for (upper = x + w; x < upper; x++)
            out_data[x] = log(out_data[x]) / sizeof(sigma);
    }

}

static void simplest_color_balance(RetinexContext *s, float *odata,
                                   const float *idata,
                                   int width, int height)
{
    int i, j, upper;
    float offset, gain;
    float min = FLT_MAX;
    float max = FLT_MIN;
    float Histogram[4097] = {0};

    for (j = 0; j < height; j++) {
        i = width * j;
        for (upper = i + width; i < upper; i++) {
            min = FFMIN(min, odata[i]);
            max = FFMAX(max, odata[i]);
        }
    }

    if (max <= min) {
        memcpy(odata, idata, sizeof(float)*width*height);
        return;
    }

    if (s->lower_thr > 0 || s->upper_thr > 0) {
        int h, HistBins = 4096;
        int Count, MaxCount;

        gain = (HistBins - 1.) / (max - min);
        offset = -min * gain;

        for (j = 0; j < height; j++) {
            i = width * j;
            for (upper = i + width; i < upper; i++) {
                Histogram[(int)(floor(odata[i] * gain + offset))]++;
            }
        }

        gain = (max - min) / (HistBins - 1);
        offset = min;

        Count = 0;
        MaxCount = (int)(width*height*s->lower_thr + 0.5);

        for (h = 0; h < HistBins; h++) {
            Count += Histogram[h];
            if (Count > MaxCount) break;
        }

        min = h * gain + offset;

        Count = 0;
        MaxCount = (int)(width*height*s->upper_thr + 0.5);

        for (h = HistBins - 1; h >= 0; h--) {
            Count += Histogram[h];
            if (Count > MaxCount) break;
        }

        max = h * gain + offset;
    }

    gain = 1. / (max - min);
    offset = -min * gain;

    if (s->lower_thr > 0 || s->upper_thr > 0) {
        for (j = 0; j < height; j++) {
            i = width * j;
            for (upper = i + width; i < upper; i++)
                odata[i] = av_clipf(odata[i] * gain + offset, 0, 1);
        }
    } else {
        for (j = 0; j < height; j++) {
            i = width * j;
            for (upper = i + width; i < upper; i++)
                odata[i] = odata[i] * gain + offset;
        }
    }
}

static void msrcp_gray(RetinexContext *s, AVFrame *in, AVFrame *out)
{
    float range = s->full ? 255 : 219;
    float gain = 1. / range;
    const int height = in->height;
    const int width = in->width;
    int x, y;

    if (s->full) {
        for (y = 0; y < height; y++) {
            float *dst = s->in_data[0] + y * width;
            const uint8_t *src = in->data[0] + y * in->linesize[0];

            for (x = 0; x < width; x++)
                dst[x] = src[x] * gain;
        }
    } else {
        float min = 255, max = 0, ceil = 255, floor = 0;

        for (y = 0; y < height; y++) {
            const uint8_t *src = in->data[0] + y * in->linesize[0];

            for (x = 0; x < width; x++) {
                min = FFMIN(min, src[x]);
                max = FFMAX(max, src[x]);
            }
        }
        if (max > min) {
            floor = min;
            ceil = max;
        }

        gain = 1 / (ceil - floor);
        for (y = 0; y < height; y++) {
            const uint8_t *src = in->data[0] + y * in->linesize[0];
            float *dst = s->in_data[0] + y * width;

            for (x = 0; x < width; x++)
                dst[x] = (src[x] - floor) * gain;
        }
    }

    msr_kernel(s->out_data[0], s->in_data[0], s->gauss,
               width, height);
    simplest_color_balance(s, s->out_data[0], s->in_data[0],
                           width, height);

    for (y = 0; y < height; y++) {
        uint8_t *dst = out->data[0] + y * out->linesize[0];
        float *src = s->out_data[0] + y * width;

        for (x = 0; x < width; x++)
            dst[x] = src[x] * range;
    }
}

static void msrcp_yuv(RetinexContext *s, AVFrame *in, AVFrame *out)
{
    float chroma_protect_mul1 = s->chroma_protect - 1;
    float chroma_protect_mul2 = 1 / log(s->chroma_protect);
    float crange = s->full ? 255 : 224;
    float yrange = s->full ? 255 : 219;
    float floor = s->full ? 0 : 16;
    float gain = 1. / yrange;
    int u, v, x, y;
    float yoffset = floor + 0.5;
    const int height = in->height;
    const int width = in->width;

    if (s->full) {
        for (y = 0; y < height; y++) {
            float *dst = s->in_data[0] + y * width;
            const uint8_t *src = in->data[0] + y * in->linesize[0];

            for (x = 0; x < width; x++)
                dst[x] = src[x] * gain;
        }
    } else {
        float min = 255, max = 0, ceil = 255, floor = 0;

        for (y = 0; y < height; y++) {
            const uint8_t *src = in->data[0] + y * in->linesize[0];

            for (x = 0; x < width; x++) {
                min = FFMIN(min, src[x]);
                max = FFMAX(max, src[x]);
            }
        }
        if (max > min) {
            floor = min;
            ceil = max;
        }

        gain = 1 / (ceil - floor);
        for (y = 0; y < height; y++) {
            const uint8_t *src = in->data[0] + y * in->linesize[0];
            float *dst = s->in_data[0] + y * width;

            for (x = 0; x < width; x++)
                dst[x] = (src[x] - floor) * gain;
        }
    }

    msr_kernel(s->out_data[0], s->in_data[0], s->gauss,
               width, height);
    simplest_color_balance(s, s->out_data[0], s->in_data[0],
                           width, height);

    for (y = 0; y < height; y++) {
        const uint8_t *usrc = in->data[1] + y * in->linesize[1];
        const uint8_t *vsrc = in->data[2] + y * in->linesize[2];
        uint8_t *ydst = out->data[0] + y * out->linesize[0];
        uint8_t *udst = out->data[1] + y * out->linesize[1];
        uint8_t *vdst = out->data[2] + y * out->linesize[2];
        const float *in_data = s->in_data[0] + y * width;
        const float *out_data = s->out_data[0] + y * width;

        for (x = 0; x < width; x++) {
            u = usrc[x] - 128;
            v = vsrc[x] - 128;
            if (s->chroma_protect > 1)
                gain = in_data[x] <= 0 ? 1 : log(out_data[x] / in_data[x] * chroma_protect_mul1 + 1) * chroma_protect_mul2;
            else
                gain = in_data[x] <= 0 ? 1 : out_data[x] / in_data[x];
            gain = FFMIN(crange / 2 / FFMAX(FFABS(u), FFABS(v)), gain);
            ydst[x] = out_data[x] * yrange + yoffset;
            udst[x] = u * gain + 128;
            vdst[x] = v * gain + 128;
        }
    }
}

static void msrcr_gbrp(RetinexContext *s, AVFrame *in, AVFrame *out)
{
    const int height = in->height;
    const int width = in->width;
    float gain;
    int x, y;

    gain = 1. / 255;
    for (y = 0; y < height; y++) {
        float *dst = s->in_data[2] + y * width;
        const uint8_t *src = in->data[2] + y * in->linesize[2];

        for (x = 0; x < width; x++)
            dst[x] = src[x] * gain;
    }

    msr_kernel(s->out_data[2], s->in_data[2], s->gauss,
               width, height);

    for (y = 0; y < height; y++) {
        float *dst = s->in_data[0] + y * width;
        const uint8_t *src = in->data[0] + y * in->linesize[0];

        for (x = 0; x < width; x++)
            dst[x] = src[x] * gain;
    }

    msr_kernel(s->out_data[0], s->in_data[0], s->gauss,
               width, height);

    for (y = 0; y < height; y++) {
        float *dst = s->in_data[1] + y * width;
        const uint8_t *src = in->data[1] + y * in->linesize[1];

        for (x = 0; x < width; x++)
            dst[x] = src[x] * gain;
    }

    msr_kernel(s->out_data[1], s->in_data[1], s->gauss,
               width, height);

    for (y = 0; y < height; y++) {
        const uint8_t *Rsrcp = in->data[2] + y * in->linesize[2];
        const uint8_t *Gsrcp = in->data[0] + y * in->linesize[0];
        const uint8_t *Bsrcp = in->data[1] + y * in->linesize[1];
        float *odataR = s->out_data[2] + y * width;
        float *odataG = s->out_data[0] + y * width;
        float *odataB = s->out_data[1] + y * width;

        for (x = 0; x < width; x++) {
            float RvalFL, GvalFL, BvalFL;
            float temp;

            RvalFL = Rsrcp[x];
            GvalFL = Gsrcp[x];
            BvalFL = Bsrcp[x];
            temp = RvalFL + GvalFL + BvalFL;
            temp = temp <= 0 ? 0 : s->restore / temp;
            odataR[x] *= log(RvalFL * temp + 1);
            odataG[x] *= log(GvalFL * temp + 1);
            odataB[x] *= log(BvalFL * temp + 1);
        }
    }

    simplest_color_balance(s, s->out_data[2], s->in_data[2], width, height);
    for (y = 0; y < height; y++) {
        const float *src = s->out_data[2] + y * width;
        uint8_t *dst = out->data[2] + y * out->linesize[2];

        for (x = 0; x < width; x++)
            dst[x] = src[x] * 255;
    }

    simplest_color_balance(s, s->out_data[0], s->in_data[0], width, height);
    for (y = 0; y < height; y++) {
        const float *src = s->out_data[0] + y * width;
        uint8_t *dst = out->data[0] + y * out->linesize[0];

        for (x = 0; x < width; x++)
            dst[x] = src[x] * 255;
    }

    simplest_color_balance(s, s->out_data[1], s->in_data[1], width, height);
    for (y = 0; y < height; y++) {
        const float *src = s->out_data[1] + y * width;
        uint8_t *dst = out->data[1] + y * out->linesize[1];

        for (x = 0; x < width; x++)
            dst[x] = src[x] * 255;
    }
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    RetinexContext *s = ctx->priv;

    s->in_data[0] = av_mallocz_array(inlink->h, inlink->w * sizeof(*s->in_data));
    s->out_data[0] = av_mallocz_array(inlink->h, inlink->w * sizeof(*s->out_data[0]));
    if (s->mode = 1) {
        s->in_data[1] = av_mallocz_array(inlink->h, inlink->w * sizeof(*s->out_data[1]));
        s->in_data[2] = av_mallocz_array(inlink->h, inlink->w * sizeof(*s->out_data[2]));
        s->out_data[1] = av_mallocz_array(inlink->h, inlink->w * sizeof(*s->out_data[1]));
        s->out_data[2] = av_mallocz_array(inlink->h, inlink->w * sizeof(*s->out_data[2]));
    }
    s->gauss = av_mallocz_array(inlink->h, inlink->w * sizeof(*s->gauss));

    switch (inlink->format) {
    case AV_PIX_FMT_GRAY8:
        s->retinex = msrcp_gray;
        break;
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUV444P:
        s->retinex = msrcp_yuv;
        break;
    case AV_PIX_FMT_GBRP:
        s->retinex = msrcr_gbrp;
        break;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    RetinexContext *s = ctx->priv;
    AVFrame *out;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    s->retinex(s, in, out);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static void uninit(AVFilterContext *ctx)
{
    RetinexContext *s = ctx->priv;

    av_freep(&s->in_data[0]);
    av_freep(&s->in_data[1]);
    av_freep(&s->in_data[2]);
    av_freep(&s->out_data[0]);
    av_freep(&s->out_data[1]);
    av_freep(&s->out_data[2]);
    av_freep(&s->gauss);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_retinex = {
    .name          = "retinex",
    .description   = NULL_IF_CONFIG_SMALL("Compress video dynamic range."),
    .priv_size     = sizeof(RetinexContext),
    .priv_class    = &retinex_class,
    .query_formats = query_formats,
    .uninit        = uninit,
    .inputs        = inputs,
    .outputs       = outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
