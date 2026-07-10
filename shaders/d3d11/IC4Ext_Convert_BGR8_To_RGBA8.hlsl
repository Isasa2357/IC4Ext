Buffer<uint> InputBytes : register(t0);
RWTexture2D<float4> Output : register(u0);

cbuffer ConvertConstants : register(b0)
{
    uint width;
    uint height;
    uint inputRowPitchBytes;
    uint inputPixelFormat;
};

uint LoadByte(uint offset)
{
    return InputBytes[offset] & 0xffu;
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;
    uint offset = tid.y * inputRowPitchBytes + tid.x * 3u;
    float b = LoadByte(offset + 0u) / 255.0f;
    float g = LoadByte(offset + 1u) / 255.0f;
    float r = LoadByte(offset + 2u) / 255.0f;
    Output[tid.xy] = float4(r, g, b, 1.0f);
}
