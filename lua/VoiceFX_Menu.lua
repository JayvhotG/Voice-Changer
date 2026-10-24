-- VoiceFX menu for SA-MP Nezuko (MonetLoader / LuaJIT / mimgui ~ImGui 1.72)
-- Controls libvoicefx.so (v5) through FFI. Tap the floating "VFX" button to open the menu.
-- The engine saves/loads its own settings (/storage/emulated/0/voicefx.cfg), so the effect keeps
-- working after restart even if this script is not loaded.
local imgui = require("mimgui")
local ffi   = require("ffi")
local bit   = require("bit")

pcall(ffi.cdef, [[
float vfx_get(int id);
void  vfx_set(int id, float v);
void  vfx_save(void);
int   vfx_stat(int id);
float vfx_statf(int id);
int   vfx_hook_retry(void);
]])

-- parameter ids (must match voicefx.cpp)
local P = { ENABLED = 0, PITCH = 1, FORMANT = 2, PRESERVE = 3, GAIN = 4, GATE = 5, MIX = 6, ROBOT = 7, ROBOT_HZ = 8 }

------------------------------------------------------------------ locate + load the engine
local lib, libErr, libPath
local function hasApi(l) return pcall(function() return l.vfx_get end) end
local function tryLoad(name)
  local ok, l = pcall(ffi.load, name)
  if ok and hasApi(l) then lib, libPath = l, name; return true end
  return false
end
local function loadEngine()
  local f = io.open("/proc/self/maps", "r")
  local seen, cands = {}, {}
  if f then
    for line in f:lines() do
      local p = line:match("(/[^%s]+%.so)")
      if p and not seen[p] and not p:find("^/system") and not p:find("^/apex") and not p:find("^/vendor") then
        seen[p] = true
        if p:find("voicefx", 1, true) then table.insert(cands, 1, p) else cands[#cands + 1] = p end
      end
    end
    f:close()
  end
  table.insert(cands, 1, "libvoicefx.so")
  for i = 1, math.min(#cands, 80) do if tryLoad(cands[i]) then return true end end
  libErr = "libvoicefx.so (v5) is not loaded. Put it in the AML mods folder, switch Modloader ON in the launcher, restart the game."
  return false
end
loadEngine()

------------------------------------------------------------------ state (allocated once)
local menuOpen = imgui.new.bool(false)
local bEnabled  = imgui.new.bool(false)
local bPreserve = imgui.new.bool(false)
local bRobot    = imgui.new.bool(false)
local fSemi     = imgui.new.float(0)
local fFormant  = imgui.new.float(1)
local fGain     = imgui.new.float(0)
local fGate     = imgui.new.float(-80)
local fMix      = imgui.new.float(1)
local fRobotHz  = imgui.new.float(80)
local dirtyAt   = nil
local LN2 = math.log(2)

local function pull()
  if not lib then return end
  bEnabled[0]  = lib.vfx_get(P.ENABLED) >= 0.5
  bPreserve[0] = lib.vfx_get(P.PRESERVE) >= 0.5
  bRobot[0]    = lib.vfx_get(P.ROBOT) >= 0.5
  fSemi[0]     = 12 * math.log(lib.vfx_get(P.PITCH)) / LN2
  fFormant[0]  = lib.vfx_get(P.FORMANT)
  fGain[0]     = lib.vfx_get(P.GAIN)
  fGate[0]     = lib.vfx_get(P.GATE)
  fMix[0]      = lib.vfx_get(P.MIX)
  fRobotHz[0]  = lib.vfx_get(P.ROBOT_HZ)
end
pull()

local function setp(id, v) if lib then lib.vfx_set(id, v) end; dirtyAt = os.clock() end

local presets = {
  { "Normal",   0,   1.00, false, false },
  { "Female",   4,   1.15, false, false },
  { "Deep",    -4,   0.85, false, false },
  { "Child",    7,   1.25, false, false },
  { "Monster", -8,   0.75, false, false },
  { "Chipmunk",10,   1.00, false, false },
  { "Robot",    0,   1.00, false, true  },
}
local function applyPreset(p)
  fSemi[0] = p[2]; fFormant[0] = p[3]; bPreserve[0] = p[4]; bRobot[0] = p[5]
  setp(P.PITCH, 2 ^ (p[2] / 12)); setp(P.FORMANT, p[3])
  setp(P.PRESERVE, p[4] and 1 or 0); setp(P.ROBOT, p[5] and 1 or 0)
  fMix[0] = 1; setp(P.MIX, 1)
  bEnabled[0] = true; setp(P.ENABLED, 1)
end

------------------------------------------------------------------ UI
local VEC0, VECW = imgui.ImVec2(0, 0), imgui.ImVec2(0, 0)
local VPAD = imgui.ImVec2(0, 0)
local COL_ON  = imgui.ImVec4(0.2, 0.85, 0.35, 1)
local COL_OFF = imgui.ImVec4(0.9, 0.35, 0.3, 1)
local COL_DIM = imgui.ImVec4(0.7, 0.7, 0.7, 1)
local BTN_FLAGS = bit.bor(imgui.WindowFlags.NoTitleBar, imgui.WindowFlags.NoResize,
                          imgui.WindowFlags.NoScrollbar, imgui.WindowFlags.AlwaysAutoResize)
local errLogged = false

local function guard(fn)
  return function(...)
    local ok, err = pcall(fn, ...)
    if not ok and not errLogged then errLogged = true; print("VoiceFX menu error: " .. tostring(err)) end
  end
end

-- floating launcher button
imgui.OnFrame(function() return true end, guard(function()
  local io = imgui.GetIO()
  local s = io.DisplaySize.y / 1080
  imgui.SetNextWindowPos(imgui.ImVec2(io.DisplaySize.x - 190 * s, 120 * s), imgui.Cond.FirstUseEver)
  imgui.PushStyleVar(imgui.StyleVar.WindowPadding, imgui.ImVec2(4 * s, 4 * s))
  imgui.Begin("##vfxbtn", nil, BTN_FLAGS)
  imgui.SetWindowFontScale(math.max(1, s * 2))
  local active = lib and lib.vfx_get(P.ENABLED) >= 0.5
  imgui.PushStyleColor(imgui.Col.Button, active and imgui.ImVec4(0.1, 0.5, 0.2, 0.9) or imgui.ImVec4(0.25, 0.25, 0.3, 0.85))
  if imgui.Button(active and "VFX ON" or "VFX", imgui.ImVec2(150 * s, 70 * s)) then menuOpen[0] = not menuOpen[0]; pull() end
  imgui.PopStyleColor()
  imgui.End()
  imgui.PopStyleVar()
end))

-- main menu
imgui.OnFrame(function() return menuOpen[0] end, guard(function()
  local io = imgui.GetIO()
  local s = io.DisplaySize.y / 1080
  imgui.SetNextWindowSize(imgui.ImVec2(760 * s, 0), imgui.Cond.FirstUseEver)
  imgui.SetNextWindowPos(imgui.ImVec2(io.DisplaySize.x * 0.5, io.DisplaySize.y * 0.5), imgui.Cond.FirstUseEver, imgui.ImVec2(0.5, 0.5))
  imgui.PushStyleVar(imgui.StyleVar.FramePadding, imgui.ImVec2(10 * s, 12 * s))
  imgui.PushStyleVar(imgui.StyleVar.ItemSpacing, imgui.ImVec2(10 * s, 12 * s))
  imgui.Begin("VoiceFX", menuOpen, imgui.WindowFlags.NoCollapse)
  imgui.SetWindowFontScale(math.max(1, s * 1.8))
  imgui.PushItemWidth(imgui.GetWindowWidth() * 0.55)

  if not lib then
    imgui.TextColored(COL_OFF, "Engine not found")
    imgui.TextWrapped(libErr or "")
    if imgui.Button("Retry") then loadEngine(); pull() end
  else
    -- master switch
    if imgui.Checkbox("Voice changer enabled", bEnabled) then setp(P.ENABLED, bEnabled[0] and 1 or 0) end

    imgui.Separator(); imgui.TextColored(COL_DIM, "Presets")
    for i, p in ipairs(presets) do
      if imgui.Button(p[1]) then applyPreset(p) end
      if i % 4 ~= 0 and i < #presets then imgui.SameLine() end
    end

    imgui.Separator(); imgui.TextColored(COL_DIM, "Voice")
    if imgui.SliderFloat("Pitch (semitones)", fSemi, -12, 12, "%.1f") then setp(P.PITCH, 2 ^ (fSemi[0] / 12)) end
    if imgui.SliderFloat("Formant ratio", fFormant, 0.5, 2.0, "%.2f") then setp(P.FORMANT, fFormant[0]) end
    if imgui.Checkbox("Preserve formants (less chipmunk)", bPreserve) then setp(P.PRESERVE, bPreserve[0] and 1 or 0) end
    if imgui.SliderFloat("Dry / wet mix", fMix, 0, 1, "%.2f") then setp(P.MIX, fMix[0]) end

    imgui.Separator(); imgui.TextColored(COL_DIM, "Microphone")
    if imgui.SliderFloat("Gain (dB)", fGain, -24, 24, "%.1f") then setp(P.GAIN, fGain[0]) end
    if imgui.SliderFloat("Noise gate (dB, -80 = off)", fGate, -80, -20, "%.0f") then setp(P.GATE, fGate[0]) end

    imgui.Separator(); imgui.TextColored(COL_DIM, "Robot effect")
    if imgui.Checkbox("Robot (ring modulator)", bRobot) then setp(P.ROBOT, bRobot[0] and 1 or 0) end
    if imgui.SliderFloat("Robot frequency (Hz)", fRobotHz, 20, 500, "%.0f") then setp(P.ROBOT_HZ, fRobotHz[0]) end

    imgui.Separator(); imgui.TextColored(COL_DIM, "Status")
    local hook = lib.vfx_stat(0)
    imgui.TextColored(hook == 1 and COL_ON or COL_OFF,
      hook == 1 and "BASS hook: installed" or (hook == -1 and "BASS hook: FAILED (see voicefx_log.txt)" or "BASS hook: not installed"))
    if hook ~= 1 then imgui.SameLine(); if imgui.Button("Retry hook") then lib.vfx_hook_retry() end end
    local starts, calls = lib.vfx_stat(1), lib.vfx_stat(2)
    imgui.Text(string.format("Mic sessions: %d   DSP calls: %d   chunk: %d bytes", starts, calls, lib.vfx_stat(3)))
    imgui.Text(string.format("Rate: %d Hz   block: %d   latency: %.0f ms   format OK: %s",
      lib.vfx_stat(4), lib.vfx_stat(6), lib.vfx_statf(2), lib.vfx_stat(5) == 1 and "yes" or "no/unknown"))
    if hook == 1 and starts == 0 then
      imgui.TextWrapped("Hook ready. Press/hold your voice-chat mic once so the game starts recording.")
    elseif starts > 0 and calls == 0 then
      imgui.TextColored(COL_OFF, "Mic is recording but BASS never called the effect (not working on this channel).")
    end
    VECW.x, VECW.y = -1, 22 * s
    imgui.Text("Mic in");  imgui.ProgressBar(math.min(1, lib.vfx_statf(0)), VECW)
    imgui.Text("Mic out"); imgui.ProgressBar(math.min(1, lib.vfx_statf(1)), VECW)

    if dirtyAt and os.clock() - dirtyAt > 1.0 then lib.vfx_save(); dirtyAt = nil end
  end
  imgui.PopItemWidth()
  imgui.End()
  imgui.PopStyleVar(2)
end))

-- optional chat command (only if the loader provides it; not verified on Nezuko)
if type(sampRegisterChatCommand) == "function" then
  pcall(sampRegisterChatCommand, "vfx", function() menuOpen[0] = not menuOpen[0]; pull() end)
end
