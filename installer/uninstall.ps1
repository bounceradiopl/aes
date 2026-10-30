# SPDX-License-Identifier: GPL-3.0-or-later
# Removes a BounceAES Master installed with install.ps1. Configuration in %ProgramData% is kept.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$Dest = Join-Path $env:ProgramFiles 'BounceAES Master'
$Service = 'BounceAESMaster'

Get-Process BounceAESTray -ErrorAction SilentlyContinue | Stop-Process -Force
if (Get-Service $Service -ErrorAction SilentlyContinue) {
    Stop-Service $Service -Force
    sc.exe delete $Service | Out-Null
}
$dll = Join-Path $Dest 'BounceAESAsio.dll'
if (Test-Path $dll) { & regsvr32.exe /s /u $dll }
netsh advfirewall firewall delete rule name="BounceAES Master" | Out-Null
Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name 'BounceAES Master' -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force 'HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\BounceAES Master' -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $Dest -ErrorAction SilentlyContinue
Write-Host "Usunięto BounceAES Master. Konfiguracja pozostała w $env:ProgramData\BounceAES Master."
