// Filters.h
// First- and second-order sections made from analog component transfer
// functions by the bilinear transform (no prewarp; everything runs at the 4x
// rate, where the error is negligible in the audio band). Transposed direct
// form II in double precision; coefficients can change between samples while
// the state is kept, so component values can move without clicks.
// Plain C++98, no allocation.

#ifndef TBCC_FILTERS_H
#define TBCC_FILTERS_H

#include <cmath>

namespace tbcc
{

const double kPi = 3.14159265358979323846;

// H(s) = (n1 s + n0) / (d1 s + d0)
struct FirstOrder
{
    double b0, b1, a1, z;

    FirstOrder() : b0 (1.0), b1 (0.0), a1 (0.0), z (0.0) {}
    void reset() { z = 0.0; }

    void setAnalog (double n1, double n0, double d1, double d0, double fs)
    {
        const double K = 2.0 * fs;
        const double a0 = d1 * K + d0;
        b0 = (n1 * K + n0) / a0;
        b1 = (n0 - n1 * K) / a0;
        a1 = (d0 - d1 * K) / a0;
    }
    void lowpass (double fc, double fs)  { const double w = 2.0 * kPi * fc; setAnalog (0.0, w, 1.0, w, fs); }
    void highpass (double fc, double fs) { const double w = 2.0 * kPi * fc; setAnalog (1.0, 0.0, 1.0, w, fs); }
    // Shelf (1 + s/w1) / (1 + s/w2)
    void shelf (double f1, double f2, double fs)
    {
        setAnalog (1.0 / (2.0 * kPi * f1), 1.0, 1.0 / (2.0 * kPi * f2), 1.0, fs);
    }

    inline double tick (double x)
    {
        const double y = b0 * x + z;
        z = b1 * x - a1 * y;
        return y;
    }

    double magnitude (double f, double fs) const
    {
        const double w = 2.0 * kPi * f / fs;
        const double cr = std::cos (w), ci = -std::sin (w);              // z^-1
        const double nr = b0 + b1 * cr, ni = b1 * ci;
        const double dr = 1.0 + a1 * cr, di = a1 * ci;
        return std::sqrt ((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

// H(s) = (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0)
struct Biquad
{
    double b0, b1, b2, a1, a2, z1, z2;

    Biquad() : b0 (1.0), b1 (0.0), b2 (0.0), a1 (0.0), a2 (0.0), z1 (0.0), z2 (0.0) {}
    void reset() { z1 = z2 = 0.0; }

    void setAnalog (double n2, double n1, double n0, double d2, double d1, double d0, double fs)
    {
        const double K = 2.0 * fs, K2 = K * K;
        const double a0 = d2 * K2 + d1 * K + d0;
        b0 = (n2 * K2 + n1 * K + n0) / a0;
        b1 = (2.0 * n0 - 2.0 * n2 * K2) / a0;
        b2 = (n2 * K2 - n1 * K + n0) / a0;
        a1 = (2.0 * d0 - 2.0 * d2 * K2) / a0;
        a2 = (d2 * K2 - d1 * K + d0) / a0;
    }
    // Resonant low-pass 1 / (s^2/w^2 + s/(Q w) + 1)
    void lowpass (double fc, double q, double fs)
    {
        const double w = 2.0 * kPi * fc;
        setAnalog (0.0, 0.0, 1.0, 1.0 / (w * w), 1.0 / (q * w), 1.0, fs);
    }

    inline double tick (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    double magnitude (double f, double fs) const
    {
        const double w = 2.0 * kPi * f / fs;
        const double c1 = std::cos (w), s1 = -std::sin (w), c2 = std::cos (2.0 * w), s2 = -std::sin (2.0 * w);
        const double nr = b0 + b1 * c1 + b2 * c2, ni = b1 * s1 + b2 * s2;
        const double dr = 1.0 + a1 * c1 + a2 * c2, di = a1 * s1 + a2 * s2;
        return std::sqrt ((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

// 741 output: linear inside the rails, rounding off into a hard limit.
inline double railClip (double v, double rail)
{
    const double knee = rail - 2.0;
    const double a = std::fabs (v);
    if (a <= knee) return v;
    const double c = knee + 2.0 * std::tanh ((a - knee) * 0.5);
    return v < 0.0 ? -c : c;
}

// 741 slew-rate limit (0.5 V/us), one step per oversampled sample.
struct Slew
{
    double y, step;
    Slew() : y (0.0), step (1.0) {}
    void reset() { y = 0.0; }
    inline double tick (double x)
    {
        const double d = x - y;
        y += d > step ? step : (d < -step ? -step : d);
        return y;
    }
};

} // namespace tbcc

#endif
