# Pet behaviours

What a pet does with the pointer, and what it tells you when it can not do it exactly. The numbers behind the choices are in
[pointer-facts.md](pointer-facts.md); the state machine is `src/overlay/petinteraction.h`, the masks `src/overlay/alphamask.h`.

## Pointer levels

A pet works at one of two levels, chosen per session and reported in `Windows()` as `pointer`:

- **global**: the overlay knows where the cursor is whether or not it is over a pet (a KWin script, Hyprland's IPC, X11). Hide under
  cursor, focus fade and smart dodge are exact, as Anima's.
- **local**: only the pointer events on the pet's own surface are known (`enter`, `leave`, `motion`). Every other compositor starts
  here. The behaviours below say what they do instead.

### Pointer sources

The level comes from the session (`src/overlay/pointercapability.h`), and only while some pet has hide, fade or dodge on does anything run:

| Session | Source | How |
|---|---|---|
| Plasma on Wayland (KWin on the bus) | global, `KWinPointerSource` | a script loaded into KWin over `org.kde.kwin.Scripting` reports `workspace.cursorPos` to the overlay's D-Bus method `ReportPointer`, at most every 34 ms; unloaded when no pet needs it; the call is accepted only from the owner of `org.kde.KWin` |
| Hyprland (its sockets found) | global, `HyprlandPointerSource` | `j/cursorpos` on the request socket, polled at 30 Hz, one request at a time |
| any X11 session | global, `PollingPointerSource` | `QCursor::pos()` every 32 ms (never the KWin script on X11) |
| everything else (sway, labwc, river, wayfire, niri, COSMIC, GNOME, a Plasma without KWin on the bus) | local, `LocalPointerSource` | only the pet's own `enter`, `leave` and motion |

The KWin script never outlives the overlay: the overlay removes a stale one at start, removes its own on exit, and the script stops reporting by itself when the
overlay's bus name has no owner (it checks every 10 s). The script has one fixed name, so the overlay must be single-instance (it is: it owns a well-known bus name).
Hyprland's `/tmp/hypr` fallback is used only for sockets owned by the current user, and a Hyprland reload shorter than 3 s does not end the global level.

A global source that cannot start (KWin refuses the script, Hyprland's socket stops answering) says `unavailable` and the session falls back to the local level
(`pointer: local`). `MURASAKI_POINTER=local` forces the local level; no value can force a global one into a session that has none. Positions are in the
compositor's global logical coordinates; `mapCursor()` (src/surface/cursorrouter.h) turns one into an output and a position on it.

### What is done where

The window of a pet that is not click-through has an input region made from the hit mask of the frame on screen (at most 128 rectangles; at most 512 for
the union of all frames), updated at most 30 times a second; it follows the pet when it moves, and it is cut to the pet's rectangle only by the arrange mode.
A pet that plays through libmpv has no frame index to follow: its region is the union (`hit mask: silhouette`) from a one-time pass over the file it plays,
cached in `$XDG_CACHE_HOME/murasaki/pets/`. The masks of all the pets share 8 MiB (`MURASAKI_MASK_CACHE_MIB`); each pet keeps its union whatever the share.
Dragging a pet (not click-through, not locked) makes its window cover the output for the length of the press; the window never moves under the pointer. On X11
only the input shape is cut (the picture is not clipped). `MURASAKI_POINTER=local` forces the local level.

## Behaviours

| Behaviour | Global pointer | Local pointer |
|---|---|---|
| hide under cursor | opacity 0 while the cursor is over an opaque pixel, click-through while hidden | fades on `enter` and keeps the union silhouette as its input region, so `leave` still arrives; the first click on a hidden pet is lost, then the region is empty for 1.2 s and clicks pass to the window below |
| focus fade | the same with opacity 0.18; exclusive with hide under cursor | the same as hide under cursor |
| smart dodge | the cursor within 26 px of the pet's rectangle: a step of `clamp(max(w, h) // 3, 54, 140)` px away from it, at most every 0.22 s, kept inside the output | the pet steps when touched (`enter` on an opaque pixel) |
| drag | a pet that is not click-through and not locked, pressed on an opaque pixel, grows to its output for the press and follows the pointer | the same |
| lock, click-through | independent of the pointer level | the same |
| stage (fullscreen mode, [stage.md](stage.md)) | none of the above applies: a stage takes no pointer, is not dragged and is not arranged; hide under cursor, focus fade, smart dodge, lock and click-through stay stored and come back when the mode is off | the same |
| states ([states.md](states.md)) | listen to the drag, the release and the dodge steps the rows above already produce and change nothing in them; the picture follows (idle, grabbed, moving, released) | the same; a pet that can not be grabbed (click-through, locked) only shows `idle`, `moving` for a dodge step and the clicks |
| mouse tracking (`look`, `follow`; [states.md](states.md)) | needs the global pointer; `follow` excludes smart dodge, hide under cursor and focus fade (not built yet: tasks 6 and 7 of the plan) | off for that pet, with `tracking: needs the global pointer` |

On a **local** pet a click-through pet with hide, fade or dodge on has to hold input to hear the pointer, so it takes clicks while
visible. That is the degrade named below, not a bug.

A faded local pet stays hidden while the pointer is inside the **union silhouette** of all its frames, even over a pixel that is
transparent in the frame being shown: the region is the union precisely so that it does not change while nobody sees the pet.

KWin does not report a pet's own region change: it re-picks the surface under the pointer on the next motion or press. The pet therefore
never waits for `enter` or `leave` as the answer to something it did itself (pointer-facts.md, U1).

## Names

The names a pet's `degraded` list can carry (`Windows()` of the overlay). One table in `src/overlay/petbehaviours.h`; a test keeps
this document equal to it.

| Name | Meaning |
|---|---|
| `hide under cursor: clicks swallowed` | local pointer level: the first click on a faded pet is lost, and a click-through pet with hide or fade on takes clicks while visible |
| `smart dodge: only when touched` | local pointer level: the pet steps away when the pointer touches it, not when it comes near |
| `hit mask: coarse` | the masks were made on a coarser grid (64 or 32 cells) to fit the budget: the hit area is a little larger than the picture |
| `hit mask: silhouette` | only the union of all frames is kept: the hit area does not follow the animation |
| `hit mask: none` | no mask yet (or the pet plays through the video path and the mask pass has not run): the whole rectangle takes input, as arrange mode does |
| `keep above off: always below windows` | a layer-shell compositor can only put the pet on the bottom layer: below every window, above the wallpaper |
| `keep above off: unsupported` | GNOME until its extension marks the window: the pet stays above |
| `pointer: local` | informational, not a gap: the session's pointer level is local. Do not count it as a degradation |

## Masks

- The mask of a frame is a bit grid over the pet's picture, cell = `ceil(longest side / 128)` pixels (at most 128 x 128 bits, 2 KiB); a cell
  is set when any pixel of it has alpha >= 16/255, so the hit area is never smaller than what is drawn (apart from pixels fainter than that threshold).
- All masks together are held to 8 MiB (each mask counts its bits plus a little overhead). When a pet's would not fit (20000 frames x 2 KiB) the grid is
  lowered (64, then 32 cells) and, if that does not fit either, only the union is kept. The union (about 2 KiB) is always kept, so a pet never has
  less than `hit mask: silhouette`; `hit mask: none` is only the time before any mask exists.
- The input region made from a mask is run-length rectangles merged vertically: **at most 128 rectangles** for a region replaced per frame
  (the cost on KWin is then within the noise of one rectangle at 30 Hz; 512 cost 8 to 14 % of a core per pet), **at most 512** for a region set once
  or rarely. Over the limit the grid is lowered and, as a last resort, the bounding rectangle is used (a superset).
- Rotation and flip turn the mask, they do not decode again: every set cell becomes the box of its turned corners, so the turned mask still covers
  every drawn pixel (a cell of margin at an arbitrary angle, exact at quarter turns when the cells tile the picture).
- Masks of pets that play from the frame cache are made while the frames are decoded; pets on the video path get a one-time mask pass (streamed, at the file's own size), cached on disk (a small versioned `.mask` file whose
  parser trusts nothing it reads).

## Keep above off

`keepAbove: false` (default `true`) asks for a pet that is not above the windows.

- **Layer-shell** (Plasma, wlroots, ...): the surface moves from the overlay layer to the **bottom** layer with `LayerShellQt::Window::setLayer`, at run
  time and without unmapping it on KWin (pointer-facts.md, U4). That is as close as a layer surface gets: above the wallpaper and the desktop's icons, below
  every window, even when clicked. Degrade: `keep above off: always below windows`.
- **X11**: the pet is an override-redirect window, which the window manager does not stack, so dropping the hints is not enough. It loses the
  `_NET_WM_STATE_ABOVE` state, the stay-on-top hint and the raise timer, **and is put in the X stack just above the topmost desktop-type window** of the manager's
  `_NET_CLIENT_LIST_STACKING` (the wallpaper), i.e. below every ordinary window; windows raised or mapped later stay above it (tested on Xvfb). Without such a window or
  without the list (no manager, or one that does not keep it) the pet goes to the very bottom of the stack, where a desktop window of such a manager could cover it.
  An ordinary window mapped before the pet was lowered but never listed by the manager (an unmanaged one) is not accounted for. No degrade name.
- **GNOME**: the window title says `| below` and the extension takes it out of the above layer and pushes it down (docs/gnome.md); not checked on a real shell,
  so the degrade `keep above off: unsupported` stays.

## Rotation and flip

`rotation` (degrees, clockwise, 0 up to 360, two decimals) and `flipH`/`flipV` are transforms of the scene graph: `PetItem.qml` puts the flip on the picture
(a `Scale` of the `art` item) and the rotation on the item that holds it, about its centre, so the picture is mirrored in its own frame and the mirrored
picture is turned (`petgeometry.h`). Nothing is decoded again and the frame cache's key does not change: a sweep of angles leaves `mode` and `cacheBytes` as they are.

- The **window** is the footprint of the turned rectangle, `w|cos a| + h|sin a|` by `w|sin a| + h|cos a|`, centred on the pet's own centre; `x`, `y` and `width`
  keep meaning the unrotated pet's top-left and width, so the window margins are `x - (footprint.w - w)/2` and the like. The footprint may leave the output;
  it is kept where at least 48 px of it can be taken, as in arrange mode (`clampToOutput` of `petgeometry.h`).
- **Arrange**: the controls turn with the pet (the handle stays at the pet's own bottom-right corner; the close glyph is counter-rotated so it stays upright), the input
  region is the footprint, and the corner handle resizes along the turned pet's own axes **keeping the opposite corner where it is on the screen**
  (`petTopLeftKeepingTopLeftCorner`; growing about the unrotated top-left would slide the pet).
- The window is whole pixels while the scene graph centres the pet in it: at an angle that is not a multiple of 90 degrees the pet can sit up to a pixel off the true centre.
- Quality: the sampler is linear; the edges of a turned picture are not antialiased. No degrade name.
- The hit mask is turned the same way (`AlphaMask::transformed(rotation, flipH, flipV)`), so the input follows the drawing: the nested test with rotations of 90 and 37 degrees and
flips checks that a press on an opaque pixel of the turned drawing drags the pet and a press where the unturned drawing was opaque goes through.

## Hide all, show all and the rescue

- **Hide all** (`murasakictl pets hide`, `show`, `toggle-hidden`, Meta+Shift+H, the GUI): the daemon keeps a session state `PetsHidden` (not stored: a restart shows the pets).
  The overlay unmaps every window, pauses the pets, decodes nothing and keeps the frames and the places.
- **Rescue** (`murasakictl pets rescue`, Meta+Shift+R, the GUI): in one write, every pet gets `hoverHide`, `focusFade`, `smartDodge` and `locked` off, `clickThrough` off,
  `keepAbove` on, and opacity at least 0.5; paths, items and positions are not touched; the pets are shown again. **The D-Bus call is the real rescue**: with pets that are
  click-through and possibly invisible a key may not be bound (Wayland compositors own the global keys), so the call and `murasakictl` always work.
- Anima's own keys are Ctrl+Shift+Space and Ctrl+Shift+R; Murasaki suggests Meta ones (a Ctrl+Shift+R grab would steal "hard reload" from browsers) and the compositor lets
  you rebind them.

## Settings that act on all of this

| Setting | Meaning | Default |
|---|---|---|
| `MURASAKI_FRAME_CACHE_MIB` | memory for the decoded frames of all the pets (the GPU frame cache); 0 turns the cache off and every pet plays through libmpv | 128 |
| `MURASAKI_MASK_CACHE_MIB` | memory for the hit masks of all the pets together; each pet keeps its union whatever its share | 8 |
| `MURASAKI_POINTER=local` | forces the local pointer level (no global source is made) | the session decides |
| `MURASAKI_DEBUG_INPUT=1` | logs the input regions the overlay applies and the pointer events its windows receive | off |

## What a pet costs when nothing is asked of it

A pet that is click-through with no behaviour on (every pet that existed before these settings) makes no mask: no decode work for it, no pass over its file, no
file under `$XDG_CACHE_HOME/murasaki/pets/`, no memory; no timer runs for it; nothing is exported on the session bus for the pointer and nothing is asked of the
compositor (the source is made on the first pet that hides, fades or dodges). Turning on a behaviour that needs masks makes them once (a frame-cache pet is decoded
again with masks; a video-path pet gets its pass). Measured 2026-10-21 in a nested KWin with five such pets (three 640x400x90 frames, two 1280x800x60 frames at 600 px;
60 s after 25 s of settling; one sample each, so the spread is not known): the build before the input work used 4.92 % of one core and 431 MiB, this build 4.87 % and
411 to 470 MiB (the memory was still settling in the first sample); no mask file, the same 124 threads. Not measured: the cost with masks on (the plan's 1 % of a core for three
pets) and with hide under the cursor on (0.5 %); `tools/bench/overlay_cost.py` of the plan was never written.

### The frames in the cache are the part that is drawn

A pet is mostly margin: the shape moves inside a canvas that is often half clear. `SpriteFrames::cropTransparentMargins()` (run on the decode thread, `SpriteItem::request`) keeps of every frame the rectangle that has
something in it, plus one clear pixel on each side that has room, and the item draws each texture at its place in the frame (`SpriteRoot::show`), so the picture is the same at any size (`tst_spriteitem`,
`croppedFramesDrawTheSamePicture`: whole against cut, at 1, 2 and 0.5 times). Hit masks are made from the whole frames before and are untouched. What the cache takes is what is drawn: `loaded(bytes)` and the
controller's budget see the cut size, so more pets fit the 128 MiB, and a pet that fits plays from textures, with no decoder running for it. Found with the cost bench (plain scenario, three 640x400x90 and two 1280x800x60
pets): the greedy admission put the two big pets in the cache (108 of 128 MiB) and left the three small ones to libmpv, whose APNG decoder cost 5.7 to 8 % of a core per pet for ever (a frame of 640x400
takes 7 ms of CPU to inflate, whatever the number of decoder threads: one, two and four threads measured the same CPU time). With the margins cut all five fit (an estimate, from the shape: about 119 MiB; the bench showed no decoder thread running). Measured 2026-10-23, three runs each, interleaved,
nested KWin, 60 s after 25 s of settling: overlay CPU 69.6 % (69.1, 69.5, 70.1) to 8.5 % (8.9, 8.4, 8.3), threads 159 to 54, RSS 525 to 492 MiB. An admission that is made on the full frame size
(a pet that has not been decoded yet) is corrected by the next pass, when the pets that were decoded report their cut size. The frames are cut as the decoder makes them (`SpriteDecoder::decode(..., cropMargins)`), and the decoder's limit counts what is kept; the controller admits a pet by what the same frames (same file, same size) took once cut when this overlay has decoded them before (`m_croppedBytes`, learned from the decoder's report, never counted above the whole frames, forgotten when a decode of that key fails), and by the size of the whole frames before the first decode. The budget is never exceeded either way: the sum of what is admitted is within it and each decode is allowed no more than what is left. A single pet whose whole frames are over the budget is never decoded, so its cut size is never learned, and it stays on the video path even if its cut frames would fit. A pet with little margin gains little. `SpriteFrames::offsets` is all or none (one place per image); the item refuses a set where it is neither.

## Per compositor

`V` verified in a nested session by a test of this repository (KWin: `tools/test-overlay-nested.sh`, `tools/test-overlay-pointer-nested.sh`), `U` covered by unit tests
only, `?` not run on any machine here (the compositor is not installed, or there is no real session).

| Behaviour | Plasma (KWin, Wayland) | GNOME (Mutter, shell extension) | wlroots (sway, labwc, Hyprland, river, wayfire, niri, COSMIC) | X11 |
|---|---|---|---|---|
| click-through per pet, lock | V (the pet stays; locked does not move) | ? (plain window pinned by the extension) | ? | U + shape read back on Xvfb (V for the input shape) |
| hit-testing on opaque pixels | V (a press on a transparent pixel passes, on an opaque one drags), input region from the mask, at most 128 rectangles per frame | ? | ? | U (the input shape, Xvfb) |
| pointer level | global (V: the script reports the cursor, 21 reports a second at most, none idle, none after unload) | local (`?`) | Hyprland global (`?`: the IPC parser is tested on hand-written replies, never against a Hyprland); the others local | global by polling (`?` against a real X11 session) |
| hide under the cursor | V | local fallback (`?`) | Hyprland as KWin (`?`), others local fallback (`?`) | U |
| focus fade | V | local fallback (`?`) | as hide under the cursor | U |
| smart dodge | V (one step of 85 px, the daemon hears it) | local: only when touched (`?`) | Hyprland as KWin, others only when touched | U |
| drag | V (the window grows to the output for the press; exact to the pixel) | ? | ? | ? |
| rotation, flip | V (the picture and the input agree at 90, 37 and flipped) | ? | ? | ? |
| keep above off | U + unit tests; not checked against a backdrop window in a nested session | `keep above off: unsupported` (`?`) | bottom layer (`?`) | U (stacking tested on Xvfb) |
| hide all, rescue, shortcuts | V for the D-Bus calls and the controller (tests); the shortcut through kglobalaccel V in the nested test | D-Bus V; shortcut through the portal `?` | D-Bus V; the portal `?` | D-Bus V; shortcut `?` |

## The checklist for a first real session on a desktop

Run it on the real session of a new desktop (it is what a nested session can not show: the real input stack, the real shell, the real portal), and write down what differs.

1. `murasaki-overlay` starts with the session; `journalctl --user -u murasaki-overlay` says which pointer level it chose (`pointer: global (...)` or `local`).
2. Add a pet with a large transparent area (`murasakictl pets add ...`); `murasakictl pets set <id> --click-through off`. `Windows()` (`busctl --user call io.github.uriel2121ger_art.Murasaki.Overlay /io/github/uriel2121ger_art/Murasaki/Overlay io.github.uriel2121ger_art.Murasaki.Overlay1 Windows`) shows `hitMask: frame` (or `silhouette` for a pet that plays through libmpv).
3. Click on the transparent part of the pet's rectangle: the click must reach what is under it. Click on the drawing and drag: the pet follows the pointer without jumping, and the place is kept after the drop (`murasakictl pets list`).
4. `--lock on`: the drag moves nothing. `--lock off`.
5. `--hover-hide on`, then move the pointer over the drawing: the pet disappears at once and comes back when the pointer leaves it; on the transparent part nothing happens. At the **local** level the first click on the hidden pet is swallowed and the next ones pass (`hide under cursor: clicks swallowed` in `degraded`).
6. `--focus-fade on` (it turns hover hide off): the pet fades to about a fifth of its opacity under the pointer.
7. `--smart-dodge on`: bring the pointer within a few pixels of the pet's edge: it steps away by about a third of its size and does not leave the output; a second step comes at most every 0.22 s. At the local level it only steps when the pointer touches the drawing.
8. `--rotation 37 --flip-h on`: the picture and the clickable area agree (repeat 3 on the turned drawing).
9. `--keep-above off`: the pet goes below the windows (still above the wallpaper); `on` brings it back. Note the degrade name in `degraded`.
10. Meta+Shift+H hides every pet and again shows them; Meta+Shift+R after turning several flags on restores solid, grabbable pets (or the same calls with `murasakictl pets hide|rescue`).
11. Plug and unplug an output, lock the session, hide and show: nothing crashes, the pets come back.
12. Leave five pets with the defaults for a minute: the overlay's CPU and RSS (`ps`) must not be higher than before the update.

If a step fails, run the overlay with `MURASAKI_DEBUG_INPUT=1` and `MURASAKI_POINTER=local` to tell the compositor's input from the pointer source, and add what was found to
`pointer-facts.md` (a new column for the compositor).
