cmake_minimum_required(VERSION 3.29)
project(VisPick VERSION 0.5.6 LANGUAGES CXX RC)
set(VISPICK_PRODUCT_VERSION "${PROJECT_VERSION}")

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "VisPick requires Windows and the MSVC v143 toolset.")
endif()
if(MSVC_VERSION LESS 1942 OR MSVC_VERSION GREATER_EQUAL 1943)
    message(FATAL_ERROR "The Windows 7 import baseline is validated with MSVC 14.42.x (VS 2022 17.12). Select it with build.ps1.")
endif()
if(DEFINED VISPICK_EXPECTED_BITS)
    math(EXPR VISPICK_ACTUAL_BITS "${CMAKE_SIZEOF_VOID_P} * 8")
    if(NOT VISPICK_ACTUAL_BITS EQUAL VISPICK_EXPECTED_BITS)
        message(FATAL_ERROR "This preset requires ${VISPICK_EXPECTED_BITS}-bit MSVC. Use build.ps1 to select the correct environment.")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
configure_file(res/Version.h.in generated/VisPickVersion.h @ONLY)
configure_file(res/VisPick.manifest.in generated/VisPick.manifest @ONLY)
option(VISPICK_ENABLE_ASAN "Instrument development builds with AddressSanitizer" OFF)
option(VISPICK_ANALYZE "Run MSVC static analysis on project code" OFF)
option(VISPICK_BUILD_TESTS "Build development-only tests" ON)
option(VISPICK_BUILD_SPIKES "Build development-only prototype executables" ON)

add_library(vispick_options INTERFACE)
target_compile_definitions(vispick_options INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0601 _WIN32_WINNT=0x0601)
target_compile_options(vispick_options INTERFACE
    /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus /guard:cf
    "$<$<CONFIG:Release>:/O2;/GL;/Gy>")
target_link_options(vispick_options INTERFACE
    /INCREMENTAL:NO /guard:cf /DYNAMICBASE /NXCOMPAT
    "$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
target_include_directories(vispick_options INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_options(vispick_options INTERFACE /HIGHENTROPYVA)
else()
    target_link_options(vispick_options INTERFACE /LARGEADDRESSAWARE /SAFESEH)
endif()
if(VISPICK_ENABLE_ASAN)
    # /RTC is incompatible with AddressSanitizer. Keep checks in x86-Debug instead.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
    target_compile_options(vispick_options INTERFACE /fsanitize=address)
endif()

add_library(vispick_core STATIC
    src/core/AppCommand.cpp src/core/Ini.cpp src/core/Config.cpp
    src/core/Hotkey.cpp src/core/Paths.cpp src/core/Log.cpp src/core/CrashDump.cpp
    src/platform/ConfigWatcher.cpp src/platform/HotkeyRegistration.cpp
    src/platform/SingleInstance.cpp src/platform/Tray.cpp src/platform/Autostart.cpp
    src/imaging/ImageCodec.cpp src/imaging/Clipboard.cpp
    src/output/SaveDialog.cpp src/output/ImageSaver.cpp src/output/CopyToast.cpp
    src/overlay/Overlay.cpp src/overlay/Selection.cpp
    src/editor/Annotation.cpp src/editor/Renderer.cpp src/editor/TextInput.cpp
    src/pin/PinManager.cpp src/pin/PinGeometry.cpp
    src/recording/FrameChange.cpp src/recording/Recorder.cpp
    src/recording/RecordingFiles.cpp src/recording/GifPreview.cpp
    src/imaging/Bitmap.cpp src/platform/Gdi.cpp src/platform/Compat.cpp
    src/platform/Window.cpp src/platform/Console.cpp
    src/capture/ScreenGrab.cpp
    src/gif/Lzw.cpp src/gif/Quantizer.cpp src/gif/GifWriter.cpp
    src/gif/GifReader.cpp src/gif/Timing.cpp)
target_include_directories(vispick_core PUBLIC src)
target_link_libraries(vispick_core PUBLIC vispick_options
    user32 gdi32 shell32 ole32 windowscodecs dwmapi winmm advapi32 wtsapi32 psapi gdiplus imm32)

if(VISPICK_BUILD_SPIKES OR VISPICK_BUILD_TESTS)
add_library(vispick_spikes STATIC
    src/core/CommandLine.cpp src/core/Report.cpp
    src/spikes/CaptureSpike.cpp src/spikes/OverlaySpike.cpp
    src/spikes/PinSpike.cpp src/spikes/GifSpike.cpp
    src/spikes/PatternProcess.cpp src/spikes/GifPipelineSpike.cpp
    src/spikes/QuantizerBenchmark.cpp)
target_link_libraries(vispick_spikes PUBLIC vispick_core)
endif()
if(VISPICK_ANALYZE)
    target_compile_options(vispick_core PRIVATE /analyze)
    if(TARGET vispick_spikes)
        target_compile_options(vispick_spikes PRIVATE /analyze)
    endif()
endif()

function(vispick_manifest target)
    target_link_options(${target} PRIVATE
        "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/generated/VisPick.manifest")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/VisPick.manifest")
endfunction()

add_executable(VisPick WIN32 src/main.cpp src/App.cpp src/platform/SystemDelayLoad.cpp res/VisPick.rc)
target_link_libraries(VisPick PRIVATE vispick_core delayimp)
target_link_options(VisPick PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DELAYLOAD:dwmapi.dll /DELAYLOAD:gdiplus.dll
    /DELAYLOAD:winmm.dll /DELAYLOAD:wtsapi32.dll)
target_link_options(VisPick PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL;/PDBALTPATH:VisPick.pdb;/OPT:REF;/OPT:ICF>")
vispick_manifest(VisPick)
if(VISPICK_ANALYZE)
    target_compile_options(VisPick PRIVATE /analyze)
endif()

if(VISPICK_BUILD_SPIKES OR VISPICK_BUILD_TESTS)
    add_executable(VisPickSpikes src/spikes/main.cpp)
    target_link_libraries(VisPickSpikes PRIVATE vispick_spikes)
    target_link_options(VisPickSpikes PRIVATE /SUBSYSTEM:CONSOLE,6.01)
    vispick_manifest(VisPickSpikes)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickSpikes PRIVATE /analyze)
    endif()
endif()

if(VISPICK_BUILD_TESTS)
    enable_testing()
    add_executable(VisPickUnitTests
        res/TestResources.rc tests/unit/AppCommandTests.cpp
        tests/unit/IniTests.cpp tests/unit/ConfigTests.cpp
        tests/unit/HotkeyTests.cpp tests/unit/PathsTests.cpp
        tests/unit/SingleInstanceTests.cpp tests/unit/AutostartTests.cpp
        tests/unit/LogTests.cpp tests/unit/TrayTests.cpp tests/unit/ImageCodecTests.cpp
        tests/unit/SelectionTests.cpp tests/unit/AnnotationTests.cpp
        tests/unit/PinTests.cpp
        tests/unit/RecordingTests.cpp
        tests/unit/TestMain.cpp tests/unit/CommandLineTests.cpp tests/unit/ReportTests.cpp
        tests/unit/BitmapTests.cpp tests/unit/GifTests.cpp
        tests/unit/ConsoleTests.cpp tests/unit/WindowTests.cpp
        tests/unit/CaptureTests.cpp tests/unit/EnvironmentTests.cpp)
    target_include_directories(VisPickUnitTests SYSTEM PRIVATE tests/third_party)
    target_link_libraries(VisPickUnitTests PRIVATE vispick_spikes)
    add_test(NAME unit COMMAND VisPickUnitTests --no-colors)
    set_tests_properties(unit PROPERTIES TIMEOUT 120)
    add_executable(VisPickTest tests/e2e/main.cpp tests/e2e/DesktopTests.cpp)
    target_link_libraries(VisPickTest PRIVATE vispick_spikes)
    add_dependencies(VisPickTest VisPickSpikes)
    vispick_manifest(VisPickTest)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickTest PRIVATE /analyze)
    endif()
    add_executable(VisPickConsoleTests tests/e2e/ConsoleIntegration.cpp)
    target_link_libraries(VisPickConsoleTests PRIVATE vispick_options user32)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickConsoleTests PRIVATE /analyze)
    endif()
    add_executable(VisPickCrashProbe tests/e2e/CrashDumpProbe.cpp)
    target_link_libraries(VisPickCrashProbe PRIVATE vispick_core)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickCrashProbe PRIVATE /analyze)
    endif()
    add_executable(VisPickAppIntegration tests/e2e/AppIntegration.cpp)
    target_link_libraries(VisPickAppIntegration PRIVATE vispick_core)
    vispick_manifest(VisPickAppIntegration)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickAppIntegration PRIVATE /analyze)
    endif()
    add_executable(VisPickOutputTests tests/e2e/OutputIntegration.cpp)
    target_link_libraries(VisPickOutputTests PRIVATE vispick_core)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickOutputTests PRIVATE /analyze)
    endif()
    add_executable(VisPickOverlayTests tests/e2e/OverlayIntegration.cpp)
    target_link_libraries(VisPickOverlayTests PRIVATE vispick_core)
    vispick_manifest(VisPickOverlayTests)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickOverlayTests PRIVATE /analyze)
    endif()
    add_executable(VisPickEditorTests tests/e2e/EditorIntegration.cpp)
    target_link_libraries(VisPickEditorTests PRIVATE vispick_core)
    vispick_manifest(VisPickEditorTests)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickEditorTests PRIVATE /analyze)
    endif()
    add_executable(VisPickPinTests tests/e2e/PinIntegration.cpp)
    target_link_libraries(VisPickPinTests PRIVATE vispick_core)
    vispick_manifest(VisPickPinTests)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickPinTests PRIVATE /analyze)
    endif()
    add_executable(VisPickRecordingIntegration tests/e2e/RecordingIntegration.cpp)
    target_link_libraries(VisPickRecordingIntegration PRIVATE vispick_core)
    vispick_manifest(VisPickRecordingIntegration)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickRecordingIntegration PRIVATE /analyze)
    endif()
    add_executable(VisPickSoak tests/e2e/Soak.cpp)
    target_link_libraries(VisPickSoak PRIVATE vispick_core)
    vispick_manifest(VisPickSoak)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickSoak PRIVATE /analyze)
    endif()
    add_executable(VisPickAppLifecycleIntegration tests/e2e/AppLifecycleIntegration.cpp)
    target_link_libraries(VisPickAppLifecycleIntegration PRIVATE vispick_core)
    vispick_manifest(VisPickAppLifecycleIntegration)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickAppLifecycleIntegration PRIVATE /analyze)
    endif()
    add_executable(VisPickGifQuality tests/e2e/GifQuality.cpp)
    target_link_libraries(VisPickGifQuality PRIVATE vispick_core)
    if(VISPICK_ANALYZE)
        target_compile_options(VisPickGifQuality PRIVATE /analyze)
    endif()
endif()

