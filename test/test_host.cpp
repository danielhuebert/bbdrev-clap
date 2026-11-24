// Minimal CLAP host harness to validate the BBD Reverb plugin end-to-end.
#include <clap/clap.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static const void *host_get_ext(const clap_host_t *, const char *) { return nullptr; }
static void host_req_restart(const clap_host_t *) {}
static void host_req_process(const clap_host_t *) {}
static void host_req_cb(const clap_host_t *) {}

static clap_host_t g_host = {
    CLAP_VERSION_INIT, nullptr, "test-host", "claude", "", "1.0",
    host_get_ext, host_req_restart, host_req_process, host_req_cb};

struct EvList {
  clap_input_events_t in;
  std::vector<clap_event_param_value_t> evs;
};
static uint32_t ev_size(const clap_input_events_t *l) {
  return (uint32_t)((EvList *)l->ctx)->evs.size();
}
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) {
  return &((EvList *)l->ctx)->evs[i].header;
}
static bool out_try_push(const clap_output_events_t *, const clap_event_header_t *) { return true; }

static clap_event_param_value_t mkparam(clap_id id, double v, uint32_t time) {
  clap_event_param_value_t e;
  std::memset(&e, 0, sizeof(e));
  e.header.size = sizeof(e);
  e.header.time = time;
  e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  e.header.type = CLAP_EVENT_PARAM_VALUE;
  e.param_id = id;
  e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
  e.value = v;
  return e;
}

struct MemBuf { std::vector<uint8_t> data; size_t rpos = 0; };
static int64_t mem_write(const clap_ostream_t *s, const void *buf, uint64_t n) {
  auto *m = (MemBuf *)s->ctx;
  const uint8_t *b = (const uint8_t *)buf;
  m->data.insert(m->data.end(), b, b + n);
  return (int64_t)n;
}
static int64_t mem_read(const clap_istream_t *s, void *buf, uint64_t n) {
  auto *m = (MemBuf *)s->ctx;
  uint64_t avail = m->data.size() - m->rpos;
  uint64_t k = n < avail ? n : avail;
  std::memcpy(buf, m->data.data() + m->rpos, k);
  m->rpos += k;
  return (int64_t)k;
}

#define CHECK(c) do { if (!(c)) { std::printf("FAIL: %s\n", #c); return 1; } } while (0)

// Param ids: Input=0 Size=1 Decay=2 Tone=3 Mix=4 Ratio=5 Mode=6
enum { P_INPUT=0, P_SIZE=1, P_DECAY=2, P_TONE=3, P_MIX=4, P_RATIO=5, P_MODE=6 };

int main(int argc, char **argv) {
  if (argc < 2) { std::printf("usage: %s <binary-in-bundle>\n", argv[0]); return 2; }

  void *dso = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(dso);
  auto *entry = (const clap_plugin_entry_t *)dlsym(dso, "clap_entry");
  CHECK(entry);
  CHECK(entry->init(argv[1]));

  auto *fac = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  CHECK(fac);
  CHECK(fac->get_plugin_count(fac) == 1);
  const auto *desc = fac->get_plugin_descriptor(fac, 0);
  CHECK(desc);
  std::printf("plugin: %s (%s) v%s\n", desc->name, desc->id, desc->version);

  const clap_plugin_t *plug = fac->create_plugin(fac, &g_host, desc->id);
  CHECK(plug && plug->init(plug));

  auto *params = (const clap_plugin_params_t *)plug->get_extension(plug, CLAP_EXT_PARAMS);
  auto *ports = (const clap_plugin_audio_ports_t *)plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS);
  auto *state = (const clap_plugin_state_t *)plug->get_extension(plug, CLAP_EXT_STATE);
  CHECK(params && ports && state);
  CHECK(params->count(plug) == 7);
  CHECK(ports->count(plug, true) == 1 && ports->count(plug, false) == 1);

  clap_audio_port_info_t pi;
  CHECK(ports->get(plug, 0, true, &pi) && pi.channel_count == 2);

  // Ratio default must read as the golden ratio, value_to_text marks "phi".
  double ratioDef = 0; params->get_value(plug, P_RATIO, &ratioDef);
  CHECK(std::fabs(ratioDef - 1.6180339887) < 1e-6);
  char rtxt[64] = {0};
  params->value_to_text(plug, P_RATIO, ratioDef, rtxt, sizeof(rtxt));
  std::printf("ratio default text = \"%s\"\n", rtxt);
  CHECK(std::strstr(rtxt, "phi") != nullptr);

  clap_output_events_t oe = {nullptr, out_try_push};
  const double sr = 48000.0;

  // Size=50 %, Decay=40 %, Mix=100 %, Tone=50 % before activate.
  {
    EvList pre; pre.in.ctx = &pre; pre.in.size = ev_size; pre.in.get = ev_get;
    pre.evs.push_back(mkparam(P_SIZE, 50.0, 0));
    pre.evs.push_back(mkparam(P_DECAY, 40.0, 0));
    pre.evs.push_back(mkparam(P_MIX, 100.0, 0));
    pre.evs.push_back(mkparam(P_TONE, 50.0, 0));
    params->flush(plug, &pre.in, &oe);
  }

  CHECK(plug->activate(plug, sr, 1, 8192));
  CHECK(plug->start_processing(plug));

  // ---- impulse: expect a finite wet tail; near-silence at frame 0 ----
  const uint32_t N = 6000;
  std::vector<float> inL(N, 0), inR(N, 0), outL(N, 0), outR(N, 0);
  inL[0] = inR[0] = 1.0f;

  float *inCh[2] = {inL.data(), inR.data()};
  float *outCh[2] = {outL.data(), outR.data()};
  clap_audio_buffer_t ab_in = {inCh, nullptr, 2, 0, 0};
  clap_audio_buffer_t ab_out = {outCh, nullptr, 2, 0, 0};
  EvList none; none.in.ctx = &none; none.in.size = ev_size; none.in.get = ev_get;

  clap_process_t proc;
  std::memset(&proc, 0, sizeof(proc));
  proc.frames_count = N;
  proc.audio_inputs = &ab_in; proc.audio_outputs = &ab_out;
  proc.audio_inputs_count = 1; proc.audio_outputs_count = 1;
  proc.in_events = &none.in; proc.out_events = &oe;
  CHECK(plug->process(plug, &proc) == CLAP_PROCESS_CONTINUE);

  // Mix=100 % -> output is the wet signal; frame 0 has no echo yet.
  CHECK(std::fabs(outL[0]) < 1e-4f);

  // Everything finite, and a real reverb tail exists later in the block.
  float tailL = 0.f, tailR = 0.f;
  bool finite = true;
  for (uint32_t i = 0; i < N; ++i) {
    if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) finite = false;
    if (i > 1000) { tailL = std::fmax(tailL, std::fabs(outL[i]));
                    tailR = std::fmax(tailR, std::fabs(outR[i])); }
  }
  std::printf("impulse: finite=%d  tail max |L|=%.4f |R|=%.4f\n",
              (int)finite, tailL, tailR);
  CHECK(finite);
  CHECK(tailL > 1e-3f && tailR > 1e-3f);  // both delay lines feed the output

  // ---- stability: 1 s of loud noise at Decay=100 %, output must stay bounded ----
  {
    plug->reset(plug);
    EvList ev; ev.in.ctx = &ev; ev.in.size = ev_size; ev.in.get = ev_get;
    ev.evs.push_back(mkparam(P_DECAY, 100.0, 0));
    ev.evs.push_back(mkparam(P_MIX, 100.0, 0));
    ev.evs.push_back(mkparam(P_INPUT, 100.0, 0));
    params->flush(plug, &ev.in, &oe);

    const uint32_t M = (uint32_t)sr;
    std::vector<float> nL(M), nR(M), oL(M, 0), oR(M, 0);
    uint32_t r = 0x2468ace0u;
    auto wn = [&]() { r ^= r<<13; r ^= r>>17; r ^= r<<5; return (float)((int32_t)r)*4.6566129e-10f; };
    for (uint32_t i = 0; i < M; ++i) { nL[i] = 0.5f*wn(); nR[i] = 0.5f*wn(); }
    float *ic[2] = {nL.data(), nR.data()};
    float *oc[2] = {oL.data(), oR.data()};
    clap_audio_buffer_t bi = {ic, nullptr, 2, 0, 0};
    clap_audio_buffer_t bo = {oc, nullptr, 2, 0, 0};
    EvList nn; nn.in.ctx = &nn; nn.in.size = ev_size; nn.in.get = ev_get;
    clap_process_t p2 = proc;
    p2.frames_count = M; p2.audio_inputs = &bi; p2.audio_outputs = &bo;
    p2.in_events = &nn.in;
    CHECK(plug->process(plug, &p2) == CLAP_PROCESS_CONTINUE);
    float peak = 0.f; bool fin = true;
    for (uint32_t i = 0; i < M; ++i) {
      if (!std::isfinite(oL[i]) || !std::isfinite(oR[i])) fin = false;
      peak = std::fmax(peak, std::fmax(std::fabs(oL[i]), std::fabs(oR[i])));
    }
    std::printf("stability (Decay=100%%): finite=%d peak=%.4f\n", (int)fin, peak);
    CHECK(fin);
    CHECK(peak <= 1.0001f);  // output is clip1m1f-limited, never blows up
  }

  // ---- state save / load roundtrip ----
  {
    EvList set; set.in.ctx = &set; set.in.size = ev_size; set.in.get = ev_get;
    set.evs.push_back(mkparam(P_SIZE, 73.0, 0));
    set.evs.push_back(mkparam(P_RATIO, 1.50, 0));
    params->flush(plug, &set.in, &oe);
    double sBefore=0, rBefore=0;
    params->get_value(plug, P_SIZE, &sBefore);
    params->get_value(plug, P_RATIO, &rBefore);

    MemBuf mb; clap_ostream_t os = {&mb, mem_write};
    CHECK(state->save(plug, &os));
    EvList clob; clob.in.ctx = &clob; clob.in.size = ev_size; clob.in.get = ev_get;
    clob.evs.push_back(mkparam(P_SIZE, 10.0, 0));
    clob.evs.push_back(mkparam(P_RATIO, 2.0, 0));
    params->flush(plug, &clob.in, &oe);
    clap_istream_t is = {&mb, mem_read};
    CHECK(state->load(plug, &is));
    double sAfter=0, rAfter=0;
    params->get_value(plug, P_SIZE, &sAfter);
    params->get_value(plug, P_RATIO, &rAfter);
    std::printf("state roundtrip: size %.0f->%.0f  ratio %.3f->%.3f\n",
                sBefore, sAfter, rBefore, rAfter);
    CHECK(std::fabs(sAfter - sBefore) < 1e-6 && std::fabs(rAfter - rBefore) < 1e-6);
  }

  // ---- mono mode: L and R outputs identical, driven by L input only ----
  {
    plug->reset(plug);
    EvList me; me.in.ctx = &me; me.in.size = ev_size; me.in.get = ev_get;
    me.evs.push_back(mkparam(P_MODE, 1.0, 0));   // Mono
    me.evs.push_back(mkparam(P_MIX, 100.0, 0));
    me.evs.push_back(mkparam(P_SIZE, 50.0, 0));
    me.evs.push_back(mkparam(P_DECAY, 40.0, 0));
    params->flush(plug, &me.in, &oe);
    char mtxt[32] = {0};
    params->value_to_text(plug, P_MODE, 1.0, mtxt, sizeof(mtxt));

    const uint32_t N2 = 4000;
    std::vector<float> iL(N2, 0), iR(N2, 0), oL(N2, 0), oR(N2, 0);
    iL[0] = 1.0f;  // impulse on LEFT only; R input is silent
    float *ic[2] = {iL.data(), iR.data()};
    float *oc[2] = {oL.data(), oR.data()};
    clap_audio_buffer_t bi = {ic, nullptr, 2, 0, 0};
    clap_audio_buffer_t bo = {oc, nullptr, 2, 0, 0};
    EvList nn; nn.in.ctx = &nn; nn.in.size = ev_size; nn.in.get = ev_get;
    clap_process_t pm = proc;
    pm.frames_count = N2; pm.audio_inputs = &bi; pm.audio_outputs = &bo;
    pm.in_events = &nn.in;
    CHECK(plug->process(plug, &pm) == CLAP_PROCESS_CONTINUE);

    float diff = 0.f, mag = 0.f;
    for (uint32_t i = 0; i < N2; ++i) {
      diff = std::fmax(diff, std::fabs(oL[i] - oR[i]));
      mag = std::fmax(mag, std::fabs(oL[i]));
    }
    std::printf("mono mode (\"%s\"): max|L-R|=%.6g  max|L|=%.4f\n", mtxt, diff, mag);
    CHECK(mag > 1e-3f);    // reverb from the single (L) input is present
    CHECK(diff < 1e-6f);   // both outputs identical (true mono)
  }

  plug->stop_processing(plug);
  plug->deactivate(plug);
  plug->destroy(plug);
  entry->deinit();
  dlclose(dso);
  std::printf("ALL CHECKS PASSED\n");
  return 0;
}
