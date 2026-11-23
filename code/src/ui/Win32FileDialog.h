#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>

namespace ledger::ui {

class Win32FileDialog {
public:
    std::optional<std::filesystem::path> Open(HWND owner) const;
    std::optional<std::filesystem::path> SaveAs(HWND owner) const;
};

} // namespace ledger::ui
