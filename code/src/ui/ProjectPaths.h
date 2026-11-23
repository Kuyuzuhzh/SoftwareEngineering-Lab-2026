#pragma once

#include <filesystem>
#include <optional>

namespace ledger::ui {

std::filesystem::path ProjectRoot();
bool SetProjectWorkingDirectory();
std::optional<std::filesystem::path> ProjectRelativeSnapshotPath(
    const std::filesystem::path& selected_path);

} // namespace ledger::ui
