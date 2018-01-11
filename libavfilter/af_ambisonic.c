/*
 * Copyright (c) 2020 Paul B Mahol
 * Copyright (c) 2017 Sanchit Sinha
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

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/float_dsp.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "audio.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include <math.h>
#include <stdio.h>

#define EVEN 0
#define ODD 1
#define MAX_ORDER 5
#define SQR(x) ((x) * (x))
#define MAX_CHANNELS SQR(MAX_ORDER + 1)

enum NearFieldType {
    NF_AUTO = -1,
    NF_NONE,
    NF_IN,
    NF_OUT,
    NB_NFTYPES,
};

enum PrecisionType {
    SINGLE,
    DOUBLE,
    NB_PTYPES,
};

enum NormType {
    N3D,
    SN3D,
    FUMA,
    NB_NTYPES,
};

enum DirectionType {
    D_X,
    D_Y,
    D_Z,
    D_C,
    NB_DTYPES,
};

enum SequenceType {
    M_ACN,
    M_FUMA,
    M_SID,
    NB_MTYPES,
};

enum Layouts {
    SAME = -1,
    MONO,
    STEREO,
    QUAD,
    L4_0,
    NB_LAYOUTS,
};

typedef struct NearField {
    double g;
    double d[MAX_ORDER];
    double z[MAX_ORDER];
} NearField;

typedef struct Xover {
    double b[3];
    double a[3];
    double w[2];
} Xover;

static const double mono_mat[] =
{
    1.,
};

static const double stereo_mat[] =
{
    1.,  1., 0., 1.,
    1., -1., 0., 1.,
};

static const double stereo_distance[] =
{
    1., 1.,
};

static const double quad_distance[] =
{
    2., 2., 2., 2.,
};

static const double quad_lf_gains[] =
{
    1., 1.,
};

static const double quad_hf_gains[] =
{
    1.41421, 0.99985,
};

static const double quad_mat[] =
{
    0.353554,  0.500000, 0.,  0.288675,
    0.353554, -0.500000, 0.,  0.288675,
    0.353554,  0.500000, 0., -0.288675,
    0.353554, -0.500000, 0., -0.288675,
};

static const double l4_0_mat[] =
{
    1.,  1., 0.,  0.,
    1., -1., 0.,  0.,
    1.,  0., 0.,  1.,
    1.,  0., 0., -1.,
};

static const struct {
    int           order;
    int           inputs;
    int           speakers;
    int           near_field;
    double        xover;
    uint64_t      outlayout;
    const double *mat;
    const double *gains[2];
    const double *speakers_distance;
} ambisonic_tab[] = {
    [MONO] = {
        .order = 0,
        .inputs = 1,
        .speakers = 1,
        .near_field = NF_NONE,
        .xover = 0.,
        .outlayout = AV_CH_LAYOUT_MONO,
        .mat = mono_mat,
        .speakers_distance = mono_mat,
    },
    [STEREO] = {
        .order = 1,
        .inputs = 4,
        .speakers = 2,
        .near_field = NF_NONE,
        .xover = 0.,
        .outlayout = AV_CH_LAYOUT_STEREO,
        .mat = stereo_mat,
        .speakers_distance = stereo_distance,
    },
    [QUAD] = {
        .order = 1,
        .inputs = 4,
        .speakers = 4,
        .near_field = NF_IN,
        .xover = 300.,
        .outlayout = AV_CH_LAYOUT_QUAD,
        .mat = quad_mat,
        .gains[0] = quad_lf_gains,
        .gains[1] = quad_hf_gains,
        .speakers_distance = quad_distance,
    },
    [L4_0] = {
        .order = 1,
        .inputs = 4,
        .speakers = 4,
        .near_field = NF_NONE,
        .xover = 0.,
        .outlayout = AV_CH_LAYOUT_4POINT0,
        .mat = l4_0_mat,
        .speakers_distance = quad_distance,
    }
};

typedef struct AmbisonicContext {
    const AVClass *class;
    int order;                    /* Order of ambisonic */
    int level;                    /* Output Level compensation */
    enum Layouts layout;          /* Output speaker layout */
    enum NormType norm;           /* Normalization Type */
    enum PrecisionType precision; /* Processing Precision Type */
    enum SequenceType seq;        /* Input Channel sequence type */
    enum NearFieldType near_field; /* Near Field compensation type */

    int invert[NB_DTYPES];        /* Axis Odd/Even Invert */
    double gain[2][NB_DTYPES];    /* Axis Odd/Even Gains */

    double yaw;                   /* Angle for yaw(x) rotation */
    double pitch;                 /* Angle for pitch(y) rotation */
    double roll;                  /* Angle for roll(z) rotation */

    int max_channels;             /* Max Channels */

    double temp;
    double xover_freq;
    double xover_ratio;

    Xover xover[2][MAX_CHANNELS];
    NearField nf[2][MAX_CHANNELS];

    int    seq_tab[NB_MTYPES][MAX_CHANNELS];
    double norm_tab[NB_NTYPES][MAX_CHANNELS];
    double rotate_mat[MAX_CHANNELS][MAX_CHANNELS];
    double mirror_mat[MAX_CHANNELS];
    double level_tab[MAX_CHANNELS];
    double gains_tab[2][MAX_CHANNELS];

    AVFrame *frame;
    AVFrame *frame2;

    void (*nf_init[MAX_ORDER])(NearField *nf, double radius,
                               double speed, double rate,
                               double gain);
    void (*nf_process[MAX_ORDER])(NearField *nf,
                                  AVFrame *frame,
                                  int ch, int add,
                                  double gain);
    void (*process)(AVFilterContext *ctx, AVFrame *in, AVFrame *out);

    AVFloatDSPContext *fdsp;
} AmbisonicContext;

#define OFFSET(x) offsetof(AmbisonicContext,x)
#define AF AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption ambisonic_options[] = {
    { "layout", "layout of output", OFFSET(layout), AV_OPT_TYPE_INT, {.i64=STEREO}, SAME, NB_LAYOUTS-1, AF , "lyt"},
    {   "mono",   "mono layout",   0, AV_OPT_TYPE_CONST, {.i64=MONO},   0, 0, AF , "lyt"},
    {   "stereo", "stereo layout", 0, AV_OPT_TYPE_CONST, {.i64=STEREO}, 0, 0, AF , "lyt"},
    {   "quad",   "quad layout",   0, AV_OPT_TYPE_CONST, {.i64=QUAD},   0, 0, AF , "lyt"},
    {   "4.0",    "4.0 layout",    0, AV_OPT_TYPE_CONST, {.i64=L4_0},   0, 0, AF , "lyt"},
    { "sequence", "input channel sequence", OFFSET(seq), AV_OPT_TYPE_INT, {.i64=M_ACN},  0, NB_MTYPES-1, AF, "seq"},
    {   "acn",  "ACN",  0, AV_OPT_TYPE_CONST, {.i64=M_ACN},  0, 0, AF, "seq"},
    {   "fuma", "FuMa", 0, AV_OPT_TYPE_CONST, {.i64=M_FUMA}, 0, 0, AF, "seq"},
    {   "sid",  "SID",  0, AV_OPT_TYPE_CONST, {.i64=M_SID},  0, 0, AF, "seq"},
    { "scaling", "input scaling format", OFFSET(norm), AV_OPT_TYPE_INT,   {.i64=SN3D}, 0, NB_NTYPES-1, AF, "scl"},
    {   "n3d",  "N3D scaling (normalised)",       0, AV_OPT_TYPE_CONST, {.i64=N3D},  0, 0, AF, "scl"},
    {   "sn3d", "SN3D scaling (semi-normalised)", 0, AV_OPT_TYPE_CONST, {.i64=SN3D}, 0, 0, AF, "scl"},
    {   "fuma", "furse malham scaling",           0, AV_OPT_TYPE_CONST, {.i64=FUMA}, 0, 0, AF, "scl"},
    { "nearfield", "near-field compenstation", OFFSET(near_field), AV_OPT_TYPE_INT, {.i64=NF_AUTO}, NF_AUTO, NB_NFTYPES-1, AF, "nf"},
    {   "auto", "auto", 0, AV_OPT_TYPE_CONST, {.i64=NF_AUTO}, 0, 0, AF, "nf"},
    {   "none", "none", 0, AV_OPT_TYPE_CONST, {.i64=NF_NONE}, 0, 0, AF, "nf"},
    {   "in",   "in",   0, AV_OPT_TYPE_CONST, {.i64=NF_IN},   0, 0, AF, "nf"},
    {   "out",  "out",  0, AV_OPT_TYPE_CONST, {.i64=NF_OUT},  0, 0, AF, "nf"},
    { "xoverfreq", "cross-over frequency", OFFSET(xover_freq), AV_OPT_TYPE_DOUBLE, {.dbl=-1.}, -1., 800., AF },
    { "xoverratio", "cross-over HF/LF ratio", OFFSET(xover_ratio), AV_OPT_TYPE_DOUBLE, {.dbl=0.}, -30., 30., AF },
    { "temp", "set temperature °C", OFFSET(temp), AV_OPT_TYPE_DOUBLE, {.dbl=20.}, -50., 50., AF },
    { "yaw",    "angle for yaw (x-axis)",   OFFSET(yaw),   AV_OPT_TYPE_DOUBLE, {.dbl=0.}, -180., 180., AF },
    { "pitch",  "angle for pitch (y-axis)", OFFSET(pitch), AV_OPT_TYPE_DOUBLE, {.dbl=0.}, -180., 180., AF },
    { "roll",   "angle for roll (z-axis)",  OFFSET(roll),  AV_OPT_TYPE_DOUBLE, {.dbl=0.}, -180., 180., AF },
    { "level",  "output level compensation", OFFSET(level), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, AF },
    { "precision", "processing precision", OFFSET(precision), AV_OPT_TYPE_INT, {.i64=SINGLE}, 0, NB_PTYPES-1, AF, "pre"},
    {   "single", "single floating-point precision",   0, AV_OPT_TYPE_CONST, {.i64=SINGLE}, 0, 0, AF, "pre"},
    {   "double", "double floating-point precision" ,  0, AV_OPT_TYPE_CONST, {.i64=DOUBLE}, 0, 0, AF, "pre"},
    { "invert_x", "invert X", OFFSET(invert[D_X]), AV_OPT_TYPE_FLAGS, {.i64=0}, 0, 3, AF, "ix"},
    {   "odd",  "invert odd harmonics",  0, AV_OPT_TYPE_CONST, {.i64=1}, 0, 0, AF, "ix"},
    {   "even", "invert even harmonics", 0, AV_OPT_TYPE_CONST, {.i64=2}, 0, 0, AF, "ix"},
    { "invert_y", "invert Y", OFFSET(invert[D_Y]), AV_OPT_TYPE_FLAGS, {.i64=0}, 0, 3, AF, "iy"},
    {   "odd",  "invert odd harmonics",  0, AV_OPT_TYPE_CONST, {.i64=1}, 0, 0, AF, "iy"},
    {   "even", "invert even harmonics", 0, AV_OPT_TYPE_CONST, {.i64=2}, 0, 0, AF, "iy"},
    { "invert_z", "invert Z", OFFSET(invert[D_Z]), AV_OPT_TYPE_FLAGS, {.i64=0}, 0, 3, AF, "iz"},
    {   "odd",  "invert odd harmonics",  0, AV_OPT_TYPE_CONST, {.i64=1}, 0, 0, AF, "iz"},
    {   "even", "invert even harmonics", 0, AV_OPT_TYPE_CONST, {.i64=2}, 0, 0, AF, "iz"},
    { "invert_c", "circular invert", OFFSET(invert[D_C]), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, AF},
    { "x_odd",  "X odd harmonics gain",  OFFSET(gain[ODD][D_X]),  AV_OPT_TYPE_DOUBLE, {.dbl=1.}, 0, 2., AF },
    { "x_even", "X even harmonics gain", OFFSET(gain[EVEN][D_X]), AV_OPT_TYPE_DOUBLE, {.dbl=1.}, 0, 2., AF },
    { "y_odd",  "Y odd harmonics gain",  OFFSET(gain[ODD][D_Y]),  AV_OPT_TYPE_DOUBLE, {.dbl=1.}, 0, 2., AF },
    { "y_even", "Y even harmonics gain", OFFSET(gain[EVEN][D_Y]), AV_OPT_TYPE_DOUBLE, {.dbl=1.}, 0, 2., AF },
    { "z_odd",  "Z odd harmonics gain",  OFFSET(gain[ODD][D_Z]),  AV_OPT_TYPE_DOUBLE, {.dbl=1.}, 0, 2., AF },
    { "z_even", "Z even harmonics gain", OFFSET(gain[EVEN][D_Z]), AV_OPT_TYPE_DOUBLE, {.dbl=1.}, 0, 2., AF },
    { "c_gain", "Circular gain",         OFFSET(gain[0][D_C]),    AV_OPT_TYPE_DOUBLE, {.dbl=1.}, 0, 2., AF },
    {NULL}
};

static void levelf(AmbisonicContext *s,
                   AVFrame *out, double level_tab[MAX_CHANNELS],
                   int nb_samples, int nb_channels)
{
    for (int ch = 0; ch < nb_channels; ch++) {
        float *dst = (float *)out->extended_data[ch];
        float mul = level_tab[ch];

        s->fdsp->vector_fmul_scalar(dst, dst, mul, FFALIGN(nb_samples, 16));
    }
}

static void leveld(AmbisonicContext *s,
                   AVFrame *out, double level_tab[MAX_CHANNELS],
                   int nb_samples, int nb_channels)
{
    for (int ch = 0; ch < nb_channels; ch++) {
        double *dst = (double *)out->extended_data[ch];
        double mul = level_tab[ch];

        s->fdsp->vector_dmul_scalar(dst, dst, mul, FFALIGN(nb_samples, 16));
    }
}

static void mirrorf(AmbisonicContext *s,
                    AVFrame *out, double mirror_mat[MAX_CHANNELS],
                    int nb_samples, int nb_channels, int *seq_tab)
{
    for (int ch = 0; ch < nb_channels; ch++) {
        float *dst = (float *)out->extended_data[ch];
        float mul = mirror_mat[seq_tab[ch]];

        s->fdsp->vector_fmul_scalar(dst, dst, mul, FFALIGN(nb_samples, 16));
    }
}

static void mirrord(AmbisonicContext *s,
                    AVFrame *out, double mirror_mat[MAX_CHANNELS],
                    int nb_samples, int nb_channels, int *seq_tab)
{
    for (int ch = 0; ch < nb_channels; ch++) {
        double *dst = (double *)out->extended_data[ch];
        double mul = mirror_mat[seq_tab[ch]];

        s->fdsp->vector_dmul_scalar(dst, dst, mul, FFALIGN(nb_samples, 16));
    }
}

static void rotatef(AmbisonicContext *s,
                    AVFrame *in, AVFrame *out,
                    double rotate_mat[MAX_CHANNELS][MAX_CHANNELS],
                    int nb_samples, int nb_channels, int *seq_tab)
{
    for (int ch = 0; ch < nb_channels; ch++) {
        float *dst = (float *)out->extended_data[ch];
        const float *src = (const float *)in->extended_data[0];
        float mul = rotate_mat[seq_tab[ch]][seq_tab[0]];

        s->fdsp->vector_fmul_scalar(dst, src, mul, FFALIGN(nb_samples, 16));

        for (int ch2 = 1; ch2 < nb_channels; ch2++) {
            const float *src = (const float *)in->extended_data[ch2];
            float mul = rotate_mat[seq_tab[ch]][seq_tab[ch2]];

            s->fdsp->vector_fmac_scalar(dst, src, mul, FFALIGN(nb_samples, 16));
        }
    }
}

static void rotated(AmbisonicContext *s,
                    AVFrame *in, AVFrame *out,
                    double rotate_mat[MAX_CHANNELS][MAX_CHANNELS],
                    int nb_samples, int nb_channels, int *seq_tab)
{
    for (int ch = 0; ch < nb_channels; ch++) {
        double *dst = (double *)out->extended_data[ch];
        const double *src = (const double *)in->extended_data[0];
        double mul = rotate_mat[seq_tab[ch]][seq_tab[0]];

        s->fdsp->vector_dmul_scalar(dst, src, mul, FFALIGN(nb_samples, 16));

        for (int ch2 = 1; ch2 < nb_channels; ch2++) {
            const double *src = (const double *)in->extended_data[ch2];
            double mul = rotate_mat[seq_tab[ch]][seq_tab[ch2]];

            s->fdsp->vector_dmac_scalar(dst, src, mul, FFALIGN(nb_samples, 16));
        }
    }
}

static void multiplyf(AmbisonicContext *s,
                      const double *decode_matrix,
                      int inputs, int outputs,
                      int *seq_tab, const double *gains_tab,
                      int nb_channels, int max_channels,
                      AVFrame *in, AVFrame *out)
{
    for (int ch = 0; ch < outputs; ch++) {
        float *dst = (float *)out->extended_data[ch];
        float gain = gains_tab ? gains_tab[ch] : 1.f;

        for (int ch2 = 0; ch2 < FFMIN3(nb_channels, max_channels, inputs); ch2++) {
            const int index = FFMIN(seq_tab[ch2], nb_channels - 1);
            const float *src = (const float *)in->extended_data[index];
            const float mul = decode_matrix[ch * inputs + ch2] * gain;

            s->fdsp->vector_fmac_scalar(dst, src, mul, FFALIGN(in->nb_samples, 16));
        }
    }
}

static void multiplyd(AmbisonicContext *s,
                      const double *decode_matrix,
                      int inputs, int outputs,
                      int *seq_tab, const double *gains_tab,
                      int nb_channels, int max_channels,
                      AVFrame *in, AVFrame *out)
{
    for (int ch = 0; ch < outputs; ch++) {
        double *dst = (double *)out->extended_data[ch];
        float gain = gains_tab ? gains_tab[ch] : 1.;

        for (int ch2 = 0; ch2 < FFMIN3(nb_channels, max_channels, inputs); ch2++) {
            const int index = FFMIN(seq_tab[ch2], nb_channels - 1);
            const double *src = (const double *)in->extended_data[index];
            const double mul = decode_matrix[ch * inputs + ch2] * gain;

            s->fdsp->vector_dmac_scalar(dst, src, mul, FFALIGN(in->nb_samples, 16));
        }
    }
}

static int query_formats(AVFilterContext *ctx)
{
    AmbisonicContext *s = ctx->priv;
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterChannelLayouts *inlayouts = NULL;
    uint64_t outlayout;
    int ret;

    outlayout = ambisonic_tab[s->layout].outlayout;

    ret = ff_add_format(&formats, s->precision == SINGLE ? AV_SAMPLE_FMT_FLTP: AV_SAMPLE_FMT_DBLP);
    if (ret)
        return ret;
    ret = ff_set_common_formats(ctx, formats);
    if (ret)
        return ret;

    ret = ff_add_channel_layout(&layouts, outlayout);
    if (ret)
        return ret;

    ret = ff_channel_layouts_ref(layouts, &ctx->outputs[0]->incfg.channel_layouts);
    if (ret)
        return ret;

    inlayouts = ff_all_channel_counts();
    if (!inlayouts)
        return AVERROR(ENOMEM);

    ret = ff_channel_layouts_ref(inlayouts, &ctx->inputs[0]->outcfg.channel_layouts);
    if (ret)
        return ret;

    formats = ff_all_samplerates();
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_samplerates(ctx, formats);
}

static void acn_to_level_order(int acn, int *level, int *order)
{
    *level = floor(sqrt(acn));
    *order = acn - *level * *level - *level;
}

static void calc_acn_sequence(AmbisonicContext *s)
{
    int *dst = s->seq_tab[M_ACN];

    for (int n = 0, i = 0; n <= s->order; n++) {
        for (int m = -n; m <= n; m++, i++)
            dst[i] = n * n + n + m;
    }
}

static void calc_fuma_sequence(AmbisonicContext *s)
{
    int *dst = s->seq_tab[M_FUMA];

    for (int n = 0, i = 0; n <= s->order; n++) {
        if (n < 2) {
            for (int m = -n; m <= n; m++)
                dst[i++] = n * n + 2 * (n - FFABS(m)) + (m < 0);
        } else {
            for (int m = -n; m <= n; m++)
                dst[i++] = SQR(n) + FFABS(m) * 2 - (m > 0);
        }
    }
}

static void calc_sid_sequence(AmbisonicContext *s)
{
    int *dst = s->seq_tab[M_SID];

    for (int n = 0, i = 0; n <= s->order; n++) {
        for (int m = -n; m <= n; m++, i++)
            dst[i] = n * n + 2 * (n - FFABS(m)) + (m < 0);
    }
}

static double factorial(int x)
{
    double prod = 1.;

    for (int i = 1; i <= x; i++)
        prod *= i;

    return prod;
}

static double n3d_norm(int i)
{
    int n, m;

    acn_to_level_order(i, &n, &m);

    return sqrt((2 * n + 1) * (2 - (m == 0)) * factorial(n - FFABS(m)) / factorial(n + FFABS(m)));
}

static double sn3d_norm(int i)
{
    int n, m;

    acn_to_level_order(i, &n, &m);

    return sqrt((2 - (m == 0)) * factorial(n - FFABS(m)) / factorial(n + FFABS(m)));
}

static void calc_sn3d_scaling(AmbisonicContext *s)
{
    double *dst = s->norm_tab[SN3D];

    for (int i = 0; i < s->max_channels; i++)
        dst[i] = 1.;
}

static void calc_n3d_scaling(AmbisonicContext *s)
{
    double *dst = s->norm_tab[N3D];

    for (int i = 0; i < s->max_channels; i++)
        dst[i] = n3d_norm(i) / sn3d_norm(i);
}

static void calc_fuma_scaling(AmbisonicContext *s)
{
    double *dst = s->norm_tab[FUMA];

    for (int i = 0; i < s->max_channels; i++) {
        dst[i] = sn3d_norm(i);

        switch (i) {
        case 0:
            dst[i] *= 1. / M_SQRT2;
        case 1:
        case 2:
        case 3:
        case 12:
        default:
            break;
        case 4:
            dst[i] *= 2. / sqrt(3.);
            break;
        case 5:
            dst[i] *= 2. / sqrt(3.);
            break;
        case 6:
            break;
        case 7:
            dst[i] *= 2. / sqrt(3.);
            break;
        case 8:
            dst[i] *= 2. / sqrt(3.);
            break;
        case 9:
            dst[i] *= sqrt(8. / 5.);
            break;
        case 10:
            dst[i] *= 3. / sqrt(5.);
            break;
        case 11:
            dst[i] *= sqrt(45. / 32.);
            break;
        case 13:
            dst[i] *= sqrt(45. / 32.);
            break;
        case 14:
            dst[i] *= 3. / sqrt(5.);
            break;
        case 15:
            dst[i] *= sqrt(8./5.);
            break;
        }
    }
}

static void multiply_mat(double out[3][3],
                         const double a[3][3],
                         const double b[3][3])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double sum = 0.;

            for (int k = 0; k < 3; k++)
                sum += a[i][k] * b[k][j];

            out[i][j] = sum;
        }
    }
}

static double P(int i, int l, int mu, int m_, double R_1[3][3],
                double R_lm1[2 * MAX_ORDER + 1][2 * MAX_ORDER + 1])
{
    double ret = 0.;
    double ri1  = R_1[i + 1][2];
    double rim1 = R_1[i + 1][0];
    double ri0  = R_1[i + 1][1];

    if (m_ == -l) {
        ret = ri1 * R_lm1[mu + l - 1][0] + rim1 * R_lm1[mu + l- 1][2 * l - 2];
    } else {
        if (m_ == l)
            ret = ri1 * R_lm1[mu + l - 1][2 * l - 2] - rim1 * R_lm1[mu + l - 1][0];
        else
            ret = ri0 * R_lm1[mu + l - 1][m_ + l - 1];
    }
    return ret;
}

static double U(int l, int m, int n, double R_1[3][3],
                double R_lm1[2 * MAX_ORDER + 1][2 * MAX_ORDER + 1])
{
    return P(0, l, m, n, R_1, R_lm1);
}

static double V(int l, int m, int n, double R_1[3][3],
                double R_lm1[2 * MAX_ORDER + 1][2 * MAX_ORDER + 1])
{
    double ret = 0.;

    if (m == 0) {
        double p0 = P( 1, l,  1, n, R_1, R_lm1);
        double p1 = P(-1, l, -1, n, R_1, R_lm1);
        ret = p0+p1;
    } else {
        if (m > 0) {
            int d = (m == 1) ? 1 : 0;
            double p0 = P( 1, l,  m - 1, n, R_1, R_lm1);
            double p1 = P(-1, l, -m + 1, n, R_1, R_lm1);

            ret = p0 * sqrt(1 + d) - p1 * (1 - d);
        } else {
            int d = (m == -1) ? 1 : 0;
            double p0 = P( 1, l,  m + 1, n, R_1, R_lm1);
            double p1 = P(-1, l, -m - 1, n, R_1, R_lm1);

            ret = p0 * (1 - d) + p1 * sqrt(1 + d);
        }
    }
    return ret;
}

static double W(int l, int m, int n, double R_1[3][3],
                double R_lm1[2 * MAX_ORDER + 1][2 * MAX_ORDER + 1])
{
    double ret = 0.;

    if (m != 0) {
        if (m > 0) {
            double p0 = P( 1, l, m + 1, n, R_1, R_lm1);
            double p1 = P(-1, l,-m - 1, n, R_1, R_lm1);

            ret = p0 + p1;
        } else {
            double p0 = P( 1, l,  m - 1, n, R_1, R_lm1);
            double p1 = P(-1, l, -m + 1, n, R_1, R_lm1);

            ret = p0 - p1;
        }
    }

    return ret;
}

static void calc_rotation_mat(AmbisonicContext *s,
                              double yaw, double pitch, double roll)
{
    double X[3][3] = {{0.}}, Y[3][3] = {{0.}}, Z[3][3] = {{0.}}, R[3][3], t[3][3];
    double R_lm1[2 * MAX_ORDER + 1][2 * MAX_ORDER + 1] = {{0.}};
    double R_1[3][3];

    X[0][0] = 1.;
    X[1][1] = X[2][2] = cosf(roll);
    X[1][2] = sinf(roll);
    X[2][1] = -X[1][2];

    Y[0][0] = Y[2][2] = cosf(pitch);
    Y[0][2] = sinf(pitch);
    Y[2][0] = -Y[0][2];
    Y[1][1] = 1.;

    Z[0][0] = Z[1][1] = cosf(yaw);
    Z[0][1] = sinf(yaw);
    Z[1][0] = -Z[0][1];
    Z[2][2] = 1.;

    multiply_mat(t, X, Y);
    multiply_mat(R, t, Z);

    R_1[0][0] = R[1][1];
    R_1[0][1] = R[1][2];
    R_1[0][2] = R[1][0];
    R_1[1][0] = R[2][1];
    R_1[1][1] = R[2][2];
    R_1[1][2] = R[2][0];
    R_1[2][0] = R[0][1];
    R_1[2][1] = R[0][2];
    R_1[2][2] = R[0][0];

    memset(s->rotate_mat, 0, sizeof(s->rotate_mat));

    s->rotate_mat[0][0] = 1.;
    s->rotate_mat[1][1] = R_1[0][0];
    s->rotate_mat[1][2] = R_1[0][1];
    s->rotate_mat[1][3] = R_1[0][2];
    s->rotate_mat[2][1] = R_1[1][0];
    s->rotate_mat[2][2] = R_1[1][1];
    s->rotate_mat[2][3] = R_1[1][2];
    s->rotate_mat[3][1] = R_1[2][0];
    s->rotate_mat[3][2] = R_1[2][1];
    s->rotate_mat[3][3] = R_1[2][2];

    R_lm1[0][0] = R_1[0][0];
    R_lm1[0][1] = R_1[0][1];
    R_lm1[0][2] = R_1[0][2];
    R_lm1[1][0] = R_1[1][0];
    R_lm1[1][1] = R_1[1][1];
    R_lm1[1][2] = R_1[1][2];
    R_lm1[2][0] = R_1[2][0];
    R_lm1[2][1] = R_1[2][1];
    R_lm1[2][2] = R_1[2][2];

    for (int l = 2; l <= s->order; l++) {
        double R_l[2 * MAX_ORDER + 1][2 * MAX_ORDER + 1] = {{0.}};

        for (int m = -l; m <= l; m++) {
            for (int n = -l; n <= l; n++) {
                int d = (m == 0) ? 1 : 0;
                double denom = FFABS(n) == l ? (2 * l) * (2 * l - 1) : l * l - n * n;
                double u = sqrt((l * l - m * m) / denom);
                double v = sqrt((1. + d) * (l + FFABS(m) - 1.) * (l + FFABS(m)) / denom) * (1. - 2. * d) * 0.5;
                double w = sqrt((l - FFABS(m) - 1.)*(l - FFABS(m)) / denom) * (1. - d) * -0.5;

                if (u)
                    u *= U(l, m, n, R_1, R_lm1);
                if (v)
                    v *= V(l, m, n, R_1, R_lm1);
                if (w)
                    w *= W(l, m, n, R_1, R_lm1);

                R_l[m + l][n + l] = u + v + w;
            }
        }

        for (int i = 0; i < 2 * l + 1; i++) {
            for (int j = 0; j < 2 * l + 1; j++)
                s->rotate_mat[l * l + i][l * l + j] = R_l[i][j];
        }

        memcpy(R_lm1, R_l, sizeof(R_l));
    }

    for (int i = 0; i < SQR(s->order + 1); i++) {
        for (int j = 0; j < SQR(s->order + 1); j++) {
            if (fabs(s->rotate_mat[i][j]) < 1e-6f)
                s->rotate_mat[i][j] = 0.;
            av_log(NULL, AV_LOG_DEBUG, "[%d][%d] = %g\n", i, j, s->rotate_mat[i][j]);
        }
    }
}

static void calc_mirror_mat(AmbisonicContext *s)
{
    for (int i = 0; i < s->max_channels; i++) {
        double gain = 1.;
        int level, order;

        acn_to_level_order(i, &level, &order);

        if (i == 0 || (!((level + order) & 1))) {
            gain *= s->gain[EVEN][D_Z];

            if (s->invert[D_Z] & 2)
                gain *= -1.;
        }

        if ((level + order) & 1) {
            gain *= s->gain[ODD][D_Z];

            if (s->invert[D_Z] & 1)
                gain *= -1.;
        }

        if (order >= 0) {
            gain *= s->gain[EVEN][D_Y];

            if (s->invert[D_Y] & 2)
                gain *= -1.;
        }

        if (order < 0) {
            gain *= s->gain[ODD][D_Y];

            if (s->invert[D_Y] & 1)
                gain *= -1.;
        }


        if (((order < 0) && (order & 1)) || ((order >= 0) && !(order & 1)) ) {
            gain *= s->gain[EVEN][D_X];

            if (s->invert[D_X] & 2)
                gain *= -1.;
        }

        if (((order < 0) && !(order & 1)) || ((order >= 0) && (order & 1))) {
            gain *= s->gain[ODD][D_X];

            if (s->invert[D_X] & 1)
                gain *= -1.;
        }

        if (level == order || level == -order) {
            gain *= s->gain[0][D_C];

            if (s->invert[D_C])
                gain *= -1.;
        }

        s->mirror_mat[i] = gain;
    }
}

static void near_field(AmbisonicContext *s, AVFrame *frame, int out, int add)
{
    for (int ch = 1; ch < frame->channels; ch++) {
        int n, m;

        acn_to_level_order(ch, &n, &m);

        if (!s->nf_process[n - 1])
            break;

        s->nf_process[n - 1](&s->nf[out][ch], frame, ch, add, 1.);
    }
}

static void xover_processf(Xover *xover, const float *src, float *dst, int nb_samples)
{
    float b0 = xover->b[0];
    float b1 = xover->b[1];
    float b2 = xover->b[2];
    float a1 = xover->a[1];
    float a2 = xover->a[2];
    float w0 = xover->w[0];
    float w1 = xover->w[1];

    for (int i = 0; i < nb_samples; i++) {
        float in = src[i];
        float out = b0 * in + w0;

        w0 = b1 * in + w1 + a1 * out;
        w1 = b2 * in + a2 * out;

        dst[i] = out;
    }

    xover->w[0] = w0;
    xover->w[1] = w1;
}

static void xover_processd(Xover *xover, const double *src, double *dst, int nb_samples)
{
    double b0 = xover->b[0];
    double b1 = xover->b[1];
    double b2 = xover->b[2];
    double a1 = xover->a[1];
    double a2 = xover->a[2];
    double w0 = xover->w[0];
    double w1 = xover->w[1];

    for (int i = 0; i < nb_samples; i++) {
        double in = src[i];
        double out = b0 * in + w0;

        w0 = b1 * in + w1 + a1 * out;
        w1 = b2 * in + a2 * out;

        dst[i] = out;
    }

    xover->w[0] = w0;
    xover->w[1] = w1;
}

static void xoverf(AmbisonicContext *s,
                   AVFrame *in, AVFrame *lf, AVFrame *hf)
{
    for (int ch = 0; ch < in->channels; ch++) {
        xover_processf(&s->xover[0][ch],
                       (const float *)in->extended_data[ch],
                       (float *)lf->extended_data[ch], in->nb_samples);

        xover_processf(&s->xover[1][ch],
                       (const float *)in->extended_data[ch],
                       (float *)hf->extended_data[ch], in->nb_samples);
    }
}

static void xoverd(AmbisonicContext *s,
                   AVFrame *in, AVFrame *lf, AVFrame *hf)
{
    for (int ch = 0; ch < in->channels; ch++) {
        xover_processd(&s->xover[0][ch],
                       (const double *)in->extended_data[ch],
                       (double *)lf->extended_data[ch], in->nb_samples);

        xover_processd(&s->xover[1][ch],
                       (const double *)in->extended_data[ch],
                       (double *)hf->extended_data[ch], in->nb_samples);
    }
}

static void process_float(AVFilterContext *ctx,
                          AVFrame *in, AVFrame *out)
{
    AmbisonicContext *s = ctx->priv;

    rotatef(s, in, s->frame, s->rotate_mat,
            in->nb_samples, FFMIN(in->channels, s->max_channels),
            s->seq_tab[s->seq]);

    mirrorf(s, s->frame, s->mirror_mat,
            in->nb_samples, FFMIN(in->channels, s->max_channels),
            s->seq_tab[s->seq]);

    if (s->near_field == NF_IN)
        near_field(s, s->frame, 0, 0);

    if (s->xover_freq >= 200.) {
        xoverf(s, s->frame, s->frame2, s->frame);

        multiplyf(s, ambisonic_tab[s->layout].mat,
                  ambisonic_tab[s->layout].inputs,
                  ambisonic_tab[s->layout].speakers,
                  s->seq_tab[s->seq],
                  s->gains_tab[0],
                  in->channels,
                  s->max_channels,
                  s->frame2, out);
    }

    multiplyf(s, ambisonic_tab[s->layout].mat,
              ambisonic_tab[s->layout].inputs,
              ambisonic_tab[s->layout].speakers,
              s->seq_tab[s->seq],
              s->xover_freq >= 200. ? s->gains_tab[1] : NULL,
              in->channels,
              s->max_channels,
              s->frame, out);

    if (s->near_field == NF_OUT)
        near_field(s, out, 1, 1);

    levelf(s, out, s->level_tab,
           out->nb_samples, out->channels);
}

static void process_double(AVFilterContext *ctx,
                           AVFrame *in, AVFrame *out)
{
    AmbisonicContext *s = ctx->priv;

    rotated(s, in, s->frame, s->rotate_mat,
            in->nb_samples, FFMIN(in->channels, s->max_channels),
            s->seq_tab[s->seq]);

    mirrord(s, s->frame, s->mirror_mat,
            in->nb_samples, FFMIN(in->channels, s->max_channels),
            s->seq_tab[s->seq]);

    if (s->near_field == NF_IN)
        near_field(s, s->frame, 0, 0);

    if (s->xover_freq >= 200.) {
        xoverd(s, s->frame, s->frame2, s->frame);

        multiplyd(s, ambisonic_tab[s->layout].mat,
                  ambisonic_tab[s->layout].inputs,
                  ambisonic_tab[s->layout].speakers,
                  s->seq_tab[s->seq],
                  s->gains_tab[0],
                  in->channels,
                  s->max_channels,
                  s->frame2, out);
    }

    multiplyd(s, ambisonic_tab[s->layout].mat,
              ambisonic_tab[s->layout].inputs,
              ambisonic_tab[s->layout].speakers,
              s->seq_tab[s->seq],
              s->xover_freq >= 200. ? s->gains_tab[1] : NULL,
              in->channels,
              s->max_channels,
              s->frame, out);

    if (s->near_field == NF_OUT)
        near_field(s, out, 1, 1);

    leveld(s, out, s->level_tab,
           out->nb_samples, out->channels);
}

static double speed_of_sound(double temp)
{
    return 1.85325 * (643.95 * sqrt(((temp + 273.15) / 273.15))) * 1000.0 / (60. * 60.);
}

static void nfield1_init(NearField *nf, double radius,
                         double speed, double rate,
                         double gain)
{
    double omega = speed / (radius * rate);
    double b1 = omega * 0.5;
    double g1 = 1.0 + b1;

    nf->d[0] = (2.0 * b1) / g1;
    nf->g = gain / g1;
}

static void nfield1_processf(NearField *nf, AVFrame *frame, int ch, int add,
                             double gain)
{
    float *dst = (float *)frame->extended_data[ch];
    float g, z0, d0;

    g = nf->g * gain;
    z0 = nf->z[0];
    d0 = nf->d[0];

    for (int n = 0; n < frame->nb_samples; n++) {
        float x = g * dst[n] - d0 * z0;
        z0 += x;
        dst[n] = x + (add ? dst[n] : 0.f);
    }

    nf->z[0] = z0;
}

static void nfield1_processd(NearField *nf, AVFrame *frame, int ch, int add,
                             double gain)
{
    double *dst = (double *)frame->extended_data[ch];
    double g, z0, d0;

    g = nf->g * gain;
    z0 = nf->z[0];
    d0 = nf->d[0];

    for (int n = 0; n < frame->nb_samples; n++) {
        double x = g * dst[n] - d0 * z0;
        z0 += x;
        dst[n] = x + (add ? dst[n] : 0.);
    }

    nf->z[0] = z0;
}

static void near_field_init(AmbisonicContext *s, int out,
                            double speed, double rate, double gain)
{
    for (int ch = 1; ch < s->max_channels; ch++) {
        int n, m;

        acn_to_level_order(ch, &n, &m);

        if (!s->nf_init[n - 1])
            break;

        s->nf_init[n - 1](&s->nf[out][ch], 1., speed, rate, gain);
    }
}

static void calc_level_tab(AmbisonicContext *s, int layout)
{
    double max_distance = 0.;

    for (int spkr = 0; spkr < ambisonic_tab[s->layout].speakers; spkr++) {
        double spkr_distance = ambisonic_tab[s->layout].speakers_distance[spkr];

        if (spkr_distance > max_distance)
            max_distance = spkr_distance;
    }

    for (int spkr = 0; spkr < ambisonic_tab[s->layout].speakers; spkr++)
        s->level_tab[spkr] = ambisonic_tab[s->layout].speakers_distance[spkr] / max_distance;
}

static void calc_gains_tab(AmbisonicContext *s, double xover_ratio)
{
    double xover_gain = pow(10., xover_ratio / 40.);

    for (int level = 0, ch = 0; level < s->order + 1; level++) {
        for (int i = 0; i < 1 + level * 2; i++, ch++) {
            double lf_gain = ambisonic_tab[s->layout].gains[0] ? ambisonic_tab[s->layout].gains[0][level] : 1.;
            double hf_gain = ambisonic_tab[s->layout].gains[1] ? ambisonic_tab[s->layout].gains[1][level] : 1.;

            s->gains_tab[0][ch] = lf_gain / xover_gain;
            s->gains_tab[1][ch] = hf_gain * xover_gain;
        }
    }
}

static void xover_init_input(Xover *xover, double freq, double rate, int hf)
{
    double k = tan(M_PI * freq / rate);
    double k2 = k * k;
    double d = k2 + 2. * k + 1.;

    if (hf) {
        xover->b[0] =  1. / d;
        xover->b[1] = -2. / d;
        xover->b[2] =  1. / d;
    } else {
        xover->b[0] = k2 / d;
        xover->b[1] = 2. * k2 / d;
        xover->b[2] = k2 / d;
    }

    xover->a[0] = 1.;
    xover->a[1] = -2 * (k2 - 1.) / d;
    xover->a[2] = -(k2 - 2 * k + 1.) / d;
}

static void xover_init(AmbisonicContext *s, double freq, double rate, int channels)
{
    for (int ch = 0; ch < channels; ch++) {
        xover_init_input(&s->xover[0][ch], freq, rate, 0);
        xover_init_input(&s->xover[1][ch], freq, rate, 1);
    }
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AmbisonicContext *s  = ctx->priv;

    s->order = ambisonic_tab[s->layout].order;
    s->max_channels = SQR(s->order + 1);

    if (s->near_field == NF_AUTO)
        s->near_field = ambisonic_tab[s->layout].near_field;
    if (s->xover_freq < 0)
        s->xover_freq = ambisonic_tab[s->layout].xover;

    calc_sn3d_scaling(s);
    calc_n3d_scaling(s);
    calc_fuma_scaling(s);

    calc_acn_sequence(s);
    calc_fuma_sequence(s);
    calc_sid_sequence(s);

    near_field_init(s, 0, speed_of_sound(s->temp), outlink->sample_rate, 1.);
    near_field_init(s, 1, speed_of_sound(s->temp), outlink->sample_rate, 1.);

    s->yaw   = (M_PI / 180.) * s->yaw;
    s->pitch = (M_PI / 180.) * s->pitch;
    s->roll  = (M_PI / 180.) * s->roll;

    calc_rotation_mat(s, s->yaw, s->pitch, s->roll);
    calc_mirror_mat(s);
    calc_level_tab(s, s->layout);
    calc_gains_tab(s, s->xover_ratio);
    xover_init(s, s->xover_freq, outlink->sample_rate, s->max_channels);

    switch (s->precision) {
    case SINGLE:
        s->nf_process[0] = nfield1_processf;
        s->process = process_float;
        break;
    case DOUBLE:
        s->nf_process[0] = nfield1_processd;
        s->process = process_double;
        break;
    default: av_assert0(0);
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AmbisonicContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;

    if (!s->frame || s->frame->nb_samples < in->nb_samples) {
        av_frame_free(&s->frame);
        av_frame_free(&s->frame2);
        s->frame  = ff_get_audio_buffer(inlink, in->nb_samples);
        s->frame2 = ff_get_audio_buffer(inlink, in->nb_samples);
        if (!s->frame || !s->frame2) {
            av_frame_free(&s->frame);
            av_frame_free(&s->frame2);
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
    }

    out = ff_get_audio_buffer(outlink, in->nb_samples);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    s->process(ctx, in, out);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

}

static av_cold int init(AVFilterContext *ctx)
{
    AmbisonicContext *s = ctx->priv;

    s->nf_init[0] = nfield1_init;

    s->fdsp = avpriv_float_dsp_alloc(0);
    if (!s->fdsp)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AmbisonicContext *s = ctx->priv;

    av_freep(&s->fdsp);
    av_frame_free(&s->frame);
    av_frame_free(&s->frame2);
}

static const AVFilterPad inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_AUDIO,
        .filter_frame   = filter_frame,
    },
    { NULL }
};
static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_output,
    },
    { NULL }
};

AVFILTER_DEFINE_CLASS(ambisonic);

AVFilter ff_af_ambisonic = {
    .name           = "ambisonic",
    .description    = NULL_IF_CONFIG_SMALL("Ambisonic decoder"),
    .query_formats  = query_formats,
    .priv_size      = sizeof(AmbisonicContext),
    .priv_class     = &ambisonic_class,
    .init           = init,
    .uninit         = uninit,
    .inputs         = inputs,
    .outputs        = outputs,
};
