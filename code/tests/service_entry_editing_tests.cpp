#include "service/LedgerService.h"

#include <algorithm>
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
    Status Save(const std::filesystem::path&, const DatabaseSnapshot&) override {
        return Status{ErrorCode::NotImplemented, "Test double."};
    }
};

Entry MakeDraft(UserId submitter = 2) {
    Entry entry;
    entry.book_id = 10;
    entry.submitter_id = submitter;
    entry.type = EntryType::expenditure;
    entry.amount = Money{1000};
    entry.occurred_at = TimePoint{std::chrono::seconds{1700000000}};
    entry.payer = L"alice";
    entry.note = L"same";
    return entry;
}

DatabaseSnapshot MakeSnapshot(bool closed = false) {
    DatabaseSnapshot snapshot;
    for (UserId id = 1; id <= 4; ++id) {
        snapshot.users.push_back(User{id, L"user", L"User", {}});
    }
    Book book;
    book.id = 10;
    book.name = L"Entries";
    book.creator_id = 1;
    book.closed = closed;
    book.members = {
        Membership{1, {Role::Creator}},
        Membership{2, {Role::Member}},
        Membership{3, {Role::Member, Role::Auditor}}};
    snapshot.books.push_back(std::move(book));
    return snapshot;
}

std::unique_ptr<LedgerService> MakeService(bool closed = false) {
    return std::make_unique<LedgerService>(
        std::make_unique<TestDataStore>(),
        MakeSnapshot(closed));
}

const Entry* FindEntry(const LedgerService& service, EntryId id) {
    const auto& entries = service.snapshot().entries;
    const auto found = std::find_if(
        entries.begin(), entries.end(), [id](const Entry& entry) {
            return entry.id == id;
        });
    return found == entries.end() ? nullptr : &*found;
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return __LINE__; \
        } \
    } while (false)

int TestAddValidationAndPermissions() {
    auto service = MakeService();

    CHECK(service->AddEntry(4, MakeDraft(4)).status.code ==
          ErrorCode::PermissionDenied);
    CHECK(service->AddEntry(2, MakeDraft(3)).status.code ==
          ErrorCode::InvalidArgument);

    auto invalid = MakeDraft();
    invalid.amount.cents = 0;
    CHECK(service->AddEntry(2, invalid).status.code == ErrorCode::ValidationFailed);
    invalid = MakeDraft();
    invalid.payer.clear();
    CHECK(service->AddEntry(2, invalid).status.code == ErrorCode::ValidationFailed);
    invalid = MakeDraft();
    invalid.note = std::wstring(2049, L'a');
    CHECK(service->AddEntry(2, invalid).status.code == ErrorCode::ValidationFailed);
    invalid = MakeDraft();
    invalid.attachments.resize(11, L"attachments/a.png");
    CHECK(service->AddEntry(2, invalid).status.code == ErrorCode::ValidationFailed);
    invalid = MakeDraft();
    invalid.attachments = {L"attachments/a.exe"};
    CHECK(service->AddEntry(2, invalid).status.code == ErrorCode::ValidationFailed);

    auto closed = MakeService(true);
    CHECK(closed->AddEntry(2, MakeDraft()).status.code == ErrorCode::InvalidState);
    return 0;
}

int TestAddEditAndSubmitSuccess() {
    auto service = MakeService();
    auto created = service->AddEntry(2, MakeDraft());
    CHECK(created.status.code == ErrorCode::Ok);
    CHECK(created.value->id == 1);
    CHECK(created.value->state == EntryState::Draft);
    CHECK(created.value->created_at != TimePoint{});
    CHECK(service->snapshot().logs.size() == 1);

    auto edited = *created.value;
    edited.amount.cents = 2000;
    edited.payer = L"bob";
    edited.note = L"edited";
    CHECK(service->EditEntry(3, edited).code == ErrorCode::PermissionDenied);
    CHECK(service->EditEntry(2, edited).code == ErrorCode::Ok);
    CHECK(FindEntry(*service, 1)->amount.cents == 2000);
    CHECK(FindEntry(*service, 1)->payer == L"bob");
    CHECK(FindEntry(*service, 1)->note == L"edited");

    CHECK(service->SubmitEntry(3, 10, 1, false).code == ErrorCode::PermissionDenied);
    CHECK(service->SubmitEntry(2, 10, 1, false).code == ErrorCode::Ok);
    CHECK(FindEntry(*service, 1)->state == EntryState::Waiting_review);
    CHECK(FindEntry(*service, 1)->submitted_at.has_value());
    CHECK(service->EditEntry(2, edited).code == ErrorCode::InvalidState);
    CHECK(service->SubmitEntry(2, 10, 1, false).code == ErrorCode::InvalidState);
    return 0;
}

int TestRejectedCanEditAndResubmit() {
    auto service = MakeService();
    const auto id = service->AddEntry(2, MakeDraft()).value->id;
    CHECK(service->SubmitEntry(2, 10, id, false).code == ErrorCode::Ok);
    CHECK(service->ReviewEntry(3, 10, id, EntryState::Rejected).code == ErrorCode::Ok);

    auto rejected = *FindEntry(*service, id);
    rejected.note = L"changed after rejection";
    CHECK(service->EditEntry(2, rejected).code == ErrorCode::Ok);
    CHECK(service->SubmitEntry(2, 10, id, false).code == ErrorCode::Ok);
    const auto* entry = FindEntry(*service, id);
    CHECK(entry->state == EntryState::Waiting_review);
    CHECK(entry->note == L"changed after rejection");
    CHECK(!entry->reviewer_id.has_value());
    CHECK(!entry->reviewed_at.has_value());
    return 0;
}

int TestDuplicateConfirmation() {
    auto service = MakeService();
    const auto first = service->AddEntry(2, MakeDraft()).value->id;
    const auto second = service->AddEntry(2, MakeDraft()).value->id;
    CHECK(service->SubmitEntry(2, 10, first, false).code == ErrorCode::Ok);
    CHECK(service->SubmitEntry(2, 10, second, false).code ==
          ErrorCode::ConfirmationRequired);
    CHECK(FindEntry(*service, second)->state == EntryState::Draft);
    CHECK(service->SubmitEntry(2, 10, second, true).code == ErrorCode::Ok);

    const auto other_user = service->AddEntry(3, MakeDraft(3)).value->id;
    CHECK(service->SubmitEntry(3, 10, other_user, false).code == ErrorCode::Ok);
    return 0;
}

int TestCapacityCleanup() {
    auto snapshot = MakeSnapshot();
    for (EntryId id = 1; id <= 2000; ++id) {
        auto entry = MakeDraft();
        entry.id = id;
        entry.occurred_at = TimePoint{std::chrono::seconds{id}};
        snapshot.entries.push_back(entry);
    }
    OperationLog log;
    log.id = 1;
    log.book_id = 10;
    log.entry_id = 1;
    snapshot.logs.push_back(log);
    LedgerService service(std::make_unique<TestDataStore>(), std::move(snapshot));

    auto created = service.AddEntry(2, MakeDraft());
    CHECK(created.status.code == ErrorCode::Ok);
    CHECK(service.snapshot().entries.size() == 2000);
    CHECK(FindEntry(service, 1) == nullptr);
    CHECK(FindEntry(service, 2001) != nullptr);
    CHECK(std::none_of(
        service.snapshot().logs.begin(),
        service.snapshot().logs.end(),
        [](const OperationLog& log) { return log.entry_id == 1; }));
    return 0;
}

} // namespace

int main() {
    if (const int result = TestAddValidationAndPermissions()) return result;
    if (const int result = TestAddEditAndSubmitSuccess()) return result;
    if (const int result = TestRejectedCanEditAndResubmit()) return result;
    if (const int result = TestDuplicateConfirmation()) return result;
    if (const int result = TestCapacityCleanup()) return result;
    return 0;
}
