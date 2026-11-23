#pragma once

#include "ledger/datastore/IDataStore.h"

namespace ledger {

class FileDataStore final : public IDataStore {
public:
    Result<DatabaseSnapshot> Load(
        const std::filesystem::path& source) override;
    Status Save(
        const std::filesystem::path& destination,
        const DatabaseSnapshot& snapshot) override;
};

} // namespace ledger
