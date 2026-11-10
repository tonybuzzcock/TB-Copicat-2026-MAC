// CopicatEcho.h
// WEM Copicat IC300 tape echo (drawing 577, 27-6-79), modelled from the sheet:
//
//   input pot network + 741 summing amp (120K||100p over 47K)
//   -> 741 record amp (560K over 15K; Sustain feedback joins here) -> record head
//   -> tape loop: bias-linearised saturation with HF self-erasure, recording and
//      spacing losses, dropouts and the splice, wow and flutter
//   -> three playback heads on push buttons, in series (unselected heads shorted,
//      so the head inductance and the Tone switch's 4n7 interact)
//   -> 741 playback preamp (its time constants give the standard tape playback curve)
//   -> Swell (10K log) and Sustain (curve set by ear against the owner's unit)
//      -> 741 output mixer (dry 27K, echo 10K, 47K||100p)
//
// The op-amp stages are linear apart from their rails and slew rate, so each is
// its exact component transfer function rather than a nodal Newton solve.
// The tape transport is not on the drawing: head times (70/220/330 ms) and
// tape speed (9.5 cm/s) are estimates.
//
// Volts in, volts out scaled so the dry signal is unity at the default Gain.
// Runs at 4x with Oversampler4x. Plain C++98, no allocation after construction.

#ifndef TBCC_COPICATECHO_H
#define TBCC_COPICATECHO_H

#include "Filters.h"
#include "Oversampler4x.h"

namespace tbcc
{

// Echo times at normal speed (the IC300's head spacing is not on the drawing):
// head 1 set to 70 ms by the owner, heads 2 and 3 estimated. The Speed trim
// scales them by 1 / (0.74x .. 1.35x).
const double kHeadSeconds[3] = { 0.070, 0.220, 0.330 };

class CopicatEcho
{
public:
    CopicatEcho();

    void prepare (double sampleRate);
    void reset();

    // Front panel (0..1 pot rotation; switches as bool)
    void setGain (double v);
    void setSwell (double v);
    void setSustain (double v);
    void setHead (int index, bool on);      // 0..2
    void setTone (bool on);
    void setEchoIn (bool in);
    // Plugin only
    void setSpeed (double v);               // 0..1 -> 0.74x..1.35x tape speed
    void setWow (double v);                 // 0..1
    void setTapeAge (double v);             // 0..1
    void setOutputGainDb (double dB);

    void process (const float* in, float* out, int numSamples);

    static int latencySamples() { return Oversampler4x::latencySamples(); }

    // Introspection for tests
    double headDelayMs (int index) const;   // current echo time of a head
    static double speedFactor (double v);
    static double audioTaper (double x);
    double sustainLoopGain() const;         // small-signal loop gain at 1 kHz, Sustain at current setting
    double peakTapeLevel() const { return peakTape; }
    void   clearPeakTape() { peakTape = 0.0; }

    enum { kBufferSize = 1 << 19, kCells = 256 };

private:
    void updateFilters (bool force);
    void tickModulation();
    double tickOs (double x, double frac);
    double readTape (double delaySamples) const;
    double dropoutAt (double loopPos) const;
    double nextNoise();
    double inputGain (double pot) const;
    double potToMixer (double pot, double rSeries, double rFeedback) const;
    double sustainGain (double pot) const;

    double fsBase, fsOs;
    Oversampler4x up, down;

    // Targets and smoothed values
    double gainT, swellT, sustainT, speedT, wowT, ageT, outT, toneT, echoT, headT[3];
    double gain, swell, sustain, speed, wow, age, outG, tone, echo, head[3];
    double smKnob, smSpeed, smSwitch;

    // Derived per base sample
    double kIn, kSwell, kSustain, outCal;

    // Circuit filters
    FirstOrder inHp, inLp, recHeadLp, preEmph, deEmph, recLoss, gapLp, mixLp, outHp;
    Biquad spacingLp, toneLp, playEq;
    Slew slew1, slew2;
    double headScale, lastPlay, readPrev;
    double appliedAge, appliedSpeed, appliedTone, appliedHeads;
    int filterCountdown;

    // Tape transport
    float tape[kBufferSize];
    int writePos;
    double avgSpeed[3], delayPrev[3], delayNow[3];
    double tapePos, loopLen;
    double wowPh[4], noiseLp, spliceEnv;
    unsigned int seed;
    float cellHit[kCells], cellDepth[kCells];

    double peakTape;
};

} // namespace tbcc

#endif
