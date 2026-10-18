#pragma once

#include "carplay/protocol/buffer_util.hpp"
#include "carplay/protocol/pair_verify.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace carplay {

/** UDP timing sync driven by the accessory (type 210/211). */
class TimingSync {
 public:
  TimingSync();
  ~TimingSync();

  bool listen(int& port, std::string& error);
  void start(const std::string& peer_host, int peer_port);
  void stop();
  std::uint64_t now_ntp() const;

 private:
  void reader_main();
  void send_request();
  std::uint64_t synced_ntp() const;

  int fd_ = -1;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mu_;
  std::string peer_host_;
  int peer_port_ = 0;
  std::int64_t clock_offset_ns_ = 0;
  std::uint64_t pending_t1_ = 0;
  bool have_pending_ = false;
  bool started_ = false;
};

/** Encrypted TCP event channel used after session SETUP. */
class EventChannel {
 public:
  using StatusHandler = std::function<void(const std::string&)>;
  /** Phone → accessory commands on the event socket (e.g. requestUI). */
  using CommandHandler = std::function<void(const std::string& type, const ByteBuffer& body)>;

  EventChannel();
  ~EventChannel();

  bool listen(const ByteBuffer& shared, int& port, std::string& error, StatusHandler status = {},
              CommandHandler commands = {});
  void stop();
  bool connected() const { return client_fd_ >= 0; }
  /** Queue until RECORD and the phone's event socket are both ready. */
  bool send_command(const ByteBuffer& body);
  void mark_record_ready();

 private:
  void accept_main();
  void client_main(int fd);
  void write_locked(const ByteBuffer& body);
  void flush_locked();
  void handle_incoming(const std::string& req);

  int listen_fd_ = -1;
  int client_fd_ = -1;
  std::thread accept_thread_;
  std::thread client_thread_;
  std::atomic<bool> stop_{false};
  ByteBuffer shared_;
  StatusHandler status_;
  CommandHandler on_command_;
  std::unique_ptr<ControlCipher> cipher_;
  std::mutex client_mu_;
  int event_cseq_ = 0;
  bool record_ready_ = false;
  std::vector<ByteBuffer> pending_;
};

enum class AudioCodec { Aac, Opus, Pcm };

/** UDP audio receiver. Decrypts AirPlay packets and plays them on the local sink. */
class AudioPort {
 public:
  using StatusHandler = std::function<void(const std::string&)>;

  struct Info {
    int type = 0;
    int sample_rate = 48000;
    int channels = 2;
    int latency_ms = 0;
    bool id_is_int = true;
    std::int64_t id_int = 0;
    std::string id_text;
  };

  AudioPort();
  ~AudioPort();
  bool listen(const ByteBuffer& key, AudioCodec codec, const Info& info, int& data_port, int& control_port,
              std::string& error, StatusHandler status);
  void stop();
  const Info& info() const;
  bool origin(std::uint32_t& sample, std::int64_t& origin_ns) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/**
 * Main-screen TCP receiver. Frames are a 128-byte AirPlay header plus a body.
 * Opcode 1 is a clear avcC config; opcode 0 is a ChaCha20-Poly1305 H.264 frame.
 */
class ScreenPort {
 public:
  using StatusHandler = std::function<void(const std::string&)>;
  /** Annex-B access unit (config SPS/PPS or one coded picture). */
  using FrameHandler = std::function<void(const ByteBuffer& annexb)>;

  ScreenPort();
  ~ScreenPort();
  bool listen(const ByteBuffer& key, int& port, std::string& error, StatusHandler status,
              FrameHandler frames = {});
  /** Fired when the phone opens the video TCP socket. */
  void set_connected_handler(std::function<void()> handler);
  void stop();
  int frames() const { return frames_.load(); }

 private:
  void accept_main();
  void client_main(int fd);
  void on_message(const std::uint8_t* header, const std::uint8_t* body, std::size_t body_len, std::string& error);
  void append_annexb(ByteBuffer& out, const std::uint8_t* nal, std::size_t len);
  void write_avcc(const std::uint8_t* data, std::size_t len);
  void emit_frame(const ByteBuffer& annexb);
  void note_rx(std::size_t bytes);
  void maybe_log_bitrate();

  int listen_fd_ = -1;
  std::atomic<int> client_fd_{-1};
  std::thread thread_;
  std::atomic<bool> stop_{false};
  ByteBuffer key_;
  StatusHandler status_;
  FrameHandler on_frame_;
  std::function<void()> on_connected_;
  std::uint64_t counter_ = 0;
  std::atomic<int> frames_{0};
  int config_bytes_ = 0;
  /** Optional diagnostic dump; disabled on the hot path (see CARPLAY_DUMP_H264). */
  bool dump_h264_ = false;
  std::string path_;
  std::mutex file_mu_;
  std::uint64_t rx_bytes_window_ = 0;
  std::uint64_t rx_bytes_total_ = 0;
  int rx_frames_window_ = 0;
  std::chrono::steady_clock::time_point rx_window_start_{};
  int idr_seen_ = 0;
};

ByteBuffer hkdf_sha512_labeled(const ByteBuffer& ikm, const char* salt, const char* info);

}  // namespace carplay
