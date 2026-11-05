<#
.SYNOPSIS
Tests Stoker's Peacock integration without HITMAN.

.DESCRIPTION
1. The scanner (hook-plugin/hitman_scan.h) on synthetic images - MSVC, and g++ under WSL when present.
2. The hook plugin in a stand-in game (hook-plugin/tests/host.cpp): Peacock's patterns in its own
   image, the server address set the way HITMAN sets it, every patch checked - on a fixed port, and
   on the automatic one, which points the game at 80 when the plugin starts no server.
3. With -Peacock: the same, with the overlay New-PeacockOverlay.ps1 builds from that release laid
   into a stand-in install, and the configured port held by something that is not Peacock. The plugin
   starts the real Peacock on a free port instead; the test signs in as the game would, ends the
   "game", and checks Peacock wrote the profile with the install's DLC and stopped by itself.
   Then Test-PeacockPlugin.ps1 against the same release.

Builds the plugin first (build.ps1).

.PARAMETER Peacock
A Peacock Windows release: Peacock-v<version>.zip or its extracted folder.
#>
param([string]$Peacock)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pluginDir = Join-Path $repo 'hook-plugin'
$work = Join-Path ([IO.Path]::GetTempPath()) "stoker-peacock-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory $work | Out-Null
$failed = $false

function Invoke-Msvc([string[]]$Lines) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio C++ tools are required.' }
    $batch = Join-Path $work 'build.cmd'
    Set-Content $batch -Encoding ascii -Value (@('@echo off', "call `"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul", "cd /d `"$work`"") + $Lines)
    & cmd.exe /c $batch
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
}

try {
    & (Join-Path $repo 'build.ps1') | Out-Null
    $dll = Join-Path $repo 'build\x64\peacock.dll'
    $sdk = Join-Path (& (Join-Path $repo 'scripts\fetch-sdk.ps1') -Project (Join-Path $repo 'stoker-plugin.yaml')) 'include'

    Invoke-Msvc @(
        "cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 `"$pluginDir\tests\scan.cpp`" /Fe:scan.exe || exit /b 1",
        "cl /nologo /utf-8 /std:c++20 /EHsc /W4 /O2 /I`"$sdk`" `"$pluginDir\tests\host.cpp`" /Fe:peacock_host.exe || exit /b 1")

    Write-Host '== scanner (MSVC)'
    & (Join-Path $work 'scan.exe'); if ($LASTEXITCODE -ne 0) { $failed = $true }

    if (Get-Command wsl.exe -ErrorAction SilentlyContinue) {
        Write-Host '== scanner (g++)'
        $source = wsl.exe -e wslpath -a (Join-Path $pluginDir 'tests\scan.cpp').Replace('\', '/')
        wsl.exe -e g++ -std=c++20 -Wall -Wextra -Werror -O2 $source -o /tmp/stoker_peacock_scan
        if ($LASTEXITCODE -eq 0) { wsl.exe -e /tmp/stoker_peacock_scan }
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }

    Write-Host '== hook plugin, patching only'
    & (Join-Path $work 'peacock_host.exe') $dll 41592 'unused' 0
    if ($LASTEXITCODE -ne 0) { $failed = $true }
    & (Join-Path $work 'peacock_host.exe') $dll 0 'unused' 0
    if ($LASTEXITCODE -ne 0) { $failed = $true }

    if ($Peacock) {
        Write-Host '== hook plugin with Peacock'
        $install = Join-Path $work 'install'
        & (Join-Path $repo 'New-PeacockOverlay.ps1') -Release $Peacock -Out $install | Out-Host
        $stokerData = Join-Path $install '.stoker'
        New-Item -ItemType Directory $stokerData | Out-Null
        Set-Content (Join-Path $stokerData 'stoker_dlc.txt') "1829590=Seven Deadly Sins`r`n" -NoNewline
        Set-Content (Join-Path $stokerData 'peacock-server.log') 'where the log was kept before logs'
        # The previous run's log, still open as a Peacock writing its last profile holds it.
        New-Item -ItemType Directory (Join-Path $stokerData 'logs') | Out-Null
        $previousLog = [IO.File]::Open((Join-Path $stokerData 'logs\peacock-server.log'), 'Append', 'Write', [IO.FileShare]'ReadWrite, Delete')
        $previousLog.Write([Text.Encoding]::ASCII.GetBytes('the previous run'), 0, 16)
        $previousLog.Flush()

        # Something that accepts connections on the configured port and never answers HTTP.
        $squatter = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $squatter.Start()
        $held = $squatter.LocalEndpoint.Port
        $dataName = "Stoker Peacock Test $([Guid]::NewGuid().ToString('N'))"
        $data = Join-Path $env:LOCALAPPDATA $dataName

        $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $work 'peacock_host.exe'))
        foreach ($a in @($dll, "$held", $dataName, '1')) { $start.ArgumentList.Add($a) }
        $start.UseShellExecute = $false
        $start.RedirectStandardInput = $true
        $start.RedirectStandardOutput = $true
        $start.Environment['STOKER_INSTALL_DIR'] = $stokerData
        $hostProcess = [Diagnostics.Process]::Start($start)
        try {
            $ready = $false
            while (!$hostProcess.StandardOutput.EndOfStream) {
                $line = $hostProcess.StandardOutput.ReadLine()
                Write-Host $line
                if ($line -match '^READY (\d+)$') { $ready = $true; $port = [int]$Matches[1]; break }
            }
            if (!$ready) { throw 'The stand-in game never had Peacock running.' }
            if ($port -eq $held) { Write-Host "FAIL the game was pointed at $held, which Peacock does not hold"; $failed = $true }

            $body = 'grant_type=external_steam&steam_userid=76561198000000002&steam_appid=1659040' +
                '&steam_clienttoken=00&steam_identity=hitman&access_token=x&locale=en-US&rgn=EU&gs=pc-prod'
            $token = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$port/oauth/token" -Body $body -ContentType 'application/x-www-form-urlencoded'
            if ($token.token_type -ne 'bearer') { Write-Host 'FAIL no token'; $failed = $true }

            $hostProcess.StandardInput.WriteLine()
            Write-Host $hostProcess.StandardOutput.ReadToEnd()
            $hostProcess.WaitForExit()
            if ($hostProcess.ExitCode -ne 0) { $failed = $true }

            # Peacock notices its game is gone within a second, writes, and exits.
            $closed = $false
            for ($i = 0; $i -lt 60 -and !$closed; $i++) {
                Start-Sleep -Milliseconds 500
                try { [Net.Sockets.TcpClient]::new('127.0.0.1', $port).Dispose() } catch { $closed = $true }
            }
            if (!$closed) { Write-Host 'FAIL Peacock is still running after its game exited'; $failed = $true }

            $userProfile = Get-ChildItem (Join-Path $data 'userdata\users') -Filter '*.json' -ErrorAction SilentlyContinue | Where-Object Name -ne 'lop.json' | Select-Object -First 1
            $entitlements = if ($userProfile) { @((Get-Content $userProfile.FullName -Raw | ConvertFrom-Json).Extensions.entP) -join ',' } else { '' }
            if ($entitlements -ne '1659040,1829590') { Write-Host "FAIL profile entitlements '$entitlements'"; $failed = $true }
            foreach ($file in 'stoker.plugin.js', 'stoker-agent.js') {
                $written = Join-Path $data "plugins\$file"
                $carried = Join-Path $repo "server\$file"
                if (!(Test-Path $written)) { Write-Host "FAIL the hook plugin did not write $file"; $failed = $true }
                elseif ((Get-FileHash $written).Hash -ne (Get-FileHash $carried).Hash) { Write-Host "FAIL $file is not the one the hook plugin carries"; $failed = $true }
            }
            $options = Get-Content (Join-Path $data 'options.ini') -Raw
            if ($options -notmatch '(?m)^updateChecking=false\r?$' -or $options -notmatch '(?m)^leaderboards=false\r?$') { Write-Host 'FAIL options were not seeded with Stoker''s values'; $failed = $true }
            $releaseOptions = Get-Content (Join-Path $install 'Peacock\options.ini') -Raw
            if ($releaseOptions -match '(?m)^\s*(\w+)\s*=' -and $options -notmatch "(?m)^$($Matches[1])\s*=") { Write-Host 'FAIL the release''s options were not kept'; $failed = $true }
            $serverLog = Join-Path $stokerData 'logs\peacock-server.log'
            if (!(Test-Path $serverLog) -or (Get-Content $serverLog -Raw) -notmatch 'Entitlements from Stoker') { Write-Host 'FAIL Peacock''s output is not in logs\peacock-server.log'; $failed = $true }
            elseif ((Get-Content $serverLog -Raw) -match 'the previous run') { Write-Host 'FAIL the server log still holds the previous run'; $failed = $true }
            if (Test-Path (Join-Path $stokerData 'peacock-server.log')) { Write-Host 'FAIL the old server log was left'; $failed = $true }
            if (!$failed) { Write-Host 'Peacock started by the plugin, signed in with the install''s DLC, stopped with its game.' -ForegroundColor Green }
        }
        finally {
            $squatter.Stop()
            $previousLog.Dispose()
            if (!$hostProcess.HasExited) { $hostProcess.Kill() }
            Get-CimInstance Win32_Process -Filter "Name = 'node.exe'" | Where-Object CommandLine -like "*$install*" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
            Start-Sleep -Milliseconds 500
            Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
        }

        Write-Host '== Peacock plugin'
        & (Join-Path $PSScriptRoot 'Test-PeacockPlugin.ps1') -Peacock (Join-Path $install 'Peacock')
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host 'Peacock integration tests FAILED' -ForegroundColor Red; exit 1 }
Write-Host 'Peacock integration tests passed' -ForegroundColor Green
