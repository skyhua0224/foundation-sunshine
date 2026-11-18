/**
 * @file src/platform/windows/ds5/ds5_probe_stats.h
 * @brief Measurement hook for the interrupt IN path of the native virtual DualSense (test builds only).
 *
 * Records, per interrupt URB, how long after the previous reply the USB/IP client submitted the next
 * request, how many requests were already waiting, and how many replies were sent.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace platf::ds5::probe {
  struct stats_t {
    std::mutex mutex;
    std::vector<double> resubmit_us;
    std::vector<unsigned> held_at_arrival;
    std::uint64_t urbs {};
    std::uint64_t replies {};
  };

  inline stats_t &
  stats() {
    static stats_t instance;
    return instance;
  }

  inline std::int64_t
  now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
}  // namespace platf::ds5::probe
