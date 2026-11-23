#pragma once

#include "ledger/common/Types.h"

namespace ledger {

class IDataStore {
public:
    virtual ~IDataStore() = default;

    virtual Result<DatabaseSnapshot> Load(
        const std::filesystem::path& source) = 0;
    virtual Status Save(
        const std::filesystem::path& destination,
        const DatabaseSnapshot& snapshot) = 0;
};

} // namespace ledger
