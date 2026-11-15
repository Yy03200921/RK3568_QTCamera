#ifndef RK3568_CAMERA_MAINWINDOW_H
#define RK3568_CAMERA_MAINWINDOW_H

#include <QElapsedTimer>
#include <QImage>
#include <QMainWindow>
#include <QSize>
#include <QString>

class CaptureWorker;
class QResizeEvent;
class QThread;

namespace Ui {
class MainWindow;
}

/* Qt显示层：只负责用户交互、页面状态和QImage显示。 */
class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void toggleCamera();
    void takePhoto();
    void showFrame(const QImage &image);
    void handleCameraStarted(const QSize &capture_size);
    void handleCameraStopped();
    void handlePhotoSaved(const QString &path, const QImage &photo);
    void showError(const QString &message);

signals:
    void requestStart();
    void requestStop();
    void requestPhoto();
    void requestPreviewSize(const QSize &size);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class CameraControlState {
        Stopped,
        Running
    };

    /* UI只处理显示和交互，不直接调用V4L2或RGA。 */
    void showLatestPhoto();
    void hidePhotoPreview();
    void updatePhotoPreview();
    void updatePreviewSize();
    void updateCameraControls(CameraControlState state);

    Ui::MainWindow *ui_ = nullptr;
    QThread *capture_thread_ = nullptr;
    CaptureWorker *capture_worker_ = nullptr;
    QSize capture_size_{16, 9};
    QSize requested_preview_size_;
    QImage latest_photo_;
    QString latest_photo_path_;
    QElapsedTimer fps_timer_;
    int frame_count_ = 0;
    bool camera_running_ = false;
};

#endif
