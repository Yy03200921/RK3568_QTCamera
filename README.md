# RK3568 V4L2 + ISP + RGA + Qt5相机

这是一个运行在RK3568 Buildroot系统上的MIPI CSI相机程序。工程使用：

- Qt 5.15 + qmake构建界面；
- V4L2 Multi-Planar API采集RKISP输出；
- MMAP管理采集缓冲区；
- DMA-BUF把原始帧直接交给RGA；
- RGA完成UYVY/YUYV到RGB888的颜色转换和预览缩放；
- Qt工作线程执行采集，主线程只处理界面；
- `QGraphicsProxyWidget`旋转完整界面并同步触摸坐标。

完整原理、代码阅读顺序、编译部署和排坑记录见：

```text
RK3568_CAMERA_DEVELOPMENT_NOTES.md
```

## 数据链路

```text
MIPI CSI Sensor
  -> RKISP
  -> /dev/video0
  -> V4L2 MMAP + DMA-BUF
  -> RGA颜色转换和硬件缩放
  -> QImage深拷贝
  -> Qt信号
  -> QLabel显示
```

原i.mx6ull裸写V4链路
摄像头
  ↓
V4L2驱动
  ↓
VIDIOC_DQBUF
  ↓
mmap后的YUV缓冲区
  ↓
CPU逐像素执行YUV转RGB
  ↓
CPU缩放或旋转
  ↓
写入Framebuffer
  ↓
LCD显示