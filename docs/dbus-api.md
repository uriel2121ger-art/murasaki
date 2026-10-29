# Murasaki D-Bus API (v1, revision 23)

- Bus: session
- Service: `io.github.uriel2121ger_art.Murasaki`
- Object: `/io/github/uriel2121ger_art/Murasaki`
- Interface: `io.github.uriel2121ger_art.Murasaki1`

The `1` suffix of the interface name is the major API version: a breaking change would introduce `Murasaki2`. `ApiVersion` is the compatible revision within that major version (currently `22`) and only grows when backwards-compatible additions are made. Clients must read `ApiVersion` on connect and refuse to work if the interface they need is missing. Items are JSON strings, and new fields may appear at any time, so ignore unknown keys.

## Properties

| Name | Type | Meaning |
|---|---|---|
| `ApiVersion` | `u` | API version, currently `22` |
| `PetsArrange` | `b` | Arrange mode of the desktop pets (see Pets API); not persisted, `false` after a daemon restart |
| `PetsHidden` | `b` | Revision 12. The desktop pets are hidden by `SetPetsHidden`; not persisted, `false` after a daemon restart |
| `WallpaperInteract` | `b` | Revision 13. The user is clicking on the wallpaper (see Interacting with the wallpaper); not persisted, `false` after a daemon restart, and never `true` while `PetsArrange` is |
| `PetsMuted` | `b` | Revision 18. The pets' sounds are muted for this session (a trigger's `mute` action or `SetPetsMuted`); on top of the setting `petsMuted`; not persisted, `false` after a restart |
| `ActiveProfile` | `s` | Revision 18. The id of the profile that was saved or switched to last, or empty. A label: nothing is restored from it at login (docs/pets/automation.md) |
| `ProfileModified` | `b` | Revision 18. The pets on the desktop differ from `ActiveProfile` (the pure diff of the profile switch; skipped entries do not count) |
| `PetRuntime` | `s` | Revision 18. JSON `{petId: {hidden?, x?, y?, output?, motion?: {fromX, fromY, toX, toY, startMs, durationMs}, opacity?, variant?, tracking? (revision 22)}}`: what timelines, triggers and scenes changed about the pets without touching their stored placement (the runtime layer). `startMs` is `CLOCK_MONOTONIC` in milliseconds. Never stored: `{}` after a daemon restart |
| `ScreenWatchState` | `s` | Revision 21. JSON `{state: off\|asking\|watching\|stopped\|refused\|failed, reason, regions, ticksPerSecond, since, paused, notes: [text]}`: whether the screen triggers are being watched (docs/pets/automation.md, "Screen triggers"). `asking`: the desktop's dialog is open; `watching`: the helper runs (`paused`: the session is locked; the portal session stays open, so the desktop's sharing indicator stays on until `StopScreenWatch`); `stopped`: the desktop ended the share; `refused`: the user said no; `failed`: `reason` names it (`screen: helper crashed`, `screen: helper keeps crashing, turned off`, `screen: portal not available`, ...). `since` is wall-clock milliseconds, `notes` the problems of the regions (`screen: region outside the shared screens`, `screen: region has no reference picture`, ...). `{state: off}` on a fresh daemon. Never stored |
| `ShortcutTriggerIds` | `s` | Revision 18. JSON `[{id, description}]`: the enabled shortcut triggers (at most 8) that the overlay registers with the desktop next to its fixed shortcuts |
| `Status` | `s` | `ok`, `steam-not-found` or `wallpaper-engine-not-installed` |
| `WallpaperEngineAssetsDir` | `s` | `<WE install>/assets`, or empty |

`Status` and `WallpaperEngineAssetsDir` are evaluated once when the daemon starts; restart the daemon to refresh them.

## Methods

| Method | Returns | Notes |
|---|---|---|
| `ListItems()` | `s` | JSON array of item summaries, sorted by title |
| `GetItem(s id)` | `s` | Full item JSON. Error `org.freedesktop.DBus.Error.InvalidArgs` if unknown |
| `Rescan()` | — | Rescan now (normally automatic) |
| `SetAssignment(s output, s desktop, s itemId, s options)` | — | Assign an item to an (output, desktop) key (`itemId` may also be `playlist:<id>`, see Playlists). `output` is a connector name such as `DP-1`, `desktop` a compositor desktop id; either may be `*` (any). `options` is a JSON object `{muted?: bool, fill?: "fill"\|"fit"\|"stretch", span?: bool, properties?: object}` or empty. `span: true` (revision 7) draws one wallpaper across all outputs as a single canvas and is valid only with `output` `*`; `fill` then applies to the whole canvas (see Spanning). `Assignments()` and `ResolveAssignment` include `span` only when it is true. `InvalidArgs` for unknown items, pet items (kind `pet`), bad options, or keys that are empty or longer than 200 characters |
| `ClearAssignment(s output, s desktop)` | — | Remove that exact key; no-op if absent |
| `Assignments()` | `s` | JSON array of `{output, desktop, itemId, muted, fill, properties}` |
| `ResolveAssignment(s output, s desktop)` | `s` | `{"assignment": entry or null, "item": full item JSON or null}`. Precedence: `(output, desktop)`, `(output, *)`, `(*, desktop)`, `(*, *)`. `item` is null when the assigned item is no longer in the library or is a pet. For a playlist assignment see Playlists |
| `LaunchShortcut(s output, s desktop, s itemDir, s key)` | `b` | A click on a dock shortcut of a wallpaper asked to start the application the user chose for the `usershortcut` property `key` of the item in `itemDir`, shown on `output` while it shows `desktop` (the pair the surface asked `ResolveAssignment` about). Called by `murasaki-surface` after it saw a real button press and the wallpaper is shown; the daemon judges the rest from its own data: the item is in the library and declares `key` as a `usershortcut`, the one assignment `ResolveAssignment(output, desktop)` returns (precedence as there: a wildcard assignment or another desktop's never stands in for it; a playlist's current item counts) shows that item and stores a desktop id for `key`, the setting `shortcutsLaunch` is on, at most 6 launches a minute over all wallpapers and not the same application twice within 5 s. The application is the entry of that id in the applications directories (a hidden entry or one not for this desktop does not count) and is started with `gio launch <file>`, else `gtk-launch <id>`, else its own `Exec` line read as the Desktop Entry Specification says, with no argument that a scene wrote. Returns `true` when an application was started; the reason it was not goes to the daemon's log. `InvalidArgs` for an invalid `output`, `desktop`, `itemDir` or `key` (control characters included). Like every method of this interface it trusts the session bus: any process of the user can call it, and could as well start the application itself |
| `Settings()` | `s` | JSON object with every setting, defaults filled in: `{webFpsLimit: int, pauseWhenCovered: bool, presenter: string, sceneEngine: string, pauseOnGame: bool, batteryMode: string, pauseApps: [string], sandboxEngines: bool, requireSandbox: bool, sceneRenderScale: double, mediaRemoteArt: bool, mediaShare: bool, shortcutsLaunch: bool, petMaxFps: int, petSaveEnergy: bool, petsVolume: int, petsMuted: bool, petsMuteWhenFullscreen: bool, stageHideOverFullscreen: bool, petsHideOnGame: bool, stageMaxPixels: int, puppetMaxPixels: int, puppetCorePath: string}` (plus the import settings) |
| `SetSettings(s settings)` | — | Merge a JSON object of settings (partial updates are fine). `InvalidArgs` for malformed JSON, unknown keys, wrong types or out-of-range values; nothing is changed in that case. `org.freedesktop.DBus.Error.Failed` if the settings file could not be written; the values are then unchanged |

Revision 5 adds the pet calls below. Everything else is unchanged, so the GUI, the surface and the Plasma plugin keep working against a revision 4 daemon; only the pet features need 5.

| Method (revision 5) | Returns | Notes |
|---|---|---|
| `ListPets()` | `s` | JSON array of placements, in the order they were added |
| `AddPet(s itemId, s variant, s output, s options)` | `s` | Shows a pet; returns its new `petId` (uuid). `variant` is a variant `name`, or empty for the first. `output` is a connector name or `*` (the primary output). `options` is a JSON object or empty, with any of `x`, `y`, `width`, `opacity`, `playOriginal`, `paused`. `InvalidArgs` for unknown items, items that are not pets or have no playable variant, unknown variants, bad options, and beyond 64 pets. A heavy variant is queued for transcoding (unless `playOriginal`) |
| `UpdatePet(s petId, s changes)` | — | Merges a JSON object into the placement: `variant`, `output`, `x`, `y`, `width`, `opacity`, `playOriginal`, `paused`, `fps`, `speed` and the behaviour fields of revision 12 (other keys are ignored; `petId` and `itemId` never change). `InvalidArgs` for unknown pets, unknown variants, wrong types and out-of-range values; nothing is changed then. A change that alters nothing does not signal |
| `RemovePet(s petId)` | — | `InvalidArgs` if unknown |
| `TranscodeState(s itemId)` | `s` | `{itemId, variants: [{name, ..., heavy, transcode}]}`: the variants of a pet item, each with its transcode state (see below). `InvalidArgs` for unknown or non-pet items |
| `RequestTranscode(s itemId, s variant)` | — | Queues the conversion of one variant, or of every variant when `variant` is empty. Already queued, running or done variants are left alone; a failed one is tried again. `InvalidArgs` for unknown items or variants |
| `SetPetsArrange(b arrange)` | — | Switches arrange mode on or off. Signals only when the value changes |
| `SetWallpaperInteract(b interact)` | — | Revision 13. Switches the interaction with the wallpaper on or off (see Interacting with the wallpaper). Signals only when the value changes; never stored. `InvalidArgs` when `interact` is true while the arrange mode of the pets is on (arranging wins; turning arrange on turns this off). While it is on the daemon keeps `murasaki-overlay` running even without pets, since the catcher windows are the overlay's |
| `SetPetsHidden(b hidden)` | — | Revision 12. Hides or shows every desktop pet (the overlay unmaps them and pauses their playback). Signals only when the value changes; never stored, so a forgotten hide cannot survive a restart |
| `RescuePets()` | — | Revision 12. The way out of a forgotten flag, in ONE write of the pets file: every pet gets `clickThrough` off (so it can be grabbed, as Anima's rescue does), `keepAbove` on, `hoverHide`, `focusFade`, `smartDodge` and `locked` off, and `opacity` 1 if it was below 0.5, and (revision 17) `stage.mode` goes to `off` for a pet that is a stage (its other stage choices stay) and (revision 19) `tracking.mode` goes to `off` for a pet that looks at or follows the mouse (its other tracking choices stay); `rotation`, flips, positions, sizes, items and variants are untouched. The pets are shown again (`PetsHidden` false). It never turns arrange mode on. `PetsChanged` fires once, and only when a pet changed. If the write fails the call is an error and nothing changes, the hidden state included. Being click-through and possibly invisible, the pets cannot be reached with the pointer: this call (`busctl --user call ... RescuePets`) is the rescue |
| `AddLocalPetItem(s path)` | `s` | Makes a pet item of an animation file on disk (`.gif`, `.webp` or `.apng`, whose header must read as an animation) and returns its item id, `local:<sha1 of the canonical path>`. Nothing is copied or linked: the daemon only remembers the path (see Local pets). Adding a file that is known returns the same id. `InvalidArgs` for relative paths, missing or non-regular files, files over 2 GiB, other extensions and unreadable animations, and beyond 256 files; `Failed`-style errors do not apply, but if the list cannot be saved the call is rejected and nothing changes. `ItemsChanged` reports the new item |
| `ImportPetAudio(s path)` | `s` | Revision 15. Queues the conversion of a sound file (an absolute path of an mp3, wav, ogg, flac, m4a, aac, wma file, or of a video: its sound) into an audio asset for the pets; the id of the job comes back. The conversion runs in `murasaki-petaudio --import` behind the sandbox, one job at a time (at most 8 waiting). `InvalidArgs` for a relative path or a full queue; the file itself is checked by the helper, and a refusal is the job's `error` (`audio: not audio`, `audio: too long`, ... see docs/pets/audio.md) |
| `ImportItemAudio(s itemId)` | `s` | Revision 15. For a pet imported from a video with `keepAudio` (`pets import video --keep-audio`): queues the conversion of the kept copy's sound, like `ImportPetAudio`, and returns the job id. `InvalidArgs` for an item this daemon did not import or one with no kept audio. No pet is changed: the pets that show the item keep `mode` `none` until the user chooses |
| `PetAudioImportState(s jobId)` | `s` | Revision 15. JSON `{id, state: queued\|running\|done\|failed\|cancelled, name, error?, result?}`; `result` is the helper's `{ok, reused, asset, durationMs, bytes, sourceFormat, sourceCodec, sourceRate, channels, degraded}`. `{}` for an unknown job (the last 16 are kept) |
| `CancelPetAudioImport(s jobId)` | — | Revision 15. A queued job is dropped, a running one is stopped (it removes its partial file) |
| `ListPetAudioAssets()` | `s` | Revision 15. JSON array `[{asset, name, durationMs, sourceFormat, bytes, refs}]`: the sounds imported, and how many pets use each |
| `RemovePetAudioAsset(s asset)` | — | Revision 15. Removes an imported sound. `InvalidArgs` while a pet uses it, or for an id that is not an asset |
| `CollectPetAudio()` | `i` | Revision 15. Removes the sounds no pet uses; returns how many. The daemon also does this at start for sounds older than a day |
| `RemoveLocalPetItem(s itemId)` | — | Forgets a local pet item (the file is left alone). `InvalidArgs` for unknown or non-local ids, and while any placement still shows the item |
| `AddLocalPuppetItem(s path)` | `s` | Revision 20. Makes a pet item of a puppet folder on disk (one Inochi2D `.inp`, or one Cubism `*.model3.json` and the files it names; docs/pets/puppets.md) and returns its item id, `local:puppet:<sha1 of the canonical path>`. Nothing is copied or converted: the daemon validates the manifest (names, sizes, the path rules, no links; the limits table of the puppets) and remembers the folder (`local-puppets.json`). The deeper check (`murasaki-puppet --check`) runs behind `murasaki-sandbox --profile puppet` in the background and its answer is merged into the item (`ItemsChanged` follows). Adding a known folder returns the same id. `InvalidArgs` for a relative path, a missing or non-folder path, a folder that is not a puppet (the reasons of the check are in the message, `puppet: not a puppet`, `puppet: unsafe path`, ...), and beyond 64 puppets |
| `RemoveLocalPuppetItem(s itemId)` | — | Revision 20. Forgets a puppet item (the folder is left alone). `InvalidArgs` for unknown ids and while any placement still shows the item |

Revision 6 adds `SetOutputScales` and sizes the transcoded copies to the pets (see Transcode buckets). A client written for revision 5 keeps working against a revision 6 daemon: the items still carry `transcode`, and the new keys are additions. `murasaki-overlay` works against revision 5 too (it then does not send the scales, and every variant has one copy at 650 px).

| Method (revision 6) | Returns | Notes |
|---|---|---|
| `SetOutputScales(s scales)` | — | The device pixel ratio of each output, a JSON object `{"DP-1": 1.0, "HDMI-A-1": 1.5}`. Outputs that are not listed count as `1`; the call replaces the whole set (`{}` forgets them). `murasaki-overlay` sends it at start, whenever the daemon appears and on every change of outputs or scale. `InvalidArgs` for anything that is not an object, more than 32 outputs, names that are not 1 to 200 characters, scales that are not numbers from `0.5` to `4`; nothing changes then. From revision 17 the value of an output may also be an object `{scale, x, y, width, height[, primary]}` (`primary` a boolean: the compositor's primary output; whole numbers: `x`, `y` within 100000, `width` and `height` 1 to 16384, logical pixels in the compositor's layout): where the output is, which lets the daemon size the copy of a stage; a bad object refuses the whole call. The scales are session state, not persisted |

Revision 7 adds playlists. A client written for revision 6 keeps working against a revision 7 daemon; it just does not know the playlist calls, and a `ResolveAssignment` reply carries one more key.

| Method (revision 7) | Returns | Notes |
|---|---|---|
| `ListPlaylists()` | `s` | JSON array of `{id, name, items: [itemId], order: "sequential"\|"shuffle", intervalMinutes, changeOnLogin}`, in the order they were created |
| `CreatePlaylist(s name)` | `s` | Makes an empty playlist (sequential, 15 minutes) and returns its `id` (uuid). `InvalidArgs` for an empty or over-long (100 characters) name and beyond 100 playlists |
| `UpdatePlaylist(s id, s changes)` | — | Merges a JSON object into the playlist: `name`, `items` (the whole list, in play order: this is how to reorder), `order`, `intervalMinutes` (1 to 1440), `changeOnLogin` (other keys are ignored; `id` never changes). Items that are new to the playlist must exist and must not be pets; items that were already in it may have left the library. `InvalidArgs` for unknown playlists, wrong types, out-of-range values, duplicate items and more than 1000 items; nothing is changed then. A change that alters nothing does not signal |
| `AddToPlaylist(s id, as itemIds)` | — | Appends items (ones already in the playlist are left where they are). `InvalidArgs` for unknown playlists, unknown items and pets; nothing is added then |
| `RemoveFromPlaylist(s id, as itemIds)` | — | Removes items; ids that are not in the playlist are ignored. `InvalidArgs` for unknown playlists |
| `DeletePlaylist(s id)` | — | `InvalidArgs` for unknown playlists and while any assignment still plays it |
| `NextInPlaylist(s output, s desktop)` | — | Skips to the next item of the playlist that the assignment serving (`output`, `desktop`) plays (the same precedence as `ResolveAssignment`) and restarts its interval. `InvalidArgs` when that is not a playlist assignment |
| `PreviousInPlaylist(s output, s desktop)` | — | The same, going back (the first item goes to the last) |

Revision 8 adds item permissions. A client written for revision 7 keeps working against a revision 8 daemon: the items carry one more key, `permissions`.

| Method (revision 8) | Returns | Notes |
|---|---|---|
| `SetItemPermission(s itemId, s permission, b allowed)` | — | Allows or denies something for one item. `permission` is `network` (a web wallpaper may reach the internet; denied by default; the local content of the item always works). `InvalidArgs` for an unknown permission, an unknown item (granting only; a grant of a vanished item can still be revoked) or an id longer than 200 characters. The change is stored in `$XDG_CONFIG_HOME/murasaki/permissions.json`, announced with `ItemsChanged` (as a changed item) and, when the item is on screen, `AssignmentsChanged`, so the surface reloads the page under the new rules |

Every item (listings, `GetItem` and the `item` of `ResolveAssignment`) carries `permissions: {network: bool}`. Items also carry `risks: [{kind, file}]` (`kind` is `executable` or `application`; `file` is relative to the item folder): programs found in the item folder or a `project.json` of type `application`. Murasaki never runs them; the field exists so clients can warn.

| Signal (revision 7) | Args |
|---|---|
| `PlaylistsChanged` | none. A playlist was created, changed or deleted |

| Signal (revision 5) | Args |
|---|---|
| `PetsChanged` | none. Any placement was added, changed or removed, or an item with a placement changed |
| `TranscodeChanged` | `s itemId`. The transcode state or progress of some variant of the pet item changed (progress is reported in steps of at least 2 points) |
| `PetsArrangeChanged` | `b arrange` |
| `PetsHiddenChanged` | `b hidden` (revision 12) |
| `PetAudioChanged` | `s jobId` (revision 15): an import job changed (its id), or the list of sounds did (empty text) |
| `WallpaperInteractChanged` | `b interact` (revision 13) |

Revision 9 adds optional Workshop downloads through SteamCMD with the user's own Steam account (see `docs/user/steamcmd.md` and the spec `docs/design/specs/2026-10-17-murasaki-steamcmd-design.md`). A client written for revision 8 keeps working against a revision 9 daemon.

| Method (revision 9) | Returns | Notes |
|---|---|---|
| `SteamCmdStatus()` | `s` | JSON `{available, program, state, user, prompt, message, jobs}`. `state` is `unavailable` (steamcmd is not installed), `logged-out`, `logging-in` or `logged-in`; `prompt` is `""`, `password`, `guard` (a Steam Guard code from email or the authenticator) or `confirm` (confirm on the phone; nothing to type) while `logging-in`; `message` is the reason of the last failed login (SteamCMD's, such as `Invalid Password`); `jobs` is `[{appId, itemId, state: queued\|running\|done\|failed, progress 0..1, error}]`, at most 20 finished ones kept |
| `SteamCmdLogin(s user)` | — | Starts a login of that account (names of 1 to 64 characters, starting with a letter or digit, then letters, digits and `_.@-`). SteamCMD uses its own cached token when it has one, else it prompts: watch `prompt` and answer with `SteamCmdSubmit`. `InvalidArgs` for a bad name, without steamcmd, or while something else runs |
| `SteamCmdSubmit(s kind, s secret)` | — | Answers the prompt: `kind` `password` or `guard`. The secret is written to SteamCMD's terminal once; it is never stored or logged (the daemon does not keep its own copy of what it wrote). `InvalidArgs` when SteamCMD is not asking for that kind, or for an empty or multi-line secret |
| `SteamCmdCancelLogin()` | — | Stops a login in progress |
| `SteamCmdLogout()` | — | Cancels downloads, signs out (SteamCMD's `+logout`) and forgets the account name |
| `SteamCmdDownload(s appId, s itemId)` | — | Queues a Workshop item of Wallpaper Engine (`431960`) or Anima Engine (`3474900`); one runs at a time. `InvalidArgs` for other apps, ids that are not 1 to 20 digits, when signed out, or without steamcmd. Queuing an item that is queued or running is a no-op |
| `SteamCmdCancel(s itemId)` | — | Cancels that item's download, or every one when empty |

| Signal (revision 9) | Args |
|---|---|
| `SteamCmdChanged` | none. The status changed (state, prompt, a job's state or progress; progress is coalesced to about ten signals a second). Re-read `SteamCmdStatus()` |

**Trust.** Like every call of this interface, the SteamCMD calls are trusted to anything running as the same user on the session bus: it can sign in, sign out and download, and **the password crosses the session bus** (a private per-user bus, not the network) in `SteamCmdSubmit`. Keep untrusted programs off your session bus. Account names must start with a letter or digit (then letters, digits and `_.@-`, 64 characters at most). Login failures are recognised from SteamCMD's wordings `...FAILED (reason)`, `FAILED login with result code reason`, `ERROR (reason)` and `Login Failure: reason`; another wording ends the login when SteamCMD exits, with "SteamCMD ended before the login finished". Cancelling, signing out or a timeout stop SteamCMD's whole process group (SIGTERM, then SIGKILL after two seconds), and a new SteamCMD starts only once the old group is gone.

Items downloaded this way land in `$XDG_DATA_HOME/murasaki/steamcmd/steamapps/workshop/content/<app>/<id>` (`$MURASAKI_STEAMCMD_DIR` overrides the directory), which the daemon reads as one more Workshop root next to Steam's. `$MURASAKI_STEAMCMD` names the steamcmd program to run (default: `steamcmd` on `PATH`, `/usr/games/steamcmd`, or SteamCMD's own installer directory). The account name is kept in `$XDG_CONFIG_HOME/murasaki/steamcmd.json`.

## Signals

| Signal | Args |
|---|---|
| `ItemsChanged` | `as added, as changed, as removed` (item ids) |
| `AssignmentsChanged` | none. Emitted after any assignment change and when `ItemsChanged` touches an assigned item |
| `SettingsChanged` | none. Emitted when `SetSettings` actually changes a value |

`ItemsChanged` reports changes derived from `project.json` (for pets, from the animation file headers and sizes) and the item set itself; editing other content files of an item does not emit it. The daemon watches the Workshop dirs of every Steam library for both Wallpaper Engine (app 431960) and Anima Engine (app 3474900), including dirs that do not exist yet (for example before the first subscription); such a dir is picked up within about 5 seconds of being created.

## Playlists

A playlist rotates wallpapers on a timer. It is played by assigning `playlist:<id>` (`SetAssignment(output, desktop, "playlist:<id>", options)`); `Assignments()` lists that assignment as it was stored. The assignment's `fill`, `muted` and `properties` apply to every item of the playlist, and properties a user saved per item still apply to that item.

The daemon keeps a position per **target**, which is the assignment key (output and desktop, wildcards included): one assignment on `*`/`*` shows the same item on every output, in step. A new playlist assignment starts at the first item (the first of a shuffled bag in `shuffle` order).

`ResolveAssignment` for a playlist assignment returns:

```json
{"assignment": {"output": "*", "desktop": "*", "itemId": "we:123", "muted": false, "fill": "fill", "properties": {}},
 "item": { "full item JSON" },
 "playlist": {"id": "<uuid>", "index": 0, "nextAt": 1792240000000}}
```

`assignment.itemId` is the item that is showing (so a client comparing replies sees the wallpaper change, and the surface cross-fades); `playlist.index` is its position in the playlist's list (`-1` and `item: null` when nothing in the playlist can be shown, for example an empty one) and `nextAt` the time of the next rotation, in milliseconds since the epoch. When the item changes the daemon emits `AssignmentsChanged`.

- **Rotation.** Every `intervalMinutes`. Nothing rotates while the session is locked (`org.freedesktop.ScreenSaver`); when it is unlocked, the rotation that came due runs once. A long gap (suspend, lock) never rotates more than once, and the next interval starts then. One timer sleeps until the earliest deadline of any target.
- **Order.** `sequential` goes through the list and wraps. `shuffle` plays every item once in a random order (a bag) and then reshuffles, never starting the new bag with the item that ended the old one.
- **Skipped items.** Items that left the library, pets and items whose status is not `ok` are skipped, both when rotating and when the playlist starts. They stay in the playlist and come back with the item.
- **Editing.** Changing the list or the order of a playlist that is playing keeps the item that is showing when it is still in the list; if it was removed, the item that followed it takes over.
- **Restarts.** The position and deadline of each target are saved in `$XDG_STATE_HOME/murasaki/playlist-state.json`. A restart does not catch up on missed rotations: a deadline in the past becomes a full interval from the start. With `changeOnLogin`, a target that was already playing the playlist advances once when the daemon starts. An invalid state file is renamed to `playlist-state.json.bad-<UTC time>` and every playlist starts over.
- **Pets** can not be in a playlist (`AddToPlaylist` and `UpdatePlaylist` refuse them).

### Playlists file

`$XDG_CONFIG_HOME/murasaki/playlists.json`, written atomically by the daemon (`{"version": 1, "playlists": [...]}`). An invalid file is renamed to `playlists.json.bad-<UTC time>` and the daemon starts with no playlists; assignments to missing playlists resolve to nothing.

## Automation (revision 18)

Profiles, triggers and timelines (docs/pets/automation.md). Their stores are `profiles.json`, `triggers.json` and `timelines.json` in `$XDG_CONFIG_HOME/murasaki/`: atomic writes, a bad file renamed to `<file>.bad-<UTC time>` and the store started empty, saving blocked if the rename fails (the rule of `pets.json`). Nothing exists until the user makes it: a fresh install has no file, no timer, no watcher and no bus subscription.

| Method | Returns | Notes |
|---|---|---|
| `ListProfiles()` | `s` | JSON array `[{id, name, pets: [Entry], origin?}]`; an Entry is a placement without `petId`, plus `slot` |
| `SaveProfile(s name)` | `s` | Saves the pets on the desktop as the profile of that name (an existing one is replaced); returns its id and makes it the `ActiveProfile`. `InvalidArgs` for a bad name or 50 profiles |
| `SwitchProfile(s idOrName)` | `s` | Makes the pets the profile's in ONE write and one `PetsChanged`: a pet that exists in both keeps its `petId` (so its window and sound), the others are added or removed. Returns JSON `{profile, kept, updated, added, removed, skipped: [{slot, itemId, reason}]}`; an entry whose item or variant is gone is skipped with `profile: item missing` or `profile: variant missing`, never an error. Wallpapers, settings and the session's hidden/arrange state are not touched |
| `SaveExampleProfile(s name)` | `s` | Revision 22. Saves the pets as a profile marked `origin: "demo"` in ONE call, and returns its id. Unlike `SaveProfile` it does not make the profile the `ActiveProfile` and does not rebind the pet slots of the timelines. A profile of the same name that is an example is replaced; one that is not (the user's) is refused with `InvalidArgs`. Switching to an example remembers the profile that was active before; deleting the example while it is active switches back to that one. Renaming an example makes it the user's (the marker goes) |
| `SetProfileOrigin(s idOrName, s origin)` | — | Revision 22. Sets or clears the `origin` of a profile: only `demo` or an empty text (a package's origin comes with its import). `InvalidArgs` for anything else and for an unknown profile |
| `RenameProfile(s idOrName, s newName)` / `DeleteProfile(s idOrName)` | — | `InvalidArgs` for an unknown profile or a name another profile has (any case) |
| `ListTriggers()` | `s` | JSON array of triggers `{id, name, enabled, cooldownMs, mode: press\|hold, source: {kind, ...}, action: {type, ...}, origin?}` |
| `SetTrigger(s triggerId, s changesJson)` | `s` | An empty id creates a trigger (the changes are its whole JSON) and returns the id; otherwise the keys given change that trigger. `InvalidArgs` names the key that is wrong. The sources and the actions are closed lists (docs/pets/automation.md); a trigger can not run a program or open a file or a web address |
| `DeleteTrigger(s triggerId)` / `SetTriggerEnabled(s triggerId, b enabled)` | — | |
| `FireTrigger(s name)` | `b` | The triggers whose source is `fire` with that name fire; with none, and the setting `fireByTimelineName`, the timeline of that name runs. True when something matched. The cooldown and the rate limits apply |
| `TestTrigger(s triggerId)` | `b` | Runs the trigger as if its source had happened. False when the limits or the switches stopped it |
| `TriggerState()` | `s` | JSON `{states: [{id, enabled, available, outcome, agoMs?, rateLimited, note?}], log: [{id, agoMs, outcome}], suspendedMs}`: the last 50 fires, in memory only. `outcome` is `ran`, `cooldown`, `rate limited`, `chain too deep`, `disabled`, `source unavailable` or `shortcut unbound`; `note` names `trigger: source unavailable` or `automation: screen triggers are not active` |
| `TriggerShortcut(s shortcutId, b pressed)` | — | The overlay reports a key of a shortcut trigger |
| `StartScreenWatch()` | — | Revision 21. The user starts watching the screen: the daemon asks the desktop's screen cast portal (its own dialog) and, when the user agrees, starts the sandboxed helper `murasaki-screenmatch` on the PipeWire remote the portal gave. Needs the setting `screenTriggersEnabled` (and `automationEnabled`), an enabled `screen` trigger and an unlocked session, else `InvalidArgs` naming which. Calling it while asking or watching is harmless. The answer is `ScreenWatchState` / `ScreenWatchChanged`; there is no automatic restart after a refusal, the desktop ending the share or a crash |
| `StartScreenWatchLeased()` | — | Added inside revision 21 (no revision of its own: a client that gets `UnknownMethod` calls `StartScreenWatch`). The same as `StartScreenWatch`, for a caller that must not leave the screen being read behind (the GUI's guided walk): when this call starts the watch (it was not asking or watching), the watch is leased to the caller's unique bus name and the daemon stops it (reason `the program that started it is gone`) when that name leaves the bus. A watch that was already running is the user's and is not leased. `StopScreenWatch` from anyone, or the watch ending by itself, ends the lease |
| `StopScreenWatch()` | — | Revision 21. Stops watching: the helper ends, the portal session is closed, what a screen trigger had running is undone. Always works; the rescue, the setting going off and the last screen trigger being disabled do it too (a locked session only pauses it) |
| `CaptureScreenReference(s triggerId, i region, i steps)` | `s` | Revision 21. While watching: takes the picture of a region of a screen trigger (`steps` 1 to 64 of them, one a second at least) from the helper, scaled to at most 360 pixels, and returns a request id (`InvalidArgs` when not watching, for an unknown trigger or region, a region outside the shared screens, or while another capture runs). The pictures come in `ScreenCaptured` (to the caller alone) and are held in memory only: one capture at a time, dropped after 60 seconds, when the watch stops, when the session is locked, or when the helper or the share fails (a capture the helper does not answer within 4 seconds fails with `{error}`). Refused while the session is locked |
| `DiscardScreenReference(s request)` | — | Revision 21. Drops a capture that is held or still running without keeping it (the user closed the preview); only the bus name that asked may. `InvalidArgs` when there is no such request |
| `CommitScreenReference(s request)` | — | Revision 21. Keeps what `ScreenCaptured` brought for that request as the reference pictures of the region (step 0 up; the old ones of the region are replaced whole), once, only while watching, only if the trigger and region still exist, and only from the bus name that asked. `InvalidArgs` for a request that is not waiting |
| `ListTimelines()` | `s` | JSON array `[{id, name, speed, restore, tracks: [{pet, startVisible, actions}], origin?}]` |
| `SetTimeline(s timelineId, s changesJson)` | `s` | Like `SetTrigger`. A bad action is `InvalidArgs` with its track and index |
| `DeleteTimeline(s idOrName)` / `RunTimeline(s idOrName)` / `StopTimeline(s idOrName)` | — | A run is cancelled and, when the timeline restores, undone first. At most 8 timelines run (`timeline: too many running`); running one again restarts it; `timeline: pet not found` when none of its tracks has a pet |
| `SetPetsMuted(b muted)` | — | Revision 18. Mutes or unmutes the pets' sounds for the session; signals only when the value changes (`PetsMutedChanged`) |
| `ResumeAutomation()` | — | Revision 18. Ends the pause of the triggers that `RescuePets` starts (a minute) |
| `RunningTimelines()` | `as` | Ids of the timelines that run |
| `ClearPetRuntime(s petId)` | — | Clears the runtime layer of that pet, or of all of them when empty. `RescuePets` also stops every timeline, clears the layer and pauses the triggers for a minute |

| Signal | Args |
|---|---|
| `ProfilesChanged` | none (the list, the active profile or the modified mark changed) |
| `TriggersChanged` / `TimelinesChanged` | none |
| `TriggerFired` | `s triggerId, s outcome` (`ran` or `released`) |
| `PetsMutedChanged` | `b muted` (revision 18) |
| `PetRuntimeChanged` | `s petId` (the layer of that pet changed; re-read `PetRuntime`) |
| `ScreenWatchChanged` | none (revision 21; re-read `ScreenWatchState`; while watching also when the scores or an activation changed, at most 16 a second) |
| `ScreenCaptured` | `s request, s json` (revision 21): `{request, trigger, region, steps: [{step, width, height, png (base64)}]}` or `{request, error}`. **Not a broadcast**: it holds pixels of the screen, so the daemon sends it as a unicast signal to the bus name that called `CaptureScreenReference`, and only that name may `CommitScreenReference` it (receive it on the connection that asked) |

Revision 22 (the demonstrations, docs/pets/automation.md "Examples"): the timeline action `track` (`{kind: "track", mode: "off"|"look"|"follow"}`: the pet's Mouse Tracking mode while the run lasts) and the key `tracking` (`"off"|"look"|"follow"`) of an entry of `PetRuntime`, which the overlay applies over the stored `tracking.mode` (nothing is written to `pets.json`; a restore, `ClearPetRuntime` or the rescue clear it); `SetProfileOrigin(s idOrName, s origin)` (below); and the setting `demoCardsDismissed`. The "Try it" buttons and `murasakictl demo` are made of the calls above (`SetTimeline`, `SetTrigger`, `SaveProfile`, `RunTimeline`, ...) with `origin: "demo"`; removing the examples deletes what carries that origin, client-side. `SetTimeline`/`SetTrigger` with `"origin": ""` clear the marker (the editors of the GUI send it when the user changes an example, which makes it theirs); a change that does not name `origin` keeps it. 

Revision 23 (the idle trigger, `docs/pets/automation.md`): the source `idle` takes an optional `state`, `"away"` (the default, written without the key) or `"back"`: `back` fires once when an idle period that lasted at least `minutes` ends (`{"kind": "idle", "minutes": 10, "state": "back"}`), so it undoes exactly the naps that fired; any other value is `InvalidArgs`. A daemon older than 23 ignores the key and stores an away trigger, so a client that writes `back` must check `ApiVersion >= 23` (`kIdleBackDBusApiVersion`) first. The surface's `SessionFacts()` gains the idle fields above and the daemon prefers them to logind's hint; `available` in the state of an `idle` trigger is true when either source knows.

Revision 21 (screen triggers, phase 9; numbered below 22 because the demonstrations were merged first, so `ApiVersion` is the newer of the two): the calls and the signals above, the property `ScreenWatchState`, the `screen` object of `TriggerState` for a screen trigger while watching (`{score: [permille per region, -1 for a region that could not be scored], detected, step, active}`, in memory only; and, watching or not, `refs: [reference pictures per region]` in the state of every screen trigger; the `note` `automation: screen triggers are not active` is given only while nothing is watched), and the settings `screenTriggersEnabled` (bool, default `false`), `screenTicksPerSecond` (1 to 16, default 5; 2 at most on battery), `screenRemember` (bool, default `false`: the portal keeps the choice of screens with a restore token, kept in `$XDG_STATE_HOME/murasaki/screen-state.json` and only while this is on), `screenWatchAtLogin` (bool, default `false`: ask for the screens once the session starts) and `screenIgnorePets` (bool, default `true`: a region more than half covered by our own pets is skipped for that tick). A `screen` trigger's `data` is the bounded form `{regions: [{kind, x, y, w, h}], match: {mode, agg, threshold, cooldownMs, cooldownFrom, startFramesToCheck, stopGraceMs, stopOnMismatch, refMode, seqLen, seqStep, loopDelayMs}}` of a Triggers pack (regions in the desktop's logical coordinates); its reference pictures live in `$XDG_DATA_HOME/murasaki/triggers/<trigger id>/` and are deleted with the trigger. Nothing is opened, started or subscribed until the user calls `StartScreenWatch`.

Settings (revision 18): `automationEnabled` (bool, default `true`: off stops every timeline and nothing fires or is subscribed), `fireByTimelineName` (bool, default `false`) and `triggerCooldownMs` (100 to 86400000, default 500: the cooldown a new trigger starts with); revision 22: `demoCardsDismissed` (an array of `timelines`, `triggers`, `profiles` and `screen`, default empty: the pages whose first-visit "Try it" card was closed; `screen` is the card of the screen-trigger section, revision 21 and later).

The overlay reports `runtimeHidden` in `Windows()` for a pet a timeline or a trigger hid, and its `ShortcutTriggers` also lists the keys of the shortcut triggers under `trigger.<id>`.

## Pause rules

Beyond covered outputs (`pauseWhenCovered`), `murasaki-surface` pauses every wallpaper (silent too) when a rule of the settings above applies, and says why: its `Outputs()` entries carry `pauseReason` (`game`, `battery` or `app`, empty otherwise) and `pauseDetail` (the `pauseApps` rule that matched) and, under the battery cap, `fpsCap` (15). The first rule that applies names the reason, in the order game, battery, application. A cap only applies while nothing pauses. GameMode and UPower are optional: their absence is a quiet state (no game, on mains), and the surface never starts them. For hosts (the Plasma plugin) the verdict travels in `HostState` (`paused`, and `fpsCap`).

## Spanning

An assignment with `span: true` and `output` `*` shows one wallpaper across every output. The canvas is the bounding box of the outputs in the compositor's logical layout (negative coordinates and mixed scales included, everything in logical pixels); each output draws the part of the canvas it covers, and the fill mode (`fill`, `fit`, `stretch`) applies to the canvas as a whole, so with `fit` an output inside a letterbox band shows nothing. Every output keeps its own renderer (the cost is n times one output). The maths lives in `src/core/canvas.h`. How each kind is drawn:

- **Video:** each output's mpv player gets `video-aspect-override`, `video-zoom` and `video-pan-x/y` that place the whole picture where the canvas puts it (`mpvViewport`; `tst_mpvspan` checks it against mpv's own fill, fit and stretch). The players of one spanned video start together and `murasaki-surface` compares their `time-pos` every 4 s, seeking any that strays more than 50 ms from the first output's. Hosts (the Plasma plugin) are separate processes and are not kept in step.
- **Scene:** the `SpanView` bridge message (see `docs/scene-vk/README.md`). Without a usable `murasaki-scene-vk` the output shows the scene's preview with the reason in the log.
- **Web pages and previews:** the page is laid out on a canvas-sized item that is shifted so that the output's rectangle lies on the screen, so `100vw` is the width of the whole canvas (it costs a canvas-sized page per output).

The rectangle follows the screens: when one moves, appears or goes, spanned outputs get their new rectangle without reloading.

## Assignments file

`$XDG_CONFIG_HOME/murasaki/assignments.json`, written atomically by the daemon. An invalid file is renamed to `assignments.json.bad-<UTC time>` and the daemon starts with no assignments.

## Settings

| Key | Type | Default | Meaning |
|---|---|---|---|
| `webFpsLimit` | integer, 10 to 144 | `30` | Frame rate cap for all animated wallpapers (web pages and scenes); the key keeps its name for compatibility |
| `pauseWhenCovered` | boolean | `true` | Pause playback while a window covers the wallpaper |
| `presenter` | string: `auto`, `layer-shell` or `plasma` | `auto` | Who draws the wallpapers. `layer-shell`: `murasaki-surface` draws its own layer-shell windows and ignores Plasma hosts. `plasma`: the Plasma wallpaper plugin draws (one `murasaki-surface --host` per screen); `murasaki-surface` creates no windows but keeps deciding what plays. `auto`: layer-shell windows, except for an output that has a registered host |
| `pauseOnGame` | boolean | `true` | Revision 7. Pause the wallpapers while GameMode (`com.feralinteractive.GameMode` on the session bus) has clients (`ClientCount` > 0): only those of the covered outputs (`occluded`) where the compositor monitor reports them (KWin, GNOME with the extension, Sway, Hyprland), every one elsewhere; `pauseReason` is `game` only on the outputs it paused. Nothing happens when GameMode is not running |
| `batteryMode` | string: `off`, `pause` or `lowfps` | `off` | Revision 7. On battery (UPower `OnBattery` on the system bus), `pause` pauses every wallpaper and `lowfps` caps them at 15 frames per second (videos through mpv's `fps` filter, web pages and scenes through their frame rate limit). Without UPower the machine counts as plugged in |
| `pauseApps` | array of strings, at most 64 | `[]` | Revision 7. Window classes (resource classes, such as `obs` or `steam_app_620`; `*` and `?` match any characters, case-insensitive, whole class): every wallpaper pauses while a window of one has the focus or any is fullscreen on the current desktop. Entries are trimmed, 1 to 100 characters. Only KWin reports window classes, so this needs KDE Plasma |
| `sandboxEngines` | bool | `true` | Revision 8. Runs the scene engines behind `murasaki-sandbox` (Landlock): they can read the item, the Wallpaper Engine assets and the system, write the GPU devices and the shader caches, and use no TCP. Turn it off only to debug; scenes on screen start again. Without the helper installed or Landlock in the kernel the engines run unconfined and say so once in their log |
| `requireSandbox` | bool | `false` | Revision 8. Refuses to start a scene engine that would run unconfined (no `murasaki-sandbox`, no Landlock, or no way to refuse TCP): the scene is not shown and `failed` says why. Off by default |
| `mediaShare` | bool | `true` | Scenes that show the cover of the playing media or listen to the media events of SceneScript are told what plays: the title, artist, album, genres, position and the cover of the player the session bus offers (MPRIS), and only those scenes (an engine says whether its scene uses them). Off: the players are not read at all and such scenes are told that nothing plays. Those scenes' scripts (Workshop scripts included) can keep what they hear in their own storage and write it in their log |
| `shortcutsLaunch` | bool | `true` | A click on the icon of a dock shortcut of a wallpaper (a `usershortcut` property the user filled with an application) starts that application. Only what the user chose is started, only after a real click on the wallpaper, and at most 6 times a minute; off: nothing is ever started by a wallpaper (the icons still show) |
| `petMaxFps` | integer, 0 to 120 | `0` | Revision 11. The most frames per second the desktop pets change frames at; `0` is no limit. Frames above it are skipped, the animation keeps its speed (docs/pets/timing.md) |
| `petsVolume` | int 0 to 100 | `100` | Revision 15. The master volume of the pets' sounds (linear), on top of each pet's own |
| `petsMuted` | bool | `false` | Revision 15. Silences every pet's sound; sounds go on silently, so a looping one is in place when unmuted |
| `stageHideOverFullscreen` | bool | `true` | Revision 17. A pet in the fullscreen mode (docs/pets/stage.md) is hidden and paused on an output while a fullscreen window, a game or a listed application is up on it; a pet's own `stage.overFullscreen` can override |
| `petsHideOnGame` | bool | `false` | Revision 24. While a game runs, the pets on the screens it plays on are hidden and paused; the others play on. The screens are those of `pauseOnGame`: the outputs a window covers (fullscreen or occluded), or every output when the session cannot say. A pet in the fullscreen mode follows `stageHideOverFullscreen` instead. The overlay's `Windows()` says `gameHidden` |
| `stageMaxPixels` | integer, 1000000 to 33177600 | `8294400` | Revision 17. The most device pixels all the windows of a stage draw in total (a 3840 x 2160 frame); above it the copies of the animation (and its frames) are made smaller and scaled up by the GPU; an original that plays as it is is not reduced |
| `puppetMaxPixels` | integer, 1000000 to 16000000 | `4194304` | Revision 20. The most pixels the picture of one puppet pet is drawn at (the engine draws smaller and the window scales it up; `Windows()` says `puppet: resolution reduced`) |
| `puppetCorePath` | string | empty | Revision 20. The Cubism Core library the user installed (Murasaki ships none). **Stored and checked only**: nothing loads it yet. Empty, or an absolute path of a regular file that belongs to you or root and that nobody else can write (a link is not followed); `SetSettings` refuses anything else with `InvalidArgs` |
| `petsMuteWhenFullscreen` | bool | `true` | Revision 15. Silences the pets' sounds while a fullscreen window, a game (GameMode) or a window of the `pauseApps` rules is up (from the surface's `PetAudioMuted`) |
| `petSaveEnergy` | bool | `true` | Revision 11. On battery (UPower) or in the power-saver profile (power-profiles-daemon) the desktop pets are held to 15 frames per second, or to `petMaxFps` if that is lower. Without these services the machine counts as plugged in. A desktop on a UPS that UPower reports as a battery counts as being on battery: turn this off there |
| `mediaRemoteArt` | bool | `true` | The scenes that show the cover of the playing media (`usertextures` `$mediaThumbnail`) get it from the player named by MPRIS. A cover given as an `https://` address is downloaded by the surface (the host, never the scene engine, which has no network): one request at a time, at most 8 MiB, 10 s, no cookies, only to public addresses (a name that resolves to a local or private one is refused). Off: only covers that are files on this machine (`file://`) are read |
| `sceneRenderScale` | number, 0 or 0.25 to 1 | `0` | The resolution the Vulkan scene engine draws at, as pixels per output pixel (the GUI offers 0, 1, 0.75 and 0.5). `0` is automatic: the engine lowers the resolution, never below half, while the GPU needs more than a quarter of each frame interval, and raises it again when there is room. Scenes on screen start again when it changes |
| `sceneEngine` | string: `auto` or `vulkan` | `auto` | Which engine draws scene wallpapers: Murasaki's Vulkan engine `murasaki-scene-vk`; `auto`, the default, is the same engine |
| `importFormat` | `apng` or `webp` | `apng` | Revision 14. The output format a pet import starts from; each import can choose its own |
| `importSizePolicy` | `fit`, `scale` or `first` | `fit` | Revision 14. Images of different sizes: letterbox into the largest canvas, scale to the first, or refuse |
| `importFps` | integer, 1 to 120 | `24` | Revision 14. The speed of an import that has no timing of its own (image sequences, ZIPs) |
| `importKeepSource` | bool | `false` | Revision 14. Keep a copy of the source files next to the converted pet |
| `importKeepAudio` | bool | `false` | Revision 14. Keep a video's audio track for later (never played now) |
| `convertGifOnImport` | bool | `false` | Revision 14. A GIF added as a local pet (`AddLocalPetItem`) is also converted to APNG; the GIF stays |
| `importWorkshopFiles` | `ask` or `auto` | `ask` | Revision 14. A ZIP or video found in a Workshop item: wait for "Import this file" or import it when the item is scanned |
| `importWorkshopMdaa` | `ask` or `auto` | `ask` | Revision 14. The same for an `.mdaa` package. Even automatically, the package's program is never extracted or run |
| `importMaxOutputMiB` | integer, 16 to 1024 | `1024` | Revision 14. The most a single import may write (the daemon passes it to the helper) |
| `importVideoMaxSide` | integer, 64 to 8192 | `1024` | Revision 14. The longest side of an imported video |
| `importRequireSandbox` | bool | `true` | Revision 14. An import runs only inside the sandbox (Landlock): without it the job fails with `sandbox unavailable`. Turn it off to import on a system without Landlock; the job then says `import: not sandboxed`. Read again at every job, no restart |
| `importVideoMaxFps` | integer, 1 to 60 | `30` | Revision 14. The highest frame rate kept from an imported video |

An integral number such as `30.0` is accepted for an integer; `30.5` is not.

### Settings file

`$XDG_CONFIG_HOME/murasaki/settings.json` (next to `assignments.json`), written atomically by the daemon. Unknown keys in the file are ignored and dropped on the next save. An invalid file is renamed to `settings.json.bad-<UTC time>` and the defaults apply.

## Item JSON

Summary keys: `id`, `source`, `title`, `kind` (`scene`/`video`/`web`/`pet`/`asset`/`unknown`; `asset` is an editor asset pack, a project without `type` that has `category` `Asset` or `file` `assets.json`: listed with status `ok`, never a wallpaper, so `SetAssignment` and playlists refuse it and the GUI keeps it out of the library), `status` (`ok`/`missing-dependency`/`invalid`), `statusDetail`, `preview`, `tags`, `contentRating`, `workshopId`, `dependencyId`. Pets add `variantCount` to their summary; no other item has it.

Ids have the form `we:<dir name>`, where the dir name is the item's folder in the Workshop dir. It is usually the numeric Workshop id but may be non-numeric (for example `local` or `myprojects` entries). For `missing-dependency` items, `statusDetail` is the bare dependency Workshop id. Presets take `kind`, `entryFile` and `contentDir` from their base item.

The full item adds `dir`, `entryFile`, `contentDir` (the dir an engine must load; for presets, the base item's dir), `presetValues` (object of preset overrides, empty for non-presets), `localization` (the project's `general.localization`, `{locale: {key: text}}` with lowercase locale names such as `en-us`; empty when absent; full item only, not in listings; clients use it to translate property labels and option labels that are keys) and `properties`, an array of `{key, type, text, value, order, min?, max?, step?, options?, condition?}`.

`dir`, `entryFile`, `contentDir` and `preview` are absolute paths as seen by the daemon (which may differ from the client's view when sandboxed).

### Pets (Anima Engine)

Items from Steam's Anima Engine (app 3474900) have ids `anima:<dir name>` (the dir name is the Workshop id), `source` `anima-engine` and `kind` `pet`. They are desktop pets, not wallpapers: `SetAssignment` rejects them with `InvalidArgs`, and clients should offer them no Apply action. Until Workshop metadata arrives (revision 5), `title` is the Workshop id, `tags` are empty and `preview` is empty (render the first frame of a variant instead).

The full item (`GetItem`) adds `variants`, the animation files directly inside the item dir (`.apng`, `.gif`, `.webp`, whatever the extension's case) whose headers read as an animation, sorted by name (case-insensitive). Each is `{name, file, width, height, fps, frames, bytes}`: `name` is the file name without extension, `file` the absolute canonical path (symlinks resolved), `width`/`height` the canvas in pixels, `frames` the frame count, `fps` the average rate (frames over the summed frame delays, rounded to 0.01, with APNG and WebP frames shorter than 10 ms counted as 10 ms so it never exceeds 100; `0` for a still image) and `bytes` the file size. Listings carry only `variantCount`, so a large library stays small; fetch the item for the list. Files that are broken or still downloading are left out of `variants`; an item whose animation files are all unreadable has `status` `invalid` and the first problem in `statusDetail`. A dir without any animation file (a download that has not started writing) is not listed. For pets `entryFile` is empty, `contentDir` equals `dir` and `properties` is empty. Those fields arrived with revision 4 as plain additions (new fields and a new `kind` value, which clients must ignore).

Revision 5 changes pet items in two ways. Their `title`, `tags` and `preview` come from the Workshop metadata once it has been fetched (see below): until then the title is the Workshop id and `preview` is empty. And each entry of `variants` in the full item (`GetItem`) gains `heavy` (`true` when the variant is at least 800 px on a side or at least 50 MB) and `transcode`, the state of its cached VP9 copy:

| `transcode.state` | Extra keys | Meaning |
|---|---|---|
| `none` | | No copy, none requested |
| `queued` | | Waiting; one conversion runs at a time |
| `running` | `progress` (0 to 1) | `ffmpeg` is converting |
| `done` | `file` | The copy (WebM, VP9 with alpha, at most 650 px on the longest side, 30 fps for sources of 50 fps or more) is ready; `file` is its absolute path |

From revision 6 every state also carries `bucket`, the longest side in pixels the copy is converted to (see Transcode buckets).
| `failed` | `error` | The last `ffmpeg` stderr lines, or the reason (`ffmpeg not found`). The pet plays the original; `RequestTranscode` tries again |

Each variant also has `buckets` (revision 6): the array of the states, in the same form as above and each with its `bucket` and `fps`, of every bucket and playback rate that is not `none`. `transcode` is the state of the bucket the variant's placements need (the biggest bucket when none shows it) at the rate of the first placement of the variant (or the item's default when none shows it), which is the one the GUI reports; `buckets` lists them all, so a pet can keep playing an older, bigger copy while the right one is converting. A revision 5 daemon has no `buckets`, and its one copy is the 650 px one.

Listings (`ListItems`) never carry `variants`.

### Local pets

Animation files the user picked are pet items too, with id `local:<sha1 of the canonical path, hex>`, `source` `local`, `kind` `pet`, the file's base name as `title`, no `workshopId` and no `preview` (render a frame of the variant). Their single variant is named after the file (without extension) and carries the same fields as a Workshop variant, including `heavy` and `transcode`. The picked paths are stored in `$XDG_CONFIG_HOME/murasaki/local-pets.json` (`{"version": 1, "files": [absolute paths]}`, written atomically; an invalid file is renamed to `local-pets.json.bad-<UTC time>`). A path whose file later vanishes or stops reading as an animation stays listed with `status` `invalid` and the problem in `statusDetail`, so it can be removed. The daemon does not watch these files: a change shows up at the next rescan. `.webm` files are not accepted yet.

### Pet placements

A placement is `{petId, itemId, variant, output, x, y, width, opacity, playOriginal, paused, fps, speed}`: `x` and `y` are the top-left corner in output-logical pixels relative to the output's own corner (within +-100000), `width` the displayed width in the same unit (16 to 16384; the height follows the aspect ratio), `opacity` 0.1 to 1, `playOriginal` plays the source file even when a transcoded copy exists, `paused` freezes the pet, the behaviour fields below (revision 12), and `fps` and `speed` say how fast it plays (docs/pets/timing.md). `fps` (an integer, `0` or 1 to 120; 24 for a new pet and for one stored without the field) shows every frame for `max(1, round(1000 / fps))` ms, which is how Anima Engine steps its pets (it ignores the delays stored in the file); `0` is not a fixed rate. `speed` (a number, 0.25 to 4) divides the file's own delays; `1` is neutral. The two do not go together (the file's own timing is `fps` 0 and `speed` 1): `{"fps": 15}` alone makes the pet a 15 fps one and leaves `speed` at 1, `{"speed": 2}` alone sets `fps` to 0, `{"fps": 0}` alone keeps the current `speed` (so on a pet at 2x it ends in speed mode, not the file's timing: send `{"fps": 0, "speed": 1}` for that), and both at once in a non-neutral combination are `InvalidArgs`. With both neutral every frame lasts what its file says, after the rule for tiny delays (a delay of 10 ms or less is 100 ms, for GIF, APNG and WebP). `AddPet` and `UpdatePet` take them like the other fields (`InvalidArgs` for a `fps` that is not a whole number from 0 to 120 or a `speed` outside its range). `fps` arrived with revision 6 (up to 60, and a default of 30 for new Anima Engine pets) and `speed` with revision 11, which also raised the limit to 120 and made 24 the default for every item, as an addition: a client written for an earlier revision ignores `speed`. Placements stored before the field existed get 24. `output` is a connector name such as `DP-1` or `*` for the primary output.

**Behaviour fields (revision 12).** All optional in every call, validated like the others (a wrong type is `InvalidArgs`, unknown keys are ignored); `ListPets` carries them all and a `pets.json` without them reads as the defaults, so a pet stored before looks and behaves exactly as it did.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `clickThrough` | bool | `true` | `false`: the pet takes the pointer on its opaque pixels (Anima's own default is an interactive pet; Murasaki keeps click-through so that no existing placement changes) |
| `locked` | bool | `false` | `true`: a drag does not move it |
| `keepAbove` | bool | `true` | `false`: the pet is not above the windows, as far as the compositor allows a layer surface to be (on layer-shell it moves to the bottom layer) |
| `hoverHide` | bool | `false` | the pet hides under the cursor; excludes `focusFade` |
| `focusFade` | bool | `false` | the pet fades under the cursor and lets it through; excludes `hoverHide` |
| `smartDodge` | bool | `false` | the pet steps away from the cursor |
| `rotation` | number | `0` | degrees; any finite number is wrapped into 0 (included) to 360 (excluded) and rounded to two decimals (`-90` is `270`, `370` is `10`) |
| `flipH`, `flipV` | bool | `false` | mirror the picture; the rotation turns the mirrored picture |

`hoverHide` and `focusFade` exclude each other: setting one to `true` alone switches the other off; both `true` in one call is `InvalidArgs`. Done differently from Anima: every flag is persisted, including `focusFade` and `smartDodge`, which Anima forces off at each start; `RescuePets` is the way out of a forgotten one. A pets file with both `hoverHide` and `focusFade` on loads with `focusFade` off, and an angle that is not a sane number loads as 0; neither costs the user the file. A daemon from before revision 12 loads a file that has the new keys (it ignores them) and drops them the first time it writes the file. This revision only stores and reports the fields; the overlay acts on them in later tasks (the scale of a pet stays its `width`).

Placements are stored in `$XDG_CONFIG_HOME/murasaki/pets.json`, written atomically. An invalid file (or one with a single invalid entry) is renamed to `pets.json.bad-<UTC time>` and the daemon starts with no pets.

### Workshop metadata and the transcode cache

For Anima Engine items the daemon asks Steam's public `ISteamRemoteStorage/GetPublishedFileDetails/v1` endpoint (POST, never with a key, all ids in one batch of up to 100) for the title, tags and preview image. The answer is cached in `$XDG_CACHE_HOME/murasaki/workshop-meta.json`, keyed by id and `time_updated`, and the preview is downloaded once per version to `$XDG_CACHE_HOME/murasaki/previews/<id>.<jpg|png|gif|webp>`. Known items are asked about again after 6 hours (unknown ids after 1 hour). Offline, timeouts, HTTP errors and rate limiting (`429` with `Retry-After`) only delay the next attempt, with exponential backoff and at least one second between requests; folder names are used meanwhile. `ItemsChanged` reports the updated ids when metadata arrives. The environment variable `MURASAKI_WORKSHOP_API_URL` replaces the endpoint (for tests).

Transcoded copies live in `$XDG_CACHE_HOME/murasaki/pets/<item>/<variant>-<bucket>-<fps>-<key>.webm`, where `<bucket>` is the longest side the copy was converted to, `<fps>` the playback rate it was retimed to (`0`: the file's own timing) and `<key>` hashes the source path, mtime and size, so an edited source is converted again and the old copies are removed. A copy named `<variant>-<bucket>-<key>.webm` or `<variant>-<key>.webm` (from before the rate or the buckets existed) counts as a copy at the file's own timing while its key matches, and is removed once nothing keeps it. The daemon queues the chosen variant of a pet when it is added (or switched to) if it is heavy, and again when the library changes. The conversion is `nice -n 10 ffmpeg` with `libvpx-vp9`, `yuva420p`, `-crf 32`, written to a temporary file in the cache and renamed when it succeeds. A conversion that prints nothing for five minutes is killed and reported as failed.

### Playback rate

The converted copy does not depend on the playback rate (since revision 11): it is made once per bucket at the file's own timing (sources of 50 fps or more are halved to 30, a property of the source), and the pet's `fps` and `speed` are applied when it plays. With the GPU frame cache the pet's `FrameClock` applies them to the frames it decoded (a change needs no decode). On the libmpv path the overlay sets libmpv's `speed` to `fps` over the file's own average rate (`fps` of the variant, also for a halved copy: a source of 50 fps or more keeps its pace in it, with every other frame gone), or to the placement's `speed`, and caps the rendering with mpv's drop-only filter (docs/pets/timing.md). Changing `fps`, `speed` or the scale of a pet, removing it and placing it again start no conversion unless the pet becomes bigger than the biggest finished copy.

Copies made by earlier versions are named `<variant>-<bucket>-<fps>-<key>.webm` with an `fps` above 0: they were retimed (`setpts=N/(<fps>*TB)`, every source frame shown for 1/`fps` s). They have uniform spacing, so they serve a placement at any fixed `fps` (libmpv's `speed` is the placement's `fps` over the copy's; 1 for the same rate) and are kept until a copy at the file's timing that is as big as the pet needs exists; placements at the file's timing or with a `speed` do not use them. `buckets` and `transcode` of a variant still carry the `fps` of each copy.

`Windows()` of the overlay reports `fps`, `speed` (libmpv's), `timing`, `timingSpeed` and `fpsCap` of each pet.

### Transcode buckets

A pet is converted to the size it is shown at, not always to 650 px. The buckets are 256, 384, 512 and 650 px on the longest side (a stage, revision 17, adds 960, 1280, 1920, 2560 and 3840: its need is the long side of the animation as it is drawn on the screen, never above the source's own size and at most 3840, and a variant keeps the biggest copy and the best one for each placement). For each placement the daemon works out `need = ceil(displayed longest side x output scale)`, where the displayed longest side follows from the placement's `width` and the variant's aspect ratio and the output scale comes from `SetOutputScales` (`*` takes the largest known scale, since the daemon does not know the primary output; no scale means `1`). The need is at most the source's longest side and at most 650, and the variant is converted to the smallest bucket that is at least the biggest need among its placements (those that do not `playOriginal`). ffmpeg never upscales, so a source smaller than its bucket keeps its size.

A pet that is added, or switched to another variant, queues its bucket at once, unless a finished copy at least as big as it needs exists: a bigger copy plays scaled down, so shrinking a pet, removing it and placing it again, or changing its rate reuse the copies, and a conversion starts only when a pet needs more than the biggest copy there is. A change of `width` or `output`, `SetOutputScales` and `RemovePet` bring the conversions up to date 3 seconds after the last of them, so dragging a handle does not start a conversion per step. As before, an automatic request never retries a bucket that failed; `RequestTranscode` does.

The overlay plays, per placement, the smallest finished bucket that is at least its own need, else the biggest finished one, else the original (unless `playOriginal`). The daemon keeps at most two finished buckets per variant: the biggest one (it serves every smaller size), the best one for each placement (never removed) and the newest others up to two. `transcode` of a variant is the finished copy in use (a bigger one included, so the GUI shows "ready"), else the state of the conversion that will serve it. The rest is deleted when a conversion finishes and by the startup cleanup.

### Pet fullscreen mode, "stage" (revision 17)

An optional placement field `stage` (docs/pets/stage.md): `{mode: "off"|"output"|"desktop", fit: "contain"|"cover"|"stretch", overFullscreen: "inherit"|"show"|"hide"}`,
all keys optional and merged key by key by `UpdatePet` (`{"stage": {"mode": "output"}}` leaves `fit` as it was; `{"stage": null}` clears it). The default is off
(a pet that is not a stage has no `stage` key in `ListPets`). `output` covers the pet's `output` with the animation, `desktop` spans every output with one picture; both
ignore `x`, `y`, `width` and the pointer flags, which are kept and come back when the mode is `off`. Unknown keys of `stage` are ignored; a wrong type or value is
`InvalidArgs`. **One stage per output**: `AddPet`/`UpdatePet` refuse a stage on an output that has one (the names are compared as written; a `*` and the primary's
own connector are told apart by the overlay, which does not show the later one and says `stage: output already has a stage`), and `desktop` takes every output.
`RescuePets` turns every stage off. `Windows()` gains, for a stage only, `stage` (`output`/`desktop`), `stageOutputs`, `stageHidden` (`starting` until the policy server has answered once, then `fullscreen`, `session` or
`hidden` while it is not drawn; empty otherwise) and `stageScale` (the fraction of full resolution drawn); a pet that is not a stage has none of these keys. The names
it can add to `degraded` are the table of docs/pets/stage.md. A daemon older than 17 ignores the key and drops it the first time it writes the file.

### Puppet pets (revision 20)

A puppet item is a pet item (`kind` `pet`, `source` `local`) with `"format": "puppet"` in its summary and a `puppet` object in its full form: `backend` (`open` or `cubism`), `version`, `checked` (the sandboxed check has answered), `parameters`, `drawables`, `textures`, `physics` (counts, null until checked), `canvas` `[w, h]`
(the size of the pet, which gives its aspect ratio; 512 x 512 until the check says), `motionGroups` `[{name, motions}]`, `expressions` `[name]`, `hitAreas` `[name]`, `reasons` and `degraded` (the names of docs/pets/puppets.md section 10). Its `variants` are not animation files: `model` (the base), then
`motion:<Group>` for each motion group and `expression:<Name>` for each expression, all with the model file as `file`, the canvas as their size and `frames` 0; nothing is decoded, cached or converted for them (no `transcode` key). A pet states' `variant` may name a motion group or an expression
(docs/pets/states.md): the group is played, or the expression set, while the state shows. A Cubism puppet is listed with `status` `MissingDependency` and `statusDetail` `puppet: needs the Cubism runtime` (Murasaki ships no Core); a puppet whose check refuses it is `Invalid` with the first reason.
`Windows()` has, for a puppet pet only, a `puppet` object `{state, fps, crashes, group, expression, look}` (`state`: `starting` until the engine has drawn its first picture, then `running`; `restarting` after a crash until the next picture; `blocked` after three crashes; `refused`; `nogpu`; `crashes` is the model's count in the crash ledger, also while it is blocked; `group` and `expression` are what the pet's state asks of the model; `look` is the gaze), and the degraded names `puppet: engine restarted`, `puppet: blocked after crashes`, `puppet: no GPU`.

### Pet states and Mouse Tracking (revision 19)

Two optional placement fields (docs/pets/states.md), both off by default and per pet; a pet without them behaves exactly as before and has neither key in `ListPets`.

- `states`: an object whose keys are `idle`, `grabbed`, `moving` and `released`. Each value is `{"variant": "<name>"}` (one animation of the item) or
  `{"sequence": {"items": ["a", "b"], "order": "linear"|"random", "advance": "auto"|"click", "once": bool, "holdMs": 500..60000}}` (1 to 16 variants; defaults
  `linear`, `auto`, `false`, `4000`). `UpdatePet` merges state by state: a state in the change replaces that state whole, `{"grabbed": null}` clears one state and
  `{"states": null}` clears all of them. A state without an entry falls back (`moving` to `grabbed`, `grabbed` and `released` to `idle`, `idle` to `variant`). A
  variant that the item lacks is accepted (the item may arrive later); the pet says `states: missing variant`. The current state is runtime and never stored or
  sent through the daemon.
- `tracking`: `{"mode": "off"|"look"|"follow", "distance": 0..4000 (160), "speed": 0.1..4 (1), "returnHome": bool (true)}`, merged key by key; `{"tracking": null}` clears it.
  `follow` excludes `smartDodge`, `hoverHide` and `focusFade`: a placement with both is `InvalidArgs` (the flags are not switched off silently).

Unknown keys are ignored; a wrong type or value is `InvalidArgs` and nothing changes. A bad `states` or `tracking` in `pets.json` costs the pet that key, not the
file. Every variant a pet's states name is transcoded like the selected one. `RescuePets` sets `tracking.mode` to `off`. `Windows()` gains `state`, `stateSource`
(`variant`, `sequence:<i>`, or `layer` while a timeline's variant is shown) and `preloadBytes` for a pet that has states, `tracking` and `trackingActive` for a pet whose tracking is not the default, and the names of docs/pets/states.md in `degraded`. A daemon
older than 19 ignores both keys and drops them the first time it writes the file.

### Pet sound (revision 15)

A placement may carry `audio`: `{"asset": "<64 lowercase hex>", "mode": "none|once|animation_loop|continuous|click", "volume": 0..100, "offsetMs": -5000..5000}`.
Absent is no sound (`mode` `none`, `volume` 75, `offsetMs` 0 are the defaults of a given object, and a default sound is not written to `pets.json`). In `AddPet`
options and `UpdatePet` changes a partial object merges field by field (`{"audio": {"volume": 40}}` keeps the asset and mode), and `{"audio": null}` removes it;
a wrong type, an unknown mode, a volume or offset out of range, or an asset that is not 64 lowercase hex characters is `InvalidArgs` and nothing changes. The asset
need not exist when it is stored (a sound can be removed or arrive later): the overlay reports `audio asset missing`. A bad `audio` in `pets.json` costs the pet
its sound, not the file. `ListPets` carries `audio` for a pet that has one. What each mode does, and what silences it, is in docs/pets/audio.md. A daemon from
before revision 15 ignores the key and drops it the first time it writes the file.

### Arrange mode

`SetPetsArrange(true)` (also reachable through `murasakictl pets arrange` and the global shortcut Meta+Shift+P of `murasaki-overlay`) tells `murasaki-overlay` to accept input on the pets so they can be moved and resized. It is a session state: `PetsArrange` is `false` after a daemon restart.

## Pet imports (revision 14)

Sprite sheets, image sequences, ZIPs of frames and GIFs become plain animation files and local pets (`docs/pets/import.md` describes the job, the limits and the names). The work runs in `murasaki-petimport` behind `murasaki-sandbox`, one job at a time.

| Member | Type | Meaning |
|---|---|---|
| `ImportPet(specJson)` | `s` -> `s` | Queues a job and returns its id. `InvalidArgs` for a spec that is not valid (the message starts with `spec refused`), or when 20 jobs wait |
| `PreviewImport(specJson)` | `s` -> `s` | Reads headers only (in the helper, at most 8 seconds): `{"ok":true,"preview":{frames,width,height,totalDelayMs,estimatedBytes,degraded}}` or `{"ok":false,"error":"<name>"}`. `totalDelayMs` is -1 when only decoding tells (a GIF) |
| `ImportState()` | `s` | JSON array of the jobs (the last 50 finished ones and the unfinished): `id`, `kind`, `name`, `state` (`queued`, `running`, `done`, `failed`, `cancelled`), `progress` (0 to 1), `frames`, `degraded`, `error` (a name) and, once the pet was added, `itemId` |
| `CancelImport(jobId)` | `s` | A queued job is dropped, a running one is stopped and its work folder removed. `InvalidArgs` for an unknown or finished job |
| `RemoveImportedPet(itemId, deleteFiles)` | `sb` | Removes the local pet like `RemoveLocalPetItem`; with `deleteFiles` also the job folder of a converted pet (only a folder directly under the import root that holds a `source.json`) |
| `StartScene(petId, index)` | `si` | Starts scene `index` (counted from 0, the `scenes` of the item JSON) of an imported `.mdaa` package on a pet of that item: sets its variant and, when the scene moves it, its position. `InvalidArgs` for an unknown pet or scene, or a scene that hides the pet |
| `ImportChanged(jobId)` | signal | A job changed state or progress (at most four a second while running) |

A finished job is added to the local pets by the daemon, so a client only waits for `state` `done` and reads `itemId`. The item JSON of such a pet has `"generated": true`; an imported package (kind `mdaa`; one local pet whose path is the job folder, one variant per animation) also carries `scenes` (`[{line, show, variant, x, y}]`, `x` and `y` only when the scene moves), `autoScene` (-1: none) and `importSpec` (the values the import was made with, without source paths). An Anima Engine pet item also carries `importable` (`[{path, name, kind, size}]`, kind `zip`, `mdaa` or `video`) for files of its folder that can be imported but are not animations. A daemon without the helper fails the job with `helper failed`.

## Try it

```bash
busctl --user call io.github.uriel2121ger_art.Murasaki /io/github/uriel2121ger_art/Murasaki \
  io.github.uriel2121ger_art.Murasaki1 ListItems
busctl --user get-property io.github.uriel2121ger_art.Murasaki /io/github/uriel2121ger_art/Murasaki \
  io.github.uriel2121ger_art.Murasaki1 Status
```

## Surface API

`murasaki-surface` registers `io.github.uriel2121ger_art.Murasaki.Surface` at `/io/github/uriel2121ger_art/Murasaki/Surface`, interface `io.github.uriel2121ger_art.Murasaki.Surface1`.

| Method | Returns | Notes |
|---|---|---|
| `Outputs()` | `s` | JSON array of `{name, desktop, occluded, itemId, playing, paused, audible, hosted, interactive, pauseReason, pauseDetail, fpsCap, failure}` (`pauseReason` to `fpsCap` are revision 7, see Pause rules; `failure` is revision 10, see Visible failures). `paused` and `audible` are the policy's decision for the output (also for a hosted one, whose playback another process carries out); `hosted` is true while a host is registered for it; `interactive` (revision 13) is true while the output shows a web page or a scene, the contents that take the pointer |
| `Desktops()` | `s` | JSON array of `{id, name, number}` from the compositor |
| `ReportKWinState(s json)` | — | Internal: called by the KWin script the surface loads |
| `ReportKWinCursor(i x, i y)` | — | Internal: called by `kwin-cursor.js` with the cursor position in the compositor's global logical pixels. Only the owner of `org.kde.KWin` is heard |
| `RegisterHost(s output)` | — | A Plasma wallpaper host (`murasaki-surface --host <output>`) announces that it draws `output`. The registration belongs to the caller's unique bus name and is dropped when that name leaves the bus. Repeating it is harmless. `InvalidArgs` for an empty or over-long name; `AccessDenied` when another bus name already hosts the output (a restarted host retries until the bus has dropped its predecessor); `LimitsExceeded` beyond 8 outputs for one caller or 64 hosted outputs in total. Registration is first come, first served: any client of the session bus may register, which fits the session bus trust model (every client of it can already call the daemon) but is no authentication |
| `ReportHostPointer(s output, d x, d y, d width, d height, u buttons, i wheelX, i wheelY)` | — | Revision 13. Internal: `murasaki-overlay` in the interaction mode reports the pointer it caught above the desktop; the surface forwards it as `HostPointer` to the host of `output`. Heard only from the owner of the overlay's bus name (anyone else is ignored, no error); values that are not finite or a size of zero are dropped, `buttons` is masked to the three buttons and the wheel clamped. No reply to say anything went wrong: it is sent at the pace of the mouse |
| `UnregisterHost(s output)` | — | Drops the caller's registration for `output`; no-op if there is none |
| `SetHostVisible(s output, b visible)` | — | The plugin's containment visibility, one more pause input: while `false` the output is paused and silent (and takes no part in who owns the audio). Only the registered caller may set it; it resets when the registration ends |
| `ReportOutputHealth(s output, s json)` | — | Internal: a host says why its output shows no wallpaper, as the `failure` object below, or `""` when it does. Only the host registered for `output` is heard; the entry of `Outputs()` carries it |
| `RetryOutput(s output)` | — | The wallpaper of `output` is made again (a new delegate and, for a scene, a new engine process). Revision 10 |
| `PetAudioMuted()` | `b` | Revision 15. Whether the sounds of the desktop pets should be muted now: a window is fullscreen (on any output), or a game or a window of the `pauseApps` rules is up. A window that merely covers an output does not count. The overlay reads it at start and on `PetAudioMutedChanged(b)` |
| `SessionFacts()` | `s` | Revision 16. JSON `{outputs: {name: {fullscreen, app, occluded}}, anyFullscreen, activeApp, openApps: [..], gameRunning, onBattery, quiet, known, monitor}`: what the compositor monitor says about the session, **per output**. `fullscreen` is a window in the real fullscreen state that is visible (not minimized, on the current desktop) on that output and `app` its application class (empty when unknown); `occluded` is the wallpaper's "covered" (fullscreen **or maximized**), which is a different fact. `openApps` are the classes of the windows that exist (minimized and other desktops too; at most 64 of at most 128 characters; no titles). `quiet` is a game (GameMode) or a window of the `pauseApps` rules, which silences the pets everywhere. `known` is false when the session has no window monitor: every other field is then empty, not "false" (and before the first publication, and in the Plasma host mode, the answer is `{"known": false, "outputs": {}}`: a missing `known` is unknown). Revision 23 adds `idleKnown`, `idle` and `idleAfterMs`, present **only** when the compositor says whether the user is away (ext-idle-notify-v1; independent of `known`): `idle` is "no input for `idleAfterMs` milliseconds" (60000), so the last input was that long before the moment the fact changed; absent means the idle is unknown, not false, and the daemon falls back to logind's `IdleHint`. **Only the daemon's and the overlay's bus-name owners get this whole JSON; any other caller gets `{outputs: {name: {fullscreen, occluded}}, anyFullscreen, known}` with no application, game or battery fact** (docs/security.md) |
| `OutputFullscreen()` | `s` | Revision 16. JSON `{name: {fullscreen, app}}`, the part of `SessionFacts()` the pets' rules read; other callers than the daemon and the overlay get `{name: {fullscreen}}` without `app` |
| `HostState(s output)` | `s` | JSON `{desktop, paused, audible, frameRateLimit, span?}`: the desktop whose wallpaper the host shows (`*` without a compositor monitor), the policy's decision, and the daemon's frame rate cap. `span` (revision 7) is present only while the output shows a spanned wallpaper: `{canvasW, canvasH, x, y, w, h}`, the output's rectangle of the canvas of all outputs, which a host cannot compute because it sees one screen. An output this process has no screen for reads as `*`, playing, audible |

| Signal | Notes |
|---|---|
| `HostStateChanged(s output)` | `HostState(output)` changed; hosts re-read it |
| `HostCursor(s output, d x, d y, d width, d height)` | Sent to the host of `output` only (a signal addressed to its bus name, not a broadcast), while its containment is visible: the cursor is at `x`, `y` inside the output, whose logical size is `width` x `height` (all in the compositor's logical pixels; the host scales to its render size). At most ~30 per second |
| `HostPointer(s output, d x, d y, d width, d height, u buttons, i wheelX, i wheelY)` | Revision 13. Sent to the host of `output` only, while its containment is visible: a pointer event of the interaction mode. `x`, `y`, `width`, `height` as in `HostCursor`; `buttons` the buttons held (bit 0 left, 1 right, 2 middle; the host presses and releases the ones that changed since the last event); `wheelX`, `wheelY` a wheel movement in 1/8 degree (120 per notch, `-120` is one notch towards the user), usually 0. Ignored while the content is not interactive |
| `HostCursorLeft(s output)` | Sent to the host of `output` only: the cursor moved to another output or outside every output. The host keeps the last position |
| `HostCommand(s output, s command)` | Sent to the host of `output` only: `retry`, what `RetryOutput` asked for |
| `OutputsChanged()` | The value `Outputs()` returns changed (outputs added or removed, a desktop switched, an output covered or uncovered, content or playing state changed). Re-read `Outputs()`. |
| `SessionFactsChanged()` | Revision 16. `SessionFacts()` changed; sent at once for the first change and at most four times a second after (a burst is one signal). Re-read it |
| `OutputFullscreenChanged()` | Revision 16. `OutputFullscreen()` changed (the same coalescing) |

**Visible failures (revision 10).** When no engine would start an item (not installed, spanning without the Vulkan engine, no sandbox when one is required), the engine crashed until the restarts ran out, or nothing was drawn 15 seconds after the wallpaper was shown, the output's entry carries `failure`: `{kind, reason, engine, canTryOther}` (`kind` is `refused`, `crashed` or `noframe`; `engine` the one that was running, empty for videos and pages; `canTryOther` kept for its clients, false with the Vulkan engine) and is `null` otherwise. The wallpaper itself shows its preview dimmed with a small mark in a corner and no text; the GUI's item page and Screens page say the reason with "Retry". A frame that arrives after the time-out clears a `noframe` failure, a new item clears any. The reason is logged once.

The surface reads the daemon's `Settings()` at start and on `SettingsChanged` (defaults for a daemon older than API 3): `webFpsLimit` caps how often animated wallpapers (web pages and scenes) draw, and `pauseWhenCovered=false` keeps covered outputs playing (and a covered output may then own the audio of its item). Per-assignment property overrides (`SetAssignment` `options.properties`) reach a running web or scene wallpaper without reloading it. Scene items need the daemon's `WallpaperEngineAssetsDir`; without it the surface shows their preview.

### Presenters and hosts

The policy server is the main `murasaki-surface`: it owns the `Surface` name, the session monitors, `PlaybackPolicy` and audio ownership. Who draws follows the daemon's `presenter` setting: with `layer-shell` it draws every output and ignores hosts; with `plasma` it creates no windows at all (it keeps resolving content and deciding playback for the hosts); with `auto` it draws every output except those with a registered host (the window is destroyed on registration and created again when the host unregisters or dies). A host registers on start, re-registers whenever the policy server (re)appears, applies `HostState` (desktop, paused, audible, frame rate) and re-reads it on `HostStateChanged`. Without a policy server a host plays unconditionally on desktop `*`.

### Cursor for Plasma hosts

Plasma's desktop containment (the icons view) sits above the wallpaper and takes every pointer event, so a hosted wallpaper never sees the mouse through the plugin and mouse-driven wallpapers (parallax scenes, hover effects of web wallpapers) would stay still. On KWin the policy server therefore gets the cursor from the compositor: while at least one host is registered and visible it loads a second KWin script (`kwin-cursor.js`, next to the window monitor) that calls `ReportKWinCursor` on `workspace.cursorPosChanged`, at most about 30 times per second (the first movement at once, a burst as one trailing report, nothing while the cursor rests); with no visible host the script is unloaded and there is no traffic at all. The server maps the global position to the output under it with the screens' geometry, throttles again to the same rate, and forwards `HostCursor` / `HostCursorLeft` to the host of that output. The host turns the position into hover and move events on its offscreen window (never clicks: those belong to the desktop, unless the user turns on the interaction mode below) and keeps the last position when the cursor leaves its output, so a parallax scene does not jump. Without KWin nothing reports the cursor, and a host only sees the mouse the plugin itself receives through the bridge.

### Interacting with the wallpaper

The cursor script above passes only the *position* to a Plasma host; clicks stay with the desktop containment, so a wallpaper with buttons (a web music player, a scene with clickable parts) cannot be used. The interaction mode is the explicit way to give them the mouse, modelled on the arrange mode of the pets:

- The daemon's session state `WallpaperInteract` (`SetWallpaperInteract`, `murasakictl wallpaper interact on|off|toggle`, the global shortcut Meta+Shift+W of `murasaki-overlay`, action `toggle-interact`).
- While it is on, the overlay opens one transparent window per output whose `Outputs()` entry is `hosted` and `interactive`, covering the output (a layer-shell surface on the top layer with a keyboard interactivity of "on demand"; not available on X11, whose windows cannot take the keyboard, nor on GNOME's shell windows). The window draws a thin border and a small badge and nothing opaque; panels under it do not take clicks while the mode lasts.
- The pointer it sees (position in the window, buttons, wheel) goes to the surface with `ReportHostPointer`, and from there as `HostPointer` to the host of that output, which turns it into the same hover, press, release and wheel events a `Pointer` message of the bridge produces (`HostOutput::injectPointer`). The position travels as logical pixels plus the output size, like `HostCursor`, and the host scales it to its render size, so a scaled output needs no further conversion. At most about 60 moves a second go out (the last is never lost); presses, releases and the wheel go at once, and a window that closes with a button down sends the release first. If the overlay disappears with a button down, the surface sends the release itself (it watches the overlay's bus name).
- The mode ends by itself on Esc (when the compositor gave the window the keyboard, which a click does on KWin), on the shortcut, after 60 seconds without pointer input (and in any case 5 minutes after it began; the daemon turns the state off after 10 minutes if the overlay never did), when the session locks (`SessionPaused`), when the surface goes away, and when no output has an interactive hosted wallpaper left. It ends when the arrange mode of the pets starts, and it cannot start while that mode is on; the pets keep working as they are.
- Presses that arrive this way are real user presses: dock shortcuts of a scene (`LaunchShortcut`, which needs "a real button press seen by the host") work from them exactly as from a click that reached the plugin's `BridgeItem`.

Desktop ids: KWin virtual desktop UUIDs, Sway workspace names, Hyprland workspace ids. Other compositors report no desktops and every output uses `*`.

## Overlay API

`murasaki-overlay` (the process that draws the desktop pets) registers `io.github.uriel2121ger_art.Murasaki.Overlay` at `/io/github/uriel2121ger_art/Murasaki/Overlay`, interface `io.github.uriel2121ger_art.Murasaki.Overlay1`. The name is D-Bus activatable (it starts `murasaki-overlay.service`), and there is only ever one instance. It needs the daemon's API revision 5; against an older daemon it shows nothing. The overlay follows `ListPets`, `PetsChanged`, `TranscodeChanged`, `ItemsChanged` and `PetsArrangeChanged`; the daemon starts the unit when the first pet is added (and at start when pets are stored) and stops it when the last is removed.

The overlay also exports an object that is **not for clients**: `/io/github/uriel2121ger_art/Murasaki/Overlay/Pointer`, interface `io.github.uriel2121ger_art.Murasaki.Overlay.Pointer1`, method `ReportPointer(i x, i y)`
(the global cursor in logical pixels). It exists for the script the overlay loads into KWin while a pet hides, fades or dodges; the overlay accepts the call only from the owner of `org.kde.KWin` (docs/security.md) and the object is made when a pet first needs the cursor.

| Member | Type | Notes |
|---|---|---|
| `Windows()` | `s` | JSON array with one entry per placement: `petId`, `itemId`, `output` (the connector, `*` resolved), `degraded` (revision 12: the names of what the window does less than asked, `keep above off: always below windows` on layer-shell and `keep above off: unsupported` on GNOME), `shown` (false while the output is missing, the item unknown or the file failed three times), `paused`, `file` (what it plays: a transcoded copy when one is done, else the original), `bucket` (revision 6: the longest side of that copy, `0` when it plays the original), `mode` (`sprite` when the frames are drawn from GPU textures, `video` when libmpv plays the file) and `cacheBytes` (what those textures take on the GPU, `0` in `video` mode), `failures`, `blocked` (the overlay gave up on the pet: three failures, or the file crashed the overlay three times), `error` (only when blocked: why), and for shown pets `x`, `y`, `width`, `height` (output-logical pixels) and `interactive` (true only in arrange mode), and the pointer input: `pointer` (`global`: the overlay knows where the cursor is; `local`: only what happens on the pet's own surface), `hitMask` (`frame`: the opaque pixels of the frame on screen decide where a click lands; `silhouette`: the union of all frames; `none`: the whole rectangle), `maskBytes` (memory of the pet's masks), `hidden` (faded by hide under the cursor or focus fade) and `degraded` (names of docs/pets/behaviours.md that apply, such as `pointer: local`, `hide under cursor: clicks swallowed`, `hit mask: coarse`), and `audio` (revision 15, for a pet with a sound: `{state: off\|idle\|playing\|unavailable\|failed, mode, degraded: [names of docs/pets/audio.md]}`) |

| `PlayPetSound(s petId)` | | Revision 15. Plays the pet's sound once now, as the test button of the GUI does, whatever its mode (also `none`: the sound is chosen and not yet switched on); the offset is not applied. `InvalidArgs` for an unknown pet or one with no sound chosen |
| `PreviewPetState(s petId, s state)` | | Revision 19. Shows a state (`idle`, `grabbed`, `moving` or `released`) of the pet's `states` on the desktop for three seconds, as the GUI's Try button does. A press that starts a drag, a dodge step, the pets being hidden, paused or arranged, or the state ending by itself (released) end it. At most one look every quarter of a second per pet, and none while the pet is being held (nor the same state again while it shows). `InvalidArgs` for an unknown pet, one without `states` or not shown, a pet that is held or was just asked, or another word |
| `StopPetSounds()` | | Revision 15. Stops every sound of every pet |
| `StopOneShotPetSounds()` | | Revision 18. Stops what plays once (clicks, wraps, starts); a pet in the mode `continuous` keeps playing. The automation's "silence the pets" |
| `ResetFailures(s petId)` | | "Try again" for a blocked pet: forgets its failures and the crash record of its file and opens it again. `InvalidArgs` for an unknown pet |
| `ShortcutsChanged()` | signal | Revision 12. The global shortcuts finished registering or failed: read `ShortcutBackend`, `ShortcutTriggers` and `ShortcutState` again |
| `WindowsChanged()` | signal | What `Windows()` returns changed; read it again |
| `ToggleArrange()` | | Flips the arrange mode through the daemon (for sessions without a global shortcut) |
| `Quit()` | | Exits the overlay |
| `ShortcutBackend` | `s` property | `portal`, `kglobalaccel`, or empty when no global shortcut is bound |
| `ShortcutState` | `s` property | Revision 12. `off` (`MURASAKI_NO_SHORTCUT`), `starting` (a backend is still registering; the portal may be asking the user), `bound` or `failed`. `ShortcutsChanged` fires when it changes |
| `ShortcutTriggers` | `s` property | Revision 12. JSON object: the key bound for each global action (`toggle-arrange`, `toggle-hidden`, `rescue`, and `toggle-interact` from revision 13), as the compositor describes it, or an empty text for one that got none (another application holds the key, or the user cleared it) |
| `SessionPaused` | `b` property | The pets are paused because the session is locked or no output is on |

Pets play without sound unless they are given one (revision 15, docs/pets/audio.md) and stay on the overlay layer, so they are above fullscreen windows. They pause only while the session is locked (`org.freedesktop.ScreenSaver.ActiveChanged`, as emitted by KDE's lock screen and GNOME) or when the compositor lists no output at all. Each pet window is anchored to the top and left of its output, so its position is the layer-shell margins, and it is exactly as large as the pet is shown. In arrange mode the window covers its whole output instead, with the input region cut down to the pet's rectangle, so that pointer positions are positions on the output while the pet is dragged, scaled or resized; the edit is sent with `UpdatePet` when the button is released (a pet can not be dragged completely off its output). A pet that cannot play its transcoded copy falls back to the original; after three failures of the original it is dropped from the screen (its placement stays) until its file changes or `ResetFailures` is called, and a started file resets the count. libmpv decodes inside the overlay, so a file that crashes the process is recorded too: the overlay keeps a marker of the file being opened under `$XDG_STATE_HOME/murasaki/overlay/` and counts a marker found at startup as a crash; after three crashes the file is blocked across restarts (until its mtime or size changes, the placement is removed or `ResetFailures` is called). If the daemon disappears, the arrange mode ends (it is the daemon's session state). A pet whose output is unplugged keeps its placement and reappears with the output; `*` follows the primary output.

The global shortcut (default Meta+Shift+P, action `toggle-arrange`) is bound through `org.kde.kglobalaccel` when it is on the bus (KDE Plasma: it answers at once and is where the user's shortcuts live; the portal there may wait minutes for an approval nobody sees, so it is only the second choice), else through the XDG `GlobalShortcuts` portal (component `murasaki`, which also lets the user rebind it in System Settings). With neither, the GUI toggle and `murasakictl pets arrange` still work.

Since revision 12 the same binding carries three actions, in one `BindShortcuts` (portal) or three registrations (kglobalaccel, component `murasaki`): `toggle-arrange` (Meta+Shift+P), `toggle-hidden` (Meta+Shift+H: calls `SetPetsHidden` with the opposite of `PetsHidden`) and `rescue` (Meta+Shift+R: calls `RescuePets`). Anima's own keys are Ctrl+Shift+Space and Ctrl+Shift+R; the compositor owns a global key and Ctrl+Shift+R would take "hard reload" from browsers, so these are Meta ones. They are configurable where the compositor keeps them: the portal's own dialog (asked at the first bind) or System Settings, Shortcuts, Murasaki on KDE; a key the user changed there is kept. Conflicts: an action whose key another application holds is reported as unbound (an empty trigger in `ShortcutTriggers`, a warning in the overlay's log, and a line in the GUI settings); the others work, and a press of the unbound one counts as soon as the user gives it a key in the portal's dialog or System Settings, with no restart. The arrange shortcut is the one that has to have its key for the portal to count as bound (otherwise the next backend, kglobalaccel, is tried for all three, which keeps the Meta+Shift+P that existed before); kglobalaccel counts as bound when any action got a key, and keeps what it registered if a later registration fails. A key the user changed is shown as bound (`Ctrl+Alt+F5`). A key code of 0 (what kglobalaccel answers for a shortcut another application holds, or one the user cleared) is **no key**: the trigger is empty, never `Key 0x0`. The pets being click-through and possibly invisible, the D-Bus calls (`murasakictl pets rescue`) are the real way out when no shortcut works.

**Hidden pets.** While `PetsHidden` is true the overlay unmaps every pet window and pauses its playback; the frames, the places and the state are kept, so showing them again is instant. While the pets are hidden the arrange mode is not shown (the daemon's `PetsArrange` stays as it is and comes back with the pets), and nothing new is opened or decoded for them until they are shown. The overlay reads `PetsHidden` and `PetsArrange` again every time it connects to the daemon (a property has no signal for a daemon that restarted), and when the daemon goes away both count as off.

### GPU frame cache

A pet that fits the budget is decoded once and then played from textures. The overlay decodes the file it plays (the transcoded copy, or the original) with libavformat and libavcodec (`libvpx-vp9` for WebM, because it keeps the alpha plane; Qt's WebP plugin for animated WebP, which FFmpeg's decoder does not read), scaled to the pixels the pet covers on its screen (displayed size x output scale, never more than the file has), premultiplied, and uploads every frame as a texture. The CPU copies are freed, the libmpv instance of that pet is destroyed, and the pet then only switches textures at the animation's own frame times: it asks the compositor for a redraw when the frame changes and not otherwise. The file is opened through the `file:` protocol only, with the container taken from the extension.

The budget is `MURASAKI_FRAME_CACHE_MIB` in the overlay's environment: 128 MiB for all pets together by default, `0` turns the cache off. A pet costs `frames x width x height x 4` bytes (from the item's header; the real size once its frames are in). The pets whose cache saves most work (frame rate x pixels) are admitted first while the budget lasts; the others keep playing through libmpv. Still images are never cached. Decoding runs on one low-priority worker thread, one pet at a time, and starts only after the video of that pet has played (so a file that can not be opened never reaches the decoder first); it goes through the same crash ledger marker as playback, so a file that crashes the decoder is blocked like one that crashes libmpv. The video keeps playing until the frames are uploaded and is then replaced without a visible gap. A decode that fails or goes over its share of the budget leaves the pet on libmpv and is not tried again for that file and size until `ResetFailures`. When the pet is resized, moved to another bucket or another output scale, the new frames are built in the background and the old ones stay on screen until then.

With `MURASAKI_OVERLAY_TEST_HOOKS=1` in the overlay's environment the extra call `SimulateEdit(s petId, s kind, d a, d b)` applies the edit a pointer would (`move` by `a`, `b`; `scale` by factor `a`; `resize` by `a` pixels) so that tests can drive the arrange mode without input injection. It fails with `AccessDenied` otherwise.
