#pragma once

#include "ledger/common/Types.h"

#include <string>
#include <string_view>

namespace ledger {

Result<std::string> SerializeSnapshot(const DatabaseSnapshot& snapshot);
Result<DatabaseSnapshot> DeserializeSnapshot(std::string_view json);

} // namespace ledger
