// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <arm_neon.h>

#include <algorithm>
#include <cmath>

#include "lite/backends/arm/math/activation.h"
#include "lite/backends/arm/math/affine_channel.h"
#include "lite/backends/arm/math/argmax.h"
#include "lite/backends/arm/math/axpy.h"
#include "lite/backends/arm/math/box_coder.h"
#include "lite/backends/arm/math/clip.h"
#include "lite/backends/arm/math/col_im_transform.h"
#include "lite/backends/arm/math/concat.h"
#include "lite/backends/arm/math/conv_block_utils.h"
#include "lite/backends/arm/math/conv_impl.h"
#include "lite/backends/arm/math/conv_transpose_depthwise.h"
#include "lite/backends/arm/math/decode_bboxes.h"
#include "lite/backends/arm/math/dropout.h"
#include "lite/backends/arm/math/elementwise.h"
#include "lite/backends/arm/math/fill_bias_relu.h"
#include "lite/backends/arm/math/gemm_prepacked_int8.h"
#include "lite/backends/arm/math/gemm_s8.h"
#include "lite/backends/arm/math/gemv_arm_int8.h"
#include "lite/backends/arm/math/interpolate.h"
#include "lite/backends/arm/math/layout.h"
#include "lite/backends/arm/math/lrn.h"
#include "lite/backends/arm/math/negative.h"
#include "lite/backends/arm/math/norm.h"
#include "lite/backends/arm/math/packed_sgemm.h"
#include "lite/backends/arm/math/packed_sgemm_c4.h"
#include "lite/backends/arm/math/pad2d.h"
#include "lite/backends/arm/math/pooling.h"
#include "lite/backends/arm/math/power.h"
#include "lite/backends/arm/math/quantize.h"
#include "lite/backends/arm/math/reduce_max.h"
#include "lite/backends/arm/math/reduce_max_min.h"
#include "lite/backends/arm/math/reduce_mean.h"
#include "lite/backends/arm/math/reduce_min.h"
#include "lite/backends/arm/math/reduce_prod.h"
#include "lite/backends/arm/math/reduce_sum.h"
#include "lite/backends/arm/math/scale.h"
#include "lite/backends/arm/math/scatter.h"
#include "lite/backends/arm/math/sequence_expand.h"
#include "lite/backends/arm/math/sequence_pool.h"
#include "lite/backends/arm/math/sequence_pool_grad.h"
#include "lite/backends/arm/math/sgemm.h"
#include "lite/backends/arm/math/sgemv.h"
#include "lite/backends/arm/math/slice.h"
#include "lite/backends/arm/math/softmax.h"
#include "lite/backends/arm/math/sparse_conv_impl.h"
#include "lite/backends/arm/math/sparse_semi_conv_impl.h"
#include "lite/backends/arm/math/split_merge_lod_tenosr.h"

namespace paddle {
namespace lite {
namespace arm {
namespace math {

#define c_inv_mant_mask ~0x7f800000u
#define c_cephes_SQRTHF 0.707106781186547524
#define c_cephes_log_p0 7.0376836292E-2
#define c_cephes_log_p1 -1.1514610310E-1
#define c_cephes_log_p2 1.1676998740E-1
#define c_cephes_log_p3 -1.2420140846E-1
#define c_cephes_log_p4 +1.4249322787E-1
#define c_cephes_log_p5 -1.6668057665E-1
#define c_cephes_log_p6 +2.0000714765E-1
#define c_cephes_log_p7 -2.4999993993E-1
#define c_cephes_log_p8 +3.3333331174E-1
#define c_cephes_log_q1 -2.12194440e-4
#define c_cephes_log_q2 0.693359375

// natural logarithm computed for 4 simultaneous float
// return NaN for x <= 0
inline float32x4_t log_ps(float32x4_t x) {
  float32x4_t one = vdupq_n_f32(1);

  x = vmaxq_f32(x, vdupq_n_f32(0));  // force flush to zero on denormal values
  uint32x4_t invalid_mask = vcleq_f32(x, vdupq_n_f32(0));

  int32x4_t ux = vreinterpretq_s32_f32(x);

  int32x4_t emm0 = vshrq_n_s32(ux, 23);

  // keep only the fractional part
  ux = vandq_s32(ux, vdupq_n_s32(c_inv_mant_mask));
  ux = vorrq_s32(ux, vreinterpretq_s32_f32(vdupq_n_f32(0.5f)));
  x = vreinterpretq_f32_s32(ux);

  emm0 = vsubq_s32(emm0, vdupq_n_s32(0x7f));
  float32x4_t e = vcvtq_f32_s32(emm0);

  e = vaddq_f32(e, one);

  // part2:
  // if( x < SQRTHF ) {
  //   e -= 1;
  //   x = x + x - 1.0;
  // } else {
  //   x = x - 1.0;
  // }
  //
  uint32x4_t mask = vcltq_f32(x, vdupq_n_f32(c_cephes_SQRTHF));
  float32x4_t tmp =
      vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(x), mask));
  x = vsubq_f32(x, one);
  e = vsubq_f32(
      e, vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(one), mask)));
  x = vaddq_f32(x, tmp);

  float32x4_t z = vmulq_f32(x, x);

  float32x4_t y = vdupq_n_f32(c_cephes_log_p0);
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, vdupq_n_f32(c_cephes_log_p1));
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, vdupq_n_f32(c_cephes_log_p2));
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, vdupq_n_f32(c_cephes_log_p3));
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, vdupq_n_f32(c_cephes_log_p4));
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, vdupq_n_f32(c_cephes_log_p5));
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, vdupq_n_f32(c_cephes_log_p6));
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, vdupq_n_f32(c_cephes_log_p7));
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, vdupq_n_f32(c_cephes_log_p8));
  y = vmulq_f32(y, x);

  y = vmulq_f32(y, z);

  tmp = vmulq_f32(e, vdupq_n_f32(c_cephes_log_q1));
  y = vaddq_f32(y, tmp);

  tmp = vmulq_f32(z, vdupq_n_f32(0.5f));
  y = vsubq_f32(y, tmp);

  tmp = vmulq_f32(e, vdupq_n_f32(c_cephes_log_q2));
  x = vaddq_f32(x, y);
  x = vaddq_f32(x, tmp);
  x = vreinterpretq_f32_u32(vorrq_u32(
      vreinterpretq_u32_f32(x), invalid_mask));  // negative arg will be NAN
  return x;
}

#define c_exp_hi 88.3762626647949f
#define c_exp_lo -88.3762626647949f

#define c_cephes_LOG2EF 1.44269504088896341
#define c_cephes_exp_C1 0.693359375
#define c_cephes_exp_C2 -2.12194440e-4

#define c_cephes_exp_p0 1.9875691500E-4
#define c_cephes_exp_p1 1.3981999507E-3
#define c_cephes_exp_p2 8.3334519073E-3
#define c_cephes_exp_p3 4.1665795894E-2
#define c_cephes_exp_p4 1.6666665459E-1
#define c_cephes_exp_p5 5.0000001201E-1

// exp() computed for 4 float at once
inline float32x4_t exp_ps(float32x4_t x) {
  float32x4_t tmp, fx;
  float32x4_t one = vdupq_n_f32(1);
  x = vminq_f32(x, vdupq_n_f32(c_exp_hi));
  x = vmaxq_f32(x, vdupq_n_f32(c_exp_lo));

  // express exp(x) as exp(g + n*log(2))
  fx = vmlaq_f32(vdupq_n_f32(0.5f), x, vdupq_n_f32(c_cephes_LOG2EF));

// perform a floorf
#ifdef __aarch64__
  tmp = vcvtq_f32_s32(vcvtq_s32_f32(fx));
#else
  uint32x4_t vmask_1 = vcgeq_f32(fx, vdupq_n_f32(0.f));
  float32x4_t voffset_1 =
      vbslq_f32(vmask_1, vdupq_n_f32(0.5f), vdupq_n_f32(-0.5f));
  float32x4_t fx_tmp_1 = vaddq_f32(fx, voffset_1);
  tmp = vcvtq_f32_s32(vcvtq_s32_f32(fx_tmp_1));
#endif

  // if greater, substract 1
  uint32x4_t mask = vcgtq_f32(tmp, fx);
  mask = vandq_u32(mask, vreinterpretq_u32_f32(one));

  fx = vsubq_f32(tmp, vreinterpretq_f32_u32(mask));

  tmp = vmulq_f32(fx, vdupq_n_f32(c_cephes_exp_C1));
  float32x4_t z = vmulq_f32(fx, vdupq_n_f32(c_cephes_exp_C2));
  x = vsubq_f32(x, tmp);
  x = vsubq_f32(x, z);

  static const float cephes_exp_p[6] = {c_cephes_exp_p0,
                                        c_cephes_exp_p1,
                                        c_cephes_exp_p2,
                                        c_cephes_exp_p3,
                                        c_cephes_exp_p4,
                                        c_cephes_exp_p5};
  float32x4_t y = vld1q_dup_f32(cephes_exp_p + 0);
  float32x4_t c1 = vld1q_dup_f32(cephes_exp_p + 1);
  float32x4_t c2 = vld1q_dup_f32(cephes_exp_p + 2);
  float32x4_t c3 = vld1q_dup_f32(cephes_exp_p + 3);
  float32x4_t c4 = vld1q_dup_f32(cephes_exp_p + 4);
  float32x4_t c5 = vld1q_dup_f32(cephes_exp_p + 5);

  y = vmulq_f32(y, x);
  z = vmulq_f32(x, x);

  y = vaddq_f32(y, c1);
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, c2);
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, c3);
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, c4);
  y = vmulq_f32(y, x);
  y = vaddq_f32(y, c5);

  y = vmulq_f32(y, z);
  y = vaddq_f32(y, x);
  y = vaddq_f32(y, one);

  // build 2^n
  int32x4_t mm;
#ifdef __aarch64__
  mm = vcvtq_s32_f32(fx);
#else
  uint32x4_t vmask_2 = vcgeq_f32(fx, vdupq_n_f32(0.f));
  float32x4_t voffset_2 =
      vbslq_f32(vmask_2, vdupq_n_f32(0.5f), vdupq_n_f32(-0.5f));
  float32x4_t fx_tmp_2 = vaddq_f32(fx, voffset_2);
  mm = vcvtq_s32_f32(fx_tmp_2);
#endif
  mm = vaddq_s32(mm, vdupq_n_s32(0x7f));
  mm = vshlq_n_s32(mm, 23);
  float32x4_t pow2n = vreinterpretq_f32_s32(mm);
  y = vmulq_f32(y, pow2n);
  return y;
}

#define c_minus_cephes_DP1 -0.78515625
#define c_minus_cephes_DP2 -2.4187564849853515625e-4
#define c_minus_cephes_DP3 -3.77489497744594108e-8
#define c_sincof_p0 -1.9515295891E-4
#define c_sincof_p1 8.3321608736E-3
#define c_sincof_p2 -1.6666654611E-1
#define c_coscof_p0 2.443315711809948E-005
#define c_coscof_p1 -1.388731625493765E-003
#define c_coscof_p2 4.166664568298827E-002
#define c_cephes_FOPI 1.27323954473516  // 4 / M_PI

// evaluation of 4 sines & cosines at once.
//
// The code is the exact rewriting of the cephes sinf function.
// Precision is excellent as long as x < 8192 (I did not bother to
// take into account the special handling they have for greater values
// -- it does not return garbage for arguments over 8192, though, but
// the extra precision is missing).
//
// Note that it is such that sinf((float)M_PI) = 8.74e-8, which is the
// surprising but correct result.
//
// Note also that when you compute sin(x), cos(x) is available at
// almost no extra price so both sin_ps and cos_ps make use of
// sincos_ps..
//
inline void sincos_ps(float32x4_t x, float32x4_t* ysin, float32x4_t* ycos) {
  // any x
  float32x4_t xmm1, xmm2, xmm3, y;

  uint32x4_t emm2;

  uint32x4_t sign_mask_sin, sign_mask_cos;
  sign_mask_sin = vcltq_f32(x, vdupq_n_f32(0));
  x = vabsq_f32(x);

  // scale by 4/Pi
  y = vmulq_f32(x, vdupq_n_f32(c_cephes_FOPI));

  // store the integer part of y in mm0
  emm2 = vcvtq_u32_f32(y);
  // j=(j+1) & (~1) (see the cephes sources)
  emm2 = vaddq_u32(emm2, vdupq_n_u32(1));
  emm2 = vandq_u32(emm2, vdupq_n_u32(~1));
  y = vcvtq_f32_u32(emm2);

  // get the polynom selection mask
  // there is one polynom for 0 <= x <= Pi/4
  // and another one for Pi/4<x<=Pi/2
  uint32x4_t poly_mask = vtstq_u32(emm2, vdupq_n_u32(2));

  // the magic pass: "Extended precision modular arithmetic"
  // x = ((x - y * DP1) - y * DP2) - y * DP3;
  xmm1 = vmulq_n_f32(y, c_minus_cephes_DP1);
  xmm2 = vmulq_n_f32(y, c_minus_cephes_DP2);
  xmm3 = vmulq_n_f32(y, c_minus_cephes_DP3);
  x = vaddq_f32(x, xmm1);
  x = vaddq_f32(x, xmm2);
  x = vaddq_f32(x, xmm3);

  sign_mask_sin = veorq_u32(sign_mask_sin, vtstq_u32(emm2, vdupq_n_u32(4)));
  sign_mask_cos = vtstq_u32(vsubq_u32(emm2, vdupq_n_u32(2)), vdupq_n_u32(4));

  // evaluate the first polynom  (0 <= x <= Pi/4) in y1,
  // and the second polynom      (Pi/4 <= x <= 0) in y2
  float32x4_t z = vmulq_f32(x, x);
  float32x4_t y1, y2;

  y1 = vmulq_n_f32(z, c_coscof_p0);
  y2 = vmulq_n_f32(z, c_sincof_p0);
  y1 = vaddq_f32(y1, vdupq_n_f32(c_coscof_p1));
  y2 = vaddq_f32(y2, vdupq_n_f32(c_sincof_p1));
  y1 = vmulq_f32(y1, z);
  y2 = vmulq_f32(y2, z);
  y1 = vaddq_f32(y1, vdupq_n_f32(c_coscof_p2));
  y2 = vaddq_f32(y2, vdupq_n_f32(c_sincof_p2));
  y1 = vmulq_f32(y1, z);
  y2 = vmulq_f32(y2, z);
  y1 = vmulq_f32(y1, z);
  y2 = vmulq_f32(y2, x);
  y1 = vsubq_f32(y1, vmulq_f32(z, vdupq_n_f32(0.5f)));
  y2 = vaddq_f32(y2, x);
  y1 = vaddq_f32(y1, vdupq_n_f32(1));

  // select the correct result from the two polynoms
  float32x4_t ys = vbslq_f32(poly_mask, y1, y2);
  float32x4_t yc = vbslq_f32(poly_mask, y2, y1);
  *ysin = vbslq_f32(sign_mask_sin, vnegq_f32(ys), ys);
  *ycos = vbslq_f32(sign_mask_cos, yc, vnegq_f32(yc));
}

inline float32x4_t sin_ps(float32x4_t x) {
  float32x4_t ysin, ycos;
  sincos_ps(x, &ysin, &ycos);
  return ysin;
}

inline float32x4_t cos_ps(float32x4_t x) {
  float32x4_t ysin, ycos;
  sincos_ps(x, &ysin, &ycos);
  return ycos;
}

inline float32x4_t div_ps(float32x4_t a, float32x4_t b) {
  float32x4_t reciprocal = vrecpeq_f32(b);
  reciprocal = vmulq_f32(vrecpsq_f32(b, reciprocal), reciprocal);
  return vmulq_f32(a, reciprocal);
}

inline float32x4_t pow_ps(float32x4_t a, float32x4_t b) {
  // pow(x, m) = exp(m * log(x))
  float32x4_t vone = vdupq_n_f32(1.f);
  // x < 0
  for (int i = 0; i < 4; i++) {
    if (a[i] < 0) {
      a[i] = -a[i];
      if (static_cast<int>(b[i]) % 2) {
        vone[i] = -1.f;
      }
    }
  }
  float32x4_t vsum = exp_ps(vmulq_f32(b, log_ps(a)));
  vsum = vmulq_f32(vsum, vone);
  return vsum;
}

inline float32x4_t vpaddq_f32(float32x4_t a, float32x4_t b) {
  float32x4_t vrst;
  vrst[0] = a[0] + a[1];
  vrst[1] = a[2] + a[3];
  vrst[2] = b[0] + b[1];
  vrst[3] = b[2] + b[3];
  return vrst;
}

template <typename T>
void fill_bias_fc(
    T* tensor, const T* bias, int num, int channel, bool flag_relu);

template <lite_api::ActivationType Act = lite_api::ActivationType::kIndentity>
inline float32x4_t vactive_f32(const float32x4_t& x) {
  return x;
}

template <>
inline float32x4_t vactive_f32<lite_api::ActivationType::kRelu>(
    const float32x4_t& x) {
  float32x4_t __zero = vdupq_n_f32(0.f);
  return vmaxq_f32(x, __zero);
}

template <>
inline float32x4_t vactive_f32<lite_api::ActivationType::kRelu6>(
    const float32x4_t& x) {
  float32x4_t __zero = vdupq_n_f32(0.f);
  float32x4_t __six = vdupq_n_f32(6.f);
  return vminq_f32(vmaxq_f32(x, __zero), __six);
}

template <>
inline float32x4_t vactive_f32<lite_api::ActivationType::kSigmoid>(
    const float32x4_t& x) {
  float32x4_t __one = vdupq_n_f32(1.f);
  float32x4_t __x = vnegq_f32(x);
  __x = exp_ps(__x);
  __x = vaddq_f32(__x, __one);
  float32x4_t __out = vrecpeq_f32(__x);
  return vmulq_f32(vrecpsq_f32(__x, __out), __out);
}

template <>
inline float32x4_t vactive_f32<lite_api::ActivationType::kTanh>(
    const float32x4_t& x) {
  float32x4_t __one = vdupq_n_f32(1.f);
  float32x4_t __x = vmulq_n_f32(x, -2.f);
  __x = exp_ps(__x);
  __x = vaddq_f32(__x, __one);
  float32x4_t __out = vrecpeq_f32(__x);
  __out = vmulq_f32(vrecpsq_f32(__x, __out), __out);
  __out = vmulq_n_f32(__out, 2.f);
  return vsubq_f32(__out, __one);
}

template <lite_api::ActivationType Act = lite_api::ActivationType::kIndentity>
inline float active_f32(const float& x) {
  return x;
}

template <>
inline float active_f32<lite_api::ActivationType::kRelu>(const float& x) {
  return std::max(x, 0.f);
}

template <>
inline float active_f32<lite_api::ActivationType::kRelu6>(const float& x) {
  return std::min(std::max(x, 0.f), 6.f);
}

template <>
inline float active_f32<lite_api::ActivationType::kSigmoid>(const float& x) {
  return 1.f / (1.f + exp(-x));
}

template <>
inline float active_f32<lite_api::ActivationType::kTanh>(const float& x) {
  return 2.f / (1.f + exp(-2.f * x)) - 1.f;
}

}  // namespace math
}  // namespace arm
}  // namespace lite
}  // namespace paddle
