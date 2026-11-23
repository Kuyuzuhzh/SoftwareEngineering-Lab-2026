#include "Win32FileDialog.h"

#include <commdlg.h>

#include <array>

namespace ledger::ui {
namespace {

constexpr wchar_t kFilter[] =
    L"账本 JSON 文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0\0";

OPENFILENAMEW MakeDialog(HWND owner, wchar_t* buffer, DWORD buffer_size) {
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = kFilter;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = buffer_size;
    dialog.lpstrDefExt = L"json";
    return dialog;
}

} // namespace

std::optional<std::filesystem::path> Win32FileDialog::Open(HWND owner) const {
    std::array<wchar_t, MAX_PATH> buffer{};
    auto dialog = MakeDialog(owner, buffer.data(), static_cast<DWORD>(buffer.size()));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog) == FALSE) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer.data());
}

std::optional<std::filesystem::path> Win32FileDialog::SaveAs(HWND owner) const {
    std::array<wchar_t, MAX_PATH> buffer{};
    auto dialog = MakeDialog(owner, buffer.data(), static_cast<DWORD>(buffer.size()));
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&dialog) == FALSE) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer.data());
}

} // namespace ledger::ui
