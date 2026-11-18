# GE GPU backend — architecture and plan

Status: complete; M0–M5 implemented and validated on `feat/gl-present-m0`.
No PR until the GE backend is complete.

## Goal

Render the PSP GE on the host GPU, at a configurable physical resolution
(1x/2x/4x/10x), while keeping the recompiled guest and the software renderer
as the source of truth for VRAM and framebuffer effects.

## Non-goals / constraints

- Do not link or copy PPSSPP code. PPSSPP is GPLv2+; this backend is an
  independent implementation. Use its docs and source only as a reference.
- OpenGL 3.3 core first. Vulkan is a later backend, not a parallel rewrite.
- The software renderer stays available as the accuracy oracle and per-draw
  fallback. It is the only backend that runs without a GPU.
- No guest-code changes for resolution. Guest coordinates remain 480x272.

## Architecture

```
guest threads                       GPU thread                 window thread
-------------                       ----------                 -------------
GE decode (ge.c)
  -> psp_render_gl frontend
       -> software renderer (guest VRAM, authoritative)
       -> command queue  ---------> dequeue
                                     render context (shared)
                                     textures/FBOs at physical res
                                     fence/sync
                                                  shared FBO texture
                                                               present context
                                                               SwapBuffers
```

### Threads and contexts

- **Guest threads** run the GE decode and `psp_render_gl`. They never call GL.
  They run the software renderer for the guest-visible framebuffer and enqueue
  snapshots of each draw.
- **GPU thread** owns the render context. It dequeues commands, uploads
  textures/snapshots, renders into shared FBOs, and signals frame completion.
- **Window thread** owns the present context and the window/message loop. It
  presents the last completed shared FBO.
- Render and present contexts are created on the window thread with a shared
  context; the render context is made current on the GPU thread, the present
  context stays on the window thread. Shared textures/FBOs cross the contexts.
- Context/thread lifetime: create on the window thread, hand the render context
  to the GPU thread, join before destroying the window/context.

### Command queue

Bounded single-producer/single-consumer ring. The producer is the guest thread
holding the CPU baton; the consumer is the GPU thread. No allocations in the
hot path. Backpressure: when full, the guest waits or falls back to software for
that draw (the software renderer already produced the pixels).

Commands (fixed-size header + payload):

- `SET_TARGET(addr, stride, fmt)`
- `CLEAR(flags, color, depth, stencil, scissor)`
- `DRAW(prim, count, vertices[], state, texture snapshot)`
- `SYNC` (guest will read VRAM / framebuffer)
- `PRESENT(frame_id)`
- `RESIZE(width, height, scale)`
- `UPLOAD_TEXTURE(hash, data, format)`

Vertex/state payloads are copied into the ring; guest pointers are never
dereferenced on the GPU thread. Textures are copied or hashed-and-cached; a
texture that points into a framebuffer is read from the software framebuffer
snapshot, not from a raced VRAM read.

### Resolution

- Logical resolution is always 480x272 for the guest.
- Physical render target is `480*scale x 272*scale` (1x/2x/4x/10x).
- The GE viewport, scissor, screen coordinates and texture coordinates are
  scaled at command recording; the guest never sees the scale.
- 2D UI and pixel-mapped sprites stay nearest-filtered (smart 2D); 3D uses the
  physical resolution and MSAA when available.
- The present context draws the completed FBO with the window's aspect fit.

### VRAM and framebuffer sync

The software renderer remains authoritative until M3:

- Guest CPU reads (`psp_read*`) and framebuffer-to-texture draws see the
  software framebuffer.
- A draw that textures from a framebuffer, or a CPU read of a render target,
  triggers a readback/sync or a per-draw software fallback.
- `sceGeDrawSync`/`sceDisplaySetFrameBuf` are natural sync points.
- At M3, FBOs can become authoritative for the main target; the software
  framebuffer is only a mirror/readback for the guest. The fallback remains for
  unsupported state.

### Textures

- Support PSP formats: 8888, 565, 5551, 4444, CLUT4/8/16/32, swizzle.
- Decode/expand textures into a GPU format; cache by guest address + content
  hash. Framebuffer textures use the software snapshot.
- Texture combine modes, alpha test, blending, stencil and depth follow the
  existing `psp_draw_state` semantics; the software renderer is the oracle for
  every new state path.
- HD texture replacement is a later layer on the same cache (hash -> override).

## Milestones

- **M0** (done): OpenGL 3.3 presenter, shared 480x272 texture, Lanczos2,
  `SETO_PRESENT=gl`, GDI fallback, launcher selector.
- **M1** (done): GPU thread + SPSC ring queue + shared context; untextured
  GE primitives drawn to an FBO and presented to the window.
- **M2** (done): textured draws (formats/CLUT/swizzle, combine modes, alpha/
  color/stencil/depth tests, blending, fog, LMODE specular) verified against
  software lockstep captures.
- **M3** (done): authoritative multi-target FBO manager (`GE_MAX_FBOS=16`),
  on-demand readbacks, FBO-as-texture + self-texturing, `GE_TRXKICK` blits,
  per-draw software fallbacks with VRAM<->FBO sync, RAM display buffer path.
- **M4** (done): physical scale `1x..10x` / `auto`, subpixel 3D vertex
  precision (`fx, fy, clip_w`), smart 2D texel-edge scaling, dynamic resize,
  and scaled FBO-to-FBO blits/readbacks.
- **M5** (done): on-demand display readback hook (`psp_render_backend.read_display`),
  lazy `ge_present` VRAM sync, raw texel + CLUT FNV-1a pre-hash skipping CPU
  texture decode on cache hits, pre-allocated `glBufferSubData` VBO +
  `glFenceSync`/`glWaitSync` non-blocking sync, hardware 3D MSAA (`2x/4x/8x`),
  GDI+ PNG HD texture dump & replacement (`SETO_DUMP_TEX_DIR` /
  `SETO_HD_TEX_DIR`), post-processing shaders (`CAS`, `PSP LCD`, `CRT`),
  composited OpenGL FPS HUD & OSK overlay, and launcher controls.

## M5 Environment Variables & Launcher Controls

- `SETO_RENDERER=gl`: Select `seto_ge_backend` (software remains default).
- `SETO_PRESENT=gl`: Enable the OpenGL 3.3 presenter window.
- `SETO_GE_GPU=draw`: Run the GE command queue + OpenGL 3.3 GPU renderer.
- `SETO_RENDER_SCALE=auto|1..10`: Internal render resolution multiplier.
- `SETO_SMART_2D=1|0`: Enable/disable crisp smart-2D filtering on non-HD 2D sprites.
- `SETO_MSAA=0|2|4|8`: Hardware MSAA sample count for 3D draws (clamped to `GL_MAX_SAMPLES` and adaptively capped to `<= 21M` samples/FBO at extreme internal scales unless `SETO_FORCE_MSAA=1`; disabled automatically for through-mode 2D and clear passes).
- `SETO_FILTER=0|1|2`: Window present scaler (`0` nearest, `1` bilinear, `2` Lanczos2).
- `SETO_POST_FILTER=0|1|2|3` (or `off|cas|lcd|crt`): Presenter post-processing filter.
- `SETO_DUMP_TEX_DIR=<dir>`: Dump unique decoded textures once as `<16-hex-hash>.png`.
- `SETO_HD_TEX_DIR=<dir>`: Load upscaled `<16-hex-hash>.png` or `.ppm` texture overrides.
- `SETO_GL_HUD=1|0`: Toggle the OpenGL FPS/VPS badge overlay (default on).
- `SETO_GE_EAGER_SYNC=1`: Force synchronous VRAM readback on every `ge_present`.
- `SETO_GL_PRESENT_SHOT_DIR=<dir>` / `SETO_GL_PRESENT_SHOT_EVERY=<N>`: Capture presented backbuffer PPMs including post filters and GL overlays.

## Testing

- Keep `ctest` green and the software renderer default.
- Capture the same scene with software and GL: compare boot/save load, duel
  board, free duel, story/field; use pixel hashes/PSNR and visual review.
- Use the existing `bench-render`/`test_raster` as state oracles; add queue
  unit tests (head/tail wrap, backpressure, payload copy).
- Any GPU path that cannot reproduce a state must fall back to software and
  report the reason; no silent black frames.

## Risks

- GE state exactness: alpha-as-stencil, color masks, blend modes, framebuffer
  aliasing, texture cache/self-texturing.
- CPU/GPU synchronization and readback cost; the guest can read VRAM at any
  time.
- Porting the software rasterizer semantics to shaders without copying GPL
  code.
- OSK text overlay and the on-screen FPS HUD currently live in the GDI path;
  the GL presenter must grow an overlay or fall back for the OSK.
