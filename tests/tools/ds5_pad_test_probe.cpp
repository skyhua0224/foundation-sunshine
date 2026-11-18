/**
 * @file tests/tools/ds5_pad_test_probe.cpp
 * @brief What the pad tests (identify, triggers, stop) put into the feedback queue of a native DualSense, with their timing
 *        (measurement tool, not a unit test).
 *
 * usage: ds5_pad_test_probe
 */
#ifdef _WIN32

  #include <windows.h>

  #include <chrono>
  #include <cstdio>
  #include <memory>
  #include <thread>
  #include <vector>

  #include <boost/log/sources/severity_logger.hpp>

  #include "src/ds5/pad_test.h"
  #include "src/platform/windows/ds5/ds5_native_client.h"
  #include "src/thread_safe.h"

boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> debug;

namespace {
  using clock_type = std::chrono::steady_clock;

  void
  drain(const char *label, const platf::feedback_queue_t &queue, clock_type::time_point begin) {
    while (auto message = queue->pop(std::chrono::milliseconds(20))) {
      const auto at = std::chrono::duration<double, std::milli>(clock_type::now() - begin).count();
      switch (message->type) {
        case platf::gamepad_feedback_e::set_rgb_led:
          std::printf("PAD_TEST %s t=%.0fms rgb=%u,%u,%u\n", label, at, static_cast<unsigned>(message->data.rgb_led.r), static_cast<unsigned>(message->data.rgb_led.g), static_cast<unsigned>(message->data.rgb_led.b));
          break;
        case platf::gamepad_feedback_e::set_adaptive_triggers: {
          const auto &t = message->data.adaptive_triggers;
          std::printf("PAD_TEST %s t=%.0fms triggers flags=0x%02x left=0x%02x right=0x%02x left_zones=%02x%02x right_zones=%02x%02x\n", label, at, static_cast<unsigned>(t.event_flags),
            static_cast<unsigned>(t.type_left), static_cast<unsigned>(t.type_right), static_cast<unsigned>(t.left[1]), static_cast<unsigned>(t.left[0]), static_cast<unsigned>(t.right[1]), static_cast<unsigned>(t.right[0]));
          break;
        }
        case platf::gamepad_feedback_e::rumble:
          std::printf("PAD_TEST %s t=%.0fms rumble=%u,%u\n", label, at, static_cast<unsigned>(message->data.rumble.lowfreq), static_cast<unsigned>(message->data.rumble.highfreq));
          break;
        default:
          break;
      }
    }
  }
}  // namespace

int
main() {
  timeBeginPeriod(1);
  auto mail = std::make_shared<safe::mail_raw_t>();
  platf::ds5::native_client_t client;
  auto queue = mail->queue<platf::gamepad_feedback_msg_t>("ds5-pad-test-probe");
  if (client.alloc({ 0, 0 }, queue, true) != 0) {
    std::puts("FAIL alloc");
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));
  drain("startup", queue, clock_type::now());
  for (const auto action : { ds5::pad_test::action_e::identify, ds5::pad_test::action_e::trigger_left, ds5::pad_test::action_e::trigger_right, ds5::pad_test::action_e::stop }) {
    const char *name = action == ds5::pad_test::action_e::identify ? "identify" : action == ds5::pad_test::action_e::trigger_left ? "trigger_left" : action == ds5::pad_test::action_e::trigger_right ? "trigger_right" : "stop";
    const auto begin = clock_type::now();
    std::thread watcher([&] { for (int i = 0; i < 60; ++i) { drain(name, queue, begin); } });
    const auto result = ds5::pad_test::run(action, [](std::chrono::milliseconds wait) { std::this_thread::sleep_for(wait); });
    const auto took = std::chrono::duration<double, std::milli>(clock_type::now() - begin).count();
    watcher.join();
    std::printf("PAD_TEST_RESULT %s result=%s pads=%u took=%.0fms\n", name, ds5::pad_test::to_string(result.outcome), result.pads, took);
  }
  /* stop while a trigger test waits */
  {
    const auto begin = clock_type::now();
    std::thread stopper([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      const auto stopped = ds5::pad_test::run(ds5::pad_test::action_e::stop, [](auto) {});
      std::printf("PAD_TEST_RESULT stop_during result=%s\n", ds5::pad_test::to_string(stopped.outcome));
    });
    const auto result = ds5::pad_test::run(ds5::pad_test::action_e::trigger_left, [](std::chrono::milliseconds wait) { std::this_thread::sleep_for(wait); });
    stopper.join();
    std::printf("PAD_TEST_RESULT trigger_left_stopped result=%s took=%.0fms\n", ds5::pad_test::to_string(result.outcome), std::chrono::duration<double, std::milli>(clock_type::now() - begin).count());
    drain("after_stop", queue, begin);
  }
  client.free(0);
  std::puts("DONE");
  return 0;
}

#else
int
main() {
  return 0;
}
#endif
