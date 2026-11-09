#include <Windows.h>
#include <delayimp.h>
#include <strsafe.h>
#include <cstring>

namespace {
FARPROC WINAPI LoadSystemDependency(unsigned notification, PDelayLoadInfo information) noexcept {
    if (notification != dliNotePreLoadLibrary) { return nullptr; }
    const wchar_t* filename = nullptr;
    if (_stricmp(information->szDll, "dwmapi.dll") == 0) { filename = L"\\dwmapi.dll"; }
    else if (_stricmp(information->szDll, "gdiplus.dll") == 0) { filename = L"\\gdiplus.dll"; }
    else if (_stricmp(information->szDll, "winmm.dll") == 0) { filename = L"\\winmm.dll"; }
    else if (_stricmp(information->szDll, "wtsapi32.dll") == 0) { filename = L"\\wtsapi32.dll"; }
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, ARRAYSIZE(path));
    if (filename && length && length < ARRAYSIZE(path) && SUCCEEDED(StringCchCatW(path, ARRAYSIZE(path), filename))) {
        // The search flag is available on updated Win7. The absolute-path
        // fallback also works on Win7 before KB2533623.
        HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module && GetLastError() == ERROR_INVALID_PARAMETER) {
            module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (module) {
            FARPROC result = nullptr;
            static_assert(sizeof(result) == sizeof(module));
            std::memcpy(&result, &module, sizeof(result));
            return result;
        }
    }
    // Returning null here would let the default helper search beside the exe.
    RaiseException(VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND), EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return nullptr;
}
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = LoadSystemDependency;
