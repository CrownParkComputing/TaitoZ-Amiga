local n=0
emu.register_frame_done(function() n=n+1
  if n==130 then manager.machine.video:snapshot() end
  if n==135 then manager.machine:exit() end end)
