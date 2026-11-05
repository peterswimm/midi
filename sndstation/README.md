# Gritty Grids for sndstation

A native port of the [Gritty Grids](../demos/gritty-grids.html) bytebeat drum machine to
[sndstation](https://github.com/sndstation/sndstation), the app platform for RK3326 /
RK3566 / H700 Linux handhelds (Anbernic RG351V and RG40XX, R36S, Powkiddy RGB30, RG
CubeXX and relatives). It builds and runs on a desktop too — macOS or Linux — which is
where it was developed and measured.

> **Status: builds clean, tested headless and on a desktop; not yet run on a handheld.**
> The bytebeat engine is verified bit-exact against the web demo (809,325 samples, see
> *The nodes are bit-exact* below). The audio path, the screens and the clean exit are
> exercised here. What is untested is the thing only hardware can test: how it feels
> under a thumb on a real stick, and how the audio thread behaves on an RK3326.

Unlike the [Max/MSP port](../max/README.md), which is the 4-corner oscillator alone,
this is the whole demo: the 25-node map, both engines, the polyrhythm, per-voice A/R/P,
ratchets, and Chaos.

![Bitseq with the Formula rhythm source](docs/bitseq-formula.png)

*Bitseq, Formula rhythm: the puck sits between four programs, the readout names them and
their weights, and the three tracks are running at 16 / 12 / 9.
([Bit voice](docs/voice-formula.png) adds a 1-bit trace per lane.)*

## What it is

[Mutable Instruments Grids](https://github.com/sonic-insurgence/Gritty-Grids) stores 25
drum patterns in a 5x5 map and bilinearly interpolates between the four nearest as you
move an X/Y position. Gritty Grids replaces those 25 patterns with 25 real bytebeat
one-liners from viznut's *Collection of oneliner music formulas* (2011), each attributed
on screen as you move over it. Everything downstream is derived from the blend:

- **Rhythm source** — *Roles* gates a Four / Backbeat / Eighths skeleton with the
  blended curve; *Formula* thresholds the curve itself, so the pattern comes from the
  program and morphs as you drag.
- **Polyrhythm** — in Formula each of the three tracks has its own length (16 / 12 / 9
  by default), so the slices phase against each other over 144 pulses.
- **Two engines** — *Bitseq* renders each hit as a short grain of the interpolated
  formula's waveshape, played at the lane's own pitch, so the drum timbre morphs with
  the map. *Bit voice* makes each lane a sustained 1-bit bytebeat oscillator — Grids'
  three trigger outs repurposed as three voices.
- **Per-voice A / R**, **P** (trigger probability, which ratchets into exponential rolls
  past three quarters), and a **Chaos** knob that mutates pattern, timing and per-hit
  timbre at once.

Nothing is sampled. Every sound on screen is arithmetic on a counter.

## Controls

The puck is the instrument; everything else is a modifier.

| | |
|---|---|
| **Left stick** | move the puck — this is the whole gesture |
| **D-pad** | move the puck, slower |
| **A** | Run / Stop (a run always starts at pulse 0) |
| **B** | engine: Bitseq / Bit voice |
| **X** | rhythm source: Roles / Formula |
| **Y** | jump to the next lane's controls |
| **L1 / R1** | pick a control |
| **L2 / R2** | change it — hold to repeat, and it coarsens the longer you hold |
| **SELECT** | back to the defaults |
| **START** | quit (back to the launcher on a handheld) |

Controls the current mode has no use for are skipped: **L** (track length) only exists
in Formula, **P** only in Bit voice, exactly as the web demo hides them. **D** is density
in Bitseq and pitch in Bit voice, and reads out in the right units for each.

On a desktop the keyboard is a full pad (sndstation's own fallback: arrows, Z/X, Return,
Backspace).

## Building

You need SDL2 and a checkout of sndstation.

```bash
sudo apt install build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev
```

```bash
git clone https://github.com/sndstation/sndstation ../../sndstation
```

```bash
cmake -S . -B build -DSNDSTATION_DIR=../../sndstation && cmake --build build -j4
```

```bash
./build/gritty-grids
```

On macOS, `brew install pkg-config sdl2 sdl2_ttf` instead of the apt line.

`SNDSTATION_DIR` is required and is checked: a wrong path fails at configure time saying
what to do about it, rather than at link time.

### Switches

| | |
|---|---|
| `--res WxH` | force the layout size |
| `--shots DIR` | headless: write one BMP per screen state (both engines x both rhythm sources) and exit. No display needed. |
| `--bench SEC` | headless: render SEC seconds of audio through both engines with the puck sweeping the map, and report cost, level and clipping. No display, no device. |

`--bench` is the one to run after touching the DSP:

```bash
./build/gritty-grids --bench 20
```

```
bitseq       48.3 ms for 20.0 s audio =  0.24% of one core   peak 1.00  rms 0.090  clipped 0.001%
bit-voice    63.4 ms for 20.0 s audio =  0.32% of one core   peak 1.00  rms 0.194  clipped 0.194%
```

(An Apple M-series desktop. An RK3326 is roughly an order of magnitude slower per core,
which still leaves both engines comfortably inside one audio thread — but that number
wants measuring on the unit, not predicting.)

## Installing on a handheld

The launcher lists an app when it finds a manifest. Copy
[`gritty-grids.json`](gritty-grids.json) to `~/.local/share/sndstation/apps/` (or ship it
in a package's `/usr/share/sndstation/apps/`) with `exec` pointing at the binary:

```bash
install -Dm755 build/gritty-grids ~/.local/bin/gritty-grids
```

```bash
install -Dm644 gritty-grids.json ~/.local/share/sndstation/apps/gritty-grids.json
```

When the launcher starts it, it hands over the display, the audio device and the
machine; quitting with START brings the launcher back.

## The nodes are bit-exact

Bytebeat is written in C's integer arithmetic but almost always *heard* through a
JavaScript engine, and the two disagree: JS coerces shift operands with ToInt32 and masks
the shift count to 5 bits, while its multiplies are doubles that only wrap when a bitwise
operator consumes them. Node 1 (`(t*(t>>5|t>>8))>>(t>>16)`) is audibly nothing but that
wrap; node 24 hinges on `0x8898a989` becoming a *negative* int32 under `>>`.

So the reference for this port is the web demo, not a textbook. `test_bytebeat` holds
golden values generated by running the demo's own `NODES` array in node, plus an FNV-1a
hash over a dense sweep — every node across `t = 0..200000` and `1M..20M`, 809,325
values:

```bash
ctest --test-dir build
```

```
test_bytebeat: 25 nodes, 809325 samples, bit-exact with the web demo
```

A change to [`bytebeat.h`](bytebeat.h) that alters one byte anywhere in that range fails
the test. The one deliberate departure is documented at the top of that file: past 2^53
JavaScript loses the low bits of a product, which only the Bit voice engine's
free-running `t` reaches, after hours of continuous play. int64 keeps them. This port is
the more correct of the two and nobody will hear the difference.

## How the port differs from the demo

Three changes, all forced by the platform or by the demo being wrong:

1. **One clock instead of two.** WebAudio let the demo schedule Bitseq hits as buffers a
   tenth of a second ahead while Bit voice ran in a ScriptProcessor. sndstation gives an
   app one `render(float*, frames)`, so both engines run sample-accurate inside it.
   Chaos's *negative* timing jitter survives that only because hits are decided one pulse
   early and queued — see the lookahead in [`engine.h`](engine.h).
2. **A fixed grain pool.** Each Bitseq hit builds its 256-sample wavetable into one of 24
   pre-allocated slots. Nothing in the render path allocates, locks or calls a
   general-purpose RNG.
3. **The grid shows what actually fires.** In Bit voice the demo draws the *density*
   pattern while the engine gates on the role (or a fixed half-range threshold) and
   ignores density entirely — that control is pitch in Bit voice. Here the grid is drawn
   from the same rule the engine uses, so what is lit is what you hear.

The map, the blend, the thresholds, the envelopes, the probability curve, the ratchet
ladder, the kick's pitch drop and the 1-bit grit are all the demo's own numbers.

## Files

| | |
|---|---|
| [`bytebeat.h`](bytebeat.h) | the 25 nodes, with JavaScript's integer semantics spelled out |
| [`engine.h`](engine.h) | the map, the blend, both engines, as one render callback |
| [`main.cpp`](main.cpp) | the screen, the pad, the controls, `--shots` and `--bench` |
| [`test_bytebeat.cpp`](test_bytebeat.cpp) | the golden values and the dense-sweep hash |
| [`gritty-grids.json`](gritty-grids.json) | the launcher's manifest |
