# Better launch animation

A native LeviLauncher mod (Android, arm64) that plays **your own animation** (made for a 17 second clip,
any length works) over Minecraft's normal launch animation.

Built from the structure of the BedrockTools template (Preloader `PL_REGISTER_MOD`, xmake, `.levipack`).

## How it works
- Hooks `eglSwapBuffers` in `libEGL.so` (the game imports it; it renders with OpenGL ES 3.1, no Vulkan).
- On the first real game frame it draws a full-screen overlay: your video is decoded by Android's `MediaPlayer`
  into a `SurfaceTexture` and drawn with a GLES shader. Audio comes from the same player.
- Touch / key / mouse input is swallowed while it plays; the overlay fades out when the video ends.
- **No Minecraft signatures or offsets are used**, so it should keep working across game updates.

## Add your animation
Copy your file to `resources/launch_animation.mp4` (MP4, H.264 + AAC, 30 fps, <= 1080p is safest).

## Build (no PC needed)
1. Push this folder to a GitHub repo.
2. Actions -> **Build** -> run it -> download artifact `BetterLaunchAnimation-levipack`.
3. Import `BetterLaunchAnimation.levipack` in LeviLauncher and enable it.

Local build: `xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c && xmake -y`

Set your name in `include/betterlaunch/Version.hpp` (`Author`) before building.

## Config (`config/config.json`, created on first launch)
| key | default | meaning |
|---|---|---|
| enabled | true | master switch |
| video | launch_animation.mp4 | file in resources/ (or absolute path) |
| scale_mode | fit | fit (black bars), fill (crop), stretch |
| volume | 1.0 | 0.0 - 1.0 |
| allow_skip | false | tap / Back to skip |
| fade_out_ms | 400 | fade into the game at the end |
| max_seconds | 30 | failsafe: overlay is always removed after this |

## Troubleshooting
`adb logcat -s BetterLaunchAnimation`. If the file is missing or can't be decoded, the mod steps aside and the
normal animation shows. The game keeps loading underneath the overlay.
