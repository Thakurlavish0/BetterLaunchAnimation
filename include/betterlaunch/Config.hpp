#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace pl::log {
class Logger;
}

namespace betterlaunch {

enum class ScaleMode {
  Fit,
  Fill,
  Stretch,
};

struct Config {
  bool enabled{true};
  std::string video{"launch_animation.mp4"};
  ScaleMode scaleMode{ScaleMode::Fit};
  float volume{1.0F};
  bool allowSkip{false};
  std::uint32_t fadeOutMs{400};
  std::uint32_t maxSeconds{30};
};

/** Load the mod configuration, writing the defaults if the file is missing. */
Config loadConfig(const std::filesystem::path &path, pl::log::Logger &logger);

} // namespace betterlaunch
