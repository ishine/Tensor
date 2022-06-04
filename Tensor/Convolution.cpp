//
//  Convolution.cpp
//  Tensor
//
//  Created by 陳均豪 on 2022/2/15.
//

#include "Tensor.hpp"
#include "TensorShape.hpp"
#include "Convolution.hpp"
#include "ConvolutionMM2DNeon.hpp"
#include "ConvolutionMM2DX86.hpp"
#include "DepthwiseConvKernelNeon.hpp"
#include "DepthwiseConvKernelX86.hpp"
#include "DilatedConvolution.hpp"
#include "ConvolutionMM2DTranspose.hpp"
#include "ConvolutionMM2DTransposeNeon.hpp"
#include "DepthwiseConvTransposeKernelNeon.hpp"

namespace otter {

DEFINE_DISPATCH(convolution_depthwise3x3_winograd_stub);

std::ostream& operator<<(std::ostream & out, const ConvParams& params) {
    out << "ConvParams {"
        << "  stride = " << IntArrayRef{params.stride}
        << "  padding = " << IntArrayRef{params.padding}
        << "  dilation = " << IntArrayRef{params.dilation}
        << "  transposed = " << params.transposed
        << "  output_padding = " << IntArrayRef{params.output_padding}
        << "  groups = " << params.groups
        << "  benchmark = " << params.benchmark
        << "}";
    return out;
}

static void check_shape_forward(const Tensor& input, const IntArrayRef& weight_sizes, const Tensor& bias, const ConvParams& params) {
    int64_t k = input.dim();
    int64_t weight_dim = weight_sizes.size();
    int64_t groups = params.groups;
    const auto& padding = params.padding;
    const auto& dilation = params.dilation;
    bool transposed = params.transposed;
    
    OTTER_CHECK(!params.is_padding_neg(), "negative padding is not supported");
    OTTER_CHECK(!params.is_output_padding_neg(), "negative output_padding is not supported");
    OTTER_CHECK(!params.is_stride_nonpos(), "non-positive stride is not supported");
    
    OTTER_CHECK(weight_dim == k,
                "Expected ", weight_dim, "-dimensional input for ", weight_dim,
                "-dimensional weight ", weight_sizes, ", but got ", k, "-dimensional input of size ",
                input.sizes(), " instead");
    OTTER_CHECK(weight_sizes[0] >= groups,
                "Given groups=", groups, ", expected weight to be at least ", groups,
                " at dimension 0, but got weight of size ", weight_sizes, " instead");
    OTTER_CHECK(weight_sizes[0] % groups == 0,
                "Given groups=", groups, ", expected weight to be divisible by ",
                groups, " at dimension 0, but got weight of size [", weight_sizes,
                "] instead");
    
    if (!transposed) {
        std::vector<int64_t> input_shape;
        std::vector<int64_t> kernel_shape;
        bool kernel_size_correct = true;
        
        OTTER_CHECK(input.size(1) == (weight_sizes[1] * groups),
                    "Given groups=", groups, ", weight of size ", weight_sizes,
                    ", expected input", input.sizes(), " to have ",
                    (weight_sizes[1] * groups), " channels, but got ", input.size(1),
                    " channels instead");

        OTTER_CHECK(!bias.defined() || (bias.dim() == 1 && bias.size(0) == weight_sizes[0]),
                    "Given weight of size ", weight_sizes,
                    ", expected bias to be 1-dimensional with ", weight_sizes[0], " elements",
                    ", but got bias of size ", bias.sizes(), " instead");
        
        for (const auto i : otter::irange(2, k)) {
            input_shape.push_back(input.size(i) + 2 * padding[i - 2]);
            // log new kernel size considering dilation
            kernel_shape.push_back(dilation[i - 2] * (weight_sizes[i] - 1) + 1);
            if (input_shape.back() < kernel_shape.back()) {
                kernel_size_correct = false;
            }
        }
        
        assert(input_shape.size() == kernel_shape.size());
        
        if (!kernel_size_correct) {
            throw "Kernel size can't be greater than actual input size";
        }
        
    } else {
        OTTER_CHECK(input.size(1) == weight_sizes[0],
            "Given transposed=", transposed, ", weight of size ", weight_sizes,
            ", expected input", input.sizes(), " to have ", weight_sizes[0],
            " channels, but got ", input.size(1), " channels instead");
        OTTER_CHECK(!bias.defined() || (bias.dim() == 1 && bias.size(0) == weight_sizes[1] * groups),
            "Given transposed=", transposed, ", weight of size ", weight_sizes,
            ", expected bias to be 1-dimensional with ", weight_sizes[1] * groups, " elements",
            ", but got bias of size ", bias.sizes(), " instead");
    }
}

ConvBackend select_proper_conv_backend(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& /*bias*/,
    const bool /*need_backward*/,
    const ConvParams& params) {
    
    const int64_t w = input.size(3);
    const int64_t h = input.size(2);
    const int64_t kernel_w = weight.size(3);
    const int64_t kernel_h = weight.size(2);
    const int64_t stride_w = params.stride[1];
    const int64_t stride_h = params.stride[0];
    const int64_t num_input = input.size(1);
    const int64_t num_output = weight.size(0);
    
    if (input.device() == Device::CPU) { // or input.is_cuda()
        if (params.is_int8(input, weight)) {
            return ConvBackend::SlideWin2dInt8;
        } else {
            if (params.transposed) {
                if (input.dim() == 4) {
                    if (params.is_dilated()) {
                        return ConvBackend::SlowTranspose2d;
                    } else {
                        if (params.use_cpu_neon(input, weight)) {
    //                        if (params.is_transpose_depthwise(input, weight)) {
    //                            return ConvBackend::DepthwiseTransposeNeon;
    //                        }
                            if (kernel_w == 4 && kernel_h == 4 && stride_w == 2 && stride_h == 2) {
                                return ConvBackend::Transpose2dNeon_4x4s2;
                            }
                        }
    //                    return ConvBackend::SlideWinTranspose2d;
                        return ConvBackend::SlowTranspose2d;
                    }
                }
            } else {
                if (input.dim() == 4) {
                    if (params.is_dilated()) {
                        return ConvBackend::SlowDilated2d;
                    } else {
                        if (params.use_cpu_neon(input, weight)) {
                            // Depthwise
                            if (params.is_depthwise(input, weight)) {
                                if (kernel_w == 3 && kernel_h == 3 && stride_w == 1 && stride_h == 1) {
                                    return ConvBackend::DepthwiseNeon_3x3s1;
                                } else if (kernel_w == 3 && kernel_h == 3 && stride_w == 2 && stride_h == 2) {
                                    return ConvBackend::DepthwiseNeon_3x3s2;
                                } else if (kernel_w == 5 && kernel_h == 5 && stride_w == 1 && stride_h == 1) {
                                    return ConvBackend::DepthwiseNeon_5x5s1;
                                } else if (kernel_w == 5 && kernel_h == 5 && stride_w == 2 && stride_h == 2) {
                                    return ConvBackend::DepthwiseNeon_5x5s2;
                                }
                            }
                            // General
                            if (kernel_w == 1 && kernel_h == 1 && stride_w == 1 && stride_h == 1) {
                                if (num_input >= 64 && num_output >= 64) {
                                    return ConvBackend::Sgemm2dNeon_1x1s1;
                                } else {
                                    return ConvBackend::SlideWin2dNeon_1x1s1;
                                }
                            } else if (kernel_w == 1 && kernel_h == 1 && stride_w == 2 && stride_h == 2) {
                                return ConvBackend::Sgemm2dNeon_1x1s2;
                            } else if (kernel_w == 3 && kernel_h == 3 && stride_w == 1 && stride_h == 1) {
                                if (num_input >= 16 && num_output >= 16 && w <= 120 && h <= 120) {
                                    return ConvBackend::WinogradNeon_3x3s1;
                                } else {
                                    return ConvBackend::SlideWin2dNeon_3x3s1;
                                }
                                
    //                            if (!need_backward && params.use_cpu_depthwise3x3_winograd(input, weight)) {
    //                                return ConvBackend::Winograd3x3Depthwise;
    //                            }
                            } else if (kernel_w == 3 && kernel_h == 3 && stride_w == 2 && stride_h == 2) {
                                auto output_shape = otter::calculate_conv_output_size(input.sizes(), weight.sizes(), params.stride, params.padding);
                                if (!(output_shape[2] >= 8 && output_shape[3] >= 8)) {
                                    return ConvBackend::Sgemm2dNeon;
                                } else {
                                    return ConvBackend::Packed2DNeon_3x3s2;
                                }
                            } else {
                                bool prefer_sgemm = true;
                                if (num_output == 1) {
                                    if ((kernel_w == 3 && num_input >= 64) || (kernel_w == 4 && num_input >= 3) || kernel_w >= 5)
                                        prefer_sgemm = false;
                                }
                                if (num_output == 2) {
                                    if ((kernel_w == 5 && num_input >= 64) || (kernel_w == 6 && num_input >= 32) || ((kernel_w == 7 || kernel_w == 8 || kernel_w == 9) && num_input >= 16) || (kernel_w >= 10 && num_input >= 8))
                                            prefer_sgemm = false;
                                }
                                
                                if (prefer_sgemm) {
                                    return ConvBackend::Sgemm2dNeon;
                                } else {
                                    return ConvBackend::SlideWin2d;
                                }
                            }
                        } else if (params.use_cpu_x86(input, weight)) {
                            // Depthwise
                            if (params.is_depthwise(input, weight)) {
                                if (weight.size(2) == 3 && weight.size(3) == 3 && params.stride[0] == 1 && params.stride[1] == 1) {
                                    return ConvBackend::DepthwiseX86_3x3s1;
                                } else if (weight.size(2) == 3 && weight.size(3) == 3 && params.stride[0] == 2 && params.stride[1] == 2) {
                                    return ConvBackend::DepthwiseX86_3x3s2;
                                }
                            }
                            // General
                            if (weight.size(2) == 3 && weight.size(3) == 3 && params.stride[0] == 1 && params.stride[1] == 1 && input.size(1) >= 16 && weight.size(0) >= 16) {
                                return ConvBackend::WinogradX86_3x3s1;
                            }
                            return ConvBackend::Sgemm2dX86;
                        } else {
                            return ConvBackend::Slow2d;
                        }
                    }
                } else {
                    // unsupported
                }
            }
        }
    } else {
        return ConvBackend::Overrideable;
    }
    throw "unsupported ConvNd parameters";
    return ConvBackend::Overrideable;
}

static auto view3d(const Tensor& tensor) -> Tensor {
    OTTER_CHECK(tensor.dim() == 4, "expected 4D tensor, got tensor with ", tensor.dim(), " dimensions instead");
    return tensor.squeeze(2);
}

static auto view4d(const Tensor& tensor) -> Tensor {
    OTTER_CHECK(tensor.dim() == 3, "expected 3D tensor, got tensor with ", tensor.dim(), " dimensions instead");
    return tensor.unsqueeze(2);
}

static Tensor subtensor(Tensor& tensor, int dim, int groups, int g) {
    if (!tensor.defined()) {
        return Tensor();
    }
    int64_t n = tensor.sizes()[dim] / groups;
    return tensor.narrow(dim, n * g, n).contiguous();
}

Tensor convolution(
    const Tensor& input_r,
    const Tensor& weight_r,
    const Tensor& weight_o,
    const Tensor& bias_r,
    IntArrayRef stride_,
    IntArrayRef padding_,
    IntArrayRef dilation_,
    bool transposed_,
    IntArrayRef output_padding_,
    int64_t groups_,
    const Tensor& input_int8_scales,
    const Tensor& weight_int8_scales) {
    
    auto input = input_r;
    auto weight = weight_r;
    auto bias = bias_r;
    auto k = weight.dim();
    auto dim = k - 2;
    
    OTTER_CHECK(k > 0, "weight should have at least three dimensions");
    
    auto weight_sizes = weight.sizes();
    
    ConvParams params;
    params.stride    = expand_param_if_needed(stride_, "stride", dim);
    params.padding   = expand_param_if_needed(padding_, "padding", dim);
    params.dilation  = expand_param_if_needed(dilation_, "dilation", dim);
    params.output_padding = expand_param_if_needed(output_padding_, "output_padding", dim);
    params.transposed = transposed_;
    params.groups    = groups_;
    
    check_shape_forward(input, weight_sizes, bias, params);
    
    if (k == 3) {
        // avoid accidentally going through NHWC for permuted 3d input.
        input = input.contiguous();
        params.view_1d_as_2d();
        input  = view4d(input);
        weight = view4d(weight);
    }
    
    bool need_backward = false; // TODO: backward propogation
    ConvBackend backend = select_proper_conv_backend(input, weight, bias, need_backward, params);
    
    Tensor output;
    
    switch (backend) {
        case ConvBackend::Winograd3x3Depthwise:
            output = convolution_depthwise3x3_winograd_stub(Device::CPU, input, weight, bias, params.stride, params.padding, params.groups);
            break;
        case ConvBackend::DepthwiseNeon_3x3s1:
            output = depthwise_conv2d_3x3s1_neon(input.contiguous(), weight, bias, params.stride, params.padding);
            break;
        case ConvBackend::DepthwiseNeon_3x3s2:
            output = depthwise_conv2d_3x3s2_neon(input.contiguous(), weight, bias, params.stride, params.padding);
            break;
        case ConvBackend::DepthwiseNeon_5x5s1:
            output = depthwise_conv2d_5x5s1_neon(input.contiguous(), weight, bias, params.stride, params.padding);
            break;
        case ConvBackend::DepthwiseNeon_5x5s2:
            output = depthwise_conv2d_5x5s2_neon(input.contiguous(), weight, bias, params.stride, params.padding);
            break;
        case ConvBackend::DepthwiseX86_3x3s1:
            output = depthwise_conv2d_3x3s1_x86_sse(input.contiguous(), weight, bias, params.stride, params.padding);
            break;
        case ConvBackend::DepthwiseX86_3x3s2:
            output = depthwise_conv2d_3x3s2_x86_sse(input.contiguous(), weight, bias, params.stride, params.padding);
            break;
        case ConvBackend::DepthwiseTransposeNeon:
            output = depthwise_deconv2d_neon(input.contiguous(), weight, weight_o, bias, params.stride, params.padding, params.output_padding, params.dilation);
            break;
        case ConvBackend::Slow2d:
        case ConvBackend::SlowTranspose2d:
        case ConvBackend::SlideWinTranspose2d:
        case ConvBackend::Transpose2dNeon_4x4s2:
        case ConvBackend::Sgemm2dNeon:
        case ConvBackend::Sgemm2dNeon_1x1s1:
        case ConvBackend::Sgemm2dNeon_1x1s2:
        case ConvBackend::Sgemm2dX86:
        case ConvBackend::SlideWin2dNeon_1x1s1:
        case ConvBackend::SlideWin2d:
        case ConvBackend::SlideWin2dNeon_3x3s1:
        case ConvBackend::WinogradNeon_3x3s1:
        case ConvBackend::Packed2DNeon_3x3s2:
        case ConvBackend::WinogradX86_3x3s1:
        case ConvBackend::SlowDilated2d:
        case ConvBackend::SlideWin2dInt8:
            if (params.groups == 1) {
                output = otter::convolution_nogroup_backend(input.contiguous(), weight, weight_o, bias, backend, params, input_int8_scales, weight_int8_scales);
            } else {
                std::vector<Tensor> outputs(params.groups);
                input = input.contiguous();
                for (const auto g : otter::irange(params.groups)) {
                    auto input_g = subtensor(input, 1, static_cast<int>(params.groups), static_cast<int>(g));
                    auto weight_g = subtensor(weight, 0, static_cast<int>(params.groups), static_cast<int>(g));
                    auto bias_g = subtensor(bias, 0, static_cast<int>(params.groups), static_cast<int>(g));
                    auto input_int8_scales_g = (input_int8_scales.defined()) ? input_int8_scales[g].view({-1}) : Tensor();
                    auto weight_int8_scales_g = (weight_int8_scales.defined()) ? weight_int8_scales[g].view({-1}) : Tensor();
                    outputs[g] = otter::convolution_nogroup_backend(input_g, weight_g, weight_o, bias_g, backend, params, input_int8_scales_g, weight_int8_scales_g);
                }
                output = otter::native::cat(outputs, 1);
            }
            
        default:
            break;
    }
    
    if (k == 3) {
        output = view3d(output);
    }
    
    return output;
}

Tensor convolution_nogroup_backend(const Tensor& self, const Tensor& weight, const Tensor& weight_o, const Tensor& bias, ConvBackend backend, ConvParams& params, const Tensor& input_int8_scales, const Tensor& weight_int8_scales) {
    auto kernel_size = weight.sizes().slice(2);
    switch (backend) {
        case ConvBackend::Slow2d:
            return otter::slow_conv2d(self, weight, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::SlowTranspose2d:
            return otter::slow_conv_transpose2d(self, weight, kernel_size, bias, params.stride, params.padding, params.output_padding, params.dilation);
        case ConvBackend::SlideWinTranspose2d:
            return otter::slide_win_conv_transpose2d(self, weight, kernel_size, bias, params.stride, params.padding, params.output_padding, params.dilation);
        case ConvBackend::Transpose2dNeon_4x4s2:
            return otter::deconv2d_4x4s2_neon(self, weight, bias, params.stride, params.padding, params.output_padding, params.dilation);
        case ConvBackend::SlowDilated2d:
            return otter::slow_conv_dilated2d(self, weight, bias, kernel_size, params.stride, params.padding, params.dilation);
        case ConvBackend::Sgemm2dNeon:
            return otter::sgemm_conv2d_neon(self, weight, weight_o, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::Sgemm2dNeon_1x1s1:
            return otter::sgemm_conv2d_1x1s1_neon(self, weight, weight_o, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::Sgemm2dX86:
            return otter::sgemm_conv2d_x86(self, weight, weight_o, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::SlideWin2dNeon_1x1s1:
            return otter::conv2d_1x1s1_neon(self, weight, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::Sgemm2dNeon_1x1s2:
            return otter::sgemm_conv2d_1x1s2_neon(self, weight, weight_o, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::SlideWin2dNeon_3x3s1:
            return otter::conv2d_3x3s1_neon(self, weight, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::WinogradNeon_3x3s1:
            return otter::conv2d_3x3s1_winograd64_neon(self, weight, weight_o, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::Packed2DNeon_3x3s2:
            return otter::conv2d_3x3s2_packed_neon(self, weight, weight_o, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::WinogradX86_3x3s1:
            return otter::conv2d_3x3s1_winograd23_x86(self, weight, weight_o, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::SlideWin2d:
            return otter::slide_win_conv2d(self, weight, bias, kernel_size, params.stride, params.padding);
        case ConvBackend::SlideWin2dInt8:
            return otter::slide_win_conv2d_int8(self, weight, weight_int8_scales, bias, input_int8_scales, kernel_size, params.stride, params.padding, params.dilation);
        default:
            OTTER_CHECK(false, "Unsupported nogroup conv backend");
    }
    
    return Tensor();
}

}   // end namespace otter
