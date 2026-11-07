-- This module declares the plugin on EVERY config pass. Loading is deferred
-- until the first pass completes; options become available on the next pass.
local M = {}

local function plugin()
  return hl.plugin and hl.plugin.hyprspaceflow
end

-- Resolve the namespace on every invocation. Never retain a plugin C function
-- in a closure: Hyprland removes the namespace when the library is unloaded.
function M.action()
  return {
    start = function(event)
      local p = plugin()
      if p and p.start then p.start(event) end
    end,
    update = function(event)
      local p = plugin()
      if p and p.update then p.update(event) end
    end,
    finish = function(event)
      local p = plugin()
      if p and p.finish then p.finish(event) end
    end,
  }
end

function M.available()
  for _, p in ipairs(hl.get_loaded_plugins()) do
    if p.name == "hyprspaceflow" then return true end
  end
  return false
end

-- Pass replace=true only when this exact horizontal binding already exists.
function M.bind(fingers, replace)
  if replace then
    hl.gesture({ fingers = fingers or 3, direction = "horizontal", action = "unset" })
  end
  hl.gesture({ fingers = fingers or 3, direction = "horizontal", action = M.action() })
end

return M
