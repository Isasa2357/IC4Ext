# IC4Ext documentation

このディレクトリには IC4Ext の設計メモを置いています。現在の実装に追従している主な入口は以下です。

## 推奨読書順

1. `design/00_OverallDesign.md`  
   ライブラリ全体の構成、責務分担、現在の実装状態。
2. `design/01_CoreTypes.md`  
   `FrameTiming`、`CpuFrame`、format enum、stats などの Core 型。
3. `design/04_FormatConversion.md`  
   入力 pixel format、GPU output format、CPU readback format の整理。
4. `design/10_D3D12Backend.md`  
   D3D12Helper 統合版 D3D12 backend の設計。
5. `design/13_ReadbackAndCpuFrame.md`  
   D3D11 / D3D12 GPU frame から `CpuFrame` へ readback する設計。
6. `design/14_CurrentStatusAndRoadmap.md`  
   実装済み機能、未実装機能、次にやるべきこと。

## 設計ドキュメント一覧

| File | 内容 |
|---|---|
| `00_OverallDesign.md` | 全体設計 |
| `01_CoreTypes.md` | Core 型、format、metadata、stats |
| `02_IC4DeviceAndStream.md` | IC4 device selection / stream setup |
| `03_D3D11Backend.md` | D3D11 backend |
| `04_FormatConversion.md` | format 変換 |
| `05_CameraCapture.md` | CameraCapture API |
| `06_CameraCaptureThread.md` | capture thread / output queue |
| `07_FrameSyncThread.md` | D3D11 frame sync |
| `08_BuildAndSampleTest.md` | build / sample / test |
| `09_DummyCameraCaptureAndControlSink.md` | DummyCameraCapture と setter forwarding |
| `10_D3D12Backend.md` | D3D12Helper 統合 backend |
| `11_BackendSelection.md` | D3D11/D3D12 backend selection |
| `12_D3D12FrameSyncThread.md` | D3D12 frame sync |
| `13_ReadbackAndCpuFrame.md` | CPU readback |
| `14_CurrentStatusAndRoadmap.md` | 現在の実装状態と TODO |
| `15_DXCRuntime.md` | DXC runtime の配置と CMake 設定 |

`IC4Ext_SelfContainedDesign_v1_3_FULL.md` は過去の統合仕様メモです。最新状態の入口としては、この `docs/README.md` と上記の個別ファイルを優先してください。
