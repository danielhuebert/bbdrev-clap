// BBD Reverb — analoger-BBD-Hall (mki x es.edu / Moritz Klein) als CLAP-Effekt.
//
// Port des minilogue xd "bbd rev" revfx auf das CLAP-Desktop-Format. Gleicher
// DSP-Kern: zwei Delaylines im Verhaeltnis des goldenen Schnitts, kreuzgekoppelt
// ueber eine verlustfreie 2x2-Hadamard-Feedbackmatrix mit EINEM Minuszeichen
// (-a + b), das koinzidente Echos ausloescht und das metallische Klingeln
// verhindert. Angepasst von der festen 48-kHz-/interleaved-Umgebung des logue
// SDK auf CLAPs variable Samplerate und nicht-interleavte (float**) Buffer.
//
//   [ A_in ]   [ -1  +1 ] [ A_out ]
//   [ B_in ] = [ +1  +1 ] [ B_out ] * g        (g = Decay)
//
// Diesmal mit ALLEN im DIY-Kit-Manual genannten Parametern (das echte Modul):
//   Input    0 .. 200 %   Eingangspegel/Drive in den Hall (Modul-"Input Gain")
//   Size     0 .. 100 %   Room Size (beide Delaylaengen, Verhaeltnis = Ratio)
//   Decay    0 .. 100 %   Feedback-Gain / Tail-Laenge (nie 0, Max nahe Selbstosz.)
//   Tone     0 .. 100 %   Tilt-Filter um 700 Hz (LP<->HP), <50 dunkel, >50 hell
//   Mix      0 .. 100 %   Dry/Wet
//   Ratio   1.20 .. 2.00  Delay-Verhaeltnis, Default 1.618 (goldener Schnitt).
//                         Weg von Phi bricht die Illusion bewusst (Manual S. 8/17).
//
// Authentischer BBD-Lo-Fi: room-size-abhaengiges Clock/2-Bandlimit pro Line
// (fc = 2048/(4*t)), sanfte Saettigung + dezenter Grit beim Zurueckschreiben.
// Room-Size-/Ratio-Aenderungen gleiten in der Tonhoehe (BBD-typisch, gewollt).
//
// Alle Parameter sind host-automatisierbar; der Zustand wird via clap.state
// gespeichert/geladen. Build: siehe CMakeLists.txt oder build.sh (-> .clap).

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
// DSP-Konstanten (wie im minilogue-xd-Original)
// ---------------------------------------------------------------------------

static constexpr double kSizeMsMin = 2.5;    // kleiner Raum / metallischer Flutter
static constexpr double kSizeMsMax = 90.0;   // grosser Raum
static constexpr double kRatioMin = 1.20;
static constexpr double kRatioMax = 2.00;
static constexpr double kRatioPhi = 1.6180339887;  // goldener Schnitt (Default)

static constexpr float kGMin = 0.25f;        // Decay-Floor (nie ganz aus)
static constexpr float kGMax = 0.686f;       // g*sqrt(2) ~ 0.97, stabil

// BBD-Bandlimit: fc = N/(4*t) mit N=2048. One-Pole-Koeff a = 1 - exp(-2*pi*fc/fs).
// Der Exponent ist samplerate-unabhaengig in Samples: 2*pi*fc/fs = 2*pi*512/len.
static constexpr float kBbdWNum = 3217.0f;   // 2*pi * 512
static constexpr float kBlAMin = 0.05f;      // untere Schranke (max. Daempfung)

static constexpr float kToneFcHz = 700.0f;   // Tilt-Pivot (Manual S. 26)
static constexpr float kSoftclipC = 0.10f;   // Saettigungsstaerke (0..1/3)
static constexpr float kGritAmt = 0.00035f;  // Rauschflur-Pegel
static constexpr double kSmoothTauS = 0.010; // Parameter-Glaettung (Re-Pitch-Feel)

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
  kParamRatio = 5,  // 1.20..2.00
  kParamCount = 6,
};

static const clap_id kParamOrder[kParamCount] = {
    kParamInput, kParamSize, kParamDecay, kParamTone, kParamMix, kParamRatio};

// ---------------------------------------------------------------------------
// Plugin-Instanz
// ---------------------------------------------------------------------------

struct BbdReverb {
  clap_plugin_t plugin;  // muss zuerst stehen; Host haelt Pointer hierauf
  const clap_host_t *host = nullptr;

  // Parameterwerte in Anzeige-Einheiten. Atomic, damit Main- und Audio-Thread
  // nie einen Wert zerreissen.
  std::atomic<double> p_input{100.0};
  std::atomic<double> p_size{50.0};
  std::atomic<double> p_decay{50.0};
  std::atomic<double> p_tone{50.0};
  std::atomic<double> p_mix{50.0};
  std::atomic<double> p_ratio{kRatioPhi};

  // Verarbeitungszustand (nur Audio-Thread, wenn aktiv).
  double sample_rate = 48000.0;
  std::vector<float> buf_a;
  std::vector<float> buf_b;
  uint32_t max_len = 0;   // allokierte Laenge (>= 90 ms + Guard)
  uint32_t wpos = 0;      // gemeinsamer Schreibkopf (Wrap-Punkt bewegt sich nie)

  // Geglaettete Laufwerte
  float s_len_a = 0.f;    // Delaylaenge A in Samples
  float s_g = 0.5f;       // Feedback-Gain
  float s_mix = 0.5f;     // Dry/Wet
  float s_tone = 0.5f;    // 0..1
  float s_gain = 1.f;     // Input-Gain linear
  float s_ratio = (float)kRatioPhi;

  // Filterzustaende
  float bl_a = 0.f;       // BBD-Bandlimit A
  float bl_b = 0.f;       // BBD-Bandlimit B
  float tone_lp = 0.f;    // Tone-Tiefpass (700 Hz)
  float tone_k = 0.1f;    // Tone-LP-Koeff (aus sr)
  float smooth_k = 0.002f;// Glaettungskoeff (aus sr)

  uint32_t rng = 0x12345678u;  // xorshift fuer den Grit

  inline float white() {
    uint32_t x = rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng = x;
    return (float)((int32_t)x) * 4.6566129e-10f;
  }

  // Ziel-Delaylaenge A (Samples) aus dem Size-Poti.
  float sizeTargetSamples() const {
    const double v = p_size.load(std::memory_order_relaxed) * 0.01;
    const double ms = kSizeMsMin + v * (kSizeMsMax - kSizeMsMin);
    float d = (float)(ms * 0.001 * sample_rate);
    const float hi = (float)max_len - 2.f;
    if (d < 1.f) d = 1.f;
    if (d > hi) d = hi;
    return d;
  }

  // BBD-Bandlimit-Koeffizient fuer eine Delaylaenge (One-Pole-LP).
  static inline float bbdCoeff(float len) {
    if (len < 1.f) len = 1.f;
    const float a = 1.f - std::exp(-kBbdWNum / len);
    return a < kBlAMin ? kBlAMin : (a > 1.f ? 1.f : a);
  }

  void clearState() {
    std::fill(buf_a.begin(), buf_a.end(), 0.f);
    std::fill(buf_b.begin(), buf_b.end(), 0.f);
    wpos = 0;
    bl_a = bl_b = tone_lp = 0.f;
    // Glaettung direkt auf die aktuellen Potiwerte setzen (kein Anfahr-Sweep).
    s_len_a = sizeTargetSamples();
    s_g = kGMin + (float)(p_decay.load() * 0.01) * (kGMax - kGMin);
    s_mix = (float)(p_mix.load() * 0.01);
    s_tone = (float)(p_tone.load() * 0.01);
    s_gain = (float)(p_input.load() * 0.01);
    s_ratio = (float)p_ratio.load();
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
    const float len_a_t = sizeTargetSamples();
    const float g_t = kGMin + (float)(p_decay.load(std::memory_order_relaxed) * 0.01) *
                                  (kGMax - kGMin);
    const float mix_t = (float)(p_mix.load(std::memory_order_relaxed) * 0.01);
    const float tone_t = (float)(p_tone.load(std::memory_order_relaxed) * 0.01);
    const float gain_t = (float)(p_input.load(std::memory_order_relaxed) * 0.01);
    const float ratio_t = (float)p_ratio.load(std::memory_order_relaxed);

    // Bandlimit-Koeffs einmal pro Block aus den aktuellen Laengen.
    const float len_b_blk = s_len_a / s_ratio;
    const float bl_ka = bbdCoeff(s_len_a);
    const float bl_kb = bbdCoeff(len_b_blk);
    const uint32_t maxd = max_len;
    const float sk = smooth_k;

    for (uint32_t i = 0; i < n; ++i) {
      // Parameter glaetten (Size/Ratio -> leichtes Re-Pitching, gewollt)
      s_len_a += sk * (len_a_t - s_len_a);
      s_g     += sk * (g_t     - s_g);
      s_mix   += sk * (mix_t   - s_mix);
      s_tone  += sk * (tone_t  - s_tone);
      s_gain  += sk * (gain_t  - s_gain);
      s_ratio += sk * (ratio_t - s_ratio);

      const float len_b = s_len_a / s_ratio;

      const float dryL = inL[i];
      const float dryR = inR[i];

      // Eingang: Mono-Summe * Input-Gain
      const float mono = 0.5f * (dryL + dryR) * s_gain;

      // Tone-Tilt um 700 Hz: out = gLow*LP + gHigh*HP, bei 0.5 unity (LP+HP=x)
      tone_lp += tone_k * (mono - tone_lp);
      const float hp = mono - tone_lp;
      const float in = 2.f * (1.f - s_tone) * tone_lp + 2.f * s_tone * hp;

      // Delayline-Ausgaenge lesen + BBD-Bandlimit
      const float rd_a = readInterp(buf_a, s_len_a);
      const float rd_b = readInterp(buf_b, len_b);
      bl_a += bl_ka * (rd_a - bl_a);
      bl_b += bl_kb * (rd_b - bl_b);
      const float a = bl_a;
      const float b = bl_b;

      // Hadamard-Feedback mit dem Minuszeichen
      const float fb_a = s_g * (-a + b);
      const float fb_b = s_g * ( a + b);

      const float grit = kGritAmt * white();

      buf_a[wpos] = softclipf(kSoftclipC, in + fb_a + grit);
      buf_b[wpos] = softclipf(kSoftclipC, in + fb_b + grit);

      if (++wpos >= maxd) wpos = 0;

      // Ausgang: A -> links, B -> rechts (natuerliche Stereobreite)
      outL[i] = clip1m1f(dryL + s_mix * (a - dryL));
      outR[i] = clip1m1f(dryR + s_mix * (b - dryR));
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
    "1.0.0",
    "Analoger-BBD-Hall: zwei Delays im goldenen Schnitt + Hadamard-Feedback mit "
    "einem Minuszeichen (mki x es.edu / Moritz Klein).",
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
  }
  return false;
}

static bool pr_text_to_value(const clap_plugin_t *, clap_id, const char *text,
                             double *out) {
  if (!text) return false;
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
    default: break;
  }
}

static void pr_flush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                     const clap_output_events_t *) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  const uint32_t n = in->size(in);
  for (uint32_t i = 0; i < n; ++i) {
    const clap_event_header_t *h = in->get(in, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID &&
        h->type == CLAP_EVENT_PARAM_VALUE)
      applyParamEvent(self, (const clap_event_param_value_t *)h);
  }
}

static const clap_plugin_params_t s_params = {
    pr_count, pr_get_info, pr_get_value,
    pr_value_to_text, pr_text_to_value, pr_flush};

// ---------------------------------------------------------------------------
// state — alle 6 Parameterwerte serialisieren
// ---------------------------------------------------------------------------

static const uint32_t kStateMagic = 0x42425231;  // 'BBR1'

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

// Layout: magic, dann 6 doubles: input, size, decay, tone, mix, ratio.
static bool st_save(const clap_plugin_t *plugin, const clap_ostream_t *os) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  uint32_t magic = kStateMagic;
  double vals[6];
  vals[0] = self->p_input.load();
  vals[1] = self->p_size.load();
  vals[2] = self->p_decay.load();
  vals[3] = self->p_tone.load();
  vals[4] = self->p_mix.load();
  vals[5] = self->p_ratio.load();
  return writeAll(os, &magic, sizeof(magic)) && writeAll(os, vals, sizeof(vals));
}

static bool st_load(const clap_plugin_t *plugin, const clap_istream_t *is) {
  auto *self = (BbdReverb *)plugin->plugin_data;
  uint32_t magic = 0;
  double vals[6] = {0};
  if (!readAll(is, &magic, sizeof(magic)) || magic != kStateMagic) return false;
  if (!readAll(is, vals, sizeof(vals))) return false;
  self->p_input.store(std::clamp(vals[0], 0.0, 200.0));
  self->p_size.store(std::clamp(vals[1], 0.0, 100.0));
  self->p_decay.store(std::clamp(vals[2], 0.0, 100.0));
  self->p_tone.store(std::clamp(vals[3], 0.0, 100.0));
  self->p_mix.store(std::clamp(vals[4], 0.0, 100.0));
  self->p_ratio.store(std::clamp(vals[5], kRatioMin, kRatioMax));
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
  // Filter-/Glaettungskoeffs aus der Samplerate.
  self->tone_k = 1.f - (float)std::exp(-2.0 * M_PI * kToneFcHz / sample_rate);
  self->smooth_k = 1.f - (float)std::exp(-1.0 / (kSmoothTauS * sample_rate));
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
      if (h->space_id == CLAP_CORE_EVENT_SPACE_ID &&
          h->type == CLAP_EVENT_PARAM_VALUE)
        applyParamEvent(self, (const clap_event_param_value_t *)h);
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
