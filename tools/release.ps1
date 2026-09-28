# ============================================================================
#  ReaAnimViewer release script. Run it through release.bat.
#
#  1. Checks the repo is clean, on main and up to date.
#  2. Asks for the new version and the changelog (Notepad).
#  3. Clean release build + checks the DLL has no VC++ runtime dependency.
#  4. Bumps the version in Extensions/ReaAnimViewer.ext, Scripts/RAV_Launcher.lua,
#     CMakeLists.txt and README.md.
#  5. Commits, tags vX.Y.Z, pushes the tag, creates the GitHub Release with the DLL.
#  6. Pushes main: GitHub Actions then regenerates index.xml with reapack-index.
# ============================================================================
# Native commands (git, gh) are checked through $LASTEXITCODE, so keep 'Continue':
# with 'Stop', Windows PowerShell can abort on harmless git progress written to stderr.
$ErrorActionPreference = 'Continue'

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$ExtPath    = 'Extensions/ReaAnimViewer.ext'
$LauncherPath = 'Scripts/RAV_Launcher.lua'
$CMakePath  = 'CMakeLists.txt'
$ReadmePath = 'README.md'
$DllPath    = Join-Path $Root 'build\Release\reaper_animviewer.dll'
$Utf8NoBom  = New-Object System.Text.UTF8Encoding($false)

function Fail([string]$Message) {
    Write-Host ''
    Write-Host "[ERROR] $Message" -ForegroundColor Red
    exit 1
}

function Step([string]$Message) {
    Write-Host ''
    Write-Host "== $Message" -ForegroundColor Cyan
}

# Runs git and stops the release on failure. Returns git's output.
function Invoke-Git {
    $output = & git @args
    if ($LASTEXITCODE -ne 0) { Fail "git $($args -join ' ') failed." }
    return $output
}

function Read-RepoText([string]$RelativePath) {
    return [IO.File]::ReadAllText((Join-Path $Root $RelativePath))
}

function Write-RepoText([string]$RelativePath, [string]$Text) {
    [IO.File]::WriteAllText((Join-Path $Root $RelativePath), $Text, $Utf8NoBom)
}

# Keeps the file's own line ending style (CRLF or LF).
function Get-NewLine([string]$Text) {
    if ($Text.Contains("`r`n")) { return "`r`n" }
    return "`n"
}

# --- 1. Preconditions -------------------------------------------------------
Step 'Checking the repository'

$gh = $env:GH
if (-not $gh) { Fail 'GitHub CLI path not set. Run release.bat instead of this script.' }

$branch = (Invoke-Git rev-parse --abbrev-ref HEAD | Out-String).Trim()
if ($branch -ne 'main') { Fail "You are on branch '$branch'. Switch to main first." }

$dirty = Invoke-Git status --porcelain --untracked-files=no
if ($dirty) { Fail "You have uncommitted changes. Commit or stash them first:`n$($dirty -join "`n")" }

$originUrl = (Invoke-Git remote get-url origin | Out-String).Trim()
$m = [regex]::Match($originUrl, 'github\.com[:/](?<owner>[^/]+)/(?<repo>[^/]+?)(\.git)?$')
if (-not $m.Success) { Fail "Cannot read the GitHub owner/repo from origin: $originUrl" }
$owner = $m.Groups['owner'].Value
$repo  = $m.Groups['repo'].Value
Write-Host "Repository: $owner/$repo"

Invoke-Git fetch origin --tags | Out-Null
Invoke-Git pull --ff-only origin main | Out-Null

# --- 2. Version and changelog ----------------------------------------------
Step 'Version'

$extText = Read-RepoText $ExtPath
$current = [regex]::Match($extText, '(?m)^@version\s+(\S+)').Groups[1].Value
Write-Host "Current version: $current"
$version = (Read-Host 'New version (examples: 0.1.0, 0.2.0-beta)').Trim()
if ($version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$') {
    Fail "Invalid version '$version'. Expected X.Y.Z or X.Y.Z-suffix."
}
$tag = "v$version"
if (Invoke-Git tag --list $tag) { Fail "Tag $tag already exists locally." }
& git ls-remote --exit-code --tags origin "refs/tags/$tag" *> $null
if ($LASTEXITCODE -eq 0) { Fail "Tag $tag already exists on GitHub." }
$isPrerelease = $version.Contains('-')

Step 'Changelog'
$notesFile = Join-Path $env:TEMP "reaanimviewer-changelog-$version.txt"
$template = @(
    "# Changelog for $tag. Write one change per line below.",
    '# Lines starting with # are ignored. Save and close Notepad to continue.',
    ''
)
[IO.File]::WriteAllLines($notesFile, [string[]]$template, $Utf8NoBom)
# No -Wait: the Windows 11 Notepad can return immediately when it is already open.
Start-Process notepad.exe -ArgumentList "`"$notesFile`""
Read-Host 'Notepad is open: write the changelog, SAVE it, then press Enter here' | Out-Null

$changes = @(Get-Content -Path $notesFile -Encoding UTF8 |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { if ($_ -match '^[-*] ') { $_ } else { "- $_" } })
if ($changes.Count -eq 0) { Fail 'Empty changelog, release cancelled.' }

# --- 3. Build ---------------------------------------------------------------
Step 'Clean release build'
& (Join-Path $Root 'build.bat') release clean noinstall nopause
if ($LASTEXITCODE -ne 0) { Fail 'Build failed, nothing was changed.' }
if (-not (Test-Path $DllPath)) { Fail "Build output not found: $DllPath" }

# The DLL must be self-contained: no dynamic VC++ runtime (see CMakeLists.txt).
$dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($DllPath))
foreach ($crt in @('MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll', 'api-ms-win-crt-')) {
    if ($dllText.IndexOf($crt, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        Fail "The DLL depends on $crt (dynamic VC++ runtime). Users would need the VC++ Redistributable."
    }
}
Write-Host 'DLL OK: no VC++ runtime dependency.'

# --- 4. Bump the version in the repo files ----------------------------------
Step 'Updating version files'

# Extensions/ReaAnimViewer.ext: version, changelog, download URLs, publish flag.
$nl = Get-NewLine $extText
$changelogBlock = '@changelog' + $nl + (($changes | ForEach-Object { "  $_" }) -join $nl) + $nl
$extText = [regex]::Replace($extText, '(?m)^@version[ \t]+\S+', "@version $version")
$extText = [regex]::Replace($extText, '(?m)^@noindex[ \t]*\r?\n', '')
$extText = [regex]::Replace($extText, '(?m)^@changelog[ \t]*\r?\n(?:[ \t]+[^\r\n]*\r?\n)*',
    [Text.RegularExpressions.MatchEvaluator] { param($x) $changelogBlock })
$extText = [regex]::Replace($extText, 'https://github\.com/[^/\s]+/[^/\s]+/releases/download/',
    "https://github.com/$owner/$repo/releases/download/")
$extText = [regex]::Replace($extText, '(?m)^([ \t]+GitHub[ \t]+)https://github\.com/\S+',
    [Text.RegularExpressions.MatchEvaluator] { param($x) $x.Groups[1].Value + "https://github.com/$owner/$repo" })
Write-RepoText $ExtPath $extText

# Scripts/RAV_Launcher.lua: same version and changelog as the extension, since the
# Demute Reaper Toolkit shows the launcher's version. Also points it to this repo.
$launcherText = Read-RepoText $LauncherPath
$lnl = Get-NewLine $launcherText
$launcherChangelog = '-- @changelog' + $lnl + (($changes | ForEach-Object { "--   $_" }) -join $lnl) + $lnl
$launcherText = [regex]::Replace($launcherText, '(?m)^-- @version[ \t]+\S+', "-- @version $version")
$launcherText = [regex]::Replace($launcherText, '(?m)^-- @changelog[ \t]*\r?\n(?:--[ \t]+[^@\r\n][^\r\n]*\r?\n)*',
    [Text.RegularExpressions.MatchEvaluator] { param($x) $launcherChangelog })
$launcherText = [regex]::Replace($launcherText, 'https://github\.com/[^/\s"]+/[^/\s"]+/raw/main/index\.xml',
    "https://github.com/$owner/$repo/raw/main/index.xml")
$launcherText = [regex]::Replace($launcherText, '(?m)^(--[ \t]+GitHub[ \t]+)https://github\.com/\S+',
    [Text.RegularExpressions.MatchEvaluator] { param($x) $x.Groups[1].Value + "https://github.com/$owner/$repo" })
Write-RepoText $LauncherPath $launcherText

# CMakeLists.txt: project(... VERSION X.Y.Z). CMake only accepts numbers.
$numericVersion = ($version -split '-')[0]
$cmakeText = Read-RepoText $CMakePath
$cmakeRegex = New-Object System.Text.RegularExpressions.Regex('(?m)^([ \t]+VERSION[ \t]+)\d+\.\d+\.\d+')
if (-not $cmakeRegex.IsMatch($cmakeText)) { Fail 'Could not find the project VERSION in CMakeLists.txt.' }
$cmakeText = $cmakeRegex.Replace($cmakeText,
    [Text.RegularExpressions.MatchEvaluator] { param($x) $x.Groups[1].Value + $numericVersion }, 1)
Write-RepoText $CMakePath $cmakeText

# README.md: "**Version:** X.Y.Z" line.
$readmeText = Read-RepoText $ReadmePath
$readmeText = [regex]::Replace($readmeText, '(?m)^\*\*Version:\*\*[^\r\n]*', "**Version:** $version")
Write-RepoText $ReadmePath $readmeText

Write-Host ''
Write-Host "Ready to publish ReaAnimViewer $tag" -ForegroundColor Green
if ($isPrerelease) { Write-Host '(marked as pre-release on GitHub)' }
Write-Host 'Changelog:'
$changes | ForEach-Object { Write-Host "  $_" }
Write-Host ''
Invoke-Git --no-pager diff --stat | ForEach-Object { Write-Host $_ }
$answer = Read-Host "Commit, tag and publish $tag now? (y/N)"
if ($answer -notmatch '^[yY]') {
    & git checkout -- $ExtPath $LauncherPath $CMakePath $ReadmePath
    Fail 'Release cancelled, version files restored.'
}

# --- 5. Commit, tag, GitHub Release -----------------------------------------
Step "Committing and tagging $tag"
Invoke-Git add -- $ExtPath $LauncherPath $CMakePath $ReadmePath | Out-Null
Invoke-Git commit -m "Release $tag" -m ($changes -join "`n") | Out-Null
Invoke-Git tag -a $tag -m "ReaAnimViewer $tag" | Out-Null
Invoke-Git push origin $tag | Out-Null

Step 'Creating the GitHub Release'
$releaseNotes = Join-Path $env:TEMP "reaanimviewer-release-notes-$version.md"
[IO.File]::WriteAllLines($releaseNotes, [string[]]$changes, $Utf8NoBom)
$ghArgs = @('release', 'create', $tag, $DllPath,
    '--repo', "$owner/$repo",
    '--title', "ReaAnimViewer $tag",
    '--notes-file', $releaseNotes,
    '--verify-tag')
if ($isPrerelease) { $ghArgs += '--prerelease' }
& $gh @ghArgs
if ($LASTEXITCODE -ne 0) {
    Fail ("The tag $tag is pushed but the GitHub Release failed. Fix the problem, then run:`n" +
        "  gh release create $tag build\Release\reaper_animviewer.dll --title `"ReaAnimViewer $tag`"`n" +
        "and finally: git push origin main")
}

# --- 6. Push main: CI regenerates index.xml ---------------------------------
Step 'Pushing main'
Invoke-Git push origin main | Out-Null

Remove-Item $notesFile, $releaseNotes -ErrorAction SilentlyContinue

Write-Host ''
Write-Host "ReaAnimViewer $tag is released." -ForegroundColor Green
Write-Host "GitHub Actions now updates index.xml (about 1 minute):"
Write-Host "  https://github.com/$owner/$repo/actions"
Write-Host 'Then ReaPack and the Demute Reaper Toolkit will offer the update.'
Write-Host 'Run "git pull" later to get the index.xml commit made by the bot.'
exit 0
