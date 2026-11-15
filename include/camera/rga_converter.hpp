#ifndef RK3568_RGA_CONVERTER_HPP
#define RK3568_RGA_CONVERTER_HPP

#include "camera/camera_frame.h"

#include <vector>

class RgaConverter final
{
public:
    RgaConverter() = default;
    ~RgaConverter() = default;
    RgaConverter(const RgaConverter &) = delete;
    RgaConverter &operator=(const RgaConverter &) = delete;

    /*
     * 从V4L2 DMA-BUF读取UYVY/YUYV，由RGA转换为RGB888。输出尺寸与
     * 输入不同时继续由RGA缩放，结果缓冲区由本对象持有。
     */
    bool convert(const CameraRawFrame &source,
                 uint32_t output_width,
                 uint32_t output_height,
                 CameraRgbFrame *destination,
                 const char **error_message);

private:
    bool prepareColorBuffer(uint32_t width, uint32_t height,
                            const char **error_message);
    bool prepareOutputBuffer(uint32_t width, uint32_t height,
                             const char **error_message);
    bool convertColor(const CameraRawFrame &source,
                      const char **error_message);
    bool resizeOutput(uint32_t width, uint32_t height,
                      const char **error_message);

    std::vector<uint8_t> color_buffer_;
    std::vector<uint8_t> output_buffer_;
    uint32_t source_width_ = 0U;
    uint32_t source_height_ = 0U;
    uint32_t source_stride_pixels_ = 0U;
    uint32_t output_stride_pixels_ = 0U;
};

#endif
