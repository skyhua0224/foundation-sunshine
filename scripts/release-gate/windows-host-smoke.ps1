<#
.SYNOPSIS
  Release-gate smoke test of the portable host build on a Windows machine that may already have an
  installation. Nothing outside -WorkDir is written; the installed service, its config and its ports
  are never touched.

.DESCRIPTION
  1. unpacks the portable ZIP into -WorkDir\host
  2. writes a private config (own ports, own state, own log, crash reports and diagnostics sent only to
     -ReportEndpoint, never to a public address)
  3. checks `Sunshine.exe --version`, starts the host as a normal process, waits for the web UI
  4. probes /serverinfo (codec support), every Web UI page, and the encoder lines in the log
  5. runs the service wrapper outside the service manager; a clean stop must exit 0 and leave no crash report
  6. stops only the process it started and scans the log and the crash folder for known bad signatures

  Result: <EvidenceDir>\host-smoke.json (one object per check: id, status pass|fail|skip, detail).
  Exit code 0 when no check failed, 1 otherwise.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File windows-host-smoke.ps1 `
    -PortableZip C:\gate\Sunshine-Portable-x64.zip -WorkDir C:\gate\run1 -ReportEndpoint http://127.0.0.1:18787
#>
param(
  [Parameter(Mandatory)] [string] $PortableZip,
  [Parameter(Mandatory)] [string] $WorkDir,
  [Parameter(Mandatory)] [string] $ReportEndpoint,
  [int] $BasePort = 59089,
  [string] $EvidenceDir = '',
  [int] $StartTimeoutSec = 90
)
$ErrorActionPreference = 'Stop'
if (-not $EvidenceDir) { $EvidenceDir = Join-Path $WorkDir 'evidence' }
$results = New-Object System.Collections.Generic.List[object]
function Add-Result($id, $status, $detail) {
  $results.Add([pscustomobject]@{ id = $id; status = $status; detail = "$detail" })
  Write-Host ("[{0}] {1} {2}" -f $status.ToUpper(), $id, $detail)
}

# ---- guards: nothing here may reach the installed copy or a public service ------------------------------
$full = [System.IO.Path]::GetFullPath($WorkDir)
if ($full -like "$env:ProgramFiles*" -or $full -like "${env:ProgramFiles(x86)}*") { throw "WorkDir must not be under Program Files: $full" }
if ($ReportEndpoint -notmatch '^https?://(127\.0\.0\.1|localhost)(:\d+)?(/|$)') { throw "ReportEndpoint must be a loopback address, got: $ReportEndpoint" }
$ports = @(($BasePort - 5), $BasePort, ($BasePort + 1), ($BasePort + 9), ($BasePort + 10), ($BasePort + 11), ($BasePort + 13), ($BasePort + 21))
$busy = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort }
if ($busy) { throw ("A port of the test range is already in use: " + (($busy | Select-Object -ExpandProperty LocalPort -Unique) -join ',')) }

New-Item -ItemType Directory -Force -Path $full, $EvidenceDir | Out-Null
$hostDir = Join-Path $full 'host'
$cfgDir = Join-Path $full 'cfg'
$logFile = Join-Path $cfgDir 'sunshine.log'
if (Test-Path $hostDir) { Remove-Item -Recurse -Force $hostDir }
if (Test-Path $cfgDir) { Remove-Item -Recurse -Force $cfgDir }
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
Expand-Archive -LiteralPath $PortableZip -DestinationPath $hostDir -Force
# a portable ZIP may carry one top-level folder
$exe = Get-ChildItem -Path $hostDir -Recurse -Filter Sunshine.exe | Select-Object -First 1
if (-not $exe) { Add-Result 'smoke.unpack' 'fail' 'Sunshine.exe not found in the portable ZIP'; $results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $EvidenceDir 'host-smoke.json'); exit 1 }
$root = $exe.Directory.FullName
Add-Result 'smoke.unpack' 'pass' $root

# ---- config -----------------------------------------------------------------------------------------------
function P($p) { $p -replace '\\', '/' }
$conf = Join-Path $cfgDir 'sunshine.conf'
@"
sunshine_name = release-gate
port = $BasePort
upnp = disabled
min_log_level = debug
log_path = $(P $logFile)
file_state = $(P (Join-Path $cfgDir 'sunshine_state.json'))
credentials_file = $(P (Join-Path $cfgDir 'sunshine_credentials.json'))
file_apps = $(P (Join-Path $cfgDir 'apps.json'))
pkey = $(P (Join-Path $cfgDir 'key.pem'))
cert = $(P (Join-Path $cfgDir 'cert.pem'))
telemetry_endpoint = $ReportEndpoint
"@ | Set-Content -Encoding ASCII $conf
$appsSrc = Join-Path $root 'assets\apps.json'
if (Test-Path $appsSrc) { Copy-Item $appsSrc (Join-Path $cfgDir 'apps.json') }
$env:SUNSHINE_TELEMETRY_ENDPOINT = $ReportEndpoint

# ---- version -----------------------------------------------------------------------------------------------
$ver = (& $exe.FullName --version 2>&1 | Out-String).Trim()
if ($ver) { Add-Result 'smoke.version' 'pass' ($ver -split "`n")[0] } else { Add-Result 'smoke.version' 'fail' 'no output from --version' }

# ---- credentials, start ----------------------------------------------------------------------------------
$user = 'gate'; $pass = 'Gate-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
& $exe.FullName $conf --creds $user $pass 2>&1 | Out-Null
$stdout = Join-Path $cfgDir 'stdout.txt'; $stderr = Join-Path $cfgDir 'stderr.txt'
$proc = Start-Process -FilePath $exe.FullName -ArgumentList ('"' + $conf + '"') -WorkingDirectory $root -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
Add-Result 'smoke.start' 'pass' "pid $($proc.Id)"
try {
  $webPort = $BasePort + 1
  $deadline = (Get-Date).AddSeconds($StartTimeoutSec); $up = $false
  while ((Get-Date) -lt $deadline) {
    if ($proc.HasExited) { break }
    if (Get-NetTCPConnection -State Listen -LocalPort $webPort -ErrorAction SilentlyContinue) { $up = $true; break }
    Start-Sleep -Milliseconds 500
  }
  if ($up) { Add-Result 'smoke.web-up' 'pass' "port $webPort listening" }
  else { Add-Result 'smoke.web-up' 'fail' ("host did not listen on $webPort; exited=" + $proc.HasExited + $(if ($proc.HasExited) { " code=" + $proc.ExitCode } else { '' })) }

  if ($up) { try {
    Start-Sleep -Seconds 5
    # serverinfo: unauthenticated HTTP on the base port
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    $si = (& $curl -s -m 10 "http://127.0.0.1:$BasePort/serverinfo") -join "`n"
    $siFile = Join-Path $EvidenceDir 'serverinfo.xml'; $si | Set-Content -Encoding UTF8 $siFile
    $headless = $false
    if (Test-Path $logFile) { $headless = [bool](Select-String -Path $logFile -Pattern 'Failed to locate an output device' -Quiet) }
    if ($si -match '<hostname>') {
      $codec = if ($si -match '<ServerCodecModeSupport>(\d+)</ServerCodecModeSupport>') { [int]$Matches[1] } else { -1 }
      # bit layout of the GameStream codec-mode mask: 0x1 H.264, 0x100 HEVC, 0x200 HEVC Main10, 0x10000 AV1 Main8, 0x20000 AV1 Main10
      $names = @(); if ($codec -ge 0) { if ($codec -band 0x1) { $names += 'H264' }; if ($codec -band 0x100) { $names += 'HEVC' }; if ($codec -band 0x200) { $names += 'HEVC-10bit' }; if ($codec -band 0x10000) { $names += 'AV1' }; if ($codec -band 0x20000) { $names += 'AV1-10bit' } }
      Add-Result 'smoke.serverinfo' 'pass' '/serverinfo answers with a host name and a pairing state'
      if ($headless) { Add-Result 'enc.codec-support' 'skip' ("no desktop in this session (no output device), so the encoder probe says nothing about the machine; mask $codec = " + ($names -join ',')) }
      else { Add-Result 'enc.codec-support' 'info' ("codec mask $codec = " + ($names -join ',')) }
      $pair = if ($si -match '<PairStatus>(\d)</PairStatus>') { $Matches[1] } else { '?' }
      Add-Result 'pair.unpaired-state' $(if ($pair -eq '0') { 'pass' } else { 'fail' }) "PairStatus $pair on a fresh state"
    } else { Add-Result 'smoke.serverinfo' 'fail' 'no <hostname> in /serverinfo' }

    # Web UI pages (HTTPS, basic auth, self-signed certificate)
    foreach ($page in '/', '/config', '/apps', '/password', '/pin', '/troubleshooting', '/dualsense', '/downloads') {
      $code = & $curl -k -s -m 20 -u "${user}:${pass}" -o NUL -w '%{http_code}' ("https://127.0.0.1:$webPort" + $page)
      $st = if ($code -eq '200') { 'pass' } else { 'fail' }
      Add-Result ("web.page" + $(if ($page -eq '/') { '.root' } else { $page -replace '/', '.' })) $st "HTTP $code"
    }
    $code = & $curl -k -s -m 20 -u "${user}:${pass}" -o (Join-Path $EvidenceDir 'api-config.json') -w '%{http_code}' "https://127.0.0.1:$webPort/api/config"
    Add-Result 'web.api.config' $(if ($code -eq '200') { 'pass' } else { 'fail' }) "HTTP $code"
    Start-Sleep -Seconds 20   # let the diagnostics sender run once against the local endpoint
  } catch { Add-Result 'smoke.probe-error' 'fail' ($_.Exception.Message + ' @ ' + $_.InvocationInfo.PositionMessage) } }
}
finally {
  if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; $proc.WaitForExit(15000) | Out-Null }
  # the host starts the Panel from its own directory: end those children too, matched by image path under WorkDir only
  Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($full + '\', [System.StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Seconds 2
}

# ---- service wrapper outside the service manager (the false-crash regression of the 2026-11-14 prerelease) ---
$svc = Get-ChildItem -Path $root -Recurse -Filter sunshinesvc.exe | Select-Object -First 1
if ($svc) {
  $svcCrash = Join-Path $svc.Directory.FullName 'config\crashes\pending'
  $before = if (Test-Path $svcCrash) { @(Get-ChildItem $svcCrash).Count } else { 0 }
  $sp = Start-Process -FilePath $svc.FullName -WorkingDirectory $svc.Directory.FullName -PassThru -WindowStyle Hidden
  if (-not $sp.WaitForExit(30000)) { Stop-Process -Id $sp.Id -Force; Add-Result 'svc.clean-exit' 'fail' 'wrapper did not exit within 30 s' }
  else {
    Start-Sleep -Seconds 2
    $after = if (Test-Path $svcCrash) { @(Get-ChildItem $svcCrash).Count } else { 0 }
    if ($sp.ExitCode -eq 0 -and $after -eq $before) { Add-Result 'svc.clean-exit' 'pass' 'exit code 0, no crash report' }
    else { Add-Result 'svc.clean-exit' 'fail' ("exit code " + $sp.ExitCode + ", crash reports " + $before + " -> " + $after) }
  }
} else { Add-Result 'svc.clean-exit' 'skip' 'sunshinesvc.exe not in the portable ZIP' }

# ---- log and crash scan --------------------------------------------------------------------------------------
$scan = Join-Path $PSScriptRoot 'scan-host-logs.ps1'
if (Test-Path $scan) {
  $scanOut = & $scan -LogPath $logFile -CrashDir (Join-Path $cfgDir 'crashes\pending') -EvidenceDir $EvidenceDir
  foreach ($r in $scanOut) { $results.Add($r) }
} else { Add-Result 'log.scan' 'skip' 'scan-host-logs.ps1 not next to this script' }

Copy-Item $logFile (Join-Path $EvidenceDir 'sunshine.log') -ErrorAction SilentlyContinue
Copy-Item $stderr (Join-Path $EvidenceDir 'stderr.txt') -ErrorAction SilentlyContinue
$results | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir 'host-smoke.json')
$failed = @($results | Where-Object { $_.status -eq 'fail' }).Count
Write-Host "done: $($results.Count) checks, $failed failed"
exit $(if ($failed -gt 0) { 1 } else { 0 })
