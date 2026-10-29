# How fast a pet plays

The user's decision (2026-10-21): the best possible, adjustable by the user and by their resources. Three parts: the rule for the
delays a file writes, the choice of timing of each pet, and a limit on how often a pet may change frames.

## The delay of a frame

A frame shows for the delay its file gives it, after one rule, the same for GIF, APNG and WebP
(`petFrameDelayMs` in `src/core/petlimits.h`, the only place):

> A delay of 10 ms or less shows for 100 ms. A longer delay is kept as it is.

0 and 1 centisecond are what encoders write for "as fast as you can", and what a GIF with no graphic control block means. Browsers
apply the threshold to GIF; applying it to APNG and WebP too is our choice, not something checked against their code (an
assumption: a file made with 10 ms frames on purpose plays at 100 ms here). Before this rule GIF took 100 ms, APNG and WebP
10 ms, and FFmpeg's APNG demuxer 66 ms for an APNG delay of 0.

Where it is applied: the header readers (`animationinfo.cpp`: the rate a variant is said to have), the decoder
(`SpriteDecoder::decode`, `probe`, WebP through Qt's plugin; for APNG the FFmpeg demuxer is opened with `default_fps=10`, so a
zero delay arrives as 100 ms) and the sweep (`tools/compat/pets_reader.py`, an independent implementation of the same rule).
One known difference: a GIF frame with no graphic control block that comes after frames that have one repeats their delay in FFmpeg's
demuxer (so the decoder plays it for that long) while the header reader counts it as 100 ms; GIFs written like that are rare, and the sweep would
report the difference as a duration mismatch.
The sprite decoder does not apply it to the transcoder's WebM, whose frame times are a copy's own. The video path (libmpv, and the WebM copies the transcoder writes for heavy files) gets the rule where FFmpeg lets it:

- APNG: the transcoder opens the file with the demuxer options `max_fps=99` and `default_fps=10`, so a delay of 0, 1 ms or 10 ms becomes 100 ms in the copy
  (checked with a real ffmpeg on a file of 20 frames of 1 ms and one of 0: a copy of 2 s each, `tst_transcodemanager`).
- GIF: FFmpeg replaces a delay of 0 by 100 ms by itself, but a delay of 1 centisecond stays 10 ms and no demuxer option changes that. A heavy GIF full of
  1-centisecond frames therefore plays faster in a copy (and in libmpv) than in the frame cache.
- Animated WebP: the transcoder's `webp_pipe` demuxer does not read animations, so the rule does not come into it.
- libmpv playing an original: it uses FFmpeg's defaults (an APNG delay of 0 is 15 fps there).
- A pet with a fixed `fps` (the default, 24): the copy is made once at the file's timing, whatever the rate (so changing the rate, or the scale,
  converts nothing), and the video path reaches the rate with libmpv's `speed` = `fps` over the file's average rate. A file with uneven delays
  therefore keeps its unevenness on the video path, while the frame cache shows every frame for exactly `max(1, round(1000 / fps))` ms: the
  average pace is the same, the rhythm is not. A speed multiplier is libmpv's `speed` as well and agrees on both paths. (Before, the transcoder
  retimed a copy per rate; those copies are still used by the pets of exactly their rate.)

A pet that moves between the paths can therefore change rhythm for files with uneven delays under a fixed rate, and speed for files with tiny GIF
delays, only while it is on the video path (a heavy file, or one that did not fit the frame cache). Making the video path agree would take the
frame times of a copy to be rewritten per rate or a rewrite of the GIF's delays before conversion; it is left open.

## Converted copies

The WebM copies of heavy files are made once per size bucket at the file's own timing and shared by every rate and speed of every pet of the variant. A
copy that is bigger than a pet needs serves it, scaled down: shrinking a pet, removing it and placing it again, or changing its rate or scale start no
conversion, only a pet that needs more than the biggest copy does. The biggest copy is never the one that is evicted. Copies made by earlier versions as
`<bucket>-<fps>-<key>.webm` were retimed to that rate, every source frame 1/`fps` s apart. That uniform spacing makes them serve a pet at ANY
fixed rate, exactly: libmpv's `speed` is the pet's rate over the copy's, and the frame cache's fixed step is the same pace. They stay until a copy at
the file's timing as big as the pet needs exists (then they go), and a pet at the file's own timing or at a speed multiplier ignores them, since the
file's delays are gone from them. So a user with pets at 24 fps and copies `-384-30-` is converted nothing, whatever fps or scale they set afterwards.
A pet whose rate is exactly the copy's plays it at speed 1.

A source of 50 fps or more is halved to 30 in a copy at the file's timing (every other frame is dropped, the wall time is kept). A fixed rate keeps
meaning "every source frame for 1/`fps`", so the speed on such a copy is `fps` over the source's rate (24 over 60 = 0.4, not 24 over 30), and the
frame cache plays the copy's delays at that ratio. Settings saved as `fps` 0 by development builds stay at the file's timing: only entries with no
`fps` key get 24, and nothing migrates the others.

Details in docs/dbus-api.md, "Playback rate" and "Transcode buckets".

The sweep (`pets_sweep.py`) no longer calls the sum of such delays a defect: the file gets a note, "N of M frames carry a delay
of 10 ms or less and play for 100 ms", and the record holds `delay_rule` with the file's sum and the played one.

## The timing of a pet

Two fields of a placement (`fps`, `speed`), three modes, in `src/core/pettiming.h`:

| mode | fields | every frame shows for |
|---|---|---|
| file | `fps` 0, `speed` 1 | its delay, by the rule above |
| fixed rate (**default: 24 fps**) | `fps` 1 to 120 | `max(1, round(1000 / fps))` ms, as Anima Engine steps its pets. 24 fps is the default of every pet (`kDefaultPetFps`, `petlimits.h`: the user's choice, "it looks right, neither slow nor sped up"); 15 fps is Anima's own rate and is offered as such |
| speed | `fps` 0, `speed` 0.25 to 4 | its delay divided by `speed` |

A fixed rate and a speed other than 1 do not go together. Setting one of them alone (`{"fps": 15}`) leaves the other neutral; asking
for both is an error.

It is a setting of the pet, stored with its placement in `pets.json` (the key `speed` is new; a file without it is a speed of 1;
a pet stored with no `fps` gets 24, Anima pets included: before this, new Anima pets started at 30 fps). `{"fps": 0}` alone keeps the current speed:
the file's own timing is both `fps` 0 and `speed` 1.

The upgrade is one way: a `pets.json` with `fps` above 60 or a `speed` key, read by a daemon from before revision 11, is an invalid file there (its
limit is 60), so that daemon quarantines it as `pets.json.bad-<time>` and starts with no pets. Keep a copy if you go back.

Where it is set:

- the GUI, the "Speed" choice of each pet;
- `murasakictl pets timing <pet id> file | fixed [fps] | speed <factor>`, and `--fps` / `--speed` on `pets add`;
- the daemon, `AddPet` and `UpdatePet` with `fps` and `speed` (docs/dbus-api.md, revision 11). Against a daemon older than that the GUI disables
  the Speed box and the two pet settings (with a tooltip) and `murasakictl` says the daemon is too old, because that daemon would drop `speed` without a word. The overlay follows `PetsChanged`
  like for any other field.

A change is live. The frame cache keeps the file's delays in the frames it decoded and the pet's `FrameClock` applies the timing
on top: the frame on screen goes on and the next ones follow the new timing, with no decode and no flicker. (A pet that is not in
the frame cache plays through libmpv: a speed is its `speed` property, a fixed rate is its `speed` too, or a copy the daemon
converted at that rate.)

## Skipping frames, never slowing down

What frame shows is decided by the wall clock, not by counting ticks: `FrameClock::update(now)` finds the frame whose time it
is. A timer that fires late shows a later frame and a frame that nobody had time to draw is skipped; the animation keeps its
length in seconds.

## The limit on frames per second

The most a pet may change frames per second (`FrameClock::setCap`): a frame that is due sooner waits, and the frames that would have
come meanwhile are skipped. It comes from three places, and the lowest wins:

1. The daemon's setting `petMaxFps` (0 to 120, default 0: no limit), in the GUI settings.
2. While the machine saves power, `kPetEnergySaveFps` (15, the cap wallpapers get with `batteryMode` `lowfps`): on battery
   (UPower `OnBattery`) or with the power-saver profile (power-profiles-daemon: `org.freedesktop.UPower.PowerProfiles` when it is there, else the older name `net.hadess.PowerProfiles`; `ActiveProfile`).
   The daemon's setting `petSaveEnergy` (default true) turns it off. A desktop on a UPS that UPower reports as a battery counts as being on battery: turn the
   setting off there. Without UPower or power-profiles-daemon the machine counts as
   plugged in and not saving. It uses the watchers of the wallpapers (`PowerWatcher`); `PowerProfileWatcher` is new, because the
   scene engine reads no power profile (its battery cap comes from the surface through `batteryMode`).
3. The pet itself, when it falls behind: the controller of the scene engine (`FpsController`, 60, 45, 30, 20 fps below the limit
   or the display's rate, with its hysteresis) is fed how late the pet's timer fired. The same class, built into the sprite
   library. Each sample is clamped to four intervals, so one stall (the session slept) does not count as a whole window of load. A late timer is the only load signal here: there is no GPU time to read in the Qt Quick item, so it measures the
   machine's response, not the GPU's.

The limit affects a pet only where its frames are shorter than the limit's interval; a 12 fps GIF never notices a 15 fps cap.
The libmpv path gets the same limit through mpv's `fps` filter, which resamples (it also repeats the frames of a slower animation), so the overlay
passes it only to a pet whose animation steps faster than the limit (`PetView::videoFpsCap`, computed from the rate it really plays at); a slower one gets none.

`Windows()` of the overlay reports `timing` ("file", "fixed" or "speed"), `timingSpeed` and `fpsCap` for each pet.

## Tests

`tst_pettiming` (the clock: modes, live change, skipping, caps, pause), `tst_petstore` (fields and defaults), `tst_settingsstore`,
`tst_animationinfo` (one rule in three formats), `tst_spritedecoder` (decoded delays), `tst_overlaycontroller` (the limit follows
settings and power), `tst_buspropertywatcher` (power profile), `tst_spriteitem` (live change on a window), `tst_ctlutil`,
`tools/compat/test_pets_sweep.py` (the rule in the sweep).
