#include "service/LedgerService.h"

#include <algorithm>
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

std::unique_ptr<LedgerService> MakeService(std::size_t user_count = 4) {
    DatabaseSnapshot snapshot;
    for (std::size_t index = 1; index <= user_count; ++index) {
        User user;
        user.id = static_cast<UserId>(index);
        user.username = L"user" + std::to_wstring(index);
        snapshot.users.push_back(std::move(user));
    }
    return std::make_unique<LedgerService>(
        std::make_unique<TestDataStore>(),
        std::move(snapshot));
}

const Book* FindBook(const LedgerService& service, BookId id) {
    const auto& books = service.snapshot().books;
    const auto found = std::find_if(
        books.begin(),
        books.end(),
        [id](const Book& book) { return book.id == id; });
    return found == books.end() ? nullptr : &*found;
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

bool HasRole(const Membership& membership, Role role) {
    return std::find(
               membership.roles.begin(),
               membership.roles.end(),
               role) != membership.roles.end();
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return __LINE__; \
        } \
    } while (false)

int TestCreateBook() {
    auto service = MakeService();

    CHECK(service->CreateBook(0, L"Operations").status.code ==
          ErrorCode::InvalidArgument);
    CHECK(service->CreateBook(1, L"").status.code == ErrorCode::InvalidArgument);
    CHECK(service->CreateBook(99, L"Operations").status.code == ErrorCode::NotFound);

    auto created = service->CreateBook(1, L"Operations");
    CHECK(created.status.code == ErrorCode::Ok);
    CHECK(created.value.has_value());
    CHECK(created.value->id == 1);
    CHECK(created.value->creator_id == 1);
    CHECK(!created.value->closed);
    CHECK(created.value->members.size() == 1);
    CHECK(HasRole(created.value->members.front(), Role::Creator));
    CHECK(!HasRole(created.value->members.front(), Role::Auditor));

    CHECK(service->CreateBook(2, L"Operations").status.code ==
          ErrorCode::AlreadyExists);

    auto second = service->CreateBook(2, L"Finance");
    CHECK(second.status.code == ErrorCode::Ok);
    CHECK(second.value->id == 2);
    return 0;
}

int TestInviteMember() {
    auto service = MakeService();
    const auto book_id = service->CreateBook(1, L"Operations").value->id;

    CHECK(service->InviteMember(2, book_id, 3).code ==
          ErrorCode::PermissionDenied);
    CHECK(service->InviteMember(1, 999, 2).code == ErrorCode::NotFound);
    CHECK(service->InviteMember(1, book_id, 99).code == ErrorCode::NotFound);
    CHECK(service->InviteMember(1, book_id, 2).code == ErrorCode::Ok);
    CHECK(service->InviteMember(1, book_id, 2).code == ErrorCode::AlreadyExists);

    const auto* book = FindBook(*service, book_id);
    CHECK(book != nullptr);
    const auto* member = FindMembership(*book, 2);
    CHECK(member != nullptr);
    CHECK(HasRole(*member, Role::Member));
    CHECK(!HasRole(*member, Role::Auditor));
    return 0;
}

int TestSetAuditorAndCombinedRoles() {
    auto service = MakeService();
    const auto book_id = service->CreateBook(1, L"Operations").value->id;
    CHECK(service->InviteMember(1, book_id, 2).code == ErrorCode::Ok);

    CHECK(service->SetAuditor(2, book_id, 2, true).code ==
          ErrorCode::PermissionDenied);
    CHECK(service->SetAuditor(1, book_id, 99, true).code == ErrorCode::NotFound);
    CHECK(service->SetAuditor(1, book_id, 3, true).code == ErrorCode::NotFound);

    CHECK(service->SetAuditor(1, book_id, 2, true).code == ErrorCode::Ok);
    CHECK(service->SetAuditor(1, book_id, 2, true).code == ErrorCode::Ok);
    CHECK(service->SetAuditor(1, book_id, 1, true).code == ErrorCode::Ok);

    const auto* book = FindBook(*service, book_id);
    CHECK(book != nullptr);
    const auto* member = FindMembership(*book, 2);
    const auto* creator = FindMembership(*book, 1);
    CHECK(member != nullptr && creator != nullptr);
    CHECK(HasRole(*member, Role::Member));
    CHECK(HasRole(*member, Role::Auditor));
    CHECK(HasRole(*creator, Role::Creator));
    CHECK(HasRole(*creator, Role::Auditor));

    CHECK(service->SetAuditor(1, book_id, 2, false).code == ErrorCode::Ok);
    CHECK(service->SetAuditor(1, book_id, 1, false).code == ErrorCode::Ok);
    book = FindBook(*service, book_id);
    member = FindMembership(*book, 2);
    creator = FindMembership(*book, 1);
    CHECK(!HasRole(*member, Role::Auditor));
    CHECK(HasRole(*member, Role::Member));
    CHECK(!HasRole(*creator, Role::Auditor));
    CHECK(HasRole(*creator, Role::Creator));
    return 0;
}

int TestClosedBookRejectsMembershipChanges() {
    DatabaseSnapshot snapshot;
    snapshot.users.push_back(User{1, L"creator", L"Creator", {}});
    snapshot.users.push_back(User{2, L"member", L"Member", {}});

    Book book;
    book.id = 1;
    book.name = L"Closed book";
    book.creator_id = 1;
    book.closed = true;
    book.members.push_back(Membership{1, {Role::Creator}});
    snapshot.books.push_back(std::move(book));

    LedgerService service(std::make_unique<TestDataStore>(), std::move(snapshot));

    CHECK(service.InviteMember(1, 1, 2).code == ErrorCode::InvalidState);
    CHECK(service.SetAuditor(1, 1, 1, true).code == ErrorCode::InvalidState);
    return 0;
}

int TestAuditorLimit() {
    auto service = MakeService(18);
    const auto book_id = service->CreateBook(1, L"Operations").value->id;

    for (UserId id = 2; id <= 18; ++id) {
        CHECK(service->InviteMember(1, book_id, id).code == ErrorCode::Ok);
    }
    for (UserId id = 2; id <= 17; ++id) {
        CHECK(service->SetAuditor(1, book_id, id, true).code == ErrorCode::Ok);
    }
    CHECK(service->SetAuditor(1, book_id, 18, true).code ==
          ErrorCode::LimitExceeded);
    return 0;
}

int TestInvitedMemberLimit() {
    auto service = MakeService(1026);
    const auto book_id = service->CreateBook(1, L"Operations").value->id;

    for (UserId id = 2; id <= 1025; ++id) {
        CHECK(service->InviteMember(1, book_id, id).code == ErrorCode::Ok);
    }
    CHECK(service->InviteMember(1, book_id, 1026).code ==
          ErrorCode::LimitExceeded);
    return 0;
}

int TestBookLimit() {
    auto service = MakeService();
    for (std::size_t index = 0; index < 1024; ++index) {
        CHECK(service->CreateBook(1, L"Book " + std::to_wstring(index)).status.code ==
              ErrorCode::Ok);
    }
    CHECK(service->CreateBook(1, L"Overflow").status.code ==
          ErrorCode::LimitExceeded);
    return 0;
}

} // namespace

int main() {
    if (const int result = TestCreateBook()) {
        return result;
    }
    if (const int result = TestInviteMember()) {
        return result;
    }
    if (const int result = TestSetAuditorAndCombinedRoles()) {
        return result;
    }
    if (const int result = TestClosedBookRejectsMembershipChanges()) {
        return result;
    }
    if (const int result = TestAuditorLimit()) {
        return result;
    }
    if (const int result = TestInvitedMemberLimit()) {
        return result;
    }
    if (const int result = TestBookLimit()) {
        return result;
    }
    return 0;
}
