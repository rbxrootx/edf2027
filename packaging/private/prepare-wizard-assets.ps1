$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$Assets = Join-Path $PSScriptRoot 'assets'
$Magick = (Get-Command magick.exe -ErrorAction Stop).Source
New-Item -ItemType Directory -Force -Path $Assets | Out-Null

& $Magick (Join-Path $RepoRoot 'docs\media\gameplay-1440p.jpg') `
    -gravity center -crop '565x1080+0+0' +repage -resize '492x944!' -strip `
    (Join-Path $Assets 'wizard-gameplay.png')
if ($LASTEXITCODE -ne 0) { throw 'Could not create the gameplay wizard image.' }

& $Magick (Join-Path $RepoRoot 'docs\media\menu-graphics.jpg') `
    -crop '500x500+0+0' +repage -resize '180x180!' -strip `
    (Join-Path $Assets 'wizard-menu.png')
if ($LASTEXITCODE -ne 0) { throw 'Could not create the menu wizard image.' }
