# Profiles, triggers and timelines (API revision 18)

The pets can be saved as a named **profile** and switched in one step, and things can happen by themselves: a **trigger** watches for something (an application gets the
focus, it is 22:00, the laptop goes on battery, a key, a call from your own scripts) and answers with one **action** (play a **timeline**, switch the profile, hide the
pets, ...).

Everything here is **native and inert by construction**: a trigger acts on pets and only on pets. It never starts a program, opens a file or a web address, reads the screen,
listens to the keyboard (outside the desktop's own shortcut dialog) or touches the network. A package's `core.exe` is never run (see "What replaces `core.exe`").

## What exists and what costs

Nothing is added until you make it: a fresh install has no `profiles.json`, `triggers.json` or `timelines.json`, and the automation adds no timer, no watcher and no bus subscription (a test pins
it). (The daemon's and the surface's own watchers, such as the lock state, exist anyway; what is new here costs nothing.) The daemon subscribes to something only while an enabled trigger needs it, and has at most one timer for the clock rules and one for a running timeline.

| Source | Needs | Costs when no enabled trigger uses it |
|---|---|---|
| `app-focus`, `app-open`, `fullscreen`, `power`, `game` | the surface's `SessionFacts()` (revision 16) | nothing: the daemon does not follow `SessionFactsChanged` |
| `session` (locked/unlocked) | the lock state the daemon already follows for the playlists | nothing |
| `idle` | the surface's idle (the compositor's `ext-idle-notify-v1`, revision 23), else logind's `IdleHint` on the system bus | nothing |
| `time`, `interval`, `idle` | one single-shot timer to the next due moment (at most an hour ahead) | no timer |
| `signal-file` | a watch on `$XDG_RUNTIME_DIR/murasaki/triggers/` | the directory is not even made |
| `dbus` | a match rule on the session bus (interface and member; the arguments are never read), at most 8 | no rule |
| `shortcut` | a key registered with the desktop (portal or kglobalaccel), at most 8 | no registration |
| `fire` | calls to `FireTrigger` | nothing |

## Profiles

`$XDG_CONFIG_HOME/murasaki/profiles.json`: `{version: 1, profiles: [{id, name, pets: [Entry], origin?}]}`. An Entry is a pet's placement without its `petId` (every field:
position, size, speed, behaviours, sound, ...) plus a `slot` that names it across switches. At most 50 profiles of 64 pets (the pet store's own limit); names 1 to 100
characters, unique in any case.

* **Save** (`murasakictl profile save NAME`): the pets on the desktop become the profile (an existing name is replaced). It becomes the *active* profile.
* **Switch** (`profile switch NAME`): the pets that exist in both keep their window, their sound and their place in the order (matched by item and variant first, then by
  item); only the fields that differ change; the rest are added or removed. One write of `pets.json`, one `PetsChanged`. A pet whose item or variant is gone is skipped with a
  note (`profile: item missing`, `profile: variant missing`) and the rest applies. Switching to what is already there writes nothing.
* A profile holds the pets only: no wallpaper, no setting, and nothing of the session (hidden, arranging, muted).
* The active profile is a label (it survives a restart in `$XDG_STATE_HOME/murasaki/automation-state.json`). Nothing is restored at login, and switching away does not save
  the setup you leave: saving is explicit. `ProfileModified` says whether the pets differ from the active profile.

## The runtime layer

A timeline moves and hides pets many times a second; writing `pets.json` for each step would wear the disk and fight your drags. So the daemon keeps a **runtime layer** per
pet that is **never stored**: `hidden`, a position override, a linear `motion`, an opacity and a variant (`PetRuntime`, revision 18). The overlay shows the placement with the
layer on top: a hidden pet's window is unmapped and the pet paused and silent; a position replaces `x` and `y`; a motion is drawn by the overlay with its own clock (the pet's
window moves every 33 ms while it lasts, and the daemon hears nothing back); a variant the item does not have is ignored.

A pet in the fullscreen mode (a **stage**, docs/pets/stage.md) fills its screens: a timeline or a trigger can **hide** it (its windows are unmapped, `stageHidden` says `hidden`) but a `place` or a `move` on it is ignored, and `Windows()` reports `runtimeMoveIgnored`.

The layer goes away by itself when the pet is removed, when the overlay or the daemon restarts, when you drag the pet (your hand wins: its position override is dropped), with
`murasakictl pets runtime clear [PET]` and with `pets rescue`, which also stops every timeline. `Windows()` reports `runtimeHidden` for a pet a timeline hid. It also lets a
package scene that hides its pet work: `StartScene` hides the pet in the layer, and a scene that shows it undoes that.

## Triggers

`$XDG_CONFIG_HOME/murasaki/triggers.json`: `{id, name, enabled, cooldownMs, mode, source, action, origin?}`. At most 100. A trigger made by an import starts **disabled**.

In the GUI, triggers live on the **Automations** page (design: `docs/design/specs/2026-10-25-murasaki-automations-design.md`),
next to the wallpaper rules of the settings (`pauseOnGame`, `pauseWhenCovered`, `batteryMode`, `pauseApps`). Each one reads as a
sentence ("When I open Discord, mute the pets") and is made or changed in a sentence editor; the application is picked from the
installed `.desktop` files and stored as the window class its windows carry (`StartupWMClass`, else the desktop file id). The former
Triggers page, with every field, screen watching and the examples, is the page's "Advanced view".

| Kind | Fields | Fires when |
|---|---|---|
| `app-focus` | `app` (a window class; `*` and `?` match, any case), `on` (`enter`, default, or `leave`) | the focused window's class starts (or stops) matching |
| `app-open` | `app`, `on` (`open` or `close`) | the first window of a matching class appears (the last closes) |
| `fullscreen` | `app?`, `output?`, `on` | a fullscreen window (of that class, on that output) appears (or goes) |
| `time` | `at` ("HH:MM", local), `days` (0 Sunday to 6; none: every day) | the clock reaches it; due while the machine slept by under 2 minutes still fires, later is skipped |
| `interval` | `everyMinutes` 1 to 1440 | every N minutes (a sleep of more than 2 minutes past the due time skips that one) |
| `idle` | `minutes` 1 to 1440, `state` (`away`, default, or `back`; revision 23) | `away`: the user has had no input for that long, once per idle period. `back`: an idle period that lasted at least `minutes` ends (the first input), once per period, so a `back` with the same `minutes` undoes exactly the naps that fired. An idle that was already there when the daemon (or the trigger) started is not a nap and its end does not fire `back` |
| `power` | `state` (`battery` or `ac`) | the battery fact changes |
| `session` | `state` (`locked` or `unlocked`) | the lock changes |
| `game` | `state` (`started` or `ended`) | GameMode's clients cross zero |
| `output` | `state` (`added` or `removed`), `name?` | an output appears or goes |
| `shortcut` | `id`, `description?` | the key you gave it in the desktop's dialog is pressed (`mode: hold`: also released) |
| `signal-file` | `name` | the file `$XDG_RUNTIME_DIR/murasaki/triggers/<name>` appears or is touched (the contents are never read) |
| `fire` | `name` | `FireTrigger(name)` or `murasakictl triggers fire NAME` |
| `dbus` | `interface`, `member`, `senderNamespace?` | a session-bus signal with that interface and member (arguments never read) |
| `screen` | regions, match settings (below, "Screen triggers") | the screen shows the picture: only while the user is watching the screen (revision 21); otherwise `automation: screen triggers are not active` |

Names of files, fires and shortcuts are `[A-Za-z0-9._-]` of 1 to 64 characters. The rules, all in `src/core/triggerengine.cpp` (pure, with an injected clock):

* **Edges, not levels**: a source fires when its condition becomes true (or false with `on: leave`); the first facts only set the starting point; facts that come back after
  being unknown (no window monitor) also only set it.
* **Cooldown** per trigger (default 500 ms, Anima's; 100 ms to a day). **Rate limit**: 20 fires a second overall and 8 per trigger. A **loop guard**: a trigger that fires soon
  after another because of what a pet's action changed is a hop of a chain; more than 4 hops in 2 seconds are cut (`trigger: chain too deep`).
* The master switch `automationEnabled` (setting, default on): off, nothing fires and nothing is subscribed. **The rescue (`pets rescue`) also pauses the triggers for a
  minute** (`TriggerState.suspendedMs` says how long is left; the Automations page and its Advanced view offer "Resume now", the call is `ResumeAutomation`), so that a loop of triggers can not win against
  the one-call way out.
* **Loops.** A trigger that fires because of a fact (a focus, an output, ...) within 2 seconds of another fire is a hop of a chain, so a feedback loop of any period under 2 s is cut after
  4 hops (`trigger: chain too deep`), and a fact trigger never fires more than 30 times in a minute.
* The last 50 results are kept in memory (`TriggerState`), never on disk.

### Actions (a closed list)

| `type` | Does |
|---|---|
| `timeline` (`id`) | runs the timeline (a hold shortcut stops it on release, which restores it) |
| `scene` (`petId` or `item`, `index`) | starts scene `index` of an imported package on that pet, or on every pet of the item |
| `profile` (`id`) | switches to the profile |
| `hide-all`, `show-all`, `toggle-hidden` | the session's hidden state of every pet (`PetsHidden`) |
| `stop-sounds` | stops what plays once (clicks, wraps, starts); a pet in the mode `continuous` keeps its sound. Use `mute` to silence everything |
| `mute` (`mode`: `on`, `off`, `toggle`) | the pets' mute **for the session** (`PetsMuted`, on top of the setting `petsMuted`, which is not written; false after a restart) |
| `play-sound` (`petId` or `item`) | the pet's own sound once (the test button's path) |

There is no action that runs a command, opens a file or a URL, reads anything or sends anything. `tst_automationclosed` pins the list (the kinds, the hooks the manager can
call, and the sources of the automation code: no `QProcess`, no `QDesktopServices`, no `system`).

A `hold` shortcut (`mode: hold`) runs its action on the press and the inverse on the release: `hide-all` <-> `show-all`, `mute on` <-> `off`, a toggle toggles, a timeline is
stopped. The end of a hold puts back what was there when it began (the pets' hidden flag, the mute flag), not the opposite action: pets you hid by hand stay hidden when a `hide-all` hold ends, and a mute you set stays set (the same for a screen trigger, which is a hold). Other actions have no inverse and are refused for `hold`. Holding works with the portal and with kglobalaccel; the portal's dialog (GNOME) is **unverified here**.

## Timelines

`$XDG_CONFIG_HOME/murasaki/timelines.json`: `{id, name, speed, restore, tracks: [{pet, startVisible, actions}], origin?}`. At most 100; 16 tracks of 256 actions; `speed` 0.25
to 4 divides every wait and move; a track may last up to 24 hours (`timeline: too long`). Tracks run in parallel, the actions of a track in order.

| `kind` | Fields | Does |
|---|---|---|
| `place` | `x`, `y`, `output?`, `anchor?` | the position override (a teleport); `anchor` is `corner` (default: x and y are the top-left corner) or `centre` |
| `show` | `variant?` | shows the pet, optionally in another animation |
| `hide` | | hides the pet |
| `move` | `x`, `y`, `anchor?`, `mode` (`teleport` or `smooth`), `durationMs` | smooth is a linear motion, and **holds the track** until it ends |
| `sound` | | the pet's own sound once |
| `wait` | `ms` (up to an hour) | pauses the track |
| `stopSounds` | | silences the sounds |
| `track` | `mode` (`off`, `look` or `follow`) | revision 22: the pet's Mouse Tracking mode while the run lasts (docs/pets/states.md). It goes in the runtime layer, so the stored `tracking` is never written and `restore`, stopping the timeline, `ClearPetRuntime` and the rescue bring the stored mode back; distance, speed and return are the pet's own |

`pet` names the pet in one way: `{petId}`, `{item, variant?}` (the first pet of that item) or `{slot}` (a pet of the last profile switch or save). A track whose pet is not
found is skipped (`timeline: pet not found`). With `restore` (default) the pets a run touched go back to their stored state when it ends or is stopped. At most 8 run
(`timeline: too many running`); running one again restarts it.

## Examples ("Try it", API revision 22)

A fresh install has nothing to look at, so the pages have a **Try it** button (Timelines, Triggers, Profiles, and the pet page's Mouse tracking) and `murasakictl demo` does the same from a
terminal. An example is made out of **your own pets** (no art ships) with the calls above, and plays or is ready to use at once:

| Page / command | Makes | Then |
|---|---|---|
| Timelines, `demo timelines` | the timeline "Example: pets across the screen": with two pets one crosses the screen with a smooth `move` while the other waits hidden, appears in a corner (`place`, `show`) and hides; with one pet it crosses, hides and comes back in the corner. `restore` on | shows its steps (read-only), then plays; everything is back at the end |
| Triggers, `demo triggers` | the same timeline and a `shortcut` trigger that plays it | you give the shortcut a key in the desktop's dialog (a demo can not preset one) and press it; "Run it now" / `triggers test` runs it without |
| Profiles, `demo profiles` | a profile of the pets as they are (`SaveExampleProfile`: it is not made the active profile and the pet slots of your timelines are not touched) | you switch to it yourself ("Switch to this profile", `profile switch`): move or remove a pet first to see them come back. Switching makes timelines that use a pet slot follow that profile; "Remove examples" while it is active goes back to the profile you had |
| Pet page, `demo tracking` | the timeline "Example: follow the cursor": `track follow`, 20 s, `restore` (`look` for a pet that is locked or a stage: it can not walk) | the pet follows the cursor for 20 s and goes home; nothing is stored, even if the pet's own `returnHome` is off (a follow the layer asks for always glides home) |

With no pet an example says so ("add a pet first", with a button to the library). Everything an example makes has **`origin: "demo"`**, and the name starts with "Example: " ("Ejemplo: " in the
Spanish interface); **Remove examples** / `demo remove` deletes exactly what carries the origin (triggers, then timelines, then profiles) and never anything else, whatever its name. A changed example (saved from the editor, or renamed) loses the marker and is yours. Examples never write
`pets.json` or the wallpapers (and the settings only in the screen walk, which puts its change back unless you keep the trigger). Doing a demo again replaces the example of the same name. The first-visit card of each page (`demoCardsDismissed` in the settings) is shown until it is closed.

**The screen-trigger example** (Automations, Advanced view, "Try a screen trigger", and `demo screen`; needs revision 21) is the one example that needs you: it can not run on its own, because nothing of the screen is read before you press Start and the desktop agrees, and no reference picture ships with it (what it looks for is a picture you take of your own screen). It makes the screen trigger "Example: hide the pets while a picture is on the screen": **off**, one placeholder region, no picture, action `hide-all` (held: the pets come back when the picture goes). The page's walk, step by step: it needs a pet; it makes the example (always a new trigger, under a free name: the walk never takes over an earlier example, which may have been kept and shaped since); it turns the setting `screenTriggersEnabled` on if it was off and the example on, because the daemon starts a watch only when a screen trigger is on; you press Start (the desktop's dialog); you drag the region on the screen; you take the picture (you see it before it is kept); then the live score is shown. **Keep this trigger** leaves the trigger, its region and its picture on and the setting on, stops watching (Start is pressed each session) and clears the demo marker: it is yours from then on and "Remove examples" never takes it. Anything else (Cancel, the window's close, also while the picture dialog is open, "Undo all of this", a refusal at the desktop's dialog, the desktop ending the share, a start the daemon turns down) stops the watch the walk started, drops a picture that was held, removes the example and turns the setting off again if the walk turned it on and no other screen trigger is on. Before each change the walk writes a small journal in the app's own config (`screen-demo.ini`: the example's id, whether it turned the setting on, whether it started the watch); if the GUI dies in the middle, the next GUI that connects finds the journal and puts back exactly that (the watch stopped, the example deleted only while it still carries the demo marker, the setting off when nothing else needs it). A picture held by a dead GUI is dropped with the watch, or by the daemon after a minute. `demo screen` only makes the example and prints the steps (turn the setting on in Settings, Automation, `triggers enable`, `triggers screen start`, `triggers screen capture`, `triggers screen regions`); it is not part of `demo all`.

## Screen triggers (API revision 21)

"When this picture is on my screen, play this timeline." A `screen` trigger holds rectangles of the screen (`regions`, in the desktop's logical coordinates; a circle or a polygon is matched as its bounding rectangle), a reference picture for each, and the match settings; while the picture is there the trigger holds and its action runs, when it goes the action is undone (a timeline is stopped, hide <-> show), as for a held shortcut. A timeline plays once while the trigger holds (Anima repeats its tracks).

**Nothing is read until the user says so.** All of these must hold: the setting `screenTriggersEnabled` (off by default), an enabled screen trigger, an unlocked session, and the user pressing Start (`StartScreenWatch`; the GUI button and `murasakictl triggers screen start` come with the next task of the phase). Starting opens the desktop's own dialog (the screen cast portal) and the user chooses the screens; a refusal leaves it stopped (`screen: permission refused`). The choice is remembered only with the setting `screenRemember`, and even then Start is pressed each session unless `screenWatchAtLogin` is on. The desktop can end the share at any time (`screen: stopped by the desktop`); there is no automatic restart. The rescue, turning the setting off and the last screen trigger being disabled or deleted (also while the dialog is open) stop it; a locked session pauses it (the helper is told to stop reading, also when the lock comes while the dialog is open, and what a trigger held is undone; the desktop's sharing indicator stays on until Stop). Whatever a screen trigger holds is also undone when the trigger is disabled, deleted or edited, when the automation switch goes off and when the watch stops. A fresh install has no portal object, no helper process, no timer and no bus call for it.

**Who reads the pixels.** One small process, `murasaki-screenmatch`, behind `murasaki-sandbox --profile screenmatch` (no files, no network, no programs: docs/security.md). The daemon gives it the PipeWire remote the portal opened and one socketpair, tells it the regions in the pixels of each monitor stream (the output's scale is applied), the references as 32x32 grey grids (the daemon decodes the stored PNGs under a cap; the helper has no decoder) and the rectangles of our own pets; it answers with scores in permille per region. The daemon runs one matcher per trigger (`src/core/screenmatch.*`): a tick is detected when any region passes the threshold (`agg`: `all` or `mean` are options), `startFramesToCheck` detected ticks in a row activate it, more than `stopGraceMs` without a detection ends it, the cooldown counts from the activation or the end, and a sequencer walks `seqLen` references. The rate is `screenTicksPerSecond` (5; 2 at most on battery), so a streak of ticks is longer than at Anima's 16 a second. Times in packs stay in milliseconds. With `stopOnMismatch` off the trigger ends when its timeline does. A region more than half covered by our own pets is skipped for that tick (`screenIgnorePets`); a pet that moves into a watched region can still make a loop: a screen trigger obeys the loop guard (the chain depth and at most 30 fires a minute), which cuts it.

**What can be wrong, by name** (in `ScreenWatchState.notes`, per region): `screen: region outside the shared screens` (never passes), `screen: region has no reference picture`, `screen: matched by structure, not SIFT; check the threshold` (an imported SIFT or ORB pack is matched by the structure score), `screen: more than 4 screen triggers are enabled; the rest are not watched`, `screen: more than 16 regions in all; the rest are not watched`. A Triggers pack's coordinates are the author's desktop pixels: they are kept as they are, so a pack made at another resolution needs its regions moved by hand.

**References.** A pack import keeps only the frames a trigger uses (scaled to at most 360 pixels, as PNGs); `ImportPackAutomation` hands them to the new trigger (`screen: ready (N reference pictures kept)`, or the reason). The user's own come from `CaptureScreenReference`: the helper sends back that one rectangle, scaled to at most 360 pixels (the only picture that ever leaves it, only while the user is watching, at most one a second), the daemon holds it in memory, the editor shows it, and `CommitScreenReference` keeps it. A deleted trigger deletes its pictures.

**A helper that crashes** is counted per binary in the crash ledger; after the third crash screen matching is off until the user turns the setting on again. **Cost** (the helper alone, one core = 100 %, measured with `tools/bench/screenmatch_cost.py`): at 1920x1080, 8 regions of 480x270 cost 0.6 % at 5 ticks a second (37 MiB resident), 2.0 % at 16; 1 region 0.13 % at 5; 16 regions 1.7 % at 5. At 3840x2160 with 16 regions of a quarter screen it is 2.8 % at 5 ticks a second (and the resident set follows the frames PipeWire maps, 146 MiB). Nothing runs while it is off.

Verified here: the pure core, the plan (including a three-monitor layout with an unknown scale), the daemon's side against a fake portal and a fake helper, the helper against a real private PipeWire producer behind the real sandbox, the import of Triggers packs with valid and hostile `.npz` frames (the automation sweep), the real portal up to its dialog in a nested KWin (`screenwatch_nested`). Not verified until the first real session: the portal's dialog on KDE, packs from real authors, a fullscreen game that bypasses the compositor, GNOME's and wlroots' portals.

## Packs

An imported `.mdaa` package gives the automation two things, both on request and both **disabled**:

* **A scene as a timeline** (`murasakictl timeline from-scene ITEM N`, `TimelineFromScene`): the scene of the action table as a one-step timeline of the item (`show` the animation and
  `place` it where the scene puts it, or `hide`). It does not restore (it would undo itself at once). Made again it is replaced, not doubled.
* **A Triggers pack** (a package with `pack.json`; `murasakictl timeline from-pack ITEM`, `ImportPackAutomation`): the importer reads `pack.json` and `meta.json` as **data** (never
  the captured frames `.npz`, never a path) into the record, and the call makes one timeline named after the pack and two triggers, a `fire` trigger named after the pack and a
  `screen` trigger that keeps the regions and the match settings and never fires (`automation: screen triggers are not active`). Nothing runs until you enable a trigger.

How a pack maps: Anima's times are absolute (`t_ms`) and its tracks loop while the screen trigger holds; the timeline plays once, with `wait`s between the actions. Each animation
takes two tracks, what is shown and when (`show`, `hide`) and where it moves (`move`; a duration above zero is a smooth move, zero a jump), so that a long move does not hold back a
hide. `place` and `wait` do nothing in Anima's runner and are dropped; a `sound` is dropped too, because its path is the author's machine's (only the file name is kept in the
record, `mdaa: sound not followed`); `speed_mult` of `meta.json` is the timeline's speed. Positions are the **centre** of the animation there: the converted `place` and `move` actions carry `anchor: centre`, and the daemon resolves them with the pet's real size when the timeline
runs (half the width and half the height back, with the width the pet is shown at and the aspect of its animation). Anima's base position is the move at time 0, else the first move: the pet
stands there from the start (a jump at time 0 is added when the first move comes later). `anim_speed_mult` of `meta.json` is not applied (set the speed of the pet itself) and the
animation's track is not repeated while the screen trigger holds (the timeline plays once); the notes say both. A track drives `{item, variant}`: it needs a pet showing that animation, so add one pet per animation of the pack; a track with no such pet is skipped. The import
refuses nothing for a bad `pack.json`: the animations come in, the pack is dropped with `mdaa: pack refused`.

## What replaces `core.exe`

Anima's action table runs when something writes `1` into `activador.act`, and the only writer is a program the package ships. Murasaki gives the same to anything **you** run:
`murasakictl triggers fire NAME` (or the `FireTrigger` call), a file `touch`ed in the signal directory, or a D-Bus signal. A package never ships code to run; it ships a
description. Importing a package that has a `core.exe` still says `mdaa executable not run`, and nothing here changes that.

## What is read

| Thing | By | Read |
|---|---|---|
| Open window classes, the focused class, which output has a fullscreen window | the surface (already, for the wallpaper) | class names only (at most 64 of 128 characters), no titles |
| Battery, GameMode, the lock | the surface and the daemon (already) | a yes/no |
| Idle | the surface (the compositor's idle notification, always on where the compositor offers it) and the daemon (logind's hint, only with an enabled `idle` trigger) | a yes/no; never which key or where the pointer is |
| Keys | the compositor, through the shortcut portal or kglobalaccel | only the activation of keys **you** gave to a trigger |
| Signal files | the daemon | their names and times, never their contents; links, directories and other names are ignored |
| D-Bus signals | the daemon | interface and member (and the sender's names for a namespace), never the arguments |

Not read: the screen, the keyboard otherwise, the microphone, the network. Mouse and gamepad buttons are not supported (Wayland has no global hooks).

## Who can call it

`FireTrigger`, `TestTrigger` and every other call here are calls on the user's session bus: **any program the user runs can make a trigger fire** (that is what `FireTrigger` is
for), and also create, change or delete triggers and timelines like `murasakictl` does. The closed lists are the protection: whatever calls, a trigger can only act on pets. A
`dbus` trigger's `senderNamespace` is a filter, **not authentication**: any program can own a name in the namespace. The daemon matches it against a table of the names it saw
the bus assign (kept from `NameOwnerChanged`) and never asks the bus while a signal arrives.

A bad `triggers.json` (or `profiles.json`, `timelines.json`) is set aside whole as `<file>.bad-<time>`: one bad entry costs the file, not only the entry (the rule of `pets.json`).

## Names

These are states and degrades, never crashes.

| Name | Meaning |
|---|---|
| `profile: item missing` | an entry's item is gone; skipped |
| `profile: variant missing` | an entry's animation is gone; skipped |
| `profile: too many pets` | a profile of more than 64 pets |
| `trigger: rate limited` | dropped by the rate limit |
| `trigger: cooldown` | dropped inside the trigger's cooldown |
| `trigger: chain too deep` | cut by the loop guard |
| `trigger: disabled` | the trigger or the master switch is off |
| `trigger: source unavailable` | the facts the source needs do not exist on this session |
| `trigger: shortcut unbound` | the desktop holds no key for the shortcut |
| `timeline: too many running` | eight run already |
| `timeline: pet not found` | no pet for a track |
| `timeline: too long` | a track lasts more than 24 hours |
| `automation: file refused` | a store file was not usable and was set aside |
| `automation: screen triggers are not active` | a `screen` trigger is kept and never fires |

## Command line

```
murasakictl profile list | save NAME | switch NAME | rename OLD NEW | delete NAME
murasakictl triggers list | state | fire NAME | enable|disable|remove|test TRIGGER
murasakictl triggers add NAME SOURCE-KIND ACTION-TYPE [source.KEY=V] [action.KEY=V] [cooldownMs=N] [mode=hold] [enabled=off]
murasakictl timeline list | show NAME | run NAME | stop NAME | delete NAME | set NAME FILE|-
murasakictl pets runtime clear [PET]
```

Wrong values exit 2; a refused spec exits 1 and says why. Examples: `murasakictl triggers add focus app-focus hide-all source.app=firefox*`; `murasakictl triggers add night
time hide-all source.at=23:30`; `touch $XDG_RUNTIME_DIR/murasaki/triggers/sleepy` with a `signal-file` trigger named `sleepy`.

## Safety and privacy review (phase 7)

What was checked, and by what, so that the claims above are not only words:

* **No action starts a process, opens a file or a web address.** The nine actions are a closed `switch` in `AutomationManager::runAction`; the hooks they call are ten
  members of `AutomationHooks`, all about pets, the clock or the item check; `tst_automationclosed` fails if an action, a source, a timeline action or a hook is added without
  being named there, and scans the sources of the automation code for `QProcess`, `QDesktopServices`, `QUrl`, network classes, `system()`, `popen`, `exec*`, `fork`, `dlopen`,
  `QScreen`/`grabWindow` and the clipboard. The sweep's `no_children` case and the nested run check that the daemon never had a child process.
* **Nothing reads the screen, the keyboard or the microphone.** The sources are the list above; the keys arrive only as the desktop's own activation of a shortcut the user
  gave to a trigger (`tst_globalshortcut`, `tst_triggershortcuts`); the facts are class names, never titles; a `screen` trigger only keeps data.
* **Imported things are inert.** A Triggers pack's frames are never opened, its sound paths are reduced to a file name and reported, its triggers are made disabled and the
  `screen` one can never fire (`tst_mdaaimport`, `tst_packautomation`, `tst_murasakiservice`); a hostile `pack.json` costs only the pack (a seeded and a hand-made case).
* **Hostile files and calls are refused by name and leave nothing half written, and the limits hold** (`automation_sweep`: 23 hostile triggers, 15 hostile timelines, seven bad
  store files set aside, the caps of 100 / 100 / 50 / 16 / 256). **The one-call way out works under load**: the rescue stops every timeline and clears the layer
  (`tst_automationmanager`, `overlay_nested`), and the master switch `automationEnabled` stops everything.
* **Idle cost is zero**: a daemon that was never asked for anything has two threads, no store file, no child and no CPU (`automation_sweep`, `fresh_install`). What the
  automation adds when nothing is enabled is nothing; the daemon's and the surface's own watchers (the lock state) exist regardless.
* **Two ways out, and what each does.** The rescue (`pets rescue`) stops every timeline, clears the runtime layer **and pauses the triggers for a minute**, so a loop of triggers can
  not undo it; the master switch `automationEnabled` stops the triggers and every running timeline (restoring what they touched) and keeps nothing subscribed. Neither deletes
  anything the user made.
* **A loop of triggers is cut** whatever its period under two seconds (4 hops), and a fact trigger can not fire more than 30 times a minute (`tst_triggerengine`).
* **Pack limits**: `pack.json` and `meta.json` are read up to 4 MiB each, 16 animations, 256 actions per animation and 64 regions with 256 points each; a package's `source.json` is
  opened without blocking and only if it is a plain file of at most 8 MiB. **The idle hint** is read from the session object logind names for this process (`GetSessionByPID`),
  not from the `auto` alias; Plasma never sets it (checked on a live session: `IdleHint=no` after a night away), which is why the surface's notification comes first.

## Measured (one machine, one session, `tools/compat/automation_sweep.py`)

A fire storm of 1 500 `FireTrigger` calls in 6.4 s (236 a second, a process per call, so the 10 000 a second of the unit test is not reachable from outside): 52 ran, within the
limit of 20 a second (147 allowed), the rest were `trigger: cooldown` or `trigger: rate limited`; the daemon's CPU was back to 0.00 % and its thread count did not grow. 300 profile
switches that add and remove a pet took 2.2 s of wall time and 0.3 s of CPU, and the daemon's resident memory moved from 25 028 KiB to 25 032 KiB. The nested KWin run
(`tools/test-overlay-nested.sh`, "automation") plays a real timeline: the pet is hidden (the compositor draws the backdrop where it was), comes back by itself, glides to another
place while the stored place stays, is hidden by a signal-file trigger, comes back with the rescue, and keeps its id through a profile switch.

## Verified and not

*Verified here*: a nested KWin run with a real timeline, a trigger, a profile switch and the rescue; the sweep; the pages of the GUI loaded with data (`tst_automationpages`); the models and their limits (unit tests and a seeded mutation run), the stores (round trip, quarantine, caps), the profile switch (including a property test),
the timeline runner (a property test: a finished run leaves nothing hidden), the trigger engine (scripted events, edges, cooldown, a 10 000-fire storm, the loop guard, missed times),
the daemon's manager, the D-Bus calls over a private bus, the signal-file watcher, the facts over a fake surface, the overlay's runtime layer, the portal and kglobalaccel
shortcuts against fakes.

*Idle*: the source of the idle is, in order, the surface's `ext-idle-notify-v1` notification (asked with `get_idle_notification` after 60 s without input, on the compositor's seat; it also counts an idle inhibitor as activity, so a video in a player that inhibits is not "away") and logind's `IdleHint`; once the surface has answered, only its answer counts (while it restarts the idle is unknown, so logind's "not idle" does not end a nap), and the first answer of a source, or the surface taking over, is a starting point and fires nothing. The clock counts a suspend (`CLOCK_BOOTTIME`). `minutes` is counted from the last input: the notification comes 60 s after it and the daemon takes those 60 s off. Verified here: the client against a fake compositor (`tst_waylandidlemonitor`: the request, `idled`, `resumed`, an idle already there, no protocol, no seat), the facts JSON (`tst_surfacecontroller`), the merge with logind and the engine's away/back (`tst_triggerengine`, `tst_automationmanager`). *Not verified*: what a real KWin sends (that it sends `idled` and `resumed` on Plasma 6, and when the lock screen or a Murasaki web wallpaper holding a wake lock counts as an inhibitor), and logind on a desktop that does set the hint.

*Not verified here*: MPRIS, live `app-open` on Hyprland/sway/COSMIC/GNOME, the portal's key dialog on GNOME, a real game's focus events,
`kglobalaccel` on a real Plasma session, Triggers packs from real authors.

*Not in this version*: a track that names an item with no pet does not make a pet for the run (it is skipped); a `sound` action plays the pet's own sound at its own volume
(no per-action volume or asset); `stop-sounds` stops what plays once, not a `continuous` sound; screen matching; a pack's track is not repeated while its screen trigger holds, and its
`anim_speed_mult` is not applied.

## First real session

Save two profiles and switch between them; add a focus trigger on a real application (`triggers add`) and focus it; a `time` trigger a minute ahead; a shortcut trigger and
give it a key in System Settings; `touch` a signal file; lock the screen with a `session` trigger; and `pets rescue`.

Screen triggers, the live part (nothing of it can be answered in a nested session, because the desktop's dialog needs a person; `tools/test-screenwatch-nested.sh` stops at `asking`):
turn on `screenTriggersEnabled` and make a screen trigger (a rectangle over something that appears on your screen, "Capture now" for its reference, a timeline such as a pet waving);
press Start and answer the desktop's dialog (check that its own indicator shows, that the notification "Murasaki is watching N regions of your screen" appears, and that sharing only the
screen you chose shows frames); bring the picture up and see the timeline play and, when the picture goes, stop; put a pet over the region and see `screenIgnorePets` skip it; lock the session
and unlock it (watching resumes without a new dialog, and the desktop's indicator stays on while locked until Stop); press Stop and `pets rescue` while it runs; turn `screenRemember` on, Stop,
Start again (no dialog) and off again (the next Start asks); end the share from the desktop's own indicator (state `stopped`); a game in fullscreen (a game that bypasses the compositor may not be
in the stream: the trigger then just never passes); GNOME's and wlroots' portals; a real Triggers pack with its `.npz` frames (`screen: ready`), and one made at another resolution (regions
outside the shared screens are named); the cursor modes (`AvailableCursorModes`: the reference must not carry the cursor).
