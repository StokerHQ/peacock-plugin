<#
.SYNOPSIS
Lays a Peacock release out as the overlay for HITMAN World of Assassination.

.DESCRIPTION
Takes a Windows release of The Peacock Project (Peacock-v<version>.zip from
https://github.com/thepeacockproject/Peacock/releases, or the folder it extracts to) and writes
<Out>\Peacock\ - the release's files, every one of them exactly as published - to upload as the
HITMAN build's overlay (Fixes -> Files over the game). Only the release's own top folder,
Peacock-v<version>, becomes Peacock, the folder the hook plugin's peacockFolder names.

Nothing of Stoker's is added: the hook plugin (app.stokerhq.peacock) carries Stoker's Peacock plugin and
writes it into Peacock's working folder under %LOCALAPPDATA% at every start, and seeds Peacock's
options there from the release's options.ini the first time. The release's PeacockPatcher.exe and
start scripts stay in the folder and are never run: the hook plugin patches the game itself.

Stoker never ships Peacock: the operator downloads a release and runs this.

.PARAMETER Release
Peacock-v<version>.zip, or the folder extracted from it.

.PARAMETER Out
An empty or new folder to write the overlay into.

.PARAMETER Zip
Also write <Out>.zip of the overlay folder.
#>
param(
    [Parameter(Mandatory)][string]$Release,
    [Parameter(Mandatory)][string]$Out,
    [switch]$Zip
)

$ErrorActionPreference = 'Stop'

if (Test-Path $Out) {
    if (Get-ChildItem -Force $Out | Select-Object -First 1) { throw "$Out is not empty." }
} else {
    New-Item -ItemType Directory $Out | Out-Null
}
$Out = (Resolve-Path $Out).Path

$extracted = $null
try {
    if ((Test-Path $Release -PathType Leaf) -and $Release -like '*.zip') {
        $extracted = Join-Path ([IO.Path]::GetTempPath()) "stoker-peacock-release-$([Guid]::NewGuid().ToString('N'))"
        Expand-Archive -LiteralPath $Release -DestinationPath $extracted
        $source = $extracted
    } elseif (Test-Path $Release -PathType Container) {
        $source = (Resolve-Path $Release).Path
    } else {
        throw "$Release is neither a Peacock release zip nor a folder."
    }

    # The release zip holds one folder, Peacock-v<version>; accept that folder or its parent.
    $chunk = Get-ChildItem -LiteralPath $source -Filter chunk0.js -Recurse -Depth 1 | Select-Object -First 1
    if (!$chunk) { throw "No chunk0.js in $Release; is it a Peacock release?" }
    $root = $chunk.Directory.FullName
    if (!(Test-Path (Join-Path $root 'nodedist\node.exe'))) {
        throw "$root has no nodedist\node.exe. Use the Windows release (Peacock-v<version>.zip), not the Linux one."
    }
    $version = if ($chunk.Directory.Name -match '^Peacock-v(.+)$') { $Matches[1] } else { 'unknown' }

    $peacock = Join-Path $Out 'Peacock'
    New-Item -ItemType Directory $peacock | Out-Null
    Get-ChildItem -LiteralPath $root -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $peacock -Recurse }

    if ($Zip) {
        $archive = "$Out.zip"
        if (Test-Path $archive) { throw "$archive already exists." }
        Compress-Archive -Path $peacock -DestinationPath $archive
        Write-Host "Wrote $archive"
    }

    $bytes = (Get-ChildItem -LiteralPath $peacock -Recurse -File | Measure-Object Length -Sum).Sum
    Write-Host ("Peacock {0} overlay in {1} ({2:N0} MB)" -f $version, $peacock, ($bytes / 1MB))
}
finally {
    if ($extracted) { Remove-Item -Recurse -Force $extracted -ErrorAction SilentlyContinue }
}
