#include "graph/node_initializers.h"
#include "layers/word2vec_reader.h"
#include "tensors/tensor_operators.h"

#include <stdint.h>
#include <algorithm>
#include <iterator>
#include <random>

namespace marian {

namespace inits {

class LambdaInit : public NodeInitializer {
  private:
    std::function<void(Tensor)> lambda_;

  public:
    LambdaInit(std::function<void(Tensor)>&& lambda) : lambda_(std::move(lambda)) {}

    void apply(Tensor tensor) override {
      lambda_(tensor);
    }
};

class LambdaInitConvert : public NodeInitializer {
  private:
    std::function<void(Tensor)> lambda_;
    Type intermediateType_;

  public:
    LambdaInitConvert(std::function<void(Tensor)>&& lambda,
                      Type intermediateType)
      : lambda_(std::move(lambda)), intermediateType_(intermediateType) {}

    void apply(Tensor tensor) override {
      if(tensor->type() != intermediateType_) {
        ABORT_IF(!graph_.lock(), "Expression graph in LambdaInitConvert has not been set or expired");

        auto allocator = graph_.lock()->allocator();
        auto memory = allocator->alloc(tensor->size(), intermediateType_);
        auto temp = TensorBase::New(memory,
                                    tensor->shape(),
                                    intermediateType_,
                                    tensor->getBackend());
        lambda_(temp);
        CopyCast(tensor, temp); // Casting from temp to tensor
        allocator->free(memory);
      }
      else {
        lambda_(tensor);
      }
    }
};

Ptr<NodeInitializer> lambda(std::function<void(Tensor)>&& func) {
  return New<LambdaInit>(std::move(func));
}

Ptr<NodeInitializer> lambda(std::function<void(Tensor)>&& func, Type intermediateType) {
  return New<LambdaInitConvert>(std::move(func), intermediateType);
}

Ptr<NodeInitializer> fromValue(float v) {
  return lambda([v](Tensor t){ t->set(v); });
}

Ptr<NodeInitializer> zeros() {
  return fromValue(0.0f);
}

Ptr<NodeInitializer> ones() {
  return fromValue(1.0f);
}

// diagonal matrix with value val along diagonal
Ptr<NodeInitializer> eye(float val) {
  auto eyeLambda = [val](Tensor t) {
    ABORT_IF(t->shape().size() != 2 || t->shape()[-1] != t->shape()[-2],
              "eye(val) is defined only for quadratic tensors, shape is {}",
              t->shape());

    // @TODO: implement efficient version on the GPU
    std::vector<float> vec(t->size(), 0);
    for(int i = 0; i < t->shape()[-1]; ++i)
      vec[i * t->shape()[0] + i] = val;

    t->set(vec);
  };

  return lambda(eyeLambda, Type::float32);
}

Ptr<NodeInitializer> uniform(float a, float b) {
  // only works for float, hence the conversion through intermedia type Type::float32
  return lambda([a, b](Tensor t) { t->getBackend()->getRandomGenerator()->uniform(t, a, b); }, Type::float32);
}

Ptr<NodeInitializer> normal(float mean, float stddev) {
  // only works for float, hence the conversion through intermedia type Type::float32
  return lambda([mean, stddev](Tensor t) { t->getBackend()->getRandomGenerator()->normal(t, mean, stddev); }, Type::float32);
}

Ptr<NodeInitializer> glorotUniform(bool fanIn, bool fanOut) {
  return lambda([fanIn, fanOut](Tensor t) {
    float scale = sqrtf(6.0f / (t->shape()[-2] + t->shape()[-1]));
    if(fanIn && !fanOut)
      scale = sqrtf(3.0f / t->shape()[-2]);
    if(!fanIn && fanOut)
      scale = sqrtf(3.0f / t->shape()[-1]);

    t->getBackend()->getRandomGenerator()->uniform(t, -scale, scale);
  }, Type::float32);
}

Ptr<NodeInitializer> glorotNormal(bool fanIn, bool fanOut) {
  return lambda([fanIn, fanOut](Tensor t) {
    float scale = sqrtf(2.0f / (t->shape()[-2] + t->shape()[-1]));
    if(fanIn && !fanOut)
      scale = sqrtf(1.0f / t->shape()[-2]);
    if(!fanIn && fanOut)
      scale = sqrtf(1.0f / t->shape()[-1]);

    t->getBackend()->getRandomGenerator()->normal(t, 0.f, scale);
  }, Type::float32);
}

Ptr<NodeInitializer> bernoulli(float prob, float scale) {
  return lambda([prob, scale](Tensor t) { Bernoulli(t, prob, scale); }, Type::float32);
}

Ptr<NodeInitializer> dropout(float dropProb) {
  return lambda([dropProb](Tensor t) { Dropout(t, dropProb); }, Type::float32);
}

// gumbel noise:
// -log(-log(uniform(0.f + eps, 1.f - eps)));
Ptr<NodeInitializer> gumbel(float eps) {
  return lambda([eps](Tensor tensor) {
    tensor->getBackend()->getRandomGenerator()->uniform(tensor, 0.f + eps, 1.f - eps);
    using namespace functional;
    Element(_1 = -log(-log(_1)), tensor);
  }, Type::float32);
}

template <typename T>
Ptr<NodeInitializer> fromVector(const std::vector<T>& v) {
  return lambda([v](Tensor t) { t->set(v.data(), v.data() + v.size()); }, typeId<T>());
}

template Ptr<NodeInitializer> fromVector<float16>(const std::vector<float16>& v);
template Ptr<NodeInitializer> fromVector<float>(const std::vector<float>& v);
template Ptr<NodeInitializer> fromVector<IndexType>(const std::vector<IndexType>& v);

Ptr<NodeInitializer> fromSparseVector(std::pair<std::vector<size_t>, std::vector<float>>& v) {
  return lambda([v](Tensor t) { t->set(1e-6); t->setSparse(v.first, v.second); });
}

// move this somewhere else
Ptr<NodeInitializer> fromWord2vec(const std::string& file,
                              int dimVoc,
                              int dimEmb,
                              bool normalize /*= false*/) {
  return lambda([file, dimVoc, dimEmb, normalize](Tensor t) {
    auto embs = Word2VecReader().read(file, dimVoc, dimEmb);
    if(normalize) {
      float norm = 0;
      for(auto e : embs)
        norm += e * e;
      norm = std::sqrt(norm);
      if(norm != 0)
        for(auto& e : embs)
          e = e / norm;
    }
    t->set(embs);
  });
}

Ptr<NodeInitializer> fromItem(const io::Item& item) {
  if(item.mapped) {
    return lambda([item](Tensor tensor) {
      // @TODO: implement other types, for now croak loudly.
      ABORT_IF(tensor->getBackend()->getDeviceId().type != DeviceType::cpu,
               "Memory mapping only works for CPU tensors");
      ABORT_IF(tensor->type() != item.type,
               "Tensor type ({}) and type for mapping ({}) do not match",
               tensor->type(),
               item.type);
      ABORT_IF(tensor->size() != item.size() / sizeOf(item.type),
               "Tensor size ({}) and mapped size ({}) do not match",
               tensor->size(),
               item.size() / sizeOf(item.type));
      auto mp = MemoryPiece::New((uint8_t*)item.ptr, tensor->size() * sizeOf(item.type));
      tensor->reset(mp);
    });
  } else {
    return lambda(
      [item](Tensor tensor) { tensor->set(item); },
      item.type);
  }
}

Ptr<NodeInitializer> fromTensor(Tensor externalTensor) {
  return lambda([externalTensor](Tensor t) { t->copyFrom(externalTensor); }, externalTensor->type());
}

Ptr<NodeInitializer> dummy() {
  return lambda([](Tensor /*t*/) { });
}

// Computes Google's sinusoidal position embeddings
Ptr<NodeInitializer> sinusoidalPositionEmbeddings(int start) {
  return lambda([start](Tensor t) {
    int dimEmb   = t->shape()[-1];
    int dimWords = (int)t->size() / dimEmb;

    float numTimescales = (float)dimEmb / 2;
    float logTimescaleIncrement = std::log(10000.f) / (numTimescales - 1.f);

    std::vector<float> vPos(dimEmb * dimWords, 0);
    for(int p = start; p < dimWords + start; ++p) {
      for(int i = 0; i < numTimescales; ++i) {
        float v = p * std::exp(i * -logTimescaleIncrement);
        vPos[(p - start) * dimEmb + i                     ] = std::sin(v);
        vPos[(p - start) * dimEmb + (int)numTimescales + i] = std::cos(v); // @TODO: is int vs. float correct for num_timescales?
      }
    }

    t->set(vPos);
  }, Type::float32);
}

}  // namespace inits

}  // namespace marian
