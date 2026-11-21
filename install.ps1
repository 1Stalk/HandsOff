& {
    $ErrorActionPreference = 'Stop'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

    if (-not [Environment]::Is64BitOperatingSystem) {
        throw 'HandsOff requires 64-bit Windows (x64).'
    }

    $repo = '1Stalk/HandsOff'
    $installDir = Join-Path $env:LOCALAPPDATA 'HandsOff'
    $tempDir = Join-Path $env:TEMP ("HandsOff-Install-" + [Guid]::NewGuid().ToString('N'))

    try {
        Write-Host '[HandsOff] Fetching latest release metadata...' -ForegroundColor Cyan
        $headers = @{ 'User-Agent' = 'HandsOff-Installer' }
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest" -Headers $headers

        $zipAsset = $release.assets | Where-Object { $_.name -like 'HandsOff-*-win-x64.zip' } | Select-Object -First 1
        if (-not $zipAsset) {
            throw "Release asset 'HandsOff-*-win-x64.zip' not found in $($release.tag_name)."
        }

        New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
        $zipPath = Join-Path $tempDir $zipAsset.name
        Write-Host "[HandsOff] Downloading $($zipAsset.name) ($($release.tag_name))..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $zipAsset.browser_download_url -OutFile $zipPath -Headers $headers -UseBasicParsing

        $sumsAsset = $release.assets | Where-Object { $_.name -eq 'SHA256SUMS.txt' } | Select-Object -First 1
        if ($sumsAsset) {
            $sumsPath = Join-Path $tempDir 'SHA256SUMS.txt'
            Invoke-WebRequest -Uri $sumsAsset.browser_download_url -OutFile $sumsPath -Headers $headers -UseBasicParsing
            $expectedLine = Get-Content $sumsPath | Where-Object { $_ -match ([regex]::Escape($zipAsset.name) + '\s*$') } | Select-Object -First 1
            if ($expectedLine) {
                $expectedHash = ($expectedLine -split '\s+')[0].Trim().ToLowerInvariant()
                $actualHash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($expectedHash -ne $actualHash) {
                    throw "SHA-256 checksum mismatch for $($zipAsset.name)! Expected $expectedHash, got $actualHash."
                }
                Write-Host '[HandsOff] SHA-256 checksum verified.' -ForegroundColor DarkGray
            }
        }

        $existingExe = Join-Path $installDir 'HandsOff.exe'
        if (Test-Path $existingExe) {
            Write-Host '[HandsOff] Unloading previous installation...' -ForegroundColor DarkGray
            & $existingExe uninstall | Out-Null
            Start-Sleep -Milliseconds 300
        }

        New-Item -ItemType Directory -Force -Path $installDir | Out-Null
        Expand-Archive -Path $zipPath -DestinationPath $installDir -Force

        $exePath = Join-Path $installDir 'HandsOff.exe'
        if (-not (Test-Path $exePath)) {
            throw "HandsOff.exe was not found after extracting to $installDir."
        }

        $installOut = (& $exePath install | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) {
            throw "HandsOff installation failed (exit code $LASTEXITCODE): $installOut"
        }

        Write-Host "[HandsOff] Installed to: $installDir" -ForegroundColor Green
        if ($installOut) {
            Write-Host $installOut -ForegroundColor Green
        }
        Write-Host '[HandsOff] To uninstall later: irm https://raw.githubusercontent.com/1Stalk/HandsOff/main/uninstall.ps1 | iex' -ForegroundColor DarkGray
    }
    finally {
        if (Test-Path $tempDir) {
            Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
