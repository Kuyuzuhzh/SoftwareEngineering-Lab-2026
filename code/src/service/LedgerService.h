#pragma once

#include "ledger/datastore/IDataStore.h"
#include "ledger/service/ILedgerService.h"

#include <memory>
#include <mutex>

namespace ledger {

class LedgerService final : public ILedgerService {
public:
    explicit LedgerService(std::unique_ptr<IDataStore> data_store);
    LedgerService(
        std::unique_ptr<IDataStore> data_store,
        DatabaseSnapshot snapshot);

    Status Open(const std::filesystem::path& source) override;
    Status SaveAs(const std::filesystem::path& destination) override;

    Result<Book> CreateBook(
        UserId creator_id,
        const std::wstring& name) override;
    Status InviteMember(
        UserId actor_id,
        BookId book_id,
        UserId user_id) override;
    Status SetAuditor(
        UserId actor_id,
        BookId book_id,
        UserId user_id,
        bool enabled) override;
    Status CloseBook(UserId actor_id, BookId book_id) override;
    Status SetBudget(
        UserId actor_id,
        BookId book_id,
        Money budget) override;

    Result<Entry> AddEntry(UserId actor_id, const Entry& draft) override;
    Status EditEntry(UserId actor_id, const Entry& entry) override;
    Status SubmitEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id,
        bool duplicate_confirmed) override;
    Status ReviewEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id,
        EntryState decision) override;
    Status DiscardEntry(
        UserId actor_id,
        BookId book_id,
        EntryId entry_id) override;
    Status RequestModification(
        UserId actor_id,
        BookId book_id,
        EntryId original_entry_id,
        const Entry& replacement) override;

    Result<std::vector<Entry>> QueryEntries(
        UserId actor_id,
        BookId book_id,
        const EntryQuery& query) override;
    Result<std::vector<Entry>> ListOwnEntries(
        UserId actor_id,
        BookId book_id) override;
    Result<std::vector<Entry>> ListPublicEntries(
        UserId actor_id,
        BookId book_id) override;
    Result<BookTotals> GetBookTotals(
        UserId actor_id,
        BookId book_id) override;
    Result<std::vector<PayerExpenditure>> GetExpenditureByPayer(
        UserId actor_id,
        BookId book_id) override;

private:
    Status PersistSnapshot();

    std::unique_ptr<IDataStore> data_store_;
    std::optional<std::filesystem::path> current_file_;
    DatabaseSnapshot snapshot_;
    std::mutex snapshot_mutex_;

public:
    const DatabaseSnapshot& snapshot() const noexcept;
};

} // namespace ledger
