# Downloads the two hand-tracking models (MediaPipe palm detector and hand landmarker,
# ONNX exports from the OpenCV Zoo, Apache-2.0) into models\. The Visual Studio project
# runs this when either file is missing.
param(
    [string]$Destination = (Join-Path $PSScriptRoot "..\models")
)
$ErrorActionPreference = "Stop"
$models = @(
    "palm_detection_mediapipe/palm_detection_mediapipe_2023feb.onnx",
    "handpose_estimation_mediapipe/handpose_estimation_mediapipe_2023feb.onnx"
)
New-Item -ItemType Directory -Force $Destination | Out-Null
foreach ($model in $models) {
    $name = Split-Path $model -Leaf
    $file = Join-Path $Destination $name
    if (Test-Path $file) { continue }
    Write-Host "Downloading $name from the OpenCV Zoo"
    # Downloaded under another name first, so a failed download never looks complete.
    $partial = "$file.download"
    Invoke-WebRequest -Uri "https://github.com/opencv/opencv_zoo/raw/main/models/$model" -OutFile $partial -UseBasicParsing
    if ((Get-Item $partial).Length -lt 100000) {
        Remove-Item $partial
        throw "Could not download $name. Download it from the OpenCV Zoo into $Destination."
    }
    Move-Item $partial $file
}
