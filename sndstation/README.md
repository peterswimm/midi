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
their weights, and the three tracks are running at 16 / 12 / 9. The three rings on the map
are patches bound to D-pad directions — the amber one is being morphed to, half way
there, with BPM, Chaos and the lane controls all part way between the two.
([Bit voice](docs/voice-formula.png) adds a 1-bit trace per lane;
[`--dark`](docs/bitseq-formula-dark.png) is the demo's phosphor.)*

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
- **Free lanes** — a lane can switch its sequencer off entirely and let the formula play
  its own music, clocked to the tempo and transposed by semitones. See
  *[A lane with no sequencer](#a-lane-with-no-sequencer)* below.

Nothing is sampled. Every sound on screen is arithmetic on a counter.

## Controls

The puck is the instrument; everything else is a modifier.

| | |
|---|---|
| **Left stick** | move the puck — this is the whole gesture |
| **D-pad** | move the puck, slower — until a patch is bound to that direction |
| **L3 + a direction** | bind the whole patch to that D-pad direction |
| **R3 + a direction** | unbind it |
| **A** | Run / Stop (a run always starts at pulse 0) |
| **B** | engine: Bitseq / Bit voice |
| **X** | rhythm source: Roles / Formula |
| **Y** | jump to the next lane's controls |
| **F** (per lane) | sequencer off: the formula plays itself |
| **K** (per lane) | transpose a free lane, in semitones |
| **L1 / R1** | pick a control |
| **L2 / R2** | change it — hold to repeat, and it coarsens the longer you hold |
| **SELECT** | back to the defaults, and clear the destinations |
| **START** | quit (back to the launcher on a handheld) |

### Destinations

A destination is a **whole patch, not a place**: the puck's position *and* every
parameter — BPM, Chaos, both engine switches, and all five controls on each of the three
lanes. Hold **L3** and press a direction to bind the current state to it; **R3** and a
direction unbinds. Press a bound direction and the instrument travels there. **SELECT**
clears all four along with the rest of the reset; nothing is written to disk.

Travel is a glide, not a jump, and it glides *everything*. A recall is a morph between
two patches, and the sweep through the middle is itself worth hearing — cutting straight
to the destination throws that away. A long hop takes longer than a short one, between
0.18 and 0.9 seconds, eased at both ends. The engine and rhythm-source switches cannot be
interpolated, so they snap at the start and the destination's engine carries the morph.

The stick interrupts a glide at any point, which leaves every value halfway between the
two patches. That is the point rather than a wrinkle: half of a morph is a patch you did
not have, and you can bind *that* to another direction.

Bound patches show on the map as rings at their stored position, with a pip on the side
they answer to, amber for the one being travelled to; the HUD shows how far along the
morph is. The pip is drawn geometry rather than an `↑`, because Open Sans has no arrow
glyphs at this size — worth knowing before anyone simplifies it back to a character.

### A lane with no sequencer

![A lane with no sequencer](docs/free-lanes.png)

*FOUR and EIGHTHS are free, an octave apart; BACKBEAT is still sequenced. The traces on
the right are the formulas' actual waveforms, not 1-bit squares.*

These one-liners are not oscillators — they are **music**, with their own drums, basslines
and phrasing, which is why viznut's collection is worth listening to rather than merely
hearing. Everything else in this app treats them as raw material: Bitseq chops a formula
into 256-sample wavetables and rebuilds it as a drum, Bit voice takes one bit of it and
gates the square with an envelope. Both are the sequencer talking over the tune.

Set a lane's **F** control to FREE and it stops: no gate, no envelope, no grain, no
probability, no ratchet. That lane's output is the formula itself.

**Nothing is done to the signal on the way out.** No low-pass, no DC blocker, no clip, no
grit, no dither — the byte leaves `bytebeat.h` and arrives at the device. The only thing
in the path is the zero-order hold that carries 8000 Hz up to the device rate, which is
what a bytebeat has always been played through and is where its aliasing character comes
from; interpolating there would smooth the sound off rather than preserve it. A sample at
full scale is a byte of 255, not a limiter engaging, because there is no limiter.
`test_free` renders real audio through the real engine and compares it with the formulas
computed directly, sample for sample — add a filter to that path and the test fails.

#### Quantised tempo and key

A free lane's pitch and its tempo are the same number, because a bytebeat has no pitch
apart from how fast `t` advances. Quantising means picking a rate at which a
**power-of-two block of `t` lands exactly on a beat** — that is the grid these programs
are written to, their bars being 2^12 or 2^13 samples long, which is why they sound like
music at 8000 Hz and like noise at 8001. The rate chosen is the power of two closest to
the native 8000: at 120 BPM that is 8192, so **4096 samples a beat**, and the formula's
own bar lines up with the sequencer's.

**K** transposes in semitones, which multiplies that rate — so only whole octaves stay on
the grid. A semitone is 2^(1/12), an irrational ratio, so the formula's bar drifts against
the beat on purpose. Nothing can separate the two without resynthesising the signal, which
would be the end of the sound design, so the app does not pretend otherwise: a key that is
not a whole octave reads `*` on the strip and *off the grid* on the lane.

The one liberty taken is the blend. Between nodes a free lane plays a weighted average of
four programs' bytes, which is a signal none of them is. Standing on a node, the weights
are 1 and 0 and you are hearing that one-liner exactly.

### Reaching the rest of the controls

Only BPM, Chaos and the focused lane's live controls are on the strip at once. **L1 / R1** walk the
whole list, crossing into the next lane when they run out; **Y** jumps straight to the
next lane. A control the current mode has no use for is skipped rather than shown dead:
**L** (track length) only exists in Formula, **P** only in Bit voice, and **K** only on a
free lane. **D** is density in Bitseq and pitch in Bit voice, and reads out in the right
units for each. A free lane has no sequencer and no envelope, so D, L, A, R and P are not
dimmed there — they are gone, and only **F** and **K** remain.

On a desktop the keyboard is a full pad (sndstation's own fallback): arrows for the
D-pad, **Z** and **X** for A and B, **C** and **V** for X and Y, **1**/**4** for L1/R1,
**2**/**3** for L2/R2, **E** and **I** for L3/R3, **Backspace** reset, **Return** quit.
With no pad attached the footer names those keys rather than the pad's buttons — it used
to say "L1/R1 pick" to someone holding a keyboard, which is the one moment the hint
mattered.

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
| `--dark` | the demo's phosphor palette instead of the default paper one |
| `--res WxH` | force the layout size |
| `--shots DIR` | headless: write one BMP per screen state (both engines x both rhythm sources) and exit. No display needed. |
| `--bench SEC` | headless: render SEC seconds of audio through both engines with the puck sweeping the map, and report cost, level and clipping. No display, no device. |

`--bench` is the one to run after touching the DSP:

```bash
./build/gritty-grids --bench 20
```

```
bitseq       49.7 ms for 20.0 s audio =  0.25% of one core   peak 1.00  rms 0.090  at full scale 0.001%
bit-voice    71.3 ms for 20.0 s audio =  0.36% of one core   peak 1.00  rms 0.193  at full scale 0.194%
free x3      70.1 ms for 20.0 s audio =  0.35% of one core   peak 1.00  rms 0.403  at full scale 0.288%
```

"At full scale" is `|x| >= 0.999`. For the two sequenced engines that is the clipper
engaging. For free lanes there is no clipper: a byte of 255 is exactly +1.0, so those
samples are the formula's own range arriving intact.

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
test_free: 4096 samples bit-exact with the formula, 4096 (2^12) per beat, rms 0.384
```

A change to [`bytebeat.h`](bytebeat.h) that alters one byte anywhere in that range fails
the test. The one deliberate departure is documented at the top of that file: past 2^53
JavaScript loses the low bits of a product, which only the Bit voice engine's
free-running `t` reaches, after hours of continuous play. int64 keeps them. This port is
the more correct of the two and nobody will hear the difference.

## How the port differs from the demo

Four changes, forced by the platform, by the demo being wrong, or by a handheld not
being a browser window:

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
4. **Paper, not phosphor, by default.** The demo is a dark CRT because it lives in a
   browser tab at night. A handheld goes outside, where a dark screen with thin bright
   green text is the worst case for a cheap panel in daylight, so the default palette is
   dark ink on paper and the accents keep the demo's hues at a value that carries on it.
   `--dark` is the demo's CSS exactly, for the times the room is dark.

The map, the blend, the thresholds, the envelopes, the probability curve, the ratchet
ladder, the kick's pitch drop and the 1-bit grit are all the demo's own numbers.

## Files

| | |
|---|---|
| [`bytebeat.h`](bytebeat.h) | the 25 nodes, with JavaScript's integer semantics spelled out |
| [`engine.h`](engine.h) | the map, the blend, both engines, as one render callback |
| [`main.cpp`](main.cpp) | the screen, the pad, the controls, `--shots` and `--bench` |
| [`test_bytebeat.cpp`](test_bytebeat.cpp) | the golden values and the dense-sweep hash |
| [`test_free.cpp`](test_free.cpp) | proof that nothing processes a free lane |
| [`gritty-grids.json`](gritty-grids.json) | the launcher's manifest |
