#pragma once

#include "SpeedGain.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace flow {

struct Page {
    double low = 0, high = 0;
    std::vector<double> stops;
};

struct Sample {
    std::size_t left = 0, right = 0;
    double leftOffset = 0, rightOffset = 0;
    double progress = 0;
    bool transition = false;
};

// All coordinates are logical desktop pixels. A gap is a complete workspace
// translation, not a dead zone: its entire distance is visible motion.
class Path {
  public:
    std::vector<Page> pages;
    double gap = 1;

    double start(std::size_t page) const {
        double x = 0;
        for (std::size_t i = 0; i < page && i < pages.size(); ++i)
            x += length(i) + gap;
        return x;
    }
    double length(std::size_t page) const { return std::max(0.0, pages[page].high - pages[page].low); }
    double end() const { return pages.empty() ? 0 : start(pages.size() - 1) + length(pages.size() - 1); }
    Sample sample(double x) const {
        if (pages.empty())
            return {};
        x = std::clamp(x, 0.0, end());
        double base = 0;
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const double last = base + length(i);
            if (x <= last || i + 1 == pages.size())
                return {i, i, pages[i].low + std::clamp(x - base, 0.0, length(i)), 0, 0, false};
            if (x < last + gap)
                return {i, i + 1, pages[i].high, pages[i + 1].low, (x - last) / gap, true};
            base = last + gap;
        }
        return {};
    }
    double snap(double x, bool columns) const {
        const auto s = sample(x);
        if (s.transition)
            return start(s.left) + length(s.left) + (s.progress >= 0.5 ? gap : 0);
        if (!columns || pages.empty() || pages[s.left].stops.empty())
            return std::clamp(x, 0.0, end());
        const auto& p = pages[s.left];
        double best = std::clamp(p.stops.front(), p.low, p.high);
        for (double stop : p.stops) {
            stop = std::clamp(stop, p.low, p.high);
            if (std::abs(stop - s.leftOffset) < std::abs(best - s.leftOffset))
                best = stop;
        }
        return start(s.left) + best - p.low;
    }
};

struct Tuning {
    double sensitivity = 3.0;
    double acceleration = 0.65;
    double accelerationSpeed = 1200.0;
    double velocityTime = 0.045;
    double friction = 9.0;
    double inertia = 0.55; // maximum coast distance in viewport widths
    double spring = 24.0;
    bool snapColumns = true;
};

enum class Phase { Idle, Dragging, Coasting, Settling };

// No compositor dependencies: exact exponential coast and critically damped
// settling make release motion independent of display refresh rate.
class Motion {
  public:
    Path path;
    Tuning tuning;
    double position = 0, velocity = 0;
    Phase phase = Phase::Idle;

    void grab() {
        phase = Phase::Dragging;
        velocity = 0;
        m_speed = 0;
    }
    void rebase(double delta) {
        const double shifted = position + delta, target = m_target + delta;
        if (std::isfinite(delta) && std::isfinite(shifted) && std::isfinite(target)) {
            position = shifted;
            m_target = target;
        }
    }
    void drag(double rawDelta, double dt) {
        if (phase != Phase::Dragging || !std::isfinite(rawDelta) || !std::isfinite(dt) || dt < 0)
            return;
        // Direct position updates never go through a smoothing filter. Only
        // speed estimation is filtered, so slow tracking has no added latency.
        const double speed = dt > 0 ? std::abs(rawDelta) / dt : 0;
        const bool timed = dt > 0 && std::isfinite(speed);
        const double alpha = timed ? -std::expm1(-dt / tuning.velocityTime) : 0;
        const double shape = timed ? detail::averageGainShape(dt, tuning.velocityTime, m_speed, speed, tuning.accelerationSpeed) :
                                     detail::gainShape(m_speed / tuning.accelerationSpeed);
        const double gain = tuning.sensitivity * (1 + tuning.acceleration * shape);
        if (timed) {
            const double filtered = m_speed * std::exp(-dt / tuning.velocityTime) + speed * alpha;
            m_speed = std::isfinite(filtered) ? filtered : std::max(m_speed, speed);
        }
        const double delta = rawDelta * gain;
        const double old = position;
        position = std::clamp(position + delta, 0.0, path.end());
        // Millisecond timestamps can repeat. Preserve direct displacement
        // and the estimator for those packets instead of dividing by zero
        // or erasing a valid fling because two events share a timestamp.
        if (rawDelta * velocity < 0)
            velocity = 0;
        if (timed) {
            const double instant = delta / dt;
            velocity = std::isfinite(instant) ? velocity + alpha * (instant - velocity) : 0;
        }
        if (position == old && delta != 0)
            velocity = 0;
    }
    void release(double staleSeconds, double viewport) {
        if (phase != Phase::Dragging)
            return;
        // Bound the estimator before ageing it. Otherwise an arbitrarily
        // large/batched sample can outlive a pause and still launch a full
        // maximum-distance fling after the fingers have stopped.
        const double cap = std::max(0.0, viewport * tuning.inertia * tuning.friction);
        velocity = std::clamp(velocity, -cap, cap);
        if (!std::isfinite(staleSeconds))
            velocity = 0;
        else
            velocity *= std::exp(-std::max(0.0, staleSeconds) / tuning.velocityTime);
        phase = std::abs(velocity) > 25 ? Phase::Coasting : Phase::Settling;
        if (phase == Phase::Settling)
            settle();
    }
    void settle() {
        m_target = path.snap(position, tuning.snapColumns);
        phase = Phase::Settling;
    }
    void tick(double dt) {
        if (!std::isfinite(dt) || dt <= 0 || phase == Phase::Idle || phase == Phase::Dragging)
            return;
        if (phase == Phase::Coasting) {
            // Integrate exactly to the stop threshold, then spend any remaining
            // frame time settling. No frame-rate-dependent tail distance.
            const double until = std::log(std::max(25.0, std::abs(velocity)) / 25.0) / tuning.friction;
            const double step = std::min(dt, until);
            const double decay = std::exp(-tuning.friction * step);
            const double proposed = position + velocity * (1 - decay) / tuning.friction;
            position = std::clamp(proposed, 0.0, path.end());
            velocity *= decay;
            dt -= step;
            if (proposed != position) {
                velocity = 0;
                settle();
            } else if (step >= until) {
                settle();
            } else {
                return;
            }
        }
        if (phase != Phase::Settling)
            return;
        const double y = position - m_target;
        const double c = velocity + tuning.spring * y;
        const double decay = std::exp(-tuning.spring * dt);
        position = std::clamp(m_target + (y + c * dt) * decay, 0.0, path.end());
        velocity = (velocity - tuning.spring * c * dt) * decay;
        if (std::abs(position - m_target) < 0.05 && std::abs(velocity) < 1) {
            position = m_target;
            velocity = 0;
            phase = Phase::Idle;
        }
    }

  private:
    double m_speed = 0, m_target = 0;
};

} // namespace flow
