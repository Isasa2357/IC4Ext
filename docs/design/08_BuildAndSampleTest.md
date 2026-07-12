# 08. Build and sample/test guide

この文書はIC4Ext 2.0.0 branchのWindows/MSVC build、D3D12 ReadOnly sample、CTest、OpenCV stress sampleの手順をまとめる。

## 1. Requirements

- Windows 10/11
- Visual Studio 2022 Build ToolsまたはIDE
- x64 MSVC toolchain
- CMake 3.21+
- IC Imaging Control 4 SDK
- D3D12対応GPU
- internet accessまたは事前取得済み依存package
- OpenCVは`MultiPipelineStressD3D12`をbuildする場合のみ必要

## 2. IC4 SDK

例:

```bat
set "IC4_SDK_ROOT=C:\Users\MiyafujiLab2\AppData\Local\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"
set "PATH=%IC4_SDK_ROOT%\bin\x64;%PATH%"
```

必要なlayout:

```text
%IC4_SDK_ROOT%\include
%IC4_SDK_ROOT%\lib\x64\ic4core.lib
%IC4_SDK_ROOT%\bin\x64\ic4core.dll
```

## 3. DXC runtime

既定ではCMakeが次を行う。

```text
既存IC4EXT_DXC_RUNTIME_DIRを確認
source/build/global NuGet cacheを検索
Microsoft.Direct3D.DXCをNuGetから取得
sample/test targetのexe横へdxcompiler.dllとdxil.dllをcopy
```

通常は次だけでよい。

```text
-DIC4EXT_FETCH_DXC_RUNTIME=ON
```

versionを固定する場合:

```text
-DIC4EXT_DXC_NUGET_VERSION=1.9.2602.24
```

明示directoryを使う場合:

```text
-DIC4EXT_DXC_RUNTIME_DIR=C:\path\to\dxc\bin\x64
```

そのdirectoryには両方が必要である。

```text
dxcompiler.dll
dxil.dll
```

## 4. D3D12 configure

失敗時にCMDを閉じない例:

```bat
set "IC4EXT_OK=1"

git fetch origin
git switch agent/ic4ext-v2-d3d12-foundation
git pull --ff-only origin agent/ic4ext-v2-d3d12-foundation

cmake -S . -B out\build\v2_d3d12 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DEPENDENCIES=ON ^
  -DIC4EXT_FETCH_D3D12HELPER=ON ^
  -DIC4EXT_FETCH_THREADKIT=ON ^
  -DIC4EXT_FETCH_NLOHMANN_JSON=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] configure failed. CMD remains open.

if "%IC4EXT_OK%"=="1" cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --parallel

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] build failed. CMD remains open.
```

## 5. OpenCV stress sample configure

OpenCV root例:

```text
C:\personal\iwatake\library\opencv
```

```bat
set "OPENCV_ROOT=C:\personal\iwatake\library\opencv"
set "OpenCV_DIR=%OPENCV_ROOT%\build\x64\vc17\lib"
set "OPENCV_BIN=%OPENCV_ROOT%\build\x64\vc17\bin"
set "PATH=%OPENCV_BIN%;%PATH%"

cmake -S . -B out\build\v2_d3d12 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON ^
  -DOpenCV_DIR="%OpenCV_DIR%"
```

`OpenCVConfig.cmake`の場所が不明な場合:

```bat
for /f "delims=" %F in ('dir /s /b "%OPENCV_ROOT%\OpenCVConfig.cmake" 2^>nul') do set "OpenCV_DIR=%~dpF"
echo OpenCV_DIR=%OpenCV_DIR%
```

## 6. Target-specific build

```bat
cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --target SingleCameraReadOnlyReadbackD3D12 ^
  --parallel

cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --target MultiCameraReadOnlySyncD3D12 ^
  --parallel

cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --target MultiPipelineStressD3D12 ^
  --parallel
```

## 7. D3D12 samples

### IC4DeviceDiagnostics

IC4 device、property、stream情報を確認する。

### SingleCameraReadOnlyReadbackD3D12

```bat
SingleCameraReadOnlyReadbackD3D12.exe ^
  --device-index 0 ^
  --frames 5 ^
  --out readonly_frame.ppm
```

検証内容:

```text
CameraCapture
FramePool lazy creation
PooledFrameConverter
ReadOnlyFrame metadata
producer fence
GPU readback
pool return
```

### MultiCameraReadOnlySyncD3D12

```bat
MultiCameraReadOnlySyncD3D12.exe ^
  --device0 0 ^
  --device1 1 ^
  --duration-sec 60 ^
  --hardware-trigger ^
  --trigger-source Line1 ^
  --timestamp-source host ^
  --max-diff-us 4000 ^
  --input-queue 512 ^
  --output-queue 512 ^
  --pool-initial 128 ^
  --pool-max 256
```

D3D12 synchronizationはtimestamp-nearestのみである。frame-number optionはない。

### MultiPipelineStressD3D12

10個の独立consumerを同時実行する。

```bat
MultiPipelineStressD3D12.exe ^
  --device0 0 ^
  --device1 1 ^
  --warmup-sec 5 ^
  --duration-sec 60 ^
  --hardware-trigger ^
  --trigger-source Line1 ^
  --timestamp-source host ^
  --max-diff-us 30000 ^
  --min-sync-fps 50 ^
  --capture-pool-initial 128 ^
  --capture-pool-max 256 ^
  --max-pending-buffers 128 ^
  --ingress-queue 256 ^
  --latest-queue 1 ^
  --all-frame-queue 32 ^
  --record-fps 160 ^
  --video-codec MJPG ^
  --output-dir stress_output ^
  --csv stress_output\metrics.csv
```

30 ms toleranceは動作確認値であり、最終値ではない。pool exhaustionを0にした後、安定する最小値へ下げる。

詳細:

```text
samples/MultiPipelineStressD3D12/README.md
docs/d3d12/VALIDATION_AND_TUNING.md
```

## 8. JSON state

```bat
--ic4-json0 C:\path\to\camera_state.json ^
--json-device-index0 0 ^
--ic4-json1 C:\path\to\camera_state.json ^
--json-device-index1 0
```

JSON内の次は実効fpsへ影響する。

```text
PixelFormat
Width / Height
AcquisitionFrameRate
ExposureTime
Trigger settings
```

## 9. CTest

### no-camera

```bat
ctest --test-dir out\build\v2_d3d12 ^
  -C Release ^
  -L no_camera ^
  --output-on-failure
```

### D3D12 ReadOnly target selection

```bat
ctest --test-dir out\build\v2_d3d12 ^
  -C Release ^
  --output-on-failure ^
  -R "test_d3d12_(readonly_pipeline|pooled_converter_device|dummy_capture_sync_integration)"
```

### Main D3D12 tests

```text
test_d3d12_core
test_d3d12_shader_reference
test_d3d12_readonly_pipeline
test_d3d12_pooled_converter_device
test_d3d12_dummy_capture_sync_integration
test_d3d12_shader_compile
```

D3D12 deviceを作れない環境では、対象testはreturn code 77でskipできる。

## 10. Test labels

```text
no_camera
camera1
camera2plus
d3d12_device
integration
```

実際のbranchで登録されているlabelは`ctest -N -V`または`ctest --print-labels`で確認する。

## 11. Runtime DLL verification

```bat
for /f "delims=" %F in ('dir /s /b out\build\v2_d3d12\MultiPipelineStressD3D12.exe') do set "STRESS_EXE=%F"
echo STRESS_EXE=%STRESS_EXE%

dir "%~dpSTRESS_EXE%dxcompiler.dll"
dir "%~dpSTRESS_EXE%dxil.dll"
```

CMD variable展開が扱いにくい場合、exeを見つけたdirectoryで直接`dir dxcompiler.dll dxil.dll`を実行する。

## 12. Common failures

### OpenCV not found

```text
-DOpenCV_DIR=<directory containing OpenCVConfig.cmake>
```

### OpenCV DLL not found at runtime

```bat
set "PATH=<opencv build x64 vc17 bin>;%PATH%"
```

### dxcompiler.dll not found

`IC4EXT_FETCH_DXC_RUNTIME=ON`を確認し、exe横の`dxcompiler.dll` / `dxil.dll`を確認する。

### No synchronized sets

確認順:

```text
camera read/push count
pool exhaustion
capture timeout
hardware trigger pulses
timestamp source
max timestamp difference
```

pool exhaustionがある状態でtimestamp toleranceだけを広げない。

### All-frame pipeline drops

`dispatchDrop`または`rejectNew`が増えているconsumerは入力rateへ追従していない。queueを無制限に増やすのではなく、処理方式を改善する。

OpenCV VideoWriterは高fps保存の最終方式ではない。
