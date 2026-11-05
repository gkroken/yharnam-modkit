-- flag_buff.lua: the flag_buff example as a script. Each time a chosen event flag turns on,
-- the player gets a SpEffect, with a banner.
--
-- plugins.ini, in the script's own section (its name: "flag_buff", or "<mod>/flag_buff" in a mod):
--   [flag_buff]
--   flag = 11000000   ; required: the event flag to watch
--   effect = 2200     ; optional: the SpEffectParam row (default 2200, a 60-second consumable effect)

local flag = tonumber(bb.config("flag") or "")
local effect = tonumber(bb.config("effect") or "") or 2200

if not flag then
  bb.log("no flag chosen: set flag = <id> in this script's plugins.ini section")
  return
end

-- Registered while the script loads, as flag watching requires.
bb.flag.on(function(id, on)
  if id ~= flag or not on then return end
  local applied = bb.effect.apply(effect)
  bb.log("flag", id, "turned on: SpEffect", effect, "applied:", applied)
  if applied then bb.message(("Flag %d: a gift of SpEffect %d"):format(id, effect)) end
end)

bb.log("watching flag", flag, "for SpEffect", effect)
