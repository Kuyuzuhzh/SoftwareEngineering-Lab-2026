#include "datastore/FileDataStore.h"

#include "ledger/service/ILedgerService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>

namespace {

using namespace ledger;

struct TempTree {
    TempTree() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    ~TempTree() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root{"build/test-output/file_persistence"};
};

Entry MakeEntry(EntryId id, EntryState state) {
    Entry entry;
    entry.id = id;
    entry.book_id = 10;
    entry.submitter_id = 2;
    entry.state = state;
    entry.type = EntryType::expenditure;
    entry.amount = Money{12345};
    entry.occurred_at = TimePoint{std::chrono::seconds{1700000000 + id}};
    entry.created_at = entry.occurred_at;
    entry.payer = L"张三";
    entry.note = L"Unicode \"note\"\nsecond line";
    entry.attachments = {L"attachments/小票 1.png"};
    if (state != EntryState::Draft) {
        entry.submitted_at = entry.occurred_at + std::chrono::seconds{10};
    }
    if (state == EntryState::Accepted) {
        entry.reviewer_id = 3;
        entry.reviewed_at = entry.occurred_at + std::chrono::seconds{20};
    }
    return entry;
}

DatabaseSnapshot MakeSnapshot() {
    DatabaseSnapshot snapshot;
    snapshot.users = {
        User{1, L"创建者", L"Creator", "hash-1"},
        User{2, L"成员", L"Member", "hash-2"},
        User{3, L"审核员", L"Auditor", "hash-3"},
        User{4, L"外部用户", L"Outsider", "hash-4"}};

    Book book;
    book.id = 10;
    book.name = L"持久化账本";
    book.creator_id = 1;
    book.members = {
        Membership{1, {Role::Creator}},
        Membership{2, {Role::Member}},
        Membership{3, {Role::Member, Role::Auditor}}};
    book.budget = Money{50000};
    snapshot.books.push_back(std::move(book));

    snapshot.entries = {
        MakeEntry(100, EntryState::Waiting_review),
        MakeEntry(101, EntryState::Draft),
        MakeEntry(102, EntryState::Accepted)};

    OperationLog log;
    log.id = 1;
    log.book_id = 10;
    log.entry_id = 102;
    log.actor_id = 3;
    log.action = L"ReviewAccepted";
    log.operated_at = TimePoint{std::chrono::seconds{1700000200}};
    log.before_json = "{\"state\":\"Waiting_review\"}";
    log.after_json = "{\"state\":\"Accepted\"}";
    log.device_info = L"Windows 11";
    snapshot.logs.push_back(std::move(log));
    return snapshot;
}

const Entry* FindEntry(const DatabaseSnapshot& snapshot, EntryId id) {
    const auto found = std::find_if(
        snapshot.entries.begin(),
        snapshot.entries.end(),
        [id](const Entry& entry) { return entry.id == id; });
    return found == snapshot.entries.end() ? nullptr : &*found;
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return __LINE__; \
        } \
    } while (false)

int TestRealFileRoundTrip(const std::filesystem::path& root) {
    FileDataStore store;
    const auto file = root / "roundtrip" / "snapshot.json";
    CHECK(store.Save(file, MakeSnapshot()).code == ErrorCode::Ok);
    CHECK(std::filesystem::exists(file));

    auto loaded = store.Load(file);
    CHECK(loaded.status.code == ErrorCode::Ok);
    CHECK(loaded.value.has_value());
    const auto& snapshot = *loaded.value;
    CHECK(snapshot.schema_version == 1);
    CHECK(snapshot.users.size() == 4);
    CHECK(snapshot.users.front().username == L"创建者");
    CHECK(snapshot.books.size() == 1);
    CHECK(snapshot.books.front().name == L"持久化账本");
    CHECK(snapshot.books.front().budget->cents == 50000);
    CHECK(snapshot.books.front().members[2].roles.size() == 2);
    CHECK(snapshot.entries.size() == 3);
    CHECK(snapshot.entries.front().payer == L"张三");
    CHECK(snapshot.entries.front().note == L"Unicode \"note\"\nsecond line");
    CHECK(snapshot.entries.front().attachments.front() ==
          std::filesystem::path{L"attachments/小票 1.png"});
    CHECK(snapshot.entries.front().occurred_at ==
          TimePoint{std::chrono::seconds{1700000100}});
    CHECK(snapshot.logs.size() == 1);
    CHECK(snapshot.logs.front().before_json == "{\"state\":\"Waiting_review\"}");
    CHECK(snapshot.logs.front().device_info == L"Windows 11");
    return 0;
}

int TestPathsAndCorruptFile(const std::filesystem::path& root) {
    FileDataStore store;
    const auto snapshot = MakeSnapshot();

    CHECK(store.Save(std::filesystem::absolute(root / "absolute.json"), snapshot).code ==
          ErrorCode::InvalidArgument);
    CHECK(store.Save("../outside.json", snapshot).code == ErrorCode::InvalidArgument);
    CHECK(store.Save("root-file.json", snapshot).code == ErrorCode::InvalidArgument);
    CHECK(store.Load(root / "missing" / "missing.json").status.code ==
          ErrorCode::NotFound);

    const auto corrupt = root / "corrupt" / "broken.json";
    std::filesystem::create_directories(corrupt.parent_path());
    {
        std::ofstream output{corrupt, std::ios::binary};
        output << "{not valid json";
    }
    CHECK(store.Load(corrupt).status.code == ErrorCode::DataCorrupted);
    return 0;
}

int TestServiceReloadAndRules(const std::filesystem::path& root) {
    const auto file = root / "service" / "ledger.json";
    auto service = CreateLedgerService(MakeSnapshot());
    CHECK(service->SaveAs(file).code == ErrorCode::Ok);

    auto created = service->CreateBook(1, L"重启后仍存在");
    CHECK(created.status.code == ErrorCode::Ok);
    const auto book_id = created.value->id;
    CHECK(service->InviteMember(1, book_id, 4).code == ErrorCode::Ok);
    CHECK(service->SetAuditor(1, book_id, 4, true).code == ErrorCode::Ok);
    CHECK(service->ReviewEntry(3, 10, 100, EntryState::Accepted).code == ErrorCode::Ok);
    CHECK(service->DiscardEntry(3, 10, 102).code == ErrorCode::Ok);

    auto reopened = CreateFileLedgerService();
    CHECK(reopened->Open(file).code == ErrorCode::Ok);
    CHECK(reopened->QueryEntries(4, 10, EntryQuery{}).status.code ==
          ErrorCode::PermissionDenied);
    CHECK(reopened->ReviewEntry(3, 10, 101, EntryState::Accepted).code ==
          ErrorCode::InvalidState);
    CHECK(reopened->QueryEntries(4, book_id, EntryQuery{}).status.code == ErrorCode::Ok);

    auto own = reopened->ListOwnEntries(2, 10);
    CHECK(own.status.code == ErrorCode::Ok);
    CHECK(own.value->size() == 3);

    FileDataStore store;
    auto persisted = store.Load(file);
    CHECK(persisted.status.code == ErrorCode::Ok);
    const auto* accepted = FindEntry(*persisted.value, 100);
    const auto* deleted = FindEntry(*persisted.value, 102);
    CHECK(accepted != nullptr && accepted->state == EntryState::Accepted);
    CHECK(accepted->reviewer_id == 3);
    CHECK(accepted->reviewed_at.has_value());
    CHECK(deleted != nullptr && deleted->state == EntryState::Deleted);
    CHECK(persisted.value->books.size() == 2);
    CHECK(persisted.value->books.back().name == L"重启后仍存在");
    CHECK(persisted.value->books.back().members.size() == 2);
    CHECK(persisted.value->books.back().members.back().user_id == 4);
    CHECK(persisted.value->books.back().members.back().roles.size() == 2);
    CHECK(persisted.value->logs.size() == 3);

    auto second_reopen = CreateFileLedgerService();
    CHECK(second_reopen->Open(file).code == ErrorCode::Ok);
    CHECK(second_reopen->ReviewEntry(3, 10, 100, EntryState::Rejected).code ==
          ErrorCode::Conflict);
    return 0;
}

} // namespace

int main() {
    TempTree temporary;
    if (const int result = TestRealFileRoundTrip(temporary.root)) {
        return result;
    }
    if (const int result = TestPathsAndCorruptFile(temporary.root)) {
        return result;
    }
    if (const int result = TestServiceReloadAndRules(temporary.root)) {
        return result;
    }
    return 0;
}
