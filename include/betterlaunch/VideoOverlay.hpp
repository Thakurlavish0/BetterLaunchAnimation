#pragma once

#include "betterlaunch/Config.hpp"

#include <jni.h>
#include <GLES3/gl31.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

namespace pl::log {
class Logger;
}

namespace betterlaunch {

/** GLES overlay which presents a local Android MediaPlayer video on the game surface. */
class VideoOverlay final {
public:
  VideoOverlay(JavaVM *javaVm, Config config, std::filesystem::path videoPath,
               pl::log::Logger &logger);
  ~VideoOverlay();

  VideoOverlay(const VideoOverlay &) = delete;
  VideoOverlay &operator=(const VideoOverlay &) = delete;

  /** Called from the eglSwapBuffers detour, with the game's GL context current. */
  void render() noexcept;

  /** Requests a skip; the fade itself is started on the render thread. */
  void requestSkip() noexcept;

  /** True while the video (including its fade-out) is covering the game. */
  [[nodiscard]] bool isVisible() const noexcept;
  [[nodiscard]] bool allowSkip() const noexcept;

  /** Stop audio/video and release Java/GLES resources. Safe to call repeatedly. */
  void stop() noexcept;

private:
  using Clock = std::chrono::steady_clock;

  enum class Phase : unsigned char {
    WaitingForPlayer,
    Playing,
    FadingOut,
    Finished,
  };

  bool initializeOnRenderThread();
  bool createExternalTexture();
  bool createShaders();
  bool createJavaSurface();
  void startPlayerWorker();
  void playerWorker() noexcept;
  void joinPlayerWorker() noexcept;
  void renderInitializedFrame();
  void updateVideoSize(JNIEnv *env);
  void drawFrame(float opacity, float displayAspect);
  void beginFade(bool pausePlayback, JNIEnv *env);
  void finishOnRenderThread() noexcept;
  void releaseJavaObjects(JNIEnv *env) noexcept;
  void releaseGlObjectsIfPossible() noexcept;
  void logJavaException(JNIEnv *env, const char *operation) noexcept;
  void setFinished() noexcept;

  JavaVM *m_javaVm{};
  Config m_config;
  std::filesystem::path m_videoPath;
  pl::log::Logger *m_logger{};

  std::mutex m_renderMutex;
  std::mutex m_workerMutex;
  std::mutex m_shutdownMutex;
  std::thread m_playerThread;

  std::atomic<Phase> m_phase{Phase::WaitingForPlayer};
  std::atomic<bool> m_visible{false};
  std::atomic<bool> m_skipRequested{false};
  std::atomic<bool> m_stopRequested{false};
  std::atomic<bool> m_playerReady{false};
  std::atomic<bool> m_playerFailed{false};

  // The GL and SurfaceTexture state is only read or changed on the render thread,
  // except during shutdown after the hook has been removed and that thread is quiescent.
  bool m_initializationAttempted{};
  bool m_glReady{};
  bool m_javaReady{};
  bool m_videoSizeSet{};
  bool m_hasFrame{};
  bool m_finishedResourcesReleased{};
  GLuint m_videoTexture{};
  GLuint m_program{};
  GLint m_uniformVideo{-1};
  GLint m_uniformTextureMatrix{-1};
  GLint m_uniformOpacity{-1};
  GLint m_uniformDisplayAspect{-1};
  GLint m_uniformVideoAspect{-1};
  GLint m_uniformScaleMode{-1};
  float m_videoAspect{16.0F / 9.0F};
  std::array<jfloat, 16> m_transformScratch{};
  jint m_videoWidth{};
  jint m_videoHeight{};
  Clock::time_point m_setupStartedAt{};
  Clock::time_point m_playStartedAt{};
  Clock::time_point m_fadeStartedAt{};

  jobject m_surfaceTexture{};
  jobject m_surface{};
  jfloatArray m_transformArray{};
  jmethodID m_surfaceUpdateTexImage{};
  jmethodID m_surfaceGetTransformMatrix{};
  jmethodID m_surfaceGetTimestamp{};
  jmethodID m_surfaceSetDefaultBufferSize{};
  jmethodID m_surfaceTextureRelease{};
  jmethodID m_surfaceRelease{};

  // Published by the worker before m_playerReady becomes true.
  jobject m_player{};
  jmethodID m_playerIsPlaying{};
  jmethodID m_playerPause{};
  jmethodID m_playerRelease{};
  jmethodID m_playerGetVideoWidth{};
  jmethodID m_playerGetVideoHeight{};
};

} // namespace betterlaunch
