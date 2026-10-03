<#
.SYNOPSIS
    Fails unless BlorgFS.sys and BlorgFS.cat are signed by BlorgFS.cer.

.DESCRIPTION
    The package ships the build's test certificate next to the driver, and
    Install-BlorgFS.ps1 trusts exactly that certificate. If the binaries were
    signed by anything else the install fails inside the guest with an error
    that points nowhere near the cause, so check it on the build machine.

    Windows only: Authenticode signatures can only be read there. CI runs it
    in the driver build job, right after msbuild.

    Status is expected to be UnknownError or NotTrusted on a machine that
    does not trust the test cert; what is checked is who signed.

.PARAMETER BuildDir
    The driver build output, $(Platform)\$(Configuration). The .sys and .cat
    are taken from its staged BlorgFS\ package directory, as packaged.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir
)

$ErrorActionPreference = "Stop"

$cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $BuildDir "BlorgFS.cer"))
$staged = Join-Path $BuildDir "BlorgFS"

foreach ($signed in @("BlorgFS.sys", "BlorgFS.cat")) {
    $sig = Get-AuthenticodeSignature (Join-Path $staged $signed)
    if (-not $sig.SignerCertificate) { throw "$signed is not signed (status: $($sig.Status))" }
    if ($sig.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
        throw "$signed is signed by $($sig.SignerCertificate.Thumbprint), not by BlorgFS.cer ($($cert.Thumbprint))"
    }
    Write-Host "OK: $signed signed by $($cert.Subject) ($($cert.Thumbprint))"
}
