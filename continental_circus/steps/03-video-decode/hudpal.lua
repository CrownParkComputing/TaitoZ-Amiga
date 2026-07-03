local function press(t,m,v) local p=manager.machine.ioport.ports[t]; if p then local f=p:field(m); if f then f:set_value(v) end end end
local function gas() local p=manager.machine.ioport.ports[":GAS"]; if p then for nm,f in pairs(p.fields) do f:set_value(0xff) end end; press(":IN0",0x00e0,0xe0) end
local pal; for _,p in pairs(manager.machine.palettes) do pal=p break end
local n=0
emu.register_frame_done(function() n=n+1
  if n==120 then press(":IN0",0x08,1) end
  if n==130 then press(":IN0",0x08,0) end
  if n==200 then press(":IN1",0x08,1) end
  if n==210 then press(":IN1",0x08,0) end
  if n>=230 then gas() end
  if n==700 then
    for _,i in ipairs({49,65,66,305,306}) do local c=pal:pen_color(i)
      print(string.format("MAME pal[%d]=0x%06x R=%d G=%d B=%d",i,c,(c>>16)&0xff,(c>>8)&0xff,c&0xff)) end
    manager.machine:exit() end end)
