//
//  im2col.hpp
//  Tensor
//
//  Created by 陳均豪 on 2022/2/24.
//

#ifndef im2col_hpp
#define im2col_hpp

#include "Utils.hpp"

namespace otter {

template <typename T>
static void im2col(
    const T* data_im,
    const int64_t channels,
    const int64_t height,
    const int64_t width,
    const int64_t output_height,
    const int64_t output_width,
    const int64_t kernel_h,
    const int64_t kernel_w,
    const int64_t pad_h,
    const int64_t pad_w,
    const int64_t stride_h,
    const int64_t stride_w,
    const int64_t dilation_h,
    const int64_t dilation_w,
    T* data_col) {
  const int64_t height_col = output_height;
  const int64_t width_col = output_width;
  const int64_t channels_col = channels * kernel_h * kernel_w;

  for (const auto c_col : otter::irange(channels_col)) {
    int64_t w_offset = c_col % kernel_w;
    int64_t h_offset = (c_col / kernel_w) % kernel_h;
    int64_t c_im = c_col / kernel_h / kernel_w;

    for (const auto h_col : otter::irange(height_col)) {
      int64_t h_im = h_col * stride_h - pad_h + h_offset * dilation_h;

      for (const auto w_col : otter::irange(width_col)) {
        int64_t w_im = w_col * stride_w - pad_w + w_offset * dilation_w;
        data_col[(c_col * height_col + h_col) * width_col + w_col] =
            (h_im >= 0 && w_im >= 0 && h_im < height && w_im < width)
            ? data_im[(c_im * height + h_im) * width + w_im]
            : static_cast<T>(0);
      }
    }
  }
}

template <typename T>
static void col2im(
    const T* data_col,
    const int64_t channels,
    const int64_t height,
    const int64_t width,
    const int64_t output_height,
    const int64_t output_width,
    const int64_t kernel_h,
    const int64_t kernel_w,
    const int64_t pad_h,
    const int64_t pad_w,
    const int64_t stride_h,
    const int64_t stride_w,
    const int64_t dilation_h,
    const int64_t dilation_w,
    T* data_im) {
  std::fill_n(data_im, height * width * channels, T(0));

  const int64_t height_col = output_height;
  const int64_t width_col = output_width;
  const int64_t channels_col = channels * kernel_h * kernel_w;

  for (const auto c_col : otter::irange(channels_col)) {
    int64_t w_offset = c_col % kernel_w;
    int64_t h_offset = (c_col / kernel_w) % kernel_h;
    int64_t c_im = c_col / kernel_h / kernel_w;

    for (const auto h_col : otter::irange(height_col)) {
      int64_t h_im = h_col * stride_h - pad_h + h_offset * dilation_h;

      for (const auto w_col : otter::irange(width_col)) {
        int64_t w_im = w_col * stride_w - pad_w + w_offset * dilation_w;

        if (h_im >= 0 && h_im < height && w_im >= 0 && w_im < width)
          data_im[(c_im * height + h_im) * width + w_im] +=
              data_col[(c_col * height_col + h_col) * width_col + w_col];
      }
    }
  }
}

}

#endif /* im2col_hpp */
