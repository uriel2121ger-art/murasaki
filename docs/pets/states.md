# Pet states and Mouse Tracking

A pet can look different depending on what is happening to it, and can look at or follow the mouse. Both are **off by default and per pet**: a pet without `states`
and `tracking` behaves exactly as before. Anima's own app shows these lists and a "Mouse Tracking" label but does nothing with them; here they work, as Murasaki's
own feature with Anima's vocabulary.

The values are `src/core/petstates.h`, the machine is `src/overlay/petstatemachine.h`.
This document is a draft that grows with the tasks of the plan; what is not built yet says so.

## The states

| State | When |
|---|---|
| `idle` | nothing is happening (Anima's "static") |
| `grabbed` | the pet is held with the mouse |
| `moving` | the pet is being carried, or stepping away from the cursor (smart dodge) |
| `released` | the pet was just dropped |

Transitions (each one is an event `PetInteraction` already produces; the state machine only listens and changes nothing in the drag, dodge or fade rules):

| From | Event | To |
|---|---|---|
| `idle`, `released` | a press that starts a drag (an opaque pixel of a pet that can be grabbed) | `grabbed` |
| `grabbed` | the pointer is 6 px or more from where it rested, button down | `moving` |
| `moving` | no motion for 300 ms, button still down | `grabbed` |
| `grabbed`, `moving` | the button is released | `released` |
| `released` | one pass of its animation, counted from its first frame on screen (never under 200 ms or over 10 s); 1.2 s for a picture with no length (a still one) | `idle` |
| `idle` | a dodge step starts | `moving`, for the length of the step (at most 5 s), then `idle` |
| any | the pet is hidden or paused, the session locks, arrange mode starts, its window is lost (an output goes, its file is blocked) | `idle` (idle's sequence keeps its place) |

A press that starts no drag (click-through, locked, a transparent pixel) changes nothing. While the arrange mode is on the pet shows `idle`. A pet that is hidden, paused, locked or arranged waits for nothing: no sequence advances and no timer runs, and when it is shown again what was waited for counts from that moment. Changing the placement (a drag commits the new x and y) does not reset the state; only what is in the table does. Every file is played in a loop here whatever its own loop count says, so "a pass" is the length the item reports (frames at the pet's rate); an animation of no length (a still one) has the 1.2 s.

## What a state shows

`states.<name>` is one of:

- `{ "variant": "<name>" }`: one animation of the item, looping as a pet does today.
- `{ "sequence": { "items": ["a", "b"], "order": "linear" | "random", "advance": "auto" | "click", "once": false, "holdMs": 4000 } }`: 1 to 16 variants.
  `linear` plays them in order and starts over; `random` picks one that is not the one just played. `auto` moves on after `holdMs` (500 to 60000, default 4000) from the first frame of the picture (after one pass when the sequence is `once`); `click` moves on when the pet is clicked. `once` plays each entry one pass and holds
  the last frame (an automatic linear sequence that is `once` stops on its last entry).
- absent: the state falls back: `moving` to `grabbed`, `grabbed` to `idle`, `released` to `idle`, `idle` to the placement's `variant`.

Entering a state starts its sequence over (a `click` sequence keeps its place: the click is what moves it). A named variant the item lacks is skipped; a state
with none valid falls back and the pet says `states: missing variant`. `UpdatePet` merges `states` state by state (a state is replaced whole, `null` clears one
state or, as a whole, all of them); wrong types or values are `InvalidArgs`. "Random, non-automatic" and "play once on touch" are guesses at what Anima's lists
mean (the shipped app never uses them): they are conservative and the semantics are unverified against Anima.

## How it runs

The states are the overlay's, not the daemon's: the state a pet is in is never stored and never goes through D-Bus (a state can change many times a second; a
placement write each time would be a store write and a `PetsChanged`). `OverlayController` keeps a `PetStateMachine` for each pet whose placement has `states`
(a pet without them has no machine, no timer, no events and no extra decode), and the window reports what `PetInput` decided: a grab, the drag's steps, the
release, a dodge step (`PetPointerEvent`). The machine's answer becomes the variant of the view, in the same layer as the runtime layer's: the window is handed
the file of the new state exactly as it is when `UpdatePet` changes `variant`, and it goes on showing its old picture until the new one has its first frame, so a
slow switch is a late change and never a blank. A variant that a timeline or a trigger set (the runtime layer) is shown as it is while the layer holds it.

* The shown width is the placement's; the height follows the new animation, and the pet's top-left corner stays put (as a variant change does today).
* The hit masks and the input region follow the file that plays: the masks of the frames on screen, or the controller's pass of the file (until it is there the
  pet takes its whole rectangle, `hit mask: coarse`). A switch during a drag neither ends it nor moves the pet.
* Every variant any state names is converted (transcoded) like the selected one. The frame cache holds one set of frames per pet, the current state's, so no
  state raises the budget; a state whose frames do not fit plays from the video and the pet says `states: over budget, playing from video`.
* The arrange mode, a pause (the placement's, the session's), hiding the pets and a pet hidden by a timeline put the pet back to `idle`.
* A click moves on the `click` sequences of `idle` and of the state the pet is in. `once` holds the last frame by pausing the window at the end of the pass the
  machine counts from the moment it handed the file over, so it holds about the last frame, not exactly; a click on a one-entry `once` sequence plays it again
  (a restart of the same file is not built yet: it resumes).
* A dodge step shows `moving` for 220 ms (the cooldown of the dodge).
* `Windows()` (the overlay's) gains `state` and `stateSource` (`variant` or `sequence:<i>`) for a pet that has states, and nothing for any other pet.

## Setting them

* `murasakictl pets state <pet id> idle|grabbed|moving|released variant <name>` or `sequence a,b,c [--order linear|random] [--advance auto|click] [--once]
  [--hold-ms N]`; `pets state <pet id> clear [state]`. `murasakictl pets track <pet id> off|look|follow [--distance N] [--speed X] [--return|--no-return]`.
  `pets list` adds `tracking:<mode>` and `states:<names>` columns for a pet that has them (and nothing for any other).
* The pet's page in the Murasaki window has a "States and mouse" section: a choice (as before, one animation, a sequence) for each state with a Try button (the
  overlay's `PreviewPetState`), and the tracking mode, distance, speed and return. All of it is in Spanish when the system is.

## Mouse Tracking

`tracking.mode` is `off` (default), `look` (the pet turns to face the cursor) or `follow` (the pet's window moves toward the cursor and stops `distance` px away).
`distance` is 0 to 4000 (default 160), `speed` 0.1 to 4 (default 1, about 200 px per second), `returnHome` (default true) sends a follower back to its stored place
when tracking ends. `follow` is exclusive with `smartDodge`, `hoverHide` and `focusFade`, also while the pet is a stage (those flags are kept there, and a stage that stops being one would otherwise turn out to be invalid). Both need the global pointer level; at the local level tracking is off
for that pet and says `tracking: needs the global pointer`. The rescue sets `tracking.mode` to `off`.

`follow`: the pet's window walks toward the cursor along the straight line and stops `distance` px from it (centre to cursor; a cursor already closer does not
push it away: that is smart dodge's job), with an ease-out and a cap of 200 px/s times `speed`, in at most 30 steps a second (`tracking: moves limited to 30 a
second` while it walks), kept inside its output. While it walks the pet is in the `moving` state. The displacement is a runtime layer on the placement (like a
timeline's move): **nothing is written** while it walks. When following ends (the mode changes, the rescue) it glides home (`returnHome`, the default) or, with
`returnHome` off, stays where it is and that place is stored once (also when it is hidden, paused or arranged while it follows: the place is stored then too, rounded to whole pixels, and the pet shows it until the daemon says it, or refuses, or after three seconds the stored one rules). A follower needs the pet's stored place as its base: while a timeline moves the pet, the walk is added on top of the timeline's place and the output's limits are applied to the stored one. Hiding, pausing, the arrange mode or losing the global pointer send it home at once. It parks
(stands still, `tracking: parked over a fullscreen window`) while a fullscreen window is on its output; a pet that is held (dragged) stands still, and the drop is
its place (a window that is lost mid-drag lets go too); a `locked` pet does not follow (`look` still works). The gap is measured from the placement's own variant, so states of different heights do not make
it chase itself.

`look`: the picture is mirrored (on top of the placement's own flip, never stored) when the cursor is on the side the item does not face; the item faces
right. A dead zone of 24 px around the pet's centre keeps it still, and it turns at most 4 times a second. The pointer source runs while a pet that tracks is
shown and not paused, hidden or arranged. `Windows()` of a pet with `tracking` carries `tracking` (the mode) and `trackingActive`.

## In a stage

A stage (docs/pets/stage.md) is a picture across a screen with no pointer, so the legacy intent of Mouse Tracking (the picture follows the cursor inside the fullscreen
canvas) is its faithful form. `look` turns the picture, in both stage modes. `follow` walks the picture inside the screen of an `output` stage (its rectangle is
kept inside the screen; the window never moves and nothing is stored: a stage's picture always goes back to the middle when following ends, whatever `returnHome`
says, and `locked`, which a stage is by nature, does not stop it). A `desktop` stage is one picture across every screen and has nowhere to walk: it looks, and says
`tracking: follow needs a stage on one screen`. States in a stage are the ones that need no pointer: `idle`, `moving` while the picture walks, sequences; there is no
`grabbed` or `released` (nothing takes the pointer). The pet whose stage the session can not draw is an ordinary pet in all of this. The plan's `PetPlacer` interface
was not made: the controller's layers (the place on the placement for a window, the offset of the picture's rectangle for a stage) already are the one place the two
modes differ, and a class with one implementation would only hide that. The follower's rectangle is the one of the pet's own variant; if the states differ in shape the picture of a state can show a little past the edge, and a stage that turns from `output` to `desktop` while the picture is away goes back on one screen only.

## Import

Anima's per-state lists (`sprites.json`, `spritesSecuencia.json`) are the user's own application configuration, not part of a Workshop item (an item is a folder of bare
animation files, one variant each) or of an `.mdaa` package (a ZIP of an `actions.mddo`, media and a program that is not run): nothing the importer reads carries state
lists, so it creates none, and `states` is set by the verbs, the pet's page or a profile (`murasakictl pets state`).

## Names

The names a pet's `degraded` list can carry; none is an error.

| Name | Meaning |
|---|---|
| `states: missing variant` | a state names a variant the item does not have |
| `states: transcode pending` | a variant a state uses has no copy at the pet's size yet |
| `states: switching slow` | the libmpv switch latency passed the limit |
| `states: over budget, playing from video` | the states do not all fit the frame-cache budget; those that do not play through libmpv |
| `tracking: needs the global pointer` | the session only offers the local pointer level |
| `tracking: parked over a fullscreen window` | a follower waits while a fullscreen window is on its output |
| `tracking: moves limited to 30 a second` | a follower steps at most 30 times a second |
| `tracking: follow needs a stage on one screen` | the pet is a stage across all screens (`desktop`): its picture can look at the cursor but not walk |

## How fast a switch shows (task 1 of the plan, U4) and the preload (task 5)

`tools/bench/state_switch.sh` (nested KWin with software GL, so the absolute numbers are pessimistic; the daemon and the overlay of the build; one pet of two 640x400
animations of 30 frames; `MURASAKI_DEBUG_STATES=1` makes the overlay log each change; a screenshot taken as soon as a change is asked says whether the pet was
blank). Time from handing the window the file of the new state to its first frame:

| Path | Before the preload (task 1, sequence of two pictures every 500 ms, 100 switches) | With the preload (task 5, `idle` and `grabbed` changed through `PreviewPetState` every 2 s) |
|---|---|---|
| libmpv (frame cache off) | min 298, median 302, p90 322, p99 331, max 347 ms | min 231, median 242, p90 286, max 448 ms (40 switches; nothing is prepared ahead on this path) |
| frame cache, room for one set only (40 MiB) | min 428, median 453, p90 494, max 500 ms (cut by the hold) | min 244, median 260, p90 284, max 313 ms (40 switches: a change decodes) |
| frame cache, room for the next picture too (128 MiB, the default) | not built | **min 0, median 0, p90 1, max 1 ms** (60 switches: the sets swap) |

No screenshot showed a blank pet in any run (the previous picture stays until the new one has its first frame; with the preload the new one is already there).

The limit of the plan is about 120 ms (`kStateSwitchSlowMs`), and `states: switching slow` is still reported whenever a change takes longer, so a pet on the video path or
without room for a second set says so. What the preload is: a pet with states on the frame cache decodes, behind the pictures that play, the picture of the state it
is likely to enter next (idle: grabbed; grabbed: moving, released, then idle; moving: released, grabbed, idle; released: idle; a random sequence is not known
beforehand and is not prepared) into a second set of frames that is not drawn and does not play. The plan of the cache admits those after every pet's own frames, in
the same order, and only when they fit in the same budget (the sum never exceeds it); the window is handed the picture once the pet's own frames are in. When the
state changes to the picture that was prepared the two sets change places (no decode, no wait), the old set is dropped and the next one is prepared; to any other
picture the pet decodes as it always did. Not built: a second paused libmpv player for pets that are not on the frame cache (the video path keeps its 0.2 to 0.3 s).
Unverified: the same numbers on a real GPU (libmpv's start and the decode will be quicker), and the cost of the first loop of a swapped set (its textures are made
when it was decoded and uploaded when first drawn, as they always are).

## What it costs

A pet without `states` and `tracking` has no machine, no tracker, no timer, no pointer source and no extra decode; in QML it has a second, empty `SpriteItem` (no
frames, no timer running). Measured with `tools/bench/overlay_cost.sh --plain-only` (five pets, the nested KWin, the machine busy with other work, so the absolute
numbers are high and noisy: the audio document has 22 % for this setup on a quiet machine), the build before phase 3 and this one, two runs each, 40 s after the
settling: overlay CPU 63 % and 57 % before, 59 % and 40 % after; overlay RSS 573 and 553 MiB before, 523 and 569 MiB after: not worse, within the noise. A pet with
states costs one timer that fires only when something is due (nothing while it rests or while it is hidden), a pet that looks costs the pointer source while it is
shown (about 30 reports a second), a follower costs 30 steps a second while it walks and nothing when it has stopped; the switch itself is in the numbers above.
Not measured: a minute of `follow` against a scripted cursor (compositor reconfigure cost of 30 margin changes a second).

## Verified and unverified

Verified: the pure machine and the values (`tst_petstates`), the placement fields (`tst_petstore`, `tst_murasakiservice`), the controller with fake windows
(`tst_overlaycontroller`: the file follows the state, no machine/timer/keys for a pet without states, the dropped picture never, the names), the pointer events
(`tst_petinput`). Unverified: a real GPU, a real compositor other than the nested KWin, and the feel of `follow` on real hardware.

## Puppet pets

A puppet pet (docs/pets/puppets.md, section 22) has the variants `model`, `motion:<Group>` and `expression:<Name>`: a state's `variant` may name a motion group (it plays while the state shows; leaving the state ends it), an expression (it shows while the state shows) or `model` (nothing). Nothing is converted, cached or preloaded for them. Mouse Tracking `look` is the puppet's gaze, not a mirror of the picture; a click plays its `TapBody` or `Tap` group when it has one.
