# Architecture

## Layer cake

```
┌──────────────────────────────────────────────────────────────────────┐
│  Host app (the Flutter infotainment system)                          │
│                                                                      │
│    Stack(children: [                                                 │
│      AndroidAutoView(),        // the projected phone screen         │
│      MyStatusBar(),            // ordinary Flutter widgets on top    │
│      MyNowPlayingCard(),       // fed by metadata, not pixels        │
│    ])                                                                │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ package:android_auto
┌───────────────────────────────▼──────────────────────────────────────┐
│  android_auto                 (app facing, pure Dart, no aasdk)      │
│    AndroidAutoView, AndroidAutoController                            │
│                                                                      │
│  android_auto_platform_interface  (pure Dart, no aasdk)              │
│    AndroidAutoPlatform, config and event models                      │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ implements
┌───────────────────────────────▼──────────────────────────────────────┐
│  android_auto_linux           (GPL-3.0, links aasdk)                 │
│                                                                      │
│  Dart:   AndroidAutoLinux ── dart:ffi ──┐   NativeCallable.listener  │
│                                         │            ▲               │
│  ─────────────────────────────────────  │  ──────────│───────────    │
│  Native (C++):                          ▼            │               │
│                                                                      │
│    android_auto_linux_plugin.cc   GTK entry point                    │
│      └─ stashes FlTextureRegistrar + FlView into a process global    │
│                                                                      │
│    aa_core.h                      flat C ABI, the only FFI surface   │
│    aa_core.cc                     session lifecycle, io_context pool │
│      ├─ VideoPipeline    H.264 ─> VA-API/SW ─> dmabuf ─> present     │
│      ├─ AudioSinks x3    PCM ─> PipeWire (media / system / speech)   │
│      ├─ AudioSource      PipeWire ─> PCM ─> phone (microphone)       │
│      ├─ InputSink        Flutter pointers ─> AA touch events         │
│      ├─ SensorSource     Dart pushes ─> AA sensor events             │
│      └─ MetadataTaps     nav / media / phone status ─> Dart events   │
│                                                                      │
│    present/gl_adapter.cc          dmabuf ─> EGLImage ─> FlTextureGL  │
│    (present/vk_adapter.cc         dmabuf ─> VkImage, not written yet)│
│    event_bus.cc                   native ─> Dart event queue         │
│                                                                      │
│    third_party/aasdk (submodule) + patches/                          │
│      USB / TCP transport, SSL, framing, protobuf, channel objects    │
└──────────────────────────────────────────────────────────────────────┘
```

## Staying ahead of the renderer, not behind it

Flutter 3.47 made Impeller the only renderer on Linux, running its **OpenGLES** backend.
`--no-enable-impeller` does nothing, which was verified on this machine. An Impeller
**Vulkan** backend for Linux desktop is in progress upstream, and Flutter GPU and
`flutter_scene` both point the same way: `flutter_scene`'s retained backend only
activates on Metal and Vulkan contexts. A host app that wants 3D in its infotainment UI
will want Linux on Vulkan.

That makes "hand Flutter a GL texture" the wrong thing to build the pipeline around,
because `FlTextureGL` is an OpenGL interface by name and by signature.

**So the pipeline's output is a dmabuf, not a texture.** A dmabuf is what VA-API already
decodes into, and it imports into either graphics API:

| Target | Import path |
|---|---|
| OpenGL / EGL | `EGL_EXT_image_dma_buf_import` to `EGLImage` to GL texture |
| Vulkan | `VK_EXT_external_memory_dma_buf` plus `VK_EXT_image_drm_format_modifier` to `VkImage` |

Built and measured: VA-API decodes into a surface that never leaves the GPU, it is
exported as two DRM prime layers (R8 luma, GR88 chroma), and `gl_adapter` imports both as
`EGLImage`s and converts them to RGBA with a three instruction shader, all on Flutter's
raster thread inside `populate()`. Wire to frame, measured on one machine against one
phone (a Pixel 8 Pro) at 1280x720: **0.9 to 1.1 ms**. The software fallback, which
converts with libswscale on the CPU, measures 2.9 ms.

Concretely, the seam is:

```
H.264 ─> decoder ─> dmabuf fd + DRM format modifier + stride  ┐
                                                              │  everything above here
                                                              │  is API agnostic
   ────────────────────────────────────────────────────────── ┤
                                                              │  present adapter,
   gl_adapter   dmabuf ─> EGLImage ─> FlTextureGL  (today)    │  the only API
   vk_adapter   dmabuf ─> VkImage  ─> whatever the Linux      │  specific code
                embedder exposes for Vulkan (not written)     ┘
```

Rules that keep this true:

- Nothing above the seam may name a GL type. The decoder hands over
  `{int fd, uint64_t modifier, uint32_t stride, offset, fourcc, width, height}`, plus
  the four edges to crop off, see "Filling a view of any shape" below. The crop is the
  adapter's job, done while sampling, so nothing is ever copied to cut the margins away.
- The adapter also owns the one scale that happens before Flutter: when the texture is
  drawn smaller than the picture, it renders at the drawn size and averages every
  source pixel into it, because Flutter's sampling of an external texture skips pixels
  when shrinking and the result looks grainy.
- `FlTextureGL` is referenced in exactly one file, `present/gl_adapter.cc`.
- The software decode fallback also produces a dmabuf where it can, and only drops to
  `FlPixelBufferTexture` when the driver gives us nothing better.

The cost of this discipline is one indirection. The cost of skipping it is a rewrite the
day Linux flips to Vulkan.

## Threads

| Thread | Owns | Must never |
|---|---|---|
| Flutter platform thread | plugin registration, method channel, FFI calls from Dart | block on IO |
| Flutter raster thread | calls our `FlTextureGL::populate`, GL context current | do decoding |
| `io_context` pool (2 threads) | aasdk transport, SSL, channel dispatch | touch GL or Dart |
| Decoder thread | libavcodec, dmabuf export | touch Dart |
| Audio writer threads (one per stream) | one `PcmSink` each, blocking writes | touch Dart, or run on the io_context |
| Capture thread (only while the phone has the microphone open) | one `PcmSource`, blocking reads | touch Dart, or outlive the phone's request |

Handoff rules:

- Decoded frames go into a small ring of dmabuf backed surfaces. The present adapter
  imports the current one, using a **shared** GL context (created with
  `gdk_window_create_gl_context()` on the `FlView` window) while we are on GL, then
  flips an index and calls `fl_texture_registrar_mark_texture_frame_available()`.
- `populate()` on the raster thread only reads the current index. No locks held across
  GL calls, no allocation.
- Native to Dart events are pushed onto a lock free queue and posted to a Dart
  `NativeCallable.listener`. Never call into Dart from an aasdk callback directly.
- PCM is copied out of the aasdk buffer on the io thread, queued, and written by the
  stream's own thread. The write blocks until the audio server takes it, which is what
  paces the head unit to real time; doing that on an io thread would stall the USB
  transport. `audio/pcm_sink.h` is the seam, the same idea as `frame_ring.h` is for
  video: nothing above it names PulseAudio.
- Microphone PCM runs the same way in reverse, through `audio/pcm_source.h`: the capture
  thread blocks on the read, which is what paces the stream to real time, and posts each
  buffer onto the channel strand to be sent. The thread exists only while the phone has
  the microphone open, which is what makes "is this machine listening" a question with an
  observable answer rather than a promise.

## The C ABI

The FFI surface is deliberately flat: plain integers, doubles and C strings, with
anything richer crossing as one JSON string per update. Everything the head unit reports
back to Dart goes through a single event callback rather than out parameters, so the
whole thing binds with `ffigen` and no hand written glue.

**`packages/android_auto_linux/linux/src/aa_core.h` is the definition**, and it carries
a doc comment on every function explaining not just what it does but why it is shaped
that way. A summary here would be a second source of truth that drifts, which is exactly
what happened to the sketch this section used to hold: by the time anybody read it, every
function name in it was wrong.

Broadly, the header groups into session lifecycle, the texture id, input, audio, the
microphone, sensors, metadata and wireless. `AaState` in that header and
`AndroidAutoConnectionState` in the platform interface cross as plain integers, so their
orders have to stay in step.

## Filling a view of any shape

The protocol names five frame sizes and every one is 16:9, while a head unit's view
almost never is once the host app has drawn anything beside it. So the phone is asked to
leave margins: it still encodes the whole frame, lays its interface out in the rectangle
inside the margins and paints the rest black, and the head unit crops the black off
again. The texture Flutter gets is exactly the view's shape.

```
AndroidAutoView layout ─> aa_session_set_view_size (physical pixels)
   ─> FrameSizeForView, unless the host app named a size
   ─> MarginsForView
   ─> service discovery: width_margin, height_margin
   ─> decoder: crop stamped on every frame of the advertised size
   ─> gl_adapter: samples only the inside, output texture is the visible size
```

What the phone does with margins is written down nowhere in the schema, and was measured
on a Pixel 8 Pro:

- The inside rectangle is **centred**: a margin announced as a total is split evenly
  between two opposite sides.
- Touch positions are **relative to the inside rectangle's top left corner, not the
  frame's, and unscaled**. The touchscreen is still announced at the full frame size,
  which is the only arrangement that survives the margins changing mid session.

A view that changes shape while a phone is projecting is followed, by
`VideoChannel::Resize`: video focus goes back to the phone, which stops the stream; an
`UpdateUiConfigRequest` carries the new margins per side and the phone answers with the
ones it took; focus is taken again and a new stream starts, laid out for them from its
first frame. The focus round trip matters. Sent alone, the update re-lays out the
phone's launcher but leaves the app in front drawn for the old margins, and the stream
freezes until something else on screen changes. The restart is also what makes the crop
exact, since the decoder is told the new margins while no frame is in flight.

## Coordinate mapping

The texture has its own resolution (say 1280x676, the frame less its margins).
`AndroidAutoView` is laid out in logical pixels and may be letterboxed. The mapping from
a Flutter `PointerEvent.localPosition` to protocol coordinates is:

```
projected_x = (local.dx - letterbox_left) / rendered_width  * texture_width
projected_y = (local.dy - letterbox_top)  / rendered_height * texture_height
```

Do this in Dart, not native, so the widget's `BoxFit` stays the single source of truth.
Nothing has to add the margins back: the phone reads positions from the corner of the
picture it drew, which is the texture's corner.

## Package boundaries and licence

`android_auto` and `android_auto_platform_interface` contain no aasdk code.
`android_auto_linux` links aasdk and is GPL-3.0-or-later, and so is every package here
today: the split is in the code, not in the licence grant.

What it buys is optionality. A future permissive implementation could be dropped in
without changing a line of host app code, and at that point relicensing the two pure
Dart packages would be a matter of agreement among their contributors rather than of
untangling anything.

## What makes this different from shelling out to the DHU

The whole reason for the project: the projection is a **texture inside the Flutter
widget tree**, not a separate X/Wayland window. That buys:

- Flutter widgets composited over the projection with full control of z-order and opacity
- The host app owns the window, fullscreen state, and multi-display layout
- Metadata channels render as real Flutter widgets, in the app's own design language
- No window manager hacks, no screen scraping, no second process to babysit
