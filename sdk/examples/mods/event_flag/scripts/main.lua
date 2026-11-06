-- event_flag: a banner when this mod's event (edits/event/m24_01_00_00.emevd.edit.toml) turns
-- flag 12401900 on, two seconds after Central Yharnam loads.
local FLAG = 12401900

bb.flag.on(function(id, on)
  if id == FLAG and on then
    bb.log("the mod's event turned flag " .. FLAG .. " on")
    bb.message("event_flag: the mod's event ran")
  end
end)
