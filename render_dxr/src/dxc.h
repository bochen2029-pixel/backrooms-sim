#pragma once
//
// render_dxr/dxc.h — runtime DirectX Shader Compiler (DXC) wrapper (M9).
//
// DXR shaders need SM 6.3+ DXIL, which the FXC/D3DCompile path used by the raster
// renderer cannot produce. We load `dxcompiler.dll` (and, beside it, `dxil.dll`
// for validation/signing) at runtime from the Windows SDK and compile HLSL shader
// libraries to signed DXIL. No build-time shader artifacts (everything procedural).
//
#include <cstdint>
#include <string>
#include <vector>

namespace br::render_dxr {

class DxcCompiler {
public:
    DxcCompiler();
    ~DxcCompiler();

    // True if dxcompiler.dll loaded and DxcCreateInstance resolved.
    bool available() const { return available_; }
    const std::string& load_path() const { return load_path_; }

    // Compile `hlsl` as a DXR shader library to signed DXIL. `target` is the DXC
    // profile: lib_6_3 (default; recursive DXR) or lib_6_5 (inline RayQuery, M9
    // phase 3 path tracer). Returns false with `err` set on failure (incl.
    // compiler diagnostics).
    bool compile_library(const std::string& hlsl, std::vector<uint8_t>& dxil,
                         std::string& err, const char* target = "lib_6_3");

private:
    void* dll_ = nullptr;          // HMODULE for dxcompiler.dll
    void* create_proc_ = nullptr;  // DxcCreateInstanceProc
    bool available_ = false;
    std::string load_path_;
};

}  // namespace br::render_dxr
