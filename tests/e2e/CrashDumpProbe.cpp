// Developer-only process. Never linked into the product or its command line.
#include "core/CrashDump.h"

#include <Windows.h>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {
__declspec(noinline) unsigned OverflowStack(unsigned depth) {
    volatile unsigned char page[4096]{};
    page[depth % sizeof(page)] = static_cast<unsigned char>(depth);
    if (depth == 65535) { return page[0]; }
    return OverflowStack(depth + 1) + page[depth % sizeof(page)];
}
} // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argumentCount < 3 || argumentCount > 4) {
        std::cerr << "Usage: VisPickCrashProbe MODE OUTPUT_DIRECTORY [BLOCKED_PREFERRED_DIRECTORY]\n";
        return 2;
    }
    const std::wstring_view mode(arguments[1]);
    if (mode != L"--install-only" && mode != L"--crash" && mode != L"--terminate" && mode != L"--abort" &&
        mode != L"--worker-exception" && mode != L"--invalid-parameter" && mode != L"--purecall" &&
        mode != L"--concurrent" && mode != L"--stack-overflow") {
        return 2;
    }
    std::vector<std::filesystem::path> candidates;
    if (argumentCount == 4) { candidates.emplace_back(arguments[3]); }
    candidates.emplace_back(arguments[2]);
    vispick::CrashDump handler;
    const auto status = handler.Install(candidates);
    if (!status.installed) {
        std::cerr << "Install failed: " << status.error << '\n';
        return 3;
    }
    if (mode == L"--install-only") {
        if (GetModuleHandleW(L"dbghelp.dll") != nullptr) { return 4; }
        return 0;
    }
    if (mode == L"--terminate") { std::terminate(); }
    if (mode == L"--worker-exception") {
        std::thread worker([] { throw 17; });
        worker.join();
    }
    if (mode == L"--abort") { std::abort(); }
    if (mode == L"--invalid-parameter") { _invalid_parameter_noinfo_noreturn(); }
    if (mode == L"--purecall") { _purecall(); }
    if (mode == L"--stack-overflow") { return static_cast<int>(OverflowStack(0)); }
    if (mode == L"--concurrent") {
        const auto start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!start) { return 6; }
        const auto fault = [start] {
            WaitForSingleObject(start, INFINITE);
            RaiseException(0xE0421001UL, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        };
        std::thread first(fault), second(fault);
        SetEvent(start);
        first.join();
        second.join();
        CloseHandle(start);
    }
    // A named, noncontinuable SEH exception makes the dump independently
    // identifiable without dereferencing invalid memory in the test harness.
    RaiseException(0xE0421001UL, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 5;
}
