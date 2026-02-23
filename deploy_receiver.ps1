# deploy_receiver.ps1 — Sign, install, and set up auto-start for the receiver.
# Run elevated (right-click -> Run as Administrator, or from an admin terminal).
#
# Prerequisites: build.bat must have been run first (receiver.exe in the same folder).
# Requires Windows SDK (for signtool.exe).

#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$src        = "$PSScriptRoot\receiver.exe"
$installDir = "$env:ProgramFiles\KMReceiver"
$dest       = "$installDir\receiver.exe"
$certName   = 'KM Receiver Code Signing'
$startupDir = [Environment]::GetFolderPath('Startup')
$shortcutPath = Join-Path $startupDir 'KM Receiver.lnk'

if (-not (Test-Path $src)) {
    Write-Error "receiver.exe not found in $PSScriptRoot. Run build.bat first."
    exit 1
}

# --- 1. Find signtool.exe from Windows SDK ---
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe' -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) {
    Write-Error "signtool.exe not found. Install the Windows SDK (only need the signing tools)."
    exit 1
}
$signtool = $signtool.FullName
Write-Host "Using signtool: $signtool"

# --- 2. Create or reuse self-signed code-signing cert ---
$myStore = New-Object System.Security.Cryptography.X509Certificates.X509Store('My', 'CurrentUser')
$myStore.Open('ReadOnly')
$cert = $myStore.Certificates |
    Where-Object { $_.Subject -eq "CN=$certName" -and $_.HasPrivateKey } |
    Select-Object -First 1
$myStore.Close()

if (-not $cert) {
    Write-Host "Creating self-signed code signing certificate..."
    Import-Module PKI -ErrorAction Stop
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$certName" `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(10)

    # Trust the cert so Windows accepts UIAccess
    $rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', 'LocalMachine')
    $rootStore.Open('ReadWrite'); $rootStore.Add($cert); $rootStore.Close()
    $pubStore = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPublisher', 'LocalMachine')
    $pubStore.Open('ReadWrite'); $pubStore.Add($cert); $pubStore.Close()
    Write-Host "Certificate created and trusted."
} else {
    Write-Host "Reusing existing certificate: $($cert.Thumbprint)"
}

# --- 3. Sign receiver.exe ---
Write-Host "Signing receiver.exe..."
$thumb = $cert.Thumbprint
& $signtool sign /sha1 $thumb /fd SHA256 /t http://timestamp.digicert.com $src 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "Timestamp server unreachable, signing without timestamp..."
    & $signtool sign /sha1 $thumb /fd SHA256 $src
    if ($LASTEXITCODE -ne 0) { throw "Signing failed." }
}
& $signtool verify /pa $src
Write-Host "Signing done."

# --- 4. Stop old receiver ---
Write-Host "Stopping old receiver..."
Get-Process receiver -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 1

# --- 5. Install to Program Files ---
if (-not (Test-Path $installDir)) {
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
}
Copy-Item $src $dest -Force
Write-Host "Installed to $dest"

# --- 6. Create startup shortcut ---
# We use a shell:Startup shortcut instead of a scheduled task because
# Explorer properly grants UIAccess when launching from a shortcut,
# while Task Scheduler does not.
$ws = New-Object -ComObject WScript.Shell
$sc = $ws.CreateShortcut($shortcutPath)
$sc.TargetPath = $dest
$sc.WorkingDirectory = $installDir
$sc.Description = 'Keyboard/Mouse receiver with UIAccess'
$sc.Save()
Write-Host "Startup shortcut created: $shortcutPath"

# Clean up any leftover scheduled task from older versions
$oldTask = Get-ScheduledTask -TaskName 'KM Receiver' -ErrorAction SilentlyContinue
if ($oldTask) {
    Unregister-ScheduledTask -TaskName 'KM Receiver' -Confirm:$false
    Write-Host "Removed old scheduled task."
}

# --- 7. Disable Secure Desktop for UAC ---
# This makes UAC prompts appear on the normal desktop so SendInput can reach them.
$regPath = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
$current = (Get-ItemProperty $regPath).PromptOnSecureDesktop
if ($current -ne 0) {
    Set-ItemProperty $regPath -Name PromptOnSecureDesktop -Value 0
    Write-Host "Disabled Secure Desktop for UAC prompts (PromptOnSecureDesktop=0)."
} else {
    Write-Host "Secure Desktop already disabled."
}

# --- 8. Launch ---
Write-Host "Launching receiver..."
Start-Process explorer.exe $dest
Start-Sleep 2
$proc = Get-Process receiver -ErrorAction SilentlyContinue
if ($proc) {
    Write-Host "`nDone! Receiver running (PID $($proc.Id))."
    Write-Host "It will auto-start at logon via the startup shortcut."
} else {
    Write-Host "`nWARN: Receiver did not start. Try launching manually: $dest"
}
