/*
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

#include <float.h>

#include "libavutil/avassert.h"
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

enum Projections {
    EQUIRECTANGULAR,
    CUBEMAP_6x1,
    CUBEMAP_3x2,
    EAC_3x2,
    FLAT,
    NB_PROJECTIONS,
};

enum Faces {
    LEFT,
    FRONT,
    RIGHT,
    TOP,
    BACK,
    DOWN,
};

struct XYRemap {
    int vi, ui;
    int v2, u2;
    float a, b, c, d;
};

typedef struct PerigonContext {
    const AVClass *class;
    int in, out;
    float fov;
    float yaw;
    float pitch;
    float roll;

    int planewidth[4], planeheight[4];
    int inplanewidth[4], inplaneheight[4];
    int nb_planes;

    struct XYRemap *remap[4];

    int (*perigon)(struct PerigonContext *s,
                    const uint8_t *src, uint8_t *dst,
                    int width, int height,
                    int in_linesize, int out_linesize,
                    const struct XYRemap *remap);
} PerigonContext;

#define OFFSET(x) offsetof(PerigonContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption perigon_options[] = {
    { "input",  "set input projection",   OFFSET(in), AV_OPT_TYPE_INT,   {.i64=EQUIRECTANGULAR}, 0, NB_PROJECTIONS-1, FLAGS, "in" },
    {     "e", "equirectangular",                  0, AV_OPT_TYPE_CONST, {.i64=EQUIRECTANGULAR}, 0,                0, FLAGS, "in" },
    {  "c6x1", "cubemap 6x1",                      0, AV_OPT_TYPE_CONST, {.i64=CUBEMAP_6x1},     0,                0, FLAGS, "in" },
    {  "c3x2", "cubemap 3x2",                      0, AV_OPT_TYPE_CONST, {.i64=CUBEMAP_3x2},     0,                0, FLAGS, "in" },
    {  "e3x2", "eac 3x2",                          0, AV_OPT_TYPE_CONST, {.i64=EAC_3x2},         0,                0, FLAGS, "in" },
    { "output", "set output projection", OFFSET(out), AV_OPT_TYPE_INT,   {.i64=CUBEMAP_3x2},     0, NB_PROJECTIONS-1, FLAGS, "out" },
    {     "e", "equirectangular",                  0, AV_OPT_TYPE_CONST, {.i64=EQUIRECTANGULAR}, 0,                0, FLAGS, "out" },
    {  "c6x1", "cubemap 6x1",                      0, AV_OPT_TYPE_CONST, {.i64=CUBEMAP_6x1},     0,                0, FLAGS, "out" },
    {  "c3x2", "cubemap 3x2",                      0, AV_OPT_TYPE_CONST, {.i64=CUBEMAP_3x2},     0,                0, FLAGS, "out" },
    {  "flat", "flat",                             0, AV_OPT_TYPE_CONST, {.i64=FLAT},            0,                0, FLAGS, "out" },
    { "fov",   "set horizontal field of view", OFFSET(fov),   AV_OPT_TYPE_FLOAT,  {.dbl=M_PI/2}, 0,           2*M_PI, FLAGS },
    { "yaw",   "set polar angle",              OFFSET(yaw),   AV_OPT_TYPE_FLOAT,  {.dbl=M_PI},   -2*M_PI,     2*M_PI, FLAGS },
    { "pitch", "set vertical pitch",           OFFSET(pitch), AV_OPT_TYPE_FLOAT,  {.dbl=0},      -2*M_PI,     2*M_PI, FLAGS },
    { "roll",  "set view rotation",            OFFSET(roll),  AV_OPT_TYPE_FLOAT,  {.dbl=0},      -2*M_PI,     2*M_PI, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(perigon);

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ422P,AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ411P,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRAP, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int bilinear(PerigonContext *s,
                    const uint8_t *src, uint8_t *dst,
                    int width, int height,
                    int in_linesize, int out_linesize,
                    const struct XYRemap *remap)
{
    float A, B, C, D;
    int x, y;

    for (y = 0; y < height; y++) {
        uint8_t *d = dst + y * out_linesize;
        for (x = 0; x < width; x++) {
            const struct XYRemap *r = &remap[y * width + x];

            A = src[r->vi * in_linesize + r->ui];
            B = src[r->vi * in_linesize + r->u2];
            C = src[r->v2 * in_linesize + r->ui];
            D = src[r->v2 * in_linesize + r->u2];
            *d++ = round(A * r->a + B * r->b + C * r->c + D * r->d);
        }
    }

    return 0;
}

static int nearest(PerigonContext *s,
                   const uint8_t *src, uint8_t *dst,
                   int width, int height,
                   int in_linesize, int out_linesize,
                   const struct XYRemap *remap)
{
    int x, y;

    for (y = 0; y < height; y++) {
        uint8_t *d = dst + y * out_linesize;
        for (x = 0; x < width; x++) {
            const struct XYRemap *r = &remap[y * width + x];

            *d++ = src[r->vi * in_linesize + r->ui];
        }
    }

    return 0;
}

static void to_cube3x2_xyz(int i, int j, int face, float ew, float eh,
                           float *x, float *y, float *z)
{
    float a = 2 * i / ew;
    float b = 2 * j / eh;

    if (face == BACK) {
        *x = -1     ;
        *y =  3. - a;
        *z =  3. - b;
    } else if (face == LEFT) {
        *x =  a  - 1;
        *y = -1     ;
        *z =  1. - b;
    } else if (face == FRONT) {
        *x =  1     ;
        *y =  a  - 3;
        *z =  1. - b;
    } else if (face == RIGHT) {
        *x =  5. - a;
        *y =  1     ;
        *z =  1. - b;
    } else if (face == TOP) {
        *x =  b  - 3;
        *y =  a  - 1;
        *z =  1     ;
    } else if (face == DOWN) {
        *x = -b  + 3;
        *y =  a  - 5;
        *z = -1     ;
    }
}

static void to_cube6x1_xyz(int i, int j, int face, float ew, float eh,
                           float *x, float *y, float *z)
{
    float a = 2 * i / ew;
    float b = 2 * j / eh;

    if (face == BACK) {
        *x = -1     ;
        *y =  9. - a;
        *z =  1. - b;
    } else if (face == LEFT) {
        *x =  a  - 1;
        *y = -1     ;
        *z =  1. - b;
    } else if (face == FRONT) {
        *x =  1     ;
        *y =  a  - 3;
        *z =  1. - b;
    } else if (face == RIGHT) {
        *x =  5. - a;
        *y =  1     ;
        *z =  1. - b;
    } else if (face == TOP) {
        *x =  b  - 1;
        *y =  a  - 7;
        *z =  1     ;
    } else if (face == DOWN) {
        *x = -b  + 1;
        *y =  a  - 11;
        *z = -1     ;
    }
}

static void to_sphere_xyz(float theta, float phi, float *x, float *y, float *z)
{
    *x = cosf(phi) * cosf(theta);
    *y = sinf(phi);
    *z = cosf(phi) * sinf(theta);
}

static void locate(float axis, float x, float y, float rad,
                   float rw, float rh, int *ox, int *oy)
{
    *ox = rw / axis * (x * cosf(rad) - y * sinf(rad));
    *oy = rh / axis * (x * sinf(rad) + y * cosf(rad));
    *ox += rw;
    *oy += rh;
}

static inline int equal(float a, float b, float epsilon)
{
    return fabs(a - b) < epsilon;
}

static inline int smaller(float a, float b, float epsilon)
{
    return ((a - b) < 0.0) && (!equal(a, b, epsilon));
}

static inline int in_range(float rd, float small, float large, float res)
{
   return    !smaller(rd, small, res)
          &&  smaller(rd, large, res);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    PerigonContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    int p, h, w;

    if (s->in == EQUIRECTANGULAR && s->out == CUBEMAP_3x2) {
        w = inlink->w / 4 * 3;
        h = inlink->h;
    } else if (s->in == EQUIRECTANGULAR && s->out == CUBEMAP_6x1) {
        w = inlink->w / 4 * 6;
        h = inlink->h / 2;
    } else if ((s->in == CUBEMAP_3x2 || s->in == EAC_3x2) && s->out == EQUIRECTANGULAR) {
        w = inlink->w / 3 * 4;
        h = inlink->h;
    } else if (s->in == CUBEMAP_6x1 && s->out == EQUIRECTANGULAR) {
        w = inlink->w / 6 * 4;
        h = inlink->h * 2;
    } else if (s->in == CUBEMAP_3x2 && s->out == CUBEMAP_6x1) {
        w = inlink->w * 2;
        h = inlink->h / 2;
    } else if (s->in == CUBEMAP_6x1 && s->out == CUBEMAP_3x2) {
        w = inlink->w / 2;
        h = inlink->h * 2;
    } else if (s->in == s->out || s->out == FLAT) {
        w = inlink->w;
        h = inlink->h;
    } else {
        av_log(ctx, AV_LOG_ERROR, "Unsupported layout input & output combination.\n");
        return AVERROR_PATCHWELCOME;
    }

    s->planeheight[1] = s->planeheight[2] = FF_CEIL_RSHIFT(h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = h;
    s->planewidth[1] = s->planewidth[2] = FF_CEIL_RSHIFT(w, desc->log2_chroma_w);
    s->planewidth[0] = s->planewidth[3] = w;

    outlink->h = h;
    outlink->w = w;

    s->inplaneheight[1] = s->inplaneheight[2] = FF_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->inplaneheight[0] = s->inplaneheight[3] = inlink->h;
    s->inplanewidth[1]  = s->inplanewidth[2]  = FF_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->inplanewidth[0]  = s->inplanewidth[3]  = inlink->w;
    s->nb_planes = av_pix_fmt_count_planes(inlink->format);

    for (p = 0; p < s->nb_planes; p++) {
        s->remap[p] = av_calloc(s->planewidth[p] * s->planeheight[p], sizeof(struct XYRemap));
        if (!s->remap[p])
            return AVERROR(ENOMEM);
    }

    if (s->in == EQUIRECTANGULAR && s->out == FLAT) {
        const float tf = tanf(s->fov / 2.f);
        const float cp = cosf(s->pitch);
        const float sp = sinf(s->pitch);
        const float sr = sinf(s->roll);
        const float cr = cosf(s->roll);

        for (p = 0; p < s->nb_planes; p++) {
            float r = s->planeheight[p] / (float)s->planewidth[p];
            const float mat[3][3] = { { 2*tf*cr,    2*sr*tf*r,   -tf*(cr+sr*r)     },
                                      {-2*sr*tf*cp, 2*cr*tf*cp*r, tf*cp*(sr-cr*r)-sp },
                                      {-2*sr*tf*sp, 2*cr*tf*sp*r, tf*sp*(sr-cr*r)+cp } };
            int width = s->planewidth[p];
            int height = s->planeheight[p];

            for (int i = 0; i < width; i++) {
                for (int j = 0; j < height; j++) {
                    struct XYRemap *r = &s->remap[p][j * width + i];
                    const float x = i / (float)width;
                    const float y = j / (float)height;
                    float px = mat[0][0] * x + mat[0][1] * y + mat[0][2];
                    float py = mat[1][0] * x + mat[1][1] * y + mat[1][2];
                    float pz = mat[2][0] * x + mat[2][1] * y + mat[2][2];
                    float theta = atan2f(px, pz) + s->yaw;
                    float phi = atan2f(py, hypotf(px, pz)) + M_PI_2;
                    float unused;

                    r->ui = modff(theta / (2 * M_PI), &unused) * width;
                    r->ui = r->ui % width;
                    if (r->ui < 0)
                        r->ui += width;
                    r->vi = (phi / M_PI) * height;
                    r->vi = r->vi % height;
                    if (r->vi < 0)
                        r->vi += height;
                }
            }
        }
        s->perigon = nearest;
    } else if (s->in == EQUIRECTANGULAR && (s->out == CUBEMAP_3x2 || s->out == CUBEMAP_6x1)) {
        for (p = 0; p < s->nb_planes; p++) {
            int face, ui, vi, u2, v2;
            float theta, R, phi, uf, vf, mu, nu, x, y, z;
            int ew, eh;
            int width = s->planewidth[p];
            int height = s->planeheight[p];
            int in_width = s->inplanewidth[p];
            int in_height = s->inplaneheight[p];
            int i, j;
            if (s->out == CUBEMAP_3x2) {
                ew = s->planewidth[p] / 3;
                eh = s->planeheight[p] / 2;
            } else if (s->out == CUBEMAP_6x1) {
                ew = s->planewidth[p] / 6;
                eh = s->planeheight[p];
            }
            for (i = 0; i < width; i++) {
                for (j = 0; j < height; j++) {
                    struct XYRemap *r = &s->remap[p][j * width + i];

                    if (s->out == CUBEMAP_3x2) {
                        face = (i / ew) + 3 * (j / (height / 2));
                        to_cube3x2_xyz(i, j, face, ew, eh, &x, &y, &z);
                    } else if (s->out == CUBEMAP_6x1) {
                        face = i / ew;
                        to_cube6x1_xyz(i, j, face, ew, eh, &x, &y, &z);
                    }
                    theta = atan2f(y, x);
                    R = hypotf(x, y);
                    phi = atan2f(z, R);
                    uf = (2.0 * ew * (theta + M_PI) / M_PI);
                    vf = (2.0 * eh * (M_PI_2 - phi) / M_PI);

                    ui = floorf(uf);
                    vi = floorf(vf);
                    u2 = ui + 1;
                    v2 = vi + 1;
                    mu = uf - ui;
                    nu = vf - vi;
                    r->vi = av_clip(vi, 0, in_height - 1);
                    r->ui = ui % in_width;
                    r->v2 = av_clip(v2, 0, in_height - 1);
                    r->u2 = u2 % in_width;
                    r->a = (1 - mu) * (1 - nu);
                    r->b =  mu * (1 - nu);
                    r->c = (1 - mu) * nu;
                    r->d = mu * nu;
                }
            }
        }
        s->perigon = bilinear;
    } else if ((s->in == CUBEMAP_3x2 || s->in == CUBEMAP_6x1 || s->in == EAC_3x2) && s->out == EQUIRECTANGULAR) {
        for (p = 0; p < s->nb_planes; p++) {
            float theta, theta_norm, phi, phi_threshold, x, y, z;
            int height = s->planeheight[p];
            int width = s->planewidth[p];
            int in_width = s->inplanewidth[p];
            int in_height = s->inplaneheight[p];
            float res;
            int rh, rw;
            int face, i, j;
            int ox, oy;

            if (s->in == CUBEMAP_3x2 || s->in == EAC_3x2) {
                res = M_PI_4 / (width / 3) / 10.0;
                rh = in_height / 4;
                rw = in_width / 6;
            } else if (s->in == CUBEMAP_6x1) {
                res = M_PI_4 / (width / 6) / 10.0;
                rh = in_height / 2;
                rw = in_width / 12;
            }

            for (i = 0; i < width; i++) {
                for (j = 0; j < height; j++) {
                    struct XYRemap *r = &s->remap[p][j * width + i];

                    x = (2. * i) / width - 1.;
                    y = (2. * j) / height - 1.;
                    theta = x * M_PI;
                    phi   = y * M_PI_2;
                    to_sphere_xyz(theta, phi, &x, &y, &z);

                    if (in_range(theta, -M_PI_4, M_PI_4, res)) {
                        face = FRONT;
                        theta_norm = theta;
                    } else if (in_range(theta, -(M_PI_2 + M_PI_4), -M_PI_4, res)) {
                        face = LEFT;
                        theta_norm = theta + M_PI_2;
                    } else if (in_range(theta, M_PI_4, M_PI_2 + M_PI_4, res)) {
                        face = RIGHT;
                        theta_norm = theta - M_PI_2;
                    } else {
                        face = BACK;
                        theta_norm = theta + ((theta > 0) ? -M_PI : M_PI);
                    }

                    phi_threshold = atan2f(1.f, 1. / cosf(theta_norm));
                    if (phi > phi_threshold) {
                        face = s->in == EAC_3x2 ? TOP : DOWN;
                    } else if (phi < -phi_threshold) {
                        face = s->in == EAC_3x2 ? DOWN : TOP;
                    } else {
                        ;
                    }

                    switch (face) {
                    case LEFT:
                        locate(z, x, y, M_PI,   rw, rh, &ox, &oy);
                        break;
                    case FRONT:
                        locate(x, z, y, 0.,     rw, rh, &ox, &oy);
                        break;
                    case RIGHT:
                        locate(z, y, x, M_PI_2, rw, rh, &ox, &oy);
                        break;
                    case TOP:
                        s->in == EAC_3x2 ?
                            locate(y, x, z, M_PI, rw, rh, &ox, &oy) :
                            locate(y, z, x, M_PI, rw, rh, &ox, &oy);
                        break;
                    case BACK:
                        s->in == EAC_3x2 ?
                            locate(x, y, z,      0, rw, rh, &ox, &oy) :
                            locate(x, y, z,-M_PI_2, rw, rh, &ox, &oy);
                        break;
                    case DOWN:
                        s->in == EAC_3x2 ?
                            locate(y, z, x, M_PI_2, rw, rh, &ox, &oy) :
                            locate(y, x, z,-M_PI_2, rw, rh, &ox, &oy);
                        break;
                    }

                    if (s->in == CUBEMAP_3x2 || s->in == EAC_3x2) {
                        if (face > 2) {
                            oy += height / 2;
                        }
                        ox += (in_width / 3) * (face % 3);
                    } else if (s->in == CUBEMAP_6x1) {
                        ox += (in_width / 6) * face;
                    }

                    r->vi = oy;
                    r->ui = ox;
                }
            }
        }
        s->perigon = nearest;
    } else if (s->in == CUBEMAP_3x2 && s->out == CUBEMAP_6x1) {
        for (p = 0; p < s->nb_planes; p++) {
            int width = s->planewidth[p];
            int height = s->planeheight[p];
            int in_width = s->inplanewidth[p];
            int in_height = s->inplaneheight[p];
            int i, j;

            for (i = 0; i < width; i++) {
                for (j = 0; j < height; j++) {
                    struct XYRemap *r = &s->remap[p][j * width + i];
                    r->vi = j;
                    r->ui = i;
                    if (i >= in_width) {
                        r->vi += in_height / 2;
                        r->ui -= in_width;
                    }
                }
            }
        }
        s->perigon = nearest;
    } else if (s->in == CUBEMAP_6x1 && s->out == CUBEMAP_3x2) {
        for (p = 0; p < s->nb_planes; p++) {
            int width = s->planewidth[p];
            int height = s->planeheight[p];
            int in_width = s->inplanewidth[p];
            int in_height = s->inplaneheight[p];
            int i, j;

            for (i = 0; i < width; i++) {
                for (j = 0; j < height; j++) {
                    struct XYRemap *r = &s->remap[p][j * width + i];
                    r->vi = j;
                    r->ui = i;
                    if (j >= in_height) {
                        r->vi -= in_height;
                        r->ui += in_width / 2;
                    }
                }
            }
        }
        s->perigon = nearest;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    PerigonContext *s = ctx->priv;
    AVFrame *out;
    int plane;

    if (s->in == s->out)
        return ff_filter_frame(outlink, in);

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    for (plane = 0; plane < s->nb_planes; plane++) {
        s->perigon(s, in->data[plane], out->data[plane],
                    s->planewidth[plane], s->planeheight[plane],
                    in->linesize[plane], out->linesize[plane],
                    s->remap[plane]);
    }

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    PerigonContext *s = ctx->priv;
    int p;

    for (p = 0; p < s->nb_planes; p++)
        av_freep(&s->remap[p]);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
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

AVFilter ff_vf_perigon = {
    .name          = "perigon",
    .description   = NULL_IF_CONFIG_SMALL("Convert between 360° projections of video."),
    .priv_size     = sizeof(PerigonContext),
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = inputs,
    .outputs       = outputs,
    .priv_class    = &perigon_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
