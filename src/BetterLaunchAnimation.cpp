#include "betterlaunch/Config.hpp"
#include "betterlaunch/Version.hpp"
#include "betterlaunch/VideoOverlay.hpp"

#include <EGL/egl.h>
#include <dlfcn.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "pl/Input.hpp"
#include "pl/Logger.hpp"
#include "pl/Mod.hpp"
#include "pl/memory/Hook.hpp"

namespace {

using EglSwapBuffersFunction = EGLBoolean(EGLAPIENTRYP)(EGLDisplay, EGLSurface);
using DlopenFunction = void *(*)(const char *, int);

pl::memory::FuncPtr g_originalSwapBuffers{};
pl::memory::FuncPtr g_originalDlopen{};
std::atomic<class BetterLaunchAnimationMod *> g_mod{};
std::mutex g_overlayMutex;
std::shared_ptr<betterlaunch::VideoOverlay> g_activeOverlay;

void *dlopenDetour(const char *filename, int flags);

std::shared_ptr<betterlaunch::VideoOverlay> activeOverlay() {
  std::lock_guard<std::mutex> lock(g_overlayMutex);
  return g_activeOverlay;
}

void setActiveOverlay(std::shared_ptr<betterlaunch::VideoOverlay> overlay) {
  std::lock_guard<std::mutex> lock(g_overlayMutex);
  g_activeOverlay = std::move(overlay);
}

void clearActiveOverlay(
    const std::shared_ptr<betterlaunch::VideoOverlay> &overlay) {
  std::lock_guard<std::mutex> lock(g_overlayMutex);
  if (g_activeOverlay == overlay) {
    g_activeOverlay.reset();
  }
}

bool isLeviLauncherProcess() {
  std::ifstream commandLine("/proc/self/cmdline", std::ios::binary);
  if (!commandLine) {
    return false;
  }

  char buffer[256]{};
  commandLine.read(buffer, static_cast<std::streamsize>(sizeof(buffer) - 1));
  const std::string processName(buffer);
  return processName == "org.levimc.launcher" ||
         processName == "org.levimc.launcher:minecraft" ||
         processName == "com.mojang.minecraftpe";
}

void *findDlopenSymbol(pl::log::Logger &logger) {
  // libdl is process-wide; keep the handle alive while the hook is installed.
  static void *dlLibrary = [] {
    void *handle = dlopen("libdl.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) {
      handle = dlopen("libdl.so", RTLD_NOW | RTLD_LOCAL);
    }
    return handle;
  }();
  if (!dlLibrary) {
    const char *error = dlerror();
    logger.error("Could not open libdl.so: {}", error ? error : "unknown error");
    return nullptr;
  }

  dlerror();
  void *symbol = dlsym(dlLibrary, "dlopen");
  if (!symbol) {
    const char *error = dlerror();
    logger.error("Could not resolve dlopen: {}",
                 error ? error : "symbol not found");
  }
  return symbol;
}

void *findEglSwapBuffers(pl::log::Logger &logger) {
  // Keep the handle alive: the hooked entry point is owned by libEGL.so.
  static void *eglLibrary = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
  if (!eglLibrary) {
    const char *error = dlerror();
    logger.error("Could not load libEGL.so: {}", error ? error : "unknown error");
    return nullptr;
  }

  dlerror();
  void *symbol = dlsym(eglLibrary, "eglSwapBuffers");
  if (!symbol) {
    const char *error = dlerror();
    logger.error("Could not resolve eglSwapBuffers: {}",
                 error ? error : "symbol not found");
  }
  return symbol;
}

EGLBoolean EGLAPIENTRY swapBuffersDetour(EGLDisplay display,
                                         EGLSurface surface) {
  const auto original =
      reinterpret_cast<EglSwapBuffersFunction>(g_originalSwapBuffers);
  if (!original) {
    return EGL_FALSE;
  }

  if (auto overlay = activeOverlay()) {
    overlay->render();
  }
  return original(display, surface);
}

void registerInputCallbacks() {
  // Preloader has no callback-unregister API. Register once and resolve the
  // current overlay dynamically so disable/re-enable does not leave stale input
  // handlers or duplicate callbacks behind.
  static std::once_flag registered;
  std::call_once(registered, [] {
    pl::input::registerTouchCallback(
        [](const pl::input::TouchEvent &event) {
          const auto current = activeOverlay();
          if (!current || !current->isVisible()) {
            return false;
          }
          if (current->allowSkip() &&
              (event.action == 0 || event.action == 5)) {
            current->requestSkip();
          }
          return true;
        });

    pl::input::registerKeyCallback([](const pl::input::KeyEvent &event) {
      const auto current = activeOverlay();
      if (!current || !current->isVisible()) {
        return false;
      }
      // Android KEYCODE_BACK is 4; KEYCODE_ESCAPE (hardware keyboards) is 111.
      if (current->allowSkip() && event.isKeyDown &&
          (event.keyCode == 4 || event.keyCode == 111)) {
        current->requestSkip();
      }
      return true;
    });

    pl::input::registerMouseCallback([](const pl::input::MouseEvent &event) {
      const auto current = activeOverlay();
      if (!current || !current->isVisible()) {
        return false;
      }
      if (current->allowSkip() && event.isDown) {
        current->requestSkip();
      }
      return true;
    });

    pl::input::registerTextInputCallback(
        [](const pl::input::TextInputEvent &) {
          const auto current = activeOverlay();
          return current && current->isVisible();
        });
  });
}

class BetterLaunchAnimationMod final {
public:
  bool load(pl::mod::ModContext &context) {
    m_logger = &context.logger();
    m_javaVm = context.javaVm();
    m_resourceDirectory = context.resourceDir();
    m_config = betterlaunch::loadConfig(context.configDir() / "config.json",
                                        *m_logger);
    m_loaded = true;
    g_mod.store(this, std::memory_order_release);
    m_logger->info("{} {} loaded", BETTERLAUNCH_NAME, BETTERLAUNCH_VERSION);

    if (!isLeviLauncherProcess()) {
      m_logger->info("Current process is not a LeviLauncher game process; "
                     "game hooks will not be installed");
      return true;
    }
    m_supportedProcess = true;
    if (!m_config.enabled) {
      m_logger->info("Animation is disabled in config/config.json");
      return true;
    }

    if (isMinecraftLibraryLoaded()) {
      m_minecraftLoaded.store(true, std::memory_order_release);
      m_logger->info("libminecraftpe.so is already loaded");
    } else if (!installDlopenHook()) {
      m_logger->warn("Could not watch for libminecraftpe.so; the game-frame "
                     "hook will be retried when the mod is enabled");
    }
    return true;
  }

  bool enable(pl::mod::ModContext &context) {
    if (!m_loaded || !m_logger) {
      return false;
    }
    if (!m_config.enabled) {
      m_logger->info("Animation is disabled in config/config.json");
      return true;
    }
    if (!m_supportedProcess) {
      return true;
    }
    if (m_enabled.load(std::memory_order_acquire)) {
      return true;
    }

    std::filesystem::path videoPath(m_config.video);
    if (!videoPath.is_absolute()) {
      videoPath = m_resourceDirectory / videoPath;
    }

    m_logger->info("Preparing launch animation from {}", videoPath.string());
    auto overlay = std::make_shared<betterlaunch::VideoOverlay>(
        m_javaVm, m_config, std::move(videoPath), *m_logger);
    m_overlay = overlay;
    setActiveOverlay(overlay);
    try {
      registerInputCallbacks();
    } catch (const std::exception &exception) {
      clearActiveOverlay(overlay);
      m_overlay.reset();
      overlay->stop();
      m_logger->error("Could not register input callbacks: {}", exception.what());
      return false;
    } catch (...) {
      clearActiveOverlay(overlay);
      m_overlay.reset();
      overlay->stop();
      m_logger->error("Could not register input callbacks");
      return false;
    }

    m_enabled.store(true, std::memory_order_release);
    if (!m_minecraftLoaded.load(std::memory_order_acquire) &&
        isMinecraftLibraryLoaded()) {
      m_minecraftLoaded.store(true, std::memory_order_release);
    }

    if (m_minecraftLoaded.load(std::memory_order_acquire)) {
      if (!installEglHook()) {
        disable(context);
        return false;
      }
    } else {
      if (!installDlopenHook()) {
        m_logger->error("Could not monitor libminecraftpe.so; no launch "
                        "animation frame hook will be installed");
        disable(context);
        return false;
      }
      m_logger->info("Waiting for libminecraftpe.so before hooking game frames");
    }
    return true;
  }

  bool disable(pl::mod::ModContext &) {
    m_enabled.store(false, std::memory_order_release);
    uninstallEglHook();

    const auto overlay = std::move(m_overlay);
    m_overlay.reset();
    if (overlay) {
      clearActiveOverlay(overlay);
      overlay->stop();
    }
    return true;
  }

  bool unload(pl::mod::ModContext &context) {
    disable(context);
    uninstallDlopenHook();
    auto *expected = this;
    g_mod.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    m_loaded = false;
    return true;
  }

  void minecraftLibraryLoaded() {
    const bool wasLoaded =
        m_minecraftLoaded.exchange(true, std::memory_order_acq_rel);
    if (wasLoaded) {
      return;
    }
    if (m_logger) {
      m_logger->info("Detected libminecraftpe.so; game-frame hook can start");
    }
    if (m_enabled.load(std::memory_order_acquire) && !installEglHook() &&
        m_logger) {
      m_logger->error("Could not install the launch animation frame hook");
    }
  }

private:
  bool isMinecraftLibraryLoaded() const {
    // RTLD_NOLOAD can be namespace-sensitive on Android. Check the process map
    // first, then use the loader's handle as a second signal.
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
      if (line.find("libminecraftpe.so") != std::string::npos) {
        return true;
      }
    }

    auto open = reinterpret_cast<DlopenFunction>(g_originalDlopen);
    if (!open) {
      open = &dlopen;
    }
    void *handle = open("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) {
      return false;
    }
    dlclose(handle);
    return true;
  }

  bool installDlopenHook() {
    std::lock_guard<std::mutex> lock(m_hookMutex);
    if (m_dlopenHookInstalled) {
      return true;
    }
    if (!m_logger) {
      return false;
    }

    void *target = findDlopenSymbol(*m_logger);
    if (!target) {
      return false;
    }
    const int hookStatus = pl::memory::hook(
        target, reinterpret_cast<pl::memory::FuncPtr>(&dlopenDetour),
        &g_originalDlopen, pl::memory::HookPriority::Normal);
    if (hookStatus != 0) {
      g_originalDlopen = nullptr;
      m_logger->error("Could not hook dlopen (status {})", hookStatus);
      return false;
    }

    m_dlopenHookTarget = target;
    m_dlopenHookInstalled = true;
    m_logger->info("Watching for libminecraftpe.so to load");
    return true;
  }

  bool installEglHook() {
    std::lock_guard<std::mutex> lock(m_hookMutex);
    if (m_eglHookInstalled) {
      return true;
    }
    if (!m_enabled.load(std::memory_order_acquire) || !activeOverlay()) {
      return false;
    }

    void *target = findEglSwapBuffers(*m_logger);
    if (!target) {
      return false;
    }
    const int hookStatus = pl::memory::hook(
        target, reinterpret_cast<pl::memory::FuncPtr>(&swapBuffersDetour),
        &g_originalSwapBuffers, pl::memory::HookPriority::Normal);
    if (hookStatus != 0) {
      g_originalSwapBuffers = nullptr;
      m_logger->error("Could not hook eglSwapBuffers (status {})", hookStatus);
      return false;
    }

    m_eglHookTarget = target;
    m_eglHookInstalled = true;
    m_logger->info("Launch animation hook installed after Minecraft loaded");
    return true;
  }

  void uninstallEglHook() {
    std::lock_guard<std::mutex> lock(m_hookMutex);
    if (!m_eglHookInstalled) {
      return;
    }
    if (!pl::memory::unhook(
            m_eglHookTarget,
            reinterpret_cast<pl::memory::FuncPtr>(&swapBuffersDetour))) {
      if (m_logger) {
        m_logger->warn("Preloader did not remove the eglSwapBuffers hook");
      }
    }
    m_eglHookInstalled = false;
    m_eglHookTarget = nullptr;
    g_originalSwapBuffers = nullptr;
  }

  void uninstallDlopenHook() {
    std::lock_guard<std::mutex> lock(m_hookMutex);
    if (!m_dlopenHookInstalled) {
      return;
    }
    if (!pl::memory::unhook(
            m_dlopenHookTarget,
            reinterpret_cast<pl::memory::FuncPtr>(&dlopenDetour))) {
      if (m_logger) {
        m_logger->warn("Preloader did not remove the dlopen watcher");
      }
    }
    m_dlopenHookInstalled = false;
    m_dlopenHookTarget = nullptr;
  }

  pl::log::Logger *m_logger{};
  JavaVM *m_javaVm{};
  std::filesystem::path m_resourceDirectory;
  betterlaunch::Config m_config;
  std::shared_ptr<betterlaunch::VideoOverlay> m_overlay;
  std::mutex m_hookMutex;
  pl::memory::FuncPtr m_eglHookTarget{};
  pl::memory::FuncPtr m_dlopenHookTarget{};
  std::atomic<bool> m_minecraftLoaded{false};
  std::atomic<bool> m_enabled{false};
  bool m_loaded{};
  bool m_supportedProcess{};
  bool m_eglHookInstalled{};
  bool m_dlopenHookInstalled{};
};

void *dlopenDetour(const char *filename, int flags) {
  const auto original = reinterpret_cast<DlopenFunction>(g_originalDlopen);
  if (!original) {
    return nullptr;
  }

  void *handle = original(filename, flags);
  if (handle && filename &&
      std::string_view(filename).find("libminecraftpe.so") !=
          std::string_view::npos) {
    if (auto *mod = g_mod.load(std::memory_order_acquire)) {
      mod->minecraftLibraryLoaded();
    }
  }
  return handle;
}

BetterLaunchAnimationMod g_betterLaunchAnimationMod{};

} // namespace

PL_REGISTER_MOD(BetterLaunchAnimationMod, g_betterLaunchAnimationMod);
