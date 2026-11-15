<#
.SYNOPSIS
  Read-only scan of a host log and its crash-report folder for the signatures the release gate cares about.
  Writes nothing except <EvidenceDir>\log-scan.txt (a copy of the matching lines). Safe to point at a live
  installation: it only reads.

  Emits objects { id, status, detail } (pass | fail | info), also usable by windows-host-smoke.ps1.

  Signatures
    log.no-terminate        no "terminate called" / "std::terminate" / unhandled-exception line in the log
    log.no-host-events-400  no bad_envelope / HTTP 400 answer to a diagnostics upload in the log (the host may not log it:
                            the authoritative check is the server side, see report-envelope-probe.py)
    crash.none              the crash folder holds no pending report; a report whose signature mentions
                            abort/terminate with the service wrapper frame 0x48a86 is the false crash of the
                            2026-11-14 prerelease and is named as such
    enc.lines (info)        encoder probe lines (which codecs the machine could encode)
#>
param(
  [Parameter(Mandatory)] [string] $LogPath,
  [string] $CrashDir = '',
  [string] $EvidenceDir = ''
)
function New-Res($id, $status, $detail) { [pscustomobject]@{ id = $id; status = $status; detail = "$detail" } }
$out = @()
if (-not (Test-Path $LogPath)) { return @(New-Res 'log.present' 'fail' "no log at $LogPath") }
$lines = Get-Content -LiteralPath $LogPath -Encoding UTF8 -ErrorAction SilentlyContinue
$out += New-Res 'log.present' 'pass' ("{0} lines" -f $lines.Count)

$term = @($lines | Select-String -Pattern 'terminate called|std::terminate|Unhandled exception|abort\(\) has been called' -SimpleMatch:$false)
$out += if ($term.Count) { New-Res 'log.no-terminate' 'fail' ("{0} line(s), first: {1}" -f $term.Count, $term[0].Line.Trim()) } else { New-Res 'log.no-terminate' 'pass' '' }

$e400 = @($lines | Select-String -Pattern 'bad_envelope|(telemetry|diagnostic|upload|v1/events|host-events)[^\r\n]{0,120}(HTTP|status|code)[ =:/]*400\b')
$out += if ($e400.Count) { New-Res 'log.no-host-events-400' 'fail' ("{0} line(s), first: {1}" -f $e400.Count, $e400[0].Line.Trim()) } else { New-Res 'log.no-host-events-400' 'pass' '' }

if ($CrashDir) {
  if (Test-Path $CrashDir) {
    $metas = @(Get-ChildItem -LiteralPath $CrashDir -Filter *.json -ErrorAction SilentlyContinue)
    if ($metas.Count -eq 0) { $out += New-Res 'crash.none' 'pass' 'crash folder is empty' }
    else {
      # the false crash of the 2026-11-14 prerelease: the service wrapper's static watchdog thread destroyed while joinable
      # (exception "terminate" in sunshinesvc.exe; the frame offsets differ between builds, so match the module and the kind)
      $svcTerm = 0
      foreach ($m in $metas) {
        $j = $null; try { $j = Get-Content -LiteralPath $m.FullName -Raw -Encoding UTF8 | ConvertFrom-Json } catch { }
        if ($j -and $j.module -eq 'sunshinesvc.exe' -and $j.exception_code -eq 'terminate') { $svcTerm++ }
      }
      $out += New-Res 'crash.none' 'fail' ("{0} pending report(s); {1} are the false service-wrapper terminate (sunshinesvc.exe, exception terminate)" -f $metas.Count, $svcTerm)
    }
  } else { $out += New-Res 'crash.none' 'pass' 'no crash folder (nothing was ever written)' }
}

$enc = @($lines | Select-String -Pattern 'Trying encoder|Encoder \[|HEVC|AV1|HDR|Found (H264|HEVC|AV1)|ServerCodecModeSupport|NVENC|AMF|QuickSync')
$out += New-Res 'enc.lines' 'info' ("{0} encoder-related line(s)" -f $enc.Count)
if ($EvidenceDir) {
  New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
  $keep = @(); $keep += $term | ForEach-Object { 'TERMINATE ' + $_.Line }; $keep += $e400 | ForEach-Object { 'E400 ' + $_.Line }; $keep += $enc | Select-Object -First 80 | ForEach-Object { 'ENC ' + $_.Line }
  $keep | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir 'log-scan.txt')
}
$out
