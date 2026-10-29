# Launch animation resource

Place a real MP4 here as `launch_animation.mp4` before building the `.levipack`.
Use H.264 video and AAC audio; 30 fps and 1080p or lower are recommended.
The build warns and can proceed if the file is absent, but rejects a present file
that does not have an MP4 container header. Without a video, the mod leaves the
normal Minecraft launch screen unchanged.
