# IC4Ext documentation

このdirectoryにはIC4Extの設計、実装状態、build、実機検証手順を置く。

IC4Ext 2.0.0では、D3D11とD3D12の両backendでReadOnly frame pipelineを正式consumer architectureとして整備している。旧physical-copy fan-out資料より、backend別ReadOnly文書を優先する。

## 推奨読書順

### D3D11 ReadOnly pipelineを利用・実装する場合

1. `d3d11/READONLY_PIPELINE.md`  
   D3D11 ReadOnly pipeline、IC4 bytesからFramePoolへのdirect compute変換、immediate-context同期、中央timestamp sync、runtime output、readbackを定義する。
2. `d3d11/SYNTHETIC_FRAME_SOURCE.md`  
   実cameraなしで任意size・fps・timestamp offsetのRGBA8 D3D11 frameを生成し、capture-thread/sync/readback経路を検証する。
3. `../samples/MultiPipelineStressD3D11/README.md`  
   実cameraまたはsynthetic sourceで10処理を同時実行するstress sampleのbuild、実行、合否条件。
4. `design/03_D3D11Backend.md`  
   D3D11 backendとlegacy APIの背景資料。
5. `design/13_ReadbackAndCpuFrame.md`  
   CPU readbackとCpuFrameの共通仕様。

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
6. backend別ReadOnly文書

## 設計ドキュメント一覧

| File | 内容 | 状態 |
|---|---|---|
| `design/00_OverallDesign.md` | 全体構成、D3D11とD3D12 ReadOnly経路 | current |
| `design/01_CoreTypes.md` | Core型、format、metadata、stats | current |
| `design/02_IC4DeviceAndStream.md` | IC4 device selection / stream setup | current |
| `design/03_D3D11Backend.md` | D3D11 backend | legacy/background |
| `design/04_FormatConversion.md` | format変換 | current |
| `design/05_CameraCapture.md` | 旧D3D11 CameraCapture詳細 | legacy D3D11 |
| `design/06_CameraCaptureThread.md` | 旧D3D11 copy fan-out capture thread | legacy D3D11 |
| `design/07_FrameSyncThread.md` | 旧D3D11 frame sync | legacy D3D11 |
| `design/08_BuildAndSampleTest.md` | build / sample / test | current |
| `design/09_DummyCameraCaptureAndControlSink.md` | D3D11中心のDummyCamera | legacy D3D11 |
| `design/10_D3D12Backend.md` | D3D12 ReadOnly backend | current |
| `design/11_BackendSelection.md` | backend selection | current |
| `design/12_D3D12FrameSyncThread.md` | timestamp-only central sync | current |
| `design/13_ReadbackAndCpuFrame.md` | CPU readback | current |
| `design/14_CurrentStatusAndRoadmap.md` | 実装状態、検証、TODO | current |
| `design/15_DXCRuntime.md` | DXC runtime自動取得・配置 | current |
| `d3d11/READONLY_PIPELINE.md` | D3D11 ReadOnly architecture | authoritative |
| `d3d11/SYNTHETIC_FRAME_SOURCE.md` | D3D11 camera-free GPU source | authoritative |
| `d3d12/READONLY_PIPELINE.md` | D3D12正式アーキテクチャ | authoritative |
| `d3d12/VALIDATION_AND_TUNING.md` | 実機評価・調整・予備結果 | authoritative |
| `d3d12/SYNTHETIC_FRAME_SOURCE.md` | D3D12 camera-free GPU frame source | authoritative |

## Version / compatibility note

- project version: `2.0.0`
- D3D11 public namespace: `IC4Ext::D3D11`
- D3D11 public include: `<IC4Ext/D3D11/ReadOnlyPipeline.hpp>`
- D3D12 public namespace: `IC4Ext::D3D12`
- D3D12 public include: `<IC4Ext/D3D12/ReadOnlyPipeline.hpp>`
- 旧physical-copy fan-out APIとのsource compatibilityは保証しない。

## 過去資料

`IC4Ext_SelfContainedDesign_v1_3_FULL.md`など、v1.xを前提とする統合仕様メモは履歴資料である。2.0.0の実装判断にはbackend別`READONLY_PIPELINE.md`を優先する。
