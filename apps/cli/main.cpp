#include "carplay/protocol/version.hpp"
#include "carplay/session/controller.hpp"
#include "carplay/session/settings_io.hpp"

#include <csignal>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

void usage() {
  std::cerr << "carplay-cli 0.1.0\n"
            << "  doctor                 protocol self-test\n"
            << "  status                 print library version and data dir\n"
            << "  start                  TCP 17000 + iAP2 receiver (Ctrl-C to stop)\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::string cmd = argc > 1 ? argv[1] : "doctor";
  auto controller = carplay::make_default_controller();

  if (cmd == "doctor" || cmd == "--help" || cmd == "-h") {
    if (cmd != "doctor") {
      usage();
      return cmd == "--help" || cmd == "-h" ? 0 : 2;
    }
    const auto report = controller->doctor();
    std::cout << "version " << report.library_version << "\n" << report.detail << "\n";
    return report.ok ? 0 : 1;
  }

  if (cmd == "status") {
    std::cout << "version " << carplay::library_version() << "\n"
              << "state " << carplay::session_state_name(controller->state()) << "\n"
              << "data " << controller->storage().data_dir() << "\n";
    return 0;
  }

  if (cmd == "start") {
    carplay::RuntimeConfig cfg;
    carplay::load_runtime_config(controller->storage(), cfg);
    if (cfg.target_bluetooth.empty()) {
      std::cerr << "请先在界面保存 iPhone 蓝牙地址，或写入 ~/.config/carplay-cpp/settings.json\n";
      return 2;
    }
    controller->set_config(cfg);
    controller->set_status_handler([](const std::string& status) { std::cout << status << "\n" << std::flush; });
    std::cout << "target " << cfg.target_bluetooth << " ssid " << cfg.ssid << "\n";
    const bool ok = controller->start();
    std::cout << "state " << carplay::session_state_name(controller->state()) << "\n";
    if (!controller->last_error().empty()) std::cout << controller->last_error() << "\n";
    if (!ok) return 1;
    static volatile std::sig_atomic_t stopping = 0;
    std::signal(SIGINT, [](int) { stopping = 1; });
    std::signal(SIGTERM, [](int) { stopping = 1; });
    while (!stopping) pause();
    controller->stop();
    return 0;
  }

  usage();
  return 2;
}
