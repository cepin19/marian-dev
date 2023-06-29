#pragma once

#include "layers_new/interface.h"
#include "graph/node_initializers.h"

namespace marian {
namespace nn {

static inline Expr swapTimeBatch(Expr input) { return swapAxes(atleast_4d(input), -2, -3); }

  // @TODO: this is an odd function to be here, this should rather be handled somewhere globally?
  // convert multiplicative 1/0 mask to additive 0/-inf log mask, and transpose to match result of bdot() op in Attention()
static inline Expr transposedLogMask(Expr mask, int dimHeads) {
  if(!mask)
    return nullptr;

  // LayerAttention expects mask in a different layout
  int dimBatch    = mask->shape()[-3];
  int dimSrcWords = mask->shape()[-2];
  mask = reshape(mask, {dimBatch, 1, 1, dimSrcWords}); // [batch size, num heads broadcast=1, max length broadcast=1, max length]

  float maskFactor = std::max(NumericLimits<float>(mask->value_type()).lowest / 2.f, -99999999.f); // to make sure we do not overflow for fp16
  auto logMask = (1 - mask) * maskFactor;
  logMask      = reshape(repeat(logMask, dimHeads, -3), {1, dimBatch * dimHeads, 1, dimSrcWords});
  return logMask;
}

/**
 * A generic Activation function layer. Any unary Marian operator or function accepted by 
 * `std::function<Expr(Expr)>` can be turned into an activation function like this: 
 ```
 auto reluLayer = New<Activation>(graph, (Expr(*)(Expr))relu)
 ```
 * The function pointer cast may be required to disambiguate the operator name if operators 
 * of the same name but with a different sets of parameters exist, otherwise it can be dropped 
 * or replaced with a more readable lambda function.
 * 
 * `Activation` will also accept lambdas for more complex activations:
 ```
 // a reasonably accurate approximation of GELU
 auto geluApprox = New<Activation>(graph, [](Expr x) { return x * sigmoid(1.702f * x); });
 ```
 */
class Activation : public Layer, public IUnaryLayer {
private:
  std::function<Expr(Expr)> actFn;

public:
  Activation(Ptr<ExpressionGraph> graph,
             const std::function<Expr(Expr)>& actFn) 
    : Layer(graph), actFn(actFn) {}

  virtual ~Activation() = default;
  
  Expr apply(Expr x) const override {
    return actFn(x);
  }
};

// A ReLU activation function layer defined via `Activation`.
struct ReLU final : public Activation {
  ReLU(Ptr<ExpressionGraph> graph)    : Activation(graph, (Expr(*)(Expr))relu) {}
};

// A GELU activation function layer defined via `Activation`.
struct GELU final : public Activation {
  GELU(Ptr<ExpressionGraph> graph)    : Activation(graph, (Expr(*)(Expr))gelu) {}
};

// A Tanh activation function layer defined via `Activation`.
struct Tanh final : public Activation {
  Tanh(Ptr<ExpressionGraph> graph)    : Activation(graph, (Expr(*)(Expr))tanh) {}
};

// A Sigmoid activation function layer defined via `Activation`.
struct Sigmoid final : public Activation {
  Sigmoid(Ptr<ExpressionGraph> graph) : Activation(graph, (Expr(*)(Expr))sigmoid) {}
};

// A Swish activation function layer defined via `Activation`.
struct Swish final : public Activation {
  Swish(Ptr<ExpressionGraph> graph)   : Activation(graph, (Expr(*)(Expr))swish) {}
};

// Factory for activation function layers from name as string.
Ptr<Activation> activationLayerByName(Ptr<ExpressionGraph> graph, const std::string& actName);

// Applies a linear transformation to the incoming data: y = xA^T + b 
struct Linear : public Layer, public IUnaryLayer {
  Expr weight;
  Expr bias;

  int dimOut;
  bool useBias{true};
  bool transposed{false};
  Ptr<inits::NodeInitializer> init;

  // Typical constructor that can take an initializer function
  Linear(Ptr<ExpressionGraph> graph, 
         int dimOut,
         bool useBias = true,
         bool transposed = false,
         Ptr<inits::NodeInitializer> init = inits::glorotUniform())
    : Layer(graph), dimOut(dimOut), useBias(useBias), init(init)
  {}

  // Alternate constructor which takes a weight parameter that will be re-used, e.g. for tied output weights.
  // Since the weights are already initialized there is no initializer. Output dimension is initialized from
  // the given weight parameter.
  Linear(Ptr<ExpressionGraph> graph,
         Expr tiedWeight,
         bool useBias = true,
         bool transposed = false)
    : Layer(graph), weight(tiedWeight), dimOut(weight->shape()[-1]), useBias(useBias), init(nullptr)
  {}

  virtual ~Linear() = default;

  Expr apply(Expr x) const override {
    int dimIn = x->shape()[-1];

    // if weight is already initialized nothing happens here
    if(transposed) {
      registerParameterLazy(weight, Shape({ dimOut, dimIn }), init);
    } else {
      registerParameterLazy(weight, Shape({ dimIn, dimOut }), init);
    }
    
    if(useBias) {
      registerParameterLazy(bias, Shape({ dimOut }), inits::zeros());
    }

    Type outputType = x->value_type();
    if(useBias)
      return marian::affine(x, 
                            marian::cast(weight, outputType), 
                            marian::cast(bias, outputType), 
                            /*transA=*/false, 
                            /*transB=*/transposed);
    else
      return marian::dot(x, 
                         marian::cast(weight, outputType), 
                         /*transA=*/false, 
                         /*transB=*/transposed);
  }
};

struct Dropout final : public Layer, public IUnaryLayer {
  float dropoutProbabilty;
  UPtr<Shape> dropoutMaskShape;
  
  Dropout(Ptr<ExpressionGraph> graph, 
          float dropoutProbabilty,
          const Shape& dropoutMaskShape) 
    : Layer(graph), dropoutProbabilty(dropoutProbabilty), dropoutMaskShape(new Shape(dropoutMaskShape))
  {}

  Dropout(Ptr<ExpressionGraph> graph, 
          float dropoutProbabilty) 
    : Layer(graph), dropoutProbabilty(dropoutProbabilty), dropoutMaskShape(nullptr)
  {}

  Expr apply(Expr input) const override {
    if(getMode() == Mode::eval)
      return input;

    if(dropoutMaskShape && dropoutProbabilty > 0.f) {
      return marian::dropout(input, dropoutProbabilty, *dropoutMaskShape);
    } else if(dropoutProbabilty > 0.f) {
      return marian::dropout(input, dropoutProbabilty, {input->shape()[-2], input->shape()[-1]});
    } else {
      return input;
    }
  }

  virtual void clear() override {}
};

struct LinearReluDropout final : public Linear {
  using Linear::weight;
  using Linear::bias;

  using Linear::dimOut;
  using Linear::useBias;
  using Linear::transposed;
  using Linear::init;

  float dropoutProbabilty;
  UPtr<Shape> dropoutMaskShape;

  // Typical constructor that can take an initializer function
  LinearReluDropout(Ptr<ExpressionGraph> graph, 
                    int dimOut,
                    float dropoutProbabilty,
                    bool useBias = true,
                    bool transposed = false,
                    Ptr<inits::NodeInitializer> init = inits::glorotUniform())
    : Linear(graph, dimOut, useBias, transposed, init),  
      dropoutProbabilty(dropoutProbabilty), 
      dropoutMaskShape(nullptr) {}

  LinearReluDropout(Ptr<ExpressionGraph> graph, 
                    int dimOut,
                    float dropoutProbabilty,
                    const Shape& dropoutMaskShape,
                    bool useBias = true,
                    bool transposed = false,
                    Ptr<inits::NodeInitializer> init = inits::glorotUniform())
    : Linear(graph, dimOut, useBias, transposed, init),  
      dropoutProbabilty(dropoutProbabilty), 
      dropoutMaskShape(new Shape(dropoutMaskShape)) {}

  Expr apply(Expr x) const override {
    int dimIn = x->shape()[-1];

    // if weight is already initialized nothing happens here
    if(transposed) {
      registerParameterLazy(weight, Shape({ dimOut, dimIn }), init);
    } else {
      registerParameterLazy(weight, Shape({ dimIn, dimOut }), init);
    }
    
    if(useBias) {
      registerParameterLazy(bias, Shape({ dimOut }), inits::zeros());
    }

    // @TODO: handle relu inplace for inference etc.
    Expr output;
    if(useBias)
      output = marian::affine(x, weight, bias, /*transA=*/false, /*transB=*/transposed);
    else
      output = marian::dot(x, weight, /*transA=*/false, /*transB=*/transposed);

    if(getMode() == Mode::eval)
      return relu(output);

    if(dropoutMaskShape && dropoutProbabilty > 0.f) {
      return marian::dropoutReluInplace(output, dropoutProbabilty, *dropoutMaskShape);
    } else if(dropoutProbabilty > 0.f) {
      return marian::dropoutReluInplace(output, dropoutProbabilty, {output->shape()[-2], output->shape()[-1]});
    } else {
      return relu(output);
    }
  }

  virtual void clear() override {}
};


struct Norm : public Layer, public IUnaryLayer {
  Norm(Ptr<ExpressionGraph> graph) : Layer(graph) {}
  virtual ~Norm() = default;

  Expr apply(Expr x) const override = 0;
};

struct LayerNorm final : public Norm {
  Expr weight;
  Expr bias;

  float eps{1e-5f};
  bool elementwiseAffine{true};

  LayerNorm(Ptr<ExpressionGraph> graph, 
            float eps = 1e-5f,
            bool elementwiseAffine = true)
   : Norm(graph), eps(eps), elementwiseAffine(elementwiseAffine) 
  {}

  Expr apply(Expr x) const override {
    int dimModel = x->shape()[-1];
    if(elementwiseAffine) {
      registerParameterLazy(weight, Shape({ dimModel }), inits::ones());
      registerParameterLazy(bias,   Shape({ dimModel }), inits::zeros());
      return marian::layerNorm(x, weight, bias, eps);
    } else {
      return marian::layerNorm(x, nullptr, nullptr, eps);
    }
  }

  virtual void clear() override {}
};

struct RMSNorm final : public Norm {
  Expr weight;

  float eps{1e-5f};
  bool elementwiseAffine{true};

  RMSNorm(Ptr<ExpressionGraph> graph, 
          float eps = 1e-5f,
          bool elementwiseAffine = true)
   : Norm(graph), eps(eps), elementwiseAffine(elementwiseAffine) 
  {}

  Expr apply(Expr x) const override {
    int dimModel = x->shape()[-1];
    if(elementwiseAffine) {
      registerParameterLazy(weight, Shape({ dimModel }), inits::ones());
      return marian::rmsNorm(x, weight, nullptr, eps);
    } else {
      return marian::rmsNorm(x, nullptr, nullptr, eps);
    }
  }
};

} // namespace nn
} // namespace marian
