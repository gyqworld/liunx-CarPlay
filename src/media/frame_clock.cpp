#include "carplay/media/frame_clock.hpp"

#include <chrono>

namespace carplay {

std::uint64_t FrameClock::now_us() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace carplay
