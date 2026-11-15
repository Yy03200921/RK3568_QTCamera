#include "mainwindow.h"

#include "camera/capture_worker.hpp"
#include "ui_mainwindow.h"

#include <QFileInfo>
#include <QIcon>
#include <QMetaObject>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QThread>
#include <QTimer>

namespace {
constexpr int kMinimumPreviewWidth = 320;
constexpr int kMinimumPreviewHeight = 180;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      ui_(new Ui::MainWindow),
      capture_thread_(new QThread(this)),
      capture_worker_(new CaptureWorker())
{
    ui_->setupUi(this);
    updateCameraControls(CameraControlState::Stopped);

    // Worker只在采集线程运行，主线程通过queued signal发出业务命令。
    capture_worker_->moveToThread(capture_thread_);
    connect(capture_thread_, &QThread::finished,
            capture_worker_, &QObject::deleteLater);
    connect(capture_thread_, &QThread::started,
            capture_worker_, &CaptureWorker::startCamera);
    connect(this, &MainWindow::requestStart,
            capture_worker_, &CaptureWorker::startCamera,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestStop,
            capture_worker_, &CaptureWorker::stopCamera,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestPhoto,
            capture_worker_, &CaptureWorker::requestPhoto,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestPreviewSize,
            capture_worker_, &CaptureWorker::setPreviewSize,
            Qt::QueuedConnection);

    connect(capture_worker_, &CaptureWorker::frameReady,
            this, &MainWindow::showFrame, Qt::QueuedConnection);
    connect(capture_worker_, &CaptureWorker::cameraStarted,
            this, &MainWindow::handleCameraStarted,
            Qt::QueuedConnection);
    connect(capture_worker_, &CaptureWorker::cameraStopped,
            this, &MainWindow::handleCameraStopped,
            Qt::QueuedConnection);
    connect(capture_worker_, &CaptureWorker::photoSaved,
            this, &MainWindow::handlePhotoSaved,
            Qt::QueuedConnection);
    connect(capture_worker_, &CaptureWorker::captureFailed,
            this, &MainWindow::showError, Qt::QueuedConnection);

    connect(ui_->shutterButton, &QPushButton::clicked,
            this, &MainWindow::takePhoto);
    connect(ui_->startStopButton, &QPushButton::clicked,
            this, &MainWindow::toggleCamera);
    connect(ui_->exitButton, &QPushButton::clicked,
            this, &QWidget::close);
    connect(ui_->backButton, &QPushButton::clicked,
            this, &MainWindow::hidePhotoPreview);
    connect(ui_->thumbnailButton, &QPushButton::clicked,
            this, &MainWindow::showLatestPhoto);

    capture_thread_->start();
    QTimer::singleShot(0, this, &MainWindow::updatePreviewSize);
}

MainWindow::~MainWindow()
{
    if (capture_thread_->isRunning()) {
        QMetaObject::invokeMethod(capture_worker_, "stopCamera",
                                  Qt::BlockingQueuedConnection);
        capture_thread_->quit();
        capture_thread_->wait();
    }
    delete ui_;
}

void MainWindow::toggleCamera()
{
    ui_->startStopButton->setEnabled(false);
    if (camera_running_) {
        ui_->statusLabel->setText(QStringLiteral("正在停止摄像头..."));
        emit requestStop();
    } else {
        ui_->statusLabel->setText(QStringLiteral("正在启动摄像头..."));
        emit requestStart();
    }
}

void MainWindow::takePhoto()
{
    if (!camera_running_) {
        return;
    }
    ui_->shutterButton->setEnabled(false);
    ui_->statusLabel->setText(QStringLiteral("正在保存照片..."));
    emit requestPhoto();
}

void MainWindow::showFrame(const QImage &image)
{
    if (image.isNull()) {
        return;
    }
    if (ui_->pageStack->currentWidget() == ui_->cameraPage) {
        // 图像已由RGA缩放到控件需要的尺寸，此处只负责显示。
        ui_->previewLabel->setPixmap(QPixmap::fromImage(image));
    }

    frame_count_++;
    const qint64 elapsed = fps_timer_.isValid() ? fps_timer_.elapsed() : 0;
    if (elapsed >= 1000) {
        const double fps = static_cast<double>(frame_count_) * 1000.0 /
                           static_cast<double>(elapsed);
        ui_->statusLabel->setText(
            QStringLiteral("%1×%2  |  %3 FPS  |  RGA 预览")
                .arg(capture_size_.width())
                .arg(capture_size_.height())
                .arg(fps, 0, 'f', 1));
        frame_count_ = 0;
        fps_timer_.restart();
    }
}

void MainWindow::handleCameraStarted(const QSize &capture_size)
{
    capture_size_ = capture_size;
    camera_running_ = true;
    frame_count_ = 0;
    fps_timer_.start();
    updateCameraControls(CameraControlState::Running);
    updatePreviewSize();
    ui_->statusLabel->setText(
        QStringLiteral("摄像头已启动：%1×%2")
            .arg(capture_size.width())
            .arg(capture_size.height()));
}

void MainWindow::handleCameraStopped()
{
    camera_running_ = false;
    updateCameraControls(CameraControlState::Stopped);
    ui_->statusLabel->setText(QStringLiteral("摄像头已停止"));
}

void MainWindow::handlePhotoSaved(const QString &path,
                                  const QImage &photo)
{
    ui_->shutterButton->setEnabled(camera_running_);
    latest_photo_ = photo;
    latest_photo_path_ = path;
    const QSize icon_size = ui_->thumbnailButton->size() - QSize(8, 8);
    const QPixmap pixmap = QPixmap::fromImage(photo).scaled(
        icon_size, Qt::KeepAspectRatio,
        Qt::SmoothTransformation);
    ui_->thumbnailButton->setText(QString());
    ui_->thumbnailButton->setIcon(QIcon(pixmap));
    ui_->thumbnailButton->setIconSize(pixmap.size());
    ui_->photoNameLabel->setText(QFileInfo(path).fileName());
    ui_->statusLabel->setText(QStringLiteral("照片已保存：") + path);
}

void MainWindow::showError(const QString &message)
{
    ui_->shutterButton->setEnabled(camera_running_);
    ui_->startStopButton->setEnabled(true);
    ui_->statusLabel->setText(QStringLiteral("错误：") + message);
}

void MainWindow::showLatestPhoto()
{
    if (latest_photo_.isNull()) {
        ui_->statusLabel->setText(QStringLiteral("还没有可预览的照片"));
        return;
    }
    ui_->previewPhotoNameLabel->setText(
        QFileInfo(latest_photo_path_).fileName());
    ui_->pageStack->setCurrentWidget(ui_->photoPage);
    QTimer::singleShot(0, this, &MainWindow::updatePhotoPreview);
}

void MainWindow::hidePhotoPreview()
{
    ui_->pageStack->setCurrentWidget(ui_->cameraPage);
    updatePreviewSize();
}

void MainWindow::updatePhotoPreview()
{
    if (latest_photo_.isNull()) {
        return;
    }
    const QSize available = ui_->photoPreviewLabel->contentsRect().size();
    if (available.isEmpty()) {
        return;
    }
    const QPixmap pixmap = QPixmap::fromImage(latest_photo_).scaled(
        available, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui_->photoPreviewLabel->setPixmap(pixmap);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (ui_->pageStack->currentWidget() == ui_->photoPage) {
        updatePhotoPreview();
    } else {
        updatePreviewSize();
    }
}

void MainWindow::updatePreviewSize()
{
    const QSize available = ui_->previewLabel->contentsRect().size();
    if (available.width() < kMinimumPreviewWidth ||
        available.height() < kMinimumPreviewHeight) {
        return;
    }

    // 保持摄像头宽高比，并把偶数尺寸交给RGA执行硬件缩放。
    const int source_width = qMax(1, capture_size_.width());
    const int source_height = qMax(1, capture_size_.height());
    int width = available.width();
    int height = width * source_height / source_width;
    if (height > available.height()) {
        height = available.height();
        width = height * source_width / source_height;
    }
    width &= ~1;
    height &= ~1;
    const QSize requested(width, height);
    if (requested != requested_preview_size_) {
        requested_preview_size_ = requested;
        emit requestPreviewSize(requested);
    }
}

void MainWindow::updateCameraControls(CameraControlState state)
{
    const bool running = state == CameraControlState::Running;
    ui_->startStopButton->setEnabled(true);
    ui_->startStopButton->setText(
        running ? QStringLiteral("停止") : QStringLiteral("启动"));
    ui_->shutterButton->setEnabled(running);
}
