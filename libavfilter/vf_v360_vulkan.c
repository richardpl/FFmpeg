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

#include "libavutil/random_seed.h"
#include "libavutil/opt.h"
#include "vulkan.h"
#include "internal.h"

enum RotationOrder {
    YAW,
    PITCH,
    ROLL,
    NB_RORDERS,
};

enum Projections {
    EQUIRECTANGULAR,
    CUBEMAP_3_2,
    CUBEMAP_6_1,
    EQUIANGULAR,
    FLAT,
    DUAL_FISHEYE,
    BARREL,
    CUBEMAP_1_6,
    STEREOGRAPHIC,
    MERCATOR,
    BALL,
    HAMMER,
    SINUSOIDAL,
    FISHEYE,
    PANNINI,
    CYLINDRICAL,
    PERSPECTIVE,
    TETRAHEDRON,
    BARREL_SPLIT,
    TSPYRAMID,
    HEQUIRECTANGULAR,
    NB_PROJECTIONS,
};

#define CGROUPS (int [3]){ 32, 32, 1 }

typedef struct V360VulkanContext {
    VulkanFilterContext vkctx;

    int initialized;
    FFVkExecContext *exec;
    VulkanPipeline *pl;

    /* Shader updators, must be in the main filter struct */
    VkDescriptorImageInfo input_images[3];
    VkDescriptorImageInfo output_images[3];

    int   planewidth[4], planeheight[4];
    int   inplanewidth[4], inplaneheight[4];
    int   in, out;
    int   width, height;
    float h_fov, v_fov;
    float ih_fov, iv_fov;
    float yaw, pitch, roll;
    char *rorder;
    int rotation_order[3];

    /* Push constants / options */
    struct {
        float flat_range[2];
        float iflat_range[2];
        float rot_mat[4][4];
    } opts;
} V360VulkanContext;

static const char flat_to_xyz[] = {
    C(0, void out_transform(out vec3 v, in ivec2 out_size, in ivec2 pos)       )
    C(0, {                                                                     )
    C(1,     vec2 fpos = vec2(pos) + vec2(0.5f, 0.5f);                         )
    C(1,     vec2 p = ((fpos / vec2(out_size)) - 0.5f)*2.0f;                   )
    C(1,     v = vec3(p[0], p[1], 1.f) * vec3(flat_range, 1.f);                )
    C(1,     v = normalize(v);                                                 )
    C(0, }                                                                     )
};

static const char xyz_to_flat[] = {
    C(0, void in_transform(int idx, in vec3 v, in ivec2 pos, in ivec2 in_size) )
    C(0, {                                                                     )
    C(1,     const float r = tan(acos(v[2]));                                  )
    C(1,     const float rr = abs(r) < 1e+6f ? r : length(in_size);            )
    C(1,     const float h = length(vec2(v[0], v[1]));                         )
    C(1,     const float c = h <= 1e-6f ? 1.f : rr / h;                        )
    C(1,     vec2 p = vec2(v[0], v[1]) / iflat_range * c;                      )
    C(1,     p = IS_WITHIN(abs(p), vec2(1.f)) ? (p/2.0f)+0.5f:vec2(0.f);       )
    C(1,     p = v[2] >= 0.f ? p : vec2(0.f);                                  )
    C(1,     vec4 res = texture(input_img[idx], p);                            )
    C(1,     imageStore(output_img[idx], pos, res);                            )
    C(0, }                                                                     )
};

static const char equirect_to_xyz[] = {
    C(0, void out_transform(out vec3 v, in ivec2 out_size, in ivec2 pos)       )
    C(0, {                                                                     )
    C(1,     vec2 fpos = 2.f * vec2(pos) + 0.5f;                               )
    C(1,     vec2 p = fpos / vec2(out_size) - 1.f;                             )
    C(1,     p = vec2(p[0] * PI, p[1] * PI_2);                                 )
    C(1,     v = vec3(cos(p[1]) * sin(p[0]), sin(p[1]), cos(p[1])*cos(p[0]));  )
    C(0, }                                                                     )
};

static const char xyz_to_equirect[] = {
    C(0, void in_transform(int idx, in vec3 v, in ivec2 pos, in ivec2 in_size) )
    C(0, {                                                                     )
    C(1,     vec2 p = vec2(atan(v[0], v[2]) / PI, asin(v[1]) / PI_2);          )
    C(1,     vec4 res = texture(input_img[idx], (p/2.0f) + 0.5f);              )
    C(1,     imageStore(output_img[idx], pos, res);                            )
    C(0, }                                                                     )
};

static const char stereographic_to_xyz[] = {
    C(0, void out_transform(out vec3 v, in ivec2 out_size, in ivec2 pos)       )
    C(0, {                                                                     )
    C(1,     vec2 fpos = vec2(pos) + vec2(0.5f, 0.5f);                         )
    C(1,     vec2 p = (fpos / vec2(out_size) - 0.5f) * 2.0f * flat_range;      )
    C(1,     const float r = length(p);                                        )
    C(1,     const float theta = atan(r) * 2.0f;                               )
    C(1,     v = vec3(p[0] / r*sin(theta), p[1] / r*sin(theta), cos(theta));   )
    C(1,     v = normalize(v);                                                 )
    C(0, }                                                                     )
};

static const char xyz_to_stereographic[] = {
    C(0, void in_transform(int idx, in vec3 v, in ivec2 pos, in ivec2 in_size) )
    C(0, {                                                                     )
    C(1,     const float theta = acos(v[2]);                                   )
    C(1,     const float r = tan(theta * 0.5f);                                )
    C(1,     const vec2 c = (r / length(vec2(v[0], v[1]))) / iflat_range;      )
    C(1,     vec2 p = vec2(v[0], v[1]) * c;                                    )
    C(1,     p = IS_WITHIN(abs(p), vec2(1.f)) ? (p/2.0f)+0.5f:vec2(0.f);       )
    C(1,     vec4 res = texture(input_img[idx], p);                            )
    C(1,     imageStore(output_img[idx], pos, res);                            )
    C(0, }                                                                     )
};

static const char fisheye_to_xyz[] = {
    C(0, void out_transform(out vec3 v, in ivec2 out_size, in ivec2 pos)       )
    C(0, {                                                                     )
    C(1,     vec2 fpos = vec2(pos) + vec2(0.5f, 0.5f);                         )
    C(1,     vec2 p = (fpos / vec2(out_size) - 0.5f) * 2.0f * flat_range;      )
    C(1,     const float r = length(p);                                        )
    C(1,     const float phi = atan(p[1], p[0]);                               )
    C(1,     const float theta = (1.f - r) * PI_2;                             )
    C(1,     v = vec3(cos(theta)*cos(phi), cos(theta)*sin(phi), sin(theta));   )
    C(1,     v = normalize(v);                                                 )
    C(0, }                                                                     )
};

static const char xyz_to_fisheye[] = {
    C(0, void in_transform(int idx, in vec3 v, in ivec2 pos, in ivec2 in_size) )
    C(0, {                                                                     )
    C(1,     const float h = length(vec2(v[0], v[1]));                         )
    C(1,     const float lh = h > 0.f ? h / 2.f : 1.f;                         )
    C(1,     const float phi = atan(h, v[2]) / PI;                             )
    C(1,     vec2 p = vec2(v[0], v[1]) * phi / lh / iflat_range;               )
    C(1,     p = (length(p) <= 1.f) ? (p/2.0f)+0.5f:vec2(0.f);                 )
    C(1,     vec4 res = texture(input_img[idx], p);                            )
    C(1,     imageStore(output_img[idx], pos, res);                            )
    C(0, }                                                                     )
};

static const char dfisheye_to_xyz[] = {
    C(0, void out_transform(out vec3 v, in ivec2 out_size, in ivec2 pos)       )
    C(0, {                                                                     )
    C(1,     const float m = pos[0] >= out_size[0] / 2 ? 1.f : -1.f;           )
    C(1,     vec2 npos = m == 1.f ? vec2(out_size[0] / 2, 0.f) : vec2(0.f);    )
    C(1,     vec2 fpos = vec2(pos) - npos + vec2(0.5f, 0.5f);                  )
    C(1,     vec2 osize = vec2(out_size) * vec2(0.5f, 1.f);                    )
    C(1,     vec2 p = (fpos / osize - 0.5f) * 2.0f * flat_range;               )
    C(1,     const float h = length(p);                                        )
    C(1,     const float lh = h > 0.f ? h : 1.f;                               )
    C(1,     const float theta = m * PI_2 * (1.f - h);                         )
    C(1,     p = p / lh;                                                       )
    C(1,     v = vec3(cos(theta)*m*p[0], cos(theta)*p[1], sin(theta));         )
    C(1,     v = normalize(v);                                                 )
    C(0, }                                                                     )
};

static const char xyz_to_dfisheye[] = {
    C(0, void in_transform(int idx, in vec3 v, in ivec2 pos, in ivec2 in_size) )
    C(0, {                                                                     )
    C(1,     const float h = length(vec2(v[0], v[1]));                         )
    C(1,     const float lh = h > 0.f ? h : 1.f;                               )
    C(1,     const float theta = acos(abs(v[2])) / PI;                         )
    C(1,     vec2 p = (vec2(v[0], v[1]) * theta)/lh/iflat_range + 0.5f;        )
    C(1,     p = p * vec2(0.5f, 1.f);                                          )
    C(1,     p = v[2] >= 0.f ? vec2(p[0]+0.5f, p[1]) : vec2(0.5f-p[0], p[1]);  )
    C(1,     vec4 res = texture(input_img[idx], p);                            )
    C(1,     imageStore(output_img[idx], pos, res);                            )
    C(0, }                                                                     )
};

static void multiply_matrix(float c[4][4], const float a[4][4], const float b[4][4])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float sum = 0.f;

            for (int k = 0; k < 3; k++)
                sum += a[i][k] * b[k][j];

            c[i][j] = sum;
        }
    }
}

static inline void calculate_rotation_matrix(float yaw, float pitch, float roll,
                                             float rot_mat[4][4],
                                             const int rotation_order[3])
{
    const float yaw_rad   = yaw   * M_PI / 180.f;
    const float pitch_rad = pitch * M_PI / 180.f;
    const float roll_rad  = roll  * M_PI / 180.f;

    const float sin_yaw   = sinf(yaw_rad);
    const float cos_yaw   = cosf(yaw_rad);
    const float sin_pitch = sinf(pitch_rad);
    const float cos_pitch = cosf(pitch_rad);
    const float sin_roll  = sinf(roll_rad);
    const float cos_roll  = cosf(roll_rad);

    float m[3][4][4];
    float temp[4][4];

    m[0][0][0] =  cos_yaw;  m[0][0][1] = 0;          m[0][0][2] =  sin_yaw;
    m[0][1][0] =  0;        m[0][1][1] = 1;          m[0][1][2] =  0;
    m[0][2][0] = -sin_yaw;  m[0][2][1] = 0;          m[0][2][2] =  cos_yaw;

    m[1][0][0] = 1;         m[1][0][1] = 0;          m[1][0][2] =  0;
    m[1][1][0] = 0;         m[1][1][1] = cos_pitch;  m[1][1][2] = -sin_pitch;
    m[1][2][0] = 0;         m[1][2][1] = sin_pitch;  m[1][2][2] =  cos_pitch;

    m[2][0][0] = cos_roll;  m[2][0][1] = -sin_roll;  m[2][0][2] =  0;
    m[2][1][0] = sin_roll;  m[2][1][1] =  cos_roll;  m[2][1][2] =  0;
    m[2][2][0] = 0;         m[2][2][1] =  0;         m[2][2][2] =  1;

    multiply_matrix(temp, m[rotation_order[0]], m[rotation_order[1]]);
    multiply_matrix(rot_mat, temp, m[rotation_order[2]]);
}

static void set_dimensions(int *outw, int *outh, int w, int h, const AVPixFmtDescriptor *desc)
{
    outw[1] = outw[2] = FF_CEIL_RSHIFT(w, desc->log2_chroma_w);
    outw[0] = outw[3] = w;
    outh[1] = outh[2] = FF_CEIL_RSHIFT(h, desc->log2_chroma_h);
    outh[0] = outh[3] = h;
}

static av_cold int init_filter(AVFilterContext *ctx, AVFrame *in)
{
    AVFilterLink *outlink = ctx->outputs[0];
    V360VulkanContext *s = ctx->priv;
    int err;

    /* Create a sampler */
    VkSampler *sampler = ff_vk_init_sampler(ctx, 0, VK_FILTER_LINEAR);
    if (!sampler)
        return AVERROR_EXTERNAL;

    s->vkctx.queue_family_idx = s->vkctx.hwctx->queue_family_comp_index;
    s->vkctx.queue_count = GET_QUEUE_COUNT(s->vkctx.hwctx, 0, 1, 0);
    s->vkctx.cur_queue_idx = av_get_random_seed() % s->vkctx.queue_count;

    s->pl = ff_vk_create_pipeline(ctx);
    if (!s->pl)
        return AVERROR(ENOMEM);

    switch (s->out) {
    case FLAT:
        s->opts.flat_range[0] = tanf(0.5f * s->h_fov * M_PI / 180.f);
        s->opts.flat_range[1] = tanf(0.5f * s->v_fov * M_PI / 180.f);
        break;
    case STEREOGRAPHIC:
        s->opts.flat_range[0] = tanf(FFMIN(s->h_fov, 359.f) * M_PI / 720.f);
        s->opts.flat_range[1] = tanf(FFMIN(s->v_fov, 359.f) * M_PI / 720.f);
        break;
    case DUAL_FISHEYE:
    case FISHEYE:
        s->opts.flat_range[0] = s->h_fov / 180.f;
        s->opts.flat_range[1] = s->v_fov / 180.f;
        break;
    }

    switch (s->in) {
    case FLAT:
        s->opts.iflat_range[0] = tanf(0.5f * s->ih_fov * M_PI / 180.f);
        s->opts.iflat_range[1] = tanf(0.5f * s->iv_fov * M_PI / 180.f);
        break;
    case STEREOGRAPHIC:
        s->opts.iflat_range[0] = tanf(FFMIN(s->ih_fov, 359.f) * M_PI / 720.f);
        s->opts.iflat_range[1] = tanf(FFMIN(s->iv_fov, 359.f) * M_PI / 720.f);
        break;
    case DUAL_FISHEYE:
    case FISHEYE:
        s->opts.iflat_range[0] = s->ih_fov / 180.f;
        s->opts.iflat_range[1] = s->iv_fov / 180.f;
        break;
    }

    s->rotation_order[0] = YAW;
    s->rotation_order[1] = PITCH;
    s->rotation_order[2] = ROLL;

    { /* Create the shader */
        const int planes = av_pix_fmt_count_planes(s->vkctx.output_format);
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->vkctx.output_format);

        set_dimensions(s->inplanewidth, s->inplaneheight, in->width, in->height, desc);
        set_dimensions(s->planewidth, s->planeheight, outlink->w, outlink->h, desc);
        calculate_rotation_matrix(s->yaw, s->pitch, s->roll, s->opts.rot_mat, s->rotation_order);

        VulkanDescriptorSetBinding desc_i[2] = {
            {
                .name       = "input_img",
                .type       = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .dimensions = 2,
                .elems      = planes,
                .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
                .updater    = s->input_images,
                .samplers   = DUP_SAMPLER_ARRAY4(*sampler),
            },
            {
                .name       = "output_img",
                .type       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .mem_layout = ff_vk_shader_rep_fmt(s->vkctx.output_format),
                .mem_quali  = "writeonly",
                .dimensions = 2,
                .elems      = planes,
                .stages     = VK_SHADER_STAGE_COMPUTE_BIT,
                .updater    = s->output_images,
            },
        };

        SPIRVShader *shd = ff_vk_init_shader(ctx, s->pl, "v360_compute",
                                             VK_SHADER_STAGE_COMPUTE_BIT);
        if (!shd)
            return AVERROR(ENOMEM);

        ff_vk_set_compute_shader_sizes(ctx, shd, CGROUPS);

        GLSLC(0, layout(push_constant, std430) uniform pushConstants {        );
        GLSLC(1,    vec2 flat_range;                                          );
        GLSLC(1,    vec2 iflat_range;                                         );
        GLSLC(1,    mat4 rot_mat;                                             );
        GLSLC(0, };                                                           );
        GLSLC(0,                                                              );

        ff_vk_add_push_constant(ctx, s->pl, 0, sizeof(s->opts),
                                VK_SHADER_STAGE_COMPUTE_BIT);

        RET(ff_vk_add_descriptor_set(ctx, s->pl, shd, desc_i, 2, 0)); /* set 0 */

        GLSLF(0, #define PI (%f)                                         ,M_PI);
        GLSLF(0, #define PI_2 (%f)                                     ,M_PI_2);
        GLSLF(0, #define SQRT2 (%f)                                   ,M_SQRT2);
        GLSLF(0, #define inplanewidth ivec4(%i, %i, %i, %i), s->inplanewidth[0],
                                                             s->inplanewidth[1],
                                                             s->inplanewidth[2],
                                                             s->inplanewidth[3]);
        GLSLF(0, #define inplaneheight ivec4(%i, %i, %i, %i), s->inplaneheight[0],
                                                              s->inplaneheight[1],
                                                              s->inplaneheight[2],
                                                              s->inplaneheight[3]);

        switch (s->out) {
        case FLAT:
            GLSLD(flat_to_xyz);
            break;
        case EQUIRECTANGULAR:
            GLSLD(equirect_to_xyz);
            break;
        case STEREOGRAPHIC:
            GLSLD(stereographic_to_xyz);
            break;
        case FISHEYE:
            GLSLD(fisheye_to_xyz);
            break;
        case DUAL_FISHEYE:
            GLSLD(dfisheye_to_xyz);
            break;
        }

        switch (s->in) {
        case FLAT:
            GLSLD(xyz_to_flat);
            break;
        case EQUIRECTANGULAR:
            GLSLD(xyz_to_equirect);
            break;
        case STEREOGRAPHIC:
            GLSLD(xyz_to_stereographic);
            break;
        case FISHEYE:
            GLSLD(xyz_to_fisheye);
            break;
        case DUAL_FISHEYE:
            GLSLD(xyz_to_dfisheye);
            break;
        }

        GLSLC(0, void main()                                                  );
        GLSLC(0, {                                                            );
        GLSLC(1,  ivec2 pos = ivec2(gl_GlobalInvocationID.xy);                );
        GLSLC(1,  vec3 vector;                                                );
        GLSLF(1,  int planes = %i;                                     ,planes);
        GLSLC(1,  for (int i = 0; i < planes; i++) {                          );
        GLSLC(2,      ivec2 out_size = imageSize(output_img[i]);              );
        GLSLC(2,      ivec2 in_size = ivec2(inplanewidth[i],inplaneheight[i]););
        GLSLC(2,      out_transform(vector, out_size, pos);                   );
        GLSLC(2,      vector = normalize((rot_mat * vec4(vector, 1.f)).xyz);  );
        GLSLC(2,      in_transform(i, vector, pos, in_size);                  );
        GLSLC(1, }                                                            );
        GLSLC(0, }                                                            );

        RET(ff_vk_compile_shader(ctx, shd, "main"));
    }

    RET(ff_vk_init_pipeline_layout(ctx, s->pl));
    RET(ff_vk_init_compute_pipeline(ctx, s->pl));

    /* Execution context */
    RET(ff_vk_create_exec_ctx(ctx, &s->exec));

    s->initialized = 1;

    return 0;

fail:
    return err;
}

static int process_frames(AVFilterContext *avctx, AVFrame *out_f, AVFrame *in_f)
{
    int err = 0;
    VkCommandBuffer cmd_buf;
    V360VulkanContext *s = avctx->priv;
    AVVkFrame *in = (AVVkFrame *)in_f->data[0];
    AVVkFrame *out = (AVVkFrame *)out_f->data[0];
    int planes = av_pix_fmt_count_planes(s->vkctx.output_format);

    /* Update descriptors and init the exec context */
    ff_vk_start_exec_recording(avctx, s->exec);
    cmd_buf = ff_vk_get_exec_buf(avctx, s->exec);

    for (int i = 0; i < planes; i++) {
        RET(ff_vk_create_imageview(avctx, s->exec, &s->input_images[i].imageView, in->img[i],
                                   av_vkfmt_from_pixfmt(s->vkctx.input_format)[i],
                                   ff_comp_identity_map));

        RET(ff_vk_create_imageview(avctx, s->exec, &s->output_images[i].imageView, out->img[i],
                                   av_vkfmt_from_pixfmt(s->vkctx.output_format)[i],
                                   ff_comp_identity_map));

        s->input_images[i].imageLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        s->output_images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    ff_vk_update_descriptor_set(avctx, s->pl, 0);

    for (int i = 0; i < planes; i++) {
        VkImageMemoryBarrier bar[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout = in->layout[i],
                .newLayout = s->input_images[i].imageLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = in->img[i],
                .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .subresourceRange.levelCount = 1,
                .subresourceRange.layerCount = 1,
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = out->layout[i],
                .newLayout = s->output_images[i].imageLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = out->img[i],
                .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .subresourceRange.levelCount = 1,
                .subresourceRange.layerCount = 1,
            },
        };

        vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, NULL, 0, NULL, FF_ARRAY_ELEMS(bar), bar);

        in->layout[i]  = bar[0].newLayout;
        in->access[i]  = bar[0].dstAccessMask;

        out->layout[i] = bar[1].newLayout;
        out->access[i] = bar[1].dstAccessMask;
    }

    ff_vk_bind_pipeline_exec(avctx, s->exec, s->pl);

    ff_vk_update_push_exec(avctx, s->exec, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(s->opts), &s->opts);

    vkCmdDispatch(cmd_buf,
                  FFALIGN(s->vkctx.output_width,  CGROUPS[0])/CGROUPS[0],
                  FFALIGN(s->vkctx.output_height, CGROUPS[1])/CGROUPS[1], 1);

    ff_vk_add_exec_dep(avctx, s->exec, in_f, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    ff_vk_add_exec_dep(avctx, s->exec, out_f, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    err = ff_vk_submit_exec_queue(avctx, s->exec);
    if (err)
        return err;

fail:
    ff_vk_discard_exec_deps(avctx, s->exec);
    return err;
}

static int v360_vulkan_filter_frame(AVFilterLink *link, AVFrame *in)
{
    int err;
    AVFilterContext *ctx = link->dst;
    V360VulkanContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    if (!s->initialized)
        RET(init_filter(ctx, in));

    RET(process_frames(ctx, out, in));

    err = av_frame_copy_props(out, in);
    if (err < 0)
        goto fail;

    av_frame_free(&in);

    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return err;
}

static int v360_vulkan_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    V360VulkanContext *s  = avctx->priv;
    AVFilterLink *inlink   = avctx->inputs[0];
    int err;

    if (s->width > 0 && s->height > 0) {
        s->vkctx.output_width  = s->width;
        s->vkctx.output_height = s->height;
    }

    s->vkctx.output_format = s->vkctx.input_format;

    err = ff_vk_filter_config_output(outlink);
    if (err < 0)
        return err;

    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    return 0;
}

static void v360_vulkan_uninit(AVFilterContext *avctx)
{
    V360VulkanContext *s = avctx->priv;

    ff_vk_filter_uninit(avctx);

    s->initialized = 0;
}

#define OFFSET(x) offsetof(V360VulkanContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption v360_vulkan_options[] = {
    {     "input", "set input projection",              OFFSET(in), AV_OPT_TYPE_INT,    {.i64=EQUIRECTANGULAR}, 0,    NB_PROJECTIONS-1, FLAGS, "in" },
    {         "e", "equirectangular",                            0, AV_OPT_TYPE_CONST,  {.i64=EQUIRECTANGULAR}, 0,                   0, FLAGS, "in" },
    {  "equirect", "equirectangular",                            0, AV_OPT_TYPE_CONST,  {.i64=EQUIRECTANGULAR}, 0,                   0, FLAGS, "in" },
    {      "flat", "regular video",                              0, AV_OPT_TYPE_CONST,  {.i64=FLAT},            0,                   0, FLAGS, "in" },
    {  "dfisheye", "dual fisheye",                               0, AV_OPT_TYPE_CONST,  {.i64=DUAL_FISHEYE},    0,                   0, FLAGS, "in" },
    {        "sg", "stereographic",                              0, AV_OPT_TYPE_CONST,  {.i64=STEREOGRAPHIC},   0,                   0, FLAGS, "in" },
    {   "fisheye", "fisheye",                                    0, AV_OPT_TYPE_CONST,  {.i64=FISHEYE}      ,   0,                   0, FLAGS, "in" },
    {    "output", "set output projection",            OFFSET(out), AV_OPT_TYPE_INT,    {.i64=FLAT},            0,    NB_PROJECTIONS-1, FLAGS, "out" },
    {         "e", "equirectangular",                            0, AV_OPT_TYPE_CONST,  {.i64=EQUIRECTANGULAR}, 0,                   0, FLAGS, "out" },
    {  "equirect", "equirectangular",                            0, AV_OPT_TYPE_CONST,  {.i64=EQUIRECTANGULAR}, 0,                   0, FLAGS, "out" },
    {      "flat", "regular video",                              0, AV_OPT_TYPE_CONST,  {.i64=FLAT},            0,                   0, FLAGS, "out" },
    {  "dfisheye", "dual fisheye",                               0, AV_OPT_TYPE_CONST,  {.i64=DUAL_FISHEYE},    0,                   0, FLAGS, "out" },
    {        "sg", "stereographic",                              0, AV_OPT_TYPE_CONST,  {.i64=STEREOGRAPHIC},   0,                   0, FLAGS, "out" },
    {   "fisheye", "fisheye",                                    0, AV_OPT_TYPE_CONST,  {.i64=FISHEYE}      ,   0,                   0, FLAGS, "out" },
    { "w", "output width",  OFFSET(width),  AV_OPT_TYPE_INT,    {.i64=0},  0, INT16_MAX, FLAGS, "w"},
    { "h", "output height", OFFSET(height), AV_OPT_TYPE_INT,    {.i64=0},  0, INT16_MAX, FLAGS, "h"},
    {       "yaw", "yaw rotation",                     OFFSET(yaw), AV_OPT_TYPE_FLOAT,  {.dbl=0.f},        -180.f,               180.f, FLAGS, "yaw"},
    {     "pitch", "pitch rotation",                 OFFSET(pitch), AV_OPT_TYPE_FLOAT,  {.dbl=0.f},        -180.f,               180.f, FLAGS, "pitch"},
    {      "roll", "roll rotation",                   OFFSET(roll), AV_OPT_TYPE_FLOAT,  {.dbl=0.f},        -180.f,               180.f, FLAGS, "roll"},
    {    "rorder", "rotation order",                OFFSET(rorder), AV_OPT_TYPE_STRING, {.str="ypr"},           0,                   0, FLAGS, "rorder"},
    { "h_fov", "set output horizontal FOV angle", OFFSET(h_fov), AV_OPT_TYPE_FLOAT, {.dbl = 90.0f}, 0.00001f, 360.0f, .flags = FLAGS },
    { "v_fov", "set output vertical FOV angle",   OFFSET(v_fov), AV_OPT_TYPE_FLOAT, {.dbl = 45.0f}, 0.00001f, 360.0f, .flags = FLAGS },
    { "ih_fov", "set input horizontal FOV angle", OFFSET(ih_fov), AV_OPT_TYPE_FLOAT, {.dbl = 90.0f}, 0.00001f, 360.0f, .flags = FLAGS },
    { "iv_fov", "set input vertical FOV angle",   OFFSET(iv_fov), AV_OPT_TYPE_FLOAT, {.dbl = 45.0f}, 0.00001f, 360.0f, .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(v360_vulkan);

static const AVFilterPad v360_vulkan_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &v360_vulkan_filter_frame,
        .config_props = &ff_vk_filter_config_input,
    },
    { NULL }
};

static const AVFilterPad v360_vulkan_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &v360_vulkan_config_output,
    },
    { NULL }
};

AVFilter ff_vf_v360_vulkan = {
    .name           = "v360_vulkan",
    .description    = NULL_IF_CONFIG_SMALL("Convert 360 projection of video."),
    .priv_size      = sizeof(V360VulkanContext),
    .init           = &ff_vk_filter_init,
    .uninit         = &v360_vulkan_uninit,
    .query_formats  = &ff_vk_filter_query_formats,
    .inputs         = v360_vulkan_inputs,
    .outputs        = v360_vulkan_outputs,
    .priv_class     = &v360_vulkan_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
