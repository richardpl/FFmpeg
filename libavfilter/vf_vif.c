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
 * Calculate VIF between two input videos.
 */

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "dualinput.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "vif.h"
#include "video.h"

typedef struct VIFContext {
    const AVClass *class;
    FFDualInputContext dinput;
    const AVPixFmtDescriptor *desc;
    int width;
    int height;
    float *data_buf;
    float *temp;
    float *ref_data;
    float *main_data;
    double vif_sum;
    uint64_t nb_frames;
} VIFContext;

#define OFFSET(x) offsetof(VIFContext, x)
#define MAX_ALIGN 32
#define ALIGN_CEIL(x) ((x) + ((x) % MAX_ALIGN ? MAX_ALIGN - (x) % MAX_ALIGN : 0))
#define OPT_RANGE_PIXEL_OFFSET (-128)

static const AVOption vif_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(vif);

const int vif_filter1d_width1[4] = { 17, 9, 5, 3 };
const float vif_filter1d_table[4][17] = {
    { 0x1.e8a77p-8,  0x1.d373b2p-7, 0x1.9a1cf6p-6, 0x1.49fd9ep-5, 0x1.e7092ep-5,
      0x1.49a044p-4, 0x1.99350ep-4, 0x1.d1e76ap-4, 0x1.e67f8p-4,  0x1.d1e76ap-4,
      0x1.99350ep-4, 0x1.49a044p-4, 0x1.e7092ep-5, 0x1.49fd9ep-5, 0x1.9a1cf6p-6,
      0x1.d373b2p-7, 0x1.e8a77p-8 },
    { 0x1.36efdap-6, 0x1.c9eaf8p-5, 0x1.ef4ac2p-4, 0x1.897424p-3, 0x1.cb1b88p-3,
      0x1.897424p-3, 0x1.ef4ac2p-4, 0x1.c9eaf8p-5, 0x1.36efdap-6 },
    { 0x1.be5f0ep-5, 0x1.f41fd6p-3, 0x1.9c4868p-2, 0x1.f41fd6p-3, 0x1.be5f0ep-5 },
    { 0x1.54be4p-3,  0x1.55a0ep-1,  0x1.54be4p-3 }
};

static void vif_dec2(const float *src, float *dst, int src_w, int src_h,
                     int src_stride, int dst_stride)
{
    int src_px_stride = src_stride / sizeof(float);
    int dst_px_stride = dst_stride / sizeof(float);

    int i, j;

    /** decimation by 2 in each direction (after gaussian blur check) */
    for (i = 0; i < src_h / 2; i++) {
        for (j = 0; j < src_w / 2; j++) {
            dst[i * dst_px_stride + j] = src[(i * 2) * src_px_stride + (j * 2)];
        }
    }
}

static float vif_sum(const float *x, int w, int h, int stride)
{
    int px_stride = stride / sizeof(float);
    int i, j;

    float sum = 0;

    for (i = 0; i < h; i++) {
        float sum_inner = 0;

        for (j = 0; j < w; j++) {
            sum_inner += x[i * px_stride + j];
        }

        sum += sum_inner;
    }

    return sum;
}

static void vif_statistic(const float *mu1_sq, const float *mu2_sq,
                          const float *mu1_mu2, const float *xx_filt,
                          const float *yy_filt, const float *xy_filt,
                          float *num, float *den, int w, int h,
                          int mu1_sq_stride, int mu2_sq_stride,
                          int mu1_mu2_stride, int xx_filt_stride,
                          int yy_filt_stride, int xy_filt_stride,
                          int num_stride, int den_stride)
{
    static const float sigma_nsq = 2;
    static const float sigma_max_inv = 4.0/(255.0*255.0);

    int mu1_sq_px_stride  = mu1_sq_stride / sizeof(float);
    int mu2_sq_px_stride  = mu2_sq_stride / sizeof(float);
    int mu1_mu2_px_stride = mu1_mu2_stride / sizeof(float);
    int xx_filt_px_stride = xx_filt_stride / sizeof(float);
    int yy_filt_px_stride = yy_filt_stride / sizeof(float);
    int xy_filt_px_stride = xy_filt_stride / sizeof(float);
    int num_px_stride = num_stride / sizeof(float);
    int den_px_stride = den_stride / sizeof(float);

    float mu1_sq_val, mu2_sq_val, mu1_mu2_val, xx_filt_val, yy_filt_val, xy_filt_val;
    float sigma1_sq, sigma2_sq, sigma12, g, sv_sq;
    float num_val, den_val;
    int i, j;

    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            mu1_sq_val  = mu1_sq[i * mu1_sq_px_stride + j];
            mu2_sq_val  = mu2_sq[i * mu2_sq_px_stride + j];
            mu1_mu2_val = mu1_mu2[i * mu1_mu2_px_stride + j];
            xx_filt_val = xx_filt[i * xx_filt_px_stride + j];
            yy_filt_val = yy_filt[i * yy_filt_px_stride + j];
            xy_filt_val = xy_filt[i * xy_filt_px_stride + j];

            sigma1_sq = xx_filt_val - mu1_sq_val;
            sigma2_sq = yy_filt_val - mu2_sq_val;
            sigma12   = xy_filt_val - mu1_mu2_val;

            if (sigma1_sq < sigma_nsq) {
                num_val = 1.0 - sigma2_sq*sigma_max_inv;
                den_val = 1.0;
            } else {
                sv_sq = (sigma2_sq + sigma_nsq) * sigma1_sq;
                if( sigma12 < 0 ) {
                    num_val = 0.0;
                } else {
                    g = sv_sq - sigma12 * sigma12;
                    num_val = log2f(sv_sq / g);
                }
                den_val = log2f(1.0f + sigma1_sq / sigma_nsq);
            }

            num[i * num_px_stride + j] = num_val;
            den[i * den_px_stride + j] = den_val;
        }
    }
}

static void vif_xx_yy_xy(const float *x, const float *y, float *xx, float *yy,
                         float *xy, int w, int h, int xstride, int ystride,
                         int xxstride, int yystride, int xystride)
{
    int x_px_stride = xstride / sizeof(float);
    int y_px_stride = ystride / sizeof(float);
    int xx_px_stride = xxstride / sizeof(float);
    int yy_px_stride = yystride / sizeof(float);
    int xy_px_stride = xystride / sizeof(float);

    int i, j;

    float xval, yval, xxval, yyval, xyval;

    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            xval = x[i * x_px_stride + j];
            yval = y[i * y_px_stride + j];

            xxval = xval * xval;
            yyval = yval * yval;
            xyval = xval * yval;

            xx[i * xx_px_stride + j] = xxval;
            yy[i * yy_px_stride + j] = yyval;
            xy[i * xy_px_stride + j] = xyval;
        }
    }
}

static void vif_filter1d(const float *filter, const float *src, float *dst,
                         float *temp_buf, int w, int h, int src_stride,
                         int dst_stride, int filt_w, float *temp)
{
    int src_px_stride = src_stride / sizeof(float);
    int dst_px_stride = dst_stride / sizeof(float);

    float filt_coeff, img_coeff;

    int i, j, filt_i, filt_j, ii, jj;

    for (i = 0; i < h; i++) {
        /** Vertical pass. */
        for (j = 0; j < w; j++) {
            float sum = 0;

            for (filt_i = 0; filt_i < filt_w; filt_i++) {
                filt_coeff = filter[filt_i];

                ii = i - filt_w / 2 + filt_i;
                ii = ii < 0 ? -ii : (ii >= h ? 2 * h - ii - 1 : ii);

                img_coeff = src[ii * src_px_stride + j];

                sum += filt_coeff * img_coeff;
            }

            temp[j] = sum;
        }

        /** Horizontal pass. */
        for (j = 0; j < w; j++) {
            float sum = 0;

            for (filt_j = 0; filt_j < filt_w; filt_j++) {
                filt_coeff = filter[filt_j];

                jj = j - filt_w / 2 + filt_j;
                jj = jj < 0 ? -jj : (jj >= w ? 2 * w - jj - 1 : jj);

                img_coeff = temp[jj];

                sum += filt_coeff * img_coeff;
            }

            dst[i * dst_px_stride + j] = sum;
        }
    }
}

int compute_vif2(const float *ref, const float *main, int w, int h,
                 int ref_stride, int main_stride, double *score,
                 double *score_num, double *score_den, double *scores,
                 float *data_buf, float *temp)
{
    char *data_top;

    float *ref_scale;
    float *main_scale;
    float *ref_sq;
    float *main_sq;
    float *ref_main;

    float *mu1;
    float *mu2;
    float *mu1_sq;
    float *mu2_sq;
    float *mu1_mu2;
    float *ref_sq_filt;
    float *main_sq_filt;
    float *ref_main_filt;
    float *num_array;
    float *den_array;
    float *temp_buf;

    const float *curr_ref_scale = ref;
    const float *curr_main_scale = main;
    int curr_ref_stride = ref_stride;
    int curr_main_stride = main_stride;

    int buf_stride = ALIGN_CEIL(w * sizeof(float));
    size_t buf_sz = (size_t)buf_stride * h;

    double num = 0;
    double den = 0;

    int scale;
    int ret = 1;

    data_top = (char *) data_buf;

    ref_scale = (float *) data_top;
    data_top += buf_sz;

    main_scale = (float *) data_top;
    data_top += buf_sz;

    ref_sq = (float *) data_top;
    data_top += buf_sz;

    main_sq = (float *) data_top;
    data_top += buf_sz;

    ref_main = (float *) data_top;
    data_top += buf_sz;

    mu1 = (float *) data_top;
    data_top += buf_sz;

    mu2 = (float *) data_top;
    data_top += buf_sz;

    mu1_sq = (float *) data_top;
    data_top += buf_sz;

    mu2_sq = (float *) data_top;
    data_top += buf_sz;

    mu1_mu2 = (float *) data_top;
    data_top += buf_sz;

    ref_sq_filt = (float *) data_top;
    data_top += buf_sz;

    main_sq_filt = (float *) data_top;
    data_top += buf_sz;

    ref_main_filt = (float *) data_top;
    data_top += buf_sz;

    num_array = (float *) data_top;
    data_top += buf_sz;

    den_array = (float *) data_top;
    data_top += buf_sz;

    temp_buf = (float *) data_top;
    data_top += buf_sz;

    for (scale = 0; scale < 4; scale++) {
        const float *filter = vif_filter1d_table[scale];
        int filter_width = vif_filter1d_width1[scale];

        int buf_valid_w = w;
        int buf_valid_h = h;

        if (scale > 0) {
            vif_filter1d(filter, curr_ref_scale, mu1, temp_buf, w, h,
                         curr_ref_stride, buf_stride, filter_width, temp);
            vif_filter1d(filter, curr_main_scale, mu2, temp_buf, w, h,
                         curr_main_stride, buf_stride, filter_width, temp);

            vif_dec2(mu1, ref_scale, buf_valid_w, buf_valid_h, buf_stride,
                     buf_stride);
            vif_dec2(mu2, main_scale, buf_valid_w, buf_valid_h, buf_stride,
                     buf_stride);

            w  = buf_valid_w / 2;
            h  = buf_valid_h / 2;

            buf_valid_w = w;
            buf_valid_h = h;

            curr_ref_scale = ref_scale;
            curr_main_scale = main_scale;

            curr_ref_stride = buf_stride;
            curr_main_stride = buf_stride;
        }

        vif_filter1d(filter, curr_ref_scale, mu1, temp_buf, w, h, curr_ref_stride,
                     buf_stride, filter_width, temp);
        vif_filter1d(filter, curr_main_scale, mu2, temp_buf, w, h, curr_main_stride,
                     buf_stride, filter_width, temp);

        vif_xx_yy_xy(mu1, mu2, mu1_sq, mu2_sq, mu1_mu2, w, h, buf_stride,
                     buf_stride, buf_stride, buf_stride, buf_stride);

        vif_xx_yy_xy(curr_ref_scale, curr_main_scale, ref_sq, main_sq, ref_main,
                     w, h, curr_ref_stride, curr_main_stride, buf_stride,
                     buf_stride, buf_stride);

        vif_filter1d(filter, ref_sq, ref_sq_filt, temp_buf, w, h, buf_stride,
                     buf_stride, filter_width, temp);
        vif_filter1d(filter, main_sq, main_sq_filt, temp_buf, w, h, buf_stride,
                     buf_stride, filter_width, temp);
        vif_filter1d(filter, ref_main, ref_main_filt, temp_buf, w, h, buf_stride,
                     buf_stride, filter_width, temp);

        vif_statistic(mu1_sq, mu2_sq, mu1_mu2, ref_sq_filt, main_sq_filt,
                      ref_main_filt, num_array, den_array, w, h, buf_stride,
                      buf_stride, buf_stride, buf_stride, buf_stride,
                      buf_stride, buf_stride, buf_stride);

        num = vif_sum(num_array, buf_valid_w, buf_valid_h, buf_stride);
        den = vif_sum(den_array, buf_valid_w, buf_valid_h, buf_stride);

        scores[2*scale] = num;
        scores[2*scale+1] = den;
    }

    *score_num = 0.0;
    *score_den = 0.0;
    for (scale = 0; scale < 4; ++scale) {
        *score_num += scores[2*scale];
        *score_den += scores[2*scale+1];
    }

    if (*score_den == 0.0) {
        *score = 1.0f;
    } else {
        *score = (*score_num) / (*score_den);
    }

    ret = 0;

    return ret;
}

#define offset_fn(type, bits) \
    static void offset_##bits##bit(VIFContext *s, const AVFrame *ref, AVFrame *main, int stride) \
{ \
    int w = s->width; \
    int h = s->height; \
    int i,j; \
    \
    int ref_stride = ref->linesize[0]; \
    int main_stride = main->linesize[0]; \
    \
    const type *ref_ptr = (const type *) ref->data[0]; \
    const type *main_ptr = (const type *) main->data[0]; \
    \
    float *ref_ptr_data = s->ref_data; \
    float *main_ptr_data = s->main_data; \
    \
    for(i = 0; i < h; i++) { \
        for(j = 0; j < w; j++) { \
            ref_ptr_data[j] = (float) ref_ptr[j] + OPT_RANGE_PIXEL_OFFSET; \
            main_ptr_data[j] = (float) main_ptr[j] + OPT_RANGE_PIXEL_OFFSET; \
        } \
        ref_ptr += ref_stride / sizeof(type); \
        ref_ptr_data += stride / sizeof(float); \
        main_ptr += main_stride / sizeof(type); \
        main_ptr_data += stride / sizeof(float); \
    } \
}

offset_fn(uint8_t, 8);
offset_fn(uint16_t, 10);

static void set_meta(AVDictionary **metadata, const char *key, float d)
{
    char value[128];
    snprintf(value, sizeof(value), "%0.2f", d);
    av_dict_set(metadata, key, value, 0);
}

static AVFrame *do_vif(AVFilterContext *ctx, AVFrame *main, const AVFrame *ref)
{
    VIFContext *s = ctx->priv;
    AVDictionary **metadata = &main->metadata;

    double score = 0.0;
    double score_num = 0.0;
    double score_den = 0.0;
    double scores[8];

    int w = s->width;
    int h = s->height;

    double stride;

    stride = ALIGN_CEIL(w * sizeof(float));

    /** Offset ref and main pixel by OPT_RANGE_PIXEL_OFFSET */
    if (s->desc->comp[0].depth <= 8) {
        offset_8bit(s, ref, main, stride);
    } else {
        offset_10bit(s, ref, main, stride);
    }

    compute_vif2(s->ref_data, s->main_data, w, h, stride, stride, &score,
                 &score_num, &score_den, scores, s->data_buf, s->temp);

    set_meta(metadata, "lavfi.vif.score", score);

    s->nb_frames++;

    s->vif_sum += score;

    return main;
}

static av_cold int init(AVFilterContext *ctx)
{
    VIFContext *s = ctx->priv;

    s->dinput.process = do_vif;

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
    VIFContext *s = ctx->priv;
    int stride;
    size_t data_sz;

    if (ctx->inputs[0]->w != ctx->inputs[1]->w ||
        ctx->inputs[0]->h != ctx->inputs[1]->h) {
        av_log(ctx, AV_LOG_ERROR, "Width and height of input videos must be same.\n");
        return AVERROR(EINVAL);
    }
    if (ctx->inputs[0]->format != ctx->inputs[1]->format) {
        av_log(ctx, AV_LOG_ERROR, "Inputs must be of same pixel format.\n");
        return AVERROR(EINVAL);
    }

    s->desc = av_pix_fmt_desc_get(inlink->format);
    s->width = ctx->inputs[0]->w;
    s->height = ctx->inputs[0]->h;

    stride = ALIGN_CEIL(s->width * sizeof(float));
    data_sz = (size_t)stride * s->height;

    if (SIZE_MAX / data_sz < 15) {
        av_log(ctx, AV_LOG_ERROR, "error: SIZE_MAX / buf_sz < 15\n");
        return AVERROR(EINVAL);
    }

    if (!(s->data_buf = av_malloc(data_sz * 16))) {
        av_log(ctx, AV_LOG_ERROR, "error: av_malloc failed for data_buf.\n");
        return AVERROR(ENOMEM);
    }
    if (!(s->ref_data = av_malloc(data_sz))) {
        av_log(ctx, AV_LOG_ERROR, "error: av_malloc failed for ref_data.\n");
        return AVERROR(ENOMEM);
    }
    if (!(s->main_data = av_malloc(data_sz))) {
        av_log(ctx, AV_LOG_ERROR, "error: av_malloc failed for main_data.\n");
        return AVERROR(ENOMEM);
    }
    if (!(s->temp = av_malloc(s->width * sizeof(float)))) {
        av_log(ctx, AV_LOG_ERROR, "error: av_malloc failed for temp.\n");
        return AVERROR(ENOMEM);
    }

    return 0;
}


static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VIFContext *s = ctx->priv;
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
    VIFContext *s = inlink->dst->priv;
    return ff_dualinput_filter_frame(&s->dinput, inlink, inpicref);
}

static int request_frame(AVFilterLink *outlink)
{
    VIFContext *s = outlink->src->priv;
    return ff_dualinput_request_frame(&s->dinput, outlink);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    VIFContext *s = ctx->priv;

    if (s->nb_frames > 0) {
        av_log(ctx, AV_LOG_INFO, "VIF AVG: %.3f\n", s->vif_sum / s->nb_frames);
    }

    av_free(s->data_buf);
    av_free(s->ref_data);
    av_free(s->main_data);
    av_free(s->temp);

    ff_dualinput_uninit(&s->dinput);
}

static const AVFilterPad vif_inputs[] = {
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

static const AVFilterPad vif_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_vif = {
    .name          = "vif",
    .description   = NULL_IF_CONFIG_SMALL("Calculate the VIF between two video streams."),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(VIFContext),
    .priv_class    = &vif_class,
    .inputs        = vif_inputs,
    .outputs       = vif_outputs,
};
