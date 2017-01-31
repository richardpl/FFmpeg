/*
 * HDMV TextST decoder
 *
 * Copyright (c) 2014 Petri Hintukainen <phintuka@users.sourceforge.net>
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

#include <string.h>

#include "libavutil/bprint.h"
#include "libavutil/colorspace.h"

#include "avcodec.h"
#include "ass.h"
#include "bytestream.h"
#include "mathops.h"

/* TODO: actually make use of various styles */

enum {
    DIALOG_STYLE_SEGMENT        = 0x81,
    DIALOG_PRESENTATION_SEGMENT = 0x82,
};

enum {
    BD_TEXTST_DATA_ESCAPE      = 0x1b,
    BD_TEXTST_DATA_STRING      = 1,
    BD_TEXTST_DATA_FONT_ID     = 2,
    BD_TEXTST_DATA_FONT_STYLE  = 3,
    BD_TEXTST_DATA_FONT_SIZE   = 4,
    BD_TEXTST_DATA_FONT_COLOR  = 5,
    BD_TEXTST_DATA_NEWLINE     = 0x0a,
    BD_TEXTST_DATA_RESET_STYLE = 0x0b,
};

typedef struct TextSTRect {
    uint16_t xpos;
    uint16_t ypos;
    uint16_t width;
    uint16_t height;
} TextSTRect;

typedef struct TextSTRegionInfo {
    TextSTRect region;
    uint8_t    background_color; /* palette entry id ref */
} TextSTRegionInfo;

typedef struct TextSTFontStyle {
    uint8_t bold;
    uint8_t italic;
    uint8_t outline_border;
} TextSTFontStyle;

typedef struct TextSTRegionStyle {
    uint8_t          region_style_id;
    TextSTRegionInfo region_info;
    TextSTRect       text_box;          /* relative to region */
    uint8_t          text_flow;
    uint8_t          text_halign;
    uint8_t          text_valign;
    uint8_t          line_space;
    uint8_t          font_id_ref;
    TextSTFontStyle  font_style;
    uint8_t          font_size;
    uint8_t          font_color;        /* palette entry id ref */
    uint8_t          outline_color;     /* palette entry id ref */
    uint8_t          outline_thickness;
} TextSTRegionStyle;

typedef struct TextSTUserStyle {
    uint8_t user_style_id;
    int16_t region_hpos_delta;
    int16_t region_vpos_delta;
    int16_t text_box_hpos_delta;
    int16_t text_box_vpos_delta;
    int16_t text_box_width_delta;
    int16_t text_box_height_delta;
    int8_t  font_size_delta;
    int8_t  line_space_delta;
} TextSTUserStyle;


typedef struct TextSTContext {
    FFASSDecoderContext ass;

    uint32_t palette[256];
    int region_style_count;
    int user_style_count;
    TextSTRegionStyle *region_styles;
    TextSTUserStyle   *user_styles;
} TextSTContext;

static void decode_region_data(AVCodecContext *avctx, GetByteContext *gb, AVBPrint *sub)
{
    while (bytestream2_get_bytes_left(gb) > 2) {
        unsigned int code, type, length;

        /* parse header */
        code = bytestream2_get_byte(gb);
        if (code != BD_TEXTST_DATA_ESCAPE) {
            continue;
        }
        type   = bytestream2_get_byte(gb);
        length = bytestream2_get_byte(gb);

        /* parse content */
        if (length > bytestream2_get_bytes_left(gb)) {
            av_log(avctx, AV_LOG_WARNING, "decode_dialog_region(): unexpected end of data\n");
            return;
        }

        switch (type) {
        case BD_TEXTST_DATA_STRING:
            av_bprint_append_data(sub, gb->buffer, length);
            break;
        case BD_TEXTST_DATA_NEWLINE:
            av_bprint_append_data(sub, "\\N", 2);
            break;
        default:
            break;
        }

        bytestream2_skip(gb, length);
    }
}

static int decode_region(AVCodecContext *avctx, GetByteContext *gb, AVBPrint *sub,
                         int *forced_on_flag, int *region_style_id_ref)
{
    GetByteContext gb_region;
    int flags, data_length;

    flags = bytestream2_get_byte(gb);
    /*continous_present_flag = !!(flags & 0x80);*/
    *forced_on_flag      = !!(flags & 0x40);
    *region_style_id_ref = bytestream2_get_byte(gb);
    data_length          = bytestream2_get_be16(gb);

    if (data_length > bytestream2_get_bytes_left(gb)) {
        av_log(avctx, AV_LOG_WARNING, "decode_dialog_region(): unexpected end of data\n");
        return -1;
    }

    bytestream2_init(&gb_region, gb->buffer, data_length);
    decode_region_data(avctx, &gb_region, sub);
    bytestream2_skip(gb, data_length);
    av_bprintf(sub, "\r\n");

    return 1;
}

static int64_t decode_pts(GetByteContext *gb)
{
    return (((uint64_t)bytestream2_get_byte(gb) & 1) << 32) | bytestream2_get_be32(gb);
}

static int decode_palette(AVCodecContext *avctx, GetByteContext *gb)
{
    TextSTContext *s = avctx->priv_data;
    unsigned length;

    length = bytestream2_get_be16(gb);

    if (length > bytestream2_get_bytes_left(gb)) {
        av_log(avctx, AV_LOG_WARNING, "decode_palette(): unexpected end of data\n");
        return -1;
    }

    while (length > 4) {
        const uint8_t *cm = ff_crop_tab + MAX_NEG_CROP;
        int index = bytestream2_get_byte(gb);
        int y = bytestream2_get_byte(gb);
        int cb = bytestream2_get_byte(gb);
        int cr = bytestream2_get_byte(gb);
        int t = bytestream2_get_byte(gb);
        int r_add, g_add, b_add;
        int r, g, b;

        YUV_TO_RGB1_CCIR(cb, cr);
        YUV_TO_RGB2_CCIR(r, g, b, y);

        s->palette[index] = ((0xFF - t) << 24) | (r << 16) | (g << 8) | b;
        length -= 5;
    }

    return 1;
}

static void decode_rect(GetByteContext *gb, TextSTRect *rect)
{
    rect->xpos   = bytestream2_get_be16(gb);
    rect->ypos   = bytestream2_get_be16(gb);
    rect->width  = bytestream2_get_be16(gb);
    rect->height = bytestream2_get_be16(gb);
}

static void decode_region_info(AVCodecContext *avctx, GetByteContext *gb, TextSTRegionInfo *region_info)
{
    decode_rect(gb, &region_info->region);
    region_info->background_color = bytestream2_get_byte(gb);
    bytestream2_skip(gb, 1);
}

static void decode_font_style(GetByteContext *gb, TextSTFontStyle *font_style)
{
    int flag = bytestream2_get_byte(gb);

    font_style->bold           = !!(flag & 1);
    font_style->italic         = !!(flag & 2);
    font_style->outline_border = !!(flag & 4);
}

static void decode_region_style(AVCodecContext *avctx, GetByteContext *gb, TextSTRegionStyle *style)
{
    style->region_style_id = bytestream2_get_byte(gb);
    decode_region_info(avctx, gb, &style->region_info);
    decode_rect(gb, &style->text_box);
    style->text_flow   = bytestream2_get_byte(gb);
    style->text_halign = bytestream2_get_byte(gb);
    style->text_valign = bytestream2_get_byte(gb);
    style->line_space  = bytestream2_get_byte(gb);
    style->font_id_ref = bytestream2_get_byte(gb);
    decode_font_style(gb, &style->font_style);
    style->font_size   = bytestream2_get_byte(gb);
    style->font_color  = bytestream2_get_byte(gb);
    style->outline_color = bytestream2_get_byte(gb);
    style->outline_thickness = bytestream2_get_byte(gb);
}

static void decode_user_style(AVCodecContext *avctx, GetByteContext *gb, TextSTUserStyle *style)
{
    style->user_style_id         = bytestream2_get_byte(gb);
    style->region_hpos_delta     = bytestream2_get_be16(gb);
    style->region_vpos_delta     = bytestream2_get_be16(gb);
    style->text_box_hpos_delta   = bytestream2_get_be16(gb);
    style->text_box_vpos_delta   = bytestream2_get_be16(gb);
    style->text_box_width_delta  = bytestream2_get_be16(gb);
    style->text_box_height_delta = bytestream2_get_be16(gb);
    style->font_size_delta       = bytestream2_get_byte(gb);
    style->line_space_delta      = bytestream2_get_byte(gb);
}

static void decode_style_segment(AVCodecContext *avctx, GetByteContext *gb, AVSubtitle *sub)
{
    TextSTContext *s = avctx->priv_data;
    int i;

    bytestream2_skip(gb, 2);
    s->region_style_count = bytestream2_get_byte(gb);
    s->user_style_count   = bytestream2_get_byte(gb);

    if (s->region_style_count) {
        av_freep(&s->region_styles);
        s->region_styles = av_calloc(s->region_style_count, sizeof(TextSTRegionStyle));
        if (!s->region_styles) {
            s->region_style_count = 0;
            return;
        }

        for (i = 0; i < s->region_style_count; i++) {
            decode_region_style(avctx, gb, &s->region_styles[i]);
        }
    }

    if (s->user_style_count) {
        av_freep(&s->user_styles);
        s->user_styles = av_calloc(s->user_style_count, sizeof(TextSTUserStyle));
        if (!s->user_styles) {
            s->user_style_count = 0;
            return;
        }

        for (i = 0; i < s->user_style_count; i++) {
            decode_user_style(avctx, gb, &s->user_styles[i]);
        }
    }

    decode_palette(avctx, gb);
}

static void decode_presentation_segment(AVCodecContext *avctx, GetByteContext *gb, AVSubtitle *sub)
{
    TextSTContext *s = avctx->priv_data;
    unsigned ii, palette_update_flag, region_count;
    int64_t start_pts, end_pts;

    start_pts = decode_pts(gb);
    end_pts   = decode_pts(gb);

    sub->pts = start_pts * 100 / 9;
    sub->start_display_time = 0;
    sub->end_display_time = (end_pts - start_pts) / 100;

    palette_update_flag = bytestream2_get_byte(gb) >> 7;
    if (palette_update_flag) {
        if (decode_palette(avctx, gb) < 0) {
            return;
        }
    }

    region_count = bytestream2_get_byte(gb);
    if (region_count > 2) {
        av_log(avctx, AV_LOG_WARNING, "too many regions (%d)\n", region_count);
        return;
    }

    for (ii = 0; ii < region_count; ii++) {
        AVBPrint buffer;
        char *dec_sub;
        int forced_on_flag, region_style_id_ref;

        av_bprint_init(&buffer, 1024, 1024);
        if (decode_region(avctx, gb, &buffer, &forced_on_flag, &region_style_id_ref) < 0) {
            av_bprint_finalize(&buffer, NULL);
            return;
        }
        av_bprint_finalize(&buffer, &dec_sub);

        ff_ass_add_rect(sub, dec_sub, s->ass.readorder++, 0, NULL, NULL);
        av_free(dec_sub);

        if (forced_on_flag && sub->num_rects > 0) {
            sub->rects[sub->num_rects - 1]->flags |= AV_SUBTITLE_FLAG_FORCED;
        }
    }

    if (bytestream2_get_bytes_left(gb)) {
        av_log(avctx, AV_LOG_WARNING, "unknown data after dialog segment (%d bytes)\n", bytestream2_get_bytes_left(gb));
    }
}

static int textst_decode_frame(AVCodecContext *avctx,
                               void *data, int *got_sub_ptr, AVPacket *avpkt)
{
    AVSubtitle *sub = data;
    int segment_type, segment_size;
    GetByteContext gb;

    if (avpkt->size < 3) {
        return avpkt->size;
    }

    bytestream2_init(&gb, avpkt->data, avpkt->size);

    segment_type = bytestream2_get_byte(&gb);
    segment_size = bytestream2_get_be16(&gb);

    if (avpkt->size < segment_size + 3) {
        av_log(avctx, AV_LOG_WARNING, "segment 0x%02x size mismatch: segment %d bytes, packet %d bytes\n",
               segment_type, segment_size, avpkt->size);
        return avpkt->size;
    }

    switch (segment_type) {
    case DIALOG_STYLE_SEGMENT:
        decode_style_segment(avctx, &gb, sub);
        break;
    case DIALOG_PRESENTATION_SEGMENT:
        decode_presentation_segment(avctx, &gb, sub);
        break;
    default:
        av_log(avctx, AV_LOG_WARNING, "unknown segment type 0x%02x\n", segment_type);
        break;
    }

    *got_sub_ptr = sub->num_rects > 0;

    return avpkt->size;
}

static av_cold int textst_init(AVCodecContext *avctx)
{
    TextSTContext *s = avctx->priv_data;
    int ret, i;

    for (i = 0; i < 256; i++)
        s->palette[i] = 0xFFFFFFFF;

    ret = ff_ass_subtitle_header_default(avctx);

    return ret;
}

AVCodec ff_textst_decoder = {
    .name           = "textst",
    .long_name      = NULL_IF_CONFIG_SMALL("HDMV TextST subtitle"),
    .type           = AVMEDIA_TYPE_SUBTITLE,
    .id             = AV_CODEC_ID_HDMV_TEXT_SUBTITLE,
    .decode         = textst_decode_frame,
    .init           = textst_init,
    .flush          = ff_ass_decoder_flush,
    .priv_data_size = sizeof(TextSTContext),
};
