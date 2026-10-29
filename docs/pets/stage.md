# The fullscreen animation mode ("stage")

One animation, with its own transparency, shown **stretched across a screen or across the whole desktop** instead of as a small pet: falling petals, a ghost
walking the screen, a light overlay, a transparent character as big as the screen. It is a **mode of a pet you already have**, off by default, switched on
and off live. A stage never takes the mouse or the keyboard.

The geometry is `src/overlay/stagegeometry.h`, the placement field `src/core/petstage.h`.

## If a stage hides your desktop

Any of these puts the pet back (nothing else is lost; the pet returns to its old place and size):

- `murasakictl pets rescue` (or the rescue key, Meta+Shift+R): every stage goes back to a pet, and the other rescue effects apply too.
- `murasakictl pets stage <pet id> off`.
- The pet's page in the Murasaki window: "Fullscreen: Off".
- D-Bus: `UpdatePet(<pet id>, '{"stage":{"mode":"off"}}')` or `'{"stage":null}'`.

## The modes

| `stage.mode` | What it does |
|---|---|
| `off` (default) | An ordinary pet. Nothing in this document applies. |
| `output` | One window covers **one output**: the pet's `output` (`*` is the primary). |
| `desktop` | The animation spans **the whole virtual desktop** (Anima's "span all monitors"): one window per output, each showing its part of one picture. |

Both ignore `x`, `y` and `width`, `locked`, `clickThrough`, `hoverHide`, `focusFade` and `smartDodge`: they are **kept** and come back when the mode is `off`.
Item, variant, `fps`, `speed`, `opacity` (0.1 to 1), `paused`, `rotation`, `flipH`, `flipV` and `keepAbove` keep their meaning. **One stage per output**: a
second stage on an output that has one is refused (`UpdatePet`/`AddPet` fail), and `desktop` takes every output.

`stage.fit` says how an animation of another shape than the screen is fitted (default `contain`):

| `fit` | |
|---|---|
| `contain` | scaled uniformly to the largest size that fits inside the target, centred; transparent bands on the other sides. Nothing is cut or distorted. |
| `cover` | scaled uniformly to the smallest size that covers the target, centred; the excess is cut. |
| `stretch` | fills the target exactly; distorts. |

The target is the output, or, for `desktop`, the **bounding box** of all the outputs (outputs can have negative coordinates, gaps and L shapes; the gaps are
transparent). A rotation of 90 or 270 degrees swaps the animation's sides before fitting; other angles fit the turned rectangle's bounding box.

Until the policy server (`murasaki-surface`) has answered once, or is known to be absent, a stage that would hide for a fullscreen window waits hidden (`stageHidden` `starting`): it is never shown over a game for the length of the first read.

`stage.overFullscreen` is `inherit` (follow the setting `stageHideOverFullscreen`, on by default), `show` or `hide`: whether the stage is hidden and paused on
an output while a fullscreen window is on it.

## Seams between outputs

The fitted picture's four edges are rounded to whole logical pixels **once**, and every output shows the part of that rectangle that lies on it, as a
normalised source rectangle computed from the same numbers. Two outputs that touch share their edge to the logical pixel (and the same number for the normalised source rectangles). On outputs of different scales the shared edge is a whole logical pixel, which is more than a device pixel on the output with the larger scale: a seam of up to one device pixel there is possible and unmeasured. Each window samples its part
at its own output's device pixels.

## Limits

- A stage is at most 16384 logical pixels a side and covers at most 8 outputs; an output beyond either is left out and the pet says `stage: too many outputs`.
- The pixels all of a stage's windows draw (device pixels, summed) are capped by the setting `stageMaxPixels` (1000000 to 33177600; default 8294400, a 3840 x
  2160 frame). Above it the picture is drawn smaller and scaled up by the GPU, and the pet says `stage: resolution reduced`. Two stages share the cap.

## Names

A stage's `degraded` list (in `Windows()`) carries these; none is an error.

| Name | Meaning |
|---|---|
| `stage: too many outputs` | more than 8 outputs, or a layout wider or taller than 16384 pixels: the first ones are covered |
| `stage: not fullscreen on this session` | the session has no way to cover an output; the pet is shown at its fitted size on the first output |
| `stage: fullscreen apps unknown` | the session cannot say whether a window is fullscreen; the stage is shown (never silently hidden) |
| `stage: resolution reduced` | the picture is drawn below full resolution to keep within `stageMaxPixels` |
| `stage: below windows unsupported` | `keepAbove` is off but the session (GNOME) can not put a stage below the windows |
| `stage: transcode pending` | a copy of the animation at the needed size is being made; the original plays meanwhile |
| `stage: hidden by a fullscreen window` | informational: a fullscreen window (named) is on the output and the stage waits |
| `stage: layer-shell missing` | the compositor has no layer surfaces and the session is not X11 or GNOME |
| `stage: output already has a stage` | two stages resolve to one output (a `*` and the primary's connector): the later one is not shown |
| `stage: no copy big enough` | copies of the animation exist but none is as big as the stage needs (a mixed-scale desk, a cap that changed): the original plays until the right one is made |
| `stage: needs a compositor` | an X11 session with no compositing manager: a screen-sized window would be an opaque rectangle over the desktop that takes no input, so the pet is shown as a pet until one runs (the overlay looks every two seconds) |
| `stage: output missing` | the stage's output is not there (unplugged, or a name that does not exist): nothing is shown until it is |
| `stage: output too large` | the output is larger than 16384 pixels on a side: it is shown as an ordinary pet |

## Playback

- **One function sizes the copy.** The overlay and the daemon compute what a stage needs with the same function (`stageRequestNeed`, `src/core/stagegeometry.h`) from the same
  inputs: the placement, the outputs (the overlay tells the daemon where each one is, its scale and which is the primary, in `SetOutputScales`), the cap factor and the
  source's size. A stage on `*` is the primary's; a daemon that was not told which is the primary sizes it for the output that needs the most. A test makes the daemon's copy
  and checks that the overlay plays it.
- **Copies.** A stage adds the buckets 960, 1280, 1920, 2560 and 3840 (the smallest that covers the animation as it is drawn at the largest scale of the outputs it covers, in
  device pixels, never above the source's size). The daemon makes them on demand, one ffmpeg at a time under `nice`; the queue goes small first, so the copies a pet needs do
  not wait behind a 4K encode. A stage's encode uses libvpx's `good` deadline at `cpu-used 4` with tile columns and four threads (a pet's keeps its settings), and is stopped
  after 30 minutes (the original plays then). A variant keeps at most two copies plus the one each placement needs, and a copy bigger than any placement needs (a stage that was
  turned off) is removed with the next sync.
- **A copy is used only if it is at least as big as the need** (a smaller one would be stretched over the screen); until then the original plays, and the pet says
  `stage: transcode pending` while a copy is queued or being made, or `stage: no copy big enough` when copies exist and none is.
- **Frame cache.** A stage of one output uses the pet's frame cache when its frames fit the budget (they are never larger than the file); a stage over a 4K screen of many frames
  does not fit and plays through libmpv, as a pet that does not fit does.
- **Desktop mode.** The windows play through libmpv, one player per output, and once a second the overlay compares their positions: any that is more than 150 ms from the reference
  is seeked to it (`spanVideoCorrections`, a looser rule than the wallpapers' 50 ms because a seek is itself a jump). The positions are compared **on the circle of the loop**
  (a window at 0.02 s and one at 3.98 s of a 4 s loop are 0.04 s apart, not 3.96), a window within 0.3 s of the end of the loop (a tenth of the loop for a short one) is left out of the round (a seek there lands on the
  wrap), and a window that was seeked is not seeked again for 3 s, until its position has settled (a rule that judges a position in the middle of a seek chases its own
  corrections). The seek is exact (`absolute+exact`): the copies have sparse keyframes, a fast seek would land seconds away. An exact seek decodes from the last keyframe while the file goes on playing, so it **lands late**: measured with `tools/stage-probe/seeklatency.cpp` on a 4 s, 3840 x 2160 copy (one keyframe, software decoding, no output) a seek took 45 to 191 ms, 123 ms in the median, growing with the distance from the keyframe (about 55 ms per second of video); with keyframes every 128 frames that is up to a quarter of a second, more on a slower computer. Each window measures its own last seek (mpv's playback-restart) and the next one is aimed that far ahead (0.1 s until it has measured one, never more than a second), so the window lands where the others are by then and the rule converges instead of re-seeking every three seconds. The reference is the first output by name among those
  that were playing at the last round, so a window that comes back from a fullscreen window follows the others (a round runs 400 ms after one returns). A hidden or paused
  window has no position and is left out.
- **The cap.** `stageMaxPixels` limits the device pixels all the stages draw together. Above it the daemon sizes the copies, and the overlay the frames, at the same fraction
  (the square root of cap over pixels, at least 0.1) of the full size. What is reported is what really plays: `stageScale` is the longest side of the copy (or the frames) over
  what it would be without the cap, and `stage: resolution reduced` is said only when that is below 1. An **original** that plays as it is (no copy to choose, a light source)
  is not reduced and does not claim to be: the cap cannot make libmpv decode a smaller picture than the file has. A copy rounded up to the next bucket is not a reduction.

## What it costs

Measured with `tools/bench/stage_cost.sh` in a nested KWin of 3840 x 2160 (a virtual compositor that may not use the GPU, so the CPU figures say how the paths compare, not what a real session
spends), one stage of a 3840 x 2160, 40-frame animation with alpha, 30 s per phase after 25 s to settle:

| | overlay CPU (of one core) | overlay RSS | note |
|---|---|---|---|
| the original (an APNG) through libmpv | 151 % | 417 MiB | what plays until a copy is ready |
| the 3840 copy (VP9 with alpha) | 60 % | 582 MiB | |
| the same with the pets held to 15 fps | 54 % | 583 MiB | the copy is 30 fps, so the cap is a resampling filter |

The **encode** of the 3840 copy (libvpx-vp9, `good`, `cpu-used 4`, four threads, tile columns, no look-ahead) took 14 s wall and 35 s of CPU for 40 frames, and **ffmpeg's resident
memory peaked at about 3 GiB** (2.3 GiB with `-lag-in-frames 0`, which a stage's copy uses; the libvpx alpha encoder and the 4K frames, not the number of frames or threads). It runs at
`nice` 10, one at a time, so it costs a computer with 8 GiB or more a slow minute and nothing else; a slower computer is covered by the 30-minute budget (the original plays then).
A long animation takes proportionally longer; none of this is paid unless a stage is made of a heavy source.

## Sessions

- **Layer-shell compositors** (Plasma, Hyprland, sway, ...): a layer surface with all four anchors on the overlay layer (or the bottom layer with keep above off). Nested KWin runs.
- **X11**: only while a **compositing manager** runs (the selection `_NET_WM_CM_S<screen>` has an owner, looked at every two seconds): without one a screen-sized translucent window is an opaque rectangle over the desktop that takes no input, so the pet stays a pet and says `stage: needs a compositor`, and becomes a stage when one starts. The pet's override-redirect window, sized to the output (or, for `desktop`, one per output), with the pet hints (notification type, above, sticky, skip taskbar and pager)
  and an empty input shape; **never** the fullscreen or maximized state, and as an override-redirect window it is in no window manager's client list, so no monitor takes it for an
  application (and a monitor ignores a window that is not of the normal type anyway). Keep above off lowers it under the ordinary windows. Verified by `tst_x11hints`,
  `tst_x11state` and `tools/test-overlay-x11.sh` (the real overlay on a private Xvfb, which has no compositor: first the pet stays a pet with the name; with a fake compositing manager that owns the selection (`murasaki-fakecm`) the window is 1280x720+0+0, has the above state and no fullscreen state, is not in the client
  list, goes under an ordinary window with keep above off and back above with it on, and the pet returns to its size when the stage ends). With no window manager on the Xvfb the
  state of a mapped window can not change, so what keep above off does there is the stacking; the window manager's own handling of the state is unverified.
- **GNOME Shell** (no layer-shell): the same output-sized window as a pet, with ` | stage output` or ` | stage desktop` in its title (`docs/gnome.md`); the extension places it on its
  output on every workspace, above the windows and at the bottom of that layer, so the pets stay in front. Below the windows is a pet's experimental `make_below`; the overlay says
  `stage: below windows unsupported`. The title parsing and the actions are covered by the node tests and `tst_shellwindows`; **what Mutter does with them is unverified without a
  real shell**.

## What is not built yet

A moved or grabbed state of a stage (Anima's "Mouse Tracking" and "Keep fullscreen" flags, which the user asked for in a later phase): the stage leaves room for it, nothing is built.

## Not built

Anima's four "Keep fullscreen static / grabbed / moved / release model" flags and "Mouse Tracking" are saved and shown by Anima Engine and read by nothing in
the shipped app. They are not built here; the stage keeps room for a moved or grabbed state when the user asks for it.

## Verified and unverified

*Verified*
- Unit and property tests: the geometry (fit modes, rotation, L layouts with negative coordinates and gaps, mixed scales, limits, a fixed-seed property test of the seams), the
  placement field and its validation, the settings, the buckets and the need of a stage, the controller with fake windows (layout, hiding, sharing the cap, span sync, outputs
  coming and going), the ctl words, the GUI client's gate.
- `tools/compat/stage_geometry_check.py`: 900 layouts against an independent implementation in exact arithmetic (`murasaki-petprobe --stage`), no disagreement.
- Nested KWin (`tools/test-overlay-nested.sh`): a stage covers the output with the contained picture, real transparency, nothing changes outside it, it takes no input and is not a
  fullscreen window for the compositor; with the policy server running, the real fullscreen backdrop hides it and the end of the policy server shows it again; turning it off gives
  the pet its old place. With two virtual outputs (`MURASAKI_NESTED_OUTPUTS=2`, `overlay_desktop_nested`) a `desktop` stage is one picture across both.

*Unverified*
- A real GNOME Shell (what Mutter does with the extension's actions for a stage: above the windows, at the bottom of that layer, on every workspace), and a window manager's own handling of an X11 stage.
- Hyprland, sway and COSMIC live fullscreen detection (the parsers have their tests, no live session here), real games: borderless "windowed fullscreen" games that the compositor
  reports as maximized are not detected as fullscreen (`pauseApps` and GameMode remain the handles).
- Multi-monitor layouts with different scales on real hardware (the seam between outputs of different scales may be one device pixel), tearing and VRR.
- The CPU and memory of a 4K stage (the budget is a cap on pixels, not a measurement; `tools/bench/measure.py` numbers are to be added at install).

## First session on a real desktop

A transparent animation across the screen; a click goes through it; a fullscreen video hides it and its end brings it back; the rescue shortcut (or `murasakictl pets rescue`)
turns it off; two monitors in `desktop` mode meet at the bezel.
