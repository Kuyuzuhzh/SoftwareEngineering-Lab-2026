#include "service/LedgerService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <thread>
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

Entry MakeEntry(EntryId id, UserId submitter_id, EntryState state) {
    Entry entry;
    entry.id = id;
    entry.book_id = 10;
    entry.submitter_id = submitter_id;
    entry.state = state;
    entry.type = EntryType::expenditure;
    entry.amount = Money{1000};
    entry.occurred_at = TimePoint{std::chrono::seconds{1700000000}};
    entry.created_at = entry.occurred_at;
    entry.payer = L"payer";
    if (state != EntryState::Draft) {
        entry.submitted_at = entry.occurred_at + std::chrono::seconds{60};
    }
    if (state == EntryState::Accepted || state == EntryState::Rejected) {
        entry.reviewer_id = 2;
        entry.reviewed_at = entry.occurred_at + std::chrono::seconds{120};
    }
    return entry;
}

std::unique_ptr<LedgerService> MakeService(bool closed = false) {
    DatabaseSnapshot snapshot;
    for (UserId id = 1; id <= 4; ++id) {
        snapshot.users.push_back(
            User{id, L"user" + std::to_wstring(id), L"User", {}});
    }

    Book book;
    book.id = 10;
    book.name = L"Review book";
    book.creator_id = 1;
    book.closed = closed;
    book.members = {
        Membership{1, {Role::Creator}},
        Membership{2, {Role::Member, Role::Auditor}},
        Membership{3, {Role::Member}},
        Membership{4, {Role::Member, Role::Auditor}}};
    snapshot.books.push_back(std::move(book));

    snapshot.entries = {
        MakeEntry(100, 3, EntryState::Waiting_review),
        MakeEntry(101, 2, EntryState::Waiting_review),
        MakeEntry(102, 3, EntryState::Waiting_review),
        MakeEntry(103, 3, EntryState::Accepted),
        MakeEntry(104, 3, EntryState::Waiting_review),
        MakeEntry(105, 3, EntryState::Draft),
        MakeEntry(106, 3, EntryState::Waiting_review),
        MakeEntry(107, 3, EntryState::Accepted),
        MakeEntry(108, 3, EntryState::Rejected),
        MakeEntry(109, 3, EntryState::Waiting_review)};

    return std::make_unique<LedgerService>(
        std::make_unique<TestDataStore>(),
        std::move(snapshot));
}

const Entry* FindEntry(const LedgerService& service, EntryId id) {
    const auto& entries = service.snapshot().entries;
    const auto found = std::find_if(
        entries.begin(),
        entries.end(),
        [id](const Entry& entry) { return entry.id == id; });
    return found == entries.end() ? nullptr : &*found;
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return __LINE__; \
        } \
    } while (false)

int TestReviewPermissionAndValidation() {
    auto service = MakeService();

    CHECK(service->ReviewEntry(3, 10, 100, EntryState::Accepted).code ==
          ErrorCode::PermissionDenied);
    CHECK(service->ReviewEntry(1, 10, 100, EntryState::Accepted).code ==
          ErrorCode::PermissionDenied);
    CHECK(service->ReviewEntry(2, 10, 100, EntryState::Draft).code ==
          ErrorCode::InvalidArgument);
    CHECK(service->ReviewEntry(2, 999, 100, EntryState::Accepted).code ==
          ErrorCode::NotFound);
    CHECK(service->ReviewEntry(2, 10, 999, EntryState::Accepted).code ==
          ErrorCode::NotFound);
    CHECK(service->ReviewEntry(2, 10, 105, EntryState::Accepted).code ==
          ErrorCode::InvalidState);
    CHECK(FindEntry(*service, 100)->state == EntryState::Waiting_review);
    CHECK(service->snapshot().logs.empty());
    return 0;
}

int TestAcceptAndReject() {
    auto service = MakeService();

    CHECK(service->ReviewEntry(2, 10, 100, EntryState::Accepted).code ==
          ErrorCode::Ok);
    const auto* accepted = FindEntry(*service, 100);
    CHECK(accepted->state == EntryState::Accepted);
    CHECK(accepted->reviewer_id == 2);
    CHECK(accepted->reviewed_at.has_value());
    CHECK(service->snapshot().logs.size() == 1);
    CHECK(service->snapshot().logs.back().action == L"ReviewAccepted");
    CHECK(service->snapshot().logs.back().actor_id == 2);
    CHECK(service->snapshot().logs.back().before_json ==
          "{\"state\":\"Waiting_review\"}");
    CHECK(service->snapshot().logs.back().after_json ==
          "{\"state\":\"Accepted\"}");

    CHECK(service->ReviewEntry(4, 10, 100, EntryState::Rejected).code ==
          ErrorCode::Conflict);
    CHECK(FindEntry(*service, 100)->reviewer_id == 2);

    CHECK(service->ReviewEntry(2, 10, 101, EntryState::Accepted).code ==
          ErrorCode::Ok);
    CHECK(FindEntry(*service, 101)->submitter_id == 2);
    CHECK(FindEntry(*service, 101)->reviewer_id == 2);

    CHECK(service->ReviewEntry(2, 10, 102, EntryState::Rejected).code ==
          ErrorCode::Ok);
    const auto* rejected = FindEntry(*service, 102);
    CHECK(rejected->state == EntryState::Rejected);
    CHECK(rejected->reviewer_id == 2);
    CHECK(rejected->reviewed_at.has_value());
    CHECK(service->snapshot().logs.back().action == L"ReviewRejected");
    CHECK(service->ReviewEntry(4, 10, 102, EntryState::Accepted).code ==
          ErrorCode::Conflict);
    return 0;
}

int TestDiscard() {
    auto service = MakeService();

    CHECK(service->DiscardEntry(4, 10, 106).code == ErrorCode::PermissionDenied);
    CHECK(service->DiscardEntry(3, 10, 107).code == ErrorCode::PermissionDenied);
    CHECK(service->DiscardEntry(2, 10, 108).code == ErrorCode::InvalidState);

    CHECK(service->DiscardEntry(3, 10, 104).code == ErrorCode::Ok);
    const auto* waiting = FindEntry(*service, 104);
    CHECK(waiting->state == EntryState::Deleted);
    CHECK(!waiting->reviewer_id.has_value());
    CHECK(!waiting->reviewed_at.has_value());
    CHECK(service->snapshot().logs.back().action == L"DiscardWaiting");
    CHECK(service->snapshot().logs.back().actor_id == 3);
    CHECK(service->DiscardEntry(3, 10, 104).code == ErrorCode::InvalidState);

    const auto original_review_time = FindEntry(*service, 103)->reviewed_at;
    CHECK(service->DiscardEntry(4, 10, 103).code == ErrorCode::Ok);
    const auto* accepted = FindEntry(*service, 103);
    CHECK(accepted->state == EntryState::Deleted);
    CHECK(accepted->reviewer_id == 2);
    CHECK(accepted->reviewed_at == original_review_time);
    CHECK(service->snapshot().logs.back().action == L"DiscardAccepted");
    CHECK(service->snapshot().logs.back().actor_id == 4);
    return 0;
}

int TestClosedBook() {
    auto service = MakeService(true);
    CHECK(service->ReviewEntry(2, 10, 100, EntryState::Accepted).code ==
          ErrorCode::InvalidState);
    CHECK(service->DiscardEntry(3, 10, 104).code == ErrorCode::InvalidState);
    CHECK(service->DiscardEntry(4, 10, 103).code == ErrorCode::InvalidState);
    return 0;
}

int TestConcurrentReviewHasSingleWinner() {
    auto service = MakeService();
    std::array<ErrorCode, 2> results{ErrorCode::InternalError, ErrorCode::InternalError};

    std::thread first([&] {
        results[0] = service->ReviewEntry(2, 10, 109, EntryState::Accepted).code;
    });
    std::thread second([&] {
        results[1] = service->ReviewEntry(4, 10, 109, EntryState::Rejected).code;
    });
    first.join();
    second.join();

    CHECK(static_cast<int>(results[0] == ErrorCode::Ok) +
              static_cast<int>(results[1] == ErrorCode::Ok) ==
          1);
    CHECK(results[0] == ErrorCode::Conflict || results[1] == ErrorCode::Conflict);

    const auto* entry = FindEntry(*service, 109);
    CHECK(entry->state == EntryState::Accepted || entry->state == EntryState::Rejected);
    CHECK(entry->reviewer_id.has_value());
    CHECK(entry->reviewed_at.has_value());
    CHECK(service->snapshot().logs.size() == 1);
    return 0;
}

} // namespace

int main() {
    if (const int result = TestReviewPermissionAndValidation()) {
        return result;
    }
    if (const int result = TestAcceptAndReject()) {
        return result;
    }
    if (const int result = TestDiscard()) {
        return result;
    }
    if (const int result = TestClosedBook()) {
        return result;
    }
    if (const int result = TestConcurrentReviewHasSingleWinner()) {
        return result;
    }
    return 0;
}
