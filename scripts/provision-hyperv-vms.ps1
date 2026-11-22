<#
.SYNOPSIS
    Automatically provisions Hyper-V test VMs from the ISOs in .reference\iso using unattended install.

.DESCRIPTION
    Creates Gen 2 Hyper-V VMs with dynamic VHDX disks on D:\HyperV (configurable), attaches each
    Windows ISO alongside an auto-generated unattended answer ISO (autounattend.xml with Tester
    auto-logon and PowerShell Direct readiness), completes OOBE automatically, captures a clean
    'clean-ready' snapshot, and writes scripts\vm-matrix.json for run-vm-matrix.ps1.

    Requires Administrator privileges (or Hyper-V Administrators + local admin for WIM inspection).
#>
[CmdletBinding()]
param(
    [string]$VmRoot = "D:\HyperV\HandsOffVMs",
    [string]$IsoRoot = "",
    [string]$Username = "Tester",
    [string]$Password = "HandsOffTest!1",
    [int]$MemoryMB = 4096,
    [int]$CpuCount = 2,
    [int]$InstallTimeoutMin = 25,
    [switch]$RecreateExisting
)

$ErrorActionPreference = "Stop"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)
if (-not $isAdmin) {
    throw "Please run this script from an Elevated (Run as Administrator) PowerShell terminal."
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
if ([string]::IsNullOrWhiteSpace($IsoRoot)) {
    $IsoRoot = Join-Path $repoRoot ".reference\iso"
}

New-Item -ItemType Directory -Force -Path $VmRoot | Out-Null

function Find-IsoByPattern {
    param([string]$Pattern)
    $match = Get-ChildItem -Path $IsoRoot -Recurse -Filter "*.iso" |
        Where-Object { $_.FullName -like $Pattern } |
        Select-Object -First 1
    if (-not $match) { throw "ISO not found matching pattern '$Pattern' under '$IsoRoot'." }
    return $match.FullName
}

$ruHome = -join ([char[]](0x0414,0x043E,0x043C,0x0430,0x0448,0x043D,0x044F,0x044F))
$ruAdmins = -join ([char[]](0x0410,0x0434,0x043C,0x0438,0x043D,0x0438,0x0441,0x0442,0x0440,0x0430,0x0442,0x043E,0x0440,0x044B))

$targets = @(
    @{
        vmName     = "HandsOff-Win10-22H2-ruRU"
        isoPath    = (Find-IsoByPattern "*windows_10*22h2*x64*.iso")
        preferEd   = @("Windows 10 Pro", "Windows 10 Enterprise", "Windows 10 $ruHome", "Windows 10 Home")
        isWin11    = $false
    },
    @{
        vmName     = "HandsOff-Win11-23H2-ruRU"
        isoPath    = (Find-IsoByPattern "*windows_11*23h2*x64*.iso")
        preferEd   = @("Windows 11 Pro", "Windows 11 Enterprise", "Windows 11 $ruHome", "Windows 11 Home")
        isWin11    = $true
    },
    @{
        vmName     = "HandsOff-Win11-26H2-ruRU"
        isoPath    = (Find-IsoByPattern "*windows_11*26h2*x64*.iso")
        preferEd   = @("Windows 11 Pro", "Windows 11 $ruHome", "Windows 11 Home", "Windows 11 Enterprise")
        isWin11    = $true
    }
)

function Resolve-WimEdition {
    param([string]$IsoPath, [string[]]$PreferredNames)
    $mnt = Mount-DiskImage -ImagePath $IsoPath -PassThru
    try {
        $drv = ($mnt | Get-Volume | Where-Object { $_.DriveLetter } | Select-Object -First 1).DriveLetter + ":"
        $wimPath = "$drv\sources\install.wim"
        if (-not (Test-Path $wimPath)) { $wimPath = "$drv\sources\install.esd" }
        $images = @(Get-WindowsImage -ImagePath $wimPath)
        foreach ($pref in $PreferredNames) {
            $hit = $images | Where-Object { $_.ImageName -eq $pref } | Select-Object -First 1
            if ($hit) { return $hit }
        }
        $proLike = $images | Where-Object { $_.ImageName -match "Pro|Enterprise" } | Select-Object -First 1
        if ($proLike) { return $proLike }
        return ($images | Select-Object -First 1)
    } finally {
        Dismount-DiskImage -ImagePath $IsoPath | Out-Null
    }
}

function Get-GenericInstallKey {
    param([string]$ImageName)
    if ($ImageName -match "Enterprise") { return "NPPR9-FWDCX-D2C8J-H872K-2YT43" }
    if ($ImageName -match "Education")  { return "NW6C2-QMPVW-D7KKK-3GKT6-VCFB2" }
    if ($ImageName -match "Home" -or $ImageName.Contains($ruHome)) { return "TX9XD-98N7V-6WMQ6-BX7FG-H8Q99" }
    return "VK7JG-NPHTM-C97JM-9MPGT-3V66T" # Pro
}

function New-UnattendIso {
    param([string]$OutputIsoPath, [string]$XmlContent)
    $tmpDir = Join-Path $env:TEMP ("UnattendSrc_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
    try {
        $xmlPath = Join-Path $tmpDir "autounattend.xml"
        [System.IO.File]::WriteAllText($xmlPath, $XmlContent, (New-Object System.Text.UTF8Encoding($false)))

        $fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
        $fsi.FileSystemsToCreate = 3 # ISO9660 + Joliet
        $fsi.VolumeName = "AUTOUNATTEND"
        $fsi.Root.AddTree($tmpDir, $false)
        $result = $fsi.CreateResultImage()
        $stream = $result.ImageStream

        if (-not ("IsoStreamWriter" -as [type])) {
            Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public static class IsoStreamWriter {
    public static void WriteToFile(object comStream, string path) {
        var istream = (IStream)comStream;
        using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write)) {
            byte[] buf = new byte[65536];
            IntPtr pcbRead = Marshal.AllocHGlobal(sizeof(int));
            try {
                while (true) {
                    istream.Read(buf, buf.Length, pcbRead);
                    int read = Marshal.ReadInt32(pcbRead);
                    if (read <= 0) break;
                    fs.Write(buf, 0, read);
                }
            } finally {
                Marshal.FreeHGlobal(pcbRead);
            }
        }
    }
}
"@
        }
        [IsoStreamWriter]::WriteToFile($stream, $OutputIsoPath)
    } finally {
        Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Build-AutounattendXml {
    param([int]$ImageIndex, [string]$ProductKey, [string]$User, [string]$Pass, [string]$AdminGroup = "Administrators")
    return @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="windowsPE">
    <component name="Microsoft-Windows-International-Core-WinPE" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <SetupUILanguage>
        <UILanguage>ru-RU</UILanguage>
      </SetupUILanguage>
      <InputLocale>0409:00000409;0419:00000419</InputLocale>
      <SystemLocale>ru-RU</SystemLocale>
      <UILanguage>ru-RU</UILanguage>
      <UserLocale>ru-RU</UserLocale>
    </component>
    <component name="Microsoft-Windows-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <RunSynchronous>
        <RunSynchronousCommand wcm:action="add">
          <Order>1</Order>
          <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path>
        </RunSynchronousCommand>
        <RunSynchronousCommand wcm:action="add">
          <Order>2</Order>
          <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path>
        </RunSynchronousCommand>
        <RunSynchronousCommand wcm:action="add">
          <Order>3</Order>
          <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1 /f</Path>
        </RunSynchronousCommand>
      </RunSynchronous>
      <DiskConfiguration>
        <Disk wcm:action="add">
          <DiskID>0</DiskID>
          <WillWipeDisk>true</WillWipeDisk>
          <CreatePartitions>
            <CreatePartition wcm:action="add"><Order>1</Order><Type>EFI</Type><Size>260</Size></CreatePartition>
            <CreatePartition wcm:action="add"><Order>2</Order><Type>MSR</Type><Size>16</Size></CreatePartition>
            <CreatePartition wcm:action="add"><Order>3</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition>
          </CreatePartitions>
          <ModifyPartitions>
            <ModifyPartition wcm:action="add"><Order>1</Order><PartitionID>1</PartitionID><Format>FAT32</Format><Label>System</Label></ModifyPartition>
            <ModifyPartition wcm:action="add"><Order>2</Order><PartitionID>2</PartitionID></ModifyPartition>
            <ModifyPartition wcm:action="add"><Order>3</Order><PartitionID>3</PartitionID><Format>NTFS</Format><Label>Windows</Label><Letter>C</Letter></ModifyPartition>
          </ModifyPartitions>
        </Disk>
      </DiskConfiguration>
      <ImageInstall>
        <OSImage>
          <InstallFrom>
            <MetaData wcm:action="add">
              <Key>/IMAGE/INDEX</Key>
              <Value>$ImageIndex</Value>
            </MetaData>
          </InstallFrom>
          <InstallTo>
            <DiskID>0</DiskID>
            <PartitionID>3</PartitionID>
          </InstallTo>
        </OSImage>
      </ImageInstall>
      <UserData>
        <AcceptEula>true</AcceptEula>
        <FullName>$User</FullName>
        <Organization>HandsOffLab</Organization>
        <ProductKey>
          <Key>$ProductKey</Key>
          <WillShowUI>Never</WillShowUI>
        </ProductKey>
      </UserData>
    </component>
  </settings>
  <settings pass="specialize">
    <component name="Microsoft-Windows-Deployment" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <RunSynchronous>
        <RunSynchronousCommand wcm:action="add">
          <Order>1</Order>
          <Path>reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>
        </RunSynchronousCommand>
      </RunSynchronous>
    </component>
  </settings>
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <ProtectYourPC>3</ProtectYourPC>
        <SkipMachineOOBE>true</SkipMachineOOBE>
        <SkipUserOOBE>true</SkipUserOOBE>
      </OOBE>
      <AutoLogon>
        <Enabled>true</Enabled>
        <LogonCount>999</LogonCount>
        <Username>$User</Username>
        <Password><Value>$Pass</Value><PlainText>true</PlainText></Password>
      </AutoLogon>
      <UserAccounts>
        <LocalAccounts>
          <LocalAccount wcm:action="add">
            <Name>$User</Name>
            <DisplayName>$User</DisplayName>
            <Group>$AdminGroup</Group>
            <Password><Value>$Pass</Value><PlainText>true</PlainText></Password>
          </LocalAccount>
        </LocalAccounts>
      </UserAccounts>
      <FirstLogonCommands>
        <SynchronousCommand wcm:action="add">
          <Order>1</Order>
          <CommandLine>powershell -NoProfile -Command "Set-ExecutionPolicy Bypass -Scope LocalMachine -Force; powercfg /change standby-timeout-ac 0; powercfg /change monitor-timeout-ac 0; New-Item -Path 'C:\HandsOffProvisioned.flag' -ItemType File -Force | Out-Null"</CommandLine>
          <Description>Configure PowerShell and mark provisioned</Description>
        </SynchronousCommand>
      </FirstLogonCommands>
    </component>
  </settings>
</unattend>
"@
}

function Send-VmBootKeyPress {
    param([string]$VmName)
    $cs = Get-CimInstance -Namespace "root\virtualization\v2" -ClassName "Msvm_ComputerSystem" -Filter "ElementName='$VmName'"
    if (-not $cs) { return }
    $kb = Get-CimAssociatedInstance -InputObject $cs -ResultClassName "Msvm_Keyboard" | Select-Object -First 1
    if (-not $kb) { return }
    for ($i = 0; $i -lt 12; $i++) {
        Invoke-CimMethod -InputObject $kb -MethodName "TypeKey" -Arguments @{ keyCode = 0x20 } | Out-Null
        Start-Sleep -Milliseconds 600
    }
}

$matrixEntries = @()
$cred = New-Object System.Management.Automation.PSCredential(
    $Username, (ConvertTo-SecureString $Password -AsPlainText -Force)
)

foreach ($t in $targets) {
    $vmName = $t.vmName
    $vmDir = Join-Path $VmRoot $vmName
    $vhdxPath = Join-Path $vmDir "$vmName.vhdx"
    $unattendIso = Join-Path $vmDir "autounattend.iso"

    $existing = Get-VM -Name $vmName -ErrorAction SilentlyContinue
    if ($existing) {
        if (-not $RecreateExisting) {
            $snap = Get-VMSnapshot -VMName $vmName -Name "clean-ready" -ErrorAction SilentlyContinue
            if ($snap) {
                Write-Host "[Provision] VM '$vmName' already has snapshot 'clean-ready'; skipping." -ForegroundColor Yellow
                $matrixEntries += @{
                    vmName = $vmName; snapshotName = "clean-ready"; username = $Username; password = $Password
                }
                continue
            }
        }
        Stop-VM -Name $vmName -TurnOff -Force -ErrorAction SilentlyContinue
        Remove-VM -Name $vmName -Force
    }
    if (Test-Path $vmDir) { Remove-Item -Path $vmDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $vmDir | Out-Null

    Write-Host "[Provision] Inspecting WIM in $(Split-Path -Leaf $t.isoPath)..." -ForegroundColor Cyan
    $img = Resolve-WimEdition -IsoPath $t.isoPath -PreferredNames $t.preferEd
    $key = Get-GenericInstallKey -ImageName $img.ImageName
    Write-Host "[Provision] Selected Index=$($img.ImageIndex) ('$($img.ImageName)')" -ForegroundColor Gray

    $adminGrp = if ($t.isoPath -match "(?i)ru-ru|\[Ru\]") { $ruAdmins } else { "Administrators" }
    $xml = Build-AutounattendXml -ImageIndex $img.ImageIndex -ProductKey $key -User $Username -Pass $Password -AdminGroup $adminGrp
    New-UnattendIso -OutputIsoPath $unattendIso -XmlContent $xml

    New-VHD -Path $vhdxPath -SizeBytes 45GB -Dynamic | Out-Null
    New-VM -Name $vmName -MemoryStartupBytes ($MemoryMB * 1MB) -Generation 2 -VHDPath $vhdxPath -Path $vmDir | Out-Null
    Set-VMProcessor -VMName $vmName -Count $CpuCount
    Set-VMMemory -VMName $vmName -DynamicMemoryEnabled $false
    Set-VM -Name $vmName -AutomaticCheckpointsEnabled $false
    Set-VMFirmware -VMName $vmName -EnableSecureBoot Off
    if ($t.isWin11) {
        Set-VMKeyProtector -VMName $vmName -NewLocalKeyProtector
        Enable-VMTPM -VMName $vmName
    }

    $dvdOs = Add-VMDvdDrive -VMName $vmName -Path $t.isoPath -Passthru
    Add-VMDvdDrive -VMName $vmName -Path $unattendIso | Out-Null
    $hd = Get-VMHardDiskDrive -VMName $vmName
    Set-VMFirmware -VMName $vmName -BootOrder $dvdOs, $hd

    Write-Host "[Provision] Starting '$vmName' and booting Windows Setup..." -ForegroundColor Cyan
    Start-VM -Name $vmName
    Start-Sleep -Seconds 1
    Send-VmBootKeyPress -VmName $vmName

    $deadline = (Get-Date).AddMinutes($InstallTimeoutMin)
    $provisioned = $false
    while ((Get-Date) -lt $deadline) {
        try {
            $ready = Invoke-Command -VMName $vmName -Credential $cred -ErrorAction Stop -ScriptBlock {
                (Test-Path "C:\HandsOffProvisioned.flag") -and ($null -ne (Get-Process -Name explorer -ErrorAction SilentlyContinue))
            }
            if ($ready) { $provisioned = $true; break }
        } catch {
            Start-Sleep -Seconds 10
            continue
        }
        Start-Sleep -Seconds 5
    }

    if (-not $provisioned) {
        throw "Timed out waiting for unattended installation & Explorer desktop on '$vmName' after ${InstallTimeoutMin}m."
    }

    # Wait for post-OOBE FirstLogonAnim ("Подготовка Windows") to clear before snapshotting
    $animDeadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $animDeadline) {
        try {
            $idle = Invoke-Command -VMName $vmName -Credential $cred -ErrorAction Stop -ScriptBlock {
                $anim = Get-Process -Name "FirstLogonAnim", "LogonUI" -ErrorAction SilentlyContinue
                $exp = Get-Process -Name "explorer" -ErrorAction SilentlyContinue
                ($null -eq $anim) -and ($null -ne $exp)
            }
            if ($idle) { break }
        } catch {}
        Start-Sleep -Seconds 3
    }

    # Eject setup ISOs and put hard disk first in boot order before snapshotting
    Get-VMDvdDrive -VMName $vmName | Set-VMDvdDrive -Path $null
    Set-VMFirmware -VMName $vmName -FirstBootDevice $hd
    Start-Sleep -Seconds 3

    Write-Host "[Provision] Capturing 'clean-ready' live snapshot for '$vmName'..." -ForegroundColor Green
    Checkpoint-VM -Name $vmName -SnapshotName "clean-ready"
    Stop-VM -Name $vmName -TurnOff -Force

    $matrixEntries += @{
        vmName       = $vmName
        snapshotName = "clean-ready"
        username     = $Username
        password     = $Password
    }
}

$matrixConfigPath = Join-Path $scriptDir "vm-matrix.json"
@{ vms = $matrixEntries } | ConvertTo-Json -Depth 4 | Set-Content -Path $matrixConfigPath -Encoding utf8
Write-Host "[Provision] Done! Matrix config written to: $matrixConfigPath" -ForegroundColor Green
