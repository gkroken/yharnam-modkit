-- weapon_tweak.lua: the weapon_tweak example as a script. Changes one param field by name once
-- the game's tables exist (they load after scripts start).
--
--   [weapon_tweak]
--   table = EquipParamWeapon    ; any table
--   row = 2000000
--   field = attackBasePhysics   ; the game's field name (port/out/paramdefs.txt lists them)
--   value = 150

local tbl = bb.config("table") or "EquipParamWeapon"
local row = tonumber(bb.config("row") or "") or 2000000
local field = bb.config("field") or "attackBasePhysics"
local value = bb.config("value") or "150"

local done = false
bb.on_frame(function()
  if done then return end
  local before = bb.param.get(tbl, row, field)
  if before == nil then return end  -- not loaded yet (or no such row or field: see the log at the end)
  done = true
  local ok = bb.param.set(tbl, row, field, value)
  bb.log(tbl, row, field, before, "->", bb.param.get(tbl, row, field), ok and "" or "(refused)")
end)
