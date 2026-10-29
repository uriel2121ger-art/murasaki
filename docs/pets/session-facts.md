# Session facts

What `murasaki-surface` knows about the desktop session, per output, and publishes read-only (API revision 16). The wallpapers' pause rules, the pets' sound (`docs/pets/audio.md`)
and the next phases (the fullscreen animation mode, profiles and triggers) read the
same facts; nothing here is stored and nothing is read from the screen, the keyboard or a window's contents.

## Who may read what

Application classes are not harmless: a PWA's class names its site, a Steam game's is `steam_app_<id>`, a Wine program's can carry a document name, and the focus changes say what the user
is doing. So the full facts go **only to Murasaki's own processes** (the callers that own the bus names of the daemon or of the overlay; calls inside the process are trusted too, for
the tests). Any other process on the session bus gets the **public** form: per output only `fullscreen` and `occluded`, `anyFullscreen` and `known`; no `app`, no `openApps`, no
`activeApp`, no `gameRunning`, no `onBattery`, no `quiet`. `OutputFullscreen()` is `{name: {fullscreen}}` for them. The change signals carry no argument; they do say that
something changed (the timing of a focus change is still visible to a listener, but not the name), and they are coalesced to four a second. See docs/security.md.

## What is reported

`SessionFacts()` (JSON; signal `SessionFactsChanged`, at most four a second, the first change at once) and its subset `OutputFullscreen()` (signal `OutputFullscreenChanged`):

| Field | Meaning |
|---|---|
| `outputs.<name>.fullscreen` | a window in the compositor's real **fullscreen** state is on this output, visible (not minimized, on the current desktop). A maximized window is not fullscreen |
| `outputs.<name>.app` | the application class (app id) of that window, empty when the compositor does not say |
| `outputs.<name>.occluded` | a fullscreen or maximized window covers the output (the wallpaper's "covered") |
| `anyFullscreen` | some output has `fullscreen` |
| `activeApp` | the class of the window with the focus, empty when unknown |
| `openApps` | the classes of the windows that exist, minimized and on other desktops included, no duplicates, at most 64, each at most 128 characters. **Only classes**: no titles, no sizes, no contents |
| `gameRunning`, `onBattery` | GameMode has clients; UPower says battery |
| `quiet` | a game or a window of the user's `pauseApps` rules is up (not a fullscreen window): the pets' sound is silent everywhere |
| `idleKnown`, `idle`, `idleAfterMs` | revision 23. Present **only** when the compositor says whether the user is away (`ext-idle-notify-v1`, a notification after `idleAfterMs` = 60 000 ms without input on the seat, asked with `get_idle_notification`: an idle inhibitor counts as activity). `idle` is true after the compositor's `idled` and false after `resumed`; the last input was `idleAfterMs` before the change. Absent means **unknown**, not awake (no Wayland, a compositor without the protocol): the daemon then uses logind's `IdleHint`, unless this surface already answered since the daemon started (then it waits for it). Independent of `known` (a session with no window monitor can still know the idle). Not in the public form |
| `known` | the session has a window monitor. When false every other field is empty and nobody should read it as "nothing is fullscreen". Before the first publication, and in the Plasma host mode, the surface answers `{"known": false, "outputs": {}}`; **a missing `known` means unknown** |
| `monitor` | one line naming the source, for logs |

## Where each compositor's answer comes from

| Session | Source | Fullscreen per output | `openApps` |
|---|---|---|---|
| KDE Plasma (KWin) | the script `kwin-monitor.js` loaded into KWin | `window.fullScreen` and the window's output, visible windows of the current desktop | every normal window |
| GNOME Shell | the Murasaki extension (`data/gnome-shell`), same JSON as KWin's | `fullScreen` and the monitor of the window's frame | every normal window that is not Murasaki's own |
| Hyprland | `j/clients`, `j/monitors` over the IPC socket | a client with `fullscreen` on the workspace its monitor shows | every mapped client |
| sway | `GET_TREE` and `GET_WORKSPACES` | a container with `fullscreen_mode` in the visible workspace of each output | every leaf container (`app_id`, or the X11 class) |
| wlroots-like, COSMIC (foreign-toplevel) | `zwlr_foreign_toplevel_manager_v1` | the fullscreen state and the outputs the window entered | every window the protocol lists |
| any X11 session | EWMH: `_NET_CLIENT_LIST`, `_NET_WM_STATE_FULLSCREEN` | the output the window overlaps most, current desktop, not hidden; Murasaki's own windows are ignored | every ordinary window (docks and the like are not) |
| anything else | none | `known: false` | none |

Hyprland reports a client's state as a mode (1 maximized, 2 fullscreen, 3 both; older versions a boolean): only a mode with the fullscreen bit is fullscreen here; a maximized client still sets `occluded`.

With sway or Hyprland **and** the foreign-toplevel protocol, the compositor's own tree says which output has a fullscreen window (it knows workspaces) and the protocol adds the
complete list of open applications.

## Limits worth knowing

* **Protocol-only compositors (COSMIC, other wlroots-likes)** know a window's outputs from the foreign-toplevel protocol, which says nothing about workspaces: a fullscreen window on a
  workspace that is not shown can still count as fullscreen on its output. Sway and Hyprland do not have this problem (their own trees say which workspace is visible).
* **Output names** are the compositor's connector names; the overlay compares them with Qt's screen names. They agree on KDE, X11, sway, Hyprland and wlroots; on GNOME the extension
  reports the names the shell uses, which is unverified against Qt's.
* A class is cut at 128 characters, a list at 64 entries (`activeApp` and the fullscreen list included).
* If a surface stops answering `SessionFacts()` (an older one replaced it), the overlay reads the single `PetAudioMuted` answer again.

## Verified and unverified

*Verified*: the idle client against a fake compositor (`tst_waylandidlemonitor`) and its facts (`tst_surfacecontroller`), the parsers of each source (`tst_sessionstate`, `tst_toplevelstate`, `tst_x11state`: fullscreen vs maximized, minimized, another desktop, our own windows, the bounds),
the service's coalescing (`tst_surfaceservice`), the controller publishing the facts (`tst_surfacecontroller`), the overlay's use for the sound (`tst_surfacemutewatcher`).
*Unverified here*: a real KWin sending `idled` and `resumed`, live Hyprland, sway, COSMIC and GNOME sessions (their parsers are tested with the shapes the compositors document, not against a running compositor), and the
"windowed fullscreen" games that some compositors report as maximized (they are not fullscreen windows to the compositor, so they are not reported).
