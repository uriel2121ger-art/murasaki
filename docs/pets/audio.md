# Pet audio

A desktop pet can make a sound: one audio file per pet, one of five modes, a volume and an offset. It is **off by default** (a pet has no sound until
you give it one and a mode other than `none`), and it costs nothing while nothing plays: no process, no audio stream, no wakeups.

## Pieces

```
 murasaki-petaudio --import (sandbox)      murasakid                      murasaki-overlay                  murasaki-petaudio (on demand)
 sound file / video's sound  ---------->  <data>/murasaki/pets/audio   pets.json "audio"   PetAudioController ---stdin---> mixer -> PipeWire
 decoded once, hostile input               <sha256 of source>.wav        (placement)        schedules by wraps,              reads only our WAV
                                           + .json                                          clicks, the pause rules
```

* **Import** (`ImportPetAudio`; or `ImportItemAudio` for the sound a pet imported from a video kept with `--keep-audio`: the kept copy of the video in its job folder) turns any accepted file into an **asset**: a WAV of PCM 16-bit, stereo, 48 kHz, named by the SHA-256 of the source file. The same file is the
  same asset (nothing is written twice). This is the one place a codec touches hostile bytes: it runs in `murasaki-petaudio --import`, started by the daemon
  behind `murasaki-sandbox --profile import` (the profile of the pet imports: read the system, the helper and the file; write the sounds folder; no TCP; no
  runtime directory in its view, so no session bus, Wayland or X11 socket) whenever the helper is installed, and required (`--require`) when the setting
  `importRequireSandbox` is on; the overlay never links a codec.
* **The player**, `murasaki-petaudio`, mixes up to 16 voices (one per pet) and plays them through one PipeWire stream (role `Event`, node `murasaki-pets-audio`,
  corked when nothing plays). The overlay starts it on the first sound, inside `murasaki-sandbox` (the system and the sounds folder read-only, no TCP; the generic policy, so it does not hide the session sockets, which the player needs for PipeWire). It follows `importRequireSandbox` (default on): then it never runs without the helper and nothing opens the door; with the setting off, `MURASAKI_PETAUDIO_SANDBOX=0` turns it off, and a helper that cannot run the player (status 125 to 127) lets the next start run unconfined once, with a warning, before confinement is tried again. A player that cannot reach PipeWire (status 3) is retried after 30 s and never changes the sandbox; it leaves after 30 s with nothing to play. `MURASAKI_DEBUG_AUDIO=1` makes the overlay log the wraps it hears and every line it sends the player. It reads assets by id, never by path.
* **The overlay's `PetAudioController`** decides when: it knows each pet's sound, whether it is shown, paused or click-through, and hears of every wrap of the
  animation and every click.

## Modes

| Mode | When the sound starts | When it stops |
|---|---|---|
| `none` | never (default) | |
| `once` | when the pet becomes visible and unpaused: it appears, the overlay starts, it is shown again after Hide all, the session is unlocked, `paused` is turned off, or the sound is chosen | at its end; hiding stops it |
| `continuous` | at the same moments, looped | pausing pauses it (and it goes on from where it was); hiding stops it |
| `animation_loop` | at each wrap of the animation to its first frame (the first showing counts), plus the offset | at its end, or when the next wrap restarts it |
| `click` | when the pet is clicked (a press and release on an opaque pixel without a drag), plus the offset | at its end, or when the next click restarts it |

* One voice per pet: a new start replaces the pet's sound with a 5 ms fade, so there is no click.
* **Offset** (-5000 to 5000 ms): positive delays the start by that long after the trigger; negative starts the sound that far into the file at the trigger. An
  offset past the end of the file plays nothing.
* **Volume** (0 to 100, 75 by default, as Anima's) is linear amplitude; the master (below) multiplies it; a soft limiter keeps several loud pets from clipping.
* **Click** never plays on a pet that is click-through (it gets no pointer events; the controller checks again), nor while arranging, nor on a pet faded by
  hover hide or focus fade (its input region is empty). A locked pet still plays: lock stops dragging, not clicking.
* A **still image** never wraps, so `animation_loop` is silent there (`audio: animation does not loop`).
* Editing a pet's sound while it plays stops the old sound and starts over by the mode (a volume change alone is a gain change, no restart). The edit applies
  at once: the overlay follows `PetsChanged`.

### Sync with the animation

The wrap time is exact: `FrameClock` knows when the loop began and how long it is under the current timing (fps, speed, cap), so the sound is scheduled for the
wrap itself, whatever frame was drawn or skipped. The item **announces each wrap ahead** (up to 150 ms before it, or at the start of the last frame when frames
are longer than that), and the overlay sends the start with the wrap's time on `CLOCK_MONOTONIC`: the player places it on the sample, so the jitter of the
overlay's timer (tens of milliseconds under load) does not reach the sound. In the nested KWin run the starts land within 0.02 ms of the wrap and exactly
one loop (1260 ms) apart. A pause, a change of timing or new frames withdraw an announcement (`cancel`); a wrap the thread missed is announced late with its exact
time and played if it is less than 100 ms old. A sound already playing is not cut by a scheduled one until the new one begins; a stall over several loops starts
one sound for the latest wrap. What remains is the stream's latency (a 512-frame quantum, about 11 ms, plus the graph's) against the
picture reaching the screen one or two vsyncs after the frame is chosen; the **offset** is the user's fine adjustment. The player takes `--latency-ms` for a
device with a known delay.

A pet that plays through libmpv (a long video, not in the frame cache) has no `FrameClock`: the video item announces its wraps from mpv's own position (`time-pos`,
`duration`, `speed`): when a loop starts (mpv's `playback-restart` after the file went back to its start) it announces the next one, `(duration - position) / speed` ahead, and
a pause, another speed or another file withdraw it. That is good to about a frame (mpv's position moves with the displayed frame); in the nested run the starts were
3584 ms apart within 5 ms. `duration` of a file that is still being read can grow during the first loop, so the first announcement of such a file may be short; the
next ones are right. A pet that moves to the frame cache hands over to the sprite item's exact announcements.

## When it is quiet

| Condition | Effect |
|---|---|
| the pet has no window (hidden with Hide all, an output that went away, blocked) | stop |
| the pet is paused, the session is locked or the screens are off | pause (`continuous`) or stop (the others) |
| `petsMuted` | the master gain goes to 0 in 20 ms; sounds go on silently, so the next wrap is on time |
| a fullscreen window, a game (GameMode) or a window of the `pauseApps` rules is up (setting `petsMuteWhenFullscreen`, on by default) | every pet's gain goes to 0, back when it ends |
| the player dies, or its audio stream breaks (PipeWire restarted: the player leaves with status 4) | the pets that hold a long sound ask again at once (a new player), the others on their next wrap, click or visit; at most 3 times in a minute, then `audio failed` until `ResetFailures` |
| no PipeWire | `audio unavailable`; retried every 30 s |

The fullscreen rule comes from `murasaki-surface`. Since revision 16 it says **on which output** a fullscreen window is (`SessionFacts()`, docs/pets/session-facts.md): the pets of that output go quiet, the pets of the other outputs keep their sound; a game or a window of the `pauseApps` rules (`quiet`) silences every output. A surface of revision 15 only has `PetAudioMuted`, one answer for all outputs, and the overlay falls back to it. A window that merely covers an output (maximized) is not a reason, and a session without a window monitor has no rule.

## What it costs

Nothing is running while no pet has a sound: no `murasaki-petaudio`, no PipeWire stream, no thread. With a sound the player is a process of about 29 MiB and 0.2 % of
a core (a looping voice, mixing at 48 kHz in 10 ms steps); it leaves 30 s after its last sound. `tools/bench/overlay_cost.sh` measures it in a nested KWin with five pets
(three of 640x400x90, two of 1280x800x60 at 600 px, all on the libmpv path with the 128 MiB frame cache) over 60 s after 25 s of settling: overlay CPU 22.4 % of one core
(23.5 and 21.7 before this phase, 22.8 and 22.0 after: the same within the noise of about 1.5 points), RSS 500 to 550 MiB both; one pet with a continuous sound,
with a sound at each loop, and with the pets muted: the overlay stays within that noise and the player adds 0.18 %. (These runs use the paced test backend, not PipeWire, and were taken before the player ran confined: the 0.18 % is the cost of the mixer, not of the player on a real stream.) (The numbers are one machine, one session, a
few runs each; they are not the 4.9 % of the older measurement in behaviours.md because the pets here are bigger and the compositor is 1920x1080.)

## Settings and calls

* Settings (daemon, revision 15): `petsVolume` 0 to 100 (100), `petsMuted` (false), `petsMuteWhenFullscreen` (true).
* A pet's sound is the placement key `audio`: `{"asset": "<64 hex>", "mode": "click", "volume": 75, "offsetMs": 0}`; absent is no sound; `null` removes it; a
  partial object merges field by field; a bad one in `pets.json` costs the pet its sound, not the file. See `docs/dbus-api.md`.
* `murasakictl pets audio import|list|remove|gc|set|off|test|stop`, `pets volume N`, `pets mute`, `pets unmute`.

## Hostile input

The import reads the file as untrusted (the rules of the video source apply): only the `file` protocol, containers from a list (MP3, WAV, Ogg, FLAC,
MP4/M4A/MOV and 3GP, raw ADTS AAC, ASF/WMA, Matroska/WebM; `format_whitelist` and a check of the probed name; MP3 and AAC need a confident probe), one audio
stream (cover art and video are never decoded; every other stream gets no codec before probing), software decoders chosen by codec id from a list (MP3, AAC,
Vorbis, Opus, FLAC, ALAC, WMA v1/v2, PCM), a regular file of at most 256 MiB, at most 8 channels and 8 to 192 kHz, at most 10 minutes (checked on what is
decoded as well as on the container's claim), an asset of at most 128 MiB, at most 1,000,000 packets, a few damaged packets skipped, a deadline of 5 minutes
and cancellation through the interrupt callback. The asset is read back by a strict parser (a 44-byte header whose sizes agree with the file's). Content
decides, not the suffix; Anima's list (`.mp3 .wav .ogg .oga .flac .m4a .aac .wma`) is only the file dialog's filter. Anything refused says why, by name:

## Names

| Name | Meaning |
|---|---|
| `audio: not audio` | a source file is not audio this build reads (the detail is libav's reason), or it is damaged before its first sample |
| `audio: container not allowed` | the file is a container the audio import does not take (the detail names it) |
| `audio: no audio stream` | the file holds no audio stream (a video without a sound track, cover art only) |
| `audio: decoder not available` | this build has no decoder for the audio's codec, or the codec is not on the allow-list |
| `audio: too long` | the sound is longer than 10 minutes |
| `audio: too short` | the sound is shorter than 20 milliseconds |
| `audio: over the size limit` | the source file is over 256 MiB, or the normalised sound would pass 128 MiB |
| `audio: time limit reached` | decoding the audio took longer than the time limit (5 minutes) |
| `audio unavailable` | the pet has a sound but the system has no PipeWire to play it |
| `audio failed` | the sound player stopped and was not started again (it failed three times in a minute) |
| `audio asset missing` | the placement names an asset that is not in the store |
| `audio asset invalid` | the asset in the store is not a sound of the normalised format |
| `audio: animation does not loop` | the mode is animation_loop but the animation is a still image or a single frame, so it never wraps |
| `audio: click needs a clickable pet` | the mode is click but the pet is click-through, so it never gets a click |
| `audio: downmixed` | informational: the sound had more than two channels and was mixed down to stereo |
| `audio: resampled` | informational: the sound was converted to 48 kHz |

An import also answers with the generic names of `docs/pets/import.md` (`source not found`, `source not a regular file`, `cancelled`, `sandbox unavailable`,
`not enough disk space`).

## What is verified

Verified by tests of this repository: the mixer (sample-exact scheduled starts, offsets, loops, ramps, limiter), the asset format, the import of every accepted
container and codec and of the hostile files (renamed text, playlists, devices, truncation, lying lengths, a memory ceiling), the protocol (a fuzz of garbage
lines), the player process with the paced backend (a scheduled start within one 10 ms step of the render loop), the controller (every mode, offsets, hot edits, pause and hide, the master,
the failures), the click, the wrap, the daemon calls, ctl, and the fullscreen rule on the policy server.

Verified against a real PipeWire (`tools/test-petaudio-pipewire.sh`: a private PipeWire with a null sink and WirePlumber's "policy" profile, which probes no hardware;
the sink's monitor is recorded): the sound reaches the sink at the right level and pitch (440 Hz), a start scheduled 1.2 s ahead is heard at its time (2013 ms against 2000
asked, a render step of 10 ms and the stream's latency included), the player works inside `murasaki-sandbox`, and leaves with status 4 when PipeWire ends. The audio sweep
(`tools/compat/audio_sweep.py`: 37 good and hostile files against an independent reader) passes, also under the import sandbox.

Not verified: playing through the user's real PipeWire/WirePlumber session (routing, ducking, a device's latency, Bluetooth), compositors other than the nested KWin, and the
perceived sync at the end of a 24 fps loop.

First session on a real desktop: give a pet a sound (`murasakictl pets audio set <pet> --file clap.wav --mode click`), turn click-through off, click it; set
`animation_loop` with a short clap and listen at frame 0; lower `pets volume`, `pets mute`; lock the screen; play a fullscreen video.
