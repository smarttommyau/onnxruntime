// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <memory>
#include "core/providers/cuda/cuda_kernel.h"
#include "contrib_ops/cpu/bert/attention_base.h"
#include "contrib_ops/cuda/bert/tensorrt_fused_multihead_attention/mha_runner.h"

namespace onnxruntime {
namespace contrib {
namespace cuda {

using namespace onnxruntime::cuda;

template <typename T>
class Attention final : public CudaKernel, public AttentionBase {
 public:
  Attention(const OpKernelInfo& info);
  ~Attention();
  Status ComputeInternal(OpKernelContext* context) const override;

 protected:
  bool disable_fused_runner_;
  mutable std::unique_ptr<MHARunner> fused_fp16_runner_;
  mutable void* data_ptr_ = nullptr;
  bool use_data_ptr_ = false;
};

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
