# hyprspaceflow

One continuous three-finger swipe through scrolling windows and workspaces.

Scroll the windows in your current workspace, keep moving at the edge, and
slide into the next workspace without lifting your fingers. Keep going to
traverse further workspaces. Reverse anywhere to follow the same path back.

Position tracks the gesture directly. A smooth speed-dependent gain helps
longer swipes travel further; release adds a short, bounded exponential coast,
followed by critically damped settling. A raw swipe-begin catches motion at its
current position, before Hyprland classifies the horizontal direction. No timer
polls while idle, no external gesture daemon, and
no machine-code hooks are used.

**Early experimental release.** Built and replay-tested against **Hyprland
0.56.2**, commit `efb50993780079460b0cbed1363e2166a2de1d9f`, with Lua configuration.
Hyprland's C++ ABI changes between builds. Rebuild after upgrading Hyprland;
the plugin refuses to load when the header ABI differs from the compositor.

## Build

Install a C++23 compiler, GNU Make, pkg-config, the development headers matching
your running Hyprland, and Lua development files. On Arch, the installed
`hyprland` package provides its headers; keep the system fully updated so the
headers and running compositor match.

```sh
git clone https://github.com/predmore/hyprspaceflow.git
cd hyprspaceflow
make
make test
make install
```

The default install paths are `~/.local/lib/hyprspaceflow/hyprspaceflow.so` and
`~/.local/share/hyprspaceflow/hyprspaceflow.lua`. Override `PREFIX` if desired.
Builds replace the shared library atomically; an already loaded copy keeps
running until explicitly unloaded or the compositor restarts.

For hyprpm:

```sh
hyprpm add https://github.com/predmore/hyprspaceflow
hyprpm enable hyprspaceflow
```

The gesture binding below is still required. With hyprpm, omit the
`hl.plugin.load(...)` line because hyprpm manages loading.

## Configure

Add this **after** your other gesture declarations in `hyprland.lua` or a Lua
module it loads. Enable Hyprland's native `scrolling` layout on the workspaces
where you want to move through windows.

```lua
local base = os.getenv("HOME") .. "/.local"
hl.plugin.load(base .. "/lib/hyprspaceflow/hyprspaceflow.so")
local flow = dofile(base .. "/share/hyprspaceflow/hyprspaceflow.lua")

if flow.available() then
    hl.config({ plugin = { hyprspaceflow = {
        sensitivity = 3.0,
        acceleration = 0.65,
        inertia = 0.55,
    } } })

    flow.bind(3)
    -- Use flow.bind(3, true) instead if a three-finger horizontal binding
    -- already exists. Hyprland requires explicitly removing that binding.
end
```

Declare `hl.plugin.load(...)` on every config pass, outside `flow.available()`.
Hyprland loads declared plugins after the first pass and reruns configuration.

On Omarchy, your existing three-finger horizontal workspace binding usually
means you should use `flow.bind(3, true)`. Keep existing vertical and pinch
bindings. If another plugin temporarily takes over horizontal swiping, have
its restoration callback call `flow.bind(3)` after removing its own binding.
For an overview that changes horizontal bindings, cancel any running inertia
when opening the overview:

```lua
if hl.plugin.hyprspaceflow then hl.plugin.hyprspaceflow.cancel() end
```

Run `hyprctl reload` and `hyprctl configerrors` after editing configuration.

## Motion settings

All options live under `plugin.hyprspaceflow` in `hl.config()`.

| Option | Default | Effect |
| --- | ---: | --- |
| `sensitivity` | `3.0` | Logical pixels per gesture unit at low speed |
| `acceleration` | `0.65` | Maximum additional gain at high speed; `0` gives linear tracking |
| `acceleration_speed` | `1200.0` | Gesture units/second that reach full extra gain |
| `friction` | `9.0` | Exponential decay per second; higher stops sooner |
| `inertia` | `0.55` | Maximum coast distance in viewport widths; `0` disables coasting |
| `spring` | `24.0` | Critical settling frequency; higher settles faster |
| `snap_columns` | `true` | Settle to a nearby column after coasting |
| `reverse` | `false` | Flip swipe direction |

By default, swiping left moves the content left and reveals windows/spaces to
the right. Gesture direction is independent of two-finger natural scrolling.
For a more literal feel, set `acceleration = 0`. For less glide, reduce `inertia`
to `0.25` or increase `friction` to `12`.

Release velocity is filtered over 45 ms, discarded on direction reversal,
and decays smoothly when the fingers pause before release. The acceleration
gain is integrated across each input packet so the same finger motion travels
the same distance across touchpad input rates. Both coasting and
settling use analytical, time-based integration, including short frame stalls.
Position during contact is
not smoothed: layout geometry is warped to the finger position rather than
chasing it through a compositor animation.

## Workspace behavior

- Existing ordinary workspaces on the originating monitor, sorted by ID.
- No wraparound, implicit workspace creation, or transfer to another monitor.
- Horizontal native scrolling layouts are traversed from their left edge to
  their right edge. Reversed horizontal tapes are supported too.
- Empty workspaces and other layouts each occupy a complete workspace page;
  vertical scrolling layouts use this same page fallback.
- Covering fullscreen windows are one page too: hidden columns do not consume
  invisible scrolling distance before the workspace starts moving.
- A workspace transition occupies one monitor width. Only the outgoing and
  incoming workspaces are force-rendered. The active workspace changes at the
  midpoint; keyboard focus follows the final viewport on completion.
- Special workspaces and locked sessions are left to Hyprland.
- Background window activity refreshes unseen pages without moving the view
  held under your fingers. The captured workspace list stays fixed until the
  next gesture; removed or transferred background pages are released safely.
- Config reloads, changes to the visible layout, other workspace switches,
  keyboard input, and clicks interrupt motion and release rendering
  ownership. A cancelled gesture drops inertia and settles at the nearest
  workspace edge, preserving the scrolling distance already travelled.
- Finishing or interrupting the gesture reconciles keyboard focus with the
  visible workspace. Explicit user focus changes keep their chosen target.

New three-finger contact pauses kinetic motion at the raw swipe-begin event.
If it becomes a horizontal gesture, motion resumes from that exact position.
If it ends or becomes another gesture, the held view settles without an extra
fling. This describes compositor event handling, not measured sensor latency.

## Diagnostics and testing

```sh
hyprctl repl 'return hl.plugin.hyprspaceflow.status().phase'
hyprctl eval 'hl.plugin.hyprspaceflow.cancel()'
make test
```

`status()` also returns position, velocity, viewport width, and per-workspace
tape/render state, controller inhibition, and the last interruption reason.
`start(event)`, `update(event)`, and `finish(event)` accept
the same tables supplied by Hyprland's Lua gesture callbacks, making recorded
input replay possible without touchpad hardware.
`raw_begin(event)` and `raw_finish(event)` replay the same raw contact path
used by the compositor's swipe-begin/end listeners.

The portable tests cover multi-workspace traversal, reversals, direct tracking,
momentum catching, stale release velocity, partial-boundary settling, refresh
rate independence, finite bounds, and randomized long recordings.
An optional [GitHub Actions template](docs/github-actions.example.yml) runs
these tests and the sanitizers. Copy it to `.github/workflows/test.yml` to
enable it in a repository with workflow write permission.

For compositor replay tests, start a **disposable nested Hyprland session**
with the plugin loaded, workspaces 1–4 persistent, and at least three tiled
windows on workspace 1. Then run:

```sh
python3 tests/integration.py --instance THE_NESTED_INSTANCE_SIGNATURE
python3 tests/review_integration.py --instance THE_NESTED_INSTANCE_SIGNATURE
```

`examples/nested.lua` supplies this fixture when Foot is installed. Launch it
with an absolute config path:

```sh
HYPRLAND_NO_SD_VARS=1 HYPRLAND_NO_SD_NOTIFY=1 Hyprland -c "$PWD/examples/nested.lua"
```

F12 exits the nested session. Use `hyprctl instances` to find its signature.

The replay runner refuses to target the instance in your current environment.
See [validation notes](docs/VALIDATION.md) for the checks run on this release.
The [independent review](docs/REVIEW.md) records the two rounds that brought
the reviewed software behavior to 9.5/10, including the remaining evidence limits.

## License

[MIT](LICENSE), copyright 2026 predmore.
