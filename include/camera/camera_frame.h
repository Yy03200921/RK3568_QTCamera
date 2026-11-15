#ifndef RK3568_CAMERA_FRAME_H
#define RK3568_CAMERA_FRAME_H

#include <stddef.h>
#include <stdint.h>

typedef enum CameraPixelFormat {
    CAMERA_PIXEL_FORMAT_UNKNOWN = 0,
    CAMERA_PIXEL_FORMAT_UYVY,
    CAMERA_PIXEL_FORMAT_YUYV
} CameraPixelFormat;

/*
 * V4L2借出的原始帧。调用方必须在QBUF归还缓冲区前完成所有读取。
 * data用于校验MMAP缓冲区，dma_buf_fd交给RGA实现零拷贝输入。
 */
typedef struct CameraRawFrame {
    const uint8_t *data;
    size_t data_size;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t sequence;
    CameraPixelFormat pixel_format;
    uint32_t buffer_index;
    size_t buffer_size;
    int dma_buf_fd;
} CameraRawFrame;

/*
 * RGA输出的RGB帧。data指向RgaConverter内部缓冲区，下一次convert可能
 * 覆盖该内存，因此Qt层必须在当前调用链内复制为自己持有的QImage。
 */
typedef struct CameraRgbFrame {
    const uint8_t *data;
    size_t data_size;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t sequence;
} CameraRgbFrame;

#endif
