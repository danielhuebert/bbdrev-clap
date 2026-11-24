# BBD Reverb — CLAP audio effect

Analoger-BBD-Hall (mki x es.edu / Moritz Klein) als Desktop-[CLAP](https://cleveraudio.org)-Plugin.
Port des minilogue xd `bbd rev` revfx — gleicher DSP-Kern (zwei Delaylines im
goldenen Schnitt, kreuzgekoppelt über eine verlustfreie 2×2-**Hadamard-Feedback-
matrix mit einem Minuszeichen**), angepasst an CLAPs variable Samplerate und
nicht-interleavte `float**`-Buffer — **diesmal mit allen im Manual genannten
Parametern**.

```
[ A_in ]   [ -1  +1 ] [ A_out ]
[ B_in ] = [ +1  +1 ] [ B_out ] · g      (g = Decay)
```

Das Minuszeichen (`-a + b`) löscht koinzidente Echos aus → kein metallisches
Klingeln, sondern ein dichter Wash.

## Parameter

| Parameter | Range        | Modul-Entsprechung | Notiz |
|-----------|--------------|--------------------|-------|
| Input     | 0 … 200 %    | Input Gain         | Pegel/Drive in den Hall (nur Wet-Pfad) |
| Size      | 0 … 100 %    | Room Size (Hero)   | beide Delaylängen (~2,5 … 90 ms), Verhältnis = Ratio. Klein = heller/metallischer Flutter, groß = dunkler, körnig |
| Decay     | 0 … 100 %    | Decay              | Feedback-Gain / Tail-Länge. Nie 0 (Floor), Max nahe Selbstoszillation |
| Tone      | 0 … 100 %    | Tone               | Tilt-Filter um 700 Hz (LP↔HP). 50 % neutral, <50 dunkel, >50 hell |
| Mix       | 0 … 100 %    | Dry/Wet            | lineare Dry/Wet-Blende |
| Ratio     | 1.20 … 2.00  | goldener Schnitt   | Delay-Verhältnis, Default **1.618 (φ)**. Weg von φ bricht die Hall-Illusion bewusst (Manual S. 8/17) |

Alle Parameter sind host-automatisier- und -modulierbar; der komplette Zustand
wird über die `clap.state`-Extension gespeichert/geladen.

Stereo: Delay A → links, Delay B → rechts (natürliche Breite). Der Hall-Kern
verarbeitet die Mono-Summe des Eingangs; der Dry-Pfad bleibt pro Kanal erhalten.

## Authentischer BBD-Lo-Fi

- **Clock/2-Bandlimit pro Line, room-size-abhängig:** aus der BBD-Physik
  (2048 Stufen) `fc = 2048/(4·t)`. Kurze Line (kleiner Raum) → höhere Clock →
  heller; lange Line → dunkler & körniger. One-Pole-LP, Koeffizient pro Block.
- **Sanfte Sättigung** (`softclip`, kubisch) + **dezenter Grit** (xorshift-
  Rauschflur) beim Zurückschreiben — der „between spring and lo-fi digital"-Charakter.
- **Re-Pitching:** Size-/Ratio-Änderungen werden geglättet → leichtes Tonhöhen-
  Gleiten (BBD-typisch, gewollt). Anders als beim `snes-delay`-CLAP wird hier
  **nicht** per Crossfade entglittet.

**Was digital gratis besser ist als in der Schaltung:** `len_b = len_a / Ratio`
ist exakt — kein Ratio-Trimmer-Drift, keine VCA-Matching-Trimmer, keine
Clock-Kennlinien-Kalibrierung (im Manual „locking the ratio" S. 30–36).

## Bauen

**Nur clang (kein CMake nötig):**

```sh
./build.sh          # Universal-Bundle build/"BBD Reverb.clap"
./build.sh install  # + nach ~/Library/Audio/Plug-Ins/CLAP kopieren
./build.sh test     # + Smoke-Test-Host bauen & laufen lassen
```

**Oder CMake:**

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Die CLAP-Header werden beim ersten Build nach `deps/clap/` geklont (Tag 1.2.10,
nicht im Repo). Ergebnis ist ein `.clap`-Bundle (macOS, Universal arm64+x86_64).

## Test

`test/test_host.cpp` ist ein minimaler CLAP-Host, der das Plugin lädt und prüft:
6 Parameter + Stereo-Ports, Ratio-Default = φ, Impulsantwort endlich mit Tail auf
beiden Kanälen, **Stabilität bei Decay = 100 %** (Ausgang bleibt beschränkt),
und State-Roundtrip. Start über `./build.sh test`.

## Status

v1 — Build + Smoke-Tests grün. In einem echten CLAP-Host (Bitwig, Reaper …)
noch nicht gegengehört.

## Herkunft

- DSP-Prinzip & Parameter: DIY-Kit-Manual des **mki x es.edu analog reverb**
  (Moritz Klein × Erica Synths). Das Manual selbst ist nicht Teil dieses Repos.
- Schwesterprojekte: minilogue xd `bbdrev` (revfx), `clap-snes-delay` (SPC Echo,
  CLAP-Vorlage), `amrm-clap`.
