#pragma once

#include <cmath>

// Smoothing factor of a first-order low-pass filter with the given cutoff (Hz), for a
// sample arriving `dt` seconds after the previous one.
inline double LowPassAlpha(double dt, double cutoff)
{
    const double tau = 1.0 / (2.0 * 3.14159265358979323846 * cutoff);
    return 1.0 / (1.0 + tau / dt);
}

// One Euro filter (Casiez et al., CHI 2012): an adaptive low-pass filter that is
// heavily smoothed while the hand is still and responsive when it moves fast.
class OneEuroFilter {
public:
    void SetParams(double minCutoff, double beta)
    {
        m_minCutoff = minCutoff;
        m_beta = beta;
    }

    double Filter(double x, double t)
    {
        if (!m_initialized) {
            m_initialized = true;
            m_x = x;
            m_dx = 0.0;
            m_t = t;
            return x;
        }
        const double dt = t - m_t;
        if (dt <= 1e-6)
            return m_x;
        m_t = t;
        const double dx = (x - m_x) / dt;
        m_dx += (dx - m_dx) * LowPassAlpha(dt, kDerivativeCutoff);
        const double cutoff = m_minCutoff + m_beta * std::abs(m_dx);
        m_x += (x - m_x) * LowPassAlpha(dt, cutoff);
        return m_x;
    }

private:
    static constexpr double kDerivativeCutoff = 1.0;

    double m_minCutoff = 1.0;
    double m_beta = 0.0;
    bool m_initialized = false;
    double m_x = 0.0, m_dx = 0.0, m_t = 0.0;
};
