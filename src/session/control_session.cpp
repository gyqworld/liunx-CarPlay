#include "carplay/session/control_session.hpp"

#include "carplay/protocol/bplist.hpp"
#include "carplay/protocol/pair_setup.hpp"
#include "carplay/protocol/tlv8.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <variant>

namespace carplay {
namespace {

constexpr const char* kPlistType = "application/x-apple-binary-plist";
constexpr const char* kMainUuid = "b7e6c5a0-1111-4000-8000-000000000001";
constexpr std::int64_t kFeatures = 0x615653aee2LL;

ByteBuffer read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return ByteBuffer(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Plist oem_icon(const std::string& path, int pixels) {
  ByteBuffer png = read_file(path);
  if (png.size() < 8 || png[0] != 0x89) return {};
  return Plist::dict({{"imageData", Plist::data(std::move(png))},
                      {"widthPixels", Plist::integer(pixels)},
                      {"heightPixels", Plist::integer(pixels)},
                      {"prerendered", Plist::boolean(true)}});
}

PlistArray load_oem_icons() {
  const char* home = std::getenv("HOME");
  const std::string root = home && *home ? std::string(home) : ".";
  const struct {
    const char* rel;
    int pixels;
  } files[] = {{"/oem-256.png", 256}, {"/oem-512.png", 512}};
  const std::string dirs[] = {root + "/.config/carplay-cpp", root + "/carplay-cpp/apps/ui-qt",
                              root + "/carplay-cpp/build"};
  PlistArray icons;
  for (const auto& file : files) {
    for (const auto& dir : dirs) {
      Plist icon = oem_icon(dir + file.rel, file.pixels);
      if (!icon.as_dict()) continue;
      icons.push_back(std::move(icon));
      break;
    }
  }
  return icons;
}

std::string header_value(const std::string& message, const char* name) {
  const std::string key = std::string("\r\n") + name + ":";
  auto pos = message.find(key);
  if (pos == std::string::npos) {
    if (message.rfind(std::string(name) + ":", 0) == 0) pos = 0;
    else return {};
  } else {
    pos += 2;
  }
  const auto start = message.find(':', pos);
  if (start == std::string::npos) return {};
  auto end = message.find("\r\n", start);
  if (end == std::string::npos) end = message.size();
  std::string value = message.substr(start + 1, end - start - 1);
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
  return value;
}

std::size_t content_length(const std::string& message) {
  const auto value = header_value(message, "Content-Length");
  if (value.empty()) return 0;
  return static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
}

ByteBuffer touch_descriptor(int x_max, int y_max) {
  auto finger = [&](int xmax, int ymax) {
    return ByteBuffer{
        0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x38, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x15, 0x00,
        0x25, 0x01, 0x09, 0x33, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0x05, 0x01,
        0x26, static_cast<std::uint8_t>(xmax & 0xff), static_cast<std::uint8_t>((xmax >> 8) & 0xff), 0x09, 0x30,
        0x75, 0x10, 0x95, 0x01, 0x81, 0x02, 0x26, static_cast<std::uint8_t>(ymax & 0xff),
        static_cast<std::uint8_t>((ymax >> 8) & 0xff), 0x09, 0x31, 0x81, 0x02, 0xc0};
  };
  ByteBuffer out{0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01};
  auto a = finger(x_max, y_max);
  auto b = finger(x_max, y_max);
  out.insert(out.end(), a.begin(), a.end());
  out.insert(out.end(), b.begin(), b.end());
  out.push_back(0xc0);
  return out;
}

const ByteBuffer kKnobDesc = {
    0x05, 0x01, 0x09, 0x08, 0xa1, 0x01, 0x05, 0x09, 0x09, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
    0x81, 0x02, 0x05, 0x0c, 0x0a, 0x23, 0x02, 0x0a, 0x24, 0x02, 0x95, 0x02, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02,
    0x81, 0x02, 0xc0, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0};

const ByteBuffer kMediaDesc = {0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x06, 0x05, 0x0c, 0x0a, 0x00,
                               0x00, 0x0a, 0xb0, 0x00, 0x0a, 0xb1, 0x00, 0x0a, 0xcd, 0x00, 0x0a, 0xb5, 0x00, 0x0a,
                               0xb6, 0x00, 0x0a, 0x9e, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0};

const ByteBuffer kTelDesc = {
    0x05, 0x0b, 0x09, 0x07, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x11, 0x05, 0x0b, 0x09, 0x00, 0x09, 0x20, 0x09, 0x21,
    0x09, 0x26, 0x09, 0x2f, 0x09, 0xb0, 0x09, 0xb1, 0x09, 0xb2, 0x09, 0xb3, 0x09, 0xb4, 0x09, 0xb5, 0x09, 0xb6,
    0x09, 0xb7, 0x09, 0xb8, 0x09, 0xb9, 0x09, 0xba, 0x09, 0xbb, 0x05, 0x07, 0x09, 0x2a, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x00, 0xc0};

Plist hid_entry(const char* uuid, const char* name, const ByteBuffer& desc) {
  return Plist::dict({{"hidProductID", Plist::integer(1)},
                      {"hidVendorID", Plist::integer(2)},
                      {"hidCountryCode", Plist::integer(0)},
                      {"uuid", Plist::string(uuid)},
                      {"name", Plist::string(name)},
                      {"displayUUID", Plist::string(kMainUuid)},
                      {"hidDescriptor", Plist::data(desc)}});
}

Plist resource(int id) {
  return Plist::dict({{"resourceID", Plist::integer(id)},
                      {"transferType", Plist::integer(1)},
                      {"transferPriority", Plist::integer(100)},
                      {"takeConstraint", Plist::integer(100)},
                      {"borrowConstraint", Plist::integer(100)},
                      {"unborrowConstraint", Plist::integer(100)}});
}

}  // namespace

void ControlSession::send_touch(float x, float y, bool down) {
  if (!events_) return;
  const int width = config_.width > 0 ? config_.width : 1280;
  const int height = config_.height > 0 ? config_.height : 720;
  if (x < 0.f) x = 0.f;
  if (y < 0.f) y = 0.f;
  if (x > 1.f) x = 1.f;
  if (y > 1.f) y = 1.f;
  const auto px = static_cast<std::uint16_t>(x * static_cast<float>(width));
  const auto py = static_cast<std::uint16_t>(y * static_cast<float>(height));
  ByteBuffer report(12, 0);
  report[0] = 0;
  report[1] = down ? 0x01 : 0x00;
  report[2] = static_cast<std::uint8_t>(px & 0xff);
  report[3] = static_cast<std::uint8_t>((px >> 8) & 0xff);
  report[4] = static_cast<std::uint8_t>(py & 0xff);
  report[5] = static_cast<std::uint8_t>((py >> 8) & 0xff);
  report[6] = 1;
  const ByteBuffer body = encode_bplist(Plist::dict({{"type", Plist::string("hidSendReport")},
                                                     {"uuid", Plist::string("2a2a2a2a")},
                                                     {"hidReport", Plist::data(std::move(report))}}));
  events_->send_command(body);
}

ControlSession::~ControlSession() {
  // Stop media first so video callbacks cannot touch the event channel after it dies.
  if (screen_) screen_->stop();
  screen_.reset();
  for (auto& audio : audios_) {
    if (audio) audio->stop();
  }
  audios_.clear();
  if (events_) events_->stop();
  events_.reset();
  if (timing_) timing_->stop();
  timing_.reset();
  if (keep_alive_) keep_alive_->stop();
  keep_alive_.reset();
}

std::vector<std::string> ControlSession::feed(const std::uint8_t* data, std::size_t len) {
  std::vector<std::string> out;
  if (data == nullptr || len == 0) return out;
  if (cipher_) {
    enc_.append(reinterpret_cast<const char*>(data), len);
    buffer_ += cipher_->open(enc_);
  } else {
    buffer_.append(reinterpret_cast<const char*>(data), len);
  }
  while (true) {
    const auto header_end = buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) break;
    const std::size_t body_len = content_length(buffer_);
    if (buffer_.size() < header_end + 4 + body_len) break;
    const std::string message = buffer_.substr(0, header_end + 4 + body_len);
    buffer_.erase(0, header_end + 4 + body_len);
    const auto line_end = message.find("\r\n");
    last_line_ = line_end == std::string::npos ? message : message.substr(0, line_end);
    const bool encrypted = cipher_.has_value();
    std::string response = handle_message(message);
    if (encrypted) response = cipher_->seal(response);
    out.push_back(std::move(response));
    if (!encrypted && verify_.verified()) cipher_.emplace(verify_.read_key(), verify_.write_key());
  }
  return out;
}

std::string ControlSession::http_response(int status, const std::string& content_type, const std::string& body,
                                          const std::string& extra_headers) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << " OK\r\n"
      << "Content-Length: " << body.size() << "\r\n";
  if (!content_type.empty()) oss << "Content-Type: " << content_type << "\r\n";
  if (!extra_headers.empty()) oss << extra_headers;
  oss << "Server: CarPlay/1.0\r\n\r\n" << body;
  return oss.str();
}

std::string ControlSession::reply(const std::string& protocol, int status, const std::string& reason,
                                  const std::string& cseq, const std::string& content_type,
                                  const std::string& body) const {
  std::string out = protocol + " " + std::to_string(status) + " " + reason + "\r\n";
  if (!cseq.empty()) out += "CSeq: " + cseq + "\r\n";
  if (!content_type.empty()) out += "Content-Type: " + content_type + "\r\n";
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  out += body;
  return out;
}

std::string ControlSession::rtsp_response(int status, const std::string& cseq, const std::string& extra) {
  std::ostringstream oss;
  oss << "RTSP/1.0 " << status << " OK\r\n"
      << "CSeq: " << cseq << "\r\n"
      << "Server: CarPlay/1.0\r\n";
  if (!extra.empty()) oss << extra;
  oss << "\r\n";
  return oss.str();
}

std::string ControlSession::handle_info(const std::string& protocol, const std::string& cseq) {
  const int width = config_.width > 0 ? config_.width : 1280;
  const int height = config_.height > 0 ? config_.height : 720;
  const int fps = config_.fps > 0 ? config_.fps : 30;
  const std::string name = config_.device_name.empty() ? "专业拧瓶盖员" : config_.device_name;
  const std::string bt = config_.adapter_bluetooth.empty() ? "02:00:00:00:00:02" : config_.adapter_bluetooth;
  const std::string device_id = pairing_device_id();
  // Prefer configured physical millimeters; otherwise derive ~240 PPI for anti-aliased icons.
  int physical_w = config_.physical_width_mm;
  int physical_h = config_.physical_height_mm;
  if (physical_w <= 0 || physical_h <= 0) {
    physical_w = std::max(1, (width * 254 + 1200) / 2400);
    physical_h = std::max(1, (height * 254 + 1200) / 2400);
  }
  Plist view = Plist::dict({{"widthPixels", Plist::integer(width)},
                            {"heightPixels", Plist::integer(height)},
                            {"originXPixels", Plist::integer(0)},
                            {"originYPixels", Plist::integer(0)},
                            {"safeArea", Plist::dict({{"widthPixels", Plist::integer(width)},
                                                      {"heightPixels", Plist::integer(height)},
                                                      {"originXPixels", Plist::integer(0)},
                                                      {"originYPixels", Plist::integer(0)},
                                                      {"drawUIOutsideSafeArea", Plist::boolean(true)}})}});

  // Match MacPlay: do not advertise maxBitRate. Phone bitrate follows Wi-Fi
  // AverageTxCapacity; a CarPlay SoftAP (Apple Device IE) raises that ceiling.
  PlistDict display_fields = {{"uuid", Plist::string(kMainUuid)},
                              {"type", Plist::integer(110)},
                              {"maxFPS", Plist::integer(fps)},
                              {"widthPixels", Plist::integer(width)},
                              {"heightPixels", Plist::integer(height)},
                              {"widthPhysical", Plist::integer(physical_w)},
                              {"heightPhysical", Plist::integer(physical_h)},
                              {"features", Plist::integer(0x0a)},
                              {"primaryInputDevice", Plist::integer(1)},
                              {"viewAreas", Plist::array({std::move(view)})},
                              {"initialViewArea", Plist::integer(0)}};
  if (config_.max_bit_rate > 0) {
    display_fields.emplace("maxBitRate", Plist::integer(config_.max_bit_rate));
  }
  Plist display = Plist::dict(std::move(display_fields));

  Plist modes = Plist::dict(
      {{"resources", Plist::array({resource(1), resource(2)})},
       {"appStates", Plist::array({Plist::dict({{"appStateID", Plist::integer(2)}, {"state", Plist::boolean(false)}}),
                                   Plist::dict({{"appStateID", Plist::integer(1)}, {"speechMode", Plist::integer(-1)}}),
                                   Plist::dict({{"appStateID", Plist::integer(3)}, {"state", Plist::boolean(false)}})})}});

  Plist info = Plist::dict(
      {{"sourceVersion", Plist::string("950.7.1")},
       {"features", Plist::integer(kFeatures)},
       {"statusFlags", Plist::integer(4)},
       {"model", Plist::string("CarPlayCPP")},
       {"manufacturer", Plist::string("CarPlayCPP")},
       {"deviceID", Plist::string(device_id)},
       {"bluetoothIDs", Plist::array({Plist::string(bt)})},
       {"name", Plist::string(name)},
       {"rightHandDrive", Plist::boolean(false)},
       {"keepAliveLowPower", Plist::boolean(false)},
       {"keepAliveSendStatsAsBody", Plist::boolean(false)},
       {"modes", std::move(modes)},
       {"audioFormats",
        Plist::array({Plist::dict({{"type", Plist::integer(100)},
                                   {"audioType", Plist::string("compatibility")},
                                   {"audioOutputFormats", Plist::integer(0xc3fc)},
                                   {"audioInputFormats", Plist::integer(0x4154)}}),
                     Plist::dict({{"type", Plist::integer(101)},
                                  {"audioType", Plist::string("compatibility")},
                                  {"audioOutputFormats", Plist::integer(0xc3fc)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("default")},
                                  {"audioOutputFormats", Plist::integer(0x7000c3fc)},
                                  {"audioInputFormats", Plist::integer(0x70004154)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("alert")},
                                  {"audioOutputFormats", Plist::integer(0x7000c3fc)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("media")},
                                  {"audioOutputFormats", Plist::integer(0x8000)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("telephony")},
                                  {"audioOutputFormats", Plist::integer(0x70004154)},
                                  {"audioInputFormats", Plist::integer(0x70004154)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("speechRecognition")},
                                  {"audioOutputFormats", Plist::integer(0x70004154)},
                                  {"audioInputFormats", Plist::integer(0x70004154)}}),
                     Plist::dict({{"type", Plist::integer(101)},
                                  {"audioType", Plist::string("default")},
                                  {"audioOutputFormats", Plist::integer(0x7000c3fc)}}),
                     Plist::dict({{"type", Plist::integer(102)},
                                  {"audioType", Plist::string("media")},
                                  {"audioOutputFormats", Plist::integer(0x800000)}})})},
       {"audioLatencies",
        Plist::array({Plist::dict({{"type", Plist::integer(100)},
                                   {"inputLatencyMicros", Plist::integer(0)},
                                   {"outputLatencyMicros", Plist::integer(0)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("default")},
                                  {"inputLatencyMicros", Plist::integer(0)},
                                  {"outputLatencyMicros", Plist::integer(0)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("media")},
                                  {"inputLatencyMicros", Plist::integer(0)},
                                  {"outputLatencyMicros", Plist::integer(0)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("telephony")},
                                  {"inputLatencyMicros", Plist::integer(0)},
                                  {"outputLatencyMicros", Plist::integer(0)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("speechRecognition")},
                                  {"inputLatencyMicros", Plist::integer(0)},
                                  {"outputLatencyMicros", Plist::integer(0)}}),
                     Plist::dict({{"type", Plist::integer(100)},
                                  {"audioType", Plist::string("alert")},
                                  {"inputLatencyMicros", Plist::integer(0)},
                                  {"outputLatencyMicros", Plist::integer(0)}}),
                     Plist::dict({{"type", Plist::integer(101)},
                                  {"inputLatencyMicros", Plist::integer(0)},
                                  {"outputLatencyMicros", Plist::integer(0)}}),
                     Plist::dict({{"type", Plist::integer(101)},
                                  {"audioType", Plist::string("default")},
                                  {"inputLatencyMicros", Plist::integer(0)},
                                  {"outputLatencyMicros", Plist::integer(0)}}),
                     Plist::dict({{"type", Plist::integer(102)},
                                  {"audioType", Plist::string("default")},
                                  {"inputLatencyMicros", Plist::integer(0)},
                                  {"outputLatencyMicros", Plist::integer(0)}})})},
       {"extendedFeatures", Plist::array({Plist::string("vocoderInfo"), Plist::string("enhancedRequestCarUI")})},
       {"displays", Plist::array({std::move(display)})},
       {"hidDevices", Plist::array({hid_entry("2a2a2a2a", "CarPlayCPP Touchscreen", touch_descriptor(width, height)),
                                    hid_entry("2a2a2a2b", "CarPlayCPP Knob", kKnobDesc),
                                    hid_entry("2a2a2a2c", "CarPlayCPP Media", kMediaDesc),
                                    hid_entry("2a2a2a2d", "CarPlayCPP Telephony", kTelDesc)})}});

  // Vehicle brand icon on the CarPlay homescreen (oemIcons).
  if (auto* dict = std::get_if<PlistDict>(&info.value)) {
    PlistArray icons = load_oem_icons();
    if (!icons.empty()) {
      dict->emplace("oemIconVisible", Plist::boolean(true));
      dict->emplace("oemIconLabel", Plist::string(name));
      dict->emplace("oemIcons", Plist::array(std::move(icons)));
      last_line_ += " 品牌图标" + std::to_string(dict->at("oemIcons").as_array() ? dict->at("oemIcons").as_array()->size() : 0);
    }
  }

  const ByteBuffer body = encode_bplist(info);
  last_line_ += " 能力已发送";
  return reply(protocol, 200, "OK", cseq, kPlistType,
               std::string(reinterpret_cast<const char*>(body.data()), body.size()));
}

void ControlSession::stop_stream(int type) {
  if (type == 110 || type == 111) {
    if (screen_) screen_->stop();
    screen_.reset();
    return;
  }
  if (type != 100 && type != 101 && type != 102) return;
  audios_.erase(std::remove_if(audios_.begin(), audios_.end(),
                               [type](const std::unique_ptr<AudioPort>& audio) {
                                 return audio && audio->info().type == type;
                               }),
                audios_.end());
}

void ControlSession::notify_host_ui(const std::string& reason) {
  last_line_ += " " + reason;
  if (status_) status_("车辆键：返回系统界面");
  if (on_host_ui_) on_host_ui_();
}

std::string ControlSession::handle_command(const std::string& protocol, const std::string& cseq,
                                           const std::string& body) {
  Plist root;
  if (!body.empty() &&
      decode_bplist(reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), root)) {
    if (const auto* dict = root.as_dict()) {
      const auto type_it = dict->find("type");
      const std::string type = type_it == dict->end() ? std::string() : type_it->second.as_string();
      last_line_ += " 命令 " + (type.empty() ? "?" : type);
      if (type == "requestUI") {
        // Vehicle/home dock button: requestUI with no url (LinuxPlay treats all requestUI).
        std::string url;
        const auto params = dict->find("params");
        if (params != dict->end() && params->second.is_dict()) {
          const auto* pd = params->second.as_dict();
          const auto u = pd->find("url");
          if (u != pd->end()) url = u->second.as_string();
        }
        if (url.empty() || url.rfind("videoplayback:", 0) != 0) {
          notify_host_ui("控制通道 requestUI");
        } else if (status_) {
          status_("requestUI " + url);
        }
      } else if (!type.empty() && status_) {
        status_("控制命令 " + type);
      }
    }
  }
  return reply(protocol, 200, "OK", cseq, {}, {});
}

void ControlSession::request_keyframe() {
  if (!events_) return;
  const ByteBuffer body = encode_bplist(Plist::dict(
      {{"type", Plist::string("forceKeyFrame")},
       {"params", Plist::dict({{"uuid", Plist::string(kMainUuid)}})}}));
  events_->send_command(body);
}

std::string ControlSession::handle_feedback(const std::string& protocol, const std::string& cseq) {
  if (audios_.empty()) return reply(protocol, 200, "OK", cseq, {}, {});
  PlistArray streams;
  using clock = std::chrono::steady_clock;
  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
  for (const auto& audio : audios_) {
    if (!audio) continue;
    const auto& meta = audio->info();
    PlistDict entry = {{"type", Plist::integer(meta.type)}, {"sampleRate", Plist::integer(meta.sample_rate)}};
    std::uint32_t first = 0;
    std::int64_t origin_ns = 0;
    if (audio->origin(first, origin_ns)) {
      const int lag_ms = std::min(meta.latency_ms, 120);
      const double latency = static_cast<double>(lag_ms) / 1000.0;
      const double elapsed = std::max(0.0, static_cast<double>(now_ns - origin_ns) / 1e9 - latency);
      const auto play = static_cast<std::uint32_t>(
          first + static_cast<std::uint32_t>(std::llround(elapsed * meta.sample_rate)));
      if (meta.id_is_int) entry["streamConnectionID"] = Plist::integer(meta.id_int);
      else entry["streamConnectionID"] = Plist::string(meta.id_text);
      const std::uint64_t ntp = timing_ ? timing_->now_ntp() : 0;
      entry["timestamp"] = Plist::integer(static_cast<std::int64_t>(ntp));
      entry["timestampRawNs"] = Plist::integer(now_ns);
      entry["sampleTime"] = Plist::integer(static_cast<std::int64_t>(play));
    }
    streams.push_back(Plist::dict(std::move(entry)));
  }
  const ByteBuffer encoded = encode_bplist(Plist::dict({{"streams", Plist::array(std::move(streams))}}));
  return reply(protocol, 200, "OK", cseq, kPlistType,
               std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
}

std::string ControlSession::handle_setup(const std::string& protocol, const std::string& cseq,
                                         const std::string& body) {
  Plist root;
  if (!body.empty() &&
      !decode_bplist(reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), root)) {
    return reply(protocol, 400, "Bad Request", cseq, "text/plain", "bad plist");
  }
  const PlistDict* dict = root.as_dict();
  if (dict != nullptr) {
    const auto streams_it = dict->find("streams");
    if (streams_it != dict->end() && streams_it->second.is_array()) {
      PlistArray resp_streams;
      for (const auto& stream : *streams_it->second.as_array()) {
        const auto* sd = stream.as_dict();
        if (sd == nullptr) continue;
        const auto type_it = sd->find("type");
        const int type = type_it == sd->end() ? 0 : static_cast<int>(type_it->second.as_int());
        if (type == 110 || type == 111) {
          std::string stream_id;
          const auto id_it = sd->find("streamConnectionID");
          if (id_it != sd->end()) stream_id = id_it->second.as_string();
          const std::string salt = "DataStream-Salt" + stream_id;
          const ByteBuffer key =
              hkdf_sha512_labeled(verify_.shared_secret(), salt.c_str(), "DataStream-Output-Encryption-Key");
          screen_ = std::make_unique<ScreenPort>();
          // Do not request keyframes from the video accept thread — that races the
          // control/event sockets. RECORD already asks for a keyframe.
          int port = 0;
          std::string error;
          if (!screen_->listen(key, port, error, status_, on_frame_)) {
            return reply(protocol, 500, "Internal Server Error", cseq, "text/plain", error);
          }
          last_line_ += " 视频口" + std::to_string(port) + " id=" + stream_id;
          resp_streams.push_back(Plist::dict({{"type", Plist::integer(type)}, {"dataPort", Plist::integer(port)}}));
        } else if (type == 100 || type == 101 || type == 102) {
          std::string stream_id;
          AudioPort::Info meta;
          meta.type = type;
          const auto id_it = sd->find("streamConnectionID");
          if (id_it != sd->end()) {
            stream_id = id_it->second.as_string();
            if (std::holds_alternative<std::int64_t>(id_it->second.value) ||
                std::holds_alternative<double>(id_it->second.value)) {
              meta.id_is_int = true;
              meta.id_int = id_it->second.as_int();
            } else {
              meta.id_is_int = false;
              meta.id_text = stream_id;
            }
          }
          const auto fmt_it = sd->find("audioFormat");
          const std::int64_t fmt = fmt_it == sd->end() ? 0 : fmt_it->second.as_int();
          const auto lat_it = sd->find("audioLatencyMs");
          meta.latency_ms = lat_it == sd->end() ? 0 : static_cast<int>(lat_it->second.as_int());
          constexpr std::int64_t kAac44 = 0x400000;
          constexpr std::int64_t kAac48 = 0x800000;
          constexpr std::int64_t kOpus = 0x10000000 | 0x20000000 | 0x40000000;
          AudioCodec codec = AudioCodec::Pcm;
          if ((fmt & (kAac44 | kAac48)) != 0) {
            codec = AudioCodec::Aac;
            meta.sample_rate = (fmt & kAac48) ? 48000 : 44100;
            meta.channels = 2;
          } else if ((fmt & kOpus) != 0) {
            codec = AudioCodec::Opus;
            meta.sample_rate = 48000;
            meta.channels = 1;
          } else if (fmt & 0x8000) {
            meta.sample_rate = 48000;
            meta.channels = 2;
          } else if (fmt & 0x4000) {
            meta.sample_rate = 48000;
            meta.channels = 1;
          } else if (fmt & 0x800) {
            meta.sample_rate = 44100;
            meta.channels = 2;
          } else if (fmt & 0x400) {
            meta.sample_rate = 44100;
            meta.channels = 1;
          } else {
            meta.sample_rate = 44100;
            meta.channels = 2;
          }
          if (meta.latency_ms <= 0) meta.latency_ms = (type == 102) ? 1000 : 0;
          const std::string salt = "DataStream-Salt" + stream_id;
          const ByteBuffer key =
              hkdf_sha512_labeled(verify_.shared_secret(), salt.c_str(), "DataStream-Output-Encryption-Key");
          stop_stream(type);
          auto audio = std::make_unique<AudioPort>();
          int data_port = 0;
          int control_port = 0;
          std::string error;
          if (!audio->listen(key, codec, meta, data_port, control_port, error, status_)) {
            return reply(protocol, 500, "Internal Server Error", cseq, "text/plain", error);
          }
          last_line_ += " 音频口" + std::to_string(data_port) + " t=" + std::to_string(type);
          PlistDict resp = {{"type", Plist::integer(type)},
                            {"dataPort", Plist::integer(data_port)},
                            {"controlPort", Plist::integer(control_port)}};
          if (meta.id_is_int) resp["streamConnectionID"] = Plist::integer(meta.id_int);
          else if (!meta.id_text.empty()) resp["streamConnectionID"] = Plist::string(meta.id_text);
          resp_streams.push_back(Plist::dict(std::move(resp)));
          audios_.push_back(std::move(audio));
        } else {
          last_line_ += " 流类型" + std::to_string(type);
        }
      }
      const ByteBuffer encoded = encode_bplist(Plist::dict({{"streams", Plist::array(std::move(resp_streams))}}));
      return reply(protocol, 200, "OK", cseq, kPlistType,
                   std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
    }
  }

  if (!verify_.verified() || verify_.shared_secret().size() != 32) {
    return reply(protocol, 500, "Internal Server Error", cseq, "text/plain", "not verified");
  }

  int timing_port = 0;
  int event_port = 0;
  std::string error;
  timing_ = std::make_unique<TimingSync>();
  events_ = std::make_unique<EventChannel>();
  // requestUI arrives on the control socket (POST /command), not the event channel.
  // Still accept it on the event socket as a fallback for firmware variants.
  if (!timing_->listen(timing_port, error) ||
      !events_->listen(verify_.shared_secret(), event_port, error, status_,
                       [this](const std::string& type, const ByteBuffer& body) {
                         if (type != "requestUI") return;
                         (void)body;
                         notify_host_ui("事件通道 requestUI");
                       })) {
    return reply(protocol, 500, "Internal Server Error", cseq, "text/plain", error);
  }
  int peer_timing = 0;
  bool keep_alive = false;
  if (dict != nullptr) {
    const auto it = dict->find("timingPort");
    if (it != dict->end()) peer_timing = static_cast<int>(it->second.as_int());
    const auto ka = dict->find("keepAliveLowPower");
    if (ka != dict->end()) keep_alive = ka->second.as_bool();
  }
  if (peer_timing > 0 && !peer_host_.empty()) timing_->start(peer_host_, peer_timing);

  PlistDict resp_dict = {{"timingPort", Plist::integer(timing_port)},
                         {"eventPort", Plist::integer(event_port)},
                         {"enabledFeatures", Plist::array({Plist::string("iAPChannel"), Plist::string("viewAreas")})}};
  if (keep_alive) {
    // Phone only needs a reachable UDP port; reuse the timing socket's peer host.
    int ka_port = 0;
    keep_alive_ = std::make_unique<TimingSync>();
    if (keep_alive_->listen(ka_port, error)) resp_dict["keepAlivePort"] = Plist::integer(ka_port);
  }
  Plist resp = Plist::dict(std::move(resp_dict));
  const ByteBuffer encoded = encode_bplist(resp);
  last_line_ += " 会话口 t=" + std::to_string(timing_port) + " e=" + std::to_string(event_port) +
                " peerT=" + std::to_string(peer_timing);
  return reply(protocol, 200, "OK", cseq, kPlistType,
               std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
}

std::string ControlSession::handle_message(const std::string& message) {
  const auto line_end = message.find("\r\n");
  const std::string line = line_end == std::string::npos ? message : message.substr(0, line_end);
  const auto cseq = header_value(message, "CSeq");
  const bool rtsp = line.find("RTSP/") != std::string::npos;
  std::string protocol = "RTSP/1.0";
  const auto proto_at = line.rfind(' ');
  if (proto_at != std::string::npos) protocol = line.substr(proto_at + 1);
  const auto header_end = message.find("\r\n\r\n");
  const std::string body = header_end == std::string::npos ? std::string() : message.substr(header_end + 4);

  if (line.find("/pair-setup") != std::string::npos) {
    const ByteBuffer response = pair_.handle(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    const auto state = Tlv8Codec::find(Tlv8Codec::decode(response), 0x06);
    if (state && !state->empty()) last_line_ += " 配对应答" + std::to_string((*state)[0]);
    return reply(protocol, 200, "OK", cseq, "application/pairing+tlv8",
                 std::string(reinterpret_cast<const char*>(response.data()), response.size()));
  }

  if (line.rfind("OPTIONS ", 0) == 0) {
    if (rtsp) {
      return rtsp_response(200, cseq.empty() ? "1" : cseq,
                           "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
    }
    return http_response(200, {}, {},
                         "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, POST, GET\r\n");
  }

  if (line.find("/pair-verify") != std::string::npos) {
    const ByteBuffer response = verify_.handle(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    const auto state = Tlv8Codec::find(Tlv8Codec::decode(response), 0x06);
    if (state && !state->empty()) last_line_ += " 校验应答" + std::to_string((*state)[0]);
    return reply(protocol, 200, "OK", cseq, "application/pairing+tlv8",
                 std::string(reinterpret_cast<const char*>(response.data()), response.size()));
  }

  if (line.find("/auth-setup") != std::string::npos) {
    if (signer_ == nullptr) return reply(protocol, 500, "Internal Server Error", cseq, "text/plain", "no signer");
    const ByteBuffer response =
        auth_setup_response(reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), *signer_);
    if (response.empty()) return reply(protocol, 400, "Bad Request", cseq, "text/plain", "auth-setup failed");
    last_line_ += " 认证已签名";
    return reply(protocol, 200, "OK", cseq, "application/octet-stream",
                 std::string(reinterpret_cast<const char*>(response.data()), response.size()));
  }

  if (line.find("/info") != std::string::npos) return handle_info(protocol, cseq);

  if (line.find("/feedback") != std::string::npos) return handle_feedback(protocol, cseq);

  if (line.find("/command") != std::string::npos) return handle_command(protocol, cseq, body);

  if (line.rfind("SETUP ", 0) == 0) return handle_setup(protocol, cseq, body);

  if (line.rfind("RECORD ", 0) == 0) {
    last_line_ += " 会话开始";
    if (events_) events_->mark_record_ready();
    request_keyframe();
    return rtsp_response(200, cseq.empty() ? "1" : cseq);
  }

  if (line.rfind("TEARDOWN ", 0) == 0) {
    bool leave_ui = false;
    Plist root;
    if (!body.empty() && decode_bplist(reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), root)) {
      if (const auto* dict = root.as_dict()) {
        const auto streams = dict->find("streams");
        if (streams != dict->end() && streams->second.is_array()) {
          last_line_ += " 拆流";
          for (const auto& s : *streams->second.as_array()) {
            if (const auto* sd = s.as_dict()) {
              const auto t = sd->find("type");
              if (t != sd->end()) {
                const int type = static_cast<int>(t->second.as_int());
                last_line_ += " " + std::to_string(type);
                stop_stream(type);
                if (type == 110 || type == 111) leave_ui = true;
              }
            }
          }
        } else {
          last_line_ += " 拆会话";
          stop_stream(110);
          stop_stream(100);
          stop_stream(101);
          stop_stream(102);
          leave_ui = true;
        }
      }
    } else {
      last_line_ += " 拆线";
      stop_stream(110);
      stop_stream(100);
      stop_stream(101);
      stop_stream(102);
      leave_ui = true;
    }
    if (leave_ui) notify_host_ui("iPhone 已退出 CarPlay");
    return rtsp_response(200, cseq.empty() ? "1" : cseq);
  }

  if (rtsp) return rtsp_response(200, cseq.empty() ? "1" : cseq);
  return http_response(200, {}, {});
}

}  // namespace carplay
