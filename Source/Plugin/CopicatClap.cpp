// CopicatClap.cpp
// TB Copicat IC300 as a CLAP plugin. clap-wrapper builds the VST3 and
// AU versions from this same code, so all three formats behave identically.
//
// Threads:
//  - Audio thread: process() / flush(). Only these touch the DSP.
//  - Main thread: parameter info, state, the panel and presets.
//  Parameter values live in atomics both sides can read. Changes from the panel
//  (or a loaded state) travel to the audio thread through a lock-free FIFO and
//  are reported to the host as gesture/value events.

#include <clap/clap.h>

#include "Editor.h"
#include "Params.h"
#include "Presets.h"
#include "CopicatClap.h"
#include "../DSP/CopicatEcho.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
  #include <xmmintrin.h>
  #define TBCC_HAVE_SSE 1
#endif

using namespace tbcc;

namespace
{

const char* const kPluginId = "com.tonybuzzcock.tb-copicat-ic300";

const char* const kFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY,
                                  CLAP_PLUGIN_FEATURE_STEREO, nullptr };

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    kPluginId,
    "TB Copicat IC300",
    "TB",
    "https://github.com/tonybuzzcock/TB-Copicat-MAC",
    "",
    "",
    "1.0.0",
    "Model of the WEM Copicat IC300 tape echo, from its circuit drawing",
    kFeatures
};

// Full names for hosts that show more than 8 characters, and a group per section.
const char* const kLongNames[kNumParams] = {
    "Gain", "Swell", "Sustain", "Head 1", "Head 2", "Head 3", "Tone", "Echo",
    "Speed", "Wow & Flutter", "Tape Age", "Output"
};
const char* const kModules[kNumParams] = {
    "Input", "Echo", "Echo", "Heads", "Heads", "Heads", "Heads", "Echo",
    "Tape", "Tape", "Tape", "Plugin"
};

bool isStepped (int i)
{
    const int k = kParamKind[i];
    return k == kSwitch || k == kHeadSw;
}

// The host sees plain values 0..1; switches are 0 or 1.
double toPlain (int i, float internal)
{
    if (isStepped (i)) return isOn (internal) ? 1.0 : 0.0;
    return internal;
}

float fromPlain (int i, double plain)
{
    if (! (plain == plain)) return kDefaults[i];
    double v = plain < 0.0 ? 0.0 : (plain > 1.0 ? 1.0 : plain);
    if (isStepped (i)) v = v >= 0.5 ? 1.0 : 0.0;
    return (float) v;
}

// Display text. The panel's readout takes it without units (it adds its own);
// hosts get the unit too.
void formatValue (int i, float v, char* buf, size_t size, bool withUnit)
{
    switch (kParamKind[i])
    {
        case kKnob:    std::snprintf (buf, size, "%.1f", v * 10.0); break;
        case kSwitch:
            if (i == kEcho) std::snprintf (buf, size, "%s", isOn (v) ? "In" : "Out");
            else            std::snprintf (buf, size, "%s", isOn (v) ? "On" : "Off");
            break;
        case kHeadSw:  std::snprintf (buf, size, "%s", isOn (v) ? "In" : "Out"); break;
        case kSpeedMs: std::snprintf (buf, size, withUnit ? "%.0f ms" : "%.0f", 1000.0 * kHeadSeconds[0] / CopicatEcho::speedFactor (v)); break;
        case kDb24:    std::snprintf (buf, size, withUnit ? "%+.1f dB" : "%+.1f", (v * 2.0 - 1.0) * kOutputRangeDb); break;
        default:       if (size) buf[0] = 0; break;
    }
}

// Single-producer (main thread) / single-consumer (audio thread) queue.
struct ParamMsg { int16_t index; uint8_t kind; float value; };
enum { kMsgValue = 0, kMsgBegin, kMsgEnd, kMsgValueQuiet };   // quiet: state load, host already knows

class MsgQueue
{
public:
    MsgQueue() : head (0), tail (0) {}
    bool push (const ParamMsg& m)
    {
        const unsigned h = head.load (std::memory_order_relaxed);
        const unsigned n = (h + 1) & (kSize - 1);
        if (n == tail.load (std::memory_order_acquire)) return false;   // full
        buf[h] = m;
        head.store (n, std::memory_order_release);
        return true;
    }
    bool pop (ParamMsg& m)
    {
        const unsigned t = tail.load (std::memory_order_relaxed);
        if (t == head.load (std::memory_order_acquire)) return false;
        m = buf[t];
        tail.store ((t + 1) & (kSize - 1), std::memory_order_release);
        return true;
    }
private:
    enum { kSize = 1024 };
    ParamMsg buf[kSize];
    std::atomic<unsigned> head, tail;
};

//==============================================================================
class Plugin : public EditorHost
{
public:
    explicit Plugin (const clap_host_t* h)
        : host (h), hostParams (nullptr), active (false), sampleRate (44100.0),
          channel1Stale (false), editor (nullptr), guiCreated (false)
    {
        std::memset (&plugin, 0, sizeof (plugin));
        plugin.desc = &kDescriptor;
        plugin.plugin_data = this;
        plugin.init = &sInit;
        plugin.destroy = &sDestroy;
        plugin.activate = &sActivate;
        plugin.deactivate = &sDeactivate;
        plugin.start_processing = &sStartProcessing;
        plugin.stop_processing = &sStopProcessing;
        plugin.reset = &sReset;
        plugin.process = &sProcess;
        plugin.get_extension = &sGetExtension;
        plugin.on_main_thread = &sOnMainThread;

        for (int i = 0; i < kNumParams; ++i) values[i].store (kDefaults[i]);
    }

    ~Plugin()
    {
#if defined(__APPLE__)
        if (editor) editorClose (editor);
#endif
    }

    clap_plugin_t plugin;

    //--------------------------------------------------------------------------
    // EditorHost (main thread)
    float getParam (int index) const override
    {
        return (index >= 0 && index < kNumParams) ? values[index].load (std::memory_order_relaxed) : 0.0f;
    }
    void getParamDisplay (int index, char* text) const override
    {
        if (index < 0 || index >= kNumParams) { text[0] = 0; return; }
        formatValue (index, getParam (index), text, 9, false);
    }
    void guiBeginEdit (int index) override { queue (index, kMsgBegin, 0.0f); }
    void guiEndEdit (int index) override   { queue (index, kMsgEnd, 0.0f); requestFlush(); }
    void guiSetParam (int index, float v) override
    {
        if (index < 0 || index >= kNumParams) return;
        v = fromPlain (index, toPlain (index, v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)));
        values[index].store (v);
        queue (index, kMsgValue, v);
        requestFlush();
    }
    void refreshPresets() override { presets.load(); }
    bool presetExists (int slot) const override { return presets.has (slot); }
    bool savePreset (int slot) override
    {
        float v[kNumParams];
        for (int i = 0; i < kNumParams; ++i) v[i] = getParam (i);
        presets.load();
        presets.put (slot, v);
        return presets.save();
    }
    bool loadPreset (int slot) override
    {
        float v[kNumParams];
        if (! presets.get (slot, v)) return false;
        for (int i = 0; i < kNumParams; ++i)
        {
            guiBeginEdit (i);
            guiSetParam (i, v[i]);
            guiEndEdit (i);
        }
        return true;
    }

private:
    static Plugin* self (const clap_plugin_t* p) { return static_cast<Plugin*> (p->plugin_data); }

    void queue (int index, uint8_t kind, float v)
    {
        ParamMsg m; m.index = (int16_t) index; m.kind = kind; m.value = v;
        if (! msgs.push (m) && kind != kMsgValue) return;   // a full queue only drops gestures
    }
    void requestFlush()
    {
        if (hostParams && hostParams->request_flush) hostParams->request_flush (host);
    }

    // Audio thread (or main thread while inactive): push a value into the DSP.
    void applyToDsp (int index)
    {
        const float f = values[index].load (std::memory_order_relaxed);
        const double v = f;
        const bool on = isOn (f);
        for (int c = 0; c < 2; ++c)
        {
            CopicatEcho& a = amp[c];
            switch (index)
            {
                case kGain:    a.setGain (v); break;
                case kSwell:   a.setSwell (v); break;
                case kSustain: a.setSustain (v); break;
                case kHead1:   a.setHead (kButtonHead[0], on); break;
                case kHead2:   a.setHead (kButtonHead[1], on); break;
                case kHead3:   a.setHead (kButtonHead[2], on); break;
                case kTone:    a.setTone (on); break;
                case kEcho:    a.setEchoIn (on); break;
                case kSpeed:   a.setSpeed (v); break;
                case kWow:     a.setWow (v); break;
                case kTapeAge: a.setTapeAge (v); break;
                case kOutput:  a.setOutputGainDb ((v * 2.0 - 1.0) * kOutputRangeDb); break;
                default: break;
            }
        }
    }

    // Apply panel/state messages to the DSP and tell the host about panel edits.
    void drainMessages (const clap_output_events_t* out)
    {
        ParamMsg m;
        while (msgs.pop (m))
        {
            if (m.index < 0 || m.index >= kNumParams) continue;
            if (m.kind == kMsgValue || m.kind == kMsgValueQuiet) applyToDsp (m.index);
            if (! out || m.kind == kMsgValueQuiet) continue;
            if (m.kind == kMsgValue)
            {
                clap_event_param_value_t e;
                std::memset (&e, 0, sizeof (e));
                e.header.size = sizeof (e);
                e.header.time = 0;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.type = CLAP_EVENT_PARAM_VALUE;
                e.param_id = (clap_id) m.index;
                e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
                e.value = toPlain (m.index, m.value);
                out->try_push (out, &e.header);
            }
            else
            {
                clap_event_param_gesture_t e;
                std::memset (&e, 0, sizeof (e));
                e.header.size = sizeof (e);
                e.header.time = 0;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.type = (m.kind == kMsgBegin) ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
                e.param_id = (clap_id) m.index;
                out->try_push (out, &e.header);
            }
        }
    }

    void handleInputEvents (const clap_input_events_t* in)
    {
        if (! in) return;
        const uint32_t n = in->size (in);
        for (uint32_t k = 0; k < n; ++k)
        {
            const clap_event_header_t* h = in->get (in, k);
            if (! h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
            const clap_event_param_value_t* e = (const clap_event_param_value_t*) h;
            if (e->param_id >= (clap_id) kNumParams) continue;
            const int i = (int) e->param_id;
            values[i].store (fromPlain (i, e->value));
            applyToDsp (i);
        }
    }

    //--------------------------------------------------------------------------
    // clap_plugin
    static bool sInit (const clap_plugin_t* p)
    {
        Plugin* s = self (p);
        s->hostParams = (const clap_host_params_t*) s->host->get_extension (s->host, CLAP_EXT_PARAMS);
        for (int i = 0; i < kNumParams; ++i) s->applyToDsp (i);
        for (int c = 0; c < 2; ++c) s->amp[c].prepare (s->sampleRate);
        return true;
    }
    static void sDestroy (const clap_plugin_t* p) { delete self (p); }
    static bool sActivate (const clap_plugin_t* p, double sr, uint32_t, uint32_t)
    {
        Plugin* s = self (p);
        ParamMsg m;
        while (s->msgs.pop (m)) {}                       // values are already in the atomics
        for (int i = 0; i < kNumParams; ++i) s->applyToDsp (i);
        s->sampleRate = sr;
        for (int c = 0; c < 2; ++c) s->amp[c].prepare (sr);
        s->channel1Stale = false;
        s->active = true;
        return true;
    }
    static void sDeactivate (const clap_plugin_t* p) { self (p)->active = false; }
    static bool sStartProcessing (const clap_plugin_t*) { return true; }
    static void sStopProcessing (const clap_plugin_t*) {}
    static void sReset (const clap_plugin_t* p)
    {
        Plugin* s = self (p);
        for (int c = 0; c < 2; ++c) s->amp[c].reset();
    }
    static void sOnMainThread (const clap_plugin_t*) {}

    static clap_process_status sProcess (const clap_plugin_t* p, const clap_process_t* proc)
    {
        Plugin* s = self (p);
        s->drainMessages (proc->out_events);
        s->handleInputEvents (proc->in_events);

        const uint32_t n = proc->frames_count;
        if (n == 0 || proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;
        const clap_audio_buffer_t& ib = proc->audio_inputs[0];
        const clap_audio_buffer_t& ob = proc->audio_outputs[0];
        if (! ib.data32 || ! ob.data32 || ob.channel_count < 1) return CLAP_PROCESS_CONTINUE;

        const float* inL = ib.data32[0];
        const float* inR = ib.channel_count > 1 ? ib.data32[1] : ib.data32[0];
        float* outL = ob.data32[0];
        float* outR = ob.channel_count > 1 ? ob.data32[1] : nullptr;

#if TBCC_HAVE_SSE
        const unsigned int oldCsr = _mm_getcsr();
        _mm_setcsr (oldCsr | 0x8040);   // flush denormals to zero
#endif
        const bool sameInput = (inL == inR) || std::memcmp (inL, inR, (size_t) n * sizeof (float)) == 0;
        if (sameInput || ! outR)
        {
            s->amp[0].process (inL, outL, (int) n);
            s->channel1Stale = true;
            if (outR && outR != outL) std::memcpy (outR, outL, (size_t) n * sizeof (float));
        }
        else
        {
            if (s->channel1Stale) { s->amp[1] = s->amp[0]; s->channel1Stale = false; }
            s->amp[0].process (inL, outL, (int) n);
            s->amp[1].process (inR, outR, (int) n);
        }
#if TBCC_HAVE_SSE
        _mm_setcsr (oldCsr);
#endif
        return CLAP_PROCESS_CONTINUE;
    }

    //--------------------------------------------------------------------------
    // Extensions
    static const void* sGetExtension (const clap_plugin_t*, const char* id)
    {
        if (! std::strcmp (id, CLAP_EXT_AUDIO_PORTS)) return &audioPortsExt;
        if (! std::strcmp (id, CLAP_EXT_PARAMS))      return &paramsExt;
        if (! std::strcmp (id, CLAP_EXT_STATE))       return &stateExt;
        if (! std::strcmp (id, CLAP_EXT_LATENCY))     return &latencyExt;
        if (! std::strcmp (id, CLAP_EXT_TAIL))        return &tailExt;
#if defined(__APPLE__)
        if (! std::strcmp (id, CLAP_EXT_GUI))         return &guiExt;
#endif
        return nullptr;
    }

    // Audio ports: one stereo in, one stereo out
    static uint32_t apCount (const clap_plugin_t*, bool) { return 1; }
    static bool apGet (const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info)
    {
        if (index != 0) return false;
        info->id = isInput ? 0 : 1;
        std::snprintf (info->name, sizeof (info->name), "%s", isInput ? "In" : "Out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 1 : 0;
        return true;
    }
    static const clap_plugin_audio_ports_t audioPortsExt;

    // Parameters
    static uint32_t pCount (const clap_plugin_t*) { return kNumParams; }
    static bool pInfo (const clap_plugin_t*, uint32_t index, clap_param_info_t* info)
    {
        if (index >= (uint32_t) kNumParams) return false;
        const int i = (int) index;
        std::memset (info, 0, sizeof (*info));
        info->id = (clap_id) i;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | (isStepped (i) ? CLAP_PARAM_IS_STEPPED : 0);
        std::snprintf (info->name, sizeof (info->name), "%s", kLongNames[i]);
        std::snprintf (info->module, sizeof (info->module), "%s", kModules[i]);
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = toPlain (i, kDefaults[i]);
        return true;
    }
    static bool pValue (const clap_plugin_t* p, clap_id id, double* out)
    {
        if (id >= (clap_id) kNumParams) return false;
        *out = toPlain ((int) id, self (p)->getParam ((int) id));
        return true;
    }
    static bool pToText (const clap_plugin_t*, clap_id id, double value, char* out, uint32_t size)
    {
        if (id >= (clap_id) kNumParams || size == 0) return false;
        formatValue ((int) id, fromPlain ((int) id, value), out, size, true);
        return true;
    }
    static bool pFromText (const clap_plugin_t*, clap_id id, const char* text, double* out)
    {
        if (id >= (clap_id) kNumParams || ! text) return false;
        const int i = (int) id;
        const int k = kParamKind[i];
        if (k == kSwitch || k == kHeadSw)
        {
            const bool on = ! std::strcmp (text, "On") || ! std::strcmp (text, "on") || ! std::strcmp (text, "In")
                         || ! std::strcmp (text, "in") || ! std::strcmp (text, "1");
            *out = on ? 1.0 : 0.0;
            return true;
        }
        char* end = nullptr;
        const double d = std::strtod (text, &end);
        if (end == text) return false;
        double v;
        if (k == kKnob)         v = d / 10.0;
        else if (k == kSpeedMs)
        {
            if (d <= 0.0) return false;
            const double speed = 1000.0 * kHeadSeconds[0] / d;          // ms of head 1 -> tape speed factor
            v = (std::log (speed) / std::log (1.35) + 1.0) * 0.5;
        }
        else                    v = (d / kOutputRangeDb + 1.0) * 0.5;
        *out = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        return true;
    }
    static void pFlush (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
    {
        Plugin* s = self (p);
        s->drainMessages (out);
        s->handleInputEvents (in);
    }
    static const clap_plugin_params_t paramsExt;

    // State: every parameter as text, so it survives future versions
    static bool sSave (const clap_plugin_t* p, const clap_ostream_t* stream)
    {
        Plugin* s = self (p);
        std::string text = "TBCC-STATE 1\n";
        char line[96];
        for (int i = 0; i < kNumParams; ++i)
        {
            std::snprintf (line, sizeof (line), "%d %.7f\n", i, (double) s->getParam (i));
            text += line;
        }
        const char* data = text.c_str();
        int64_t left = (int64_t) text.size();
        while (left > 0)
        {
            const int64_t w = stream->write (stream, data, (uint64_t) left);
            if (w <= 0) return false;
            data += w; left -= w;
        }
        return true;
    }
    static bool sLoad (const clap_plugin_t* p, const clap_istream_t* stream)
    {
        Plugin* s = self (p);
        std::string text;
        char buf[512];
        for (int guard = 0; guard < 64; ++guard)
        {
            const int64_t r = stream->read (stream, buf, sizeof (buf));
            if (r < 0) return false;
            if (r == 0) break;
            text.append (buf, (size_t) r);
        }
        if (text.compare (0, 12, "TBCC-STATE 1") != 0) return false;
        const char* q = text.c_str() + 12;
        while (*q)
        {
            char* end = nullptr;
            const long idx = std::strtol (q, &end, 10);
            if (end == q) break;
            q = end;
            const double v = std::strtod (q, &end);
            if (end == q) break;
            q = end;
            if (idx < 0 || idx >= kNumParams || ! (v == v)) continue;
            const float f = fromPlain ((int) idx, toPlain ((int) idx, (float) (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v))));
            s->values[idx].store (f);
            if (s->active) s->queue ((int) idx, kMsgValueQuiet, f);
            else s->applyToDsp ((int) idx);
        }
        if (s->hostParams)
        {
            if (s->hostParams->rescan) s->hostParams->rescan (s->host, CLAP_PARAM_RESCAN_VALUES);
            s->requestFlush();
        }
        return true;
    }
    static const clap_plugin_state_t stateExt;

    static uint32_t latGet (const clap_plugin_t*) { return (uint32_t) CopicatEcho::latencySamples(); }
    static const clap_plugin_latency_t latencyExt;

    static uint32_t tailGet (const clap_plugin_t* p) { return (uint32_t) (self (p)->sampleRate * 10.0); }  // Sustain repeats
    static const clap_plugin_tail_t tailExt;

#if defined(__APPLE__)
    // GUI: the Cocoa panel, fixed size
    static bool gSupported (const clap_plugin_t*, const char* api, bool floating)
    {
        return ! floating && api && ! std::strcmp (api, CLAP_WINDOW_API_COCOA);
    }
    static bool gPreferred (const clap_plugin_t*, const char** api, bool* floating)
    {
        *api = CLAP_WINDOW_API_COCOA; *floating = false; return true;
    }
    static bool gCreate (const clap_plugin_t* p, const char* api, bool floating)
    {
        if (! gSupported (p, api, floating)) return false;
        self (p)->guiCreated = true;
        return true;
    }
    static void gDestroy (const clap_plugin_t* p)
    {
        Plugin* s = self (p);
        if (s->editor) { editorClose (s->editor); s->editor = nullptr; }
        s->guiCreated = false;
    }
    static bool gSetScale (const clap_plugin_t*, double) { return false; }   // Cocoa scales by itself
    static bool gGetSize (const clap_plugin_t*, uint32_t* w, uint32_t* h)
    {
        *w = kEditorWidth; *h = kEditorHeight; return true;
    }
    static bool gCanResize (const clap_plugin_t*) { return false; }
    static bool gHints (const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
    static bool gAdjust (const clap_plugin_t*, uint32_t* w, uint32_t* h)
    {
        *w = kEditorWidth; *h = kEditorHeight; return true;
    }
    static bool gSetSize (const clap_plugin_t*, uint32_t w, uint32_t h)
    {
        return w == (uint32_t) kEditorWidth && h == (uint32_t) kEditorHeight;
    }
    static bool gSetParent (const clap_plugin_t* p, const clap_window_t* win)
    {
        Plugin* s = self (p);
        if (! s->guiCreated || ! win || ! win->cocoa) return false;
        if (s->editor) { editorClose (s->editor); s->editor = nullptr; }
        s->editor = editorOpen (win->cocoa, s);
        return s->editor != nullptr;
    }
    static bool gTransient (const clap_plugin_t*, const clap_window_t*) { return false; }
    static void gTitle (const clap_plugin_t*, const char*) {}
    static bool gShow (const clap_plugin_t*) { return true; }
    static bool gHide (const clap_plugin_t*) { return true; }
    static const clap_plugin_gui_t guiExt;
#endif

    //--------------------------------------------------------------------------
    const clap_host_t* host;
    const clap_host_params_t* hostParams;
    std::atomic<float> values[kNumParams];
    MsgQueue msgs;
    bool active;
    double sampleRate;
    CopicatEcho amp[2];          // ~2 MB each (the tape loop): the plugin lives on the heap
    bool channel1Stale;
    void* editor;
    bool guiCreated;
    PresetStore presets;
};

const clap_plugin_audio_ports_t Plugin::audioPortsExt = { &Plugin::apCount, &Plugin::apGet };
const clap_plugin_params_t Plugin::paramsExt = { &Plugin::pCount, &Plugin::pInfo, &Plugin::pValue,
                                                 &Plugin::pToText, &Plugin::pFromText, &Plugin::pFlush };
const clap_plugin_state_t Plugin::stateExt = { &Plugin::sSave, &Plugin::sLoad };
const clap_plugin_latency_t Plugin::latencyExt = { &Plugin::latGet };
const clap_plugin_tail_t Plugin::tailExt = { &Plugin::tailGet };
#if defined(__APPLE__)
const clap_plugin_gui_t Plugin::guiExt = { &Plugin::gSupported, &Plugin::gPreferred, &Plugin::gCreate,
                                           &Plugin::gDestroy, &Plugin::gSetScale, &Plugin::gGetSize,
                                           &Plugin::gCanResize, &Plugin::gHints, &Plugin::gAdjust,
                                           &Plugin::gSetSize, &Plugin::gSetParent, &Plugin::gTransient,
                                           &Plugin::gTitle, &Plugin::gShow, &Plugin::gHide };
#endif

//==============================================================================
// Factory

uint32_t fCount (const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* fDescriptor (const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
}
const clap_plugin_t* fCreate (const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    if (! host || ! id || std::strcmp (id, kPluginId) != 0) return nullptr;
    if (! clap_version_is_compatible (host->clap_version)) return nullptr;
    Plugin* p = new (std::nothrow) Plugin (host);
    return p ? &p->plugin : nullptr;
}
const clap_plugin_factory_t kFactory = { &fCount, &fDescriptor, &fCreate };

} // namespace

bool tbcc_entry_init (const char*) { return true; }
void tbcc_entry_deinit() {}
const void* tbcc_entry_get_factory (const char* factoryId)
{
    if (factoryId && ! std::strcmp (factoryId, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}
