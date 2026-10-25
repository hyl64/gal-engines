# Performance

Measured on an Apple Silicon Mac with CLANNAD Full Voice (Chinese
translation, 231 scenarios, 4.0 GB of assets).

## Where the time goes

| Situation | CPU | Memory |
| --- | --- | --- |
| Waiting for a click, static scene | **0.4 %** (1.14 s of CPU over 4 min 14 s) | 8.7 MB |
| Title screen animation | 11–14 % | 25–44 MB |

A 2-second `sample` of the animated title, 1014 samples:

    759  SDL_SYS_DelayNS -> nanosleep          the 10 ms frame slice, asleep
    171  SDLGraphicsSystem::EndFrame           drawing and presenting
    120  |- SDL_GL_SwapBuffers                 of which present
     64  |    gldPresentFramebufferData       ... through the GL-on-Metal bridge
     64  |       SLSFlushSurfaceWithOptions   ... and the window server
     71  instruction execution, audio, text

So three quarters of the time the engine is deliberately asleep; of the
quarter it is awake, about half is the cost of putting a finished frame on
the screen.

## The render path

rlvm asks SDL 1.2 for an OpenGL window and draws with the fixed-function
pipeline:

    rlvm -> OpenGL 2.1 -> AppleMetalOpenGLRenderer -> Metal -> AGX GPU

It is on the GPU, but through a translator Apple deprecated years ago. The
proof is in the running process: `AppleMetalOpenGLRenderer.bundle` and
`AGXMetal13_3.bundle/...metallib` are both mapped.

`sdl12-compat` sits in front of that (and in front of `sdl2-compat`, which
sits in front of SDL3), so a frame passes through three compatibility
layers before the GPU sees it.

## Startup

| Phase | Time |
| --- | --- |
| Launch to SEEN.TXT open | 1.0 s (from a local folder) / 1.4-2.0 s (from ~/Documents) |
| Launch to title screen | ~20 s |
| Parsing all 231 scenarios (offline) | 3.9 s |

The title figure is the game's own opening movie and logos, not engine
overhead. Cold-start measurements vary by several seconds with page cache
state, so treat them as approximate.

## Keep game data out of cloud-synced folders

A launch from `~/Documents` was caught with every sample blocked in an
`open()` syscall, inside `CheckBadEngine`'s directory walk: the file
provider daemon that syncs Desktop and Documents was holding the
directory. The same launch from `/Users/Shared` opened SEEN.TXT in 1.0 s.

Games are multi-gigabyte read-mostly trees that every filesystem scan now
has to negotiate with a sync service. Keep them on a local path.

## Levers, in order of value

1. **Skip the present when the frame did not change.** `EndFrame` always
   flushes and swaps. `screen_needs_refresh()` already gates most of this,
   but a static scene with a blinking cursor still presents.
2. **Batch the draw calls.** rlvm issues `glBegin`/`glVertex2f` per quad.
   A VN draws few quads, but each one costs a round trip through the
   GL-to-Metal bridge.
3. **Leave OpenGL.** Compositing into an `SDL_Surface` and presenting one
   texture through SDL3's renderer would drop two compatibility layers on
   a workload this small.

None of these is urgent: the engine spends most of its life asleep, and
idle cost is already 0.4 % CPU.
