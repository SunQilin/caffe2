#include "recurrent_op_cudnn.h"
#include "caffe2/utils/math.h"

namespace caffe2 {

namespace detail {

template <typename T>
TensorDescriptors<T>::TensorDescriptors(
    size_t n,
    const std::vector<int>& dim,
    const std::vector<int>& stride) {
  descs_.resize(n);
  CHECK_EQ(dim.size(), stride.size());
  for (auto i = 0; i < n; ++i) {
    CUDNN_CHECK(cudnnCreateTensorDescriptor(&descs_[i]));
    CUDNN_CHECK(cudnnSetTensorNdDescriptor(
        descs_[i],
        cudnnTypeWrapper<T>::type,
        dim.size(),
        dim.data(),
        stride.data()));
  }
}

template <typename T>
TensorDescriptors<T>::~TensorDescriptors() {
  for (auto desc : descs_) {
    cudnnDestroyTensorDescriptor(desc);
  }
}
}

template <typename T>
RecurrentBaseOp<T>::RecurrentBaseOp(
    const OperatorDef& operator_def,
    Workspace* ws)
    : Operator<CUDAContext>(operator_def, ws), cudnn_wrapper_(&context_) {
  CUDNN_CHECK(cudnnCreateDropoutDescriptor(&dropoutDesc_));
  CUDNN_CHECK(cudnnCreateRNNDescriptor(&rnnDesc_));
  CUDNN_CHECK(cudnnCreateFilterDescriptor(&wDesc_));
  CUDNN_CHECK(cudnnCreateTensorDescriptor(&hxDesc_));
  CUDNN_CHECK(cudnnCreateTensorDescriptor(&cxDesc_));
  CUDNN_CHECK(cudnnCreateTensorDescriptor(&hyDesc_));
  CUDNN_CHECK(cudnnCreateTensorDescriptor(&cyDesc_));
}

template <typename T>
RecurrentBaseOp<T>::~RecurrentBaseOp() {
  CUDNN_CHECK(cudnnDestroyDropoutDescriptor(dropoutDesc_));
  CUDNN_CHECK(cudnnDestroyRNNDescriptor(rnnDesc_));
  CUDNN_CHECK(cudnnDestroyFilterDescriptor(wDesc_));
  CUDNN_CHECK(cudnnDestroyTensorDescriptor(hxDesc_));
  CUDNN_CHECK(cudnnDestroyTensorDescriptor(cxDesc_));
  CUDNN_CHECK(cudnnDestroyTensorDescriptor(hyDesc_));
  CUDNN_CHECK(cudnnDestroyTensorDescriptor(cyDesc_));
}

template <typename T>
void RecurrentBaseOp<T>::initialize(
    const Tensor<CUDAContext>& input,
    Tensor<CUDAContext>* dropoutStates,
    Tensor<CUDAContext>* output,
    Tensor<CUDAContext>* hiddenOutput,
    Tensor<CUDAContext>* cellOutput) {
  static_assert(sizeof(T) == 4, ""); // workaround clang bug
  CHECK_GE(input.ndim(), 3);
  const int seqLength = input.dim(0);
  const int batchSize = input.dim(1);
  const int inputDim = input.dim(2);
  const int hiddenSize = OperatorBase::GetSingleArgument<int>("hidden_size", 0);
  CHECK_GT(hiddenSize, 0);
  const auto bidirectional =
      OperatorBase::GetSingleArgument<int>("bidirectional", 0);
  CHECK(bidirectional == 0 || bidirectional == 1);
  const auto numDirections = bidirectional == 1 ? 2 : 1;
  const auto outputDim = hiddenSize * numDirections;
  const auto rnnDirection =
      bidirectional == 1 ? CUDNN_BIDIRECTIONAL : CUDNN_UNIDIRECTIONAL;
  const auto numLayers = OperatorBase::GetSingleArgument<int>("num_layers", 0);
  CHECK_GT(numLayers, 0);
  const auto& rnnModeStr =
      OperatorBase::GetSingleArgument<string>("rnn_mode", "");
  CHECK(rnnModeStr == "lstm" || rnnModeStr == "gru");
  const auto rnnMode = rnnModeStr == "lstm" ? CUDNN_LSTM : CUDNN_GRU;
  const auto& rnnInputStr =
      OperatorBase::GetSingleArgument<string>("input_mode", "");
  CHECK(rnnInputStr == "linear" || rnnInputStr == "skip");
  const auto rnnInput =
      rnnInputStr == "linear" ? CUDNN_LINEAR_INPUT : CUDNN_SKIP_INPUT;
  // Dropout setup
  {
    size_t stateSize;
    CUDNN_CHECK(cudnnDropoutGetStatesSize(
        cudnn_wrapper_.inline_cudnn_handle(), &stateSize));
    dropoutStates->Resize(std::vector<int>{static_cast<int>(
        stateSize / 4 /* sizeof(T) - workaround clang bug */)});
    CUDNN_CHECK(cudnnSetDropoutDescriptor(
        dropoutDesc_,
        cudnn_wrapper_.inline_cudnn_handle(),
        OperatorBase::GetSingleArgument<float>("dropout", 0.0),
        dropoutStates->template mutable_data<T>(),
        stateSize,
        OperatorBase::GetSingleArgument<int>("seed", 0)));
  }

  // RNN setup
  {
    CUDNN_CHECK(cudnnSetRNNDescriptor(
        rnnDesc_,
        hiddenSize,
        numLayers,
        dropoutDesc_,
        rnnInput,
        rnnDirection,
        rnnMode,
        cudnnTypeWrapper<T>::type));
  }
  // X setup
  {
    xDesc_.reset(new detail::TensorDescriptors<T>(
        seqLength,
        {inputDim, batchSize, 1},
        {1, inputDim, inputDim * batchSize}));
  }
  // Y setup
  {
    yDesc_.reset(new detail::TensorDescriptors<T>(
        seqLength,
        {outputDim, batchSize, 1},
        {1, outputDim, outputDim * batchSize}));

    if (output) {
      output->Resize(std::vector<int>{seqLength, batchSize, outputDim});
    }
  }

  // Hidden/Cell setup
  {
    const std::array<int, 3> dim{
        hiddenSize, batchSize, numLayers * numDirections};
    const std::array<int, 3> stride{1, hiddenSize, hiddenSize * batchSize};
    CUDNN_CHECK(cudnnSetTensorNdDescriptor(
        hxDesc_, cudnnTypeWrapper<T>::type, 3, dim.data(), stride.data()));
    CUDNN_CHECK(cudnnSetTensorNdDescriptor(
        cxDesc_, cudnnTypeWrapper<T>::type, 3, dim.data(), stride.data()));
    CUDNN_CHECK(cudnnSetTensorNdDescriptor(
        hyDesc_, cudnnTypeWrapper<T>::type, 3, dim.data(), stride.data()));
    CUDNN_CHECK(cudnnSetTensorNdDescriptor(
        cyDesc_, cudnnTypeWrapper<T>::type, 3, dim.data(), stride.data()));

    if (hiddenOutput) {
      hiddenOutput->Resize(
          std::vector<int>{numLayers * numDirections, batchSize, hiddenSize});
    }
    if (cellOutput) {
      cellOutput->Resize(
          std::vector<int>{numLayers * numDirections, batchSize, hiddenSize});
    }
  }

  // Weights setup
  {
    size_t weightsSize;
    CUDNN_CHECK(cudnnGetRNNParamsSize(
        cudnn_wrapper_.inline_cudnn_handle(),
        rnnDesc_,
        xDesc_->descs()[0],
        &weightsSize,
        cudnnTypeWrapper<T>::type));
    const std::array<int, 3> dims{
        static_cast<int>(
            weightsSize / 4 /* sizeof(T) - workaround clang bug */),
        1,
        1};
    CUDNN_CHECK(cudnnSetFilterNdDescriptor(
        wDesc_, cudnnTypeWrapper<T>::type, CUDNN_TENSOR_NCHW, 3, dims.data()));
  }

  // RNN workspace size
  {
    CUDNN_CHECK(cudnnGetRNNWorkspaceSize(
        cudnn_wrapper_.inline_cudnn_handle(),
        rnnDesc_,
        seqLength,
        xDesc_->descs(),
        &cudnnWsNbytes_));
  }
}

template <typename T>
bool RecurrentOp<T>::RunOnDevice() {
  const int seqLength = Input(INPUT).dim32(0);
  if (Input(INPUT).dims() != cachedInputDims_) {
    initialize(
        Input(INPUT),
        Output(DROPOUT_STATES),
        Output(OUTPUT),
        Output(HIDDEN_OUTPUT),
        Output(CELL_OUTPUT));
    cachedInputDims_ = Input(INPUT).dims();
  }

  // Validation checks
  size_t weightsSize;
  CUDNN_CHECK(cudnnGetRNNParamsSize(
      cudnn_wrapper_.inline_cudnn_handle(),
      rnnDesc_,
      xDesc_->descs()[0],
      &weightsSize,
      cudnnTypeWrapper<T>::type));
  CHECK_EQ(Input(WEIGHT).nbytes(), weightsSize);

  // Training reserve size
  CUDNN_CHECK(cudnnGetRNNTrainingReserveSize(
      cudnn_wrapper_.inline_cudnn_handle(),
      rnnDesc_,
      seqLength,
      xDesc_->descs(),
      &reserveNbytes_));
  Output(RNN_SCRATCH)
      ->Resize(std::vector<int>{static_cast<int>(
          reserveNbytes_ / 4 /* sizeof(T) - workaround clang bug */)});
  Output(RNN_SCRATCH)->template mutable_data<T>();

  if (OperatorBase::GetSingleArgument<int>("is_test", 0)) {
    cudnn_wrapper_.with_cudnn_state(0, [&](CuDNNState* state) {
      CUDNN_CHECK(cudnnRNNForwardInference(
          state->cudnn_handle(),
          rnnDesc_,
          seqLength,
          xDesc_->descs(),
          Input(INPUT).template data<T>(),
          hxDesc_,
          Input(HIDDEN_INPUT).template data<T>(),
          cxDesc_,
          Input(CELL_INPUT).template data<T>(),
          wDesc_,
          Input(WEIGHT).template data<T>(),
          yDesc_->descs(),
          Output(OUTPUT)->template mutable_data<T>(),
          hyDesc_,
          Output(HIDDEN_OUTPUT)->template mutable_data<T>(),
          cyDesc_,
          Output(CELL_OUTPUT)->template mutable_data<T>(),
          state->workspace().get(cudnnWsNbytes_),
          cudnnWsNbytes_));
    });
  } else {
    cudnn_wrapper_.with_cudnn_state(0, [&](CuDNNState* state) {
      CUDNN_CHECK(cudnnRNNForwardTraining(
          state->cudnn_handle(),
          rnnDesc_,
          seqLength,
          xDesc_->descs(),
          Input(INPUT).template data<T>(),
          hxDesc_,
          Input(HIDDEN_INPUT).template data<T>(),
          cxDesc_,
          Input(CELL_INPUT).template data<T>(),
          wDesc_,
          Input(WEIGHT).template data<T>(),
          yDesc_->descs(),
          Output(OUTPUT)->template mutable_data<T>(),
          hyDesc_,
          Output(HIDDEN_OUTPUT)->template mutable_data<T>(),
          cyDesc_,
          Output(CELL_OUTPUT)->template mutable_data<T>(),
          state->workspace().get(cudnnWsNbytes_),
          cudnnWsNbytes_,
          Output(RNN_SCRATCH)->template mutable_data<T>(),
          reserveNbytes_));
    });
  }

  return true;
}

template <typename T>
bool RecurrentGradientOp<T>::RunOnDevice() {
  const int seqLength = Input(INPUT).dim32(0);
  if (Input(INPUT).dims() != cachedInputDims_) {
    initialize(Input(INPUT), Output(DROPOUT_STATES));
    cachedInputDims_ = Input(INPUT).dims();
  }
  CUDNN_CHECK(cudnnGetRNNTrainingReserveSize(
      cudnn_wrapper_.inline_cudnn_handle(),
      rnnDesc_,
      seqLength,
      xDesc_->descs(),
      &reserveNbytes_));
  CHECK_EQ(reserveNbytes_, Input(RNN_SCRATCH).nbytes());
  Output(GRAD_INPUT)->ResizeLike(Input(INPUT));
  Output(GRAD_HIDDEN_INPUT)->ResizeLike(Input(HIDDEN_INPUT));
  Output(GRAD_CELL_INPUT)->ResizeLike(Input(CELL_INPUT));

  Output(GRAD_WEIGHT)->ResizeLike(Input(WEIGHT));
  math::Set<T, CUDAContext>(
      Output(GRAD_WEIGHT)->size(),
      0.0,
      Output(GRAD_WEIGHT)->template mutable_data<T>(),
      &context_);

  cudnn_wrapper_.with_cudnn_state(0, [&](CuDNNState* state) {
    CUDNN_CHECK(cudnnRNNBackwardData(
        state->cudnn_handle(),
        rnnDesc_,
        seqLength,
        yDesc_->descs(),
        Input(OUTPUT).template data<T>(),
        yDesc_->descs(),
        Input(GRAD_OUTPUT).template data<T>(),
        hyDesc_,
        Input(GRAD_HIDDEN_OUTPUT).template data<T>(),
        cyDesc_,
        Input(GRAD_CELL_OUTPUT).template data<T>(),
        wDesc_,
        Input(WEIGHT).template data<T>(),
        hxDesc_,
        Input(HIDDEN_INPUT).template data<T>(),
        cxDesc_,
        Input(CELL_INPUT).template data<T>(),
        xDesc_->descs(),
        Output(GRAD_INPUT)->template mutable_data<T>(),
        hxDesc_,
        Output(GRAD_HIDDEN_INPUT)->template mutable_data<T>(),
        cxDesc_,
        Output(GRAD_CELL_INPUT)->template mutable_data<T>(),
        state->workspace().get(cudnnWsNbytes_),
        cudnnWsNbytes_,
        Input(RNN_SCRATCH).template data<T>(),
        reserveNbytes_));
    CUDNN_CHECK(cudnnRNNBackwardWeights(
        state->cudnn_handle(),
        rnnDesc_,
        seqLength,
        xDesc_->descs(),
        Input(INPUT).template data<T>(),
        hxDesc_,
        Input(HIDDEN_INPUT).template data<T>(),
        yDesc_->descs(),
        Input(OUTPUT).template data<T>(),
        state->workspace().get(cudnnWsNbytes_),
        cudnnWsNbytes_,
        wDesc_,
        Output(GRAD_WEIGHT)->template mutable_data<T>(),
        Input(RNN_SCRATCH).template data<T>(),
        reserveNbytes_));
  });
  return true;
}

template <typename T>
bool RecurrentInitOp<T>::RunOnDevice() {
  initialize(Input(INPUT), Output(DROPOUT_STATES));
  size_t weightsSize;
  CUDNN_CHECK(cudnnGetRNNParamsSize(
      cudnn_wrapper_.inline_cudnn_handle(),
      rnnDesc_,
      xDesc_->descs()[0],
      &weightsSize,
      cudnnTypeWrapper<T>::type));
  Output(WEIGHT)->Resize(std::vector<int>{(static_cast<int>(
      weightsSize / 4 /* sizeof(T) - workaround clang bug */))});
  math::RandUniform<T, CUDAContext>(
      Output(WEIGHT)->size(),
      -OperatorBase::GetSingleArgument<float>("scale", 0.01),
      OperatorBase::GetSingleArgument<float>("scale", 0.01),
      Output(WEIGHT)->template mutable_data<T>(),
      &context_);

  if (OperatorBase::GetSingleArgument<string>("rnn_mode", "lstm") != "lstm") {
    return true;
  }

  // For LSTMs, initialize the forget gates to have a bias of 1.0
  for (auto i = 0; i < OperatorBase::GetSingleArgument<int>("num_layers", 0);
       ++i) {
    cudnnFilterDescriptor_t biasDesc;
    CUDNN_CHECK(cudnnCreateFilterDescriptor(&biasDesc));
    void* bias;
    CUDNN_CHECK(cudnnGetRNNLinLayerBiasParams(
        cudnn_wrapper_.inline_cudnn_handle(),
        rnnDesc_,
        i,
        xDesc_->descs()[0],
        wDesc_,
        Output(WEIGHT)->template data<T>(),
        5, // Forget gate bias for recurrent input
        biasDesc,
        &bias));
    int numBiasDims;
    std::array<int, 3> biasDims;
    cudnnDataType_t dt;
    cudnnTensorFormat_t tf;
    // For some reason, the CuDNN Bias tensor is 3 dimensional
    CUDNN_CHECK(cudnnGetFilterNdDescriptor(
        biasDesc, 3, &dt, &tf, &numBiasDims, biasDims.data()));
    CHECK_EQ(numBiasDims, 3);
    math::Set<T, CUDAContext>(
        biasDims[0] * biasDims[1] * biasDims[2],
        1.0,
        static_cast<T*>(bias),
        &context_);
  }
  return true;
}

REGISTER_CUDNN_OPERATOR(Recurrent, RecurrentOp<float>);
OPERATOR_SCHEMA(Recurrent).NumInputs(4).NumOutputs(5).SetDoc(R"DOC(

Recurrent wraps the CuDNN R5 RNN implementation. See the CuDNN R5
documentation for more information.

In general, the implementation takes an input (TxNxD) tensor, the
hidden state input (NxD), the cell input (NxD), and a weight tensor
(effectively an opaque blob, where the size and layout is dictated by
CuDNN).

The outputs are the output (again, TxNxD), the final hidden/cell
states (NxD). These can be reset (at sequence boundaries across
minibatches) by multiplying by zero.

The CuDNN arguments (hidden_size, bidirectional, num_layers, rnn_mode,
input_mode) are passed directly through to CuDNN.

)DOC");
REGISTER_CUDNN_OPERATOR(RecurrentGradient, RecurrentGradientOp<float>);
OPERATOR_SCHEMA(RecurrentGradient).NumInputs(9).NumOutputs(5);
REGISTER_CUDNN_OPERATOR(RecurrentInit, RecurrentInitOp<float>);
OPERATOR_SCHEMA(RecurrentInit).NumInputs(1).NumOutputs(2);

struct GetRecurrentGradient : public GradientMakerBase {
  using GradientMakerBase::GradientMakerBase;
  vector<OperatorDef> GetGradientDefs() override {
    return SingleGradientDef(
        "RecurrentGradient",
        "",
        vector<string>{I(0), // INPUT
                       I(1), // HIDDEN_INPUT
                       I(2), // CELL_INPUT
                       I(3), // WEIGHT
                       O(3), // RNN_SCRATCH
                       O(0), // OUTPUT
                       GO(0), // GRAD_OUTPUT
                       GO(1), // GRAD_HIDDEN_OUTPUT
                       GO(2)}, // GRAD_CELL_OUTPUT
        vector<string>{
            GI(0), // GRAD_INPUT
            GI(1), // GRAD_HIDDEN_INPUT
            GI(2), // GRAD_CELL_INPUT
            GI(3), // GRAD_WEIGHT
            O(4) // DROPOUT_STATES
        });
  }
};
REGISTER_GRADIENT(Recurrent, GetRecurrentGradient);
}
