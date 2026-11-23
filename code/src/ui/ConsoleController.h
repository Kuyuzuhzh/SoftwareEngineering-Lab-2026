#pragma once

#include "ledger/service/ILedgerService.h"

#include <memory>
#include <ostream>
#include <string>

namespace ledger::ui {

enum class ConsoleCommandResult { Success, Failure, Exit };

class ConsoleController {
public:
    explicit ConsoleController(std::unique_ptr<ILedgerService> service);

    ConsoleCommandResult Execute(
        const std::wstring& line,
        std::wostream& output);

private:
    std::unique_ptr<ILedgerService> service_;
};

} // namespace ledger::ui
