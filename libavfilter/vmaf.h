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

/**
 * @file
 * Calculate the VMAF between two input videos.
 */

/** Normalization type */
const char *norm_type = "linear_rescale";

/** cliping to be applied on vmaf score */
const double score_clip[2] = {
    0.0,
    100.0
};

/** feature vector */
const char *feature_names[6] = {
    "VMAF_feature_adm2_score",
    "VMAF_feature_motion2_score",
    "VMAF_feature_vif_scale0_score",
    "VMAF_feature_vif_scale1_score",
    "VMAF_feature_vif_scale2_score",
    "VMAF_feature_vif_scale3_score"
};

const double intercepts[7] = {
    -0.3092981927591963,
    -1.7993968597186747,
    -0.003017198086831897,
    -0.1728125095425364,
    -0.5294309090081222,
    -0.7577185792093722,
    -1.083428597549764
};

const double slopes[7] = {
    0.012020766332648465,
    2.8098077502505414,
    0.06264407466686016,
    1.222763456258933,
    1.5360318811084146,
    1.7620864995501058,
    2.08656468286432
};

/** transform constants */
const double score_transform[3] = {
    1.70674692,
    1.72643844,
    -0.00705305
};

typedef struct {
    int index;
    double value;
} svm_node;

typedef struct {
    int l;
    double *y;
    svm_node **x;
} svm_problem;

typedef struct {
    int svm_type;
    int kernel_type;
    int degree;    /** for poly */
    double gamma;    /** for poly/rbf/sigmoid */
    double coef0;    /** for poly/sigmoid */

    /** these are for training only */
    double cache_size; /** in MB */
    double eps;    /** stopping criteria */
    double C;    /** for C_SVC, EPSILON_SVR and NU_SVR */
    int nr_weight;        /** for C_SVC */
    int *weight_label;    /** for C_SVC */
    double* weight;        /** for C_SVC */
    double nu;    /** for NU_SVC, ONE_CLASS, and NU_SVR */
    double p;    /** for EPSILON_SVR */
    int shrinking;    /** use the shrinking heuristics */
    int probability; /** do probability estimates */
} svm_parameter;

/**
 * svm_model
 */
typedef struct {
    svm_parameter param;    /** parameter */
    int nr_class;        /** number of classes, = 2 in regression/one class svm */
    int l;            /** total #SV */
    svm_node **SV;        /** SVs (SV[l]) */
    double **sv_coef;    /** coefficients for SVs in decision functions (sv_coef[k-1][l]) */
    double *rho;        /** constants in decision functions (rho[k*(k-1)/2]) */
    double *probA;        /** pariwise probability information */
    double *probB;
    int *sv_indices;        /** sv_indices[0,...,nSV-1] are values in [1,...,num_traning_data] to indicate SVs in the training set */

    /** for classification only */

    int *label;        /** label of each class (label[k]) */
    int *nSV;        /** number of SVs for each class (nSV[k]) */
    /** nSV[0] + nSV[1] + ... + nSV[k-1] = l */
    int free_sv;        /** 1 if svm_model is created by svm_load_model*/
    /** 0 if svm_model is created by svm_train */
} svm_model;


typedef struct {
    const svm_node **x;
    double *x_square;

    // svm_parameter
    const int kernel_type;
    const int degree;
    const double gamma;
    const double coef0;
} Kernel;

