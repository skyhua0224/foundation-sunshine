/**
 * @file src/precompiled.h
 * @brief Headers precompiled for the large targets (cmake/targets/precompiled_headers.cmake). Never included by a source file.
 */
#pragma once

// windows.h is left out on purpose: sources include it with their own macro settings, and a copy included here first
// hides NTSTATUS from display.h in several of them (measured: it saves another 4%, not worth the exceptions).

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

// Targets that do not see the Boost.Log include directories skip these.
#if __has_include(<boost/log/sources/severity_logger.hpp>)
  #include <boost/log/sources/severity_logger.hpp>
#endif
#if __has_include(<boost/property_tree/ptree.hpp>)
  #include <boost/property_tree/ptree.hpp>
#endif
