#pragma once

#include <cstdint>

namespace carplay {

/** Simple monotonic microsecond clock for A/V pacing stubs. */
class FrameClock {
 public:
  static std::uint64_t now_us();
};

}  // namespace carplay
