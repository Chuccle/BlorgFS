<#
.SYNOPSIS
    Turns a freshly installed Windows into the BlorgFS test guest's golden
    image. Runs once, inside the guest, from the autounattend first logon.

.DESCRIPTION
    Everything a test run would otherwise have to do (and reboot for) is
    done here once, so a run starts from a guest that can load a test-signed
    driver straight away:

      - OpenSSH server, key-only, PowerShell as the default shell. This is
        the channel guestctl and agents use to act in the guest.
      - QEMU guest agent (when the virtio-win ISO is attached), the fallback
        channel for when the guest network is what broke.
      - Driver Verifier's standard checks on BlorgFS.sys, the default for
        a test run, so the run does not have to reboot to apply them.
      - Test signing on, and boot-failure recovery off, so a bugcheck
        reboots straight back into Windows instead of parking at a recovery
        menu nobody can see.
      - Kernel crash dumps kept on C:, where Get-GuestDiagnostics.ps1 finds
        them.
      - Windows Update off, high-performance power plan: a run should not
        compete with a servicing stack or fall asleep.

    It powers the machine off when done, success or not. build-image.sh
    waits for that power-off and then boots the image once more to check
    the result over SSH, so a failure here surfaces there rather than as a
    hung build.

.PARAMETER ConfigDrive
    The config ISO's drive, e.g. "E:". Holds this script, the OpenSSH zip
    and authorized_keys.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ConfigDrive
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Start-Transcript -Path 'C:\blorgfs-image.log' -Append | Out-Null

$script:Failed = @()

function Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Host "==> $Name" -ForegroundColor Cyan
    try {
        & $Body
    } catch {
        Write-Host "FAILED: $Name -- $($_.Exception.Message)" -ForegroundColor Red
        $script:Failed += $Name
    }
}

function Invoke-Native {
    param([string]$File, [string[]]$Arguments)
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File $($Arguments -join ' ') exited $LASTEXITCODE" }
}

New-Item -ItemType Directory -Force -Path 'C:\blorgfs-ci' | Out-Null

Step 'Execution policy' {
    Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope LocalMachine -Force
}

Step 'OpenSSH server' {
    $zip = Join-Path $ConfigDrive 'OpenSSH-Win64.zip'
    $dest = 'C:\Program Files\OpenSSH'
    Expand-Archive -Path $zip -DestinationPath 'C:\Program Files' -Force
    if (Test-Path 'C:\Program Files\OpenSSH-Win64') {
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
        Rename-Item 'C:\Program Files\OpenSSH-Win64' 'OpenSSH'
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $dest 'install-sshd.ps1')
    if ($LASTEXITCODE -ne 0) { throw "install-sshd.ps1 exited $LASTEXITCODE" }

    # First start writes %ProgramData%\ssh\sshd_config and the host keys.
    Set-Service -Name sshd -StartupType Automatic
    Start-Service sshd
    Stop-Service sshd

    # Administrators read their keys from one machine-wide file (the
    # stock sshd_config's "Match Group administrators" block), and sshd
    # ignores that file unless only Administrators and SYSTEM can touch it.
    $keys = 'C:\ProgramData\ssh\administrators_authorized_keys'
    Copy-Item (Join-Path $ConfigDrive 'authorized_keys') $keys -Force
    Invoke-Native icacls.exe @($keys, '/inheritance:r', '/grant', 'Administrators:F', '/grant', 'SYSTEM:F')

    $cfg = 'C:\ProgramData\ssh\sshd_config'
    $text = Get-Content $cfg -Raw
    $text = $text -replace '(?m)^#?\s*PasswordAuthentication\s+\S+', 'PasswordAuthentication no'
    $text = $text -replace '(?m)^#?\s*PubkeyAuthentication\s+\S+', 'PubkeyAuthentication yes'
    Set-Content -Path $cfg -Value $text -Encoding ascii

    New-Item -Path 'HKLM:\SOFTWARE\OpenSSH' -Force | Out-Null
    New-ItemProperty -Path 'HKLM:\SOFTWARE\OpenSSH' -Name DefaultShell -PropertyType String -Force `
        -Value 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' | Out-Null

    if (-not (Get-NetFirewallRule -Name 'BlorgFS-SSH' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name 'BlorgFS-SSH' -DisplayName 'OpenSSH (BlorgFS test guest)' `
            -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow -Profile Any | Out-Null
    }
    Start-Service sshd
}

Step 'QEMU guest agent (optional)' {
    $virtio = Get-PSDrive -PSProvider FileSystem |
        ForEach-Object { $_.Root } |
        Where-Object { Test-Path (Join-Path $_ 'guest-agent\qemu-ga-x86_64.msi') } |
        Select-Object -First 1
    if (-not $virtio) {
        Write-Host '    virtio-win ISO not attached; skipping (SSH is the primary channel)'
        return
    }
    # vioserial carries the agent's channel; without it the service starts
    # and never sees its port. The 2k22 build also loads on 2025.
    $inf = Get-ChildItem (Join-Path $virtio 'vioserial') -Recurse -Filter 'vioser.inf' |
        Where-Object { $_.FullName -match '\\2k22\\amd64\\' } | Select-Object -First 1
    if ($inf) { Invoke-Native pnputil.exe @('/add-driver', $inf.FullName, '/install') }
    $msi = Join-Path $virtio 'guest-agent\qemu-ga-x86_64.msi'
    $p = Start-Process msiexec.exe -ArgumentList @('/i', "`"$msi`"", '/qn', '/norestart') -Wait -PassThru
    if ($p.ExitCode -notin 0, 3010) { throw "qemu-ga install exited $($p.ExitCode)" }
}

Step 'Test signing, and no recovery menu after a crash' {
    Invoke-Native bcdedit.exe @('/set', '{current}', 'testsigning', 'on')
    Invoke-Native bcdedit.exe @('/set', '{current}', 'bootstatuspolicy', 'ignoreallfailures')
    Invoke-Native bcdedit.exe @('/set', '{current}', 'recoveryenabled', 'no')
    Invoke-Native bcdedit.exe @('/timeout', '0')
}

# Keyed by image name, so it can be set before the driver exists. Done
# here so a default (verifier-on) run needs no reboot before its tests:
# Prepare-Guest.ps1 only reboots when a run asks for something else.
Step 'Driver Verifier on BlorgFS.sys' {
    verifier.exe /standard /driver BlorgFS.sys | Out-Null
}

Step 'Kernel crash dumps' {
    $k = 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
    Set-ItemProperty $k CrashDumpEnabled 2      # kernel memory dump
    Set-ItemProperty $k AutoReboot 1
    Set-ItemProperty $k Overwrite 1
    Set-ItemProperty $k AlwaysKeepMemoryDump 1  # disk-space pressure must not eat the evidence
    Set-ItemProperty $k MinidumpsCount 50
}

Step 'Windows Update off' {
    foreach ($svc in 'wuauserv', 'UsoSvc', 'WaaSMedicSvc') {
        try { Stop-Service $svc -Force -ErrorAction SilentlyContinue } catch { Write-Verbose "stop $svc failed" }
        # WaaSMedicSvc refuses Set-Service; the registry is the only lever.
        Set-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$svc" Start 4 -ErrorAction SilentlyContinue
    }
    $au = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU'
    New-Item -Path $au -Force | Out-Null
    Set-ItemProperty $au NoAutoUpdate 1
}

Step 'Power and shutdown prompts' {
    Invoke-Native powercfg.exe @('/setactive', 'SCHEME_MIN')
    Invoke-Native powercfg.exe @('/hibernate', 'off')
    $r = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Reliability'
    New-Item -Path $r -Force | Out-Null
    Set-ItemProperty $r ShutdownReasonOn 0
}

Step 'Record image info' {
    $os = Get-CimInstance Win32_OperatingSystem
    [ordered]@{
        built      = (Get-Date).ToUniversalTime().ToString('o')
        caption    = $os.Caption
        version    = $os.Version
        build      = $os.BuildNumber
        failedSteps = $script:Failed
    } | ConvertTo-Json | Set-Content -Path 'C:\blorgfs-ci\image-info.json' -Encoding ascii
}

Step 'Trim free space so the image compresses' {
    Remove-Item -Recurse -Force "$env:TEMP\*" -ErrorAction SilentlyContinue
    Optimize-Volume -DriveLetter C -ReTrim
}

if ($script:Failed.Count) {
    Write-Host "Golden image setup finished with failures: $($script:Failed -join ', ')" -ForegroundColor Red
} else {
    Write-Host 'Golden image setup finished.' -ForegroundColor Green
}
Stop-Transcript | Out-Null
Stop-Computer -Force
