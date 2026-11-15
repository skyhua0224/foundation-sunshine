<#
.SYNOPSIS
  Release-gate installer scenarios: fresh install, upgrade from older releases, each component, the
  USB/IP guard, service state, uninstall leftovers.

.DESCRIPTION
  RUN THIS ONLY ON A CLEAN WINDOWS MACHINE (a snapshot with no Foundation Sunshine / Sunshine on it).
  The installer has a fixed AppId and installs into the previous install directory, registers the
  SunshineService service, firewall rules and drivers. It cannot be redirected to a side directory
  without replacing an existing installation, so the script refuses to start when it finds one:
    - the service SunshineService, or
    - the uninstall entry of the installer's AppId, or
    - the default install directory.
  It never starts or stops a virtual machine and never restores a snapshot; getting a clean machine is
  the caller's job.

  Every scenario installs, checks, and uninstalls again. Evidence per scenario goes to
  <EvidenceDir>\<scenario>\ (Inno log, file list, service query, result json).
  Result: <EvidenceDir>\installer-gate.json; exit code 0 when nothing failed.

.PARAMETER NewInstaller   the installer under test
.PARAMETER OldInstallers  hashtable label -> path of older installers to upgrade from, e.g.
                          @{ 'v2026.1031' = 'C:\gate\old\1031.exe'; 'v2026.1114.173055' = 'C:\gate\old\1114.exe' }
.PARAMETER CleanMachineConfirmed  required: the caller states this machine is disposable
#>
param(
  [Parameter(Mandatory)] [string] $NewInstaller,
  [hashtable] $OldInstallers = @{},
  [Parameter(Mandatory)] [string] $EvidenceDir,
  [Parameter(Mandatory)] [switch] $CleanMachineConfirmed,
  [string[]] $OptionalComponents = @('vdd', 'firewall', 'autostart', 'gamepad', 'usbip', 'vmouse', 'tools')
)
$ErrorActionPreference = 'Stop'
$AppIdKey = 'Microsoft\Windows\CurrentVersion\Uninstall\{B16E5C0D-7B8A-4E3B-9F1A-2D4C6E8F0A1B}_is1'
$DefaultDir = Join-Path $env:ProgramFiles 'Sunshine'
$results = New-Object System.Collections.Generic.List[object]
function Add-Result($id, $status, $detail) { $results.Add([pscustomobject]@{ id = $id; status = $status; detail = "$detail" }); Write-Host ("[{0}] {1} {2}" -f $status.ToUpper(), $id, $detail) }

function Test-Clean {
  $why = @()
  if (Get-Service -Name SunshineService -ErrorAction SilentlyContinue) { $why += 'service SunshineService exists' }
  if (Test-Path "HKLM:\SOFTWARE\$AppIdKey") { $why += 'installer AppId is registered' }
  if (Test-Path "HKLM:\SOFTWARE\WOW6432Node\$AppIdKey") { $why += 'installer AppId is registered (WOW6432Node)' }
  if (Test-Path $DefaultDir) { $why += "$DefaultDir exists" }
  $why
}
$dirty = Test-Clean
if ($dirty) { throw ("Not a clean machine, refusing to run: " + ($dirty -join '; ')) }

# files that must exist per component (paths relative to the install directory; from cmake/packaging/sunshine.iss.in)
$files = @{
  application = @('Sunshine.exe', 'tools\sunshinesvc.exe', 'scripts\install-service.bat', 'scripts\uninstall-service.bat')
  assets      = @('assets', 'tools\usbip-win2-root.ps1', 'tools\restart64.exe')
  gui         = @('assets\gui\sunshine-gui.exe')
  vdd         = @('scripts\install-vdd.bat', 'scripts\uninstall-vdd.bat')
  firewall    = @('scripts\add-firewall-rule.bat', 'scripts\delete-firewall-rule.bat')
  autostart   = @('scripts\autostart-service.bat')
  gamepad     = @('scripts\install-gamepad.bat', 'scripts\gamepad')
  usbip       = @('scripts\install-usbip.bat', 'scripts\usbip')
  vmouse      = @('scripts\vmouse\install-vmouse.bat')
  tools       = @('tools\dxgi-info.exe', 'tools\audio-info.exe')
}

function Install-Inno($exe, $args2, $dir) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $log = Join-Path $dir 'inno.log'
  $p = Start-Process -FilePath $exe -ArgumentList (@('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/LOG=`"$log`"") + $args2) -PassThru -Wait
  [pscustomobject]@{ Code = $p.ExitCode; Log = $log }
}

function Uninstall-Inno($dir) {
  $u = Join-Path $DefaultDir 'unins000.exe'
  if (-not (Test-Path $u)) { return [pscustomobject]@{ Code = -1; Log = '' } }
  $log = Join-Path $dir 'uninstall.log'
  $p = Start-Process -FilePath $u -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$log`"") -PassThru -Wait
  [pscustomobject]@{ Code = $p.ExitCode; Log = $log }
}

function Check-Install($scn, $dir, $components, $installRc) {
  # 0 = ok, 3010 = restart recommended (drivers)
  Add-Result "$scn.exit-code" $(if ($installRc.Code -in 0, 3010) { 'pass' } else { 'fail' }) "installer exit $($installRc.Code)"
  $bad = @(Select-String -Path $installRc.Log -Pattern 'Exception|Failed to|Error:' -ErrorAction SilentlyContinue)
  Add-Result "$scn.inno-log" $(if ($bad.Count -eq 0) { 'pass' } else { 'fail' }) $(if ($bad.Count) { $bad[0].Line.Trim() } else { 'no exception lines' })
  foreach ($c in $components) {
    foreach ($f in $files[$c]) {
      $ok = Test-Path (Join-Path $DefaultDir $f)
      if (-not $ok) { Add-Result "$scn.file.$c" 'fail' "missing $f" }
    }
  }
  Add-Result "$scn.files" 'pass' ("checked components: " + ($components -join ','))
  $svc = Get-Service -Name SunshineService -ErrorAction SilentlyContinue
  Add-Result "$scn.service" $(if ($svc) { 'pass' } else { 'fail' }) $(if ($svc) { "$($svc.Status), start type $((Get-CimInstance Win32_Service -Filter "Name='SunshineService'").StartMode)" } else { 'SunshineService not registered' })
  Get-ChildItem $DefaultDir -Recurse -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName | Set-Content (Join-Path $dir 'files.txt')
  $resUsb = Join-Path $DefaultDir 'scripts\result-usbip.txt'
  if (Test-Path $resUsb) { Add-Result "$scn.usbip-result" 'info' ("install-usbip result file: " + (Get-Content $resUsb -Raw).Trim()) }
}

function Check-Uninstalled($scn, $dir) {
  $left = @(); if (Get-Service -Name SunshineService -ErrorAction SilentlyContinue) { $left += 'service' }
  if (Test-Path "HKLM:\SOFTWARE\$AppIdKey") { $left += 'uninstall registry entry' }
  # the user's config may stay on purpose; every other file under the install directory must be gone
  $rest = @(); if (Test-Path $DefaultDir) { $rest = @(Get-ChildItem $DefaultDir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch '\\config\\' }) }
  if ($rest.Count) { $left += ("{0} file(s) left, first {1}" -f $rest.Count, $rest[0].FullName) }
  $fw = & netsh advfirewall firewall show rule name=all 2>$null | Select-String -Pattern 'Sunshine' -SimpleMatch
  if ($fw) { $left += 'firewall rule(s)' }
  Add-Result "$scn.uninstall-clean" $(if ($left.Count -eq 0) { 'pass' } else { 'fail' }) $(if ($left.Count) { $left -join '; ' } else { 'service, registry, files, firewall rules all gone' })
  # remove what a failed uninstall left so the next scenario starts clean (only our own service and directory)
  if ($left.Count) {
    if (Get-Service -Name SunshineService -ErrorAction SilentlyContinue) { & sc.exe stop SunshineService | Out-Null; & sc.exe delete SunshineService | Out-Null }
    if (Test-Path $DefaultDir) { Remove-Item -Recurse -Force $DefaultDir -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force "HKLM:\SOFTWARE\$AppIdKey" -ErrorAction SilentlyContinue
  }
}

function Run-Scenario($scn, [scriptblock]$body) {
  $dir = Join-Path $EvidenceDir $scn
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $d = Test-Clean
  if ($d) { Add-Result "$scn.precondition" 'fail' ("machine not clean before scenario: " + ($d -join '; ')); return }
  try { & $body $dir } catch { Add-Result "$scn.error" 'fail' $_.Exception.Message; Uninstall-Inno $dir | Out-Null }
}

# ---- scenarios -----------------------------------------------------------------------------------------
Run-Scenario 'fresh-recommended' {
  param($dir)
  $r = Install-Inno $NewInstaller @('/TYPE=recommended') $dir
  Check-Install 'fresh-recommended' $dir @('application', 'assets', 'gui', 'vdd', 'firewall', 'autostart', 'gamepad', 'usbip', 'tools') $r
  $u = Uninstall-Inno $dir; Add-Result 'fresh-recommended.uninstall-exit' $(if ($u.Code -eq 0) { 'pass' } else { 'fail' }) "exit $($u.Code)"
  Check-Uninstalled 'fresh-recommended' $dir
}
Run-Scenario 'fresh-full' {
  param($dir)
  $r = Install-Inno $NewInstaller @('/TYPE=full') $dir
  Check-Install 'fresh-full' $dir @($files.Keys) $r
  # USB/IP guard: installing the same version again must leave the transport alone (a reinstall detaches attached devices)
  $r2 = Install-Inno $NewInstaller @('/TYPE=full') (Join-Path $dir 'again')
  $note = if (Test-Path (Join-Path $DefaultDir 'scripts\result-usbip.txt')) { (Get-Content (Join-Path $DefaultDir 'scripts\result-usbip.txt') -Raw).Trim() } else { '' }
  Add-Result 'fresh-full.usbip-guard-reinstall' $(if ($r2.Code -in 0, 3010 -and $note -in '0') { 'pass' } else { 'fail' }) "second install exit $($r2.Code), usbip result '$note'"
  $u = Uninstall-Inno $dir; Add-Result 'fresh-full.uninstall-exit' $(if ($u.Code -eq 0) { 'pass' } else { 'fail' }) "exit $($u.Code)"
  Check-Uninstalled 'fresh-full' $dir
}
Run-Scenario 'fresh-compact' {
  param($dir)
  $r = Install-Inno $NewInstaller @('/TYPE=compact') $dir
  Check-Install 'fresh-compact' $dir @('application', 'assets', 'gui', 'firewall') $r
  $u = Uninstall-Inno $dir; Add-Result 'fresh-compact.uninstall-exit' $(if ($u.Code -eq 0) { 'pass' } else { 'fail' }) "exit $($u.Code)"
  Check-Uninstalled 'fresh-compact' $dir
}
foreach ($c in $OptionalComponents) {
  Run-Scenario "component-$c" {
    param($dir)
    $r = Install-Inno $NewInstaller @("/COMPONENTS=`"application,assets,gui,$c`"") $dir
    Check-Install "component-$c" $dir @('application', 'assets', 'gui', $c) $r
    $u = Uninstall-Inno $dir; Add-Result "component-$c.uninstall-exit" $(if ($u.Code -eq 0) { 'pass' } else { 'fail' }) "exit $($u.Code)"
    Check-Uninstalled "component-$c" $dir
  }
}
foreach ($label in $OldInstallers.Keys) {
  $scn = "upgrade-from-$label"
  Run-Scenario $scn {
    param($dir)
    $o = Install-Inno $OldInstallers[$label] @('/TYPE=recommended') (Join-Path $dir 'old')
    Add-Result "$scn.old-install" $(if ($o.Code -in 0, 3010) { 'pass' } else { 'fail' }) "old installer exit $($o.Code)"
    # the owner's settings must survive: leave a marker in the config folder and look for it after the upgrade
    New-Item -ItemType Directory -Force -Path (Join-Path $DefaultDir 'config') | Out-Null
    'marker' | Set-Content (Join-Path $DefaultDir 'config\release-gate-marker.txt')
    $r = Install-Inno $NewInstaller @('/TYPE=recommended') $dir
    Check-Install $scn $dir @('application', 'assets', 'gui', 'vdd', 'firewall', 'autostart', 'gamepad', 'usbip', 'tools') $r
    Add-Result "$scn.config-kept" $(if (Test-Path (Join-Path $DefaultDir 'config\release-gate-marker.txt')) { 'pass' } else { 'fail' }) 'config folder content survives the upgrade'
    $u = Uninstall-Inno $dir; Add-Result "$scn.uninstall-exit" $(if ($u.Code -eq 0) { 'pass' } else { 'fail' }) "exit $($u.Code)"
    Check-Uninstalled $scn $dir
  }
}

New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$results | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir 'installer-gate.json')
$failed = @($results | Where-Object { $_.status -eq 'fail' }).Count
Write-Host "done: $($results.Count) checks, $failed failed"
exit $(if ($failed -gt 0) { 1 } else { 0 })
