#include "video_surface.hpp"

#include <QApplication>
#include <QIcon>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

VideoSurface::VideoSurface(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_OpaquePaintEvent);
  setMouseTracking(false);
  setMinimumSize(960, 540);
  setWindowTitle(QStringLiteral("专业拧瓶盖员"));
  setWindowIcon(QApplication::windowIcon());
  setWindowFlag(Qt::Window);
  setFocusPolicy(Qt::StrongFocus);
  resize(1280, 720);
  wheel_timer_ = new QTimer(this);
  wheel_timer_->setSingleShot(true);
  connect(wheel_timer_, &QTimer::timeout, this, [this] { endWheel(); });
  decode_thread_ = std::thread([this] { decodeLoop(); });
}

void VideoSurface::enterFullscreen() {
  show();
  showFullScreen();
  raise();
  activateWindow();
  setFocus(Qt::OtherFocusReason);
}

void VideoSurface::leaveFullscreen() {
  if (isFullScreen()) showNormal();
  hide();
}

void VideoSurface::prepareForSession() {
  announced_first_ = false;
  pixmap_ = QPixmap();
  frame_size_ = {};
  hud_text_.clear();
  {
    std::lock_guard<std::mutex> lock(frame_mu_);
    pending_frame_ = QImage();
    present_posted_ = false;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    pending_.clear();
  }
  // Keep window hidden until the first frame so the settings log stays visible.
  hide();
}

void VideoSurface::setHudText(const QString& text) {
  if (hud_text_ == text) return;
  hud_text_ = text;
  if (isVisible()) update();
}

VideoSurface::~VideoSurface() {
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    stop_decode_ = true;
  }
  queue_cv_.notify_all();
  if (decode_thread_.joinable()) decode_thread_.join();
}

void VideoSurface::setTouchHandler(std::function<void(float, float, bool)> handler) { touch_ = std::move(handler); }

QRect VideoSurface::contentRect() const {
  QSize source = size();
  if (!frame_size_.isEmpty()) source = frame_size_;
  else if (!pixmap_.isNull()) source = pixmap_.size();
  const QSize fitted = source.scaled(size(), Qt::KeepAspectRatio);
  return QRect((width() - fitted.width()) / 2, (height() - fitted.height()) / 2, fitted.width(), fitted.height());
}

std::optional<QPointF> VideoSurface::mapPoint(QPoint pos, bool clamp) const {
  const QRect area = contentRect();
  if (area.width() < 2 || area.height() < 2) return std::nullopt;
  if (!clamp && !area.contains(pos)) return std::nullopt;
  double x = (static_cast<double>(pos.x()) - area.left()) / static_cast<double>(area.width());
  double y = (static_cast<double>(pos.y()) - area.top()) / static_cast<double>(area.height());
  x = std::clamp(x, 0.0, 1.0);
  y = std::clamp(y, 0.0, 1.0);
  return QPointF(x, y);
}

void VideoSurface::emitTouch(const QPointF& point, bool down) {
  if (touch_) touch_(static_cast<float>(point.x()), static_cast<float>(point.y()), down);
}

void VideoSurface::endWheel() {
  if (!wheel_down_) return;
  wheel_down_ = false;
  emitTouch(last_, false);
}

void VideoSurface::onAnnexB(const QByteArray& data) {
  if (data.isEmpty()) return;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    // Keep every access unit. Dropping a P-frame tears the picture while dragging.
    pending_.push_back(data);
    while (pending_.size() > 90) pending_.removeFirst();
  }
  queue_cv_.notify_one();
}

void VideoSurface::postPresent() {
  bool need = false;
  {
    std::lock_guard<std::mutex> lock(frame_mu_);
    if (!present_posted_) {
      present_posted_ = true;
      need = true;
    }
  }
  if (need) QMetaObject::invokeMethod(this, "presentPending", Qt::QueuedConnection);
}

void VideoSurface::presentPending() {
  QImage frame;
  {
    std::lock_guard<std::mutex> lock(frame_mu_);
    frame = std::move(pending_frame_);
    present_posted_ = false;
  }
  if (!frame.isNull()) {
    const bool size_changed = frame_size_ != frame.size();
    frame_size_ = frame.size();
    if (size_changed) emit streamSizeChanged(frame_size_);
    // QPixmap must be created on the GUI/X11 thread — drawing QImage via XCB was SEGV'ing.
    pixmap_ = QPixmap::fromImage(frame);
    pixmap_.setDevicePixelRatio(1);
    if (!announced_first_) {
      announced_first_ = true;
      enterFullscreen();
      emit firstFramePresented();
    } else {
      update();
    }
  }
  bool again = false;
  {
    std::lock_guard<std::mutex> lock(frame_mu_);
    if (!pending_frame_.isNull() && !present_posted_) {
      present_posted_ = true;
      again = true;
    }
  }
  if (again) QMetaObject::invokeMethod(this, "presentPending", Qt::QueuedConnection);
}

void VideoSurface::decodeLoop() {
  H264Decoder decoder;
  while (true) {
    QByteArray data;
    {
      std::unique_lock<std::mutex> lock(queue_mu_);
      queue_cv_.wait(lock, [&] { return stop_decode_ || !pending_.isEmpty(); });
      if (stop_decode_ && pending_.isEmpty()) return;
      data = pending_.takeFirst();
    }
    carplay::ByteBuffer annexb(reinterpret_cast<const std::uint8_t*>(data.constData()),
                               reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size());
    QImage decoded;
    if (!decoder.feed(annexb, decoded) || decoded.isNull()) continue;
    {
      std::lock_guard<std::mutex> lock(frame_mu_);
      pending_frame_ = std::move(decoded);
    }
    postPresent();
  }
}

void VideoSurface::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), Qt::black);
  if (pixmap_.isNull()) {
    p.setPen(Qt::lightGray);
    p.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待 CarPlay 画面…"));
    return;
  }
  const QRect dest = contentRect();
  // Exact pixel match: direct blit. Any size mismatch uses smooth scale —
  // nearest-neighbor made icon edges look stair-stepped.
  if (pixmap_.size() == dest.size()) {
    p.drawPixmap(dest.topLeft(), pixmap_);
  } else {
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawPixmap(dest, pixmap_);
  }
  if (!hud_text_.isEmpty()) {
    const QFontMetrics fm(p.font());
    const int pad = 8;
    const QRect text = fm.boundingRect(hud_text_).adjusted(-pad, -4, pad, 4);
    QRect box(12, 12, text.width(), text.height());
    p.fillRect(box, QColor(0, 0, 0, 160));
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, hud_text_);
  }
}

void VideoSurface::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return;
  endWheel();
  const auto point = mapPoint(event->pos(), false);
  if (!point) return;
  dragging_ = true;
  last_ = *point;
  emitTouch(last_, true);
}

void VideoSurface::mouseMoveEvent(QMouseEvent* event) {
  if (!dragging_) return;
  const auto point = mapPoint(event->pos(), true);
  if (!point) return;
  last_ = *point;
  emitTouch(last_, true);
}

void VideoSurface::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !dragging_) return;
  dragging_ = false;
  if (const auto point = mapPoint(event->pos(), true)) last_ = *point;
  emitTouch(last_, false);
}

void VideoSurface::wheelEvent(QWheelEvent* event) {
  if (dragging_) return;
  const auto origin = mapPoint(event->position().toPoint(), false);
  if (!origin) return;
  if (!wheel_down_) {
    last_ = *origin;
    wheel_down_ = true;
    emitTouch(last_, true);
  }
  const QPoint delta = event->angleDelta();
  double x = last_.x() - std::clamp(delta.x() / 120.0, -1.0, 1.0) * 0.04;
  double y = last_.y() - std::clamp(delta.y() / 120.0, -1.0, 1.0) * 0.06;
  last_ = QPointF(std::clamp(x, 0.02, 0.98), std::clamp(y, 0.02, 0.98));
  emitTouch(last_, true);
  wheel_timer_->start(180);
  event->accept();
}
