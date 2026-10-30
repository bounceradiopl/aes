# SPDX-License-Identifier: GPL-3.0-or-later
# Manual installation of BounceAES Master without the MSI (e.g. for testing a CI build).
# Run in an elevated PowerShell from the folder that contains the binaries:
#   powershell -ExecutionPolicy Bypass -File install.ps1
#Requires -RunAsAdministrator
param([string]$Source = $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$Dest = Join-Path $env:ProgramFiles 'BounceAES Master'
$Service = 'BounceAESMaster'
$Clsid = '{6B1F3C2A-8E4D-4B7A-9C51-2A7E0D4B3F10}'

Write-Host "Instalacja BounceAES Master do $Dest"

if (Get-Service $Service -ErrorAction SilentlyContinue) {
    Stop-Service $Service -Force -ErrorAction SilentlyContinue
}
Get-Process BounceAESTray -ErrorAction SilentlyContinue | Stop-Process -Force

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
foreach ($f in 'BounceAESService.exe', 'BounceAESAsio.dll', 'BounceAESTray.exe') {
    Copy-Item (Join-Path $Source $f) $Dest -Force
}

# Service with automatic restart after a failure (5 s, three times)
$exe = Join-Path $Dest 'BounceAESService.exe'
if (-not (Get-Service $Service -ErrorAction SilentlyContinue)) {
    sc.exe create $Service binPath= "`"$exe`"" start= auto DisplayName= "BounceAES Master" | Out-Null
}
sc.exe description $Service "Strumienie audio AES67, zegar PTP i panel WWW dla sterownika ASIO BounceAES Master." | Out-Null
sc.exe failure $Service reset= 86400 actions= restart/5000/restart/5000/restart/5000 | Out-Null

# ASIO driver registration (COM + ASIO driver list)
& regsvr32.exe /s (Join-Path $Dest 'BounceAESAsio.dll')
if (-not (Test-Path "HKLM:\SOFTWARE\ASIO\BounceAES Master")) { throw "Rejestracja sterownika ASIO nie powiodła się" }

# Event Log source
$evt = 'HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\BounceAES Master'
New-Item -Force -Path $evt | Out-Null
New-ItemProperty -Force -Path $evt -Name EventMessageFile -PropertyType ExpandString `
    -Value '%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\EventLogMessages.dll' | Out-Null
New-ItemProperty -Force -Path $evt -Name TypesSupported -PropertyType DWord -Value 7 | Out-Null

# Firewall: allow the service (RTP 5004, PTP 319/320, SAP 9875, panel 8067)
netsh advfirewall firewall delete rule name="BounceAES Master" | Out-Null
netsh advfirewall firewall add rule name="BounceAES Master" dir=in action=allow program="$exe" enable=yes profile=any | Out-Null

# Tray application at logon
New-ItemProperty -Force -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name 'BounceAES Master' `
    -Value "`"$(Join-Path $Dest 'BounceAESTray.exe')`"" | Out-Null

# Windows' own PTP client would compete for UDP 319/320
$ptp = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\PtpClient' -ErrorAction SilentlyContinue
if ($ptp -and $ptp.Enabled -eq 1) {
    Write-Warning "Włączony jest klient PTP usługi w32time. Wyłącz go: reg add HKLM\SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\PtpClient /v Enabled /t REG_DWORD /d 0 /f, potem restart w32time."
}

Start-Service $Service
Start-Process (Join-Path $Dest 'BounceAESTray.exe')
Write-Host "Gotowe. Panel: http://127.0.0.1:8067 (przy pierwszym otwarciu ustaw hasło)."
