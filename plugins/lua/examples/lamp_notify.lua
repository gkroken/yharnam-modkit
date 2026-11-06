-- lamp_notify.lua: a banner when you light a lamp, naming the area, and how many of its lamps
-- you have lit. It only reads the game, so it runs online too.
--
-- The lamps' lit flags are each lamp's event flag base + 10. The bases come from the maps' event
-- scripts (event 7000, one call per lamp), as bbhost lists them (droogie/bbhost
-- src/engine/world_chr.cpp, GPL-3.0). The Chalice Dungeons have no lamps there.
--
-- plugins.ini, in the script's own section (optional):
--   [lamp_notify]
--   seconds = 5   ; how long the banner stays

local seconds = tonumber(bb.config("seconds") or "") or 5

local areas = {
  {"Abandoned Old Workshop", {12117800}},
  {"Hemwick Charnel Lane", {12207800, 12207820}},
  {"Old Yharnam", {12307800, 12307820, 12307840}},
  {"Cathedral Ward", {12407800, 12407820}},
  {"Central Yharnam", {12417800, 12417820, 12417840, 12417860}},
  {"Upper Cathedral Ward", {12427800, 12427820, 12427840}},
  {"Forsaken Cainhurst Castle", {12507800, 12507820, 12507840}},
  {"Nightmare of Mensis", {12607800, 12607820, 12607840, 12607860}},
  {"Forbidden Woods", {12707800, 12707820}},
  {"Yahar'gul", {12807800, 12807820, 12807840, 12807860}},
  {"Byrgenwerth", {13207800, 13207820, 13207840, 13207860}},
  {"Nightmare Frontier", {13307800, 13307820}},
  {"Hunter's Nightmare", {13407800, 13407820, 13407840, 13407860}},
  {"Research Hall", {13507800, 13507820, 13507840}},
  {"Fishing Hamlet", {13607800, 13607820, 13607840}},
}

-- Lit flag -> its area.
local area_of = {}
for _, area in ipairs(areas) do
  for _, base in ipairs(area[2]) do area_of[base + 10] = area end
end

local function lit_in(area, just_lit)
  local lit = 0
  for _, base in ipairs(area[2]) do
    if base + 10 == just_lit or bb.flag.get(base + 10) then lit = lit + 1 end
  end
  return lit
end

-- Registered while the script loads, as flag watching requires.
bb.flag.on(function(id, on)
  local area = area_of[id]
  if not area or not on then return end
  local lit, total = lit_in(area, id), #area[2]
  bb.message(("Lamp lit: %s (%d of %d)"):format(area[1], lit, total), seconds)
  bb.log(("lamp flag %d lit: %s, %d of %d"):format(id, area[1], lit, total))
end)

bb.log(("watching %d lamps"):format((function() local n = 0 for _ in pairs(area_of) do n = n + 1 end return n end)()))
