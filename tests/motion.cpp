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
        near(m.velocity, 0, 1e-9, "holding fingers before lifting prevents stale fling");
        check(m.phase == flow::Phase::Settling, "stale release only settles");

        double finalPosition = -1;
        for (double hz : {30., 60., 90., 120., 144., 240.}) {
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
