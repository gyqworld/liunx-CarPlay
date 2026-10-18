#include "carplay/session/session_services.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(CP_HAVE_OPENSSL)
#include <openssl/evp.h>
#endif

#if defined(CP_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace carplay {
namespace {

ByteBuffer audio_chacha_open(const ByteBuffer& key, const ByteBuffer& nonce, const std::uint8_t* data,
                             std::size_t len, const std::uint8_t* aad, std::size_t aad_len) {
#if !defined(CP_HAVE_OPENSSL)
  (void)key;
  (void)nonce;
  (void)data;
  (void)len;
  (void)aad;
  (void)aad_len;
  return {};
#else
  if (key.size() != 32 || nonce.size() != 12 || len < 16) return {};
  const std::size_t cipher_len = len - 16;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return {};
  int out_len = 0;
  int extra = 0;
  int final_len = 0;
  ByteBuffer plain(cipher_len);
  const bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
                  EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
                  (aad_len == 0 || EVP_DecryptUpdate(ctx, nullptr, &extra, aad, static_cast<int>(aad_len)) == 1) &&
                  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, const_cast<std::uint8_t*>(data + cipher_len)) ==
                      1 &&
                  EVP_DecryptUpdate(ctx, plain.data(), &out_len, data, static_cast<int>(cipher_len)) == 1 &&
                  EVP_DecryptFinal_ex(ctx, plain.data() + out_len, &final_len) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return {};
  plain.resize(static_cast<std::size_t>(out_len + final_len));
  return plain;
#endif
}

int bind_udp6_local(int& fd, std::string& error) {
  fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) {
    error = "音频socket失败";
    return -1;
  }
  int one = 1;
  int off = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  const int rcv = 1 << 20;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = "音频端口绑定失败";
    ::close(fd);
    fd = -1;
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    error = "音频端口查询失败";
    ::close(fd);
    fd = -1;
    return -1;
  }
  return ntohs(addr.sin6_port);
}

struct QueuedPacket {
  std::uint16_t sequence = 0;
  std::uint32_t sample = 0;
  ByteBuffer payload;
};

}  // namespace

struct AudioPort::Impl {
  ByteBuffer key;
  AudioCodec codec = AudioCodec::Aac;
  Info info;
  StatusHandler status;
  int data_fd = -1;
  int control_fd = -1;
  FILE* play = nullptr;
  std::thread recv_thread;
  std::thread play_thread;
  std::atomic<bool> stop{false};
  std::atomic<int> packets{0};
  std::atomic<int> drops{0};
  mutable std::mutex origin_mu;
  bool have_origin = false;
  std::uint32_t first_sample = 0;
  std::int64_t origin_ns = 0;

  std::mutex queue_mu;
  std::condition_variable queue_cv;
  std::deque<QueuedPacket> queue;
  bool have_seq = false;
  std::uint16_t next_seq = 0;
  int prebuffer_packets = 8;
  int max_queue = 64;
  int pipe_latency_ms = 200;

  std::mutex pcm_mu;
  std::condition_variable pcm_cv;
  std::deque<std::vector<std::uint8_t>> pcm_chunks;
  std::size_t pcm_bytes = 0;
  std::size_t pcm_high = 0;
  std::size_t pcm_target = 0;
  std::size_t pcm_low = 0;
  bool primed = false;

  static void boost_thread() {
#if defined(__linux__)
    // Stay on SCHED_OTHER — SCHED_RR can starve the Qt/X11 thread and contributed to UI SEGV.
    sched_param sp{};
    sp.sched_priority = 0;
    ::pthread_setschedparam(::pthread_self(), SCHED_OTHER, &sp);
#endif
  }

#if defined(CP_HAVE_FFMPEG)
  AVCodecContext* avctx = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  SwrContext* swr = nullptr;
#endif

  ~Impl() { close_all(); }

  void close_all() {
    stop = true;
    queue_cv.notify_all();
    pcm_cv.notify_all();
    if (data_fd >= 0) {
      ::shutdown(data_fd, SHUT_RDWR);
      ::close(data_fd);
      data_fd = -1;
    }
    if (control_fd >= 0) {
      ::close(control_fd);
      control_fd = -1;
    }
    if (recv_thread.joinable()) recv_thread.join();
    if (play_thread.joinable()) play_thread.join();
    if (play) {
      ::pclose(play);
      play = nullptr;
    }
#if defined(CP_HAVE_FFMPEG)
    if (swr) {
      swr_free(&swr);
      swr = nullptr;
    }
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (avctx) avcodec_free_context(&avctx);
#endif
  }

  void configure_buffering() {
    // Media (type 102) needs a deep buffer so 1080p decode spikes do not underrun.
    const bool media = info.type == 102 || info.latency_ms >= 200;
    if (media) {
      prebuffer_packets = 12;
      max_queue = 96;
      pipe_latency_ms = 250;
      pcm_target = static_cast<std::size_t>(info.sample_rate * info.channels * 2 * 200 / 1000);
      pcm_high = pcm_target * 3;
      pcm_low = pcm_target / 3;
    } else {
      prebuffer_packets = 6;
      max_queue = 48;
      pipe_latency_ms = 120;
      pcm_target = static_cast<std::size_t>(info.sample_rate * info.channels * 2 * 80 / 1000);
      pcm_high = pcm_target * 3;
      pcm_low = pcm_target / 3;
    }
  }

  bool open_player(std::string& error) {
    char cmd[320];
    std::snprintf(cmd, sizeof(cmd),
                  "pw-cat --playback --raw --format s16 --rate %d --channels %d --latency %dms --quality 4 - "
                  "2>/dev/null",
                  info.sample_rate, info.channels, pipe_latency_ms);
    play = ::popen(cmd, "w");
    if (!play) {
      error = "无法打开音频输出(pw-cat)";
      return false;
    }
    // Large stdio buffer; avoid fflush-per-packet which starves PipeWire.
    std::setvbuf(play, nullptr, _IOFBF, 256 * 1024);
    return true;
  }

#if defined(CP_HAVE_FFMPEG)
  bool open_decoder(std::string& error) {
    if (codec == AudioCodec::Pcm) return true;
    const AVCodecID id = codec == AudioCodec::Opus ? AV_CODEC_ID_OPUS : AV_CODEC_ID_AAC;
    const AVCodec* avcodec = avcodec_find_decoder(id);
    if (!avcodec) {
      error = "找不到音频解码器";
      return false;
    }
    avctx = avcodec_alloc_context3(avcodec);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!avctx || !frame || !packet) {
      error = "音频解码器分配失败";
      return false;
    }
    avctx->sample_rate = info.sample_rate;
    av_channel_layout_default(&avctx->ch_layout, info.channels);
    // Keep LOW_DELAY only for realtime/nav streams; media benefits from a fuller decoder.
    if (info.type != 102) avctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (codec == AudioCodec::Aac) {
      const int freq_index = info.sample_rate == 48000 ? 3 : 4;
      const std::uint8_t asc[2] = {
          static_cast<std::uint8_t>((2 << 3) | (freq_index >> 1)),
          static_cast<std::uint8_t>(((freq_index & 1) << 7) | (info.channels << 3))};
      avctx->extradata = static_cast<std::uint8_t*>(av_malloc(2 + AV_INPUT_BUFFER_PADDING_SIZE));
      if (!avctx->extradata) {
        error = "AAC配置分配失败";
        return false;
      }
      std::memcpy(avctx->extradata, asc, 2);
      avctx->extradata_size = 2;
    }
    if (avcodec_open2(avctx, avcodec, nullptr) < 0) {
      error = "打开音频解码器失败";
      return false;
    }
    return true;
  }

  bool ensure_swr(AVFrame* in) {
    if (swr) return true;
    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, info.channels);
    if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, info.sample_rate, &in->ch_layout,
                            static_cast<AVSampleFormat>(in->format), in->sample_rate, 0, nullptr) < 0) {
      return false;
    }
    if (swr_init(swr) < 0) {
      swr_free(&swr);
      return false;
    }
    return true;
  }

  void push_pcm(const std::uint8_t* data, std::size_t bytes) {
    if (bytes == 0) return;
    std::vector<std::uint8_t> chunk(data, data + bytes);
    std::lock_guard<std::mutex> lock(pcm_mu);
    while (pcm_bytes + bytes > pcm_high && !pcm_chunks.empty()) {
      pcm_bytes -= pcm_chunks.front().size();
      pcm_chunks.pop_front();
      ++drops;
    }
    pcm_bytes += chunk.size();
    pcm_chunks.push_back(std::move(chunk));
    if (!primed && pcm_bytes >= pcm_target) primed = true;
    pcm_cv.notify_one();
  }

  void decode_packet(const ByteBuffer& payload) {
    if (codec == AudioCodec::Pcm) {
      // AirPlay LPCM on the wire is big-endian s16.
      std::vector<std::uint8_t> le(payload.size());
      for (std::size_t i = 0; i + 1 < payload.size(); i += 2) {
        le[i] = payload[i + 1];
        le[i + 1] = payload[i];
      }
      push_pcm(le.data(), le.size() - (le.size() % 2));
      return;
    }
    if (!avctx || !packet || !frame) return;
    av_packet_unref(packet);
    if (av_new_packet(packet, static_cast<int>(payload.size())) < 0) return;
    std::memcpy(packet->data, payload.data(), payload.size());
    if (avcodec_send_packet(avctx, packet) < 0) return;
    while (avcodec_receive_frame(avctx, frame) == 0) {
      if (!ensure_swr(frame)) return;
      const int out_samples = swr_get_out_samples(swr, frame->nb_samples) + 32;
      if (out_samples <= 0) continue;
      std::vector<std::uint8_t> out(static_cast<std::size_t>(out_samples) * info.channels * 2);
      std::uint8_t* dst[] = {out.data()};
      const int got = swr_convert(swr, dst, out_samples, const_cast<const std::uint8_t**>(frame->extended_data),
                                  frame->nb_samples);
      if (got > 0) push_pcm(out.data(), static_cast<std::size_t>(got) * info.channels * 2);
    }
  }
#else
  bool open_decoder(std::string& error) {
    error = "未编译FFmpeg音频支持";
    return false;
  }
  void decode_packet(const ByteBuffer&) {}
  void push_pcm(const std::uint8_t*, std::size_t) {}
#endif

  static int seq_delta(std::uint16_t a, std::uint16_t b) {
    return static_cast<std::int16_t>(a - b);
  }

  void enqueue(QueuedPacket pkt) {
    std::lock_guard<std::mutex> lock(queue_mu);
    if (!have_seq) {
      have_seq = true;
      next_seq = pkt.sequence;
    } else if (seq_delta(pkt.sequence, next_seq) < -32) {
      // Far in the past — likely wrap/reset; resync.
      next_seq = pkt.sequence;
      queue.clear();
    } else if (seq_delta(pkt.sequence, next_seq) < 0) {
      ++drops;
      return;
    }
    auto it = std::lower_bound(queue.begin(), queue.end(), pkt.sequence,
                               [](const QueuedPacket& q, std::uint16_t seq) { return seq_delta(q.sequence, seq) < 0; });
    if (it != queue.end() && it->sequence == pkt.sequence) return;
    queue.insert(it, std::move(pkt));
    while (static_cast<int>(queue.size()) > max_queue) {
      queue.pop_front();
      ++drops;
      if (!queue.empty()) next_seq = queue.front().sequence;
    }
    queue_cv.notify_one();
  }

  void on_datagram(const std::uint8_t* data, std::size_t len) {
    if (len < 36) return;
    ByteBuffer nonce(12, 0);
    std::memcpy(nonce.data() + 4, data + len - 8, 8);
    ByteBuffer plain = audio_chacha_open(key, nonce, data + 12, len - 12 - 8, data + 4, 8);
    if (plain.empty()) return;
    const std::uint16_t sequence =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[2]) << 8) | data[3]);
    const std::uint32_t sample = (static_cast<std::uint32_t>(data[4]) << 24) | (static_cast<std::uint32_t>(data[5]) << 16) |
                                 (static_cast<std::uint32_t>(data[6]) << 8) | data[7];
    {
      std::lock_guard<std::mutex> lock(origin_mu);
      if (!have_origin) {
        have_origin = true;
        first_sample = sample;
        origin_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
        if (status) status("音频首包 type" + std::to_string(info.type));
      }
    }
    QueuedPacket pkt;
    pkt.sequence = sequence;
    pkt.sample = sample;
    pkt.payload = std::move(plain);
    enqueue(std::move(pkt));
    const int n = ++packets;
    if ((n == 1 || n % 500 == 0) && status) {
      std::size_t q = 0;
      {
        std::lock_guard<std::mutex> lock(queue_mu);
        q = queue.size();
      }
      status("音频包 " + std::to_string(n) + " q=" + std::to_string(q) + " drop=" + std::to_string(drops.load()));
    }
  }

  void receiver_main() {
    std::uint8_t buf[4096];
    while (!stop && data_fd >= 0) {
      pollfd pfd{data_fd, POLLIN, 0};
      if (::poll(&pfd, 1, 50) <= 0) continue;
      while (!stop) {
        const ssize_t n = ::recv(data_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) break;
        on_datagram(buf, static_cast<std::size_t>(n));
      }
    }
    queue_cv.notify_all();
    pcm_cv.notify_all();
  }

  void decoder_main() {
    boost_thread();
    bool started = false;
    auto gap_since = std::chrono::steady_clock::time_point{};
    while (!stop) {
      QueuedPacket pkt;
      {
        std::unique_lock<std::mutex> lock(queue_mu);
        queue_cv.wait_for(lock, std::chrono::milliseconds(10), [&] {
          return stop || (started ? !queue.empty() : static_cast<int>(queue.size()) >= prebuffer_packets);
        });
        if (stop) break;
        if (queue.empty()) continue;
        if (!started) {
          if (static_cast<int>(queue.size()) < prebuffer_packets) continue;
          next_seq = queue.front().sequence;
          started = true;
          if (status) status("音频缓冲就绪 " + std::to_string(queue.size()) + "包");
        }
        if (queue.front().sequence != next_seq) {
          const auto now = std::chrono::steady_clock::now();
          if (gap_since == std::chrono::steady_clock::time_point{}) gap_since = now;
          // Wait longer for late packets before skipping — skipping causes audible gaps.
          if (now - gap_since < std::chrono::milliseconds(80)) continue;
          drops += std::max(0, seq_delta(queue.front().sequence, next_seq));
          next_seq = queue.front().sequence;
        }
        gap_since = {};
        pkt = std::move(queue.front());
        queue.pop_front();
        next_seq = static_cast<std::uint16_t>(pkt.sequence + 1);
      }
      decode_packet(pkt.payload);
    }
  }

  void writer_main() {
    boost_thread();
    std::vector<std::uint8_t> chunk;
    chunk.reserve(pcm_target);
    int flushes = 0;
    while (!stop) {
      chunk.clear();
      {
        std::unique_lock<std::mutex> lock(pcm_mu);
        pcm_cv.wait_for(lock, std::chrono::milliseconds(10), [&] {
          return stop || (primed && pcm_bytes > 0);
        });
        if (stop) break;
        if (pcm_chunks.empty()) {
          // Underrun: pause until pcm_target refills instead of drip-feeding.
          if (primed && pcm_bytes < pcm_low) primed = false;
          continue;
        }
        if (!primed) continue;
        // Prefer ~40ms blocks so PipeWire stays fed across video decode spikes.
        const std::size_t want =
            static_cast<std::size_t>(info.sample_rate * info.channels * 2 * 40 / 1000);
        while (!pcm_chunks.empty() && chunk.size() < want) {
          auto front = std::move(pcm_chunks.front());
          pcm_chunks.pop_front();
          pcm_bytes -= front.size();
          chunk.insert(chunk.end(), front.begin(), front.end());
        }
      }
      if (!play || chunk.empty()) continue;
      const std::size_t wrote = std::fwrite(chunk.data(), 1, chunk.size(), play);
      if (wrote != chunk.size()) break;
      if ((++flushes % 8) == 0) std::fflush(play);
    }
  }

  void play_pipeline() {
    boost_thread();
    std::thread decode([this] { decoder_main(); });
    writer_main();
    queue_cv.notify_all();
    if (decode.joinable()) decode.join();
  }
};

AudioPort::AudioPort() : impl_(std::make_unique<Impl>()) {}
AudioPort::~AudioPort() { stop(); }

bool AudioPort::listen(const ByteBuffer& key, AudioCodec codec, const Info& info, int& data_port, int& control_port,
                       std::string& error, StatusHandler status) {
  stop();
  impl_ = std::make_unique<Impl>();
  if (key.size() != 32) {
    error = "音频密钥无效";
    return false;
  }
  impl_->key = key;
  impl_->codec = codec;
  impl_->info = info;
  impl_->status = std::move(status);
  impl_->configure_buffering();
  data_port = bind_udp6_local(impl_->data_fd, error);
  if (data_port < 0) return false;
  control_port = bind_udp6_local(impl_->control_fd, error);
  if (control_port < 0) return false;
  if (!impl_->open_player(error)) return false;
  if (!impl_->open_decoder(error)) return false;
  impl_->stop = false;
  impl_->recv_thread = std::thread([this] { impl_->receiver_main(); });
  impl_->play_thread = std::thread([this] { impl_->play_pipeline(); });
  if (impl_->status) {
    const char* name = codec == AudioCodec::Aac ? "aac" : codec == AudioCodec::Opus ? "opus" : "pcm";
    impl_->status(std::string("音频口 d=") + std::to_string(data_port) + " c=" + std::to_string(control_port) + " " +
                  name + " " + std::to_string(info.sample_rate) + "Hz buf=" +
                  std::to_string(impl_->pipe_latency_ms) + "ms");
  }
  return true;
}

void AudioPort::stop() {
  if (impl_) impl_->close_all();
}

const AudioPort::Info& AudioPort::info() const { return impl_->info; }

bool AudioPort::origin(std::uint32_t& sample, std::int64_t& origin_ns) const {
  std::lock_guard<std::mutex> lock(impl_->origin_mu);
  if (!impl_->have_origin) return false;
  sample = impl_->first_sample;
  origin_ns = impl_->origin_ns;
  return true;
}

}  // namespace carplay
