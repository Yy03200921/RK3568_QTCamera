#include "mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#ifdef RK_CAMERA_OPENGL_VIEWPORT
#include <QOpenGLWidget>
#endif
#include <QPointF>
#include <QScreen>
#include <QSize>

namespace {
constexpr int kDefaultRotationDegrees = 90;
constexpr int kFallbackScreenWidth = 1920;
constexpr int kFallbackScreenHeight = 1080;

int normalizeRotation(int rotation)
{
    int normalized = rotation % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    if (normalized == 0 || normalized == 90 ||
        normalized == 180 || normalized == 270) {
        return normalized;
    }
    qWarning() << "界面旋转只支持 0、90、180、270 度，使用默认值 90";
    return kDefaultRotationDegrees;
}

int configuredRotation()
{
    bool valid = false;
    const int rotation = qEnvironmentVariableIntValue(
        "RK_CAMERA_UI_ROTATION", &valid);
    return normalizeRotation(valid ? rotation : kDefaultRotationDegrees);
}

QSize screenSize(const QScreen *screen)
{
    if (screen == nullptr) {
        return QSize(kFallbackScreenWidth, kFallbackScreenHeight);
    }
    return screen->geometry().size();
}

QSize unrotatedContentSize(const QSize &output_size, int rotation)
{
    if (rotation == 90 || rotation == 270) {
        return output_size.transposed();
    }
    return output_size;
}

QPointF rotatedContentPosition(const QSize &output_size, int rotation)
{
    switch (rotation) {
    case 90:
        return QPointF(output_size.width(), 0.0);
    case 180:
        return QPointF(output_size.width(), output_size.height());
    case 270:
        return QPointF(0.0, output_size.height());
    default:
        return QPointF(0.0, 0.0);
    }
}

void configureDisplay(QGraphicsView *view, const QSize &output_size)
{
    view->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    view->setFrameShape(QFrame::NoFrame);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    view->setSceneRect(0.0, 0.0,
                       output_size.width(), output_size.height());
    view->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    view->setStyleSheet(QStringLiteral("background:#080a0d; border:0;"));
#ifdef RK_CAMERA_OPENGL_VIEWPORT
    // 使用 GPU 合成旋转后的 Qt 场景，避免 CPU 逐像素旋转整个界面。
    view->setViewport(new QOpenGLWidget(view));
#endif
    view->resize(output_size);
}

void addRotatedCamera(QGraphicsScene *scene,
                      const QSize &output_size,
                      int rotation)
{
    const QSize content_size =
        unrotatedContentSize(output_size, rotation);
    MainWindow *camera_window = new MainWindow();
    camera_window->setFixedSize(content_size);

    // Proxy负责绘制变换和输入事件逆变换，因此按钮触摸位置会随界面旋转。
    QGraphicsProxyWidget *proxy =
        scene->addWidget(camera_window, Qt::Widget);
    proxy->setCacheMode(QGraphicsItem::NoCache);
    proxy->setTransformOriginPoint(0.0, 0.0);
    proxy->setRotation(rotation);
    proxy->setPos(rotatedContentPosition(output_size, rotation));
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("RK3568 Camera"));
    app.setOrganizationName(QStringLiteral("RK3568"));

    QScreen *screen = app.primaryScreen();
    const QSize output_size = screenSize(screen);
    const int rotation = configuredRotation();

    QGraphicsScene scene;
    QGraphicsView display(&scene);
    configureDisplay(&display, output_size);
    addRotatedCamera(&scene, output_size, rotation);

    qInfo() << "Qt platform:" << app.platformName()
            << "screen:" << output_size
            << "UI rotation:" << rotation;
    display.showFullScreen();
    return app.exec();
}
