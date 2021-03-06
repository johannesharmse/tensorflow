/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// See docs in ../ops/nn_ops.cc. This opkernel uses MKL library, create MKL
// layout and primitives, use MKL dnn primitives to compute convolution backward
// input

#ifdef INTEL_MKL

#define USE_EIGEN_TENSOR
#define EIGEN_USE_THREADS
#include <algorithm>
#include <vector>
#ifdef INTEL_MKL_ML
#include "mkl_dnn.h"
#include "mkl_dnn_types.h"
#endif
#include "tensorflow/core/framework/numeric_op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_slice.h"
#include "tensorflow/core/kernels/conv_grad_ops.h"
#include "tensorflow/core/kernels/mkl_conv_ops.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/util/mkl_util.h"
#include "tensorflow/core/util/padding.h"
#include "tensorflow/core/util/tensor_format.h"
#include "tensorflow/core/util/use_cudnn.h"
#include "tensorflow/core/util/work_sharder.h"

#ifndef INTEL_MKL_ML
#include "mkldnn.hpp"

using mkldnn::convolution_backward_data;
using mkldnn::prop_kind;
using mkldnn::stream;
#endif

namespace tensorflow {
typedef Eigen::ThreadPoolDevice CPUDevice;

#ifndef INTEL_MKL_ML

/// utility classes enabling primitive reuse for backward conv2d ops.
struct MklConvBwdInputParams {
  memory::dims diff_src_dims;
  memory::dims filter_dims;
  memory::dims diff_dst_dims;
  memory::dims strides;
  memory::dims dilations;
  memory::dims padding_left;
  memory::dims padding_right;
  padding_kind padding;

  MklConvBwdInputParams(memory::dims diff_src_dims,
    memory::dims filter_dims, memory::dims diff_dst_dims,
    memory::dims strides, memory::dims dilations,
    memory::dims padding_left, memory::dims padding_right,
    padding_kind padding) :
      diff_src_dims(diff_src_dims), filter_dims(filter_dims),
      diff_dst_dims(diff_dst_dims), strides(strides),
      dilations(dilations), padding_left(padding_left),
      padding_right(padding_right), padding(padding) {
  }
};

template <typename T>
class MklConv2DBwdInputPrimitive : public MklPrimitive {
 public:
  explicit MklConv2DBwdInputPrimitive(
      const MklConvBwdInputParams& convBwdInputDims) :
           cpu_engine_(engine::cpu, 0) {
    context_.bwd_input_stream.reset(new stream(stream::kind::eager));

    // create conv primitive
    if (context_.conv_bwd_input == nullptr) {
      Setup(convBwdInputDims);
    }
  }
  ~MklConv2DBwdInputPrimitive() {}

  // Convolution backward filter (weights)
  //   diff_src_data: output data buffer of diff_src
  //   filter_data:   input data buffer of filter (weights)
  //   diff_dst_data: input data buffer of dst
  // Bias does not matter here
  void Execute(const T* diff_src_data,
      const T* filter_data, const T* diff_dst_data) {
    context_.diff_src_mem->set_data_handle(
        static_cast<T*>(const_cast<T*>(diff_src_data)));
    context_.filter_mem->set_data_handle(
        static_cast<T*>(const_cast<T*>(filter_data)));
    context_.diff_dst_mem->set_data_handle(
        static_cast<T*>(const_cast<T*>(diff_dst_data)));

    context_.bwd_input_stream->submit(context_.bwd_input_primitives);

    // set back data handle
    context_.diff_src_mem->set_data_handle(DummyData);
    context_.filter_mem->set_data_handle(DummyData);
    context_.diff_dst_mem->set_data_handle(DummyData);
    return;
  }

  memory::format GetFilterMemoryFormat() const {
    return context_.filter_fmt;
  }

  memory::format GetDiffDstMemoryFormat() const {
    return context_.diff_dst_fmt;
  }

  std::shared_ptr<mkldnn::convolution_backward_data::primitive_desc>
  GetPrimitiveDesc() const {
    return context_.bwd_input_pd;
  }

 private:
  // Primitive reuse context for Conv2D Bwd Input op
  struct ConvBwdInputContext {
    // expected memory format for this primitive instance
    memory::format filter_fmt;
    memory::format diff_dst_fmt;

    // MKLDNN memory
    std::shared_ptr<mkldnn::memory> diff_src_mem;
    std::shared_ptr<mkldnn::memory> filter_mem;
    std::shared_ptr<mkldnn::memory> diff_dst_mem;

    // convolution primitive
    std::shared_ptr<mkldnn::convolution_backward_data::primitive_desc>
        bwd_input_pd;
    std::shared_ptr<mkldnn::primitive> conv_bwd_input;

    // desc & prmitive desc
    std::shared_ptr<mkldnn::convolution_backward_data::desc> bwd_input_desc;
    std::shared_ptr<mkldnn::convolution_forward::desc> fwd_desc;
    std::shared_ptr<mkldnn::convolution_forward::primitive_desc> fwd_pd;

    // memory desc: forward & backward can share same memory::desc
    std::shared_ptr<memory::desc> diff_src_md;
    std::shared_ptr<memory::desc> filter_md;
    std::shared_ptr<memory::desc> diff_dst_md;

    // MKL pipeline
    std::shared_ptr<mkldnn::stream> bwd_input_stream;
    std::vector<mkldnn::primitive> bwd_input_primitives;

    ConvBwdInputContext() :
        filter_fmt(memory::format::any), diff_dst_fmt(memory::format::any),
        diff_src_mem(nullptr), filter_mem(nullptr), diff_dst_mem(nullptr),
        bwd_input_pd(nullptr), conv_bwd_input(nullptr),
        bwd_input_desc(nullptr), fwd_desc(nullptr), fwd_pd(nullptr),
        diff_src_md(nullptr), filter_md(nullptr), diff_dst_md(nullptr),
        bwd_input_stream(nullptr) {
    }
  };


  void Setup(const MklConvBwdInputParams& convBwdInputDims) {
    // create memory descriptors for convolution data w/ no specified format
    context_.diff_src_md.reset(new memory::desc(
        {convBwdInputDims.diff_src_dims},
        MklDnnType<T>(), memory::format::any));
    context_.filter_md.reset(new memory::desc(
        {convBwdInputDims.filter_dims},
        MklDnnType<T>(), memory::format::any));
    context_.diff_dst_md.reset(new memory::desc(
        {convBwdInputDims.diff_dst_dims},
        MklDnnType<T>(), memory::format::any));

    // create convolution primitives
    context_.bwd_input_desc.reset(new convolution_backward_data::desc(
        convolution_direct, *context_.diff_src_md, *context_.filter_md,
        *context_.diff_dst_md, convBwdInputDims.strides,
        convBwdInputDims.dilations, convBwdInputDims.padding_left,
        convBwdInputDims.padding_right, convBwdInputDims.padding));

    context_.fwd_desc.reset(new convolution_forward::desc(prop_kind::forward,
        convolution_direct, *context_.diff_src_md, *context_.filter_md,
        *context_.diff_dst_md, convBwdInputDims.strides,
        convBwdInputDims.dilations, convBwdInputDims.padding_left,
        convBwdInputDims.padding_right, convBwdInputDims.padding));

    context_.fwd_pd.reset(new convolution_forward::primitive_desc(
        *context_.fwd_desc, cpu_engine_));

    // create backward conv prim desc
    context_.bwd_input_pd.reset(
        new convolution_backward_data::primitive_desc(
        *context_.bwd_input_desc, cpu_engine_, *context_.fwd_pd));

    // create memory primitive based on dummy data
    context_.diff_src_mem.reset(new memory(
        context_.bwd_input_pd.get()->diff_src_primitive_desc(), DummyData));
    context_.filter_mem.reset(new memory(
        context_.bwd_input_pd.get()->weights_primitive_desc(), DummyData));
    context_.diff_dst_mem.reset(new memory(
        context_.bwd_input_pd.get()->diff_dst_primitive_desc(), DummyData));

    // store the expected memory format
    context_.filter_fmt = static_cast<memory::format>(
     context_.bwd_input_pd.get()->weights_primitive_desc().desc().data.format);
    context_.diff_dst_fmt = static_cast<memory::format>(
     context_.bwd_input_pd.get()->diff_dst_primitive_desc().desc().data.format);

    // create convolution primitive and add it to net
    context_.conv_bwd_input.reset(new convolution_backward_data(
        *context_.bwd_input_pd, *context_.diff_dst_mem,
        *context_.filter_mem, *context_.diff_src_mem));

    context_.bwd_input_primitives.push_back(*context_.conv_bwd_input);
  }

  struct ConvBwdInputContext context_;
  engine cpu_engine_;
};

template <typename T>
class MklConv2DBwdInputPrimitiveFactory : public MklPrimitiveFactory<T> {
 private:
  MklConv2DBwdInputPrimitiveFactory() {}
  ~MklConv2DBwdInputPrimitiveFactory() {}

 public:
  static MklConv2DBwdInputPrimitive<T>* Get(
      const MklConvBwdInputParams& convBwdInputDims) {
    MklConv2DBwdInputPrimitive<T>* conv2d_bwd_input = nullptr;

    // look into the pool for reusable primitive
    conv2d_bwd_input = dynamic_cast<MklConv2DBwdInputPrimitive<T>*> (
        MklConv2DBwdInputPrimitiveFactory<T>::GetInstance().GetConv2dBwdInput(
            convBwdInputDims));

    if (conv2d_bwd_input == nullptr) {
      conv2d_bwd_input = new MklConv2DBwdInputPrimitive<T>(
          convBwdInputDims);
      MklConv2DBwdInputPrimitiveFactory<T>::GetInstance().SetConv2dBwdInput(
          convBwdInputDims, conv2d_bwd_input);
    }
    return conv2d_bwd_input;
  }

 private:
  static MklConv2DBwdInputPrimitiveFactory& GetInstance() {
    static MklConv2DBwdInputPrimitiveFactory instance_;
    return instance_;
  }

  static string CreateKey(const MklConvBwdInputParams& convBwdInputDims) {
    string prefix = "conv2d_bwd_input";
    FactoryKeyCreator key_creator;
    key_creator.AddAsKey(prefix);
    key_creator.AddAsKey(convBwdInputDims.diff_src_dims);
    key_creator.AddAsKey(convBwdInputDims.filter_dims);
    key_creator.AddAsKey(convBwdInputDims.diff_dst_dims);
    key_creator.AddAsKey(convBwdInputDims.strides);
    key_creator.AddAsKey(convBwdInputDims.dilations);
    key_creator.AddAsKey(convBwdInputDims.padding_left);
    key_creator.AddAsKey(convBwdInputDims.padding_right);
    return key_creator.GetKey();
  }

  MklPrimitive* GetConv2dBwdInput(
      const MklConvBwdInputParams& convBwdInputDims) {
    string key = CreateKey(convBwdInputDims);
    return this->GetOp(key);
  }

  void SetConv2dBwdInput(
      const MklConvBwdInputParams& convBwdInputDims, MklPrimitive *op) {
    string key = CreateKey(convBwdInputDims);
    this->SetOp(key, op);
  }
};

#endif

#ifdef INTEL_MKL_ML

template <typename Device, class T>
class MklConv2DCustomBackpropInputOp : public OpKernel {
 public:
  ~MklConv2DCustomBackpropInputOp() {}
  explicit MklConv2DCustomBackpropInputOp(OpKernelConstruction* context)
      : OpKernel(context) {
    string dataformat;
    OP_REQUIRES_OK(context, context->GetAttr("data_format", &dataformat));
    OP_REQUIRES(context, FormatFromString(dataformat, &data_format),
                errors::InvalidArgument("Invalid data format"));
    OP_REQUIRES_OK(context, context->GetAttr("strides", &strides));
    int stride_n = GetTensorDim(strides, data_format, 'N');
    int stride_c = GetTensorDim(strides, data_format, 'C');
    OP_REQUIRES(
        context, (stride_n == 1 && stride_c == 1),
        errors::InvalidArgument("Current implementation does not yet support "
                                "strides in the batch and depth dimensions."));

    OP_REQUIRES_OK(context, context->GetAttr("padding", &padding));
  }

  void Compute(OpKernelContext* context) override {
    MklConvBackInputOpContext mkl_context;
    const Tensor& input = MklGetInput(context, 0);
    const Tensor& filter = MklGetInput(context, 1);

    GetMklShape(context, 1, &(mkl_context.filter_shape));
    bool filter_in_mkl_format = mkl_context.filter_shape.IsMklTensor();

    const Tensor& out_backprop = MklGetInput(context, 2);
    GetMklShape(context, 2, &(mkl_context.outback_shape));
    bool outback_in_mkl_format = mkl_context.outback_shape.IsMklTensor();

    TensorShape input_shape, filter_shape, outback_shape;

    // Generate input shape.
    OP_REQUIRES(
        context, TensorShapeUtils::IsVector(input.shape()),
        errors::InvalidArgument(
            "Conv2DBackpropInput: input_sizes input must be 1-dim, not ",
            input.dims()));
    OP_REQUIRES_OK(
        context, TensorShapeUtils::MakeShape(input.vec<int32>(), &input_shape));

    // Generate shape for filter prop if input is in MKL format.
    if (filter_in_mkl_format) {
      OP_REQUIRES(context, mkl_context.filter_shape.GetDimension() == 4,
                  errors::InvalidArgument(
                      "Conv2DCustomBackpropInput: size must be 4-dim"));

      const int64* filter_sizes =
          (const int64*)mkl_context.filter_shape.GetSizes();
      const int64 filter_dims = mkl_context.filter_shape.GetDimension();

      OP_REQUIRES_OK(context, TensorShapeUtils::MakeShape(
                                  filter_sizes, filter_dims, &filter_shape));
    } else {
      filter_shape = filter.shape();
    }

    // Generate shape for outback prop if input is in MKL format.
    if (outback_in_mkl_format) {
      OP_REQUIRES(context, mkl_context.outback_shape.GetDimension() == 4,
                  errors::InvalidArgument(
                      "Conv2DCustomBackpropInput: size must be 4-dim"));

      MklSizesToTFSizes(context, data_format, mkl_context.outback_shape,
                        &outback_shape);
    } else {
      outback_shape = out_backprop.shape();
    }

    ConvBackpropDimensions dims;
    OP_REQUIRES_OK(
        context,
        ConvBackpropComputeDimensions(
            "Conv2DCustomBackpropInput", /*num_spatial_dims=*/2, input_shape,
            filter_shape, outback_shape, strides, padding, data_format, &dims));

    int64 pad_top, pad_bottom;
    int64 pad_left, pad_right;
    OP_REQUIRES_OK(
        context,
        GetWindowedOutputSizeVerbose(
            dims.spatial_dims[0].input_size, dims.spatial_dims[0].filter_size,
            dims.spatial_dims[0].stride, padding,
            &dims.spatial_dims[0].output_size, &pad_top, &pad_bottom));
    OP_REQUIRES_OK(
        context,
        GetWindowedOutputSizeVerbose(
            dims.spatial_dims[1].input_size, dims.spatial_dims[1].filter_size,
            dims.spatial_dims[1].stride, padding,
            &dims.spatial_dims[1].output_size, &pad_left, &pad_right));

    mkl_context.in_dims = 4;

    mkl_context.in_sizes[0] =
        static_cast<size_t>(dims.spatial_dims[1].input_size);
    mkl_context.in_sizes[1] =
        static_cast<size_t>(dims.spatial_dims[0].input_size);
    mkl_context.in_sizes[2] = static_cast<size_t>(dims.in_depth);
    mkl_context.in_sizes[3] = static_cast<size_t>(dims.batch_size);

    mkl_context.out_sizes[0] =
        static_cast<size_t>(dims.spatial_dims[1].output_size);
    mkl_context.out_sizes[1] =
        static_cast<size_t>(dims.spatial_dims[0].output_size);
    mkl_context.out_sizes[2] = static_cast<size_t>(dims.out_depth);
    mkl_context.out_sizes[3] = static_cast<size_t>(dims.batch_size);

    mkl_context.input_offset[0] = static_cast<int>(-pad_left);
    mkl_context.input_offset[1] = static_cast<int>(-pad_top);

    mkl_context.conv_strides[0] =
        static_cast<size_t>(dims.spatial_dims[1].stride);
    mkl_context.conv_strides[1] =
        static_cast<size_t>(dims.spatial_dims[0].stride);

    GetStridesFromSizes(data_format, mkl_context.out_strides,
                        mkl_context.out_sizes);
    GetStridesFromSizes(data_format, mkl_context.in_strides,
                        mkl_context.in_sizes);

    mkl_context.filter_size[0] = dims.spatial_dims[1].filter_size;
    mkl_context.filter_size[1] = dims.spatial_dims[0].filter_size;
    mkl_context.filter_size[2] = dims.in_depth;
    mkl_context.filter_size[3] = dims.out_depth;

    mkl_context.filter_stride[0] =
        mkl_context.filter_size[2] * mkl_context.filter_size[3];
    mkl_context.filter_stride[1] = mkl_context.filter_size[2] *
                                   mkl_context.filter_size[0] *
                                   mkl_context.filter_size[3];
    mkl_context.filter_stride[2] = mkl_context.filter_size[3];
    mkl_context.filter_stride[3] = 1;

    CHECK_EQ(
        dnnConvolutionCreateBackwardData_F32(
            &mkl_context.prim_bwddata, NULL, dnnAlgorithmConvolutionDirect,
            mkl_context.in_dims, mkl_context.in_sizes, mkl_context.out_sizes,
            mkl_context.filter_size, mkl_context.conv_strides,
            mkl_context.input_offset, dnnBorderZeros),
        E_SUCCESS);

    // Allocate output tensor and shape
    TensorShape mkl_out_shape;
    MklShape mklOutputShape;
    mklOutputShape.SetMklTensor(true);
    mklOutputShape.SetMklLayout(mkl_context.prim_bwddata, dnnResourceDiffSrc);
    mklOutputShape.SetTfLayout(mkl_context.in_dims, mkl_context.in_sizes,
                               mkl_context.in_strides);
    // MKL might change the dimension ordering.
    // Create mapping to recover the original TF dimension order
    mklOutputShape.SetTfDimOrder(mkl_context.in_dims, data_format);

    Tensor* in_backprop = nullptr;
    mkl_out_shape.AddDim(dnnLayoutGetMemorySize_F32(static_cast<dnnLayout_t>(
                             mklOutputShape.GetMklLayout())) /
                         sizeof(T));
    AllocateOutputSetMklShape(context, 0, &in_backprop, mkl_out_shape,
                              mklOutputShape);

    mkl_context.conv_res[dnnResourceDiffSrc] =
        static_cast<void*>(const_cast<T*>(in_backprop->flat<T>().data()));

    mkl_context.MklCreateInputLayouts(context);
    Tensor mkl_tmp_outbackprop_buf_tensor, mkl_tmp_filter_buf_tensor;
    mkl_context.MklPrepareConvolutionInputs(
        context, &mkl_tmp_outbackprop_buf_tensor, &mkl_tmp_filter_buf_tensor);

    CHECK_EQ(dnnExecute_F32(mkl_context.prim_bwddata, mkl_context.conv_res),
             E_SUCCESS);
    mkl_context.MklCleanup();
  }

 private:
  typedef struct {
    int in_dims;
    size_t in_sizes[4];
    size_t in_strides[4];
    size_t out_sizes[4];
    size_t out_strides[4];
    int input_offset[2];
    size_t filter_size[4];
    size_t filter_stride[4];
    size_t conv_strides[2];
    MklShape filter_shape, outback_shape;
    dnnPrimitive_t prim_bwddata;
    void* conv_res[dnnResourceNumber];
    dnnLayout_t lt_filter, lt_outbackprop;

    // Create MKL dnnLayout_t objects for tensors coming into the layer
    void MklCreateInputLayouts(OpKernelContext* context) {
      bool filter_in_mkl_format = filter_shape.IsMklTensor();
      bool outback_in_mkl_format = outback_shape.IsMklTensor();
      if (filter_in_mkl_format) {
        lt_filter = (dnnLayout_t)filter_shape.GetCurLayout();
      } else {
        CHECK_EQ(dnnLayoutCreate_F32(&lt_filter, in_dims, filter_size,
                                     filter_stride),
                 E_SUCCESS);
      }

      if (outback_in_mkl_format) {
        lt_outbackprop = (dnnLayout_t)outback_shape.GetCurLayout();
      } else {
        CHECK_EQ(dnnLayoutCreate_F32(&lt_outbackprop, in_dims, out_sizes,
                                     out_strides),
                 E_SUCCESS);
      }
    }

    // Compare incoming input tensor layouts with MKL preferred layouts and
    // convert data to the preferred layout if necessary
    void MklPrepareConvolutionInputs(OpKernelContext* context,
                                     Tensor* mkl_tmp_outbackprop_buf_tensor,
                                     Tensor* mkl_tmp_filter_buf_tensor) {
      dnnPrimitive_t mkl_convert_filter = nullptr,
                     mkl_convert_outbackprop = nullptr;
      void *mkl_filter_buf = nullptr, *mkl_outbackprop_buf = nullptr;
      dnnLayout_t mkl_lt_filter_internal = nullptr,
                  mkl_lt_outbackprop_internal = nullptr;
      CHECK_EQ(dnnLayoutCreateFromPrimitive_F32(
                   &mkl_lt_filter_internal, prim_bwddata, dnnResourceFilter),
               E_SUCCESS);

      const Tensor& filter = MklGetInput(context, 1);

      CHECK_EQ(
          dnnLayoutCreateFromPrimitive_F32(&mkl_lt_outbackprop_internal,
                                           prim_bwddata, dnnResourceDiffDst),
          E_SUCCESS);
      if (!dnnLayoutCompare_F32(mkl_lt_filter_internal, lt_filter)) {
        // Create conversion primitive
        CHECK_EQ(dnnConversionCreate_F32(&mkl_convert_filter, lt_filter,
                                         mkl_lt_filter_internal),
                 E_SUCCESS);

        AllocTmpBuffer(context, mkl_tmp_filter_buf_tensor,
                       mkl_lt_filter_internal, &mkl_filter_buf);
        CHECK_EQ(
            dnnConversionExecute_F32(
                mkl_convert_filter,
                static_cast<void*>(const_cast<T*>(filter.flat<T>().data())),
                mkl_filter_buf),
            E_SUCCESS);

        // Assign filter buf to resources[] for convolution.
        conv_res[dnnResourceFilter] = mkl_filter_buf;
        dnnDelete_F32(mkl_convert_filter);
      } else {
        // If we do not need any layout conversion for filter, then
        // we directly assign input filter to resources[].
        conv_res[dnnResourceFilter] =
            static_cast<void*>(const_cast<T*>(filter.flat<T>().data()));
      }
      dnnLayoutDelete_F32(mkl_lt_filter_internal);
      const Tensor& out_backprop = MklGetInput(context, 2);
      // --
      // We do similar steps as above for outputbackprop.
      if (!dnnLayoutCompare_F32(mkl_lt_outbackprop_internal, lt_outbackprop)) {
        CHECK_EQ(
            dnnConversionCreate_F32(&mkl_convert_outbackprop, lt_outbackprop,
                                    mkl_lt_outbackprop_internal),
            E_SUCCESS);
        AllocTmpBuffer(context, mkl_tmp_outbackprop_buf_tensor,
                       mkl_lt_outbackprop_internal, &mkl_outbackprop_buf);

        CHECK_EQ(dnnConversionExecute_F32(mkl_convert_outbackprop,
                                          static_cast<void*>(const_cast<T*>(
                                              out_backprop.flat<T>().data())),
                                          mkl_outbackprop_buf),
                 E_SUCCESS);

        conv_res[dnnResourceDiffDst] = mkl_outbackprop_buf;
        dnnDelete_F32(mkl_convert_outbackprop);
      } else {
        conv_res[dnnResourceDiffDst] =
            static_cast<void*>(const_cast<T*>(out_backprop.flat<T>().data()));
      }
      dnnLayoutDelete_F32(mkl_lt_outbackprop_internal);
    }

    // Cleanup member layouts and primitives
    void MklCleanup() {
      bool filter_in_mkl_format = filter_shape.IsMklTensor();
      bool outback_in_mkl_format = outback_shape.IsMklTensor();
      if (!filter_in_mkl_format) dnnLayoutDelete_F32(lt_filter);
      if (!outback_in_mkl_format) dnnLayoutDelete_F32(lt_outbackprop);
      dnnDelete_F32(prim_bwddata);
    }
  } MklConvBackInputOpContext;

  std::vector<int32> strides;
  Padding padding;
  TensorFormat data_format;
};

#else

template <typename Device, class T>
class MklConv2DCustomBackpropInputOp
    : public MklConv2DBackpropCommonOp<Device, T> {
 public:
  explicit MklConv2DCustomBackpropInputOp(OpKernelConstruction* context)
      : MklConv2DBackpropCommonOp<Device, T>(context) {
  }

  ~MklConv2DCustomBackpropInputOp() {}

  void Compute(OpKernelContext* context) {
    try {
      MklDnnData<T> filter(&cpu_engine);
      MklDnnData<T> diff_dst(&cpu_engine);

      // Input tensors
      const int kInputIdx = 0, kFilterIdx = 1, kOutbpropIdx = 2;
      const Tensor& src_tensor = MklGetInput(context, kInputIdx);
      const Tensor& filter_tensor = MklGetInput(context, kFilterIdx);
      const Tensor& diff_dst_tensor = MklGetInput(context, kOutbpropIdx);

      MklDnnShape src_mkl_shape, filter_mkl_shape, diff_dst_mkl_shape;
      GetMklShape(context, kInputIdx, &src_mkl_shape);
      GetMklShape(context, kFilterIdx, &filter_mkl_shape);
      GetMklShape(context, kOutbpropIdx, &diff_dst_mkl_shape);
      // Allow operator-specific sanity checking of shapes.
      ValidateMklShapes(src_mkl_shape, filter_mkl_shape,
                        diff_dst_mkl_shape);

      // Allow operator-specific generation of shapes.
      // E.g., Conv2DBackpropFilter gets filter as filter_sizes. It is a
      // tensor containing shape of filter. So filter.shape() is not
      // a correct way to get filter shape. These operator-specific calls
      // allow this class to handle this case.
      TensorShape src_tf_shape = MakeInputTfShape(context, src_tensor);
      TensorShape filter_tf_shape = MakeFilterTfShape(context, filter_tensor);
      TensorShape diff_dst_tf_shape = GetTfShape(context, kOutbpropIdx);

      // Corner cases: output with 0 elements and 0 batch size.
      Tensor* diff_src_tensor = nullptr;
      if (src_tf_shape.num_elements() == 0 ||
          filter_tf_shape.num_elements() == 0 ||
          diff_dst_tf_shape.num_elements() == 0) {
        MklDnnShape diff_src_mkl_shape;
        diff_src_mkl_shape.SetMklTensor(false);
        TensorShape diff_src_tf_shape = GetOutputTfShape(
            src_tf_shape, filter_tf_shape, diff_dst_tf_shape);
        const int kOutputIdx = 0;
        AllocateOutputSetMklShape(context, kOutputIdx, &diff_src_tensor,
                       diff_src_tf_shape, diff_src_mkl_shape);
        CHECK_NOTNULL(diff_src_tensor);

        // if output tensor has more than 0 elements, we need to 0 them out.
        auto diff_src_data = diff_src_tensor->flat<T>().data();
        for (size_t i = 0; i < diff_src_tf_shape.num_elements(); ++i) {
          diff_src_data[i] = 0;
        }
        return;
      }
      // By default, all dims are in MKL order. Only dims in TF order
      // are those with postfix tf_order.
      memory::dims diff_dst_dims, fwd_src_dims, fwd_filter_dims;
      memory::dims padding_left, padding_right, dilations, strides;
      memory::dims fwd_output_dims, fwd_output_dims_tf_order;

      // Get forward convolution parameters.
      MklDnnConvUtil conv_utl(context, this->strides_, this->padding_,
          this->data_format_, this->dilations_);
      conv_utl.GetConvFwdSizesInMklOrder(
          src_tf_shape, filter_tf_shape, &fwd_src_dims, &fwd_filter_dims,
          &strides, &dilations, &fwd_output_dims_tf_order, &fwd_output_dims,
          &padding_left, &padding_right);
      if (!context->status().ok()) return;

      // Create Convolution forward descriptor since Convolution backward
      // API needs it. For that, we first need to create input, filter
      // and output memory descriptors.
      auto tf_fmt = TFDataFormatToMklDnnDataFormat(this->data_format_);

      // If filter is in MKL layout, then simply grab filter layout;
      // otherwise, construct filter in TF layout.
      // For TF layout, filter is in HWIO format.
      auto fwd_filter_md = filter_mkl_shape.IsMklTensor()
                         ? filter_mkl_shape.GetMklLayout()
                         : memory::desc(fwd_filter_dims, MklDnnType<T>(),
                                        memory::format::hwio);

      conv_utl.GetInputSizeInMklOrder(diff_dst_tf_shape, &diff_dst_dims);
      if (!context->status().ok()) return;
      auto diff_dst_md = diff_dst_mkl_shape.IsMklTensor()
                       ? diff_dst_mkl_shape.GetMklLayout()
                       : memory::desc(diff_dst_dims,
                           MklDnnType<T>(), tf_fmt);

      dilations[kDilationH] -= 1;
      dilations[kDilationW] -= 1;

      MklConv2DBwdInputPrimitive<T> *conv2d_bwd_input = nullptr;
      conv_utl.GetInputSizeInMklOrder(diff_dst_tf_shape, &diff_dst_dims);
      MklConvBwdInputParams convBwdInputDims(fwd_src_dims, fwd_filter_dims,
          diff_dst_dims, strides, dilations, padding_left, padding_right,
          TFPaddingToMklDnnPadding(this->padding_));
      conv2d_bwd_input = MklConv2DBwdInputPrimitiveFactory<T>::Get(
          convBwdInputDims);
      auto bwd_input_pd = conv2d_bwd_input->GetPrimitiveDesc();

      // allocate output tensor
      auto diff_src_pd = bwd_input_pd->diff_src_primitive_desc();
      auto bwd_diff_src_dims = GetOutputDims(fwd_src_dims, fwd_filter_dims);
      auto bwd_diff_src_format = GetOutputFormat(tf_fmt);
      MklDnnShape diff_src_mkl_shape;
      diff_src_mkl_shape.SetMklTensor(true);
      diff_src_mkl_shape.SetMklLayout(&diff_src_pd);
      diff_src_mkl_shape.SetElemType(MklDnnType<T>());
      diff_src_mkl_shape.SetTfLayout(bwd_diff_src_dims.size(),
          bwd_diff_src_dims, bwd_diff_src_format);
      TensorShape diff_src_tf_shape;
      diff_src_tf_shape.AddDim(diff_src_pd.get_size() / sizeof(T));
      AllocateOutputSetMklShape(context, 0, &diff_src_tensor,
          diff_src_tf_shape, diff_src_mkl_shape);

      T *diff_src_data = static_cast<T*>(const_cast<T*>(
          diff_src_tensor->flat<T>().data()));

      // check if filter and diff_dst need reorder
      T* filter_data = nullptr;
      if (fwd_filter_md.data.format !=
          conv2d_bwd_input->GetFilterMemoryFormat()) {
        filter.SetUsrMem(fwd_filter_md, &filter_tensor);
        filter.CheckReorderToOpMem(bwd_input_pd->weights_primitive_desc());
        filter_data = static_cast<T*>(filter.GetOpMem().get_data_handle());
      } else {
        filter_data = static_cast<T*>(const_cast<T*>(
                       filter_tensor.flat<T>().data()));
      }

      T* diff_dst_data = nullptr;
      if (diff_dst_md.data.format !=
          conv2d_bwd_input->GetDiffDstMemoryFormat()) {
        diff_dst.SetUsrMem(diff_dst_md, &diff_dst_tensor);
        diff_dst.CheckReorderToOpMem(bwd_input_pd->diff_dst_primitive_desc());
        diff_dst_data = static_cast<T*>(
                         diff_dst.GetOpMem().get_data_handle());
      } else {
        diff_dst_data = static_cast<T*>(const_cast<T*>(
                         diff_dst_tensor.flat<T>().data()));
      }

      // execute convolution input bwd
      conv2d_bwd_input->Execute(diff_src_data, filter_data, diff_dst_data);
    } catch (mkldnn::error& e) {
      string error_msg = "Status: " + std::to_string(e.status) +
                         ", message: " + string(e.message) + ", in file " +
                         string(__FILE__) + ":" + std::to_string(__LINE__);
      OP_REQUIRES_OK(
          context,
          errors::Aborted("Operation received an exception:", error_msg));
    }
  }

 private:
  const int kInputIndex_Filter = 1, kInputIndex_InputSizes = 0;
  const int kDilationH = 0, kDilationW = 1;
  engine cpu_engine = engine(engine::cpu, 0);

  // Validate input shapes.
  // Function asserts that input shapes are valid.
  void ValidateMklShapes(const MklDnnShape& input_mkl_shape,
                         const MklDnnShape& filter_mkl_shape,
                         const MklDnnShape& obp_mkl_shape) {
    // Tensor that feeds to 'Input' slot of BackpropInput is always just a shape
    // of the Tensor and never an actual tensor. So it will never be in MKL
    // layout.
    CHECK(!input_mkl_shape.IsMklTensor())
        << "Conv2DBackpropInput: input should not be in MKL Layout";
  }

  // Get TensorFlow shape of input tensor.
  TensorShape MakeInputTfShape(OpKernelContext* context,
                               const Tensor& input_tensor) {
    TensorShape input_tf_shape;
    CHECK_EQ(TensorShapeUtils::IsVector(input_tensor.shape()), true);
    CHECK_EQ(
        TensorShapeUtils::MakeShape(input_tensor.vec<int32>(), &input_tf_shape)
            .ok(),
        true);
    return input_tf_shape;
  }

  // Get TensorFlow shape of filter tensor.
  TensorShape MakeFilterTfShape(OpKernelContext* context,
                                const Tensor& filter_tensor) {
    return GetTfShape(context, kInputIndex_Filter);
  }

  // Get the Tensorflow shape of Output (diff_src),
  // which is same as shape of Conv2D 'input'.
  TensorShape GetOutputTfShape(const TensorShape& input_shape,
                               const TensorShape& filter_shape,
                               const TensorShape& outbprop_shape) {
    return input_shape;
  }

  // Get the Tensorflow shape of Output (diff_src),
  // which is same as shape of Conv2D 'input'.
  const memory::dims& GetOutputDims(const memory::dims& fwd_input_dims,
                                    const memory::dims& fwd_filter_dims) {
    return fwd_input_dims;
  }

  // Output layout is Tensorflow's layout in data format order.
  memory::format GetOutputFormat(const memory::format data_format) {
    return data_format;
  }

  // Allocate output tensor.
  void AllocateOutputTensor(
      OpKernelContext* context,
      const convolution_backward_data::primitive_desc& conv_pd,
      const memory::dims& output_dims_mkl_order,
      memory::format output_tf_format, Tensor** output_tensor) {
    CHECK_NOTNULL(output_tensor);

    // Output primitive descriptor for backward data is diff_src.
    auto dst_pd = conv_pd.diff_src_primitive_desc();

    // Allocate shape of Mkl tensor.
    MklDnnShape output_mkl_shape;
    output_mkl_shape.SetMklTensor(true);
    output_mkl_shape.SetMklLayout(&dst_pd);
    output_mkl_shape.SetElemType(MklDnnType<T>());
    output_mkl_shape.SetTfLayout(output_dims_mkl_order.size(),
                                 output_dims_mkl_order, output_tf_format);

    // Allocate shape of TF tensor.
    TensorShape output_tf_shape;
    output_tf_shape.AddDim(dst_pd.get_size() / sizeof(T));

    AllocateOutputSetMklShape(context, 0, output_tensor, output_tf_shape,
                              output_mkl_shape);
  }
};

#endif  // INTEL_MKL_ML

#define REGISTER_MKL_CPU_KERNELS(T)                                 \
  REGISTER_KERNEL_BUILDER(Name("_MklConv2DBackpropInput")           \
                              .Device(DEVICE_CPU)                   \
                              .TypeConstraint<T>("T")               \
                              .Label(mkl_op_registry::kMklOpLabel), \
                          MklConv2DCustomBackpropInputOp<CPUDevice, T>);

TF_CALL_float(REGISTER_MKL_CPU_KERNELS);
#undef REGISTER_MKL_CPU_KERNELS

}  // namespace tensorflow
#endif  // INTEL_MKL
