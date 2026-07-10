#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef _WIN32
int main()
{
    std::cout << "test_d3d12_shader_compile skipped on non-Windows\n";
    return 77;
}
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace {

std::wstring Widen(const std::filesystem::path& path)
{
    return path.wstring();
}

void CompileShaderFile(const std::filesystem::path& path)
{
    assert(std::filesystem::exists(path));

    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const auto wpath = Widen(path);
    const HRESULT hr = D3DCompileFromFile(wpath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                          "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                          &bytecode, &errors);
    if (FAILED(hr)) {
        std::cerr << "D3DCompileFromFile failed for " << path.string() << " hr=0x" << std::hex << hr << std::dec << "\n";
        if (errors) {
            std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), static_cast<std::streamsize>(errors->GetBufferSize()));
            std::cerr << "\n";
        }
    }
    assert(SUCCEEDED(hr));
    assert(bytecode != nullptr);
    assert(bytecode->GetBufferSize() > 0);
}

} // namespace

int main()
{
    const std::filesystem::path sourceDir = std::filesystem::path(IC4EXT_SOURCE_DIR);
    CompileShaderFile(sourceDir / "shaders" / "d3d12" / "IC4Ext_D3D12_Convert_To_RGBA8.hlsl");
    CompileShaderFile(sourceDir / "shaders" / "d3d12" / "IC4Ext_D3D12_Convert_To_R8.hlsl");
    std::cout << "test_d3d12_shader_compile passed\n";
    return 0;
}
#endif
