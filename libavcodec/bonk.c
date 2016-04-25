/*
 * Bonk audio decoder
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
#include "avcodec.h"
#define BITSTREAM_READER_LE
#include "get_bits.h"
#include "bytestream.h"
#include "internal.h"

typedef struct BitCount {
    uint8_t bit;
    unsigned count;
} BitCount;

typedef struct BonkContext {
    GetBitContext gb;
    int skip;

    uint8_t *bitstream;
    int max_framesize;
    int bitstream_size;
    int bitstream_index;

    uint64_t nb_samples;
    int lossless;
    int mid_side;
    int n_taps;
    int down_sampling;
    int samples_per_packet;

    int state[2][2048], k[2048];
    int *samples;
    int *input_samples;
    uint8_t quant[2048];
    BitCount *bits;
} BonkContext;

static av_cold int bonk_close(AVCodecContext *avctx)
{
    BonkContext *s = avctx->priv_data;

    av_freep(&s->bitstream);
    av_freep(&s->input_samples);
    av_freep(&s->samples);
    av_freep(&s->bits);
    s->bitstream_size = 0;

    return 0;
}

static av_cold int bonk_init(AVCodecContext *avctx)
{
    BonkContext *s = avctx->priv_data;
    int i;

    avctx->sample_fmt     = AV_SAMPLE_FMT_S16;
    if (avctx->extradata_size < 17)
        return AVERROR(EINVAL);

    if (avctx->channels < 1 || avctx->channels > 2)
        return AVERROR_INVALIDDATA;

    s->nb_samples = AV_RL32(avctx->extradata + 1) / avctx->channels;
    if (!s->nb_samples)
        s->nb_samples = UINT64_MAX;
    s->lossless = avctx->extradata[10] != 0;
    s->mid_side = avctx->extradata[11] != 0;
    s->n_taps = AV_RL16(avctx->extradata + 12);
    if (!s->n_taps || s->n_taps > 2048)
        return AVERROR(EINVAL);

    s->down_sampling = avctx->extradata[14];
    if (!s->down_sampling)
        return AVERROR(EINVAL);

    s->samples_per_packet = AV_RL16(avctx->extradata + 15);
    if (!s->samples_per_packet)
        return AVERROR(EINVAL);
    s->max_framesize = s->samples_per_packet * avctx->channels * s->down_sampling * 8;
    s->bitstream = av_calloc(s->max_framesize + AV_INPUT_BUFFER_PADDING_SIZE, sizeof(*s->bitstream));
    if (!s->bitstream)
        return AVERROR(ENOMEM);

    s->input_samples = av_calloc(s->samples_per_packet, sizeof(*s->input_samples));
    if (!s->input_samples)
        return AVERROR(ENOMEM);

    s->samples = av_calloc(s->samples_per_packet * s->down_sampling * avctx->channels, sizeof(*s->samples));
    if (!s->samples)
        return AVERROR(ENOMEM);

    s->bits = av_calloc(s->max_framesize * 8, sizeof(*s->bits));
    if (!s->bits)
        return AVERROR(ENOMEM);

    for (i = 0; i < 512; i++) {
        s->quant[i] = sqrt(i + 1);
    }

    return 0;
}

static int bits_to_store(uint32_t x)
{
    int res = 0;

    while (x) {
        res++;
        x >>= 1;
    }

    return res;
}

static unsigned read_uint_max(BonkContext *s, int max)
{
    unsigned value = 0;
    int i, bits;

    if (max == 0)
        return 0;

    bits = bits_to_store(max);

    for (i = 0; i < bits - 1; i++)
        if (get_bits1(&s->gb))
            value += 1 << i;

    if ((value | (1 << (bits - 1))) <= max)
        if (get_bits1(&s->gb))
            value += 1 << (bits - 1);

    return value;
}

static int intlist_read(BonkContext *s, int *buf, int entries, int base_2_part)
{
    int i, low_bits = 0, x = 0, max_x;
    int n_zeros = 0, step = 256, dominant = 0;
    int pos = 0, level = 0;
    BitCount *bits = s->bits;

    if (base_2_part) {
        low_bits = get_bits(&s->gb, 4);

        if (low_bits) {
            for (i = 0; i < entries; i++)
                buf[i] = get_bits(&s->gb, low_bits);
        } else {
            memset(buf, 0, entries * sizeof(*buf));
        }
    } else {
        memset(buf, 0, entries * sizeof(*buf));
    }

    while (n_zeros < entries) {
        int steplet = step >> 8;

        if (get_bits_left(&s->gb) <= 0)
            return AVERROR_INVALIDDATA;

        if (!get_bits1(&s->gb)) {
            if (steplet > 0) {
                bits[x  ].bit   = dominant;
                bits[x++].count = steplet;
            }

            if (!dominant)
                n_zeros += steplet;

            step += step / 8;
        } else {
            int actual_run = read_uint_max(s, steplet - 1);

            if (actual_run > 0) {
                bits[x  ].bit   = dominant;
                bits[x++].count = actual_run;
            }

            bits[x  ].bit   = !dominant;
            bits[x++].count = 1;

            if (!dominant)
                n_zeros += actual_run;
            else
                n_zeros++;

            step -= step / 8;
        }

        if (step < 256) {
            if (step == 0)
                return AVERROR_INVALIDDATA;
            step = 65536 / step;
            dominant = !dominant;
        }
    }

    max_x = x;
    x = 0;
    n_zeros = 0;
    for (i = 0; n_zeros < entries; i++) {
        while (1) {
            if (pos >= entries) {
                pos = 0;
                level += 1 << low_bits;
            }

            if (buf[pos] >= level)
                break;

            pos++;
        }

        if (x >= max_x)
            return AVERROR_INVALIDDATA;
        if (bits[x].bit)
            buf[pos] += 1 << low_bits;
        else
            n_zeros++;

        bits[x].count--;
        if (bits[x].count == 0)
            x++;

        pos++;
    }

    for (i = 0; i < entries; i++) {
        if (buf[i] && get_bits1(&s->gb)) {
            buf[i] = -buf[i];
        }
    }

    return 0;
}

static inline int shift_down(int a, int b)
{
    return (a >> b) + (a < 0);
}

static inline int shift(int a, int b)
{
    return a + (1 << b - 1) >> b;
}

#define LATTICE_SHIFT 10
#define SAMPLE_SHIFT   4
#define SAMPLE_FACTOR (1 << SAMPLE_SHIFT)

static int predictor_calc_error(int *k, int *state, int order, int error)
{
    int i, x = error - shift_down(k[order-1] * state[order-1], LATTICE_SHIFT);
    int *k_ptr = &(k[order-2]),
        *state_ptr = &(state[order-2]);

    for (i = order-2; i >= 0; i--, k_ptr--, state_ptr--) {
        int k_value = *k_ptr, state_value = *state_ptr;

        x -= shift_down(k_value * state_value, LATTICE_SHIFT);
        state_ptr[1] = state_value + shift_down(k_value * x, LATTICE_SHIFT);
    }

    // don't drift too far, to avoid overflows
    av_clip(x, -(SAMPLE_FACTOR << 16), SAMPLE_FACTOR << 16);

    state[0] = x;

    return x;
}

static void predictor_init_state(int *k, int *state, int order)
{
    int i;

    for (i = order - 2; i >= 0; i--) {
        int j, p, x = state[i];

        for (j = 0, p = i + 1; p < order; j++,p++) {
            int tmp = x + shift_down(k[j] * state[p], LATTICE_SHIFT);

            state[p] += shift_down(k[j] * x, LATTICE_SHIFT);
            x = tmp;
        }
    }
}

static int bonk_decode(AVCodecContext *avctx, void *data,
                       int *got_frame_ptr, AVPacket *pkt)
{
    BonkContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;
    AVFrame *frame = data;
    const uint8_t *buf;
    int16_t *samples;
    int i, j, ch, quant, n, buf_size, input_buf_size;
    int ret = AVERROR_INVALIDDATA;

    if ((!pkt->size && !s->bitstream_size) || s->nb_samples == 0) {
        *got_frame_ptr = 0;
        return pkt->size;
    }

    buf_size = FFMIN(pkt->size, s->max_framesize - s->bitstream_size);
    input_buf_size = buf_size;
    if (s->bitstream_index + s->bitstream_size + buf_size + AV_INPUT_BUFFER_PADDING_SIZE > s->max_framesize) {
        memmove(s->bitstream, &s->bitstream[s->bitstream_index], s->bitstream_size);
        s->bitstream_index = 0;
    }
    if (pkt->data)
        memcpy(&s->bitstream[s->bitstream_index + s->bitstream_size], pkt->data, buf_size);
    buf                = &s->bitstream[s->bitstream_index];
    buf_size          += s->bitstream_size;
    s->bitstream_size  = buf_size;
    if (buf_size < s->max_framesize && pkt->data) {
        *got_frame_ptr = 0;
        return input_buf_size;
    }

    frame->nb_samples = FFMIN(s->samples_per_packet * s->down_sampling, s->nb_samples);
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    samples = (int16_t *)frame->data[0];

    init_get_bits8(gb, buf, buf_size);
    skip_bits(gb, s->skip);

    if ((ret = intlist_read(s, s->k, s->n_taps, 0)) < 0)
        goto fail;

    for (i = 0; i < s->n_taps; i++) {
        s->k[i] *= s->quant[i];
    }
    quant = s->lossless ? 1 : get_bits(&s->gb, 16) * SAMPLE_FACTOR;

    for (ch = 0; ch < avctx->channels; ch++) {
        int *sample = s->samples + ch;

        predictor_init_state(s->k, s->state[ch], s->n_taps);
        if ((ret = intlist_read(s, s->input_samples, s->samples_per_packet, 1)) < 0)
            goto fail;

        for (i = 0; i < s->samples_per_packet; i++) {
            for (j = 0; j < s->down_sampling - 1; j++) {
                *sample = predictor_calc_error(s->k, s->state[ch], s->n_taps, 0);
                sample += avctx->channels;
            }

            *sample = predictor_calc_error(s->k, s->state[ch], s->n_taps, s->input_samples[i] * quant);
            sample += avctx->channels;
        }

        for (i = 0; i < s->n_taps; i++)
            s->state[ch][i] = s->samples[s->samples_per_packet * s->down_sampling * avctx->channels -
                                         avctx->channels + ch - i * avctx->channels];
    }

    if (s->mid_side && avctx->channels == 2) {
        for (i = 0; i < frame->nb_samples * 2; i += 2) {
            s->samples[i + 1] += shift(s->samples[i], 1);
            s->samples[i]     -= s->samples[i + 1];
        }
    }

    if (!s->lossless) {
        for (i = 0; i < frame->nb_samples * avctx->channels; i++)
            s->samples[i] = shift(s->samples[i], 4);
    }

    for (i = 0; i < frame->nb_samples * avctx->channels; i++) {
        samples[i] = av_clip_int16(s->samples[i]);
    }

    s->nb_samples -= frame->nb_samples;

    s->skip = get_bits_count(gb) - 8 * (get_bits_count(gb) / 8);
    n = get_bits_count(gb) / 8;

    if (n > buf_size) {
fail:
        s->bitstream_size = 0;
        s->bitstream_index = 0;
        return ret;
    }

    *got_frame_ptr = 1;

    if (s->bitstream_size) {
        s->bitstream_index += n;
        s->bitstream_size  -= n;
        return input_buf_size;
    }
    return n;
}

AVCodec ff_bonk_decoder = {
    .name           = "bonk",
    .long_name      = NULL_IF_CONFIG_SMALL("Bonk"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_BONK,
    .init           = bonk_init,
    .decode         = bonk_decode,
    .close          = bonk_close,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(BonkContext),
};
