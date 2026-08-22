$ErrorActionPreference = "Stop"

# $PSScriptRoot is this script's own directory -- keeps the whole thing
# portable regardless of who checks it out or where, rather than a path
# specific to one machine/session.
$rfcPath = Join-Path $PSScriptRoot "testdata\rfc8448.txt"
$lines = Get-Content -Path $rfcPath -Encoding UTF8

$junkMarkers = @("Thomson", "RFC 8448", "[Page")

function Extract-Hex($startLine, $endLine, $labelLine) {
    $hexPairs = New-Object System.Collections.Generic.List[string]
    for ($i = $startLine - 1; $i -lt $endLine; $i++) {
        $line = $lines[$i]
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $isJunk = $false
        foreach ($m in $junkMarkers) { if ($line -match [regex]::Escape($m)) { $isJunk = $true; break } }
        if ($isJunk) { continue }
        if ($labelLine -ne $null -and $i -eq ($labelLine - 1)) {
            $idx = $line.IndexOf("):")
            if ($idx -ne -1) { $line = $line.Substring($idx + 2) }
        }
        $matches = [regex]::Matches($line, '\b[0-9a-fA-F]{2}\b')
        foreach ($m in $matches) { $hexPairs.Add($m.Value.ToLower()) }
    }
    return ($hexPairs -join "")
}

$vectors = [ordered]@{
    "client_hello_196"        = Extract-Hex 177 186 177
    "server_hello_90"         = Extract-Hex 241 245 241
    "p256_client_private"     = Extract-Hex 1700 1701 1700
    "p256_client_public"      = Extract-Hex 1703 1706 1703
    "p256_server_private"     = Extract-Hex 1818 1819 1818
    "p256_server_public"      = Extract-Hex 1821 1824 1821
    "p256_shared_secret_ikm"  = Extract-Hex 1863 1864 1863
    "early_secret"            = Extract-Hex 219 220 219
    "empty_hash"              = Extract-Hex 252 253 252
    "derived_for_handshake"   = Extract-Hex 259 260 259
    "shared_secret_ikm"       = Extract-Hex 267 268 267
    "handshake_secret"        = Extract-Hex 270 271 270
    "transcript_hash_ch_sh"   = Extract-Hex 287 288 287
    "client_hs_traffic_secret"= Extract-Hex 294 295 294
    "server_hs_traffic_secret"= Extract-Hex 309 310 309
    "derived_for_master"      = Extract-Hex 324 325 324
    "master_secret"           = Extract-Hex 343 344 343
    "server_hs_write_key"     = Extract-Hex 367 368 367
    "server_hs_write_iv"      = Extract-Hex 372 372 372
    "finished_key"            = Extract-Hex 433 434 433
    "finished_mac"            = Extract-Hex 436 437 436
    "payload_657"             = Extract-Hex 457 488 457
    "complete_record_679"     = Extract-Hex 490 530 490
}

$expectedLengths = @{
    "client_hello_196" = 196; "server_hello_90" = 90;
    "p256_client_private" = 32; "p256_client_public" = 65;
    "p256_server_private" = 32; "p256_server_public" = 65;
    "p256_shared_secret_ikm" = 32;
    "early_secret" = 32; "empty_hash" = 32; "derived_for_handshake" = 32;
    "shared_secret_ikm" = 32; "handshake_secret" = 32; "transcript_hash_ch_sh" = 32;
    "client_hs_traffic_secret" = 32; "server_hs_traffic_secret" = 32;
    "derived_for_master" = 32; "master_secret" = 32;
    "server_hs_write_key" = 16; "server_hs_write_iv" = 12;
    "finished_key" = 32; "finished_mac" = 32;
    "payload_657" = 657; "complete_record_679" = 679;
}

$ok = $true
foreach ($name in $vectors.Keys) {
    $hexstr = $vectors[$name]
    $actual = $hexstr.Length / 2
    $expected = $expectedLengths[$name]
    $status = if ($actual -eq $expected) { "OK" } else { "MISMATCH"; $ok = $false }
    Write-Host "$name : $actual bytes (expected $expected) $status"
}

if (-not $ok) {
    Write-Host "LENGTH MISMATCHES FOUND -- fix line ranges before trusting output"
    exit 1
}

Write-Host "ALL LENGTHS OK"

New-Item -ItemType Directory -Force -Path $PSScriptRoot | Out-Null

# snake_case -> PascalCase struct field names (namespaced via the Rfc8448
# struct rather than a pile of g_-prefixed globals, per project convention).
$fieldNames = [ordered]@{
    "client_hello_196"         = "ClientHello196"
    "server_hello_90"          = "ServerHello90"
    "p256_client_private"      = "P256ClientPrivate"
    "p256_client_public"       = "P256ClientPublic"
    "p256_server_private"      = "P256ServerPrivate"
    "p256_server_public"       = "P256ServerPublic"
    "p256_shared_secret_ikm"   = "P256SharedSecretIkm"
    "early_secret"             = "EarlySecret"
    "empty_hash"               = "EmptyHash"
    "derived_for_handshake"    = "DerivedForHandshake"
    "shared_secret_ikm"        = "SharedSecretIkm"
    "handshake_secret"         = "HandshakeSecret"
    "transcript_hash_ch_sh"    = "TranscriptHashChSh"
    "client_hs_traffic_secret" = "ClientHsTrafficSecret"
    "server_hs_traffic_secret" = "ServerHsTrafficSecret"
    "derived_for_master"       = "DerivedForMaster"
    "master_secret"            = "MasterSecret"
    "server_hs_write_key"      = "ServerHsWriteKey"
    "server_hs_write_iv"       = "ServerHsWriteIv"
    "finished_key"             = "FinishedKey"
    "finished_mac"             = "FinishedMac"
    "payload_657"              = "Payload657"
    "complete_record_679"      = "CompleteRecord679"
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("// Extracted programmatically from RFC 8448 section 3 (Simple 1-RTT")
[void]$sb.AppendLine("// Handshake, x25519 example) -- see extract_vectors.ps1. Values are used")
[void]$sb.AppendLine("// only as opaque byte strings feeding the (curve-agnostic) HKDF key")
[void]$sb.AppendLine("// schedule and AES-128-GCM record layer under test; the fact that the")
[void]$sb.AppendLine("// source example uses x25519 rather than this driver's P-256 doesn't")
[void]$sb.AppendLine("// matter here, since Stage 1 doesn't touch ECDH at all. Namespaced under")
[void]$sb.AppendLine("// one Rfc8448 struct instance rather than per-vector g_ globals -- use")
[void]$sb.AppendLine("// sizeof(Rfc8448.FieldName) for a vector's length, fixed-size arrays")
[void]$sb.AppendLine("// need no separate _len constant.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("typedef struct _RFC8448_VECTORS")
[void]$sb.AppendLine("{")
foreach ($name in $vectors.Keys) {
    $len = $vectors[$name].Length / 2
    [void]$sb.AppendLine("    unsigned char $($fieldNames[$name])[$len];")
}
[void]$sb.AppendLine("} RFC8448_VECTORS;")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const RFC8448_VECTORS Rfc8448 =")
[void]$sb.AppendLine("{")
foreach ($name in $vectors.Keys) {
    $hexstr = $vectors[$name]
    $bytePairs = for ($i = 0; $i -lt $hexstr.Length; $i += 2) { "0x" + $hexstr.Substring($i, 2) }
    $arr = $bytePairs -join ", "
    [void]$sb.AppendLine("    { $arr }, // $($fieldNames[$name])")
}
[void]$sb.AppendLine("};")

Set-Content -Path (Join-Path $PSScriptRoot "Rfc8448Vectors.h") -Value $sb.ToString() -Encoding utf8

Write-Host "Wrote TlsTest/Rfc8448Vectors.h"
