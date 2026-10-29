# Pointer facts

What a Wayland client can know and do about the pointer, measured per compositor. It decides the design of the pet
interactions (hide under cursor, focus fade, smart dodge, click-through, hit-testing, drag, keep above off).

The numbers come from `tools/test-overlay-pointer-nested.sh` (ctest `overlay_pointer_nested`, skipped unless
`MURASAKI_NESTED_TESTS=1`): a test layer surface (`tools/overlay-pointer-probe/`, not the overlay) in a private nested,
headless compositor, with pointer motion injected by a client of the compositor. The script writes one `FACT` line per
measurement to `facts-<compositor>.txt` in its output directory. A cell that was not run says `unverified`; nothing marked
so is promised by the design.

Machine of the run: Ryzen 9 9800X3D, Plasma's `kwin_wayland --virtual` (1280x720), Qt 6, LayerShellQt 6. Since 2026-10-25
also sway 1.11 (`WLR_BACKENDS=headless`) and Hyprland 0.53.3 (as a window of a `kwin_wayland --virtual` host, like
`tools/test-wlroots-nested.sh`), both 1280x720, one run each. Their facts are recorded, not pinned: only KWin's fail the test.

## Status at the close of phase 2 (2026-10-21)

Everything below was measured in task 0 on nested KWin and has been used since: the decisions at the end of this document are the design that shipped. What the later
tasks added or checked, all on nested KWin with the real overlay and the fake-input client of `tools/overlay-pointer-probe/` (one connection for the whole run, F-notes above):

- The KWin script of the pointer source (`src/overlay/kwin-pet-cursor.js`), loaded into the nested KWin: one report at load with the resting position, 16 reports for 100
  one-pixel moves in 747 ms (the throttle is 34 ms), the last position exact, none while idle, none after `unloadScript`, and none to a new owner of the overlay's bus name after the
  first owner was killed (the script asks the bus every 10 s, 400 ms in the test, and stops after two answers of "no owner").
- The overlay's behaviours, with the real overlay and a static pet: click-through lets a drag through; a press on a transparent pixel does not drag, one on the drawing moves the pet by
  exactly the pointer's movement and the daemon hears it; a locked pet stays; hide under the cursor takes the pet out of the screenshot and brings it back; focus fade fades and returns;
  smart dodge from 10 px away moves the pet 85 px (`clamp(256 // 3, 54, 140)`) and the daemon hears it. Rotated (90 and 37 degrees) and flipped pets: the drawing and the input region
  agree (with the mask not turned, the same script fails 7 of its checks).
- Still **unverified**: every compositor but KWin, sway and Hyprland (labwc, river, wayfire, niri, COSMIC, Mutter); sway and Hyprland outside a nested headless
  session; a real Plasma session (the real input stack, a real
  `kglobalacceld`, multi-monitor layouts); the local level against a real compositor (only the unit tests of the state machine cover its rules); X11 beyond the input-shape test
  on Xvfb. `docs/pets/behaviours.md` has the checklist for a first real session and the per-compositor table.

## U5: moving the pointer in a nested compositor

| Compositor | Result |
|---|---|
| KWin (nested, `--virtual`) | **Works.** A client of `org_kde_kwin_fake_input` (`authenticate`, `pointer_motion_absolute`, `button`) moves the pointer and presses buttons; the probe sees `enter 110 105` for an injected move to output (310, 255) over a surface at (200, 150). The compositor must run with `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1`, which `test-overlay-nested.sh` already sets, so the desktop-file authorisation was not needed and was not tried. libei and KWin's test options were not tried |
| sway | **Works** with a `zwlr_virtual_pointer_v1` client (`motion_absolute` over the 1280x720 extent, `button`, `frame`): `enter 110 105`. `swaymsg seat - cursor set/press/release` does **not**: a headless sway has no pointer device, its seat has no pointer capability and the client never gets a `wl_pointer`. The virtual pointer is that device, so it must exist before the probe starts |
| Hyprland | **Works** with the same client, buttons included (`enter 110 105`). `hyprctl dispatch movecursor` was dropped with it (it had no button) |
| labwc, river, wayfire, niri, COSMIC, Mutter | `unverified`; labwc, river and wayfire offer the same protocol, so the client would very likely do, but it has not been run |

Pitfall, worth knowing for any test that injects: **one fake-input connection for the whole test.** Each new connection adds an
input device, KWin announces the seat again and Qt recreates its `wl_pointer` (`wl_pointer.release`, `get_pointer`), which drops
the pointer focus and with it every pending `leave`. The first version of the script started an injector per move and saw
"no leave, ever" on every region change; the cause was the injector, not KWin. The client of the script is one process fed by
a FIFO. The client refuses any `WAYLAND_DISPLAY` that is not `wayland-mon.*` (KWin) or a bare name under a private
`/tmp/mon.*` runtime directory (sway and Hyprland pick `wayland-N` themselves), so it can not reach the user's session.

## U1: input region changes with the pointer inside

The probe changes its input region (committed with the next frame) with the pointer parked on it, then moves the pointer by
one pixel. Latency is measured from the command; "no event" means 0.8 s of silence.

| Change (pointer does not move) | KWin without motion | KWin after one pixel of motion | sway and Hyprland | labwc, Mutter |
|---|---|---|---|---|
| pointer inside, region now excludes it (`rect 0 0 20 20`) | **no event** | `leave` | the same as KWin | unverified |
| pointer outside, region grows to contain it (`disc`) | **no event** | `enter` (local 100, 100) | the same | unverified |
| pointer inside, region emptied (`Qt::WindowTransparentForInput`) | **no event** | `leave` | the same | unverified |
| pointer outside, region back to infinite (`setMask` of an empty region) | **no event** | `enter` | the same | unverified |
| pointer inside, region now excludes it, then a **click** with no motion | `leave`; **the press is not delivered** (it goes to what is below) | | **`press`, `release` delivered to the probe**, no `leave`: the stale focus keeps the click | unverified |
| region grows back over the pointer, then a click with no motion | `enter`, `press`, `release` in the same millisecond: the click **is** delivered | | `press`, `release` (the focus never left) | unverified |
| pointer inside, surface **unmapped**, then a click with no motion | **no event**: the click does not reach it | | **no event**, on both | unverified |
| mapped again under the still pointer, then a click with no motion | `press`, `release` | | sway: `enter` only; Hyprland: nothing (until motion) | unverified |

So on KWin **a region change is not a focus event**: the compositor re-picks the surface under the pointer on the next
motion or the next button press, not on commit. Consequences:

- The client must never wait for `enter` or `leave` as the answer to its own region change; both arrive with the next motion.
- `hide under cursor` at the local level (3.2): the pet fades on `enter`, keeps its region (so `leave` still arrives with the
  next motion out of the silhouette). A press while it is hidden reaches the pet (the first click is lost, the named
  degrade); the pet then empties its region; **the next click, with no motion, goes to the window below** (the press
  re-picks, `leave` is delivered, the press is not), which is exactly the 1.2 s release the plan describes; after 1.2 s the
  region is restored and the next motion gives `enter` again. The release works on KWin as designed.
- **On sway and Hyprland a press does not re-pick the surface**: only motion does. With the region emptied and the pointer
  still, the next click goes to the pet again, not to the window below. An unmapped surface keeps the focus nowhere: so
  since 2026-10-25 the release **unmaps** the window once the button of the swallowed click is up, and maps it again when
  the 1.2 s end (`PetInteraction::released`). The same on every compositor; a focus-faded pet (opacity 0.18) is not drawn for
  those 1.2 s. What a compositor sends on the map again does not matter: the next motion brings `enter`, as before.
- The global level (KWin script, `workspace.cursorPos`) does not depend on any of this.

## U2: cost of a region replaced at 30 Hz

Probe replaces its region every 33 ms (alternating two parities of a checkerboard, so Qt can not merge rectangles: the worst
case for a given count) and repaints; CPU is `utime + stime` from `/proc` over 20 s, in percent of one core, after 2 s of
settling. The machine was in use, so each case ran twice, in opposite orders, and the run was repeated; the ranges are the
spread over the runs.

| Case | KWin (compositor) | probe (client) |
|---|---|---|
| idle, static region | 0.0 to 0.1 % | 0.0 % |
| 1 rectangle replaced by another, 30 Hz | 1.1 to 4.2 % | 1.0 to 4.1 % |
| 128 rectangles, 30 Hz | 2.1 to 4.0 % | 1.7 to 3.1 % |
| 512 rectangles, 30 Hz | **4.6 to 21.6 %** (typically 8 to 14 %) | 1.3 to 5.5 % |

The client side is small whatever the count (the repaint dominates). In the compositor 128 rectangles are within the noise of one
rectangle; 512 rectangles cost about 6 to 10 points of a core more, per pet, per 30 Hz update. Per-frame updates are cheap up
to 128 rectangles and not free at 512.

sway (one run, idle machine): 0.2 % for 1 rectangle, 0.4 % for 128, 1.3 to 1.5 % for 512; the client 0.8 to 1.2 %. Hyprland: the
compositor 0.0 % in every case and the client 0.9 to 3.7 %; as a window of the virtual host it may not repaint for itself, so its
0.0 is not a cost measured. Others: unverified.

## U3: `QWindow::setMask` with many rectangles

The probe sets a checkerboard of `cols x rows` cells (so `cols * rows / 2` rectangles) and the script moves the pointer to the
centre of randomly chosen cells, on and off the checkerboard, and records which positions the client received (as `enter`
or `move`).

| Rectangles committed (client's `QRegion::rectCount`) | Sampled points on a rectangle | Reached the client | Sampled points off the rectangles | Reached the client |
|---|---|---|---|---|
| 512 (32 x 32 cells of 6 px) | 205 | 205 (100 %) | 195 | 0 |
| 2048 (64 x 64 cells of 3 px) | 286 | 286 (100 %) | 314 | 0 |

Qt hands the whole region to KWin and KWin applies all of it: no merging that loses cells, no upper bound seen up to 2048.
sway and Hyprland give the same table (205 of 205 and 286 of 286 on, 0 off). Others: unverified.

## U4: `setLayer` on a mapped surface

Two overlapping probes: A (orange, moved around) and B (blue, on `top`). A pixel inside both discs is read from a screenshot
after each change of A's layer.

| A's layer | Pixel shows |
|---|---|
| overlay (start) | A (orange) |
| bottom | B (blue) |
| overlay | A |
| top (same layer as B, A was mapped first) | B |
| overlay | A |

The stacking follows the request each time, within 0.7 s. Flicker: A repaints at 60 Hz; the frame log (paint events) has
240 paints with a median gap of 17 ms and a maximum of 18 to 19 ms with no layer change, and 154 paints with the same median and
maximum gap (18 ms) while `setLayer` is called 20 times at 10 Hz. No unmap (`expose exposed=0`), no resize or configure event
during the changes. The compositor-side frame is not observable from here (a screenshot stalls the compositor), so "no flicker" means:
nothing on the client's side moved and the cadence did not change. sway and Hyprland: `setLayer` unmaps nothing (0
`expose exposed=0`); the stacking is unverified there (no screenshot tool in the script). Others: unverified.

## 3.3: grow on press (the drag layout)

The small surface (200 x 200 at (200, 150), anchored top-left with margins) grows on the `press` event to the whole output
(anchored on all four edges, margin 0, `resize(output)`), keeps the input region (re-applied in the new coordinates), and shrinks
back on the release.

- The press, the motion with the button down and the release are all delivered to the same surface: no `leave` or `enter`
  around the grow (implicit grab). The release arrives in output coordinates (`release 700 500`), then the surface shrinks and
  the `leave` for the pointer now outside it follows.
- **Coordinates switch frames a few milliseconds after the press.** With 40 one-pixel moves 3 ms apart starting right after the press,
  1 to 2 events (1 and 4 ms after the press) arrived in the old small-surface frame (local to (200, 150)) and the other 38 to 39
  in the output frame (the first at 4 to 6 ms). The client itself painted the grown buffer 1 ms after the press. A drag that
  adds up deltas between consecutive events would jump by the origin shift (200, 150) on the switch.
- Compositor-side (a KWin script logging `frameGeometryChanged` with the wall clock): the window goes from (200,150) 200x200
  to 1280x720 at (200,150) and to (0,0) in the same millisecond, stays so for the press (about 0.6 s here) and goes back to 200x200
  at (0,0) and (200,150) in the same millisecond. There is no separate step that outlasts a millisecond.
- What is drawn: the orange centroid in screenshots before the press, with the button down (surface grown) and after the release is
  the same to 0.1 px (298.6, 248.6; 7276 samples each). A flash lasting a single frame between the steps would not show in a
  screenshot: **not observed, not excluded**.
- **sway keeps the old frame for the whole grab.** The client paints 1280x720 at once, but every event with the button down is
  local to the small surface at (200, 150): the slow drag to output (500, 400) and (700, 500) arrives as `move 300 250` and
  `move 500 350`, the release as `release 500 350`; in the fast drag all 40 moves are in the old frame. Since 2026-10-25 the
  drag reads every event in both frames and takes the one nearer the last pointer position, switching to the grown frame
  once and for all (F12 in `petinteraction.h`): no compositor is named, KWin, Hyprland and sway all follow.
- **Hyprland animates the growth** (with its default animations; the probe's runs had them on too, but its window grew by
  200 x 150 px, too little to show): measured with the real overlay, a pet at (900, 500) whose window grows to the output
  slides there, so that a drag's motion comes in frames whose origin was (287, 159) and then (151, 84), on the line from
  (900, 500) to (0, 0), neither the old frame nor the grown one. Read in either, the drag went the wrong way. Since
  2026-10-25 the drag also hears relative motion (zwp_relative_pointer_v1, which does not depend on any frame) and uses it
  while the local step between two events does not match it (F13 in `petinteraction.h`); when they match the frame held
  still and F12 reads it. A drag that hears no relative motion (no protocol, an absolute device such as a tablet) is F12 alone,
  and would still go wrong on an animated growth. Note: Hyprland's parser ignores a one-line block (`animations { enabled =
  false }` leaves it on); the `category:option = value` form works.
- **Hyprland switches like KWin** in the fast drag (3 of 40 moves in the old frame, at 0 and 7 ms; the first in the output frame
  at 12 ms, later than KWin's 4 to 6 ms). In the slow drag no motion with the button down reached the probe and the release came
  at the press position (`release 120 110`). One run: unexplained, not reproduced.

## Decisions that follow (amended in the plan, section 3)

1. The local-level hide with the 1.2 s release stays as designed (U1): it works on KWin and needs no more than a click that
   arrives without motion. The state machine treats `enter`/`leave` as consequences of motion and presses, never of its own region
   changes.
2. Input regions made from masks: **at most 128 rectangles for a region updated per frame** (U2); up to 512 for a region that is set
   once (the `silhouette` mode) or changes rarely; Qt and KWin accept 2048 (U3), so the cap is a cost choice, not a limit.
3. The drag layout is kept (3.3), with the rule that the drag is computed from the **absolute** pointer position (origin of the
   grown surface plus local coordinates, with the press position taken at the press) and that motion events in the first 16 ms after
   the press are ignored (they may be in the old frame). Skipping them loses nothing because the offset is absolute.
4. `keepAbove: false` through `setLayer(bottom)` on a mapped surface is fine on KWin (U4).
5. Nested checks of pointer behaviour on KWin can inject motion (U5), so `tst_*` plus `overlay_nested` extensions of tasks 4 to 8
   use the same injector (`tools/overlay-pointer-probe/`, one connection) instead of the `Windows()` flags. Since 2026-10-25 the
   injector also drives sway and Hyprland (`zwlr_virtual_pointer_v1`).
6. (2026-10-25) The drag picks its frame by continuity (3.3) and the release unmaps the window (U1), for every compositor.
   Checked: the unit tests of the state machine, the probe on KWin, sway and Hyprland, and the real overlay on nested KWin
   (`overlay_nested`).
7. (2026-10-25, later) The real overlay runs on sway and Hyprland (`overlay_nested_sway`, `overlay_nested_hyprland`: the same
   script and checks). What it found, fixed for every compositor: a press on a faded pet arrives at the global level too (sway
   and Hyprland give it to the surface the pointer last moved onto), so it starts the same release; the window takes that press
   although its region is empty; the drag follows an animated growth by the relative motion (3.3, F13). What a local pet can not
   do it says (`degraded`): smart dodge only when touched, no look or follow; on sway and Hyprland without a portal there is no
   global shortcut.
