#include "Win32FileDialog.h"
#include "ProjectPaths.h"
#include "OperationsDialog.h"

#include "ledger/service/ILedgerService.h"

#include <windows.h>

#include <cerrno>
#include <cwchar>
#include <memory>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"SharedLedgerSkeletonWindow";
constexpr wchar_t kCreateBookDialogClass[] = L"SharedLedgerCreateBookDialog";
constexpr wchar_t kDefaultDataFile[] = L"data/ledger.json";
constexpr int kOpenButton = 1001;
constexpr int kSaveButton = 1002;
constexpr int kStatusLabel = 1003;
constexpr int kCreateBookButton = 1004;
constexpr int kOperationsButton = 1005;
constexpr int kBookNameEdit = 2001;
constexpr int kCreatorIdEdit = 2002;
constexpr int kConfirmCreateButton = 2003;
constexpr int kCancelCreateButton = 2004;

struct WindowState {
    WindowState() {
        service = ledger::CreateFileLedgerService();
        startup_status = service->Open(kDefaultDataFile);
        if (startup_status.code == ledger::ErrorCode::NotFound) {
            ledger::DatabaseSnapshot snapshot;
            for (ledger::UserId id = 1; id <= 4; ++id) {
                snapshot.users.push_back(ledger::User{
                    id,
                    L"user" + std::to_wstring(id),
                    L"用户" + std::to_wstring(id),
                    {}});
            }
            service = ledger::CreateLedgerService(std::move(snapshot));
            startup_status = service->SaveAs(kDefaultDataFile);
        }
        if (startup_status.code == ledger::ErrorCode::Ok) {
            current_file = kDefaultDataFile;
        }
    }

    std::unique_ptr<ledger::ILedgerService> service;
    ledger::ui::Win32FileDialog dialogs;
    std::filesystem::path current_file;
    ledger::Status startup_status;
};

WindowState* GetState(HWND window) {
    return reinterpret_cast<WindowState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

void SetStatus(HWND window, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(window, kStatusLabel), text.c_str());
}

std::wstring WideError(const std::string& text) {
    if (text.empty()) {
        return L"操作失败。";
    }
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (size <= 0) {
        return L"操作失败，错误说明编码无效。";
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size);
    return result;
}

void OpenFile(HWND window, WindowState& state) {
    const auto path = state.dialogs.Open(window);
    if (!path) {
        return;
    }

    const auto relative = ledger::ui::ProjectRelativeSnapshotPath(*path);
    if (!relative) {
        SetStatus(window, L"只能选择项目子目录中的快照文件");
        return;
    }
    if (!ledger::ui::SetProjectWorkingDirectory()) {
        SetStatus(window, L"无法设置项目工作目录");
        return;
    }

    const auto status = state.service->Open(*relative);
    if (status.code == ledger::ErrorCode::Ok) {
        state.current_file = *relative;
        SetStatus(window, L"已打开：" + relative->wstring());
    } else {
        SetStatus(window, L"无法打开文件：" + WideError(status.message));
    }
}

void SaveFile(HWND window, WindowState& state) {
    const auto path = state.dialogs.SaveAs(window);
    if (!path) {
        return;
    }

    const auto relative = ledger::ui::ProjectRelativeSnapshotPath(*path);
    if (!relative) {
        SetStatus(window, L"只能选择项目子目录中的快照文件");
        return;
    }
    if (!ledger::ui::SetProjectWorkingDirectory()) {
        SetStatus(window, L"无法设置项目工作目录");
        return;
    }

    const auto status = state.service->SaveAs(*relative);
    if (status.code == ledger::ErrorCode::Ok) {
        state.current_file = *relative;
        SetStatus(window, L"已保存：" + relative->wstring());
    } else {
        SetStatus(window, L"无法保存文件：" + WideError(status.message));
    }
}

struct CreateBookDialogState {
    explicit CreateBookDialogState(WindowState& app_state)
        : app(app_state) {}

    WindowState& app;
};

CreateBookDialogState* GetDialogState(HWND dialog) {
    return reinterpret_cast<CreateBookDialogState*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
}

std::wstring ControlText(HWND dialog, int control_id) {
    HWND control = GetDlgItem(dialog, control_id);
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

void CreateBookDialogControls(HWND dialog) {
    const auto instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
    CreateWindowExW(
        0, L"STATIC", L"账本名称：", WS_CHILD | WS_VISIBLE,
        20, 22, 90, 24, dialog, nullptr, instance, nullptr);
    CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        115,
        18,
        245,
        26,
        dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBookNameEdit)),
        instance,
        nullptr);
    CreateWindowExW(
        0, L"STATIC", L"创建者编号：", WS_CHILD | WS_VISIBLE,
        20, 62, 90, 24, dialog, nullptr, instance, nullptr);
    CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"1",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
        115,
        58,
        120,
        26,
        dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCreatorIdEdit)),
        instance,
        nullptr);
    CreateWindowExW(
        0,
        L"BUTTON",
        L"创建",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        115,
        110,
        100,
        32,
        dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConfirmCreateButton)),
        instance,
        nullptr);
    CreateWindowExW(
        0,
        L"BUTTON",
        L"取消",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        235,
        110,
        100,
        32,
        dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelCreateButton)),
        instance,
        nullptr);
    SetFocus(GetDlgItem(dialog, kBookNameEdit));
}

void ConfirmCreateBook(HWND dialog, CreateBookDialogState& state) {
    const auto name = ControlText(dialog, kBookNameEdit);
    const auto id_text = ControlText(dialog, kCreatorIdEdit);
    wchar_t* end = nullptr;
    errno = 0;
    const auto creator_id = std::wcstoll(id_text.c_str(), &end, 10);
    if (errno == ERANGE || end == id_text.c_str() || end == nullptr ||
        *end != L'\0' || creator_id <= 0) {
        MessageBoxW(
            dialog,
            L"创建者编号必须为正整数。",
            L"输入无效",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (!ledger::ui::SetProjectWorkingDirectory()) {
        MessageBoxW(
            dialog,
            L"无法设置项目工作目录。",
            L"新建账本失败",
            MB_OK | MB_ICONERROR);
        return;
    }
    auto result = state.app.service->CreateBook(creator_id, name);
    if (result.status.code != ledger::ErrorCode::Ok) {
        MessageBoxW(
            dialog,
            WideError(result.status.message).c_str(),
            L"新建账本失败",
            MB_OK | MB_ICONERROR);
        return;
    }

    const auto message = L"账本创建成功，账本编号：" +
                         std::to_wstring(result.value->id);
    SetStatus(GetAncestor(dialog, GA_ROOTOWNER), message);
    MessageBoxW(dialog, message.c_str(), L"新建账本", MB_OK | MB_ICONINFORMATION);
    DestroyWindow(dialog);
}

LRESULT CALLBACK CreateBookDialogProcedure(
    HWND dialog,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(
            dialog,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    switch (message) {
    case WM_CREATE:
        CreateBookDialogControls(dialog);
        return 0;
    case WM_COMMAND:
        if (auto* state = GetDialogState(dialog)) {
            if (LOWORD(w_param) == kConfirmCreateButton) {
                ConfirmCreateBook(dialog, *state);
                return 0;
            }
            if (LOWORD(w_param) == kCancelCreateButton) {
                DestroyWindow(dialog);
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(dialog);
        return 0;
    case WM_NCDESTROY:
        delete GetDialogState(dialog);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(dialog, message, w_param, l_param);
}

void ShowCreateBookDialog(HWND owner, WindowState& state) {
    auto dialog_state = std::make_unique<CreateBookDialogState>(state);
    const auto instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));

    EnableWindow(owner, FALSE);
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kCreateBookDialogClass,
        L"新建账本",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        400,
        200,
        owner,
        nullptr,
        instance,
        dialog_state.get());
    if (dialog == nullptr) {
        EnableWindow(owner, TRUE);
        return;
    }
    dialog_state.release();

    // Keep input in this dialog until it is confirmed or cancelled.
    MSG message{};
    while (IsWindow(dialog)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (result == -1) {
            break;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
}

void CreateControls(HWND window) {
    const auto instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(window, GWLP_HINSTANCE));

    CreateWindowExW(
        0,
        L"STATIC",
        L"多人共享记账系统。数据文件：data/ledger.json",
        WS_CHILD | WS_VISIBLE,
        20,
        20,
        490,
        40,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabel)),
        instance,
        nullptr);
    CreateWindowExW(
        0,
        L"BUTTON",
        L"打开账本...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20,
        85,
        150,
        32,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenButton)),
        instance,
        nullptr);
    CreateWindowExW(
        0,
        L"BUTTON",
        L"账本另存为...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        190,
        85,
        150,
        32,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButton)),
        instance,
        nullptr);
    CreateWindowExW(
        0,
        L"BUTTON",
        L"新建账本",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        360,
        85,
        150,
        32,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCreateBookButton)),
        instance,
        nullptr);
    CreateWindowExW(
        0,
        L"BUTTON",
        L"账本操作",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20,
        130,
        150,
        32,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOperationsButton)),
        instance,
        nullptr);
}

LRESULT CALLBACK WindowProcedure(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    switch (message) {
    case WM_CREATE:
        CreateControls(window);
        if (const auto* state = GetState(window);
            state && state->startup_status.code != ledger::ErrorCode::Ok) {
            SetStatus(window, L"数据文件加载失败：" + WideError(state->startup_status.message));
        }
        return 0;
    case WM_COMMAND:
        if (auto* state = GetState(window)) {
            switch (LOWORD(w_param)) {
            case kOpenButton:
                OpenFile(window, *state);
                return 0;
            case kSaveButton:
                SaveFile(window, *state);
                return 0;
            case kCreateBookButton:
                ShowCreateBookDialog(window, *state);
                return 0;
            case kOperationsButton:
                ledger::ui::ShowOperationsDialog(window, *state->service);
                return 0;
            default:
                break;
            }
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        delete GetState(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int show_command) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    window_class.lpfnWndProc = WindowProcedure;
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        return 1;
    }

    window_class.lpfnWndProc = CreateBookDialogProcedure;
    window_class.lpszClassName = kCreateBookDialogClass;
    if (RegisterClassExW(&window_class) == 0) {
        return 1;
    }

    ledger::ui::SetProjectWorkingDirectory();

    auto state = std::make_unique<WindowState>();
    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        L"多人共享记账系统",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        550,
        240,
        nullptr,
        nullptr,
        instance,
        state.get());
    if (window == nullptr) {
        return 1;
    }
    state.release();

    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
