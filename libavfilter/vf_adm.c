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
 * Calculate the ADM between two input videos.
 */

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "framesync2.h"
#include "internal.h"
#include "adm.h"
#include "video.h"
#include <emmintrin.h>

typedef struct ADMContext {
    const AVClass *class;
    FFFrameSync fs;
    const AVPixFmtDescriptor *desc;
    int width;
    int height;
    int16_t *data_buf;
    int16_t *temp_lo;
    int16_t *temp_hi;
    double adm_sum;
    uint64_t nb_frames;
} ADMContext;

static const AVOption adm_options[] = {
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(adm, ADMContext, fs);

#define MAX_ALIGN 32
#define ALIGN_CEIL(x) ((x) + ((x) % MAX_ALIGN ? MAX_ALIGN - (x) % MAX_ALIGN : 0))
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static float rcp(float x)
{
    float xi = _mm_cvtss_f32(_mm_rcp_ss(_mm_load_ss(&x)));
    return xi + xi * (1.0 - x * xi);
}

#define DIVS(n, d) ((n) * rcp(d))

static int32_t get_cube(int16_t val)
{
    return val * val * val;
}

static int16_t adm_sum_cube(const int16_t *x, int w, int h, ptrdiff_t stride,
                            double border_factor)
{
    ptrdiff_t px_stride = stride / sizeof(int16_t);
    int left = w * border_factor - 0.5;
    int top = h * border_factor - 0.5;
    int right = w - left;
    int bottom = h - top;

    int i, j;

    int sum = 0;

    for (i = top; i < bottom; i++) {
        for (j = left; j < right; j++) {
            sum += get_cube(FFABS(x[i * px_stride + j]));
        }
    }

    return ceil(cbrt(sum)) + ceil(cbrt((bottom - top) * (right - left) / 32.0));
}

static void adm_decouple(const adm_dwt_band_t *ref, const adm_dwt_band_t *main,
                         const adm_dwt_band_t *r, const adm_dwt_band_t *a,
                         int w, int h, ptrdiff_t ref_stride, ptrdiff_t main_stride,
                         ptrdiff_t r_stride, ptrdiff_t a_stride)
{
    const float cos_1deg_sq = cos(1.0 * M_PI / 180.0) * cos(1.0 * M_PI / 180.0);
    const float eps = 1e-30;

    ptrdiff_t ref_px_stride = ref_stride / sizeof(int16_t);
    ptrdiff_t main_px_stride = main_stride / sizeof(int16_t);
    ptrdiff_t r_px_stride = r_stride / sizeof(int16_t);
    ptrdiff_t a_px_stride = a_stride / sizeof(int16_t);

    int oh, ov, od, th, tv, td;
    float kh, kv, kd, tmph, tmpv, tmpd;
    float ot_dp, o_mag_sq, t_mag_sq;
    int angle_flag;
    int i, j;

    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            oh = ref->band_h[i * ref_px_stride + j];
            ov = ref->band_v[i * ref_px_stride + j];
            od = ref->band_d[i * ref_px_stride + j];
            th = main->band_h[i * main_px_stride + j];
            tv = main->band_v[i * main_px_stride + j];
            td = main->band_d[i * main_px_stride + j];

            kh = DIVS(th, oh + eps);
            kv = DIVS(tv, ov + eps);
            kd = DIVS(td, od + eps);

            kh = kh < 0 ? 0 : (kh > 1 ? 1 : kh);
            kv = kv < 0 ? 0 : (kv > 1 ? 1 : kv);
            kd = kd < 0 ? 0 : (kd > 1 ? 1 : kd);

            tmph = kh * oh;
            tmpv = kv * ov;
            tmpd = kd * od;

            ot_dp = oh * th + ov * tv;
            o_mag_sq = oh * oh + ov * ov;
            t_mag_sq = th * th + tv * tv;

            angle_flag = (ot_dp >= 0) && (ot_dp * ot_dp >= cos_1deg_sq *
                                          o_mag_sq * t_mag_sq);

            if (angle_flag) {
                tmph = th;
                tmpv = tv;
                tmpd = td;
            }

            r->band_h[i * r_px_stride + j] = ceil(tmph);
            r->band_v[i * r_px_stride + j] = ceil(tmpv);
            r->band_d[i * r_px_stride + j] = ceil(tmpd);

            a->band_h[i * a_px_stride + j] = ceil(th - tmph);
            a->band_v[i * a_px_stride + j] = ceil(tv - tmpv);
            a->band_d[i * a_px_stride + j] = ceil(td - tmpd);
        }
    }
}

static void adm_csf(const adm_dwt_band_t *src, const adm_dwt_band_t *dst,
                    int orig_h, int scale, int w, int h, ptrdiff_t src_stride,
                    ptrdiff_t dst_stride)
{
    const int16_t *src_angles[3] = { src->band_h, src->band_v, src->band_d };
    int16_t *dst_angles[3] = { dst->band_h, dst->band_v, dst->band_d };

    const int16_t *src_ptr;
    int16_t *dst_ptr;

    ptrdiff_t src_px_stride = src_stride / sizeof(int16_t);
    ptrdiff_t dst_px_stride = dst_stride / sizeof(int16_t);

    uint16_t rfactor[3] = {lrint((1.0 / Q[scale][0]) * (1 << N)),
        lrint((1.0 / Q[scale][0]) * (1 << N)),
        lrint((1.0 / Q[scale][1]) * (1 << N))};

    int i, j, theta;

    for (theta = 0; theta < 3; theta++) {
        src_ptr = src_angles[theta];
        dst_ptr = dst_angles[theta];

        for (i = 0; i < h; i++) {
            for (j = 0; j < w; j++) {
                dst_ptr[i * dst_px_stride + j] = (rfactor[theta] *
                                                  src_ptr[i * src_px_stride + j]) >> N;
            }
        }
    }
}

static void adm_cm_thresh(const adm_dwt_band_t *src, int16_t *dst, int w, int h,
                          ptrdiff_t src_stride, ptrdiff_t dst_stride)
{
    const int16_t *angles[3] = { src->band_h, src->band_v, src->band_d };
    const int16_t *src_ptr;

    ptrdiff_t src_px_stride = src_stride / sizeof(int16_t);
    ptrdiff_t dst_px_stride = dst_stride / sizeof(int16_t);

    int filt_coeff, img_coeff;

    int theta, i, j, filt_i, filt_j, src_i, src_j;

    for (i = 0; i < h; i++) {

        for (j = 0; j < w; j++) {
            dst[i * dst_px_stride + j] = 0;
        }

        for (theta = 0; theta < 3; ++theta) {
            src_ptr = angles[theta];

            for (j = 0; j < w; j++) {
                int sum = 0;

                for (filt_i = 0; filt_i < 3; filt_i++) {
                    for (filt_j = 0; filt_j < 3; filt_j++) {
                        filt_coeff = (lrint((filt_i == 1 && filt_j == 1) ? 1.0 /
                                            15.0 : 1.0 / 30.0) * (1 << N));

                        src_i = i - 1 + filt_i;
                        src_j = j - 1 + filt_j;

                        src_i = FFABS(src_i);
                        if (src_i >= h) {
                            src_i = 2 * h - src_i - 1;
                        }
                        src_j = FFABS(src_j);
                        if (src_j >= w) {
                            src_j = 2 * w - src_j - 1;
                        }
                        img_coeff = FFABS(src_ptr[src_i * src_px_stride + src_j]);

                        sum += filt_coeff * img_coeff;
                    }
                }

                dst[i * dst_px_stride + j] += sum >> N;
            }
        }
    }
}

static void adm_cm(const adm_dwt_band_t *src, const adm_dwt_band_t *dst,
                   const int16_t *thresh, int w, int h, ptrdiff_t src_stride,
                   ptrdiff_t dst_stride, ptrdiff_t thresh_stride)
{
    ptrdiff_t src_px_stride = src_stride / sizeof(int16_t);
    ptrdiff_t dst_px_stride = dst_stride / sizeof(int16_t);
    ptrdiff_t thresh_px_stride = thresh_stride / sizeof(int16_t);

    int xh, xv, xd, thr;

    int i, j;

    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j++) {
            xh  = src->band_h[i * src_px_stride + j];
            xv  = src->band_v[i * src_px_stride + j];
            xd  = src->band_d[i * src_px_stride + j];
            thr = thresh[i * thresh_px_stride + j];

            xh = FFABS(xh) - thr;
            xv = FFABS(xv) - thr;
            xd = FFABS(xd) - thr;

            xh = xh < 0 ? 0 : xh;
            xv = xv < 0 ? 0 : xv;
            xd = xd < 0 ? 0 : xd;

            dst->band_h[i * dst_px_stride + j] = xh;
            dst->band_v[i * dst_px_stride + j] = xv;
            dst->band_d[i * dst_px_stride + j] = xd;
        }
    }
}

#define adm_dwt2_fn(type, bits) \
    static void adm_dwt2_##bits##bit(const type *src, const adm_dwt_band_t *dst, \
                                     int w, int h, ptrdiff_t src_stride, \
                                     ptrdiff_t dst_stride, int16_t *temp_lo, \
                                     int16_t* temp_hi) \
{ \
    const int32_t *filter_lo = dwt2_db2_coeffs_lo_int; \
    const int32_t *filter_hi = dwt2_db2_coeffs_hi_int; \
    int filt_w = sizeof(dwt2_db2_coeffs_lo_int) / sizeof(int); \
    \
    ptrdiff_t src_px_stride = src_stride / sizeof(type); \
    ptrdiff_t dst_px_stride = dst_stride / sizeof(int16_t); \
    \
    int filt_coeff_lo, filt_coeff_hi, img_coeff; \
    \
    int i, j, filt_i, filt_j, src_i, src_j; \
    \
    for (i = 0; i < (h + 1) / 2; i++) { \
        /** Vertical pass. */ \
        for (j = 0; j < w; j++) { \
            int sum_lo = 0; \
            int sum_hi = 0; \
            \
            for (filt_i = 0; filt_i < filt_w; filt_i++) { \
                filt_coeff_lo = filter_lo[filt_i]; \
                filt_coeff_hi = filter_hi[filt_i]; \
                \
                src_i = 2 * i - 1 + filt_i; \
                \
                src_i = FFABS(src_i); \
                if (src_i >= h) { \
                    src_i = 2 * h - src_i - 1; \
                } \
                \
                img_coeff = src[src_i * src_px_stride + j]; \
                \
                sum_lo += filt_coeff_lo * img_coeff; \
                sum_hi += filt_coeff_hi * img_coeff; \
            } \
            \
            temp_lo[j] = sum_lo >> N; \
            temp_hi[j] = sum_hi >> N; \
        } \
        \
        /** Horizontal pass (lo). */ \
        for (j = 0; j < (w + 1) / 2; j++) { \
            int sum_lo = 0; \
            int sum_hi = 0; \
            \
            for (filt_j = 0; filt_j < filt_w; filt_j++) { \
                filt_coeff_lo = filter_lo[filt_j]; \
                filt_coeff_hi = filter_hi[filt_j]; \
                \
                src_j = 2 * j - 1 + filt_j; \
                \
                src_j = FFABS(src_j); \
                if (src_j >= w) { \
                    src_j = 2 * w - src_j - 1; \
                } \
                \
                img_coeff = temp_lo[src_j]; \
                \
                sum_lo += filt_coeff_lo * img_coeff; \
                sum_hi += filt_coeff_hi * img_coeff; \
            } \
            \
            dst->band_a[i * dst_px_stride + j] = sum_lo >> N; \
            dst->band_v[i * dst_px_stride + j] = sum_hi >> N; \
        } \
        \
        /** Horizontal pass (hi). */ \
        for (j = 0; j < (w + 1) / 2; j++) { \
            int sum_lo = 0; \
            int sum_hi = 0; \
            \
            for (filt_j = 0; filt_j < filt_w; filt_j++) { \
                filt_coeff_lo = filter_lo[filt_j]; \
                filt_coeff_hi = filter_hi[filt_j]; \
                \
                src_j = 2 * j - 1 + filt_j; \
                \
                src_j = FFABS(src_j); \
                if (src_j >= w) { \
                    src_j = 2 * w - src_j - 1; \
                } \
                \
                img_coeff = temp_hi[src_j]; \
                \
                sum_lo += filt_coeff_lo * img_coeff; \
                sum_hi += filt_coeff_hi * img_coeff; \
            } \
            \
            dst->band_h[i * dst_px_stride + j] = sum_lo >> N; \
            dst->band_d[i * dst_px_stride + j] = sum_hi >> N; \
        } \
    } \
}

adm_dwt2_fn(uint8_t, 8);
adm_dwt2_fn(uint16_t, 10);
adm_dwt2_fn(int16_t, 32);

static void adm_buffer_copy(const void *src, void *dst, int linewidth, int h,
                            ptrdiff_t src_stride, ptrdiff_t dst_stride)
{
    const char *src_p = src;
    char *dst_p = dst;
    int i;

    for (i = 0; i < h; i++) {
        memcpy(dst_p, src_p, linewidth);
        src_p += src_stride;
        dst_p += dst_stride;
    }
}

static char *init_dwt_band(adm_dwt_band_t *band, char *data_top, size_t buf_sz)
{
    band->band_a = (int16_t *) data_top;
    data_top += buf_sz;
    band->band_h = (int16_t *) data_top;
    data_top += buf_sz;
    band->band_v = (int16_t *) data_top;
    data_top += buf_sz;
    band->band_d = (int16_t *) data_top;
    data_top += buf_sz;
    return data_top;
}

int compute_adm2(const void *ref, const void *main, int w, int h,
                 ptrdiff_t ref_stride, ptrdiff_t main_stride, double *score,
                 double *score_num, double *score_den, double *scores,
                 int16_t *data_buf, int16_t *temp_lo, int16_t *temp_hi,
                 uint8_t type)
{
    double numden_limit = 1e-2 * (w * h) / (1920.0 * 1080.0);

    char *data_top;

    int16_t *ref_scale;
    int16_t *main_scale;

    adm_dwt_band_t ref_dwt2;
    adm_dwt_band_t main_dwt2;

    adm_dwt_band_t decouple_r;
    adm_dwt_band_t decouple_a;

    adm_dwt_band_t csf_o;
    adm_dwt_band_t csf_r;
    adm_dwt_band_t csf_a;

    int16_t *mta;

    adm_dwt_band_t cm_r;

    const void *curr_ref_scale = ref;
    const void *curr_main_scale = main;
    ptrdiff_t curr_ref_stride = ref_stride;
    ptrdiff_t curr_main_stride = main_stride;

    int orig_h = h;

    ptrdiff_t buf_stride = ALIGN_CEIL(((w + 1) / 2) * sizeof(int16_t));
    size_t buf_sz = (size_t)buf_stride * ((h + 1) / 2);

    double num = 0;
    double den = 0;

    int scale;
    int ret = 1;

    data_top = (char *) (data_buf);

    ref_scale = (int16_t *) data_top;
    data_top += buf_sz;
    main_scale = (int16_t *) data_top;
    data_top += buf_sz;

    data_top = init_dwt_band(&ref_dwt2, data_top, buf_sz);
    data_top = init_dwt_band(&main_dwt2, data_top, buf_sz);
    data_top = init_dwt_band(&decouple_r, data_top, buf_sz);
    data_top = init_dwt_band(&decouple_a, data_top, buf_sz);
    data_top = init_dwt_band(&csf_o, data_top, buf_sz);
    data_top = init_dwt_band(&csf_r, data_top, buf_sz);
    data_top = init_dwt_band(&csf_a, data_top, buf_sz);

    mta = (int16_t *) data_top;
    data_top += buf_sz;

    data_top = init_dwt_band(&cm_r, data_top, buf_sz);

    for (scale = 0; scale < 4; scale++) {
        float num_scale = 0.0;
        float den_scale = 0.0;

        if(!scale) {
            if(type <= 8) {
                adm_dwt2_8bit((const uint8_t *) curr_ref_scale, &ref_dwt2, w,
                              h, curr_ref_stride, buf_stride, temp_lo, temp_hi);
                adm_dwt2_8bit((const uint8_t *) curr_main_scale, &main_dwt2, w,
                              h, curr_main_stride, buf_stride, temp_lo, temp_hi);
            } else {
                adm_dwt2_10bit((const uint16_t *) curr_ref_scale, &ref_dwt2, w,
                               h, curr_ref_stride, buf_stride, temp_lo, temp_hi);
                adm_dwt2_10bit((const uint16_t *) curr_main_scale, &main_dwt2, w,
                               h, curr_main_stride, buf_stride, temp_lo, temp_hi);
            }
        } else{
            adm_dwt2_32bit((const int16_t *) curr_ref_scale, &ref_dwt2, w, h,
                           curr_ref_stride, buf_stride, temp_lo, temp_hi);
            adm_dwt2_32bit((const int16_t *) curr_main_scale, &main_dwt2, w, h,
                           curr_main_stride, buf_stride, temp_lo, temp_hi);
        }

        w = (w + 1) / 2;
        h = (h + 1) / 2;

        adm_decouple(&ref_dwt2, &main_dwt2, &decouple_r, &decouple_a, w, h,
                     buf_stride, buf_stride, buf_stride, buf_stride);

        adm_csf(&ref_dwt2, &csf_o, orig_h, scale, w, h, buf_stride, buf_stride);
        adm_csf(&decouple_r, &csf_r, orig_h, scale, w, h, buf_stride, buf_stride);
        adm_csf(&decouple_a, &csf_a, orig_h, scale, w, h, buf_stride, buf_stride);

        adm_cm_thresh(&csf_a, mta, w, h, buf_stride, buf_stride);
        adm_cm(&csf_r, &cm_r, mta, w, h, buf_stride, buf_stride, buf_stride);

        num_scale += adm_sum_cube(cm_r.band_h, w, h, buf_stride, ADM_BORDER_FACTOR);
        num_scale += adm_sum_cube(cm_r.band_v, w, h, buf_stride, ADM_BORDER_FACTOR);
        num_scale += adm_sum_cube(cm_r.band_d, w, h, buf_stride, ADM_BORDER_FACTOR);

        den_scale += adm_sum_cube(csf_o.band_h, w, h, buf_stride, ADM_BORDER_FACTOR);
        den_scale += adm_sum_cube(csf_o.band_v, w, h, buf_stride, ADM_BORDER_FACTOR);
        den_scale += adm_sum_cube(csf_o.band_d, w, h, buf_stride, ADM_BORDER_FACTOR);

        num += num_scale;
        den += den_scale;

        adm_buffer_copy(ref_dwt2.band_a, ref_scale, w * sizeof(int16_t), h,
                        buf_stride, buf_stride);
        adm_buffer_copy(main_dwt2.band_a, main_scale, w * sizeof(int16_t), h,
                        buf_stride, buf_stride);

        curr_ref_scale = ref_scale;
        curr_main_scale = main_scale;
        curr_ref_stride = buf_stride;
        curr_main_stride = buf_stride;

        scores[2 * scale + 0] = num_scale;
        scores[2 * scale + 1] = den_scale;
    }

    num = num < numden_limit ? 0 : num;
    den = den < numden_limit ? 0 : den;

    if (den == 0.0) {
        *score = 1.0;
    } else {
        *score = num / den;
    }
    *score_num = num;
    *score_den = den;

    ret = 0;

    return ret;
}

static void set_meta(AVDictionary **metadata, const char *key, float d)
{
    char value[128];
    snprintf(value, sizeof(value), "%0.2f", d);
    av_dict_set(metadata, key, value, 0);
}

static int do_vmaf(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    ADMContext *s = ctx->priv;
    AVFrame *main, *ref;
    AVDictionary **metadata;
    int ret;
    double score = 0.0;
    double score_num = 0;
    double score_den = 0;
    double scores[2 * 4];

    int w = s->width;
    int h = s->height;

    ptrdiff_t ref_stride, main_stride;

    ret = ff_framesync2_dualinput_get(fs, &main, &ref);
    if (ret < 0)
        return ret;
    if (!ref)
        return ff_filter_frame(ctx->outputs[0], main);

    metadata = &main->metadata;

    ref_stride = ref->linesize[0];
    main_stride = main->linesize[0];

    compute_adm2(ref->data[0], main->data[0], w, h, ref_stride, main_stride,
                 &score, &score_num, &score_den, scores, s->data_buf, s->temp_lo,
                 s->temp_hi, s->desc->comp[0].depth);

    set_meta(metadata, "lavfi.adm.score", score);

    s->nb_frames++;

    s->adm_sum += score;

    return ff_filter_frame(ctx->outputs[0], main);
}

static av_cold int init(AVFilterContext *ctx)
{
    ADMContext *s = ctx->priv;

    int i;
    for(i = 0; i < 4; i++) {
        dwt2_db2_coeffs_lo_int[i] = lrint(dwt2_db2_coeffs_lo[i] * (1 << N));
        dwt2_db2_coeffs_hi_int[i] = lrint(dwt2_db2_coeffs_hi[i] * (1 << N));
    }

    s->fs.on_event = do_vmaf;

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
    ADMContext *s = ctx->priv;
    ptrdiff_t buf_stride;
    size_t buf_sz;
    ptrdiff_t stride;

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

    buf_stride = ALIGN_CEIL(((s->width + 1) / 2) * sizeof(int16_t));
    buf_sz = (size_t)buf_stride * ((s->height + 1) / 2);

    if (SIZE_MAX / buf_sz < 35) {
        av_log(ctx, AV_LOG_ERROR, "error: SIZE_MAX / buf_sz_one < 35");
        return AVERROR(EINVAL);
    }

    if (!(s->data_buf = av_malloc(buf_sz * 35))) {
        return AVERROR(ENOMEM);
    }

    stride = ALIGN_CEIL(s->width * sizeof(int16_t));
    if (!(s->temp_lo = av_malloc(stride))) {
        return AVERROR(ENOMEM);
    }

    if (!(s->temp_hi = av_malloc(stride))) {
        return AVERROR(ENOMEM);
    }

    return 0;
}


static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ADMContext *s = ctx->priv;
    AVFilterLink *mainlink = ctx->inputs[0];
    int ret;

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

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    ADMContext *s = ctx->priv;
    return ff_framesync2_activate(&s->fs);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ADMContext *s = ctx->priv;

    ff_framesync2_uninit(&s->fs);

    if (s->nb_frames > 0) {
        av_log(ctx, AV_LOG_INFO, "ADM AVG: %.3f\n", s->adm_sum / s->nb_frames);
    }

    av_free(s->data_buf);
    av_free(s->temp_lo);
    av_free(s->temp_hi);
}

static const AVFilterPad adm_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
    },{
        .name         = "reference",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_ref,
    },
    { NULL }
};

static const AVFilterPad adm_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_vf_adm = {
    .name          = "adm",
    .description   = NULL_IF_CONFIG_SMALL("Calculate the ADM score between two video streams."),
    .preinit       = adm_framesync_preinit,
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .activate      = activate,
    .priv_size     = sizeof(ADMContext),
    .priv_class    = &adm_class,
    .inputs        = adm_inputs,
    .outputs       = adm_outputs,
};
