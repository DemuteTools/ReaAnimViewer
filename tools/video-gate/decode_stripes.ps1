# ============================================================================
#  Video FX sync check (Epic 11). Run it through decode_stripes.bat.
#
#  Reads a video rendered with the RAV video FX test pattern on ("RAV: Video FX
#  test pattern") and decodes, for EVERY frame, the 16-bar code at the top of the
#  picture (most significant bit on the left, white = 1). Each code must be the
#  frame's project frame index: frame n of the file = first frame index + n. The
#  first frame index (region start x frame rate) is asked for: without it only the
#  steps between frames are checked, and a constant A/V offset would pass. The
#  background below the code band must be orange (azure = red and blue swapped).
#
#  Writes <video>.stripes.csv next to the video (frame, code, expected, match) and
#  prints a summary. ffmpeg is installed with winget on first use if missing.
# ============================================================================
param([string]$Video, [string]$FirstFrame)

$ErrorActionPreference = 'Continue'

function Fail([string]$Message) {
    Write-Host ''
    Write-Host "[ERROR] $Message" -ForegroundColor Red
    exit 1
}

# --- Video file --------------------------------------------------------------
if (-not $Video) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Video rendered with the RAV video FX test pattern'
    $dialog.Filter = 'Video files|*.mp4;*.mov;*.mkv;*.avi;*.webm|All files|*.*'
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'No video chosen.' }
    $Video = $dialog.FileName
}
if (-not (Test-Path -LiteralPath $Video)) { Fail "File not found: $Video" }
$Video = (Resolve-Path -LiteralPath $Video).Path

# --- Expected first frame index -----------------------------------------------
if (-not $PSBoundParameters.ContainsKey('FirstFrame')) {
    Add-Type -AssemblyName Microsoft.VisualBasic
    $FirstFrame = [Microsoft.VisualBasic.Interaction]::InputBox(
        ("Project frame index of the first rendered frame = region start in seconds x frame rate " +
         "(10 s at 30 fps = 300; 0 when the render starts at 0:00).`n`nLeave empty to skip this check " +
         "(then a constant offset between picture and project time is NOT detected)."),
        'RAV video FX sync check', '')
}
$expectedFirst = $null
if ($FirstFrame -and $FirstFrame.Trim() -ne '') {
    $parsed = 0
    if (-not [int]::TryParse($FirstFrame.Trim(), [ref]$parsed) -or $parsed -lt 0) {
        Fail "Not a frame index: $FirstFrame"
    }
    $expectedFirst = $parsed % 65536
}

# --- ffmpeg ------------------------------------------------------------------
function Find-Ffmpeg {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $links = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\ffmpeg.exe'
    if (Test-Path $links) { return $links }
    $packages = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path $packages) {
        $found = Get-ChildItem -Path $packages -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

$ffmpeg = Find-Ffmpeg
if (-not $ffmpeg) {
    Write-Host 'ffmpeg is not installed. Installing it with winget (one time)...'
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Fail 'winget is not available. Install ffmpeg from https://www.gyan.dev/ffmpeg/builds/ and run this again.'
    }
    & winget install --id Gyan.FFmpeg -e --source winget --accept-source-agreements --accept-package-agreements
    $ffmpeg = Find-Ffmpeg
    if (-not $ffmpeg) { Fail 'ffmpeg was installed but not found. Close this window and run the check again.' }
}

# --- Decode --------------------------------------------------------------------
# Top 1/8 of each frame (the code band), averaged down to 16 grey pixels: one per bar.
# A white bar averages ~230, a black one ~25 (the grey gaps add a little), threshold 128.
Write-Host "Decoding $Video ..."
$raw = Join-Path $env:TEMP ("rav_stripes_{0}.gray" -f [Guid]::NewGuid().ToString('N'))
& $ffmpeg -v error -i $Video -an -fps_mode passthrough `
    -vf 'crop=iw:trunc(ih/8):0:0,scale=16:1:flags=area' -pix_fmt gray -f rawvideo -y $raw
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $raw)) { Fail 'ffmpeg could not decode the video.' }

$bytes = [IO.File]::ReadAllBytes($raw)
Remove-Item $raw -ErrorAction SilentlyContinue
$frames = [int][Math]::Floor($bytes.Length / 16)
if ($frames -eq 0) { Fail 'The video has no frames.' }

# Colour: a strip of the background at mid-height, 16 RGB samples per frame. The
# sliding bar covers at most one sample; the average of the 16 stays orange.
$rawRgb = Join-Path $env:TEMP ("rav_stripes_{0}.rgb" -f [Guid]::NewGuid().ToString('N'))
& $ffmpeg -v error -i $Video -an -fps_mode passthrough `
    -vf 'crop=iw:trunc(ih/8):0:trunc(ih/2),scale=16:1:flags=area' -pix_fmt rgb24 -f rawvideo -y $rawRgb
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $rawRgb)) { Fail 'ffmpeg could not decode the video colours.' }
$rgb = [IO.File]::ReadAllBytes($rawRgb)
Remove-Item $rawRgb -ErrorAction SilentlyContinue
$colourFrames = [int][Math]::Floor($rgb.Length / 48)
$notOrange = 0
$firstNotOrange = $null
for ($n = 0; $n -lt $colourFrames; $n++) {
    $r = 0
    $b = 0
    for ($k = 0; $k -lt 16; $k++) {
        $r += $rgb[$n * 48 + $k * 3]
        $b += $rgb[$n * 48 + $k * 3 + 2]
    }
    if (($r - $b) / 16 -lt 64) {  # orange = (255,128,0): red far above blue
        $notOrange++
        if ($null -eq $firstNotOrange) { $firstNotOrange = $n }
    }
}

$csvPath = $Video + '.stripes.csv'
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('frame,code,expected,match')

$first = $null
$mismatches = 0
$firstMismatches = New-Object System.Collections.Generic.List[string]
for ($n = 0; $n -lt $frames; $n++) {
    $code = 0
    for ($k = 0; $k -lt 16; $k++) {
        $code = $code * 2
        if ($bytes[$n * 16 + $k] -ge 128) { $code = $code + 1 }
    }
    if ($null -eq $first) { $first = $code }
    $base = $(if ($null -ne $expectedFirst) { $expectedFirst } else { $first })
    $expected = ($base + $n) % 65536
    $match = ($code -eq $expected)
    if (-not $match) {
        $mismatches++
        if ($firstMismatches.Count -lt 20) { $firstMismatches.Add("  frame $n : code $code, expected $expected") }
    }
    $lines.Add(('{0},{1},{2},{3}' -f $n, $code, $expected, $(if ($match) { 'yes' } else { 'NO' })))
}
[IO.File]::WriteAllLines($csvPath, $lines)

# --- Report ----------------------------------------------------------------------
Write-Host ''
Write-Host "Frames decoded : $frames"
Write-Host "First code     : $first  (= project frame index of the first frame; 0 when the render starts at 0:00)"
if ($null -ne $expectedFirst) {
    Write-Host "Expected first : $expectedFirst"
} else {
    Write-Host 'Expected first : not given - only the steps between frames are checked'
}
Write-Host "Per-frame list : $csvPath"
Write-Host ''
if ($mismatches -eq 0 -and $notOrange -eq 0) {
    if ($null -ne $expectedFirst) {
        Write-Host "PASS - every frame's code equals its project frame index ($frames / $frames), colours right." -ForegroundColor Green
    } else {
        Write-Host "PASS (partial) - codes step by one on every frame ($frames / $frames), colours right; the first frame index was not checked." -ForegroundColor Yellow
    }
    exit 0
}
if ($notOrange -gt 0) {
    Write-Host "FAIL - the background is not orange on $notOrange frames (first: frame $firstNotOrange)." -ForegroundColor Red
    Write-Host 'Azure blue means red and blue are swapped in the FX output; black means the FX drew nothing there.'
}
if ($mismatches -gt 0) {
    Write-Host "FAIL - $mismatches of $frames frames do not match. First ones:" -ForegroundColor Red
    $firstMismatches | ForEach-Object { Write-Host $_ }
    if ($null -ne $expectedFirst -and $first -ne $expectedFirst) {
        Write-Host "The first code is $first, not $expectedFirst: the picture is offset from project time (or the"
        Write-Host 'expected index typed above is wrong: region start in seconds x frame rate).'
    }
    Write-Host 'A run of mismatches where the code jumps is a sync error; a code of 0 on every frame means the'
    Write-Host 'test pattern was off (action "RAV: Video FX test pattern") or the FX drew nothing. If every'
    Write-Host 'code is wrong, render at the project video size: a letterboxed or cropped picture moves the code band.'
}
exit 2
