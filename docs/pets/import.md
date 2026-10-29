# Importing pets

Murasaki turns the sources Anima Engine's authoring tools take into plain animation files and local pets: a sprite sheet, a folder or list of images, a ZIP of frames, a GIF (to APNG), a video and an `.mdaa` package. The code is `src/import` (the pure logic and the job), `murasaki-petimport` (the helper), `src/daemon/petimportmanager.*` (the queue) and the daemon API of revision 14 in `docs/dbus-api.md`.

## The shape

    source --job--> <data>/murasaki/pets/imported/<id>/<name>.apng (or .webp) + source.json  --> a local pet

`<id>` is the first 24 hex digits of the SHA-1 of the canonical spec (without paths) and the SHA-256 of every source file, so the same import twice is one folder and a changed source or option is another. The output is written to `<id>.part/`, checked (the header reader must read the same frame count and size back) and moved with one atomic rename; a failed or cancelled job removes the `.part` folder, and the daemon removes the ones a crash left when it starts. Deleting the pet offers to delete the folder too (`RemoveImportedPet`); nothing else ever deletes anything.

`source.json` (version 1) holds the canonical spec, each source's file name, size and SHA-256 (never an absolute path), the output's file, frames, size, total delay, byte count and a SHA-256 over all frame pixels, the tool and the informational names that applied. With `keepSource` the sources are copied into `source/` in the folder and go away with it.

## The job

A JSON object of at most 2 MiB (room for the 20000 files of a sequence; a folder is one source), validated by one function for the daemon and the helper (unknown keys are ignored, a wrong type or range is `spec refused`).

| Key | Meaning |
|---|---|
| `kind` | `sheet`, `images`, `zip`, `gif`, `mdaa` or `video` |
| `name` | shown to the user; the file name is a sanitised form of it (letters, digits, `-_. `, 64 bytes, never a path) |
| `sources` | absolute paths: one for `sheet`, `zip`, `gif`; two or more files, or one folder, for `images` (natural order: 2 before 10) |
| `frameWidth`, `frameHeight`, `frameCount`, `startFrame`, `paddingLeft`, `paddingTop`, `spacingH`, `spacingV`, `columns`, `rows` | the sheet's grid. Anima's defaults: 64 x 64, 4 frames, from frame 1, no padding or spacing; `columns` and `rows` can only narrow the grid |
| `frameMs` or `fps` | the delay of every frame (sheet default 100 ms; images and ZIP default 24 fps, the pets' own default); not both. A GIF keeps its own delays |
| `sizePolicy` | for images of different sizes: `fit` (default: letterbox into the largest canvas, no distortion), `scale` (every image scaled to the first one's size) or `first` (refuse) |
| `format` | `apng` (default) or `webp` (`libwebpmux`; lossless unless `webpQuality` is 1 to 99) |
| `colorKey` | `{color "RRGGBB", tolerance 0..100, softness 0..100}`: off unless given; any kind. A plain colour distance in linear RGB, not a model |
| `keepSource` | copy the sources into the job folder (default off) |
| `maxSide`, `maxFps`, `startMs`, `durationMs` | video only: longest side (16 to 8192, default 1024; a smaller video is never scaled up), most frames a second (1 to 60, default 30; others are dropped by timestamp), trim (milliseconds from the start, 0 duration is to the end) |
| `keepAudio` | video only: keep the sound for phase 5 (default off). The audio itself is not decoded or played: the job keeps a copy of the video in `source/` and the record says `audio: kept`; phase 5 takes the track from there |

A sheet cell that falls partly outside the sheet is an error, not a crop. A GIF is decoded by Qt, which reads a delay of 0 or 1 centisecond as 100 ms, the same as the pets' own rule for tiny delays, so playback is identical.

## Video

`src/import/videosource.*` (library `murasaki_import_video`, libavformat and libavcodec) turns the first video stream of a file into RGBA frames for a `FrameSink`. A video is read as hostile input:

- Only the `file` protocol is open, and the demuxer is chosen from the file's content among MP4/MOV (also M4A, 3GP, MJ2), Matroska/WebM, AVI, GIF and APNG (`format_whitelist`, checked again on the probed name). A playlist, a `concat` list, an image glob, a subtitle file, MPEG-TS or a device is `video: container not allowed` when the probe can name it, else `not a video`; an MP4 whose samples are in another file is not followed (`enable_drefs` off).
- A source is a regular file of at most 2 GiB (`/dev/zero`, a pipe or a symlink to either is `source not a regular file`). Probing reads at most 8 MiB, 5 seconds and 32 frames of stream.
- One video stream only (cover art is not one). Streams other than the chosen one get no codec before probing and are discarded, so no audio, subtitle or attachment decoder is ever opened; the audio is never decoded (`keepAudio` keeps a copy of the video for phase 5, see the job table). At most two decoder threads.
- Alpha is the video's own: a pixel format with an alpha plane (`yuva*`, `rgba`, `bgra`, `gbrap`, ProRes 4444) or a VP8/VP9 WebM tagged with `alpha_mode` (decoded with libvpx, which keeps it). Anything else is opaque and says `import: opaque video (no alpha)`; the colour key can still cut a background. Alpha is straight, as the writers expect.
- Decoders come from an explicit list by codec id (H.264, HEVC, VP8, VP9, AV1 via libdav1d then libaom, MPEG-4, MPEG-1/2, ProRes, PNG, APNG, GIF, MJPEG, FFV1, QuickTime RLE, Ut Video), software only: nothing hardware, experimental or that starts a process; libvpx only for a stream tagged with alpha. Every decoder gets `max_pixels` = 35 megapixels (8K), also during probing, so a header that lies or a mid-stream size change cannot make it allocate more.
- Limits are checked against what is decoded as well as what the container claims: the picture at most 16384 a side and 35 megapixels, at most 600000 decoded frames and 300 gigapixels of decoded pictures, at most the frame setting kept (`too many frames`), and the sink's byte budget (`output over the budget`). A few damaged packets are skipped (16 in a row end the stream; what was decoded is kept). Open, probe, read and decode honour cancellation and a deadline (default 10 minutes, `deadlineMs`; `video: time limit reached`); one decode of a very large frame is not interruptible.

The settings the user can change per import (defaults from the plan; `VideoSettings` in `videosource.h`): longest side 1024 (16 to 8192; a smaller video is never scaled up), frame rate cap 30 (1 to 60; frames are dropped by timestamp, never blended, and the total length is kept), trim start and duration in ms (0 and 0 is the whole video), the frame limit (at most 20000) and the deadline. A picture with alpha is scaled in premultiplied space, so a transparent neighbour's colour does not bleed into an edge. Colours follow the stream's tags (BT.709 for HD, BT.601 for SD when untagged).

The colour key is the same tool for every source: wrap the sink in a `KeyedSink` (`keyedsink.h`) and the key is applied to each frame, with the caller's buffer untouched. Off unless the job turns it on.

## Limits and hostile input

Everything is checked from headers before anything is allocated: a source is a regular file of at most 2 GiB (a symlink is followed once and its target must be regular), a sequence at most 4 GiB in all and 20000 images, each side 1 to 8192, a sheet at most 16384 a side and 100 megapixels, at most 20000 frames, the output at most 1 GiB by default (and always at most the hard limit). The format of an image is decided by its first bytes (PNG, JPEG, WebP, GIF) and given to Qt as the format, so no other image plugin ever parses a hostile file and a file name decides nothing. A PNG whose header claims a huge canvas is refused by name from its IHDR.

A ZIP is read by `BoundedZip`: the end record must be exactly at the end (a comment cannot hide another), one disk, no ZIP64, a directory that ends where the end record starts, at most 20064 entries, members stored or deflated only, no encryption, no symlinks, one member at most 64 MiB for the importer, a compression ratio of at most 200:1 (members under 4 KiB are exempt) and a hard cap on the inflated size, the stored CRC-32 and size verified. Names are never used as paths: empty, absolute, drive-letter, `.`/`..` components, empty components, NUL or control characters, bidirectional controls, over 512 bytes and duplicates (after case folding) are refused. Nothing from an archive is written as a file; members are decoded in memory one at a time. Entries that are not images are ignored and counted; a program (`.exe`, `.dll`, `.so`, `.sh`, ...) is named as `program not run` and never touched.

## Packages (.mdaa)

`kind: "mdaa"` with one source. An `.mdaa` is a ZIP; it is read with the same bounded reader and never extracted by path. The animations it holds (GIF, APNG, animated WebP, found by their first bytes, not their names) are written by us under names we choose into the job folder and checked with the header readers; each becomes a **variant** of one local pet (`LocalPetImporter` accepts the job folder). `miniatura.png` becomes `preview.png`. `actions.mddo` is parsed by the strict parser (JSON first, then a closed literal grammar; never evaluated) into **scenes**: a line is kept when its path is a member of the package (an absolute path of the author's machine, a `..` path or a missing file is dropped and named). Scenes are listed in the item JSON (`scenes`), start by hand (`murasakictl pets scene PET N`, `StartScene`) and, with `autoScene` in the spec, a new pet of the package starts in that scene. A scene that hides the pet is listed but cannot be started yet: it needs the triggers of a later phase. Nothing in Murasaki fires the table by itself, as in Anima without `core.exe`.

`core.exe` and every other program are never extracted and never run: `core.exe` is only hashed (SHA-256 in the record, `mdaa executable not run` on the result), the others are named (`program not run: <file>`). Sounds are not played (`audio not played`). A package with `pack.json` is a triggers pack: its animations are imported, and `pack.json` and `meta.json` are read as data into the record's `pack` (tracks, actions, the screen regions kept inert; docs/pets/automation.md, "Packs"). The captured frames (`frames/region<i>.npz`, one array `frames` of shape [frames, height, width, 3], uint8) are read only by a strict `.npy` reader of ours: the fixed header grammar is parsed and nothing is evaluated, object arrays and every dtype but unsigned 8-bit are refused (so no serialised program is ever loaded), as are Fortran order, other shapes, more than 1024 frames, a side over 4096 or more than 256 MiB of data (checked on the header, before any data is read), truncated data and data after the array; each refusal has its own name (`npy: ...`, `npz: ...`). Only the names `frames/region<i>.npz` and `frames/region_<i>.npz` (i without leading zeros) map to a region, and a region that has both uses neither (`screen refs: two frames files for one region`). Only the frames a trigger uses are kept (the anchor, or the run of `seq_len` frames `seq_step` apart; at most 64 a region, 16 regions, 256 in all, the last as a hard cut), and a clip that starts past the end of the array keeps none. Cancel and the time budget are asked inside a frames file too, scaled to a longer side of at most 360 and written as pictures under `screenrefs/region<i>/step<k>.png` in the job folder, with the result in the record's `screenRefs` (`status` is `ready`, or the name of the first reason). Nothing of the live screen is involved. The picture store of the triggers (`~/.local/share/murasaki/triggers/<trigger id>/`) takes them from there when a trigger is made; the screen source itself stays inactive. Sound paths are the author's machine's: only the file name is kept (`mdaa: sound not followed`). The timelines are made on request, disabled (`mdaa: triggers pack, timelines not applied`). At most 256 animations.

## Workshop items

An Anima Workshop item that carries a ZIP, a package or a video next to its animation is **never opened at scan time**: the item check names it (`frame archive not imported`, `mdaa package not imported`, `video source not imported`) and the item JSON lists it as `importable` (`path`, `name`, `kind`, `size`). The item page offers "Import this file" / "Import this package" for each, which opens the import form with the file already chosen. The settings `importWorkshopFiles` (ZIPs and videos) and `importWorkshopMdaa` (packages) choose between `ask` (the default: only the button) and `auto`: the daemon then imports each file it has not imported yet, with the settings' defaults, once (it remembers the path, size and time in `auto-imported.json` next to the imports; a file that changed is imported again). A video follows `importWorkshopFiles` too, with `importVideoMaxSide`, `importVideoMaxFps` and `importKeepAudio`. Even automatically, a package's `core.exe` is never extracted or run and the result says `mdaa executable not run`.

## Upgrading

The importer used to leave Workshop videos alone. Since it takes them, **a user who already has `importWorkshopFiles` set to `auto` gets every video of their Anima Workshop items (up to 256 MiB each) imported the first time the daemon runs after the update**, with the settings' video defaults; each is imported once (`auto-imported.json`), the files land under the import folder and show up as new local pets. The default is `ask`, which changes nothing. A user who wants ZIPs automatic but not videos has no switch for that yet: leave the setting on `ask` and use "Import this file".

## Sandbox

The daemon starts `murasaki-petimport` as `murasaki-sandbox --profile import --src SOURCE... --out OUTROOT [--require] -- murasaki-petimport ...` and passes nothing else: the profile (`importProfile` in `src/sandbox/profile.cpp`) is the one reviewed place that says what the helper sees. It may read `/usr`, `/lib*`, the loader cache, the time zone, `/dev/urandom`, `/dev/null`, its own `/proc/self`, its own folder and the libraries its RUNPATH names (a Qt outside `/usr`), and the named sources; it may write the output root and nothing else; no TCP, no home, no GPU. Like the scene engines it runs in a mount namespace of its own (bubblewrap) first: a new network, process and IPC namespace and a tmpfs holding only the paths above, with **no runtime directory at all**, so the session bus, the Wayland and X11 sockets and systemd --user do not exist for it (Landlock alone, up to ABI 8, cannot refuse a connect to a pathname unix socket). Under `--require`, which the daemon passes by default, the profile refuses to run without that namespace (`murasaki-sandbox --report` says `unixSocketsHidden`); without `--require` it falls back to Landlock alone and says so. The sandbox clears the environment (no `QT_PLUGIN_PATH`, `QT_IMAGEIO_MAXALLOC`, `LD_PRELOAD`, bus address), sets the limits (4 GiB of address space, 60 minutes of CPU (the threads of a decoder add up; the job's own 15-minute time budget ends it first), no core files, 256 descriptors, 4 GiB per file), and makes the job die with the daemon (`PR_SET_PDEATHSIG`). Tests: `tst_sandbox` (the profile's reads, writes, network, environment, limits), `tst_sandboxargs`, and the sweep, which runs every case behind the profile with `--require`.

The setting `importRequireSandbox` (default on) makes a job fail with `sandbox unavailable` when it cannot be confined (no Landlock, or no mount namespace: some systems forbid unprivileged user namespaces); it is read again at every job. With it off, a job on a system without Landlock (or without the helper) still runs and says `import: not sandboxed` on the job. The helper also limits itself, the daemon stops a job after 30 minutes of wall time (`time budget exceeded`), the helper stops between frames after 15 minutes, one job runs at a time (20 may wait), and progress goes out as JSON lines relayed at most four a second. `PreviewImport` answers asynchronously over D-Bus: one preview at a time, the same spec shares an answer, at most 8 wait (`busy` beyond that).

Work budgets: frames x canvas of one import is at most 2 gigapixels (`output over the budget`), checked from headers; a package's output is counted as it is written. An automatic import of a Workshop file skips files over 256 MiB; they stay "Import this file". The animations of a package are copied verbatim after the header readers accept them, so a hostile GIF or WebP inside it is parsed by the decoder at playback as any Workshop animation is; that decoder is not part of this sandbox (docs/security.md).

## Names

What a refused or informational import says. A refusal is a name, optionally followed by `: ` and a detail. A test keeps this table equal to `src/import/petimport.h`.

| Name | Meaning |
|---|---|
| `sheet: frame size is zero` | the frame width or height of a sprite sheet is zero or negative |
| `sheet: frame larger than the sheet` | a frame does not fit once, after the padding, in the sheet |
| `sheet: the sheet holds fewer frames than asked` | counting from the start frame the sheet holds fewer frames than asked |
| `sheet: too many frames` | more frames than the frame cache decodes |
| `sheet: padding outside the sheet` | the padding or spacing is negative, or puts the first frame outside the sheet |
| `sheet: sheet over the size limit` | the sheet image is over 16384 pixels a side or 100 megapixels |
| `sheet: start frame is not a frame of the sheet` | the start frame is below 1 or past the last cell of the sheet |
| `zip: not a zip` | the file is not a ZIP this reader accepts (no end record, a comment trick, ZIP64, several disks, damaged directory) |
| `zip: entry refused` | a member is refused: its name, method, size, ratio or position breaks a rule (the detail says which) |
| `zip: over the size limit` | the members together are over the size or count limit |
| `mdaa: actions.mddo line refused` | a line of actions.mddo is not an action this reader accepts; the line is skipped and the others kept |
| `mdaa: actions.mddo refused` | actions.mddo as a whole is over its limits (size, lines) |
| `mdaa: not a zip` | the .mdaa file is not a ZIP this reader accepts (the detail is the ZIP reader's) |
| `mdaa: no media` | the package holds no animation Murasaki can use |
| `mdaa: path outside the package` | an actions.mddo line names a file that is not a member of the package (an absolute path of the author's machine, a .. path, or a file that is not there); the line is dropped |
| `mdaa: media refused` | a media member did not read back as an animation (header, frame count or size); the member is skipped |
| `mdaa: triggers pack, timelines not applied` | informational: the package is a triggers pack (pack.json); its animations were imported and its timelines are kept in the record (made on request, disabled) |
| `mdaa: pack refused` | a part of pack.json or meta.json is not usable (not an object, over a limit, an unknown action); that part is dropped and the rest kept |
| `mdaa: sound not followed` | informational: a sound action of a triggers pack names a file by the author's own path; only the file name is kept and nothing is read or played |
| `mdaa: frames not read` | informational: the pack carries captured frames (.npz) that could not be used (see the screen refs notes); nothing of them is kept |
| `npy: not an npy file` | a captured-frames array does not start with the .npy magic |
| `npy: version not supported` | the .npy format version is not 1.0, 2.0 or 3.0 |
| `npy: header too large` | the .npy header is over 64 KiB |
| `npy: header malformed` | the .npy header is not the fixed dictionary of descr, fortran_order and shape (the detail says what is wrong) |
| `npy: dtype not uint8` | the array is not unsigned 8-bit (|u1 or <u1); object arrays and every other type are refused and never loaded |
| `npy: fortran order` | the array is stored in Fortran order |
| `npy: shape is not frames x height x width x 3` | the array is not 4-dimensional [frames, height, width, 3], or a dimension is zero |
| `npy: over the size limit` | the array has more than 1024 frames, a side over 4096, or more than 256 MiB of data; refused before any data is read |
| `npy: truncated` | the array holds fewer bytes than its shape needs |
| `npy: data after the array` | the array is followed by bytes its shape does not account for |
| `npz: not a zip` | a frames file (.npz) is not a ZIP this reader accepts |
| `npz: no frames array` | a frames file (.npz) holds no array named frames |
| `screen refs: no frames for the region` | the pack has a screen region but no frames file for it |
| `screen refs: frames for a region the pack does not have` | the pack carries frames for a region number the pack does not list; they are ignored |
| `screen refs: two frames files for one region` | the pack has both frames/region<i>.npz and frames/region_<i>.npz for one region; neither is used, because there is no telling which is the right one |
| `screen refs: sequence shortened` | informational: the clip ends before the sequence does, so fewer reference frames were kept than seq_len |
| `screen refs: over the limit, dropped` | informational: more regions or reference frames than the import keeps (16 regions, 64 frames each, 256 in all); the rest are dropped |
| `mdaa executable not run` | informational: the package carries core.exe; it was hashed and dropped, never extracted or started |
| `program not run` | informational: an archive member is a program (.exe, .dll, .so, .sh, ...); it was ignored and never touched |
| `audio not played` | informational: the package carries sound files; they are not played yet |
| `spec refused` | the job description is not valid JSON of the expected shape, or a value is out of range (the detail says which) |
| `source not found` | a source path does not exist |
| `source not a regular file` | a source is a directory (where a file is needed), a device, a pipe or a socket |
| `source over the size limit` | a source file is over the file size limit |
| `not an image` | a source file is not an image this build reads, or its header claims a size over the limits |
| `images: fewer than 2` | an animation needs at least two images |
| `images: sizes differ` | the images differ in size and the size policy is first |
| `images: too many` | more images than the frame cache decodes |
| `zip: no images` | the archive holds no image the importer can use |
| `too many frames` | the result would have more frames than the frame cache decodes |
| `output over the budget` | the converted file would pass the output limit |
| `not enough disk space` | less is free where the file would go than the output could need (frames x canvas as raw RGBA, at most the output limit) |
| `not a video` | a source file is not a video this build reads (the detail is libav's reason), or it is damaged before its first frame |
| `video: container not allowed` | the file is a container the importer does not take: only MP4/MOV, Matroska/WebM, AVI, GIF and APNG are read (the detail names it) |
| `video: no video stream` | the file holds no video stream (cover art does not count), or no frame falls inside the trim |
| `video: decoder not available` | this build has no decoder for the video's codec |
| `video: picture over the size limit` | the video's picture is over the size limit (16384 pixels a side or 35 megapixels, which is 8K), claimed or decoded |
| `video: time limit reached` | opening, probing and decoding the video took longer than the time limit setting (default 10 minutes) |
| `helper failed` | the helper process ended without a result |
| `time budget exceeded` | the import ran longer than its time budget (checked between frames) and was stopped cleanly; nothing was kept |
| `import: not sandboxed` | informational: this import ran without the sandbox (importRequireSandbox is off and Landlock or murasaki-sandbox was not there) |
| `cancelled` | the user cancelled the import; nothing was kept |
| `sandbox unavailable` | the sandbox is required and not available on this system |
| `images: N files ignored` | informational: the folder held N files that are not images (a Thumbs.db, a README); they were skipped |
| `zip: N entries ignored` | informational: the archive held N entries that are not images; they were skipped and counted |
| `import: images scaled to one size` | informational: the images were scaled to the first image's size |
| `import: opaque video (no alpha)` | informational: the video has no alpha channel, so every pixel is opaque (the colour key can still cut a background) |
| `import: video scaled down` | informational: the video was larger than the longest-side setting and was scaled down |
| `import: frame rate lowered` | informational: frames were dropped to keep to the frame rate setting |

## murasakictl

    murasakictl pets import sheet SHEET.png --frame 64x64 --count 8 [--start N] [--padding L,T] [--spacing H,V] [--frame-ms 100]
    murasakictl pets import images DIR|FILE... [--fps 24] [--size first|fit|scale]
    murasakictl pets import zip FRAMES.zip [--fps 24]
    murasakictl pets import gif ANIMATION.gif
    murasakictl pets import video CLIP.mp4 [--max-side 1024] [--max-fps 30] [--start S] [--duration S] [--keep-audio]
    # every kind: --name, --format apng|webp [--quality 1-100], --key-color RRGGBB [--tolerance P --softness P], --keep-source, --preview, --no-wait
    murasakictl pets import state | cancel JOB | remove ITEM [--delete-files]

It prints the item id of the new pet and waits for the job (`--no-wait` prints the job id). `--preview` prints what the job would make (frames, size, an upper bound of the bytes) and stops. A wrong value exits with status 2; a refused or failed import prints the name and exits 1.

## Daemon calls (revision 14)

See `docs/dbus-api.md`: `ImportPet`, `PreviewImport`, `ImportState`, `CancelImport`, `RemoveImportedPet` and the signal `ImportChanged`. A finished job is added as a local pet by the daemon (the job's `itemId`), and the item JSON of a converted pet carries `"generated": true`.
