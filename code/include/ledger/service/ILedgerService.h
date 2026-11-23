#pragma once

#include "ledger/common/Types.h"

#include <memory>

namespace ledger {

class ILedgerService {
public:
    virtual ~ILedgerService() = default;

    virtual Status Open(const std::filesystem::path& source) = 0;
    virtual Status SaveAs(const std::filesystem::path& destination) = 0;

    virtual Result<Book> CreateBook(
        UserId creator_id,
        const std::wstring& name) = 0;
    virtual Status InviteMember(
        UserId actor_id,
        BookId book_id,
        UserId user_id) = 0;
    virtual Status SetAuditor(
        UserId actor_id,
        BookId book_id,
        UserId user_id,
        bool enabled) = 0;
    virtual Status CloseBook(UserId actor_id, BookId book_id) = 0;
    virtual Status SetBudget(
        UserId actor_id,
        BookId book_id,
        Money budget) = 0;

    virtual Result<Entry> AddEntry(UserId actor_id, const Entry& draft) = 0;
    virtual Status EditEntry(UserId actor_id, const Entry& entry) = 0;
    virtual Status SubmitEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id,
        bool duplicate_confirmed) = 0;
    virtual Status ReviewEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id,
        EntryState decision) = 0;
    virtual Status DiscardEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id) = 0;
    virtual Status RequestModification(
        UserId actor_id,
        BookId book_id,
        EntryId original_entry_id,
        const Entry& replacement) = 0;

    virtual Result<std::vector<Entry>> QueryEntries(
        UserId actor_id,
        BookId book_id,
        const EntryQuery& query) = 0;
    virtual Result<std::vector<Entry>> ListOwnEntries(
        UserId actor_id,
        BookId book_id) = 0;
    virtual Result<std::vector<Entry>> ListPublicEntries(
        UserId actor_id,
        BookId book_id) = 0;
    virtual Result<BookTotals> GetBookTotals(
        UserId actor_id,
        BookId book_id) = 0;
    virtual Result<std::vector<PayerExpenditure>> GetExpenditureByPayer(
        UserId actor_id,
        BookId book_id) = 0;
};

std::unique_ptr<ILedgerService> CreateFileLedgerService();
std::unique_ptr<ILedgerService> CreateLedgerService(DatabaseSnapshot snapshot);

} // namespace ledger
