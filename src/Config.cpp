#include "betterlaunch/Config.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include "pl/Logger.hpp"

namespace betterlaunch {
namespace {

using Json = nlohmann::json;

Json defaultJson() {
  return Json{
      {"enabled", true},
      {"video", "launch_animation.mp4"},
      {"scale_mode", "fit"},
      {"volume", 1.0},
      {"allow_skip", false},
      {"fade_out_ms", 400},
      {"max_seconds", 30},
  };
}

void writeDefaults(const std::filesystem::path &path, pl::log::Logger &logger) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    logger.warn("Could not create config directory {}: {}",
                path.parent_path().string(), error.message());
    return;
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    logger.warn("Could not create config file {}", path.string());
    return;
  }
  output << defaultJson().dump(2) << '\n';
  if (!output) {
    logger.warn("Could not finish writing config file {}", path.string());
  }
}

const Json *findTypedValue(const Json &object, std::string_view key,
                           bool (*predicate)(const Json &)) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto found = object.find(std::string(key));
  if (found == object.end() || !predicate(*found)) {
    return nullptr;
  }
  return &*found;
}

bool isBoolean(const Json &value) { return value.is_boolean(); }
bool isString(const Json &value) { return value.is_string(); }
bool isNumber(const Json &value) { return value.is_number(); }
bool isInteger(const Json &value) {
  return value.is_number_integer() || value.is_number_unsigned();
}

std::uint32_t boundedInteger(const Json &value, std::uint32_t minimum,
                             std::uint32_t maximum) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        number, minimum, maximum));
  }
  const auto number = value.get<std::int64_t>();
  return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
      number, minimum, maximum));
}

} // namespace

Config loadConfig(const std::filesystem::path &path, pl::log::Logger &logger) {
  Config config;
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) {
    logger.warn("Could not inspect config file {}: {}", path.string(),
                error.message());
    return config;
  }
  if (!exists) {
    writeDefaults(path, logger);
    return config;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    logger.warn("Could not read config file {}; using defaults", path.string());
    return config;
  }

  Json values;
  try {
    input >> values;
  } catch (const std::exception &exception) {
    logger.warn("Invalid config file {}: {}; using defaults", path.string(),
                exception.what());
    return config;
  }
  if (!values.is_object()) {
    logger.warn("Config file {} must contain a JSON object; using defaults",
                path.string());
    return config;
  }

  if (const auto *value = findTypedValue(values, "enabled", isBoolean)) {
    config.enabled = value->get<bool>();
  }
  if (const auto *value = findTypedValue(values, "video", isString)) {
    const auto video = value->get<std::string>();
    if (!video.empty()) {
      config.video = video;
    } else {
      logger.warn("Empty video path in {}; using launch_animation.mp4",
                  path.string());
    }
  }
  if (const auto *value =
          findTypedValue(values, "scale_mode", isString)) {
    const auto mode = value->get<std::string>();
    if (mode == "fit") {
      config.scaleMode = ScaleMode::Fit;
    } else if (mode == "fill") {
      config.scaleMode = ScaleMode::Fill;
    } else if (mode == "stretch") {
      config.scaleMode = ScaleMode::Stretch;
    } else {
      logger.warn("Unknown scale_mode '{}'; using fit", mode);
    }
  }
  if (const auto *value = findTypedValue(values, "volume", isNumber)) {
    const double volume = value->get<double>();
    if (std::isfinite(volume)) {
      config.volume = static_cast<float>(std::clamp(volume, 0.0, 1.0));
    }
  }
  if (const auto *value =
          findTypedValue(values, "allow_skip", isBoolean)) {
    config.allowSkip = value->get<bool>();
  }
  if (const auto *value =
          findTypedValue(values, "fade_out_ms", isInteger)) {
    config.fadeOutMs = boundedInteger(*value, 0, 5000);
  }
  if (const auto *value =
          findTypedValue(values, "max_seconds", isInteger)) {
    config.maxSeconds = boundedInteger(*value, 1, 600);
  }

  return config;
}

} // namespace betterlaunch
