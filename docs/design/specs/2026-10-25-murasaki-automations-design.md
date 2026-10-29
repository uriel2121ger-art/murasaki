# Automations: one place, plain sentences (design)

Date: 2026-10-25. Status: design agreed with the user in a brainstorming session. 2026-10-26: phase 1 (UI only) implemented on
branch `feat/automations-ui`, waiting for the user's review; "the open applications first" in the picker is not in it (the surface
gives the session's window classes only to the daemon and the overlay, docs/security.md), so the list is alphabetical.

## Goal

The Triggers page is hard to use for most people: a form with 13 "when" kinds and 9 "then" kinds, technical fields (window classes with
`*` and `?`, milliseconds, signal files, D-Bus), actions that first need a timeline or a profile made on another page, and screen triggers
in a dialog of their own. Meanwhile the wallpaper rules (pause while gaming, when covered, on battery, for listed apps) are hidden in
Settings. The goal is one **Automations** area that anyone understands, with ready-made examples, for the wallpapers and for the pets.

References (facts only, clean-room rule): Wallpaper Engine for Windows (the original) has per-application rules (pause, stop, mute when
an app is open, focused or fullscreen) and playlists by time of day; waywallen pauses, mutes or stops by window and session state, blurs
a paused wallpaper and rotates playlists. Nothing is copied; the examples below cover what their users expect.

## Decisions (agreed)

1. **One area, two families.** "Automatizaciones" replaces the Triggers page and takes the wallpaper rules out of Settings. Each
   automation reads as a sentence: "Cuando ___, entonces ___". Families: wallpapers (pause, mute, lower fps, change wallpaper, blur)
   and pets (hide or show, mute, play a sound, play an animation or timeline, switch profile, change state).
2. **Page layout (B):** "Mis automatizaciones" on top (only those added or made, each with a switch and its sentence), and under it a
   gallery **"Ideas para empezar"**: cards added with one click, then adjustable. "Nueva" opens the editor.
3. **Editor (A): a sentence with slots.** "Cuando [condition ▾] entonces [action ▾]" in one screen; each coloured slot opens a list with
   icons. "+ añadir otra acción". Technical fields (window class by hand, cooldown in ms, signal file, D-Bus, fire name) live under
   "Más opciones", closed by default.
4. **No emojis.** Monochrome icons of the system theme (Breeze, Adwaita: `media-playback-pause`, `input-gaming`, ...), so they follow the
   desktop's light or dark theme.
5. **Applications from a list, not typed.** The app slot is a drop-down of installed applications from the XDG `.desktop` files (user,
   system, Flatpak and Snap exports), with name and icon; the open ones first. It follows installs and uninstalls live (a file watcher
   on the application dirs). The rule stores the window class / app id (desktop file id, `StartupWMClass`); typing one stays possible
   under "Más opciones".
6. **Actions are a closed list** (as today: nothing runs a program, opens a file or a web address). Several actions per automation, and
   "al terminar, deshacer" (when the condition ends, undo: the game ends, the app closes).

Conditions: abro o uso una app; juego; algo ocupa la pantalla completa; cargador conectado o desconectado; a una hora o ciertos días; cada
X minutos; me ausento X minutos; bloqueo o desbloqueo la sesión; conecto o desconecto una pantalla; pulso una tecla; aparece algo en la
pantalla (the screen trigger, same editor).

Ideas (the gallery): Modo juego (pause that screen's wallpaper, mute the pets: today's `pauseOnGame`, per covered screen since 3ca3975);
Pantalla completa (`pauseWhenCovered`); Ahorro de batería (`batteryMode` lowfps or pause); Silencio en llamadas (Discord, Zoom, Meet:
mute the pets); Siesta (idle X min: the pets sleep or hide); Buenos días (a time: the pets play something); Rotar fondos (new);
Desenfocar en pausa (new).

## Optimised phasing (each phase its own plan)

1. **UI only, no backend change.** The Automations page (layout B), the sentence editor (A) over today's trigger API, the app picker
   (`.desktop` model with a watcher, in the app), system icons, "Más opciones". The four wallpaper ideas are bound to the existing
   settings (`pauseOnGame`, `pauseWhenCovered`, `batteryMode`, `pauseApps`): their switch writes the setting, so the policy server does
   not change. Settings keeps a link "Ahora en Automatizaciones". Spanish translations with every string.
2. **Several actions and undo.** `Trigger.actions` (a list; `action` read as a list of one for old files), "al terminar, deshacer" with
   `inverseAction` generalised beyond hold shortcuts; API revision bump; `tst_automationclosed` keeps the closed lists pinned.
3. **New wallpaper actions.** Change wallpaper and rotation (a playlist by minutes or by time of day, as WE's), blur while paused, and
   per-app wallpaper rules (pause, mute or lower fps when an app is open or focused), which absorb `pauseApps`. These act on the
   wallpapers: `docs/pets/automation.md` ("a trigger acts on pets only") is amended to "acts on pets and on Murasaki's own wallpapers,
   never on anything else", and the closed-list tests grow with it.
4. **Screen triggers in the same editor**, with the guided capture, plus the first live check of the portal dialog (needs the user).

## Testing

Each phase: QML tests for the page and the editor (sentence text for every condition and action, slots, "Más opciones" closed by
default), a unit test of the `.desktop` model (parse, Flatpak/Snap dirs, live add and remove), the existing trigger and pause-policy
tests unchanged in phase 1, `tst_automationclosed` extended in phases 2 and 3, and a nested run of the GUI where it applies.

## Out of scope

Running programs or scripts from an automation, network actions, conditions that read the keyboard or the microphone.
