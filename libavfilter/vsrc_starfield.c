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

#include "libavutil/internal.h"
#include "libavutil/lfg.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/random_seed.h"
#include "libavutil/avstring.h"
#include "avfilter.h"
#include "internal.h"
#include "formats.h"
#include "video.h"

typedef struct Star {
    int x, y;
    AVRational z;
    unsigned r, g, b;
} Star;

typedef struct StarFieldContext {
    const AVClass *class;

    int w, h;
    uint64_t pts;
    AVRational frame_rate;
    AVRational speed;
    int nb_stars;
    int64_t seed;

    AVLFG lfg;
    Star *stars;
} StarFieldContext;

#define OFFSET(x) offsetof(StarFieldContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption starfield_options[] = {
    { "size",  "set video size",   OFFSET(w),          AV_OPT_TYPE_IMAGE_SIZE, {.str = "hd720"},   0,         0, FLAGS },
    { "rate",  "set video rate",   OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"},      0,   INT_MAX, FLAGS },
    { "stars", "set stars number", OFFSET(nb_stars),   AV_OPT_TYPE_INT,        {.i64 = 1024},      1,     81920, FLAGS },
    { "seed",  "set seed",         OFFSET(seed),       AV_OPT_TYPE_INT64,      {.i64 = -1},       -1, INT64_MAX, FLAGS },
    { "speed", "set speed",        OFFSET(speed),      AV_OPT_TYPE_RATIONAL,   {.dbl = 1.01},      1,         2, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(starfield);

static void init_star(StarFieldContext *s, Star *star, int depth)
{
    star->x = av_lfg_get(&s->lfg) - INT_MAX;
    star->y = av_lfg_get(&s->lfg) - INT_MAX;
    star->z.num = star->z.den = 1;
    star->r = av_lfg_get(&s->lfg) >> av_log2(depth / 32);
    star->g = av_lfg_get(&s->lfg) >> av_log2(depth / 32);
    star->b = av_lfg_get(&s->lfg) >> av_log2(depth / 32);
}

static av_cold int init(AVFilterContext *ctx)
{
    StarFieldContext *s = ctx->priv;
    int i;

    s->stars = av_calloc(s->nb_stars, sizeof(*s->stars));
    if (!s->stars)
        return AVERROR(ENOMEM);

    if (s->seed == -1)
        s->seed = av_get_random_seed();
    av_lfg_init(&s->lfg, s->seed);

    for (i = 0; i < s->nb_stars; i++) {
        init_star(s, &s->stars[i], i+1);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    StarFieldContext *s = ctx->priv;

    av_freep(&s->stars);
}

static int config_props(AVFilterLink *outlink)
{
    StarFieldContext *s = outlink->src->priv;

    outlink->w = s->w;
    outlink->h = s->h;
    outlink->time_base = av_inv_q(s->frame_rate);

    return 0;
}

static void fill_picture(AVFilterContext *ctx, AVFrame *out)
{
    StarFieldContext *s = ctx->priv;
    const int divx = UINT_MAX / out->width;
    const int divy = UINT_MAX / out->height;
    const int hw = out->width >> 1;
    const int hh = out->height >> 1;
    const int linesize = out->linesize[0];
    int i;

    for (i = 0; i < out->height; i++) {
        memset(out->data[0] + out->linesize[0] * i, 0, out->width * 4);
    }

    for (i = 0; i < s->nb_stars; i++) {
        Star *star = &s->stars[i];
        int x, y;

        x = av_rescale(star->x / divx, star->z.num, star->z.den) + hw;
        y = av_rescale(star->y / divy, star->z.num, star->z.den) + hh;
        if (x < 0 || x >= out->width ||
            y < 0 || y >= out->height) {
            init_star(s, &s->stars[i], i+1);
            continue;
        }

        out->data[0][y * linesize + x * 4 + 0] = av_clip_uint8((av_rescale(star->r, star->z.num, star->z.den) >> 24));
        out->data[0][y * linesize + x * 4 + 1] = av_clip_uint8((av_rescale(star->g, star->z.num, star->z.den) >> 24));
        out->data[0][y * linesize + x * 4 + 2] = av_clip_uint8((av_rescale(star->b, star->z.num, star->z.den) >> 24));

        star->z = av_mul_q(star->z, s->speed);
    }
}

static int request_frame(AVFilterLink *outlink)
{
    StarFieldContext *s = outlink->src->priv;
    AVFrame *out = ff_get_video_buffer(outlink, s->w, s->h);
    if (!out)
        return AVERROR(ENOMEM);
    out->sample_aspect_ratio = (AVRational) {1, 1};
    fill_picture(outlink->src, out);

    out->pts = s->pts++;

    return ff_filter_frame(outlink, out);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_RGB0, AV_PIX_FMT_NONE };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static const AVFilterPad starfield_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame,
        .config_props  = config_props,
    },
    { NULL }
};

AVFilter ff_vsrc_starfield = {
    .name          = "starfield",
    .description   = NULL_IF_CONFIG_SMALL("Create retro 3D star field."),
    .priv_size     = sizeof(StarFieldContext),
    .priv_class    = &starfield_class,
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = NULL,
    .outputs       = starfield_outputs,
};
