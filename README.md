# BBD Reverb

An analog-style **bucket-brigade reverb** as a desktop [CLAP](https://cleveraudio.org)
audio effect (macOS, universal arm64 + x86_64).

It reimplements the DSP idea behind the **mki x es.edu *analog reverb*** DIY kit by
**[Moritz Klein](https://www.youtube.com/@MoritzKlein0)** and
**[Erica Synths](https://www.ericasynths.lv/)**: a reverb built from just **two delay
lines** tuned to the **golden ratio**, cross-coupled through a lossless 2×2
**Hadamard feedback matrix with a single minus sign**. That one minus sign cancels
coinciding echoes, which is what turns two flutter echoes into a dense, smooth wash
instead of a metallic ring.

```
[ A_in ]   [ -1  +1 ] [ A_out ]
[ B_in ] = [ +1  +1 ] [ B_out ] · g      (g = Decay)
```

This is a port of my own minilogue xd `bbd rev` user effect to CLAP — same core,
adapted to CLAP's variable sample rate and non-interleaved `float**` buffers, and
extended with every control the original module exposes.

> Independent, unofficial project. Not affiliated with or endorsed by Erica Synths
> or Moritz Klein. See [Credits](#credits).

## Parameters

| Parameter | Range                | Module control | Notes |
|-----------|----------------------|----------------|-------|
| Input     | 0 … 200 %            | Input Gain     | drive into the reverb (affects the wet path only) |
| Size      | 0 … 100 %            | Room Size      | both delay lengths (~2.5 … 90 ms), ratio set by *Ratio*. Small = brighter / metallic flutter, large = darker, grainier |
| Decay     | 0 … 100 %            | Decay          | feedback gain / tail length. Never fully 0, max is near self-oscillation |
| Tone      | 0 … 100 %            | Tone           | tilt filter around 700 Hz (LP↔HP). 50 % is neutral, below = darker, above = brighter |
| Mix       | 0 … 100 %            | Dry/Wet        | linear dry/wet blend |
| Ratio     | 1.20 … 2.00          | golden ratio   | delay ratio, default **1.618 (φ)**. Moving away from φ deliberately breaks the reverb illusion into discrete/ringing echoes |
| Stages    | 512 / 1024 / 2048 / 4096 | BBD chip   | emulated BBD stage count. Fewer stages = lower clock at the same delay time = darker / grainier **regardless of room size** (like a smaller BBD chip). Default **2048** |
| Mode      | Stereo / Mono        | —              | **Mono** = single input channel (not summed), output = the delay-A tap on both sides, like the mono Eurorack module. **Stereo** = A→L / B→R |

All parameters are host-automatable; the six continuous ones (Input, Size, Decay,
Tone, Mix, Ratio) are also host-**modulatable**. The full state is saved/restored
through the `clap.state` extension.

- **Stereo:** delay A → left, delay B → right (natural width); the reverb core
  processes the mono sum of the input, the dry path stays per channel.
- **Mono** (like the hardware): input is the left channel only (not summed); the
  output is the delay-A tap — which already contains both delays via the
  cross-coupling — fed identically to both channels.

## The BBD lo-fi character

A bucket-brigade delay does not just low-pass the signal. Its recognizable sound
comes from four things, all modelled here:

- **Clock-rate sample & hold per line** (`f_clk = Stages / delay-time`, so it
  depends on both *Size* and *Stages*) → band-limiting **and aliasing** as a
  *wanted* part of the sound. Larger rooms (and fewer stages) clock slower →
  grainier and darker.
- **A 2-pole reconstruction low-pass inside the feedback loop** → dark, and the
  darkening **compounds on every repeat** (the classic BBD "warmth"). Its corner
  tracks the clock, hard-capped (~0.9 … 5.2 kHz).
- **Companding "breathing":** a noise floor whose level follows the signal
  envelope (fast attack / slow release) — the hiss swells with the signal and
  ducks back in the gaps, the typical BBD pumping.
- **Drive + soft (cubic) saturation** for the nonlinearity.

The dry path stays full-bandwidth; only the reverb is lo-fi.

**What comes for free versus the analog circuit:** `len_b = len_a / Ratio` is
*exact*, so there is no clock-ratio drift to trim, no VCA matching, and no
oscillator-curve calibration — the parts the kit documentation spends the most
effort on ("locking the ratio").

## Build

**Just clang (no CMake needed):**

```sh
./build.sh          # universal bundle  build/"BBD Reverb.clap"
./build.sh install  # + copy to ~/Library/Audio/Plug-Ins/CLAP
./build.sh test     # + build & run the smoke-test host
```

**Or CMake:**

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The CLAP headers are cloned into `deps/clap/` on first build (tag 1.2.10) and are
not committed. The result is a `.clap` bundle (macOS, universal arm64 + x86_64).

## Test

`test/test_host.cpp` is a minimal CLAP host that loads the plugin and checks:
8 parameters + stereo ports, the `Ratio` default reading as φ and `Stages` as the
chip values, a finite impulse response with a tail on both channels, **stability at
Decay = 100 %** (the output stays bounded), **Mono mode** (both outputs identical),
**parameter modulation** via `CLAP_EVENT_PARAM_MOD`, and a state save/load
round-trip. Run it with `./build.sh test`.

## Credits

The DSP concept, the signal flow, and the parameter set all come from the
**mki x es.edu *analog reverb*** DIY kit, designed and documented by
**Moritz Klein** and **Erica Synths**. Their kit documentation — the full
schematic plus the design rationale behind every stage — was the reference used to
derive this port. Huge thanks to them for making analog circuit design this
approachable; if you want to understand *why* the two-delays-and-a-minus-sign trick
works, their material is the place to start.

- Moritz Klein — https://www.youtube.com/@MoritzKlein0
- Erica Synths (mki x es.edu DIY kits) — https://www.ericasynths.lv/shop/diy-kits-1/

The kit documentation itself is **not** included in this repository. Please get the
kit from Erica Synths to support their educational work.

Sister projects:
- minilogue xd `bbd rev` — the original logue-SDK `revfx` this was ported from.
- [`clap-snes-delay`](https://github.com/danielhuebert) — the SPC Echo CLAP that
  provided the plugin scaffolding.

## License

MIT — see [LICENSE](LICENSE). This covers the code in this repository only; the
mki x es.edu kit and its documentation remain the property of their authors.

## Status

Working and verified in Bitwig (parameters, automation and modulation). Smoke tests
green. If you try it in other hosts, feedback is welcome.
