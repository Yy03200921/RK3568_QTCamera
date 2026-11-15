#include "camera/capture_worker.hpp"

#include "camera/camera_frame.h"
#include "camera/rga_converter.hpp"
#include "camera/v4l2_capture.h"

#include <QDateTime>
#include <QDir>
#include <QMetaType>
#include <QStandardPaths>
#include <QTimer>

namespace {
constexpr uint32_t kDefaultCaptureWidth = 1280U;
constexpr uint32_t kDefaultCaptureHeight = 720U;
constexpr uint32_t kCaptureBufferCount = 4U;
constexpr int kCaptureTimeoutMs = 500;
constexpr int kMinDimension = 160;
constexpr int kMaxCaptureWidth = 3840;
constexpr int kMaxCaptureHeight = 2160;

uint32_t environmentDimension(const char *name, uint32_t fallback,
                              int maximum)
{
    bool valid = false;
    const int value = qEnvironmentVariableIntValue(name, &valid);
    if (!valid || value < kMinDimension || value > maximum) {
        return fallback;
    }
    return static_cast<uint32_t>(value);
}

QString photoDirectory()
{
    QString path = qEnvironmentVariable("RK_CAMERA_PHOTO_DIR");
    if (!path.isEmpty()) {
        return path;
    }
    path = QStandardPaths::writableLocation(
        QStandardPaths::PicturesLocation);
    if (path.isEmpty()) {
        path = QDir::homePath() + QStringLiteral("/Pictures");
    }
    return path + QStringLiteral("/RKCamera");
}
}

CaptureWorker::CaptureWorker(QObject *parent)
    : QObject(parent),
      preview_converter_(new RgaConverter()),
      photo_converter_(new RgaConverter())
{
    // 跨线程信号传递图像和尺寸前，显式注册Qt元类型。
    qRegisterMetaType<QImage>("QImage");
    qRegisterMetaType<QSize>("QSize");
}

CaptureWorker::~CaptureWorker()
{
    closeCamera();
}

bool CaptureWorker::openCamera(QString *error_message)
{
    // 环境变量只负责运行时配置，底层采集流程不依赖Qt界面。
    const QString device = qEnvironmentVariable(
        "RK_CAMERA_DEVICE", QStringLiteral("/dev/video0"));
    const QByteArray device_bytes = device.toLocal8Bit();
    V4l2CaptureConfig config{};
    config.device_path = device_bytes.constData();
    config.width = environmentDimension(
        "RK_CAMERA_WIDTH", kDefaultCaptureWidth, kMaxCaptureWidth);
    config.height = environmentDimension(
        "RK_CAMERA_HEIGHT", kDefaultCaptureHeight, kMaxCaptureHeight);
    config.pixel_format = CAMERA_PIXEL_FORMAT_UYVY;
    config.buffer_count = kCaptureBufferCount;

    capture_ = v4l2_capture_create(&config);
    if (capture_ == nullptr) {
        *error_message = QStringLiteral("创建 V4L2 采集对象失败");
        return false;
    }
    if (v4l2_capture_open(capture_) != 0 ||
        v4l2_capture_start(capture_) != 0) {
        *error_message = QString::fromLocal8Bit(
            v4l2_capture_last_error(capture_));
        closeCamera();
        return false;
    }
    return true;
}

void CaptureWorker::startCamera()
{
    if (running_) {
        return;
    }
    QString error_message;
    if (!openCamera(&error_message)) {
        emit captureFailed(error_message);
        return;
    }

    running_ = true;
    photo_pending_ = false;
    emit cameraStarted(QSize(
        static_cast<int>(v4l2_capture_width(capture_)),
        static_cast<int>(v4l2_capture_height(capture_))));
    scheduleNextFrame();
}

void CaptureWorker::stopCamera()
{
    const bool was_active = running_ || capture_ != nullptr;
    running_ = false;
    photo_pending_ = false;
    closeCamera();
    if (was_active) {
        emit cameraStopped();
    }
}

void CaptureWorker::requestPhoto()
{
    if (!running_) {
        emit captureFailed(QStringLiteral("摄像头尚未启动"));
        return;
    }
    photo_pending_ = true;
}

void CaptureWorker::setPreviewSize(const QSize &size)
{
    if (size.width() < kMinDimension || size.height() < kMinDimension) {
        return;
    }
    const int width = size.width() & ~1;
    const int height = size.height() & ~1;
    preview_size_ = QSize(width, height);
}

void CaptureWorker::scheduleNextFrame()
{
    if (running_) {
        // 每次只处理一帧，再把控制权还给工作线程事件循环。
        QTimer::singleShot(0, this, &CaptureWorker::captureNextFrame);
    }
}

QImage CaptureWorker::copyFrame(const CameraRgbFrame &frame) const
{
    if (frame.data == nullptr || frame.width == 0U || frame.height == 0U) {
        return QImage();
    }
    // view不拥有RGA缓冲区，copy后QImage才拥有独立且线程安全的数据。
    const QImage view(frame.data, static_cast<int>(frame.width),
                      static_cast<int>(frame.height),
                      static_cast<int>(frame.bytes_per_line),
                      QImage::Format_RGB888);
    return view.copy();
}

bool CaptureWorker::convertPreview(const CameraRawFrame &raw,
                                   QImage *image,
                                   QString *error_message)
{
    CameraRgbFrame frame{};
    const char *rga_error = nullptr;
    if (!preview_converter_->convert(
            raw, static_cast<uint32_t>(preview_size_.width()),
            static_cast<uint32_t>(preview_size_.height()),
            &frame, &rga_error)) {
        *error_message = QString::fromLocal8Bit(
            rga_error == nullptr ? "RGA 预览转换失败" : rga_error);
        return false;
    }
    *image = copyFrame(frame);
    if (image->isNull()) {
        *error_message = QStringLiteral("RGA 预览图像为空");
        return false;
    }
    return true;
}

bool CaptureWorker::convertPhoto(const CameraRawFrame &raw,
                                 QImage *image,
                                 QString *error_message)
{
    CameraRgbFrame frame{};
    const char *rga_error = nullptr;
    if (!photo_converter_->convert(
            raw, raw.width, raw.height, &frame, &rga_error)) {
        *error_message = QString::fromLocal8Bit(
            rga_error == nullptr ? "RGA 拍照转换失败" : rga_error);
        return false;
    }
    *image = copyFrame(frame);
    if (image->isNull()) {
        *error_message = QStringLiteral("RGA 拍照图像为空");
        return false;
    }
    return true;
}

bool CaptureWorker::savePhoto(const QImage &image,
                              QString *saved_path) const
{
    if (image.isNull() || saved_path == nullptr) {
        return false;
    }
    const QString directory = photoDirectory();
    if (!QDir().mkpath(directory)) {
        return false;
    }

    const QString base_name =
        QStringLiteral("IMG_") +
        QDateTime::currentDateTime().toString(
            QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    QString path = directory + QLatin1Char('/') + base_name +
                   QStringLiteral(".jpg");
    if (!image.save(path, "JPG", 92)) {
        path = directory + QLatin1Char('/') + base_name +
               QStringLiteral(".bmp");
        if (!image.save(path, "BMP")) {
            return false;
        }
    }
    *saved_path = path;
    return true;
}

void CaptureWorker::captureNextFrame()
{
    if (!running_ || capture_ == nullptr) {
        return;
    }

    CameraRawFrame raw{};
    const int dequeue_result = v4l2_capture_dequeue(
        capture_, &raw, kCaptureTimeoutMs);
    if (dequeue_result == 1) {
        scheduleNextFrame();
        return;
    }
    if (dequeue_result != 0) {
        const QString message = QString::fromLocal8Bit(
            v4l2_capture_last_error(capture_));
        stopCamera();
        emit captureFailed(message);
        return;
    }

    // 同一个V4L2缓冲区先生成预览，收到拍照请求时再生成全尺寸照片。
    QImage preview_image;
    QString preview_error;
    const bool preview_ok = convertPreview(
        raw, &preview_image, &preview_error);

    const bool take_photo = photo_pending_;
    photo_pending_ = false;
    QImage photo_image;
    QString photo_error;
    bool photo_ok = true;
    if (take_photo) {
        photo_ok = convertPhoto(raw, &photo_image, &photo_error);
    }

    // RGA和QImage复制完成后立即归还V4L2缓冲区，避免阻塞采集队列。
    if (v4l2_capture_queue(capture_, raw.buffer_index) != 0) {
        const QString message = QString::fromLocal8Bit(
            v4l2_capture_last_error(capture_));
        stopCamera();
        emit captureFailed(message);
        return;
    }

    if (!preview_ok) {
        stopCamera();
        emit captureFailed(preview_error);
        return;
    }
    emit frameReady(preview_image);

    if (take_photo && !photo_ok) {
        emit captureFailed(photo_error);
    } else if (take_photo) {
        QString path;
        if (savePhoto(photo_image, &path)) {
            emit photoSaved(path, photo_image);
        } else {
            emit captureFailed(QStringLiteral("照片保存失败"));
        }
    }
    scheduleNextFrame();
}

void CaptureWorker::closeCamera()
{
    if (capture_ != nullptr) {
        v4l2_capture_destroy(capture_);
        capture_ = nullptr;
    }
}
