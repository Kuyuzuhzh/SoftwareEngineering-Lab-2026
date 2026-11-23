#include "ui/ProjectPaths.h"

#include <filesystem>

namespace {

struct WorkingDirectoryGuard {
    WorkingDirectoryGuard()
        : original(std::filesystem::current_path()) {}
    ~WorkingDirectoryGuard() {
        std::error_code error;
        std::filesystem::current_path(original, error);
    }
    std::filesystem::path original;
};

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return __LINE__; \
        } \
    } while (false)

int TestProjectRelativePaths() {
    WorkingDirectoryGuard guard;
    const auto root = ledger::ui::ProjectRoot();
    CHECK(std::filesystem::exists(root / "CMakeLists.txt"));
    CHECK(std::filesystem::exists(root / "Frame.md"));

    const auto data_file = ledger::ui::ProjectRelativeSnapshotPath(
        root / "data" / "ledger.json");
    CHECK(data_file.has_value());
    CHECK(*data_file == std::filesystem::path("data/ledger.json"));

    const auto normalized = ledger::ui::ProjectRelativeSnapshotPath(
        root / "data" / ".." / "data" / "normalized.json");
    CHECK(normalized.has_value());
    CHECK(*normalized == std::filesystem::path("data/normalized.json"));

    CHECK(!ledger::ui::ProjectRelativeSnapshotPath(root / "root-file.json"));
    CHECK(!ledger::ui::ProjectRelativeSnapshotPath(
        root.parent_path() / "outside.json"));
    CHECK(!ledger::ui::ProjectRelativeSnapshotPath({}));

    std::filesystem::current_path(root / "build");
    CHECK(ledger::ui::SetProjectWorkingDirectory());
    CHECK(std::filesystem::current_path() == root);
    return 0;
}

} // namespace

int main() {
    return TestProjectRelativePaths();
}
