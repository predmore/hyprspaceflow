#include "Motion.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <lua.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
using Scrolling = Layout::Tiled::CScrollingAlgorithm;
using ScrollData = Layout::Tiled::SScrollingData;

HANDLE handle = nullptr;
SP<Config::Values::CFloatValue> sensitivity, acceleration, accelSpeed, friction, inertia, spring;
SP<Config::Values::CBoolValue> snapColumns, reverse;

Scrolling* scrolling(const PHLWORKSPACE& ws) {
    if (!ws || !ws->m_space || !ws->m_space->algorithm())
        return nullptr;
    return dynamic_cast<Scrolling*>(ws->m_space->algorithm()->tiledAlgo().get());
}

SP<ScrollData> scrollData(Scrolling* algorithm) {
    if (!algorithm)
        return nullptr;
    // The center query works for inactive workspaces and floating focus too.
    const auto col = algorithm->getColumnAtViewportCenter();
    return col ? col->scrollingData.lock() : nullptr;
}

struct Workspace {
    PHLWORKSPACEREF workspace;
    WP<ScrollData> data;
    double savedOffset = 0;
    double extent = 0;
    size_t columns = 0;
    float savedAlpha = 1;
    bool savedForce = false, touched = false, savedInhibition = false, hadData = false;
};

class Runtime {
  public:
    flow::Motion motion;
    std::vector<Workspace> workspaces;
    PHLMONITORREF monitor;
    PHLWORKSPACEREF initial, expected;
    Clock::time_point lastFrame{}, lastInput{};
    uint32_t timestamp = 0;
    bool active = false, applying = false, hasTime = false;
    bool savedScanoutBlock = false;
    std::vector<CHyprSignalListener> listeners;

    Runtime() {
        auto& events = Event::bus()->m_events;
        listeners.emplace_back(events.render.pre.listen([this](PHLMONITOR m) {
            if (!active || m != monitor.lock() || motion.phase == flow::Phase::Dragging)
                return;
            if (!valid()) {
                abort(false);
                return;
            }
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - lastFrame).count();
            lastFrame = now;
            // A suspended compositor must not replay seconds of invisible travel.
            if (dt > 0.25) {
                motion.velocity = 0;
                motion.settle();
            }
            motion.tick(std::min(dt, 0.05));
            apply();
            if (motion.phase == flow::Phase::Idle)
                finish();
        }));
        listeners.emplace_back(events.workspace.active.listen([this](PHLWORKSPACE ws) {
            if (active && !applying && ws != expected.lock())
                abort(false);
        }));
        listeners.emplace_back(events.workspace.specialActive.listen([this](PHLWORKSPACE, PHLMONITOR) { abort(false); }));
        listeners.emplace_back(events.workspace.removed.listen([this](PHLWORKSPACEREF) { if (!applying) abort(false); }));
        listeners.emplace_back(events.workspace.created.listen([this](PHLWORKSPACEREF) { if (!applying) abort(false); }));
        listeners.emplace_back(events.workspace.moveToMonitor.listen([this](PHLWORKSPACE, PHLMONITOR) { abort(false); }));
        listeners.emplace_back(events.monitor.preRemoved.listen([this](PHLMONITOR) { abort(false); }));
        listeners.emplace_back(events.monitor.layoutChanged.listen([this]() { abort(false); }));
        listeners.emplace_back(events.monitor.focused.listen([this](PHLMONITOR m) {
            if (active && m != monitor.lock()) abort(false);
        }));
        listeners.emplace_back(events.config.preReload.listen([this]() { abort(false); }));
        listeners.emplace_back(events.window.open.listen([this](PHLWINDOW) { if (!applying) abort(false); }));
        listeners.emplace_back(events.window.close.listen([this](PHLWINDOW) { if (!applying) abort(false); }));
        listeners.emplace_back(events.window.moveToWorkspace.listen([this](PHLWINDOW, PHLWORKSPACE) { if (!applying) abort(false); }));
        listeners.emplace_back(events.window.floating.listen([this](PHLWINDOW) { if (!applying) abort(false); }));
        listeners.emplace_back(events.window.fullscreen.listen([this](PHLWINDOW) { if (!applying) abort(false); }));
        listeners.emplace_back(events.window.active.listen([this](PHLWINDOW, Desktop::eFocusReason) { if (!applying) abort(false); }));
        listeners.emplace_back(events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent, Event::SCallbackInfo&) { stopForInput(); }));
        listeners.emplace_back(events.input.mouse.button.listen([this](IPointer::SButtonEvent, Event::SCallbackInfo&) { stopForInput(); }));
        if (g_pSessionLockManager)
            listeners.emplace_back(g_pSessionLockManager->m_events.lock.listen([this]() { abort(false); }));
    }

    ~Runtime() { abort(false); }

    bool valid() const {
        const auto m = monitor.lock();
        if (!m || !m->m_enabled || m->m_activeSpecialWorkspace ||
            (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()))
            return false;
        if (m->m_activeWorkspace != expected.lock())
            return false;
        for (const auto& state : workspaces) {
            const auto ws = state.workspace.lock();
            if (!::valid(ws) || ws->m_monitor.lock() != m)
                return false;
            // Layout replacement destroys the old data; never retain an
            // algorithm pointer across callbacks or a config reload.
            if (state.hadData && (!state.data || scrollData(scrolling(ws)) != state.data.lock()))
                return false;
            if (const auto data = state.data.lock(); data &&
                (data->columns.size() != state.columns || std::abs(data->maxWidth() - state.extent) > 0.1))
                return false;
        }
        return true;
    }

    flow::Page pageFor(const PHLWORKSPACE& ws, Workspace& state) {
        flow::Page page;
        const auto algorithm = scrolling(ws);
        const auto data = scrollData(algorithm);
        if (!data || !data->controller || !data->controller->isPrimaryHorizontal())
            return page; // mixed layouts and empty workspaces are full pages
        state.data = data;
        state.hadData = true;
        auto& tape = *data->controller;
        state.savedOffset = tape.getOffset();
        state.savedInhibition = tape.getScrollInhibitor().isInhibited;
        const double viewport = algorithm->primaryViewportSize();
        const auto area = algorithm->usableArea();
        static auto fullscreenOne = CConfigValue<Config::INTEGER>("scrolling:fullscreen_on_one_column");
        static auto fit = CConfigValue<Config::INTEGER>("scrolling:focus_fit_method");
        const double extent = data->maxWidth();
        state.extent = extent;
        state.columns = data->columns.size();
        if (!tape.stripCount() || viewport <= 0)
            return page;
        if (extent <= viewport) {
            page.low = page.high = std::round((extent - viewport) / 2);
        } else if (*fit == 0) {
            page.low = (tape.calculateStripSize(0, area, *fullscreenOne) - viewport) / 2;
            const auto last = tape.stripCount() - 1;
            page.high = tape.calculateStripStart(last, area, *fullscreenOne) +
                        (tape.calculateStripSize(last, area, *fullscreenOne) - viewport) / 2;
        } else {
            page.low = 0;
            page.high = extent - viewport;
        }
        // Physical left-to-right route coordinates also support reversed tapes.
        if (tape.isReversed()) {
            const double lo = page.low;
            page.low = -page.high;
            page.high = -lo;
        }
        for (size_t i = 0; i < tape.stripCount(); ++i) {
            const double start = tape.calculateStripStart(i, area, *fullscreenOne);
            const double size = tape.calculateStripSize(i, area, *fullscreenOne);
            auto add = [&](double stop) { page.stops.push_back(tape.isReversed() ? -stop : stop); };
            add(start + (size - viewport) / 2);
            if (*fit != 0) {
                add(start);
                add(start + size - viewport);
            }
        }
        return page;
    }

    bool begin(uint32_t time) {
        const auto m = Desktop::focusState()->monitor();
        if (!m || m->m_activeSpecialWorkspace || !m->m_activeWorkspace ||
            (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) ||
            g_pUnifiedWorkspaceSwipe->isGestureInProgress())
            return false;
        if (active && monitor.lock() == m && valid()) {
            motion.grab(); // catch momentum at its exact visible position
            initial = expected;
            for (auto& state : workspaces)
                if (const auto data = state.data.lock()) state.savedOffset = data->controller->getOffset();
        } else {
            abort(false);
            monitor = m;
            initial = expected = m->m_activeWorkspace;
            motion = {};
            motion.path.gap = std::max(1.0, m->m_size.x);
            std::vector<PHLWORKSPACE> ordered;
            for (const auto& ws : State::workspaceState()->workspaces()) {
                if (ws->m_monitor.lock() == m && !ws->m_isSpecialWorkspace)
                    ordered.push_back(ws.lock());
            }
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a->m_id < b->m_id; });
            for (const auto& ws : ordered) {
                Workspace state;
                state.workspace = ws;
                state.savedAlpha = ws->m_alpha->value();
                state.savedForce = ws->m_forceRendering;
                auto page = pageFor(ws, state);
                const auto index = workspaces.size();
                workspaces.push_back(state);
                motion.path.pages.push_back(page);
                if (ws == initial.lock()) {
                    double offset = state.savedOffset;
                    if (const auto data = state.data.lock(); data && data->controller->isReversed()) offset = -offset;
                    motion.position = motion.path.start(index) + std::clamp(offset, page.low, page.high) - page.low;
                }
            }
            if (workspaces.empty())
                return false;
            motion.grab();
            active = true;
            // Direct scanout bypasses render.pre entirely. Keep the compositor
            // rendering while this interaction owns animated geometry.
            savedScanoutBlock = g_pHyprRenderer->m_directScanoutBlocked;
            g_pHyprRenderer->m_directScanoutBlocked = true;
        }
        motion.tuning = {sensitivity->value(), acceleration->value(), accelSpeed->value(), 0.045,
                         friction->value(), inertia->value(), spring->value(), snapColumns->value()};
        timestamp = time;
        hasTime = true;
        lastFrame = lastInput = Clock::now();
        return true;
    }

    void update(double dx, uint32_t time) {
        if (!active || motion.phase != flow::Phase::Dragging)
            return;
        if (!valid()) {
            abort(false);
            return;
        }
        if (!std::isfinite(dx) || std::abs(dx) > 10000)
            return;
        // Unsigned subtraction handles the libinput timestamp wrap naturally.
        const uint32_t elapsed = hasTime ? time - timestamp : 0;
        const double dt = elapsed <= 200 ? elapsed / 1000.0 : 1.0;
        timestamp = time;
        hasTime = true;
        lastInput = Clock::now();
        motion.drag((reverse->value() ? 1 : -1) * dx, dt);
        apply();
    }

    void release(uint32_t time, bool cancelled) {
        if (!active)
            return;
        if (cancelled) {
            abort(true);
            return;
        }
        if (!valid()) {
            abort(false);
            return;
        }
        const double eventAge = (time - timestamp) / 1000.0;
        const double wallAge = std::chrono::duration<double>(Clock::now() - lastInput).count();
        motion.release(std::max(eventAge, wallAge), motion.path.gap);
        lastFrame = Clock::now();
        damage();
    }

    void damage() {
        if (const auto m = monitor.lock()) {
            g_pHyprRenderer->damageMonitor(m);
            // render.pre runs before m_renderingActive becomes true. Merely
            // calling scheduleFrame there is absorbed by the current frame;
            // request another frame through the renderer's pending flag.
            if (active && motion.phase != flow::Phase::Dragging && motion.phase != flow::Phase::Idle)
                m->m_pendingFrame = true;
            m->scheduleFrame();
        }
    }

    void tape(std::size_t index, double offset) {
        auto& state = workspaces[index];
        const auto data = state.data.lock();
        if (!data || !data->controller)
            return;
        auto& controller = *data->controller;
        controller.getScrollInhibitor().isInhibited = false;
        controller.setOffset(controller.isReversed() ? -offset : offset);
        data->recalculate(true); // warp geometry: no compositor easing behind the fingers
    }

    void apply() {
        if (!active)
            return;
        const auto m = monitor.lock();
        if (!m)
            return;
        const auto sample = motion.path.sample(motion.position);
        const auto selected = sample.transition && sample.progress >= 0.5 ? sample.right : sample.left;
        const auto selectedWorkspace = workspaces[selected].workspace.lock();
        applying = true;
        if (m->m_activeWorkspace != selectedWorkspace) {
            // A large sample may skip directly over a workspace pair. Native
            // switching still changes the old workspace's transform, so own
            // its cleanup even when it was never drawn by a previous sample.
            for (auto& state : workspaces)
                if (state.workspace.lock() == m->m_activeWorkspace) state.touched = true;
            expected = selectedWorkspace;
            m->changeWorkspace(selectedWorkspace, false, true, true);
        }
        // Only two workspaces are rendered at a boundary. Workspaces outside
        // the current pair retain their scrolling position for a reversal.
        for (size_t i = 0; i < workspaces.size(); ++i) {
            auto& state = workspaces[i];
            const auto ws = state.workspace.lock();
            if (!ws)
                continue;
            const bool visible = i == sample.left || (sample.transition && i == sample.right);
            if (!visible && !state.touched)
                continue;
            state.touched = true;
            ws->m_forceRendering = visible;
            ws->m_alpha->setValueAndWarp(visible ? 1.F : 0.F);
            double offset = 0;
            if (sample.transition)
                offset = i == sample.left ? -sample.progress * motion.path.gap : (1 - sample.progress) * motion.path.gap;
            ws->m_renderOffset->setValueAndWarp(visible ? Vector2D{offset, 0.0} : Vector2D{});
            if (i == sample.left) tape(i, sample.leftOffset);
            if (sample.transition && i == sample.right) tape(i, sample.rightOffset);
            if (visible) ws->updateWindowDecos();
        }
        applying = false;
        damage();
    }

    void cleanup() {
        const auto m = monitor.lock();
        for (auto& state : workspaces) {
            if (!state.touched)
                continue;
            if (const auto ws = state.workspace.lock()) {
                ws->m_forceRendering = state.savedForce;
                ws->m_renderOffset->setValueAndWarp(Vector2D{});
                ws->m_alpha->setValueAndWarp(m && m->m_activeWorkspace == ws ? 1.F : state.savedAlpha);
            }
            if (const auto data = state.data.lock()) {
                auto& inhibit = data->controller->getScrollInhibitor();
                inhibit.isInhibited = state.savedInhibition;
                inhibit.offsetWhenInhibited = data->controller->getOffset();
            }
        }
        active = false;
        g_pHyprRenderer->m_directScanoutBlocked = savedScanoutBlock;
        motion.phase = flow::Phase::Idle;
        workspaces.clear();
        damage();
        monitor.reset();
        expected.reset();
        initial.reset();
    }

    void finish() {
        if (!active)
            return;
        const auto m = monitor.lock();
        const auto ws = m ? m->m_activeWorkspace : nullptr;
        applying = true;
        if (auto* algorithm = scrolling(ws)) {
            const auto data = scrollData(algorithm);
            const double offset = data ? data->controller->getOffset() : 0;
            algorithm->focusColumn(algorithm->getColumnAtViewportCenter());
            if (data) {
                data->controller->getScrollInhibitor().isInhibited = false;
                data->controller->setOffset(offset);
                data->recalculate(true);
            }
        } else if (ws) {
            Desktop::focusState()->fullWindowFocus(ws->getFocusCandidate(), Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
        }
        applying = false;
        cleanup();
    }

    void stopForInput() {
        if (!active)
            return;
        motion.velocity = 0;
        motion.position = motion.path.snap(motion.position, false);
        apply();
        finish();
    }

    void abort(bool restore) {
        if (!active)
            return;
        applying = true;
        if (restore) {
            const auto m = monitor.lock();
            const auto ws = initial.lock();
            if (m && ::valid(ws)) {
                expected = ws;
                m->changeWorkspace(ws, false, true, false);
            }
            for (auto& state : workspaces) {
                if (state.touched) {
                    if (const auto data = state.data.lock()) {
                        data->controller->getScrollInhibitor().isInhibited = false;
                        data->controller->setOffset(state.savedOffset);
                        data->recalculate(true);
                    }
                }
            }
        }
        applying = false;
        cleanup();
    }
};

std::unique_ptr<Runtime> runtime;

uint32_t timeField(lua_State* L) {
    lua_getfield(L, 1, "time_ms");
    const auto time = static_cast<uint32_t>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    return time;
}

int start(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const bool started = runtime && runtime->begin(timeField(L));
    lua_pushboolean(L, started);
    return 1;
}
int update(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const auto time = timeField(L);
    lua_getfield(L, 1, "delta");
    luaL_checktype(L, -1, LUA_TTABLE);
    lua_getfield(L, -1, "x");
    const auto delta = luaL_checknumber(L, -1);
    lua_pop(L, 2);
    if (runtime) runtime->update(delta, time);
    return 0;
}
int end(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const auto time = timeField(L);
    lua_getfield(L, 1, "cancelled");
    const bool cancelled = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (runtime) runtime->release(time, cancelled);
    return 0;
}
int cancel(lua_State*) {
    if (runtime) runtime->abort(false);
    return 0;
}
int status(lua_State* L) {
    lua_newtable(L);
    lua_pushboolean(L, runtime && runtime->active); lua_setfield(L, -2, "active");
    lua_pushnumber(L, runtime ? runtime->motion.position : 0); lua_setfield(L, -2, "position");
    lua_pushnumber(L, runtime ? runtime->motion.velocity : 0); lua_setfield(L, -2, "velocity");
    const char* phases[] = {"idle", "dragging", "coasting", "settling"};
    lua_pushstring(L, phases[runtime ? static_cast<int>(runtime->motion.phase) : 0]); lua_setfield(L, -2, "phase");
    lua_pushinteger(L, runtime ? runtime->workspaces.size() : 0); lua_setfield(L, -2, "workspaces");
    lua_pushboolean(L, g_pHyprRenderer->m_directScanoutBlocked); lua_setfield(L, -2, "scanout_blocked");
    lua_pushnumber(L, runtime ? runtime->motion.path.gap : 0); lua_setfield(L, -2, "viewport");
    lua_newtable(L);
    int index = 1;
    for (const auto& ws : State::workspaceState()->workspaces()) {
        if (ws->m_isSpecialWorkspace) continue;
        lua_newtable(L);
        lua_pushinteger(L, ws->m_id); lua_setfield(L, -2, "id");
        lua_pushnumber(L, ws->m_renderOffset->value().x); lua_setfield(L, -2, "render_x");
        lua_pushboolean(L, ws->m_forceRendering); lua_setfield(L, -2, "forced");
        lua_pushnumber(L, ws->m_alpha->value()); lua_setfield(L, -2, "alpha");
        if (const auto data = scrollData(scrolling(ws.lock()))) {
            lua_pushnumber(L, data->controller->getOffset()); lua_setfield(L, -2, "tape");
        }
        if (runtime && runtime->active) {
            for (size_t i = 0; i < runtime->workspaces.size(); ++i) {
                if (runtime->workspaces[i].workspace.lock() != ws.lock()) continue;
                const auto& page = runtime->motion.path.pages[i];
                lua_pushnumber(L, runtime->motion.path.start(i)); lua_setfield(L, -2, "start");
                lua_pushnumber(L, page.low); lua_setfield(L, -2, "low");
                lua_pushnumber(L, page.high); lua_setfield(L, -2, "high");
            }
        }
        lua_rawseti(L, -2, index++);
    }
    lua_setfield(L, -2, "pages");
    return 1;
}

void options() {
    auto floating = [](const char* name, const char* description, double def, double lo, double hi) {
        auto value = makeShared<Config::Values::CFloatValue>(name, description, def,
            Config::Values::SFloatValueOptions{.min = lo, .max = hi});
        if (!HyprlandAPI::addConfigValueV2(handle, value)) throw std::runtime_error("hyprspaceflow: option registration failed");
        return value;
    };
    sensitivity = floating("plugin:hyprspaceflow:sensitivity", "Logical pixels per gesture unit", 3, 0.1, 20);
    acceleration = floating("plugin:hyprspaceflow:acceleration", "Maximum extra speed-dependent gain", 0.65, 0, 3);
    accelSpeed = floating("plugin:hyprspaceflow:acceleration_speed", "Gesture units per second for full acceleration", 1200, 50, 10000);
    friction = floating("plugin:hyprspaceflow:friction", "Inertia decay per second", 9, 2, 40);
    inertia = floating("plugin:hyprspaceflow:inertia", "Maximum coast in viewport widths", 0.55, 0, 3);
    spring = floating("plugin:hyprspaceflow:spring", "Critical settling frequency per second", 24, 8, 60);
    snapColumns = makeShared<Config::Values::CBoolValue>("plugin:hyprspaceflow:snap_columns", "Settle on a column after coasting", true);
    reverse = makeShared<Config::Values::CBoolValue>("plugin:hyprspaceflow:reverse", "Reverse swipe direction", false);
    if (!HyprlandAPI::addConfigValueV2(handle, snapColumns) || !HyprlandAPI::addConfigValueV2(handle, reverse))
        throw std::runtime_error("hyprspaceflow: option registration failed");
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE pluginHandle) {
    handle = pluginHandle;
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        throw std::runtime_error("hyprspaceflow: rebuild against the running Hyprland headers (ABI mismatch)");
    options();
    runtime = std::make_unique<Runtime>();
    if (!HyprlandAPI::addLuaFunction(handle, "hyprspaceflow", "start", start) ||
        !HyprlandAPI::addLuaFunction(handle, "hyprspaceflow", "update", update) ||
        !HyprlandAPI::addLuaFunction(handle, "hyprspaceflow", "finish", end) ||
        !HyprlandAPI::addLuaFunction(handle, "hyprspaceflow", "cancel", cancel) ||
        !HyprlandAPI::addLuaFunction(handle, "hyprspaceflow", "status", status)) {
        runtime.reset();
        throw std::runtime_error("hyprspaceflow: Lua registration failed");
    }
    return {"hyprspaceflow", "Continuous scrolling-to-workspace swipes with inertia", "predmore", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    runtime.reset();
    sensitivity.reset(); acceleration.reset(); accelSpeed.reset();
    friction.reset(); inertia.reset(); spring.reset(); snapColumns.reset(); reverse.reset();
    handle = nullptr;
}
