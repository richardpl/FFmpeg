/*
 * Copyright (c) 2021 Paul B Mahol
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
 * Pixel warp filter
 */

#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

enum WarpEdge {
    CLIP_EDGE,
    FIXED_EDGE,
    NB_WARPEDGE,
};

typedef struct WarpPoints {
    float x0, y0, x1, y1;
} WarpPoints;

typedef struct Matrix {
    int m, n;
    float *t;
} Matrix;

typedef struct WarpContext {
    const AVClass *class;
    char *points_str;

    int mode;
    int interpolation;
    int edge;
    int nb_points;
    int nb_planes;

    double *points;
    int points_size;

    int nb_warp_points;
    WarpPoints *warp_points;

    int black[4];

    int elements;
    int uv_linesize;
    int16_t *u, *v, *du, *dv;

    int (*warp_slice)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);

    void (*remap_line)(uint8_t *dst, int width, int height,
                       const uint8_t *const src, ptrdiff_t in_linesize,
                       const int16_t *const u, const int16_t *const v,
                       const int16_t *const du, const int16_t *const dv,
                       int fixed);
} WarpContext;

#define OFFSET(x) offsetof(WarpContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption warp_options[] = {
    { "points", "set warp points", OFFSET(points_str), AV_OPT_TYPE_STRING, {.str="0 0 0 0"}, 0, 0, FLAGS },
    { "mode", "set warp mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "mode" },
    { "abs", "absolute", 0, AV_OPT_TYPE_CONST, {.i64=0}, 0, 1, FLAGS, "mode" },
    { "rel", "relative", 0, AV_OPT_TYPE_CONST, {.i64=1}, 0, 1, FLAGS, "mode" },
    { "interpolation", "set interpolation", OFFSET(interpolation), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS },
    { "edge", "set edge mode", OFFSET(edge), AV_OPT_TYPE_INT, {.i64=FIXED_EDGE}, 0, NB_WARPEDGE-1, FLAGS, "edge" },
    { "clip",  "clip edge",   0, AV_OPT_TYPE_CONST, {.i64=CLIP_EDGE},  0, 1, FLAGS, "edge" },
    { "fixed", "fixed color", 0, AV_OPT_TYPE_CONST, {.i64=FIXED_EDGE}, 0, 1, FLAGS, "edge" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(warp);

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRAP,
        AV_PIX_FMT_YUV444P9, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV444P12,
        AV_PIX_FMT_YUV444P14, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_YUVA444P9, AV_PIX_FMT_YUVA444P10, AV_PIX_FMT_YUVA444P12, AV_PIX_FMT_YUVA444P16,
        AV_PIX_FMT_GBRP9, AV_PIX_FMT_GBRP10, AV_PIX_FMT_GBRP12,
        AV_PIX_FMT_GBRP14, AV_PIX_FMT_GBRP16,
        AV_PIX_FMT_GBRAP10, AV_PIX_FMT_GBRAP12, AV_PIX_FMT_GBRAP16,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY9,
        AV_PIX_FMT_GRAY10, AV_PIX_FMT_GRAY12,
        AV_PIX_FMT_GRAY14, AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int create_matrix(Matrix *matrix, int m, int n)
{
    matrix->t = av_calloc(m * n, sizeof(*matrix->t));
    if (!matrix->t)
        return AVERROR(ENOMEM);

    matrix->n = n;
    matrix->m = m;

    return 0;
}

static void free_matrix(Matrix *matrix)
{
    av_freep(&matrix->t);

    matrix->n = 0;
    matrix->m = 0;
}

static void multiply(Matrix *a, Matrix *b, Matrix *c)
{
    for (int i = 0; i < c->m; i++) {
        for (int j = 0; j < c->n; j++) {
            c->t[i * c->n + j] = 0.f;

            for (int k = 0; k < a->n; k++)
                c->t[i * c->n + j] += a->t[i * a->n + k] * b->t[k * b->n + j];
        }
    }
}

static void inverse(Matrix *matrix)
{
    float pv, pav, temp, tt;
    int i, ik, j, jk, k;
    float det;
    int n = matrix->n;

    float *pc = av_calloc(n, sizeof(*pc));
    float *pl = av_calloc(n, sizeof(*pl));
    float *cs = av_calloc(n, sizeof(*cs));

    if (!pc || !pl || !cs)
        goto fail;

    det = 1.f;

    for (k = 0; k < n; k++) {
        pv = matrix->t[k * n + k];
        ik = k;
        jk = k;
        pav = fabsf(pv);
        for (i = k; i < n; i++) {
            for (j = k; j < n; j++) {
                temp = fabsf(matrix->t[i * n + j]);
                if (temp > pav) {
                    pv = matrix->t[i * n + j];
                    pav = fabsf(pv);
                    ik = i;
                    jk = j;
                }
            }
        }

        pc[k] = jk;
        pl[k] = ik;

        if (ik != k)
            det = -det;
        if (jk != k)
            det = -det;

        det  = det * pv;
        temp = fabsf(det);

        if (ik != k) {
            for (i = 0; i < n; i++) {
                tt = matrix->t[ik * n + i];
                matrix->t[ik * n + i] = matrix->t[k * n + i];
                matrix->t[k * n + i] = tt;
            }
        }

        if (jk != k) {
            for (i = 0; i < n; i++) {
                tt = matrix->t[i * n + jk];
                matrix->t[i * n + jk] = matrix->t[i * n + k];
                matrix->t[i * n + k] = tt;
            }
        }

        for (i = 0; i < n; i++) {
            cs[i] = matrix->t[i * n + k];
            matrix->t[i * n + k] = 0;
        }

        cs[k] = 0;
        matrix->t[k * n + k] = 1;

        temp = fabsf(pv);

        for (i = 0; i < n; i++)
            matrix->t[k * n + i] = matrix->t[k * n + i] / pv;

        for (j = 0; j < n; j++) {
            if (j == k)
                j++;
            if (j < n) {
                for (i = 0; i < n; i++) {
                    matrix->t[j * n + i] = matrix->t[j * n + i] - cs[j] * matrix->t[k * n + i];
                }
            }
        }
    }

    for (i = n - 1; i >= 0; i--) {
        ik = (int)pc[i];
        if (ik != i) {
            for (j = 0; j < n; j++) {
                tt = matrix->t[i * n + j];
                matrix->t[i * n + j] = matrix->t[ik * n + j];
                matrix->t[ik * n + j] = tt;
            }
        }
    }

    for (j = n - 1; j >= 0; j--) {
        jk = (int)pl[j];
        if (jk != j) {
            for (i = 0; i < n; i++) {
                tt = matrix->t[i * n + j];
                matrix->t[i * n + j] = matrix->t[i * n + jk];
                matrix->t[i * n + jk] = tt;
            }
        }
    }

fail:

    av_free(pc);
    av_free(pl);
    av_free(cs);
}

static void warp_remap(WarpContext *s, Matrix *vox, Matrix *voy, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float bx = x;
            float by = y;
            float dx, dy;

            float ox = vox->t[s->nb_warp_points] +
                       vox->t[s->nb_warp_points + 1] * bx +
                       vox->t[s->nb_warp_points + 2] * by;
            float oy = voy->t[s->nb_warp_points] +
                       voy->t[s->nb_warp_points + 1] * bx +
                       voy->t[s->nb_warp_points + 2] * by;

            for (int i = 0; i < s->nb_warp_points; i++) {
                float t = s->warp_points[i].x0 - bx;
                float d = t * t;

                t = s->warp_points[i].y0 - by;
                d += t * t;
                if (d > 0)
                    d = d * logf(d) * 0.5f;
                ox += vox->t[i] * d;
                oy += voy->t[i] * d;
            }

            ox += bx;
            oy += by;

            dx = ox - floorf(ox);
            dy = oy - floorf(oy);

            ox -= dx;
            oy -= dy;

            s->du[y * s->uv_linesize + x] = dx * (1 << 15);
            s->dv[y * s->uv_linesize + x] = dy * (1 << 15);

            if (s->edge == CLIP_EDGE) {
                s->u[y * s->uv_linesize + x] = av_clip(ox, 0, w - 1);
                s->v[y * s->uv_linesize + x] = av_clip(oy, 0, h - 1);
            } else if (s->edge == FIXED_EDGE) {
                s->u[y * s->uv_linesize + x] = ox >= 0 && ox < w - 1 ? ox : -1;
                s->v[y * s->uv_linesize + x] = oy >= 0 && oy < h - 1 ? oy : -1;
            }
        }
    }
}

#define DEFINE_REMAP1_LINE(bits, div)                                                    \
static void remap1_##bits##bit_line_c(uint8_t *dst, int width, int height,               \
                                      const uint8_t *const src,                          \
                                      ptrdiff_t in_linesize,                             \
                                      const int16_t *const u, const int16_t *const v,    \
                                      const int16_t *const du, const int16_t *const dv,  \
                                      int fixed)                                         \
{                                                                                        \
    const uint##bits##_t *const s = (const uint##bits##_t *const)src;                    \
    uint##bits##_t *d = (uint##bits##_t *)dst;                                           \
                                                                                         \
    in_linesize /= div;                                                                  \
                                                                                         \
    for (int x = 0; x < width; x++)                                                      \
        d[x] = v[x] >= 0 && u[x] >= 0 ? s[v[x] * in_linesize + u[x]] : fixed;            \
}

DEFINE_REMAP1_LINE( 8, 1)
DEFINE_REMAP1_LINE(16, 2)

#define DEFINE_REMAP2_LINE(bits, div)                                                    \
static void remap2_##bits##bit_line_c(uint8_t *dst, int w, int h,                        \
                                      const uint8_t *const src,                          \
                                      ptrdiff_t in_linesize,                             \
                                      const int16_t *const u, const int16_t *const v,    \
                                      const int16_t *const du, const int16_t *const dv,  \
                                      int fixed)                                         \
{                                                                                        \
    const uint##bits##_t *const s = (const uint##bits##_t *const)src;                    \
    uint##bits##_t *d = (uint##bits##_t *)dst;                                           \
                                                                                         \
    in_linesize /= div;                                                                  \
                                                                                         \
    for (int x = 0; x < w; x++) {                                                        \
        const int mapped = v[x] >= 0 && u[x] >= 0;                                       \
        int64_t sum = 0;                                                                 \
                                                                                         \
        if (!mapped) {                                                                   \
            d[x] = fixed;                                                                \
            continue;                                                                    \
        }                                                                                \
                                                                                         \
        {                                                                                \
            int64_t au = du[x];                                                          \
            int64_t av = dv[x];                                                          \
            int64_t zu = (1 << 15) - du[x];                                              \
            int64_t zv = (1 << 15) - dv[x];                                              \
            int ax = u[x];                                                               \
            int ay = v[x];                                                               \
            int bx = FFMIN(ax + 1, w - 1);                                               \
            int by = FFMIN(ay + 1, h - 1);                                               \
            sum += zu * zv * (s[ay * in_linesize + ax]);                                 \
            sum += au * zv * (s[ay * in_linesize + bx]);                                 \
            sum += zu * av * (s[by * in_linesize + ax]);                                 \
            sum += au * av * (s[by * in_linesize + bx]);                                 \
            d[x] = ((sum + (1LL << 29)) >> 30);                                          \
        }                                                                                \
    }                                                                                    \
}

DEFINE_REMAP2_LINE( 8, 1)
DEFINE_REMAP2_LINE(16, 2)

#define DEFINE_REMAP(ws)                                                                \
static int warp##ws##_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)    \
{                                                                                       \
    ThreadData *td = arg;                                                               \
    const WarpContext *s = ctx->priv;                                                   \
    const AVFrame *in = td->in;                                                         \
    AVFrame *out = td->out;                                                             \
                                                                                        \
    for (int plane = 0; plane < s->nb_planes; plane++) {                                \
        const int in_linesize  = in->linesize[plane];                                   \
        const int out_linesize = out->linesize[plane];                                  \
        const int uv_linesize = s->uv_linesize;                                         \
        const uint8_t *const src = in->data[plane];                                     \
        uint8_t *dst = out->data[plane];                                                \
        const int width = in->width;                                                    \
        const int height = in->height;                                                  \
                                                                                        \
        const int slice_start = (height *  jobnr     ) / nb_jobs;                       \
        const int slice_end   = (height * (jobnr + 1)) / nb_jobs;                       \
                                                                                        \
        for (int y = slice_start; y < slice_end; y++) {                                 \
            const int16_t *const u = s->u + y * uv_linesize * ws * ws;                  \
            const int16_t *const v = s->v + y * uv_linesize * ws * ws;                  \
            const int16_t *const du = s->du + y * uv_linesize * ws * ws;                \
            const int16_t *const dv = s->dv + y * uv_linesize * ws * ws;                \
                                                                                        \
            s->remap_line(dst + y * out_linesize,                                       \
                          width, height, src, in_linesize,                              \
                          u, v, du, dv, s->black[plane]);                               \
        }                                                                               \
    }                                                                                   \
                                                                                        \
    return 0;                                                                           \
}

DEFINE_REMAP(1)

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    WarpContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    const int depth = desc->comp[0].depth;
    const int rgb = !!(desc->flags & AV_PIX_FMT_FLAG_RGB);
    char *p = s->points_str;
    double *new_points;
    Matrix L = { 0 }, vx = { 0 }, vy = { 0 };
    Matrix vox = { 0 }, voy = { 0 };
    int sizeof_uv;
    int ret;

    s->nb_points = 0;
    s->nb_planes = av_pix_fmt_count_planes(inlink->format);

    s->black[0] = s->black[3] = 0;
    s->black[1] = s->black[2] = rgb ? 0 : (1 << (depth - 1));

    av_freep(&s->points);
    s->points_size = 0;
    new_points = av_fast_realloc(NULL, &s->points_size, 1 * sizeof(*s->points));
    if (!new_points)
        return AVERROR(ENOMEM);
    s->points = new_points;

    while (p && *p) {
        s->points[s->nb_points++] = av_strtod(p, &p);
        if (p && *p) p++;

        new_points = av_fast_realloc(s->points, &s->points_size, (s->nb_points + 1) * sizeof(*s->points));
        if (!new_points)
            return AVERROR(ENOMEM);
        s->points = new_points;
    }

    if (s->nb_points & 3)
        return AVERROR(EINVAL);

    s->nb_warp_points = s->nb_points / 4;
    s->warp_points = av_calloc(s->nb_warp_points, sizeof(*s->warp_points));

    for (int n = 0; n < s->nb_warp_points; n++) {
        if (s->mode) {
            s->warp_points[n].x0 = s->points[n * 4 + 0] + s->points[n * 4 + 2];
            s->warp_points[n].y0 = s->points[n * 4 + 1] + s->points[n * 4 + 3];
            s->warp_points[n].x1 = -s->points[n * 4 + 2];
            s->warp_points[n].y1 = -s->points[n * 4 + 3];
        } else {
            s->warp_points[n].x0 = s->points[n * 4 + 2];
            s->warp_points[n].y0 = s->points[n * 4 + 3];
            s->warp_points[n].x1 = s->points[n * 4 + 0] - s->points[n * 4 + 2];
            s->warp_points[n].y1 = s->points[n * 4 + 1] - s->points[n * 4 + 3];
        }
    }

    ret = create_matrix(&L, s->nb_warp_points + 3, s->nb_warp_points + 3);
    if (ret < 0)
        goto fail;

    ret = create_matrix(&vx, s->nb_warp_points + 3, 1);
    if (ret < 0)
        goto fail;

    ret = create_matrix(&vy, s->nb_warp_points + 3, 1);
    if (ret < 0)
        goto fail;

    ret = create_matrix(&vox, s->nb_warp_points + 3, 1);
    if (ret < 0)
        goto fail;

    ret = create_matrix(&voy, s->nb_warp_points + 3, 1);
    if (ret < 0)
        goto fail;

    for (int i = 0; i < s->nb_warp_points; i++) {
        for (int j = 0; j < s->nb_warp_points; j++) {
            float t = s->warp_points[i].x0 - s->warp_points[j].x0;
            float d = t * t;

            t  = s->warp_points[i].y0 - s->warp_points[j].y0;
            d += t * t;
            if (d > 0)
                L.t[i * L.n + j] = d * logf(d) * 0.5f;
        }

        L.t[i * L.n + s->nb_warp_points] = 1;
        L.t[i * L.n + s->nb_warp_points + 1] = s->warp_points[i].x0;
        L.t[i * L.n + s->nb_warp_points + 2] = s->warp_points[i].y0;

        L.t[s->nb_warp_points * L.n + i] = 1;
        L.t[(s->nb_warp_points + 1) * L.n + i] = s->warp_points[i].x0;
        L.t[(s->nb_warp_points + 2) * L.n + i] = s->warp_points[i].y0;

        vx.t[i] = s->warp_points[i].x1;
        vy.t[i] = s->warp_points[i].y1;
    }

    inverse(&L);

    multiply(&L, &vx, &vox);
    multiply(&L, &vy, &voy);

    s->elements = 1;
    sizeof_uv = sizeof(int16_t) * s->elements;
    s->uv_linesize = FFALIGN(inlink->w, 8);

    s->u  = av_calloc(s->uv_linesize * inlink->h, sizeof_uv);
    s->v  = av_calloc(s->uv_linesize * inlink->h, sizeof_uv);
    s->du = av_calloc(s->uv_linesize * inlink->h, sizeof_uv);
    s->dv = av_calloc(s->uv_linesize * inlink->h, sizeof_uv);
    if (!s->u || !s->v || !s->du || !s->dv) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    warp_remap(s, &vox, &voy, inlink->w, inlink->h);

    s->warp_slice = warp1_slice;
    s->remap_line = depth <= 8 ? remap1_8bit_line_c : remap1_16bit_line_c;
    if (s->interpolation == 1)
        s->remap_line = depth <= 8 ? remap2_8bit_line_c : remap2_16bit_line_c;

fail:

    free_matrix(&L);
    free_matrix(&vx);
    free_matrix(&vy);
    free_matrix(&vox);
    free_matrix(&voy);

    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    WarpContext *s = ctx->priv;
    AVFrame *out;
    ThreadData td;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    td.in = in;
    td.out = out;

    ctx->internal->execute(ctx, s->warp_slice, &td, NULL, FFMIN(outlink->h, ff_filter_get_nb_threads(ctx)));

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    WarpContext *s = ctx->priv;

    av_freep(&s->points);
    s->points_size = 0;
    av_freep(&s->warp_points);
    s->nb_warp_points = 0;

    av_freep(&s->u);
    av_freep(&s->v);
    av_freep(&s->du);
    av_freep(&s->dv);
}

static const AVFilterPad warp_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad warp_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_vf_warp = {
    .name          = "warp",
    .description   = NULL_IF_CONFIG_SMALL("Warp pixels."),
    .priv_size     = sizeof(WarpContext),
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = warp_inputs,
    .outputs       = warp_outputs,
    .priv_class    = &warp_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_SLICE_THREADS,
};
