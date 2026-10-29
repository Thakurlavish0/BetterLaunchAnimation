#include "betterlaunch/VideoOverlay.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl31.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <stdexcept>
#include <utility>
#include <vector>

#include "pl/Logger.hpp"

#ifndef GL_TEXTURE_BINDING_EXTERNAL_OES
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif

namespace betterlaunch {
namespace {

using Clock = std::chrono::steady_clock;

class ScopedJniEnv final {
public:
  explicit ScopedJniEnv(JavaVM *javaVm) : m_javaVm(javaVm) {
    if (!m_javaVm) {
      return;
    }

    void *environment = nullptr;
    const jint status = m_javaVm->GetEnv(&environment, JNI_VERSION_1_6);
    if (status == JNI_OK) {
      m_env = static_cast<JNIEnv *>(environment);
    } else if (status == JNI_EDETACHED &&
               m_javaVm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) {
      m_attached = true;
    }
  }

  ScopedJniEnv(const ScopedJniEnv &) = delete;
  ScopedJniEnv &operator=(const ScopedJniEnv &) = delete;

  ~ScopedJniEnv() {
    if (m_attached && m_javaVm) {
      m_javaVm->DetachCurrentThread();
    }
  }

  [[nodiscard]] JNIEnv *get() const noexcept { return m_env; }

private:
  JavaVM *m_javaVm{};
  JNIEnv *m_env{};
  bool m_attached{};
};

template <typename Ref> class ScopedLocalRef final {
public:
  ScopedLocalRef(JNIEnv *env, Ref ref) : m_env(env), m_ref(ref) {}
  ScopedLocalRef(const ScopedLocalRef &) = delete;
  ScopedLocalRef &operator=(const ScopedLocalRef &) = delete;
  ~ScopedLocalRef() {
    if (m_env && m_ref) {
      m_env->DeleteLocalRef(m_ref);
    }
  }
  [[nodiscard]] Ref get() const noexcept { return m_ref; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return m_ref != nullptr;
  }

private:
  JNIEnv *m_env{};
  Ref m_ref{};
};

bool takeJavaException(JNIEnv *env) noexcept {
  if (!env || !env->ExceptionCheck()) {
    return false;
  }
  env->ExceptionClear();
  return true;
}

template <typename Ref>
bool failedJniResult(JNIEnv *env, Ref result) noexcept {
  const bool exception = takeJavaException(env);
  return result == nullptr || exception;
}

bool hasExtension(const char *extensions, std::string_view requested) {
  if (!extensions || requested.empty()) {
    return false;
  }

  const char *current = extensions;
  while ((current = std::strstr(current, requested.data())) != nullptr) {
    const bool startsAtWord = current == extensions || current[-1] == ' ';
    const char next = current[requested.size()];
    const bool endsAtWord = next == '\0' || next == ' ';
    if (startsAtWord && endsAtWord) {
      return true;
    }
    current += requested.size();
  }
  return false;
}

GLuint compileShader(GLenum type, const char *source, std::string &errorLog) {
  const GLuint shader = glCreateShader(type);
  if (!shader) {
    errorLog = "glCreateShader returned zero";
    return 0;
  }

  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }

  GLint logLength = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
  if (logLength > 1) {
    std::vector<char> log(static_cast<std::size_t>(logLength), '\0');
    glGetShaderInfoLog(shader, logLength, nullptr, log.data());
    errorLog.assign(log.data());
  } else {
    errorLog = "shader compilation failed without an info log";
  }
  glDeleteShader(shader);
  return 0;
}

class ScopedGlState final {
public:
  ScopedGlState() {
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_activeTexture);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &m_externalTexture);
    glGetIntegerv(GL_SAMPLER_BINDING, &m_sampler);
    glActiveTexture(static_cast<GLenum>(m_activeTexture));

    glGetIntegerv(GL_BLEND_SRC_RGB, &m_blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &m_blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blendEquationAlpha);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthWriteMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, m_colorWriteMask.data());

    m_blendEnabled = glIsEnabled(GL_BLEND);
    m_depthEnabled = glIsEnabled(GL_DEPTH_TEST);
    m_cullEnabled = glIsEnabled(GL_CULL_FACE);
    m_scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    m_stencilEnabled = glIsEnabled(GL_STENCIL_TEST);
  }

  ScopedGlState(const ScopedGlState &) = delete;
  ScopedGlState &operator=(const ScopedGlState &) = delete;

  ~ScopedGlState() {
    glUseProgram(static_cast<GLuint>(m_program));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES,
                  static_cast<GLuint>(m_externalTexture));
    glBindSampler(0, static_cast<GLuint>(m_sampler));
    glActiveTexture(static_cast<GLenum>(m_activeTexture));

    glBlendEquationSeparate(static_cast<GLenum>(m_blendEquationRgb),
                            static_cast<GLenum>(m_blendEquationAlpha));
    glBlendFuncSeparate(static_cast<GLenum>(m_blendSrcRgb),
                        static_cast<GLenum>(m_blendDstRgb),
                        static_cast<GLenum>(m_blendSrcAlpha),
                        static_cast<GLenum>(m_blendDstAlpha));
    glDepthMask(m_depthWriteMask);
    glColorMask(m_colorWriteMask[0], m_colorWriteMask[1], m_colorWriteMask[2],
                m_colorWriteMask[3]);

    restoreCapability(GL_BLEND, m_blendEnabled);
    restoreCapability(GL_DEPTH_TEST, m_depthEnabled);
    restoreCapability(GL_CULL_FACE, m_cullEnabled);
    restoreCapability(GL_SCISSOR_TEST, m_scissorEnabled);
    restoreCapability(GL_STENCIL_TEST, m_stencilEnabled);
  }

private:
  static void restoreCapability(GLenum capability, GLboolean wasEnabled) {
    if (wasEnabled == GL_TRUE) {
      glEnable(capability);
    } else {
      glDisable(capability);
    }
  }

  GLint m_program{};
  GLint m_activeTexture{GL_TEXTURE0};
  GLint m_externalTexture{};
  GLint m_sampler{};
  GLint m_blendSrcRgb{};
  GLint m_blendDstRgb{};
  GLint m_blendSrcAlpha{};
  GLint m_blendDstAlpha{};
  GLint m_blendEquationRgb{};
  GLint m_blendEquationAlpha{};
  GLboolean m_depthWriteMask{GL_TRUE};
  std::array<GLboolean, 4> m_colorWriteMask{GL_TRUE, GL_TRUE, GL_TRUE,
                                            GL_TRUE};
  GLboolean m_blendEnabled{};
  GLboolean m_depthEnabled{};
  GLboolean m_cullEnabled{};
  GLboolean m_scissorEnabled{};
  GLboolean m_stencilEnabled{};
};

constexpr char kVertexShader[] = R"glsl(#version 300 es
precision highp float;
out vec2 vUv;
void main() {
  vec2 position;
  if (gl_VertexID == 0) {
    position = vec2(-1.0, -1.0);
  } else if (gl_VertexID == 1) {
    position = vec2(1.0, -1.0);
  } else if (gl_VertexID == 2) {
    position = vec2(-1.0, 1.0);
  } else {
    position = vec2(1.0, 1.0);
  }
  vUv = position * 0.5 + 0.5;
  gl_Position = vec4(position, 0.0, 1.0);
}
)glsl";

constexpr char kFragmentShader[] = R"glsl(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES uVideo;
uniform mat4 uTextureMatrix;
uniform float uOpacity;
uniform float uDisplayAspect;
uniform float uVideoAspect;
uniform int uScaleMode;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec2 uv = vUv;
  if (uScaleMode == 0) {
    vec2 size = vec2(1.0, 1.0);
    if (uVideoAspect > uDisplayAspect) {
      size.y = uDisplayAspect / uVideoAspect;
    } else {
      size.x = uVideoAspect / uDisplayAspect;
    }
    vec2 localUv = (vUv - (vec2(1.0, 1.0) - size) * 0.5) / size;
    if (localUv.x < 0.0 || localUv.x > 1.0 ||
        localUv.y < 0.0 || localUv.y > 1.0) {
      fragColor = vec4(0.0, 0.0, 0.0, uOpacity);
      return;
    }
    uv = localUv;
  } else if (uScaleMode == 1) {
    if (uVideoAspect > uDisplayAspect) {
      uv.x = (vUv.x - 0.5) * (uDisplayAspect / uVideoAspect) + 0.5;
    } else {
      uv.y = (vUv.y - 0.5) * (uVideoAspect / uDisplayAspect) + 0.5;
    }
  }

  vec2 transformedUv = (uTextureMatrix * vec4(uv, 0.0, 1.0)).xy;
  vec3 color = texture(uVideo, transformedUv).rgb;
  fragColor = vec4(color, uOpacity);
}
)glsl";

} // namespace

VideoOverlay::VideoOverlay(JavaVM *javaVm, Config config,
                           std::filesystem::path videoPath,
                           pl::log::Logger &logger)
    : m_javaVm(javaVm), m_config(std::move(config)),
      m_videoPath(std::move(videoPath)), m_logger(&logger) {}

VideoOverlay::~VideoOverlay() { stop(); }

void VideoOverlay::requestSkip() noexcept {
  if (m_config.allowSkip && m_visible.load(std::memory_order_acquire)) {
    m_skipRequested.store(true, std::memory_order_release);
  }
}

bool VideoOverlay::isVisible() const noexcept {
  return m_visible.load(std::memory_order_acquire);
}

bool VideoOverlay::allowSkip() const noexcept { return m_config.allowSkip; }

void VideoOverlay::render() noexcept {
  try {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (m_stopRequested.load(std::memory_order_acquire)) {
      finishOnRenderThread();
      return;
    }
    if (m_phase.load(std::memory_order_acquire) == Phase::Finished) {
      return;
    }

    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
      return;
    }

    if (!m_initializationAttempted) {
      m_initializationAttempted = true;
      m_setupStartedAt = Clock::now();
      if (!initializeOnRenderThread()) {
        finishOnRenderThread();
        return;
      }
    }

    if (m_phase.load(std::memory_order_acquire) == Phase::Finished) {
      finishOnRenderThread();
      return;
    }

    if (m_playerFailed.load(std::memory_order_acquire)) {
      m_logger->warn("Animation playback failed; showing the normal game frame");
      finishOnRenderThread();
      return;
    }

    renderInitializedFrame();
    if (m_phase.load(std::memory_order_acquire) == Phase::Finished) {
      finishOnRenderThread();
    }
  } catch (const std::exception &exception) {
    m_logger->error("Launch animation render failed: {}", exception.what());
    try {
      std::lock_guard<std::mutex> lock(m_renderMutex);
      finishOnRenderThread();
    } catch (...) {
      setFinished();
    }
  } catch (...) {
    m_logger->error("Launch animation render failed with an unknown error");
    try {
      std::lock_guard<std::mutex> lock(m_renderMutex);
      finishOnRenderThread();
    } catch (...) {
      setFinished();
    }
  }
}

bool VideoOverlay::initializeOnRenderThread() {
  if (!m_javaVm) {
    m_logger->error("JavaVM was not provided by the preloader");
    return false;
  }

  std::error_code fileError;
  if (!std::filesystem::is_regular_file(m_videoPath, fileError)) {
    m_logger->warn("Animation file is missing or unreadable: {}",
                   m_videoPath.string());
    return false;
  }

  const char *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
  if (!hasExtension(extensions, "GL_OES_EGL_image_external_essl3")) {
    m_logger->error(
        "This GLES driver does not expose GL_OES_EGL_image_external_essl3; "
        "cannot display the MediaPlayer surface");
    return false;
  }

  if (!createExternalTexture()) {
    m_logger->error("Could not allocate the video texture");
    return false;
  }
  if (!createShaders()) {
    releaseGlObjectsIfPossible();
    return false;
  }
  if (!createJavaSurface()) {
    releaseGlObjectsIfPossible();
    return false;
  }

  m_glReady = true;
  try {
    startPlayerWorker();
  } catch (const std::exception &exception) {
    m_logger->error("Could not start the Android video worker: {}",
                    exception.what());
    return false;
  }
  return true;
}

bool VideoOverlay::createExternalTexture() {
  GLint previousActiveTexture = GL_TEXTURE0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
  glActiveTexture(GL_TEXTURE0);

  GLint previousTexture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &previousTexture);
  glGenTextures(1, &m_videoTexture);
  if (!m_videoTexture) {
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    return false;
  }

  glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_videoTexture);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLuint>(previousTexture));
  glActiveTexture(static_cast<GLenum>(previousActiveTexture));
  return true;
}

bool VideoOverlay::createShaders() {
  std::string errorLog;
  const GLuint vertexShader =
      compileShader(GL_VERTEX_SHADER, kVertexShader, errorLog);
  if (!vertexShader) {
    m_logger->error("Launch animation vertex shader failed: {}", errorLog);
    return false;
  }

  const GLuint fragmentShader =
      compileShader(GL_FRAGMENT_SHADER, kFragmentShader, errorLog);
  if (!fragmentShader) {
    glDeleteShader(vertexShader);
    m_logger->error("Launch animation fragment shader failed: {}", errorLog);
    return false;
  }

  m_program = glCreateProgram();
  if (!m_program) {
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    m_logger->error("glCreateProgram returned zero for the animation shader");
    return false;
  }

  glAttachShader(m_program, vertexShader);
  glAttachShader(m_program, fragmentShader);
  glLinkProgram(m_program);
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);

  GLint linked = GL_FALSE;
  glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    GLint logLength = 0;
    glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 1) {
      std::vector<char> log(static_cast<std::size_t>(logLength), '\0');
      glGetProgramInfoLog(m_program, logLength, nullptr, log.data());
      errorLog.assign(log.data());
    } else {
      errorLog = "program linking failed without an info log";
    }
    glDeleteProgram(m_program);
    m_program = 0;
    m_logger->error("Launch animation shader link failed: {}", errorLog);
    return false;
  }

  m_uniformVideo = glGetUniformLocation(m_program, "uVideo");
  m_uniformTextureMatrix = glGetUniformLocation(m_program, "uTextureMatrix");
  m_uniformOpacity = glGetUniformLocation(m_program, "uOpacity");
  m_uniformDisplayAspect = glGetUniformLocation(m_program, "uDisplayAspect");
  m_uniformVideoAspect = glGetUniformLocation(m_program, "uVideoAspect");
  m_uniformScaleMode = glGetUniformLocation(m_program, "uScaleMode");
  if (m_uniformVideo < 0 || m_uniformTextureMatrix < 0 || m_uniformOpacity < 0 ||
      m_uniformDisplayAspect < 0 || m_uniformVideoAspect < 0 ||
      m_uniformScaleMode < 0) {
    m_logger->error("A required launch animation shader uniform was optimized out");
    glDeleteProgram(m_program);
    m_program = 0;
    return false;
  }
  return true;
}

bool VideoOverlay::createJavaSurface() {
  ScopedJniEnv scopedEnv(m_javaVm);
  JNIEnv *env = scopedEnv.get();
  if (!env) {
    m_logger->error("Could not attach the GL thread to the Java VM");
    return false;
  }

  ScopedLocalRef<jclass> textureClass(
      env, env->FindClass("android/graphics/SurfaceTexture"));
  if (failedJniResult(env, textureClass.get())) {
    m_logger->error("Could not find android.graphics.SurfaceTexture");
    return false;
  }

  const jmethodID textureConstructor =
      env->GetMethodID(textureClass.get(), "<init>", "(I)V");
  m_surfaceUpdateTexImage =
      env->GetMethodID(textureClass.get(), "updateTexImage", "()V");
  m_surfaceGetTransformMatrix = env->GetMethodID(
      textureClass.get(), "getTransformMatrix", "([F)V");
  m_surfaceGetTimestamp =
      env->GetMethodID(textureClass.get(), "getTimestamp", "()J");
  m_surfaceSetDefaultBufferSize =
      env->GetMethodID(textureClass.get(), "setDefaultBufferSize", "(II)V");
  m_surfaceTextureRelease =
      env->GetMethodID(textureClass.get(), "release", "()V");
  const bool textureMethodException = takeJavaException(env);
  if (!textureConstructor || !m_surfaceUpdateTexImage ||
      !m_surfaceGetTransformMatrix || !m_surfaceGetTimestamp ||
      !m_surfaceSetDefaultBufferSize || !m_surfaceTextureRelease ||
      textureMethodException) {
    m_logger->error("Could not resolve SurfaceTexture methods");
    return false;
  }

  ScopedLocalRef<jobject> localTexture(
      env, env->NewObject(textureClass.get(), textureConstructor,
                          static_cast<jint>(m_videoTexture)));
  if (failedJniResult(env, localTexture.get())) {
    m_logger->error("Could not create android.graphics.SurfaceTexture");
    return false;
  }
  m_surfaceTexture = env->NewGlobalRef(localTexture.get());
  if (failedJniResult(env, m_surfaceTexture)) {
    m_logger->error("Could not retain the SurfaceTexture reference");
    return false;
  }

  ScopedLocalRef<jclass> surfaceClass(env, env->FindClass("android/view/Surface"));
  if (failedJniResult(env, surfaceClass.get())) {
    m_logger->error("Could not find android.view.Surface");
    return false;
  }
  const jmethodID surfaceConstructor = env->GetMethodID(
      surfaceClass.get(), "<init>", "(Landroid/graphics/SurfaceTexture;)V");
  m_surfaceRelease = env->GetMethodID(surfaceClass.get(), "release", "()V");
  const bool surfaceMethodException = takeJavaException(env);
  if (!surfaceConstructor || !m_surfaceRelease || surfaceMethodException) {
    m_logger->error("Could not resolve android.view.Surface methods");
    return false;
  }

  ScopedLocalRef<jobject> localSurface(
      env, env->NewObject(surfaceClass.get(), surfaceConstructor,
                          localTexture.get()));
  if (failedJniResult(env, localSurface.get())) {
    m_logger->error("Could not create android.view.Surface");
    return false;
  }
  m_surface = env->NewGlobalRef(localSurface.get());
  if (failedJniResult(env, m_surface)) {
    m_logger->error("Could not retain the Android video Surface");
    return false;
  }

  ScopedLocalRef<jfloatArray> localMatrix(env, env->NewFloatArray(16));
  if (failedJniResult(env, localMatrix.get())) {
    m_logger->error("Could not allocate the SurfaceTexture transform matrix");
    return false;
  }
  m_transformArray = static_cast<jfloatArray>(env->NewGlobalRef(localMatrix.get()));
  if (failedJniResult(env, m_transformArray)) {
    m_logger->error("Could not retain the SurfaceTexture transform matrix");
    return false;
  }

  m_javaReady = true;
  return true;
}

void VideoOverlay::startPlayerWorker() {
  std::lock_guard<std::mutex> lock(m_workerMutex);
  if (m_playerThread.joinable()) {
    throw std::runtime_error("video worker was already started");
  }
  m_playerThread = std::thread(&VideoOverlay::playerWorker, this);
}

void VideoOverlay::playerWorker() noexcept {
  bool started = false;
  std::string failureReason;

  try {
    if (m_stopRequested.load(std::memory_order_acquire)) {
      return;
    }

    {
      ScopedJniEnv scopedEnv(m_javaVm);
      JNIEnv *env = scopedEnv.get();
      if (!env) {
        failureReason = "could not attach the video worker to the Java VM";
      } else {
        ScopedLocalRef<jclass> playerClass(
            env, env->FindClass("android/media/MediaPlayer"));
        if (failedJniResult(env, playerClass.get())) {
          failureReason = "could not find android.media.MediaPlayer";
        } else {
          const jmethodID constructor =
              env->GetMethodID(playerClass.get(), "<init>", "()V");
          const jmethodID setDataSource = env->GetMethodID(
              playerClass.get(), "setDataSource", "(Ljava/lang/String;)V");
          const jmethodID setSurface = env->GetMethodID(
              playerClass.get(), "setSurface", "(Landroid/view/Surface;)V");
          const jmethodID setVolume =
              env->GetMethodID(playerClass.get(), "setVolume", "(FF)V");
          const jmethodID prepareAsync =
              env->GetMethodID(playerClass.get(), "prepareAsync", "()V");
          const jmethodID getDuration =
              env->GetMethodID(playerClass.get(), "getDuration", "()I");
          const jmethodID start =
              env->GetMethodID(playerClass.get(), "start", "()V");
          m_playerIsPlaying =
              env->GetMethodID(playerClass.get(), "isPlaying", "()Z");
          m_playerPause = env->GetMethodID(playerClass.get(), "pause", "()V");
          m_playerRelease =
              env->GetMethodID(playerClass.get(), "release", "()V");
          m_playerGetVideoWidth =
              env->GetMethodID(playerClass.get(), "getVideoWidth", "()I");
          m_playerGetVideoHeight =
              env->GetMethodID(playerClass.get(), "getVideoHeight", "()I");

          const bool methodsValid =
              constructor && setDataSource && setSurface && setVolume &&
              prepareAsync && getDuration && start && m_playerIsPlaying &&
              m_playerPause && m_playerRelease && m_playerGetVideoWidth &&
              m_playerGetVideoHeight;
          const bool methodException = takeJavaException(env);
          if (!methodsValid || methodException) {
            failureReason = "could not resolve MediaPlayer methods";
          } else {
            ScopedLocalRef<jobject> localPlayer(
                env, env->NewObject(playerClass.get(), constructor));
            if (failedJniResult(env, localPlayer.get())) {
              failureReason = "could not create MediaPlayer";
            } else {
              const std::string videoPath = m_videoPath.string();
              ScopedLocalRef<jstring> source(
                  env, env->NewStringUTF(videoPath.c_str()));
              ScopedLocalRef<jobject> surface(
                  env, env->NewLocalRef(m_surface));
              const bool referenceException = takeJavaException(env);
              if (!source || !surface || referenceException) {
                failureReason = "could not create Java references for the video";
              } else if (m_stopRequested.load(std::memory_order_acquire)) {
                failureReason = "stopped";
              } else {
                env->CallVoidMethod(localPlayer.get(), setDataSource,
                                    source.get());
                if (takeJavaException(env)) {
                  failureReason = "MediaPlayer.setDataSource failed";
                } else {
                  env->CallVoidMethod(localPlayer.get(), setSurface,
                                      surface.get());
                  if (takeJavaException(env)) {
                    failureReason = "MediaPlayer.setSurface failed";
                  } else {
                    env->CallVoidMethod(localPlayer.get(), setVolume,
                                        static_cast<jfloat>(m_config.volume),
                                        static_cast<jfloat>(m_config.volume));
                    if (takeJavaException(env)) {
                      failureReason = "MediaPlayer.setVolume failed";
                    } else {
                      env->CallVoidMethod(localPlayer.get(), prepareAsync);
                      if (takeJavaException(env)) {
                        failureReason = "MediaPlayer.prepareAsync failed";
                      } else {
                        const auto requestedTimeout =
                            std::chrono::milliseconds(
                                static_cast<long long>(m_config.maxSeconds) *
                                1000LL);
                        const auto prepareTimeout = std::clamp(
                            requestedTimeout, std::chrono::milliseconds(5000),
                            std::chrono::milliseconds(30000));
                        const auto deadline = Clock::now() + prepareTimeout;
                        bool prepared = false;
                        while (!m_stopRequested.load(std::memory_order_acquire) &&
                               Clock::now() < deadline) {
                          const jint duration = env->CallIntMethod(
                              localPlayer.get(), getDuration);
                          if (env->ExceptionCheck()) {
                            // MediaPlayer reports an illegal state until its
                            // asynchronous prepare event has completed.
                            env->ExceptionClear();
                          } else if (duration > 0) {
                            prepared = true;
                            break;
                          }
                          std::this_thread::sleep_for(
                              std::chrono::milliseconds(50));
                        }

                        if (!prepared) {
                          failureReason = m_stopRequested.load(
                                              std::memory_order_acquire)
                                              ? "stopped"
                                              : "timed out while preparing the video";
                        } else if (m_stopRequested.load(
                                       std::memory_order_acquire)) {
                          failureReason = "stopped";
                        } else {
                          env->CallVoidMethod(localPlayer.get(), start);
                          if (takeJavaException(env)) {
                            failureReason = "MediaPlayer.start failed";
                          } else {
                            m_player = env->NewGlobalRef(localPlayer.get());
                            if (failedJniResult(env, m_player)) {
                              m_player = nullptr;
                              failureReason =
                                  "could not retain the MediaPlayer instance";
                            } else {
                              started = true;
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }

              if (!started && m_playerRelease) {
                env->CallVoidMethod(localPlayer.get(), m_playerRelease);
                takeJavaException(env);
              }
            }
          }
        }
      }
    }
  } catch (const std::exception &exception) {
    failureReason = exception.what();
  } catch (...) {
    failureReason = "unknown native error in the video worker";
  }

  if (started) {
    m_playStartedAt = Clock::now();
    m_playerReady.store(true, std::memory_order_release);
  } else if (!m_stopRequested.load(std::memory_order_acquire) &&
             failureReason != "stopped") {
    m_logger->warn("Could not play '{}' ({}); the normal animation will show",
                   m_videoPath.string(),
                   failureReason.empty() ? "unknown MediaPlayer error"
                                         : failureReason);
    m_playerFailed.store(true, std::memory_order_release);
  }
}

void VideoOverlay::joinPlayerWorker() noexcept {
  try {
    std::lock_guard<std::mutex> lock(m_workerMutex);
    if (m_playerThread.joinable() &&
        m_playerThread.get_id() != std::this_thread::get_id()) {
      m_playerThread.join();
    }
  } catch (...) {
    m_logger->error("Could not join the Android video worker");
  }
}

void VideoOverlay::renderInitializedFrame() {
  if (!m_playerReady.load(std::memory_order_acquire)) {
    if (Clock::now() - m_setupStartedAt >=
        std::chrono::seconds(m_config.maxSeconds)) {
      m_logger->warn("Animation did not become ready before max_seconds");
      m_stopRequested.store(true, std::memory_order_release);
      finishOnRenderThread();
    }
    return;
  }

  ScopedJniEnv scopedEnv(m_javaVm);
  JNIEnv *env = scopedEnv.get();
  if (!env) {
    m_logger->error("Could not attach the render thread to Java");
    m_playerFailed.store(true, std::memory_order_release);
    return;
  }

  ScopedGlState savedGlState;
  updateVideoSize(env);

  env->CallVoidMethod(m_surfaceTexture, m_surfaceUpdateTexImage);
  if (env->ExceptionCheck()) {
    logJavaException(env, "SurfaceTexture.updateTexImage");
    m_playerFailed.store(true, std::memory_order_release);
    return;
  }

  const jlong timestamp =
      env->CallLongMethod(m_surfaceTexture, m_surfaceGetTimestamp);
  if (env->ExceptionCheck()) {
    logJavaException(env, "SurfaceTexture.getTimestamp");
    m_playerFailed.store(true, std::memory_order_release);
    return;
  }

  const auto now = Clock::now();
  if (!m_hasFrame && timestamp <= 0 &&
      now - m_playStartedAt < std::chrono::milliseconds(250)) {
    return;
  }

  env->CallVoidMethod(m_surfaceTexture, m_surfaceGetTransformMatrix,
                      m_transformArray);
  if (env->ExceptionCheck()) {
    logJavaException(env, "SurfaceTexture.getTransformMatrix");
    m_playerFailed.store(true, std::memory_order_release);
    return;
  }
  env->GetFloatArrayRegion(m_transformArray, 0,
                           static_cast<jsize>(m_transformScratch.size()),
                           m_transformScratch.data());
  if (env->ExceptionCheck()) {
    logJavaException(env, "reading the SurfaceTexture transform matrix");
    m_playerFailed.store(true, std::memory_order_release);
    return;
  }

  if (!m_videoSizeSet &&
      now - m_playStartedAt > std::chrono::seconds(5)) {
    m_logger->warn("MediaPlayer did not report a video track");
    m_playerFailed.store(true, std::memory_order_release);
    return;
  }

  if (!m_hasFrame) {
    m_hasFrame = true;
    m_phase.store(Phase::Playing, std::memory_order_release);
    m_visible.store(true, std::memory_order_release);
  }

  jboolean javaPlaying = env->CallBooleanMethod(m_player, m_playerIsPlaying);
  if (env->ExceptionCheck()) {
    logJavaException(env, "MediaPlayer.isPlaying");
    m_playerFailed.store(true, std::memory_order_release);
    return;
  }
  const bool playing = javaPlaying == JNI_TRUE;

  Phase phase = m_phase.load(std::memory_order_acquire);
  if (phase == Phase::Playing) {
    const bool skipped = m_skipRequested.exchange(false,
                                                   std::memory_order_acq_rel);
    const bool timedOut =
        now - m_playStartedAt >= std::chrono::seconds(m_config.maxSeconds);
    if (!playing || skipped || timedOut) {
      beginFade(playing, env);
      phase = m_phase.load(std::memory_order_acquire);
    }
  }

  if (phase == Phase::Playing) {
    drawFrame(1.0F, 1.0F);
    return;
  }

  if (phase == Phase::FadingOut) {
    const auto fadeDuration = std::chrono::milliseconds(m_config.fadeOutMs);
    if (fadeDuration.count() == 0) {
      setFinished();
      return;
    }

    const auto elapsed = now - m_fadeStartedAt;
    const float progress = std::clamp(
        std::chrono::duration<float>(elapsed).count() /
            std::chrono::duration<float>(fadeDuration).count(),
        0.0F, 1.0F);
    if (progress >= 1.0F) {
      setFinished();
      return;
    }

    drawFrame(1.0F - progress, 1.0F);
  }
}

void VideoOverlay::updateVideoSize(JNIEnv *env) {
  if (m_videoSizeSet || !m_player || !m_playerGetVideoWidth ||
      !m_playerGetVideoHeight) {
    return;
  }

  const jint width = env->CallIntMethod(m_player, m_playerGetVideoWidth);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return;
  }
  const jint height = env->CallIntMethod(m_player, m_playerGetVideoHeight);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return;
  }
  if (width <= 0 || height <= 0) {
    return;
  }

  env->CallVoidMethod(m_surfaceTexture, m_surfaceSetDefaultBufferSize, width,
                      height);
  if (env->ExceptionCheck()) {
    logJavaException(env, "SurfaceTexture.setDefaultBufferSize");
    return;
  }

  m_videoWidth = width;
  m_videoHeight = height;
  m_videoAspect = static_cast<float>(width) / static_cast<float>(height);
  m_videoSizeSet = std::isfinite(m_videoAspect) && m_videoAspect > 0.0F;
}

void VideoOverlay::drawFrame(float opacity, float displayAspect) {
  GLint viewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, viewport);
  if (viewport[2] <= 0 || viewport[3] <= 0) {
    EGLint width = 0;
    EGLint height = 0;
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE &&
        eglQuerySurface(display, surface, EGL_WIDTH, &width) == EGL_TRUE &&
        eglQuerySurface(display, surface, EGL_HEIGHT, &height) == EGL_TRUE &&
        width > 0 && height > 0) {
      displayAspect = static_cast<float>(width) / static_cast<float>(height);
    }
  } else {
    displayAspect = static_cast<float>(viewport[2]) /
                    static_cast<float>(viewport[3]);
  }
  if (!std::isfinite(displayAspect) || displayAspect <= 0.0F) {
    displayAspect = 16.0F / 9.0F;
  }

  glUseProgram(m_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_videoTexture);
  glBindSampler(0, 0);
  glUniform1i(m_uniformVideo, 0);
  glUniformMatrix4fv(m_uniformTextureMatrix, 1, GL_FALSE,
                     reinterpret_cast<const GLfloat *>(m_transformScratch.data()));
  glUniform1f(m_uniformOpacity, opacity);
  glUniform1f(m_uniformDisplayAspect, displayAspect);
  glUniform1f(m_uniformVideoAspect, m_videoAspect);
  glUniform1i(m_uniformScaleMode, static_cast<GLint>(m_config.scaleMode));

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void VideoOverlay::beginFade(bool pausePlayback, JNIEnv *env) {
  m_skipRequested.store(false, std::memory_order_release);
  if (pausePlayback && m_player && m_playerPause) {
    env->CallVoidMethod(m_player, m_playerPause);
    if (env->ExceptionCheck()) {
      logJavaException(env, "MediaPlayer.pause");
    }
  }

  m_fadeStartedAt = Clock::now();
  m_phase.store(Phase::FadingOut, std::memory_order_release);
}

void VideoOverlay::finishOnRenderThread() noexcept {
  if (m_finishedResourcesReleased) {
    setFinished();
    return;
  }

  joinPlayerWorker();
  ScopedJniEnv scopedEnv(m_javaVm);
  if (JNIEnv *env = scopedEnv.get()) {
    releaseJavaObjects(env);
  } else if (m_javaReady) {
    m_logger->warn("Java references could not be released; the process is exiting");
  }
  releaseGlObjectsIfPossible();
  m_finishedResourcesReleased = true;
  setFinished();
}

void VideoOverlay::releaseJavaObjects(JNIEnv *env) noexcept {
  if (!env) {
    return;
  }

  if (m_player) {
    if (m_playerRelease) {
      env->CallVoidMethod(m_player, m_playerRelease);
      takeJavaException(env);
    }
    env->DeleteGlobalRef(m_player);
    m_player = nullptr;
  }
  if (m_surface) {
    if (m_surfaceRelease) {
      env->CallVoidMethod(m_surface, m_surfaceRelease);
      takeJavaException(env);
    }
    env->DeleteGlobalRef(m_surface);
    m_surface = nullptr;
  }
  if (m_surfaceTexture) {
    if (m_surfaceTextureRelease) {
      env->CallVoidMethod(m_surfaceTexture, m_surfaceTextureRelease);
      takeJavaException(env);
    }
    env->DeleteGlobalRef(m_surfaceTexture);
    m_surfaceTexture = nullptr;
  }
  if (m_transformArray) {
    env->DeleteGlobalRef(m_transformArray);
    m_transformArray = nullptr;
  }
  m_javaReady = false;
}

void VideoOverlay::releaseGlObjectsIfPossible() noexcept {
  if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
    return;
  }

  if (m_program) {
    glDeleteProgram(m_program);
    m_program = 0;
  }
  if (m_videoTexture) {
    glDeleteTextures(1, &m_videoTexture);
    m_videoTexture = 0;
  }
  m_glReady = false;
}

void VideoOverlay::logJavaException(JNIEnv *env,
                                    const char *operation) noexcept {
  if (env && env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  m_logger->warn("Java call failed while {}", operation);
}

void VideoOverlay::setFinished() noexcept {
  m_visible.store(false, std::memory_order_release);
  m_phase.store(Phase::Finished, std::memory_order_release);
  m_skipRequested.store(false, std::memory_order_release);
}

void VideoOverlay::stop() noexcept {
  try {
    std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
    m_stopRequested.store(true, std::memory_order_release);
    joinPlayerWorker();

    std::lock_guard<std::mutex> renderLock(m_renderMutex);
    if (!m_finishedResourcesReleased) {
      ScopedJniEnv scopedEnv(m_javaVm);
      if (JNIEnv *env = scopedEnv.get()) {
        releaseJavaObjects(env);
      } else if (m_javaReady) {
        m_logger->warn("Java references could not be released during shutdown");
      }
      releaseGlObjectsIfPossible();
      m_finishedResourcesReleased = true;
    }
    setFinished();
  } catch (...) {
    setFinished();
  }
}

} // namespace betterlaunch
