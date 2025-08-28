# Force UTF-8 in PowerShell and persist to profiles

# 1) Content to write into profiles
$fix = @'
$enc = New-Object System.Text.UTF8Encoding($false)
[Console]::InputEncoding  = $enc
[Console]::OutputEncoding = $enc
try { chcp 65001 > $null } catch {}

# Default UTF-8 for common cmdlets
$PSDefaultParameterValues['*:Encoding']           = 'utf8'
$PSDefaultParameterValues['Out-File:Encoding']    = 'utf8'
$PSDefaultParameterValues['Set-Content:Encoding'] = 'utf8'
$PSDefaultParameterValues['Add-Content:Encoding'] = 'utf8'
$PSDefaultParameterValues['Get-Content:Encoding'] = 'utf8'
'@

# 2) Allow profile loading for current user (no system-wide change)
try { Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned -Force } catch {}

# 3) Write to CurrentUserAllHosts (applies to most hosts)
$allHosts = $PROFILE.CurrentUserAllHosts
$allDir   = Split-Path $allHosts
if (-not (Test-Path $allDir)) { New-Item -Path $allDir -ItemType Directory -Force | Out-Null }

# 4) Also write to CurrentUserCurrentHost (extra safety)
$curHost = $PROFILE.CurrentUserCurrentHost
$curDir  = Split-Path $curHost
if (-not (Test-Path $curDir)) { New-Item -Path $curDir -ItemType Directory -Force | Out-Null }

# Pick an encoding name that works across PS versions
# PS 7+: use utf8BOM to ensure BOM; PS 5.1: 'utf8' writes with BOM
$encName = if ($PSVersionTable.PSVersion.Major -ge 6) { 'utf8BOM' } else { 'utf8' }

$fix | Set-Content -Path $allHosts -Encoding $encName
$fix | Set-Content -Path $curHost  -Encoding $encName

"`nWritten profiles:"
" - $allHosts"
" - $curHost"
"`nRestart PowerShell (or the Windows Terminal tab), then run:"
"  cat .\README.md"
"  [Console]::OutputEncoding"
"  chcp"
