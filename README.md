# Tuner

> [!WARNING]
> **This is an experiment, and it's not an Akai feature.** Tuner draws by writing straight into MPC's display memory
> from inside the plugin, something MPC OS doesn't offer plugins and could stop working in any firmware update. I'm not
> affiliated with Akai or inMusic. Tuner was built with AI ([Claude Code](https://claude.ai/code)). If something goes
> wrong, please **don't report it to Akai**. Open an issue [here](https://github.com/jacob-sabella/mpc-vst-tuner/issues)
> instead.

A chromatic tuner for the Akai MPC, as an insert effect, with a live needle or strobe on the MPC's own screen.

Put Tuner on the track your guitar, bass or synth comes in on, play a note, and it shows the note, how many cents
sharp or flat you are, and the frequency. It runs on MPC OS standalone devices (MPC Live, One, X, Key and Force),
loaded by MPC's built-in plugin host.

![The needle view on an MPC Key 37](docs/screenshots/needle.png)

## Why it's different

An MPC plugin's screen is a skin: pictures and controls that MPC draws, refreshed when a parameter changes. That's
fine for knobs, but far too slow and coarse for a tuner. Tuner leaves a blank area in its skin and draws into that
area itself, about 20 to 24 times a second, from a background thread inside the plugin:

- **It finds its own area.** The blank area's edges carry two colour patterns. The drawing thread looks for them on
  screen, so nothing about the screen's size, rotation or header height is hard-coded.
- **It only draws while you can see it.** On another page, under a menu, or with another plugin open, the patterns
  are gone and it leaves the screen alone.
- **No flicker.** It draws into the buffer MPC paints its own UI into, so MPC shows Tuner's frame along with
  everything else. If it can't find that buffer it draws on the screen directly (the TARGET switch picks either).

## Using it

- **NEEDLE**: the note, the cents off (cyan within 3 cents, yellow within 10, pink beyond) and a needle over a
  ±50 cent scale.
- **STROBE**: four bands that drift left when you're flat and right when you're sharp, and stand still in tune. The
  further off you are, the faster they move.
- **REF**: the pitch of A4, 430 to 450 Hz (440 by default).
- **MUTE**: silences the track while you tune; the tuner still hears it.
- **TARGET**: AUTO (MPC's own buffer when it can find it), SCREEN or SOURCE. Leave it on AUTO.

The level of the input is shown in the top left corner. Below about -50 dB the tuner treats it as silence, holds the
last note dimmed for a second and a half, then shows "---".

![The strobe view](docs/screenshots/strobe.png)

## Good to know

- Tuner hears one note at a time. Play single notes: a chord reads as whatever period its notes have in common, usually
  a low note.
- It reads 27 Hz to about 1.4 kHz, from a 5-string bass's low B to the top of a guitar neck.
- The pitch detector runs on the drawing thread, never on the audio thread, and only while Tuner's page is on screen.
- With two Tuners in a project, the one inserted last draws.
- Tested on an MPC Key 37 (Gen1) with MPC OS 3.9.1. The Gen2 build (MPC Live III, Key 61 and others) is built but
  hasn't been tried on a device.

## How it works

`src/engine.c` is the tuner: the audio thread copies the input into a ring buffer and passes the audio through.
Once a frame, the drawing thread runs YIN (de Cheveigné and Kawahara, 2002) on the last 93 ms of input decimated
to 11025 Hz, refines the period at 44.1 kHz, takes the median of the last five readings and draws.

`src/screendraw.c` finds the display, the blank area and MPC's UI buffer, runs the drawing thread, and copies each
frame in. It checks the edge patterns column by column while it copies, so a page change halfway through a frame
can't leave Tuner's pixels on the next page.

### Build and test

Needs Docker (with QEMU for the ARM images) and an [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)
checkout next to this repo (or set `MPC_VST`).

```
tests/run.sh          # pitch test against known notes, then the wrapper's host test (x86, ASan/UBSan)
vst/build.sh          # vst/build/tuner.so (Gen1) and vst/build/aarch64/tuner.so (Gen2), plus the skin
tools/make_canvas.py  # redraws vst/canvas.png from the marker colours in src/screendraw.h
```

## Vibe Coded

This project was built with [Claude Code](https://claude.ai/code): the tuner, the screen drawing, the skin and the
tests.

## Credits and license

The idea of a plugin drawing into MPC's display buffers came from Lewinator56's experiments, shared on the MPC
plugin community's Discord. Tuner's code is its own.

Tuner is MIT licensed (`LICENSE`).
