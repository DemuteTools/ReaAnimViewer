-- @description ReaAnimViewer
-- @version 0.2.1
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
--   - Fix: clicking Run in the Demute Reaper Toolkit did nothing when a toolbar button for RAV: Open Viewer already existed
--   - Run no longer closes the viewer when it is already open
-- @provides
--   [main] .
--   [win64 extension] reaper_animviewer.dll https://github.com/DemuteTools/ReaAnimViewer/releases/download/v$version/$path

------------------------------------------------------------------------------
-- Settings
------------------------------------------------------------------------------

local TITLE          = "ReaAnimViewer"
local OPEN_VIEWER_ID = "_RAV_OPEN_VIEWER"   -- registered by the extension (see src/plugin_main.cpp)
local DLL_NAME       = "reaper_animviewer.dll"

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

-- The DLL the Demute Reaper Toolkit downloads next to this script.
local function sibling_dll()
  local path = debug.getinfo(1, "S").source:match("^@(.+)$")
  local dir = path and path:match("^(.*)[/\\]")
  return dir and (dir .. SEP .. DLL_NAME)
end

-- Leftovers of a previous self-update (src/self_update.cpp): .old* and .new.
-- They may still be locked by the running extension: failures are ignored.
local function remove_leftovers()
  local dir = userplugins_dir()
  local pattern = "^" .. (DLL_NAME:gsub("%.", "%%.")) .. "%.(%a+)%d*$"
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

local function ask_restart()
  message("ReaAnimViewer is installed.\n\n"
    .. "Restart REAPER, then use the action \"RAV: Open Viewer\".")
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

  local installed = userplugins_dir() .. SEP .. DLL_NAME

  -- Extension loaded: open the viewer. Updates are handled by ReaPack, or by the
  -- extension itself when it was installed through the Toolkit.
  local open_viewer = loaded_viewer_command()
  if open_viewer then
    -- The action toggles the viewer: only run it when the viewer is closed.
    if reaper.GetToggleCommandState(open_viewer) ~= 1 then
      reaper.Main_OnCommand(open_viewer, 0)
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
