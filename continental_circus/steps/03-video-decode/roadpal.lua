local function press(t,m,v) local p=manager.machine.ioport.ports[t]; if p then local f=p:field(m); if f then f:set_value(v) end end end
local function gas() local p=manager.machine.ioport.ports[":GAS"]; if p then for nm,f in pairs(p.fields) do f:set_value(0xff) end end; press(":IN0",0x00e0,0xe0) end
local main=manager.machine.devices[":maincpu"]; local sp=main.spaces["program"]
local pb=-1
sp:install_write_tap(0x090000,0x090001,"rpb",function(off,data,mask) pb=(data>>6)&3 end)
-- find a palette device
local pal=nil
for tag,dev in pairs(manager.machine.devices) do
  if dev.palette then pal=dev.palette; print("palette via "..tag) break end
end
local n=0
emu.register_frame_done(function() n=n+1
  if n==120 then press(":IN0",0x08,1) end
  if n==130 then press(":IN0",0x08,0) end
  if n==200 then press(":IN1",0x08,1) end
  if n==210 then press(":IN1",0x08,0) end
  if n>=230 then gas() end
  if n==700 then
    print("MAME race road_palbank="..pb)
    if pal then for _,i in ipairs({3472,3473,3474,3475,3476}) do
      local c=pal:pen_color(i); print(string.format("pal[%d]=0x%08x",i,c)) end end
    manager.machine:exit()
  end end)
