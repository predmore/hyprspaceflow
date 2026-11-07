#pragma once

#include <algorithm>
#include <cmath>

namespace flow::detail {

inline double gainShape(double q) {
    q = std::clamp(q, 0.0, 1.0);
    return q * q * (3 - 2 * q);
}

// Mean of r(t)^k, where r(t)=(1-exp(-t/tau))/(1-exp(-T/tau)).
// Bounded endpoints keep the cubic well-conditioned even when an unusually
// fast packet reaches full acceleration very early in its interval.
inline double gainMoment(int k, double h) {
    const double v = -std::expm1(-h);
    if (v < 0.25) {
        // h=-log(1-v). Evaluate the tail directly instead of subtracting
        // nearly equal numbers when the input interval is short.
        double sum = 1.0 / (k + 1), power = 1;
        for (int i = 1; i < 32; ++i) {
            power *= v;
            const double term = power / (k + i + 1);
            sum += term;
            if (term < sum * 1e-16)
                break;
        }
        return (v / h) * sum;
    }
    double tail = h, power = 1;
    for (int j = 1; j <= k; ++j) {
        power *= v;
        tail -= power / j;
    }
    return tail / (h * power);
}

inline double boundedGainIntegral(double dt, double tau, double q0, double q1) {
    if (dt <= 0)
        return 0;
    const double d = q1 - q0, h = dt / tau;
    const double average = gainShape(q0) + 6 * q0 * (1 - q0) * d * gainMoment(1, h) +
                           (3 - 6 * q0) * d * d * gainMoment(2, h) - 2 * d * d * d * gainMoment(3, h);
    return dt * std::clamp(average, 0.0, 1.0);
}

// Analytically average the saturated cubic gain while the speed estimate
// follows ds/dt=(inputSpeed-s)/tau. Sampling only at the packet's endpoint
// otherwise gives different distances on 60 Hz and 240 Hz devices.
inline double averageGainShape(double dt, double tau, double speed0, double inputSpeed, double threshold) {
    const double a = inputSpeed / threshold, q0 = speed0 / threshold;
    const double q1 = q0 * std::exp(-dt / tau) + a * (-std::expm1(-dt / tau));
    if (std::max(q0, q1) <= 1)
        return boundedGainIntegral(dt, tau, q0, q1) / dt;
    if (std::min(q0, q1) >= 1)
        return 1;
    // The estimate crosses saturation at most once. The upward/downward
    // expressions avoid cancellation at either extreme.
    const double crossing = q0 < 1 ? -tau * std::log1p(-(1 - q0) / (a - q0)) :
                                    -tau * std::log((1 - a) / (q0 - a));
    const double at = std::clamp(crossing, 0.0, dt);
    const double integral = q0 < 1 ? boundedGainIntegral(at, tau, q0, 1) + (dt - at) :
                                    at + boundedGainIntegral(dt - at, tau, 1, q1);
    return std::clamp(integral / dt, 0.0, 1.0);
}

} // namespace flow::detail
