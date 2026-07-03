local main = manager.machine.devices[":maincpu"]
local sp = main.spaces["program"]
local last=-1
sp:install_write_tap(0x090000,0x090001,"outw",function(off,data,mask)
  local v=(data>>6)&3
  if v~=last then last=v; print(string.format("FRAMEMARK road_palbank=%d",v)) end
end)
local n=0
emu.register_frame_done(function() n=n+1
  if n%200==0 then print(string.format("frame %d: road_palbank=%d",n,last)) end
  if n==1600 then manager.machine:exit() end end)
