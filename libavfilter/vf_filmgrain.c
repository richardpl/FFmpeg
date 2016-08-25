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

#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define STRETCH_3D (-1.f / 6.f)
#define SQUISH_3D  (1.f / 3.f)
#define NORM_3D    (1.f / 103.f)

typedef struct Contribution3 {
    float dx, dy, dz;
    int xsb, ysb, zsb;
    struct Contribution3 *Next;
} Contribution3;

typedef struct OpenSimplexNoise {
    uint8_t perm[256];
    uint8_t perm3D[256];

    Contribution3 *lookup3D[2048];
    Contribution3 *contributions3D[24];
} OpenSimplexNoise;

typedef struct FilmGrainContext {
    const AVClass *class;

    int depth;
    int nb_planes;
    int linesize[4];
    int planewidth[4];
    int planeheight[4];

    float size;
    float speed;
    float strength;
    int planes;

    int64_t seed[4];

    OpenSimplexNoise osn[4];
} FilmGrainContext;

#define OFFSET(x) offsetof(FilmGrainContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption filmgrain_options[] = {
    { "size",     "set grain size",   OFFSET(size),     AV_OPT_TYPE_FLOAT, {.dbl=1600},20, 6400,FLAGS },
    { "strength", "set strength",     OFFSET(strength), AV_OPT_TYPE_FLOAT, {.dbl=.25}, 0,  1,   FLAGS },
    { "speed",    "set change speed", OFFSET(speed),    AV_OPT_TYPE_FLOAT, {.dbl=1},   0,  10,  FLAGS },
    { "planes",   "set planes",       OFFSET(planes),   AV_OPT_TYPE_INT,   {.i64=1},   0,  0xF, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(filmgrain);

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pixel_fmts[] = {
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY9,
        AV_PIX_FMT_GRAY10, AV_PIX_FMT_GRAY12, AV_PIX_FMT_GRAY14,
        AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_YUVJ411P,
        AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
        AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV440P10,
        AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV420P12,
        AV_PIX_FMT_YUV440P12,
        AV_PIX_FMT_YUV444P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV420P14,
        AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRP9, AV_PIX_FMT_GBRP10,
        AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRP14, AV_PIX_FMT_GBRP16,
        AV_PIX_FMT_YUVA420P,  AV_PIX_FMT_YUVA422P,   AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_YUVA444P9, AV_PIX_FMT_YUVA444P10, AV_PIX_FMT_YUVA444P12, AV_PIX_FMT_YUVA444P16,
        AV_PIX_FMT_YUVA422P9, AV_PIX_FMT_YUVA422P10, AV_PIX_FMT_YUVA422P12, AV_PIX_FMT_YUVA422P16,
        AV_PIX_FMT_YUVA420P9, AV_PIX_FMT_YUVA420P10, AV_PIX_FMT_YUVA420P16,
        AV_PIX_FMT_GBRAP,     AV_PIX_FMT_GBRAP10,    AV_PIX_FMT_GBRAP12,    AV_PIX_FMT_GBRAP16,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *formats = ff_make_format_list(pixel_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, formats);
}

static int config_input(AVFilterLink *inlink)
{
    FilmGrainContext *s = inlink->dst->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    int ret;

    s->depth = desc->comp[0].depth;
    s->nb_planes = av_pix_fmt_count_planes(inlink->format);

    if ((ret = av_image_fill_linesizes(s->linesize, inlink->format, inlink->w)) < 0)
        return ret;

    s->planewidth[1] = s->planewidth[2] = AV_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->planewidth[0] = s->planewidth[3] = inlink->w;
    s->planeheight[1] = s->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;

    return 0;
}

static void init_noise(OpenSimplexNoise *n, int64_t seed)
{
    int8_t source[256];

    for (int i = 0; i < 256; i++)
        source[i] = i;

    seed = seed * 6364136223846793005LL + 1442695040888963407LL;
    seed = seed * 6364136223846793005LL + 1442695040888963407LL;
    seed = seed * 6364136223846793005LL + 1442695040888963407LL;

    for (int i = 255; i >= 0; i--) {
        seed = seed * 6364136223846793005LL + 1442695040888963407LL;
        int r = (int)((seed + 31) % (i + 1));
        if (r < 0)
            r += (i + 1);
        n->perm[i] = source[r];
        n->perm3D[i] = (uint8_t)((n->perm[i] % 24) * 3);
        source[r] = source[i];
    }
}

static const uint8_t base3D[][24] =
{
    { 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1 },
    { 2, 1, 1, 0, 2, 1, 0, 1, 2, 0, 1, 1, 3, 1, 1, 1 },
    { 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 2, 1, 1, 0, 2, 1, 0, 1, 2, 0, 1, 1 }
};

static const int8_t p3D[] =
{
     0, 0, 1, -1, 0, 0, 1, 0, -1, 0, 0, -1, 1, 0, 0, 0, 1,
    -1, 0, 0, -1, 0, 1, 0, 0, -1, 1, 0, 2, 1, 1, 0, 1, 1,
     1, -1, 0, 2, 1, 0, 1, 1, 1, -1, 1, 0, 2, 0, 1, 1, 1,
    -1, 1, 1, 1, 3, 2, 1, 0, 3, 1, 2, 0, 1, 3, 2, 0, 1, 3,
     1, 0, 2, 1, 3, 0, 2, 1, 3, 0, 1, 2, 1, 1, 1, 0, 0, 2,
     2, 0, 0, 1, 1, 0, 1, 0, 2, 0, 2, 0, 1, 1, 0, 0, 1, 2,
     0, 0, 2, 2, 0, 0, 0, 0, 1, 1, -1, 1, 2, 0, 0, 0, 0, 1,
     -1, 1, 1, 2, 0, 0, 0, 0, 1, 1, 1, -1, 2, 3, 1, 1, 1, 2,
     0, 0, 2, 2, 3, 1, 1, 1, 2, 2, 0, 0, 2, 3, 1, 1, 1, 2, 0,
     2, 0, 2, 1, 1, -1, 1, 2, 0, 0, 2, 2, 1, 1, -1, 1, 2, 2,
     0, 0, 2, 1, -1, 1, 1, 2, 0, 0, 2, 2, 1, -1, 1, 1, 2, 0,
     2, 0, 2, 1, 1, 1, -1, 2, 2, 0, 0, 2, 1, 1, 1, -1, 2, 0, 2, 0,
};

static const uint16_t lookupPairs3D[] =
{
    0, 2, 1, 1, 2, 2, 5, 1, 6, 0, 7, 0, 32, 2, 34, 2, 129, 1, 133, 1,
    160, 5, 161, 5, 518, 0, 519, 0, 546, 4, 550, 4, 645, 3, 647, 3,
    672, 5, 673, 5, 674, 4, 677, 3, 678, 4, 679, 3, 680, 13, 681, 13,
    682, 12, 685, 14, 686, 12, 687, 14, 712, 20, 714, 18, 809, 21, 813,
    23, 840, 20, 841, 21, 1198, 19, 1199, 22, 1226, 18, 1230, 19, 1325,
    23, 1327, 22, 1352, 15, 1353, 17, 1354, 15, 1357, 17, 1358, 16, 1359,
    16, 1360, 11, 1361, 10, 1362, 11, 1365, 10, 1366, 9, 1367, 9, 1392,
    11, 1394, 11, 1489, 10, 1493, 10, 1520, 8, 1521, 8, 1878, 9, 1879, 9,
    1906, 7, 1910, 7, 2005, 6, 2007, 6, 2032, 8, 2033, 8, 2034, 7, 2037,
    6, 2038, 7, 2039, 6,
};

/*
 * Gradients for 3D. They approximate the directions to the
 * vertices of a rhombicuboctahedron from the center, skewed so
 * that the triangular and square facets can be inscribed inside
 * circles of the same radius.
 */
static const int8_t gradients3D[] = {
    -11,  4,  4, -4,  11,  4, 4,  4,  11,
     11,  4,  4,  4,  11,  4, 4,  4,  11,
    -11, -4,  4, -4, -11,  4, 4, -4,  11,
     11, -4,  4,  4, -11,  4, 4, -4,  11,
    -11,  4, -4, -4,  11, -4, 4,  4, -11,
     11,  4, -4,  4,  11, -4, 4,  4, -11,
    -11, -4, -4, -4, -11, -4, 4, -4, -11,
     11, -4, -4,  4, -11, -4, 4, -4, -11,
};

static void EContribution3(Contribution3 *c, float multiplier, int xsb, int ysb, int zsb)
{
    c->xsb = xsb;
    c->ysb = ysb;
    c->zsb = zsb;

    c->dx = -xsb - multiplier * SQUISH_3D;
    c->dy = -ysb - multiplier * SQUISH_3D;
    c->dz = -zsb - multiplier * SQUISH_3D;
}

static float evaluate(const OpenSimplexNoise *const n, float x, float y, float z)
{
    float stretchOffset = (x + y + z) * STRETCH_3D;
    float xs = x + stretchOffset;
    float ys = y + stretchOffset;
    float zs = z + stretchOffset;

    int xsb = floorf(xs);
    int ysb = floorf(ys);
    int zsb = floorf(zs);

    float squishOffset = (xsb + ysb + zsb) * SQUISH_3D;
    float dx0 = x - (xsb + squishOffset);
    float dy0 = y - (ysb + squishOffset);
    float dz0 = z - (zsb + squishOffset);

    float xins = xs - xsb;
    float yins = ys - ysb;
    float zins = zs - zsb;

    float inSum = xins + yins + zins;

    int hash =
        (int)(yins - zins + 1) |
        (int)(xins - yins + 1) << 1 |
        (int)(xins - zins + 1) << 2 |
        (int)(inSum) << 3 |
        (int)(inSum + zins) << 5 |
        (int)(inSum + yins) << 7 |
        (int)(inSum + xins) << 9;

    Contribution3 *c = n->lookup3D[hash];

    float value = 0.0;

    while (c != NULL) {
        float dx = dx0 + c->dx;
        float dy = dy0 + c->dy;
        float dz = dz0 + c->dz;
        float attn = 2.f - dx * dx - dy * dy - dz * dz;

        if (attn > 0.f) {
            int px = xsb + c->xsb;
            int py = ysb + c->ysb;
            int pz = zsb + c->zsb;

            int i = n->perm3D[(n->perm[(n->perm[px & 0xFF] + py) & 0xFF] + pz) & 0xFF];
            float valuePart = gradients3D[i] * dx + gradients3D[i + 1] * dy + gradients3D[i + 2] * dz;

            attn *= attn;
            value += attn * attn * valuePart;
        }

        c = c->Next;
    }

    return value * NORM_3D;
}

typedef struct ThreadData {
    AVFrame *in, *out;
    int plane;
    float strength;
} ThreadData;

static int grain8_plane_slice(AVFilterContext *ctx, ThreadData *td, int jobnr, int nb_jobs, int p)
{
    FilmGrainContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFrame *in = td->in;
    AVFrame *out = td->out;
    const int width = s->planewidth[p];
    const int height = s->planeheight[p];
    const float xsize = s->size / width;
    const float ysize = s->size / height;
    const int slice_start = (height *  jobnr   ) / nb_jobs;
    const int slice_end   = (height * (jobnr+1)) / nb_jobs;
    const uint8_t *src = in->data[p] + slice_start * in->linesize[p];
    uint8_t *dst = out->data[p] + slice_start * out->linesize[p];
    const float strength = s->strength * 0.3f * 255.f;
    const float z = inlink->frame_count_out * s->speed;
    OpenSimplexNoise *n = &s->osn[p];

    for (int y = slice_start; y < slice_end; y++) {
        if (!((1 << p) & s->planes)) {
            if (in != out)
                av_image_copy_plane(dst, out->linesize[p], src, in->linesize[p],
                                    s->linesize[p], slice_end - slice_start);
            continue;
        }

        for (int x = 0; x < s->planewidth[p]; x++) {
            float noise = evaluate(n, x * xsize, y * ysize, z);

            dst[x] = av_clip_uint8(src[x] + strength * noise);
        }

        dst += out->linesize[p];
        src += in->linesize[p];
    }

    return 0;
}

static int grain8_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    FilmGrainContext *s = ctx->priv;
    ThreadData *td = arg;

    for (int p = 0; p < s->nb_planes; p++)
        grain8_plane_slice(ctx, td, jobnr, nb_jobs, p);

    return 0;
}

static void grain16(AVFilterContext *ctx, AVFrame *in, AVFrame *out)
{
    AVFilterLink *inlink = ctx->inputs[0];
    FilmGrainContext *s = ctx->priv;

    for (int p = 0; p < s->nb_planes; p++) {
        OpenSimplexNoise *n = &s->osn[p];
        const float z = inlink->frame_count_out * s->speed;
        const float size = 1.f / ((1.f + 8 * s->size / 100.f) / 800.f);
        const float strength = s->strength * ((1 << s->depth - 1) - 0.5);
        const uint16_t *src = (const uint16_t *)in->data[p];
        uint16_t *dst = (uint16_t *)out->data[p];
        int y, x;

        if (!((1 << p) & s->planes) && in != out) {
            if (in != out)
                av_image_copy_plane(out->data[p], out->linesize[p], in->data[p], in->linesize[p],
                                    s->linesize[p], s->planeheight[p]);
            continue;
        }

        for (y = 0; y < s->planeheight[p]; y++) {
            for (x = 0; x < s->planewidth[p]; x++) {
                float noise = evaluate(n, x * size, y * size, z);

                dst[x] = av_clip_uint16(src[x] + strength * noise);
            }

            dst += out->linesize[p] / 2;
            src += in->linesize[p] / 2;
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    FilmGrainContext *s = ctx->priv;
    ThreadData td;
    AVFrame *out;

    if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    td.in = in, td.out = out;
    ctx->internal->execute(ctx, grain8_slice, &td, NULL,
                           FFMIN(s->planeheight[1], ff_filter_get_nb_threads(ctx)));

    if (in != out)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int noise_lookup(OpenSimplexNoise *n)
{
    for (int i = 0; i < 216; i += 9) {
        const uint8_t *baseSet = base3D[p3D[i]];
        const int baseSetSize = p3D[i] == 2 ? 24 : 16;
        Contribution3 *previous = NULL, *current = NULL;

        for (int k = 0; k < baseSetSize; k += 4) {
            current = av_mallocz(sizeof(Contribution3));
            if (!current)
                return AVERROR(ENOMEM);

            EContribution3(current, baseSet[k], baseSet[k + 1],
                           baseSet[k + 2], baseSet[k + 3]);
            if (previous == NULL) {
                n->contributions3D[i / 9] = current;
            } else {
                previous->Next = current;
            }

            previous = current;
        }

        current->Next = av_mallocz(sizeof(Contribution3));
        if (!current->Next)
            return AVERROR(ENOMEM);
        EContribution3(current->Next, p3D[i + 1], p3D[i + 2], p3D[i + 3], p3D[i + 4]);
        current->Next->Next = av_mallocz(sizeof(Contribution3));
        if (!current->Next->Next)
            return AVERROR(ENOMEM);
        EContribution3(current->Next->Next, p3D[i + 5], p3D[i + 6], p3D[i + 7], p3D[i + 8]);
    }

    for (int i = 0; i < FF_ARRAY_ELEMS(lookupPairs3D); i += 2)
        n->lookup3D[lookupPairs3D[i]] = n->contributions3D[lookupPairs3D[i + 1]];

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    FilmGrainContext *s = ctx->priv;

    for (int p = 0; p < 4; p++) {
        init_noise(&s->osn[p], s->seed[p]);
        noise_lookup(&s->osn[p]);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FilmGrainContext *s = ctx->priv;

}

static const AVFilterPad filmgrain_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad filmgrain_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_filmgrain = {
    .name          = "filmgrain",
    .description   = NULL_IF_CONFIG_SMALL("Add film grain."),
    .priv_size     = sizeof(FilmGrainContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = filmgrain_inputs,
    .outputs       = filmgrain_outputs,
    .priv_class    = &filmgrain_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_SLICE_THREADS,
    .process_command = ff_filter_process_command,
};
