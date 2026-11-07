#!/usr/bin/env python3
"""Independent experience regressions for an explicitly selected nested instance.

Start examples/nested.lua first. It must provide persistent workspaces 1..4,
three or more hyprspaceflow-test windows on workspace 1, and the local plugin.
This runner creates its own destination/background Foot windows, exercises the
disposable compositor, and closes its own fixtures on exit. It never targets
an instance by numeric alias or the caller's current desktop signature.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import time


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--instance', required=True)
parser.add_argument('--plugin', type=Path, default=Path(__file__).resolve().parents[1] / 'hyprspaceflow.so')
parser.add_argument('--skip-lifecycle', action='store_true', help='Leave unload/reload checks to an external harness.')
args = parser.parse_args()
if not re.fullmatch(r'[0-9a-f]{40}_\d+_\d+', args.instance):
    parser.error('Use the complete disposable instance signature, never a numeric alias.')
if args.instance == os.environ.get('HYPRLAND_INSTANCE_SIGNATURE'):
    parser.error('Refusing to target the current desktop compositor.')
if not args.plugin.is_absolute() or not args.plugin.is_file():
    parser.error('--plugin must name the local shared library used by the nested config.')


def ctl(*parts):
    result = subprocess.run(['hyprctl', '-i', args.instance, *parts], text=True,
                            capture_output=True, timeout=8)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout.strip()


def lua(code):
    result = ctl('eval', code)
    if result != 'ok':
        raise AssertionError(result)


def data(*command):
    return json.loads(ctl('-j', *command))


def wait_for(fn, message, timeout=3):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        result = fn()
        if result:
            return result
        time.sleep(.01)
    raise AssertionError(message)


def install_helpers():
    lua('''
function __flow_review_json(v)
  local t=type(v)
  if t=="nil" then return "null" end
  if t=="boolean" then return v and "true" or "false" end
  if t=="number" then
    assert(v==v and math.abs(v)<math.huge,"nonfinite diagnostic number")
    return tostring(v)
  end
  if t=="string" then return string.format("%q",v) end
  assert(t=="table","unexpected diagnostic value")
  local items={}
  if #v>0 then
    for _,item in ipairs(v) do table.insert(items,__flow_review_json(item)) end
    return "["..table.concat(items,",").."]"
  end
  for k,item in pairs(v) do
    table.insert(items,string.format("%q",tostring(k))..":"..__flow_review_json(item))
  end
  return "{"..table.concat(items,",").."}"
end
function __flow_review_page(id)
  for _,p in ipairs(hl.plugin.hyprspaceflow.status().pages) do
    if p.id==id then return p end
  end
  error("missing workspace "..id)
end
hl.plugin.hyprspaceflow.cancel()
hl.config({input={follow_mouse=0},scrolling={direction="right",focus_fit_method=0},
  plugin={hyprspaceflow={sensitivity=1.0,acceleration=0.0,inertia=0.55,snap_columns=false}}})
''')


def status():
    return json.loads(ctl('repl', 'return __flow_review_json(hl.plugin.hyprspaceflow.status())'))


def pages(snapshot=None):
    raw = (snapshot or status())['pages']
    return {int(p['id']): p for p in (raw.values() if isinstance(raw, dict) else raw)}


def finite_geometry():
    for window in data('clients'):
        for value in (*window['at'], *window['size']):
            assert math.isfinite(value), 'window geometry became nonfinite'
        assert min(window['size']) > 0, 'window size became nonpositive'
    for page in pages().values():
        for name in ('render_x', 'alpha', 'tape', 'low', 'high', 'start'):
            if name in page:
                assert math.isfinite(page[name]), f'nonfinite {name}'


def clean_pair(ids=(1, 2)):
    state = status()
    assert not state['active'], 'gesture ownership was not released'
    assert state['phase'] == 'idle', 'release did not reach idle'
    assert not state['scanout_blocked'], 'direct-scanout override leaked'
    for workspace_id in ids:
        page = pages(state).get(workspace_id)
        if page:
            assert not page['forced'], f'workspace {workspace_id} remains force-rendered'
            assert abs(page['render_x']) < .1, f'workspace {workspace_id} retains a gesture transform'
    finite_geometry()


def focused_on_active(require_window=True):
    workspace = data('activeworkspace')['id']
    window = data('activewindow')
    if require_window:
        assert window and window.get('mapped'), 'no keyboard-focused destination window'
    if window:
        assert window['workspace']['id'] == workspace, 'keyboard focus remains on a hidden workspace'


def focus_window(address):
    lua(f'hl.plugin.hyprspaceflow.cancel(); hl.dispatch(hl.dsp.focus({{window={json.dumps("address:" + address)}}}))')


def wait_idle():
    wait_for(lambda: status()['phase'] == 'idle', 'release did not settle')


def begin_at_left():
    focus_window(first)
    lua('''
local p=hl.plugin.hyprspaceflow
assert(p.start({time_ms=1000}))
local page=__flow_review_page(1)
local start=page.start
p.update({time_ms=1010,delta={x=p.status().position-start}})
assert(p.status().active,"lost ownership while moving to tape edge")
''')


def move_transition(fraction):
    lua(f'''
local p=hl.plugin.hyprspaceflow
local page=__flow_review_page(1)
local goal=page.start+page.high-page.low+p.status().viewport*{fraction}
p.update({{time_ms=1020,delta={{x=-(goal-p.status().position)}}}})
assert(p.status().active,"lost ownership at workspace boundary")
''')


def begin_coast():
    begin_at_left()
    lua('''
local p=hl.plugin.hyprspaceflow
for i=1,12 do p.update({time_ms=1010+i*8,delta={x=-12}}) end
p.finish({time_ms=1106,cancelled=false})
assert(p.status().phase=="coasting","fixture did not generate momentum")
''')


def fixture_command(app_id):
    return 'foot --app-id=' + shlex.quote(app_id) + ' --title=' + shlex.quote(app_id)


def create_fixture(name, workspace, silent=False):
    app_id = f'hyprspaceflow-review-{os.getpid()}-{name}'
    command = fixture_command(app_id)
    if silent:
        command = f'[workspace {workspace} silent] ' + command
        lua(f'hl.dispatch(hl.dsp.exec_cmd({json.dumps(command)}))')
    else:
        lua(f'hl.exec_cmd({json.dumps(command)})')
    window = wait_for(lambda: next((w for w in data('clients') if w['class'] == app_id), None),
                      f'{name} Foot fixture did not map')
    created.append(window['address'])
    if not silent:
        lua(f'hl.dispatch(hl.dsp.window.move({{window={json.dumps("address:" + window["address"])},workspace="{workspace}",follow=false}}))')
    wait_for(lambda: next((w for w in data('clients') if w['address'] == window['address'] and
                          w['workspace']['id'] == workspace), None), 'fixture landed on wrong workspace')
    return window['address']


def close_fixture(address):
    if not any(w['address'] == address for w in data('clients')):
        return
    lua(f'hl.dispatch(hl.dsp.window.close({{window={json.dumps("address:" + address)}}}))')
    wait_for(lambda: not any(w['address'] == address for w in data('clients')), 'fixture did not close')


def set_fullscreen(address, enabled):
    # Matched to Hyprland 0.56.2's LuaBindingsDispatchers.cpp: an explicit
    # window target and layout_aware=false request covering default fullscreen
    # without focusing the window or activating its hidden workspace.
    action = 'set' if enabled else 'unset'
    lua(f'hl.dispatch(hl.dsp.window.fullscreen({{window={json.dumps("address:" + address)},mode="fullscreen",action="{action}",layout_aware=false}}))')


def visit_second_then_third():
    begin_at_left()
    lua('''
local p=hl.plugin.hyprspaceflow
local page=__flow_review_page(2)
local middle=page.start+(page.high-page.low)/2
p.update({time_ms=1020,delta={x=-(middle-p.status().position)}})
assert(p.status().active and hl.get_active_workspace().id==2,"did not visit populated workspace 2")
assert(__flow_review_page(2).inhibited,"test did not acquire workspace 2 tape inhibition")
local third=__flow_review_page(3).start
p.update({time_ms=1030,delta={x=-(third-p.status().position)}})
assert(p.status().active and hl.get_active_workspace().id==3,"did not leave workspace 2 hidden")
assert(__flow_review_page(2).inhibited,"workspace 2 tape was not retained for the ownership test")
''')


def passed(name):
    print('PASS', name, flush=True)


# Read-only fixture verification runs before any mutation. The main desktop
# cannot accidentally be selected through an alias or an ordinary project tab.
workspace_info = {w['id']: w for w in data('workspaces')}
assert all(workspace_info.get(i, {}).get('ispersistent') for i in range(1, 5)), 'Need disposable persistent fixture workspaces 1..4'
source_windows = [w for w in data('clients') if w['workspace']['id'] == 1 and
                  w['class'] == 'hyprspaceflow-test' and not w['floating']]
assert len(source_windows) >= 3, 'Need the disposable nested Foot fixture on workspace 1'
first = min(source_windows, key=lambda w: w['at'][0])['address']
created = []
unloaded = False
fullscreen = False
hidden_fullscreen = None
moved_workspace_origin = None

try:
    install_helpers()
    destination = create_fixture('destination', 2)

    begin_at_left()
    move_transition(.7)
    assert data('activeworkspace')['id'] == 2
    lua('hl.plugin.hyprspaceflow.cancel()')
    clean_pair()
    focused_on_active()
    passed('explicit cancellation transfers keyboard focus to the visible workspace')

    begin_at_left()
    move_transition(.2)
    position = status()['position']
    background = create_fixture('background', 4, silent=True)
    active_id = data('activeworkspace')['id']
    background_id = next(w['workspace']['id'] for w in data('clients') if w['address'] == background)
    assert status()['active'], f'background mapping interrupted the swipe (active workspace={active_id}, background workspace={background_id})'
    assert abs(status()['position'] - position) < .1, 'background mapping shifted the held position'
    assert data('activeworkspace')['id'] == 1, 'silent background fixture stole the active workspace'
    close_fixture(background)
    assert status()['active'], f'background close interrupted the swipe (active workspace={data("activeworkspace")["id"]})'
    lua('hl.plugin.hyprspaceflow.update({time_ms=1030,delta={x=-15}})')
    assert status()['active'], 'background topology invalidated the next input sample'
    lua('hl.plugin.hyprspaceflow.cancel()')
    clean_pair()
    focused_on_active()
    passed('background open/close does not interrupt or displace the active gesture')

    begin_at_left()
    move_transition(.7)
    close_fixture(destination)
    wait_idle()
    clean_pair()
    focused_on_active(require_window=False)
    passed('visible topology interruption releases ownership and does not focus a hidden window')
    destination = create_fixture('destination-again', 2)

    focus_window(first)
    set_fullscreen(first, True)
    fullscreen = True
    wait_for(lambda: data('activewindow').get('fullscreen', 0) != 0, 'fullscreen fixture did not enter fullscreen')
    lua('assert(hl.plugin.hyprspaceflow.start({time_ms=2000}))')
    source = pages()[1]
    assert abs(source['high'] - source['low']) < .1, 'fullscreen has invisible hidden-column travel'
    width = status()['viewport']
    lua(f'hl.plugin.hyprspaceflow.update({{time_ms=2010,delta={{x=-{width * .25}}}}})')
    assert abs(pages()[1]['render_x'] + width * .25) < .2, 'fullscreen motion is not immediately visible'
    lua('hl.plugin.hyprspaceflow.cancel()')
    clean_pair()
    set_fullscreen(first, False)
    fullscreen = False
    passed('covering fullscreen is a single page with immediate workspace tracking')

    # Hidden fallback refresh must return the old tape controller's inhibition
    # before replacing its tracked data with a fullscreen page. Three columns
    # make a subsequent native column move observable, without another plugin
    # start masking a leaked inhibition flag.
    create_fixture('inhibitor-column-a', 2)
    create_fixture('inhibitor-column-b', 2)
    baseline_inhibited = pages()[2]['inhibited']
    assert not baseline_inhibited, 'fixture tape is already inhibited before ownership test'
    visit_second_then_third()
    previous_focus = data('activewindow').get('address')
    set_fullscreen(destination, True)
    hidden_fullscreen = destination
    wait_for(lambda: next((w for w in data('clients') if w['address'] == destination and
                           w.get('fullscreen') == 2), None), 'hidden covering fullscreen did not apply')
    assert data('activeworkspace')['id'] == 3, 'targeted fullscreen activated its hidden workspace'
    assert data('activewindow').get('address') == previous_focus, 'targeted fullscreen changed keyboard focus'
    lua('''
local p=hl.plugin.hyprspaceflow
p.update({time_ms=1040,delta={x=-1}})
assert(p.status().active,"hidden fullscreen refresh interrupted the gesture")
local page=__flow_review_page(2)
assert(math.abs(page.high-page.low)<.1,"hidden fullscreen did not refresh to a page fallback")
p.cancel()
''')
    clean_pair((1, 2, 3))
    assert pages()[2]['inhibited'] == baseline_inhibited, 'fullscreen fallback leaked the previously owned tape inhibition'
    set_fullscreen(destination, False)
    hidden_fullscreen = None
    focus_window(destination)
    before_native_move = pages()[2]['tape']
    lua('hl.dispatch(hl.dsp.layout("move +col"))')
    assert abs(pages()[2]['tape'] - before_native_move) > 20, 'native tape remained locked after hidden fullscreen exited'
    assert pages()[2]['inhibited'] == baseline_inhibited, 'native movement changed restored inhibition unexpectedly'
    focused_on_active()
    passed('hidden fullscreen fallback releases the old tape controller and native scrolling remains usable')

    # Run the companion removal case only when the caller supplies a second
    # output in the disposable compositor. Do not create or reconfigure one.
    origin = next(w['monitor'] for w in data('workspaces') if w['id'] == 2)
    other_output = next((m['name'] for m in data('monitors') if m['name'] != origin), None)
    if other_output:
        baseline_inhibited = pages()[2]['inhibited']
        visit_second_then_third()
        lua(f'hl.dispatch(hl.dsp.workspace.move({{workspace="2",monitor={json.dumps(other_output)}}}))')
        moved_workspace_origin = origin
        assert data('activeworkspace')['id'] == 3, 'moving the hidden workspace changed the active workspace'
        lua('hl.plugin.hyprspaceflow.update({time_ms=1040,delta={x=-1}}); assert(hl.plugin.hyprspaceflow.status().active); hl.plugin.hyprspaceflow.cancel()')
        clean_pair((1, 3))
        assert pages()[2]['inhibited'] == baseline_inhibited, 'removing a touched workspace from the route leaked inhibition'
        assert not pages()[2]['forced'], 'moved workspace remains force-rendered by the old gesture'
        lua(f'hl.dispatch(hl.dsp.workspace.move({{workspace="2",monitor={json.dumps(origin)}}}))')
        moved_workspace_origin = None
        passed('moving a previously touched hidden workspace off-monitor releases its tape ownership')
    else:
        print('SKIP touched-workspace monitor removal (disposable compositor has one output)', flush=True)

    # A newly created empty source is intentionally not made persistent. It
    # must survive a mid-swipe workspace change long enough to reverse/carry on.
    lua('hl.dispatch(hl.dsp.focus({workspace="9"})); assert(hl.plugin.hyprspaceflow.start({time_ms=3000}))')
    assert not next(w for w in data('workspaces') if w['id'] == 9)['ispersistent']
    width = status()['viewport']
    lua(f'hl.plugin.hyprspaceflow.update({{time_ms=3010,delta={{x={width * .6}}}}})')
    time.sleep(.04)
    assert status()['active'], 'empty nonpersistent source vanished during the swipe'
    lua(f'hl.plugin.hyprspaceflow.update({{time_ms=3020,delta={{x={width * .6}}}}})')
    assert status()['active'], 'could not continue across a sparse empty source'
    lua('hl.plugin.hyprspaceflow.finish({time_ms=3200,cancelled=true})')
    wait_idle()
    clean_pair((3, 4, 9))
    focused_on_active(require_window=False)
    passed('sparse nonpersistent empty workspace does not invalidate continuous traversal')

    begin_coast()
    lua('''
local p=hl.plugin.hyprspaceflow
assert(type(p.raw_begin)=="function" and type(p.raw_finish)=="function","missing raw-touch replay API")
local x=p.status().position
p.raw_begin({time_ms=1120,fingers=3})
assert(p.status().awaiting_gesture,"raw contact did not hold momentum")
assert(p.status().position==x,"raw contact jumped")
__flow_review_caught=x
''')
    time.sleep(.08)
    lua('''
local p=hl.plugin.hyprspaceflow
assert(math.abs(p.status().position-__flow_review_caught)<.01,"momentum continued before direction classification")
assert(p.start({time_ms=1200}))
assert(not p.status().awaiting_gesture,"recognized swipe did not claim the held contact")
assert(math.abs(p.status().position-__flow_review_caught)<.01,"recognized catch jumped")
p.cancel()
''')
    clean_pair()
    passed('raw three-finger contact catches momentum before horizontal classification')

    begin_coast()
    lua('hl.plugin.hyprspaceflow.raw_begin({time_ms=1120,fingers=3})')
    held = status()['position']
    time.sleep(.06)
    assert abs(status()['position'] - held) < .01
    lua('hl.plugin.hyprspaceflow.raw_finish({time_ms=1200,cancelled=false})')
    wait_idle()
    assert not status()['awaiting_gesture'], 'unclassified/vertical contact stranded a pending catch'
    clean_pair()
    focused_on_active()
    passed('contact ending without a horizontal gesture settles and releases the held state')

    # Block only the disposable compositor for 120 ms. This is below the
    # suspend guard and should be fully integrated on the next rendered frame.
    # With the former 50 ms clamp the first velocity ratio is approximately
    # exp(-9*.05)=.638 rather than exp(-9*.12)=.340.
    begin_at_left()
    release = json.loads(ctl('repl', '''
local p=hl.plugin.hyprspaceflow
for i=1,12 do p.update({time_ms=1010+i*8,delta={x=-12}}) end
p.finish({time_ms=1106,cancelled=false})
assert(p.status().phase=="coasting")
local release={velocity=p.status().velocity,position=p.status().position}
os.execute("sleep 0.12")
return __flow_review_json(release)
'''))
    release_velocity = release['velocity']
    release_position = release['position']
    after = wait_for(lambda: (s if (s := status())['position'] > release_position + 1 else None),
                     'no frame after the 120 ms stall', timeout=.1)
    assert after['phase'] == 'coasting', 'stall fixture coast ended too early to measure'
    assert abs(after['velocity']) / abs(release_velocity) < .5, 'runtime dropped elapsed release time after a frame stall'
    lua('hl.plugin.hyprspaceflow.cancel()')
    clean_pair()
    passed('120 ms render stall integrates elapsed coasting time rather than a 50 ms slice')

    if args.skip_lifecycle:
        print('SKIP unload/reload lifecycle (--skip-lifecycle)', flush=True)
    else:
        begin_at_left()
        move_transition(.7)
        ctl('reload')
        install_helpers()
        clean_pair()
        focused_on_active()
        assert not ctl('configerrors'), 'config errors after active-swipe reload'
        passed('configuration reload releases transforms, scanout ownership, and hidden keyboard focus')

        begin_at_left()
        move_transition(.7)
        ctl('plugin', 'unload', str(args.plugin))
        unloaded = True
        loaded = data('plugin', 'list')
        assert all(p['name'] != 'hyprspaceflow' for p in loaded), 'library remained loaded'
        focused_on_active()
        ctl('plugin', 'load', str(args.plugin))
        unloaded = False
        install_helpers()
        clean_pair()
        assert not ctl('configerrors'), 'config errors after library reload'
        passed('library unload/reload during a swipe leaves normal compositor state')

    print('All independent experience integration checks passed.', flush=True)
finally:
    # This file is only for a disposable session, but close only owned windows.
    if unloaded:
        ctl('plugin', 'load', str(args.plugin))
        install_helpers()
    try:
        lua('if hl.plugin.hyprspaceflow then hl.plugin.hyprspaceflow.cancel() end')
        if fullscreen:
            focus_window(first)
            set_fullscreen(first, False)
        if hidden_fullscreen:
            set_fullscreen(hidden_fullscreen, False)
        if moved_workspace_origin:
            lua(f'hl.dispatch(hl.dsp.workspace.move({{workspace="2",monitor={json.dumps(moved_workspace_origin)}}}))')
        for address in reversed(created):
            close_fixture(address)
    except (AssertionError, subprocess.SubprocessError):
        # Preserve the original assertion when cleanup is no longer possible;
        # terminating the disposable compositor removes remaining fixtures.
        pass
