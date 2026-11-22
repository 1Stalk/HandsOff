& {
    $ErrorActionPreference = 'Stop'

    $installDir = Join-Path $env:LOCALAPPDATA 'HandsOff'
    $exePath = Join-Path $installDir 'HandsOff.exe'

    if (Test-Path $exePath) {
        Write-Host '[HandsOff] Uninstalling and unloading from Explorer...' -ForegroundColor Cyan
        $out = (& $exePath uninstall | Out-String).Trim()
        if ($out) {
            Write-Host $out
        }
        Start-Sleep -Milliseconds 300
    } else {
        Write-Host '[HandsOff] Cleaning up registrations...' -ForegroundColor Cyan
        try {
            $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\HandsOffUnloadEvent')
            $null = $ev.Set()
            Start-Sleep -Milliseconds 350
            $null = $ev.Reset()
            $ev.Dispose()
        } catch {
        }

        $clsid = '{8E4A2B19-5C71-4D83-9A2E-6F1C9B04E5D2}'
        foreach ($hive in @('HKCU:', 'HKLM:')) {
            $keys = @(
                "$hive\Software\Classes\Drive\shellex\FolderExtensions\$clsid",
                "$hive\Software\Classes\Directory\shellex\CopyHookHandlers\HandsOff",
                "$hive\Software\Classes\Directory\shellex\CopyHookHandlers\CloseAlready",
                "$hive\Software\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers\            HandsOff",
                "$hive\Software\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers\   HandsOff",
                "$hive\Software\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers\   CloseAlready",
                "$hive\Software\Classes\CLSID\$clsid\InprocServer32",
                "$hive\Software\Classes\CLSID\$clsid"
            )
            foreach ($key in $keys) {
                try {
                    if (Test-Path -LiteralPath $key) {
                        Remove-Item -LiteralPath $key -Force -ErrorAction SilentlyContinue
                    }
                } catch {
                }
            }
            try {
                Remove-ItemProperty -LiteralPath "$hive\Software\Microsoft\Windows\CurrentVersion\Run" -Name 'HandsOff' -ErrorAction SilentlyContinue
                Remove-ItemProperty -LiteralPath "$hive\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" -Name $clsid -ErrorAction SilentlyContinue
            } catch {
            }
        }

        $svc = Get-Service -Name 'HandsOffSvc' -ErrorAction SilentlyContinue
        if ($svc) {
            & sc.exe stop HandsOffSvc | Out-Null
            & sc.exe delete HandsOffSvc | Out-Null
        }
    }

    if (Test-Path $installDir) {
        for ($i = 0; $i -lt 5; $i++) {
            try {
                Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction Stop
                break
            } catch {
                Start-Sleep -Milliseconds 250
            }
        }
    }

    if (Test-Path $installDir) {
        Write-Warning "Could not fully remove $installDir (a file may still be in use). Restart Explorer and delete the folder manually."
    } else {
        Write-Host '[HandsOff] Uninstalled and removed cleanly.' -ForegroundColor Green
    }
}
