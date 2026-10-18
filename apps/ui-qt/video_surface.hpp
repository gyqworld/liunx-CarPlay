#pragma once

#include "h264_decoder.hpp"

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QSize>
#include <QPointF>
#include <QWidget>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

class QMouseEvent;
class QTimer;
class QWheelEvent;

class VideoSurface : public QWidget {
  Q_OBJECT
 public:
  explicit VideoSurface(QWidget* parent = nullptr);
  ~VideoSurface() override;
  void setTouchHandler(std::function<void(float x, float y, bool down)> handler);
  void enterFullscreen();
  void leaveFullscreen();
  void prepareForSession();
  bool hasPicture() const { return !pixmap_.isNull(); }

 signals:
  /** Esc / vehicle-key path: return to host settings UI. */
  void hostUiRequested();
  /** First decoded frame — MainWindow can switch to fullscreen. */
  void firstFramePresented();
  void streamSizeChanged(QSize size);

 public slots:
  void onAnnexB(const QByteArray& data);
  void setHudText(const QString& text);

 private slots:
  void presentPending();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  std::optional<QPointF> mapPoint(QPoint pos, bool clamp) const;
  QRect contentRect() const;
  void emitTouch(const QPointF& point, bool down);
  void endWheel();
  void decodeLoop();
  void postPresent();

  std::mutex queue_mu_;
  std::condition_variable queue_cv_;
  QList<QByteArray> pending_;
  bool stop_decode_ = false;
  std::thread decode_thread_;

  std::mutex frame_mu_;
  QImage pending_frame_;
  bool present_posted_ = false;
  QSize frame_size_;
  QPixmap pixmap_;
  bool announced_first_ = false;
  QString hud_text_;

  bool dragging_ = false;
  bool wheel_down_ = false;
  QPointF last_{0.5, 0.5};
  QTimer* wheel_timer_ = nullptr;
  std::function<void(float, float, bool)> touch_;
};
