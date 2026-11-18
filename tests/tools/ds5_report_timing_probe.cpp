/**
 * @file tests/tools/ds5_report_timing_probe.cpp
 * @brief How often a game can read new DualSense reports from the native virtual pad (measurement tool, not a unit test).
 *
 * Attaches N native virtual pads over USB/IP, reads every pad's HID input reports with high-resolution timestamps while this
 * process injects client input (sticks, buttons, motion) at fixed rates, and prints per-phase statistics:
 * read intervals (all reads and reads that carried a changed value), submit-to-read latency, motion continuity,
 * lost reports (replies sent by the device minus reads completed), host CPU and the USB/IP client's re-submit delay.
 *
 * usage: ds5_report_timing_probe [pads=1] [phase_ms=6000] [phases=sticks,buttons,gyro,mixed,stall40,stall80,stall160,idle,burstidle,sparseidle,tap0,tap80,tap120,tap160]
 */
#ifdef _WIN32

  #include <windows.h>

  #include <hidsdi.h>
  #include <setupapi.h>
  #include <tlhelp32.h>

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cmath>
  #include <cstdint>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <vector>

  #include <boost/log/sources/severity_logger.hpp>

  #include "src/ds5/tuning_profile.h"
  #include "src/platform/windows/ds5/ds5_native_client.h"
  #include "src/platform/windows/ds5/ds5_probe_stats.h"
  #include "src/thread_safe.h"

boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> debug;

namespace {
  using clock_type = std::chrono::steady_clock;

  /** Save key 101 (the motion report rate the host asks clients for) through the real tuning store; the saved value is published to the runtime and pushed to the live pads. */
  bool
  save_motion_cap(ds5::tuning::store_t &store, unsigned hz) {
    const auto result = store.apply_json({ { "schema", "ALK-DS5T/1" }, { "values", { { "ds5.compat.reportRateCapHz", hz } } } });
    if (!result.ok) std::printf("save of key 101 = %u failed: %s\n", hz, result.error.c_str());
    return result.ok;
  }

  /** What the clients are asked for: every set_motion_event_state waiting in the feedback queues of the pads. */
  void
  print_motion_requests(const char *when, const std::vector<platf::feedback_queue_t> &queues) {
    for (std::size_t index = 0; index != queues.size(); ++index) {
      while (auto message = queues[index]->pop(std::chrono::milliseconds(50))) {
        if (message->type != platf::gamepad_feedback_e::set_motion_event_state) continue;
        std::printf("MOTION_REQUEST %s pad=%zu type=%s rate=%u Hz\n", when, index,
          message->data.motion_event_state.motion_type == LI_MOTION_TYPE_GYRO ? "gyro" : "accel",
          static_cast<unsigned>(message->data.motion_event_state.report_rate));
      }
    }
  }

  double
  qpc_us() {
    static const double frequency = [] {
      LARGE_INTEGER value;
      QueryPerformanceFrequency(&value);
      return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1e6 / frequency;
  }

  double
  thread_cpu_ms(HANDLE thread) {
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(thread, &creation, &exit, &kernel, &user)) return 0.0;
    const auto to_ms = [](FILETIME value) {
      return static_cast<double>((static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime) / 10000.0;
    };
    return to_ms(kernel) + to_ms(user);
  }

  void
  name_this_thread(const wchar_t *name) {
    using fn_t = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static const auto fn = reinterpret_cast<fn_t>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "SetThreadDescription")));
    if (fn) fn(GetCurrentThread(), name);
  }

  struct thread_cpu_t {
    double cpu_ms {};
    double kernel_ms {};
    std::string name;
  };

  // CPU time of every thread of this process by thread id, with the name set by name_this_thread() (or "?")
  std::map<DWORD, thread_cpu_t>
  snapshot_threads() {
    using get_fn_t = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static const auto get_description = reinterpret_cast<get_fn_t>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription")));
    std::map<DWORD, thread_cpu_t> result;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    THREADENTRY32 entry { sizeof(entry) };
    for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
      const auto thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
      if (!thread) continue;
      thread_cpu_t value;
      value.cpu_ms = thread_cpu_ms(thread);
      {
        FILETIME creation, exit, kernel, user;
        if (GetThreadTimes(thread, &creation, &exit, &kernel, &user))
          value.kernel_ms = static_cast<double>((static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) / 10000.0;
      }
      value.name = "?";
      PWSTR description = nullptr;
      if (get_description && SUCCEEDED(get_description(thread, &description)) && description) {
        std::string narrow;
        for (auto *c = description; *c; ++c) narrow.push_back(static_cast<char>(*c));
        if (!narrow.empty()) value.name = narrow;
        LocalFree(description);
      }
      CloseHandle(thread);
      result.emplace(entry.th32ThreadID, std::move(value));
    }
    CloseHandle(snapshot);
    return result;
  }

  struct system_times_t {
    double idle {}, kernel {}, user {};
  };

  system_times_t
  system_times() {
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    const auto to_ms = [](FILETIME value) {
      return static_cast<double>((static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime) / 10000.0;
    };
    return { to_ms(idle), to_ms(kernel), to_ms(user) };
  }

  double
  process_cpu_ms() {
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    const auto to_ms = [](FILETIME value) {
      return static_cast<double>((static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime) / 10000.0;
    };
    return to_ms(kernel) + to_ms(user);
  }

  struct summary_t {
    std::size_t count {};
    double p50 {}, p95 {}, p99 {}, max {}, mean {};
  };

  summary_t
  summarize(std::vector<double> values) {
    summary_t result;
    if (values.empty()) return result;
    std::sort(values.begin(), values.end());
    const auto pick = [&](double fraction) { return values[std::min(values.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(values.size())))]; };
    result.count = values.size();
    result.p50 = pick(0.50);
    result.p95 = pick(0.95);
    result.p99 = pick(0.99);
    result.max = values.back();
    double sum = 0;
    for (auto value : values) sum += value;
    result.mean = sum / static_cast<double>(values.size());
    return result;
  }

  void
  print_summary(const char *label, const summary_t &s, const char *unit) {
    std::printf("    %-34s n=%-6zu mean=%9.1f p50=%9.1f p95=%9.1f p99=%9.1f max=%10.1f %s\n", label, s.count, s.mean, s.p50, s.p95, s.p99, s.max, unit);
  }

  struct read_t {
    double at_us;
    std::array<std::uint8_t, 64> report;
  };

  struct pad_t {
    int index { -1 };
    HANDLE file { INVALID_HANDLE_VALUE };
    std::thread reader;
    std::atomic_bool stop { false };
    std::atomic_uint stall_ms { 0 };  // when set, the reader sleeps this long every 200 ms
    std::vector<read_t> reads;
    std::mutex reads_mutex;
    bool recording { false };
    std::atomic<HANDLE> reader_handle { nullptr };
    double reader_cpu_ms { 0 };
  };

  std::vector<HANDLE>
  open_virtual_hids() {
    std::vector<HANDLE> result;
    GUID guid {};
    HidD_GetHidGuid(&guid);
    auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return result;
    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA entry { sizeof(entry) };
      if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &entry)) break;
      DWORD size = 0;
      SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
      if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
      std::vector<std::uint8_t> storage(size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(*detail);
      if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr)) continue;
      if (std::wstring(detail->DevicePath).find(L"vid_054c&pid_0ce6") == std::wstring::npos) continue;
      auto file = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (file != INVALID_HANDLE_VALUE) result.push_back(file);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
  }

  std::int16_t
  le16(const std::array<std::uint8_t, 64> &report, std::size_t offset) {
    return static_cast<std::int16_t>(report[offset] | (report[offset + 1] << 8));
  }

  std::uint32_t
  le32(const std::array<std::uint8_t, 64> &report, std::size_t offset) {
    return static_cast<std::uint32_t>(report[offset]) | (static_cast<std::uint32_t>(report[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(report[offset + 2]) << 16) | (static_cast<std::uint32_t>(report[offset + 3]) << 24);
  }

  struct injection_t {
    double at_us;
    int pad;
    int kind;  // 0 stick, 1 button, 2 gyro
    int value;
  };

  struct phase_options_t {
    const char *name;
    unsigned stick_hz;
    unsigned button_hz;
    unsigned gyro_hz;
    unsigned stall_ms;
    unsigned inject_ms;  // 0: inject for the whole phase; otherwise only for the first inject_ms, then the pad is left idle
    unsigned tap_ms { 0 };  // short presses of A that last this long ...
    unsigned tap_period_ms { 0 };  // ... one every tap_period_ms (0: none)
  };

  double g_injector_cpu_ms = 0;
  unsigned g_taps_injected = 0;

  void
  run_phase(platf::ds5::native_client_t &client, std::vector<std::unique_ptr<pad_t>> &pads, const phase_options_t &options, unsigned phase_ms) {
    namespace probe = platf::ds5::probe;
    for (auto &pad : pads) {
      std::lock_guard lock(pad->reads_mutex);
      pad->recording = false;
      pad->reads.clear();
      pad->reads.reserve(static_cast<std::size_t>(phase_ms) * 3);
      pad->stall_ms.store(options.stall_ms);
    }
    std::vector<injection_t> injections;
    injections.reserve(static_cast<std::size_t>(phase_ms) * (options.stick_hz + options.button_hz + options.gyro_hz) * pads.size() / 1000 + 100);
    for (auto &pad : pads) {
      platf::gamepad_state_t neutral {};
      client.submit_input(pad->index, neutral);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (auto &pad : pads) {
      std::lock_guard lock(pad->reads_mutex);
      pad->reads.clear();
      pad->recording = true;
    }
    {
      // counted over the same window as the reads
      auto &stats = probe::stats();
      std::lock_guard lock(stats.mutex);
      stats.resubmit_us.clear();
      stats.held_at_arrival.clear();
      stats.urbs = 0;
      stats.replies = 0;
    }

    const auto threads_before = snapshot_threads();
    const double cpu_before = process_cpu_ms();
    const auto system_before = system_times();
    double reader_before = 0;
    for (auto &pad : pads) reader_before += thread_cpu_ms(pad->reader_handle.load());
    g_taps_injected = 0;
    const auto begin = clock_type::now();
    const double begin_us = qpc_us();
    std::thread injector([&] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
      name_this_thread(L"probe-injector");
      const auto period = [](unsigned hz) { return hz == 0 ? clock_type::duration::max() : std::chrono::duration_cast<clock_type::duration>(std::chrono::microseconds(1000000 / hz)); };
      auto stick_next = begin, button_next = begin + std::chrono::microseconds(1300), gyro_next = begin + std::chrono::microseconds(700);
      HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
      unsigned stick_n = 0, button_n = 0, gyro_n = 0;
      auto tap_next = begin + std::chrono::microseconds(2300);
      auto tap_release = clock_type::time_point::max();
      bool tap_down = false;
      std::vector<platf::gamepad_state_t> state(pads.size());
      const auto end = begin + std::chrono::milliseconds(phase_ms);
      const auto inject_end = options.inject_ms ? begin + std::chrono::milliseconds(options.inject_ms) : end;
      while (clock_type::now() < end) {
        const auto now = clock_type::now();
        if (now >= inject_end) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        bool did = false;
        if (options.stick_hz && now >= stick_next) {
          stick_next += period(options.stick_hz);
          ++stick_n;
          const auto marker = static_cast<int>(1u + stick_n % 255u);
          for (std::size_t p = 0; p != pads.size(); ++p) {
            state[p].lsX = static_cast<std::int16_t>(marker * 257 - 32768);
            injections.push_back({ qpc_us(), static_cast<int>(p), 0, marker });
            client.submit_input(pads[p]->index, state[p]);
          }
          did = true;
        }
        if (options.button_hz && now >= button_next) {
          button_next += period(options.button_hz);
          ++button_n;
          const int down = static_cast<int>(button_n & 1u);
          for (std::size_t p = 0; p != pads.size(); ++p) {
            state[p].buttonFlags = down ? platf::A : 0;
            injections.push_back({ qpc_us(), static_cast<int>(p), 1, down });
            client.submit_input(pads[p]->index, state[p]);
          }
          did = true;
        }
        if (options.tap_period_ms) {
          if (!tap_down && now >= tap_next) {
            tap_down = true;
            tap_release = now + std::chrono::milliseconds(options.tap_ms);
            tap_next += std::chrono::milliseconds(options.tap_period_ms);
            ++g_taps_injected;
            for (std::size_t p = 0; p != pads.size(); ++p) {
              state[p].buttonFlags = platf::A;
              client.submit_input(pads[p]->index, state[p]);
            }
            did = true;
          }
          else if (tap_down && now >= tap_release) {
            tap_down = false;
            tap_release = clock_type::time_point::max();
            for (std::size_t p = 0; p != pads.size(); ++p) {
              state[p].buttonFlags = 0;
              client.submit_input(pads[p]->index, state[p]);
            }
            did = true;
          }
        }
        if (options.gyro_hz && now >= gyro_next) {
          gyro_next += period(options.gyro_hz);
          ++gyro_n;
          // slow sine, 20 s period, +-90 deg/s: consecutive samples differ by a few counts only
          const double t = static_cast<double>(gyro_n) / static_cast<double>(options.gyro_hz);
          /* ALK_PROBE_GYRO_ALT: every sample differs from the one before (a 3 deg/s step each time), so the reads that carry a new gyro value count the samples, not the quantisation of a slow sine */
          static const bool alternate = std::getenv("ALK_PROBE_GYRO_ALT") != nullptr;
          const float x = static_cast<float>(90.0 * std::sin(2.0 * 3.14159265358979 * t / 20.0)) + (alternate && (gyro_n & 1u) != 0u ? 3.0f : 0.0f);
          for (std::size_t p = 0; p != pads.size(); ++p) {
            platf::gamepad_motion_t gyro { { pads[p]->index, static_cast<std::uint8_t>(pads[p]->index) }, LI_MOTION_TYPE_GYRO, x, 0.0f, 0.0f };
            injections.push_back({ qpc_us(), static_cast<int>(p), 2, static_cast<int>(std::lround(x * 100.0f)) });
            client.submit_motion(gyro);
          }
          did = true;
        }
        if (!did) {
          // sleep until the earliest next event instead of spinning, so the process CPU is the real cost of the work
          auto wake = std::min({ options.stick_hz ? stick_next : clock_type::time_point::max(), options.button_hz ? button_next : clock_type::time_point::max(),
            options.gyro_hz ? gyro_next : clock_type::time_point::max(),
            options.tap_period_ms ? (tap_down ? tap_release : tap_next) : clock_type::time_point::max() });
          const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(wake - clock_type::now()).count();
          if (wait > 200 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(wait - 150) * 10;  // 100 ns units, relative; wake slightly early, then yield
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 50);
          }
          else std::this_thread::yield();
        }
      }
      if (timer) CloseHandle(timer);
      g_injector_cpu_ms = thread_cpu_ms(GetCurrentThread());
    });
    injector.join();
    const double wall_ms = (qpc_us() - begin_us) / 1000.0;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::uint64_t urbs = 0, replies = 0;
    std::vector<double> resubmit;
    std::vector<unsigned> held;
    for (auto &pad : pads) pad->reads_mutex.lock();
    {
      auto &stats = probe::stats();
      std::lock_guard lock(stats.mutex);
      urbs = stats.urbs;
      replies = stats.replies;
      resubmit = stats.resubmit_us;
      held = stats.held_at_arrival;
    }
    for (auto &pad : pads) {
      pad->recording = false;
      pad->reads_mutex.unlock();
    }
    const auto threads_after = snapshot_threads();
    const double cpu_after = process_cpu_ms();
    const auto system_after = system_times();
    double reader_after = 0;
    for (auto &pad : pads) reader_after += thread_cpu_ms(pad->reader_handle.load());

    std::size_t total_reads = 0;
    for (auto &pad : pads) total_reads += pad->reads.size();

    std::printf("PHASE %s pads=%zu phase_ms=%u stick_hz=%u button_hz=%u gyro_hz=%u reader_stall_ms=%u\n", options.name, pads.size(), phase_ms,
      options.stick_hz, options.button_hz, options.gyro_hz, options.stall_ms);
    std::printf("  host_cpu: process=%.1f ms over %.0f ms wall (%.2f%% of one core); readers=%.1f ms, injector=%.1f ms => host path=%.2f%% of one core, %.2f%% per pad\n",
      cpu_after - cpu_before, wall_ms, 100.0 * (cpu_after - cpu_before) / wall_ms, reader_after - reader_before, g_injector_cpu_ms,
      100.0 * (cpu_after - cpu_before - (reader_after - reader_before) - g_injector_cpu_ms) / wall_ms,
      100.0 * (cpu_after - cpu_before - (reader_after - reader_before) - g_injector_cpu_ms) / wall_ms / static_cast<double>(pads.size()));
    {
      // CPU per thread over the phase: the injector and readers are the probe itself, the rest is the host path
      struct slot_t {
        double cpu_ms {}, kernel_ms {};
        int count {};
      };
      std::map<std::string, slot_t> by_name;
      for (const auto &[tid, after] : threads_after) {
        const auto before = threads_before.find(tid);
        const double delta = after.cpu_ms - (before != threads_before.end() ? before->second.cpu_ms : 0.0);
        if (delta < 0.5) continue;
        auto &slot = by_name[after.name];
        slot.cpu_ms += delta;
        slot.kernel_ms += after.kernel_ms - (before != threads_before.end() ? before->second.kernel_ms : 0.0);
        ++slot.count;
      }
      std::printf("  threads:");
      for (const auto &[name, slot] : by_name)
        std::printf(" %s x%d=%.1f ms (%.2f%% of one core, kernel %.0f%%);", name.c_str(), slot.count, slot.cpu_ms, 100.0 * slot.cpu_ms / wall_ms,
          slot.cpu_ms > 0 ? 100.0 * slot.kernel_ms / slot.cpu_ms : 0.0);
      std::printf("\n");
    }
    {
      // kernel time includes the idle time, DPCs and interrupts (usbip-win2 and the HID stack run there)
      const double idle = system_after.idle - system_before.idle;
      const double kernel = system_after.kernel - system_before.kernel;
      const double user = system_after.user - system_before.user;
      const double total = kernel + user;
      std::printf("  system_cpu: busy=%.2f%% of all CPUs (%.2f cores of %u), kernel-mode work=%.2f%%, user-mode work=%.2f%% of all CPUs\n", 100.0 * (total - idle) / total,
        (total - idle) / total * static_cast<double>(std::thread::hardware_concurrency()), std::thread::hardware_concurrency(), 100.0 * (kernel - idle) / total, 100.0 * user / total);
    }
    std::printf("  reports: device replies=%llu, urb requests=%llu, reads completed by readers=%zu, lost (replies-reads)=%lld (%.2f%%)\n",
      static_cast<unsigned long long>(replies), static_cast<unsigned long long>(urbs), total_reads, static_cast<long long>(replies) - static_cast<long long>(total_reads),
      replies ? 100.0 * (static_cast<double>(replies) - static_cast<double>(total_reads)) / static_cast<double>(replies) : 0.0);
    // usbip-win2 re-submit speed
    {
      std::vector<double> positive;
      for (auto value : resubmit) {
        if (value >= 0) positive.push_back(value);
      }
      print_summary("usbip-win2 re-submit after reply", summarize(positive), "us");
      std::size_t pending = 0;
      for (auto value : held) pending += value > 0 ? 1 : 0;
      std::printf("    requests that found another one already waiting: %zu of %zu\n", pending, held.size());
    }
    for (auto &pad : pads) {
      auto &reads = pad->reads;
      std::vector<double> all, fresh_stick, fresh_button, fresh_gyro, motion_ts_dt_us, latency_stick, latency_button;
      for (std::size_t i = 1; i < reads.size(); ++i) all.push_back(reads[i].at_us - reads[i - 1].at_us);
      std::size_t gyro_spikes = 0, gyro_changes = 0, ts_backwards = 0, ts_large = 0;
      std::vector<double> steps;
      std::uint32_t previous_ts = 0;
      bool have_ts = false;
      // stick/button/gyro: fresh-read intervals (time between two reads that each carried a different value than the read before)
      double prev_stick_change_at = -1, prev_button_change_at = -1, prev_gyro_change_at = -1;
      int prev_stick = -1, prev_button = -1;
      std::int16_t prev_g = 0;
      bool have_prev_g = false;
      for (std::size_t i = 0; i < reads.size(); ++i) {
        const auto &r = reads[i];
        const int stick = r.report[1];
        const int button = r.report[8] & 0x20;
        const auto gyro = le16(r.report, 16);
        if (prev_stick >= 0 && stick != prev_stick) {
          if (prev_stick_change_at >= 0) fresh_stick.push_back(r.at_us - prev_stick_change_at);
          prev_stick_change_at = r.at_us;
        }
        if (prev_button >= 0 && button != prev_button) {
          if (prev_button_change_at >= 0) fresh_button.push_back(r.at_us - prev_button_change_at);
          prev_button_change_at = r.at_us;
        }
        if (have_prev_g && gyro != prev_g) {
          ++gyro_changes;
          if (prev_gyro_change_at >= 0) fresh_gyro.push_back(r.at_us - prev_gyro_change_at);
          prev_gyro_change_at = r.at_us;
          if (!have_ts || static_cast<std::int32_t>(le32(r.report, 28) - previous_ts) < 3 * 20000) steps.push_back(std::abs(static_cast<double>(gyro) - static_cast<double>(prev_g)));
        }
        prev_stick = stick;
        prev_button = button;
        prev_g = gyro;
        have_prev_g = true;
        const auto ts = le32(r.report, 28);
        if (have_ts && ts != previous_ts) {
          const auto delta = static_cast<std::int32_t>(ts - previous_ts);
          if (delta < 0) ++ts_backwards;
          else {
            const double dt_us = static_cast<double>(delta) / 3.0;
            motion_ts_dt_us.push_back(dt_us);
            if (dt_us > 20000.0) ++ts_large;
          }
        }
        previous_ts = ts;
        have_ts = true;
      }
      // submit-to-read latency
      std::size_t cursor = 0;
      for (const auto &inj : injections) {
        if (inj.pad != static_cast<int>(&pad - pads.data()) || inj.kind == 2) continue;
        while (cursor != reads.size() && reads[cursor].at_us < inj.at_us) ++cursor;
        for (auto probe_at = cursor; probe_at != reads.size() && probe_at < cursor + 16; ++probe_at) {
          const auto &r = reads[probe_at];
          bool match = false;
          if (inj.kind == 0) {
            const int delta = static_cast<int>(r.report[1]) - inj.value;
            match = delta == 0 || (delta > 0 && delta < 64);
          }
          else {
            match = ((r.report[8] & 0x20) != 0) == (inj.value != 0);
          }
          if (match) {
            (inj.kind == 0 ? latency_stick : latency_button).push_back(r.at_us - inj.at_us);
            break;
          }
        }
        if (inj.kind == 1) cursor = std::min(cursor, reads.size());
      }
      // typical per-sample step of a 90 deg/s, 20 s period sine at gyro_hz samples/s, in counts
      std::printf("  pad %d: %zu reads, %.1f reads/s\n", pad->index, reads.size(), static_cast<double>(reads.size()) * 1000.0 / wall_ms);
      print_summary("interval, all reads", summarize(all), "us");
      if (options.tap_period_ms) {
        unsigned rises = 0, falls = 0;
        int prev = -1;
        for (const auto &r : reads) {
          const int down = (r.report[8] & 0x20) != 0 ? 1 : 0;
          if (prev == 0 && down == 1) ++rises;
          if (prev == 1 && down == 0) ++falls;
          prev = down;
        }
        std::printf("    taps (%u ms every %u ms, reader stall %u ms): injected %u, presses seen %u, releases seen %u, presses lost %d (%.1f%%)\n", options.tap_ms,
          options.tap_period_ms, options.stall_ms, g_taps_injected, rises, falls, static_cast<int>(g_taps_injected) - static_cast<int>(rises),
          g_taps_injected ? 100.0 * (static_cast<double>(g_taps_injected) - static_cast<double>(rises)) / static_cast<double>(g_taps_injected) : 0.0);
      }
      if (options.inject_ms != 0 || (!options.stick_hz && !options.button_hz && !options.gyro_hz)) {
        // reads after the last injection (plus 20 ms): does the 4 ms heartbeat keep running while nothing changes?
        const double idle_from = begin_us + static_cast<double>(options.inject_ms) * 1000.0 + 20000.0;
        std::vector<double> idle_intervals;
        std::size_t idle_reads = 0;
        double previous_at = -1, first_after = -1, last_at = 0;
        for (const auto &r : reads) {
          if (r.at_us < idle_from) continue;
          if (first_after < 0) first_after = r.at_us;
          ++idle_reads;
          if (previous_at >= 0) idle_intervals.push_back(r.at_us - previous_at);
          previous_at = r.at_us;
          last_at = r.at_us;
        }
        const double idle_span_ms = (begin_us + static_cast<double>(phase_ms) * 1000.0 - idle_from) / 1000.0;
        std::printf("    idle part (after injection stopped): %zu reads in %.0f ms = %.1f reads/s; first read %.1f ms after the idle part began, last %.1f ms before the end\n", idle_reads, idle_span_ms,
          idle_span_ms > 0 ? static_cast<double>(idle_reads) * 1000.0 / idle_span_ms : 0.0, first_after >= 0 ? (first_after - idle_from) / 1000.0 : -1.0, last_at > 0 ? (begin_us + static_cast<double>(phase_ms) * 1000.0 - last_at) / 1000.0 : -1.0);
        print_summary("idle part: interval between reads", summarize(idle_intervals), "us");
      }
      if (options.stick_hz) {
        print_summary("interval, reads with new stick", summarize(fresh_stick), "us");
        print_summary("latency, stick submit->read", summarize(latency_stick), "us");
      }
      if (options.button_hz) {
        print_summary("interval, reads with new button", summarize(fresh_button), "us");
        print_summary("latency, button submit->read", summarize(latency_button), "us");
      }
      if (options.gyro_hz) {
        print_summary("interval, reads with new gyro", summarize(fresh_gyro), "us");
        print_summary("sensor timestamp step", summarize(motion_ts_dt_us), "us");
        const auto step_summary = summarize(steps);
        // a sample was skipped or the value jumped when a step is far above what the sine allows: its steepest slope is
        // 90 * 2pi / 20 = 28.3 deg/s per second, i.e. under one count per sample; anything above 8 counts is a jump
        for (auto step : steps) if (step > 8.0) ++gyro_spikes;
        std::printf("    gyro: %zu value changes, step p50=%.1f p99=%.1f max=%.1f counts, jumps(>8 counts)=%zu, timestamp backwards=%zu, timestamp step >20ms=%zu\n",
          gyro_changes, step_summary.p50, step_summary.p99, step_summary.max, gyro_spikes, ts_backwards, ts_large);
      }
    }
    std::fflush(stdout);
  }
}  // namespace

int
main(int argc, char **argv) {
  const unsigned pad_count = argc > 1 ? std::max(1u, std::min(4u, static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)))) : 1u;
  const unsigned phase_ms = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 6000u;
  const std::string wanted = argc > 3 ? std::string(",") + argv[3] + "," : std::string(",sticks,buttons,gyro,mixed,stall40,stall80,stall160,idle,burstidle,sparseidle,tap0,tap80,tap120,tap160,gyro500,gyro1000,");
  timeBeginPeriod(1);
  auto mail = std::make_shared<safe::mail_raw_t>();
  /* ALK_PROBE_MOTION_CAP=<Hz>: key 101 as saved before the pads are allocated (default 250). ALK_PROBE_MOTION_LIVE=1: after the pads
   * are up, save 500, 1000, 100 and 250 in turn and print what the clients are asked for each time. */
  const auto temp = std::filesystem::temp_directory_path() / "ds5-probe-tuning.json";
  std::error_code ignored;
  std::filesystem::remove(temp, ignored);
  ds5::tuning::store_t tuning_store(temp);
  tuning_store.load();
  if (const auto *cap = std::getenv("ALK_PROBE_MOTION_CAP")) save_motion_cap(tuning_store, static_cast<unsigned>(std::strtoul(cap, nullptr, 10)));
  platf::ds5::native_client_t client;
  std::vector<platf::feedback_queue_t> queues;
  for (unsigned index = 0; index != pad_count; ++index) {
    auto queue = mail->queue<platf::gamepad_feedback_msg_t>("ds5-report-timing-" + std::to_string(index));
    queues.push_back(queue);
    if (client.alloc({ static_cast<int>(index), static_cast<std::uint8_t>(index) }, std::move(queue), true) != 0) {
      std::printf("FAIL alloc pad %u\n", index);
      return 1;
    }
  }
  print_motion_requests("at-allocation", queues);
  if (std::getenv("ALK_PROBE_MOTION_LIVE") != nullptr) {
    for (const unsigned hz : { 500u, 1000u, 100u, 250u }) {
      save_motion_cap(tuning_store, hz);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      std::printf("LIVE saved key 101 = %u Hz\n", hz);
      print_motion_requests("after-save", queues);
    }
  }
  std::vector<HANDLE> handles;
  for (int attempt = 0; attempt != 40; ++attempt) {
    for (auto handle : handles) CloseHandle(handle);
    handles = open_virtual_hids();
    if (handles.size() >= pad_count) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  std::printf("HID devices found: %zu (wanted %u)\n", handles.size(), pad_count);
  if (handles.size() < pad_count) return 2;
  std::vector<std::unique_ptr<pad_t>> pads;
  for (auto handle : handles) {
    auto pad = std::make_unique<pad_t>();
    pad->file = handle;
    std::array<std::uint8_t, 64> feature {};
    feature[0] = 0x09;
    HidD_GetFeature(handle, feature.data(), static_cast<ULONG>(feature.size()));
    pad->index = static_cast<int>(feature[6]) - 1;
    ULONG buffers = 0;
    HidD_GetNumInputBuffers(handle, &buffers);
    std::printf("HID pad %d: input buffers (HID class queue length) = %lu\n", pad->index, static_cast<unsigned long>(buffers));
    if (pad->index < 0 || pad->index >= static_cast<int>(pad_count)) pad->index = static_cast<int>(pads.size());
    pads.push_back(std::move(pad));
  }
  for (auto &pad : pads) {
    auto *raw = pad.get();
    raw->reader = std::thread([raw] {
      HANDLE self = nullptr;
      DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
      raw->reader_handle.store(self);
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
      name_this_thread(L"probe-reader");
      std::array<std::uint8_t, 64> buffer {};
      auto next_stall = clock_type::now() + std::chrono::milliseconds(200);
      while (!raw->stop.load(std::memory_order_acquire)) {
        DWORD bytes = 0;
        if (!ReadFile(raw->file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) break;
        {
          std::lock_guard lock(raw->reads_mutex);
          if (raw->recording) raw->reads.push_back({ qpc_us(), buffer });
        }
        const auto stall = raw->stall_ms.load(std::memory_order_relaxed);
        if (stall != 0 && clock_type::now() >= next_stall) {
          std::this_thread::sleep_for(std::chrono::milliseconds(stall));
          next_stall = clock_type::now() + std::chrono::milliseconds(200);
        }
      }
    });
  }
  while (std::any_of(pads.begin(), pads.end(), [](auto &p) { return p->reader_handle.load() == nullptr; })) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const std::vector<phase_options_t> all_phases = {
    { "sticks", 500, 0, 0, 0, 0 },
    { "buttons", 0, 125, 0, 0, 0 },
    { "gyro", 0, 0, 250, 0, 0 },
    { "gyro500", 0, 0, 500, 0, 0 },
    { "gyro1000", 0, 0, 1000, 0, 0 },
    { "mixed", 500, 125, 250, 0, 0 },
    { "stall40", 500, 0, 0, 40, 0 },
    { "stall80", 500, 0, 0, 80, 0 },
    { "stall160", 500, 0, 0, 160, 0 },
    { "idle", 0, 0, 0, 0, 0 },
    { "burstidle", 500, 0, 0, 0, 1000 },
    { "sparseidle", 0, 125, 0, 0, 1000 },
    { "tap0", 500, 0, 0, 0, 0, 30, 70 },
    { "tap80", 500, 0, 0, 80, 0, 30, 70 },
    { "tap120", 500, 0, 0, 120, 0, 30, 70 },
    { "tap160", 500, 0, 0, 160, 0, 30, 70 },
  };
  for (const auto &phase : all_phases) {
    if (wanted.find(std::string(",") + phase.name + ",") == std::string::npos) continue;
    run_phase(client, pads, phase, phase_ms);
  }
  for (auto &pad : pads) pad->stop.store(true);
  for (auto &pad : pads) CancelIoEx(pad->file, nullptr);
  for (auto &pad : pads) pad->reader.join();
  for (auto &pad : pads) client.free(pad->index);
  for (auto &pad : pads) CloseHandle(pad->file);
  std::puts("DONE");
  return 0;
}

#else
int
main() {
  return 0;
}
#endif
