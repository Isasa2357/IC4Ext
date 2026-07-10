# 15. DXC runtime

D3D11Helper / D3D12Helper の shader runtime compile や sample/test 実行時には `dxcompiler.dll` と `dxil.dll` が必要になる場合があります。

## Required files

```txt
dxcompiler.dll
dxil.dll
```

この2つが同じディレクトリにある必要があります。

## Recommended project-local layout

このプロジェクトでは、現在以下のような project-local 配置を想定できます。

```txt
<IC4Ext root>/package/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxcompiler.dll
<IC4Ext root>/package/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxil.dll
```

NuGet の既定出力を使う場合は `package` ではなく `packages` になる場合があります。

## CMake variables

IC4Ext 側:

```txt
IC4EXT_DXC_RUNTIME_DIR
```

Helper 側:

```txt
D3D11HELPER_DXC_RUNTIME_DIR
D3D12HELPER_DXC_RUNTIME_DIR
```

通常は3つとも同じ directory を渡します。

```bat
set "IC4EXT_DXC_RUNTIME_DIR=%CD%\package\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\bin\x64"

cmake -S . -B out\build\default ^
  -DIC4EXT_DXC_RUNTIME_DIR="%IC4EXT_DXC_RUNTIME_DIR%" ^
  -DD3D11HELPER_DXC_RUNTIME_DIR="%IC4EXT_DXC_RUNTIME_DIR%" ^
  -DD3D12HELPER_DXC_RUNTIME_DIR="%IC4EXT_DXC_RUNTIME_DIR%"
```

## Runtime PATH

Sample/test executable が DLL を見つけられるように、実行前に PATH に追加します。

```bat
set "PATH=%IC4EXT_DXC_RUNTIME_DIR%;%PATH%"
```

## Verification

```bat
dir "%IC4EXT_DXC_RUNTIME_DIR%\dxcompiler.dll"
dir "%IC4EXT_DXC_RUNTIME_DIR%\dxil.dll"
```

両方が見つかれば DXC runtime path は正しく設定されています。
