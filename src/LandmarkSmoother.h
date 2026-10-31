#pragma once

#include "OneEuroFilter.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

// Steadies a hand's 21 landmarks the way MediaPipe does (LandmarksSmoothingCalculator):
// a One Euro filter on every coordinate, with speed measured in hand sizes per second
// so it behaves the same near and far from the camera. A still joint is held steady,
// and because the cutoff climbs steeply with speed, a moving one follows with almost
// no lag (about 0.2% of the hand's size once it is under way).
class LandmarkSmoother {
public:
    // Smooths `points`, observed at time t (seconds), in place.
    void Apply(std::array<cv::Point3f, 21>& points, double t)
    {
        cv::Point2f lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
        for (const cv::Point3f& p : points) {
            lo = cv::Point2f(std::min(lo.x, p.x), std::min(lo.y, p.y));
            hi = cv::Point2f(std::max(hi.x, p.x), std::max(hi.y, p.y));
        }
        const double scale = ((hi.x - lo.x) + (hi.y - lo.y)) / 2;   // hand size
        if (!(scale > 0))
            return;
        if (!m_initialized || t - m_time > kMaxGap) {
            for (size_t i = 0; i < points.size(); ++i)
                for (int k = 0; k < 3; ++k)
                    m_axes[i][k] = {At(points[i], k), At(points[i], k), 0.0};
            m_time = t;
            m_initialized = true;
            return;
        }
        const double dt = t - m_time;
        if (dt <= 1e-6)
            return;
        m_time = t;
        const double speedAlpha = LowPassAlpha(dt, kSpeedCutoff);
        for (size_t i = 0; i < points.size(); ++i) {
            for (int k = 0; k < 3; ++k) {
                Axis& a = m_axes[i][k];
                const double x = At(points[i], k);
                a.speed += speedAlpha * ((x - a.raw) / (scale * dt) - a.speed);
                a.value += LowPassAlpha(dt, kMinCutoff + kBeta * std::abs(a.speed)) * (x - a.value);
                a.raw = x;
                At(points[i], k) = float(a.value);
            }
        }
    }

private:
    // MediaPipe's parameters: cutoff (Hz) when still, its rise per hand size per
    // second of speed, and the cutoff of the speed estimate itself.
    static constexpr double kMinCutoff = 0.05;
    static constexpr double kBeta = 80.0;
    static constexpr double kSpeedCutoff = 1.0;
    static constexpr double kMaxGap = 0.5;   // seconds without data before starting over

    struct Axis {
        double value = 0.0;   // smoothed
        double raw = 0.0;     // last observation
        double speed = 0.0;   // smoothed, hand sizes per second
    };

    static float& At(cv::Point3f& p, int k) { return k == 0 ? p.x : k == 1 ? p.y : p.z; }

    std::array<std::array<Axis, 3>, 21> m_axes{};
    double m_time = 0.0;
    bool m_initialized = false;
};
