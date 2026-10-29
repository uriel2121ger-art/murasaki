# Downloading Workshop items with SteamCMD

Murasaki can download Wallpaper Engine (and Anima Engine) Workshop items without the Steam client, using **your own
Steam account** through **SteamCMD**, Valve's official command-line tool. It is optional: items downloaded by Steam keep
working exactly as before, and nothing here changes how Murasaki behaves unless you sign in.

## What it needs

- SteamCMD installed by you: on Ubuntu and Debian `sudo apt install steamcmd` (it is in the `multiverse`/`non-free`
  sections). Murasaki finds `steamcmd` on `PATH` or in `/usr/games`; `MURASAKI_STEAMCMD=/path/to/steamcmd` overrides it.
- A Steam account that **owns Wallpaper Engine** (and Anima Engine for pets). Murasaki does not impersonate any app,
  does not download anonymously and does not get around ownership: SteamCMD asks Steam for the item with your account, and
  Steam decides.
- Restart the Murasaki service after installing SteamCMD, so that it sees it.

The Steam client does not need to be installed or running.

## Signing in

**GUI.** *Settings → Steam account (SteamCMD) → Sign in…*, or press **Download with SteamCMD** on a Workshop item. Enter
your account name and password. Depending on your account, SteamCMD then asks for a **Steam Guard code** (from the email
Steam sent, or from your authenticator app) or for you to **confirm the sign-in in the Steam Mobile app**; the dialog shows
whichever it is. A wrong password or code, or too many attempts, are reported with Steam's own message.

**Terminal.**

```bash
murasakictl steamcmd login myaccount      # asks for the password (not echoed) and, if needed, the Guard code
murasakictl steamcmd status
murasakictl steamcmd logout
```

**Your password is never stored**: not in Murasaki's files, not in its settings, not in logs, not on a command line. It
travels once, over your own D-Bus session bus, from the dialog (or the terminal) to the Murasaki service, which types it on
SteamCMD's terminal; the service never stores it. SteamCMD keeps its own login token (that is how it avoids asking again); Murasaki keeps
only your account name, to fill in the dialog next time.

If the token expires, the next download fails with "Your Steam session has expired. Sign in again." and the account shows
as signed out.

**Signing out** runs SteamCMD's own logout, so that it holds no session for the account, and makes Murasaki forget the
account name.

## Downloading

On a Workshop item's page, **Download with SteamCMD** (next to **Open in Steam**) queues it. The page shows the progress and
a **Cancel** button; downloads run one at a time. When it finishes, the item appears in the library by itself and **Use
now** works as usual.

```bash
murasakictl steamcmd download 1081733658 2002018810     # Wallpaper Engine items
murasakictl steamcmd download --app pets 3474900123     # Anima Engine pets
murasakictl steamcmd cancel [item id]                   # one download, or all of them
murasakictl steamcmd status
```

Items go to `~/.local/share/murasaki/steamcmd/steamapps/workshop/content/<app>/<id>` (override with
`MURASAKI_STEAMCMD_DIR`). Murasaki reads that directory as one more Workshop folder, next to Steam's, and never writes into
items. If an item exists in both, the usual rule for duplicates applies (the first playable one wins).

## Limits

- Items you have not subscribed to or bought can fail with "Access Denied" or "No subscription": that is Steam's answer.
- Updates are not automatic: download an item again to refresh it.
- The Workshop page itself is still Steam's website; browsing and subscribing there is unchanged.

## En espanol (resumen)

Con SteamCMD (la herramienta oficial de Valve) Murasaki puede descargar items del Workshop **con tu propia cuenta de
Steam**, sin el cliente de Steam. Instala `steamcmd` tu mismo (`sudo apt install steamcmd`) y reinicia el servicio de
Murasaki. Tu cuenta debe poseer Wallpaper Engine. Inicia sesion en Ajustes (o con `murasakictl steamcmd login CUENTA`),
introduce el codigo de Steam Guard o confirma en el movil si te lo piden, y usa "Descargar con SteamCMD" en la pagina de
un item. **La contrasena nunca se guarda**: se escribe una sola vez en la terminal de SteamCMD, que conserva su propio
inicio de sesion. Cerrar sesion termina tambien la sesion de SteamCMD.
