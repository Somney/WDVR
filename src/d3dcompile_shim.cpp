// Loads d3dcompiler_47.dll only when ImGui first needs it, instead of making
// dxgi.dll depend on it at load time (a missing/blocked dependency would stop
// the game from starting at all).
#include <windows.h>
#include <d3dcompiler.h>
#include "log.h"

extern "C" HRESULT WINAPI D3DCompile(LPCVOID src, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT f1, UINT f2,
                                     ID3DBlob** code, ID3DBlob** errors) {
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                UINT, UINT, ID3DBlob**, ID3DBlob**);
    static Fn real = nullptr;
    if (!real) {
        const char* names[] = { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll" };
        for (const char* n : names) {
            if (HMODULE h = LoadLibraryA(n)) { real = (Fn)GetProcAddress(h, "D3DCompile"); if (real) { wdvr::log("Using %s", n); break; } }
        }
        if (!real) { wdvr::log("No d3dcompiler DLL found - menu can't draw"); return E_FAIL; }
    }
    return real(src, size, name, defines, include, entry, target, f1, f2, code, errors);
}
