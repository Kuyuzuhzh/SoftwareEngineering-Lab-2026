#include "ConsoleController.h"

#include <windows.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

namespace ledger::ui {
namespace {

const wchar_t* ErrorName(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok:
        return L"成功";
    case ErrorCode::NotImplemented:
        return L"尚未实现";
    case ErrorCode::InvalidArgument:
        return L"参数无效";
    case ErrorCode::Unauthenticated:
        return L"未登录或登录信息无效";
    case ErrorCode::PermissionDenied:
        return L"权限不足";
    case ErrorCode::NotFound:
        return L"未找到";
    case ErrorCode::AlreadyExists:
        return L"已存在";
    case ErrorCode::InvalidState:
        return L"状态无效";
    case ErrorCode::Conflict:
        return L"操作冲突";
    case ErrorCode::ValidationFailed:
        return L"校验失败";
    case ErrorCode::LimitExceeded:
        return L"超出限制";
    case ErrorCode::ConfirmationRequired:
        return L"需要确认";
    case ErrorCode::OperationCancelled:
        return L"操作已取消";
    case ErrorCode::IoError:
        return L"文件读写错误";
    case ErrorCode::DataCorrupted:
        return L"数据损坏";
    case ErrorCode::Unsupported:
        return L"不支持";
    case ErrorCode::InternalError:
        return L"内部错误";
    }
    return L"未知错误";
}

std::wstring WidenUtf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (size <= 0) {
        return L"错误说明编码无效";
    }
    std::wstring result;
    result.resize(static_cast<std::size_t>(size));
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size);
    return result;
}

bool PrintStatus(const Status& status, std::wostream& output) {
    if (status.code == ErrorCode::Ok) {
        output << L"操作成功\n";
        return true;
    }
    output << L"错误=" << ErrorName(status.code)
           << L" 说明=" << WidenUtf8(status.message) << L'\n';
    return false;
}

bool AtEnd(std::wistringstream& input) {
    input >> std::ws;
    return input.eof();
}

bool ParseMoney(const std::wstring& text, Money& money) {
    if (text.empty()) {
        return false;
    }

    const auto decimal = text.find(L'.');
    if (decimal != std::wstring::npos &&
        text.find(L'.', decimal + 1) != std::wstring::npos) {
        return false;
    }

    const auto whole = text.substr(0, decimal);
    const auto fraction = decimal == std::wstring::npos
                              ? std::wstring{}
                              : text.substr(decimal + 1);
    if (whole.empty() || fraction.size() > 2 ||
        (decimal != std::wstring::npos && fraction.empty())) {
        return false;
    }

    std::int64_t cents = 0;
    for (const wchar_t character : whole) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        cents = cents * 10 + (character - L'0');
        if (cents > 1000000) {
            return false;
        }
    }

    cents *= 100;
    for (const wchar_t character : fraction) {
        if (character < L'0' || character > L'9') {
            return false;
        }
    }
    if (!fraction.empty()) {
        cents += (fraction[0] - L'0') * 10;
    }
    if (fraction.size() == 2) {
        cents += fraction[1] - L'0';
    }
    money.cents = cents;
    return true;
}

const wchar_t* StateName(EntryState state) {
    switch (state) {
    case EntryState::Draft:
        return L"未发布";
    case EntryState::Waiting_review:
        return L"待审核";
    case EntryState::Rejected:
        return L"已驳回";
    case EntryState::Accepted:
        return L"已通过";
    case EntryState::Deleted:
        return L"已作废";
    }
    return L"未知状态";
}

void PrintEntries(const std::vector<Entry>& entries, std::wostream& output) {
    output << L"数量=" << entries.size() << L'\n';
    for (const auto& entry : entries) {
        output << L"编号=" << entry.id << L" 状态=" << StateName(entry.state)
               << L" 类型="
               << (entry.type == EntryType::Income ? L"收入" : L"支出")
               << L" 金额=" << entry.amount.cents / 100 << L'.'
               << std::setw(2) << std::setfill(L'0') << entry.amount.cents % 100
               << std::setfill(L' ') << L" 付款人=" << entry.payer << L'\n';
    }
}

ConsoleCommandResult InvalidCommand(std::wostream& output) {
    output << L"错误=无效命令\n";
    return ConsoleCommandResult::Failure;
}

} // namespace

ConsoleController::ConsoleController(std::unique_ptr<ILedgerService> service)
    : service_(std::move(service)) {}

ConsoleCommandResult ConsoleController::Execute(
    const std::wstring& line,
    std::wostream& output) {
    std::wistringstream input{line};
    std::wstring command;
    if (!(input >> command)) {
        return ConsoleCommandResult::Success;
    }

    if (command == L"exit") {
        return ConsoleCommandResult::Exit;
    }
    if (command == L"help") {
        output
            << L"create <创建者编号> <带引号的账本名称>：创建账本\n"
            << L"invite <操作人编号> <账本编号> <用户编号>：邀请成员\n"
            << L"auditor <操作人编号> <账本编号> <用户编号> <on|off>：设置或取消审核员\n"
            << L"accept|reject|discard <操作人编号> <账本编号> <条目编号>：通过、驳回或作废\n"
            << L"own <用户编号> <账本编号>：查看本人所有状态的条目\n"
            << L"query <用户编号> <账本编号> [状态] [收支类型] [最低金额] [最高金额] [日期] [付款人] [排序字段] [排序方向]：筛选条目\n"
            << L"省略的查询条件使用 -。日期选项：today、yesterday、3days、1month、2months。\n";
        return ConsoleCommandResult::Success;
    }

    if (command == L"create") {
        UserId creator_id{};
        std::wstring name;
        if (!(input >> creator_id >> std::quoted(name)) || !AtEnd(input)) {
            return InvalidCommand(output);
        }
        auto result = service_->CreateBook(creator_id, name);
        if (!PrintStatus(result.status, output)) {
            return ConsoleCommandResult::Failure;
        }
        output << L"账本编号=" << result.value->id << L'\n';
        return ConsoleCommandResult::Success;
    }

    if (command == L"invite" || command == L"auditor") {
        UserId actor_id{};
        BookId book_id{};
        UserId user_id{};
        if (!(input >> actor_id >> book_id >> user_id)) {
            return InvalidCommand(output);
        }
        if (command == L"invite") {
            if (!AtEnd(input)) {
                return InvalidCommand(output);
            }
            return PrintStatus(service_->InviteMember(actor_id, book_id, user_id), output)
                       ? ConsoleCommandResult::Success
                       : ConsoleCommandResult::Failure;
        }

        std::wstring enabled;
        if (!(input >> enabled) || !AtEnd(input) ||
            (enabled != L"on" && enabled != L"off")) {
            return InvalidCommand(output);
        }
        return PrintStatus(
                   service_->SetAuditor(
                       actor_id,
                       book_id,
                       user_id,
                       enabled == L"on"),
                   output)
                   ? ConsoleCommandResult::Success
                   : ConsoleCommandResult::Failure;
    }

    if (command == L"accept" || command == L"reject" || command == L"discard") {
        UserId actor_id{};
        BookId book_id{};
        EntryId entry_id{};
        if (!(input >> actor_id >> book_id >> entry_id) || !AtEnd(input)) {
            return InvalidCommand(output);
        }
        const auto status = command == L"discard"
                                ? service_->DiscardEntry(actor_id, book_id, entry_id)
                                : service_->ReviewEntry(
                                      actor_id,
                                      book_id,
                                      entry_id,
                                      command == L"accept"
                                          ? EntryState::Accepted
                                          : EntryState::Rejected);
        return PrintStatus(status, output)
                   ? ConsoleCommandResult::Success
                   : ConsoleCommandResult::Failure;
    }

    if (command == L"own") {
        UserId actor_id{};
        BookId book_id{};
        if (!(input >> actor_id >> book_id) || !AtEnd(input)) {
            return InvalidCommand(output);
        }
        auto result = service_->ListOwnEntries(actor_id, book_id);
        if (!PrintStatus(result.status, output)) {
            return ConsoleCommandResult::Failure;
        }
        PrintEntries(*result.value, output);
        return ConsoleCommandResult::Success;
    }

    if (command == L"query") {
        UserId actor_id{};
        BookId book_id{};
        if (!(input >> actor_id >> book_id)) {
            return InvalidCommand(output);
        }

        std::array<std::wstring, 8> options{
            L"-", L"-", L"-", L"-", L"-", L"-", L"time", L"desc"};
        std::size_t index = 0;
        std::wstring option;
        while (input >> option) {
            if (index >= options.size()) {
                return InvalidCommand(output);
            }
            options[index++] = option;
        }

        EntryQuery query;
        if (options[0] == L"accepted") {
            query.state = EntryState::Accepted;
        } else if (options[0] == L"waiting") {
            query.state = EntryState::Waiting_review;
        } else if (options[0] != L"-") {
            return InvalidCommand(output);
        }

        if (options[1] == L"income") {
            query.type = EntryType::Income;
        } else if (options[1] == L"expenditure") {
            query.type = EntryType::expenditure;
        } else if (options[1] != L"-") {
            return InvalidCommand(output);
        }

        Money minimum{};
        Money maximum{};
        if (options[2] != L"-") {
            if (!ParseMoney(options[2], minimum)) {
                return InvalidCommand(output);
            }
            query.minimum_amount = minimum;
        }
        if (options[3] != L"-") {
            if (!ParseMoney(options[3], maximum)) {
                return InvalidCommand(output);
            }
            query.maximum_amount = maximum;
        }

        if (options[4] == L"today") {
            query.date_preset = DatePreset::Today;
        } else if (options[4] == L"yesterday") {
            query.date_preset = DatePreset::Yesterday;
        } else if (options[4] == L"3days") {
            query.date_preset = DatePreset::ThreeDaysAgo;
        } else if (options[4] == L"1month") {
            query.date_preset = DatePreset::LastMonth;
        } else if (options[4] == L"2months") {
            query.date_preset = DatePreset::LastTwoMonths;
        } else if (options[4] != L"-") {
            return InvalidCommand(output);
        }

        if (options[5] != L"-") {
            query.payer = options[5];
        }
        if (options[6] == L"time") {
            query.sort_field = SortField::Time;
        } else if (options[6] == L"amount") {
            query.sort_field = SortField::Amount;
        } else {
            return InvalidCommand(output);
        }
        if (options[7] == L"asc") {
            query.sort_direction = SortDirection::Ascending;
        } else if (options[7] == L"desc") {
            query.sort_direction = SortDirection::Descending;
        } else {
            return InvalidCommand(output);
        }

        auto result = service_->QueryEntries(actor_id, book_id, query);
        if (!PrintStatus(result.status, output)) {
            return ConsoleCommandResult::Failure;
        }
        PrintEntries(*result.value, output);
        return ConsoleCommandResult::Success;
    }

    return InvalidCommand(output);
}

} // namespace ledger::ui
