-- sound_log.lua -- log YM2610 key-ons + SYT commands during contcirc attract.
-- mame contcirc -rompath ~/Downloads -autoboot_script sound_log.lua -video none -seconds_to_run 20 -nothrottle
local mac = manager.machine
local aud = mac.devices[":audiocpu"]
local sub = mac.devices[":cpub"]
if not sub then sub = mac.devices[":sub"] end

local ymw, keyon, adpcma, cmds = 0, 0, 0, 0
local la, lpb = 0, 0
local function hinib(d) return d - (d % 16) end   -- nonzero => some high-nibble bit set

if aud then
  local ap = aud.spaces["program"]
  ap:install_write_tap(0xe000, 0xe003, "ym", function(off, data, mask)
    ymw = ymw + 1
    if off == 0xe000 then la = data
    elseif off == 0xe001 then if la == 0x28 and hinib(data) ~= 0 then keyon = keyon + 1 end
    elseif off == 0xe002 then lpb = data
    elseif off == 0xe003 then if lpb == 0x00 and data < 0x80 and (data % 64) ~= 0 then adpcma = adpcma + 1 end
    end
  end)
end
if sub then
  local sp = sub.spaces["program"]
  sp:install_write_tap(0x200000, 0x200003, "syt", function(off, data, mask)
    if off == 0x200003 or off == 0x200002 then cmds = cmds + 1 end
  end)
end

local function press(tag, mask, v)
  local p = mac.ioport.ports[tag]
  if p then local f = p:field(mask); if f then f:set_value(v) end end
end
local frames = 0
emu.register_frame_done(function()
  frames = frames + 1
  if frames == 120 then press(":IN0", 0x08, 1) end      -- COIN1 down
  if frames == 130 then press(":IN0", 0x08, 0) end      -- COIN1 up
  if frames == 200 then press(":IN1", 0x08, 1) end      -- START1 down
  if frames == 210 then press(":IN1", 0x08, 0) end      -- START1 up
  if frames == 300 or frames == 700 or frames == 1400 then
    print(string.format("MAME-SOUND @%d: YMwrites=%d FMkeyon=%d ADPCMAkey=%d SYTcmds=%d",
      frames, ymw, keyon, adpcma, cmds))
  end
end)
