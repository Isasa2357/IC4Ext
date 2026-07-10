# 03. D3D11 backend 設計 v1.3

## 1. 目的

この文書は IC4Ext の D3D11 backend に必要な device/context、resource、fence、shader 読み込み、copy の仕様を定義する。

初期実装では D3D11 backend のみを実装する。D3D12 backend は存在しない。

## 2. 外部から受け取る D3D11 core

`D3D11CameraCapture::open()` は D3D11Helper の core object を受け取る。

```cpp
bool open(const IC4DeviceSelector& selector,
          const CameraCaptureConfig& config,
          D3D11CoreLib::D3D11Core* core);
```

`core == nullptr` は open 失敗にする。

`core` から最低限取得できる必要があるもの:

```cpp
ID3D11Device* device;
ID3D11DeviceContext* immediateContext;
```

IC4Ext は `core` を所有しない。`D3D11CameraCapture` と `D3D11CameraCaptureThread` の寿命中、呼び出し側は `core` を生存させる。

## 3. D3D11.4 fence 前提

IC4Ext は D3D11 GPU work 完了通知に D3D11 fence を使う。

必要 interface:

```cpp
ID3D11Device5
ID3D11DeviceContext4
ID3D11Fence
```

backend 初期化時に次を行う。

```cpp
ComPtr<ID3D11Device5> device5;
ComPtr<ID3D11DeviceContext4> context4;
ComPtr<ID3D11Fence> fence;

HRESULT hr1 = device->QueryInterface(IID_PPV_ARGS(&device5));
HRESULT hr2 = context->QueryInterface(IID_PPV_ARGS(&context4));
HRESULT hr3 = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
```

いずれかが失敗した場合、backend 初期化は失敗する。

`ID3D11Query(D3D11_QUERY_EVENT)` fallback は実装しない。

## 4. D3D11FenceManager

fence value の採番と token 生成を管理する class を作る。

```cpp
class D3D11FenceManager
{
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    D3D11ReadyToken signal();
    bool wait(const D3D11ReadyToken& token, std::uint32_t timeoutMs);
    bool isReady(const D3D11ReadyToken& token) const;

private:
    Microsoft::WRL::ComPtr<ID3D11Device5> device5_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    std::atomic<std::uint64_t> nextValue_{1};
};
```

`signal()` は、直前までに immediate context に投入された GPU command の後ろに fence signal を入れる。

```cpp
std::uint64_t value = nextValue_.fetch_add(1);
context4_->Signal(fence_.Get(), value);
return D3D11ReadyToken{ fence_, value };
```

## 5. D3D11ReadyToken

```cpp
struct D3D11ReadyToken
{
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    std::uint64_t value = 0;

    bool isValid() const noexcept
    {
        return fence != nullptr && value != 0;
    }

    bool isReady() const
    {
        return isValid() && fence->GetCompletedValue() >= value;
    }

    bool wait(std::uint32_t timeoutMs = INFINITE) const;
};
```

`wait()` の実装方針:

```txt
1. token が invalid なら false
2. GetCompletedValue() >= value なら true
3. CreateEvent
4. fence->SetEventOnCompletion(value, event)
5. WaitForSingleObject(event, timeoutMs)
6. WAIT_OBJECT_0 なら true、それ以外は false
```

## 6. raw upload buffer

IC4 image buffer は CPU memory である。D3D11 compute shader で扱うため、まず raw byte buffer として GPU へ upload する。

BGR8 は 24-bit であり、D3D11 texture format として直接扱いにくい。そのため、初期実装では input を texture としてではなく raw byte buffer として扱うことを推奨する。

推奨 resource:

```txt
ID3D11Buffer rawInputBuffer
  bind: D3D11_BIND_SHADER_RESOURCE
  usage: D3D11_USAGE_DEFAULT または D3D11_USAGE_DYNAMIC
  data: IC4 image buffer の row pitch を含む raw bytes
```

SRV は buffer SRV として作る。compute shader は byte offset を計算して input を読む。

実装の簡略化として、CPU 側で BGR8 を BGRA8 へ expand してから texture upload してもよいが、初期設計上の推奨は raw byte buffer + compute shader である。

## 7. output texture

`GpuFrameFormat` に応じて output texture を作る。

```txt
RGBA8 -> DXGI_FORMAT_R8G8B8A8_UNORM
R8    -> DXGI_FORMAT_R8_UNORM
```

基本 descriptor:

```cpp
D3D11_TEXTURE2D_DESC desc = {};
desc.Width = width;
desc.Height = height;
desc.MipLevels = 1;
desc.ArraySize = 1;
desc.Format = dxgiFormat;
desc.SampleDesc.Count = 1;
desc.Usage = D3D11_USAGE_DEFAULT;
desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
```

`FrameOutputSpec::createSrv` が true なら SRV を作る。compute shader が output へ書き込むため、通常は UAV も作る。

`DXGI_FORMAT_R8_UNORM` の UAV support は device feature に依存する可能性があるため、初期化時に `CheckFormatSupport` で検証する。未対応なら `Mono8 -> R8` を unsupported にするか、内部 RGBA8 出力に fallback する。ただし fallback した場合は metadata に実 output format を正しく残す。

## 8. shader 読み込み

shader は `.hlsl` と `.cso` の両方をサポートする。

推奨 class:

```cpp
class D3D11ShaderLoader
{
public:
    bool loadComputeShader(ID3D11Device* device,
                           const ShaderLoadConfig& config,
                           std::string_view baseName,
                           Microsoft::WRL::ComPtr<ID3D11ComputeShader>& outShader,
                           std::vector<std::uint8_t>* outBytecode = nullptr);
};
```

`baseName` 例:

```txt
IC4Ext_Convert_Bayer8_To_RGBA8
IC4Ext_Convert_BGR8_To_RGBA8
IC4Ext_Convert_BGRa8_To_RGBA8
IC4Ext_Convert_Mono8_To_R8
IC4Ext_Convert_Mono8_To_RGBA8
```

読み込み規則:

```txt
ShaderInputKind::CsoFile:
  shaderDirectory / (baseName + ".cso") を読む

ShaderInputKind::HlslFile:
  shaderDirectory / (baseName + ".hlsl") を読む
  entryPoint / target で compile する

ShaderInputKind::Auto:
  preferCsoWhenBothExist が true なら .cso -> .hlsl の順
  false なら .hlsl -> .cso の順
```

`.hlsl` compile は D3D11Helper の機能を使ってよい。D3D11Helper 側に compile helper がない場合は、`D3DCompileFromFile` または DXC を IC4Ext 側で使ってよい。

## 9. constant buffer

compute shader へ渡す parameter は constant buffer にまとめる。

```cpp
struct ConvertConstants
{
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t inputRowPitchBytes;
    std::uint32_t inputPixelFormat; // CameraPixelFormat を uint32_t 化
};
```

Bayer pattern を shader に渡すには `inputPixelFormat` を使う。

## 10. compute dispatch

dispatch group size は shader と合わせる。

例:

```hlsl
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
```

dispatch count:

```cpp
UINT groupsX = (width  + 15) / 16;
UINT groupsY = (height + 15) / 16;
context->Dispatch(groupsX, groupsY, 1);
```

dispatch 後に UAV を unbind する。

```cpp
ID3D11UnorderedAccessView* nullUav = nullptr;
context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
```

その後、`D3D11FenceManager::signal()` を呼び、返却 frame の `ready` に token を入れる。

## 11. D3D11FrameCopier

複数 output queue へ配送する場合、consumer ごとに独立 texture を持たせる。

```cpp
class D3D11FrameCopier
{
public:
    bool copyFrame(const D3D11CameraFrame& src,
                   D3D11CameraFrame& dst);
};
```

copy 方針:

1. `src` と同じ size / format の texture を作る。
2. `ID3D11DeviceContext::CopyResource` または `CopySubresourceRegion` で copy する。
3. copy 後に fence signal し、`dst.ready` に token を入れる。
4. CPU wait はしない。

metadata と timing は src から dst へコピーする。

## 12. CPU wait 禁止の原則

`read()`、`FrameConverter`、`FrameCopier`、`CameraCaptureThread` は原則として GPU 完了を CPU wait しない。

返却 frame には `D3D11ReadyToken` を持たせる。consumer は texture を使う直前に必要なら `frame.ready.wait()` を呼ぶ。

ただし、test や sample で frame の完了を確認する目的では wait してよい。
