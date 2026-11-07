#!/usr/bin/env python3
"""Replay gestures in an explicitly selected disposable Hyprland session.

Requires four persistent workspaces (1..4), and at least three tiled windows
on workspace 1. Does not target the caller's main compositor instance.
"""
import argparse
import json
import os
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--instance', required=True)
args = parser.parse_args()
if args.instance == os.environ.get('HYPRLAND_INSTANCE_SIGNATURE'):
    parser.error('Use a disposable nested instance, not your current desktop.')

def ctl(*parts):
    result = subprocess.run(['hyprctl', '-i', args.instance, *parts], text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout.strip()

def lua(code):
    result = ctl('eval', code)
    if result != 'ok':
        raise AssertionError(result)

def case(name, code):
    lua(code)
    print('PASS', name, flush=True)

def wait_idle():
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if ctl('repl', 'return hl.plugin.hyprspaceflow.status().phase') == 'idle':
            return
        time.sleep(.03)
    raise AssertionError('release did not settle')

lua('''
function flow_test_clean(nativeSwitch)
  local s = hl.plugin.hyprspaceflow.status()
  assert(not s.active, "motion did not finish")
  assert(not s.scanout_blocked, "direct scanout override leaked")
  for _, p in ipairs(s.pages) do
    if nativeSwitch then
      flow_test_baseline[p.id] = p.render_x
    else
      assert(math.abs(p.render_x) < 0.1 or math.abs(p.render_x-flow_test_baseline[p.id]) < .1, "workspace transform leaked")
    end
    assert(not p.forced, "forced rendering leaked")
  end
end
function flow_test_page(id)
  for _, p in ipairs(hl.plugin.hyprspaceflow.status().pages) do
    if p.id == id then return p end
  end
  error("missing workspace")
end
hl.plugin.hyprspaceflow.cancel()
hl.config({plugin={hyprspaceflow={sensitivity=1.0,acceleration=0.0,inertia=0.0,snap_columns=false}}})
hl.dispatch(hl.dsp.focus({workspace="1"}))
flow_test_baseline = {}
for _, p in ipairs(hl.plugin.hyprspaceflow.status().pages) do flow_test_baseline[p.id] = p.render_x end
''')
clients = json.loads(ctl('-j', 'clients'))
assert sum(w['workspace']['id'] == 1 and not w['floating'] for w in clients) >= 3, 'Need 3+ fixture windows on workspace 1'

case('direct tracking, edge handoff and complete reverse', '''
local p = hl.plugin.hyprspaceflow
assert(p.start({time_ms=1000}))
local s = p.status()
assert(s.scanout_blocked, "animated geometry must inhibit direct scanout")
p.update({time_ms=1010,delta={x=s.position}})
assert(math.abs(p.status().position) < .01)
local before = flow_test_page(1).tape
p.update({time_ms=1020,delta={x=-80}})
assert(math.abs(flow_test_page(1).tape - before - 80) < .1, "tracking lag")
local page = flow_test_page(1)
local goal = page.high - page.low + s.viewport * .25
p.update({time_ms=1030,delta={x=-(goal - p.status().position)}})
assert(math.abs(flow_test_page(1).render_x + s.viewport * .25) < .1)
assert(math.abs(flow_test_page(2).render_x - s.viewport * .75) < .1)
assert(math.abs(flow_test_page(1).tape - page.high) < .1)
local last = flow_test_page(4)
p.update({time_ms=1040,delta={x=-(last.start - p.status().position)}})
assert(hl.get_active_workspace().id == 4, "could not cross three boundaries in one gesture")
p.update({time_ms=1050,delta={x=p.status().position}})
assert(hl.get_active_workspace().id == 1, "reversal lost remaining distance")
p.cancel()
flow_test_clean()
''')

case('explicit cancellation releases rendering ownership', '''
local p = hl.plugin.hyprspaceflow
assert(p.start({time_ms=2000}))
local last = flow_test_page(4)
p.update({time_ms=2010,delta={x=-(last.start-p.status().position)}})
assert(hl.get_active_workspace().id == 4)
p.cancel()
assert(hl.get_active_workspace().id == 4)
flow_test_clean()
''')

case('inertia and immediate momentum catch', '''
local p = hl.plugin.hyprspaceflow
hl.config({plugin={hyprspaceflow={inertia=0.55}}})
assert(p.start({time_ms=3000}))
p.update({time_ms=3010,delta={x=p.status().position}})
for i=1,12 do p.update({time_ms=3010+i*8,delta={x=-20}}) end
flow_test_release_position = p.status().position
p.finish({time_ms=3106,cancelled=false})
assert(p.status().phase == "coasting")
''')
time.sleep(.08)
case('release moves, catch keeps position', '''
local p = hl.plugin.hyprspaceflow
assert(p.status().position > flow_test_release_position, "inertia not rendered")
local caught = p.status().position
assert(p.start({time_ms=3200}))
assert(math.abs(p.status().position-caught) < .01, "catch jumped")
assert(p.status().velocity == 0)
p.cancel()
flow_test_clean()
''')

case('partial workspace release begins settling', '''
local p = hl.plugin.hyprspaceflow
hl.config({plugin={hyprspaceflow={inertia=0.0}}})
assert(p.start({time_ms=4000}))
local goal = flow_test_page(2).start - p.status().viewport * .2
p.update({time_ms=4010,delta={x=-(goal-p.status().position)}})
p.finish({time_ms=4010,cancelled=false})
''')
time.sleep(1)
case('settling converges and clears transforms', '''
flow_test_clean()
assert(hl.get_active_workspace().id == 2)
''')

case('external workspace switch cancels ownership', '''
local p = hl.plugin.hyprspaceflow
assert(p.start({time_ms=5000}))
p.update({time_ms=5010,delta={x=-100}})
hl.dispatch(hl.dsp.focus({workspace="1"}))
flow_test_clean(true)
assert(hl.get_active_workspace().id == 1)
''')

case('invalid input and timestamp wrap are safe', '''
local p = hl.plugin.hyprspaceflow
assert(p.start({time_ms=4294967290}))
local x = p.status().position
p.update({time_ms=2,delta={x=0/0}})
assert(p.status().position == x)
p.update({time_ms=2,delta={x=-10}})
assert(p.status().position == x+10)
p.cancel()
flow_test_clean()
''')

case('reversed scrolling tape tracks physical direction', '''
local p = hl.plugin.hyprspaceflow
hl.dispatch(hl.dsp.focus({workspace="1"}))
hl.config({scrolling={direction="left"}})
hl.dispatch(hl.dsp.layout("move +1"))
assert(p.start({time_ms=7000}))
local s=p.status()
p.update({time_ms=7010,delta={x=s.position}})
local low=flow_test_page(1).tape
p.update({time_ms=7020,delta={x=-80}})
assert(math.abs(flow_test_page(1).tape-low+80)<.1, "reversed physical direction is incorrect")
p.cancel()
hl.config({scrolling={direction="right"}})
hl.dispatch(hl.dsp.layout("move +1"))
flow_test_clean()
''')

case('mixed workspace layout remains a full workspace page', '''
local p = hl.plugin.hyprspaceflow
hl.workspace_rule({workspace="2",layout="dwindle"})
assert(p.start({time_ms=8000}))
assert(flow_test_page(2).high == flow_test_page(2).low)
local target = flow_test_page(3).start
p.update({time_ms=8010,delta={x=-(target-p.status().position)}})
assert(hl.get_active_workspace().id == 3, "mixed layout blocked continuous traversal")
p.cancel()
hl.workspace_rule({workspace="2",layout="scrolling"})
flow_test_clean()
''')

# A physical cancelled end used to restore the first window after the user
# had already traversed the whole tape. Exercise both native end variants,
# both sides of the midpoint, and a reversal from the incoming workspace.
fixture_windows = [w for w in json.loads(ctl('-j', 'clients'))
                   if w['workspace']['id'] == 1 and not w['floating']]
first = min(fixture_windows, key=lambda w: w['at'][0])['address']
for cancelled in (False, True):
    for fraction in (.49, .51):
        for reverse_direction in (False, True):
            label = f'edge release cancelled={cancelled} progress={fraction} reverse={reverse_direction}'
            case(label + ' begins', f'''
local p=hl.plugin.hyprspaceflow
p.cancel()
hl.config({{input={{follow_mouse=1}},scrolling={{focus_fit_method=1}},plugin={{hyprspaceflow={{sensitivity=1.0,acceleration=0.0,inertia=0.55,snap_columns=true}}}}}})
hl.dispatch(hl.dsp.focus({{window="address:{first}"}}))
if {str(reverse_direction).lower()} then hl.dispatch(hl.dsp.focus({{workspace="2"}})) end
assert(p.start({{time_ms=10000}}))
local page=flow_test_page(1)
flow_test_edge=page.high
local goal=page.start+page.high-page.low+p.status().viewport*{fraction}
p.update({{time_ms=10010,delta={{x=-(goal-p.status().position)}}}})
p.finish({{time_ms=10200,cancelled={str(cancelled).lower()}}})
''')
            wait_idle()
            expected_workspace = 1 if fraction < .5 else 2
            case(label + ' preserves boundary', f'''
flow_test_clean(true)
assert(math.abs(flow_test_page(1).render_x)<.1 and math.abs(flow_test_page(2).render_x)<.1, "boundary transforms leaked")
assert(hl.get_active_workspace().id == {expected_workspace}, "settled to wrong workspace")
assert(math.abs(flow_test_page(1).tape-flow_test_edge)<.1, "returned to start of outgoing workspace")
''')

print('All compositor integration checks passed.')
