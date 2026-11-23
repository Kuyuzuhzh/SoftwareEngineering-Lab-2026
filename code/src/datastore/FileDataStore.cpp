#include "FileDataStore.h"

#include "JsonSnapshot.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <system_error>

namespace ledger {
namespace {

bool IsSafeSnapshotPath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory() || !path.has_parent_path() || !path.has_filename()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

Status PathError() {
    return Status{
        ErrorCode::InvalidArgument,
        "快照必须保存在项目子目录中，并使用相对路径。"};
}

} // namespace

Result<DatabaseSnapshot> FileDataStore::Load(
    const std::filesystem::path& source) {
    if (!IsSafeSnapshotPath(source)) {
        return Result<DatabaseSnapshot>{PathError(), std::nullopt};
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(source, error);
    if (error) {
        return Result<DatabaseSnapshot>{
            Status{ErrorCode::IoError, "无法检查快照路径。"},
            std::nullopt};
    }
    if (!exists) {
        return Result<DatabaseSnapshot>{
            Status{ErrorCode::NotFound, "快照文件不存在。"},
            std::nullopt};
    }

    std::ifstream input{source, std::ios::binary};
    if (!input) {
        return Result<DatabaseSnapshot>{
            Status{ErrorCode::IoError, "无法打开快照文件。"},
            std::nullopt};
    }
    const std::string json{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        return Result<DatabaseSnapshot>{
            Status{ErrorCode::IoError, "无法读取快照文件。"},
            std::nullopt};
    }
    return DeserializeSnapshot(json);
}

Status FileDataStore::Save(
    const std::filesystem::path& destination,
    const DatabaseSnapshot& snapshot) {
    if (!IsSafeSnapshotPath(destination)) {
        return PathError();
    }

    auto serialized = SerializeSnapshot(snapshot);
    if (serialized.status.code != ErrorCode::Ok) {
        return serialized.status;
    }

    const auto parent = destination.parent_path();
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        return Status{ErrorCode::IoError, "无法创建快照目录。"};
    }

    auto temporary = destination;
    temporary += L".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            return Status{ErrorCode::IoError, "无法打开临时快照文件。"};
        }
        output.write(
            serialized.value->data(),
            static_cast<std::streamsize>(serialized.value->size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return Status{ErrorCode::IoError, "无法写入临时快照文件。"};
        }
    }

    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::filesystem::remove(temporary, error);
        return Status{ErrorCode::IoError, "无法以原子方式替换快照文件。"};
    }
    return Status{};
}

} // namespace ledger
