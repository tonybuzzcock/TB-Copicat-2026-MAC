// CopicatEcho.cpp
// See CopicatEcho.h for the signal path. Component values are from drawing 577.

#include "CopicatEcho.h"

#include <cmath>
#include <cstring>

namespace tbcc
{

namespace
{
    // Op-amps (741 on +/-12.5 V zener rails)
    const double kRail      = 11.0;      // output swing
    const double kSlewRate  = 0.5e6;     // V/s

    // Input stage: jack -47K- 250K log pot -wiper- 47K -> summing node, 120K||100p feedback
    const double kRin = 47.0e3, kPotIn = 250.0e3, kRsum = 47.0e3, kRf1 = 120.0e3, kCf1 = 100.0e-12;
    // Record amp: 15K in, 560K feedback; Sustain wiper via 10K + 15u
    const double kR15 = 15.0e3, kRf2 = 560.0e3;
    // Swell / Sustain: playback amp -1.5K- 10K log pot -wiper- 10K
    const double kRplayOut = 1.5e3, kPot10 = 10.0e3, kRwiper = 10.0e3;
    // Output mixer: dry 27K, echo 10K, 47K||100p feedback
    const double kRdry = 27.0e3, kRf4 = 47.0e3, kCf4 = 100.0e-12;
    // Playback preamp: 3K3 to ground, feedback 120K || 470p || (33K + .022u)
    const double kRp1 = 120.0e3, kCp1 = 470.0e-12, kRp2 = 33.0e3, kCp2 = 0.022e-6;

    // Levels marked on the drawing: 1.7 V at the record amp, 90 mV from the playback amp
    const double kNominalRec  = 1.7;
    const double kNominalPlay = 0.09;

    // Tape (estimates; not on the drawing)
    const double kTapeSat    = 2.2;      // saturation, in units of nominal record level
    const double kTapeSpeed  = 0.095;    // m/s at normal speed (3.75 ips)
    const double kLoopLength = 0.60;     // m
    const double kGap        = 6.0e-6;   // playback head gap, m
    const double kCoating    = 10.0e-6;  // oxide thickness, m (thickness loss)
    const double kHeadL      = 0.30;     // H per playback head
    const double kStrayC     = 300.0e-12;
    const double kToneC      = 4.7e-9;   // the Tone switch's 4n7

    const double kDefaultGainPot = 0.6;  // dry is unity here (matches Params.h)

    inline double parallel (double a, double b) { return (a <= 0.0 || b <= 0.0) ? 0.0 : a * b / (a + b); }
}

double CopicatEcho::audioTaper (double x)
{
    const double k = 4.394449154672439;   // 10% of the track at mid-rotation (log pot)
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    return (std::exp (k * x) - 1.0) / (std::exp (k) - 1.0);
}

double CopicatEcho::speedFactor (double v)
{
    return std::pow (1.35, 2.0 * v - 1.0);
}

CopicatEcho::CopicatEcho()
    : fsBase (44100.0), fsOs (176400.0),
      gainT (kDefaultGainPot), swellT (0.55), sustainT (0.45), speedT (0.5), wowT (0.3), ageT (0.25),
      outT (1.0), toneT (0.0), echoT (1.0),
      kIn (0.0), kSwell (0.0), kSustain (0.0), outCal (1.0),
      headScale (1.0), lastPlay (0.0), readPrev (0.0),
      appliedAge (-1.0), appliedSpeed (-1.0), appliedTone (-1.0), appliedHeads (-1.0), filterCountdown (0),
      writePos (0), tapePos (0.0), loopLen (kLoopLength / kTapeSpeed), noiseLp (0.0), spliceEnv (0.0),
      seed (12345u), peakTape (0.0)
{
    headT[0] = 1.0; headT[1] = 0.0; headT[2] = 0.0;
    // Fixed dropout map of this loop of tape (which cells have a coating fault, how deep)
    for (int i = 0; i < kCells; ++i)
    {
        cellHit[i] = (float) (0.5 + 0.5 * nextNoise() / std::sqrt (3.0));
        cellDepth[i] = (float) (0.5 + 0.5 * nextNoise() / std::sqrt (3.0));
    }
    seed = 777u;
    prepare (44100.0);
}

//==============================================================================
double CopicatEcho::nextNoise()
{
    seed = seed * 1664525u + 1013904223u;
    return ((double) (seed >> 8) / 8388608.0 - 1.0) * 1.7320508075688772;   // unit variance
}

double CopicatEcho::inputGain (double pot) const
{
    const double rBot = kPotIn * audioTaper (pot), rTop = kPotIn - rBot;
    const double rp = parallel (rBot, kRsum);
    return (kRf1 / kRsum) * rp / (kRin + rTop + rp);
}

double CopicatEcho::sustainGain (double pot) const
{
    // Loop gain (at 1 kHz, nominal level) against Sustain rotation, set by ear
    // against the owner's unit: a couple of repeats low down, 8-10 strong repeats
    // around 7, long near-steady repeats around 9, runaway only near the top.
    static const double kLoop[11] = { 0.0, 0.05, 0.12, 0.20, 0.32, 0.47, 0.60, 0.72, 0.84, 0.95, 1.08 };
    if (pot <= 0.0) return 0.0;
    if (pot >= 1.0) return kLoop[10] * kNominalRec / kNominalPlay;
    const double x = pot * 10.0;
    const int i = (int) x;
    const double g = kLoop[i] + (kLoop[i + 1] - kLoop[i]) * (x - i);
    return g * kNominalRec / kNominalPlay;
}

double CopicatEcho::potToMixer (double pot, double rSeries, double rFeedback) const
{
    const double rBot = kPot10 * audioTaper (pot), rTop = kPot10 - rBot;
    const double rp = parallel (rBot, rSeries);
    return (rFeedback / rSeries) * rp / (kRplayOut + rTop + rp);
}

//==============================================================================
void CopicatEcho::prepare (double sampleRate)
{
    fsBase = sampleRate > 1000.0 ? sampleRate : 44100.0;
    fsOs = fsBase * Oversampler4x::kFactor;

    smKnob   = std::exp (-1.0 / (0.020 * fsBase));
    smSpeed  = std::exp (-1.0 / (0.350 * fsBase));    // motor and capstan inertia
    smSwitch = std::exp (-1.0 / (0.006 * fsBase));

    inHp.highpass (3.0, fsOs);
    inLp.lowpass (1.0 / (2.0 * kPi * kRf1 * kCf1), fsOs);
    recHeadLp.lowpass (16000.0, fsOs);                 // 10K feed into the record head's inductance
    preEmph.shelf (2500.0, 8000.0, fsOs);              // HF saturates first (self-erasure) ...
    deEmph.shelf (8000.0, 2500.0, fsOs);               // ... exact inverse afterwards
    mixLp.lowpass (1.0 / (2.0 * kPi * kRf4 * kCf4), fsOs);
    outHp.highpass (5.0, fsOs);

    // Playback preamp: the network's time constants, (120K + 33K) x .022u = 3.37 ms
    // and 120K x 470p = 56 us, are the standard tape playback curve
    // (3180 us / 50 us). With the head's rising output that gives a flat response
    // from about 50 Hz to 3 kHz, so each repeat keeps its body. (3K3 sets the gain,
    // which the 1.7 V / 90 mV calibration below takes care of.)
    {
        const double T1 = (kRp1 + kRp2) * kCp2, T2 = kRp1 * kCp1;
        playEq.setAnalog (0.0, T2, 1.0, 0.0, T1, 1.0, fsOs);
    }

    slew1.step = slew2.step = kSlewRate / fsOs;
    outCal = 1.0 / (inputGain (kDefaultGainPot) * kRf4 / kRdry);

    // Playback scale: 1.7 V recorded gives 90 mV from the playback amp at 1 kHz,
    // with new tape, one head, Tone off, normal speed
    {
        const double fs = fsOs, f = 1000.0;
        FirstOrder rl, gl; Biquad tl;
        rl.lowpass (2.0 * kTapeSpeed / (2.0 * kPi * kCoating), fs);
        gl.lowpass (0.443 * kTapeSpeed / kGap, fs);
        tl.lowpass (1.0 / (2.0 * kPi * std::sqrt (kHeadL * kStrayC)), 1.0, fs);
        const double w = 2.0 * kPi * f / fs;
        const double deriv = 2.0 * std::sin (w * 0.5) * fs / (2.0 * kPi * 1000.0);
        const double chain = recHeadLp.magnitude (f, fs) * rl.magnitude (f, fs) * deriv
                           * gl.magnitude (f, fs) * tl.magnitude (f, fs) * playEq.magnitude (f, fs);
        headScale = kNominalPlay / chain;
    }

    reset();
}

void CopicatEcho::reset()
{
    gain = gainT; swell = swellT; sustain = sustainT; speed = speedT; wow = wowT; age = ageT;
    outG = outT; tone = toneT; echo = echoT;
    for (int i = 0; i < 3; ++i) head[i] = headT[i];

    up.reset(); down.reset();
    inHp.reset(); inLp.reset(); recHeadLp.reset(); preEmph.reset(); deEmph.reset();
    recLoss.reset(); gapLp.reset(); mixLp.reset(); outHp.reset();
    spacingLp.reset(); toneLp.reset(); playEq.reset();
    slew1.reset(); slew2.reset();
    lastPlay = 0.0; readPrev = 0.0;

    std::memset (tape, 0, sizeof (tape));
    writePos = 0;
    tapePos = 0.0; loopLen = kLoopLength / kTapeSpeed;
    for (int i = 0; i < 4; ++i) wowPh[i] = 0.17 * i;
    noiseLp = 0.0; spliceEnv = 0.0;
    seed = 777u;

    const double s = speedFactor (speed);
    for (int i = 0; i < 3; ++i)
    {
        avgSpeed[i] = s;
        delayNow[i] = delayPrev[i] = kHeadSeconds[i] * fsOs / s;
    }
    kIn = inputGain (gain);
    kSwell = potToMixer (swell, kRwiper, kRf4);
    kSustain = sustainGain (sustain);
    updateFilters (true);
    filterCountdown = 0;
    peakTape = 0.0;
}

//==============================================================================
void CopicatEcho::setGain (double v)    { gainT = v; }
void CopicatEcho::setSwell (double v)   { swellT = v; }
void CopicatEcho::setSustain (double v) { sustainT = v; }
void CopicatEcho::setHead (int index, bool on) { if (index >= 0 && index < 3) headT[index] = on ? 1.0 : 0.0; }
void CopicatEcho::setTone (bool on)     { toneT = on ? 1.0 : 0.0; }
void CopicatEcho::setEchoIn (bool in)   { echoT = in ? 1.0 : 0.0; }
void CopicatEcho::setSpeed (double v)   { speedT = v; }
void CopicatEcho::setWow (double v)     { wowT = v; }
void CopicatEcho::setTapeAge (double v) { ageT = v; }
void CopicatEcho::setOutputGainDb (double dB) { outT = std::pow (10.0, dB / 20.0); }

double CopicatEcho::headDelayMs (int index) const
{
    return (index >= 0 && index < 3) ? 1000.0 * delayNow[index] / fsOs : 0.0;
}

double CopicatEcho::sustainLoopGain() const
{
    return kSustain * kNominalPlay / kNominalRec;
}

//==============================================================================
// HF losses and the head / Tone network, recomputed while their controls move.
void CopicatEcho::updateFilters (bool force)
{
    const double spd = speedFactor (speed);
    double nHeads = head[0] + head[1] + head[2];
    if (nHeads < 1.0) nHeads = 1.0;
    if (! force && std::fabs (spd - appliedSpeed) < 1.0e-4 && std::fabs (age - appliedAge) < 1.0e-4
        && std::fabs (tone - appliedTone) < 1.0e-4 && std::fabs (nHeads - appliedHeads) < 1.0e-4)
        return;
    appliedSpeed = spd; appliedAge = age; appliedTone = tone; appliedHeads = nHeads;

    const double lim = 0.42 * fsOs;
    const double v = kTapeSpeed * spd;
    double f = 2.0 * v / (2.0 * kPi * kCoating);                // coating thickness loss
    recLoss.lowpass (f < lim ? f : lim, fsOs);
    f = 0.443 * v / kGap;                                       // playback gap loss
    gapLp.lowpass (f < lim ? f : lim, fsOs);
    const double d = 0.5e-6 + 4.0e-6 * age;                     // head-to-tape spacing: worn, dirty tape
    f = 0.345 * v / (2.0 * kPi * d);
    spacingLp.lowpass (f < lim ? f : lim, 0.7071, fsOs);
    // Heads in series (unselected ones shorted) resonate with stray C, or with the 4n7
    const double C = kStrayC + tone * kToneC;
    f = 1.0 / (2.0 * kPi * std::sqrt (nHeads * kHeadL * C));
    toneLp.lowpass (f < lim ? f : lim, 0.65 + 0.35 * (1.0 - tone), fsOs);
}

double CopicatEcho::dropoutAt (double loopPos) const
{
    double c = loopPos / loopLen * kCells;
    if (c < 0.0) c = 0.0;
    int i0 = (int) c;
    const double fr = c - i0;
    i0 %= kCells;
    const int i1 = (i0 + 1) % kCells;
    const double density = 0.02 + 0.18 * age;
    const double depthScale = 0.10 + 0.55 * age;
    double g[2];
    const int idx[2] = { i0, i1 };
    for (int k = 0; k < 2; ++k)
    {
        double dip = cellHit[idx[k]] < density ? cellDepth[idx[k]] * depthScale : 0.0;
        if (idx[k] == 0) dip += 0.05 + 0.35 * age;            // the splice
        g[k] = 1.0 - (dip < 0.9 ? dip : 0.9);
    }
    return g[0] + (g[1] - g[0]) * fr;
}

// Tape speed with wow, flutter and the splice bump; then each head's echo time.
void CopicatEcho::tickModulation()
{
    const double freqs[4] = { 0.55, 1.27, 7.6, 12.9 };
    double sn[4];
    for (int k = 0; k < 4; ++k)
    {
        wowPh[k] += freqs[k] / fsBase;
        if (wowPh[k] >= 1.0) wowPh[k] -= 1.0;
        sn[k] = std::sin (2.0 * kPi * wowPh[k]);
    }
    const double cN = 1.0 - std::exp (-2.0 * kPi * 0.8 / fsBase);
    noiseLp += (nextNoise() - noiseLp) * cN;
    const double nl = noiseLp / std::sqrt (cN * 0.5);

    double flut = wow + 0.5 * age;
    if (flut > 1.0) flut = 1.0;
    const double dev = wow * (0.0040 * sn[0] + 0.0015 * sn[1] + 0.0025 * nl)
                     + flut * (0.0008 * sn[2] + 0.0004 * sn[3])
                     - spliceEnv * 0.004 * age;
    const double s = speedFactor (speed) * (1.0 + dev);

    tapePos += s / fsBase;
    if (tapePos >= loopLen) { tapePos -= loopLen; spliceEnv = 1.0; }
    spliceEnv *= std::exp (-1.0 / (0.025 * fsBase));

    const double maxD = (double) (kBufferSize - 8);
    for (int i = 0; i < 3; ++i)
    {
        const double tau = 0.5 * kHeadSeconds[i];
        avgSpeed[i] += (s - avgSpeed[i]) * (1.0 - std::exp (-1.0 / (tau * fsBase)));
        delayPrev[i] = delayNow[i];
        double dl = kHeadSeconds[i] * fsOs / avgSpeed[i];
        if (dl > maxD) dl = maxD;
        if (dl < 8.0) dl = 8.0;
        delayNow[i] = dl;
    }
}

double CopicatEcho::readTape (double delaySamples) const
{
    const int mask = kBufferSize - 1;
    const double pos = (double) writePos - delaySamples;
    const double fl = std::floor (pos);
    const double t = pos - fl;
    const int i = (int) fl;
    const double xm1 = tape[(i - 1) & mask], x0 = tape[i & mask], x1 = tape[(i + 1) & mask], x2 = tape[(i + 2) & mask];
    // 4-point Hermite
    const double c1 = 0.5 * (x1 - xm1);
    const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
    const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

//==============================================================================
double CopicatEcho::tickOs (double x, double frac)
{
    // Playback heads (reading tape recorded at least one head-time ago)
    double r = 0.0;
    double nHeads = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const double g = head[i];
        if (g > 1.0e-5)
        {
            r += g * readTape (delayPrev[i] + (delayNow[i] - delayPrev[i]) * frac);
            nHeads += g;
        }
    }
    if (nHeads > 0.0)
        r += nextNoise() * std::pow (10.0, (-74.0 + 26.0 * age) / 20.0) * std::sqrt (nHeads);   // hiss

    double v = (r - readPrev) * (fsOs / (2.0 * kPi * 1000.0));   // head output: rate of change of flux
    readPrev = r;
    v = gapLp.tick (v);
    v = spacingLp.tick (v);
    v = toneLp.tick (v);
    double vp = railClip (playEq.tick (v * headScale), kRail);
    vp *= echo;                                                   // Echo in/out
    lastPlay = vp;

    // Input stage and record amp (Sustain feedback joins at its summing node)
    const double v1 = slew1.tick (railClip (inLp.tick (inHp.tick (x)) * kIn, kRail));
    const double vr = slew2.tick (railClip ((kRf2 / kR15) * v1 + kSustain * vp, kRail));

    // Record head and tape
    double u = preEmph.tick (recHeadLp.tick (vr) / kNominalRec);
    u = kTapeSat * std::tanh (u / kTapeSat);
    u = recLoss.tick (deEmph.tick (u));
    u *= dropoutAt (tapePos);
    const double au = std::fabs (u);
    if (au > peakTape) peakTape = au;
    tape[writePos] = (float) u;
    writePos = (writePos + 1) & (kBufferSize - 1);

    // Output mixer
    double vo = (kRf4 / kRdry) * v1 + kSwell * vp;
    vo = railClip (mixLp.tick (vo), kRail);
    return outHp.tick (vo);
}

void CopicatEcho::process (const float* in, float* out, int numSamples)
{
    const double aK = 1.0 - smKnob, aS = 1.0 - smSpeed, aW = 1.0 - smSwitch;
    double xs[Oversampler4x::kFactor], ys[Oversampler4x::kFactor];

    for (int n = 0; n < numSamples; ++n)
    {
        gain += (gainT - gain) * aK;
        swell += (swellT - swell) * aK;
        sustain += (sustainT - sustain) * aK;
        wow += (wowT - wow) * aK;
        age += (ageT - age) * aK;
        outG += (outT - outG) * aK;
        speed += (speedT - speed) * aS;
        tone += (toneT - tone) * aW;
        echo += (echoT - echo) * aW;
        for (int i = 0; i < 3; ++i) head[i] += (headT[i] - head[i]) * aW;

        kIn = inputGain (gain);
        kSwell = potToMixer (swell, kRwiper, kRf4);
        kSustain = sustainGain (sustain);

        if (--filterCountdown <= 0) { updateFilters (false); filterCountdown = 16; }
        tickModulation();

        float xin = in[n];
        if (! (xin == xin)) xin = 0.0f;
        up.upsample ((double) xin, xs);
        for (int k = 0; k < Oversampler4x::kFactor; ++k)
            ys[k] = tickOs (xs[k], (double) (k + 1) / Oversampler4x::kFactor);
        const double y = down.downsample (ys) * outCal * outG;
        out[n] = (float) y;
    }
}

} // namespace tbcc
