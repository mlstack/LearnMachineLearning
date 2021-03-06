#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <thread>

#include "mlp.hpp"

namespace {
  namespace mx = mxnet::cpp;
  namespace ch = std::chrono;

  // The names of the model and params files are
  // fixed
  const std::string MODEL_FILENAME {"model.json"};
  const std::string PARAMS_FILENAME {"model-params.json"};

  // Utility function to read an entire file into a string
  static void readall (const std::string& path, std::string *content) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    ifs.seekg(0, std::ios::end);
    size_t length = ifs.tellg();
    content->resize(length);
    ifs.seekg(0, std::ios::beg);
    ifs.read(&content->at(0), content->size());
  }
}

// This constructor initializes the network, and
// leaves it ready for execution
Mlp::Mlp(const MlpParams& params) : params_{params} {
  const std::size_t layers = params.hidden.size();

  // Let's define the network first
  auto data = mx::Symbol::Variable("data");
  auto label = mx::Symbol::Variable("labels");

  // Reserve space for parameters
  weights_.resize(layers);
  biases_.resize(layers);
  outputs_.resize(layers);

  // Build each layer
  for (auto i = 0U; i < layers; i++) {
    const auto& istr = std::to_string(i);
    weights_[i] = mx::Symbol::Variable(std::string("w") + istr);
    biases_[i] = mx::Symbol::Variable(std::string("b") + istr);
    mx::Symbol fc = mx::FullyConnected(std::string("fc") + istr,
				       i == 0 ? data : outputs_[i-1],
				       weights_[i], biases_[i], params.hidden[i]);
    if (i == layers - 1) {
      outputs_[i] = fc;
    } else {
      outputs_[i] = mx::Activation(std::string("act") + istr, fc, mx::ActivationActType::kRelu);
    }
  }

  // Define the outpu
  network_ = mx::SoftmaxOutput("softmax", outputs_[layers - 1], label);

  ////////////////////////////////////////////////////////////////////////////////
  // Now that the network has been built, we need to configure it
  ////////////////////////////////////////////////////////////////////////////////
  auto ctx = mx::Context::cpu();
  args_["data"] = mx::NDArray(mx::Shape(params.batch_size, params.dim), ctx);
  args_["labels"] = mx::NDArray(mx::Shape(params.batch_size), ctx);

  // Let MXNet infer shapes other parameters such as weights
  network_.InferArgsMap(ctx, &args_, args_);

  // Initialize all parameters with uniform distribution U(-0.01, 0.01)
  auto initializer = mx::Uniform(0.01);
  for (auto& arg : args_) {
    // arg.first is parameter name, and arg.second is the value
    initializer(arg.first, &arg.second);
  }

  // Create sgd optimizer
  opt_ = mx::OptimizerRegistry::Find("sgd");
  opt_->SetParam("lr", params.learning_rate)
      ->SetParam("wd", params.weight_decay);

  // Create executor by binding parameters to the model
  exec_ = network_.SimpleBind(ctx, args_);
}

Mlp::Mlp(const std::string& dir) {
  // Load the params
  const std::string params_filepath = dir + "/" + PARAMS_FILENAME;
  args_ = mx::NDArray::LoadToMap(params_filepath);

  // Load the network
  const std::string model_filepath = dir + "/" + MODEL_FILENAME;
  network_ = mx::Symbol::Load(model_filepath);

  // Initialize the executor
  // Create executor by binding parameters to the model
  auto ctx = mx::Context::cpu();
  exec_ = network_.SimpleBind(ctx, args_);
}

void Mlp::fit(const std::string& data_train_csv_path,
	      const std::string& labels_train_csv_path,
	      const std::string& data_test_path,
	      const std::string& labels_test_path) {
  bool test = true;
  if (data_test_path == "" || labels_test_path == "") {
    test = false;
  }

  // Let's load the data
  auto train_iter = mx::MXDataIter("CSVIter")
    .SetParam("data_csv", data_train_csv_path)
    .SetParam("label_csv", labels_train_csv_path)
    .SetParam("data_shape", mx::Shape{params_.dim})
    .SetParam("label_shape", mx::Shape{1})
    .SetParam("batch_size", params_.batch_size)
    .CreateDataIter();

  auto test_iter = mx::MXDataIter("CSVIter");
  if (test) {
    test_iter.SetParam("data_csv", data_test_path)
    .SetParam("label_csv", labels_test_path)
    .SetParam("data_shape", mx::Shape{params_.dim})
    .SetParam("label_shape", mx::Shape{1})
    .SetParam("batch_size", params_.batch_size)
    .CreateDataIter();
  }

  // There is a bug in MxNet prefetcher, sleeping
  // for a little while seems to avoid it
  std::this_thread::sleep_for(ch::milliseconds{100});

  const auto arg_names = network_.ListArguments();
  auto epochs = params_.epochs;
  auto tic = ch::system_clock::now();
  while (epochs > 0) {
    train_iter.Reset();

    while (train_iter.Next()) {
      auto data_batch = train_iter.GetDataBatch();
      // Set data and label
      data_batch.data.CopyTo(&args_["data"]);
      data_batch.label.CopyTo(&args_["labels"]);
      args_["data"].WaitAll();
      args_["labels"].WaitAll();
      // Compute gradients
      exec_->Forward(true);
      exec_->Backward();
      // Update parameters
      for (size_t i = 0; i < arg_names.size(); ++i) {
	if (arg_names[i] == "data" || arg_names[i] == "labels") continue;
	opt_->Update(i, exec_->arg_arrays[i], exec_->grad_arrays[i]);
      }
    }

    --epochs;
    if (!test) {
      continue;
    }

    mx::Accuracy acc;
    test_iter.Reset();
    while (test_iter.Next()) {
      auto data_batch = test_iter.GetDataBatch();
      data_batch.data.CopyTo(&args_["data"]);
      data_batch.label.CopyTo(&args_["labels"]);
      // Forward pass is enough as no gradient is needed when evaluating
      exec_->Forward(false);
      acc.Update(data_batch.label, exec_->outputs[0]);
    }

    std::cout << "Epoch: " << epochs << " " << acc.Get() << std::endl;;
  }

  auto toc = ch::system_clock::now();
  std::cout << ch::duration_cast<ch::milliseconds>(toc - tic).count()
	    << std::endl;
};

std::vector<mx::NDArray> Mlp::predict(const std::string& filepath) {
  // Record start time for reporting purposes
  const auto tic = ch::system_clock::now();

  // Infer feature dim from the arguments
  const std::uint32_t feature_dim = args_["data"].GetShape()[1];

  // Let's load the data. By not defining labels, we are telling MxNet
  // there are no labels
  const mx_uint batch = 16;
  auto train_iter = mx::MXDataIter("CSVIter")
    .SetParam("data_csv", filepath)
    .SetParam("data_shape", mx::Shape{feature_dim})
    .SetParam("batch_size", batch)
    .CreateDataIter();
  train_iter.Reset();

  // Iterate through the batches
  std::vector<mx::NDArray> results;
  const auto ctx = mx::Context::cpu();
  while (train_iter.Next()) {
    // Get batch
    const auto& data_batch = train_iter.GetDataBatch();

    // Set data and label
    data_batch.data.CopyTo(&args_["data"]);
    data_batch.label.CopyTo(&args_["labels"]);
    args_["data"].WaitAll();
    args_["labels"].WaitAll();

    // Compute gradients
    exec_->Forward(true);

    // Gather results
    mx::NDArray res{mx::Shape{exec_->outputs[0].GetShape()}, ctx};
    exec_->outputs[0].CopyTo(&res);
    res.WaitAll();
    results.push_back(res);
  }

  // Report latency and return
  const auto toc = ch::system_clock::now();
  std::cout << ch::duration_cast<ch::milliseconds>(toc - tic).count()
	    << std::endl;
  return results;
}

void Mlp::reset() {};

void Mlp::save_model(const std::string &dir) {
  // First save the model
  const std::string model_filepath = dir + "/" + MODEL_FILENAME;
  network_.Save(model_filepath);

  // Now we need to save the parameters
  const std::string params_filepath = dir + "/" + PARAMS_FILENAME;
  mx::NDArray::Save(params_filepath, args_);
}
