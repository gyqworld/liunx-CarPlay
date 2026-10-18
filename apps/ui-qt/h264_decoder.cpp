#include "h264_decoder.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder() { reset(); }

void H264Decoder::reset() {
  if (sws_) {
    sws_freeContext(sws_);
    sws_ = nullptr;
  }
  if (packet_) av_packet_free(&packet_);
  if (frame_) av_frame_free(&frame_);
  if (ctx_) avcodec_free_context(&ctx_);
  sws_w_ = 0;
  sws_h_ = 0;
  fed_ = false;
  last_sps_.clear();
}

bool H264Decoder::ensure_open() {
  if (ctx_) return true;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    error_ = "找不到 H.264 解码器";
    return false;
  }
  ctx_ = avcodec_alloc_context3(codec);
  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  if (!ctx_ || !frame_ || !packet_) {
    error_ = "解码器分配失败";
    return false;
  }
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  if (avcodec_open2(ctx_, codec, nullptr) < 0) {
    error_ = "打开 H.264 解码器失败";
    return false;
  }
  return true;
}

bool H264Decoder::decode_packet(QImage& out) {
  int send = avcodec_send_packet(ctx_, packet_);
  if (send < 0 && send != AVERROR(EAGAIN)) {
    error_ = "送入解码包失败";
    return false;
  }
  bool got = false;
  while (true) {
    const int rc = avcodec_receive_frame(ctx_, frame_);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
    if (rc < 0) {
      error_ = "取出解码帧失败";
      return false;
    }
    if (frame_->width <= 0 || frame_->height <= 0 || frame_->data[0] == nullptr) continue;
    if (!sws_ || sws_w_ != frame_->width || sws_h_ != frame_->height ||
        sws_ == nullptr) {
      if (sws_) sws_freeContext(sws_);
      sws_ = sws_getContext(frame_->width, frame_->height, static_cast<AVPixelFormat>(frame_->format), frame_->width,
                            frame_->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      sws_w_ = frame_->width;
      sws_h_ = frame_->height;
      if (!sws_) {
        error_ = "色彩转换初始化失败";
        return false;
      }
      // CarPlay sends full-range yuvj420p. Leaving the range unset lifts blacks and looks hazy.
      const int* coeff = sws_getCoefficients(SWS_CS_ITU709);
      sws_setColorspaceDetails(sws_, coeff, 1, coeff, 1, 0, 1 << 16, 1 << 16);
    }
    QImage image(frame_->width, frame_->height, QImage::Format_RGB32);
    if (image.isNull()) {
      error_ = "图像分配失败";
      return false;
    }
    uint8_t* dst[] = {image.bits()};
    int dst_linesize[] = {static_cast<int>(image.bytesPerLine())};
    if (sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, dst_linesize) <= 0) continue;
    out = std::move(image);
    got = true;
  }
  return got;
}

bool H264Decoder::hasSps(const QByteArray& data) { return !extractSps(data).isEmpty(); }

QByteArray H264Decoder::extractSps(const QByteArray& data) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.constData());
  const int n = data.size();
  for (int i = 0; i + 4 < n; ++i) {
    int sc = 0;
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) sc = 3;
    else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) sc = 4;
    if (sc == 0) continue;
    const int off = i + sc;
    if (off >= n) continue;
    if ((p[off] & 0x1f) != 7) continue;
    int end = n;
    for (int j = off + 1; j + 3 < n; ++j) {
      if (p[j] == 0 && p[j + 1] == 0 && (p[j + 2] == 1 || (p[j + 2] == 0 && j + 3 < n && p[j + 3] == 1))) {
        end = j;
        break;
      }
    }
    return QByteArray(reinterpret_cast<const char*>(p + i), end - i);
  }
  return {};
}

bool H264Decoder::feed(const carplay::ByteBuffer& annexb, QImage& out) {
  if (annexb.empty() || annexb.size() > 8 * 1024 * 1024) return false;
  const QByteArray view(reinterpret_cast<const char*>(annexb.data()), static_cast<int>(annexb.size()));
  // Compare SPS NAL only — full IDR access units always differ and must not reset.
  const QByteArray sps = extractSps(view);
  if (!sps.isEmpty() && sps != last_sps_) {
    if (fed_) reset();
    last_sps_ = sps;
  }
  if (!ensure_open()) return false;
  av_packet_unref(packet_);
  if (av_new_packet(packet_, static_cast<int>(annexb.size())) < 0) {
    error_ = "分配 AVPacket 失败";
    return false;
  }
  std::memcpy(packet_->data, annexb.data(), annexb.size());
  fed_ = true;
  return decode_packet(out);
}
