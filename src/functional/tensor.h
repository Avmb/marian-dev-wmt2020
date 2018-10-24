#pragma once

#include "functional/array.h"
#include "functional/shape.h"
#include "tensors/tensor.h"

namespace marian {
namespace functional {

// general case, do nothing. Usually the number of elements in a tensor is correctly mirrored in the
// shape object. Only special multi-element types like floatX4 (4 floats), floatX16 (16 floats)
// and halfX2 (2 half) require special handling. Similar for multi-element integer types to be added
// later.
template <typename T>
inline marian::Shape adapt(const marian::Shape& shape) {
  return shape;
}

// modify last shape dimension to automatically map to a larger stride. We are moving now by 4 floats
// at once and need to stop earlier. This is a shallow typecast to bascially an array of 4 floats.

#ifndef __CUDA_ARCH__

template <>
inline marian::Shape adapt<float32x4>(const marian::Shape& shape) {
  ABORT_IF(shape[-1] % 4 != 0,
           "Last dim ({}) is not a multiple of 4 while converting to Tensor<float32x4>",
           shape[-1]);

  marian::Shape x4Shape = shape;
  x4Shape.set(-1, shape[-1] / 4);
  return x4Shape;
}

template <>
inline marian::Shape adapt<float32x8>(const marian::Shape& shape) {
  ABORT_IF(shape[-1] % 8 != 0,
           "Last dim ({}) is not a multiple of 8 while converting to Tensor<float32x4>",
           shape[-1]);

  marian::Shape x8Shape = shape;
  x8Shape.set(-1, shape[-1] / 8);
  return x8Shape;
}

#endif

template <typename T, const int D>
struct View {
  T* data_;
  ConstantShape<D> shape_;

  __HD__ View() {}

  __HD__ View(T* ptr, const ConstantShape<D>& shape)
      : data_(ptr), shape_(shape) {}

  __H__ View(marian::Tensor t) : data_(t->data<T>()), shape_(adapt<T>(t->shape())) {}

  __HDI__ T& operator[](size_t i) {
     return data_[shape_.index(i)];
  }

  __HDI__ const T& operator[](size_t i) const {
     return data_[shape_.index(i)];
  }

  __HDI__ T& operator[](const Array<int, D>& indices) {
    return data_[shape_.index(indices)];
  }

  __HDI__ const T& operator[](const Array<int, D>& indices) const {
     return data_[shape_.index(indices)];
  }

  __HDI__ T* data() { return data_; }
  __HDI__ const T* data() const { return data_; }

  __HDI__ ConstantShape<D>& shape() { return shape_; }
  __HDI__ const ConstantShape<D>& shape() const { return shape_; }

  __HDI__ size_t size() const { return shape_.elements(); }

  std::string debug(int precision = 8, int dispCols = 5) {
    std::stringstream strm;
    assert(shape_.size());

    strm << shape_;
    strm << " type=" << request<T>();
    strm << " ptr=" << (size_t)data_;
    strm << std::endl;

    size_t totSize = shape_.elements();
    std::vector<T> values(totSize);
    for(int i = 0; i < size(); ++i)
      values[i] = operator[](i);

    int colWidth  = precision + 4;
    strm << std::fixed << std::setprecision(precision) << std::setfill(' ');

    for(int i = 0; i < values.size(); ++i) {
      Array<int, D> dims;
      shape().dims(i, dims);

      bool disp = true;
      for(int j = 0; j < dims.size(); ++j)
        disp = disp && (dims[j] < dispCols || dims[j] >= shape()[j] - dispCols);

      if(disp) {
        if(dims.back() == 0) {
          bool par = true;
          std::vector<std::string> p;
          for(int j = (int)dims.size() - 1; j >= 0; --j) {
            if(dims[j] != 0)
              par = false;

            p.push_back(par ? "[" : " ");
          }
          for(auto it = p.rbegin(); it != p.rend(); ++it)
            strm << *it;
          strm << " ";
        }

        strm << std::setw(colWidth);
        strm << values[i];
        strm << " ";

        if(dims.back() + 1 == shape().back()) {
          for(int j = (int)dims.size() - 1; j >= 0; --j) {
            if(dims[j] + 1 != shape()[j])
              break;
            strm << "]";
          }
          strm << std::endl;
        }

        bool prev = true;
        for(int j = (int)dims.size() - 1; j >= 0; --j) {
          if(j < (int)dims.size() - 1)
            prev = prev && dims[j + 1] + 1 == shape()[j + 1];
          if(prev && dims[j] + 1 == dispCols && shape()[j] > 2 * dispCols) {
            if(j < (int)dims.size() - 1)
              for(int k = 0; k <= j; ++k)
                strm << " ";
            strm << "... ";
            if(j < (int)dims.size() - 1)
              strm << std::endl;
            break;
          }
        }
      }
    }
    strm << std::endl;
    return strm.str();
  }
};

template <typename T, const int D>
__HDI__ View<T, D> slice(View<T, D> view, const Array<Slice, D>& slices) {
  const auto& slicedShape = view.shape().slice(slices);
  return View<T, D>(view.data(), slicedShape);
}

// template <typename T, const int D, class ...Slices>
// View<T, D> slice(View<T, D> view,
//                  const Slices&... slices) {
//   return slice(view, {slices...});
// }

template <typename T>
__HDI__ View<T, 1> slice(View<T, 1>& view,
                         const Slice& slice0) {
  return slice(view, {slice0});
}

template <typename T>
__HDI__ View<T, 2> slice(View<T, 2>& view,
                         const Slice& slice0,
                         const Slice& slice1) {
  return slice(view, {slice0, slice1});
}

template <typename T>
__HDI__ View<T, 3> slice(View<T, 3>& view,
                        const Slice& slice0,
                        const Slice& slice1,
                        const Slice& slice2) {
  return slice(view, {slice0, slice1, slice2});
}

template <typename T>
__HDI__ View<T, 4> slice(View<T, 4>& view,
                         const Slice& slice0,
                         const Slice& slice1,
                         const Slice& slice2,
                         const Slice& slice3) {
  return slice(view, {slice0, slice1, slice2, slice3});
}

// template <typename T, const int D1, const int D2>
// View<T, D2> reshape(View<T, D1>& view, const ConstantShape<D2>& shape) {
//   auto reshaped = view.shape().reshape(shape);
//   return View<T, D2>(view.data(), reshaped);
// }

template <typename T>
using Tensor = View<T, CONST_SHAPE_DIMS>;

}  // namespace functional
}  // namespace marian