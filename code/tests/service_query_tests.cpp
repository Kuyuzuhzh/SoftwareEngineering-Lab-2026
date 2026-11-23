#include "service/LedgerService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <utility>

namespace {

using namespace ledger;

class TestDataStore final : public IDataStore {
public:
    Result<DatabaseSnapshot> Load(const std::filesystem::path&) override {
        return Result<DatabaseSnapshot>{
            Status{ErrorCode::NotImplemented, "Test double."},
            std::nullopt};
    }

    Status Save(
        const std::filesystem::path&,
        const DatabaseSnapshot&) override {
        return Status{ErrorCode::NotImplemented, "Test double."};
    }
};

Entry MakeEntry(
    EntryId id,
    BookId book_id,
    EntryState state,
    EntryType type,
    std::int64_t cents,
    TimePoint occurred_at,
    const wchar_t* payer) {
    Entry entry;
    entry.id = id;
    entry.book_id = book_id;
    entry.submitter_id = 2;
    entry.state = state;
    entry.type = type;
    entry.amount = Money{cents};
    entry.occurred_at = occurred_at;
    entry.created_at = occurred_at;
    entry.payer = payer;
    return entry;
}

std::unique_ptr<LedgerService> MakeService(
    bool closed = false,
    bool corrupt_state = false) {
    DatabaseSnapshot snapshot;
    for (UserId id = 1; id <= 4; ++id) {
        snapshot.users.push_back(
            User{id, L"user" + std::to_wstring(id), L"User", {}});
    }

    Book book;
    book.id = 10;
    book.name = L"Query book";
    book.creator_id = 1;
    book.closed = closed;
    book.members = {
        Membership{1, {Role::Creator}},
        Membership{2, {Role::Member}},
        Membership{3, {Role::Member, Role::Auditor}}};
    snapshot.books.push_back(book);
    book.id = 20;
    book.name = L"Other book";
    snapshot.books.push_back(std::move(book));

    const auto now = std::chrono::system_clock::now();
    snapshot.entries = {
        MakeEntry(100, 10, EntryState::Accepted, EntryType::Income, 1000, now, L"zoe"),
        MakeEntry(101, 10, EntryState::Accepted, EntryType::expenditure, 2000, now, L"bob"),
        MakeEntry(102, 10, EntryState::Waiting_review, EntryType::expenditure, 1500, now, L"alice"),
        MakeEntry(103, 10, EntryState::Rejected, EntryType::Income, 1000, now, L"charlie"),
        MakeEntry(104, 10, EntryState::Deleted, EntryType::Income, 1000, now, L"deleted"),
        MakeEntry(105, 10, EntryState::Accepted, EntryType::Income, 1000, now, L"alice"),
        MakeEntry(106, 10, EntryState::Accepted, EntryType::Income, 3000, now - std::chrono::hours{24}, L"yesterday"),
        MakeEntry(107, 10, EntryState::Accepted, EntryType::expenditure, 4000, now - std::chrono::hours{72}, L"three-days"),
        MakeEntry(108, 10, EntryState::Accepted, EntryType::Income, 5000, now - std::chrono::hours{24 * 20}, L"month"),
        MakeEntry(109, 10, EntryState::Accepted, EntryType::Income, 6000, now - std::chrono::hours{24 * 45}, L"two-months"),
        MakeEntry(110, 10, EntryState::Accepted, EntryType::Income, 7000, now - std::chrono::hours{24 * 90}, L"old"),
        MakeEntry(111, 20, EntryState::Accepted, EntryType::Income, 1000, now, L"other-book")};
    if (corrupt_state) {
        snapshot.entries.front().state = static_cast<EntryState>(999);
    }

    return std::make_unique<LedgerService>(
        std::make_unique<TestDataStore>(),
        std::move(snapshot));
}

std::vector<EntryId> Ids(const Result<std::vector<Entry>>& result) {
    std::vector<EntryId> ids;
    if (result.value.has_value()) {
        for (const auto& entry : *result.value) {
            ids.push_back(entry.id);
        }
    }
    return ids;
}

bool SameIds(
    const Result<std::vector<Entry>>& result,
    const std::vector<EntryId>& expected) {
    return result.status.code == ErrorCode::Ok && Ids(result) == expected;
}

bool SameIdSet(
    const Result<std::vector<Entry>>& result,
    std::vector<EntryId> expected) {
    if (result.status.code != ErrorCode::Ok || !result.value.has_value()) {
        return false;
    }
    auto actual = Ids(result);
    if (actual.size() != expected.size()) {
        return false;
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected;
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return __LINE__; \
        } \
    } while (false)

int TestPermissionsAndInvalidState() {
    auto service = MakeService();
    EntryQuery query;

    CHECK(service->QueryEntries(4, 10, query).status.code ==
          ErrorCode::PermissionDenied);
    CHECK(service->QueryEntries(2, 999, query).status.code == ErrorCode::NotFound);
    CHECK(service->QueryEntries(0, 10, query).status.code == ErrorCode::InvalidArgument);

    query.state = EntryState::Waiting_review;
    CHECK(service->QueryEntries(2, 10, query).status.code ==
          ErrorCode::PermissionDenied);
    query.state = EntryState::Rejected;
    CHECK(service->QueryEntries(3, 10, query).status.code ==
          ErrorCode::InvalidArgument);
    query.state = static_cast<EntryState>(999);
    CHECK(service->QueryEntries(3, 10, query).status.code ==
          ErrorCode::InvalidArgument);

    auto closed_service = MakeService(true);
    CHECK(closed_service->QueryEntries(2, 10, EntryQuery{}).status.code ==
          ErrorCode::InvalidState);

    auto corrupted_service = MakeService(false, true);
    CHECK(corrupted_service->QueryEntries(2, 10, EntryQuery{}).status.code ==
          ErrorCode::DataCorrupted);
    return 0;
}

int TestRoleVisibility() {
    auto service = MakeService();

    auto member = service->QueryEntries(2, 10, EntryQuery{});
    CHECK(member.status.code == ErrorCode::Ok);
    CHECK(member.value->size() == 8);
    CHECK(std::none_of(member.value->begin(), member.value->end(), [](const Entry& entry) {
        return entry.state != EntryState::Accepted;
    }));

    auto creator = service->QueryEntries(1, 10, EntryQuery{});
    CHECK(creator.status.code == ErrorCode::Ok);
    CHECK(creator.value->size() == 8);

    auto auditor = service->QueryEntries(3, 10, EntryQuery{});
    CHECK(auditor.status.code == ErrorCode::Ok);
    CHECK(auditor.value->size() == 9);
    CHECK(std::all_of(auditor.value->begin(), auditor.value->end(), [](const Entry& entry) {
        return entry.state == EntryState::Accepted ||
               entry.state == EntryState::Waiting_review;
    }));

    EntryQuery waiting;
    waiting.state = EntryState::Waiting_review;
    CHECK(SameIds(service->QueryEntries(3, 10, waiting), {102}));
    return 0;
}

int TestFilters() {
    auto service = MakeService();

    EntryQuery amount;
    amount.minimum_amount = Money{1000};
    amount.maximum_amount = Money{2000};
    amount.sort_field = SortField::Amount;
    amount.sort_direction = SortDirection::Ascending;
    CHECK(SameIds(service->QueryEntries(2, 10, amount), {105, 100, 101}));

    EntryQuery type;
    type.type = EntryType::Income;
    auto incomes = service->QueryEntries(2, 10, type);
    CHECK(incomes.status.code == ErrorCode::Ok);
    CHECK(incomes.value->size() == 6);
    CHECK(std::all_of(incomes.value->begin(), incomes.value->end(), [](const Entry& entry) {
        return entry.type == EntryType::Income;
    }));

    EntryQuery payer;
    payer.payer = std::wstring{L"alice"};
    CHECK(SameIdSet(service->QueryEntries(3, 10, payer), {105, 102}));
    payer.payer = std::wstring{L"ali"};
    CHECK(SameIdSet(service->QueryEntries(3, 10, payer), {}));

    EntryQuery combined;
    combined.state = EntryState::Waiting_review;
    combined.type = EntryType::expenditure;
    combined.minimum_amount = Money{1000};
    combined.maximum_amount = Money{2000};
    combined.date_preset = DatePreset::Today;
    combined.payer = std::wstring{L"alice"};
    CHECK(SameIds(service->QueryEntries(3, 10, combined), {102}));
    return 0;
}

int TestDates() {
    auto service = MakeService();
    EntryQuery query;

    query.date_preset = DatePreset::Today;
    auto today = service->QueryEntries(2, 10, query);
    CHECK(today.status.code == ErrorCode::Ok);
    CHECK(today.value->size() == 3);

    query.date_preset = DatePreset::Yesterday;
    CHECK(SameIds(service->QueryEntries(2, 10, query), {106}));

    query.date_preset = DatePreset::ThreeDaysAgo;
    CHECK(SameIds(service->QueryEntries(2, 10, query), {107}));

    query.date_preset = DatePreset::LastMonth;
    auto month = service->QueryEntries(2, 10, query);
    CHECK(month.status.code == ErrorCode::Ok);
    CHECK(month.value->size() == 6);
    CHECK(std::none_of(month.value->begin(), month.value->end(), [](const Entry& entry) {
        return entry.id == 109 || entry.id == 110;
    }));

    query.date_preset = DatePreset::LastTwoMonths;
    auto two_months = service->QueryEntries(2, 10, query);
    CHECK(two_months.status.code == ErrorCode::Ok);
    CHECK(two_months.value->size() == 7);
    CHECK(std::none_of(two_months.value->begin(), two_months.value->end(), [](const Entry& entry) {
        return entry.id == 110;
    }));
    return 0;
}

int TestSorting() {
    auto service = MakeService();

    CHECK(SameIds(
        service->QueryEntries(2, 10, EntryQuery{}),
        {105, 101, 100, 106, 107, 108, 109, 110}));

    EntryQuery amount;
    amount.sort_field = SortField::Amount;
    amount.sort_direction = SortDirection::Ascending;
    CHECK(SameIds(
        service->QueryEntries(2, 10, amount),
        {105, 100, 101, 106, 107, 108, 109, 110}));

    amount.sort_direction = SortDirection::Descending;
    CHECK(SameIds(
        service->QueryEntries(2, 10, amount),
        {110, 109, 108, 107, 106, 101, 105, 100}));
    return 0;
}

int TestInvalidFilters() {
    auto service = MakeService();
    EntryQuery query;

    query.minimum_amount = Money{0};
    CHECK(service->QueryEntries(2, 10, query).status.code ==
          ErrorCode::InvalidArgument);
    query = EntryQuery{};
    query.maximum_amount = Money{1000000};
    CHECK(service->QueryEntries(2, 10, query).status.code ==
          ErrorCode::InvalidArgument);
    query = EntryQuery{};
    query.minimum_amount = Money{2000};
    query.maximum_amount = Money{1000};
    CHECK(service->QueryEntries(2, 10, query).status.code ==
          ErrorCode::InvalidArgument);
    query = EntryQuery{};
    query.type = static_cast<EntryType>(999);
    CHECK(service->QueryEntries(2, 10, query).status.code ==
          ErrorCode::InvalidArgument);
    query = EntryQuery{};
    query.date_preset = static_cast<DatePreset>(999);
    CHECK(service->QueryEntries(2, 10, query).status.code ==
          ErrorCode::InvalidArgument);
    query = EntryQuery{};
    query.sort_field = static_cast<SortField>(999);
    CHECK(service->QueryEntries(2, 10, query).status.code ==
          ErrorCode::InvalidArgument);
    query.sort_field = SortField::Time;
    query.sort_direction = static_cast<SortDirection>(999);
    CHECK(service->QueryEntries(2, 10, query).status.code ==
          ErrorCode::InvalidArgument);
    return 0;
}

} // namespace

int main() {
    if (const int result = TestPermissionsAndInvalidState()) {
        return result;
    }
    if (const int result = TestRoleVisibility()) {
        return result;
    }
    if (const int result = TestFilters()) {
        return result;
    }
    if (const int result = TestDates()) {
        return result;
    }
    if (const int result = TestSorting()) {
        return result;
    }
    if (const int result = TestInvalidFilters()) {
        return result;
    }
    return 0;
}
