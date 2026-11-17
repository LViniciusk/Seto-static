# GE GPU backend — architecture and plan

Status: design; M0 (OpenGL presenter) implemented on `feat/gl-present-m0`.
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
- **M1**: GPU thread + queue/context plumbing; `psp_render_gl` frontend that
  runs software + enqueues snapshots; GPU draws untextured colored primitives
  to an FBO; window presents the FBO. Test: duel board primitives vs software.
- **M2**: textured draws: formats/CLUT/swizzle, combine, filtering. Test:
  cards/UI/3D textures vs software captures.
- **M3**: framebuffer manager: FBO per target, readbacks, framebuffer textures,
  block transfers, effects; per-draw fallback.
- **M4**: physical scale 1x/2x/4x/10x; 2D smart filtering; resize; present.
- **M5**: MSAA, HD texture replacement, post filters, performance.

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
