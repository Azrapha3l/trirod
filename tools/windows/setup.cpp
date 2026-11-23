// Windows install front end. Setup and extraction run in Trirod-Launcher.exe; Trirod.exe is the game-only runtime.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <cwctype>

namespace fs = std::filesystem;

static constexpr int SRC_EDIT = 101, SRC_BROWSE = 102, OUT_EDIT = 103, OUT_BROWSE = 104, INSTALL = 105, STATUS = 106;
static HWND g_window, g_src, g_out, g_status, g_install;

static std::wstring text(HWND control) {
    int n = GetWindowTextLengthW(control);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(control, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}

static void status(const wchar_t *s) { SetWindowTextW(g_status, s); }

static std::wstring normalized(const fs::path &path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    if (ec) p = fs::absolute(path, ec).lexically_normal();
    std::wstring s = p.wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

static bool path_contains(const std::wstring &child, const std::wstring &parent) {
    if (child.size() < parent.size()) return false;
    if (CompareStringOrdinal(child.data(), (int)parent.size(), parent.data(), (int)parent.size(), TRUE) != CSTR_EQUAL)
        return false;
    return child.size() == parent.size() || parent.back() == L'\\' || parent.back() == L'/' ||
           child[parent.size()] == L'\\' || child[parent.size()] == L'/';
}

static std::wstring select_folder(const wchar_t *title, const std::wstring &initial) {
    IFileDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    if (!initial.empty()) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dialog->SetFolder(item);
            item->Release();
        }
    }
    std::wstring result;
    if (SUCCEEDED(dialog->Show(g_window))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

static std::wstring select_file(const wchar_t *title, const std::wstring &initial_dir, const wchar_t *filter_name,
                                const wchar_t *filter_pattern) {
    IFileDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    COMDLG_FILTERSPEC filter{filter_name, filter_pattern};
    dialog->SetFileTypes(1, &filter);
    if (!initial_dir.empty()) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial_dir.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dialog->SetFolder(item);
            item->Release();
        }
    }
    std::wstring result;
    if (SUCCEEDED(dialog->Show(g_window))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

static std::wstring find_keys(const fs::path &package) {
    std::error_code ec;
    fs::path beside = package.parent_path() / L"prod.keys";
    if (fs::is_regular_file(beside, ec)) return beside.wstring();
    PWSTR profile = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) {
        fs::path standard = fs::path(profile) / L".switch" / L"prod.keys";
        CoTaskMemFree(profile);
        if (fs::is_regular_file(standard, ec)) return standard.wstring();
    }
    return {};
}

static bool copy_payload(const fs::path &payload, const fs::path &output, std::wstring &error) {
    std::error_code ec;
    if (!fs::is_directory(payload, ec)) { error = L"The Setup package is incomplete: its payload folder is missing."; return false; }
    fs::create_directories(output, ec);
    if (ec) { error = L"The selected output folder can't be created."; return false; }
    for (fs::recursive_directory_iterator it(payload, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        fs::path relative = fs::relative(it->path(), payload, ec);
        if (ec) break;
        fs::path target = output / relative;
        if (it->is_directory(ec)) fs::create_directories(target, ec);
        else if (it->is_regular_file(ec)) {
            fs::create_directories(target.parent_path(), ec);
            if (!ec) fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) break;
    }
    if (ec) { error = L"Couldn't copy the runtime files to the selected output folder."; return false; }
    return true;
}

static bool launch_setup() {
    std::wstring source = text(g_src), output = text(g_out), error, keys;
    if (output.empty()) { MessageBoxW(g_window, L"Choose the output folder.", L"EoWRecomp Setup", MB_ICONINFORMATION); return false; }
    std::error_code ec;
    if (!source.empty()) {
        fs::path package(source);
        std::wstring ext = package.extension().wstring();
        for (wchar_t &ch : ext) ch = (wchar_t)towlower(ch);
        if (ext != L".nsp" || !fs::is_regular_file(package, ec)) {
            MessageBoxW(g_window, L"Select the base-game NSP file.", L"EoWRecomp Setup", MB_ICONERROR);
            return false;
        }
        keys = find_keys(package);
        if (keys.empty()) {
            keys = select_file(L"Select your prod.keys file", package.parent_path().wstring(), L"prod.keys", L"prod.keys");
            if (keys.empty()) return false;
        }
    }
    if (source.empty() && (!fs::is_regular_file(fs::path(output) / L"extracted" / L"exefs" / L"main", ec) ||
                           !fs::is_regular_file(fs::path(output) / L"extracted" / L"romfs.bin", ec))) {
        MessageBoxW(g_window, L"Choose the folder containing your game package, or select an output folder that already has extracted ExeFS and RomFS files.",
                    L"EoWRecomp Setup", MB_ICONINFORMATION);
        return false;
    }
    fs::path app = fs::path(output) / L"Trirod-Launcher.exe";
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!n || n >= MAX_PATH) { MessageBoxW(g_window, L"Couldn't locate the Setup package.", L"EoWRecomp Setup", MB_ICONERROR); return false; }
    fs::path package_dir = fs::path(self).parent_path();
    fs::path payload = package_dir / L"payload";
    std::wstring source_norm = source.empty() ? std::wstring() : normalized(source), output_norm = normalized(output);
    std::wstring package_norm = normalized(package_dir);
    if ((!source_norm.empty() && (path_contains(source_norm, output_norm) || path_contains(output_norm, source_norm))) ||
        path_contains(output_norm, package_norm) || path_contains(package_norm, output_norm)) {
        MessageBoxW(g_window, L"Choose an output folder separate from both the game files folder and the Setup package.",
                    L"EoWRecomp Setup", MB_ICONWARNING);
        return false;
    }
    status(L"Copying the EoWRecomp runtime to the output folder...");
    EnableWindow(g_install, FALSE);
    UpdateWindow(g_window);
    if (!copy_payload(payload, output, error)) {
        EnableWindow(g_install, TRUE);
        status(L"Setup couldn't copy the runtime files.");
        MessageBoxW(g_window, error.c_str(), L"EoWRecomp Setup", MB_ICONERROR);
        return false;
    }
    if (!fs::is_regular_file(app, ec)) {
        EnableWindow(g_install, TRUE);
        status(L"The setup launcher is missing from the copied payload.");
        MessageBoxW(g_window, L"The payload doesn't contain Trirod-Launcher.exe.", L"Trirod Installer", MB_ICONERROR);
        return false;
    }

    SetEnvironmentVariableW(L"EOWRECOMP_DATA_ROOT", output.c_str());
    if (source.empty()) {
        SetEnvironmentVariableW(L"EOWRECOMP_LAUNCHER_SETUP", nullptr);
        SetEnvironmentVariableW(L"EOWRECOMP_LAUNCHER_PACKAGE", nullptr);
        SetEnvironmentVariableW(L"EOWRECOMP_LAUNCHER_KEYS", nullptr);
        SetEnvironmentVariableW(L"EOWRECOMP_LAUNCHER_RECOMPILE", L"1");
    } else {
        SetEnvironmentVariableW(L"EOWRECOMP_LAUNCHER_RECOMPILE", nullptr);
        SetEnvironmentVariableW(L"EOWRECOMP_LAUNCHER_SETUP", nullptr);
        SetEnvironmentVariableW(L"EOWRECOMP_LAUNCHER_PACKAGE", source.c_str());
        SetEnvironmentVariableW(L"EOWRECOMP_LAUNCHER_KEYS", keys.c_str());
    }
    std::wstring command = L"\"" + app.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, output.c_str(), &si, &pi)) {
        EnableWindow(g_install, TRUE);
        status(L"Couldn't start the EoWRecomp setup launcher.");
        MessageBoxW(g_window, L"The runtime files were copied, but Windows couldn't start Trirod-Launcher.exe.", L"Trirod Installer", MB_ICONERROR);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    status(L"The launcher is running. It will prepare the game and start Trirod.exe when you choose Play.");
    return true;
}

static HWND label(HWND parent, const wchar_t *value, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", value, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, nullptr, nullptr);
}
static HWND button(HWND parent, const wchar_t *value, int id, int x, int y, int w, int h) {
    return CreateWindowW(L"BUTTON", value, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, w, h, parent,
                         (HMENU)(INT_PTR)id, nullptr, nullptr);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        label(hwnd, L"Game NSP file (leave blank to recompile from the selected output folder)", 24, 22, 710, 24);
        g_src = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY | WS_BORDER,
                              24, 50, 570, 30, hwnd, (HMENU)(INT_PTR)SRC_EDIT, nullptr, nullptr);
        button(hwnd, L"Browse...", SRC_BROWSE, 606, 50, 120, 30);
        label(hwnd, L"Output folder (Setup, extracted files, and compiled game EXE)", 24, 100, 650, 24);
        g_out = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER,
                              24, 128, 570, 30, hwnd, (HMENU)(INT_PTR)OUT_EDIT, nullptr, nullptr);
        button(hwnd, L"Browse...", OUT_BROWSE, 606, 128, 120, 30);
        label(hwnd, L"Setup reads the NSP once; extracted files and the standalone game EXE go to the output folder. prod.keys is auto-found beside the NSP or in your .switch folder.",
              24, 181, 700, 42);
        g_install = button(hwnd, L"Set up / recompile", INSTALL, 24, 241, 180, 38);
        g_status = label(hwnd, L"Choose the NSP and output folder to begin.", 24, 294, 700, 48);
        PWSTR profile = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) {
            std::wstring initial = std::wstring(profile) + L"\\Games\\EoWRecomp";
            SetWindowTextW(g_out, initial.c_str());
            CoTaskMemFree(profile);
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == SRC_BROWSE || id == OUT_BROWSE) {
            HWND field = id == SRC_BROWSE ? g_src : g_out;
            std::wstring selected = id == SRC_BROWSE
                                        ? select_file(L"Select the Echoes of Wisdom base-game NSP", text(field).empty() ? L"" : fs::path(text(field)).parent_path().wstring(),
                                                      L"Nintendo Switch package (*.nsp)", L"*.nsp")
                                        : select_folder(L"Select the EoWRecomp install folder", text(field));
            if (!selected.empty()) SetWindowTextW(field, selected.c_str());
            return 0;
        }
        if (id == INSTALL) { launch_setup(); return 0; }
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"EoWRecompSetupWindow";
    RegisterClassW(&wc);
    g_window = CreateWindowW(wc.lpszClassName, L"EoWRecomp Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 770, 390, nullptr, nullptr, instance, nullptr);
    if (!g_window) { CoUninitialize(); return 1; }
    ShowWindow(g_window, show);
    UpdateWindow(g_window);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize();
    return (int)msg.wParam;
}
