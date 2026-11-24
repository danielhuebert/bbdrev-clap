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
| Stages    | 512/1024/2048/4096 | BBD-Chip     | nachgebildete BBD-Stufenzahl. Weniger Stufen = niedrigere Clock bei gleicher Zeit = dunkler/körniger **unabhängig von der Raumgröße** (wie ein kleinerer Chip). Default **2048** (Chip des Kits) |
| Mode      | Stereo / Mono| —                  | **Mono** = nur ein Kanal (nicht summiert), Ausgang = Delay-A-Tap auf beide Seiten, wie das Mono-Eurorack-Modul. **Stereo** = A→L / B→R |

Alle Parameter sind host-automatisier- und -modulierbar; der komplette Zustand
wird über die `clap.state`-Extension gespeichert/geladen.

- **Stereo:** Delay A → links, Delay B → rechts (natürliche Breite), Eingang =
  Mono-Summe, Dry-Pfad pro Kanal erhalten.
- **Mono** (wie die Hardware): Eingang = nur der linke Kanal (nicht summiert),
  Ausgang = Delay-A-Tap (enthält via Kreuzkopplung bereits beide Delays),
  identisch auf beide Kanäle.

## Authentischer BBD-Lo-Fi

Recherchierter BBD-Charakter (nicht nur ein Tiefpass):

- **Clock-Rate-Sample&Hold pro Line** (`f_clk = Stages / Delayzeit`, room- *und*
  `Stages`-abhängig) → Bandbegrenzung **und Aliasing** als *gewollter* Teil des
  Klangs. Große Räume takten langsamer → körniger/dunkler; weniger `Stages`
  macht es körnig über den ganzen Size-Bereich (Grit entkoppelt von der Größe).
- **2-poliges Rekonstruktionsfilter im Feedback** → dunkel, und das Abdunkeln
  **kumuliert über jede Wiederholung** (die BBD-„warmth"). Eckfrequenz an die
  Clock gekoppelt, hart gedeckelt (~900 Hz … 5,2 kHz).
- **Companding-„Atmen":** Rauschflur, dessen Pegel der Signal-Hüllkurve folgt
  (fast attack / slow release) — der Hiss schwillt mit dem Signal an und zieht
  sich in Pausen zurück, das typische BBD-Pumpen.
- **Drive + weiche Sättigung** (kubischer Softclip) für die Nichtlinearität.
- **Re-Pitching:** Size-/Ratio-Änderungen gleiten in der Tonhöhe (BBD-typisch,
  gewollt — anders als beim `snes-delay`-CLAP, der per Crossfade entglittet).

Der trockene Pfad bleibt voll bandbreitig; nur der Hall ist lo-fi.

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
8 Parameter + Stereo-Ports, Ratio-Default = φ, Impulsantwort endlich mit Tail auf
beiden Kanälen, **Stabilität bei Decay = 100 %** (Ausgang bleibt beschränkt),
**Mono-Mode** (beide Ausgänge exakt identisch), und State-Roundtrip.
Start über `./build.sh test`.

## Status

v1.2 — Build + Smoke-Tests grün. BBD-Lo-Fi-Kern überarbeitet (Clock-S&H/Aliasing,
2-poliges Rekonstruktionsfilter, Companding-Atmen, Drive) + Mono/Stereo-Switch.
In einem echten CLAP-Host (Bitwig, Reaper …) noch nicht gegengehört.

## Herkunft

- DSP-Prinzip & Parameter: DIY-Kit-Manual des **mki x es.edu analog reverb**
  (Moritz Klein × Erica Synths). Das Manual selbst ist nicht Teil dieses Repos.
- Schwesterprojekte: minilogue xd `bbdrev` (revfx), `clap-snes-delay` (SPC Echo,
  CLAP-Vorlage), `amrm-clap`.
