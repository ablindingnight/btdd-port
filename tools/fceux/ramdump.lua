-- ramdump.lua (FCEUX 2.6): play the loaded movie and dump the 2 KB CPU RAM after frames, for
-- tools/ramdiff.py to compare with btdd_headless --dump-ram.
--
-- Environment (absolute paths; FCEUX's working directory is its own folder):
--   BTDD_RAMDUMP  output file (put it in ..\Gamefiles\Movies\..., never in the repo)
--   BTDD_FRAMES   last frame to dump (default: movie length)
--   BTDD_EVERY    dump every N frames (default 1); the last frame is always dumped
--   BTDD_SPEED    emu.speedmode value (default "maximum"; frame skipping does not change emulation in
--                 the Windows build, it only skips drawing)
-- Record format: uint32 little-endian frame number (emu.framecount() after emu.frameadvance()), then
-- 2048 bytes of RAM ($0000-$07FF).
--
-- Run (example):
--   fceux64.exe -cfg <cfg with newppu 0> -playmovie <movie.fm2> -lua <this file> <rom.nes>

local path = assert(os.getenv("BTDD_RAMDUMP"), "set BTDD_RAMDUMP")
local every = tonumber(os.getenv("BTDD_EVERY") or "1")
local last = tonumber(os.getenv("BTDD_FRAMES") or "0")
if last == 0 then last = movie.length() end

local out = assert(io.open(path, "wb"))
emu.speedmode(os.getenv("BTDD_SPEED") or "maximum")

local function u32(n)
  local b0 = n % 256; n = math.floor(n / 256)
  local b1 = n % 256; n = math.floor(n / 256)
  local b2 = n % 256; n = math.floor(n / 256)
  return string.char(b0, b1, b2, n % 256)
end

while true do
  emu.frameadvance()
  local fc = emu.framecount()
  if fc % every == 0 or fc >= last then
    out:write(u32(fc))
    out:write(memory.readbyterange(0, 2048))
  end
  if fc >= last then break end
end
out:close()
-- marker so the caller can tell a finished dump from a killed one
local done = assert(io.open(path .. ".done", "w"))
done:write("frames " .. last .. " movie " .. movie.mode() .. "\n")
done:close()
emu.exit()
