#include "camera/rga_converter.hpp"

#include <cstring>
#include <stdio.h>

#include <rga/rga.h>
#include <rga/im2d.h>

#include <limits>

namespace {
constexpr uint32_t kPixelAlignment = 16U;
constexpr uint32_t kYuvBytesPerPixel = 2U;
constexpr uint32_t kRgbBytesPerPixel = 3U;

void setError(const char **output, const char *message)
{
    if (output != nullptr) {
        *output = message;
    }
}

bool calculateRgbLayout(uint32_t width, uint32_t height,
                        uint32_t *stride_pixels, size_t *bytes)
{
    const uint32_t max_int =
        static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (stride_pixels == nullptr || bytes == nullptr || width == 0U ||
        height == 0U || height > max_int ||
        width > max_int - (kPixelAlignment - 1U)) {
        return false;
    }
    const uint32_t stride =
        (width + kPixelAlignment - 1U) & ~(kPixelAlignment - 1U);
    const size_t max_size = std::numeric_limits<size_t>::max();
    if (height > max_size / stride / kRgbBytesPerPixel) {
        return false;
    }
    *stride_pixels = stride;
    *bytes = static_cast<size_t>(stride) * height * kRgbBytesPerPixel;
    return *bytes <=
           static_cast<size_t>(std::numeric_limits<int>::max());
}

bool resizeVector(std::vector<uint8_t> *buffer, size_t bytes,
                  const char **error_message)
{
    if (buffer == nullptr) {
        setError(error_message, "RGB buffer pointer is null");
        return false;
    }
    try {
        buffer->resize(bytes);
    } catch (...) {
        setError(error_message, "RGB buffer allocation failed");
        return false;
    }
    return true;
}

bool validSource(const CameraRawFrame &source)
{
    const uint32_t max_int =
        static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (source.data == nullptr || source.dma_buf_fd < 0 ||
        source.width == 0U || source.height == 0U ||
        source.width % 2U != 0U ||
        source.width > max_int || source.height > max_int ||
        source.width > std::numeric_limits<uint32_t>::max() /
                           kYuvBytesPerPixel ||
        source.bytes_per_line % kYuvBytesPerPixel != 0U ||
        source.bytes_per_line < source.width * kYuvBytesPerPixel ||
        source.bytes_per_line / kYuvBytesPerPixel > max_int) {
        return false;
    }
    if (static_cast<size_t>(source.bytes_per_line) >
        std::numeric_limits<size_t>::max() / source.height) {
        return false;
    }
    const size_t required = static_cast<size_t>(source.bytes_per_line) *
                            source.height;
    return required <= source.data_size && required <= source.buffer_size &&
           (source.pixel_format == CAMERA_PIXEL_FORMAT_UYVY ||
            source.pixel_format == CAMERA_PIXEL_FORMAT_YUYV);
}
}

bool RgaConverter::prepareColorBuffer(uint32_t width, uint32_t height,
                                      const char **error_message)
{
    size_t bytes = 0U;
    uint32_t stride = 0U;
    if (!calculateRgbLayout(width, height, &stride, &bytes)) {
        setError(error_message, "Invalid RGA color buffer dimensions");
        return false;
    }
    if (!resizeVector(&color_buffer_, bytes, error_message)) {
        return false;
    }
    source_width_ = width;
    source_height_ = height;
    source_stride_pixels_ = stride;
    return true;
}

bool RgaConverter::prepareOutputBuffer(uint32_t width, uint32_t height,
                                       const char **error_message)
{
    size_t bytes = 0U;
    uint32_t stride = 0U;
    if (!calculateRgbLayout(width, height, &stride, &bytes)) {
        setError(error_message, "Invalid RGA output dimensions");
        return false;
    }
    if (!resizeVector(&output_buffer_, bytes, error_message)) {
        return false;
    }
    output_stride_pixels_ = stride;
    return true;
}

bool RgaConverter::convertColor(const CameraRawFrame &source,
                                const char **error_message)
{
    const int source_format = source.pixel_format == CAMERA_PIXEL_FORMAT_UYVY
                                  ? RK_FORMAT_UYVY_422
                                  : RK_FORMAT_YUYV_422;
    // 输入直接引用V4L2导出的DMA-BUF，CPU不复制摄像头原始帧。
    rga_buffer_t input = wrapbuffer_fd(
        source.dma_buf_fd, static_cast<int>(source.width),
        static_cast<int>(source.height), source_format,
        static_cast<int>(source.bytes_per_line / kYuvBytesPerPixel),
        static_cast<int>(source.height));
    // 输出写入CPU可访问内存，后续Qt会深拷贝为独立QImage。
    rga_buffer_t output = wrapbuffer_virtualaddr(
        color_buffer_.data(), static_cast<int>(source.width),
        static_cast<int>(source.height), RK_FORMAT_RGB_888,
        static_cast<int>(source_stride_pixels_),
        static_cast<int>(source.height));
    const im_rect source_rect{};
    const im_rect destination_rect{};
    if (imcheck(input, output, source_rect, destination_rect) !=
        IM_STATUS_NOERROR) {
        setError(error_message, "RGA rejected color conversion layout");
        return false;
    }
    if (imcvtcolor(input, output, source_format, RK_FORMAT_RGB_888,
                   IM_YUV_TO_RGB_BT601_FULL) != IM_STATUS_SUCCESS) {
        setError(error_message, "RGA YUV422 to RGB conversion failed");
        return false;
    }
    return true;
}

bool RgaConverter::resizeOutput(uint32_t width, uint32_t height,
                                const char **error_message)
{
    rga_buffer_t input = wrapbuffer_virtualaddr(
        color_buffer_.data(), static_cast<int>(source_width_),
        static_cast<int>(source_height_), RK_FORMAT_RGB_888,
        static_cast<int>(source_stride_pixels_),
        static_cast<int>(source_height_));
    rga_buffer_t output = wrapbuffer_virtualaddr(
        output_buffer_.data(), static_cast<int>(width),
        static_cast<int>(height), RK_FORMAT_RGB_888,
        static_cast<int>(output_stride_pixels_), static_cast<int>(height));
    const im_rect source_rect{};
    const im_rect destination_rect{};
    if (imcheck(input, output, source_rect, destination_rect) !=
        IM_STATUS_NOERROR) {
        setError(error_message, "RGA rejected preview scaling layout");
        return false;
    }
    if (imresize(input, output) != IM_STATUS_SUCCESS) {
        setError(error_message, "RGA preview scaling failed");
        return false;
    }
    return true;
}

bool RgaConverter::convert(const CameraRawFrame &source,
                           uint32_t output_width, uint32_t output_height,
                           CameraRgbFrame *destination,
                           const char **error_message)
{
    if (error_message != nullptr) {
        *error_message = nullptr;
    }
    if (destination == nullptr) {
        setError(error_message, "RGB output pointer is null");
        return false;
    }
    *destination = CameraRgbFrame{};
    if (!validSource(source) || output_width == 0U || output_height == 0U) {
        setError(error_message, "Invalid or unsupported V4L2 frame");
        return false;
    }
    if (!prepareColorBuffer(source.width, source.height, error_message) ||
        !convertColor(source, error_message)) {
        return false;
    }

    const bool original_size =
        output_width == source.width && output_height == source.height;
    if (!original_size &&
        (!prepareOutputBuffer(output_width, output_height, error_message) ||
         !resizeOutput(output_width, output_height, error_message))) {
        return false;
    }

    destination->data = original_size ? color_buffer_.data()
                                      : output_buffer_.data();
    destination->data_size = original_size ? color_buffer_.size()
                                           : output_buffer_.size();
    destination->width = output_width;
    destination->height = output_height;
    destination->bytes_per_line =
        (original_size ? source_stride_pixels_ : output_stride_pixels_) *
        kRgbBytesPerPixel;
    destination->sequence = source.sequence;
    return true;
}
