#ifndef RK_V4L2_CAPTURE_H
#define RK_V4L2_CAPTURE_H

#include "camera_frame.h"

#include <stdint.h>

typedef struct V4l2Capture V4l2Capture;

/* 创建采集对象时的期望参数，驱动可能返回不同的实际宽高和步长。 */
typedef struct V4l2CaptureConfig {
    const char *device_path;
    uint32_t width;
    uint32_t height;
    CameraPixelFormat pixel_format;
    uint32_t buffer_count;
} V4l2CaptureConfig;

#ifdef __cplusplus
extern "C" {
#endif

/* 分配采集对象并保存配置，此时尚未打开设备节点。 */
V4l2Capture *v4l2_capture_create(const V4l2CaptureConfig *config);

/* 停止采集、解除MMAP、关闭DMA-BUF和设备节点，然后释放对象。 */
void v4l2_capture_destroy(V4l2Capture *capture);

/* 打开设备，检查能力，协商格式并申请MMAP/DMA-BUF缓冲区。 */
int v4l2_capture_open(V4l2Capture *capture);

/* 将所有缓冲区QBUF入队，然后执行VIDIOC_STREAMON。 */
int v4l2_capture_start(V4l2Capture *capture);

/*
 * 等待并取出一帧。返回0表示成功，1表示超时或暂时无帧，-1表示错误。
 * 成功后必须调用v4l2_capture_queue归还frame->buffer_index。
 */
int v4l2_capture_dequeue(V4l2Capture *capture,
                         CameraRawFrame *frame,
                         int timeout_ms);

/* 将处理完成的缓冲区重新QBUF给V4L2驱动。 */
int v4l2_capture_queue(V4l2Capture *capture, uint32_t buffer_index);

/* 执行STREAMOFF但保留设备和已映射缓冲区。 */
int v4l2_capture_stop(V4l2Capture *capture);

/* 停止采集、解除MMAP、关闭DMA-BUF和设备节点，但不释放对象。 */
void v4l2_capture_close(V4l2Capture *capture);

/* 返回最近一次底层操作的错误文本。 */
const char *v4l2_capture_last_error(const V4l2Capture *capture);

/* 返回VIDIOC_S_FMT协商后的实际图像尺寸。 */
uint32_t v4l2_capture_width(const V4l2Capture *capture);
uint32_t v4l2_capture_height(const V4l2Capture *capture);

#ifdef __cplusplus
}
#endif

#endif
