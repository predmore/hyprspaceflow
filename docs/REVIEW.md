# Independent experience review

Version 0.2.0 received **9.5/10** after two review rounds, up from **7.8/10**
for 0.1.1. The final weighted score was 9.535, with no unresolved high- or
medium-priority blockers.

A separate reviewer agent assessed the implementation and evidence without
editing the production code. A second agent audited the motion mathematics,
and another wrote the compositor regressions. The implementer addressed the
findings and ran the isolated compositor scenarios; the reviewer independently
reran the portable and sanitizer suites and examined the scenario assertions
and pass evidence.

| Criterion | Weight | Initial | Final |
| --- | ---: | ---: | ---: |
| Direct tracking and acceleration | 25% | 8.2 | 9.6 |
| Boundaries and reversals | 25% | 8.5 | 9.7 |
| Inertia, catching, and settling | 20% | 7.5 | 9.5 |
| Focus, interruption, and lifecycle integration | 20% | 6.5 | 9.5 |
| Test credibility and operational clarity | 10% | 7.0 | 9.1 |

The review drove fixes for hidden-window keyboard focus, background activity
interruptions, dropped frame time, packet-dependent acceleration, an abrupt
pause cutoff, delayed momentum catching, fullscreen hidden travel, and
controller/visual ownership leaks during workspace changes.

Evidence includes 40,254 portable checks, independent AddressSanitizer and
UndefinedBehaviorSanitizer runs, both compositor replay suites, a deliberate
120 ms stall, and two-output workspace transfer using a headless test output.
The count includes randomized bounds assertions; it does not represent
40,254 distinct usability scenarios.

This is a software-behavior assessment, not a measured human rating of physical
touchpad comfort or sensor-to-photon latency. Physical VRR and hardware
multi-monitor testing remain outside the evidence. A representative release
estimator retained about 1.25 logical pixels of coast variation between
60 Hz and 240 Hz input; accelerated displacement itself matched the reference.

The exact reviewed source tree was
`b3e84f0e27233f2f1d1e316d7a7a790a7b920145`. Only this review record and release
documentation/pins were added afterward.

The original reports are preserved verbatim:

- [Round 1](review-round-1.txt)
- [Final independent assessment](review-round-2.txt)

See [validation details](VALIDATION.md) for the test environment and scope.
