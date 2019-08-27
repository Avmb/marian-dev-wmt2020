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

Expr ExpressionGraph::add(Expr node) {
  auto found = tensors_->findOrRemember(node);
  if(found) {
    return found;
  } else {
    node->setId(count_++);

    // record in foward graph
    nodesForward_.push_back(node);

    // record in backward graph if training, and keep track of roots
    if(!inferenceOnly_ && node->trainable()) {
      nodesBackward_.push_back(node);
      topNodes_.insert(node); // opportunistically record all new nodes as roots (gets removed once consumed)
    }

    if(topNodes_.count(node)) // only erase children of nodes with are themselves in the topNodes list
      for(auto child : node->children())
        topNodes_.erase(child); // this child is consumed and therefore not a root

    return node;
  }
}

// Call on every checkpoint in backwards order
void createSubtape(Expr node) {
  //std::cerr << "Recursing: " << node->getId() << " " << node->type() << std::endl;
  auto subtape = New<std::list<Expr>>();

  for(auto child : node->children()) {
    if(child->isCheckpoint()) {
      //std::cerr << "cut" << std::endl;
      /* do not descend */
    } else {
      if(child->getSubtape()) {
        //std::cerr << "Already visited: " << node->getId() << " " << node->type() << std::endl;
        /* already visited */
      } else {
        createSubtape(child);
        subtape->splice(subtape->end(), *(child->getSubtape()));
      }
    }
  }

  if(!node->isCheckpoint())
    subtape->push_back(node);

  node->setSubtape(subtape);
}


void ExpressionGraph::forwardNext() {
  // @TODO: check if allocation works properly
  tensors_->clearShorttermMemory();

  if(checkpointing_) {
    for(auto top : topNodes_)
      top->markCheckpoint();

    auto it = nodesBackward_.rbegin();
    while(it != nodesBackward_.rend()) {
      auto v = *it;
      if(v->isCheckpoint())
        createSubtape(v);
      it++;
    }

    // To avoid recomputation of range from last checkpoint to the top,
    // turn all node on last subtape into checkpoints and clear subtape.
    // @TODO: put this into special backprob function? Needs to know that we are done with adding nodes
    for(auto top : topNodes_) {
      if(top->getSubtape()) {
        for(auto& node : *top->getSubtape())
          node->markCheckpoint();
        top->getSubtape()->clear();
      }
    }
  }

  // graphviz("dump.dot");

  forward(nodesForward_, /*finalPass=*/!checkpointing_); // if checkPointing, this is not final
}

void ExpressionGraph::forward(std::list<Expr>& forwardTape, bool finalPass) {
  while(!forwardTape.empty()) {
    auto v = forwardTape.front();

    //std::cerr << "Forward: " << v->getId() << " " << v->type() << " ";
    //std::cerr << v->isCheckpoint() << std::endl;

    v->allocate();
    /*if(!isFake())*/ v->init();

    for(auto& child : v->children())
      ABORT_IF(!child->val(), "De-allocated child {} {} of {} {}", child->getId(), child->type(), v->getId(), v->type());

    /*if(!isFake())*/ v->forward();

    if(v->trainable() && throwNan_) {
      bool isNan = false, isInf = false;
      checkNan(v->val(), isNan, isInf);
      if(isNan || isInf) {
        LOG(critical, "Detected NaN ({}) or Inf ({}) in value (forward pass)", isNan, isInf);
        LOG(critical, "\tType: {}, Shape: {}, Name: {}, Id: {}, Hash: {}",
            v->type(), v->shape(), v->name(), v->getId(), v->hash());
        LOG(critical, "Children: {}", v->children().size());
        for(auto&& child : v->children()) {
          LOG(critical, "\tType: {}, Shape: {}, Name: {}, Id: {}, Hash: {}",
            child->type(), child->shape(), child->name(), child->getId(), child->hash());
        }
      }
    }

    if(v->marked_for_debug()) {
      Logger log = spdlog::get("general");
      if(log) {
        LOG(info, "Debug: {} op={}", v->debug_message(), v->type());
        LOG(info, v->val()->debug());
      }
      else {
        std::cerr << "Debug: " << v->debug_message() << " op=" << v->type() << std::endl;
        std::cerr << v->val()->debug() << std::endl;
      }
    }

    // @TODO: Make sure this is optimal
    if(inferenceOnly_)
      v->children().clear();

    if(checkpointing_ && !finalPass) {
      auto subtape = v->getSubtape();
      if(subtape) {
        for(auto& node : *subtape) {
          //std::cerr << "Freeing: " << node->getId() << " " << node->type() << " " << node.useCount() << std::endl;
          node->free();
        }
      }
    }

    forwardTape.pop_front();
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
  if(zero /*&& !isFake()*/)
    params_->set_zero_adjoint();

  for(auto&& v : topNodes_)
    v->init_dependent();

  // named_.clear();
  topNodes_.clear();

  tensors_->clearShorttermMemory();

  bool firstNan = true;
  while(!nodesBackward_.empty()) {
    auto v = nodesBackward_.back();
    nodesBackward_.pop_back();

    for(auto&& child : v->children())
      if(child->trainable() && child->type() != "param")
        child->set_zero_adjoint();

    if(checkpointing_ && v->getSubtape()) {
      //std::cerr << "Back: " << v->getId() << " " << v->type() << " " << v->getSubtape()->size() << std::endl;
      forward(*v->getSubtape(), /*finalPass=*/true);
    }

    if(v->trainable() && v->marked_for_debug()) {
      LOG(info, "Debug Grad: {} op={}", v->debug_message(), v->type());
      LOG(info, v->grad()->debug());
    }

    // if(v->trainable() && clipValue != 0) {
    //   using namespace functional;
    //   if(v->type() != "param") { // parameter gradients get clipped later
    //     float l2norm = L2Norm(v->grad(), allocator());
    //     if(l2norm > clipValue) {
    //       float scale = clipValue / l2norm;
    //       Element(_1 = scale * _1, v->grad()); // rescale gradient norm
    //     }
    //   }
    // }

    if(v->trainable()/* && !isFake()*/)
      v->backward();

    if(throwNan_ && firstNan) {
      for(auto&& child : v->children()) {
        if(child->trainable()) {
          bool isNan = false, isInf = false;
          checkNan(child->grad(), isNan, isInf);
          if(isNan) {
            LOG(critical, "Detected NaN ({}) or Inf ({}) in gradient (backward pass) of child node", isNan, isInf);
            LOG(critical, "Child - Type: {}, Shape: {}, Name: {}, Id: {}, Hash: {}",
                child->type(), child->shape(), child->name(), child->getId(), child->hash());
            LOG(critical, "Parent - Type: {}, Shape: {}, Name: {}, Id: {}, Hash: {}",
                v->type(), v->shape(), v->name(), v->getId(), v->hash());
            firstNan = false;
          }
        }
      }
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
  // sorted by name in std::map
  for(auto p : params()->getMap()) {
    std::string pName = p.first;

    if(!namespace_.empty()) {
      if(pName.substr(0, namespace_.size() + 2) == namespace_ + "::")
        pName = pName.substr(namespace_.size() + 2);
    }

    Tensor val = p.second->val();
    io::Item item;
    val->get(item, pName);
    item.convert(saveType_);
    ioItems.emplace_back(std::move(item));
  }
}

}  // namespace marian
