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

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/ffmath.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/parseutils.h"
#include "libavutil/timestamp.h"
#include "libavutil/xga_font_data.h"
#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "audio.h"
#include "video.h"

typedef struct AVSyncTestContext {
    const AVClass *class;

    int w, h;
    AVRational frame_rate;
    int sample_rate;
    int64_t apts, vpts, prev_vpts;
    float amplitude;
    int period;
    int delay;
    int cycle;

    int previ;
    int beep;
    int beep_duration;
    int flash;
    int dir;
    float vdelay, delay_max, delay_min;
    float delay_range;

    FFDrawContext draw;
    FFDrawColor white;
    FFDrawColor black;
    FFDrawColor gray;
} AVSyncTestContext;

#define OFFSET(x) offsetof(AVSyncTestContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define V AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption avsynctest_options[] = {
    {"size",       "set frame size",  OFFSET(w),           AV_OPT_TYPE_IMAGE_SIZE, {.str="hd720"},   0,   0, V },
    {"s",          "set frame size",  OFFSET(w),           AV_OPT_TYPE_IMAGE_SIZE, {.str="hd720"},   0,   0, V },
    {"framerate",  "set frame rate",  OFFSET(frame_rate),  AV_OPT_TYPE_VIDEO_RATE, {.str="30"},   0,INT_MAX, V },
    {"fr",         "set frame rate",  OFFSET(frame_rate),  AV_OPT_TYPE_VIDEO_RATE, {.str="30"},   0,INT_MAX, V },
    {"samplerate", "set sample rate", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64=44100},8000,192000, A },
    {"sr",         "set sample rate", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64=44100},8000,192000, A },
    {"amplitude",  "set amplitude",   OFFSET(amplitude),   AV_OPT_TYPE_FLOAT,      {.dbl=.5},       0.,  1., A },
    {"a",          "set amplitude",   OFFSET(amplitude),   AV_OPT_TYPE_FLOAT,      {.dbl=.5},       0.,  1., A },
    {"period",     "set beep period", OFFSET(period),      AV_OPT_TYPE_INT,        {.i64=3},         1, 99., A },
    {"p",          "set beep period", OFFSET(period),      AV_OPT_TYPE_INT,        {.i64=3},         1, 99., A },
    {"delay",      "set flash delay", OFFSET(delay),       AV_OPT_TYPE_INT,        {.i64=0},       -30,  30, V },
    {"d",          "set flash delay", OFFSET(delay),       AV_OPT_TYPE_INT,        {.i64=0},       -30,  30, V },
    {"cycle",      "set delay cycle", OFFSET(cycle),       AV_OPT_TYPE_BOOL,       {.i64=0},         0,   1, V },
    {"c",          "set delay cycle", OFFSET(cycle),       AV_OPT_TYPE_BOOL,       {.i64=0},         0,   1, V },
    {NULL},
};

AVFILTER_DEFINE_CLASS(avsynctest);

static av_cold int query_formats(AVFilterContext *ctx)
{
    AVSyncTestContext *s = ctx->priv;
    static const int64_t chlayouts[] = { AV_CH_LAYOUT_MONO, -1 };
    int sample_rates[] = { s->sample_rate, -1 };
    static const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_NONE
    };
    AVFilterFormats *formats;
    AVFilterChannelLayouts *layouts;
    int ret;

    formats = ff_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->in_formats)) < 0)
        return ret;

    formats = ff_draw_supported_pixel_formats(0);
    if (!formats)
        return AVERROR(ENOMEM);
    if ((ret = ff_formats_ref(formats, &ctx->outputs[1]->in_formats)) < 0)
        return ret;

    layouts = avfilter_make_format64_list(chlayouts);
    if (!layouts)
        return AVERROR(ENOMEM);
    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        return ret;

    formats = ff_make_format_list(sample_rates);
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_samplerates(ctx, formats);
}

static av_cold int aconfig_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVSyncTestContext *s = ctx->priv;

    outlink->sample_rate = s->sample_rate;
    outlink->time_base = (AVRational){1, s->sample_rate};

    s->beep_duration = s->sample_rate * s->frame_rate.den / s->frame_rate.num ;

    return 0;
}

static av_cold int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVSyncTestContext *s = ctx->priv;

    outlink->w = s->w;
    outlink->h = s->h;
    outlink->time_base = av_inv_q(s->frame_rate);
    outlink->sample_aspect_ratio = (AVRational) {1, 1};
    s->delay_min = -av_q2d(s->frame_rate) / 2;
    s->delay_max = -s->delay_min;
    s->delay_range = s->delay_max - s->delay_min;
    s->vdelay = s->delay;
    s->dir = 1;

    ff_draw_init(&s->draw, outlink->format, 0);

    ff_draw_color(&s->draw, &s->black, (uint8_t[]){   0,   0,   0, 255} );
    ff_draw_color(&s->draw, &s->white, (uint8_t[]){ 255, 255, 255, 255} );
    ff_draw_color(&s->draw, &s->gray,  (uint8_t[]){ 128, 128, 128, 255} );

    return 0;
}

static int arequest_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVSyncTestContext *s = ctx->priv;
    AVFrame *out = ff_get_audio_buffer(outlink, 1024);
    const float a = s->amplitude;
    float *dst;
    int i;
    if (!out)
        return AVERROR(ENOMEM);

    out->pts = s->apts;
    dst = (float *)out->data[0];

    for (i = 0; i < 1024; i++) {
        if (((s->apts + i) % (s->period * s->sample_rate)) == 0)
            s->beep = 1;
        if (s->beep) {
            dst[i] = a * sinf(800 * 2 * M_PI * av_q2d(outlink->time_base) * (s->apts + i));
            s->beep++;
        } else {
            dst[i] = 0;
        }
        if (s->beep >= s->beep_duration) {
            s->beep = 0;
        }
    }
    s->apts += out->nb_samples;

    return ff_filter_frame(outlink, out);
}

static void draw_text(FFDrawContext *draw, AVFrame *out, FFDrawColor *color,
                      int x0, int y0, const uint8_t *text)
{
    int x = x0;

    for (; *text; text++) {
        if (*text == '\n') {
            x = x0;
            y0 += 8;
            continue;
        }
        ff_blend_mask(draw, color, out->data, out->linesize,
                      out->width, out->height,
                      avpriv_cga_font + *text * 8, 1, 8, 8, 0, 0, x, y0);
        x += 8;
    }
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVSyncTestContext *s = ctx->priv;
    AVFrame *out = ff_get_video_buffer(outlink, s->w, s->h);
    int step = outlink->w / s->delay_range;
    int offset = (outlink->w - step * s->delay_range);
    double intpart;
    char text[128];
    int i;

    if (!out)
        return AVERROR(ENOMEM);

    ff_fill_rectangle(&s->draw, &s->black, out->data, out->linesize,
                      0, 0, outlink->w, outlink->h);

    snprintf(text, sizeof(text), "FRN: %"PRId64"", s->vpts);
    draw_text(&s->draw, out, &s->white, outlink->w * .1, outlink->h * .1, text);

    snprintf(text, sizeof(text), "SEC: %s", av_ts2timestr(s->vpts, &outlink->time_base));
    draw_text(&s->draw, out, &s->white, outlink->w * .1, outlink->h * .9, text);

    snprintf(text, sizeof(text), "DLY: %d", (int)s->vdelay);
    draw_text(&s->draw, out, &s->white, outlink->w * .9 - strlen(text) * 8, outlink->h * .9, text);

    snprintf(text, sizeof(text), "FPS: %d/%d", s->frame_rate.num, s->frame_rate.den);
    draw_text(&s->draw, out, &s->white, outlink->w * .9 - strlen(text) * 8, outlink->h * .1, text);

    snprintf(text, sizeof(text), "P: %d", s->period);
    draw_text(&s->draw, out, &s->gray, outlink->w * .5 - strlen(text) * 4, outlink->h * .9, text);

    snprintf(text, sizeof(text), "SR: %d", s->sample_rate);
    draw_text(&s->draw, out, &s->gray, outlink->w * .5 - strlen(text) * 4, outlink->h * .1, text);

    snprintf(text, sizeof(text), "A: %f", s->amplitude);
    draw_text(&s->draw, out, &s->gray, outlink->w * .1, outlink->h * .5, text);

    snprintf(text, sizeof(text), "WxH: %dx%d", outlink->w, outlink->h);
    draw_text(&s->draw, out, &s->gray, outlink->w * .9 - strlen(text) * 8, outlink->h * .5, text);

    modf(av_q2d(outlink->time_base) * (s->vpts + s->vdelay), &intpart);

    ff_fill_rectangle(&s->draw, &s->white, out->data, out->linesize,
                      ((int)((out->width - (out->width * .025)) * (av_q2d(outlink->time_base) * s->vpts - intpart)) + out->width / 2) % out->width,
                      outlink->h * .7, outlink->w * .025, outlink->h * .05);

    if (s->previ + 1 == intpart) {
        s->flash++;
        if (s->flash >= s->period) {
            if (s->cycle) {
                s->vdelay += s->dir;
            }
            if (s->vdelay >= s->delay_max) {
                s->dir = -1;
                s->prev_vpts = s->vpts;
            } else if (s->vdelay <= s->delay_min) {
                s->dir = 1;
                s->prev_vpts = s->vpts;
            }
            ff_fill_rectangle(&s->draw, &s->white, out->data, out->linesize,
                              outlink->w * .35, outlink->h * .35, outlink->w * .25, outlink->h * .25);
            s->flash = 0;
        }
    }
    s->previ = intpart;

    for (i = 0; i < s->delay_range; i++) {
        ff_fill_rectangle(&s->draw, &s->white, out->data, out->linesize,
                          offset + step * i, outlink->h * .7, 1, outlink->h * .05);
    }

    out->pts = s->vpts++;

    return ff_filter_frame(outlink, out);
}

static const AVFilterPad avsynctest_outputs[] = {
    {
        .name          = "audio",
        .type          = AVMEDIA_TYPE_AUDIO,
        .request_frame = arequest_frame,
        .config_props  = aconfig_props,
    },
    {
        .name          = "video",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame,
        .config_props  = config_props,
    },
    { NULL }
};

AVFilter ff_avsrc_avsynctest = {
    .name          = "avsynctest",
    .description   = NULL_IF_CONFIG_SMALL("Generate an Audio Video Sync Test."),
    .query_formats = query_formats,
    .priv_size     = sizeof(AVSyncTestContext),
    .inputs        = NULL,
    .outputs       = avsynctest_outputs,
    .priv_class    = &avsynctest_class,
};
