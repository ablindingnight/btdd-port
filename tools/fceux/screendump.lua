-- screendump.lua (FCEUX 2.6): at chosen frames, save FCEUX's screenshot and the NES palette index of every
-- pixel (256x240 bytes, row-major), for comparison with btdd_shot (tools/shotdiff.py).
--   BTDD_SHOTS   comma list of frame numbers (emu.framecount() after emu.frameadvance())
--   BTDD_OUT     output folder (absolute path, outside the repo)
local out = assert(os.getenv("BTDD_OUT"), "set BTDD_OUT")
local want, last = {}, 0
for n in string.gmatch(os.getenv("BTDD_SHOTS") or "", "%d+") do
  n = tonumber(n); want[n] = true; if n > last then last = n end
end
emu.speedmode(os.getenv("BTDD_SPEED") or "maximum")
while emu.framecount() < last do
  emu.frameadvance()
  local fc = emu.framecount()
  if want[fc] then
    local f = assert(io.open(string.format("%s/fceux_idx_%d.bin", out, fc), "wb"))
    for y = 0, 239 do
      local row = {}
      for x = 0, 255 do
        local _, _, _, p = emu.getscreenpixel(x, y, true)
        row[x + 1] = string.char(p)
      end
      f:write(table.concat(row))
    end
    f:close()
    gui.savescreenshotas(string.format("%s/fceux_shot_%d.png", out, fc))
  end
end
emu.exit()
