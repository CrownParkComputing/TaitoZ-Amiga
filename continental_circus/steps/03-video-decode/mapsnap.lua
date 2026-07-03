local function press(t,m,v) local p=manager.machine.ioport.ports[t]; if p then local f=p:field(m); if f then f:set_value(v) end end end
local n=0
emu.register_frame_done(function() n=n+1
  if n==120 then press(":IN0",0x08,1) end
  if n==130 then press(":IN0",0x08,0) end
  if n==200 then press(":IN1",0x08,1) end
  if n==210 then press(":IN1",0x08,0) end
  if n==203 or n==209 or n==215 or n==221 then manager.machine.video:snapshot() end
  if n==227 then manager.machine:exit() end end)
