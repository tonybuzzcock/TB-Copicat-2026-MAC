// copicat_test.cpp
// Offline checks for the TB Copicat IC300 DSP. Builds on Linux or macOS:
//   make -C Tests run      (writes demo WAVs to Tests/renders)
//
// 1. Dry path: unity at the default Gain, Gain pot monotonic, Echo out = dry only
// 2. Echo times of heads 1, 2, 3 and the Speed trim
// 3. Echo level and tone: Swell, frequency response, Tone switch, Tape Age
// 4. Sustain: repeats decay below the threshold; self-oscillation stays bounded
// 5. Wow & flutter moves the echo; head buttons switch without clicks
// 6. Stability sweep: switch combinations x knob extremes x 10 mV-4 V x 41 Hz-15 kHz, plus noise
// 7. Automation of every control while playing
// 8. CPU per second of audio
// 9. Demo renders (32-bit float WAV)

#include "CopicatEcho.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

using namespace tbcc;

static const double kPiT = 3.14159265358979323846;
static const int kSr = 44100;
static int failures = 0;

static void check (bool ok, const char* what)
{
    std::printf ("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (! ok) ++failures;
}

static bool allFinite (const std::vector<float>& x, double limit)
{
    for (size_t i = 0; i < x.size(); ++i)
        if (! (x[i] == x[i]) || std::fabs (x[i]) > limit) return false;
    return true;
}

static double rmsRange (const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0.0;
    if (to > x.size()) to = x.size();
    for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
    return to > from ? std::sqrt (s / (double) (to - from)) : 0.0;
}

static double peakRange (const std::vector<float>& x, size_t from, size_t to)
{
    double p = 0.0;
    if (to > x.size()) to = x.size();
    for (size_t i = from; i < to; ++i) p = std::max (p, (double) std::fabs (x[i]));
    return p;
}

static std::vector<float> sine (double f, double amp, double sec)
{
    std::vector<float> v ((size_t) (sec * kSr));
    for (size_t i = 0; i < v.size(); ++i) v[i] = (float) (amp * std::sin (2.0 * kPiT * f * i / kSr));
    return v;
}

// A short tone burst at t = 0, then silence
static std::vector<float> burst (double f, double amp, double burstSec, double totalSec)
{
    std::vector<float> v ((size_t) (totalSec * kSr), 0.0f);
    const size_t n = (size_t) (burstSec * kSr);
    for (size_t i = 0; i < n; ++i)
    {
        const double w = std::sin (kPiT * (double) i / n);       // Hann-ish envelope
        v[i] = (float) (amp * w * w * std::sin (2.0 * kPiT * f * i / kSr));
    }
    return v;
}

static std::vector<float> run (CopicatEcho& a, const std::vector<float>& in)
{
    std::vector<float> out (in.size());
    const int block = 256;
    for (size_t pos = 0; pos < in.size(); pos += block)
    {
        const int n = (int) ((in.size() - pos) < (size_t) block ? (in.size() - pos) : block);
        a.process (&in[pos], &out[pos], n);
    }
    return out;
}

static void setAll (CopicatEcho& a, double gain, double swell, double sustain, bool h1, bool h2, bool h3,
                    bool tone, bool echo, double speed, double wow, double age)
{
    a.setGain (gain); a.setSwell (swell); a.setSustain (sustain);
    a.setHead (0, h1); a.setHead (1, h2); a.setHead (2, h3);
    a.setTone (tone); a.setEchoIn (echo);
    a.setSpeed (speed); a.setWow (wow); a.setTapeAge (age);
    a.setOutputGainDb (0.0);
    a.reset();
}

static void cleanSetup (CopicatEcho& a)
{
    setAll (a, 0.6, 0.55, 0.0, true, false, false, false, true, 0.5, 0.0, 0.25);
}

// Time (ms) of the strongest envelope peak in [fromMs, toMs]
static double peakTimeMs (const std::vector<float>& x, double fromMs, double toMs)
{
    const size_t a = (size_t) (fromMs * kSr / 1000.0), b = std::min (x.size(), (size_t) (toMs * kSr / 1000.0));
    double best = 0.0; size_t at = a;
    double env = 0.0;
    for (size_t i = a; i < b; ++i)
    {
        env = std::max ((double) std::fabs (x[i]), env * 0.999);
        if (env > best) { best = env; at = i; }
    }
    return 1000.0 * (double) at / kSr;
}

// Echo-only level at frequency f relative to the same burst's dry level (dB)
static double echoRelDb (CopicatEcho& a, double f, double amp)
{
    a.reset();
    std::vector<float> o = run (a, burst (f, amp, 0.03, 0.4));
    const double lat = 1000.0 * CopicatEcho::latencySamples() / kSr;
    const double t1 = a.headDelayMs (0);
    const double dry = peakRange (o, 0, (size_t) ((35.0 + lat) * kSr / 1000.0));
    const double echo = peakRange (o, (size_t) ((t1 + lat - 5.0) * kSr / 1000.0), (size_t) ((t1 + lat + 40.0) * kSr / 1000.0));
    return 20.0 * std::log10 (echo / dry);
}

static void writeWav (const std::string& path, const std::vector<float>& x, int sr)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) { std::printf ("  could not write %s\n", path.c_str()); return; }
    const unsigned int dataBytes = (unsigned int) (x.size() * 4);
    const unsigned int riff = 36 + dataBytes;
    const unsigned short fmt = 3, ch = 1, bits = 32, align = 4;
    const unsigned int rate = (unsigned int) sr, bps = rate * 4, fmtLen = 16;
    std::fwrite ("RIFF", 1, 4, f); std::fwrite (&riff, 4, 1, f);
    std::fwrite ("WAVEfmt ", 1, 8, f); std::fwrite (&fmtLen, 4, 1, f);
    std::fwrite (&fmt, 2, 1, f); std::fwrite (&ch, 2, 1, f);
    std::fwrite (&rate, 4, 1, f); std::fwrite (&bps, 4, 1, f);
    std::fwrite (&align, 2, 1, f); std::fwrite (&bits, 2, 1, f);
    std::fwrite ("data", 1, 4, f); std::fwrite (&dataBytes, 4, 1, f);
    std::fwrite (&x[0], 4, x.size(), f);
    std::fclose (f);
}

// Karplus-Strong plucked string as a stand-in for a DI guitar track.
static void pluck (std::vector<float>& buf, size_t start, double freq, double amp, double dur)
{
    const int period = (int) (kSr / freq + 0.5);
    std::vector<double> line (period);
    unsigned int seed = (unsigned int) (freq * 1000.0);
    for (int i = 0; i < period; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        line[i] = ((seed >> 8) / 8388608.0 - 1.0);
    }
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 1; i < period; ++i) line[i] = 0.5 * (line[i] + line[i - 1]);
    const size_t n = (size_t) (dur * kSr);
    int idx = 0;
    for (size_t i = 0; i < n && start + i < buf.size(); ++i)
    {
        const int next = (idx + 1) % period;
        const double y = line[idx];
        line[idx] = 0.996 * 0.5 * (line[idx] + line[next]);
        idx = next;
        const double env = (i > n - 2000) ? (double) (n - i) / 2000.0 : 1.0;
        buf[start + i] += (float) (amp * y * env);
    }
}

// A twangy single-note line with gaps for the echoes, then a held chord
static std::vector<float> makeLine (double seconds)
{
    std::vector<float> g ((size_t) (kSr * seconds), 0.0f);
    const double notes[][3] = {
        { 0.00, 329.63, 0.30 }, { 0.40, 392.00, 0.30 }, { 0.80, 440.00, 0.30 }, { 1.20, 493.88, 0.30 },
        { 1.60, 587.33, 0.35 }, { 2.60, 493.88, 0.30 }, { 3.00, 440.00, 0.30 }, { 3.40, 329.63, 0.35 },
        { 4.60, 164.81, 0.30 }, { 4.60, 246.94, 0.25 }, { 4.60, 329.63, 0.20 }, { 4.60, 415.30, 0.18 },
    };
    for (size_t i = 0; i < sizeof (notes) / sizeof (notes[0]); ++i)
        pluck (g, (size_t) (notes[i][0] * kSr), notes[i][1], notes[i][2], notes[i][0] < 4.5 ? 0.35 : 2.5);
    return g;
}

int main (int argc, char** argv)
{
    const std::string outDir = argc > 1 ? argv[1] : "renders";
    CopicatEcho* ap = new CopicatEcho();     // ~2 MB of tape: keep it off the stack
    CopicatEcho& a = *ap;
    a.prepare (kSr);
    const int lat = CopicatEcho::latencySamples();
    const double latMs = 1000.0 * lat / kSr;

    std::printf ("1. Dry path\n");
    {
        setAll (a, 0.6, 0.55, 0.0, false, false, false, false, true, 0.5, 0.0, 0.0);
        std::vector<float> o = run (a, sine (1000.0, 0.01, 0.5));
        const double g = 20.0 * std::log10 (rmsRange (o, kSr / 4, o.size()) / (0.01 / std::sqrt (2.0)));
        std::printf ("  dry gain at default Gain: %+.2f dB\n", g);
        check (std::fabs (g) < 0.5, "dry is unity at the default Gain");
        double prev = -1.0; bool mono = true;
        for (int k = 1; k <= 10; ++k)
        {
            setAll (a, k / 10.0, 0.55, 0.0, false, false, false, false, true, 0.5, 0.0, 0.0);
            o = run (a, sine (1000.0, 0.005, 0.3));
            const double r = rmsRange (o, kSr / 6, o.size());
            if (r <= prev) mono = false;
            prev = r;
        }
        check (mono, "Gain pot raises the level monotonically");
        setAll (a, 0.6, 1.0, 0.6, true, true, true, false, false, 0.5, 0.0, 0.0);
        o = run (a, burst (1000.0, 0.1, 0.05, 1.0));
        const double tail = peakRange (o, (size_t) ((80.0 + latMs) * kSr / 1000.0), o.size());
        check (tail < 1.0e-3, "Echo out leaves only the dry signal");
    }

    std::printf ("\n2. Echo times\n");
    {
        const char* names[3] = { "head 1", "head 2", "head 3" };
        for (int h = 0; h < 3; ++h)
        {
            setAll (a, 0.6, 0.8, 0.0, h == 0, h == 1, h == 2, false, true, 0.5, 0.0, 0.0);
            std::vector<float> o = run (a, burst (1000.0, 0.1, 0.02, 0.6));
            const double t = peakTimeMs (o, 50.0 + latMs, 500.0) - latMs - 10.0;   // burst peak sits at 10 ms
            char msg[96];
            std::snprintf (msg, sizeof (msg), "%s echo at %.1f ms (expect %.0f +/- 3)", names[h], t, kHeadSeconds[h] * 1000.0);
            check (std::fabs (t - kHeadSeconds[h] * 1000.0) < 3.0, msg);
        }
        setAll (a, 0.6, 0.8, 0.0, true, false, false, false, true, 0.0, 0.0, 0.0);
        const double slow = a.headDelayMs (0);
        setAll (a, 0.6, 0.8, 0.0, true, false, false, false, true, 1.0, 0.0, 0.0);
        const double fast = a.headDelayMs (0);
        std::printf ("  Speed 0 -> head 1 at %.1f ms, Speed 10 -> %.1f ms\n", slow, fast);
        check (std::fabs (slow - 1000.0 * kHeadSeconds[0] * 1.35) < 2.0 && std::fabs (fast - 1000.0 * kHeadSeconds[0] / 1.35) < 2.0, "Speed trim slows and speeds the tape");
    }

    std::printf ("\n3. Echo level and tone (echo relative to dry, head 1)\n");
    {
        cleanSetup (a);
        const double fr[] = { 100.0, 200.0, 500.0, 1000.0, 2000.0, 3000.0, 5000.0, 8000.0 };
        double r[8];
        std::printf ("  default tape, Tone off:");
        for (int i = 0; i < 8; ++i) { r[i] = echoRelDb (a, fr[i], 0.02); std::printf (" %.0f:%+.1f", fr[i], r[i]); }
        std::printf ("\n");
        check (std::fabs (r[1] - r[3]) < 3.0 && std::fabs (r[2] - r[3]) < 2.0, "echo keeps its body: flat from 200 Hz to 1 kHz");
        check (r[7] < r[4] - 6.0, "and the top rolls off like tape");
        setAll (a, 0.6, 0.55, 0.0, true, false, false, true, true, 0.5, 0.0, 0.25);
        const double t5 = echoRelDb (a, 5000.0, 0.02);
        std::printf ("  Tone on, 5 kHz: %+.1f dB\n", t5);
        check (t5 < r[6] - 3.0, "Tone switch (4n7) darkens the echo");
        setAll (a, 0.6, 0.55, 0.0, true, false, false, false, true, 0.5, 0.0, 1.0);
        const double a5 = echoRelDb (a, 5000.0, 0.02);
        std::printf ("  old tape, 5 kHz: %+.1f dB\n", a5);
        check (a5 < r[6] - 6.0, "worn tape loses top end");
        setAll (a, 0.6, 0.55, 0.0, true, true, true, true, true, 0.5, 0.0, 0.25);
        const double h3 = echoRelDb (a, 3000.0, 0.02);
        setAll (a, 0.6, 0.55, 0.0, true, false, false, true, true, 0.5, 0.0, 0.25);
        const double h1 = echoRelDb (a, 3000.0, 0.02);
        std::printf ("  Tone on, 3 kHz: one head %+.1f dB, three heads %+.1f dB\n", h1, h3);
        check (h3 < h1 - 2.0, "three heads in series are darker than one with Tone on (more inductance)");
        double prevE = -200.0; bool mono = true;
        for (int k = 2; k <= 10; k += 2)
        {
            setAll (a, 0.6, k / 10.0, 0.0, true, false, false, false, true, 0.5, 0.0, 0.25);
            const double e = echoRelDb (a, 1000.0, 0.02);
            if (e <= prevE) mono = false;
            prevE = e;
        }
        std::printf ("  Swell 10: echo %+.1f dB re dry\n", prevE);
        check (mono && prevE > 6.0, "Swell raises the echo, up past the dry level");
    }

    std::printf ("\n4. Sustain\n");
    {
        double thresh = -1.0;
        for (int k = 0; k <= 100; ++k)
        {
            a.setSustain (k / 100.0); a.reset();
            if (a.sustainLoopGain() >= 1.0) { thresh = k / 10.0; break; }
        }
        std::printf ("  self-oscillation starts near Sustain %.1f\n", thresh);
        check (thresh > 9.0 && thresh < 9.8, "runaway only near the top of the dial (about 9.4)");

        // Repeats of a low guitar note (196 Hz) at Sustain 5, 7 and 8
        const double knobs[3] = { 0.5, 0.7, 0.8 };
        double keep[3];
        for (int k = 0; k < 3; ++k)
        {
            setAll (a, 0.6, 0.6, knobs[k], false, true, false, false, true, 0.5, 0.0, 0.25);   // head 2, 220 ms
            std::vector<float> o = run (a, burst (196.0, 0.1, 0.06, 4.0));
            const double t = kHeadSeconds[1], l = latMs / 1000.0;
            const double p1 = peakRange (o, (size_t) ((t + l - 0.03) * kSr), (size_t) ((t + l + 0.09) * kSr));
            const double p10 = peakRange (o, (size_t) ((10 * t + l - 0.03) * kSr), (size_t) ((10 * t + l + 0.09) * kSr));
            keep[k] = 20.0 * std::log10 (p10 / p1);
            std::printf ("  Sustain %.0f: repeat 10 is %+.1f dB from repeat 1\n", knobs[k] * 10.0, keep[k]);
        }
        check (keep[0] < -25.0 && keep[0] > -60.0, "Sustain 5: repeats fade over several passes");
        check (keep[1] > -32.0 && keep[1] < -18.0, "Sustain 7: about ten repeats, then fading");
        check (keep[2] > -20.0, "Sustain 8: repeats keep going");

        setAll (a, 0.6, 0.6, 0.45, true, false, false, false, true, 0.5, 0.0, 0.0);
        std::vector<float> o = run (a, burst (800.0, 0.1, 0.03, 2.5));
        const double t = kHeadSeconds[0] + latMs / 1000.0, w = 0.4 * kHeadSeconds[0];
        const double r1 = peakRange (o, (size_t) ((1.0 * t - w) * kSr), (size_t) ((1.0 * t + w) * kSr));
        const double r5 = peakRange (o, (size_t) ((5.0 * kHeadSeconds[0] + latMs / 1000.0 - w) * kSr), (size_t) ((5.0 * kHeadSeconds[0] + latMs / 1000.0 + w) * kSr));
        std::printf ("  repeat 1 %.4f, repeat 5 %.4f\n", r1, r5);
        check (r5 < r1 * 0.7 && r5 > r1 * 0.02, "repeats die away at a medium setting");

        setAll (a, 0.6, 0.6, 1.0, true, true, true, false, true, 0.5, 0.5, 0.3);
        o = run (a, burst (800.0, 0.1, 0.03, 8.0));
        const double late = rmsRange (o, (size_t) (6.0 * kSr), o.size());
        std::printf ("  full Sustain, 6-8 s: rms %.3f, peak %.3f, tape peak %.2f x nominal\n",
                     late, peakRange (o, 0, o.size()), a.peakTapeLevel());
        check (late > 0.005, "full Sustain runs away into continuous oscillation");
        check (allFinite (o, 20.0) && a.peakTapeLevel() < 2.3, "and the tape saturation holds it bounded");
    }

    std::printf ("\n5. Wow & flutter, switching\n");
    {
        setAll (a, 0.6, 0.8, 0.0, true, false, false, false, true, 0.5, 0.0, 0.0);
        std::vector<float> o0 = run (a, sine (1000.0, 0.02, 2.0));
        setAll (a, 0.6, 0.8, 0.0, true, false, false, false, true, 0.5, 1.0, 0.0);
        std::vector<float> o1 = run (a, sine (1000.0, 0.02, 2.0));
        double diff = 0.0;
        for (size_t i = kSr; i < o0.size(); ++i) diff += std::fabs (o0[i] - o1[i]);
        diff /= (double) (o0.size() - kSr);
        check (diff > 0.002, "Wow & flutter modulates the echo");

        setAll (a, 0.6, 0.8, 0.3, true, false, false, false, true, 0.5, 0.3, 0.2);
        std::vector<float> in = sine (220.0, 0.05, 3.0), out (in.size());
        double maxStep = 0.0;
        for (size_t pos = 0; pos < in.size(); pos += 256)
        {
            const int blk = (int) std::min ((size_t) 256, in.size() - pos);
            const int s = (int) (pos / 256);
            if (pos > (size_t) kSr) { a.setHead (1, (s % 20) < 10); a.setHead (2, (s % 14) < 7); a.setEchoIn ((s % 30) < 25); a.setTone ((s % 18) < 9); }
            a.process (&in[pos], &out[pos], blk);
        }
        for (size_t i = kSr + 1; i < out.size(); ++i) maxStep = std::max (maxStep, (double) std::fabs (out[i] - out[i - 1]));
        std::printf ("  largest step while switching: %.4f (signal step ~%.4f)\n", maxStep, 2.0 * kPiT * 220.0 / kSr * 0.05 * 3.0);
        check (maxStep < 0.06, "head buttons, Tone and Echo switch without clicks");
    }

    std::printf ("\n6. Stability sweep\n");
    {
        int runs = 0; bool ok = true;
        const double levels[] = { 0.01, 0.1, 1.0, 4.0 };
        const double freqs[] = { 41.0, 440.0, 3000.0, 15000.0 };
        for (int combo = 1; combo < 8; combo += 2)
            for (int knob = 0; knob < 2; ++knob)
                for (int li = 0; li < 4; ++li)
                    for (int fi = 0; fi < 4; ++fi)
                    {
                        const double kv = knob ? 1.0 : 0.0;
                        setAll (a, kv, kv, knob ? 1.0 : 0.3, (combo & 1) != 0, (combo & 2) != 0, (combo & 4) != 0,
                                fi & 1, true, knob ? 0.0 : 1.0, kv, kv);
                        std::vector<float> o = run (a, sine (freqs[fi], levels[li], 0.6));
                        ++runs;
                        if (! allFinite (o, 50.0)) { ok = false; std::printf ("    fails: combo %d knob %d %.2f V %.0f Hz\n", combo, knob, levels[li], freqs[fi]); }
                    }
        std::vector<float> noise ((size_t) (2 * kSr));
        unsigned int s = 1;
        for (size_t i = 0; i < noise.size(); ++i) { s = s * 1664525u + 1013904223u; noise[i] = (float) (((s >> 8) / 8388608.0 - 1.0) * 6.0); }
        setAll (a, 1.0, 1.0, 1.0, true, true, true, false, true, 0.0, 1.0, 1.0);
        ok = ok && allFinite (run (a, noise), 50.0);
        std::printf ("  %d sine runs + loud noise\n", runs);
        check (ok, "no NaN or runaway values anywhere");
    }

    std::printf ("\n7. Automation\n");
    {
        cleanSetup (a);
        std::vector<float> in = makeLine (6.0), out (in.size());
        for (size_t pos = 0; pos < in.size(); pos += 128)
        {
            const int blk = (int) std::min ((size_t) 128, in.size() - pos);
            const double t = (double) pos / kSr;
            a.setGain (0.5 + 0.5 * std::sin (t * 2.1));
            a.setSwell (0.5 + 0.5 * std::sin (t * 1.3));
            a.setSustain (0.4 + 0.4 * std::sin (t * 0.7));
            a.setSpeed (0.5 + 0.5 * std::sin (t * 0.9));
            a.setWow (0.5 + 0.5 * std::sin (t * 3.3));
            a.setTapeAge (0.5 + 0.5 * std::sin (t * 0.5));
            a.setOutputGainDb (6.0 * std::sin (t * 1.7));
            a.process (&in[pos], &out[pos], blk);
        }
        check (allFinite (out, 20.0), "every control moving while playing stays finite");
    }

    std::printf ("\n8. CPU\n");
    {
        setAll (a, 0.6, 0.6, 0.5, true, true, true, false, true, 0.5, 0.3, 0.3);
        std::vector<float> in = makeLine (10.0);
        const std::clock_t c0 = std::clock();
        std::vector<float> o = run (a, in);
        const double sec = (double) (std::clock() - c0) / CLOCKS_PER_SEC;
        std::printf ("  %.1f ms per second of audio (one channel, all heads)\n", sec * 100.0);
        check (sec < 5.0, "runs comfortably in real time");
    }

    std::printf ("\n9. Renders -> %s\n", outDir.c_str());
    {
        struct Demo { const char* file; double gain, swell, sustain; bool h1, h2, h3, tone; double speed, wow, age; };
        const Demo demos[] = {
            { "copicat_head1_slapback.wav",   0.6, 0.55, 0.30, true,  false, false, false, 0.5, 0.3, 0.25 },
            { "copicat_heads123_repeats.wav", 0.6, 0.60, 0.60, true,  true,  true,  false, 0.5, 0.3, 0.25 },
            { "copicat_head3_tone_on.wav",    0.6, 0.60, 0.55, false, false, true,  true,  0.5, 0.3, 0.25 },
            { "copicat_old_tape_wobbly.wav",  0.6, 0.65, 0.55, true,  false, true,  false, 0.4, 0.9, 0.85 },
            { "copicat_head3_sustain7.wav",   0.6, 0.60, 0.70, false, false, true,  false, 0.5, 0.3, 0.25 },
            { "copicat_runaway.wav",          0.7, 0.60, 0.96, true,  true,  false, false, 0.5, 0.4, 0.30 },
        };
        std::vector<float> line = makeLine (9.0);
        for (size_t d = 0; d < sizeof (demos) / sizeof (demos[0]); ++d)
        {
            const Demo& m = demos[d];
            setAll (a, m.gain, m.swell, m.sustain, m.h1, m.h2, m.h3, m.tone, true, m.speed, m.wow, m.age);
            std::vector<float> o = run (a, line);
            const double pk = peakRange (o, 0, o.size());
            if (pk > 0.9) for (size_t i = 0; i < o.size(); ++i) o[i] = (float) (o[i] * 0.9 / pk);
            writeWav (outDir + "/" + m.file, o, kSr);
        }
        writeWav (outDir + "/dry_input.wav", line, kSr);
    }

    delete ap;
    std::printf ("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL DSP CHECKS PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
