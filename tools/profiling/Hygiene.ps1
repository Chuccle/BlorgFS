'verifier: ' + ((verifier /querysettings) -join ' ' -replace '\s+', ' ')
sc.exe query BlorgFS | Select-String STATE
'B: mounted: ' + (Test-Path 'B:\')
Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\BlorgFS\Parameters | Format-List
try { 'defender realtime: ' + (Get-MpComputerStatus).RealTimeProtectionEnabled } catch { 'defender: n/a' }
fltmc instances 2>$null | Select-String -SimpleMatch 'B:'
