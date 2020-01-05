#pragma once

#include <cstdint>
#include <string>

#include "common/shape.h"

#include "functional/array.h"

namespace marian {

namespace functional {

#define CONST_SHAPE_DIMS 4

// attempts at low-level slicing and proper views, not integrated yet
#if 0 
const size_t MAX_INT = std::numeric_limits<size_t>::max();
struct Slice {
  static const size_t END{MAX_INT}; // fix

  size_t begin{0};
  size_t end{END};
  size_t stride{1};

  Slice(size_t b, size_t e, size_t s = 1)
  : begin(b), end(e), stride(s) {}

  Slice()
  : begin(0), end(END), stride(1) {}

  Slice(size_t i)
  : begin(i), end(i + 1), stride(1) {}

  Slice(const std::initializer_list<size_t>& l) {
    std::vector<size_t> v(l);
    switch(v.size()) {
      case 0: begin = 0;    end = END;      stride = 1;    break;
      case 1: begin = v[0]; end = v[0] + 1; stride = 1;    break;
      case 2: begin = v[0]; end = v[1];     stride = 1;    break;
      case 3: begin = v[0]; end = v[1];     stride = v[2]; break;
      default:
        ABORT("Too many elements in slice: {}", v.size());
    }
  }
};

const Slice All;
#endif

/**
 * @brief Represents the size of each dimension in a tensor.
 */

template <const size_t N>
struct ConstantShape {
  Array<size_t, N> shape_;
  Array<size_t, N> stride_;
  Array<size_t, N> bstride_;

  size_t elements_{1};
  size_t offset_{0};

  // @TODO: review all these constructors
  HOST_DEVICE ConstantShape() {
    shape_.fill(1);
    stride_.fill(1);
    bstride_.fill(0);
  }

  HOST_DEVICE ConstantShape(const ConstantShape& shape)
      : shape_(shape.shape_),
        stride_(shape.stride_),
        bstride_(shape.bstride_),
        elements_(shape.elements_),
        offset_(shape.offset_) {}

  template <size_t M>
  HOST_DEVICE ConstantShape(const Array<size_t, M>& shape) {
    ABORT_IF(M > N, "Recompile with CONST_SHAPE_DIMS >= {}", M);

    std::copy(shape.begin(), shape.end(), shape_.begin() + N - M);
    if(N - M)
      std::fill_n(shape_.begin(), N - M, 1);

    updateStrides();
    updateElements();
  }

  HOST_DEVICE ConstantShape(const Array<size_t, N>& shape,
                            const Array<size_t, N>& stride,
                            size_t offset)
  : shape_(shape), stride_(stride), offset_(offset) {
    updateElements();
  }

  ConstantShape(const marian::Shape& shape) {
    size_t filled = shape.size();

    ABORT_IF(filled > N,
             "Recompile with CONST_SHAPE_DIMS >= " + std::to_string(filled));

    std::copy(shape.begin(), shape.end(), shape_.begin() + N - filled);
    if(N - filled)
      std::fill_n(shape_.begin(), N - filled, 1);

    updateStrides();
    updateElements();
  }

  // @TODO: do we need bstrides at all?
  HOST_DEVICE_INLINE void updateStrides() {
    stride_[N - 1] = 1;
    bstride_[N - 1] = shape_[N - 1] == 1 ? 0 : stride_[N - 1];

    for(int i = N - 2; i >= 0; --i) {
      stride_[i] = stride_[i + 1] * shape_[i + 1];
      bstride_[i] = shape_[i] == 1 ? 0 : stride_[i];
    }
  }

  HOST_DEVICE_INLINE void updateElements() {
    elements_ = 1;
    for(size_t i = 0; i < N; ++i)
      elements_ *= shape_[i];
  }

  HOST_DEVICE_INLINE void set(size_t i, size_t dim) {
    shape_[i] = dim;
    updateStrides();
    updateElements();
  }

  HOST_DEVICE_INLINE const size_t& dim(size_t i) const { return shape_[i]; }

  HOST_DEVICE_INLINE const size_t& back() const { return dim(N - 1); }

  HOST_DEVICE_INLINE const size_t& operator[](size_t i) const { return dim(i); }

  HOST_DEVICE_INLINE const size_t& stride(size_t i) const { return stride_[i]; }

  HOST_DEVICE_INLINE const size_t& bstride(size_t i) const { return bstride_[i]; }

  HOST_DEVICE_INLINE static constexpr size_t size() { return N; }

  HOST_DEVICE_INLINE size_t elements() const { return elements_; }

  // The following functions iterate over shape dimensions and use recursive
  // templates. They unroll over a compile-time defined number of dimensions.

  // Struct for recurrent template calls over shape dimensions,
  // version for K > 0
  template <const size_t K, const size_t D> struct I {
    HOST_DEVICE_INLINE static size_t index(const Array<size_t, D>& dims,
                                           const Array<size_t, D>& stride) {
      return dims[K] * stride[K] + I<K-1, D>::index(dims, stride);
    }

    HOST_DEVICE_INLINE static size_t index(size_t si,
                                           const Array<size_t, D>& shape,
                                           const Array<size_t, D>& stride) {
      return (si % shape[K]) * stride[K] + I<K-1, D>::index(si / shape[K], shape, stride);
    }

    HOST_DEVICE_INLINE static void dims(size_t si,
                             Array<size_t, D>& dims,
                             const Array<size_t, D>& shape) {
      dims[K] = si % shape[K];
      I<K-1, D>::dims(si / shape[K], dims, shape);
    }

  };

  // Struct for recurrent template calls over shape dimensions,
  // specialization for K == 0
  template <const size_t D> struct I<0, D> {
    HOST_DEVICE_INLINE static size_t index(const Array<size_t, D>& dims,
                                           const Array<size_t, D>& stride) {
      return dims[0] * stride[0];
    }

    HOST_DEVICE_INLINE static size_t index(size_t si,
                                           const Array<size_t, D>& shape,
                                           const Array<size_t, D>& stride) {
      return (si % shape[0]) * stride[0];
    }

   HOST_DEVICE_INLINE static void dims(size_t si,
                                       Array<size_t, D>& dims,
                                       const Array<size_t, D>& shape) {
      dims[0] = si % shape[0];
    }
  };

  HOST_DEVICE_INLINE size_t index(const Array<size_t, N>& dims) const {
    return (size_t)offset_ + I<N-1, N>::index(dims, stride_);
  }

  HOST_DEVICE_INLINE size_t index(size_t si) const {
    return (size_t)offset_ + I<N-1, N>::index(si, shape_, stride_);
  }

  HOST_DEVICE_INLINE void dims(size_t si, Array<size_t, N>& dims) const {
    I<N-1, N>::dims(si, dims, shape_);
  }

  HOST_DEVICE_INLINE size_t bindex(const Array<size_t, N>& dims) const {
    size_t i = 0;
    // ?? : return offset_ + I<N-1, N>::index(d, bstride_);
    for(size_t j = 0; j < N; ++j)
      i += dims[j] * bstride_[j];
    return i;
  }

  // @TODO: should this check all the members?
  HOST_DEVICE_INLINE bool operator==(const ConstantShape& other) const {
    for(size_t i = 0; i < N; ++i)
      if(shape_[i] != other[i])
        return false;
    return true;
  }

  HOST_DEVICE_INLINE bool operator!=(const ConstantShape& other) const {
    return !(*this == other);
  }

  std::string toString() const {
    std::stringstream strm;
    // @TODO: add more information
    strm << "shape=" << (*this)[0];
    for(size_t i = 1; i < size(); ++i)
      strm << "x" << (*this)[i];
    strm << " size=" << elements();
    return strm.str();
  }

// @TODO: attempts at proper slicing. Works but not integrated anywhere. To be revisited.
#if 0
  // Performs numpy-like slicing on a given shape object. The number
  // of slices corresponds to the number of dimensions.
  HOST_DEVICE_INLINE ConstantShape<N> slice(const Array<Slice, N>& slices) {
    // @TODO: add various checks
    Array<size_t, N> offsets;
    Array<size_t, N> shape;
    Array<size_t, N> stride;
    for(size_t i = 0; i < N; ++i) {
      size_t beg = slices[i].begin;
      // restrict maximum value to actual shape size if larger than shape size
      size_t end = slices[i].end < shape_[i] ? slices[i].end : shape_[i];
      size_t str = slices[i].stride;

      // collect starting points for all coordinates
      offsets[i] = beg;

      // when calculating the new shape, take into account stride
      // TODO: std::ceil does not work on the GPU
      shape[i]   = std::ceil((end - beg) / (float) str);

      // new stride is just old stride multiplied by slice stride
      stride[i]  = str * stride_[i];
    }

    // map offset coordinates into single offset index
    size_t offset = index(offsets);

    return ConstantShape<N>(shape, stride, offset);
  }

// non-continguous slices cannot be reshaped! need to be copied
//   template <const size_t D>
//   HOST_DEVICE_INLINE ConstantShape<D> reshape(const ConstantShape<D>& other) const {
//     // @TODO: add various checks
// #ifndef __CUDA__ARCH__
//     ABORT_IF(elements() != other.elements(),
//              "Reshaping operation requires matching number of elements");
// #endif

//     Array<size_t, D> stride;
//     for(size_t i = 0; i < D; ++i) {
//       stride[i] = /*other.stride_[i] **/ stride_[i];
//     }

//     stride[D - 1] = stride_[N - 1];
//     for(size_t i = 2; i < D + 1; ++i) {
//       stride[D - i] = stride[D - i + 1] * stride_[N - i + 1] * shape_[D - i + 1];
//     }

//     return ConstantShape<D>(other.shape_, stride, offset_);
//   }
#endif

  friend std::ostream& operator<<(std::ostream& strm, const ConstantShape<N>& shape) {
    strm << shape.toString();
    return strm;
  }
};

typedef ConstantShape<CONST_SHAPE_DIMS> Shape;
}  // namespace functional
}  // namespace marian
