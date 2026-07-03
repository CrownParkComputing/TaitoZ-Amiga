for tag,port in pairs(manager.machine.ioport.ports) do
  for name,field in pairs(port.fields) do
    print(string.format("%-14s | %-22s | mask=%04x type=%s", tag, name, field.mask, tostring(field.type)))
  end
end
manager.machine:exit()
