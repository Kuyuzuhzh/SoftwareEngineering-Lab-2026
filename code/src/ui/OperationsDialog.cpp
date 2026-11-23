#include "OperationsDialog.h"

#include "ProjectPaths.h"

#include <cerrno>
#include <chrono>
#include <cwchar>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace ledger::ui {
namespace {

constexpr wchar_t kDialogClass[] = L"SharedLedgerOperationsDialog";
constexpr int kActorEdit = 3001;
constexpr int kBookEdit = 3002;
constexpr int kTargetEdit = 3003;
constexpr int kAmountEdit = 3004;
constexpr int kTypeEdit = 3005;
constexpr int kDateEdit = 3006;
constexpr int kPayerEdit = 3007;
constexpr int kNoteEdit = 3008;
constexpr int kAttachmentsEdit = 3009;
constexpr int kStateEdit = 3010;
constexpr int kMinimumEdit = 3011;
constexpr int kMaximumEdit = 3012;
constexpr int kDatePresetEdit = 3013;
constexpr int kSortFieldEdit = 3014;
constexpr int kSortDirectionEdit = 3015;
constexpr int kOutputEdit = 3016;
constexpr int kInviteButton = 3101;
constexpr int kAuditorOnButton = 3102;
constexpr int kAuditorOffButton = 3103;
constexpr int kAddButton = 3104;
constexpr int kEditButton = 3105;
constexpr int kSubmitButton = 3106;
constexpr int kAcceptButton = 3107;
constexpr int kRejectButton = 3108;
constexpr int kDiscardButton = 3109;
constexpr int kQueryButton = 3110;
constexpr int kOwnButton = 3111;
constexpr int kCloseButton = 3112;

struct DialogState {
    explicit DialogState(ILedgerService& service) : service(service) {}
    ILedgerService& service;
};

DialogState* State(HWND dialog) {
    return reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
}

std::wstring Text(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

void Output(HWND dialog, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(dialog, kOutputEdit), text.c_str());
}

std::wstring WideError(const std::string& text) {
    if (text.empty()) return L"操作失败";
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"操作失败，错误说明编码无效";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

bool ShowStatus(HWND dialog, const Status& status) {
    if (status.code == ErrorCode::Ok) {
        Output(dialog, L"操作成功");
        return true;
    }
    Output(dialog, L"操作失败：" + WideError(status.message));
    return false;
}

bool Id(HWND dialog, int control, std::int64_t& value) {
    const auto text = Text(dialog, control);
    wchar_t* end = nullptr;
    errno = 0;
    value = std::wcstoll(text.c_str(), &end, 10);
    return errno != ERANGE && end != text.c_str() && end != nullptr &&
           *end == L'\0' && value > 0;
}

bool Ids(HWND dialog, UserId& actor, BookId& book, EntryId& target) {
    if (!Id(dialog, kActorEdit, actor) || !Id(dialog, kBookEdit, book) ||
        !Id(dialog, kTargetEdit, target)) {
        Output(dialog, L"操作人、账本和目标编号必须为正整数");
        return false;
    }
    return true;
}

bool MoneyValue(const std::wstring& text, Money& money) {
    if (text.empty()) return false;
    const auto dot = text.find(L'.');
    if (dot != std::wstring::npos && text.find(L'.', dot + 1) != std::wstring::npos) {
        return false;
    }
    const auto whole = text.substr(0, dot);
    const auto fraction = dot == std::wstring::npos ? std::wstring{} : text.substr(dot + 1);
    if (whole.empty() || fraction.size() > 2 ||
        (dot != std::wstring::npos && fraction.empty())) {
        return false;
    }
    std::int64_t value = 0;
    for (const wchar_t character : whole) {
        if (character < L'0' || character > L'9') return false;
        value = value * 10 + character - L'0';
        if (value > 1000000) return false;
    }
    value *= 100;
    for (const wchar_t character : fraction) {
        if (character < L'0' || character > L'9') return false;
    }
    if (!fraction.empty()) value += (fraction[0] - L'0') * 10;
    if (fraction.size() == 2) value += fraction[1] - L'0';
    money.cents = value;
    return true;
}

TimePoint DateValue(const std::wstring& text, bool& valid) {
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t character : text) {
        if (character > 127) {
            valid = false;
            return {};
        }
        narrow.push_back(static_cast<char>(character));
    }
    std::tm local{};
    std::istringstream input{narrow};
    input >> std::get_time(&local, "%Y-%m-%d");
    valid = !input.fail() && input.peek() == std::char_traits<char>::eof() &&
            text.size() == 10;
    if (!valid) return {};
    const auto utc = _mkgmtime64(&local);
    valid = utc != -1;
    return valid ? std::chrono::system_clock::from_time_t(static_cast<time_t>(utc)) -
                       std::chrono::hours{8}
                 : TimePoint{};
}

std::wstring Trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool EntryValue(HWND dialog, Entry& entry, bool include_id) {
    UserId actor{};
    BookId book{};
    EntryId target{};
    if (!Id(dialog, kActorEdit, actor) || !Id(dialog, kBookEdit, book) ||
        (include_id && !Id(dialog, kTargetEdit, target))) {
        Output(dialog, L"操作人、账本和条目编号必须为正整数");
        return false;
    }
    entry.id = include_id ? target : 0;
    entry.book_id = book;
    entry.submitter_id = actor;
    if (!MoneyValue(Text(dialog, kAmountEdit), entry.amount)) {
        Output(dialog, L"金额格式无效");
        return false;
    }
    const auto type = Text(dialog, kTypeEdit);
    if (type == L"income") entry.type = EntryType::Income;
    else if (type == L"expenditure") entry.type = EntryType::expenditure;
    else {
        Output(dialog, L"收支类型必须为 income 或 expenditure");
        return false;
    }
    bool valid_date = false;
    entry.occurred_at = DateValue(Text(dialog, kDateEdit), valid_date);
    if (!valid_date) {
        Output(dialog, L"发生日期必须为 YYYY-MM-DD");
        return false;
    }
    entry.payer = Text(dialog, kPayerEdit);
    entry.note = Text(dialog, kNoteEdit);
    entry.attachments.clear();
    std::wstring attachments = Text(dialog, kAttachmentsEdit);
    std::size_t start = 0;
    while (start <= attachments.size()) {
        const auto end = attachments.find(L';', start);
        auto path = Trim(attachments.substr(start, end - start));
        if (!path.empty()) entry.attachments.emplace_back(path);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}

const wchar_t* StateText(EntryState state) {
    switch (state) {
    case EntryState::Draft: return L"未发布";
    case EntryState::Waiting_review: return L"待审核";
    case EntryState::Rejected: return L"已驳回";
    case EntryState::Accepted: return L"已通过";
    case EntryState::Deleted: return L"已作废";
    }
    return L"未知";
}

std::wstring EntryText(const std::vector<Entry>& entries) {
    std::wostringstream output;
    output << L"数量=" << entries.size() << L"\r\n";
    for (const auto& entry : entries) {
        output << L"编号=" << entry.id << L" 状态=" << StateText(entry.state)
               << L" 类型=" << (entry.type == EntryType::Income ? L"收入" : L"支出")
               << L" 金额=" << entry.amount.cents / 100 << L'.'
               << std::setw(2) << std::setfill(L'0') << entry.amount.cents % 100
               << std::setfill(L' ') << L" 付款人=" << entry.payer
               << L" 备注=" << entry.note << L"\r\n";
    }
    return output.str();
}

void Invite(HWND dialog, DialogState& state, bool auditor, bool enabled) {
    UserId actor{};
    BookId book{};
    EntryId target{};
    if (!Ids(dialog, actor, book, target)) return;
    ShowStatus(
        dialog,
        auditor ? state.service.SetAuditor(actor, book, target, enabled)
                : state.service.InviteMember(actor, book, target));
}

void AddOrEdit(HWND dialog, DialogState& state, bool edit) {
    Entry entry;
    if (!EntryValue(dialog, entry, edit)) return;
    if (edit) {
        ShowStatus(dialog, state.service.EditEntry(entry.submitter_id, entry));
        return;
    }
    auto result = state.service.AddEntry(entry.submitter_id, entry);
    if (ShowStatus(dialog, result.status)) {
        Output(dialog, L"草稿创建成功，条目编号=" + std::to_wstring(result.value->id));
    }
}

void Submit(HWND dialog, DialogState& state) {
    UserId actor{};
    BookId book{};
    EntryId target{};
    if (!Ids(dialog, actor, book, target)) return;
    auto status = state.service.SubmitEntry(actor, book, target, false);
    if (status.code == ErrorCode::ConfirmationRequired) {
        const int answer = MessageBoxW(
            dialog,
            L"疑似重复记账，是否确认提交？",
            L"重复提交确认",
            MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            Output(dialog, L"已取消提交");
            return;
        }
        status = state.service.SubmitEntry(actor, book, target, true);
    }
    ShowStatus(dialog, status);
}

void Review(HWND dialog, DialogState& state, EntryState decision) {
    UserId actor{};
    BookId book{};
    EntryId target{};
    if (!Ids(dialog, actor, book, target)) return;
    ShowStatus(dialog, state.service.ReviewEntry(actor, book, target, decision));
}

void Discard(HWND dialog, DialogState& state) {
    UserId actor{};
    BookId book{};
    EntryId target{};
    if (!Ids(dialog, actor, book, target)) return;
    ShowStatus(dialog, state.service.DiscardEntry(actor, book, target));
}

void Query(HWND dialog, DialogState& state) {
    UserId actor{};
    BookId book{};
    if (!Id(dialog, kActorEdit, actor) || !Id(dialog, kBookEdit, book)) {
        Output(dialog, L"操作人和账本编号必须为正整数");
        return;
    }
    EntryQuery query;
    const auto state_text = Text(dialog, kStateEdit);
    if (state_text == L"accepted") query.state = EntryState::Accepted;
    else if (state_text == L"waiting") query.state = EntryState::Waiting_review;
    else if (state_text != L"-") {
        Output(dialog, L"查询状态必须为 -、accepted 或 waiting");
        return;
    }
    const auto type = Text(dialog, kTypeEdit);
    if (type == L"income") query.type = EntryType::Income;
    else if (type == L"expenditure") query.type = EntryType::expenditure;
    else if (type != L"-") {
        Output(dialog, L"收支类型必须为 -、income 或 expenditure");
        return;
    }
    Money value{};
    const auto minimum = Text(dialog, kMinimumEdit);
    const auto maximum = Text(dialog, kMaximumEdit);
    if (minimum != L"-") {
        if (!MoneyValue(minimum, value)) { Output(dialog, L"最低金额格式无效"); return; }
        query.minimum_amount = value;
    }
    if (maximum != L"-") {
        if (!MoneyValue(maximum, value)) { Output(dialog, L"最高金额格式无效"); return; }
        query.maximum_amount = value;
    }
    const auto preset = Text(dialog, kDatePresetEdit);
    if (preset == L"today") query.date_preset = DatePreset::Today;
    else if (preset == L"yesterday") query.date_preset = DatePreset::Yesterday;
    else if (preset == L"3days") query.date_preset = DatePreset::ThreeDaysAgo;
    else if (preset == L"1month") query.date_preset = DatePreset::LastMonth;
    else if (preset == L"2months") query.date_preset = DatePreset::LastTwoMonths;
    else if (preset != L"-") { Output(dialog, L"日期筛选项无效"); return; }
    const auto payer = Text(dialog, kPayerEdit);
    if (!payer.empty() && payer != L"-") query.payer = payer;
    const auto sort = Text(dialog, kSortFieldEdit);
    if (sort == L"time") query.sort_field = SortField::Time;
    else if (sort == L"amount") query.sort_field = SortField::Amount;
    else { Output(dialog, L"排序字段必须为 time 或 amount"); return; }
    const auto direction = Text(dialog, kSortDirectionEdit);
    if (direction == L"asc") query.sort_direction = SortDirection::Ascending;
    else if (direction == L"desc") query.sort_direction = SortDirection::Descending;
    else { Output(dialog, L"排序方向必须为 asc 或 desc"); return; }

    auto result = state.service.QueryEntries(actor, book, query);
    if (result.status.code == ErrorCode::Ok) Output(dialog, EntryText(*result.value));
    else ShowStatus(dialog, result.status);
}

void Own(HWND dialog, DialogState& state) {
    UserId actor{};
    BookId book{};
    if (!Id(dialog, kActorEdit, actor) || !Id(dialog, kBookEdit, book)) {
        Output(dialog, L"操作人和账本编号必须为正整数");
        return;
    }
    auto result = state.service.ListOwnEntries(actor, book);
    if (result.status.code == ErrorCode::Ok) Output(dialog, EntryText(*result.value));
    else ShowStatus(dialog, result.status);
}

HWND Label(HWND dialog, const wchar_t* text, int x, int y, int width, int height = 22) {
    return CreateWindowExW(
        0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
        x, y, width, height, dialog, nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)), nullptr);
}

HWND Edit(HWND dialog, int id, const wchar_t* value, int y) {
    return CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", value,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        285, y, 475, 24, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)), nullptr);
}

void Button(HWND dialog, int id, const wchar_t* text, int x, int y) {
    CreateWindowExW(
        0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, 175, 28, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)), nullptr);
}

std::wstring CurrentDate() {
    const auto now = std::chrono::system_clock::now() + std::chrono::hours{8};
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    gmtime_s(&local, &time);
    wchar_t buffer[11]{};
    swprintf_s(buffer, L"%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buffer;
}

void Controls(HWND dialog) {
    const auto current_date = CurrentDate();
    const std::vector<std::pair<const wchar_t*, const wchar_t*>> fields{
        {L"操作人编号", L"1"}, {L"账本编号", L"1"}, {L"用户/条目编号", L"1"},
        {L"金额", L"10.00"}, {L"收支类型(income/expenditure/-)", L"expenditure"},
        {L"发生日期(YYYY-MM-DD)", current_date.c_str()}, {L"付款人(-表示不筛选)", L""},
        {L"备注", L""}, {L"附件路径(分号分隔)", L""},
        {L"查询状态(-/accepted/waiting)", L"-"}, {L"最低金额(-)", L"-"},
        {L"最高金额(-)", L"-"}, {L"日期筛选(-/today/yesterday/3days/\r\n1month/2months)", L"-"},
        {L"排序字段(time/amount)", L"time"}, {L"排序方向(asc/desc)", L"desc"}};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const int extra_height = index > 12 ? 24 : 0;
        const int y = 12 + static_cast<int>(index) * 28 + extra_height;
        Label(dialog, fields[index].first, 20, y, 255, index == 12 ? 44 : 22);
        Edit(dialog, kActorEdit + static_cast<int>(index), fields[index].second, y - 3);
    }

    const std::vector<std::pair<int, const wchar_t*>> buttons{
        {kInviteButton, L"邀请成员"}, {kAuditorOnButton, L"设为审核员"},
        {kAuditorOffButton, L"取消审核员"}, {kAddButton, L"新建草稿"},
        {kEditButton, L"保存编辑"}, {kSubmitButton, L"提交条目"},
        {kAcceptButton, L"审核通过"}, {kRejectButton, L"驳回"},
        {kDiscardButton, L"作废待审核/已通过"}, {kQueryButton, L"组合查询"},
        {kOwnButton, L"本人条目"}, {kCloseButton, L"关闭"}};
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        Button(
            dialog,
            buttons[index].first,
            buttons[index].second,
            20 + static_cast<int>(index % 4) * 190,
            462 + static_cast<int>(index / 4) * 34);
    }
    CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"操作结果显示在此处",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        20,
        572,
        740,
        105,
        dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutputEdit)),
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)),
        nullptr);
}

LRESULT CALLBACK Procedure(HWND dialog, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    switch (message) {
    case WM_CREATE:
        Controls(dialog);
        return 0;
    case WM_COMMAND:
        if (auto* state = State(dialog)) {
            switch (LOWORD(w_param)) {
            case kInviteButton: Invite(dialog, *state, false, false); return 0;
            case kAuditorOnButton: Invite(dialog, *state, true, true); return 0;
            case kAuditorOffButton: Invite(dialog, *state, true, false); return 0;
            case kAddButton: AddOrEdit(dialog, *state, false); return 0;
            case kEditButton: AddOrEdit(dialog, *state, true); return 0;
            case kSubmitButton: Submit(dialog, *state); return 0;
            case kAcceptButton: Review(dialog, *state, EntryState::Accepted); return 0;
            case kRejectButton: Review(dialog, *state, EntryState::Rejected); return 0;
            case kDiscardButton: Discard(dialog, *state); return 0;
            case kQueryButton: Query(dialog, *state); return 0;
            case kOwnButton: Own(dialog, *state); return 0;
            case kCloseButton: DestroyWindow(dialog); return 0;
            default: break;
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(dialog);
        return 0;
    case WM_NCDESTROY:
        delete State(dialog);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(dialog, message, w_param, l_param);
}

} // namespace

void ShowOperationsDialog(HWND owner, ILedgerService& service) {
    SetProjectWorkingDirectory();
    const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    WNDCLASSEXW window_class{};
    if (GetClassInfoExW(instance, kDialogClass, &window_class) == 0) {
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = Procedure;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kDialogClass;
        RegisterClassExW(&window_class);
    }

    auto state = std::make_unique<DialogState>(service);
    EnableWindow(owner, FALSE);
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kDialogClass,
        L"账本操作",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        800,
        734,
        owner,
        nullptr,
        instance,
        state.get());
    if (dialog == nullptr) {
        EnableWindow(owner, TRUE);
        return;
    }
    state.release();

    MSG message{};
    while (IsWindow(dialog)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (result == -1) break;
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
}

} // namespace ledger::ui
