# IC4Ext documentation

このdirectoryにはIC4Extの設計、実装状態、build、実機検証手順を置く。

IC4Ext 2.0.0では、D3D12側の正式経路はReadOnly frame pipelineである。旧D3D12 physical-copy fan-out資料より、以下のD3D12文書を優先する。

## 推奨読書順

### D3D12 ReadOnly pipelineを利用・実装する場合

1. `d3d12/READONLY_PIPELINE.md`  
   D3D12 ReadOnly pipelineの正式設計。所有権、FramePool、同期、runtime output、GPU lifetime、readbackを定義する。
2. `d3d12/VALIDATION_AND_TUNING.md`  
   pool sizing、timestamp tolerance、10-pipeline stress、実測値、合否判定、10分soak手順。
3. `d3d12/SYNTHETIC_FRAME_SOURCE.md`  
   実カメラなしで任意サイズ・fps・timestamp offsetのRGBA ReadOnlyFrameをGPU生成し、capture-thread/sync/readback経路を検証する方法。
4. `../samples/MultiPipelineStressD3D12/README.md`  
   10処理同時sampleのbuild、OpenCV設定、実行command、出力の読み方。
5. `design/10_D3D12Backend.md`  
   D3D12Helper統合、resource/queue/fenceの責務。
6. `design/12_D3D12FrameSyncThread.md`  
   timestamp-nearest、完全同期set、runtime output registry。
7. `design/13_ReadbackAndCpuFrame.md`  
   ReadOnlyFrameからのreadbackとconsumer分離。
8. `design/14_CurrentStatusAndRoadmap.md`  
   実装済み機能、予備検証結果、残作業。

### ライブラリ全体を読む場合

1. `design/00_OverallDesign.md`
2. `design/01_CoreTypes.md`
3. `design/02_IC4DeviceAndStream.md`
4. `design/04_FormatConversion.md`
5. `design/08_BuildAndSampleTest.md`
6. backend別文書

## 設計ドキュメント一覧

| File | 内容 | 状態 |
|---|---|---|
| `design/00_OverallDesign.md` | 全体構成、D3D11とD3D12 ReadOnly経路 | current |
| `design/01_CoreTypes.md` | Core型、format、metadata、stats | current |
| `design/02_IC4DeviceAndStream.md` | IC4 device selection / stream setup | current |
| `design/03_D3D11Backend.md` | D3D11 backend | legacy/current D3D11 |
| `design/04_FormatConversion.md` | format変換 | current |
| `design/05_CameraCapture.md` | D3D11 CameraCapture詳細 | D3D11-specific |
| `design/06_CameraCaptureThread.md` | D3D11 copy fan-out capture thread | D3D11-specific |
| `design/07_FrameSyncThread.md` | D3D11 frame sync | D3D11-specific |
| `design/08_BuildAndSampleTest.md` | build / sample / test | current |
| `design/09_DummyCameraCaptureAndControlSink.md` | D3D11中心のDummyCamera | legacy D3D11 |
| `design/10_D3D12Backend.md` | D3D12 ReadOnly backend | current |
| `design/11_BackendSelection.md` | backend selection | current |
| `design/12_D3D12FrameSyncThread.md` | timestamp-only central sync | current |
| `design/13_ReadbackAndCpuFrame.md` | CPU readback | current |
| `design/14_CurrentStatusAndRoadmap.md` | 実装状態、検証、TODO | current |
| `design/15_DXCRuntime.md` | DXC runtime自動取得・配置 | current |
| `d3d12/READONLY_PIPELINE.md` | D3D12正式アーキテクチャ | authoritative |
| `d3d12/VALIDATION_AND_TUNING.md` | 実機評価・調整・予備結果 | authoritative |
| `d3d12/SYNTHETIC_FRAME_SOURCE.md` | camera-free GPU frame sourceと統合test | authoritative |

## Version / compatibility note

- project version: `2.0.0`
- D3D12 public namespace: `IC4Ext::D3D12`
- D3D12 public include: `<IC4Ext/D3D12/ReadOnlyPipeline.hpp>`
- D3D12旧physical-copy fan-out APIとの互換性は保証しない。
- D3D11側は既存設計を残しているが、D3D12の新設計と完全に対称ではない。

## 過去資料

`IC4Ext_SelfContainedDesign_v1_3_FULL.md`など、v1.xを前提とする統合仕様メモは履歴資料である。D3D12 2.0.0の実装判断には、`d3d12/READONLY_PIPELINE.md`を優先する。
