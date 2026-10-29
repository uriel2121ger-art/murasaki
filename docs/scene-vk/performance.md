# Performance of the scene engine

Measured on the development machine's integrated GPU (Radeon Graphics, 2 CU, Raphael) at 2560x1440 and 30 fps. GPU busy
is the `drm-engine-gfx` time of the engine process from `/proc/<pid>/fdinfo` over 3 s, one item at a time.

## Profiling

```
MURASAKI_SCENE_VK_PROFILE=1 murasaki-scene-vk ... 2> engine.log      # or through bridge-dump --delay 20
python3 src/scene-vk/tools/profile.py engine.log --top 15
```

Each step of the frame is bracketed by timestamp queries (`render/gpuprofiler.h`): a step's time includes the barriers
before it. Without the variable only the total GPU time of the frame is measured (two timestamps), which the adaptive frame
rate uses.

## Where the time goes

Before any change (one item at a time, 2560x1440, 30 fps):

| Item | GPU ms/frame | Top costs |
|---|---|---|
| 1815527157 "5k universe" | 24.3 | `godrays_cast` 6.2 (30 samples per pixel at half size), waterripple 3.2, pulse 2.8, present 2.0 |
| 1487027709 | 37.3 | particles 7.1 (160 sprites of a big soft system, fill bound), godrays_cast 3.6, shine_cast 3.3, waterripple 3.2 |
| 2655078184 | 20.7 | many small layers, particles 2.0, present 2.0 |
| 2182253741 | 1.4 | present only (the scene is still) |

The final copy to the export image ("present") costs about 2 ms at 2560x1440 on this GPU whatever the render scale.

## What was done

| Change | Effect |
|---|---|
| Scene target sized to the visible output pixels (`viewmapping.h`) | 1815527157: 94% -> 76% (it was drawn at 2x the output) |
| No GPU work for a still scene, one keep-alive frame a second | 2182253741: 8% -> 0.2% |
| Layers that do not depend on time keep their buffers | 2655078184: 64% -> 55%, 1815527157: 76% -> 72% |
| `godrays` and `shine` chains at a quarter size (SSIM 0.9999) | 1815527157: 72% -> 42% |
| Big soft particle systems at half size | `particles 19` of 1487027709: 5.6 -> 2.1 ms |
| Adaptive resolution (budget 25% of the frame interval, never below half the setting) | 1815527157 20%, 2655078184 19% |
| Adaptive frame rate (also steps down at 35% when the resolution is at its floor) | 1487027709 25% at 20 fps |

## Results (share of the iGPU, 2560x1440, 30 fps requested)

| Item | Before | After (render quality Automatic) |
|---|---|---|
| 1815527157 | 94.5% | 19.8% |
| 2655078184 | 85.5% | 19.3% |
| 1487027709 | 77.2% | 24.9% (20 fps) |
| 2182253741 | 8.0% | 0.2% |

The numbers move by a few points from run to run (the particle counts and the GPU clock vary).

## Video textures without a copy

The video plugin keeps the VAAPI surfaces it decodes into and hands them to the engine as DRM PRIME dma-bufs (`vaExportSurfaceHandle`,
one buffer holding both planes of NV12 or P010). The engine imports each buffer once as a Vulkan image with its DRM format modifier
(`VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`), keeps up to 48 imports keyed by the buffer, and draws the picture
into the video's RGBA texture with one triangle through a `VkSamplerYcbcrConversion` (the hardware applies the matrix, the range and
the chroma siting). Nothing is read back, converted with swscale or uploaded. If any step cannot be done the engine says why once and the
same pictures come the old way: the surfaces converted on the spot (`MURASAKI_SCENE_VK_VIDEO=copy` forces it), then CPU decoding
(`MURASAKI_SCENE_VK_VIDEO=cpu`). VAAPI refuses pictures smaller than 64 pixels in either direction, which the tests' first clip is.

Measured at 2560x1440, 30 fps (`tools/bench/bridge_bench.py`, three video scenes, `copy` against the default):

| Scene | CPU of the engine, copy | zero-copy | GPU | RSS |
|---|---:|---:|---:|---:|
| 1080p clip | 105% | 8.0% | 20.9% to 12.7% | 179 to 156 MB |
| 1080p recording | 89.7% | 3.9% | 16.3% to 15.7% | 248 to 219 MB |
| 720p clip with effects | 46.5% | 14.8% | 53.0% to 52.2% | 159 to 138 MB |

## The present pass writes every pixel

The last pass copies the scene into the output buffer. When the picture's rectangle reaches every edge of the output it no longer clears
the buffer first (nor loads it): `DONT_CARE` instead of `CLEAR`. On the iGPU this halves the pass, 1.6 ms to 0.8 ms at 2560x1440 on a
light scene, 1.4 to 0.8 ms on a heavy one; `MURASAKI_SCENE_VK_PRESENT_CLEAR=1` brings the old behaviour back for comparison. Replacing
the pass by a plain image copy was not done: the pass forces alpha to 1, and nothing guarantees the scene's target is opaque.

## Compressed textures (BC7): not done

The question was whether transcoding large RGBA textures to BC7 once, in the decoded-texture cache, would pay. An upper bound
on what any texture compression can win is a run where every texture is replaced by its smallest mip (a few texels, always in
cache) and everything else is drawn as usual, same items, 2560x1440 at 30 fps:

| Item | GPU busy, real textures | GPU busy, tiny textures | Gain |
|---|---|---|---|
| 2655078184 | 52.1% | 49.7% | 4.6% |
| 1815527157 | 45.4% | 44.5% | 2.0% |

(The numbers are from a busy machine, a build was running; the point is the ratio.) BC7 is 4 times smaller than RGBA8, not
free, so the real gain would be a fraction of that: well under the 10% that would justify a vendored encoder
(bc7enc_rdo), the encoding time on first load and the quality loss. The cost of these scenes is in the render targets
(effect passes read and write full frames), not in the pictures they sample.

## Soak test

`tools/soak.py` starts the engine on one item after another against a bare bridge consumer (it releases every frame), at nice 19,
and samples what can leak: resident memory, GPU memory (`drm-memory-vram` and `drm-memory-gtt` of the process's DRM clients in
`/proc/PID/fdinfo`), open descriptors, threads, CPU, frames delivered and the sizes of the texture and shader cache folders. Its
summary names every run whose memory, GPU memory or descriptors grew (judged past the first quarter, which is loading) and every
engine that exited on its own.

```
python3 tools/soak.py --engine build/ci/src/scene-vk/murasaki-scene-vk --hours 8 --switch-every 20 --sample 10 --scenes 3
python3 tools/test_soak.py     # the analysis, on canned data
```

A two minute smoke run on three light items (6 switches) showed 73 MB resident, 37 MB of VRAM, 15 descriptors and no growth.

### Soak of the zero-copy build (feat/zero-copy, 3.2 h, 1280x720, nested session)

22 items never used before in a soak (20 scenes the Vulkan engine accepts, picked with a seed, one web, one video), switched every 2 minutes,
96 switches, sampled every 10 s: no crash, no zombie, nothing left behind but QtWebEngine's zygotes, which go with the surface. Growth per
hour after the first hour: daemon +2.5 MB, surface +5 MB, engines +12.6 MB, GPU memory +43 MB (steps at the web and video items, not a
slope: the same scene reads 79, 120, 119, 164, 123 MB on five visits), descriptors +3, threads +1.4. The eight dma-buf descriptors left
after the last switch are the web wallpaper's (known issues).
