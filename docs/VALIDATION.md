# Validation

Initial validation: November 6, 2026.

Environment: Arch Linux on AArch64, GCC 16.1.1, Lua 5.5.1, Hyprland 0.56.2
(`efb50993780079460b0cbed1363e2166a2de1d9f`). The plugin is original code
under the MIT license; Hyprland sources were consulted for API behavior.

## Portable motion

`make test`: **40,047 checks passed**, including 20,000 seeded randomized
input samples. Tests exercise distance conservation across several boundaries
in one event, reversals, acceleration bounds, timestamp gaps, partial
transitions, momentum catching, stale velocity suppression, and finite bounds.
The same coast is checked at 30, 60, 90, 120, 144, and 240 Hz.

The motion suite also passed AddressSanitizer and UndefinedBehaviorSanitizer.
Local LeakSanitizer was disabled because the agent sandbox uses tracing;
the provided CI template uses the sanitizer defaults on GitHub Actions.

## Compositor integration

A separate nested Hyprland instance loaded the shared library against its
matching ABI. Four persistent workspaces and five real Foot windows provided
the scrolling fixture. `tests/integration.py` passed these replay checks:

- Direct camera tracking without compositor animation lag.
- Exact 25%/75% outgoing/incoming workspace translations at a boundary.
- Crossing three workspace boundaries and reversing without another start.
- Releasing rendering ownership on explicit cancellation.
- Visible inertia after release and catching it without a position jump.
- Partial workspace transitions settling to an endpoint and becoming idle.
- External workspace switches releasing gesture ownership.
- Nonfinite input rejection and 32-bit input timestamp wrap.
- Physical tracking on a reversed horizontal scrolling tape.
- Continuous traversal through a workspace using the dwindle layout.

Manual IPC checks also unloaded the library during a running swipe, loaded it
again, and reloaded config mid-swipe. The compositor remained responsive,
rendering ownership was released, and configuration errors were empty.

The integration suite exposed and verified fixes for two compositor-specific
issues: skipped-page cleanup on large input samples, and requesting the next
release-animation frame before Hyprland marks the renderer active.

## Scope

The 0.1.1 regression suite additionally releases at 49% and 51% of the
workspace transition, with normal and cancelled end events in both directions.
It starts with focus at the far-left window, enables column snapping and
focus-following, then verifies that workspace 1 retains its rightmost tape
offset after release. The old cancelled-end behavior reproduced a rewind to
the initial offset; cancelled contact now suppresses inertia and settles at
the current boundary instead.

Replay tests exercise real compositor geometry and rendering state. They do
not measure physical touchpad feel or frame pacing with a high-speed camera.
Defaults are initial tuning values; real-device feedback can refine the gain
and inertia. Multi-monitor routing is restricted in code to the originating
monitor; physical multi-monitor and VRR testing have not been performed.
