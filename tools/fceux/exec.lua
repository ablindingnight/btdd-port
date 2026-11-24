-- exec.lua (FCEUX 2.6): log when instructions at chosen addresses start (cycle count, registers) and the
-- cycle count at each frame end; counterpart of `btdd_headless --exec`. Debugging aid for M1 timing work.
--   BTDD_EXEC    hex CPU addresses, e.g. "8291,82AC"
--   BTDD_FRAMES  frames to run (default 10)
--   BTDD_OUT     output text file (absolute path, outside the repo)
local out = assert(io.open(assert(os.getenv("BTDD_OUT"), "set BTDD_OUT"), "w"))
local frames = tonumber(os.getenv("BTDD_FRAMES") or "10")
emu.speedmode(os.getenv("BTDD_SPEED") or "maximum")

for spec in string.gmatch(os.getenv("BTDD_EXEC") or "", "[^,]+") do
  local a = tonumber(spec, 16)
  memory.registerexec(a, function()
    out:write(string.format("x frame %d cyc %d pc $%04X  A%02X X%02X Y%02X P%02X\n", emu.framecount(),
      debugger.getcyclescount(), a, memory.getregister("a"), memory.getregister("x"),
      memory.getregister("y"), memory.getregister("p")))
  end)
end

for i = 1, frames do
  emu.frameadvance()
  out:write(string.format("f %d cyc %d\n", emu.framecount(), debugger.getcyclescount()))
end
out:close()
emu.exit()
