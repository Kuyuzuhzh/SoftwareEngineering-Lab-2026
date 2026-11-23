#include "ProjectPaths.h"

#include <windows.h>

#include <string>
#include <system_error>

namespace ledger::ui {
namespace {

bool IsProjectRoot(const std::filesystem::path& path) {
    std::error_code error;
    const bool has_build_file = std::filesystem::exists(path / "CMakeLists.txt", error);
    if (error || !has_build_file) {
        return false;
    }
    const bool has_frame_file = std::filesystem::exists(path / "Frame.md", error);
    return !error && has_frame_file;
}

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::filesystem::current_path();
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

bool HasParentTraversal(const std::filesystem::path& path) {
    for (const auto& component : path) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

} // namespace

std::filesystem::path ProjectRoot() {
    auto candidate = ExecutableDirectory();
    for (int depth = 0; depth < 8; ++depth) {
        if (IsProjectRoot(candidate)) {
            return candidate;
        }
        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }

    const auto current = std::filesystem::current_path();
    return IsProjectRoot(current) ? current : ExecutableDirectory();
}

bool SetProjectWorkingDirectory() {
    return SetCurrentDirectoryW(ProjectRoot().c_str()) != 0;
}

std::optional<std::filesystem::path> ProjectRelativeSnapshotPath(
    const std::filesystem::path& selected_path) {
    if (selected_path.empty()) {
        return std::nullopt;
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(ProjectRoot(), error);
    if (error) {
        return std::nullopt;
    }
    auto selected = std::filesystem::absolute(selected_path, error);
    if (error) {
        return std::nullopt;
    }
    selected = selected.lexically_normal();

    const auto relative = selected.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory() ||
        !relative.has_parent_path() || !relative.has_filename() ||
        HasParentTraversal(relative)) {
        return std::nullopt;
    }
    return relative;
}

} // namespace ledger::ui
