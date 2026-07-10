Buffer<uint> InputBytes : register(t0);
RWTexture2D<float4> Output : register(u0);

cbuffer ConvertConstants : register(b0)
{
    uint width;
    uint height;
    uint inputRowPitchBytes;
    uint inputPixelFormat;
};

// Must match IC4Ext::CameraPixelFormat numeric values in Config.hpp.
static const uint FMT_BayerRG8 = 2u;
static const uint FMT_BayerGR8 = 3u;
static const uint FMT_BayerGB8 = 4u;
static const uint FMT_BayerBG8 = 5u;

uint ClampX(int x) { return (uint)clamp(x, 0, (int)width - 1); }
uint ClampY(int y) { return (uint)clamp(y, 0, (int)height - 1); }

uint LoadByte(uint x, uint y)
{
    return InputBytes[y * inputRowPitchBytes + x] & 0xffu;
}

float S(int x, int y)
{
    return LoadByte(ClampX(x), ClampY(y)) / 255.0f;
}

void BayerColor(uint fmt, uint x, uint y, out bool isR, out bool isG, out bool isB)
{
    bool evenX = (x & 1u) == 0u;
    bool evenY = (y & 1u) == 0u;
    isR = false;
    isG = false;
    isB = false;

    if (fmt == FMT_BayerRG8) {
        isR = evenX && evenY;
        isB = !evenX && !evenY;
    } else if (fmt == FMT_BayerGR8) {
        isR = !evenX && evenY;
        isB = evenX && !evenY;
    } else if (fmt == FMT_BayerGB8) {
        isR = evenX && !evenY;
        isB = !evenX && evenY;
    } else { // BayerBG8
        isR = !evenX && !evenY;
        isB = evenX && evenY;
    }
    isG = !isR && !isB;
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;

    int x = (int)tid.x;
    int y = (int)tid.y;
    bool isR, isG, isB;
    BayerColor(inputPixelFormat, tid.x, tid.y, isR, isG, isB);

    float c = S(x, y);
    float r, g, b;
    if (isR) {
        r = c;
        g = (S(x - 1, y) + S(x + 1, y) + S(x, y - 1) + S(x, y + 1)) * 0.25f;
        b = (S(x - 1, y - 1) + S(x + 1, y - 1) + S(x - 1, y + 1) + S(x + 1, y + 1)) * 0.25f;
    } else if (isB) {
        b = c;
        g = (S(x - 1, y) + S(x + 1, y) + S(x, y - 1) + S(x, y + 1)) * 0.25f;
        r = (S(x - 1, y - 1) + S(x + 1, y - 1) + S(x - 1, y + 1) + S(x + 1, y + 1)) * 0.25f;
    } else {
        g = c;
        // Green pixels alternate between red-neighbor horizontally and vertically.
        bool leftRightAreRed;
        bool dummyG, dummyB;
        BayerColor(inputPixelFormat, ClampX(x - 1), tid.y, leftRightAreRed, dummyG, dummyB);
        if (leftRightAreRed) {
            r = (S(x - 1, y) + S(x + 1, y)) * 0.5f;
            b = (S(x, y - 1) + S(x, y + 1)) * 0.5f;
        } else {
            r = (S(x, y - 1) + S(x, y + 1)) * 0.5f;
            b = (S(x - 1, y) + S(x + 1, y)) * 0.5f;
        }
    }

    Output[tid.xy] = float4(r, g, b, 1.0f);
}
