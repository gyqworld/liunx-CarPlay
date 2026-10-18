#pragma once

#include "carplay/protocol/buffer_util.hpp"

#include <QByteArray>
#include <QImage>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/** Minimal annex-B H.264 → RGB32 decoder for the CarPlay screen stream. */
class H264Decoder {
 public:
  H264Decoder();
  ~H264Decoder();

  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  bool feed(const carplay::ByteBuffer& annexb, QImage& out);
  static bool hasSps(const QByteArray& data);
  static QByteArray extractSps(const QByteArray& data);
  const std::string& last_error() const { return error_; }

 private:
  bool ensure_open();
  bool decode_packet(QImage& out);
  void reset();

  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  SwsContext* sws_ = nullptr;
  int sws_w_ = 0;
  int sws_h_ = 0;
  bool fed_ = false;
  QByteArray last_sps_;
  std::string error_;
};
