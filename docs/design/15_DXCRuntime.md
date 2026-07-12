# 15. DXC runtime

D3D shader runtime compileを使うsample/testでは、Windows実行時に次のDLLが必要になる場合がある。

```text
dxcompiler.dll
dxil.dll
```

IC4Ext 2.0.0では、CMakeがMicrosoft.Direct3D.DXCを検索またはNuGetから取得し、必要なexecutableと同じdirectoryへ両DLLをcopyできる。

## 1. Resolution order

`ic4ext_resolve_dxc_runtime()`は概ね次の順で探す。

```text
1. IC4EXT_DXC_RUNTIME_DIR
2. source tree内の既存packages
3. build tree内の既存packages
4. IC4EXT_DXC_NUGET_ROOT
5. user global NuGet cache
6. Microsoft.Direct3D.DXCをNuGet CLIでrestore
7. NuGet package URLから直接download/extract
```

`dxcompiler.dll`と`dxil.dll`の両方が存在するdirectoryだけを有効とする。

## 2. CMake variables

```text
IC4EXT_DXC_RUNTIME_DIR
  両DLLを含むdirectoryの明示override。

IC4EXT_FETCH_DXC_RUNTIME
  見つからない場合にNuGetから取得する。default ON。

IC4EXT_DXC_NUGET_PACKAGE
  default Microsoft.Direct3D.DXC。

IC4EXT_DXC_NUGET_VERSION
  optional。空ならfeed側でversionを解決。

IC4EXT_DXC_NUGET_ROOT
  restore/download先。defaultはbuild treeの_deps/dxc_nuget。
```

## 3. Recommended configure

通常:

```bat
cmake -S . -B out\build\v2_d3d12 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON
```

version固定:

```bat
cmake -S . -B out\build\v2_d3d12 ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON ^
  -DIC4EXT_DXC_NUGET_VERSION=1.9.2602.24
```

local directory override:

```bat
set "IC4EXT_DXC_RUNTIME_DIR=C:\path\to\Microsoft.Direct3D.DXC\build\native\bin\x64"

cmake -S . -B out\build\v2_d3d12 ^
  -DIC4EXT_DXC_RUNTIME_DIR="%IC4EXT_DXC_RUNTIME_DIR%"
```

## 4. Deployment to executable directory

sample/test CMakeListsで次を呼ぶ。

```cmake
ic4ext_copy_dxc_runtime_to_target(MyExecutable)
```

build後:

```text
<TARGET_FILE_DIR>/MyExecutable.exe
<TARGET_FILE_DIR>/dxcompiler.dll
<TARGET_FILE_DIR>/dxil.dll
```

同じDLLが既にあれば`copy_if_different`を使う。

## 5. NuGet CLI fallback

`nuget.exe`が見つかる場合、CMakeは概ね次を実行する。

```text
nuget install Microsoft.Direct3D.DXC
  -OutputDirectory <IC4EXT_DXC_NUGET_ROOT>
  -NonInteractive
  -DirectDownload
```

`IC4EXT_DXC_NUGET_VERSION`が設定されていれば`-Version`を追加する。

NuGet CLIがない、またはrestoreに失敗した場合はNuGet package URLから`.nupkg`をdownloadし、CMakeのarchive extractで展開する。

## 6. Verification

まずexecutableを探す。

```bat
for /f "delims=" %F in ('dir /s /b out\build\v2_d3d12\MultiPipelineStressD3D12.exe') do set "STRESS_EXE=%F"
echo %STRESS_EXE%
```

表示されたdirectoryで確認する。

```bat
dir dxcompiler.dll
dir dxil.dll
```

build tree全体から探す場合:

```bat
dir /s /b out\build\v2_d3d12\dxcompiler.dll
dir /s /b out\build\v2_d3d12\dxil.dll
```

## 7. PATH is normally unnecessary

DLLをexeと同じdirectoryへcopyするため、通常はDXC directoryをPATHへ追加する必要はない。

PATH追加が必要なのは、copy helperを使わない独自executableや、configure/build外でDLLを直接利用する場合である。

```bat
set "PATH=%IC4EXT_DXC_RUNTIME_DIR%;%PATH%"
```

## 8. Common failures

### Explicit directory is incomplete

```text
IC4EXT_DXC_RUNTIME_DIR is set but does not contain both DLLs
```

両方を同じdirectoryへ置く。

### Offline configure

internetなしでNuGet restoreできない場合、事前にpackageを展開し、`IC4EXT_DXC_RUNTIME_DIR`を明示する。

### Wrong architecture

x64 buildではx64 DLLを使う。x86 DLLを混在させない。

### DLL copied but load still fails

次を確認する。

```text
exeとDLLが同じdirectory
x64/x86一致
dxcompiler.dllとdxil.dllのversion組合せ
OpenCV等、別DLLのmissing errorではないか
```
