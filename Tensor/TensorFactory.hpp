//
//  TensorFactory.hpp
//  Tensor
//
//  Created by 陳均豪 on 2022/1/29.
//

#ifndef TensorFactory_hpp
#define TensorFactory_hpp

#include "Tensor.hpp"

namespace otter {

Tensor empty(IntArrayRef size, ScalarType dtype);

Tensor empty_strided(IntArrayRef size, IntArrayRef stride, ScalarType dtype);

Tensor empty_like(const Tensor& self);
Tensor empty_like(const Tensor& self, const TensorOptions& options);
Tensor empty_like(const Tensor& self, ScalarType dtype);

Tensor clone(const Tensor& src);

Tensor full(IntArrayRef size, const Scalar& fill_value, ScalarType dtype);

Tensor zeros(IntArrayRef size, ScalarType dtype);
Tensor ones(IntArrayRef size, ScalarType dtype);

Tensor linspace(const Scalar& start, const Scalar& end, int64_t steps, ScalarType dtype);


}   // namespace otter

#endif /* TensorFactory_hpp */