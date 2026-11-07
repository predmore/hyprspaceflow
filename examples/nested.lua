-- Run with: HYPRLAND_NO_SD_VARS=1 HYPRLAND_NO_SD_NOTIFY=1 Hyprland -c /absolute/path/to/examples/nested.lua
local source = debug.getinfo(1, "S").source:gsub("^@", "")
local root = assert(source:match("^(.*)/examples/nested.lua$"), "Use an absolute config path")
hl.monitor({ output = "", mode = "1000x700@60", position = "auto", scale = 1 })
hl.config({
  general = { layout = "scrolling", gaps_in = 8, gaps_out = 8 },
  scrolling = { column_width = 0.5, fullscreen_on_one_column = false, focus_fit_method = 0 },
  input = { follow_mouse = 0 },
  misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
  cursor = { no_hardware_cursors = true },
})
hl.animation({ leaf = "global", enabled = false })
for i = 1, 4 do hl.workspace_rule({ workspace = tostring(i), persistent = true }) end
hl.plugin.load(root .. "/hyprspaceflow.so")
local flow = dofile(root .. "/examples/hyprspaceflow.lua")
if flow.available() then flow.bind(3) end
hl.on("hyprland.start", function()
  for i = 1, 5 do
    hl.exec_cmd("foot --app-id=hyprspaceflow-test --title=flow-" .. i)
  end
end)
hl.bind("F12", function() hl.dispatch(hl.dsp.exit()) end)
