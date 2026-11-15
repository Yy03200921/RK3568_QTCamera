#ifndef RK3568_CAMERA_CAPTURE_WORKER_HPP
#define RK3568_CAMERA_CAPTURE_WORKER_HPP

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

struct V4l2Capture;
struct CameraRgbFrame;
class RgaConverter;

/*
 * 摄像头业务工作对象。实例被移动到独立QThread，V4L2等待、RGA转换和
 * 照片保存都不阻塞Qt主界面线程。公共操作通过queued signal调用。
 */
class CaptureWorker final : public QObject
{
    Q_OBJECT

public:
    explicit CaptureWorker(QObject *parent = nullptr);
    ~CaptureWorker() override;

public slots:
    void startCamera();
    void stopCamera();
    void requestPhoto();
    void setPreviewSize(const QSize &size);

signals:
    void frameReady(const QImage &image);
    void cameraStarted(const QSize &capture_size);
    void cameraStopped();
    void photoSaved(const QString &path, const QImage &thumbnail);
    void captureFailed(const QString &message);

private:
    bool openCamera(QString *error_message);
    void captureNextFrame();
    void scheduleNextFrame();
    void closeCamera();
    QImage copyFrame(const CameraRgbFrame &frame) const;
    bool convertPreview(const struct CameraRawFrame &raw,
                        QImage *image, QString *error_message);
    bool convertPhoto(const struct CameraRawFrame &raw,
                      QImage *image, QString *error_message);
    bool savePhoto(const QImage &image, QString *saved_path) const;

    V4l2Capture *capture_ = nullptr;
    std::unique_ptr<RgaConverter> preview_converter_;
    std::unique_ptr<RgaConverter> photo_converter_;
    QSize preview_size_{960, 540};
    bool running_ = false;
    bool photo_pending_ = false;
};

#endif
