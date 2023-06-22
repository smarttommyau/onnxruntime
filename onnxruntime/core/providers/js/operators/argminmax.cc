// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "argmax.h"

namespace onnxruntime {
namespace js {

#define REGISTER_ARGMAX_ELEMENTWISE_VERSIONED_KERNEL(ArgMinMaxOp, sinceVersion, endVersion) \
  ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_EX(                                                  \
      ArgMinMaxOp,                                                                          \
      kOnnxDomain,                                                                          \
      sinceVersion, endVersion,                                                             \
      float,                                                                                \
      kJsExecutionProvider,                                                                 \
      (*KernelDefBuilder::Create())                                                         \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),                       \
      ArgMinMaxOp<float>);

#define REGISTER_ARGMAX_ELEMENTWISE_KERNEL(ArgMinMaxOp, sinceVersion)   \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                        \
      ArgMinMaxOp,                                                      \
      kOnnxDomain,                                                      \
      sinceVersion,                                                     \
      float,                                                            \
      kJsExecutionProvider,                                             \
      (*KernelDefBuilder::Create())                                     \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<float>())    \
          .InputMemoryType(OrtMemTypeCPU, 1),                           \
      ArgMinMaxOp<float>);

REGISTER_ARGMAX_ELEMENTWISE_VERSIONED_KERNEL(ArgMax, 1, 10);
REGISTER_ARGMAX_ELEMENTWISE_VERSIONED_KERNEL(ArgMax, 11, 11);
REGISTER_ARGMAX_ELEMENTWISE_KERNEL(ArgMax, 12);

REGISTER_ARGMAX_ELEMENTWISE_VERSIONED_KERNEL(ArgMin, 1, 10);
REGISTER_ARGMAX_ELEMENTWISE_VERSIONED_KERNEL(ArgMin, 11, 11);
REGISTER_ARGMAX_ELEMENTWISE_KERNEL(ArgMin, 12);

}  // namespace js
}  // namespace onnxruntime
