# 公司多人共享记账系统工程骨架

目标平台：Windows 11  
实现语言：C++17  
构建工具：CMake + Visual Studio 2022  
当前阶段：提供可编译的功能切片、控制台与基础 Windows 文件 UI、JSON 文件持久化和自动化测试；其余业务接口保留 stub。

## 1. 目录树

```text
code/
|-- CMakeLists.txt
|-- Frame.md
|-- include/
|   `-- ledger/
|       |-- common/
|       |   `-- Types.h
|       |-- datastore/
|       |   `-- IDataStore.h
|       `-- service/
|           `-- ILedgerService.h
|-- src/
|   |-- datastore/
|   |   |-- FileDataStore.cpp
|   |   |-- FileDataStore.h
|   |   |-- JsonSnapshot.cpp
|   |   `-- JsonSnapshot.h
|   |-- service/
|   |   |-- LedgerService.cpp
|   |   `-- LedgerService.h
|   `-- ui/
|       |-- ConsoleController.cpp
|       |-- ConsoleController.h
|       |-- console_main.cpp
|       |-- main.cpp
|       |-- OperationsDialog.cpp
|       |-- OperationsDialog.h
|       |-- ProjectPaths.cpp
|       |-- ProjectPaths.h
|       |-- Win32FileDialog.cpp
|       `-- Win32FileDialog.h
`-- tests/
    |-- service_entry_editing_tests.cpp
    |-- file_persistence_tests.cpp
    |-- empty_test.cpp
    |-- service_membership_tests.cpp
    |-- service_review_tests.cpp
    |-- service_own_entries_tests.cpp
    |-- service_query_tests.cpp
    |-- console_ui_tests.cpp
    `-- ui_paths_tests.cpp
```

## 2. 模块依赖表

| 模块 | 允许依赖 | 禁止依赖 |
|---|---|---|
| `common` | C++ 标准库 | UI、Service、DataStore、Windows API |
| `datastore` | `common` | Service、UI、业务权限规则 |
| `service` | `common`、DataStore 接口及其文件实现 | UI、stdout 输出 |
| `ui` | Service 公共接口、`common`、Win32/Common Dialog API | 直接包含或调用 DataStore |
| `tests` | 各层公共接口 | 依赖具体业务流程已实现的假设 |

依赖方向固定为 `UI -> Service -> DataStore`。工厂函数 `CreateFileLedgerService()` 在 Service 内部组装文件存储实现，使 UI 不需要直接依赖 DataStore。

## 3. 模块职责

| 模块 | 负责 | 不负责 |
|---|---|---|
| `common` | ID、枚举、实体、查询条件、日志、快照、状态与返回值类型 | 权限判断、状态转换、文件读写、UI |
| `datastore` | JSON 快照的加载与保存接口、未来的原子写入和序列化 | 登录、角色权限、审核、金额校验、查询筛选、统计、通知 |
| `service` | 对外业务 API、未来执行权限校验、状态转换、查询、排序、统计和重复确认 | UI 控件、文件对话框、stdout/stderr 输出、具体 JSON 格式 |
| `ui` | Windows 11 基础窗口、打开文件、另存为文件、调用 Service、展示操作结果 | 业务规则、直接修改实体、直接访问持久化文件 |
| `tests` | CTest 入口和后续单元测试 | 当前不断言任何业务行为 |

除“创建账本、邀请成员、设置审核员、新建草稿、编辑、提交、审核通过、驳回、作废、查看本人所有状态条目、条目筛选和排序”外，其余 Service 业务方法均返回 `ErrorCode::NotImplemented`。UI 的文件打开和保存通过同一 Service 持久化接口执行。

Service 使用内存中的当前快照执行操作，并在绑定文件后立即通过 DataStore 持久化；未绑定文件的内部快照注入仅用于单元测试。`InviteMember()` 表示被邀请者已经接受邀请，因此直接建立 `Member` 成员关系；邀请发送和接受界面不在本切片内。控制台 UI 通过 `CreateLedgerService(DatabaseSnapshot)` 和 `Open()`/`SaveAs()` 使用 Service，不直接接触 DataStore。

## 4. 公共头文件

### 4.1 `include/ledger/common/Types.h`

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ledger {

using UserId = std::int64_t;
using BookId = std::int64_t;
using EntryId = std::int64_t;
using LogId = std::int64_t;
using TimePoint = std::chrono::system_clock::time_point;

enum class Role { Creator, Auditor, Member };

enum class EntryState {
    Draft,
    Waiting_review,
    Rejected,
    Accepted,
    Deleted
};

enum class EntryType { Income, expenditure };

enum class ErrorCode {
    Ok = 0,
    NotImplemented,
    InvalidArgument,
    Unauthenticated,
    PermissionDenied,
    NotFound,
    AlreadyExists,
    InvalidState,
    Conflict,
    ValidationFailed,
    LimitExceeded,
    ConfirmationRequired,
    OperationCancelled,
    IoError,
    DataCorrupted,
    Unsupported,
    InternalError
};

struct Status {
    ErrorCode code{ErrorCode::Ok};
    std::string message;
};

template <typename T>
struct Result {
    Status status{};
    std::optional<T> value;
};

struct Money {
    std::int64_t cents{0};
};

struct User {
    UserId id{0};
    std::wstring username;
    std::wstring display_name;
    std::string password_hash;
};

struct Membership {
    UserId user_id{0};
    std::vector<Role> roles;
};

struct Book {
    BookId id{0};
    std::wstring name;
    UserId creator_id{0};
    std::vector<Membership> members;
    std::optional<Money> budget;
    bool closed{false};
};

struct Entry {
    EntryId id{0};
    BookId book_id{0};
    UserId submitter_id{0};
    EntryState state{EntryState::Draft};
    EntryType type{EntryType::Income};
    Money amount{};
    TimePoint occurred_at{};
    std::wstring payer;
    std::wstring note;
    std::vector<std::filesystem::path> attachments;
    TimePoint created_at{};
    std::optional<TimePoint> submitted_at;
    std::optional<UserId> reviewer_id;
    std::optional<TimePoint> reviewed_at;
    std::optional<EntryId> replaces_entry_id;
};

struct OperationLog {
    LogId id{0};
    BookId book_id{0};
    std::optional<EntryId> entry_id;
    UserId actor_id{0};
    std::wstring action;
    TimePoint operated_at{};
    std::string before_json;
    std::string after_json;
    std::wstring device_info;
};

enum class DatePreset {
    Today,
    Yesterday,
    ThreeDaysAgo,
    LastMonth,
    LastTwoMonths
};

enum class SortField { Time, Amount };
enum class SortDirection { Ascending, Descending };

struct EntryQuery {
    std::optional<EntryState> state;
    std::optional<EntryType> type;
    std::optional<Money> minimum_amount;
    std::optional<Money> maximum_amount;
    std::optional<DatePreset> date_preset;
    std::optional<std::wstring> payer;
    SortField sort_field{SortField::Time};
    SortDirection sort_direction{SortDirection::Descending};
};

struct BookTotals {
    Money income{};
    Money expenditure{};
};

struct PayerExpenditure {
    std::wstring payer;
    Money expenditure{};
};

struct DatabaseSnapshot {
    std::uint64_t schema_version{1};
    std::vector<User> users;
    std::vector<Book> books;
    std::vector<Entry> entries;
    std::vector<OperationLog> logs;
};

} // namespace ledger
```

### 4.2 `include/ledger/datastore/IDataStore.h`

```cpp
#pragma once

#include "ledger/common/Types.h"

namespace ledger {

class IDataStore {
public:
    virtual ~IDataStore() = default;

    virtual Result<DatabaseSnapshot> Load(
        const std::filesystem::path& source) = 0;
    virtual Status Save(
        const std::filesystem::path& destination,
        const DatabaseSnapshot& snapshot) = 0;
};

} // namespace ledger
```

### 4.3 `include/ledger/service/ILedgerService.h`

```cpp
#pragma once

#include "ledger/common/Types.h"

#include <memory>

namespace ledger {

class ILedgerService {
public:
    virtual ~ILedgerService() = default;

    virtual Status Open(const std::filesystem::path& source) = 0;
    virtual Status SaveAs(const std::filesystem::path& destination) = 0;

    virtual Result<Book> CreateBook(
        UserId creator_id,
        const std::wstring& name) = 0;
    virtual Status InviteMember(
        UserId actor_id,
        BookId book_id,
        UserId user_id) = 0;
    virtual Status SetAuditor(
        UserId actor_id,
        BookId book_id,
        UserId user_id,
        bool enabled) = 0;
    virtual Status CloseBook(UserId actor_id, BookId book_id) = 0;
    virtual Status SetBudget(
        UserId actor_id,
        BookId book_id,
        Money budget) = 0;

    virtual Result<Entry> AddEntry(UserId actor_id, const Entry& draft) = 0;
    virtual Status EditEntry(UserId actor_id, const Entry& entry) = 0;
    virtual Status SubmitEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id,
        bool duplicate_confirmed) = 0;
    virtual Status ReviewEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id,
        EntryState decision) = 0;
    virtual Status DiscardEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id) = 0;
    virtual Status RequestModification(
        UserId actor_id,
        BookId book_id,
        EntryId original_entry_id,
        const Entry& replacement) = 0;

    virtual Result<std::vector<Entry>> QueryEntries(
        UserId actor_id,
        BookId book_id,
        const EntryQuery& query) = 0;
    virtual Result<std::vector<Entry>> ListOwnEntries(
        UserId actor_id,
        BookId book_id) = 0;
    virtual Result<std::vector<Entry>> ListPublicEntries(
        UserId actor_id,
        BookId book_id) = 0;
    virtual Result<BookTotals> GetBookTotals(
        UserId actor_id,
        BookId book_id) = 0;
    virtual Result<std::vector<PayerExpenditure>> GetExpenditureByPayer(
        UserId actor_id,
        BookId book_id) = 0;
};

std::unique_ptr<ILedgerService> CreateFileLedgerService();
std::unique_ptr<ILedgerService> CreateLedgerService(DatabaseSnapshot snapshot);

} // namespace ledger
```

## 5. 统一错误码

| 错误码 | 用途 |
|---|---|
| `Ok` | 操作成功 |
| `NotImplemented` | 当前骨架阶段的默认返回 |
| `InvalidArgument` | 参数为空、格式错误或范围组合不合法 |
| `Unauthenticated` | 未登录或登录信息无效 |
| `PermissionDenied` | 角色、条目归属或账本成员权限不足 |
| `NotFound` | 用户、账本、条目或文件不存在 |
| `AlreadyExists` | 账本名称等唯一字段冲突 |
| `InvalidState` | 当前账本或条目状态不允许操作 |
| `Conflict` | 并发审核、修改或保存冲突 |
| `ValidationFailed` | 金额、备注、附件等字段校验失败 |
| `LimitExceeded` | 成员、审核员、附件或容量限制冲突 |
| `ConfirmationRequired` | 疑似重复记账或关闭账本需要确认 |
| `OperationCancelled` | 用户取消需要确认的操作 |
| `IoError` | 文件打开、读取、写入或替换失败 |
| `DataCorrupted` | 快照格式、schema 或持久化数据损坏 |
| `Unsupported` | 文件格式、附件格式或功能不受支持 |
| `InternalError` | 无法归入其他类别的内部错误 |

错误通过 `Status` 或 `Result<T>` 返回。Service 和 DataStore 不输出到 stdout；UI 负责将错误转换为 Windows 提示。

## 6. 内存所有权规则

1. `CreateFileLedgerService()` 返回 `std::unique_ptr<ILedgerService>`，调用者拥有 Service。
2. Service 使用 `std::unique_ptr<IDataStore>` 独占其 DataStore；接口具有虚析构函数。
3. 公共 API 不使用拥有所有权的原始指针，不手动 `new/delete`，不返回悬空引用。
4. 输入参数使用 `const&`，仅表示调用期间借用；被调方不得保存该引用。
5. 实体、字符串、路径、容器和 `Result<T>` 均按值持有或转移所有权。
6. ID 仅表示实体关联，不表示所有权，也不代替指针生命周期管理。
7. `DatabaseSnapshot` 独占其实体容器；Service 是未来唯一允许修改业务快照的上层模块。
8. 附件路径不转移 UI 的所有权。数据库工作区管理附件副本；逻辑作废保留附件，容量自动清理时才物理删除关联附件和日志。
9. UI 不得持有 DataStore 指针，Service 不得持有 UI 窗口或回调所有权。

## 7. 数据持久化方案

- 主文件采用单个 UTF-8 JSON 快照，建议扩展名为 `.ledger.json`。
- 快照包含 `schema_version`、用户、账本、条目和日志；不保存明文密码，只保存密码哈希。
- 金额以整数 `cents` 保存，`1` 表示 `0.01`，避免二进制浮点误差。
- 时间以 UTC ISO 8601 格式保存；日期筛选时由 Service 按 UTC+8 解释。
- 附件存于主文件旁的 `<主文件名>.attachments/` 目录，JSON 中保存相对路径。
- 保存流程设计为“写入同目录临时文件 -> 刷新并关闭 -> 原子替换主文件”，避免半写入快照。
- `Deleted` 条目及其日志、附件继续保留；容量达到 2000 条后的自动清理由 Service 决定，DataStore 只执行持久化变更。
- `Load()`、`Save()` 已实现 UTF-8 JSON 读写；保存使用同目录临时文件和 Windows 原子替换。
- 快照路径必须是带子目录的相对路径，禁止绝对路径和 `..`。控制台默认使用 `data/ledger.json`，从 `code/` 启动时位于 `code/data/ledger.json`。
- Windows 文件对话框使用 `OFN_NOCHANGEDIR`，UI 在打开、保存和新建前恢复 `code/` 工作目录，避免相对路径被解析为 `data/data/ledger.json`。
- 打开文件后，创建账本、邀请成员、设置审核员、审核和作废成功时自动保存；保存失败会回滚本次内存变更。
- 后续仍需增加跨进程文件锁、schema 迁移和备份恢复策略。

## 8. CMake 构建方式

要求 Windows 11、Visual Studio 2022 C++ 工作负载和支持 C++17 的 CMake。工程仅支持 Windows，UI 链接系统 `Comdlg32`。

```powershell
cd "C:\Users\31399\Documents\ChatGPT\软工实验\code"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

生成目标：

| 目标 | 类型 | 说明 |
|---|---|---|
| `ledger_common` | INTERFACE library | 公共数据类型 |
| `ledger_datastore` | STATIC library | 文件存储 stub |
| `ledger_service` | STATIC library | 业务服务 stub |
| `ledger_ui` | Windows executable | 基础 Win32 文件 UI |
| `ledger_console_ui` | STATIC library | 控制台命令解析与 Service 调用 |
| `ledger_console` | executable | 可交互的控制台 UI |
| `ledger_ui_paths` | STATIC library | UI 项目根目录定位与快照相对路径转换 |
| `ledger_tests` | executable/CTest | 空测试，当前固定返回成功 |
| `ledger_service_tests` | executable/CTest | 创建账本、邀请成员、设置审核员测试 |
| `ledger_review_tests` | executable/CTest | 审核通过、驳回、作废及并发审核测试 |
| `ledger_own_entries_tests` | executable/CTest | 本人条目权限、状态和成功路径测试 |
| `ledger_query_tests` | executable/CTest | 类别、金额、收支、日期、付款人和排序测试 |
| `ledger_console_tests` | executable/CTest | UI 无权限、非法状态和成功路径测试 |
| `ledger_file_persistence_tests` | executable/CTest | 真实文件往返、路径限制、重载和业务规则测试 |
| `ledger_ui_paths_tests` | executable/CTest | 绝对路径转换、目录规范化和越界路径拒绝测试 |
| `ledger_entry_editing_tests` | executable/CTest | 草稿、编辑、提交、重复确认和容量清理测试 |

`BUILD_TESTING` 默认为 `ON`。可在配置时增加 `-DBUILD_TESTING=OFF` 关闭测试目标。
