# The pet item check

`murasaki-overlay --check ITEM` and `murasakictl pets check ITEM` say whether the overlay can play an item, the way
`murasaki-scene-vk --check` does for scenes. The sweep of the pets (`tools/compat/`) and any script read the same line.

ITEM is one of:

- an Anima Engine Workshop item folder (`.../steamapps/workshop/content/3474900/<id>`): every apng, gif or webp directly inside
  it is a variant, as the importer reads it;
- an animation file: a local pet;
- `anima:<id>`: the folder `<id>` in the Workshop content dirs of Anima Engine (Steam's libraries and Murasaki's SteamCMD dir).
  The id is one folder name, never a path.

Nothing is decoded, drawn or run. The check reads the headers with the readers the importers use (`readAnimationInfo`:
chunk and block walks with seeks, never the pixel data) and lists the item's folder, so a 300 MB file costs as much as a small
one and no file is read twice. A decode of the frames against an independent decoder is the sweep's job.

## Output

One line on stdout, exit status 0 whenever a verdict was printed (a non-zero status means the check itself could not run):

```
{"ok":true,"reasons":[],"degraded":[],"engine":"murasaki-overlay","version":"0.1.0"}
{"ok":false,"reasons":["not a readable animation: a.apng"],"degraded":[],"engine":"murasaki-overlay","version":"0.1.0"}
```

The shape is the scene engine's, with the same keys in the same order. `degraded: a; b` also goes to stderr when the list is
not empty.

- **complete**: `ok` true and `degraded` empty. What it promises: every animation file has headers that read as an APNG, GIF or
  WebP of plausible size, none is over a limit below, and the frames fit the frame cache at the largest size the overlay decodes
  them at, so the pet plays from the GPU frame cache. It does **not** promise that every frame decodes: `ok` means "the headers
  are plausible", never a full decode (that is the sweep's job).
- **degraded**: `ok` true and `degraded` not empty. The item plays; each entry names something it plays without.
- **refused**: `ok` false and `reasons` not empty. Nothing of the item can play.

An entry is a name of the tables below, alone or followed by `: ` and the file or value it is about (`variant skipped: bad.gif`).
Match the part before the first `: `. The names are constants in one table (`petcheck::kNames` in `src/core/petcheck.h`); a test
keeps this document and the table equal, and checks that every entry the check prints starts with one of them.

## Refused

| Name | Meaning |
|---|---|
| `item not found` | the item directory, file or anima:<id> does not exist |
| `no animation file` | the item folder holds no apng, gif or webp file |
| `unsupported file type` | the file's extension is not one Murasaki reads |
| `unsupported container` | the item holds only a container Murasaki does not import yet (mdaa, zip, a video, a live2d model) |
| `not a readable animation` | no animation of the item has a header that reads as an apng, gif or webp (broken, truncated or implausible) |
| `not a regular file inside the item folder` | an animation file is a symbolic link out of the item folder, or not a regular file |
| `file over the size limit` | every animation is over the 2 GiB a decoder opens at most |
| `canvas over the video path limit` | every animation has a canvas side over 16384 pixels, more than the video path plays |

An item with several animations is refused only when none of them can play; the first problem found is the reason. When some
play and others do not, the item is degraded with `variant skipped`.

## Degraded

| Name | Meaning |
|---|---|
| `exceeds the frame cache budget` | the frames, at the size the overlay decodes them at most (650 px longest side), do not fit the default 128 MiB frame cache; the pet plays through the video path |
| `too many files to examine` | the item has more animation files than the check examines (64); the rest were not looked at |
| `variant skipped` | some animation files of the item are unreadable, too large or over the video limit; the others play |
| `too many frames for the frame cache` | more frames than the frame cache decodes; the pet plays through the video path |
| `canvas too large for the frame cache` | a canvas side over the frame cache's limit; the pet plays through the video path |
| `mdaa package not imported` | the item carries an .mdaa package; only its plain animations play until the user imports it ("Import this package" on the item page, or the setting `importWorkshopMdaa`) |
| `video source not imported` | the item carries a video (mp4, webm, mkv, mov, avi); it is not turned into a pet until the user imports it (Import this file) |
| `frame archive not imported` | the item carries a zip of frames or a sprite sheet source; it is not imported until the user imports it ("Import this file" on the item page, or the setting `importWorkshopFiles`) |
| `audio not played` | the item carries a sound; pets are silent |
| `program not run` | the item ships a program (never run); the animations play |

The frame cache limits (`kMaxPetFrames`, `kMaxPetCanvasSide` in `src/core/petlimits.h`) are the ones the overlay's `SpriteDecoder`
uses: 20000 frames and a canvas side of 8192 pixels. Past them the overlay keeps the pet on the video path, which plays it
(through the transcoder's display-sized copy), so the item is not refused. A pet that does not fit the frame cache is not refused either: the overlay falls back to the video path when a decode fails or
does not fit (`onSpriteDone` in the overlay controller; the log says "playing the video"), so `exceeds the frame cache budget`
is the most common degrade. The check compares `frames x width x height x 4` with the **default** 128 MiB (never the environment's
`MURASAKI_FRAME_CACHE_MIB`), at the size the overlay never exceeds when decoding: the canvas scaled to at most 650 px on its longest
side. The real decision is made per placement, at the displayed size, so a small pet may still be cached.

The video path (libmpv) loads a frame as one GPU texture, so a canvas side over 16384 pixels is refused (`canvas over the video path
limit`) although the header readers accept up to 32768; 16384 is the texture size GPUs offer, an assumption nothing here has
measured. An item examines at most 64 animation files; the rest are named by `too many files to examine`.

The extensions behind the not-imported names: `mdaa`; `mp4`, `m4v`, `webm`, `mkv`, `mov`, `avi`; `zip`; `moc3` (named only when it is all the item holds: Anima does not draw Live2D either, so alongside an animation it is no gap); `mp3`, `wav`, `ogg`,
`flac`, `m4a`, `opus`. An item that holds only mdaa, video, zip or moc3 files and no animation is refused as an `unsupported
container` (the value lists the extensions); a file named like an animation whose first bytes are a zip, an MP4, a Matroska or an
AVI is refused the same way. A program is an ELF, PE or Mach-O file, a shebang script or one of the extensions of
`scanItemRisks`; Murasaki never runs it.

## Where the numbers come from

| Limit | Value | Source |
|---|---|---|
| Largest animation file | 2 GiB | `kMaxPetFileBytes` (`src/core/petlimits.h`, `kMaxLocalPetBytes`) |
| Canvas of a header | 1 to 32768 per side | `readAnimationInfo` |
| Frames and canvas side for the frame cache | 20000, 8192 | `src/core/petlimits.h` |
| Canvas side of the video path | 16384 | `kMaxPetVideoSide` |
| Chunks a header reader walks | 2 GiB in 4 KiB pieces (524288 + 1024) | `kMaxPetChunks` |
| Animation files examined per item | 64 | `kMaxCheckedFiles` |
| Frame cache memory | 128 MiB (`MURASAKI_FRAME_CACHE_MIB`, at most 16384) | `src/core/petlimits.h` |
| Transcode sizes | 256, 384, 512, 650 | `kTranscodeBuckets` (`src/core/transcodebuckets.h`) |
