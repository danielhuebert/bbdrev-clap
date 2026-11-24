// BBD Reverb — analoger-BBD-Hall (mki x es.edu / Moritz Klein) als CLAP-Effekt.
//
// Port des minilogue xd "bbd rev" revfx auf das CLAP-Desktop-Format. Gleicher
// DSP-Kern: zwei Delaylines im Verhaeltnis des goldenen Schnitts, kreuzgekoppelt
// ueber eine verlustfreie 2x2-Hadamard-Feedbackmatrix mit EINEM Minuszeichen
// (-a + b), das koinzidente Echos ausloescht und das metallische Klingeln
// verhindert.
//
//   [ A_in ]   [ -1  +1 ] [ A_out ]
//   [ B_in ] = [ +1  +1 ] [ B_out ] * g        (g = Decay)
//
// Alle im DIY-Kit-Manual genannten Parameter des echten Moduls:
//   Input    0 .. 200 %   Eingangspegel/Drive in den Hall
//   Size     0 .. 100 %   Room Size (beide Delaylaengen, Verhaeltnis = Ratio)
//   Decay    0 .. 100 %   Feedback-Gain / Tail-Laenge (nie 0, Max nahe Selbstosz.)
//   Tone     0 .. 100 %   Tilt-Filter um 700 Hz (LP<->HP), 50 % neutral
//   Mix      0 .. 100 %   Dry/Wet
//   Ratio   1.20 .. 2.00  Delay-Verhaeltnis, Default 1.618 (goldener Schnitt)
//   Mode     Stereo|Mono  Mono = ein Kanal (nicht summiert), Ausgang = Delay-A-
//                         Tap auf beide Seiten (wie das Mono-Eurorack-Modul)
//
// --- Authentischer BBD-Lo-Fi (recherchierter Charakter) ---
//   * Clock-Rate-Sample&Hold pro Line (f_clk = 2048 Stufen / Delayzeit), room-
//     abhaengig -> Bandbegrenzung + ALIASING (gewollter BBD-Grit).
//   * 2-poliges Rekonstruktions-Tiefpassfilter im Loop -> dunkel, und das
//     Abdunkeln KUMULIERT ueber jede Wiederholung (BBD-"warmth").
//   * Companding-"Atmen": Rauschflur, dessen Pegel der Signal-Huellkurve folgt
//     (Hiss schwillt mit dem Signal an, zieht sich in Pausen zurueck).
//   * Drive + weiche Saettigung (Nichtlinearitaet).
//   Trockener Pfad bleibt voll bandbreitig; nur der Hall ist lo-fi.
//
// Alle Parameter sind host-automatisier-/modulierbar; Zustand via clap.state.
// Build: siehe CMakeLists.txt oder build.sh (-> .clap).

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// DSP-Konstanten
// ---------------------------------------------------------------------------

static constexpr double kSizeMsMin = 2.5;    // kleiner Raum / metallischer Flutter
static constexpr double kSizeMsMax = 90.0;   // grosser Raum
static constexpr double kRatioMin = 1.20;
static constexpr double kRatioMax = 2.00;
static constexpr double kRatioPhi = 1.6180339887;  // goldener Schnitt (Default)

static constexpr float kGMin = 0.25f;        // Decay-Floor (nie ganz aus)
static constexpr float kGMax = 0.686f;       // g*sqrt(2) ~ 0.97, stabil

// BBD-Stufen (f_clk = Stages / Delayzeit). Gerastert auf echte Chip-Werte;
// weniger Stufen = niedrigere Clock bei gleicher Delayzeit = dunkler/koerniger,
// unabhaengig von der Raumgroesse (wie ein kleinerer BBD-Chip).
static const float kStagesTable[4] = {512.f, 1024.f, 2048.f, 4096.f};
static constexpr int kStagesCount = 4;
static constexpr int kStagesDefaultIdx = 2;  // 2048 (Chip des Kits)
static inline float stagesValue(double idx) {
  int i = (int)std::lround(idx);
  if (i < 0) i = 0;
  if (i >= kStagesCount) i = kStagesCount - 1;
  return kStagesTable[i];
}

// Rekonstruktions-/Anti-Alias-Tiefpass: Eckfrequenz = Bruchteil der Clock,
// hart gedeckelt -> auch kleine Raeume bleiben dunkel (BBD-typisch).
static constexpr float kLpClockFrac = 0.30f;
static constexpr float kLpMaxHz = 5200.f;
static constexpr float kLpMinHz = 900.f;

static constexpr float kToneFcHz = 700.f;    // Tilt-Pivot (Manual S. 26)
static constexpr float kDrive = 1.4f;        // Eingangs-Drive in die S(aettigung)
static constexpr float kSoftclipC = 0.12f;   // Saettigungsstaerke (0..1/3)

// Companding-"Atmen": Rauschflur + huellkurvengefuehrter Anteil.
static constexpr float kHissFloor = 0.0006f; // konstanter Grundhiss (~ -64 dB)
static constexpr float kHissMod = 0.006f;    // mit dem Signal atmender Anteil

static constexpr double kSmoothTauS = 0.010; // Parameter-Glaettung (Re-Pitch-Feel)
static constexpr double kEnvAtkS = 0.003;    // Huellkurve Attack (3 ms)
static constexpr double kEnvRelS = 0.120;    // Huellkurve Release (120 ms, Tail)

static inline float clip1m1f(float x) {
  return x < -1.f ? -1.f : (x > 1.f ? 1.f : x);
}
static inline float softclipf(float c, float x) {
  x = clip1m1f(x);
  return x - c * (x * x * x);
}

// ---------------------------------------------------------------------------
// Parameter
// ---------------------------------------------------------------------------

enum {
  kParamInput = 0,  // percent (0..200)
  kParamSize = 1,   // percent (0..100)
  kParamDecay = 2,  // percent (0..100)
  kParamTone = 3,   // percent (0..100), 50 = neutral
  kParamMix = 4,    // percent (0..100)
  kParamRatio = 5,   // 1.20..2.00
  kParamMode = 6,    // stepped: 0 = Stereo, 1 = Mono
  kParamStages = 7,  // stepped: index 0..3 -> 512/1024/2048/4096
  kParamCount = 8,
};

static const clap_id kParamOrder[kParamCount] = {
    kParamInput, kParamSize, kParamDecay, kParamTone,
    kParamMix,   kParamRatio, kParamStages, kParamMode};

// ---------------------------------------------------------------------------
// Plugin-Instanz
// ---------------------------------------------------------------------------

struct BbdReverb {
  clap_plugin_t plugin;  // muss zuerst stehen; Host haelt Pointer hierauf
  const clap_host_t *host = nullptr;

  std::atomic<double> p_input{100.0};
  std::atomic<double> p_size{50.0};
  std::atomic<double> p_decay{50.0};
  std::atomic<double> p_tone{50.0};
  std::atomic<double> p_mix{50.0};
  std::atomic<double> p_ratio{kRatioPhi};
  std::atomic<double> p_mode{0.0};  // 0 = Stereo, 1 = Mono
  std::atomic<double> p_stages{(double)kStagesDefaultIdx};  // index 0..3

  // Modulations-Offsets (CLAP PARAM_MOD) fuer die modulierbaren Parameter.
  // Effektiver Wert = Basiswert (oben) + Offset, erst im Processing angewandt.
  std::atomic<double> m_input{0.0};
  std::atomic<double> m_size{0.0};
  std::atomic<double> m_decay{0.0};
  std::atomic<double> m_tone{0.0};
  std::atomic<double> m_mix{0.0};
  std::atomic<double> m_ratio{0.0};

  double sample_rate = 48000.0;
  std::vector<float> buf_a;
  std::vector<float> buf_b;
  uint32_t max_len = 0;
  uint32_t wpos = 0;

  // Geglaettete Laufwerte
  float s_len_a = 0.f;
  float s_g = 0.5f;
  float s_mix = 0.5f;
  float s_tone = 0.5f;
  float s_gain = 1.f;
  float s_ratio = (float)kRatioPhi;
  float s_stages = 2048.f;  // geglaettete Stufenzahl (-> f_clk, Bandbreite)

  // BBD-Clock-Sample&Hold pro Line
  float phase_a = 0.f, phase_b = 0.f;
  float held_a = 0.f, held_b = 0.f;
  // 2-polige Rekonstruktions-Tiefpaesse pro Line
  float lpa1 = 0.f, lpa2 = 0.f, lpb1 = 0.f, lpb2 = 0.f;
  // Tone-Tilt + Companding-Huellkurve
  float tone_lp = 0.f;
  float env = 0.f;

  // Koeffizienten aus der Samplerate
  float tone_k = 0.1f;
  float smooth_k = 0.002f;
  float env_atk = 0.1f;
  float env_rel = 0.01f;

  uint32_t rng = 0x12345678u;
  inline float white() {
    uint32_t x = rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng = x;
    return (float)((int32_t)x) * 4.6566129e-10f;
  }

  float lenForSize(double sizePct) const {
    const double v = sizePct * 0.01;
    const double ms = kSizeMsMin + v * (kSizeMsMax - kSizeMsMin);
    float d = (float)(ms * 0.001 * sample_rate);
    const float hi = (float)max_len - 2.f;
    if (d < 1.f) d = 1.f;
    if (d > hi) d = hi;
    return d;
  }
  float sizeTargetSamples() const {
    return lenForSize(p_size.load(std::memory_order_relaxed));
  }

  // One-Pole-Koeffizient fuer eine Eckfrequenz (Hz).
  float lpCoeff(float fc) const {
    if (fc < kLpMinHz) fc = kLpMinHz;
    if (fc > kLpMaxHz) fc = kLpMaxHz;
    return 1.f - (float)std::exp(-2.0 * M_PI * fc / sample_rate);
  }

  void clearState() {
    std::fill(buf_a.begin(), buf_a.end(), 0.f);
    std::fill(buf_b.begin(), buf_b.end(), 0.f);
    wpos = 0;
    phase_a = phase_b = 0.f;
    held_a = held_b = 0.f;
    lpa1 = lpa2 = lpb1 = lpb2 = 0.f;
    tone_lp = 0.f;
    env = 0.f;
    s_len_a = sizeTargetSamples();
    s_g = kGMin + (float)(p_decay.load() * 0.01) * (kGMax - kGMin);
    s_mix = (float)(p_mix.load() * 0.01);
    s_tone = (float)(p_tone.load() * 0.01);
    s_gain = (float)(p_input.load() * 0.01);
    s_ratio = (float)p_ratio.load();
    s_stages = stagesValue(p_stages.load());
  }

  inline float readInterp(const std::vector<float> &buf, float d) const {
    float rp = (float)wpos - d;
    if (rp < 0.f) rp += (float)max_len;
    uint32_t i0 = (uint32_t)rp;
    const float frac = rp - (float)i0;
    uint32_t i1 = i0 + 1;
    if (i1 >= max_len) i1 -= max_len;
    return buf[i0] + frac * (buf[i1] - buf[i0]);
  }

  void run(float *outL, float *outR, const float *inL, const float *inR,
           uint32_t n) {
    // Effektive Werte = Basis + Modulations-Offset (CLAP PARAM_MOD), geklemmt.
    const double ip = std::clamp(p_input.load(std::memory_order_relaxed) +
                                 m_input.load(std::memory_order_relaxed), 0.0, 200.0);
    const double sz = std::clamp(p_size.load(std::memory_order_relaxed) +
                                 m_size.load(std::memory_order_relaxed), 0.0, 100.0);
    const double dc = std::clamp(p_decay.load(std::memory_order_relaxed) +
                                 m_decay.load(std::memory_order_relaxed), 0.0, 100.0);
    const double tn = std::clamp(p_tone.load(std::memory_order_relaxed) +
                                 m_tone.load(std::memory_order_relaxed), 0.0, 100.0);
    const double mx = std::clamp(p_mix.load(std::memory_order_relaxed) +
                                 m_mix.load(std::memory_order_relaxed), 0.0, 100.0);
    const double rt = std::clamp(p_ratio.load(std::memory_order_relaxed) +
                                 m_ratio.load(std::memory_order_relaxed), kRatioMin, kRatioMax);

    const float len_a_t = lenForSize(sz);
    const float g_t = kGMin + (float)(dc * 0.01) * (kGMax - kGMin);
    const float mix_t = (float)(mx * 0.01);
    const float tone_t = (float)(tn * 0.01);
    const float gain_t = (float)(ip * 0.01);
    const float ratio_t = (float)rt;
    const float stages_t = stagesValue(p_stages.load(std::memory_order_relaxed));
    const bool mono = p_mode.load(std::memory_order_relaxed) >= 0.5;

    // Filter-Eckfrequenzen einmal pro Block aus den aktuellen Clocks.
    // f_clk[Hz] = Stages * fs / len ; corner = clamp(f_clk * frac).
    const float fs = (float)sample_rate;
    const float len_b0 = s_len_a / s_ratio;
    const float lp_ka = lpCoeff((s_stages * fs / s_len_a) * kLpClockFrac);
    const float lp_kb = lpCoeff((s_stages * fs / len_b0) * kLpClockFrac);
    const uint32_t maxd = max_len;
    const float sk = smooth_k;

    for (uint32_t i = 0; i < n; ++i) {
      s_len_a += sk * (len_a_t - s_len_a);
      s_g     += sk * (g_t     - s_g);
      s_mix   += sk * (mix_t   - s_mix);
      s_tone  += sk * (tone_t  - s_tone);
      s_gain  += sk * (gain_t  - s_gain);
      s_ratio += sk * (ratio_t - s_ratio);
      s_stages += sk * (stages_t - s_stages);

      const float len_b = s_len_a / s_ratio;

      const float dryL = inL[i];
      const float dryR = inR[i];

      // Eingang: Mono = nur ein Kanal (nicht summiert), Stereo = Summe.
      const float src = mono ? dryL : 0.5f * (dryL + dryR);
      const float scaled = src * s_gain;

      // Tone-Tilt um 700 Hz (bei 50 % unity, da LP+HP = x)
      tone_lp += tone_k * (scaled - tone_lp);
      const float hp = scaled - tone_lp;
      const float inj = 2.f * (1.f - s_tone) * tone_lp + 2.f * s_tone * hp;

      // Companding-Huellkurve (fast attack / slow release) fuer das Atmen
      const float ax = std::fabs(inj);
      env += (ax > env ? env_atk : env_rel) * (ax - env);

      // BBD-Clock-Sample&Hold der Delay-Ausgaenge (Aliasing + Bandbegrenzung).
      // Phaseninkrement = f_clk/fs = 2048/len. Bei len > 2048 (grosse Raeume)
      // < 1 Tick/Sample -> ZOH-Stufen; bei kleinen Raeumen jede Probe neu.
      const float inc_a = s_stages / s_len_a;
      const float inc_b = s_stages / len_b;
      phase_a += inc_a;
      if (phase_a >= 1.f) { phase_a -= std::floor(phase_a); held_a = readInterp(buf_a, s_len_a); }
      phase_b += inc_b;
      if (phase_b >= 1.f) { phase_b -= std::floor(phase_b); held_b = readInterp(buf_b, len_b); }

      // 2-poliges Rekonstruktionsfilter -> dunkel, kumuliert ueber Repeats.
      lpa1 += lp_ka * (held_a - lpa1); lpa2 += lp_ka * (lpa1 - lpa2);
      lpb1 += lp_kb * (held_b - lpb1); lpb2 += lp_kb * (lpb1 - lpb2);
      const float a = lpa2;
      const float b = lpb2;

      // Hadamard-Feedback mit dem Minuszeichen
      const float fb_a = s_g * (-a + b);
      const float fb_b = s_g * ( a + b);

      // atmendes Companding-Rauschen
      const float noise = (kHissFloor + kHissMod * env) * white();

      // Drive nur auf die Einspeisung (Feedback bleibt stabil), weiche Saettigung
      buf_a[wpos] = softclipf(kSoftclipC, kDrive * inj + fb_a + noise);
      buf_b[wpos] = softclipf(kSoftclipC, kDrive * inj + fb_b + noise);

      if (++wpos >= maxd) wpos = 0;

      if (mono) {
        // wie die Hardware: ein Ausgang = Delay-A-Tap (enthaelt via Kreuz-
        // kopplung bereits beide Delays), identisch auf beide Kanaele.
        const float o = clip1m1f(dryL + s_mix * (a - dryL));
        outL[i] = o;
        outR[i] = o;
      } else {
        outL[i] = clip1m1f(dryL + s_mix * (a - dryL));
        outR[i] = clip1m1f(dryR + s_mix * (b - dryR));
      }
    }
  }
};

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const char *const s_features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB,
    CLAP_PLUGIN_FEATURE_STEREO, nullptr};

static const clap_plugin_descriptor_t s_descriptor = {
    CLAP_VERSION_INIT,
    "com.danielhuebert.bbd-reverb",
    "BBD Reverb",
    "Daniel Huebert",
    "https://github.com/danielhuebert",
    "",
    "",
    "1.2.1",
    "Analoger-BBD-Hall: zwei Delays im goldenen Schnitt + Hadamard-Feedback mit "
    "einem Minuszeichen, mit BBD-Lo-Fi (mki x es.edu / Moritz Klein).",
    s_features,
};

// ---------------------------------------------------------------------------
// audio-ports: ein Stereo-In, ein Stereo-Out
// ---------------------------------------------------------------------------

static uint32_t ap_count(const clap_plugin_t *, bool) { return 1; }

static bool ap_get(const clap_plugin_t *, uint32_t index, bool is_input,
                   clap_audio_port_info_t *info) {
  if (index != 0) return false;
  info->id = is_input ? 0 : 1;
  std::snprintf(info->name, sizeof(info->name), "%s",
                is_input ? "Input" : "Output");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = is_input ? 1 : 0;
  return true;
}

static const clap_plugin_audio_ports_t s_audio_ports = {ap_count, ap_get};

// ---------------------------------------------------------------------------
// params
// ---------------------------------------------------------------------------

static uint32_t pr_count(const clap_plugin_t *) { return kParamCount; }

static bool pr_get_info(const clap_plugin_t *, uint32_t index,
                        clap_param_info_t *info) {
  if (index >= kParamCount) return false;
  const clap_id id = kParamOrder[index];
  std::memset(info, 0, sizeof(*info));
  info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
  switch (id) {
    case kParamInput:
      info->id = kParamInput;
      std::snprintf(info->name, sizeof(info->name), "Input");
      info->min_value = 0.0; info->max_value = 200.0; info->default_value = 100.0;
      return true;
    case kParamSize:
      info->id = kParamSize;
      std::snprintf(info->name, sizeof(info->name), "Size");
      info->min_value = 0.0; info->max_value = 100.0; info->default_value = 50.0;
      return true;
    case kParamDecay:
      info->id = kParamDecay;
      std::snprintf(info->name, sizeof(info->name), "Decay");
      info->min_value = 0.0; info->max_value = 100.0; info->default_value = 50.0;
      return true;
    case kParamTone:
      info->id = kParamTone;
      std::snprintf(info->name, sizeof(info->name), "Tone");
      info->min_value = 0.0; info->max_value = 100.0; info->default_value = 50.0;
      return true;
    case kParamMix:
      info->id = kParamMix;
      std::snprintf(info->name, sizeof(info->name), "Mix");
      info->min_value = 0.0; info->max_value = 100.0; info->default_value = 50.0;
      return true;
    case kParamRatio:
      info->id = kParamRatio;
      std::snprintf(info->name, sizeof(info->name), "Ratio");
      info->min_value = kRatioMin; info->max_value = kRatioMax;
      info->default_value = kRatioPhi;
      return true;
    case kParamMode:
      info->id = kParamMode;
      info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
      std::snprintf(info->name, sizeof(info->name), "Mode");
      info->min_value = 0.0; info->max_value = 1.0; info->default_value = 0.0;
      return true;
    case kParamStages:
      info->id = kParamStages;
      info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
      std::snprintf(info->name, sizeof(info->name), "Stages");
      info->min_value = 0.0; info->max_value = (double)(kStagesCount - 1);
      info->default_value = (double)kStagesDefaultIdx;
      return true;
    default:
      return false;
  }
}

static bool pr_get_value(const clap_plugin_t *plugin, clap_id id, double *out) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  switch (id) {
    case kParamInput: *out = self->p_input.load(); return true;
    case kParamSize:  *out = self->p_size.load();  return true;
    case kParamDecay: *out = self->p_decay.load(); return true;
    case kParamTone:  *out = self->p_tone.load();  return true;
    case kParamMix:   *out = self->p_mix.load();   return true;
    case kParamRatio: *out = self->p_ratio.load(); return true;
    case kParamMode:  *out = self->p_mode.load();  return true;
    case kParamStages:*out = self->p_stages.load();return true;
  }
  return false;
}

static bool pr_value_to_text(const clap_plugin_t *, clap_id id, double value,
                             char *out, uint32_t cap) {
  switch (id) {
    case kParamInput:
    case kParamSize:
    case kParamDecay:
    case kParamTone:
    case kParamMix: std::snprintf(out, cap, "%.0f %%", value); return true;
    case kParamRatio:
      if (std::fabs(value - kRatioPhi) < 0.004)
        std::snprintf(out, cap, "%.3f (phi)", value);
      else
        std::snprintf(out, cap, "%.3f", value);
      return true;
    case kParamMode:
      std::snprintf(out, cap, "%s", value >= 0.5 ? "Mono" : "Stereo");
      return true;
    case kParamStages:
      std::snprintf(out, cap, "%.0f", stagesValue(value));
      return true;
  }
  return false;
}

static bool pr_text_to_value(const clap_plugin_t *, clap_id id, const char *text,
                             double *out) {
  if (!text) return false;
  if (id == kParamMode) {
    *out = (text[0] == 'M' || text[0] == 'm' || std::atof(text) >= 0.5) ? 1.0 : 0.0;
    return true;
  }
  if (id == kParamStages) {
    // accept a stage count (e.g. "1024") or an index; snap to nearest chip.
    const double v = std::atof(text);
    int best = kStagesDefaultIdx;
    if (v >= 0.0 && v <= (double)(kStagesCount - 1) && v == std::floor(v)) {
      best = (int)v;  // already an index
    } else {
      double bd = 1e18;
      for (int i = 0; i < kStagesCount; ++i) {
        const double d = std::fabs(v - kStagesTable[i]);
        if (d < bd) { bd = d; best = i; }
      }
    }
    *out = (double)best;
    return true;
  }
  *out = std::atof(text);
  return true;
}

static void applyParamEvent(BbdReverb *self, const clap_event_param_value_t *ev) {
  switch (ev->param_id) {
    case kParamInput: self->p_input.store(std::clamp(ev->value, 0.0, 200.0)); break;
    case kParamSize:  self->p_size.store(std::clamp(ev->value, 0.0, 100.0)); break;
    case kParamDecay: self->p_decay.store(std::clamp(ev->value, 0.0, 100.0)); break;
    case kParamTone:  self->p_tone.store(std::clamp(ev->value, 0.0, 100.0)); break;
    case kParamMix:   self->p_mix.store(std::clamp(ev->value, 0.0, 100.0)); break;
    case kParamRatio: self->p_ratio.store(std::clamp(ev->value, kRatioMin, kRatioMax)); break;
    case kParamMode:  self->p_mode.store(ev->value >= 0.5 ? 1.0 : 0.0); break;
    case kParamStages:
      self->p_stages.store(std::clamp((double)std::lround(ev->value), 0.0,
                                      (double)(kStagesCount - 1)));
      break;
    default: break;
  }
}

// CLAP PARAM_MOD: ein additiver Offset auf den Basiswert (globale Modulation).
static void applyParamMod(BbdReverb *self, const clap_event_param_mod_t *ev) {
  switch (ev->param_id) {
    case kParamInput: self->m_input.store(ev->amount); break;
    case kParamSize:  self->m_size.store(ev->amount); break;
    case kParamDecay: self->m_decay.store(ev->amount); break;
    case kParamTone:  self->m_tone.store(ev->amount); break;
    case kParamMix:   self->m_mix.store(ev->amount); break;
    case kParamRatio: self->m_ratio.store(ev->amount); break;
    default: break;  // Mode/Stages sind nicht modulierbar
  }
}

// Ein eingehendes Core-Event anwenden (Value oder Mod).
static void handleEvent(BbdReverb *self, const clap_event_header_t *h) {
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (h->type == CLAP_EVENT_PARAM_VALUE)
    applyParamEvent(self, (const clap_event_param_value_t *)h);
  else if (h->type == CLAP_EVENT_PARAM_MOD)
    applyParamMod(self, (const clap_event_param_mod_t *)h);
}

static void pr_flush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                     const clap_output_events_t *) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  const uint32_t n = in->size(in);
  for (uint32_t i = 0; i < n; ++i)
    handleEvent(self, in->get(in, i));
}

static const clap_plugin_params_t s_params = {
    pr_count, pr_get_info, pr_get_value,
    pr_value_to_text, pr_text_to_value, pr_flush};

// ---------------------------------------------------------------------------
// state — alle 7 Parameterwerte serialisieren
// ---------------------------------------------------------------------------

static const uint32_t kStateMagic = 0x42425233;  // 'BBR3'

static bool writeAll(const clap_ostream_t *os, const void *p, uint64_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint64_t done = 0;
  while (done < n) {
    int64_t w = os->write(os, b + done, n - done);
    if (w <= 0) return false;
    done += (uint64_t)w;
  }
  return true;
}
static bool readAll(const clap_istream_t *is, void *p, uint64_t n) {
  uint8_t *b = (uint8_t *)p;
  uint64_t done = 0;
  while (done < n) {
    int64_t r = is->read(is, b + done, n - done);
    if (r <= 0) return false;
    done += (uint64_t)r;
  }
  return true;
}

// Layout: magic, dann 8 doubles: input, size, decay, tone, mix, ratio, mode, stages.
static bool st_save(const clap_plugin_t *plugin, const clap_ostream_t *os) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  uint32_t magic = kStateMagic;
  double vals[8];
  vals[0] = self->p_input.load();
  vals[1] = self->p_size.load();
  vals[2] = self->p_decay.load();
  vals[3] = self->p_tone.load();
  vals[4] = self->p_mix.load();
  vals[5] = self->p_ratio.load();
  vals[6] = self->p_mode.load();
  vals[7] = self->p_stages.load();
  return writeAll(os, &magic, sizeof(magic)) && writeAll(os, vals, sizeof(vals));
}

static bool st_load(const clap_plugin_t *plugin, const clap_istream_t *is) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  uint32_t magic = 0;
  double vals[8] = {0};
  if (!readAll(is, &magic, sizeof(magic)) || magic != kStateMagic) return false;
  if (!readAll(is, vals, sizeof(vals))) return false;
  self->p_input.store(std::clamp(vals[0], 0.0, 200.0));
  self->p_size.store(std::clamp(vals[1], 0.0, 100.0));
  self->p_decay.store(std::clamp(vals[2], 0.0, 100.0));
  self->p_tone.store(std::clamp(vals[3], 0.0, 100.0));
  self->p_mix.store(std::clamp(vals[4], 0.0, 100.0));
  self->p_ratio.store(std::clamp(vals[5], kRatioMin, kRatioMax));
  self->p_mode.store(vals[6] >= 0.5 ? 1.0 : 0.0);
  self->p_stages.store(std::clamp((double)std::lround(vals[7]), 0.0,
                                  (double)(kStagesCount - 1)));
  return true;
}

static const clap_plugin_state_t s_state = {st_save, st_load};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static bool plug_init(const clap_plugin_t *) { return true; }
static void plug_destroy(const clap_plugin_t *plugin) {
  delete (BbdReverb *)plugin->plugin_data;
}

static bool plug_activate(const clap_plugin_t *plugin, double sample_rate,
                          uint32_t, uint32_t) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  self->sample_rate = sample_rate;
  self->max_len = (uint32_t)std::ceil(kSizeMsMax * 0.001 * sample_rate) + 8;
  self->buf_a.assign(self->max_len, 0.f);
  self->buf_b.assign(self->max_len, 0.f);
  self->tone_k = 1.f - (float)std::exp(-2.0 * M_PI * kToneFcHz / sample_rate);
  self->smooth_k = 1.f - (float)std::exp(-1.0 / (kSmoothTauS * sample_rate));
  self->env_atk = 1.f - (float)std::exp(-1.0 / (kEnvAtkS * sample_rate));
  self->env_rel = 1.f - (float)std::exp(-1.0 / (kEnvRelS * sample_rate));
  self->clearState();
  return true;
}

static void plug_deactivate(const clap_plugin_t *) {}
static bool plug_start_processing(const clap_plugin_t *) { return true; }
static void plug_stop_processing(const clap_plugin_t *) {}
static void plug_reset(const clap_plugin_t *plugin) {
  ((BbdReverb *)plugin->plugin_data)->clearState();
}

static clap_process_status plug_process(const clap_plugin_t *plugin,
                                        const clap_process_t *process) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  const uint32_t nframes = process->frames_count;

  if (process->audio_inputs_count < 1 || process->audio_outputs_count < 1)
    return CLAP_PROCESS_CONTINUE;

  const clap_audio_buffer_t &in = process->audio_inputs[0];
  clap_audio_buffer_t &out = process->audio_outputs[0];
  if (!in.data32 || !out.data32 || in.channel_count < 1 ||
      out.channel_count < 1)
    return CLAP_PROCESS_CONTINUE;

  const float *inL = in.data32[0];
  const float *inR = in.channel_count > 1 ? in.data32[1] : in.data32[0];
  float *outL = out.data32[0];
  float *outR = out.channel_count > 1 ? out.data32[1] : out.data32[0];

  const clap_input_events_t *evs = process->in_events;
  const uint32_t nev = evs ? evs->size(evs) : 0;
  uint32_t ev_i = 0;
  uint32_t cur = 0;

  while (cur < nframes) {
    while (ev_i < nev) {
      const clap_event_header_t *h = evs->get(evs, ev_i);
      if (h->time > cur) break;
      handleEvent(self, h);
      ++ev_i;
    }
    uint32_t next = nframes;
    if (ev_i < nev) {
      const clap_event_header_t *h = evs->get(evs, ev_i);
      if (h->time < nframes) next = h->time;
    }
    self->run(outL + cur, outR + cur, inL + cur, inR + cur, next - cur);
    cur = next;
  }
  return CLAP_PROCESS_CONTINUE;
}

static const void *plug_get_extension(const clap_plugin_t *, const char *id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &s_state;
  return nullptr;
}
static void plug_on_main_thread(const clap_plugin_t *) {}

// ---------------------------------------------------------------------------
// Factory + Entry
// ---------------------------------------------------------------------------

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *) { return 1; }
static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(
    const clap_plugin_factory_t *, uint32_t index) {
  return index == 0 ? &s_descriptor : nullptr;
}
static const clap_plugin_t *factory_create_plugin(
    const clap_plugin_factory_t *, const clap_host_t *host,
    const char *plugin_id) {
  if (!plugin_id || std::strcmp(plugin_id, s_descriptor.id) != 0) return nullptr;
  auto *self = new BbdReverb();
  self->host = host;
  self->plugin.desc = &s_descriptor;
  self->plugin.plugin_data = self;
  self->plugin.init = plug_init;
  self->plugin.destroy = plug_destroy;
  self->plugin.activate = plug_activate;
  self->plugin.deactivate = plug_deactivate;
  self->plugin.start_processing = plug_start_processing;
  self->plugin.stop_processing = plug_stop_processing;
  self->plugin.reset = plug_reset;
  self->plugin.process = plug_process;
  self->plugin.get_extension = plug_get_extension;
  self->plugin.on_main_thread = plug_on_main_thread;
  return &self->plugin;
}

static const clap_plugin_factory_t s_factory = {
    factory_get_plugin_count, factory_get_plugin_descriptor,
    factory_create_plugin};

static bool entry_init(const char *) { return true; }
static void entry_deinit(void) {}
static const void *entry_get_factory(const char *factory_id) {
  if (!std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &s_factory;
  return nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
