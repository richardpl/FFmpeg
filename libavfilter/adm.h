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

#ifndef AVFILTER_ADM_H
#define AVFILTER_ADM_H
/** Formula (1), page 1165 - display visual resolution (DVR),
 * in pixels/degree of visual angle. This should be 56.55
 */
#define R 56.55
/** Percentage of frame to discard on all 4 sides */
#define ADM_BORDER_FACTOR (0.1)

#define N 15

typedef struct adm_dwt_band_t {
    int16_t *band_a; /** Low-pass V + low-pass H. */
    int16_t *band_v; /** Low-pass V + high-pass H. */
    int16_t *band_h; /** High-pass V + low-pass H. */
    int16_t *band_d; /** High-pass V + high-pass H. */
} adm_dwt_band_t;

static const float dwt2_db2_coeffs_lo[4] = {
    0.482962913144690,  0.836516303737469,
    0.224143868041857, -0.129409522550921
};
static const float dwt2_db2_coeffs_hi[4] = {
    -0.129409522550921, -0.224143868041857,
    0.836516303737469,  -0.482962913144690
};

static int32_t dwt2_db2_coeffs_lo_int[4];
static int32_t dwt2_db2_coeffs_hi_int[4];

/**
 * The following dwt basis function amplitudes, Q(lambda,theta), are taken from
 * "Visibility of Wavelet Quantization Noise"
 * by A. B. Watson, G. Y. Yang, J. A. Solomon and J. Villasenor
 * IEEE Trans. on Image Processing, Vol. 6, No 8, Aug. 1997
 * Page 1172, Table V
 * The table has been transposed, i.e. it can be used directly to obtain Q[lambda][theta]
 * These amplitudes were calculated for the 7-9 biorthogonal wavelet basis
 */
static const float Q[4][2] = {
    { 57.534645,  169.767410, },
    { 31.265896,  69.937431,  },
    { 23.056629,  40.990150,  },
    { 21.895033,  31.936741,  },
};

/** function to compute adm score */
int compute_adm2(const void *ref, const void *main, int w, int h,
                 ptrdiff_t ref_stride, ptrdiff_t main_stride, double *score,
                 double *score_num, double *score_den, double *scores,
                 int16_t *data_buf, int16_t *temp_lo, int16_t *temp_hi,
                 uint8_t type);

#endif /* AVFILTER_ADM_H */
