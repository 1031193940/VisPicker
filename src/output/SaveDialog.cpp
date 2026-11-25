#include "output/SaveDialog.h"
#include "core/Paths.h"
#include "platform/FileCommit.h"
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <utility>

namespace vispick {
namespace {
void Check(HRESULT result, const char* description) {
    if (FAILED(result)) { throw std::runtime_error(description); }
}
struct Apartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Apartment() { Check(status, "Save dialog requires the UI COM apartment"); }
    ~Apartment() { CoUninitialize(); }
};
} // namespace

SaveDestination ResolveSaveDestination(std::filesystem::path selectedPath, const UINT fileTypeIndex, const bool gif) {
    if (selectedPath.empty() || selectedPath.filename().empty()) {
        throw std::invalid_argument("Save destination must include a file name");
    }
    auto extension = selectedPath.extension().native();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    SaveDestination destination{std::move(selectedPath), SaveFormat::Png};
    if (gif) {
        if (extension != L".gif") { destination.path += L".gif"; }
        return destination;
    }
    if (extension == L".png") {
        destination.format = SaveFormat::Png;
    } else if (extension == L".jpg" || extension == L".jpeg") {
        destination.format = SaveFormat::Jpg;
    } else if (extension == L".bmp") {
        destination.format = SaveFormat::Bmp;
    } else if (fileTypeIndex == 2U) {
        destination.path += L".jpg";
        destination.format = SaveFormat::Jpg;
    } else if (fileTypeIndex == 3U) {
        destination.path += L".bmp";
        destination.format = SaveFormat::Bmp;
    } else {
        destination.path += L".png";
        destination.format = SaveFormat::Png;
    }
    return destination;
}

std::optional<SaveDestination> ChooseSavePath(HWND owner, const Config::Save& settings, int width, int height,
                                              std::wstring_view fixedExtension, std::filesystem::path* lastFolder,
                                              const bool english) {
    Apartment apartment;
    Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
    Check(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)),
          "Create save dialog failed");
    DWORD options = 0;
    Check(dialog->GetOptions(&options), "Read save dialog options failed");
    Check(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR),
          "Set save dialog options failed");
    const COMDLG_FILTERSPEC imageFilters[] = {
        {L"PNG (*.png)", L"*.png"}, {L"JPEG (*.jpg;*.jpeg)", L"*.jpg;*.jpeg"}, {L"BMP (*.bmp)", L"*.bmp"}};
    const COMDLG_FILTERSPEC gifFilter[] = {{L"GIF (*.gif)", L"*.gif"}};
    const bool gif = fixedExtension == L"gif";
    if (!fixedExtension.empty() && !gif) { throw std::invalid_argument("Unsupported fixed save extension"); }
    Check(dialog->SetFileTypes(gif ? 1U : 3U, gif ? gifFilter : imageFilters), "Set save file filters failed");
    Check(dialog->SetFileTypeIndex(gif || settings.format == SaveFormat::Png ? 1U
                                   : settings.format == SaveFormat::Jpg      ? 2U
                                                                             : 3U),
          "Set default save format failed");
    Check(dialog->SetDefaultExtension(gif                                  ? L"gif"
                                      : settings.format == SaveFormat::Png ? L"png"
                                      : settings.format == SaveFormat::Jpg ? L"jpg"
                                                                           : L"bmp"),
          "Set save file extension failed");
    auto folder = lastFolder && !lastFolder->empty()
                      ? *lastFolder
                      : ExpandSaveFolder(settings.folder, DefaultFolderTemplateContext());
    Microsoft::WRL::ComPtr<IShellItem> directory;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&directory)))) {
        Check(dialog->SetFolder(directory.Get()), "Set save folder failed");
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const auto fileName = ExpandFileName(settings.fileName, time, width, height);
    Check(dialog->SetFileName(fileName.c_str()), "Set suggested file name failed");
    const auto shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { return std::nullopt; }
    Check(shown, "Show save dialog failed");
    Microsoft::WRL::ComPtr<IShellItem> selected;
    Check(dialog->GetResult(&selected), "Get save destination failed");
    PWSTR text = nullptr;
    Check(selected->GetDisplayName(SIGDN_FILESYSPATH, &text), "Read save destination failed");
    std::filesystem::path selectedPath;
    try {
        selectedPath = text;
    } catch (...) {
        CoTaskMemFree(text);
        throw;
    }
    CoTaskMemFree(text);
    UINT fileTypeIndex = 1;
    (void)dialog->GetFileTypeIndex(&fileTypeIndex);
    const auto originalPath = selectedPath;
    auto destination = ResolveSaveDestination(std::move(selectedPath), fileTypeIndex, gif);
    if (destination.path != originalPath) {
        const auto extended = ExtendedFilePath(destination.path);
        if (GetFileAttributesW(extended.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const auto prompt = (english ? L"File already exists:\n" : L"文件已存在：\n") + destination.path.native() +
                                (english ? L"\n\nReplace it?" : L"\n\n是否覆盖？");
            if (MessageBoxW(owner, prompt.c_str(), L"VisPick", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return std::nullopt;
            }
        }
    }
    if (lastFolder) { *lastFolder = destination.path.parent_path(); }
    return destination;
}

void ShowSavedFile(HWND owner, const std::filesystem::path& file) {
    const auto folder = file.parent_path();
    const auto opened =
        reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (opened <= 32) { throw std::runtime_error("Open saved image folder failed"); }
}
} // namespace vispick
