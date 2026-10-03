-- @description ReaAnimViewer
-- @version 0.2.2
-- @author Anthony Deneyer
-- @about
--   # ReaAnimViewer
--
--   3D animation viewer for REAPER. Load glTF and FBX animations onto your timeline
--   and watch the animated character play in sync with the playhead, from any camera angle.
--
--   Installs the ReaAnimViewer extension when needed and opens the viewer.
--   Restart REAPER after installing or updating. Windows 64-bit only.
-- @links
--   GitHub https://github.com/DemuteTools/ReaAnimViewer
-- @changelog
--   - Fix: Run said "installed" without installing anything when ReaPack still listed a deleted copy of the extension. It now opens ReaPack and tells you which package to uninstall
-- @provides
--   [nomain] .
--   [win64 extension] reaper_animviewer.dll https://github.com/DemuteTools/ReaAnimViewer/releases/download/v$version/$path
--   [win64 extension] FX/rav_video_fx.clap https://github.com/DemuteTools/ReaAnimViewer/releases/download/v$version/rav_video_fx.clap

------------------------------------------------------------------------------
-- Settings
------------------------------------------------------------------------------

local TITLE          = "ReaAnimViewer"
local OPEN_VIEWER_ID = "_RAV_OPEN_VIEWER"   -- registered by the extension (see src/plugin_main.cpp)
local DLL_NAME       = "reaper_animviewer.dll"
local CLAP_NAME      = "rav_video_fx.clap"      -- the video FX, in UserPlugins/FX

local SEP = package.config:sub(1, 1)

------------------------------------------------------------------------------
-- Helpers
------------------------------------------------------------------------------

local function message(text)
  reaper.MB(text, TITLE, 0)
end

local function userplugins_dir()
  return reaper.GetResourcePath() .. SEP .. "UserPlugins"
end

local function fx_dir()
  return userplugins_dir() .. SEP .. "FX"
end

-- Folder of this script (where the Demute Reaper Toolkit puts the package files).
local function script_dir()
  local path = debug.getinfo(1, "S").source:match("^@(.+)$")
  return path and path:match("^(.*)[/\\]")
end

-- The DLL the Demute Reaper Toolkit downloads next to this script.
local function sibling_dll()
  local dir = script_dir()
  return dir and (dir .. SEP .. DLL_NAME)
end

-- The video FX the Toolkit downloads with it: in an FX sub-folder (the package
-- path) or next to this script.
local function sibling_clap()
  local dir = script_dir()
  if not dir then return nil end
  for _, path in ipairs({ dir .. SEP .. "FX" .. SEP .. CLAP_NAME, dir .. SEP .. CLAP_NAME }) do
    if reaper.file_exists(path) then return path end
  end
  return nil
end

-- Leftovers of a previous self-update (src/self_update.cpp): <file>.old* and
-- <file>.new in `dir`. They may still be locked by the running extension: failures
-- are ignored.
local function remove_leftovers_of(dir, file_name)
  local pattern = "^" .. (file_name:gsub("%.", "%%.")) .. "%.(%a+)%d*$"
  local leftovers = {}
  reaper.EnumerateFiles(dir, -1)  -- drop REAPER's cached listing of the folder
  local i = 0
  while true do
    local name = reaper.EnumerateFiles(dir, i)
    if not name then break end
    local ext = name:lower():match(pattern)
    if ext == "old" or ext == "new" then leftovers[#leftovers + 1] = name end
    i = i + 1
  end
  for _, name in ipairs(leftovers) do
    os.remove(dir .. SEP .. name)
  end
end

local function remove_leftovers()
  remove_leftovers_of(userplugins_dir(), DLL_NAME)
  remove_leftovers_of(fx_dir(), CLAP_NAME)
end

-- Command id of "RAV: Open Viewer" when the extension is loaded, else nil.
-- NamedCommandLookup alone is not enough: REAPER also reserves an id for a named
-- command that a toolbar, menu or shortcut refers to, even when the extension is
-- not loaded. The extension reports a toggle state (0 or 1) for its action; an id
-- that is only reserved reports -1.
local function loaded_viewer_command()
  local id = reaper.NamedCommandLookup(OPEN_VIEWER_ID)
  if id == 0 or reaper.GetToggleCommandState(id) == -1 then return nil end
  return id
end

-- "<category>/<package>" of the ReaPack package that owns this file, or nil.
-- ReaPack answers from its registry, so it can own a file that no longer exists
-- (deleted by hand).
local function reapack_owner(path)
  if not reaper.ReaPack_GetOwner then return nil end
  local entry = reaper.ReaPack_GetOwner(path)
  if not entry then return nil end
  local name = "ReaAnimViewer"
  if reaper.ReaPack_GetEntryInfo then
    local ok, _, category, package = reaper.ReaPack_GetEntryInfo(entry)
    if ok and category and package then name = category .. "/" .. package end
  end
  if reaper.ReaPack_FreeEntry then reaper.ReaPack_FreeEntry(entry) end
  return name
end

local function read_file(path)
  local file = io.open(path, "rb")
  if not file then return nil end
  local data = file:read("*a")
  file:close()
  return data
end

local function copy_file(from, to)
  local data = read_file(from)
  if not data or #data == 0 then return false, "cannot read " .. from end
  local dst, err = io.open(to, "wb")
  if not dst then return false, err end
  local ok, werr = dst:write(data)
  local closed, cerr = dst:close()
  if not ok or not closed then
    os.remove(to)
    return false, werr or cerr
  end
  return true
end

-- Takes this script out of the Actions list, so "RAV: Open Viewer" is the only
-- entry. The package does not register it ([nomain]), but the Toolkit's Run button
-- registers it before running it, and 0.2.x installs registered it: removed after
-- every run. Run keeps working since the Toolkit registers it again each time.
local function remove_own_action()
  local _, path = reaper.get_action_context()
  if path and path ~= "" then
    reaper.AddRemoveReaScript(false, 0, path, true)
  end
end

local function ask_restart()
  message("ReaAnimViewer is installed.\n\n"
    .. "Restart REAPER, then use the action \"RAV: Open Viewer\".")
end

-- Toolkit install: put the video FX where REAPER scans CLAP plug-ins, when it is
-- missing there. Updates are done by the extension's self-update (same version as
-- the DLL). ReaPack installs it itself (FX/ line of @provides). Returns true when
-- it was copied now (REAPER only lists it after a restart).
local function install_video_fx()
  local installed = fx_dir() .. SEP .. CLAP_NAME
  if reaper.file_exists(installed) or reapack_owner(installed) then return false end
  local sibling = sibling_clap()
  if not sibling then return false end
  reaper.RecursiveCreateDirectory(fx_dir(), 0)
  local ok = copy_file(sibling, installed)
  return ok == true
end

------------------------------------------------------------------------------
-- Main
------------------------------------------------------------------------------

local function main()
  if not reaper.GetOS():match("Win64") then
    message("ReaAnimViewer is only available for Windows 64-bit for now.")
    return
  end

  remove_leftovers()
  local video_fx_new = install_video_fx()

  local installed = userplugins_dir() .. SEP .. DLL_NAME

  -- Extension loaded: open the viewer. Updates are handled by ReaPack, or by the
  -- extension itself when it was installed through the Toolkit.
  local open_viewer = loaded_viewer_command()
  if open_viewer then
    -- The action toggles the viewer: only run it when the viewer is closed.
    if reaper.GetToggleCommandState(open_viewer) ~= 1 then
      reaper.Main_OnCommand(open_viewer, 0)
    end
    if video_fx_new then
      message("The RAV video FX was installed.\n\nRestart REAPER to use it.")
    end
    return
  end

  local sibling = sibling_dll()
  local has_sibling = sibling and reaper.file_exists(sibling)

  -- On disk but not loaded yet (REAPER loads extensions at startup only).
  local owner = reapack_owner(installed)
  if owner and reaper.file_exists(installed) then
    ask_restart()
    return
  end
  if owner then
    -- ReaPack still lists the DLL as installed but the file is gone: copying it
    -- here would leave it tied to that package, so let ReaPack release it first.
    if reaper.ReaPack_BrowsePackages then reaper.ReaPack_BrowsePackages("ReaAnimViewer") end
    message("ReaPack still lists ReaAnimViewer as installed (package " .. owner .. "), "
      .. "but the extension file is missing.\n\n"
      .. "In the ReaPack window: right-click that ReaAnimViewer package, choose Uninstall, "
      .. "click Apply, then click Run again.")
    return
  end
  if reaper.file_exists(installed) then
    -- Not ReaPack's and not loaded: if it differs from the Toolkit copy, it may be
    -- damaged (REAPER failed to load it). It is not locked, so reinstall it.
    local current = read_file(installed)
    if has_sibling and current ~= read_file(sibling) then
      local ok, err = copy_file(sibling, installed)
      if not ok then
        message("Could not reinstall ReaAnimViewer into\n" .. installed .. "\n\n" .. tostring(err))
        return
      end
    end
    ask_restart()
    return
  end

  -- First run after a Toolkit install: put the DLL where REAPER loads it.
  if has_sibling then
    reaper.RecursiveCreateDirectory(userplugins_dir(), 0)
    local ok, err = copy_file(sibling, installed)
    if not ok then
      message("Could not install ReaAnimViewer into\n" .. installed .. "\n\n" .. tostring(err))
      return
    end
    ask_restart()
    return
  end

  message("The ReaAnimViewer extension is not installed.\n\n"
    .. "Install the ReaAnimViewer card from the Demute Reaper Toolkit, or the "
    .. "ReaAnimViewer package from ReaPack, then restart REAPER.")
end

main()
remove_own_action()
