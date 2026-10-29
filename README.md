# Better Launch Animation

A native Android arm64 LeviLauncher mod that plays a video over Minecraft's launch screen, then fades back to the game. The mod source uses the LeviLauncher **Preloader Android** lifecycle and packages as a `.levipack`.

> The linked [`bedrock-tool/bedrocktool`](https://github.com/bedrock-tool/bedrocktool) is a separate Go desktop/CLI utility for Bedrock worlds, skins, and related tools; it does not provide LeviLauncher native-mod hooks. This mod therefore uses the compatible Preloader Android API already configured in this repository rather than importing unrelated code from that project.

## Add your animation

Copy your MP4 to `resources/launch_animation.mp4`. Recommended format: MP4 with H.264 video and AAC audio, 30 fps, and 1080p or lower. The repository does not include a video, so until you add one the mod safely leaves Minecraft's normal launch animation visible.

## What the mod does

- Hooks `eglSwapBuffers` in `libEGL.so` using the Preloader hook API; no Minecraft signatures or offsets are used.
- Decodes the local video with Android `MediaPlayer` on a worker thread and presents it with a `SurfaceTexture` / GLES external texture.
- Supports fit (black bars), fill (center crop), and stretch scale modes; audio volume is configurable.
- Swallows touch, key, mouse, and text input while the video is visible. Optional tap/Back/Escape skipping fades the video out.
- Fades out at the end of playback and has a configurable maximum display time. Missing or unsupported video falls back to the normal game screen.

## Build (no PC needed)

1. Push this folder to a GitHub repository.
2. Open **Actions → Build → Run workflow**.
3. Download the `BetterLaunchAnimation-levipack` artifact.
4. Import `BetterLaunchAnimation.levipack` in LeviLauncher and enable the mod.

Local build:

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

Set the author in `include/betterlaunch/Version.hpp` before building. The pack contains the native library, `manifest.json`, icon, and files from `resources/`.

## Config

Created at `config/config.json` on first launch:

| key | default | meaning |
|---|---:|---|
| `enabled` | `true` | Master switch |
| `video` | `launch_animation.mp4` | File under `resources/` (or an absolute path) |
| `scale_mode` | `fit` | `fit` (black bars), `fill` (crop), or `stretch` |
| `volume` | `1.0` | Audio volume from `0.0` to `1.0` |
| `allow_skip` | `false` | Allow tap, Back, or Escape to start the fade |
| `fade_out_ms` | `400` | Fade duration in milliseconds |
| `max_seconds` | `30` | Maximum overlay time and preparation failsafe, in seconds |

## Troubleshooting

Use `adb logcat -s "Better Launch Animation"` for diagnostics. If the video is missing, unreadable, or unsupported by Android's media decoder/GLES external-texture extension, the mod logs the reason and steps aside so the normal game animation can continue.
