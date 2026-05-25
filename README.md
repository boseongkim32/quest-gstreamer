# Quest GStreamer

Low-latency H.264 RTP/UDP video streaming for Meta Quest 3 in Unity, using
GStreamer, Android MediaCodec, and a Vulkan zero-copy render path.

This repository is intentionally GitHub-only. It is not a Unity Package Manager
package and does not include a `package.json`.

## Target

- Unity: 2022.3 LTS
- Device: Meta Quest 3
- Platform: Android, ARM64 only
- Graphics API: Vulkan only
- Stream format: H.264 over RTP/UDP
- Native path: GStreamer `udpsrc`/RTP/H.264 parse -> Android MediaCodec ->
  AImageReader/AHardwareBuffer -> Vulkan blit into a Unity `RenderTexture`

Other headsets, OpenGLES, desktop players, and non-ARM64 Android builds are out
of scope for this repo.

## Unity Setup

Copy this folder into your Unity project:

```text
UnityAssets/QuestGStreamer
```

Recommended destination:

```text
YourUnityProject/Assets/QuestGStreamer
```

The folder includes:

```text
GstVideoStream.cs
Plugins/Android/libs/arm64-v8a/libgst_unity_plugin.so
Plugins/Android/libs/arm64-v8a/libgstreamer_android.so
Plugins/Android/libs/arm64-v8a/libc++_shared.so
```

In Unity:

1. Set the build target to Android.
2. Enable ARM64.
3. Use Vulkan as the graphics API.
4. Deploy to Meta Quest 3.
5. Add `GstVideoStream` to a GameObject.
6. Assign `targetRenderer` to the renderer that should display the video.
7. Set the UDP port, texture size, decoder name, MIME type, and expected frame
   rate in the inspector.

The sender should already be producing the RTP/UDP stream before the Unity scene
activates the plugin. If no UDP packets arrive for 1.5 seconds, the native
stream transitions to `Stalled`; call `RestartStream()` after the sender is back.

## Example Sender Script

```bash
gst-launch-1.0 videotestsrc pattern=ball is-live=true \
  ! video/x-raw,width=3840,height=2160,framerate=60/1,format=I420 \
  ! vtenc_h264 bitrate=50000 realtime=true allow-frame-reordering=false \
  ! video/x-h264,profile=high \
  ! h264parse config-interval=-1 \
  ! rtph264pay pt=96 \
  ! udpsink host=<ip_address> port=5000
```
## Default Decoder Settings

The C# component defaults to:

```text
decoderCodecName = c2.qti.avc.decoder.low_latency
decoderMime = video/avc
expectedFrameRate = 60
port = 5000
textureWidth = 1280
textureHeight = 720
```

Leave `decoderCodecName` empty to let Android choose a generic AVC decoder.

## Rebuilding Native Libraries

The prebuilt libraries in `UnityAssets/QuestGStreamer/Plugins/Android/libs` are
included for convenience. To rebuild them, install:

- Android NDK r25.2.9519653
- GStreamer Android universal 1.26.9

Then run:

```sh
GSTREAMER_ROOT_ANDROID=/path/to/gstreamer-1.0-android-universal-1.26.9 \
ndk-build APP_BUILD_SCRIPT=jni/Executable.mk NDK_APPLICATION_MK=jni/Application.mk
```

Copy the rebuilt outputs into the Unity asset folder:

```text
libs/arm64-v8a/libgst_unity_plugin.so
libs/arm64-v8a/libgstreamer_android.so
libs/arm64-v8a/libc++_shared.so
```

to:

```text
UnityAssets/QuestGStreamer/Plugins/Android/libs/arm64-v8a/
```

## Runtime States

`GstVideoStream` exposes the native stream state:

```text
Stopped
Starting
Streaming
Stalled
Failed
```

`Stalled` means the UDP source stopped delivering packets. `Failed` means the
native pipeline hit a terminal decoder, renderer, format, or source startup
error. In either case, restart after fixing the sender or stream conditions.

## Repository Layout

```text
UnityAssets/QuestGStreamer/   Copy this folder into a Unity project's Assets
jni/                          Native Android/GStreamer/MediaCodec/Vulkan code
include/Unity/                Unity native plugin headers
```
