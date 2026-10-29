# User shortcuts (application icons and launching) for murasaki-scene-vk

Status: APPROVED by the lead (2026-10-21) with the decisions at the end; phase 2 in progress (the host parts that do not need the media branch are built: `src/shortcut/`, the property type and the GUI picker). Goal: the 23 library scenes that degrade as `user shortcuts`
(26 items declare a `usershortcut` property) show the icon of the application the user picked for each slot, and a click on that icon
starts the application, as WE does.

## Facts found

- **Property.** In `project.json` a shortcut is a property `{"type": "usershortcut", "value": "", "text": ..., "condition": ...}` (checked in
  3577990983, 3595252638, 3733754206 and 23 more). The user fills it with an application; `value` is empty until then. The scenes name
  them `launcher1`..`launcher32`, `s01c`.., `l1`.., `next`/`pause`/`previous`, ...
- **Icon.** The scene binds the property as a texture slot: `usertextures` entry `{"name": "<property>", "type": "usershortcut"}`
  (docs/scene-vk/formats.md, 3 / 1 / 21 items). The slot's own listed texture (`default_dock_icon01`..) is what shows today and what WE
  shows until a choice is made. The engine has no source for it (docs/scene-vk/formats.md, "What shows with nothing filling a slot").
- **Launch.** The scripts call `engine.openUserShortcut(name)` from `cursorClick` (read in 3595252638 and 3733754206). `name` is the
  layer's name lowercased with spaces removed (`formattedName = layerName.toLowerCase().replace(/ /g, '')`), which is the property key
  (`launcher1`); the click handler also checks `thisLayer.visible`, and 3733754206 only launches for a press shorter than 0.33 s.
  The scene therefore chooses a **property**, never an application. Today the prelude's `openUserShortcut(name)` returns false
  (docs/scene-vk/scripts-api.tsv: "no User Shortcut property on this platform").
- **Where things run.** The engine is in `murasaki-sandbox` (landlock/bwrap): no session bus, no network, no `/usr/share/applications`
  worth reading, no process spawning. The host (GUI, surface, daemon) is the only side that can read icon themes and launch programs.
  The bridge (SOCK_SEQPACKET, `MsgHeader` + payload, fds by SCM_RIGHTS) is the only host-engine channel. F's media channel
  (feat/univ-media, read-only: `src/bridge/mediaview.h`, `src/media/`) is the pattern: Murasaki extension messages 44 `Media`, 45
  `MediaThumbnail` (fixed 16-byte struct + one sealed memfd of RGBA8, at most 512 x 512), 46 `MediaWants` (producer to consumer),
  Hello version 4, `UniqueFd sealedPixelsFd()` / `readSealedPixels()` which check size and seals, a host hub (`MediaHub::shared()`),
  a pure-function policy file (`mediapolicy.*`) with the limits, and the rule that the engine treats everything as untrusted.
- **Host property pipeline.** `src/core/weproperty.*` parses `project.json` property types (no `usershortcut`: `Type::Unknown`);
  `src/app/propertymodel.cpp` lists editable types (`slider bool color combo textinput file directory`) and
  `src/app/qml/PropertyEditor.qml` has one delegate per type; values the user sets are stored per assignment and sent to the engine as the
  `Properties` JSON. So the GUI work is a new type, a new delegate and a value that never goes to the engine as it is.

## Threat model

The scene is untrusted Workshop content with full control of its scripts. It must not be able to: start a program the user did not
assign, start one without the user acting, start one with arguments, flood the user with launches, learn which applications are
installed beyond the icons the user chose to put in this wallpaper, or make the host read arbitrary files. The application list, the
desktop entries and the icon files are also untrusted input of the host (any local program can write a `.desktop`), so they are parsed
with limits.

## Design

### Overview

```
GUI (PropertyEditor)  -- picks an application, stores its desktop id as the property value (host side only)
host (ShortcutHub)    -- resolves the id to a desktop entry, rasterises its icon, sends ShortcutIcon (+ the property's value as an opaque token)
engine (renderer)     -- binds the icon to the slot's texture; on openUserShortcut(name) sends ShortcutLaunch
host (ShortcutHub)    -- checks the request against its own table, a recent real click and rate limits, then launches the entry the user chose
```

### 1. Application catalogue and the editor (GUI process, `src/shortcut/` + `src/app`)

- `shortcut::DesktopEntry` (new, own parser, bounded): reads `*.desktop` from `$XDG_DATA_HOME/applications`, each of
  `$XDG_DATA_DIRS/applications` and the Flatpak/Snap export directories (`~/.local/share/flatpak/exports/share/applications`,
  `/var/lib/flatpak/exports/share/applications`, `/var/lib/snapd/desktop/applications`), non-recursive plus one level of subdirectory
  (the spec's `a-b.desktop` id from `a/b.desktop`). Limits: a file at most 64 KiB, regular files only (no symlinks out of the tree: the
  real path must stay under a root it was found from), at most 4096 files and 1024 listed entries, 30 ms-bounded batches off the GUI
  thread. Keys read, from `[Desktop Entry]` only: `Type` (must be `Application`), `Name` (and the locale's `Name[xx]`), `Icon`,
  `NoDisplay`, `Hidden`, `OnlyShowIn`/`NotShowIn` (against `XDG_CURRENT_DESKTOP`), `TryExec` (must resolve to an executable in
  `PATH`). `Exec` is **not** read for launching (see 4); it is not even stored. Strings are cut to 256 UTF-8 bytes with control
  characters removed (as `media::cleanText`). The id is the desktop-file id of the XDG spec (`org.mozilla.firefox.desktop`).
- Alternative considered: KService/KIO (`KApplicationTrader`). Not used: KDE Frameworks are not a dependency of the GUI and the GNOME and
  wlroots hosts would not have them. QSettings-style parsing of `.desktop` is not used either: its escape rules differ from the spec's
  and it follows `Exec` field codes nobody here needs.
- `ShortcutCatalog` (new, `src/app`, a `QAbstractListModel`): id, name, icon (an `image://appicon/<id>` provider that rasterises with
  the same code as the engine path, 48 px), sorted by name, a text filter. Refreshes when the applications directories change
  (`QFileSystemWatcher` on the roots, debounced 1 s) and when the GUI opens a `usershortcut` editor.
- `PropertyModel`/`PropertyEditor.qml`: `usershortcut` joins `isEditableType`; the delegate is a button with the chosen application's icon
  and name (or "None") opening a popup with a search field and the catalogue, and a "Clear" entry. `WeProperty::Type::UserShortcut` is
  added to `weproperty.*`. The stored value is the **desktop id** (a string), in the same per-assignment store as the other properties.
  A stored id that no longer resolves (application uninstalled) shows as "Missing application: id" and counts as empty for the engine.

### 2. What the engine is told (host-side rewrite of the `Properties` JSON)

The host never sends the desktop id to the engine. For each `usershortcut` property of the wallpaper (the host knows them from
`project.json`): empty or unresolvable -> `""`; resolved -> the entry's cleaned `Name` (so a script that reads the property sees
something truthy and readable, as WE's value is not an opaque handle; assumption: nothing in the library reads it, only the `usertextures`
slot and `openUserShortcut(name)` use it). Applying the rewrite lives where the host builds the message today (`BridgeClient`'s
properties path); the engine's own validation of the property JSON is unchanged.

### 3. Protocol: Murasaki extension `src/bridge/shortcutview.h`, Hello version 5

Like `mediaview.h`: kept out of `protocol.h`, a producer ignores unknown types, and a consumer sends nothing to a producer that
announced less than `kShortcutVersion = 5` (version is monotonic: 5 includes 4's messages). Type numbers are proposals to agree with F
(44-46 are taken): `47 ShortcutIcon` (consumer -> producer) and `48 ShortcutLaunch` (producer -> consumer).

- `ShortcutIcon` (consumer -> producer), a fixed struct and at most one fd:
  `{u32 width, u32 height, u32 generation, u32 reserved, char name[64]}` (80 bytes, `has_unique_object_representations`) + one sealed
  memfd of exactly `width * height * 4` bytes, RGBA8 straight alpha, each side 1..`kMaxShortcutIconSide` = 256. `name` is the property
  key, UTF-8, NUL terminated inside the field, 1..63 bytes, no control characters. Width and height 0 and no fd: the slot has no icon
  (cleared): the engine goes back to the scene's listed default. `validate()` follows `validate(MediaThumbnail)`; the pixels are read with
  the existing `readSealedPixels` (regular file, exact size, shrink/grow/write seals), copied and closed. Property keys longer than 63
  bytes are not sent and are logged once by the host. At most 64 slots per scene (the library's maximum is 32); a rejected message is
  dropped and never a reason to kill the engine.
- `ShortcutLaunch` (producer -> consumer): `{u32 serial, u32 reserved, char name[64]}` (72 bytes), no fd. `name` is the argument of
  `openUserShortcut`, cut to 63 bytes by the engine (an over-long one is not sent: the script's call returns false). The host answers
  nothing: the script's call returns `true` when the engine has the property assigned and the request was sent (assumption: WE's return
  is "an action was performed"; the library never uses it), `false` otherwise.
- No `Wants` message: icons go only for slots the user filled, so there is nothing to gate. The engine ignores an icon for a name that
  is not one of its `usertextures` slots (counted, logged once).

### 4. Host: `ShortcutHub` (src/shortcut, one per host process, like `MediaHub`)

Pure-function policy in `shortcutpolicy.*` (testable without Qt widgets or a bus): limits, name checks, the click window and rate
limits below.

- **Icon.** `Icon=` is either an absolute path or a theme name. Theme name: `QIcon::fromTheme(name)` then `pixmap(256, 256)` (SVG via Qt's
  SVG icon engine when present); absolute path: only a regular file, at most 4 MiB, inside the entry's own directory tree or an icon
  directory (`/usr/share/icons`, `/usr/share/pixmaps`, `~/.local/share/icons`, Flatpak/Snap export icons), no symlink leaving them;
  decoding with `QImageReader` after a size check (at most 4096 per side, 16 Mpixel, allocation limit 128 MB), on a worker thread.
  The result is scaled to fit 256 x 256 keeping the aspect, centred on a transparent 256 x 256 canvas, converted to RGBA8 straight
  alpha (262 144 bytes), sent with a new `generation`. No icon found: the generic `application-x-executable`, and failing that, none
  (width 0). An icon the host cannot decode within 1 s is dropped.
- **When.** After the engine's Hello (replayed on an engine restart), on every change of the assignment, on an icon-theme change
  (`QIconLoader` / `QGuiApplication::paletteChanged` is not enough: the theme name from `QPlatformTheme` is watched). At most one
  `ShortcutIcon` per slot per second; cached by desktop id and size (LRU 64).
- **Launch.** `ShortcutLaunch` is accepted only if **all** hold, else dropped (and logged once per wallpaper per reason):
  1. the wallpaper's setting `shortcutsLaunch` is on (proposal: on by default; one switch in Settings, "Let wallpapers start the
     applications I chose");
  2. `name` is exactly one of the `usershortcut` keys of this wallpaper's `project.json` and its assigned value resolves to a catalogue
     entry now (the id is re-resolved against the installed entries at launch time);
  3. a real user button press was delivered to this engine within the last 0.75 s (the host sees every `Pointer` message it sends; a
     synthetic or scripted event inside the engine cannot make one) and the pointer is over the engine's surface;
  4. at most one launch per 2 s per wallpaper and 6 per minute overall; the same application is not started again within 5 s;
  5. the wallpaper is the one the user sees (not paused, not a hidden output).
  The application is started with **no arguments from the scene**, by the first of these that exists (`shortcut/launcher.*`):
  `gio launch <real path of the entry>` (GIO applies `Exec`, field codes, DBus activation, `TryExec` and Flatpak's `flatpak run` through
  the exported entry); else `gtk-launch <id>`; else, as the last resort, the entry's own `Exec` line read as the Desktop Entry
  Specification says: split on spaces, double quotes with the escapes `\" \` \$ \\` inside them, `%%` a percent sign, the field codes
  `%f %F %u %U %i %c %k` (and the deprecated `%d %D %n %N %v %m`) **dropped and replaced by nothing**, no shell, and the line refused
  (nothing starts, the reason logged) for an unterminated quote, an unknown `%` code, a reserved character outside quotes, an empty
  result, more than 64 arguments or 4096 bytes, a program that is not found, or an entry with `Terminal=true`. Started through
  `QProcess::startDetached` (argv, a new session, no stdio, `GIO_LAUNCHED_DESKTOP_FILE*`, `DESKTOP_STARTUP_ID` and
  `XDG_ACTIVATION_TOKEN` removed from the environment). The scene's string never reaches a path, a command line or the environment.
- **Where it runs: the daemon, not the surface.** The process that receives `ShortcutLaunch` is the one that hosts the engine, the
  surface (`murasaki-surface`), which also holds the compositor connection and the bridge's attack surface. It does not start
  programs. It applies what only it can know (a real button press reached this engine; the wallpaper is shown) and then asks the daemon
  (`murasakid`, the user's service: it owns the settings and the assignments, has no Wayland or X connection) over D-Bus,
  `LaunchShortcut(wallpaper, key)`. The daemon is the one that checks the switch, re-reads the assignment of the property from its own
  store, resolves the entry again, applies the global rate limit (one process, so a real global count), and launches. A flaw in
  reading or starting a program therefore cannot touch compositor state, and a compromised engine cannot reach the daemon except
  through the surface's validated message. The GUI's wallpaper preview (`render/sceneitem.cpp`) does not launch anything.
- **Failure** (entry gone, `gio` fails): nothing visible to the scene; a line in the host log.

### 5. Engine (`src/scene-vk`)

- `bridgesink` decodes both messages (validators as above) into `ControlMessage` alternatives; the scene runner keeps a table
  property key -> dynamic GPU texture (like `$mediaThumbnail`): created at the first icon, replaced in place on a new generation,
  dropped on a clear (the slot returns to the listed texture). At most 64 textures of 256 x 256 (16 MiB) per scene.
- `usertextures` entry `{"type": "usershortcut", "name": K}` binds the texture of K when it has one; otherwise the slot keeps the
  texture the scene lists there (today's behaviour, which is what WE shows with nothing chosen). A layer using the slot is redrawn when
  its icon changes (a texture revision, as video textures do). The renderer's `drawing without: user texture <property>` log line and
  the `Scene::userTextureProperties` bookkeeping go away for this type.
- `engine.openUserShortcut(name)` (prelude + `scripthost`): returns false when `name` is not a string, is longer than 63 bytes, or the
  property is unassigned (the engine knows from the property JSON: non-empty value); otherwise it queues a `ShortcutLaunch` (at most
  4 pending, 1 sent per 500 ms: the engine's own flood guard; the host's limits are the ones that count) and returns true.
  scripts-api.tsv: `openUserShortcut` becomes `partial` ("sends the request to the host, which starts the application the user chose
  only after a click and within its rate limits").
- `--check`: the `user shortcuts` degrade goes away when the slots can be fed (the engine runs under the host); standalone it is
  unchanged. (Same handling as F's `media thumbnail`; to be aligned with what feat/univ-media does.)

### 6. Limits (all in `shortcutpolicy.h` / `shortcutview.h`)

| What | Limit |
|---|---|
| desktop file size / files scanned / entries listed | 64 KiB / 4096 / 1024 |
| strings from a desktop entry | 256 UTF-8 bytes |
| icon file / decoded picture | 4 MiB / 16 Mpixel, 4096 per side |
| icon sent | at most 256 x 256 RGBA8 (256 KiB); one per slot per second |
| slots per scene / name length | 64 / 63 bytes |
| launch: click window / per wallpaper / overall / same app | 0.75 s / 1 per 2 s / 6 per min / 5 s |
| engine -> host launch requests | 4 pending, 1 per 500 ms |

## Security summary

The scene can ask for a property key. The host maps it with its own table, to the entry **the user** chose in the GUI, only after a click
the host itself saw, at a bounded rate, with no argument, with a switch the user controls, and never from text of the scene. The engine
never learns an application path or id (only a display name) and has no process or network access added. The host parses desktop
entries and icons as hostile (bounded size and count, real-path checks, worker thread, decode limits). A wrong or malicious entry in
the user's own applications directories is outside this model (it can already be launched by the user without the wallpaper).

## Tests (phase 2)

- `tst_shortcut_policy`: desktop entry parser on valid, hostile (huge, no group, duplicate keys, binary, deep paths, symlink escapes) and
  locale cases; id derivation; `OnlyShowIn`; `TryExec`; cleaning and limits; the launch gate (click window, rate limits, unknown name,
  unassigned, switch off), pure functions with a fake clock.
- `tst_bridge_shortcut`: struct sizes, `validate()` (name checks, width/height, fd counts), sealed fd round trip, wrong size, wrong
  seals, more than one fd; unknown type ignored by an old consumer.
- `tst_scenevk_shortcut` (engine): a `usertextures` slot takes the icon, a clear returns the default, a rename or a name that is not a slot
  is ignored, 64-slot cap, a redraw on a new generation, `openUserShortcut` return values and the flood guard, with a fake sink.
- GUI: `PropertyModel` lists `usershortcut`, stores the id, shows a missing one; the catalogue model with a fixture directory.
- Launch: a fake launcher (injected function) records what would be started; tests assert argv has only the entry path.
- Scenes: `--check` and a snap of 3595252638 and 3733754206 with a fixture catalogue (icons from the test theme).

## Phases

1. This document (approved by the lead).
2. After the approval and F's merge of `feat/univ-media`: `shortcutview.h` + validators and tests; `src/shortcut/` (parser, policy, icon
   rasteriser, hub); engine binding and `openUserShortcut`; GUI type, delegate, catalogue; docs (formats.md, scripts-api.tsv, security.md,
   settings). Order: protocol and engine first (testable with a fake host), then host, then GUI.

## Open questions for the lead

1. Message numbers 47/48 and `kShortcutVersion = 5`: agree with F (and whether Hello can announce a single "version" monotonically).
2. `shortcutsLaunch` default: on (WE starts the application on a click; the user chose it) or off (one more step for users)? Proposal: on.
3. Should a click on an unassigned slot do anything (WE: probably opens its settings)? Proposal: nothing.
4. The value the engine sees for an assigned property: the application's display name (proposal) or a constant such as "1".
5. `gio launch` as the launcher with an own `Exec` fallback (proposal) or only `gio` (smaller attack surface; fails where glib's tools are absent).

## Decisions (2026-10-21, lead)

1. Message types 47 `ShortcutIcon` and 48 `ShortcutLaunch`, Hello version 5, above F's 44-46 and version 4; the table of the Murasaki
   extension's types goes in `src/bridge/mediaview.h` (or wherever F keeps it) when the media branch is merged, so no number is reused.
2. `shortcutsLaunch` is **on** by default, in Settings next to the media ones, with a clear text ("Let wallpapers start the applications I chose").
   It is added with the bridge work (it touches `src/core/settings.cpp` and the daemon's settings, which feat/univ-media also changes).
3. A click on an unassigned slot does nothing.
4. The engine receives the application's display name, at most 64 bytes, with control and bidirectional characters removed
   (`shortcut::cleanDisplayName`); never the id or a path.
5. Launch order: `gio launch <path>`, then `gtk-launch <id>`, then the own `Exec` reader as the last resort (conforming to the
   specification, field codes dropped, no shell, hostile tests); if there is no safe way nothing starts and the reason is logged.
   Flatpak works through the exported entry (gio resolves `flatpak run`). The launch runs in the daemon, not in the surface (above).

## Built (phase 2, complete)

- `src/shortcut/`: `desktopentry` (the reader and scanner), `shortcutpolicy` (keys, names, the gate, the limiter, the daemon's policy), `launcher` and `launchassigned` (planning
  and starting with an injected launcher), `projectkeys`, `shortcutcatalog` (the GUI list); and the host half `iconrender`, `shortcutitem` (properties rewrite, icons, the
  launch decision, one per SceneItem) and `daemonlauncher` (the D-Bus call).
- `src/bridge/shortcutview.h`: messages 47 and 48, Hello version 5, with the table of the extension's types also in `mediaview.h`; `BridgeClient::sendShortcutIcon`,
  `shortcutLaunchRequested`.
- Engine: `BridgeSink` decodes the icon and sends the launch request; `SceneRenderer::setShortcutIcon` keeps `$shortcut:K`; `MediaBinding::shortcut` binds the slots; the `user shortcuts` degrade is gone;
  `engine.openUserShortcut` asks through a handler (at most one request a half second).
- Host wiring: `BridgeItem` (`propertiesForEngine`, `propertiesApplied`, `buttonPressed`), `SceneItem` (the `ShortcutItem` of the item), `SurfaceController` (the setting).
- Daemon: `LaunchShortcut`, the setting `shortcutsLaunch` (default on) and the switch in Settings.
- Tests: `tst_shortcut` (policy, entries, launcher, catalogue, daemon policy), `tst_shortcuthost` (properties, icons, hostile icon files, the gate with a real bridge),
  `tst_scenevk_shortcut` (scripts, bindings, wire validators), `tst_bridgeclient`, `tst_scenevk_render` (the slot), `tst_murasakiservice`, `tst_settingsstore`.
- Docs: `docs/security.md`, `docs/dbus-api.md`, `docs/scene-vk/formats.md`, `scripts-api.tsv`.

## Changes after the security review (2026-10-21)

- `LaunchShortcut(output, desktop, itemDir, key)`: the daemon asks `resolve(output, desktop)` for the one assignment shown and requires that it shows the item and stores the key;
  the surface passes the desktop it asked about (`content.desktop`, `SceneItem::desktop`). A test has competing wildcard / output / desktop assignments.
- The click window is 750 ms. `docs/security.md` says the click gate is the surface's and the daemon trusts the session bus, and that the sandboxed engine has no bus.
- Desktop ids that start with `-` are refused and `gtk-launch` gets `--` before the id. A quoted field code alone (`"%u"`) is no argument.
- Icon files: png, jpeg, webp and gif only. Theme icons: found from the theme's files (inheritance, sizes, pixmaps; SVG through QtSvg when built) and decoded on the icon worker (one thread
  of its own, answers delivered through an object on the GUI thread); no `QIcon` outside the GUI thread (it is not safe there).
- Control characters in `itemDir` and `key` are refused before anything is logged; `cleanDisplayName` also drops tag characters and variation selectors and cuts runs of combining marks.
