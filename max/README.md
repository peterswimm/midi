# Gritty Grids for Max/MSP (`gen~` + MaxMCP)

A native-audio port of the [Gritty Grids](../demos/gritty-grids.html) bytebeat drum
machine, built so you can drive it from **Claude Code** through
[MaxMCP](https://github.com/signalcompose/MaxMCP) — controlling the map, pitch, and
timbre with natural language while it runs as real DSP in Max.

> **Status: draft v1, untested in Max.** It was authored without a Max instance to
> run against. The bytebeat *math* is the same as the (working) web demo; the thing
> to verify is how GenExpr handles integer/bitwise ops (see *Known unknowns*). Treat
> this as a starting patch to test and iterate — report what it sounds like and I'll
> tune it.

## Why this only works locally

MaxMCP is an MCP server that runs **inside Max on your Mac** (`maxmcp.mxo`, Max 9+).
Claude Code has to be on the *same machine* as Max to reach it — so drive this from a
**local Claude Code session on your Mac**, not a remote/web session (a cloud container
has no route to your local Max).

## Prerequisites

- macOS 13+, **Max 9+**
- **MaxMCP** package installed (drop it in `~/Documents/Max 9/Packages/`)
- **Claude Code running locally** on the same Mac

## Build the patch

1. New patcher. Add a **`gen~`** object, then double-click it to open the gen editor.
2. Delete the default contents; add a **`codebox`**. Paste all of
   [`gritty-grids.genexpr`](./gritty-grids.genexpr) into it.
3. The codebox has **one outlet** (`out1`). Back in the parent patch, wire
   `gen~` → **`ezdac~`** (both channels).
4. **Params** in `gen~` are set by sending `name value` messages to the `gen~` inlet.
   Add UI and prepend the param name:
   - `[flonum]` → `[prepend x]`  → `gen~`  (map X, 0–1)
   - `[flonum]` → `[prepend y]`  → `gen~`  (map Y, 0–1)
   - `[flonum]` → `[prepend rate]` → `gen~`  (pitch, 0.1–128)
   - `[toggle]` → `[prepend bits]` → `gen~`  (1 = 1-bit square, 0 = 8-bit wave)
   - `[flonum]` → `[prepend level]` → `gen~`
5. Hit the speaker on `ezdac~`, move X/Y — the timbre should morph between the four
   programs. (If it's silent or noise, see *Known unknowns*.)

### Optional: rhythm layer

Keep the *sound* in `gen~` and do *rhythm* in the patch, mirroring the web version:
`[metro]` → `[counter]` → threshold/`[expr]` → gate an `[adsr~]`/`[line~]` on the
`gen~` output, or just retrigger `level`. Per-track polyrhythm = three `metro`/counter
chains at different lengths.

## Wire it to MaxMCP

Follow MaxMCP's own examples — `01-claude-code-connection.maxpat` (connect) and
`03-group-assignment.maxpat` / `07-mcp-tools-test.maxpat` (register + control):

1. Add the **`maxmcp`** object to the patch and make the Claude Code connection as in
   example 01.
2. **Register** each control you want Claude to reach, giving it a clear alias — e.g.
   `map_x`, `map_y`, `pitch`, `bits`, `level` (and the rhythm controls if you add
   them). Aliases are how Claude addresses them, so name them for humans.
3. Route a registered control's output into the matching `[prepend …] → gen~` chain.

Exact registration syntax lives in those example patches (I'm describing the shape,
not inventing message names I can't verify against your install).

## Drive it from local Claude Code

With Max open, the patch loaded, and MaxMCP connected, a **local** Claude Code session
sees the MaxMCP tools. Then, in natural language:

- "set `map_x` to 0.9 and `map_y` to 0.15"
- "slowly sweep `map_y` from 0 to 1"
- "flip `bits` to 8-bit and drop `pitch` to 8"
- "randomize `map_x`/`map_y` every bar" (with a rhythm layer)

## Known unknowns (help me tune these)

- **GenExpr integer/bitwise width.** Classic bytebeat assumes 32-bit integer overflow.
  gen~ runs 64-bit doubles and casts for bit ops; if the width differs, the timbre
  drifts from the web demo. The `t % 16777216` wrap keeps values in exact-integer
  range; we may need to tune that or add explicit masking.
- **1-bit vs 8-bit.** `bits=1` (square) is the "trigger-out voice" sound; `bits=0` is
  the wavetable-ish tone the Bitseq grains use. Start with `bits=1`.
- **Scope of v1.** This is the 4-corner (2×2) interpolating oscillator — the core
  thesis. The full 25-node map, per-track polyrhythm, A/R/P, ratchets, grains, and
  Chaos aren't ported yet; they're the next iterations once the oscillator sounds
  right.

## Files

- `gritty-grids.genexpr` — the gen~ codebox source (the engine).
- `README.md` — this guide.
