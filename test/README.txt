GC-Chiptune -- test set
=======================

Two tracks per backend: one light and one heavy, chosen by MEASUREMENT, not at
random. The factor given is real time on the host, over a 10 s render; divide by
~7.6 for the Gekko estimate (docs/STATUS.md 7.2).

  backend   file                                     host    Gekko est.
  --------  ---------------------------------------  ------  ----------
  midi      mid-light - CLASS - POD Gold ...          x555.6  x73
  midi      mid-heavy - BACKLASH - Fighting Force     x15.3   x2.0   <-- corpus worst case
  module    xm-light - Anarchy - Silly Stars intro    x1111   x146
  module    xm-heavy - DARKSiDERS - Royal Heroes      x73.0   x9.6   <-- corpus worst case

The .v2m test files are gone: the V2 synth does not hold real time on the Gekko
and the format is no longer read at all (docs/STATUS.md 13).

ONE file of the five has been converted, and it is NO LONGER a standard file --
do not replace it with the original:

  soundfont.sf2   TimGM6mb, run through tools/sf2_prep. TinySoundFont reads the
                  RIFF container by raw copy, i.e. little-endian: a raw
                  soundfont cannot work on the Gekko. The player detects this
                  and answers "soundfont not prepared".
                  Note that TimGM6mb is GPL v2: fine for personal testing, to be
                  replaced with a permissive one before distribution.

Without a soundfont, .mid files are skipped: a MIDI file contains no sound, only
notes.

Output frequency, chosen by the player according to the format (on-target
measurements, in per mille of real time, maximum; 1000 = hard limit):

                          48 kHz          32 kHz      selected
  midi at 64 voices        506 (0 dry)     321 (0)     48 kHz
  module                    29 (0)          17 (0)     48 kHz

MIDI is capped at 64 simultaneous voices. Without that cap the worst case asks
for 205 and leaves real time (1525 per mille): those are release tails piling
up, not notes. At the limit, the synth sacrifices the voices furthest into their
release -- you lose reverberation, not notes. 64 is the polyphony of the GM
expanders of the era.


Usage
-----

Copy the "chiptunes" folder to the root of a FAT32 SD card, then launch
gc-chiptune.dol (Swiss, SD Gecko in slot A / slot B, or SD2SP2).

Controls: up/down move the cursor (hold to scroll), left/right leave and enter a
folder, A opens or plays, B goes up, START plays the whole tree, X pauses,
Y cycles the repeat mode, Z toggles shuffle, the C-stick sets the volume and
skips tracks, L+R rebuilds the index.

The screen shows the backend, the output frequency, and the CPU load in per
mille of real time: 1000 = hard limit.

NOTE: this does NOT work under Dolphin, which emulates neither the SD Gecko nor
the SD2SP2 (docs/TEST-DOLPHIN.md). Real hardware is required.
