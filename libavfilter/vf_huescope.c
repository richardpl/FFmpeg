/*
 * Copyright (c) 2016 Paul B Mahol
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

#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

enum HuescopeMode {
    GRAY,
    COLOR,
    COLOR2,
    MODE_NB
};

typedef struct HuescopeContext {
    const AVClass *class;
    int w, h;
    int mode;
    int intensity;
    float fintensity;
    const uint8_t *bg_color;
    int planewidth[4];
    int planeheight[4];
    int mult;
    int max;
    float mid;

    void (*huescope)(struct HuescopeContext *s,
                        AVFrame *in, AVFrame *out);
} HuescopeContext;

#define OFFSET(x) offsetof(HuescopeContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption huescope_options[] = {
    { "size", "set huescope size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, {.str="hd720"}, 0, 0, FLAGS},
    { "s",    "set huescope size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, {.str="hd720"}, 0, 0, FLAGS},
    { "mode", "set vectorscope mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=0}, 0, MODE_NB-1, FLAGS, "mode"},
    { "m",    "set vectorscope mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=0}, 0, MODE_NB-1, FLAGS, "mode"},
    {   "gray",   0, 0, AV_OPT_TYPE_CONST, {.i64=GRAY},   0, 0, FLAGS, "mode" },
    {   "color",  0, 0, AV_OPT_TYPE_CONST, {.i64=COLOR},  0, 0, FLAGS, "mode" },
    {   "color2", 0, 0, AV_OPT_TYPE_CONST, {.i64=COLOR2}, 0, 0, FLAGS, "mode" },
    { "intensity", "set intensity", OFFSET(fintensity), AV_OPT_TYPE_FLOAT, {.dbl=0.004}, 0, 1, FLAGS},
    { "i",         "set intensity", OFFSET(fintensity), AV_OPT_TYPE_FLOAT, {.dbl=0.004}, 0, 1, FLAGS},
    { NULL }
};

AVFILTER_DEFINE_CLASS(huescope);

static const enum AVPixelFormat in_pix_fmts[] = {
    AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUV411P,  AV_PIX_FMT_YUVJ411P,
    AV_PIX_FMT_YUV440P,  AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat out_yuv8_pix_fmts[] = {
    AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat out_yuv9_pix_fmts[] = {
    AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat out_yuv10_pix_fmts[] = {
    AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat out_yuv12_pix_fmts[] = {
    AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat out_yuv16_pix_fmts[] = {
    AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_NONE
};

static int query_formats(AVFilterContext *ctx)
{
    HuescopeContext *s = ctx->priv;
    const enum AVPixelFormat *out_pix_fmts;
    const AVPixFmtDescriptor *desc;
    AVFilterFormats *avff;
    int depth, i, ret;

    if (!ctx->inputs[0]->in_formats ||
        !ctx->inputs[0]->in_formats->nb_formats) {
        return AVERROR(EAGAIN);
    }

    if (!ctx->inputs[0]->out_formats) {
        if ((ret = ff_formats_ref(ff_make_format_list(in_pix_fmts), &ctx->inputs[0]->out_formats)) < 0)
            return ret;
    }

    avff = ctx->inputs[0]->in_formats;
    desc = av_pix_fmt_desc_get(avff->formats[0]);
    depth = desc->comp[0].depth;
    for (i = 1; i < avff->nb_formats; i++) {
        desc = av_pix_fmt_desc_get(avff->formats[i]);
        if (depth != desc->comp[0].depth)
            return AVERROR(EAGAIN);
    }

    if (depth == 9)
        out_pix_fmts = out_yuv9_pix_fmts;
    else if (depth == 10)
        out_pix_fmts = out_yuv10_pix_fmts;
    else if (depth == 12)
        out_pix_fmts = out_yuv12_pix_fmts;
    else if (depth == 16)
        out_pix_fmts = out_yuv16_pix_fmts;
    else
        out_pix_fmts = out_yuv8_pix_fmts;
    if ((ret = ff_formats_ref(ff_make_format_list(out_pix_fmts), &ctx->outputs[0]->in_formats)) < 0)
        return ret;

    return 0;
}

static const uint8_t black_yuva_color[4] = { 0, 127, 127, 0 };

static int config_output(AVFilterLink *outlink)
{
    HuescopeContext *s = outlink->src->priv;

    s->intensity = s->fintensity * (256 - 1);
    outlink->h = s->h;
    outlink->w = s->w;
    outlink->sample_aspect_ratio = (AVRational){1,1};

    return 0;
}

static void huescope8(HuescopeContext *s, AVFrame *in, AVFrame *out)
{
    const uint8_t * const *src = (const uint8_t * const *)in->data;
    const int slinesizex = in->linesize[1];
    const int slinesizey = in->linesize[2];
    const int slinesized = in->linesize[0];
    const int dlinesize = out->linesize[0];
    const int intensity = s->intensity;
    const int h = s->planeheight[2];
    const int w = s->planewidth[1];
    const uint8_t *spx = src[1];
    const uint8_t *spy = src[2];
    const uint8_t *spd = src[0];
    uint8_t **dst = out->data;
    uint8_t *dpx = dst[1];
    uint8_t *dpy = dst[2];
    uint8_t *dpd = dst[0];
    int i, j, k;

    for (k = 0; k < 4 && dst[k]; k++)
        for (i = 0; i < out->height ; i++)
            memset(dst[k] + i * out->linesize[k],
                   s->mode == COLOR && k == 0 ? 0 : s->bg_color[k], out->width);

    switch (s->mode) {
    case GRAY:
        for (i = 0; i < h; i++) {
            const int iwx = i * slinesizex;
            const int iwy = i * slinesizey;
            for (j = 0; j < w; j++) {
                const float u = (spx[iwx + j] - 127.5) / 127.5;
                const float v = (spy[iwy + j] - 127.5) / 127.5;
                int x = av_clipf((atan2f(v, u) / (M_PI * 2) + .5), 0, 1) * (out->width - 1);
                int y = (spd[iwx + j] / 255.) * (out->height - 1);
                int pos = dlinesize * y + x;
                dpd[pos] = FFMIN(dpd[pos] + intensity, 255);
                if (dst[3])
                    dst[3][pos] = 255;
            }
        }
        break;
    case COLOR:
        for (i = 0; i < h; i++) {
            const int iwx = i * slinesizex;
            const int iwy = i * slinesizey;
            for (j = 0; j < w; j++) {
                const float u = (spx[iwx + j] - 127.5) / 127.5;
                const float v = (spy[iwy + j] - 127.5) / 127.5;
                int x = av_clipf((atan2f(v, u) / (M_PI * 2) + .5), 0, 1) * (out->width - 1);
                int y = (spd[iwx + j] / 255.) * (out->height - 1);
                int pos = dlinesize * y + x;
                dpd[pos] = FFMIN(dpd[pos] + intensity, 255);
                dpx[pos] = spx[iwx + j];
                dpy[pos] = spy[iwy + j];
                if (dst[3])
                    dst[3][pos] = 255;
            }
        }
        break;
    case COLOR2:
        for (i = 0; i < h; i++) {
            const int iwx = i * slinesizex;
            const int iwy = i * slinesizey;
            for (j = 0; j < w; j++) {
                const float u = (spx[iwx + j] - 127.5) / 127.5;
                const float v = (spy[iwy + j] - 127.5) / 127.5;
                int x = av_clipf((atan2f(v, u) / (M_PI * 2) + .5), 0, 1) * (out->width - 1);
                int y = (spd[iwx + j] / 255.) * (out->height - 1);
                int pos = dlinesize * y + x;
                dpd[pos] = spd[iwx + j];
                dpx[pos] = spx[iwx + j];
                dpy[pos] = spy[iwy + j];
                if (dst[3])
                    dst[3][pos] = 255;
            }
        }
        break;
    default:
        av_assert0(0);
    }
}

static void huescope16(HuescopeContext *s, AVFrame *in, AVFrame *out)
{
    const uint16_t * const *src = (const uint16_t * const *)in->data;
    const int slinesizex = in->linesize[1] / 2;
    const int slinesizey = in->linesize[2] / 2;
    const int slinesized = in->linesize[0] / 2;
    const int dlinesize = out->linesize[0] / 2;
    const int intensity = s->intensity;
    const int h = s->planeheight[2];
    const int w = s->planewidth[1];
    const uint16_t *spx = src[1];
    const uint16_t *spy = src[2];
    uint16_t **dst = (uint16_t **)out->data;
    uint16_t *dpx = dst[1];
    uint16_t *dpy = dst[2];
    uint16_t *dpd = dst[0];
    const int max = s->max;
    const float mid = s->mid;
    int hh = s->h / 2;
    int i, j, k;

    for (k = 0; k < 4 && dst[k]; k++) {
        const int mult = s->mult;

        for (i = 0; i < out->height ; i++)
            for (j = 0; j < out->width; j++)
                AV_WN16(out->data[k] + i * out->linesize[k] + j * 2,
                        s->mode == COLOR && k == 0 ? 0 : s->bg_color[k] * mult);
    }

    switch (s->mode) {
    case GRAY:
            for (i = 0; i < h; i++) {
                const int iwx = i * slinesizex;
                const int iwy = i * slinesizey;
                for (j = 0; j < w; j++) {
                    const float u = av_clipf((spx[iwx + j] - mid) / mid, -1, 1);
                    const float v = av_clipf((spy[iwy + j] - mid) / mid, -1, 1);
                    int x = hh + hh * u * sqrtf(1 - 0.5 * v * v);
                    int y = hh + hh * v * sqrtf(1 - 0.5 * u * u);
                    int pos = dlinesize * y + x;
                    dpd[pos] = FFMIN(dpd[pos] + intensity, max);
                    if (dst[3])
                        dst[3][pos] = 255;
                }
            }
        break;
    default:
        av_assert0(0);
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx  = inlink->dst;
    HuescopeContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    out->pts = in->pts;

    s->huescope(s, in, out);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int config_input(AVFilterLink *inlink)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    HuescopeContext *s = inlink->dst->priv;

    s->mult = (1 << desc->comp[0].depth) / 256;
    s->max = (1 << desc->comp[0].depth) - 1;
    s->mid = s->max / 2.;

    if (desc->comp[0].depth == 8)
        s->huescope = huescope8;
    else
        s->huescope = huescope16;

    s->bg_color = black_yuva_color;

    s->planeheight[1] = s->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;
    s->planewidth[1]  = s->planewidth[2]  = AV_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->planewidth[0]  = s->planewidth[3]  = inlink->w;

    return 0;
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
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_vf_huescope = {
    .name          = "huescope",
    .description   = NULL_IF_CONFIG_SMALL("Video hue scope."),
    .priv_size     = sizeof(HuescopeContext),
    .priv_class    = &huescope_class,
    .query_formats = query_formats,
    .inputs        = inputs,
    .outputs       = outputs,
};
