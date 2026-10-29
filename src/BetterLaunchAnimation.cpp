#include "betterlaunch/Config.hpp"
#include "betterlaunch/Version.hpp"
#include "betterlaunch/VideoOverlay.hpp"

#include <EGL/egl.h>
#include <dlfcn.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "pl/Input.hpp"
#include "pl/Logger.hpp"
#include "pl/Mod.hpp"
#include "pl/memory/Hook.hpp"

namespace {

using EglSwapBuffersFunction = EGLBoolean(EGLAPIENTRYP)(EGLDisplay, EGLSurface);

pl::memory::FuncPtr g_originalSwapBuffers{};
std::mutex g_overlayMutex;
std::shared_ptr<betterlaunch::VideoOverlay> g_activeOverlay;

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

void registerInputCallbacks(
    const std::shared_ptr<betterlaunch::VideoOverlay> &overlay) {
  const std::weak_ptr<betterlaunch::VideoOverlay> weakOverlay = overlay;

  pl::input::registerTouchCallback(
      [weakOverlay](const pl::input::TouchEvent &event) {
        const auto current = weakOverlay.lock();
        if (!current || !current->isVisible()) {
          return false;
        }
        if (current->allowSkip() && (event.action == 0 || event.action == 5)) {
          current->requestSkip();
        }
        return true;
      });

  pl::input::registerKeyCallback(
      [weakOverlay](const pl::input::KeyEvent &event) {
        const auto current = weakOverlay.lock();
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

  pl::input::registerMouseCallback(
      [weakOverlay](const pl::input::MouseEvent &event) {
        const auto current = weakOverlay.lock();
        if (!current || !current->isVisible()) {
          return false;
        }
        if (current->allowSkip() && event.isDown) {
          current->requestSkip();
        }
        return true;
      });

  pl::input::registerTextInputCallback(
      [weakOverlay](const pl::input::TextInputEvent &) {
        const auto current = weakOverlay.lock();
        return current && current->isVisible();
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
    m_logger->info("{} {} loaded", BETTERLAUNCH_NAME, BETTERLAUNCH_VERSION);
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
    if (m_overlay) {
      return true;
    }

    std::filesystem::path videoPath(m_config.video);
    if (!videoPath.is_absolute()) {
      videoPath = m_resourceDirectory / videoPath;
    }

    m_logger->info("Preparing launch animation from {}", videoPath.string());
    auto overlay = std::make_shared<betterlaunch::VideoOverlay>(
        m_javaVm, m_config, std::move(videoPath), *m_logger);
    void *target = findEglSwapBuffers(*m_logger);
    if (!target) {
      return false;
    }

    setActiveOverlay(overlay);
    const int hookStatus = pl::memory::hook(
        target, reinterpret_cast<pl::memory::FuncPtr>(&swapBuffersDetour),
        &g_originalSwapBuffers, pl::memory::HookPriority::Normal);
    if (hookStatus != 0) {
      clearActiveOverlay(overlay);
      m_logger->error("Could not hook eglSwapBuffers (status {})", hookStatus);
      return false;
    }

    m_hookTarget = target;
    m_hookInstalled = true;
    m_overlay = std::move(overlay);
    try {
      registerInputCallbacks(m_overlay);
    } catch (...) {
      disable(context);
      throw;
    }
    m_logger->info("Launch animation hook installed");
    return true;
  }

  bool disable(pl::mod::ModContext &) {
    if (!m_overlay) {
      return true;
    }

    const auto overlay = m_overlay;
    // Prevent new calls into the overlay before stopping its Java worker.
    if (m_hookInstalled) {
      if (!pl::memory::unhook(
              m_hookTarget,
              reinterpret_cast<pl::memory::FuncPtr>(&swapBuffersDetour))) {
        m_logger->warn("Preloader did not remove the eglSwapBuffers hook");
      }
      m_hookInstalled = false;
      m_hookTarget = nullptr;
    }
    clearActiveOverlay(overlay);
    overlay->stop();
    m_overlay.reset();
    return true;
  }

  bool unload(pl::mod::ModContext &context) { return disable(context); }

private:
  pl::log::Logger *m_logger{};
  JavaVM *m_javaVm{};
  std::filesystem::path m_resourceDirectory;
  betterlaunch::Config m_config;
  std::shared_ptr<betterlaunch::VideoOverlay> m_overlay;
  pl::memory::FuncPtr m_hookTarget{};
  bool m_loaded{};
  bool m_hookInstalled{};
};

} // namespace

PL_REGISTER_MOD(BetterLaunchAnimationMod, BetterLaunchAnimationMod{});
