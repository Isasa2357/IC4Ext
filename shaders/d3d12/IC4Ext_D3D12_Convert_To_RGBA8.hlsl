ByteAddressBuffer gInput : register(t0);
RWTexture2D<float4> gOutput : register(u0);

cbuffer Params : register(b0)
{
    uint gWidth;
    uint gHeight;
    uint gInputRowPitchBytes;
    uint gInputFormat;   // CameraPixelFormat enum value
    uint gBayerPattern;  // 0:BGGR, 1:GBRG, 2:GRBG, 3:RGGB
    uint gReserved0;
    uint gReserved1;
    uint gReserved2;
};

static const uint FMT_MONO8    = 1u;
static const uint FMT_BAYERRG8 = 2u;
static const uint FMT_BAYERGR8 = 3u;
static const uint FMT_BAYERGB8 = 4u;
static const uint FMT_BAYERBG8 = 5u;
static const uint FMT_BGR8     = 6u;
static const uint FMT_BGRA8    = 7u;

uint BytesPerPixel()
{
    if (gInputFormat == FMT_BGR8) return 3u;
    if (gInputFormat == FMT_BGRA8) return 4u;
    return 1u;
}

uint LoadByteAtAddress(uint byteAddress)
{
    const uint aligned = byteAddress & ~3u;
    const uint shift = (byteAddress & 3u) * 8u;
    return (gInput.Load(aligned) >> shift) & 0xffu;
}

uint LoadByte(uint x, uint y, uint component)
{
    x = min(x, gWidth - 1u);
    y = min(y, gHeight - 1u);
    return LoadByteAtAddress(y * gInputRowPitchBytes + x * BytesPerPixel() + component);
}

uint BayerColorAt(uint x, uint y)
{
    const uint px = x & 1u;
    const uint py = y & 1u;

    if (gBayerPattern == 0u) {        // BGGR: B G / G R
        if (py == 0u) return (px == 0u) ? 2u : 1u;
        else          return (px == 0u) ? 1u : 0u;
    }
    else if (gBayerPattern == 1u) {   // GBRG: G B / R G
        if (py == 0u) return (px == 0u) ? 1u : 2u;
        else          return (px == 0u) ? 0u : 1u;
    }
    else if (gBayerPattern == 2u) {   // GRBG: G R / B G
        if (py == 0u) return (px == 0u) ? 1u : 0u;
        else          return (px == 0u) ? 2u : 1u;
    }
    else {                            // RGGB: R G / G B
        if (py == 0u) return (px == 0u) ? 0u : 1u;
        else          return (px == 0u) ? 1u : 2u;
    }
}

float SampleBayer(int x, int y)
{
    x = clamp(x, 0, int(gWidth) - 1);
    y = clamp(y, 0, int(gHeight) - 1);
    return float(LoadByte(uint(x), uint(y), 0u)) / 255.0f;
}

float4 ConvertBayer(uint xU, uint yU)
{
    const int x = int(xU);
    const int y = int(yU);
    const uint c = BayerColorAt(xU, yU);

    const float center = SampleBayer(x, y);
    float r;
    float g;
    float b;

    if (c == 0u) { // R
        r = center;
        g = 0.25f * (SampleBayer(x - 1, y) + SampleBayer(x + 1, y)
                   + SampleBayer(x, y - 1) + SampleBayer(x, y + 1));
        b = 0.25f * (SampleBayer(x - 1, y - 1) + SampleBayer(x + 1, y - 1)
                   + SampleBayer(x - 1, y + 1) + SampleBayer(x + 1, y + 1));
    }
    else if (c == 2u) { // B
        b = center;
        g = 0.25f * (SampleBayer(x - 1, y) + SampleBayer(x + 1, y)
                   + SampleBayer(x, y - 1) + SampleBayer(x, y + 1));
        r = 0.25f * (SampleBayer(x - 1, y - 1) + SampleBayer(x + 1, y - 1)
                   + SampleBayer(x - 1, y + 1) + SampleBayer(x + 1, y + 1));
    }
    else { // G
        g = center;
        const uint leftColor  = BayerColorAt(uint(max(x - 1, 0)), yU);
        const uint rightColor = BayerColorAt(uint(min(x + 1, int(gWidth) - 1)), yU);
        if (leftColor == 0u || rightColor == 0u) {
            r = 0.5f * (SampleBayer(x - 1, y) + SampleBayer(x + 1, y));
            b = 0.5f * (SampleBayer(x, y - 1) + SampleBayer(x, y + 1));
        }
        else {
            b = 0.5f * (SampleBayer(x - 1, y) + SampleBayer(x + 1, y));
            r = 0.5f * (SampleBayer(x, y - 1) + SampleBayer(x, y + 1));
        }
    }

    return float4(r, g, b, 1.0f);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gWidth || tid.y >= gHeight) return;

    float4 color = float4(0.0f, 0.0f, 0.0f, 1.0f);

    if (gInputFormat == FMT_MONO8) {
        const float v = float(LoadByte(tid.x, tid.y, 0u)) / 255.0f;
        color = float4(v, v, v, 1.0f);
    }
    else if (gInputFormat == FMT_BGR8) {
        const float b = float(LoadByte(tid.x, tid.y, 0u)) / 255.0f;
        const float g = float(LoadByte(tid.x, tid.y, 1u)) / 255.0f;
        const float r = float(LoadByte(tid.x, tid.y, 2u)) / 255.0f;
        color = float4(r, g, b, 1.0f);
    }
    else if (gInputFormat == FMT_BGRA8) {
        const float b = float(LoadByte(tid.x, tid.y, 0u)) / 255.0f;
        const float g = float(LoadByte(tid.x, tid.y, 1u)) / 255.0f;
        const float r = float(LoadByte(tid.x, tid.y, 2u)) / 255.0f;
        const float a = float(LoadByte(tid.x, tid.y, 3u)) / 255.0f;
        color = float4(r, g, b, a);
    }
    else {
        color = ConvertBayer(tid.x, tid.y);
    }

    gOutput[tid.xy] = color;
}
