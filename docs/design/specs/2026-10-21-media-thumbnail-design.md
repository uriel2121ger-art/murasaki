# Media thumbnail and media events (MPRIS) for murasaki-scene-vk

Status: approved and implemented (2026-10-21); what differs from the first draft is marked *as built*. Goal: the 52 library scenes that degrade as `media thumbnail` (`usertextures` `$mediaThumbnail`
/ `$mediaPreviousThumbnail`) show the cover of what plays, and SceneScript gets the media events it expects.

## Facts found

- The engine runs inside `murasaki-sandbox` (landlock/bwrap): no session bus, no network. Everything the host tells it
  travels on the bridge socket (`src/bridge/protocol.h`, SOCK_SEQPACKET, `MsgHeader` + payload, fds by SCM_RIGHTS).
  Consumer to producer messages today: Release, RequestLinear/Shm, Pause, Properties (JSON, at most 64 KiB), Volume, Pointer,
  Resize, Scaling, Fps, Quit. The host side of the socket is `BridgeClient` / `BridgeItem` (`src/bridge/qml`), used by
  `surface/hostoutput.cpp` and `render/sceneitem.cpp`; the engine side is `core/bridgesink.cpp`, which hands a `ControlMessage`
  to the engine. Unknown message types are ignored by both ends.
- The channel carries at most 64 KiB + 4 bytes a message, so a cover cannot travel as JSON; `ShmBuffers` already sends a memfd
  by SCM_RIGHTS, which is the way to carry pixels.
- Qt DBus is already linked (`murasaki_sessionwatch`, `murasaki_plasma`, `murasakid_service`; `surface/buspropertywatcher.*`
  follows a property of an optional service). Qt Network is linked by the surface monitors and the daemon.
- The engine has dynamic textures already (video textures resolved by name in `textureNamed`).
- SceneScript side (`docs/scene-vk/scripts-api.tsv`): `MediaStatusEvent.enabled` (false today), `MediaPlaybackEvent.state`
  (always STOPPED), `MediaPropertiesEvent` (title, artist, albumTitle, albumArtist, subTitle, contentType, genres),
  `MediaThumbnailEvent` (hasThumbnail and five colours), `MediaTimelineEvent` (position, duration); the registered events are
  `mediaStatusChanged`, `mediaPlaybackChanged`, `mediaPropertiesChanged` and friends. Today none is fed.

## Design

### Host (outside the sandbox): `MediaWatcher` (new, Qt DBus, one per host process)
- Follows `org.mpris.MediaPlayer2.*` names on the session bus (`NameOwnerChanged`), reads `Metadata`, `PlaybackStatus`,
  `Position`, `Rate` of `org.mpris.MediaPlayer2.Player` and listens to `PropertiesChanged` and `Seeked`. Reads only from names
  that have an owner. Chooses the player: the one Playing (the most recent if several), else the last one that played, else none.
- Everything a player says is untrusted: strings are cut to 256 UTF-8 bytes (genres: 8 entries), control characters stripped,
  `mpris:length` / `Position` clamped to 0..24 h, bad types ignored. Updates are coalesced (200 ms); position is not streamed: it
  is sent with a timestamp and the rate on a change or a seek and the engine extrapolates.
- Cover (`mpris:artUrl`), decoded in the host, never in the engine:
  - `file://`: a regular file (no devices, fifos; symlinks resolved), at most 8 MiB.
  - `https://` (Spotify and browsers use it): downloaded by the host with QNetworkAccessManager: https only (`http://` is
    ignored), at most 4 redirects https to https, 8 MiB, 10 s, no cookies or credentials, loopback / private / link-local
    addresses refused, one download at a time, at most one new URL every 2 s, an LRU of the last 8 pictures by URL. The sandbox
    never gets network. Switch: setting `media.remoteArt` (proposal: on).
  - Decoding: `QImageReader` with a size check before reading (at most 8192 on a side and 32 Mpixel) and an allocation limit; the
    result is scaled to at most 512 x 512 (aspect kept) and converted to RGBA8 straight alpha. Anything else (data: URLs, other
    schemes, failures) means "no cover".
- Sends to each connected engine two new messages and replays the current state after the engine's `Hello` (engine restart).

### Protocol (consumer to producer, appended; as built: Murasaki's extension `src/bridge/mediaview.h`, Hello version 4 (`kMediaVersion`), message types 44 `Media` and 45 `MediaThumbnail` (43 is SpanView); the base version stays 2 and a consumer never sends them to a producer that announced less)
- `Media = 44`: JSON in the Properties encoding (`u32 length` + UTF-8, at most 16 KiB): `{"enabled":bool,"state":"playing|paused|stopped",
  "title","artist","albumTitle","albumArtist","contentType"?, "genres":[...], "position":sec,"positionAt":monotonic ms,"rate":1.0,
  "duration":sec}`. `enabled` false means no player (the engine goes back to today's behaviour).
- `MediaThumbnail = 45`: `{u32 width, u32 height, u32 generation, u32 reserved}` plus one memfd (sealed, RGBA8, exactly
  `width*height*4` bytes). Width and height 0 (no fd): the track has no cover. At most 512 x 512 (1 MiB). The engine rejects a
  mismatching size, more than one fd, or a huge size; a rejected message is dropped, never a reason to kill the engine.

### Engine
- `bridgesink` decodes both into new `ControlMessage` alternatives; the engine keeps a `MediaState` and two dynamic GPU textures
  `$mediaThumbnail` (current) and `$mediaPreviousThumbnail` (the one before the last change of cover).
- A `usertextures` entry `{"type":"system","name":"$mediaThumbnail"}` binds that texture to its slot; until the first cover and
  whenever the track has none the slot keeps the file the scene lists there (today's placeholder). The `media thumbnail`
  degrade is removed (it stays only if the host never sends media: it does not, the degrade list is static).
- Layers using the slot are redrawn when the cover changes (a texture revision, as video textures do).
- SceneScript: `MediaStatusEvent`, `MediaPlaybackEvent`, `MediaPropertiesEvent`, `MediaThumbnailEvent` (hasThumbnail, and
  the primary / secondary / tertiary colours, the text colour and the high contrast colour computed in the engine from the
  512 px cover: dominant buckets by hue, text colour by luminance; deterministic), and `MediaTimelineEvent` once a second while
  playing, extrapolated from position, timestamp and rate. Event shapes follow scripts-api.tsv; it is updated.

### Security and limits (summary)
No network and no bus in the sandbox; the host does all I/O with untrusted input; bounded sizes everywhere (strings 256 B, JSON
16 KiB, art 8 MiB in and 1 MiB out, 32 Mpixel decoded); bounded rates (200 ms coalescing, 2 s per new URL, thumbnail messages
at most one a second); no new dependency; the engine copies the memfd contents once and closes it.

## Decisions (2026-10-21)
`media.remoteArt` is the setting `mediaRemoteArt`, on by default. Player order as in the host section. Colours computed in the engine (algorithm in docs/scene-vk/scripts.md). Position is sent with the report and the engine extrapolates from the moment it arrives (as built: no `positionAt`). The address check is made on the resolved addresses (QHostInfo), IPv6 included.

## Open questions (answered)
1. `media.remoteArt` default on or off? (Spotify gives only https URLs.)
2. Several players: "the Playing one, else the last" is the proposal; a setting to pin a player is not planned.
3. Colours of `MediaThumbnailEvent`: computed in the engine (proposal), or left at fixed values in a first version.

## Changes after the security review (2026-10-21)
`MediaWants` (type 46, producer to consumer) lets a scene say whether it uses the media; nothing is read or sent before it says yes and `mediaShare` is known to be on (new setting, default on). The remote-art and share settings are tri-state in the host: unknown counts as off. Decoding and file reads run on a worker thread; formats are PNG, JPEG, WebP and GIF; 16 Mpixel; the download is direct, `Accept-Encoding: identity`, with a 15 s total timer; more address ranges are refused (documentation, benchmarking, 6to4, Teredo, site-local); strings are cut to 4096 characters before cleaning; a flood of MPRIS names evicts the oldest inactive player; `MediaHub::shared()` lives on the heap under the application object.
