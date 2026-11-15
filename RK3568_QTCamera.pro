QT += widgets

CONFIG += c++17
TEMPLATE = app
TARGET = rk3568_camera_qt

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    src/camera/capture_worker.cpp \
    src/camera/rga_converter.cpp \
    src/camera/v4l2_capture.c \
    src/main.cpp \
    src/mainwindow.cpp

# 添加头文件搜索根目录，这样 #include <camera/rga_converter.hpp> 才生效
INCLUDEPATH += $$PWD/include

HEADERS += \
    include/camera/camera_frame.h \
    include/camera/capture_worker.hpp \
    include/camera/rga_converter.hpp \
    include/camera/v4l2_capture.h \
    src/mainwindow.h

FORMS += \
    src/mainwindow.ui

# 添加动态库搜索
LIBS += -lrga

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
