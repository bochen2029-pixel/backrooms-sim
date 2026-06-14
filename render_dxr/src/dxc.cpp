#include "dxc.h"

#include <windows.h>
#include <unknwn.h>
#include <combaseapi.h>
#include <dxcapi.h>

namespace br::render_dxr {

namespace {

// Locate dxcompiler.dll in the Windows SDK if it isn't already on the loader's
// search path. dxil.dll (the validator/signer) sits beside it and is found by
// dxcompiler from its own directory, so loading by full path keeps signing working.
std::wstring find_sdk_dxcompiler() {
    const wchar_t* roots[] = {
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\",
        L"C:\\Program Files\\Windows Kits\\10\\bin\\",
    };
    for (const wchar_t* root : roots) {
        const std::wstring pattern = std::wstring(root) + L"10.*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::wstring best;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::wstring cand = std::wstring(root) + fd.cFileName + L"\\x64\\dxcompiler.dll";
            if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) best = cand;  // highest version wins
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!best.empty()) return best;
    }
    return L"";
}

std::string narrow_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

DxcCompiler::DxcCompiler() {
    HMODULE mod = LoadLibraryW(L"dxcompiler.dll");
    std::string path = "dxcompiler.dll";
    if (!mod) {
        const std::wstring sdk = find_sdk_dxcompiler();
        if (!sdk.empty()) {
            mod = LoadLibraryW(sdk.c_str());
            path = narrow_utf8(sdk);
        }
    }
    if (!mod) return;
    FARPROC proc = GetProcAddress(mod, "DxcCreateInstance");
    if (!proc) { FreeLibrary(mod); return; }
    dll_ = mod;
    create_proc_ = reinterpret_cast<void*>(proc);
    available_ = true;
    load_path_ = path;
}

DxcCompiler::~DxcCompiler() {
    if (dll_) FreeLibrary(static_cast<HMODULE>(dll_));
}

bool DxcCompiler::compile_library(const std::string& hlsl, std::vector<uint8_t>& dxil,
                                  std::string& err, const char* target) {
    if (!available_) { err = "dxcompiler.dll not available"; return false; }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(create_proc_);

    // Widen the target profile (e.g. "lib_6_3", "lib_6_5") for the DXC arg list.
    const char* prof = (target && *target) ? target : "lib_6_3";
    std::wstring targetW(prof, prof + std::char_traits<char>::length(prof));

    IDxcCompiler3* compiler = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        err = "DxcCreateInstance(IDxcCompiler3) failed";
        return false;
    }

    DxcBuffer src{};
    src.Ptr = hlsl.data();
    src.Size = hlsl.size();
    src.Encoding = DXC_CP_UTF8;

    const wchar_t* args[] = { L"-T", targetW.c_str(), L"-Qstrip_reflect" };
    const UINT32 argc = static_cast<UINT32>(sizeof(args) / sizeof(args[0]));

    IDxcResult* result = nullptr;
    const HRESULT hr = compiler->Compile(&src, args, argc, nullptr, IID_PPV_ARGS(&result));
    bool ok = false;
    if (SUCCEEDED(hr) && result) {
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        IDxcBlobUtf8* errors = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength() > 0 && FAILED(status)) {
            err.assign(errors->GetStringPointer(), errors->GetStringLength());
        }
        if (errors) errors->Release();
        if (SUCCEEDED(status)) {
            IDxcBlob* obj = nullptr;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) &&
                obj && obj->GetBufferSize() > 0) {
                const uint8_t* p = static_cast<const uint8_t*>(obj->GetBufferPointer());
                dxil.assign(p, p + obj->GetBufferSize());
                ok = true;
            }
            if (obj) obj->Release();
            if (!ok && err.empty()) err = "DXC produced an empty object";
        }
    } else {
        err = "IDxcCompiler3::Compile call failed";
    }
    if (result) result->Release();
    compiler->Release();
    return ok;
}

}  // namespace br::render_dxr
