[CmdletBinding()]
param(
    [switch]$SkipNativeBuild,
    [switch]$KeepVerificationInstall,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$BuildDirectory = 'build-0.3.38'
)

$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$ParentGameRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot '..'))
$PackageRoot = Join-Path $RepoRoot 'out\package'
$StageRoot = Join-Path $PackageRoot 'staging'
$SourceRoot = Join-Path $StageRoot 'source'
$GameRoot = Join-Path $StageRoot 'game-data'
# Keep this release build isolated from older builds that may be running during
# private testing. In particular, Windows cannot replace an open client EXE.
$BuildRoot = Join-Path $PackageRoot $BuildDirectory
# Every installer build gets a fresh payload tree. Older stage trees may contain
# files written by a running client/server, so never reuse them as installer input.
$PayloadRunId = [Guid]::NewGuid().ToString('N')
$PayloadRoot = Join-Path $StageRoot ('payload-' + $PayloadRunId)
$PayloadBin = Join-Path $PayloadRoot 'bin'
$PayloadData = Join-Path $PayloadRoot 'data'
$SdkSource = Join-Path $RepoRoot 'out\tools\rx010'
$SdkPrefix = Join-Path $RepoRoot 'out\tools\sdki-msvc'
$InnoCompiler = Join-Path $RepoRoot 'out\tools\InnoSetup6\ISCC.exe'
$PrivateRoot = $PSScriptRoot
$InstallerSource = Join-Path $PrivateRoot 'edf2027-private.iss'
$GeneratedRoot = Join-Path $RepoRoot 'generated\default'

function Copy-Tree([string]$Source, [string]$Destination, [string[]]$Exclude = @(),
                   [string[]]$ExcludeFiles = @()) {
    if (!(Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Required directory is missing: $Source"
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $arguments = @($Source, $Destination, '/E', '/IS', '/COPY:DAT', '/DCOPY:DAT', '/R:1', '/W:1', '/XJ',
        '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
    if ($Exclude.Count) { $arguments += '/XD'; $arguments += $Exclude }
    if ($ExcludeFiles.Count) { $arguments += '/XF'; $arguments += $ExcludeFiles }
    & robocopy.exe @arguments | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Robocopy failed ($LASTEXITCODE): $Source -> $Destination" }
}

function Get-SelectedGameDirectories([string]$Root) {
    $required = @('Bat', 'Effect', 'MainSequence', 'MenuBg', 'Mission', 'Object',
                  'Shader', 'Sound', 'System', 'Weapon', 'XUI')
    $maps = @(Get-ChildItem -LiteralPath $Root -Directory | Where-Object { $_.Name -match '^Map\d+$' } |
        Sort-Object Name | ForEach-Object { $_.Name })
    if (!$maps.Count) { throw "No Map folders found in $Root" }
    foreach ($name in $required + $maps) {
        if (!(Test-Path -LiteralPath (Join-Path $Root $name) -PathType Container)) {
            throw "Required runtime game-data directory is missing: $name"
        }
    }
    return @($required + $maps)
}

function Get-GameFileInventory([string]$Root, [string[]]$Directories) {
    $files = @()
    foreach ($directory in $Directories) {
        $files += Get-ChildItem -LiteralPath (Join-Path $Root $directory) -File -Recurse -Force |
            Where-Object { $_.Extension -notin @('.id0', '.id1', '.nam', '.til') -and
                           $_.Name -ne 'edf_menu_init_report.txt' }
    }
    foreach ($name in @('default.xex', 'default_decrypted.xex')) {
        $path = Join-Path $Root $name
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required game executable is missing: $path" }
        $files += Get-Item -LiteralPath $path
    }
    return $files
}

function Get-RelativeInventory([string]$Root, [System.IO.FileInfo[]]$Files) {
    $inventory = @{}
    foreach ($file in $Files) {
        $relative = $file.FullName.Substring($Root.Length).TrimStart('\').Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $inventory[$relative] = '{0}:{1}' -f [int64]$file.Length, $hash
    }
    return $inventory
}

function Add-RuntimeDll([System.IO.FileInfo]$File, [string]$Destination) {
    $target = Join-Path $Destination $File.Name
    if (Test-Path -LiteralPath $target) {
        $existingHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        $incomingHash = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash
        if ($existingHash -ne $incomingHash) {
            throw "Runtime DLL name collision with different contents: $($File.Name) ($($File.FullName))"
        }
        return
    }
    Copy-Item -LiteralPath $File.FullName -Destination $target
}

function Invoke-VsBuild([string]$BuildScript) {
    $command = 'call "' + $BuildScript + '"'
    & cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0) { throw "Native CMake build failed with exit code $LASTEXITCODE. See $BuildScript" }
}

New-Item -ItemType Directory -Force -Path $PackageRoot, $StageRoot | Out-Null
if (!(Test-Path -LiteralPath $InnoCompiler -PathType Leaf)) {
    throw "Inno Setup 6 compiler not found at $InnoCompiler. Install the official compiler into out/tools first."
}
if (!(Test-Path -LiteralPath (Join-Path $SdkPrefix 'lib\cmake\rexglue\rexglueConfig.cmake') -PathType Leaf)) {
    throw "Official ReXGlue SDK 0.10.0 install not found at $SdkPrefix. Build the pinned SDK first."
}

# The user-supplied tree is the sibling of this repo. Stage only named runtime
# directories and the two XEX files; never recurse the extracted-game root.
if (!(Test-Path -LiteralPath $GameRoot -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $GameRoot | Out-Null
    $initialDirectories = Get-SelectedGameDirectories $ParentGameRoot
    foreach ($directory in $initialDirectories) {
        Copy-Tree (Join-Path $ParentGameRoot $directory) (Join-Path $GameRoot $directory)
    }
    foreach ($name in @('default.xex', 'default_decrypted.xex')) {
        Copy-Item -LiteralPath (Join-Path $ParentGameRoot $name) -Destination (Join-Path $GameRoot $name)
    }
}
$GameDirectories = Get-SelectedGameDirectories $GameRoot
$MapDirectories = @($GameDirectories | Where-Object { $_ -match '^Map\d+$' })
$InstallerMapDirectories = @('Map01', 'Map02', 'Map03', 'Map04', 'Map05', 'Map07', 'Map08')
if (Compare-Object $InstallerMapDirectories $MapDirectories) {
    throw "Update the private installer .iss map entries to match: $($MapDirectories -join ', ')"
}
$ParentDirectories = Get-SelectedGameDirectories $ParentGameRoot
if (Compare-Object $ParentDirectories $GameDirectories) {
    throw 'Staged game-data directories do not match the complete extracted-game directory set.'
}
$ParentInventory = Get-RelativeInventory $ParentGameRoot (Get-GameFileInventory $ParentGameRoot $ParentDirectories)
$StagedInventory = Get-RelativeInventory $GameRoot (Get-GameFileInventory $GameRoot $GameDirectories)
$inventoryDifferences = @(
    $ParentInventory.Keys | Where-Object {
        !$StagedInventory.ContainsKey($_) -or $ParentInventory[$_] -ne $StagedInventory[$_]
    }
    $StagedInventory.Keys | Where-Object { !$ParentInventory.ContainsKey($_) }
)
if ($inventoryDifferences.Count) {
    throw "Staged game-data is incomplete or differs from the extracted parent by SHA-256: $($inventoryDifferences[0])"
}
$ExpectedGameFiles = Get-GameFileInventory $GameRoot $GameDirectories
$ForbiddenGameFiles = Get-ChildItem -LiteralPath $GameRoot -File -Recurse -Force |
    Where-Object { $_.Extension -in @('.id0', '.id1', '.nam', '.til') -or $_.Name -eq 'edf_menu_init_report.txt' }
if ($ForbiddenGameFiles) { throw "Analysis sidecars/reports are present in staged game data: $($ForbiddenGameFiles[0].FullName)" }

if (!$SkipNativeBuild) {
    # Copy the current working tree to an isolated source tree. The stock-generator
    # fallback changes only this copy; generated/default in the working tree is never built.
    $exclude = @((Join-Path $RepoRoot '.git'), (Join-Path $RepoRoot 'out'), (Join-Path $RepoRoot '.vs'))
    Copy-Tree $RepoRoot $SourceRoot $exclude -ExcludeFiles @('identity.edfkey')
    $stageManifest = Join-Path $SourceRoot 'edf2017_manifest.toml'
    $manifest = [System.IO.File]::ReadAllText($stageManifest)
    $gameRootToml = $GameRoot.Replace('\', '/')
    $manifest = [regex]::Replace($manifest, '(?m)^game_root\s*=\s*".*"$', ('game_root = "' + $gameRootToml + '"'))
    $manifest = [regex]::Replace($manifest, '(?m)^file_path\s*=\s*".*"$', ('file_path = "' + $gameRootToml + '/default.xex"'))
    [System.IO.File]::WriteAllText($stageManifest, $manifest, [System.Text.UTF8Encoding]::new($false))

    $forkConfig = Join-Path $SourceRoot 'edf2017_codegen_fork.toml'
    $forkText = [System.IO.File]::ReadAllText($forkConfig)
    foreach ($key in @('edf_ipa_locals', 'edf_ipa_audit', 'edf_ipa_abi_boundary', 'edf_ipa_vtables', 'edf_ipa_fast_abi')) {
        $forkText = [regex]::Replace($forkText, ('(?m)^(' + $key + '\s*=\s*)true\s*$'), '${1}false')
    }
    if ($forkText -match '(?m)^edf_ipa_(locals|audit|abi_boundary|vtables|fast_abi)\s*=\s*true\s*$') {
        throw 'The isolated stock-generator staging copy still enables a codegen-fork option.'
    }
    [System.IO.File]::WriteAllText($forkConfig, $forkText, [System.Text.UTF8Encoding]::new($false))
    $stockStamp = Join-Path $SourceRoot 'generated\default\codegen.build.stamp'
    if (Test-Path -LiteralPath $stockStamp) { Remove-Item -LiteralPath $stockStamp }
    $codegenExe = Join-Path $SdkPrefix 'bin\rexglue.exe'
    if (!(Test-Path -LiteralPath $codegenExe -PathType Leaf)) {
        throw "Pinned ReXGlue code generator is missing at $codegenExe"
    }
    # The generator keeps its own content stamp under generated/default. Removing
    # only CMake's stamp can silently reuse retail-generated sources for a different
    # staged XEX; force the isolated copy to emit code for this exact game payload.
    & $codegenExe codegen $stageManifest --ignore-stamp
    if ($LASTEXITCODE -ne 0) { throw "Prototype XEX code generation failed with exit code $LASTEXITCODE" }

    $cmake = 'C:\Program Files\CMake\bin\cmake.exe'
    $ninja = 'C:\msys64\ucrt64\bin\ninja.exe'
    $clang = 'C:\Program Files\LLVM\bin\clang.exe'
    $clangxx = 'C:\Program Files\LLVM\bin\clang++.exe'
    $vsdev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
    foreach ($tool in @($cmake, $ninja, $clang, $clangxx, $vsdev)) {
        if (!(Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Required native build tool not found: $tool" }
    }
    $buildScript = Join-Path $StageRoot 'build-native-client.cmd'
    $sourceArg = $SourceRoot.Replace('/', '\')
    $buildArg = $BuildRoot.Replace('/', '\')
    $sdkArg = $SdkPrefix.Replace('/', '\')
    $commandLines = @(
        '@echo off',
        ('call "' + $vsdev + '" -arch=amd64 -host_arch=amd64 >nul'),
        'if errorlevel 1 exit /b %errorlevel%',
        ('"' + $cmake + '" -S "' + $sourceArg + '" -B "' + $buildArg + '" -G Ninja ' +
         '-DCMAKE_MAKE_PROGRAM="' + $ninja + '" -DCMAKE_C_COMPILER="' + $clang + '" ' +
         '-DCMAKE_CXX_COMPILER="' + $clangxx + '" -DCMAKE_BUILD_TYPE=Release ' +
         '-DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" ' +
         '-DCMAKE_PREFIX_PATH="' + $sdkArg + '" -DREXSDK_VERSION=0.10.0 ' +
         '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DBUILD_TESTING=OFF -DEDF2027_BUILD_MULTIPLAYER=ON ' +
         '-DEDF2027_BUILD_HEADLESS_GUEST_PROBE=ON -DEDF2027_SKIP_GUEST_PROBE_INSTRUMENTATION=ON ' +
         '-DEDF_GUEST_OPT_PROFILE=v3 -DEDF_HOST_OPT_PROFILE=v3 -DEDF_PGO= ' +
         '-DEDF2027_NATIVE_GRAPHICS_ONLY=OFF'),
        'if errorlevel 1 exit /b %errorlevel%',
        ('"' + $cmake + '" --build "' + $buildArg + '" --target edf2027 edf2027_server edf2027_master edf2027_authoritative_host audit_native_dependencies --parallel 8'),
        'if errorlevel 1 exit /b %errorlevel%'
    )
    [System.IO.File]::WriteAllLines($buildScript, $commandLines, [System.Text.Encoding]::ASCII)
    Remove-Item Env:EDF_REXGLUE_CODEGEN_EXE -ErrorAction SilentlyContinue
    Invoke-VsBuild $buildScript
}

$requiredBuildFiles = @(
    (Join-Path $BuildRoot 'edf2027.exe'),
    (Join-Path $BuildRoot 'src\multiplayer\edf2027_server.exe'),
    (Join-Path $BuildRoot 'src\multiplayer\edf2027_master.exe'),
    (Join-Path $BuildRoot 'edf2027_authoritative_host.exe')
)
foreach ($file in $requiredBuildFiles) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required native build output is missing: $file" }
}

New-Item -ItemType Directory -Force -Path $PayloadBin, $PayloadData | Out-Null
foreach ($file in $requiredBuildFiles) { Copy-Item -LiteralPath $file -Destination $PayloadBin }

# The client loads this database beside the executable at startup, and imports
# D3DCOMPILER_47.dll eagerly to compile the original game's HLSL shaders.
$controllerDb = Join-Path $BuildRoot 'gamecontrollerdb.txt'
if (!(Test-Path -LiteralPath $controllerDb -PathType Leaf)) {
    throw "Required controller database was not staged beside the client: $controllerDb"
}
Copy-Item -LiteralPath $controllerDb -Destination $PayloadBin
$d3dCompiler = 'C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll'
if (!(Test-Path -LiteralPath $d3dCompiler -PathType Leaf)) {
    throw "Windows SDK D3D compiler redistributable was not found: $d3dCompiler"
}
Add-RuntimeDll (Get-Item -LiteralPath $d3dCompiler) $PayloadBin

$runtimeDlls = @()
foreach ($directory in @($BuildRoot, (Join-Path $SdkPrefix 'bin'))) {
    if (Test-Path -LiteralPath $directory -PathType Container) {
        $runtimeDlls += Get-ChildItem -LiteralPath $directory -File -Recurse -Filter '*.dll'
    }
}
$vsRedistRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC'
$vsVersion = Get-ChildItem -LiteralPath $vsRedistRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$vsVersion) { throw "Microsoft VC++ x64 redistributable DLLs were not found under $vsRedistRoot" }
$crtDirectory = Join-Path $vsVersion.FullName 'x64\Microsoft.VC143.CRT'
if (!(Test-Path -LiteralPath $crtDirectory -PathType Container)) { throw "VC143 CRT directory missing: $crtDirectory" }
$runtimeDlls += Get-ChildItem -LiteralPath $crtDirectory -File -Filter '*.dll'
foreach ($dll in $runtimeDlls | Where-Object { $_.Name -notmatch 'xenos' }) { Add-RuntimeDll $dll $PayloadBin }
if (!(Get-ChildItem -LiteralPath $PayloadBin -File -Filter '*.dll')) { throw 'No runtime DLLs were collected for the installer.' }

# Keep the app-local hard dependencies explicit. FidelityFX is loaded only when
# the native FSR option is used; the current ReXGlue runtime imports no FFX DLL.
$requiredRuntimeFiles = @(
    'edf2027.exe', 'edf2027_server.exe', 'edf2027_master.exe', 'edf2027_authoritative_host.exe',
    'rexruntime.dll', 'd3dcompiler_47.dll', 'gamecontrollerdb.txt',
    'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'libsodium.dll'
)
foreach ($name in $requiredRuntimeFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $PayloadBin $name) -PathType Leaf)) {
        throw "Required runtime file is missing from the installer: $name"
    }
}
$ffxStatus = if (Test-Path -LiteralPath (Join-Path $PayloadBin 'amd_fidelityfx_dx12.dll') -PathType Leaf) {
    'bundled'
} else {
    'optional DLL absent; game startup does not depend on it, native FSR is unavailable'
}

$serverDataStage = Join-Path $PayloadData 'server-data'
$examplesStage = Join-Path $PayloadData 'examples'
$toolsStage = Join-Path $PayloadData 'tools'
$docsStage = Join-Path $PayloadData 'docs'
$licensesStage = Join-Path $PayloadData 'licenses'
Copy-Tree (Join-Path $RepoRoot 'server-data') $serverDataStage -ExcludeFiles @('identity.edfkey')
Copy-Item -LiteralPath (Join-Path $PrivateRoot 'server-data-readme.txt') -Destination (Join-Path $serverDataStage 'README.txt') -Force
Copy-Tree (Join-Path $RepoRoot 'examples') $examplesStage
$defaultServerScript = Join-Path $serverDataStage 'scripts\server_owned_ant_combat.luau'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $defaultServerScript) | Out-Null
Copy-Item -LiteralPath (Join-Path $examplesStage 'server_owned_ant_combat.luau') -Destination $defaultServerScript -Force
New-Item -ItemType Directory -Force -Path $toolsStage | Out-Null
Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools\native-benchmark-skipintro-input.txt') -Destination (Join-Path $toolsStage 'native-benchmark-skipintro-input.txt')
Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools\native-headless-mission-input.txt') -Destination (Join-Path $toolsStage 'native-headless-mission-input.txt')
New-Item -ItemType Directory -Force -Path $docsStage, $licensesStage | Out-Null
foreach ($document in @('multiplayer.md', 'multiplayer-networking.md')) {
    $sourceDocument = Join-Path $RepoRoot (Join-Path 'docs' $document)
    if (!(Test-Path -LiteralPath $sourceDocument -PathType Leaf)) { throw "Self-host documentation missing: $sourceDocument" }
    Copy-Item -LiteralPath $sourceDocument -Destination $docsStage
}
if (Test-Path -LiteralPath (Join-Path $RepoRoot 'LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'LICENSE') -Destination $licensesStage
}
Copy-Tree (Join-Path $RepoRoot 'LICENSES') (Join-Path $licensesStage 'port-third-party')
Copy-Item -LiteralPath (Join-Path $SdkSource 'LICENSE') -Destination (Join-Path $licensesStage 'ReXGlue-SDK-LICENSE')
$sdkLicenses = Join-Path $SdkPrefix 'licenses'
if (Test-Path -LiteralPath $sdkLicenses -PathType Container) {
Copy-Tree $sdkLicenses (Join-Path $licensesStage 'ReXGlue-SDK-third-party')
}

# Do not ship runtime state even if a future staging copy unexpectedly adds it.
$forbiddenPayloadFiles = @(Get-ChildItem -LiteralPath $PayloadRoot -File -Recurse -Force |
    Where-Object { $_.Name -eq 'identity.edfkey' -or $_.Extension -in @('.log', '.dmp') })
$forbiddenPayloadDirectories = @(Get-ChildItem -LiteralPath $PayloadRoot -Directory -Recurse -Force |
    Where-Object { $_.Name -in @('native_cache', 'logs') })
if ($forbiddenPayloadFiles.Count -or $forbiddenPayloadDirectories.Count) {
    $firstForbidden = if ($forbiddenPayloadFiles.Count) {
        $forbiddenPayloadFiles[0].FullName
    } else {
        $forbiddenPayloadDirectories[0].FullName
    }
    throw "Runtime state is not allowed in the private installer payload: $firstForbidden"
}

if (!(Test-Path -LiteralPath $InstallerSource -PathType Leaf)) { throw "Private installer source missing: $InstallerSource" }
& (Join-Path $PrivateRoot 'prepare-wizard-assets.ps1')
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw 'Could not prepare the private installer wizard screenshots.' }
$env:EDF2027_PRIVATE_REPO_ROOT = $RepoRoot
$env:EDF2027_PRIVATE_GAME_ROOT = $GameRoot
$env:EDF2027_PRIVATE_BIN_ROOT = $PayloadBin
$env:EDF2027_PRIVATE_DATA_ROOT = $PayloadData
$env:EDF2027_PRIVATE_SOURCE_ROOT = $PrivateRoot
$env:EDF2027_PRIVATE_PACKAGE_ROOT = $PackageRoot
& $InnoCompiler '/Qp' $InstallerSource
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compiler failed with exit code $LASTEXITCODE" }
$installer = Join-Path $PackageRoot 'EDF2027-private-test-0.3.38-beta.exe'
if (!(Test-Path -LiteralPath $installer -PathType Leaf)) { throw "Installer artifact was not created: $installer" }

# Install into an out/package-only verification folder. This checks every selected
# asset path/size, the excluded sidecars, the bundled executables, and the shortcut's
# game_data_root argument without touching the user's normal app-data configuration.
$verifyId = Get-Date -Format 'yyyyMMdd-HHmmss'
$verifyRoot = Join-Path $PackageRoot ('verify-install-' + $verifyId)
$installArgs = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /DIR="' + $verifyRoot + '"'
$install = Start-Process -FilePath $installer -ArgumentList $installArgs -Wait -PassThru
if ($install.ExitCode -ne 0) { throw "Verification install failed with exit code $($install.ExitCode)" }
$installedGameRoot = Join-Path $verifyRoot 'game'
$expectedInventory = Get-RelativeInventory $GameRoot $ExpectedGameFiles
$installedFiles = @(Get-ChildItem -LiteralPath $installedGameRoot -File -Recurse -Force)
$installedInventory = Get-RelativeInventory $installedGameRoot $installedFiles
$missingFiles = @($expectedInventory.Keys | Where-Object { !$installedInventory.ContainsKey($_) })
$extraFiles = @($installedInventory.Keys | Where-Object { !$expectedInventory.ContainsKey($_) })
$wrongContents = @($expectedInventory.Keys | Where-Object {
    $installedInventory.ContainsKey($_) -and $installedInventory[$_] -ne $expectedInventory[$_]
})
if ($missingFiles.Count -or $extraFiles.Count -or $wrongContents.Count) {
    throw "Installed game payload mismatch: missing=$($missingFiles.Count), extra=$($extraFiles.Count), content_mismatch=$($wrongContents.Count)"
}
foreach ($name in @('default.xex', 'default_decrypted.xex')) {
    $sourceHash = (Get-FileHash -LiteralPath (Join-Path $GameRoot $name) -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash -LiteralPath (Join-Path $installedGameRoot $name) -Algorithm SHA256).Hash
    if ($sourceHash -ne $installedHash) { throw "$name did not install byte-for-byte" }
}
if (!(Test-Path -LiteralPath (Join-Path $verifyRoot 'edf2027.exe')) -or
    !(Test-Path -LiteralPath (Join-Path $verifyRoot 'edf2027_server.exe')) -or
    !(Test-Path -LiteralPath (Join-Path $verifyRoot 'edf2027_master.exe')) -or
    !(Test-Path -LiteralPath (Join-Path $verifyRoot 'edf2027_authoritative_host.exe'))) {
    throw 'One or more required client/server executables did not install.'
}
foreach ($payload in @(
    @{ Root = $PayloadBin; Recursive = $false },
    @{ Root = $PayloadData; Recursive = $true }
)) {
    $sourceFiles = if ($payload.Recursive) {
        Get-ChildItem -LiteralPath $payload.Root -File -Recurse -Force
    } else {
        # The installer maps BuildBin\* to {app}, so only top-level runtime
        # files are installed. Ignore generated logs and native shader caches
        # that may live below this folder after a client launch.
        Get-ChildItem -LiteralPath $payload.Root -File -Force
    }
    foreach ($sourceFile in $sourceFiles) {
        $relative = $sourceFile.FullName.Substring($payload.Root.Length).TrimStart('\')
        $installedFile = Join-Path $verifyRoot $relative
        if (!(Test-Path -LiteralPath $installedFile -PathType Leaf)) {
            throw "Required installer payload did not install: $relative"
        }
        $sourceHash = (Get-FileHash -LiteralPath $sourceFile.FullName -Algorithm SHA256).Hash
        $installedHash = (Get-FileHash -LiteralPath $installedFile -Algorithm SHA256).Hash
        if ($sourceHash -ne $installedHash) { throw "Installer payload differs after install: $relative" }
    }
}
if (!(Test-Path -LiteralPath (Join-Path $verifyRoot 'docs\multiplayer.md')) -or
    !(Test-Path -LiteralPath (Join-Path $verifyRoot 'docs\multiplayer-networking.md')) -or
    !(Test-Path -LiteralPath (Join-Path $verifyRoot 'examples\server_owned_ant_combat.luau')) -or
    !(Test-Path -LiteralPath (Join-Path $verifyRoot 'server-data\scripts\server_owned_ant_combat.luau')) -or
    !(Test-Path -LiteralPath (Join-Path $verifyRoot 'tools\native-benchmark-skipintro-input.txt')) -or
    !(Test-Path -LiteralPath (Join-Path $verifyRoot 'tools\native-headless-mission-input.txt'))) {
    throw 'Self-host scripts, data, or instructions are incomplete in the installation.'
}
$programs = [Environment]::GetFolderPath([Environment+SpecialFolder]::Programs)
$shortcutPath = Join-Path $programs 'EDF2027 Private Test\EDF2027.lnk'
if (!(Test-Path -LiteralPath $shortcutPath -PathType Leaf)) { throw "Installed game shortcut missing: $shortcutPath" }
$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
$expectedGameParameter = '--game_data_root="' + $installedGameRoot + '"'
if ([System.IO.Path]::GetFullPath($shortcut.TargetPath) -ne [System.IO.Path]::GetFullPath((Join-Path $verifyRoot 'edf2027.exe')) -or
    !$shortcut.Arguments.Contains($expectedGameParameter) -or
    !(Test-Path -LiteralPath (Join-Path $installedGameRoot 'default.xex') -PathType Leaf)) {
    throw 'Installed game shortcut does not resolve its --game_data_root to the installed XEX folder.'
}
$hostShortcutPath = Join-Path $programs 'EDF2027 Private Test\Experimental Guest-Backed Host.lnk'
if (!(Test-Path -LiteralPath $hostShortcutPath -PathType Leaf)) { throw "Installed guest host shortcut missing: $hostShortcutPath" }
$hostShortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($hostShortcutPath)
$expectedHostInput = '--input-script "' + (Join-Path $verifyRoot 'tools\native-headless-mission-input.txt') + '"'
if ([System.IO.Path]::GetFullPath($hostShortcut.TargetPath) -ne [System.IO.Path]::GetFullPath((Join-Path $verifyRoot 'edf2027_authoritative_host.exe')) -or
    !$hostShortcut.Arguments.Contains($expectedHostInput) -or
    !$hostShortcut.Arguments.Contains('--mission-actor-reconciliation') -or
    !$hostShortcut.Arguments.Contains('--trace-input-consumers') -or
    !$hostShortcut.Arguments.Contains('--scripts "' + (Join-Path $verifyRoot 'server-data\scripts') + '"')) {
    throw 'Installed guest host shortcut does not use the bundled headless mission route.'
}

$summary = @(
    "Installer: $installer",
    "SDK: official ReXGlue v0.10.0 source build; stock generator fallback in isolated staging source.",
    'Guest probe dispatch traces are disabled because stock generator output uses a different dispatch layout; host/runtime input hooks remain compiled.',
    "Game payload files: $($expectedGameFiles.Count); bytes: $(($expectedGameFiles | Measure-Object Length -Sum).Sum); staged and installed file contents verified by SHA-256.",
    'Temporary install: complete game payload and shortcut target verified by SHA-256/path checks; removed after verification.',
    "Client/server payload files: $((Get-ChildItem $PayloadBin -File -Force).Count); includes client, dedicated server, directory server, guest-backed authority host, controller mappings, D3D compiler, and bundled runtime DLLs; every installer-mapped file SHA-256 verified after install.",
    "FidelityFX: $ffxStatus."
)
[System.IO.File]::WriteAllLines((Join-Path $PackageRoot 'verification.txt'), $summary, [System.Text.Encoding]::UTF8)
Write-Host ($summary -join [Environment]::NewLine)

if (!$KeepVerificationInstall) {
    $uninstaller = Join-Path $verifyRoot 'unins000.exe'
    if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
        $uninstall = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -Wait -PassThru
        if ($uninstall.ExitCode -ne 0) { throw "Verification uninstall returned $($uninstall.ExitCode)" }
    }
}
