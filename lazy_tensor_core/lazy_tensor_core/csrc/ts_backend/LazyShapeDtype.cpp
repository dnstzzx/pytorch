#pragma once

// This file contains autogenerated LazyTensor IR nodes
#include <c10/core/ScalarType.h>
#include <c10/util/Optional.h>
#include <vector>

#include "lazy_tensor_core/csrc/ts_backend/LazyShapeDtype.h"

namespace torch_lazy_tensors{
namespace ir {
namespace ops {

std::vector<int64_t> compute_shape_mean(const at::Tensor & self, c10::optional<at::ScalarType> dtype) {
    return std::vector<int64_t>({1});
}
c10::ScalarType compute_dtype_mean(const at::Tensor & self, c10::optional<at::ScalarType> dtype) {
    if(dtype.has_value()){
        return dtype.value();
    }
    return self.scalar_type();
}

} // namespace ops
} // namespace ir
} // namespace torch_lazy_tensors