# Better Launch Animation

A native Android arm64 LeviLauncher mod that plays a video over Minecraft's launch screen, then fades back to the game. Its project and `.levipack` layout follow the native-mod pattern used by [QYCottage/BedrockTools](https://github.com/QYCottage/BedrockTools), with the Preloader lifecycle and xmake build. The overlay itself uses Android media and GLES APIs, not Minecraft signatures or version-specific offsets.

## Add your animation

Place the actual video at `resources/launch_animation.mp4`. Use MP4/H.264 video with AAC audio; 30 fps and 1080p or lower are recommended. The packager checks the MP4 container header so a placeholder or renamed non-video file cannot silently produce a nonworking pack. If the file is absent, the mod falls back to the normal Minecraft launch animation.

## What the mod does

- Follows the BedrockTools load sequence: watches for `libminecraftpe.so` and only then hooks `eglSwapBuffers`, avoiding playback over LeviLauncher's own UI.
- Decodes the local video with Android `MediaPlayer` on a worker thread and presents it with a `SurfaceTexture` / GLES external texture.
- Supports fit (black bars), fill (center crop), and stretch modes; audio volume is configurable.
- Swallows touch, key, mouse, and text input while the video is visible. Optional tap/Back/Escape skipping fades the video out.
- Fades out at the end of playback and has a configurable maximum display time. Missing or unsupported media falls back to the normal game screen.

## Build

The project requires Android NDK r28c, xmake, and Python 3. GitHub Actions builds the Android arm64 package:

1. Push the project to GitHub or use **Actions → Build → Run workflow**.
2. Download the `BetterLaunchAnimation-levipack` artifact.
3. Import `BetterLaunchAnimation.levipack` in LeviLauncher and enable the mod.

Local build:

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The `.levipack` places `libBetterLaunchAnimation.so` at its root, includes `manifest.json`, the icon, and files under `resources/`. Set the author in `include/betterlaunch/Version.hpp` before building.

## Config

Created at `config/config.json` on first launch:

| key | default | meaning |
|---|---:|---|
| `enabled` | `true` | Master switch |
| `video` | `launch_animation.mp4` | File under `resources/` or an absolute path |
| `scale_mode` | `fit` | `fit` (black bars), `fill` (crop), or `stretch` |
| `volume` | `1.0` | Audio volume from `0.0` to `1.0` |
| `allow_skip` | `false` | Allow tap, Back, or Escape to start the fade |
| `fade_out_ms` | `400` | Fade duration in milliseconds |
| `max_seconds` | `30` | Maximum overlay time and preparation failsafe, in seconds |

## Troubleshooting

Use `adb logcat -s "Better Launch Animation"` for diagnostics. If the video is unreadable or unsupported by Android's media decoder/GLES external-texture extension, the mod logs the reason and steps aside so the normal game animation can continue.
