#include "ui/ConsoleController.h"

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace {

using namespace ledger;
using ledger::ui::ConsoleCommandResult;
using ledger::ui::ConsoleController;

DatabaseSnapshot MakeSnapshot() {
    DatabaseSnapshot snapshot;
    for (UserId id = 1; id <= 4; ++id) {
        snapshot.users.push_back(
            User{id, L"user" + std::to_wstring(id), L"User", {}});
    }

    Book book;
    book.id = 10;
    book.name = L"UI book";
    book.creator_id = 1;
    book.members = {
        Membership{1, {Role::Creator}},
        Membership{2, {Role::Member}},
        Membership{3, {Role::Member, Role::Auditor}}};
    snapshot.books.push_back(std::move(book));

    const auto now = std::chrono::system_clock::now();
    const auto make_entry = [&](EntryId id,
                                UserId submitter,
                                EntryState state,
                                EntryType type,
                                std::int64_t cents,
                                const wchar_t* payer) {
        Entry entry;
        entry.id = id;
        entry.book_id = 10;
        entry.submitter_id = submitter;
        entry.state = state;
        entry.type = type;
        entry.amount = Money{cents};
        entry.occurred_at = now;
        entry.created_at = now;
        entry.payer = payer;
        return entry;
    };
    snapshot.entries = {
        make_entry(100, 2, EntryState::Waiting_review, EntryType::Income, 1000, L"alice"),
        make_entry(101, 2, EntryState::Draft, EntryType::Income, 1000, L"alice"),
        make_entry(102, 2, EntryState::Accepted, EntryType::Income, 1000, L"alice"),
        make_entry(103, 2, EntryState::Rejected, EntryType::Income, 1000, L"alice"),
        make_entry(104, 2, EntryState::Deleted, EntryType::Income, 1000, L"alice"),
        make_entry(105, 3, EntryState::Waiting_review, EntryType::expenditure, 2000, L"bob"),
        make_entry(106, 2, EntryState::Waiting_review, EntryType::expenditure, 2000, L"bob")};
    return snapshot;
}

std::unique_ptr<ConsoleController> MakeController() {
    return std::make_unique<ConsoleController>(
        CreateLedgerService(MakeSnapshot()));
}

struct Invocation {
    ConsoleCommandResult result;
    std::wstring output;
};

Invocation Run(ConsoleController& controller, const std::wstring& command) {
    std::wostringstream output;
    const auto result = controller.Execute(command, output);
    return Invocation{result, output.str()};
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            return __LINE__; \
        } \
    } while (false)

int TestPermissionFailuresReachUi() {
    auto controller = MakeController();

    auto query = Run(*controller, L"query 4 10");
    CHECK(query.result == ConsoleCommandResult::Failure);
    CHECK(query.output.find(L"权限不足") != std::wstring::npos);

    auto review = Run(*controller, L"accept 2 10 100");
    CHECK(review.result == ConsoleCommandResult::Failure);
    CHECK(review.output.find(L"权限不足") != std::wstring::npos);
    return 0;
}

int TestInvalidStateReachesUi() {
    auto controller = MakeController();
    auto result = Run(*controller, L"accept 3 10 101");
    CHECK(result.result == ConsoleCommandResult::Failure);
    CHECK(result.output.find(L"状态无效") != std::wstring::npos);
    return 0;
}

int TestMembershipSuccessPath() {
    auto controller = MakeController();

    auto created = Run(*controller, L"create 1 \"Created from UI\"");
    CHECK(created.result == ConsoleCommandResult::Success);
    CHECK(created.output.find(L"账本编号=11") != std::wstring::npos);
    CHECK(Run(*controller, L"invite 1 11 4").result ==
          ConsoleCommandResult::Success);
    CHECK(Run(*controller, L"auditor 1 11 4 on").result ==
          ConsoleCommandResult::Success);
    CHECK(Run(*controller, L"query 4 11").result ==
          ConsoleCommandResult::Success);
    return 0;
}

int TestReviewAndDiscardSuccessPath() {
    auto controller = MakeController();

    CHECK(Run(*controller, L"accept 3 10 100").result ==
          ConsoleCommandResult::Success);
    CHECK(Run(*controller, L"reject 3 10 105").result ==
          ConsoleCommandResult::Success);
    CHECK(Run(*controller, L"discard 2 10 106").result ==
          ConsoleCommandResult::Success);

    auto own = Run(*controller, L"own 2 10");
    CHECK(own.result == ConsoleCommandResult::Success);
    CHECK(own.output.find(L"数量=6") != std::wstring::npos);
    CHECK(own.output.find(L"状态=未发布") != std::wstring::npos);
    CHECK(own.output.find(L"状态=已通过") != std::wstring::npos);
    CHECK(own.output.find(L"状态=已驳回") != std::wstring::npos);
    CHECK(own.output.find(L"状态=已作废") != std::wstring::npos);
    return 0;
}

int TestQuerySuccessPath() {
    auto controller = MakeController();

    auto waiting = Run(
        *controller,
        L"query 3 10 waiting expenditure 1.00 20.00 today bob amount asc");
    CHECK(waiting.result == ConsoleCommandResult::Success);
    CHECK(waiting.output.find(L"数量=2") != std::wstring::npos);
    CHECK(waiting.output.find(L"编号=105") != std::wstring::npos);
    CHECK(waiting.output.find(L"编号=106") != std::wstring::npos);

    auto accepted = Run(
        *controller,
        L"query 2 10 accepted income 10.00 10.00 today alice time desc");
    CHECK(accepted.result == ConsoleCommandResult::Success);
    CHECK(accepted.output.find(L"数量=1") != std::wstring::npos);
    CHECK(accepted.output.find(L"编号=102") != std::wstring::npos);
    return 0;
}

int TestInputAndExit() {
    auto controller = MakeController();
    CHECK(Run(*controller, L"query 3 10 waiting expenditure 1e2 20 today bob").result ==
          ConsoleCommandResult::Failure);
    CHECK(Run(*controller, L"unknown").result == ConsoleCommandResult::Failure);
    CHECK(Run(*controller, L"exit").result == ConsoleCommandResult::Exit);
    return 0;
}

} // namespace

int main() {
    if (const int result = TestPermissionFailuresReachUi()) {
        return result;
    }
    if (const int result = TestInvalidStateReachesUi()) {
        return result;
    }
    if (const int result = TestMembershipSuccessPath()) {
        return result;
    }
    if (const int result = TestReviewAndDiscardSuccessPath()) {
        return result;
    }
    if (const int result = TestQuerySuccessPath()) {
        return result;
    }
    if (const int result = TestInputAndExit()) {
        return result;
    }
    return 0;
}
