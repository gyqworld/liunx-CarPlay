#include "carplay/protocol/iap_link.hpp"

#include "carplay/protocol/csm_framer.hpp"

#include <algorithm>
#include <chrono>

namespace carplay {
namespace {

ByteBuffer u16be(std::uint16_t value) {
  ByteBuffer out(2);
  write_be16(out.data(), value);
  return out;
}

void append(ByteBuffer& dst, const ByteBuffer& src) { dst.insert(dst.end(), src.begin(), src.end()); }

std::uint64_t now_ms() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool checksum_ok(const std::uint8_t* data, std::size_t len) {
  unsigned sum = 0;
  for (std::size_t i = 0; i < len; ++i) sum += data[i];
  return (sum & 0xff) == 0;
}

const ByteBuffer kSyncPayload = {1, 4, 255, 255, 15, 160, 1, 244, 4, 3, 10, 0, 2, 11, 2, 1, 12, 1, 2};
const ByteBuffer kZeroAckPayload = {1, 4, 255, 255, 0, 0, 0, 0, 0, 0, 10, 0, 2, 11, 2, 1, 12, 1, 2};

}  // namespace

ByteBuffer iap_packet(std::uint8_t control, std::uint8_t seq, std::uint8_t ack, std::uint8_t session,
                      const ByteBuffer& payload) {
  CsmFramer::Packet packet;
  packet.control = control;
  packet.seq = seq;
  packet.ack = ack;
  packet.session_id = session;
  packet.payload = payload;
  return CsmFramer::encode(packet);
}

ByteBuffer iap_param(std::uint16_t id, const ByteBuffer& value) {
  ByteBuffer out = u16be(static_cast<std::uint16_t>(value.size() + 4));
  append(out, u16be(id));
  append(out, value);
  return out;
}

ByteBuffer iap_csm(std::uint16_t id, const std::vector<ByteBuffer>& params) {
  ByteBuffer body;
  for (const auto& param : params) append(body, param);
  ByteBuffer out = {0x40, 0x40};
  append(out, u16be(static_cast<std::uint16_t>(body.size() + 6)));
  append(out, u16be(id));
  append(out, body);
  return out;
}

IapLink::IapLink(Writer writer) : IapLink(std::move(writer), Options{}) {}

IapLink::IapLink(Writer writer, Options options) : writer_(std::move(writer)), options_(options) {
  writer_(CsmFramer::kDetect, sizeof(CsmFramer::kDetect));
  if (options_.initiate) {
    state_ = State::Negotiate;
    sync();
  }
}

void IapLink::write_packet(std::uint8_t control, std::uint8_t seq, std::uint8_t ack, std::uint8_t session,
                           const ByteBuffer& payload) {
  const ByteBuffer frame = iap_packet(control, seq, ack, session, payload);
  if (writer_) writer_(frame.data(), frame.size());
}

void IapLink::fail(const std::string& reason) {
  if (state_ == State::Dead) return;
  failure_ = reason;
  close();
  if (on_failed) on_failed(reason);
}

void IapLink::close() {
  state_ = State::Dead;
  pending_.clear();
  queue_.clear();
  out_of_order_.clear();
}

void IapLink::sync() {
  write_packet(0x80, seq_, ack_, 0, options_.zero_ack ? kZeroAckPayload : kSyncPayload);
}

void IapLink::tick(std::uint64_t now_ms) {
  if (!clock_started_) {
    clock_started_ = true;
    began_ms_ = now_ms;
    last_negotiate_ms_ = now_ms;
  }
  if (state_ == State::Detect) {
    if (now_ms - began_ms_ > 15000) return fail("iAP2检测超时");
    if (now_ms - last_negotiate_ms_ > 1000) {
      writer_(CsmFramer::kDetect, sizeof(CsmFramer::kDetect));
      last_negotiate_ms_ = now_ms;
    }
  } else if (state_ == State::Negotiate) {
    if (now_ms - began_ms_ > 20000) return fail("iAP2链路协商超时");
    if (now_ms - last_negotiate_ms_ > 500) {
      sync();
      last_negotiate_ms_ = now_ms;
    }
  } else if (state_ == State::Normal) {
    for (auto& pending : pending_) {
      if (timeout_ms_ > 0 && now_ms - pending.time_ms >= static_cast<std::uint64_t>(timeout_ms_)) {
        if (++pending.retries > max_retries_) return fail("iAP2确认超时");
        pending.time_ms = now_ms;
        write_packet(0x40, pending.seq, ack_, pending.session, pending.data);
      }
    }
  }
}

void IapLink::flush() {
  if (state_ != State::Normal) return;
  while (!queue_.empty() && static_cast<int>(pending_.size()) < max_outgoing_) {
    Queued next = std::move(queue_.front());
    queue_.erase(queue_.begin());
    seq_ = static_cast<std::uint8_t>((seq_ + 1) & 255);
    Pending pending;
    pending.seq = seq_;
    pending.session = next.session;
    pending.data = next.data;
    pending.time_ms = now_ms();
    if (timeout_ms_ > 0) pending_.push_back(pending);
    write_packet(0x40, pending.seq, ack_, pending.session, pending.data);
  }
}

void IapLink::send(const ByteBuffer& payload, std::uint8_t session) {
  if (state_ == State::Dead) return fail("iAP2链路已关闭");
  const int size = std::max(1, max_len_ - 10);
  for (std::size_t offset = 0; offset < payload.size() || offset == 0; offset += static_cast<std::size_t>(size)) {
    if (payload.empty()) {
      queue_.push_back(Queued{session, {}});
      break;
    }
    const std::size_t n = std::min(payload.size() - offset, static_cast<std::size_t>(size));
    queue_.push_back(Queued{session, ByteBuffer(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                                payload.begin() + static_cast<std::ptrdiff_t>(offset + n))});
    if (offset + n >= payload.size()) break;
  }
  if (queue_.size() > 256) return fail("iAP2发送缓冲超限");
  flush();
}

void IapLink::deliver(std::uint8_t session, const ByteBuffer& data) {
  if (session == 12) {
    if (on_file) on_file(data);
    return;
  }
  if (session != 10) return;
  csm_.insert(csm_.end(), data.begin(), data.end());
  while (csm_.size() >= 6) {
    if (read_be16(csm_.data()) != 0x4040) return fail("iAP2控制消息标记无效");
    const std::uint16_t len = read_be16(csm_.data() + 2);
    if (len < 6) return fail("iAP2控制消息长度无效");
    if (csm_.size() < len) return;
    const std::uint16_t id = read_be16(csm_.data() + 4);
    std::map<std::uint16_t, ByteBuffer> fields;
    std::size_t i = 6;
    while (i + 4 <= len) {
      const std::uint16_t plen = read_be16(csm_.data() + i);
      if (plen < 4 || i + plen > len) return fail("iAP2参数长度无效");
      const std::uint16_t pid = read_be16(csm_.data() + i + 2);
      fields[pid] = ByteBuffer(csm_.begin() + static_cast<std::ptrdiff_t>(i + 4),
                               csm_.begin() + static_cast<std::ptrdiff_t>(i + plen));
      i += plen;
    }
    csm_.erase(csm_.begin(), csm_.begin() + len);
    if (on_csm) on_csm(id, fields);
  }
}

void IapLink::feed(const std::uint8_t* data, std::size_t len) {
  if (state_ == State::Dead || data == nullptr || len == 0) return;
  buffer_.insert(buffer_.end(), data, data + len);
  if (buffer_.size() > 2 * 1024 * 1024) return fail("iAP2接收缓冲超限");

  const auto marker = [](const ByteBuffer& buf) {
    return buf.size() >= 6 && std::equal(CsmFramer::kDetect, CsmFramer::kDetect + 6, buf.begin());
  };
  if (state_ == State::Detect) {
    if (buffer_.size() < 6) return;
    if (!marker(buffer_)) return fail("设备未回应iAP2检测标记");
    buffer_.erase(buffer_.begin(), buffer_.begin() + 6);
    state_ = State::Negotiate;
    sync();
  }
  if (state_ == State::Negotiate && marker(buffer_)) buffer_.erase(buffer_.begin(), buffer_.begin() + 6);

  while (state_ != State::Dead && buffer_.size() >= 9) {
    if (read_be16(buffer_.data()) != CsmFramer::kSync) {
      buffer_.erase(buffer_.begin());
      continue;
    }
    const std::uint16_t length = read_be16(buffer_.data() + 2);
    if (length < 9) return fail("iAP2包长度无效");
    if (buffer_.size() < length) return;
    ByteBuffer packet(buffer_.begin(), buffer_.begin() + length);
    buffer_.erase(buffer_.begin(), buffer_.begin() + length);
    if (!checksum_ok(packet.data(), 9) || (length > 9 && !checksum_ok(packet.data() + 9, length - 9))) continue;

    const std::uint8_t ctrl = packet[4];
    const std::uint8_t seq = packet[5];
    const std::uint8_t ack = packet[6];
    const std::uint8_t sid = packet[7];
    ByteBuffer payload;
    const bool has_payload = length > 9;
    if (has_payload) payload.assign(packet.begin() + 9, packet.begin() + length - 1);
    if (ctrl & 0x10) return fail("iPhone重置iAP2链路");
    if ((ctrl & 0x80) && (!has_payload || payload.size() < 10 || payload[0] != 1)) return fail("无效的iAP2同步参数");
    if (ctrl & 0x80) {
      max_outgoing_ = std::max(1, std::min(64, static_cast<int>(payload[1])));
      max_len_ = std::max(64, static_cast<int>(read_be16(payload.data() + 2)));
      timeout_ms_ = read_be16(payload.data() + 4);
      max_retries_ = payload[8];
      ack_ = seq;
      write_packet(0x40, seq_, ack_, 0, {});
    }
    if (ctrl & 0x40) {
      // Cumulative ACK: drop packets the peer has accepted. Keep only those still ahead of ack.
      pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                    [&](const Pending& item) {
                                      return ((ack - item.seq) & 255) <= 127;
                                    }),
                     pending_.end());
      if (state_ == State::Negotiate) {
        state_ = State::Normal;
        if (on_ready) on_ready();
      }
      flush();
    }
    if ((ctrl & 0x20) && has_payload) {
      for (auto& item : pending_) {
        if (std::find(payload.begin(), payload.end(), item.seq) == payload.end()) {
          item.time_ms = now_ms();
          write_packet(0x40, item.seq, ack_, item.session, item.data);
        }
      }
    }
    if ((ctrl & ~0x40) == 0 && has_payload) {
      const int distance = (seq - ack_) & 255;
      if (distance == 1) {
        ack_ = seq;
        deliver(sid, payload);
        while (out_of_order_.count(static_cast<std::uint8_t>((ack_ + 1) & 255))) {
          const auto next = static_cast<std::uint8_t>((ack_ + 1) & 255);
          const Queued queued = out_of_order_[next];
          out_of_order_.erase(next);
          ack_ = next;
          deliver(queued.session, queued.data);
        }
      } else if (distance > 1 && distance < 128 && out_of_order_.size() < 64) {
        out_of_order_[seq] = Queued{sid, payload};
        ByteBuffer missing;
        for (const auto& item : out_of_order_) missing.push_back(item.first);
        write_packet(0x20, seq_, ack_, 0, missing);
      }
      write_packet(0x40, seq_, ack_, 0, {});
    }
  }
}

}  // namespace carplay
