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
