// clap_host_test.cpp
// A minimal CLAP host: loads the built plugin binary, then checks the
// descriptor, parameters, text conversion, processing, automation events,
// state save/restore, latency and tail.
//
//   clap_host_test <path to the .clap binary>
// On macOS pass the binary inside the bundle:
//   "TB Copicat IC300.clap/Contents/MacOS/TB Copicat IC300"

#include <clap/clap.h>

#include "Params.h"
#include "CopicatEcho.h"

#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <string>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (! ok) ++failures;
}

// ---- Host ----
static int flushRequests = 0, rescans = 0;
static void hpRescan (const clap_host_t*, clap_param_rescan_flags) { ++rescans; }
static void hpClear (const clap_host_t*, clap_id, clap_param_clear_flags) {}
static void hpRequestFlush (const clap_host_t*) { ++flushRequests; }
static const clap_host_params_t hostParams = { hpRescan, hpClear, hpRequestFlush };

static const void* hGetExt (const clap_host_t*, const char* id)
{
    return ! std::strcmp (id, CLAP_EXT_PARAMS) ? &hostParams : nullptr;
}
static void hNoop (const clap_host_t*) {}
static const clap_host_t host = { CLAP_VERSION_INIT, nullptr, "clap_host_test", "TB", "", "1.0",
                                  hGetExt, hNoop, hNoop, hNoop };

// ---- Event lists ----
struct InEvents
{
    std::vector<clap_event_param_value_t> ev;
    clap_input_events_t list;
    InEvents() { list.ctx = this; list.size = &sSize; list.get = &sGet; }
    static uint32_t sSize (const clap_input_events_t* l) { return (uint32_t) ((InEvents*) l->ctx)->ev.size(); }
    static const clap_event_header_t* sGet (const clap_input_events_t* l, uint32_t i) { return &((InEvents*) l->ctx)->ev[i].header; }
    void add (clap_id id, double v)
    {
        clap_event_param_value_t e;
        std::memset (&e, 0, sizeof (e));
        e.header.size = sizeof (e); e.header.space_id = CLAP_CORE_EVENT_SPACE_ID; e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = id; e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1; e.value = v;
        ev.push_back (e);
    }
};

struct OutEvents
{
    int values = 0, begins = 0, ends = 0;
    clap_output_events_t list;
    OutEvents() { list.ctx = this; list.try_push = &sPush; }
    static bool sPush (const clap_output_events_t* l, const clap_event_header_t* h)
    {
        OutEvents* o = (OutEvents*) l->ctx;
        if (h->type == CLAP_EVENT_PARAM_VALUE) ++o->values;
        if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) ++o->begins;
        if (h->type == CLAP_EVENT_PARAM_GESTURE_END) ++o->ends;
        return true;
    }
};

// ---- Streams ----
struct OStream { std::string data; clap_ostream_t s; OStream() { s.ctx = this; s.write = &w; }
    static int64_t w (const clap_ostream_t* st, const void* b, uint64_t n) { ((OStream*) st->ctx)->data.append ((const char*) b, (size_t) n); return (int64_t) n; } };
struct IStream { std::string data; size_t pos = 0; clap_istream_t s; IStream() { s.ctx = this; s.read = &r; }
    static int64_t r (const clap_istream_t* st, void* b, uint64_t n)
    {
        IStream* is = (IStream*) st->ctx;
        // Hand the data over a few bytes at a time to prove partial reads work
        size_t k = std::min ((size_t) n, std::min ((size_t) 37, is->data.size() - is->pos));
        std::memcpy (b, is->data.data() + is->pos, k); is->pos += k; return (int64_t) k;
    } };

static bool finite (const std::vector<float>& x, float lim)
{
    for (size_t i = 0; i < x.size(); ++i) if (! (x[i] == x[i]) || std::fabs (x[i]) > lim) return false;
    return true;
}

// Run 'frames' of audio through the plugin in blocks.
static void run (const clap_plugin_t* p, std::vector<float>& l, std::vector<float>& r,
                 std::vector<float>& ol, std::vector<float>& orr, uint32_t block,
                 InEvents* in = nullptr, OutEvents* out = nullptr)
{
    InEvents emptyIn; OutEvents sink;
    for (size_t pos = 0; pos < l.size(); pos += block)
    {
        const uint32_t n = (uint32_t) std::min ((size_t) block, l.size() - pos);
        float* ins[2] = { &l[pos], &r[pos] };
        float* outs[2] = { &ol[pos], &orr[pos] };
        clap_audio_buffer_t ib; std::memset (&ib, 0, sizeof (ib)); ib.data32 = ins; ib.channel_count = 2;
        clap_audio_buffer_t ob; std::memset (&ob, 0, sizeof (ob)); ob.data32 = outs; ob.channel_count = 2;
        clap_process_t pr; std::memset (&pr, 0, sizeof (pr));
        pr.steady_time = -1; pr.frames_count = n;
        pr.audio_inputs = &ib; pr.audio_inputs_count = 1;
        pr.audio_outputs = &ob; pr.audio_outputs_count = 1;
        pr.in_events = (in && pos == 0) ? &in->list : &emptyIn.list;
        pr.out_events = out ? &out->list : &sink.list;
        p->process (p, &pr);
    }
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: %s <plugin binary>\n", argv[0]); return 2; }
    std::printf ("Loading %s\n", argv[1]);
    void* lib = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (! lib) { std::printf ("dlopen failed: %s\n", dlerror()); return 1; }
    const clap_plugin_entry_t* entry = (const clap_plugin_entry_t*) dlsym (lib, "clap_entry");
    check (entry != nullptr, "exports clap_entry");
    if (! entry) return 1;
    check (entry->init (argv[1]), "entry init");
    const clap_plugin_factory_t* f = (const clap_plugin_factory_t*) entry->get_factory (CLAP_PLUGIN_FACTORY_ID);
    check (f && f->get_plugin_count (f) == 1, "one plugin in the factory");
    const clap_plugin_descriptor_t* d = f->get_plugin_descriptor (f, 0);
    std::printf ("  %s by %s, id %s, version %s\n", d->name, d->vendor, d->id, d->version);
    check (! std::strcmp (d->name, "TB Copicat IC300"), "plugin name");

    const clap_plugin_t* p = f->create_plugin (f, &host, d->id);
    check (p && p->init (p), "create and init");
    if (! p) return 1;

    std::printf ("\nParameters\n");
    const clap_plugin_params_t* pp = (const clap_plugin_params_t*) p->get_extension (p, CLAP_EXT_PARAMS);
    check (pp && pp->count (p) == tbcc::kNumParams, "12 parameters");
    const char* order[tbcc::kNumParams] = { "Gain", "Swell", "Sustain", "Head 1", "Head 2", "Head 3", "Tone", "Echo",
        "Speed", "Wow & Flutter", "Tape Age", "Output" };
    bool namesOk = true, defaultsOk = true;
    for (uint32_t i = 0; i < pp->count (p); ++i)
    {
        clap_param_info_t info;
        pp->get_info (p, i, &info);
        double v = -1; pp->get_value (p, info.id, &v);
        char txt[64] = { 0 }; pp->value_to_text (p, info.id, v, txt, sizeof (txt));
        std::printf ("  %-14s %-7s = %-8s (%.2f, %g..%g%s)\n", info.name, info.module, txt, v, info.min_value, info.max_value,
                     (info.flags & CLAP_PARAM_IS_STEPPED) ? ", stepped" : "");
        if (std::strcmp (info.name, order[i]) != 0) namesOk = false;
        if (std::fabs (v - info.default_value) > 1e-6) defaultsOk = false;
    }
    check (namesOk, "parameters in front-panel order");
    check (defaultsOk, "values start at their defaults");
    double tv = 0;
    check (pp->text_to_value (p, tbcc::kGain, "7.5", &tv) && std::fabs (tv - 0.75) < 1e-9, "text '7.5' -> Gain 0.75");
    check (pp->text_to_value (p, tbcc::kHead2, "In", &tv) && tv == 1.0, "text 'In' -> head button pressed");
    char txt[32];
    pp->value_to_text (p, tbcc::kSpeed, 0.5, txt, sizeof (txt));
    check (! std::strcmp (txt, "70 ms"), "Speed reads head 1's echo time, 70 ms");
    check (pp->text_to_value (p, tbcc::kSpeed, "70", &tv) && std::fabs (tv - 0.5) < 1e-6, "text '70' -> normal speed");
    pp->value_to_text (p, tbcc::kOutput, 1.0, txt, sizeof (txt));
    check (! std::strcmp (txt, "+24.0 dB"), "Output reads in dB");

    std::printf ("\nAudio\n");
    const clap_plugin_audio_ports_t* ap = (const clap_plugin_audio_ports_t*) p->get_extension (p, CLAP_EXT_AUDIO_PORTS);
    clap_audio_port_info_t pi;
    check (ap && ap->count (p, true) == 1 && ap->count (p, false) == 1 && ap->get (p, 0, false, &pi) && pi.channel_count == 2,
           "one stereo input and output");
    const clap_plugin_latency_t* lat = (const clap_plugin_latency_t*) p->get_extension (p, CLAP_EXT_LATENCY);
    const clap_plugin_tail_t* tail = (const clap_plugin_tail_t*) p->get_extension (p, CLAP_EXT_TAIL);
    check (p->activate (p, 44100.0, 1, 4096), "activate at 44.1 kHz");
    p->start_processing (p);
    check (lat && lat->get (p) == (uint32_t) tbcc::CopicatEcho::latencySamples(), "reports the oversampler latency");
    check (tail && tail->get (p) > 44100, "reports a tail for the repeats");

    const int sr = 44100;
    std::vector<float> l (sr), r (sr), ol (sr), orr (sr);
    for (int i = 0; i < sr; ++i) l[i] = r[i] = (float) (0.2 * std::sin (2.0 * 3.14159265358979 * 196.0 * i / sr));
    const std::clock_t t0 = std::clock();
    run (p, l, r, ol, orr, 512);
    const double secs = (double) (std::clock() - t0) / CLOCKS_PER_SEC;
    float pk = 0, lr = 0;
    for (int i = 0; i < sr; ++i) { pk = std::max (pk, std::fabs (ol[i])); lr = std::max (lr, std::fabs (ol[i] - orr[i])); }
    std::printf ("  peak %.3f, CPU %.1f%% of one core\n", pk, 100.0 * secs);
    check (pk > 0.01 && finite (ol, 10.0f), "produces output");
    check (lr == 0.0f, "mono source gives identical L/R");
    for (int i = 0; i < sr; ++i) r[i] = (float) (0.2 * std::sin (2.0 * 3.14159265358979 * 247.0 * i / sr));
    run (p, l, r, ol, orr, 333);
    lr = 0; for (int i = sr / 2; i < sr; ++i) lr = std::max (lr, std::fabs (ol[i] - orr[i]));
    check (lr > 0.001f && finite (ol, 10.0f) && finite (orr, 10.0f), "independent stereo channels");
    run (p, l, r, l, r, 64);
    check (finite (l, 10.0f) && finite (r, 10.0f), "in-place buffers");

    std::printf ("\nHead buttons\n");
    {
        const double expectMs[3] = { 70.0, 220.0, 330.0 };
        for (int b = 0; b < 3; ++b)
        {
            InEvents in;
            in.add (tbcc::kSustain, 0.0); in.add (tbcc::kWow, 0.0);
            for (int k = 0; k < 3; ++k) in.add ((clap_id) (tbcc::kHead1 + k), k == b ? 1.0 : 0.0);
            std::vector<float> z (sr, 0.0f), z2 (sr, 0.0f), oa (sr), ob (sr);
            run (p, z, z2, oa, ob, 512, &in);                              // settle, clear old echoes
            std::vector<float> a (sr, 0.0f), bb (sr, 0.0f);
            for (int i = 0; i < 441; ++i) a[i] = bb[i] = (float) (0.2 * std::sin (3.14159265358979 * i / 441.0) * std::sin (2.0 * 3.14159265358979 * 1000.0 * i / sr));
            run (p, a, bb, oa, ob, 512);
            int at = (int) (0.05 * sr); float best = 0;
            for (int i = (int) (0.05 * sr); i < sr; ++i) if (std::fabs (oa[i]) > best) { best = std::fabs (oa[i]); at = i; }
            const double ms = 1000.0 * (at - (int) lat->get (p)) / sr - 5.0;
            char msg[80];
            std::snprintf (msg, sizeof (msg), "button %d plays the %.0f ms head (measured %.1f)", b + 1, expectMs[b], ms);
            check (std::fabs (ms - expectMs[b]) < 4.0, msg);
        }
    }

    std::printf ("\nAutomation\n");
    {
        InEvents in; in.add (tbcc::kGain, 0.9); in.add (tbcc::kHead2, 1.0); in.add (tbcc::kTone, 1.0); in.add (tbcc::kSustain, 0.45);
        std::vector<float> a (4096, 0.1f), b (4096, 0.1f), oa (4096), ob (4096);
        run (p, a, b, oa, ob, 512, &in);
        double v;
        pp->get_value (p, tbcc::kGain, &v);  check (std::fabs (v - 0.9) < 1e-6, "host automation sets Gain");
        pp->get_value (p, tbcc::kTone, &v);  check (v == 1.0, "host automation sets Tone on");
        check (finite (oa, 10.0f), "output stays finite while automated");
    }

    std::printf ("\nState\n");
    {
        const clap_plugin_state_t* st = (const clap_plugin_state_t*) p->get_extension (p, CLAP_EXT_STATE);
        OStream os;
        check (st && st->save (p, &os.s) && os.data.size() > 50, "saves state");
        InEvents in; in.add (tbcc::kGain, 0.1); in.add (tbcc::kTone, 0.0); in.add (tbcc::kSustain, 0.8);
        std::vector<float> a (512, 0.0f), b (512, 0.0f), oa (512), ob (512);
        run (p, a, b, oa, ob, 512, &in);
        IStream is; is.data = os.data;
        rescans = 0;
        check (st->load (p, &is.s), "loads state");
        double g, t, su;
        pp->get_value (p, tbcc::kGain, &g); pp->get_value (p, tbcc::kTone, &t); pp->get_value (p, tbcc::kSustain, &su);
        check (std::fabs (g - 0.9) < 1e-6 && t == 1.0 && std::fabs (su - 0.45) < 1e-6, "state restores every value");
        check (rescans > 0, "host is told to re-read the values");
        IStream junk; junk.data = "not a state";
        check (! st->load (p, &junk.s), "rejects foreign data");
    }

    std::printf ("\nFlush while stopped\n");
    {
        p->stop_processing (p);
        InEvents in; in.add (tbcc::kSwell, 0.25);
        OutEvents out;
        pp->flush (p, &in.list, &out.list);
        double v; pp->get_value (p, tbcc::kSwell, &v);
        check (std::fabs (v - 0.25) < 1e-6, "flush applies host changes without audio running");
    }

    std::printf ("\nSample-rate change to 96 kHz\n");
    p->deactivate (p);
    check (p->activate (p, 96000.0, 1, 4096), "re-activate at 96 kHz");
    p->start_processing (p);
    {
        std::vector<float> a (96000), b (96000), oa (96000), ob (96000);
        for (int i = 0; i < 96000; ++i) a[i] = b[i] = (float) (0.2 * std::sin (2.0 * 3.14159265358979 * 196.0 * i / 96000));
        run (p, a, b, oa, ob, 1024);
        check (finite (oa, 10.0f), "runs at 96 kHz");
    }
    // Silence settles (no repeats)
    {
        InEvents in; in.add (tbcc::kSustain, 0.0);
        std::vector<float> z (96000 * 3, 0.0f), z2 (96000 * 3, 0.0f), oa (96000 * 3), ob (96000 * 3);
        run (p, z, z2, oa, ob, 1024, &in);
        float t = 0; for (size_t i = 96000 * 2; i < oa.size(); ++i) t = std::max (t, std::fabs (oa[i]));
        std::printf ("  residual after 2 s of silence: %.2g\n", t);
        check (t < 1e-3f, "silence settles to silence");
    }

    p->stop_processing (p);
    p->deactivate (p);
    p->destroy (p);
    entry->deinit();
    check (true, "destroy and deinit");
    dlclose (lib);

    std::printf ("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL CLAP HOST CHECKS PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
