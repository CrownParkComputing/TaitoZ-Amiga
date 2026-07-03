-- Dump contcirc authoritative VRAM/palette from MAME at frame DUMP_FRAME, then exit.
-- Run: mame contcirc -rompath ~/Downloads -autoboot_script contcirc_dump.lua -video soft -seconds_to_run 30
-- Files written to the CWD MAME was launched from (we cd into 03-video-decode).
local DUMP_FRAME = tonumber(os.getenv("DUMP_FRAME") or "900")
local n = 0
local road_palbank = 0

local function sp() return manager.machine.devices[":maincpu"].spaces["program"] end

-- tap contcirc_out_w (0x090001) to capture road palette bank (bits 6-7)
sp():install_write_tap(0x090000, 0x090001, "out_w", function(offset, data, mask)
  road_palbank = (data & 0xc0) >> 6
end)

local function dump_bytes(fn, base, len)
  local s = sp(); local f = io.open(fn, "wb")
  for i = 0, len-1 do f:write(string.char(s:read_u8(base+i))) end
  f:close()
end

-- (sprite/spritemap regions are assembled from raw ROMs by make_sprites.py)

local function dump_palette(fn)
  local s = sp(); local f = io.open(fn, "wb")
  for i = 0, 0xfff do
    s:write_u16(0x100000, i)               -- set PCR address latch
    local v = s:read_u16(0x100002)         -- read raw xRGB555 word
    f:write(string.char(v % 256, math.floor(v/256) % 256))  -- little-endian (matches host dump + renderer)
  end
  f:close()
end

emu.register_frame_done(function()
  n = n + 1
  if n == DUMP_FRAME then
    dump_bytes("oracle_scn_ram.bin",  0x200000, 0x10000)
    dump_bytes("oracle_scn_ctrl.bin", 0x220000, 0x10)
    dump_bytes("oracle_road_ram.bin", 0x300000, 0x2000)
    dump_bytes("oracle_spriteram.bin",0x400000, 0x700)
    dump_palette("oracle_palette.bin")
    local pf = io.open("oracle_road_palbank.txt", "w"); pf:write(tostring(road_palbank)); pf:close()
    print(string.format("contcirc dump done at frame %d, road_palbank=%d", n, road_palbank))
    manager.machine.video:snapshot()
    manager.machine:exit()
  end
end)
