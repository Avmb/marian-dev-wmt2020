#include "graph/expression_graph.h"
#include <sstream>

#include "tensors/tensor_operators.h"

namespace marian {

ExpressionGraph::ExpressionGraph(bool inference, bool optimized)
  : inferenceOnly_(inference),
    optimized_(optimized),
    backend_(nullptr) {}

void ExpressionGraph::setDevice(DeviceId deviceId, Ptr<Device> device) {
  if(!backend_) {
    backend_ = BackendByDeviceId(deviceId, Config::seed);
    params_ = New<Parameters>();
    params_->init(backend_);
    if(device)
      tensors_ = New<Tensors>(backend_, device);
    else
      tensors_ = New<Tensors>(backend_);
  }
}

void ExpressionGraph::backward(bool zero, float clipValue) {
  if(topNodes_.size() > 1) {
    LOG(critical, "There are more ({}) than one top most nodes for backward pass:", topNodes_.size());
    for(auto node : topNodes_) {
      LOG(critical,
          "\tType: {}, Shape: {}, Name: {}, Id: {}, Hash: {}",
          node->type(),
          node->shape(),
          node->name(),
          node->getId(),
          node->hash());
    }
    ABORT("Aborting");
  }

  params_->allocateBackward();
  if(zero)
    params_->set_zero_adjoint();

  for(auto&& v : topNodes_)
    v->init_dependent();

  // named_.clear();
  topNodes_.clear();

  tensors_->clearShorttermMemory();

  std::unordered_set<size_t> clipMe;

  bool firstNan = true;
  while(!nodesBackward_.empty()) {
    auto v = nodesBackward_.back();
    nodesBackward_.pop_back();

    for(auto&& child : v->children()) {
      if(child->trainable() && child->type() != "param")
        child->set_zero_adjoint();
      /*if(v->type() == "dot" || v->type() == "bdot")
        clipMe.insert(child->getId());*/
    }

    if(v->trainable()) {
      v->backward();
      if(clipValue != 0 /*&& clipMe.count(v->getId()) > 0*/) {
        using namespace functional;
        Element(_1 = clip(_1, clipValue), v->grad());
      }
    }


    if(throwNan_ && firstNan) {
      for(auto&& child : v->children()) {
        if(child->trainable()) {
          bool isNan = false, isInf = false;
          checkNan(child->grad(), isNan, isInf);
          if(isNan) {
            LOG(critical, "Detected NaN ({}) or Inf ({}) in gradient (backward pass) of child node", isNan, isInf);
            LOG(critical, "Child - Type: {}, Shape: {}, Name: {}, Id: {}, Hash: {}",
                child->type(), child->shape(), child->name(), child->getId(), child->hash());
            //LOG(critical, "Value debug: {}", child->val()->debug());
            //LOG(critical, "Grad debug: {}", child->grad()->debug());
            LOG(critical, "Parent - Type: {}, Shape: {}, Name: {}, Id: {}, Hash: {}",
                v->type(), v->shape(), v->name(), v->getId(), v->hash());
            //LOG(critical, "Value debug: {}", v->val()->debug());
            //LOG(critical, "Grad debug: {}", v->grad()->debug());
            //ABORT("Aborting");
            firstNan = false;
          }
        }
      }
    }

    if(v->trainable() && v->marked_for_debug()) {
      LOG(info, "Debug Grad: {} op={}", v->debug_message(), v->type());
      LOG(info, v->grad()->debug());
    }

    v->children().clear();
  }
}

Expr ExpressionGraph::dropout(float prob, const Shape& shape, Type valueType) {
  return constant(shape, inits::dropout(prob), valueType);
}

Expr ExpressionGraph::dropout(float prob, const Shape& shape) {
  return constant(shape, inits::dropout(prob), parameterType_);
}

void ExpressionGraph::checkNan(Tensor t, bool& isNan, bool& isInf) {
  IsNan(t, allocator(), isNan, isInf);
}

void ExpressionGraph::save(std::vector<io::Item>& ioItems) {
  for(auto p : params()->getMap()) {
    std::string pName = p.first;

    if(!namespace_.empty()) {
      if(pName.substr(0, namespace_.size() + 2) == namespace_ + "::")
        pName = pName.substr(namespace_.size() + 2);
    }

    Tensor val = p.second->val();
    io::Item item;
    // item.name = pName;
    // item.type = float32; // @TODO: handle conversion
    val->get(item, pName);
    ioItems.emplace_back(std::move(item));
  }
}

}  // namespace marian
