#include "ConsoleController.h"

#include <windows.h>

#include <chrono>
#include <iostream>
#include <string>

namespace {

ledger::DatabaseSnapshot MakeDemoSnapshot() {
    ledger::DatabaseSnapshot snapshot;
    for (ledger::UserId id = 1; id <= 4; ++id) {
        snapshot.users.push_back(
            ledger::User{id, L"user" + std::to_wstring(id), L"User", {}});
    }

    ledger::Book book;
    book.id = 10;
    book.name = L"Demo book";
    book.creator_id = 1;
    book.members = {
        ledger::Membership{1, {ledger::Role::Creator}},
        ledger::Membership{2, {ledger::Role::Member}},
        ledger::Membership{3, {ledger::Role::Member, ledger::Role::Auditor}}};
    snapshot.books.push_back(std::move(book));

    const auto now = std::chrono::system_clock::now();
    const auto make_entry = [&](ledger::EntryId id,
                                ledger::UserId submitter,
                                ledger::EntryState state,
                                ledger::EntryType type,
                                std::int64_t cents,
                                const wchar_t* payer) {
        ledger::Entry entry;
        entry.id = id;
        entry.book_id = 10;
        entry.submitter_id = submitter;
        entry.state = state;
        entry.type = type;
        entry.amount = ledger::Money{cents};
        entry.occurred_at = now;
        entry.created_at = now;
        entry.payer = payer;
        return entry;
    };
    snapshot.entries = {
        make_entry(100, 2, ledger::EntryState::Waiting_review, ledger::EntryType::Income, 1000, L"alice"),
        make_entry(101, 2, ledger::EntryState::Draft, ledger::EntryType::Income, 1000, L"alice"),
        make_entry(102, 2, ledger::EntryState::Accepted, ledger::EntryType::Income, 1000, L"alice"),
        make_entry(103, 2, ledger::EntryState::Rejected, ledger::EntryType::Income, 1000, L"alice"),
        make_entry(104, 2, ledger::EntryState::Deleted, ledger::EntryType::Income, 1000, L"alice"),
        make_entry(105, 3, ledger::EntryState::Waiting_review, ledger::EntryType::expenditure, 2000, L"bob"),
        make_entry(106, 2, ledger::EntryState::Waiting_review, ledger::EntryType::expenditure, 2000, L"bob")};
    return snapshot;
}

} // namespace

int main() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    const std::filesystem::path data_file = "data/ledger.json";
    auto service = ledger::CreateFileLedgerService();
    auto status = service->Open(data_file);
    if (status.code == ledger::ErrorCode::NotFound) {
        service = ledger::CreateLedgerService(MakeDemoSnapshot());
        status = service->SaveAs(data_file);
    }
    if (status.code != ledger::ErrorCode::Ok) {
        std::cerr << status.message << '\n';
        return 1;
    }

    ledger::ui::ConsoleController controller{std::move(service)};
    std::wcout << L"多人共享记账系统控制台。输入 help 查看命令说明。\n";

    std::wstring line;
    while (true) {
        std::wcout << L"> ";
        if (!std::getline(std::wcin, line)) {
            break;
        }
        const auto result = controller.Execute(line, std::wcout);
        if (result == ledger::ui::ConsoleCommandResult::Exit) {
            break;
        }
    }
    return 0;
}
