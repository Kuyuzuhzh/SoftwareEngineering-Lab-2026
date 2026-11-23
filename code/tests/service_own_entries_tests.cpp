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
    UserId submitter_id,
    EntryState state,
    int occurred_seconds,
    const wchar_t* payer) {
    Entry entry;
    entry.id = id;
    entry.book_id = book_id;
    entry.submitter_id = submitter_id;
    entry.state = state;
    entry.occurred_at = TimePoint{std::chrono::seconds{occurred_seconds}};
    entry.created_at = entry.occurred_at;
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
    book.name = L"Own entries";
    book.creator_id = 1;
    book.closed = closed;
    book.members = {
        Membership{1, {Role::Creator}},
        Membership{2, {Role::Member}},
        Membership{3, {Role::Auditor}}};
    snapshot.books.push_back(book);

    book.id = 20;
    book.name = L"Other book";
    book.members = {Membership{2, {Role::Member}}};
    snapshot.books.push_back(std::move(book));

    snapshot.entries = {
        MakeEntry(100, 10, 2, EntryState::Draft, 100, L"zoe"),
        MakeEntry(101, 10, 2, EntryState::Waiting_review, 400, L"bob"),
        MakeEntry(102, 10, 2, EntryState::Rejected, 300, L"carol"),
        MakeEntry(103, 10, 2, EntryState::Accepted, 400, L"alice"),
        MakeEntry(104, 10, 2, EntryState::Deleted, 200, L"dave"),
        MakeEntry(105, 10, 3, EntryState::Waiting_review, 500, L"other"),
        MakeEntry(106, 20, 2, EntryState::Accepted, 600, L"other-book")};
    if (corrupt_state) {
        snapshot.entries.front().state = static_cast<EntryState>(999);
    }

    return std::make_unique<LedgerService>(
        std::make_unique<TestDataStore>(),
        std::move(snapshot));
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return __LINE__; \
        } \
    } while (false)

int TestPermissionAndStateFailures() {
    auto service = MakeService();

    auto outsider = service->ListOwnEntries(4, 10);
    CHECK(outsider.status.code == ErrorCode::PermissionDenied);
    CHECK(!outsider.value.has_value());

    CHECK(service->ListOwnEntries(99, 10).status.code ==
          ErrorCode::PermissionDenied);
    CHECK(service->ListOwnEntries(2, 999).status.code == ErrorCode::NotFound);
    CHECK(service->ListOwnEntries(0, 10).status.code == ErrorCode::InvalidArgument);
    CHECK(service->ListOwnEntries(2, 0).status.code == ErrorCode::InvalidArgument);

    auto closed_service = MakeService(true);
    auto closed = closed_service->ListOwnEntries(2, 10);
    CHECK(closed.status.code == ErrorCode::InvalidState);
    CHECK(!closed.value.has_value());

    auto corrupted_service = MakeService(false, true);
    auto corrupted = corrupted_service->ListOwnEntries(2, 10);
    CHECK(corrupted.status.code == ErrorCode::DataCorrupted);
    CHECK(!corrupted.value.has_value());
    return 0;
}

int TestReturnsAllOwnStatesOnly() {
    auto service = MakeService();
    auto result = service->ListOwnEntries(2, 10);

    CHECK(result.status.code == ErrorCode::Ok);
    CHECK(result.value.has_value());
    const auto& entries = *result.value;
    CHECK(entries.size() == 5);

    const std::array<EntryId, 5> expected_order{103, 101, 102, 104, 100};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        CHECK(entries[index].id == expected_order[index]);
        CHECK(entries[index].book_id == 10);
        CHECK(entries[index].submitter_id == 2);
    }

    CHECK(std::count_if(entries.begin(), entries.end(), [](const Entry& entry) {
              return entry.state == EntryState::Draft;
          }) == 1);
    CHECK(std::count_if(entries.begin(), entries.end(), [](const Entry& entry) {
              return entry.state == EntryState::Waiting_review;
          }) == 1);
    CHECK(std::count_if(entries.begin(), entries.end(), [](const Entry& entry) {
              return entry.state == EntryState::Rejected;
          }) == 1);
    CHECK(std::count_if(entries.begin(), entries.end(), [](const Entry& entry) {
              return entry.state == EntryState::Accepted;
          }) == 1);
    CHECK(std::count_if(entries.begin(), entries.end(), [](const Entry& entry) {
              return entry.state == EntryState::Deleted;
          }) == 1);

    auto empty = service->ListOwnEntries(1, 10);
    CHECK(empty.status.code == ErrorCode::Ok);
    CHECK(empty.value.has_value() && empty.value->empty());
    return 0;
}

int TestAuditorCanViewOwnEntries() {
    auto service = MakeService();
    auto result = service->ListOwnEntries(3, 10);
    CHECK(result.status.code == ErrorCode::Ok);
    CHECK(result.value.has_value());
    CHECK(result.value->size() == 1);
    CHECK(result.value->front().id == 105);
    return 0;
}

} // namespace

int main() {
    if (const int result = TestPermissionAndStateFailures()) {
        return result;
    }
    if (const int result = TestReturnsAllOwnStatesOnly()) {
        return result;
    }
    if (const int result = TestAuditorCanViewOwnEntries()) {
        return result;
    }
    return 0;
}
