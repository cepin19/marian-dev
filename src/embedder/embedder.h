#pragma once

#include "marian.h"

#include "common/config.h"
#include "common/options.h"
#include "data/batch_generator.h"
#include "data/corpus.h"
#include "data/corpus_nbest.h"
#include "models/costs.h"
#include "models/model_task.h"
#include "embedder/vector_collector.h"
#include "training/scheduler.h"
#include "training/validator.h"

namespace marian {

using namespace data;

/*
 * The tool is used to create output sentence embeddings from available
 * Marian encoders. With --compute-similiarity it can return the cosine
 * similarity between two sentences provided from two sources.
 */
class Embedder {
private:
  Ptr<models::IModel> model_;

public:
  Embedder(Ptr<Options> options)
    : model_(createModelFromOptions(options, models::usage::embedding)) {}

  void load(Ptr<ExpressionGraph> graph, Ptr<io::ModelWeights> modelFile) {
    model_->load(graph, modelFile);
  }

  Expr build(Ptr<ExpressionGraph> graph, Ptr<data::CorpusBatch> batch) {
    auto embedder = std::dynamic_pointer_cast<EncoderPooler>(model_);
    ABORT_IF(!embedder, "Could not cast to EncoderPooler");
    return embedder->apply(graph, batch, /*clearGraph=*/true)[0];
  }
};

/*
 * Actual Embed task. @TODO: this should be simplified in the future.
 */
template <class Model>
class Embed : public ModelTask {
private:
  Ptr<Options> options_;
  Ptr<CorpusBase> corpus_;
  std::vector<Ptr<ExpressionGraph>> graphs_;
  std::vector<Ptr<Model>> models_;
  Ptr<io::ModelWeights> modelFile_;

public:
  Embed(Ptr<Options> options) : options_(options) {

    options_ = options_->with("inference", true,
                              "shuffle", "none");

    // if a similarity is computed then double the input types and vocabs for
    // the two encoders that are used in the model.
    if(options->get<bool>("compute-similarity")) {
      auto vVocabs     = options_->get<std::vector<std::string>>("vocabs");
      auto vDimVocabs  = options_->get<std::vector<size_t>>("dim-vocabs");

      vVocabs.push_back(vVocabs.back());
      vDimVocabs.push_back(vDimVocabs.back());

      options_ = options_->with("vocabs",      vVocabs,
                                "dim-vocabs",  vDimVocabs,
                                "input-types", std::vector<std::string>(vVocabs.size(), "sequence"));
    }

    corpus_ = New<Corpus>(options_);
    corpus_->prepare();

    auto devices = Config::getDevices(options_);

    for(auto device : devices) {
      auto graph = New<ExpressionGraph>(true);

      auto precison = options_->get<std::vector<std::string>>("precision", {"float32"});
      graph->setDefaultElementType(typeFromString(precison[0])); // only use first type, used for parameter type in graph
      graph->setDevice(device);
      graph->reserveWorkspaceMB(options_->get<int>("workspace"));
      graphs_.push_back(graph);
    }

    auto modelPath = options_->get<std::string>("model");
    modelFile_ = New<io::ModelWeights>(modelPath);

    models_.resize(graphs_.size());
    ThreadPool pool(graphs_.size(), graphs_.size());
    for(size_t i = 0; i < graphs_.size(); ++i) {
      pool.enqueue(
          [=](size_t j) {
            models_[j] = New<Model>(options_);
            models_[j]->load(graphs_[j], modelFile_);
          },
          i);
    }
  }

  void run() override {
    LOG(info, "Embedding");
    timer::Timer timer;

    auto batchGenerator = New<BatchGenerator<CorpusBase>>(corpus_, options_);
    batchGenerator->prepare();

    auto output = New<VectorCollector>(options_->get<std::string>("output"), options_->get<bool>("binary"));

    size_t batchId = 0;
    {
      ThreadPool pool(graphs_.size(), graphs_.size());

      for(auto batch : *batchGenerator) {
        auto task = [=](size_t id) {
          thread_local Ptr<ExpressionGraph> graph;
          thread_local Ptr<Model> builder;

          if(!graph) {
            graph = graphs_[id % graphs_.size()];
            builder = models_[id % graphs_.size()];
          }

          auto embeddings = builder->build(graph, batch);
          graph->forward();

          // handle copying from fp32 or fp16 embeddings correctly.
          std::vector<float> sentVectors;
          if(embeddings->value_type() == Type::float32) {
            embeddings->val()->get(sentVectors);
          } else if (embeddings->value_type() == Type::float16) {
            std::vector<float16> sentVectors16;
            embeddings->val()->get(sentVectors16);
            sentVectors.reserve(sentVectors16.size());
            for(auto& v: sentVectors16)
              sentVectors.push_back(v);
          } else {
            ABORT("Unknown embedding type {}", embeddings->value_type());
          }

          // collect embedding vector per sentence.
          // if we compute similarities this is only one similarity per sentence pair.
          for(size_t i = 0; i < batch->size(); ++i) {
              auto embSize = embeddings->shape()[-1];
              auto beg = i * embSize;
              auto end = (i + 1) * embSize;
              std::vector<float> sentVector(sentVectors.begin() + beg, sentVectors.begin() + end);
              output->Write((long)batch->getSentenceIds()[i],
                            sentVector);
          }
        };

        pool.enqueue(task, batchId++);
      }
    }
    LOG(info, "Total time: {:.5f}s wall", timer.elapsed());
  }

};

}  // namespace marian
