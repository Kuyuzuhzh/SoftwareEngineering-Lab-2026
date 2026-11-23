#pragma once

#include "ledger/service/ILedgerService.h"

#include <windows.h>

namespace ledger::ui {

void ShowOperationsDialog(HWND owner, ILedgerService& service);

} // namespace ledger::ui
