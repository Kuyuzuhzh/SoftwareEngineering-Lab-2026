#include "LedgerService.h"

#include "datastore/FileDataStore.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <limits>
#include <utility>

namespace ledger {
namespace {

constexpr std::size_t kMaximumBooks = 1024;
constexpr std::size_t kMaximumInvitedMembers = 1024;
constexpr std::size_t kMaximumAuditors = 16;
constexpr std::int64_t kMinimumMoneyCents = 1;
constexpr std::int64_t kMaximumMoneyCents = 999999;

Status StubStatus() {
    return Status{ErrorCode::NotImplemented, "该服务操作尚未实现。"};
}

template <typename T>
Result<T> StubResult() {
    return Result<T>{StubStatus(), std::nullopt};
}

Status Success() {
    return Status{};
}

Status Failure(ErrorCode code, const char* message) {
    return Status{code, message};
}

const char* StateName(EntryState state) {
    switch (state) {
    case EntryState::Draft:
        return "Draft";
    case EntryState::Waiting_review:
        return "Waiting_review";
    case EntryState::Rejected:
        return "Rejected";
    case EntryState::Accepted:
        return "Accepted";
    case EntryState::Deleted:
        return "Deleted";
    }
    return "Unknown";
}

bool IsValidEntryState(EntryState state) {
    switch (state) {
    case EntryState::Draft:
    case EntryState::Waiting_review:
    case EntryState::Rejected:
    case EntryState::Accepted:
    case EntryState::Deleted:
        return true;
    }
    return false;
}

bool IsValidEntryType(EntryType type) {
    return type == EntryType::Income || type == EntryType::expenditure;
}

Status ValidateEntryContent(const Entry& entry) {
    if (!IsValidEntryType(entry.type) ||
        entry.amount.cents < kMinimumMoneyCents ||
        entry.amount.cents > kMaximumMoneyCents ||
        entry.occurred_at == TimePoint{} || entry.payer.empty()) {
        return Failure(
            ErrorCode::ValidationFailed,
            "收支类型、金额、发生时间和付款人必须填写有效值。");
    }

    int note_bytes = 0;
    if (!entry.note.empty()) {
        note_bytes = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            entry.note.data(),
            static_cast<int>(entry.note.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (note_bytes <= 0 || note_bytes > 2048) {
            return Failure(ErrorCode::ValidationFailed, "备注格式无效或超过 2048 字节。");
        }
    }

    if (entry.attachments.size() > 10) {
        return Failure(ErrorCode::ValidationFailed, "附件数量不能超过 10 个。");
    }
    for (const auto& attachment : entry.attachments) {
        if (attachment.empty() || attachment.is_absolute() ||
            attachment.has_root_name() || attachment.has_root_directory()) {
            return Failure(ErrorCode::ValidationFailed, "附件必须使用相对路径。");
        }
        for (const auto& component : attachment) {
            if (component == "..") {
                return Failure(ErrorCode::ValidationFailed, "附件路径不能包含 ..。");
            }
        }
        auto extension = attachment.extension().wstring();
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
        if (extension != L".txt" && extension != L".jpg" && extension != L".png") {
            return Failure(ErrorCode::ValidationFailed, "附件仅支持 .txt、.jpg 和 .png。");
        }

        std::error_code error;
        if (!std::filesystem::is_regular_file(attachment, error) || error) {
            return Failure(ErrorCode::ValidationFailed, "附件文件不存在。");
        }
        const auto size = std::filesystem::file_size(attachment, error);
        if (error || size > 64ULL * 1024ULL * 1024ULL) {
            return Failure(ErrorCode::ValidationFailed, "附件大小不能超过 64MB。");
        }
    }
    return Success();
}

struct DateRange {
    TimePoint begin;
    TimePoint end;
    bool include_end{false};
};

std::tm LocalUtcPlus8(TimePoint value) {
    const auto local_time = std::chrono::system_clock::to_time_t(
        value + std::chrono::hours{8});
    std::tm local{};
    gmtime_s(&local, &local_time);
    return local;
}

TimePoint FromLocalUtcPlus8(std::tm local) {
    const auto utc = _mkgmtime64(&local);
    return std::chrono::system_clock::from_time_t(static_cast<time_t>(utc)) -
           std::chrono::hours{8};
}

int DaysInMonth(int year, int zero_based_month) {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (zero_based_month != 1) {
        return days[zero_based_month];
    }
    const bool leap =
        (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
}

std::tm ShiftCalendarMonths(std::tm local, int months) {
    const int current_month = local.tm_year * 12 + local.tm_mon;
    const int target_month = current_month - months;
    local.tm_year = target_month / 12;
    local.tm_mon = target_month % 12;
    if (local.tm_mon < 0) {
        local.tm_mon += 12;
        --local.tm_year;
    }

    const int calendar_year = local.tm_year + 1900;
    local.tm_mday = std::min(
        local.tm_mday,
        DaysInMonth(calendar_year, local.tm_mon));
    return local;
}

Result<DateRange> ResolveDateRange(DatePreset preset, TimePoint now) {
    const auto local_now = LocalUtcPlus8(now);
    auto local_midnight = local_now;
    local_midnight.tm_hour = 0;
    local_midnight.tm_min = 0;
    local_midnight.tm_sec = 0;
    const auto today = FromLocalUtcPlus8(local_midnight);
    constexpr auto day = std::chrono::hours{24};

    switch (preset) {
    case DatePreset::Today:
        return Result<DateRange>{Success(), DateRange{today, today + day, false}};
    case DatePreset::Yesterday:
        return Result<DateRange>{Success(), DateRange{today - day, today, false}};
    case DatePreset::ThreeDaysAgo:
        return Result<DateRange>{
            Success(),
            DateRange{today - day * 3, today - day * 2, false}};
    case DatePreset::LastMonth:
        return Result<DateRange>{
            Success(),
            DateRange{FromLocalUtcPlus8(ShiftCalendarMonths(local_now, 1)), now, true}};
    case DatePreset::LastTwoMonths:
        return Result<DateRange>{
            Success(),
            DateRange{FromLocalUtcPlus8(ShiftCalendarMonths(local_now, 2)), now, true}};
    }
    return Result<DateRange>{
        Failure(ErrorCode::InvalidArgument, "日期预设无效。"),
        std::nullopt};
}

bool IsValidSortField(SortField field) {
    return field == SortField::Time || field == SortField::Amount;
}

bool IsValidSortDirection(SortDirection direction) {
    return direction == SortDirection::Ascending ||
           direction == SortDirection::Descending;
}

std::string StateJson(EntryState state) {
    return std::string{"{\"state\":\""} + StateName(state) + "\"}";
}

Result<LogId> NextLogId(const DatabaseSnapshot& snapshot) {
    LogId next_id = 1;
    for (const auto& log : snapshot.logs) {
        if (log.id == std::numeric_limits<LogId>::max()) {
            return Result<LogId>{
                Failure(ErrorCode::Conflict, "没有可用的日志编号。"),
                std::nullopt};
        }
        next_id = std::max(next_id, static_cast<LogId>(log.id + 1));
    }
    return Result<LogId>{Success(), next_id};
}

Result<EntryId> NextEntryId(const DatabaseSnapshot& snapshot) {
    EntryId next_id = 1;
    for (const auto& entry : snapshot.entries) {
        if (entry.id == std::numeric_limits<EntryId>::max()) {
            return Result<EntryId>{
                Failure(ErrorCode::Conflict, "没有可用的条目编号。"),
                std::nullopt};
        }
        next_id = std::max(next_id, static_cast<EntryId>(entry.id + 1));
    }
    return Result<EntryId>{Success(), next_id};
}

OperationLog MakeStateLog(
    LogId id,
    const Entry& entry,
    UserId actor_id,
    const wchar_t* action,
    EntryState before,
    EntryState after,
    TimePoint operated_at) {
    OperationLog log;
    log.id = id;
    log.book_id = entry.book_id;
    log.entry_id = entry.id;
    log.actor_id = actor_id;
    log.action = action;
    log.operated_at = operated_at;
    log.before_json = StateJson(before);
    log.after_json = StateJson(after);
    return log;
}

bool ContainsRole(const Membership& membership, Role role) {
    return std::find(
               membership.roles.begin(),
               membership.roles.end(),
               role) != membership.roles.end();
}

void AddRole(Membership& membership, Role role) {
    if (!ContainsRole(membership, role)) {
        membership.roles.push_back(role);
    }
}

void RemoveRole(Membership& membership, Role role) {
    membership.roles.erase(
        std::remove(membership.roles.begin(), membership.roles.end(), role),
        membership.roles.end());
}

Membership* FindMembership(Book& book, UserId user_id) {
    const auto found = std::find_if(
        book.members.begin(),
        book.members.end(),
        [user_id](const Membership& membership) {
            return membership.user_id == user_id;
        });
    return found == book.members.end() ? nullptr : &*found;
}

const Membership* FindMembership(const Book& book, UserId user_id) {
    const auto found = std::find_if(
        book.members.begin(),
        book.members.end(),
        [user_id](const Membership& membership) {
            return membership.user_id == user_id;
        });
    return found == book.members.end() ? nullptr : &*found;
}

} // namespace

LedgerService::LedgerService(std::unique_ptr<IDataStore> data_store)
    : LedgerService(std::move(data_store), DatabaseSnapshot{}) {}

LedgerService::LedgerService(
    std::unique_ptr<IDataStore> data_store,
    DatabaseSnapshot snapshot)
    : data_store_(std::move(data_store)), snapshot_(std::move(snapshot)) {}

Status LedgerService::Open(const std::filesystem::path& source) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (!data_store_) {
        return Failure(ErrorCode::InternalError, "尚未配置数据存储。");
    }
    auto loaded = data_store_->Load(source);
    if (loaded.status.code != ErrorCode::Ok) {
        return loaded.status;
    }
    snapshot_ = std::move(*loaded.value);
    current_file_ = source;
    return Success();
}

Status LedgerService::SaveAs(const std::filesystem::path& destination) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (!data_store_) {
        return Failure(ErrorCode::InternalError, "尚未配置数据存储。");
    }
    const auto status = data_store_->Save(destination, snapshot_);
    if (status.code == ErrorCode::Ok) {
        current_file_ = destination;
    }
    return status;
}

Status LedgerService::PersistSnapshot() {
    if (!current_file_) {
        return Success();
    }
    if (!data_store_) {
        return Failure(ErrorCode::InternalError, "尚未配置数据存储。");
    }
    return data_store_->Save(*current_file_, snapshot_);
}

Result<Book> LedgerService::CreateBook(
    UserId creator_id,
    const std::wstring& name) {
    if (creator_id <= 0 || name.empty()) {
        return Result<Book>{
            Failure(ErrorCode::InvalidArgument, "必须提供创建者编号和账本名称。"),
            std::nullopt};
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    const bool user_exists = std::any_of(
        snapshot_.users.begin(),
        snapshot_.users.end(),
        [creator_id](const User& user) { return user.id == creator_id; });
    if (!user_exists) {
        return Result<Book>{
            Failure(ErrorCode::NotFound, "创建者不存在。"),
            std::nullopt};
    }

    if (snapshot_.books.size() >= kMaximumBooks) {
        return Result<Book>{
            Failure(ErrorCode::LimitExceeded, "账本数量已达到上限。"),
            std::nullopt};
    }

    const bool duplicate_name = std::any_of(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [&name](const Book& book) { return book.name == name; });
    if (duplicate_name) {
        return Result<Book>{
            Failure(ErrorCode::AlreadyExists, "账本名称已存在。"),
            std::nullopt};
    }

    BookId next_id = 1;
    for (const auto& book : snapshot_.books) {
        if (book.id == std::numeric_limits<BookId>::max()) {
            return Result<Book>{
                Failure(ErrorCode::Conflict, "没有可用的账本编号。"),
                std::nullopt};
        }
        next_id = std::max(next_id, static_cast<BookId>(book.id + 1));
    }

    Book book;
    book.id = next_id;
    book.name = name;
    book.creator_id = creator_id;
    book.members.push_back(Membership{creator_id, {Role::Creator}});
    snapshot_.books.push_back(book);

    const auto persisted = PersistSnapshot();
    if (persisted.code != ErrorCode::Ok) {
        snapshot_.books.pop_back();
        return Result<Book>{persisted, std::nullopt};
    }
    return Result<Book>{Success(), snapshot_.books.back()};
}

Status LedgerService::InviteMember(
    UserId actor_id,
    BookId book_id,
    UserId user_id) {
    if (actor_id <= 0 || book_id <= 0 || user_id <= 0) {
        return Failure(ErrorCode::InvalidArgument, "编号必须为正数。");
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    const auto book_found = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [book_id](const Book& book) { return book.id == book_id; });
    if (book_found == snapshot_.books.end()) {
        return Failure(ErrorCode::NotFound, "账本不存在。");
    }

    auto& book = *book_found;
    if (book.creator_id != actor_id) {
        return Failure(ErrorCode::PermissionDenied, "只有创建者可以邀请成员。");
    }
    if (book.closed) {
        return Failure(ErrorCode::InvalidState, "已关闭的账本不能加入成员。");
    }

    const bool user_exists = std::any_of(
        snapshot_.users.begin(),
        snapshot_.users.end(),
        [user_id](const User& user) { return user.id == user_id; });
    if (!user_exists) {
        return Failure(ErrorCode::NotFound, "被邀请的用户不存在。");
    }
    if (FindMembership(book, user_id) != nullptr) {
        return Failure(ErrorCode::AlreadyExists, "该用户已经是账本成员。");
    }

    const auto invited_count = static_cast<std::size_t>(std::count_if(
        book.members.begin(),
        book.members.end(),
        [&book](const Membership& membership) {
            return membership.user_id != book.creator_id;
        }));
    if (invited_count >= kMaximumInvitedMembers) {
        return Failure(ErrorCode::LimitExceeded, "受邀成员数量已达到上限。");
    }

    book.members.push_back(Membership{user_id, {Role::Member}});
    const auto persisted = PersistSnapshot();
    if (persisted.code != ErrorCode::Ok) {
        book.members.pop_back();
    }
    return persisted;
}

Status LedgerService::SetAuditor(
    UserId actor_id,
    BookId book_id,
    UserId user_id,
    bool enabled) {
    if (actor_id <= 0 || book_id <= 0 || user_id <= 0) {
        return Failure(ErrorCode::InvalidArgument, "编号必须为正数。");
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    const auto book_found = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [book_id](const Book& book) { return book.id == book_id; });
    if (book_found == snapshot_.books.end()) {
        return Failure(ErrorCode::NotFound, "账本不存在。");
    }

    auto& book = *book_found;
    if (book.creator_id != actor_id) {
        return Failure(
            ErrorCode::PermissionDenied,
            "只有创建者可以设置审核员。");
    }
    if (book.closed) {
        return Failure(
            ErrorCode::InvalidState,
            "已关闭的账本不能变更审核员。");
    }

    auto* membership = FindMembership(book, user_id);
    if (membership == nullptr) {
        return Failure(ErrorCode::NotFound, "目标用户不是账本成员。");
    }

    const auto original_roles = membership->roles;
    if (enabled) {
        if (ContainsRole(*membership, Role::Auditor)) {
            return Success();
        }

        const auto auditor_count = static_cast<std::size_t>(std::count_if(
            book.members.begin(),
            book.members.end(),
            [](const Membership& candidate) {
                return ContainsRole(candidate, Role::Auditor);
            }));
        if (auditor_count >= kMaximumAuditors) {
            return Failure(ErrorCode::LimitExceeded, "审核员数量已达到上限。");
        }
        AddRole(*membership, Role::Auditor);
        const auto persisted = PersistSnapshot();
        if (persisted.code != ErrorCode::Ok) {
            membership->roles = original_roles;
        }
        return persisted;
    }

    RemoveRole(*membership, Role::Auditor);
    if (user_id != book.creator_id) {
        AddRole(*membership, Role::Member);
    }
    const auto persisted = PersistSnapshot();
    if (persisted.code != ErrorCode::Ok) {
        membership->roles = original_roles;
    }
    return persisted;
}

Status LedgerService::CloseBook(UserId, BookId) {
    return StubStatus();
}

Status LedgerService::SetBudget(UserId, BookId, Money) {
    return StubStatus();
}

Result<Entry> LedgerService::AddEntry(UserId actor_id, const Entry& draft) {
    if (actor_id <= 0 || draft.book_id <= 0 || draft.submitter_id != actor_id) {
        return Result<Entry>{
            Failure(ErrorCode::InvalidArgument, "编号必须为正数，且提交者必须为本人。"),
            std::nullopt};
    }
    const auto valid = ValidateEntryContent(draft);
    if (valid.code != ErrorCode::Ok) {
        return Result<Entry>{valid, std::nullopt};
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    const auto book = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [&draft](const Book& candidate) { return candidate.id == draft.book_id; });
    if (book == snapshot_.books.end()) {
        return Result<Entry>{Failure(ErrorCode::NotFound, "账本不存在。"), std::nullopt};
    }
    if (book->closed) {
        return Result<Entry>{
            Failure(ErrorCode::InvalidState, "已关闭的账本不能新建条目。"),
            std::nullopt};
    }
    if (FindMembership(*book, actor_id) == nullptr) {
        return Result<Entry>{
            Failure(ErrorCode::PermissionDenied, "只有账本成员可以新建条目。"),
            std::nullopt};
    }

    auto next_id = NextEntryId(snapshot_);
    if (next_id.status.code != ErrorCode::Ok) {
        return Result<Entry>{next_id.status, std::nullopt};
    }
    auto next_log_id = NextLogId(snapshot_);
    if (next_log_id.status.code != ErrorCode::Ok) {
        return Result<Entry>{next_log_id.status, std::nullopt};
    }

    DatabaseSnapshot previous = snapshot_;
    if (snapshot_.entries.size() >= 2000) {
        const auto oldest = std::min_element(
            snapshot_.entries.begin(),
            snapshot_.entries.end(),
            [](const Entry& left, const Entry& right) {
                return left.occurred_at < right.occurred_at;
            });
        const auto oldest_id = oldest->id;
        snapshot_.logs.erase(
            std::remove_if(
                snapshot_.logs.begin(),
                snapshot_.logs.end(),
                [oldest_id](const OperationLog& log) {
                    return log.entry_id == oldest_id;
                }),
            snapshot_.logs.end());
        snapshot_.entries.erase(oldest);
        next_log_id = NextLogId(snapshot_);
        if (next_log_id.status.code != ErrorCode::Ok) {
            return Result<Entry>{next_log_id.status, std::nullopt};
        }
    }

    Entry entry = draft;
    entry.id = *next_id.value;
    entry.state = EntryState::Draft;
    entry.created_at = std::chrono::system_clock::now();
    entry.submitted_at.reset();
    entry.reviewer_id.reset();
    entry.reviewed_at.reset();
    entry.replaces_entry_id.reset();
    snapshot_.entries.push_back(entry);
    snapshot_.logs.push_back(MakeStateLog(
        *next_log_id.value,
        entry,
        actor_id,
        L"CreateDraft",
        EntryState::Draft,
        EntryState::Draft,
        entry.created_at));

    const auto persisted = PersistSnapshot();
    if (persisted.code != ErrorCode::Ok) {
        snapshot_ = std::move(previous);
        return Result<Entry>{persisted, std::nullopt};
    }
    return Result<Entry>{Success(), snapshot_.entries.back()};
}

Status LedgerService::EditEntry(UserId actor_id, const Entry& edited) {
    if (actor_id <= 0 || edited.id <= 0 || edited.book_id <= 0) {
        return Failure(ErrorCode::InvalidArgument, "编号必须为正数。");
    }
    const auto valid = ValidateEntryContent(edited);
    if (valid.code != ErrorCode::Ok) {
        return valid;
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    const auto book = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [&edited](const Book& candidate) { return candidate.id == edited.book_id; });
    if (book == snapshot_.books.end()) {
        return Failure(ErrorCode::NotFound, "账本不存在。");
    }
    if (book->closed) {
        return Failure(ErrorCode::InvalidState, "已关闭账本中的条目不能编辑。");
    }

    const auto entry = std::find_if(
        snapshot_.entries.begin(),
        snapshot_.entries.end(),
        [&edited](const Entry& candidate) {
            return candidate.id == edited.id && candidate.book_id == edited.book_id;
        });
    if (entry == snapshot_.entries.end()) {
        return Failure(ErrorCode::NotFound, "该账本中不存在此条目。");
    }
    if (entry->submitter_id != actor_id) {
        return Failure(ErrorCode::PermissionDenied, "只能编辑本人条目。");
    }
    if (entry->state != EntryState::Draft && entry->state != EntryState::Rejected) {
        return Failure(ErrorCode::InvalidState, "只有未发布或已驳回条目可以编辑。");
    }

    auto next_log_id = NextLogId(snapshot_);
    if (next_log_id.status.code != ErrorCode::Ok) {
        return next_log_id.status;
    }
    const auto previous = *entry;
    const auto previous_state = entry->state;
    const auto operated_at = std::chrono::system_clock::now();

    entry->type = edited.type;
    entry->amount = edited.amount;
    entry->occurred_at = edited.occurred_at;
    entry->payer = edited.payer;
    entry->note = edited.note;
    entry->attachments = edited.attachments;
    snapshot_.logs.push_back(MakeStateLog(
        *next_log_id.value,
        *entry,
        actor_id,
        previous_state == EntryState::Draft ? L"EditDraft" : L"EditRejected",
        previous_state,
        previous_state,
        operated_at));

    const auto persisted = PersistSnapshot();
    if (persisted.code != ErrorCode::Ok) {
        snapshot_.logs.pop_back();
        *entry = previous;
    }
    return persisted;
}

Status LedgerService::SubmitEntry(
    UserId actor_id,
    BookId book_id,
    EntryId entry_id,
    bool duplicate_confirmed) {
    if (actor_id <= 0 || book_id <= 0 || entry_id <= 0) {
        return Failure(ErrorCode::InvalidArgument, "编号必须为正数。");
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    const auto book = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [book_id](const Book& candidate) { return candidate.id == book_id; });
    if (book == snapshot_.books.end()) {
        return Failure(ErrorCode::NotFound, "账本不存在。");
    }
    if (book->closed) {
        return Failure(ErrorCode::InvalidState, "已关闭账本中的条目不能提交。");
    }

    const auto entry = std::find_if(
        snapshot_.entries.begin(),
        snapshot_.entries.end(),
        [book_id, entry_id](const Entry& candidate) {
            return candidate.id == entry_id && candidate.book_id == book_id;
        });
    if (entry == snapshot_.entries.end()) {
        return Failure(ErrorCode::NotFound, "该账本中不存在此条目。");
    }
    if (entry->submitter_id != actor_id) {
        return Failure(ErrorCode::PermissionDenied, "只能提交本人条目。");
    }
    if (entry->state != EntryState::Draft && entry->state != EntryState::Rejected) {
        return Failure(ErrorCode::InvalidState, "只有未发布或已驳回条目可以提交。");
    }
    const auto valid = ValidateEntryContent(*entry);
    if (valid.code != ErrorCode::Ok) {
        return valid;
    }

    const auto submitted_at = std::chrono::system_clock::now();
    if (!duplicate_confirmed) {
        const auto duplicate = std::find_if(
            snapshot_.entries.begin(),
            snapshot_.entries.end(),
            [&](const Entry& candidate) {
                if (candidate.id == entry_id || candidate.book_id != book_id ||
                    candidate.submitter_id != actor_id ||
                    !candidate.submitted_at.has_value()) {
                    return false;
                }
                const auto interval = submitted_at >= *candidate.submitted_at
                                          ? submitted_at - *candidate.submitted_at
                                          : *candidate.submitted_at - submitted_at;
                return interval <= std::chrono::seconds{60} &&
                       candidate.amount.cents == entry->amount.cents &&
                       candidate.type == entry->type &&
                       candidate.occurred_at == entry->occurred_at &&
                       candidate.note == entry->note;
            });
        if (duplicate != snapshot_.entries.end()) {
            return Failure(
                ErrorCode::ConfirmationRequired,
                "疑似重复记账，是否确认提交？");
        }
    }

    auto next_log_id = NextLogId(snapshot_);
    if (next_log_id.status.code != ErrorCode::Ok) {
        return next_log_id.status;
    }
    const auto previous = *entry;
    const auto previous_state = entry->state;
    entry->state = EntryState::Waiting_review;
    entry->submitted_at = submitted_at;
    entry->reviewer_id.reset();
    entry->reviewed_at.reset();
    snapshot_.logs.push_back(MakeStateLog(
        *next_log_id.value,
        *entry,
        actor_id,
        L"SubmitEntry",
        previous_state,
        EntryState::Waiting_review,
        submitted_at));

    const auto persisted = PersistSnapshot();
    if (persisted.code != ErrorCode::Ok) {
        snapshot_.logs.pop_back();
        *entry = previous;
    }
    return persisted;
}

Status LedgerService::ReviewEntry(
    UserId actor_id,
    BookId book_id,
    EntryId entry_id,
    EntryState decision) {
    if (actor_id <= 0 || book_id <= 0 || entry_id <= 0) {
        return Failure(ErrorCode::InvalidArgument, "编号必须为正数。");
    }
    if (decision != EntryState::Accepted && decision != EntryState::Rejected) {
        return Failure(
            ErrorCode::InvalidArgument,
            "审核结果只能为通过或驳回。");
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    const auto book = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [book_id](const Book& candidate) { return candidate.id == book_id; });
    if (book == snapshot_.books.end()) {
        return Failure(ErrorCode::NotFound, "账本不存在。");
    }
    if (book->closed) {
        return Failure(ErrorCode::InvalidState, "已关闭的账本不能审核。");
    }

    const auto* membership = FindMembership(*book, actor_id);
    if (membership == nullptr || !ContainsRole(*membership, Role::Auditor)) {
        return Failure(ErrorCode::PermissionDenied, "只有审核员可以审核条目。");
    }

    const auto entry = std::find_if(
        snapshot_.entries.begin(),
        snapshot_.entries.end(),
        [book_id, entry_id](const Entry& candidate) {
            return candidate.id == entry_id && candidate.book_id == book_id;
        });
    if (entry == snapshot_.entries.end()) {
        return Failure(ErrorCode::NotFound, "该账本中不存在此条目。");
    }
    if (entry->state != EntryState::Waiting_review) {
        if (entry->state == EntryState::Accepted ||
            entry->state == EntryState::Rejected) {
            return Failure(ErrorCode::Conflict, "该条目已完成审核。");
        }
        return Failure(ErrorCode::InvalidState, "只有待审核条目可以审核。");
    }

    auto next_log_id = NextLogId(snapshot_);
    if (next_log_id.status.code != ErrorCode::Ok) {
        return next_log_id.status;
    }

    const auto operated_at = std::chrono::system_clock::now();
    auto log = MakeStateLog(
        *next_log_id.value,
        *entry,
        actor_id,
        decision == EntryState::Accepted ? L"ReviewAccepted" : L"ReviewRejected",
        EntryState::Waiting_review,
        decision,
        operated_at);
    const auto previous_reviewer = entry->reviewer_id;
    const auto previous_reviewed_at = entry->reviewed_at;
    snapshot_.logs.push_back(std::move(log));

    entry->state = decision;
    entry->reviewer_id = actor_id;
    entry->reviewed_at = operated_at;
    const auto persisted = PersistSnapshot();
    if (persisted.code != ErrorCode::Ok) {
        snapshot_.logs.pop_back();
        entry->state = EntryState::Waiting_review;
        entry->reviewer_id = previous_reviewer;
        entry->reviewed_at = previous_reviewed_at;
    }
    return persisted;
}

Status LedgerService::DiscardEntry(
    UserId actor_id,
    BookId book_id,
    EntryId entry_id) {
    if (actor_id <= 0 || book_id <= 0 || entry_id <= 0) {
        return Failure(ErrorCode::InvalidArgument, "编号必须为正数。");
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    const auto book = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [book_id](const Book& candidate) { return candidate.id == book_id; });
    if (book == snapshot_.books.end()) {
        return Failure(ErrorCode::NotFound, "账本不存在。");
    }
    if (book->closed) {
        return Failure(ErrorCode::InvalidState, "已关闭账本中的条目不能作废。");
    }

    const auto entry = std::find_if(
        snapshot_.entries.begin(),
        snapshot_.entries.end(),
        [book_id, entry_id](const Entry& candidate) {
            return candidate.id == entry_id && candidate.book_id == book_id;
        });
    if (entry == snapshot_.entries.end()) {
        return Failure(ErrorCode::NotFound, "该账本中不存在此条目。");
    }

    const auto* membership = FindMembership(*book, actor_id);
    const bool is_auditor =
        membership != nullptr && ContainsRole(*membership, Role::Auditor);
    const wchar_t* action = nullptr;

    if (entry->state == EntryState::Waiting_review) {
        if (entry->submitter_id != actor_id) {
            return Failure(
                ErrorCode::PermissionDenied,
                "只有原提交者可以作废待审核条目。");
        }
        action = L"DiscardWaiting";
    } else if (entry->state == EntryState::Accepted) {
        if (!is_auditor) {
            return Failure(
                ErrorCode::PermissionDenied,
                "只有审核员可以作废已通过条目。");
        }
        action = L"DiscardAccepted";
    } else {
        return Failure(
            ErrorCode::InvalidState,
            "只有待审核或已通过条目可以作废。");
    }

    auto next_log_id = NextLogId(snapshot_);
    if (next_log_id.status.code != ErrorCode::Ok) {
        return next_log_id.status;
    }

    const auto previous_state = entry->state;
    const auto operated_at = std::chrono::system_clock::now();
    auto log = MakeStateLog(
        *next_log_id.value,
        *entry,
        actor_id,
        action,
        previous_state,
        EntryState::Deleted,
        operated_at);
    snapshot_.logs.push_back(std::move(log));
    entry->state = EntryState::Deleted;
    const auto persisted = PersistSnapshot();
    if (persisted.code != ErrorCode::Ok) {
        snapshot_.logs.pop_back();
        entry->state = previous_state;
    }
    return persisted;
}

Status LedgerService::RequestModification(
    UserId,
    BookId,
    EntryId,
    const Entry&) {
    return StubStatus();
}

Result<std::vector<Entry>> LedgerService::QueryEntries(
    UserId actor_id,
    BookId book_id,
    const EntryQuery& query) {
    if (actor_id <= 0 || book_id <= 0) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::InvalidArgument, "编号必须为正数。"),
            std::nullopt};
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    const auto book = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [book_id](const Book& candidate) { return candidate.id == book_id; });
    if (book == snapshot_.books.end()) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::NotFound, "账本不存在。"),
            std::nullopt};
    }
    if (book->closed) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::InvalidState, "已关闭的账本不能查询。"),
            std::nullopt};
    }

    const auto* membership = FindMembership(*book, actor_id);
    if (membership == nullptr) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::PermissionDenied, "只有账本成员可以查询条目。"),
            std::nullopt};
    }
    const bool is_auditor = ContainsRole(*membership, Role::Auditor);

    if (query.state.has_value()) {
        if (!IsValidEntryState(*query.state) ||
            (*query.state != EntryState::Accepted &&
             *query.state != EntryState::Waiting_review)) {
            return Result<std::vector<Entry>>{
                Failure(
                    ErrorCode::InvalidArgument,
                    "查询状态只能为已通过或待审核。"),
                std::nullopt};
        }
        if (!is_auditor && *query.state == EntryState::Waiting_review) {
            return Result<std::vector<Entry>>{
                Failure(
                    ErrorCode::PermissionDenied,
                    "只有审核员可以查询待审核条目。"),
                std::nullopt};
        }
    }
    if (query.type.has_value() && !IsValidEntryType(*query.type)) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::InvalidArgument, "收支类型无效。"),
            std::nullopt};
    }
    if (!IsValidSortField(query.sort_field) ||
        !IsValidSortDirection(query.sort_direction)) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::InvalidArgument, "排序选项无效。"),
            std::nullopt};
    }

    const auto valid_amount = [](const Money& amount) {
        return amount.cents >= kMinimumMoneyCents &&
               amount.cents <= kMaximumMoneyCents;
    };
    if ((query.minimum_amount.has_value() && !valid_amount(*query.minimum_amount)) ||
        (query.maximum_amount.has_value() && !valid_amount(*query.maximum_amount)) ||
        (query.minimum_amount.has_value() && query.maximum_amount.has_value() &&
         query.minimum_amount->cents > query.maximum_amount->cents)) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::InvalidArgument, "金额范围无效。"),
            std::nullopt};
    }

    std::optional<DateRange> date_range;
    if (query.date_preset.has_value()) {
        auto resolved = ResolveDateRange(
            *query.date_preset,
            std::chrono::system_clock::now());
        if (resolved.status.code != ErrorCode::Ok) {
            return Result<std::vector<Entry>>{resolved.status, std::nullopt};
        }
        date_range = *resolved.value;
    }

    for (const auto& entry : snapshot_.entries) {
        if (entry.book_id == book_id && !IsValidEntryState(entry.state)) {
            return Result<std::vector<Entry>>{
                Failure(ErrorCode::DataCorrupted, "条目状态数据无效。"),
                std::nullopt};
        }
    }

    std::vector<Entry> entries;
    for (const auto& entry : snapshot_.entries) {
        if (entry.book_id != book_id) {
            continue;
        }

        if (query.state.has_value()) {
            if (entry.state != *query.state) {
                continue;
            }
        } else if (is_auditor) {
            if (entry.state != EntryState::Accepted &&
                entry.state != EntryState::Waiting_review) {
                continue;
            }
        } else if (entry.state != EntryState::Accepted) {
            continue;
        }

        if (query.type.has_value() && entry.type != *query.type) {
            continue;
        }
        if (query.minimum_amount.has_value() &&
            entry.amount.cents < query.minimum_amount->cents) {
            continue;
        }
        if (query.maximum_amount.has_value() &&
            entry.amount.cents > query.maximum_amount->cents) {
            continue;
        }
        if (query.payer.has_value() && entry.payer != *query.payer) {
            continue;
        }
        if (date_range.has_value()) {
            if (entry.occurred_at < date_range->begin) {
                continue;
            }
            if (date_range->include_end) {
                if (entry.occurred_at > date_range->end) {
                    continue;
                }
            } else if (entry.occurred_at >= date_range->end) {
                continue;
            }
        }
        entries.push_back(entry);
    }

    std::stable_sort(
        entries.begin(),
        entries.end(),
        [&query](const Entry& left, const Entry& right) {
            if (query.sort_field == SortField::Time &&
                left.occurred_at != right.occurred_at) {
                return query.sort_direction == SortDirection::Descending
                           ? left.occurred_at > right.occurred_at
                           : left.occurred_at < right.occurred_at;
            }
            if (query.sort_field == SortField::Amount &&
                left.amount.cents != right.amount.cents) {
                return query.sort_direction == SortDirection::Descending
                           ? left.amount.cents > right.amount.cents
                           : left.amount.cents < right.amount.cents;
            }
            return left.payer < right.payer;
        });

    return Result<std::vector<Entry>>{Success(), std::move(entries)};
}

Result<std::vector<Entry>> LedgerService::ListOwnEntries(
    UserId actor_id,
    BookId book_id) {
    if (actor_id <= 0 || book_id <= 0) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::InvalidArgument, "编号必须为正数。"),
            std::nullopt};
    }

    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    const auto book = std::find_if(
        snapshot_.books.begin(),
        snapshot_.books.end(),
        [book_id](const Book& candidate) { return candidate.id == book_id; });
    if (book == snapshot_.books.end()) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::NotFound, "账本不存在。"),
            std::nullopt};
    }
    if (book->closed) {
        return Result<std::vector<Entry>>{
            Failure(ErrorCode::InvalidState, "已关闭的账本不能查看。"),
            std::nullopt};
    }
    if (FindMembership(*book, actor_id) == nullptr) {
        return Result<std::vector<Entry>>{
            Failure(
                ErrorCode::PermissionDenied,
                "只有账本成员可以查看本人条目。"),
            std::nullopt};
    }

    std::vector<Entry> entries;
    for (const auto& entry : snapshot_.entries) {
        if (entry.book_id != book_id || entry.submitter_id != actor_id) {
            continue;
        }
        if (!IsValidEntryState(entry.state)) {
            return Result<std::vector<Entry>>{
                Failure(ErrorCode::DataCorrupted, "条目状态数据无效。"),
                std::nullopt};
        }
        entries.push_back(entry);
    }

    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](const Entry& left, const Entry& right) {
            if (left.occurred_at != right.occurred_at) {
                return left.occurred_at > right.occurred_at;
            }
            return left.payer < right.payer;
        });

    return Result<std::vector<Entry>>{Success(), std::move(entries)};
}

Result<std::vector<Entry>> LedgerService::ListPublicEntries(UserId, BookId) {
    return StubResult<std::vector<Entry>>();
}

Result<BookTotals> LedgerService::GetBookTotals(UserId, BookId) {
    return StubResult<BookTotals>();
}

Result<std::vector<PayerExpenditure>> LedgerService::GetExpenditureByPayer(
    UserId,
    BookId) {
    return StubResult<std::vector<PayerExpenditure>>();
}

std::unique_ptr<ILedgerService> CreateFileLedgerService() {
    return std::make_unique<LedgerService>(std::make_unique<FileDataStore>());
}

std::unique_ptr<ILedgerService> CreateLedgerService(DatabaseSnapshot snapshot) {
    return std::make_unique<LedgerService>(
        std::make_unique<FileDataStore>(),
        std::move(snapshot));
}

const DatabaseSnapshot& LedgerService::snapshot() const noexcept {
    return snapshot_;
}

} // namespace ledger
