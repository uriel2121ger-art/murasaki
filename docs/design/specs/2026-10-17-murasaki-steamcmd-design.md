# Murasaki subproject 13: optional Workshop downloads through SteamCMD

Date: 2026-10-17
Status: written for the user, who asked for a way to download Workshop items without the Steam client. Decisions below
are defaults; the user may revise them.

## Goal

Let a user who owns Wallpaper Engine (and, for pets, Anima Engine) download Workshop items with **their own Steam
account**, through Valve's **official SteamCMD**, when the Steam client is missing, not running, or not preferred.
Nothing about this replaces the Steam-client path: items downloaded by Steam keep working exactly as before.

## Decisions

1. **Official tool, own account, no impersonation.** Murasaki runs `steamcmd` (from the distribution package, found on
   `PATH`, in `/usr/games`, or at `$MURASAKI_STEAMCMD`) as the user. It logs in with the user's account and asks for
   `workshop_download_item 431960 <id>` (or `3474900` for pets). No App ID is faked, nothing is downloaded anonymously,
   no Steam network protocol is reimplemented, and no credentials leave the machine except to Steam through SteamCMD.
2. **The password is never stored.** Not on disk, not in the settings, not in logs, not on a command line. The GUI (or
   `murasakictl`) hands it to the daemon once over the user's D-Bus session bus; the daemon writes it to SteamCMD's
   terminal; the daemon never stores it and does not keep its own copy of the bytes it wrote. SteamCMD keeps its own login token (that is how it avoids asking again); Murasaki stores only
   the account name, to prefill the dialog.
3. **Login uses SteamCMD's own prompts.** SteamCMD runs in a pseudo-terminal (echo off) driven by the daemon, which
   recognises the prompts: password, Steam Guard code (email or authenticator) and "confirm in the Steam Mobile app". The
   GUI shows a dialog for each; the daemon exposes the current prompt in its status. A wrong password or code, a rate
   limit and a timeout end the login with SteamCMD's message.
4. **Downloads** run one at a time, each as `steamcmd +force_install_dir <dir> +login <user> +workshop_download_item
   <app> <id> +quit`, with `<dir>` = `$XDG_DATA_HOME/murasaki/steamcmd`. Progress comes from SteamCMD's
   `Update state ... progress: N (a / b)` lines. A job can be cancelled (the process is stopped); a failure carries
   SteamCMD's reason ("Access Denied", "No subscription", a login that has expired...). A login that SteamCMD asks for
   again during a download fails the job with "sign in again" and returns the account to signed-out.
5. **Library.** `<dir>/steamapps/workshop/content/431960` and `.../3474900` are extra Workshop roots of the library,
   indexed read-only like Steam's (same ids, `we:<id>` and `anima:<id>`; an id also present in Steam's library keeps the
   existing duplicate rule). The Workshop page's installed/downloading state also reads the `appworkshop_*.acf` SteamCMD
   writes there. The directory is watched, so a finished download appears by itself.
6. **Works without the Steam client**, and without the Steam client running; it needs `steamcmd` installed (the user
   installs it: `apt install steamcmd`) and a Steam account that owns the app.
7. **Clear UI.** Everything SteamCMD-related says it uses the user's own Steam account and their ownership of Wallpaper
   Engine, and links nothing else. Settings has an account section (sign in, sign out, status); the Workshop page offers
   "Download with SteamCMD" next to "Open in Steam" (the primary action when no Steam client is found), with progress,
   cancel and errors.
8. **Sign out** runs SteamCMD's `+logout` and forgets the account name; SteamCMD then holds no session for it.

## API (revision 9)

`SteamCmdStatus() -> s` JSON `{available, program, state, user, prompt, message, jobs}` where `state` is `unavailable`
(no steamcmd), `logged-out`, `logging-in` or `logged-in`; `prompt` is `""`, `password`, `guard` or `confirm`; `jobs` is a
list of `{appId, itemId, state: queued|running|done|failed, progress 0..1, error}` (the last 20 finished ones stay).
`SteamCmdLogin(user)`, `SteamCmdSubmit(kind, secret)` (`password` or `guard`), `SteamCmdCancelLogin()`,
`SteamCmdLogout()`, `SteamCmdDownload(appId, itemId)`, `SteamCmdCancel(itemId)` (empty: everything) and the signal
`SteamCmdChanged()`. `murasakictl steamcmd status|login|logout|download|cancel`.

## Tests

A fake `steamcmd` shell script (prompts for a password and a Steam Guard code, mobile confirmation, progress lines,
failures, slow downloads) stands in for the real tool: the parser, the state machine, the queue and cancel, the D-Bus
facade on a private bus, the ctl, and the GUI client. The real SteamCMD and Steam are never contacted by the tests.

## Out of scope

Installing SteamCMD, anonymous downloads, other Steam apps, updating items automatically, and a Workshop browser of
our own (the page remains Steam's website).
