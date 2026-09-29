-- @description ReaAnimViewer Launcher
-- @version 0.1.1
-- @author Anthony Deneyer
-- @about
--   # ReaAnimViewer Launcher
--
--   Opens the ReaAnimViewer 3D animation viewer. Installs the ReaAnimViewer extension
--   through ReaPack when it is missing, and updates it when this launcher is newer.
--
--   Used by the Demute Reaper Toolkit, which can only install scripts: the extension
--   itself (a DLL) must live in UserPlugins, so this launcher lets ReaPack handle it.
--   Updating the launcher from the Toolkit then updates the extension too.
--
--   Windows 64-bit only.
-- @links
--   GitHub https://github.com/DemuteTools/ReaAnimViewer
-- @changelog
--   - Fix an issue where close and reopen the view makes the view empty
-- @provides [main] .

------------------------------------------------------------------------------
-- Settings
------------------------------------------------------------------------------

local TITLE          = "ReaAnimViewer"
local REPO_NAME      = "ReaAnimViewer"
local REPO_URL       = "https://github.com/DemuteTools/ReaAnimViewer/raw/main/index.xml"
local OPEN_VIEWER_ID = "_RAV_OPEN_VIEWER"   -- registered by the extension (see src/plugin_main.cpp)
local REAPACK_SYNC   = "_REAPACK_SYNC"      -- "ReaPack: Synchronize packages"
local DLL_NAME       = "reaper_animviewer.dll"
local TIMEOUT_SECONDS = 90

-- MB() button types and return values
local MB_OK, MB_YESNO, IDYES = 0, 4, 6

------------------------------------------------------------------------------
-- Helpers
------------------------------------------------------------------------------

local function message(text, buttons)
  return reaper.MB(text, TITLE, buttons or MB_OK)
end

local function dll_path()
  local sep = package.config:sub(1, 1)
  return reaper.GetResourcePath() .. sep .. "UserPlugins" .. sep .. DLL_NAME
end

local function has_reapack()
  return reaper.ReaPack_AddSetRepository ~= nil
end

-- Version of this launcher, read from its own @version header. release.bat keeps it
-- equal to the extension version, so it is the version the extension should have.
local function launcher_version()
  local path = debug.getinfo(1, "S").source:match("^@(.+)$")
  local file = path and io.open(path, "r")
  if not file then return nil end
  local text = file:read("*a")
  file:close()
  return text:match("@version%s+(%S+)")
end

-- Version of the extension as installed by ReaPack, or nil when ReaPack does not
-- own the DLL (not installed, or copied by hand / by build.bat during development).
local function installed_extension_version()
  if not has_reapack() then return nil end
  local entry = reaper.ReaPack_GetOwner(dll_path())
  if not entry then return nil end
  local ok, _, _, _, _, _, version = reaper.ReaPack_GetEntryInfo(entry)
  reaper.ReaPack_FreeEntry(entry)
  if ok then return version end
  return nil
end

-- True when version a is lower than version b.
local function is_older(a, b)
  if not (a and b) then return false end
  return reaper.ReaPack_CompareVersions(a, b) < 0
end

local function ask_restart(what)
  message("ReaAnimViewer " .. what .. ".\n\n"
    .. "Restart REAPER to load it, then run this launcher again "
    .. "(or the action \"RAV: Open Viewer\").")
end

local function open_reapack_browser(text)
  reaper.ReaPack_BrowsePackages(REPO_NAME)
  message(text .. "\n\nIn the ReaPack window that just opened: right-click \"ReaAnimViewer\", "
    .. "choose Install (or Update), click Apply, then restart REAPER.")
end

-- Starts a ReaPack synchronization, then waits (without blocking REAPER) until
-- is_done() returns true. Falls back to the ReaPack browser after a timeout.
local function sync_and_wait(is_done, on_done, timeout_text)
  local sync = reaper.NamedCommandLookup(REAPACK_SYNC)
  if sync == 0 then
    open_reapack_browser(timeout_text)
    return
  end
  reaper.Main_OnCommand(sync, 0)

  local start = reaper.time_precise()
  local function poll()
    if is_done() then
      on_done()
    elseif reaper.time_precise() - start > TIMEOUT_SECONDS then
      open_reapack_browser(timeout_text)
    else
      reaper.defer(poll)
    end
  end
  reaper.defer(poll)
end

------------------------------------------------------------------------------
-- Actions
------------------------------------------------------------------------------

local function install_extension()
  if not has_reapack() then
    message("ReaPack is required to install ReaAnimViewer.\n\n"
      .. "Install it from https://reapack.com, restart REAPER and run this launcher again.")
    return
  end

  local answer = message("The ReaAnimViewer extension is not installed yet.\n\n"
    .. "Install it now with ReaPack?", MB_YESNO)
  if answer ~= IDYES then return end

  -- autoInstall = 1: ReaPack installs every package of this repository when it synchronizes.
  local ok, err = reaper.ReaPack_AddSetRepository(REPO_NAME, REPO_URL, true, 1)
  if not ok then
    message("Could not add the ReaAnimViewer repository to ReaPack:\n\n" .. tostring(err))
    return
  end
  reaper.ReaPack_ProcessQueue(true)

  sync_and_wait(
    function() return reaper.file_exists(dll_path()) end,
    function() ask_restart("is installed") end,
    "ReaPack did not install ReaAnimViewer automatically.")
end

local function update_extension(installed, target)
  local answer = message("A new version of ReaAnimViewer is available.\n\n"
    .. "Installed: " .. installed .. "\nNew: " .. target .. "\n\n"
    .. "Update it now with ReaPack?", MB_YESNO)
  if answer ~= IDYES then return false end

  sync_and_wait(
    function() return not is_older(installed_extension_version(), target) end,
    function() ask_restart("is updated") end,
    "ReaPack did not update ReaAnimViewer automatically.")
  return true
end

------------------------------------------------------------------------------
-- Main
------------------------------------------------------------------------------

local function main()
  if not reaper.GetOS():match("Win64") then
    message("ReaAnimViewer is only available for Windows 64-bit for now.")
    return
  end

  local target    = launcher_version()
  local installed = installed_extension_version()
  local loaded    = reaper.NamedCommandLookup(OPEN_VIEWER_ID) ~= 0

  -- 1. The launcher is newer than the installed extension (the Toolkit updated the
  --    launcher): update the extension through ReaPack. "No" opens the current one.
  if installed and is_older(installed, target) then
    if update_extension(installed, target) then return end
  end

  -- 2. Extension loaded: open the viewer.
  if loaded then
    reaper.Main_OnCommand(reaper.NamedCommandLookup(OPEN_VIEWER_ID), 0)
    return
  end

  -- 3. Installed but not loaded yet (REAPER loads extensions at startup only).
  if reaper.file_exists(dll_path()) then
    ask_restart("is installed")
    return
  end

  -- 4. Not installed: let ReaPack install it.
  install_extension()
end

main()
