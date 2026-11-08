# SPDX-License-Identifier: GPL-3.0-or-later
# Internal code signing for BounceAES Master with the company's own self-signed certificate.
#   -CreateCert            create the certificate (CurrentUser\My) and export bounce-codesign.cer
#   -Path <folder>         sign every .exe and .dll in the folder, except libStereoTool_*.dll (Thimeo's
#                          file stays as published: its SHA-256 is pinned and named in StereoTool-NOTICE.txt)
#   -Trust -Cer <file>     trust the certificate on this computer (administrator)
param(
  [string]$Path,
  [switch]$CreateCert,
  [switch]$Trust,
  [string]$Cer = 'bounce-codesign.cer',
  [string]$Subject = 'CN=BOUNCE SP. Z O.O., O=BOUNCE SP. Z O.O., C=PL',
  [string]$TimestampServer = ''   # optional, e.g. http://timestamp.digicert.com (sends file hashes to that service)
)
$ErrorActionPreference = 'Stop'

if ($CreateCert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -KeyAlgorithm RSA -KeyLength 3072 `
    -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
  Export-Certificate -Cert $cert -FilePath $Cer | Out-Null
  Write-Host "Utworzono certyfikat $($cert.Thumbprint), część publiczna: $Cer"
}

if ($Trust) {
  foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher') {
    Import-Certificate -FilePath $Cer -CertStoreLocation $store | Out-Null
  }
  Write-Host "Certyfikat $Cer jest zaufany na tym komputerze"
}

if ($Path) {
  $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.Subject -eq $Subject } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
  if (-not $cert) { throw "Brak certyfikatu '$Subject' w CurrentUser\My (użyj -CreateCert)" }
  $files = Get-ChildItem -Path $Path -Include *.exe, *.dll -Recurse
  foreach ($f in $files) {
    if ($f.Name -like 'libStereoTool_*.dll') {
      Write-Host ("{0,-40} {1}" -f $f.Name, 'pominięty (plik Thimeo)')
      continue
    }
    $args = @{ FilePath = $f.FullName; Certificate = $cert; HashAlgorithm = 'SHA256' }
    if ($TimestampServer) { $args.TimestampServer = $TimestampServer }
    $r = Set-AuthenticodeSignature @args
    Write-Host ("{0,-40} {1}" -f $f.Name, $r.Status)
  }
}
