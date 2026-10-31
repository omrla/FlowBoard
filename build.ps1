# Command-line build of FlowBoard.slnx with MSBuild. (In Visual Studio, just open it.)
#   .\build.ps1                 # Release build: x64\Release\FlowBoard.exe
#   .\build.ps1 -Config Debug
param(
    [string]$Config = "Release"
)

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "Visual Studio with the 'Desktop development with C++' workload is required." }

& $msbuild (Join-Path $PSScriptRoot "FlowBoard.slnx") /p:Configuration=$Config /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
Write-Host "Built $PSScriptRoot\x64\$Config\FlowBoard.exe"
