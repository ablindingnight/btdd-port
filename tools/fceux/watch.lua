-- watch.lua (FCEUX 2.6): log CPU writes to chosen RAM addresses and the cycle count at every frame end,
-- to line up against `btdd_headless --watch`. Debugging aid for M1 timing work.
--   BTDD_WATCH   hex addresses, e.g. "05,1F,0300-030F"
--   BTDD_FRAMES  frames to run (default 10)
--   BTDD_OUT     output text file (absolute path, outside the repo)
local out = assert(io.open(assert(os.getenv("BTDD_OUT"), "set BTDD_OUT"), "w"))
local frames = tonumber(os.getenv("BTDD_FRAMES") or "10")
emu.speedmode(os.getenv("BTDD_SPEED") or "maximum")

local function hook(addr)
  local v = memory.readbyte(addr)
  out:write(string.format("w frame %d cyc %d pc $%04X  $%04X = %02X  A%02X X%02X Y%02X\n",
    emu.framecount() , debugger.getcyclescount(), memory.getregister("pc"), addr, v,
    memory.getregister("a"), memory.getregister("x"), memory.getregister("y")))
end

for spec in string.gmatch(os.getenv("BTDD_WATCH") or "", "[^,]+") do
  local lo, hi = string.match(spec, "^(%x+)%-(%x+)$")
  if not lo then lo = spec; hi = spec end
  for a = tonumber(lo, 16), tonumber(hi, 16) do
    memory.registerwrite(a, function() hook(a) end)
  end
end

for i = 1, frames do
  emu.frameadvance()
  out:write(string.format("f %d cyc %d\n", emu.framecount(), debugger.getcyclescount()))
end
out:close()
emu.exit()
