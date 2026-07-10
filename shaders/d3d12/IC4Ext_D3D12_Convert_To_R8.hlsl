ByteAddressBuffer gInput : register(t0);
RWTexture2D<float> gOutput : register(u0);

cbuffer Params : register(b0)
{
    uint gWidth;
    uint gHeight;
    uint gInputRowPitchBytes;
    uint gInputFormat;
    uint gBayerPattern;
    uint gReserved0;
    uint gReserved1;
    uint gReserved2;
};

uint LoadByteAtAddress(uint byteAddress)
{
    const uint aligned = byteAddress & ~3u;
    const uint shift = (byteAddress & 3u) * 8u;
    return (gInput.Load(aligned) >> shift) & 0xffu;
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gWidth || tid.y >= gHeight) return;
    const uint byteAddress = tid.y * gInputRowPitchBytes + tid.x;
    gOutput[tid.xy] = float(LoadByteAtAddress(byteAddress)) / 255.0f;
}
