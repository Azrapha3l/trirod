// Trirod-Launcher.exe (Windows): the light setup, update, build and Play window (docs/RELEASE-WORKFLOW.md).
//
// It owns configuration (where the player's game files are), the update check against the project's GitHub
// Releases, orchestration of the external tools and the display of their progress, and starting the game. It does
// not parse game files, translate code or shaders, or touch the GPU: those are the tools' work -
//   trbuild.exe files / code      (runtime/tools/trbuild.c: game files, trgen, zig cc, links)
//   Trirod-ShaderPrep.exe         (runtime/tools/shaderprep.c: headless Vulkan shader / pipeline preparation)
//   Trirod.exe                    (the game, linked by trbuild code)
// The window draws with SDL's 2D renderer (Direct3D 11/12, OpenGL or software; never Vulkan) and Dear ImGui. Tools
// talk to it with the line protocol of runtime/tools/protocol.h (docs/TOOL-PROTOCOL.md).
//
//   Trirod-Launcher.exe [--dev] [--data <folder>]
//     --dev: development mode. No update check or download; the tools beside this executable (a local package from
//            setup/build.ps1) are used as they are; Recompile is always offered; nothing is ever uploaded.
//     --data: where game files, the built game, saves and caches live (default: this executable's folder).
//     --recompile: start Recompile as soon as the chosen game files pass their check (scripted rebuilds).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#ifndef TRIROD_VERSION
#define TRIROD_VERSION "dev"
#endif
// owner/name of the GitHub repository whose Releases carry trirod-release.json (Makefile UPDATE_REPO). Empty: this
// build has no update source and never goes online.
#ifndef TRIROD_UPDATE_REPO
#define TRIROD_UPDATE_REPO ""
#endif
#define RELEASE_MANIFEST "trirod-release.json"
#define RELEASE_CHANNEL "windows-x64"

// ================================================================== small helpers
static std::wstring widen(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, NULL, NULL);
    return s;
}
static bool file_exists(const std::string &p) {
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dir_exists(const std::string &p) {
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool path_exists(const std::string &p) { return GetFileAttributesW(widen(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string read_file(const std::string &p) {
    FILE *f = _wfopen(widen(p).c_str(), L"rb");
    if (!f) return "";
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
// written to <path>.tmp, then renamed over: a crash never leaves half a file
static bool write_file(const std::string &p, const std::string &data) {
    std::string tmp = p + ".tmp";
    FILE *f = _wfopen(widen(tmp).c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0) ok = false;
    if (ok) ok = MoveFileExW(widen(tmp).c_str(), widen(p).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(widen(tmp).c_str());
    return ok;
}
static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}
static std::string now_iso() {
    time_t t = time(NULL);
    struct tm g;
    gmtime_s(&g, &t);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}
static std::string exe_dir() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, p, (DWORD)(sizeof p / sizeof *p));
    std::wstring w(p, n);
    size_t s = w.find_last_of(L"\\/");
    return narrow(s == std::wstring::npos ? w : w.substr(0, s));
}
static std::string full_path(const std::string &p) {
    wchar_t out[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(widen(p).c_str(), (DWORD)(sizeof out / sizeof *out), out, NULL);
    return n ? narrow(std::wstring(out, n)) : p;
}
static std::string win_error(DWORD e) {
    wchar_t *msg = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0,
                   (LPWSTR)&msg, 0, NULL);
    std::string s = msg ? trim(narrow(msg)) : "";
    if (msg) LocalFree(msg);
    return s + " (" + std::to_string(e) + ")";
}

// ================================================================== JSON (the manifests and GitHub's API answers)
struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json &operator[](const char *k) const {
        static const Json none;
        for (auto &kv : o) if (kv.first == k) return kv.second;
        return none;
    }
    std::string str(const std::string &def = "") const { return kind == Str ? s : def; }
    double num(double def = 0) const { return kind == Num ? n : def; }
};

struct JsonParser {
    const char *p, *end;
    bool ok = true;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void utf8(std::string &out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) out += (char)(0xC0 | cp >> 6), out += (char)(0x80 | (cp & 0x3F));
        else if (cp < 0x10000) out += (char)(0xE0 | cp >> 12), out += (char)(0x80 | ((cp >> 6) & 0x3F)), out += (char)(0x80 | (cp & 0x3F));
        else out += (char)(0xF0 | cp >> 18), out += (char)(0x80 | ((cp >> 12) & 0x3F)), out += (char)(0x80 | ((cp >> 6) & 0x3F)),
             out += (char)(0x80 | (cp & 0x3F));
    }
    std::string string() {
        std::string out;
        if (p >= end || *p != '"') { ok = false; return out; }
        for (p++; p < end && *p != '"'; p++) {
            if (*p != '\\') { out += *p; continue; }
            if (++p >= end) break;
            switch (*p) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (end - p < 5) { ok = false; return out; }
                unsigned cp = (unsigned)strtoul(std::string(p + 1, 4).c_str(), NULL, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 7 && p[1] == '\\' && p[2] == 'u') {
                    unsigned lo = (unsigned)strtoul(std::string(p + 3, 4).c_str(), NULL, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                utf8(out, cp);
                break;
            }
            default: out += *p;
            }
        }
        if (p >= end) ok = false;
        else p++;
        return out;
    }
    Json value(int depth = 0) {
        Json j;
        ws();
        if (p >= end || depth > 64) { ok = false; return j; }
        if (*p == '{') {
            j.kind = Json::Obj;
            p++;
            ws();
            if (p < end && *p == '}') { p++; return j; }
            while (ok) {
                ws();
                std::string k = string();
                ws();
                if (p >= end || *p != ':') { ok = false; break; }
                p++;
                j.o.emplace_back(k, value(depth + 1));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; break; }
                ok = false;
            }
        } else if (*p == '[') {
            j.kind = Json::Arr;
            p++;
            ws();
            if (p < end && *p == ']') { p++; return j; }
            while (ok) {
                j.a.push_back(value(depth + 1));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; break; }
                ok = false;
            }
        } else if (*p == '"') {
            j.kind = Json::Str;
            j.s = string();
        } else if (!strncmp(p, "true", 4)) j.kind = Json::Bool, j.b = true, p += 4;
        else if (!strncmp(p, "false", 5)) j.kind = Json::Bool, p += 5;
        else if (!strncmp(p, "null", 4)) p += 4;
        else {
            char *e;
            j.n = strtod(p, &e);
            if (e == p) ok = false;
            j.kind = Json::Num;
            p = e;
        }
        return j;
    }
};
static bool json_parse(const std::string &text, Json &out) {
    // (PowerShell's Set-Content -Encoding utf8 writes a byte-order mark)
    size_t skip = text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
                  (unsigned char)text[2] == 0xBF ? 3 : 0;
    JsonParser jp{text.data() + skip, text.data() + text.size()};
    out = jp.value();
    return jp.ok;
}
static std::string json_quote(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += (char)c;
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

// ================================================================== versions
// Release tags (runtime/ui/launcher.mm EOWRECOMP_VERSION): alpha_<n> < beta_<n> < v<major>.<minor>.<patch>. The
// order decides whether a release is newer; a tag in no known form is never "newer" (no update offered).
static bool version_key(const std::string &t, std::vector<long> &k) {
    k.clear();
    int n;
    char end;
    if (sscanf(t.c_str(), "alpha_%d%c", &n, &end) == 1) { k = {0, n}; return true; }
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1) { k = {1, n}; return true; }
    std::string v = !t.empty() && t[0] == 'v' ? t.substr(1) : t;
    int a = 0, b = 0, c = 0;
    if (sscanf(v.c_str(), "%d.%d.%d%c", &a, &b, &c, &end) == 3) { k = {2, a, b, c}; return true; }
    return false;
}
static int version_compare(const std::string &x, const std::string &y) {  // -2: not comparable
    std::vector<long> a, b;
    if (!version_key(x, a) || !version_key(y, b)) return -2;
    return a < b ? -1 : a > b ? 1 : 0;
}
static std::string version_label(const std::string &t) {
    int n;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1) return "Beta " + std::to_string(n);
    if (sscanf(t.c_str(), "alpha_%d%c", &n, &end) == 1) return "Alpha " + std::to_string(n);
    return t;
}

// ================================================================== the installation
struct Paths {
    std::string install;  // this executable's folder: the launcher, the tools, the runtime DLLs
    std::string data;     // game files, the built game, saves, caches, the launcher's own settings
    std::string trbuild() const { return install + "\\trbuild.exe"; }
    std::string shaderprep() const { return install + "\\Trirod-ShaderPrep.exe"; }
    std::string tools_json() const { return install + "\\tools\\trirod-tools.json"; }
    std::string extracted() const { return data + "\\extracted"; }
    std::string game_exe() const { return data + "\\Trirod.exe"; }
    std::string config() const { return data + "\\trirod-launcher.ini"; }
    std::string install_json() const { return data + "\\trirod-install.json"; }
    std::string logs() const { return data + "\\logs"; }
    std::string shader_report() const { return data + "\\cache\\shaderprep.json"; }
};
static Paths g_paths;
static bool g_dev;

// tools/trirod-tools.json (setup/pack_tools.ps1): which release the tools are and the build identifiers they produce
struct ToolsInfo {
    bool present = false;
    std::string version, gamelib_id, gamebuild_id, why;
};
static ToolsInfo read_tools() {
    ToolsInfo t;
    for (const std::string &p : {g_paths.trbuild(), g_paths.shaderprep(), g_paths.tools_json()})
        if (!file_exists(p)) { t.why = "missing " + p; return t; }
    Json j;
    if (!json_parse(read_file(g_paths.tools_json()), j) || j.kind != Json::Obj) { t.why = "unreadable tools manifest"; return t; }
    t.version = j["version"].str();
    t.gamelib_id = j["gamelib_id"].str();
    t.gamebuild_id = j["gamebuild_id"].str();
    if (t.version.empty() || t.gamelib_id.empty() || t.gamebuild_id.empty()) {
        t.why = "the tools manifest has no version / build identifiers (rebuild the package with setup/build.ps1)";
        return t;
    }
    t.present = true;
    return t;
}

static std::string id_file(const std::string &p) { return trim(read_file(p)); }

// Is the game built for these tools? "" = yes, else what is missing or out of date (what Recompile will fix)
static std::string build_state(const ToolsInfo &t) {
    if (!t.present) return "the build tools are missing";
    std::string main = g_paths.extracted() + "\\exefs\\main";
    if (!file_exists(main) || !file_exists(g_paths.extracted() + "\\romfs.bin")) return "the game files are not prepared";
    std::string dll = id_file(g_paths.extracted() + "\\Trirod.dll.id"), exe = id_file(g_paths.game_exe() + ".id");
    if (!file_exists(g_paths.extracted() + "\\Trirod.dll") || dll.empty()) return "the game code has not been built";
    if (!file_exists(g_paths.game_exe()) || exe.empty()) return "Trirod.exe has not been built";
    if (dll != t.gamelib_id) return "the game code was built by other tools (" + dll + ", these make " + t.gamelib_id + ")";
    if (exe != t.gamebuild_id) return "Trirod.exe was built from another runtime (" + exe + ", these make " + t.gamebuild_id + ")";
    return "";
}

// <data>/trirod-launcher.ini: the chosen game files (key=value, UTF-8)
static std::map<std::string, std::string> g_config;
static void config_load() {
    g_config.clear();
    std::string s = read_file(g_paths.config());
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        std::string line = trim(s.substr(i, e == std::string::npos ? std::string::npos : e - i));
        i = e == std::string::npos ? s.size() : e + 1;
        size_t eq = line.find('=');
        if (!line.empty() && line[0] != '#' && eq != std::string::npos) g_config[line.substr(0, eq)] = line.substr(eq + 1);
    }
}
static void config_save() {
    std::string s = "# Trirod-Launcher settings (the launcher rewrites this file)\n";
    for (auto &kv : g_config) s += kv.first + "=" + kv.second + "\n";
    write_file(g_paths.config(), s);
}

// ================================================================== shared state (UI thread <-> worker threads)
enum class StepState { Pending, Running, Done, Failed, Warned, Skipped };
struct Step {
    std::string id, title;
    StepState state = StepState::Pending;
    double progress = 0;
    std::string text, tool, note;  // current activity, the command running, the outcome
    std::vector<std::string> info;
};
struct LogLine { int kind; std::string text; };  // 0 output, 1 event, 2 error, 3 launcher
static std::mutex g_mu;
static std::vector<Step> g_steps;
static std::vector<LogLine> g_log;
static FILE *g_log_file;
static std::atomic<bool> g_busy{false}, g_cancel{false};
static std::string g_status, g_error;       // the headline under the title; the last failure
static bool g_build_ok_once;                 // a build finished in this session
static HANDLE g_job;                         // the running tool and everything it started (Cancel ends them)

static void log_line(int kind, const std::string &t) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_log.size() > 20000) g_log.erase(g_log.begin(), g_log.begin() + 5000);
    g_log.push_back({kind, t});
    if (g_log_file) {
        static const char *const tag[] = {"   ", "@@ ", "!! ", ">> "};
        fprintf(g_log_file, "%s%s\n", tag[kind], t.c_str());
        fflush(g_log_file);
    }
}
static void set_status(const std::string &s) { std::lock_guard<std::mutex> l(g_mu); g_status = s; }
static Step *step(const std::string &id) {
    for (auto &s : g_steps) if (s.id == id) return &s;
    return NULL;
}

// ================================================================== running a tool
// One child: stdout read for "@@" events (the rest is output), stderr as output; both into the details and the
// build log. In a job object, so Cancel (or the launcher closing) ends the tool and the compilers it started.
struct ToolResult { int exit = -1; std::string error, check; bool done = false, skipped = false; std::string skip; };

static void handle_event(const std::string &line, ToolResult &r) {
    // line without the "@@"
    size_t sp = line.find(' ');
    std::string kind = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
    std::lock_guard<std::mutex> l(g_mu);
    Step *cur = NULL;
    for (auto &s : g_steps) if (s.state == StepState::Running) cur = &s;
    if (kind == "stage") {
        size_t s2 = rest.find(' ');
        std::string id = rest.substr(0, s2);
        for (auto &s : g_steps) if (s.state == StepState::Running && s.id != id) s.state = StepState::Done, s.progress = 1;
        Step *st = step(id);
        if (st) {
            st->state = StepState::Running;
            st->progress = 0;
            if (s2 != std::string::npos) st->text = rest.substr(s2 + 1);
        }
    } else if (kind == "progress" && cur) {
        char *e;
        double f = strtod(rest.c_str(), &e);
        cur->progress = f < 0 ? 0 : f > 1 ? 1 : f;
        while (*e == ' ') e++;
        if (*e) cur->text = e;
    } else if (kind == "tool" && cur) {
        // "tool <name> start <command line>" / "tool <name> end <exit> <log>"
        size_t a = rest.find(' '), b = a == std::string::npos ? a : rest.find(' ', a + 1);
        // (compiles start while trgen still translates: that stage keeps showing trgen's command)
        bool overlap = cur->id == "translate" && rest.compare(0, a, "cc") == 0;
        if (b != std::string::npos && rest.compare(a + 1, 5, "start") == 0 && !overlap) cur->tool = rest.substr(b + 1);
    } else if (kind == "info" && cur) cur->info.push_back(rest);
    else if (kind == "warn" && cur) cur->info.push_back("Warning: " + rest);
    else if (kind == "skip") { r.skipped = true; r.skip = rest; }
    else if (kind == "error") r.error = rest;
    else if (kind == "check") r.check = rest;
    else if (kind == "done") r.done = true;
}

static void reader(HANDLE h, bool events, ToolResult *r) {
    std::string pending;
    char buf[4096];
    DWORD n;
    auto flush_line = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (events && line.size() >= 2 && line[0] == '@' && line[1] == '@') {
            handle_event(line.substr(2), *r);
            log_line(1, line.substr(2));
        } else log_line(0, line);
    };
    while (ReadFile(h, buf, sizeof buf, &n, NULL) && n) {
        pending.append(buf, n);
        size_t i;
        while ((i = pending.find('\n')) != std::string::npos) {
            flush_line(pending.substr(0, i));
            pending.erase(0, i + 1);
        }
    }
    if (!pending.empty()) flush_line(pending);
}

static std::wstring quote_arg(const std::string &a) {
    std::wstring w = widen(a);
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;
    std::wstring o = L"\"";
    int bs = 0;
    for (wchar_t c : w) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') o.append((size_t)bs * 2 + 1, L'\\');
        else o.append((size_t)bs, L'\\');
        bs = 0;
        o += c;
    }
    o.append((size_t)bs * 2, L'\\');
    return o + L"\"";
}

// The environment children get: this one's, with the install folder first on PATH (the runtime DLLs live there,
// also when the data folder is elsewhere)
static std::wstring child_env() {
    std::wstring env;
    wchar_t *block = GetEnvironmentStringsW();
    bool path_done = false;
    for (wchar_t *e = block; *e; e += wcslen(e) + 1) {
        std::wstring v = e;
        if (!_wcsnicmp(v.c_str(), L"PATH=", 5)) { v = L"PATH=" + widen(g_paths.install) + L";" + v.substr(5); path_done = true; }
        env += v;
        env += L'\0';
    }
    FreeEnvironmentStringsW(block);
    if (!path_done) { env += L"PATH=" + widen(g_paths.install); env += L'\0'; }
    env += L'\0';
    return env;
}

static ToolResult run_tool(const std::string &label, const std::vector<std::string> &argv, const std::string &cwd) {
    ToolResult r;
    std::wstring cmd;
    std::string shown;
    for (size_t i = 0; i < argv.size(); i++) {
        cmd += (i ? L" " : L"") + quote_arg(argv[i]);
        shown += (i ? " " : "") + narrow(quote_arg(argv[i]));
    }
    log_line(3, label + ": " + shown);
    {
        std::lock_guard<std::mutex> l(g_mu);
        for (auto &s : g_steps) if (s.state == StepState::Running) s.tool = shown;
    }
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out_r, out_w, err_r, err_w;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        r.error = "could not create pipes: " + win_error(GetLastError());
        return r;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi = {};
    std::wstring env = child_env();
    std::wstring wcwd = widen(cwd);
    BOOL started = CreateProcessW(widen(argv[0]).c_str(), cmd.data(), NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
                                  env.data(), wcwd.empty() ? NULL : wcwd.c_str(), &si, &pi);
    DWORD start_error = GetLastError();
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!started) {
        CloseHandle(out_r);
        CloseHandle(err_r);
        r.error = label + " could not start: " + win_error(start_error);
        log_line(2, r.error);
        return r;
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    AssignProcessToJobObject(job, pi.hProcess);
    { std::lock_guard<std::mutex> l(g_mu); g_job = job; }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::thread to(reader, out_r, true, &r), te(reader, err_r, false, &r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    to.join();
    te.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(out_r);
    CloseHandle(err_r);
    { std::lock_guard<std::mutex> l(g_mu); g_job = NULL; }
    CloseHandle(job);
    r.exit = (int)code;
    if (g_cancel) r.error = "cancelled";
    else if (r.exit != 0 && r.error.empty())
        r.error = label + " stopped with exit status " + std::to_string(r.exit) + " without saying why: see the details";
    log_line(r.exit ? 2 : 3, label + " exited with status " + std::to_string(r.exit));
    return r;
}

// ================================================================== GitHub releases (HTTPS through WinHTTP)
// Only https://api.github.com/repos/<repo>/releases/latest and the release's own assets
// (https://github.com/<repo>/releases/download/...) are ever requested. GitHub redirects asset downloads to its
// storage host; WinHTTP follows those redirects.
static bool url_split(const std::wstring &url, std::wstring &host, std::wstring &path) {
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t h[256], p[2048];
    uc.lpszHostName = h, uc.dwHostNameLength = 256, uc.lpszUrlPath = p, uc.dwUrlPathLength = 2048;
    wchar_t x[1024];
    uc.lpszExtraInfo = x, uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    host.assign(h, uc.dwHostNameLength);
    path.assign(p, uc.dwUrlPathLength);
    path.append(x, uc.dwExtraInfoLength);
    return true;
}

// GET url; the body to `sink` (false from it stops). why on failure.
static bool http_get(const std::string &url, const std::function<bool(const char *, size_t)> &sink, uint64_t *length,
                     std::string &why) {
    std::wstring host, path;
    if (!url_split(widen(url), host, path)) { why = "not an https URL: " + url; return false; }
    HINTERNET s = WinHttpOpen(L"Trirod-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { why = "WinHTTP: " + win_error(GetLastError()); return false; }
    WinHttpSetTimeouts(s, 10000, 15000, 30000, 60000);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : NULL;
    bool ok = r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1L,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL);
    if (!ok) why = "could not reach " + narrow(host) + ": " + win_error(GetLastError());
    DWORD status = 0, sz = sizeof status;
    if (ok) {
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) { ok = false; why = narrow(host) + " answered HTTP " + std::to_string(status); }
    }
    if (ok && length) {
        wchar_t len[32];
        DWORD lsz = sizeof len;
        *length = WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lsz,
                                      WINHTTP_NO_HEADER_INDEX) ? _wcstoui64(len, NULL, 10) : 0;
    }
    std::vector<char> buf(1 << 16);
    while (ok) {
        DWORD got = 0;
        if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &got)) { ok = false; why = "download interrupted: " + win_error(GetLastError()); break; }
        if (!got) break;
        if (!sink(buf.data(), got)) { ok = false; why = g_cancel ? "cancelled" : "could not save the download"; break; }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

static bool http_text(const std::string &url, std::string &out, std::string &why) {
    out.clear();
    return http_get(url, [&](const char *d, size_t n) { if (out.size() + n > (8u << 20)) return false; out.append(d, n); return true; },
                    NULL, why);
}

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    Sha256() {
        if (!BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    }
    ~Sha256() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void add(const void *d, size_t n) { if (h) BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0); }
    std::string hex() {
        unsigned char out[32];
        if (!h || BCryptFinishHash(h, out, 32, 0)) return "";
        char s[65];
        for (int i = 0; i < 32; i++) snprintf(s + 2 * i, 3, "%02x", out[i]);
        return s;
    }
};

// The latest release, as its trirod-release.json describes it (setup/make_release.ps1)
struct Release {
    bool checked = false, ok = false;
    std::string why;                 // check failed / no update source
    std::string tag, version, notes_url;
    bool newer = false, required = false;
    std::string gamelib_id, gamebuild_id;
    std::string asset_name, asset_url, asset_sha256;
    uint64_t asset_size = 0;
};
static Release g_release;
static std::atomic<bool> g_checking{false};

static bool asset_url_ok(const std::string &url, const std::string &repo) {
    return url.rfind("https://github.com/" + repo + "/releases/download/", 0) == 0;
}

static Release check_release(const ToolsInfo &tools) {
    Release rel;
    rel.checked = true;
    std::string repo = TRIROD_UPDATE_REPO;
    if (repo.empty()) { rel.why = "this build has no update source configured"; return rel; }
    std::string body, why;
    if (!http_text("https://api.github.com/repos/" + repo + "/releases/latest", body, why)) {
        // (GitHub's answer for a repository without a published release, or one this PC can't see)
        rel.why = why.find("HTTP 404") != std::string::npos ? "no published release of " + repo + " was found (" + why + ")" : why;
        return rel;
    }
    Json j;
    if (!json_parse(body, j) || j.kind != Json::Obj) { rel.why = "GitHub's release answer could not be read"; return rel; }
    rel.tag = j["tag_name"].str();
    rel.notes_url = j["html_url"].str();
    std::string manifest_url;
    std::map<std::string, std::pair<std::string, uint64_t>> assets;
    for (auto &a : j["assets"].a) {
        std::string name = a["name"].str(), url = a["browser_download_url"].str();
        assets[name] = {url, (uint64_t)a["size"].num()};
        if (name == RELEASE_MANIFEST) manifest_url = url;
    }
    if (manifest_url.empty()) { rel.why = "release " + rel.tag + " has no " RELEASE_MANIFEST; return rel; }
    if (!asset_url_ok(manifest_url, repo)) { rel.why = "unexpected manifest location " + manifest_url; return rel; }
    if (!http_text(manifest_url, body, why)) { rel.why = why; return rel; }
    if (!json_parse(body, j) || j["format"].str() != "trirod-release" || (int)j["schema"].num() != 1) {
        rel.why = RELEASE_MANIFEST " of " + rel.tag + " is not a schema-1 Trirod release manifest";
        return rel;
    }
    if (j["channel"].str() != RELEASE_CHANNEL) { rel.why = "the latest release is not for " RELEASE_CHANNEL; return rel; }
    rel.version = j["version"].str();
    rel.gamelib_id = j["build"]["gamelib_id"].str();
    rel.gamebuild_id = j["build"]["gamebuild_id"].str();
    std::string min_launcher = j["min_launcher"].str();
    if (!min_launcher.empty() && version_compare(TRIROD_VERSION, min_launcher) == -1) {
        rel.why = "release " + rel.version + " needs a newer launcher (" + min_launcher + "): download it from " + rel.notes_url;
        return rel;
    }
    for (auto &a : j["assets"].a) {
        if (a["kind"].str() != "tools") continue;
        rel.asset_name = a["name"].str();
        rel.asset_sha256 = a["sha256"].str();
        rel.asset_size = (uint64_t)a["size"].num();
    }
    auto it = assets.find(rel.asset_name);
    if (rel.asset_name.empty() || rel.asset_sha256.size() != 64 || it == assets.end()) {
        rel.why = RELEASE_MANIFEST " names no tools package attached to release " + rel.tag;
        return rel;
    }
    rel.asset_url = it->second.first;
    if (!asset_url_ok(rel.asset_url, repo)) { rel.why = "unexpected package location " + rel.asset_url; return rel; }
    if (it->second.second && rel.asset_size && it->second.second != rel.asset_size) {
        rel.why = "the attached package's size does not match " RELEASE_MANIFEST;
        return rel;
    }
    rel.ok = true;
    int cmp = tools.present ? version_compare(rel.version, tools.version) : 1;
    rel.newer = cmp == 1;
    // (a new release recompiles only when its tools make other game code or another runtime than the build here)
    rel.required = rel.newer && (!tools.present || rel.gamelib_id != tools.gamelib_id || rel.gamebuild_id != tools.gamebuild_id ||
                                 j["required"].kind == Json::Bool && j["required"].b);
    return rel;
}

// What a tools package may hold at its top level, and what it must never touch: the player's data stays as it is
static bool protected_name(const std::string &n) {
    static const char *const keep[] = {"extracted", "cache", "save", "saves", "logs", "updates", "fonts", "rando",
                                       "Trirod.exe", "Trirod.exe.id", "eowrecomp.ini", "trirod-launcher.ini",
                                       "trirod-install.json", "eowrecomp.log"};
    for (const char *k : keep) if (!_stricmp(n.c_str(), k)) return true;
    return false;
}

static std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = narrow(fd.cFileName);
        if (n != "." && n != "..") out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static bool remove_tree(const std::string &p) {
    if (!path_exists(p)) return true;
    if (!dir_exists(p)) return DeleteFileW(widen(p).c_str()) != 0;
    for (auto &n : list_dir(p)) remove_tree(p + "\\" + n);
    return RemoveDirectoryW(widen(p).c_str()) != 0;
}

// Download, verify, unpack and install the release's tools package. The installed tools are moved aside first and
// put back if anything fails, so a failed update leaves the working installation as it was.
static bool apply_update(const Release &rel, std::string &why) {
    Step *st;
    {
        std::lock_guard<std::mutex> l(g_mu);
        st = step("update");
        st->state = StepState::Running;
        st->text = "Downloading " + rel.asset_name;
        st->tool = rel.asset_url;
    }
    std::string dir = g_paths.install + "\\updates\\" + rel.version;
    remove_tree(dir);
    CreateDirectoryW(widen(g_paths.install + "\\updates").c_str(), NULL);
    CreateDirectoryW(widen(dir).c_str(), NULL);
    std::string zip = dir + "\\" + rel.asset_name, part = zip + ".part";
    log_line(3, "downloading " + rel.asset_url + " (" + std::to_string(rel.asset_size) + " bytes)");
    FILE *f = _wfopen(widen(part).c_str(), L"wb");
    if (!f) { why = "could not write " + part; return false; }
    Sha256 sha;
    uint64_t got = 0, length = 0;
    bool ok = http_get(rel.asset_url, [&](const char *d, size_t n) {
        if (g_cancel || fwrite(d, 1, n, f) != n) return false;
        sha.add(d, n);
        got += n;
        std::lock_guard<std::mutex> l(g_mu);
        uint64_t total = rel.asset_size ? rel.asset_size : length;
        st->progress = total ? 0.8 * (double)got / (double)total : 0;
        char t[128];
        snprintf(t, sizeof t, "Downloading %s: %.1f of %.1f MB", rel.asset_name.c_str(), got / 1048576.0, total / 1048576.0);
        st->text = t;
        return true;
    }, &length, why);
    if (fclose(f) != 0) ok = false;
    if (!ok) { DeleteFileW(widen(part).c_str()); return false; }
    std::string digest = sha.hex();
    if (got != rel.asset_size || _stricmp(digest.c_str(), rel.asset_sha256.c_str())) {
        DeleteFileW(widen(part).c_str());
        why = "the downloaded package failed verification (size " + std::to_string(got) + ", SHA-256 " + digest +
              "; expected " + std::to_string(rel.asset_size) + ", " + rel.asset_sha256 + ")";
        return false;
    }
    MoveFileExW(widen(part).c_str(), widen(zip).c_str(), MOVEFILE_REPLACE_EXISTING);
    log_line(3, "verified SHA-256 " + digest);

    // unpack with Windows' own tar (bsdtar reads zip), into a stage folder
    std::string stage = dir + "\\stage";
    CreateDirectoryW(widen(stage).c_str(), NULL);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    {
        std::lock_guard<std::mutex> l(g_mu);
        st->progress = 0.82;
        st->text = "Unpacking the tools";
    }
    ToolResult t = run_tool("tar", {narrow(sys) + "\\tar.exe", "-xf", zip, "-C", stage}, dir);
    if (t.exit != 0) { why = "the package could not be unpacked (tar exit " + std::to_string(t.exit) + ")"; return false; }
    Json tj;
    if (!json_parse(read_file(stage + "\\tools\\trirod-tools.json"), tj) || tj["version"].str() != rel.version) {
        why = "the package's tools/trirod-tools.json does not say version " + rel.version;
        return false;
    }
    std::vector<std::string> names = list_dir(stage);
    for (auto &n : names)
        if (protected_name(n)) { why = "the package contains '" + n + "', which belongs to the player's data: refused"; return false; }

    // swap: each top-level entry moved aside, the new one moved in; on failure everything moved back
    {
        std::lock_guard<std::mutex> l(g_mu);
        st->progress = 0.9;
        st->text = "Installing the tools";
    }
    std::string backup = dir + "\\previous";
    CreateDirectoryW(widen(backup).c_str(), NULL);
    std::vector<std::string> moved_old, moved_new;
    ok = true;
    for (auto &n : names) {
        std::string cur = g_paths.install + "\\" + n, old = backup + "\\" + n;
        if (path_exists(cur)) {
            if (!MoveFileExW(widen(cur).c_str(), widen(old).c_str(), 0)) {
                why = "could not move " + cur + " aside: " + win_error(GetLastError()) + " (close the game and try again)";
                ok = false;
                break;
            }
            moved_old.push_back(n);
        }
        if (!MoveFileExW(widen(stage + "\\" + n).c_str(), widen(cur).c_str(), 0)) {
            why = "could not install " + cur + ": " + win_error(GetLastError());
            ok = false;
            break;
        }
        moved_new.push_back(n);
    }
    if (!ok) {
        for (auto &n : moved_new) MoveFileExW(widen(g_paths.install + "\\" + n).c_str(), widen(stage + "\\" + n).c_str(), 0);
        for (auto &n : moved_old) MoveFileExW(widen(backup + "\\" + n).c_str(), widen(g_paths.install + "\\" + n).c_str(), 0);
        log_line(2, "update rolled back: the previous tools are in place");
        return false;
    }
    DeleteFileW(widen(zip).c_str());
    log_line(3, "installed tools " + rel.version + " (the previous files are in " + backup + " until the next update)");
    std::lock_guard<std::mutex> l(g_mu);
    st->state = StepState::Done;
    st->progress = 1;
    st->text = "Tools " + rel.version + " installed";
    return true;
}

// ================================================================== the build workflow (worker thread)
static void reset_steps(bool with_update) {
    std::lock_guard<std::mutex> l(g_mu);
    g_steps.clear();
    if (with_update) g_steps.push_back({"update", "Updating Trirod tools"});
    g_steps.push_back({"files", "Preparing game files"});
    g_steps.push_back({"translate", "Translating game code for PC"});
    g_steps.push_back({"compile", "Compiling and linking the generated code"});
    g_steps.push_back({"shaders", "Preparing shaders for the graphics device"});
    g_log.clear();
    g_error.clear();
}

static void open_build_log() {
    CreateDirectoryW(widen(g_paths.logs()).c_str(), NULL);
    std::string p = g_paths.logs() + "\\last-build.log";
    if (g_log_file) fclose(g_log_file);
    g_log_file = _wfopen(widen(p).c_str(), L"wb");
    log_line(3, std::string("Trirod-Launcher ") + TRIROD_VERSION + (g_dev ? " (development mode)" : "") + ", " + now_iso());
    log_line(3, "install folder " + g_paths.install + ", data folder " + g_paths.data);
}

static void fail_running(const std::string &why) {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto &s : g_steps)
        if (s.state == StepState::Running) { s.state = StepState::Failed; s.note = why; }
    g_error = why;
}

static void write_install(const ToolsInfo &t, const std::string &shaders, const std::string &shader_note) {
    std::string s = "{\n  \"format\": \"trirod-install\",\n  \"schema\": 1,\n";
    s += "  \"tools_version\": " + json_quote(t.version) + ",\n";
    s += "  \"gamelib_id\": " + json_quote(t.gamelib_id) + ",\n";
    s += "  \"gamebuild_id\": " + json_quote(t.gamebuild_id) + ",\n";
    s += "  \"source\": " + json_quote(g_config["source"]) + ",\n";
    s += "  \"built\": " + json_quote(now_iso()) + ",\n";
    s += "  \"development\": " + std::string(g_dev ? "true" : "false") + ",\n";
    s += "  \"shaders\": " + json_quote(shaders) + ",\n";
    s += "  \"shaders_note\": " + json_quote(shader_note) + "\n}\n";
    write_file(g_paths.install_json(), s);
}

static void workflow(bool update_first, Release rel) {
    open_build_log();
    std::string why;
    if (update_first) {
        set_status("Updating the tools");
        if (!apply_update(rel, why)) {
            fail_running("Update failed: " + why);
            log_line(2, "update failed: " + why);
            set_status(build_state(read_tools()).empty() ? "Update failed: the installed version still works"
                                                          : "Update failed");
            g_busy = false;
            return;
        }
    }
    ToolsInfo tools = read_tools();
    if (!tools.present) {
        fail_running("The build tools are missing: " + tools.why);
        set_status("Build tools missing");
        g_busy = false;
        return;
    }
    log_line(3, "tools " + tools.version + ": game code id " + tools.gamelib_id + ", runtime id " + tools.gamebuild_id);
    std::string source = g_config["source"];
    struct Run { const char *label; std::vector<std::string> argv; };
    std::vector<std::string> files = {g_paths.trbuild(), "files", "--data", g_paths.data};
    if (!source.empty()) files.insert(files.end(), {"--source", source});
    Run runs[] = {
        {"trbuild files", files},
        {"trbuild code", {g_paths.trbuild(), "code", "--data", g_paths.data}},
    };
    set_status("Building the game from your files");
    for (auto &run : runs) {
        ToolResult r = run_tool(run.label, run.argv, g_paths.data);
        if (r.exit != 0 || !r.done) {
            fail_running(r.error.empty() ? std::string(run.label) + " did not finish" : r.error);
            set_status(g_cancel ? "Cancelled" : "The build failed");
            g_busy = false;
            return;
        }
        std::lock_guard<std::mutex> l(g_mu);
        for (auto &s : g_steps)
            if (s.state == StepState::Running) s.state = StepState::Done, s.progress = 1;
    }
    // The shaders: never a failed build. What isn't prepared is built by the game when first drawn.
    std::string shaders = "prepared", note;
    ToolResult r = run_tool("Trirod-ShaderPrep", {g_paths.shaderprep(), "--data", g_paths.data}, g_paths.data);
    {
        std::lock_guard<std::mutex> l(g_mu);
        Step *s = step("shaders");
        if (r.exit == 0 && r.done) s->state = StepState::Done, s->progress = 1;
        else if (g_cancel) s->state = StepState::Failed, s->note = "cancelled";
        else {
            shaders = r.exit == 3 ? "skipped" : "failed";
            note = r.exit == 3 ? r.skip : r.error;
            s->state = r.exit == 3 ? StepState::Skipped : StepState::Warned;
            s->note = (r.exit == 3 ? "Left to the game: " : "Not prepared (the game builds them when first drawn): ") + note;
        }
    }
    if (g_cancel) { set_status("Cancelled"); g_busy = false; return; }
    std::string state = build_state(tools);
    if (!state.empty()) {
        fail_running("the build finished but " + state);
        set_status("The build failed");
        g_busy = false;
        return;
    }
    write_install(tools, shaders, note);
    log_line(3, "build complete");
    { std::lock_guard<std::mutex> l(g_mu); g_build_ok_once = true; }
    set_status("Ready to play");
    g_busy = false;
}

// trbuild check (worker thread): is the chosen source usable?
struct Validation { bool running = false, done = false, ok = false; std::string text, for_source; };
static Validation g_valid;
static void validate(std::string source) {
    ToolResult r = run_tool("trbuild check", source.empty() ? std::vector<std::string>{g_paths.trbuild(), "check", "--data", g_paths.data}
                                                            : std::vector<std::string>{g_paths.trbuild(), "check", "--data", g_paths.data, "--source", source},
                            g_paths.data);
    std::lock_guard<std::mutex> l(g_mu);
    g_valid.running = false;
    g_valid.done = true;
    g_valid.for_source = source;
    g_valid.ok = r.check.rfind("ok ", 0) == 0;
    g_valid.text = r.check.size() > 3 ? r.check.substr(r.check.find(' ') + 1) : r.error.empty() ? "the game files could not be checked" : r.error;
}

// ================================================================== actions
static void start_validation(const std::string &source) {
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_valid.running) return;
        g_valid = Validation();
        g_valid.running = true;
    }
    std::thread(validate, source).detach();
}

static std::string g_picked;  // from the file dialogs (any thread)
static std::mutex g_pick_mu;
static void SDLCALL picked(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> l(g_pick_mu);
    g_picked = files[0];
}

static void start_workflow(bool update_first) {
    if (g_busy.exchange(true)) return;
    g_cancel = false;
    reset_steps(update_first);
    Release rel = g_release;
    std::thread(workflow, update_first, rel).detach();
}

static void cancel() {
    g_cancel = true;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_job) TerminateJobObject(g_job, 1);
}

static bool play(std::string &why) {
    std::wstring exe = widen(g_paths.game_exe());
    std::wstring cmd = L"\"" + exe + L"\"";
    std::wstring env = child_env();
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(),
                        widen(g_paths.data).c_str(), &si, &pi)) {
        why = "Trirod.exe could not start: " + win_error(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void open_folder(const std::string &p) { ShellExecuteW(NULL, L"open", widen(p).c_str(), NULL, NULL, SW_SHOWNORMAL); }

// ================================================================== the window
static void style() {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 0;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.ChildRounding = 6;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.085f, 0.10f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.115f, 0.135f, 1);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.22f, 0.28f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.30f, 0.38f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.36f, 0.46f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.30f, 0.66f, 0.45f, 1);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.19f, 0.24f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.27f, 0.34f, 1);
}

static bool primary_button(const char *label, bool enabled = true) {
    if (!enabled) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.36f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.62f, 0.43f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.46f, 0.32f, 1));
    bool r = ImGui::Button(label, ImVec2(220, 0));
    ImGui::PopStyleColor(3);
    if (!enabled) ImGui::EndDisabled();
    return r;
}

static void draw_steps(const std::vector<Step> &steps) {
    for (auto &s : steps) {
        const char *mark = "  ";
        ImVec4 c(0.6f, 0.6f, 0.6f, 1);
        switch (s.state) {
        case StepState::Pending: mark = "-"; break;
        case StepState::Running: mark = ">"; c = ImVec4(0.55f, 0.78f, 1, 1); break;
        case StepState::Done: mark = "OK"; c = ImVec4(0.45f, 0.85f, 0.55f, 1); break;
        case StepState::Failed: mark = "X"; c = ImVec4(1, 0.45f, 0.4f, 1); break;
        case StepState::Warned: mark = "!"; c = ImVec4(1, 0.8f, 0.35f, 1); break;
        case StepState::Skipped: mark = "~"; c = ImVec4(1, 0.8f, 0.35f, 1); break;
        }
        ImGui::TextColored(c, "%-2s", mark);
        ImGui::SameLine(40);
        ImGui::TextColored(s.state == StepState::Pending ? ImVec4(0.6f, 0.6f, 0.6f, 1) : ImVec4(1, 1, 1, 1), "%s", s.title.c_str());
        if (s.state == StepState::Running) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
            ImGui::Text("%3d%%", (int)std::lround(s.progress * 100));
            ImGui::Indent(30);
            ImGui::ProgressBar((float)s.progress, ImVec2(-1, 6), "");
            if (!s.text.empty()) ImGui::TextDisabled("%s", s.text.c_str());
            if (!s.tool.empty()) ImGui::TextDisabled("Running: %.300s", s.tool.c_str());
            ImGui::Unindent(30);
        } else {
            ImGui::Indent(30);
            if (!s.note.empty()) ImGui::TextColored(c, "%s", s.note.c_str());
            if (s.state == StepState::Done || s.state == StepState::Warned || s.state == StepState::Skipped)
                for (auto &i : s.info) ImGui::TextDisabled("%s", i.c_str());
            ImGui::Unindent(30);
        }
    }
}

static std::string shader_summary() {
    Json j;
    if (!json_parse(read_file(g_paths.shader_report()), j) || j["format"].str() != "trirod-shaderprep") return "";
    std::string st = j["status"].str();
    if (st == "skipped") return "Shaders: left to the game (" + j["reason"].str() + ")";
    char b[400];
    snprintf(b, sizeof b, "Shaders prepared for %s: %d recorded pipelines, %d of the game's shader programs as SPIR-V",
             j["device"]["name"].str().c_str(), (int)j["recipes"]["pipelines_ok"].num(), (int)j["catalog"]["spirv_ok"].num());
    return b;
}

int main(int argc, char **argv) {
    g_paths.install = exe_dir();
    g_paths.data = g_paths.install;
    bool auto_recompile = false;  // --recompile: start Recompile as soon as the game files are checked
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dev")) g_dev = true;
        else if (!strcmp(argv[i], "--recompile")) auto_recompile = true;
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) g_paths.data = full_path(argv[++i]);
    }
    CreateDirectoryW(widen(g_paths.data).c_str(), NULL);
    config_load();

    // SDL's 2D renderer, never its Vulkan one: the launcher leaves the GPU's Vulkan device to the game and the tools
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11,direct3d12,direct3d,opengl,software");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        MessageBoxA(NULL, SDL_GetError(), "Trirod Launcher", MB_ICONERROR);
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    SDL_Window *win = SDL_CreateWindow("TriRod", (int)(920 * scale), (int)(680 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!ren) {
        MessageBoxA(NULL, SDL_GetError(), "Trirod Launcher", MB_ICONERROR);
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);
    {
        std::string ico = g_paths.install + "\\icon.png";
        SDL_Surface *s = file_exists(ico) ? SDL_LoadPNG(ico.c_str()) : NULL;
        if (s) SDL_SetWindowIcon(win, s), SDL_DestroySurface(s);
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    style();
    ImGui::GetStyle().ScaleAllSizes(scale);
    // Windows' own UI font when it is there (nothing is shipped for it); ImGui's built-in one otherwise
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::string ui_font = narrow(windir) + "\\Fonts\\segoeui.ttf", ui_bold = narrow(windir) + "\\Fonts\\segoeuib.ttf";
    ImFont *font = file_exists(ui_font) ? io.Fonts->AddFontFromFileTTF(ui_font.c_str(), 17 * scale) : NULL;
    ImFont *big = file_exists(ui_bold) ? io.Fonts->AddFontFromFileTTF(ui_bold.c_str(), 30 * scale) : NULL;
    if (!font) font = io.Fonts->AddFontDefault();
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    SDL_Log("[launcher] renderer %s, install %s, data %s%s", SDL_GetRendererName(ren), g_paths.install.c_str(),
            g_paths.data.c_str(), g_dev ? ", development mode" : "");

    ToolsInfo tools = read_tools();
    Uint64 tools_read_at = SDL_GetTicks();
    if (tools.present && (!g_config["source"].empty() || file_exists(g_paths.extracted() + "\\exefs\\main")))
        start_validation(g_config["source"]);
    if (!g_dev) {
        g_checking = true;
        std::thread([tools] {
            Release r = check_release(tools);
            std::lock_guard<std::mutex> l(g_mu);
            g_release = r;
            g_checking = false;
        }).detach();
    }
    bool show_details = false, quit = false, follow = true;
    std::string play_error;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
        }
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }
        {
            std::lock_guard<std::mutex> l(g_pick_mu);
            if (!g_picked.empty()) {
                g_config["source"] = g_picked;
                config_save();
                g_picked.clear();
                start_validation(g_config["source"]);
            }
        }
        // snapshot of the shared state for this frame
        std::vector<Step> steps;
        std::vector<LogLine> log;
        std::string status, error;
        Release rel;
        Validation val;
        bool busy = g_busy;
        {
            std::lock_guard<std::mutex> l(g_mu);
            steps = g_steps;
            if (show_details) log = g_log;
            status = g_status;
            error = g_error;
            rel = g_release;
            val = g_valid;
        }
        // (the files change only through the tools: read again once a second while idle)
        static std::string built = build_state(tools);
        if (!busy && SDL_GetTicks() - tools_read_at > 1000) {
            tools = read_tools();
            built = build_state(tools);
            tools_read_at = SDL_GetTicks();
        }
        bool have_files = file_exists(g_paths.extracted() + "\\exefs\\main");
        bool source_ok = val.done && val.ok;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (big) ImGui::PushFont(big);
        ImGui::Text("TriRod");
        if (big) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s  %s", version_label(tools.present ? tools.version : TRIROD_VERSION).c_str(),
                            g_dev ? "| Development mode" : "");
        ImGui::TextDisabled("Fan project, not affiliated with Nintendo. Needs your own legally obtained copy of the game, v1.0.0.");
        ImGui::Separator();

        // ---- updates
        if (g_dev) ImGui::TextColored(ImVec4(0.55f, 0.78f, 1, 1), "Development mode: no update check or download; tools from %s",
                                      g_paths.install.c_str());
        else if (g_checking) ImGui::TextDisabled("Checking for updates...");
        else if (!rel.ok) ImGui::TextDisabled("Updates: %s", rel.why.c_str());
        else if (rel.newer) ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Update available: %s%s", version_label(rel.version).c_str(),
                                               rel.required ? " (the game will be recompiled)" : "");
        else ImGui::TextDisabled("Up to date (%s)", version_label(rel.version).c_str());
        if (!tools.present) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Build tools missing: %s", tools.why.c_str());

        // ---- game files
        ImGui::Spacing();
        ImGui::Text("Your game files");
        std::string src = g_config["source"];
        ImGui::TextDisabled("%s", src.empty() ? (have_files ? "(already prepared in the data folder)" : "Not chosen yet")
                                              : src.c_str());
        if (val.running) ImGui::TextDisabled("Checking...");
        else if (val.done) ImGui::TextColored(val.ok ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s",
                                              val.text.c_str());
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Choose game folder...")) SDL_ShowOpenFolderDialog(picked, NULL, win, NULL, false);
        ImGui::SameLine();
        static const SDL_DialogFileFilter filters[] = {{"Game package", "xci;nsp;nca"}};
        if (ImGui::Button("Choose .xci / .nsp...")) SDL_ShowOpenFileDialog(picked, NULL, win, filters, 1, NULL, false);
        if (busy) ImGui::EndDisabled();

        // ---- what to do
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        bool ready = built.empty();
        bool need_update = !g_dev && rel.ok && rel.newer;
        bool can_build = tools.present && (source_ok || (have_files && src.empty()));
        if (auto_recompile && !busy && can_build) auto_recompile = false, start_workflow(false);
        if (busy) {
            if (ImGui::Button("Cancel", ImVec2(220, 0))) cancel();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.c_str());
        } else {
            std::string headline = !status.empty() && !g_build_ok_once && !error.empty() ? status
                                 : ready ? (need_update ? "An update is available" : "Ready to play")
                                 : !tools.present ? (g_dev ? "Build the local package first (setup/build.ps1)" : "The tools need to be downloaded")
                                 : !can_build ? "Choose your game files"
                                 : "Recompile to build the game for this version (" + built + ")";
            if (need_update && (rel.required || !ready)) {
                if (primary_button(rel.required || !ready ? "Update and recompile" : "Update")) start_workflow(true);
                if (ready) {
                    ImGui::SameLine();
                    if (ImGui::Button("Play installed version", ImVec2(220, 0))) {
                        if (play(play_error)) quit = true;
                    }
                }
            } else if (ready) {
                if (primary_button("Play")) {
                    if (play(play_error)) quit = !g_dev;
                }
                ImGui::SameLine();
                if (ImGui::Button("Recompile", ImVec2(160, 0))) start_workflow(false);
                if (need_update) {
                    ImGui::SameLine();
                    if (ImGui::Button("Update", ImVec2(160, 0))) start_workflow(true);
                }
            } else if (!tools.present && !g_dev && rel.ok) {
                if (primary_button("Download tools and build")) start_workflow(true);
            } else {
                if (primary_button("Recompile", can_build)) start_workflow(false);
            }
            ImGui::TextDisabled("%s", headline.c_str());
            if (ready) {
                std::string sh = shader_summary();
                if (!sh.empty()) ImGui::TextDisabled("%s", sh.c_str());
            }
            if (!play_error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", play_error.c_str());
        }
        if (!error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", error.c_str());

        // ---- progress and details
        if (!steps.empty()) {
            ImGui::Spacing();
            draw_steps(steps);
        }
        ImGui::Spacing();
        if (ImGui::Checkbox("Show details", &show_details)) {}
        ImGui::SameLine();
        if (ImGui::SmallButton("Open logs folder")) { CreateDirectoryW(widen(g_paths.logs()).c_str(), NULL); open_folder(g_paths.logs()); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Open data folder")) open_folder(g_paths.data);
        if (show_details) {
            ImGui::SameLine();
            ImGui::Checkbox("Follow", &follow);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                std::string all;
                for (auto &l : log) all += l.text + "\n";
                SDL_SetClipboardText(all.c_str());
            }
            ImGui::BeginChild("##details", ImVec2(-1, -1), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            ImGuiListClipper clip;
            clip.Begin((int)log.size());
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                    const LogLine &l = log[(size_t)i];
                    ImVec4 c = l.kind == 1 ? ImVec4(0.55f, 0.78f, 1, 1) : l.kind == 2 ? ImVec4(1, 0.45f, 0.4f, 1)
                             : l.kind == 3 ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(0.82f, 0.82f, 0.82f, 1);
                    ImGui::TextColored(c, "%s", l.text.c_str());
                }
            if (follow && busy) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 19, 22, 26, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
        if (!busy && !show_details) SDL_Delay(15);
    }
    if (g_busy) {
        cancel();
        for (int i = 0; i < 200 && g_busy; i++) SDL_Delay(10);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
