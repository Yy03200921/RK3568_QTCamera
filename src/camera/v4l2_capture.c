#define _GNU_SOURCE
#include "camera/v4l2_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAMERA_MAX_PLANES 2U
#define CAMERA_MIN_BUFFERS 3U
#define CAMERA_MAX_BUFFERS 8U
#define CAMERA_ERR_SIZE 192U

typedef struct CaptureBuffer {
    void *address;
    size_t length;
    int dma_buf_fd;
    int queued;
} CaptureBuffer;

struct V4l2Capture {
    char *device_path;
    int fd;
    enum v4l2_buf_type type;
    CaptureBuffer *buffers;
    uint32_t buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t size_image;
    uint32_t requested_width;
    uint32_t requested_height;
    uint32_t requested_buffers;
    CameraPixelFormat requested_format;
    CameraPixelFormat pixel_format;
    char error[CAMERA_ERR_SIZE];
};

static int retry_ioctl(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void set_error(V4l2Capture *capture, const char *operation)
{
    (void)snprintf(capture->error, sizeof(capture->error), "%s: %s",
                   operation, strerror(errno));
}

static uint32_t to_v4l2_format(CameraPixelFormat format)
{
    if (format == CAMERA_PIXEL_FORMAT_UYVY) {
        return V4L2_PIX_FMT_UYVY;
    }
    if (format == CAMERA_PIXEL_FORMAT_YUYV) {
        return V4L2_PIX_FMT_YUYV;
    }
    return 0U;
}

static CameraPixelFormat from_v4l2_format(uint32_t format)
{
    if (format == V4L2_PIX_FMT_UYVY) {
        return CAMERA_PIXEL_FORMAT_UYVY;
    }
    if (format == V4L2_PIX_FMT_YUYV) {
        return CAMERA_PIXEL_FORMAT_YUYV;
    }
    return CAMERA_PIXEL_FORMAT_UNKNOWN;
}

static int configure_format(V4l2Capture *capture,
                            const V4l2CaptureConfig *config)
{
    struct v4l2_format format;
    struct v4l2_fmtdesc description;
    uint32_t requested = to_v4l2_format(config->pixel_format);
    int supported = 0;

    /* 先枚举格式，避免驱动静默替换成业务层不支持的像素格式。 */
    memset(&description, 0, sizeof(description));
    description.type = capture->type;
    while (retry_ioctl(capture->fd, VIDIOC_ENUM_FMT, &description) == 0) {
        if (description.pixelformat == requested) {
            supported = 1;
            break;
        }
        description.index++;
    }
    if (!supported) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "requested V4L2 pixel format is unsupported");
        return -1;
    }

    memset(&format, 0, sizeof(format));
    format.type = capture->type;
    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = requested;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (retry_ioctl(capture->fd, VIDIOC_S_FMT, &format) < 0) {
        set_error(capture, "VIDIOC_S_FMT");
        return -1;
    }
    if (format.fmt.pix_mp.num_planes != 1U ||
        from_v4l2_format(format.fmt.pix_mp.pixelformat) ==
            CAMERA_PIXEL_FORMAT_UNKNOWN) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "only single-plane UYVY/YUYV capture is supported");
        return -1;
    }

    capture->width = format.fmt.pix_mp.width;
    capture->height = format.fmt.pix_mp.height;
    capture->pixel_format =
        from_v4l2_format(format.fmt.pix_mp.pixelformat);
    capture->bytes_per_line = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    capture->size_image = format.fmt.pix_mp.plane_fmt[0].sizeimage;
    return 0;
}

static void release_buffers(V4l2Capture *capture)
{
    uint32_t index;
    if (capture->buffers == NULL) {
        return;
    }
    for (index = 0U; index < capture->buffer_count; index++) {
        if (capture->buffers[index].address != MAP_FAILED &&
            capture->buffers[index].address != NULL) {
            (void)munmap(capture->buffers[index].address,
                         capture->buffers[index].length);
        }
        if (capture->buffers[index].dma_buf_fd >= 0) {
            (void)close(capture->buffers[index].dma_buf_fd);
        }
    }
    free(capture->buffers);
    capture->buffers = NULL;
    capture->buffer_count = 0U;
}

static int allocate_buffers(V4l2Capture *capture, uint32_t requested_count)
{
    struct v4l2_requestbuffers request;
    uint32_t index;

    /* MMAP用于管理V4L2缓冲区生命周期，DMA-BUF用于交给RGA。 */
    memset(&request, 0, sizeof(request));
    request.type = capture->type;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = requested_count;
    if (retry_ioctl(capture->fd, VIDIOC_REQBUFS, &request) < 0) {
        set_error(capture, "VIDIOC_REQBUFS");
        return -1;
    }
    if (request.count < CAMERA_MIN_BUFFERS ||
        request.count > CAMERA_MAX_BUFFERS) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "driver returned an invalid buffer count");
        return -1;
    }
    capture->buffers = calloc(request.count, sizeof(*capture->buffers));
    if (capture->buffers == NULL) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "buffer metadata allocation failed");
        return -1;
    }
    capture->buffer_count = request.count;
    for (index = 0U; index < request.count; index++) {
        capture->buffers[index].address = MAP_FAILED;
        capture->buffers[index].dma_buf_fd = -1;
    }

    for (index = 0U; index < request.count; index++) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[CAMERA_MAX_PLANES];
        struct v4l2_exportbuffer export_buffer;

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = capture->type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1U;
        buffer.m.planes = planes;
        if (retry_ioctl(capture->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            set_error(capture, "VIDIOC_QUERYBUF");
            return -1;
        }
        if (buffer.length != 1U || planes[0].length < capture->size_image) {
            (void)snprintf(capture->error, sizeof(capture->error),
                           "driver buffer layout is unsupported");
            return -1;
        }
        capture->buffers[index].length = planes[0].length;
        capture->buffers[index].address = mmap(
            NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
            capture->fd, (off_t)planes[0].m.mem_offset);
        if (capture->buffers[index].address == MAP_FAILED) {
            set_error(capture, "mmap");
            return -1;
        }

        /* RGA 通过 DMA-BUF 访问同一块 V4L2 缓冲区，避免 CPU 拷贝原始帧。 */
        memset(&export_buffer, 0, sizeof(export_buffer));
        export_buffer.type = capture->type;
        export_buffer.index = index;
        export_buffer.plane = 0U;
        export_buffer.flags = O_CLOEXEC;
        if (retry_ioctl(capture->fd, VIDIOC_EXPBUF, &export_buffer) < 0) {
            set_error(capture, "VIDIOC_EXPBUF (RGA DMA-BUF import required)");
            return -1;
        }
        capture->buffers[index].dma_buf_fd = export_buffer.fd;
    }
    return 0;
}

V4l2Capture *v4l2_capture_create(const V4l2CaptureConfig *config)
{
    V4l2Capture *capture;
    uint32_t format;
    if (config == NULL || config->device_path == NULL ||
        config->device_path[0] == '\0' ||
        config->width == 0U || config->height == 0U ||
        config->buffer_count < CAMERA_MIN_BUFFERS ||
        config->buffer_count > CAMERA_MAX_BUFFERS) {
        return NULL;
    }
    format = to_v4l2_format(config->pixel_format);
    if (format == 0U) {
        return NULL;
    }
    capture = calloc(1U, sizeof(*capture));
    if (capture == NULL) {
        return NULL;
    }
    capture->fd = -1;
    capture->device_path = strdup(config->device_path);
    if (capture->device_path == NULL) {
        free(capture);
        return NULL;
    }
    capture->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    capture->requested_width = config->width;
    capture->requested_height = config->height;
    capture->requested_buffers = config->buffer_count;
    capture->requested_format = config->pixel_format;
    capture->error[0] = '\0';
    return capture;
}

int v4l2_capture_open(V4l2Capture *capture)
{
    struct v4l2_capability capability;
    uint32_t device_caps;
    V4l2CaptureConfig config;
    if (capture == NULL || capture->fd >= 0) {
        return -1;
    }
    capture->fd = open(capture->device_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (capture->fd < 0) {
        set_error(capture, "open video device");
        return -1;
    }
    memset(&capability, 0, sizeof(capability));
    if (retry_ioctl(capture->fd, VIDIOC_QUERYCAP, &capability) < 0) {
        set_error(capture, "VIDIOC_QUERYCAP");
        goto fail;
    }
    device_caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U
                      ? capability.device_caps
                      : capability.capabilities;
    if ((device_caps & V4L2_CAP_STREAMING) == 0U ||
        (device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0U) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "device lacks multi-plane streaming capture");
        goto fail;
    }
    memset(&config, 0, sizeof(config));
    config.width = capture->requested_width;
    config.height = capture->requested_height;
    config.pixel_format = capture->requested_format;
    config.buffer_count = capture->requested_buffers;
    if (configure_format(capture, &config) < 0 ||
        allocate_buffers(capture, config.buffer_count) < 0) {
        goto fail;
    }
    return 0;
fail:
    v4l2_capture_close(capture);
    return -1;
}

int v4l2_capture_start(V4l2Capture *capture)
{
    uint32_t index;
    if (capture == NULL || capture->fd < 0 || capture->buffers == NULL) {
        return -1;
    }
    /* STREAMON之前必须先把所有可用缓冲区交给驱动。 */
    for (index = 0U; index < capture->buffer_count; index++) {
        struct v4l2_buffer buffer;
        struct v4l2_plane plane[CAMERA_MAX_PLANES];
        memset(&buffer, 0, sizeof(buffer));
        memset(plane, 0, sizeof(plane));
        buffer.type = capture->type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1U;
        buffer.m.planes = plane;
        plane[0].length = capture->buffers[index].length;
        if (retry_ioctl(capture->fd, VIDIOC_QBUF, &buffer) < 0) {
            set_error(capture, "VIDIOC_QBUF");
            (void)v4l2_capture_stop(capture);
            return -1;
        }
        capture->buffers[index].queued = 1;
    }
    if (retry_ioctl(capture->fd, VIDIOC_STREAMON, &capture->type) < 0) {
        set_error(capture, "VIDIOC_STREAMON");
        (void)v4l2_capture_stop(capture);
        return -1;
    }
    return 0;
}

int v4l2_capture_dequeue(V4l2Capture *capture,
                         CameraRawFrame *frame,
                         int timeout_ms)
{
    struct pollfd descriptor;
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[CAMERA_MAX_PLANES];
    int poll_result;
    if (capture == NULL || capture->fd < 0 || frame == NULL ||
        timeout_ms < 0) {
        return -1;
    }
    /* poll避免在无帧时空转占满CPU，并为停止操作保留超时出口。 */
    descriptor.fd = capture->fd;
    descriptor.events = POLLIN | POLLPRI;
    descriptor.revents = 0;
    do {
        poll_result = poll(&descriptor, 1U, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        return 1;
    }
    if (poll_result < 0 ||
        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        set_error(capture, "poll video device");
        return -1;
    }
    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.type = capture->type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.length = 1U;
    buffer.m.planes = planes;
    planes[0].length = (uint32_t)capture->buffers[0].length;
    if (retry_ioctl(capture->fd, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return 1;
        }
        set_error(capture, "VIDIOC_DQBUF");
        return -1;
    }
    if (buffer.index >= capture->buffer_count || buffer.length != 1U ||
        planes[0].data_offset > planes[0].bytesused ||
        planes[0].bytesused > capture->buffers[buffer.index].length) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "driver returned invalid frame metadata");
        return -1;
    }
    if (planes[0].data_offset != 0U) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "non-zero V4L2 data offset is unsupported");
        return -1;
    }
    if (capture->height > 0U &&
        capture->bytes_per_line > SIZE_MAX / capture->height) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "frame size calculation overflow");
        return -1;
    }
    if ((size_t)capture->bytes_per_line * capture->height >
        capture->buffers[buffer.index].length) {
        (void)snprintf(capture->error, sizeof(capture->error),
                       "negotiated frame layout exceeds V4L2 buffer");
        return -1;
    }
    capture->buffers[buffer.index].queued = 0;
    /* DQBUF 后帧由本模块借出；调用方完成 RGA 后必须 QBUF 归还。 */
    memset(frame, 0, sizeof(*frame));
    frame->data = (const uint8_t *)capture->buffers[buffer.index].address +
                  planes[0].data_offset;
    frame->data_size = planes[0].bytesused - planes[0].data_offset;
    frame->width = capture->width;
    frame->height = capture->height;
    frame->bytes_per_line = capture->bytes_per_line;
    frame->sequence = buffer.sequence;
    frame->pixel_format = capture->pixel_format;
    frame->buffer_index = buffer.index;
    frame->buffer_size = capture->buffers[buffer.index].length;
    frame->dma_buf_fd = capture->buffers[buffer.index].dma_buf_fd;
    return 0;
}

int v4l2_capture_queue(V4l2Capture *capture, uint32_t buffer_index)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane plane[CAMERA_MAX_PLANES];
    if (capture == NULL || buffer_index >= capture->buffer_count ||
        capture->buffers[buffer_index].queued != 0) {
        return -1;
    }
    memset(&buffer, 0, sizeof(buffer));
    memset(plane, 0, sizeof(plane));
    buffer.type = capture->type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = buffer_index;
    buffer.length = 1U;
    buffer.m.planes = plane;
    plane[0].length = capture->buffers[buffer_index].length;
    if (retry_ioctl(capture->fd, VIDIOC_QBUF, &buffer) < 0) {
        set_error(capture, "VIDIOC_QBUF");
        return -1;
    }
    capture->buffers[buffer_index].queued = 1;
    return 0;
}

int v4l2_capture_stop(V4l2Capture *capture)
{
    uint32_t index;
    enum v4l2_buf_type type;
    if (capture == NULL || capture->fd < 0) {
        return 0;
    }
    type = capture->type;
    if (retry_ioctl(capture->fd, VIDIOC_STREAMOFF, &type) < 0 &&
        errno != EINVAL) {
        set_error(capture, "VIDIOC_STREAMOFF");
        return -1;
    }
    for (index = 0U; index < capture->buffer_count; index++) {
        capture->buffers[index].queued = 0;
    }
    return 0;
}

void v4l2_capture_close(V4l2Capture *capture)
{
    struct v4l2_requestbuffers request;
    if (capture == NULL) {
        return;
    }
    if (capture->fd >= 0) {
        (void)v4l2_capture_stop(capture);
        release_buffers(capture);
        memset(&request, 0, sizeof(request));
        request.type = capture->type;
        request.memory = V4L2_MEMORY_MMAP;
        (void)retry_ioctl(capture->fd, VIDIOC_REQBUFS, &request);
        (void)close(capture->fd);
        capture->fd = -1;
    } else {
        release_buffers(capture);
    }
}

void v4l2_capture_destroy(V4l2Capture *capture)
{
    if (capture == NULL) {
        return;
    }
    v4l2_capture_close(capture);
    free(capture->device_path);
    free(capture);
}

const char *v4l2_capture_last_error(const V4l2Capture *capture)
{
    return capture == NULL ? "invalid capture object" : capture->error;
}

uint32_t v4l2_capture_width(const V4l2Capture *capture)
{
    return capture == NULL ? 0U : capture->width;
}

uint32_t v4l2_capture_height(const V4l2Capture *capture)
{
    return capture == NULL ? 0U : capture->height;
}

