/**
 * @file tests/tools/ds5_mic_probe.cpp
 * @brief The client microphone on the DualSense microphone endpoint, measured end to end (measurement tool, not a unit test).
 *
 * Allocates one native virtual DualSense, hands blocks of the client microphone to the endpoint the way the microphone
 * redirect does (ds5::mic_route::feed(): mono, 48 kHz, 20 ms blocks, paced by the steady clock), and records the
 * Windows capture endpoint "Headset Microphone (Wireless Controller)" with the device's own timestamps.
 *
 *   tone      a 440 Hz tone for 10 s: how much of it arrives (500 ms windows), gaps of silence inside it, WASAPI discontinuities
 *   latency   silence with a 1 kHz burst every 500 ms: time from the hand-over of the block to the burst in the recording
 *   route     key 96 saved as "none": the endpoint is silent; saved as "controller" again: the signal is back
 *
 * usage: ds5_mic_probe [tone_s=10] [latency_s=12]
 */
#ifdef _WIN32

  #include <windows.h>

  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cmath>
  #include <cstdint>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <memory>
  #include <numbers>
  #include <string>
  #include <thread>
  #include <vector>

  #include <boost/log/sources/severity_logger.hpp>

  #include "src/ds5/mic_route.h"
  #include "src/ds5/tuning_profile.h"
  #include "src/platform/windows/capture_endpoint_recorder.h"
  #include "src/platform/windows/ds5/ds5_native_client.h"
  #include "src/thread_safe.h"

boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> debug;

namespace {
  using clock_type = std::chrono::steady_clock;

  std::uint64_t
  qpc_100ns() {
    static const double frequency = [] {
      LARGE_INTEGER value;
      QueryPerformanceFrequency(&value);
      return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(static_cast<double>(now.QuadPart) * 1e7 / frequency);
  }

  enum class mode_e : int { silence, tone, markers };

  struct marker_t {
    std::uint64_t handed_over_100ns;
  };

  double
  tone_amplitude(const std::vector<float> &x, double frequency, double rate) {
    const double omega = 2.0 * std::numbers::pi * frequency / rate;
    const double coeff = 2.0 * std::cos(omega);
    double s1 = 0, s2 = 0;
    for (const float v : x) {
      const double s = v + coeff * s1 - s2;
      s2 = s1;
      s1 = s;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / static_cast<double>(x.size());
  }

  double
  percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
  }

  struct recording_t {
    std::vector<float> samples;  // first channel, in order
    std::vector<std::uint64_t> block_qpc;  // per block: time of the first frame
    std::vector<std::uint64_t> block_read_qpc;
    std::vector<std::size_t> block_start;  // index in samples
    std::size_t discontinuities { 0 };
    std::size_t silent_flags { 0 };
    std::uint32_t rate { 0 };
  };

  recording_t
  record(platf::audio::capture_endpoint_recorder &recorder, std::chrono::milliseconds duration) {
    std::vector<platf::audio::capture_endpoint_recorder::block> blocks;
    recorder.record(duration, blocks, std::chrono::milliseconds(3000));
    recording_t result;
    result.rate = recorder.sample_rate();
    for (const auto &b : blocks) {
      result.block_qpc.push_back(b.qpc_100ns);
      result.block_read_qpc.push_back(b.read_qpc_100ns);
      result.block_start.push_back(result.samples.size());
      result.samples.insert(result.samples.end(), b.samples.begin(), b.samples.end());
      if (b.discontinuity) ++result.discontinuities;
      if (b.silent) ++result.silent_flags;
    }
    return result;
  }
}  // namespace

int
main(int argc, char **argv) {
  const unsigned tone_s = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 10u;
  const unsigned latency_s = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 12u;
  auto mail = std::make_shared<safe::mail_raw_t>();
  const auto temp = std::filesystem::temp_directory_path() / "ds5-mic-probe-tuning.json";
  std::error_code ignored;
  std::filesystem::remove(temp, ignored);
  ds5::tuning::store_t store(temp);
  store.load();
  platf::ds5::native_client_t client;
  auto queue = mail->queue<platf::gamepad_feedback_msg_t>("ds5-mic-probe");
  if (client.alloc({ 0, 0 }, std::move(queue), true) != 0) {
    std::puts("FAIL alloc");
    return 1;
  }

  std::atomic<int> mode { static_cast<int>(mode_e::silence) };
  std::atomic_bool stop { false };
  std::vector<marker_t> markers;
  std::mutex markers_mutex;
  std::thread writer([&] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    constexpr std::size_t block_frames = 960;
    std::vector<std::int16_t> block(block_frames);
    std::uint64_t index = 0;
    std::uint64_t blocks_written = 0;
    auto next = clock_type::now();
    while (!stop.load()) {
      const auto current = static_cast<mode_e>(mode.load());
      bool marker_block = false;
      for (std::size_t i = 0; i < block_frames; ++i, ++index) {
        double value = 0.0;
        if (current == mode_e::tone) value = 0.18 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(index) / 48000.0);
        if (current == mode_e::markers && (blocks_written % 25) == 0 && i < 480) {  // every 500 ms, a 10 ms burst at the start of a block
          value = 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / 48000.0);
          marker_block = true;
        }
        block[i] = static_cast<std::int16_t>(std::lround(value * 32767.0));
      }
      if (marker_block) {
        std::lock_guard lock(markers_mutex);
        markers.push_back({ qpc_100ns() });
      }
      ds5::mic_route::feed(block.data(), block.size());
      ++blocks_written;
      next += std::chrono::milliseconds(20);
      std::this_thread::sleep_until(next);
    }
  });

  platf::audio::capture_endpoint_recorder recorder;
  if (!recorder.open(L"Wireless Controller", std::chrono::seconds(30))) {
    std::puts("FAIL no capture endpoint of the pad");
    stop = true;
    writer.join();
    return 2;
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // 1. tone
  mode = static_cast<int>(mode_e::tone);
  auto tone = record(recorder, std::chrono::seconds(tone_s));
  {
    const auto rate = tone.rate;
    std::size_t windows = 0, good = 0, gaps = 0;
    double longest_gap_ms = 0;
    const std::size_t window = rate / 2;
    for (std::size_t start = rate; start + window <= tone.samples.size(); start += window) {
      const std::vector<float> piece(tone.samples.begin() + static_cast<std::ptrdiff_t>(start), tone.samples.begin() + static_cast<std::ptrdiff_t>(start + window));
      ++windows;
      if (tone_amplitude(piece, 440.0, rate) > 0.18 * 0.8) ++good;
    }
    std::size_t run = 0;
    for (std::size_t i = static_cast<std::size_t>(rate); i < tone.samples.size(); ++i) {
      if (std::abs(tone.samples[i]) < 1e-4f) ++run;
      else {
        if (run >= 48) {
          ++gaps;
          longest_gap_ms = std::max(longest_gap_ms, 1000.0 * static_cast<double>(run) / rate);
        }
        run = 0;
      }
    }
    std::printf("MIC_TONE seconds=%.2f windows=%zu good=%zu gaps_over_1ms=%zu longest_gap_ms=%.2f wasapi_discontinuities=%zu silent_flags=%zu rate=%u\n",
      static_cast<double>(tone.samples.size()) / rate, windows, good, gaps, longest_gap_ms, tone.discontinuities, tone.silent_flags, rate);
  }

  // 2. latency
  {
    { std::lock_guard lock(markers_mutex); markers.clear(); }
    mode = static_cast<int>(mode_e::markers);
    auto rec = record(recorder, std::chrono::seconds(latency_s));
    std::vector<marker_t> handed;
    { std::lock_guard lock(markers_mutex); handed = markers; }
    std::vector<double> device_ms, read_ms;
    // onsets: the first sample above 0.1 after at least 100 ms below it
    std::vector<std::size_t> onsets;
    std::size_t quiet = 0;
    for (std::size_t i = 0; i < rec.samples.size(); ++i) {
      if (std::abs(rec.samples[i]) > 0.1f) {
        if (quiet >= rec.rate / 10) onsets.push_back(i);
        quiet = 0;
      }
      else ++quiet;
    }
    const auto block_of = [&](std::size_t sample) {
      std::size_t found = 0;
      for (std::size_t b = 0; b < rec.block_start.size(); ++b) if (rec.block_start[b] <= sample) found = b;
      return found;
    };
    // match each onset to the latest hand-over that is not after it
    for (const auto onset : onsets) {
      const auto block = block_of(onset);
      const auto at = rec.block_qpc[block] + static_cast<std::uint64_t>(static_cast<double>(onset - rec.block_start[block]) * 1e7 / rec.rate);
      const auto read = rec.block_read_qpc[block];
      const marker_t *best = nullptr;
      for (const auto &m : handed) if (m.handed_over_100ns <= at + 200000 && (best == nullptr || m.handed_over_100ns > best->handed_over_100ns)) best = &m;
      if (best == nullptr) continue;
      device_ms.push_back(static_cast<double>(static_cast<std::int64_t>(at) - static_cast<std::int64_t>(best->handed_over_100ns)) / 1e4);
      read_ms.push_back(static_cast<double>(static_cast<std::int64_t>(read) - static_cast<std::int64_t>(best->handed_over_100ns)) / 1e4);
    }
    std::printf("MIC_LATENCY markers_handed=%zu heard=%zu device_ms(min/p50/p95/max)=%.1f/%.1f/%.1f/%.1f app_read_ms(min/p50/p95/max)=%.1f/%.1f/%.1f/%.1f discontinuities=%zu\n",
      handed.size(), device_ms.size(), percentile(device_ms, 0), percentile(device_ms, 0.5), percentile(device_ms, 0.95), percentile(device_ms, 1),
      percentile(read_ms, 0), percentile(read_ms, 0.5), percentile(read_ms, 0.95), percentile(read_ms, 1), rec.discontinuities);
  }

  // 3. route none, then controller again
  {
    mode = static_cast<int>(mode_e::tone);
    store.apply_json({ { "schema", "ALK-DS5T/1" }, { "values", { { "ds5.audio.micRoute", "none" } } } });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto silent = record(recorder, std::chrono::seconds(2));
    float peak = 0;
    for (const auto v : silent.samples) peak = std::max(peak, std::abs(v));
    store.apply_json({ { "schema", "ALK-DS5T/1" }, { "values", { { "ds5.audio.micRoute", "controller" } } } });
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto back = record(recorder, std::chrono::seconds(3));
    std::size_t windows = 0, good = 0;
    const std::size_t window = back.rate / 2;
    for (std::size_t start = back.rate; start + window <= back.samples.size(); start += window) {
      const std::vector<float> piece(back.samples.begin() + static_cast<std::ptrdiff_t>(start), back.samples.begin() + static_cast<std::ptrdiff_t>(start + window));
      ++windows;
      if (tone_amplitude(piece, 440.0, back.rate) > 0.18 * 0.8) ++good;
    }
    std::printf("MIC_ROUTE none_peak=%.5f controller_again_windows=%zu good=%zu\n", peak, windows, good);
  }

  stop = true;
  writer.join();
  recorder.close();
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
