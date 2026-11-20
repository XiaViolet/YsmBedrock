#include "NativeModelPicker.h"
#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <stdexcept>

namespace ysm {
namespace {
// The dialog pumps its own STA. Close it on that same thread if the
// world or the plugin goes away while a file is being chosen.
struct Picker {
    IFileOpenDialog* dialog;
    std::stop_token stop;
};
thread_local Picker* activePicker = nullptr;
void CALLBACK pollClose(HWND, UINT, UINT_PTR, DWORD) {
    if (activePicker && activePicker->stop.stop_requested())
        activePicker->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}
}
std::optional<std::filesystem::path> pickModelSource(bool folder, void* owner, std::stop_token stop) {
    if (stop.stop_requested()) return {};
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        throw std::runtime_error("无法打开 Windows 文件选择器");
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("无法创建 Windows 文件选择器");
    FILEOPENDIALOGOPTIONS flags{};
    if (FAILED(dialog->GetOptions(&flags)) || FAILED(dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR
            | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST)))) throw std::runtime_error("无法配置文件选择器");
    COMDLG_FILTERSPEC formats[] = {{L"YSM 模型包 (*.zip, *.ysm)", L"*.zip;*.ysm"}};
    if (!folder) dialog->SetFileTypes(1, formats);
    dialog->SetTitle(folder ? L"选择 YSM 模型源文件夹" : L"选择 YSM 模型包");
    Picker picker{dialog.Get(), stop};
    auto timer = SetTimer(nullptr, 0, 200, pollClose);
    if (!timer) throw std::runtime_error("无法启动文件选择器");
    struct End { UINT_PTR timer; ~End() { activePicker = nullptr; KillTimer(nullptr,timer); } } end{timer};
    activePicker = &picker;
    auto result = dialog->Show(static_cast<HWND>(owner));
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || stop.stop_requested()) return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR text = nullptr;
    if (FAILED(result) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &text)))
        throw std::runtime_error("无法读取选中路径");
    struct Free { PWSTR text; ~Free() { CoTaskMemFree(text); } } free{text};
    return std::filesystem::path(text);
}
}
