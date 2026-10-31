# Downloads ONNX Runtime with DirectML (Microsoft's official NuGet package, MIT licence) into
# third_party\onnxruntime. FlowBoard uses it to run the hand models on the graphics card.
# The Visual Studio project runs this when the folder is missing.
# DirectML itself ships with Windows 10 1903 and later.
param(
    [string]$Version = "1.24.4",
    [string]$Destination = (Join-Path $PSScriptRoot "..\third_party\onnxruntime")
)
$ErrorActionPreference = "Stop"
$marker = Join-Path $Destination "lib\onnxruntime.lib"
if (Test-Path $marker) { exit 0 }

$work = Join-Path $Destination ".download"
New-Item -ItemType Directory -Force $work | Out-Null
$package = Join-Path $work "package.zip"
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/$Version/" +
       "microsoft.ml.onnxruntime.directml.$Version.nupkg"
Write-Host "Downloading ONNX Runtime $Version with DirectML from nuget.org"
Invoke-WebRequest -Uri $url -OutFile $package -UseBasicParsing
$unpacked = Join-Path $work "package"
Expand-Archive -Path $package -DestinationPath $unpacked -Force

foreach ($dir in "include", "bin", "lib") {
    New-Item -ItemType Directory -Force (Join-Path $Destination $dir) | Out-Null
}
Copy-Item "$unpacked\build\native\include\*" (Join-Path $Destination "include") -Force
Copy-Item "$unpacked\runtimes\win-x64\native\*.dll" (Join-Path $Destination "bin") -Force
Copy-Item "$unpacked\LICENSE", "$unpacked\ThirdPartyNotices.txt" $Destination -Force
# The import library goes last: its presence marks a complete download.
Copy-Item "$unpacked\runtimes\win-x64\native\onnxruntime.lib" (Join-Path $Destination "lib") -Force
Remove-Item -Recurse -Force $work
Write-Host "ONNX Runtime $Version ready in $Destination"
