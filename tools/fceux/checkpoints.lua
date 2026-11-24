-- checkpoints.lua (FCEUX 2.6): save a state every time the level ($000D) or section ($0015) changes, for
-- human play-testing from any point of a route. Two ways to drive the game:
--   * with a movie (-playmovie): the movie's input, until BTDD_FRAMES or the movie's end;
--   * without a movie: the menu script in BTDD_MENU, e.g. "300:title,330:S,340:-,350:S,360:-,370:T,380:-"
--     = at frame N press buttons on both pads (letters A B S=Select T=Start U D L R, "-" = release) or save a state named
--     after the colon word ("title"); then keep going with no input until BTDD_FRAMES.
-- Environment (absolute paths; FCEUX's working directory is its own folder):
--   BTDD_CKPT_DIR     output folder (under ..\Gamefiles, never in the repo: save states are ROM-derived)
--   BTDD_CKPT_PREFIX  file name prefix, e.g. "2P"
--   BTDD_FRAMES       last frame (default: movie length)
--   BTDD_MENU         see above (no-movie runs only)
--   BTDD_LOAD         start from this state file instead (BTDD_POKE "addr=val,...", once; BTDD_RUN frames)
-- Output: PREFIX_fNNNNNN_LXX_SYY.fcs (uncompressed, still with the movie chunks when a movie was playing:
-- tools/fcs_checkpoints.py strips them and renames) and PREFIX.tsv: frame, level, section, mode $16,
-- lives $11/$12, hp of entity slots 0/1 ($051B,x), file.
-- Run: fceux64.exe [-readonly 1 -playmovie MOVIE] -lua checkpoints.lua ROM   (from bash, so the env reaches us)

local dir = assert(os.getenv("BTDD_CKPT_DIR"), "set BTDD_CKPT_DIR")
local prefix = os.getenv("BTDD_CKPT_PREFIX") or "ckpt"
local last = tonumber(os.getenv("BTDD_FRAMES") or "0")
if last == 0 and movie.active() then last = movie.length() end
if last == 0 then last = 3000 end

local menu = {}
for f, what in string.gmatch(os.getenv("BTDD_MENU") or "", "(%d+):([^,]+)") do
  menu[tonumber(f)] = what
end

local log = assert(io.open(dir .. "\\" .. prefix .. ".tsv", "w"))
log:write("frame\tlevel\tsection\tmode\tlives1\tlives2\thp0\thp1\tfile\n")
emu.speedmode("maximum")

local function save(name)
  local fc = emu.framecount()
  local file = string.format("%s\\%s_f%06d_%s.fcs", dir, prefix, fc, name)
  local st = savestate.create(file)
  savestate.save(st)
  savestate.persist(st)
  local r = memory.readbyte
  log:write(string.format("%d\t%02X\t%02X\t%02X\t%d\t%d\t%d\t%d\t%s\n", fc, r(0x0D), r(0x15), r(0x16), r(0x11),
    r(0x12), r(0x051B), r(0x051C), file))
  log:flush()
end

-- optional start from a state file, with one-time RAM pokes ("16=00,12=FF"), e.g. to switch a 2P state to 1P
local load = os.getenv("BTDD_LOAD")
if load then
  savestate.load(savestate.create(load))
  for a, v in string.gmatch(os.getenv("BTDD_POKE") or "", "(%x+)=(%x+)") do
    memory.writebyte(tonumber(a, 16), tonumber(v, 16))
  end
  last = emu.framecount() + tonumber(os.getenv("BTDD_RUN") or "600")
end

local held = {}
local keys = { A = "A", B = "B", S = "select", T = "start", U = "up", D = "down", L = "left", R = "right" }
local pl, ps = -1, -1
while emu.framecount() < last do
  local what = menu[emu.framecount()]
  if what then
    if what == "-" then held = {}
    elseif string.match(what, "^[ABSTUDLR]+$") then
      held = {}
      for c in string.gmatch(what, ".") do held[keys[c]] = true end
    else save(what) end
  end
  if not movie.active() then joypad.set(1, held); joypad.set(2, held) end
  emu.frameadvance()
  local l, s = memory.readbyte(0x0D), memory.readbyte(0x15)
  if l ~= pl or s ~= ps then
    if pl >= 0 then save(string.format("L%02X_S%02X", l, s)) end
    pl, ps = l, s
  end
end
gui.savescreenshotas(dir .. "\\" .. prefix .. "_last.png")
emu.frameadvance()
log:close()
local done = assert(io.open(dir .. "\\" .. prefix .. ".done", "w"))
done:write("frames " .. emu.framecount() .. "\n")
done:close()
emu.exit()
