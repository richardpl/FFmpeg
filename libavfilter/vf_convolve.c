/*
 * Copyright (c) 2017 Paul B Mahol
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

#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavcodec/avfft.h"

#include "avfilter.h"
#include "formats.h"
#include "framesync2.h"
#include "internal.h"
#include "video.h"

typedef struct ConvolveContext {
    const AVClass *class;
    FFFrameSync fs;

    FFTContext *fft[4];
    FFTContext *ifft[4];

    int n[4];
    int fft_bits[4];
    int fft_len[4];
    int planewidth[4];
    int planeheight[4];

    FFTComplex *fft_hdata[4];
    FFTComplex *fft_vdata[4];
    FFTComplex *fft_hdata_impulse[4];
    FFTComplex *fft_vdata_impulse[4];

    int planes;
} ConvolveContext;

#define OFFSET(x) offsetof(ConvolveContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption convolve_options[] = {
    { "planes", "set planes to convolve", OFFSET(planes), AV_OPT_TYPE_INT, {.i64 = 15}, 0, 15, FLAGS },
    { NULL },
};

FRAMESYNC_DEFINE_CLASS(convolve, ConvolveContext, fs);

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pixel_fmts_fftfilt[] = {
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_GBRP,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pixel_fmts_fftfilt);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_input_main(AVFilterLink *inlink)
{
    ConvolveContext *s = inlink->dst->priv;
    int fft_bits, i;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    s->planewidth[1] = s->planewidth[2] = AV_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->planewidth[0] = s->planewidth[3] = inlink->w;
    s->planeheight[1] = s->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;

    for (i = 0; i < desc->nb_components; i++) {
        int w = s->planewidth[i];
        int h = s->planeheight[i];
        int n = FFMAX(w, h) + FFMIN(w, h) / 2;

        for (fft_bits = 1; 1 << fft_bits < n; fft_bits++);

        s->fft_bits[i] = fft_bits;
        s->n[i] = n;

        s->fft_len[i] = 1 << s->fft_bits[i];

        if (!(s->fft_hdata[i] = av_calloc(n, s->fft_len[i] * sizeof(FFTComplex))))
            return AVERROR(ENOMEM);

        if (!(s->fft_vdata[i] = av_calloc(s->fft_len[i], s->fft_len[i] * sizeof(FFTComplex))))
            return AVERROR(ENOMEM);

        if (!(s->fft_hdata_impulse[i] = av_calloc(n, s->fft_len[i] * sizeof(FFTComplex))))
            return AVERROR(ENOMEM);

        if (!(s->fft_vdata_impulse[i] = av_calloc(s->fft_len[i], s->fft_len[i] * sizeof(FFTComplex))))
            return AVERROR(ENOMEM);
    }

    return 0;
}

static int config_input_impulse(AVFilterLink *inlink)
{
    AVFilterContext *ctx  = inlink->dst;

    if (ctx->inputs[0]->w != ctx->inputs[1]->w ||
        ctx->inputs[0]->h != ctx->inputs[1]->h) {
        av_log(ctx, AV_LOG_ERROR, "Width and height of input videos must be same.\n");
        return AVERROR(EINVAL);
    }
    if (ctx->inputs[0]->format != ctx->inputs[1]->format) {
        av_log(ctx, AV_LOG_ERROR, "Inputs must be of same pixel format.\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static void fft_horizontal(ConvolveContext *s, FFTComplex *fft_hdata, int fft_len,
                           AVFrame *in, int w, int h, int n, int plane, float scale)
{
    int y, x;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            fft_hdata[y * fft_len + x].re = in->data[plane][in->linesize[plane]*y+x] * scale;
            fft_hdata[y * fft_len + x].im = 0;
        }
        for (; x < fft_len; x++) {
            fft_hdata[y * fft_len + x].re = 0;
            fft_hdata[y * fft_len + x].im = 0;
        }

        av_fft_permute(s->fft[plane], fft_hdata + y * fft_len);
        av_fft_calc(s->fft[plane], fft_hdata + y * fft_len);
    }

    for (; y < n; y++) {
        for (x = 0; x < fft_len; x++) {
            fft_hdata[y * fft_len + x].re = 0;
            fft_hdata[y * fft_len + x].im = 0;
        }
    }
}

static void fft_vertical(ConvolveContext *s, FFTComplex *fft_hdata, FFTComplex *fft_vdata,
                         int fft_len, int h, int plane)
{
    int y, x;

    for (y = 0; y < fft_len; y++) {
        for (x = 0; x < h; x++) {
            fft_vdata[y * fft_len + x].re = fft_hdata[x * fft_len + y].re;
            fft_vdata[y * fft_len + x].im = fft_hdata[x * fft_len + y].im;
        }
        for (; x < fft_len; x++) {
            fft_vdata[y * fft_len + x].re = 0;
            fft_vdata[y * fft_len + x].im = 0;
        }
        av_fft_permute(s->fft[plane], fft_vdata + y * fft_len);
        av_fft_calc(s->fft[plane], fft_vdata + y * fft_len);
    }
}

static void ifft_vertical(ConvolveContext *s, int fft_len, int h, int plane)
{
    int y, x;

    for (y = 0; y < fft_len; y++) {
        av_fft_permute(s->ifft[plane], s->fft_vdata[plane] + y * fft_len);
        av_fft_calc(s->ifft[plane], s->fft_vdata[plane] + y * fft_len);
        for (x = 0; x < h; x++) {
            s->fft_hdata[plane][x * fft_len + y].re = s->fft_vdata[plane][y * fft_len + x].re;
            s->fft_hdata[plane][x * fft_len + y].im = s->fft_vdata[plane][y * fft_len + x].im;
        }
    }
}

static void ifft_horizontal(ConvolveContext *s, AVFrame *out, int fft_len,
                            int w, int h, int n, int plane)
{
    const float scale = 1.f / (fft_len * fft_len);
    int y, x;

    for (y = 0; y < n; y++) {
        av_fft_permute(s->ifft[plane], s->fft_hdata[plane] + y * fft_len);
        av_fft_calc(s->ifft[plane], s->fft_hdata[plane] + y * fft_len);
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++)
            out->data[plane][out->linesize[plane] * y + x] =
                av_clip(s->fft_hdata[plane][(y+h/2)* fft_len + x + w/2].re * scale, 0, 255);
    }
}

static int do_convolve(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFilterLink *outlink = ctx->outputs[0];
    const AVPixFmtDescriptor *desc;
    ConvolveContext *s = ctx->priv;
    AVFrame *mainpic = NULL, *impulsepic = NULL;
    int ret, y, x, plane;

    ret = ff_framesync2_dualinput_get(fs, &mainpic, &impulsepic);
    if (ret < 0)
        return ret;
    if (!impulsepic)
        return ff_filter_frame(ctx->outputs[0], mainpic);

    desc = av_pix_fmt_desc_get(outlink->format);
    for (plane = 0; plane < desc->nb_components; plane++) {
        const int fft_len = s->fft_len[plane];
        int w = s->planewidth[plane];
        int h = s->planeheight[plane];
        int n = s->n[plane];
        float total = 0;

        if (!(s->planes & (1 << plane)))
            continue;

        fft_horizontal(s, s->fft_hdata[plane], fft_len, mainpic, w, h, n, plane, 1.f);
        fft_vertical(s, s->fft_hdata[plane], s->fft_vdata[plane],
                     fft_len, n, plane);

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                total += impulsepic->data[plane][y * impulsepic->linesize[plane] + x];
            }
        }
        total = FFMAX(1, total);

        fft_horizontal(s, s->fft_hdata_impulse[plane], fft_len, impulsepic, w, h, n, plane, 1 / total);
        fft_vertical(s, s->fft_hdata_impulse[plane], s->fft_vdata_impulse[plane],
                     fft_len, n, plane);

        for (y = 0; y < fft_len; y++) {
            for (x = 0; x < fft_len; x++) {
                FFTSample re, im, ire, iim;

                re = s->fft_vdata[plane][y*fft_len + x].re;
                im = s->fft_vdata[plane][y*fft_len + x].im;
                ire = s->fft_vdata_impulse[plane][y*fft_len + x].re;
                iim = s->fft_vdata_impulse[plane][y*fft_len + x].im;

                s->fft_vdata[plane][y*fft_len + x].re = ire * re - iim * im;
                s->fft_vdata[plane][y*fft_len + x].im = iim * re + ire * im;
            }
        }

        ifft_vertical(s, fft_len, n, plane);
        ifft_horizontal(s, mainpic, fft_len, w, h, n, plane);
    }

    return ff_filter_frame(outlink, mainpic);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ConvolveContext *s = ctx->priv;
    AVFilterLink *mainlink = ctx->inputs[0];
    int ret, i;

    s->fs.on_event = do_convolve;
    ret = ff_framesync2_init_dualinput(&s->fs, ctx);
    if (ret < 0)
        return ret;
    outlink->w = mainlink->w;
    outlink->h = mainlink->h;
    outlink->time_base = mainlink->time_base;
    outlink->sample_aspect_ratio = mainlink->sample_aspect_ratio;
    outlink->frame_rate = mainlink->frame_rate;

    if ((ret = ff_framesync2_configure(&s->fs)) < 0)
        return ret;

    for (i = 0; i < 4; i++) {
        s->fft[i]  = av_fft_init(s->fft_bits[i], 0);
        s->ifft[i] = av_fft_init(s->fft_bits[i], 1);
    }

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    ConvolveContext *s = ctx->priv;
    return ff_framesync2_activate(&s->fs);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ConvolveContext *s = ctx->priv;
    int i;

    for (i = 0; i < 4; i++) {
        av_freep(&s->fft_hdata[i]);
        av_freep(&s->fft_vdata[i]);
        av_freep(&s->fft_hdata_impulse[i]);
        av_freep(&s->fft_vdata_impulse[i]);
        av_fft_end(s->fft[i]);
        av_fft_end(s->ifft[i]);
    }

    ff_framesync2_uninit(&s->fs);
}

static const AVFilterPad convolve_inputs[] = {
    {
        .name          = "main",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_input_main,
    },{
        .name          = "impulse",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_input_impulse,
    },
    { NULL }
};

static const AVFilterPad convolve_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_vf_convolve = {
    .name          = "convolve",
    .description   = NULL_IF_CONFIG_SMALL("Convolve first video stream with second video stream."),
    .preinit       = convolve_framesync_preinit,
    .uninit        = uninit,
    .query_formats = query_formats,
    .activate      = activate,
    .priv_size     = sizeof(ConvolveContext),
    .priv_class    = &convolve_class,
    .inputs        = convolve_inputs,
    .outputs       = convolve_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
};
