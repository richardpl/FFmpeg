/*
 * Apple ProRes compatible decoder
 *
 * Copyright (c) 2010-2011 Maxim Poliakovski
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

#include "config.h"
#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "idctdsp.h"
#include "proresdsp.h"
#include "simple_idct.h"

#define CLIP10(x) (av_clip((x), (1 << (10 - 8)), (1 << 10) - (1 << (10 - 8)) - 1))
#define CLIP12(x) (av_clip((x), (1 << (12 - 8)), (1 << 12) - (1 << (12 - 8)) - 1))

/**
 * Clamp and output pixels of a slice
 */
#define PUT_PIXELS(depth)                                                    \
static void put_pixels##depth(uint16_t *dst, ptrdiff_t linesize, const int16_t *in) \
{                                                                            \
    int x, y, src_offset, dst_offset;                                        \
                                                                             \
    for (y = 0, dst_offset = 0; y < 8; y++, dst_offset += linesize) {        \
        for (x = 0; x < 8; x++) {                                            \
            src_offset = (y << 3) + x;                                       \
                                                                             \
            dst[dst_offset + x] = CLIP##depth(in[src_offset]);               \
        }                                                                    \
    }                                                                        \
}                                                                            \
                                                                             \
static void prores_idct_put##depth##_c(uint16_t *out, ptrdiff_t linesize,    \
                              int16_t *block, const int16_t *qmat)           \
{                                                                            \
    ff_prores_idct##depth(block, qmat);                                      \
    put_pixels##depth(out, linesize >> 1, block);                            \
}


PUT_PIXELS(10)
PUT_PIXELS(12)

av_cold void ff_proresdsp_init(ProresDSPContext *dsp, AVCodecContext *avctx)
{
    dsp->idct_put10 = prores_idct_put10_c;
    dsp->idct_put12 = prores_idct_put12_c;
    dsp->idct_permutation_type = FF_IDCT_PERM_NONE;

    if (ARCH_X86)
        ff_proresdsp_init_x86(dsp, avctx);

    ff_init_scantable_permutation(dsp->idct_permutation,
                                  dsp->idct_permutation_type);
}
