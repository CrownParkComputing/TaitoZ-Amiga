-- sound_race.lua -- log YM2610 key-ons + SYT commands during start + gas.
-- Run: mame contcirc -rompath ~/Downloads -autoboot_script sound_race.lua -video none -sound none -nothrottle
local mac = manager.machine
local aud = mac.devices[":audiocpu"]
local sub = mac.devices[":cpub"] or mac.devices[":sub"]

local ymw, keyon, adpcma, cmds = 0, 0, 0, 0
local la, lb = 0, 0

if aud then
  local ap = aud.spaces["program"]
  ap:install_write_tap(0xe000, 0xe003, "ym", function(off, data, mask)
    ymw = ymw + 1
    if off == 0xe000 then la = data
    elseif off == 0xe001 then if la == 0x28 and (data & 0xf0) ~= 0 then keyon = keyon + 1 end
    elseif off == 0xe002 then lb = data
    elseif off == 0xe003 then if lb == 0x00 and data < 0x80 and (data & 0x3f) ~= 0 then adpcma = adpcma + 1 end
    end
  end)
end

if sub then
  sub.spaces["program"]:install_write_tap(0x200000, 0x200003, "syt", function(off, data, mask)
    if off == 0x200003 or off == 0x200002 then cmds = cmds + 1 end
  end)
end

local function press(tag, mask, value)
  local p = mac.ioport.ports[tag]
  if p then local f = p:field(mask); if f then f:set_value(value) end end
end

local function gas()
  local p = mac.ioport.ports[":GAS"]
  if p then for _, f in pairs(p.fields) do f:set_value(7) end end
end

local n = 0
emu.register_frame_done(function()
  n = n + 1
  if n == 120 then press(":IN0", 0x08, 1) end
  if n == 130 then press(":IN0", 0x08, 0) end
  if n == 200 then press(":IN1", 0x08, 1) end
  if n == 210 then press(":IN1", 0x08, 0) end
  if n >= 230 then gas() end
  if n == 300 or n == 700 or n == 1400 then
    print(string.format("MAME-RACE @%d: YMwrites=%d FMkeyon=%d ADPCMAkey=%d SYTcmds=%d", n, ymw, keyon, adpcma, cmds))
  end
  if n == 1410 then mac:exit() end
end)
