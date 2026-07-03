local aud=manager.machine.devices[":audiocpu"]
local sub=manager.machine.devices[":cpub"]
local ymw,keyon,adpcma,cmds=0,0,0,0
local la,lpb=0,0
if aud then local ap=aud.spaces["program"]
  ap:install_write_tap(0xe000,0xe003,"ym",function(off,data,mask) ymw=ymw+1
    if off==0xe000 then la=data elseif off==0xe001 then if la==0x28 and (data-(data%16))~=0 then keyon=keyon+1 end
    elseif off==0xe002 then lpb=data elseif off==0xe003 then if lpb==0x00 and data<0x80 and (data%64)~=0 then adpcma=adpcma+1 end end end) end
if sub then sub.spaces["program"]:install_write_tap(0x200000,0x200003,"syt",function(off,data,mask) if off==0x200003 or off==0x200002 then cmds=cmds+1 end end) end
local n=0
emu.register_frame_done(function() n=n+1
  if n==2000 or n==4000 or n==6000 then print(string.format("MAME-ATTRACT @%d: YMw=%d FMkeyon=%d ADPCMAkey=%d SYTcmds=%d",n,ymw,keyon,adpcma,cmds)) end
  if n==6010 then manager.machine:exit() end end)
