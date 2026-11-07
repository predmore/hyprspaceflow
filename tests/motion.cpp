#include "../src/Motion.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

static int checks = 0;
static void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static void near(double a, double b, double tolerance, const char* message) {
    check(std::abs(a - b) <= tolerance, message);
}
static flow::Path fixture() {
    return {{{-200, 1800, {-200, 300, 800, 1300, 1800}},
             {0, 1000, {0, 500, 1000}},
             {0, 0, {0}},
             {-100, 900, {-100, 400, 900}}}, 1000};
}
static flow::Motion motion() {
    flow::Motion m;
    m.path = fixture();
    m.tuning.acceleration = 0;
    m.tuning.sensitivity = 1;
    m.grab();
    return m;
}
static void run(flow::Motion& m, double hz) {
    for (int i = 0; i < hz * 8 && m.phase != flow::Phase::Idle; ++i)
        m.tick(1.0 / hz);
    check(m.phase == flow::Phase::Idle, "release must converge");
}

static flow::Motion freeMotion() {
    flow::Motion m;
    m.path = {{{0, 100000000, {}}}, 1000};
    m.position = 10000;
    m.tuning.snapColumns = false;
    m.grab();
    return m;
}

// Independent numerical reference: integrate the physical constant input
// speed against its exact speed-filter trajectory with Simpson quadrature.
static double referenceDrag(double speed, double duration, double initialSpeed = 0) {
    constexpr int slices = 20000;
    auto at = [&](double t) {
        const double estimate = initialSpeed * std::exp(-t / 0.045) + speed * (1 - std::exp(-t / 0.045));
        const double q = std::clamp(estimate / 1200, 0.0, 1.0);
        return speed * 3 * (1 + 0.65 * q * q * (3 - 2 * q));
    };
    double sum = at(0) + at(duration);
    for (int i = 1; i < slices; ++i)
        sum += (i % 2 ? 4 : 2) * at(duration * i / slices);
    return sum * duration / (3 * slices);
}

static void advance(flow::Motion& m, double seconds, double hz) {
    const int frames = static_cast<int>(std::floor(seconds * hz));
    for (int i = 0; i < frames; ++i)
        m.tick(1 / hz);
    const double remaining = seconds - frames / hz;
    if (remaining > 1e-12)
        m.tick(remaining);
}

int main() {
    try {
        auto path = fixture();
        near(path.end(), 7000, 1e-9, "path includes every scroll and workspace transition");
        auto s = path.sample(2500);
        check(s.transition && s.left == 0 && s.right == 1, "boundary connects adjacent workspaces");
        near(s.progress, .5, 1e-9, "continuous half-workspace translation");
        near(s.leftOffset, 1800, 1e-9, "outgoing tape stays at edge");
        near(s.rightOffset, 0, 1e-9, "incoming tape starts at edge");

        auto m = motion();
        m.drag(6600, .05);
        s = m.path.sample(m.position);
        check(!s.transition && s.left == 3, "one update can cross several workspaces without lifting");
        near(s.leftOffset, 500, 1e-9, "leftover motion is preserved after all boundaries");
        m.drag(-4400, .05);
        s = m.path.sample(m.position);
        check(s.transition && s.left == 0, "one reversal traverses the same path backwards");
        near(s.progress, .2, 1e-9, "reverse keeps exact continuous position");
        check(m.velocity < 0, "reversal immediately discards forward momentum");

        m = motion();
        m.position = 2150;
        m.tuning.inertia = 0;
        m.release(0, 1000);
        run(m, 60);
        near(m.position, 2000, .05, "small partial boundary returns to outgoing workspace");
        m = motion();
        m.position = 2750;
        m.tuning.inertia = 0;
        m.release(0, 1000);
        run(m, 120);
        near(m.position, 3000, .05, "large partial boundary settles to incoming workspace");

        m = motion();
        for (int i = 0; i < 20; ++i) m.drag(25, .008);
        m.release(0, 1000);
        check(m.phase == flow::Phase::Coasting, "quick swipe has inertia");
        const double releasePosition = m.position;
        m.tick(.05);
        check(m.position > releasePosition, "inertia is visible after release");
        const double caught = m.position;
        m.grab();
        m.tick(.05);
        near(m.position, caught, 1e-9, "touching again catches motion without a jump");
        near(m.velocity, 0, 1e-9, "new touch clears kinetic velocity");

        m = motion();
        for (int i = 0; i < 10; ++i) m.drag(50, .008);
        m.release(.3, 1000);
        check(std::abs(m.velocity) < 25, "holding fingers before lifting prevents stale fling");
        check(m.phase == flow::Phase::Settling, "stale release only settles");

        double finalPosition = -1;
        for (double hz : {10., 15., 30., 60., 90., 120., 144., 240.}) {
            m = motion();
            m.tuning.snapColumns = false;
            for (int i = 0; i < 15; ++i) m.drag(35, .008);
            m.release(0, 1000);
            run(m, hz);
            if (finalPosition < 0) finalPosition = m.position;
            near(m.position, finalPosition, .1, "coasting distance is refresh-rate independent");
            check(m.position <= 525 + 550.1, "inertia respects viewport distance budget");
        }

        for (double eventHz : {60., 120., 240.}) {
            m = motion();
            for (int i = 0; i < int(eventHz); ++i) m.drag(500 / eventHz, 1 / eventHz);
            near(m.position, 500, 1e-7, "direct tracking preserves distance across input rates");
        }
        auto distanceWithAcceleration = [](double speed) {
            auto accelerated = motion();
            accelerated.tuning.acceleration = .65;
            for (int i = 0; i < 100; ++i) accelerated.drag(2, 2 / speed);
            return accelerated.position;
        };
        check(distanceWithAcceleration(1400) > distanceWithAcceleration(100) * 1.4,
              "fast swipes receive smooth bounded acceleration");
        check(distanceWithAcceleration(1400) <= 200 * 1.65 + .01,
              "acceleration never exceeds configured gain");

        for (double speed : {100., 1200., 1800., 10000.}) {
            for (double duration : {.025, .05, .1, .2}) {
                const double expected = referenceDrag(speed, duration);
                for (int packets : {1, 3, 6, 24, 120}) {
                    m = freeMotion();
                    for (int i = 0; i < packets; ++i)
                        m.drag(speed * duration / packets, duration / packets);
                    near(m.position - 10000, expected, 1e-6,
                         "accelerated distance agrees with physical reference regardless of packetization");
                }
            }
        }
        // Include a downward crossing of the gain saturation threshold.
        const double initialEstimate = 2400 * (1 - std::exp(-.15 / .045));
        const double twoSegment = referenceDrag(2400, .15) + referenceDrag(150, .2, initialEstimate);
        for (int packets : {1, 2, 6, 24, 120}) {
            m = freeMotion();
            for (int i = 0; i < packets; ++i) m.drag(2400 * .15 / packets, .15 / packets);
            for (int i = 0; i < packets; ++i) m.drag(150 * .2 / packets, .2 / packets);
            near(m.position - 10000, twoSegment, 1e-6, "gain decay through saturation is packet-independent");
        }

        double previousCoast = -1;
        for (int milliseconds = 90; milliseconds <= 110; ++milliseconds) {
            m = freeMotion();
            for (int i = 0; i < 12; ++i) m.drag(10, .008);
            const double initialVelocity = m.velocity, start = m.position;
            const double pause = milliseconds / 1000.0;
            m.release(pause, 1000);
            near(m.velocity, std::clamp(initialVelocity, -4950.0, 4950.0) * std::exp(-pause / .045), 1e-8,
                 "release velocity decays continuously through a 100 ms pause");
            run(m, 240);
            const double coast = m.position - start;
            if (previousCoast >= 0)
                check(coast <= previousCoast && previousCoast - coast < 3,
                      "one millisecond of extra pause cannot suddenly remove the whole coast");
            previousCoast = coast;
        }
        m = freeMotion();
        m.velocity = 1e12;
        const double beforePausedBurst = m.position;
        m.release(.18, 1000);
        near(m.velocity, 4950 * std::exp(-.18 / .045), 1e-8,
             "pause decay reduces the maximum permissible velocity even after a huge batched sample");
        run(m, 240);
        check(m.position - beforePausedBurst < 11,
              "a paused burst cannot launch the full maximum coast distance");
        m = freeMotion();
        for (int i = 0; i < 12; ++i) m.drag(10, .008);
        const double timedVelocity = m.velocity, timedPosition = m.position;
        m.drag(0, 0);
        near(m.position, timedPosition, 0, "duplicate stationary timestamp preserves position");
        near(m.velocity, timedVelocity, 0, "duplicate timestamp does not erase a valid release estimate");
        m.drag(-1, 0);
        check(m.position < timedPosition && m.velocity == 0, "untimed reversal discards old-direction momentum immediately");
        m.drag(-1, .008);
        check(m.velocity < 0, "timed tracking resumes normally after an untimed reversal");
        m.release(std::numeric_limits<double>::infinity(), 1000);
        near(m.velocity, 0, 0, "nonfinite release age cannot fling");

        // Compare trajectories at real elapsed times, not just the final stop.
        for (double hz : {10., 15., 60., 240.}) {
            for (double seconds : {.1, .2, .3}) {
                m = freeMotion();
                m.velocity = 600;
                m.release(0, 1000);
                advance(m, seconds, hz);
                near(m.position, 10000 + 600 * (1 - std::exp(-9 * seconds)) / 9, 1e-8,
                     "coasting follows the analytic trajectory at low and high frame rates");
                near(m.velocity, 600 * std::exp(-9 * seconds), 1e-8,
                     "coasting velocity uses full elapsed time at low frame rates");

                m = motion();
                m.position = 2450;
                m.tuning.inertia = 0;
                m.release(0, 1000);
                advance(m, seconds, hz);
                near(m.position, 2000 + 450 * (1 + 24 * seconds) * std::exp(-24 * seconds), 1e-8,
                     "critical settling follows the analytic trajectory at low and high frame rates");
            }
        }

        m = motion();
        m.position = 2150;
        m.tuning.inertia = 0;
        m.release(0, 1000);
        m.tick(.03);
        const double rebasedPosition = m.position, rebasedVelocity = m.velocity;
        m.path.pages[0].high += 300;
        m.rebase(300);
        near(m.position, rebasedPosition + 300, 1e-9, "rebase shifts the current coordinate without a visual jump");
        near(m.velocity, rebasedVelocity, 0, "rebase preserves settling velocity");
        check(m.phase == flow::Phase::Settling, "rebase preserves the active motion phase");
        run(m, 60);
        near(m.position, 2300, .05, "rebase shifts the private settling target as well as position");

        m = motion();
        m.position = 3200;
        m.velocity = 500;
        m.release(0, 1000);
        auto original = m;
        m.path.pages[0].high += 300;
        m.rebase(300);
        advance(m, .1, 60);
        advance(original, .1, 60);
        near(m.position, original.position + 300, 1e-8, "coasting survives a path coordinate rebase");
        near(m.path.sample(m.position).leftOffset, original.path.sample(original.position).leftOffset, 1e-8,
             "rebasing earlier workspaces preserves the visible tape offset");
        run(m, 60);
        run(original, 60);
        near(m.position, original.position + 300, .05, "rebased coasting lands at the same physical column");
        m = motion();
        const auto before = m.position;
        m.drag(std::numeric_limits<double>::quiet_NaN(), .01);
        m.drag(std::numeric_limits<double>::infinity(), .01);
        m.tick(std::numeric_limits<double>::quiet_NaN());
        near(m.position, before, 0, "malformed samples cannot poison the motion state");
        m.drag(-1000, .01);
        near(m.position, 0, 0, "outer edge does not wrap");
        m.drag(100000, .01);
        near(m.position, m.path.end(), 0, "large updates stay bounded");

        // Randomized long recordings exercise boundary continuity, reversals,
        // empty pages and finite results under mixed input/frame timing.
        std::mt19937 rng(20261106);
        std::uniform_real_distribution<double> delta(-250, 250);
        std::uniform_real_distribution<double> duration(.002, .04);
        m = motion();
        for (int i = 0; i < 20000; ++i) {
            m.drag(delta(rng), duration(rng));
            check(std::isfinite(m.position) && m.position >= 0 && m.position <= m.path.end(), "recording stays finite and bounded");
            const auto sample = m.path.sample(m.position);
            check(sample.left < m.path.pages.size() && sample.right < m.path.pages.size(), "recording never addresses an invalid workspace");
        }
        flow::Path empty;
        near(empty.end(), 0, 0, "empty path is safe");
        std::cout << checks << " motion checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
