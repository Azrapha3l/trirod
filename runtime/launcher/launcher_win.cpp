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
//     --prepare-shaders: start "Prepare shaders" (only Trirod-ShaderPrep) when the game is already built.
//     --console (or EOWRECOMP_CONSOLE=1): a diagnostic console. The launcher is a GUI program and opens none by
//            default; with this it prints its build log there (the console it was started from, or a new one) and
//            starts Trirod.exe with --console, which prints the game's detailed diagnostics to the same console.
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
    // written by the game when the shader cache wasn't prepared for its GPU and driver (device.c pipeline_cache_check)
    std::string shaders_stale() const { return data + "\\cache\\shaders-stale.txt"; }
    std::string shader_report() const { return data + "\\cache\\shaderprep.json"; }
};
static Paths g_paths;
static bool g_dev;
static bool g_console;  // --console / EOWRECOMP_CONSOLE=1

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
static std::string g_notice;                 // a one-time note (the self-update: "Updated to ...")
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
    if (g_console) fprintf(stderr, "%s\n", t.c_str());
}
static void set_status(const std::string &s) { std::lock_guard<std::mutex> l(g_mu); g_status = s; }
static Step *step(const std::string &id) {
    for (auto &s : g_steps) if (s.id == id) return &s;
    return NULL;
}

// ================================================================== running a tool
// One child: stdout read for "@@" events (the rest is output), stderr as output; both into the details and the
// build log. In a job object, so Cancel (or the launcher closing) ends the tool and the compilers it started.
struct ToolResult { int exit = -1; std::string error, check, extracted, version; bool done = false, skipped = false; std::string skip; };

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
    else if (kind == "extracted") r.extracted = rest;
    else if (kind == "version") r.version = rest;  // (Trirod-Launcher.exe --version: the self-update's check)
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
    bool new_launcher = false;       // min_launcher is newer than this launcher: the update replaces it (it always does)
    std::string gamelib_id, gamebuild_id;
    std::string asset_name, asset_url, asset_sha256;
    std::string local_file;          // --update-from: the package is this file, not a download
    uint64_t asset_size = 0;
};
static Release g_release;
static std::atomic<bool> g_checking{false};

static bool asset_url_ok(const std::string &url, const std::string &repo) {
    return url.rfind("https://github.com/" + repo + "/releases/download/", 0) == 0;
}

// The manifest's own fields (both sources): version, build identifiers, the tools package, and how the release
// relates to what is installed. false = not installable here (rel.why).
static bool release_fields(const Json &j, const ToolsInfo &tools, Release &rel) {
    if (j["format"].str() != "trirod-release" || (int)j["schema"].num() != 1) {
        rel.why = RELEASE_MANIFEST " of " + rel.tag + " is not a schema-1 Trirod release manifest";
        return false;
    }
    if (j["channel"].str() != RELEASE_CHANNEL) { rel.why = "the latest release is not for " RELEASE_CHANNEL; return false; }
    rel.version = j["version"].str();
    rel.gamelib_id = j["build"]["gamelib_id"].str();
    rel.gamebuild_id = j["build"]["gamebuild_id"].str();
    // min_launcher: the launcher this release's tools need. The update installs the package's own launcher and restarts
    // into it, so an older launcher here is replaced, not a reason to refuse (that refusal is what kept every release
    // whose min_launcher was its own version, make_release.ps1's default, from ever being installed). min_updater: the
    // oldest launcher whose self-update can install this package (empty: any that has one); older ones can't.
    std::string min_launcher = j["min_launcher"].str(), min_updater = j["min_updater"].str();
    rel.new_launcher = !min_launcher.empty() && version_compare(TRIROD_VERSION, min_launcher) == -1;
    if (!min_updater.empty() && version_compare(TRIROD_VERSION, min_updater) == -1) {
        rel.why = "release " + rel.version + " can only be installed by launcher " + min_updater + " or newer: download it from " +
                  rel.notes_url;
        return false;
    }
    for (auto &a : j["assets"].a) {
        if (a["kind"].str() != "tools") continue;
        rel.asset_name = a["name"].str();
        rel.asset_sha256 = a["sha256"].str();
        rel.asset_size = (uint64_t)a["size"].num();
    }
    if (rel.asset_name.empty() || rel.asset_sha256.size() != 64 || !rel.asset_size) {
        rel.why = RELEASE_MANIFEST " of " + rel.tag + " names no tools package (name, size and SHA-256)";
        return false;
    }
    int cmp = tools.present ? version_compare(rel.version, tools.version) : 1;
    rel.newer = cmp == 1;
    // (a new release recompiles only when its tools make other game code or another runtime than the build here)
    rel.required = rel.newer && (!tools.present || rel.gamelib_id != tools.gamelib_id || rel.gamebuild_id != tools.gamebuild_id ||
                                 (j["required"].kind == Json::Bool && j["required"].b));
    return true;
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
    if (!json_parse(body, j)) { rel.why = RELEASE_MANIFEST " of " + rel.tag + " could not be read"; return rel; }
    if (!release_fields(j, tools, rel)) return rel;
    auto it = assets.find(rel.asset_name);
    if (it == assets.end()) {
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
    return rel;
}

// --update-from <folder>: a release kept on disk (its trirod-release.json and the zip it names), for testing the
// update and installing without GitHub. The package is checked against the manifest exactly as a download is.
static Release check_release_local(const std::string &folder, const ToolsInfo &tools) {
    Release rel;
    rel.checked = true;
    rel.notes_url = folder;
    Json j;
    if (!json_parse(read_file(folder + "\\" RELEASE_MANIFEST), j) || j.kind != Json::Obj) {
        rel.why = "no readable " RELEASE_MANIFEST " in " + folder;
        return rel;
    }
    rel.tag = j["version"].str();
    if (!release_fields(j, tools, rel)) return rel;
    if (rel.asset_name.find_first_of("\\/:") != std::string::npos) { rel.why = "the manifest's package name is not a file name"; return rel; }
    rel.local_file = folder + "\\" + rel.asset_name;
    rel.asset_url = rel.local_file;
    if (!file_exists(rel.local_file)) { rel.why = "the package " + rel.local_file + " is missing"; return rel; }
    rel.ok = true;
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

// ================================================================== self-update
// A tools package carries the launcher too, and Windows can't replace an executable while it runs. (It can rename
// one: the swap here used to move the running Trirod-Launcher.exe aside and the new one in, after which this process
// kept running the old code and DLLs into the recompile, and the new launcher only took over at the next manual
// start.) So the NEW launcher installs the update, from its unpacked stage folder, once this one has closed:
//   1. stage_update (this launcher): download (or --update-from copy), size and SHA-256 against the manifest, unpack
//      into updates\<version>\stage, check the contents, run the staged launcher's --version, write update.json.
//   2. hand_off_update: start stage\Trirod-Launcher.exe --apply-update <dir> --wait-pid <this pid> and wait for its
//      "Staged" signal; then this launcher closes. No signal: it stays open and nothing installed was touched.
//   3. apply_mode (the staged launcher): wait for this process to end, move each installed top-level entry aside into
//      updates\<version>\previous and the package's in, start the installed (new) launcher with the same --data /
//      --dev / --console plus --updated <version> (and --recompile when the release needs it), wait for its "Started"
//      signal. A failed move, start or signal moves everything back and starts the previous launcher with
//      --update-error <why>. The steps are logged in updates\<version>\apply.log.
static const char *const PACKAGE_REQUIRED[] = {"Trirod-Launcher.exe", "trbuild.exe", "Trirod-ShaderPrep.exe", "SDL3.dll",
                                               "tools\\trirod-tools.json"};
static std::atomic<bool> g_quit_for_update{false};  // the staged launcher took over: close this one

static std::string signal_name(const char *what, DWORD pid) {
    return std::string("Local\\Trirod-Launcher-") + what + "-" + std::to_string(pid);
}
// Tell whoever started this process (it created the event under our pid) that a phase is reached
static void signal_parent(const char *what) {
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, widen(signal_name(what, GetCurrentProcessId())).c_str());
    if (e) SetEvent(e), CloseHandle(e);
}

// Start argv suspended, create its named event, let it run, and wait for the event, its exit, or the timeout.
// true = signaled (the process keeps running; *pid says which). false: why (and the process is ended).
static bool start_and_wait_signal(const std::vector<std::string> &argv, const std::string &cwd, const char *what,
                                  DWORD timeout_ms, DWORD *pid, std::string &why) {
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); i++) cmd += (i ? L" " : L"") + quote_arg(argv[i]);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(widen(argv[0]).c_str(), cmd.data(), NULL, NULL, FALSE, CREATE_SUSPENDED, NULL,
                        widen(cwd).c_str(), &si, &pi)) {
        why = argv[0] + " could not start: " + win_error(GetLastError());
        return false;
    }
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, widen(signal_name(what, pi.dwProcessId)).c_str());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    HANDLE waits[2] = {ev, pi.hProcess};
    DWORD w = ev ? WaitForMultipleObjects(2, waits, FALSE, timeout_ms) : WAIT_FAILED;
    bool ok = w == WAIT_OBJECT_0;
    if (!ok) {
        DWORD code = 0;
        if (w == WAIT_OBJECT_0 + 1 && GetExitCodeProcess(pi.hProcess, &code))
            why = argv[0] + " ended at start (exit status " + std::to_string(code) + ")";
        else why = argv[0] + " did not answer within " + std::to_string(timeout_ms / 1000) + " s";
        TerminateProcess(pi.hProcess, 1);
    }
    if (pid) *pid = pi.dwProcessId;
    if (ev) CloseHandle(ev);
    CloseHandle(pi.hProcess);
    return ok;
}

// 1. Download / copy, verify, unpack and check the package; write update.json for the staged launcher.
static bool stage_update(const Release &rel, std::string &why) {
    Step *st;
    {
        std::lock_guard<std::mutex> l(g_mu);
        st = step("update");
        st->state = StepState::Running;
        st->text = (rel.local_file.empty() ? "Downloading " : "Reading ") + rel.asset_name;
        st->tool = rel.asset_url;
    }
    std::string dir = g_paths.install + "\\updates\\" + rel.version;
    if (!remove_tree(dir)) { why = "could not clear " + dir + " (is a launcher from it still running?)"; return false; }
    CreateDirectoryW(widen(g_paths.install + "\\updates").c_str(), NULL);
    CreateDirectoryW(widen(dir).c_str(), NULL);
    std::string zip = dir + "\\" + rel.asset_name, part = zip + ".part";
    log_line(3, (rel.local_file.empty() ? "downloading " : "reading ") + rel.asset_url + " (" + std::to_string(rel.asset_size) + " bytes)");
    FILE *f = _wfopen(widen(part).c_str(), L"wb");
    if (!f) { why = "could not write " + part; return false; }
    Sha256 sha;
    uint64_t got = 0, length = 0;
    auto sink = [&](const char *d, size_t n) {
        if (g_cancel || fwrite(d, 1, n, f) != n) return false;
        sha.add(d, n);
        got += n;
        std::lock_guard<std::mutex> l(g_mu);
        uint64_t total = rel.asset_size ? rel.asset_size : length;
        st->progress = total ? 0.7 * (double)got / (double)total : 0;
        char t[160];
        snprintf(t, sizeof t, "%s %s: %.1f of %.1f MB", rel.local_file.empty() ? "Downloading" : "Reading",
                 rel.asset_name.c_str(), got / 1048576.0, total / 1048576.0);
        st->text = t;
        return true;
    };
    bool ok;
    if (rel.local_file.empty()) ok = http_get(rel.asset_url, sink, &length, why);
    else {
        FILE *in = _wfopen(widen(rel.local_file).c_str(), L"rb");
        ok = in != NULL;
        if (!in) why = "could not read " + rel.local_file;
        static char buf[1 << 20];
        size_t n;
        while (ok && in && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = sink(buf, n);
        if (in) fclose(in);
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { DeleteFileW(widen(part).c_str()); if (why.empty()) why = g_cancel ? "cancelled" : "could not save the package"; return false; }
    std::string digest = sha.hex();
    if (got != rel.asset_size || _stricmp(digest.c_str(), rel.asset_sha256.c_str())) {
        DeleteFileW(widen(part).c_str());
        why = "the package failed verification (size " + std::to_string(got) + ", SHA-256 " + digest + "; expected " +
              std::to_string(rel.asset_size) + ", " + rel.asset_sha256 + ")";
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
        st->progress = 0.75;
        st->text = "Unpacking the tools";
    }
    ToolResult t = run_tool("tar", {narrow(sys) + "\\tar.exe", "-xf", zip, "-C", stage}, dir);
    if (t.exit != 0) { why = "the package could not be unpacked (tar exit " + std::to_string(t.exit) + ")"; return false; }
    DeleteFileW(widen(zip).c_str());
    Json tj;
    if (!json_parse(read_file(stage + "\\tools\\trirod-tools.json"), tj) || tj["version"].str() != rel.version) {
        why = "the package's tools/trirod-tools.json does not say version " + rel.version;
        return false;
    }
    for (auto &n : list_dir(stage))
        if (protected_name(n)) { why = "the package contains '" + n + "', which belongs to the player's data: refused"; return false; }
    for (const char *n : PACKAGE_REQUIRED)
        if (!file_exists(stage + "\\" + n)) { why = std::string("the package has no ") + n; return false; }
    // The staged launcher must start and be this release's (it is what installs the rest and what runs afterwards)
    {
        std::lock_guard<std::mutex> l(g_mu);
        st->progress = 0.85;
        st->text = "Checking the new launcher";
    }
    ToolResult v = run_tool("new launcher", {stage + "\\Trirod-Launcher.exe", "--version"}, stage);
    if (v.exit != 0 || v.version != rel.version) {
        why = "the package's launcher " + (v.exit != 0 ? "does not start (" + (v.error.empty() ? "exit " + std::to_string(v.exit) : v.error) + ")"
                                                         : "reports version " + v.version + ", not " + rel.version);
        return false;
    }
    std::string s = "{\n  \"format\": \"trirod-update\",\n  \"schema\": 1,\n";
    s += "  \"version\": " + json_quote(rel.version) + ",\n";
    s += "  \"install\": " + json_quote(g_paths.install) + ",\n";
    s += "  \"data\": " + json_quote(g_paths.data) + ",\n";
    s += "  \"dev\": " + std::string(g_dev ? "true" : "false") + ",\n";
    s += "  \"console\": " + std::string(g_console ? "true" : "false") + ",\n";
    s += "  \"recompile\": " + std::string(rel.required ? "true" : "false") + ",\n";
    s += "  \"from_launcher\": " + json_quote(TRIROD_VERSION) + "\n}\n";
    if (!write_file(dir + "\\update.json", s)) { why = "could not write " + dir + "\\update.json"; return false; }
    return true;
}

// 2. Start the staged launcher in apply mode; true once it has taken over (this launcher must then close)
static bool hand_off_update(const Release &rel, std::string &why) {
    std::string dir = g_paths.install + "\\updates\\" + rel.version, stage = dir + "\\stage";
    {
        std::lock_guard<std::mutex> l(g_mu);
        Step *st = step("update");
        st->progress = 0.95;
        st->text = "Starting the new launcher to install the update";
    }
    log_line(3, "handing the update to " + stage + "\\Trirod-Launcher.exe");
    if (!start_and_wait_signal({stage + "\\Trirod-Launcher.exe", "--apply-update", dir, "--wait-pid",
                                std::to_string(GetCurrentProcessId())},
                               stage, "Staged", 30000, NULL, why))
        return false;
    log_line(3, "the new launcher took over: closing; it installs " + rel.version + " and starts it");
    return true;
}

// ---- 3. apply mode (the staged launcher, no window)
static FILE *g_apply_log;
static void apply_log(const std::string &t) {
    if (g_apply_log) fprintf(g_apply_log, "%s %s\n", now_iso().c_str(), t.c_str()), fflush(g_apply_log);
}

// Move the stage's top-level entries into install, the installed ones aside first; false = everything moved back
static bool swap_into(const std::string &stage, const std::string &install, const std::string &backup,
                      const std::vector<std::string> &names, std::vector<std::string> &moved_old,
                      std::vector<std::string> &moved_new, std::string &why) {
    for (auto &n : names) {
        std::string cur = install + "\\" + n, old = backup + "\\" + n;
        if (path_exists(cur)) {
            if (!MoveFileExW(widen(cur).c_str(), widen(old).c_str(), 0)) {
                why = "could not move " + cur + " aside: " + win_error(GetLastError()) + " (is the game or a tool still running?)";
                return false;
            }
            moved_old.push_back(n);
        }
        if (!MoveFileExW(widen(stage + "\\" + n).c_str(), widen(cur).c_str(), 0)) {
            why = "could not install " + cur + ": " + win_error(GetLastError());
            return false;
        }
        moved_new.push_back(n);
    }
    return true;
}
static void swap_back(const std::string &stage, const std::string &install, const std::string &backup,
                      const std::vector<std::string> &moved_old, const std::vector<std::string> &moved_new) {
    for (auto &n : moved_new)
        if (!MoveFileExW(widen(install + "\\" + n).c_str(), widen(stage + "\\" + n).c_str(), 0))
            apply_log("rollback: could not move " + n + " back to the stage: " + win_error(GetLastError()));
    for (auto &n : moved_old)
        if (!MoveFileExW(widen(backup + "\\" + n).c_str(), widen(install + "\\" + n).c_str(), 0))
            apply_log("rollback: could not restore " + n + ": " + win_error(GetLastError()));
}

static std::vector<std::string> relaunch_args(const std::string &install, const Json &u) {
    std::vector<std::string> a = {install + "\\Trirod-Launcher.exe", "--data", u["data"].str()};
    if (u["dev"].kind == Json::Bool && u["dev"].b) a.push_back("--dev");
    if (u["console"].kind == Json::Bool && u["console"].b) a.push_back("--console");
    return a;
}

static int apply_fail(const std::string &msg) {
    apply_log("FAILED: " + msg);
    MessageBoxW(NULL, widen("The Trirod update could not be installed:\n\n" + msg).c_str(), L"Trirod Launcher", MB_ICONERROR);
    return 1;
}

static int apply_mode(const std::string &dir_arg, DWORD wait_pid) {
    std::string dir = full_path(dir_arg), stage = dir + "\\stage", backup = dir + "\\previous";
    g_apply_log = _wfopen(widen(dir + "\\apply.log").c_str(), L"ab");
    apply_log(std::string("Trirod-Launcher ") + TRIROD_VERSION + " applying the update in " + dir);
    Json u;
    if (!json_parse(read_file(dir + "\\update.json"), u) || u["format"].str() != "trirod-update" ||
        u["version"].str() != TRIROD_VERSION || u["install"].str().empty() || u["data"].str().empty())
        return apply_fail(dir + "\\update.json is missing or is not this launcher's update");
    std::string install = u["install"].str();
    // (this executable must be the staged one: it moves its own folder's entries into the installation)
    if (_stricmp(full_path(exe_dir()).c_str(), full_path(stage).c_str()))
        return apply_fail("--apply-update runs only from the update's stage folder (" + stage + ")");
    // The previous launcher closes once it sees this signal; wait for it to be gone before touching its files
    HANDLE old = wait_pid ? OpenProcess(SYNCHRONIZE, FALSE, wait_pid) : NULL;
    signal_parent("Staged");
    if (old) {
        DWORD w = WaitForSingleObject(old, 60000);
        CloseHandle(old);
        if (w != WAIT_OBJECT_0) return apply_fail("the previous launcher did not close within 60 s; nothing was changed");
    }
    apply_log("the previous launcher has closed");
    std::vector<std::string> names = list_dir(stage), moved_old, moved_new;
    for (auto &n : names)
        if (protected_name(n)) return apply_fail("the package contains '" + n + "', which belongs to the player's data");
    remove_tree(backup);
    CreateDirectoryW(widen(backup).c_str(), NULL);
    std::string why;
    bool ok = swap_into(stage, install, backup, names, moved_old, moved_new, why);
    if (ok) {
        apply_log("installed " + std::to_string(moved_new.size()) + " entries (" + std::to_string(moved_old.size()) +
                  " previous ones kept in " + backup + ")");
        std::vector<std::string> a = relaunch_args(install, u);
        a.insert(a.end(), {"--updated", u["version"].str()});
        if (u["recompile"].kind == Json::Bool && u["recompile"].b) a.push_back("--recompile");
        DWORD pid = 0;
        ok = start_and_wait_signal(a, install, "Started", 60000, &pid, why);
        if (ok) {
            apply_log("the new launcher " + std::to_string(pid) + " is running: update complete");
            // older updates' folders (their previous files) are no longer the way back: keep only this one
            for (auto &n : list_dir(install + "\\updates"))
                if (_stricmp(n.c_str(), u["version"].str().c_str())) remove_tree(install + "\\updates\\" + n);
            return 0;
        }
        why = "the new launcher " + why;
    }
    apply_log("installing failed: " + why + "; restoring the previous installation");
    swap_back(stage, install, backup, moved_old, moved_new);
    std::vector<std::string> a = relaunch_args(install, u);
    a.insert(a.end(), {"--update-error", why});
    std::string why2;
    if (!start_and_wait_signal(a, install, "Started", 60000, NULL, why2))
        return apply_fail(why + "\n\nThe previous installation was restored, but its launcher could not be started (" + why2 +
                          "): start Trirod-Launcher.exe yourself.");
    apply_log("the previous launcher is running again");
    return 1;
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

// Trirod-ShaderPrep (worker thread): never a failed build. What isn't prepared is built by the game when first drawn.
static void prepare_shaders(std::string &shaders, std::string &note) {
    shaders = "prepared";
    note.clear();
    ToolResult r = run_tool("Trirod-ShaderPrep", {g_paths.shaderprep(), "--data", g_paths.data}, g_paths.data);
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

// "Prepare shaders": only the last step, for a game that is already built (another GPU or driver, or a cache from an
// older build: the notice the game leaves in cache\shaders-stale.txt).
static void shaders_workflow() {
    open_build_log();
    ToolsInfo tools = read_tools();
    std::string shaders, note;
    set_status("Preparing shaders for this graphics card");
    prepare_shaders(shaders, note);
    if (g_cancel) { set_status("Cancelled"); g_busy = false; return; }
    if (build_state(tools).empty()) write_install(tools, shaders, note);
    log_line(3, "shader preparation " + shaders);
    set_status(shaders == "prepared" ? "Shaders prepared: ready to play" : "Shaders not prepared: the game builds them as it goes");
    g_busy = false;
}

static void workflow(bool update_first, Release rel) {
    open_build_log();
    std::string why;
    if (update_first) {
        // The new launcher installs the package and restarts into it (with --recompile when the release needs it):
        // this one stops here and closes. Nothing installed changes before that hand-off succeeds.
        set_status("Updating the tools");
        if (!stage_update(rel, why) || !hand_off_update(rel, why)) {
            fail_running("Update failed: " + why);
            log_line(2, "update failed: " + why + " (the installed version was not changed)");
            set_status(build_state(read_tools()).empty() ? "Update failed: the installed version still works"
                                                          : "Update failed");
            g_busy = false;
            return;
        }
        set_status("Restarting into Trirod " + version_label(rel.version));
        g_quit_for_update = true;
        return;  // (g_busy stays set: nothing else may start before the window closes)
    }
    ToolsInfo tools = read_tools();
    if (!tools.present) {
        fail_running("The build tools are missing: " + tools.why);
        set_status("Build tools missing");
        g_busy = false;
        return;
    }
    log_line(3, "tools " + tools.version + ": game code id " + tools.gamelib_id + ", runtime id " + tools.gamebuild_id);
    std::string source = g_config["source"], update = g_config["update"];
    struct Run { const char *label; std::vector<std::string> argv; };
    std::vector<std::string> files = {g_paths.trbuild(), "files", "--data", g_paths.data};
    if (!source.empty()) files.insert(files.end(), {"--source", source});
    if (!source.empty() && !update.empty()) files.insert(files.end(), {"--update", update});
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
    std::string shaders, note;
    prepare_shaders(shaders, note);
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

// trbuild check (worker thread): is the chosen source usable, what is it (base / update by content), and is what is
// already extracted current for it? ("@@extracted <none|current|stale|incomplete> <text>", docs/TOOL-PROTOCOL.md)
struct Validation {
    bool running = false, done = false, ok = false;
    std::string text, for_source, for_update;
    std::string extracted, extracted_text;  // the extraction's state for this selection, and why
};
static Validation g_valid;
static void validate(std::string source, std::string update) {
    std::vector<std::string> argv = {g_paths.trbuild(), "check", "--data", g_paths.data};
    if (!source.empty()) argv.insert(argv.end(), {"--source", source});
    if (!source.empty() && !update.empty()) argv.insert(argv.end(), {"--update", update});
    ToolResult r = run_tool("trbuild check", argv, g_paths.data);
    std::lock_guard<std::mutex> l(g_mu);
    g_valid.running = false;
    g_valid.done = true;
    g_valid.for_source = source;
    g_valid.for_update = update;
    g_valid.ok = r.check.rfind("ok ", 0) == 0;
    g_valid.text = r.check.size() > 3 ? r.check.substr(r.check.find(' ') + 1) : r.error.empty() ? "the game files could not be checked" : r.error;
    size_t sp = r.extracted.find(' ');
    g_valid.extracted = r.extracted.substr(0, sp);
    g_valid.extracted_text = sp == std::string::npos ? "" : r.extracted.substr(sp + 1);
}

// ================================================================== actions
static void start_validation(const std::string &source, const std::string &update) {
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_valid.running) return;
        g_valid = Validation();
        g_valid.running = true;
    }
    std::thread(validate, source, update).detach();
}

static std::string g_picked, g_picked_update;  // from the file dialogs (any thread)
static std::mutex g_pick_mu;
static void SDLCALL picked(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> l(g_pick_mu);
    g_picked = files[0];
}
static void SDLCALL picked_update(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> l(g_pick_mu);
    g_picked_update = files[0];
}

static void start_workflow(bool update_first) {
    if (g_busy.exchange(true)) return;
    g_cancel = false;
    reset_steps(update_first);
    Release rel = g_release;
    std::thread(workflow, update_first, rel).detach();
}

static void start_shaders_workflow() {
    if (g_busy.exchange(true)) return;
    g_cancel = false;
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_steps.clear();
        g_steps.push_back({"shaders", "Preparing shaders for the graphics device"});
        g_log.clear();
        g_error.clear();
    }
    std::thread(shaders_workflow).detach();
}

static void cancel() {
    g_cancel = true;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_job) TerminateJobObject(g_job, 1);
}

static bool play(std::string &why) {
    std::wstring exe = widen(g_paths.game_exe());
    std::wstring cmd = L"\"" + exe + L"\"" + (g_console ? L" --console" : L"");
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

// One launcher per installation and data folder: two would install updates and run builds over each other. The
// self-update's restarted launcher waits for the closing one's lock (it is gone once that process has ended).
static HANDLE g_instance;
static bool single_instance(DWORD wait_ms) {
    std::string key = g_paths.install + "|" + g_paths.data;
    uint64_t h = 1469598103934665603ull;
    for (char c : key) h = (h ^ (uint8_t)tolower((unsigned char)(c == '/' ? '\\' : c))) * 1099511628211ull;
    char name[64];
    snprintf(name, sizeof name, "Local\\Trirod-Launcher-%016llx", (unsigned long long)h);
    g_instance = CreateMutexA(NULL, FALSE, name);
    if (!g_instance) return true;  // (no lock to take: don't refuse to start over it)
    DWORD w = WaitForSingleObject(g_instance, wait_ms);
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
}

int main(int argc, char **argv) {
    g_paths.install = exe_dir();
    g_paths.data = g_paths.install;
    bool auto_recompile = false;  // --recompile: start Recompile as soon as the game files are checked
    bool auto_shaders = false;    // --prepare-shaders: start "Prepare shaders" once the built game is found
    bool auto_update = false;     // --update: start the update as soon as the check finds a newer release (tests)
    std::string apply_dir, updated, update_error, update_from;
    DWORD wait_pid = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dev")) g_dev = true;
        else if (!strcmp(argv[i], "--recompile")) auto_recompile = true;
        else if (!strcmp(argv[i], "--prepare-shaders")) auto_shaders = true;
        else if (!strcmp(argv[i], "--console")) g_console = true;
        else if (!strcmp(argv[i], "--update")) auto_update = true;
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) g_paths.data = full_path(argv[++i]);
        else if (!strcmp(argv[i], "--update-from") && i + 1 < argc) update_from = full_path(argv[++i]);
        // the self-update (stage_update / apply_mode): internal arguments
        else if (!strcmp(argv[i], "--apply-update") && i + 1 < argc) apply_dir = argv[++i];
        else if (!strcmp(argv[i], "--wait-pid") && i + 1 < argc) wait_pid = (DWORD)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--updated") && i + 1 < argc) updated = argv[++i];
        else if (!strcmp(argv[i], "--update-error") && i + 1 < argc) update_error = argv[++i];
        else if (!strcmp(argv[i], "--version")) {  // (an event line: the self-update reads it through run_tool)
            std::string line = std::string("@@version ") + TRIROD_VERSION + "\n";
            DWORD n;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &n, NULL);
            return 0;
        }
    }
    if (!apply_dir.empty()) return apply_mode(apply_dir, wait_pid);
    if (!single_instance(updated.empty() && update_error.empty() ? 0 : 15000)) {
        MessageBoxW(NULL, widen("Trirod Launcher is already running for " + g_paths.data + ".").c_str(), L"Trirod Launcher",
                    MB_ICONINFORMATION);
        return 0;
    }
    if (!updated.empty()) g_notice = "Updated to Trirod " + version_label(updated) + ".";
    if (!update_error.empty()) g_error = "The update could not be installed: " + update_error +
                                         ". The previous version was restored and still works.";
    {   // (as runtime/platform/windows.c: the console it was started from, or a new one; a redirected stderr stays)
        const char *e = getenv("EOWRECOMP_CONSOLE");
        if (e && *e && strcmp(e, "0")) g_console = true;
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if (g_console && (!h || h == INVALID_HANDLE_VALUE) && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            setvbuf(stderr, NULL, _IONBF, 0);
        }
        if (g_console) fprintf(stderr, "Trirod-Launcher %s: diagnostic console on\n", TRIROD_VERSION);
    }
    CreateDirectoryW(widen(g_paths.data).c_str(), NULL);
    config_load();
    // SDL's log lines only on the diagnostic console: its default output on Windows attaches to the parent's console,
    // which would print into the terminal the launcher was started from
    SDL_SetLogOutputFunction([](void *, int, SDL_LogPriority, const char *message) {
        if (g_console) fprintf(stderr, "[sdl] %s\n", message);
    }, NULL);

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
        start_validation(g_config["source"], g_config["update"]);
    if (!g_dev || !update_from.empty()) {  // (--update-from: a release on disk, also in development mode)
        g_checking = true;
        std::thread([tools, update_from] {
            Release r = update_from.empty() ? check_release(tools) : check_release_local(update_from, tools);
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
            if (!g_picked.empty() || !g_picked_update.empty()) {
                if (!g_picked.empty()) g_config["source"] = g_picked;
                if (!g_picked_update.empty()) g_config["update"] = g_picked_update;
                config_save();
                g_picked.clear(), g_picked_update.clear();
                start_validation(g_config["source"], g_config["update"]);
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
        static std::string stale;  // (cache\shaders-stale.txt: why the game found no prepared shaders for its GPU)
        if (!busy && SDL_GetTicks() - tools_read_at > 1000) {
            tools = read_tools();
            built = build_state(tools);
            stale = file_exists(g_paths.shaders_stale()) ? read_file(g_paths.shaders_stale()) : "";
            while (!stale.empty() && (stale.back() == '\n' || stale.back() == '\r')) stale.pop_back();
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
        ImGui::TextDisabled("Fan project, not affiliated with Nintendo. Needs your own legally obtained copy of the game (v1.0.0, or v1.1.0 with its update).");
        ImGui::Separator();

        // ---- updates
        if (g_dev && update_from.empty())
            ImGui::TextColored(ImVec4(0.55f, 0.78f, 1, 1), "Development mode: no update check or download; tools from %s",
                               g_paths.install.c_str());
        else if (g_checking) ImGui::TextDisabled("Checking for updates...");
        else if (!rel.ok) ImGui::TextDisabled("Updates: %s", rel.why.c_str());
        else if (rel.newer) ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Update available: %s%s%s%s", version_label(rel.version).c_str(),
                                               rel.local_file.empty() ? "" : " (from a local folder)",
                                               rel.new_launcher ? " (includes a new launcher: it restarts)" : "",
                                               rel.required ? " (the game will be recompiled)" : "");
        else ImGui::TextDisabled("Up to date (%s)", version_label(rel.version).c_str());
        if (!g_notice.empty()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s", g_notice.c_str());
        if (!tools.present) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Build tools missing: %s", tools.why.c_str());

        // ---- game files
        ImGui::Spacing();
        ImGui::Text("Your game files");
        std::string src = g_config["source"], upd = g_config["update"];
        ImGui::TextDisabled("%s", src.empty() ? (have_files ? "(already prepared in the data folder)" : "Not chosen yet")
                                              : src.c_str());
        if (!upd.empty()) ImGui::TextDisabled("Update: %s", upd.c_str());
        // (what the check found applies to this selection only; a new pick starts a new check)
        bool val_current = val.done && val.for_source == src && val.for_update == upd;
        if (val.running) ImGui::TextDisabled("Checking...");
        else if (val_current) {
            ImGui::TextColored(val.ok ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s%s",
                               val.ok && !src.empty() ? "Detected: " : "", val.text.c_str());
            if (val.extracted == "current")
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "Extracted files: current (%s)", val.extracted_text.c_str());
            else if (val.extracted == "stale")
                ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Extracted files: out of date (%s). Recompile imports the "
                                   "chosen files; the current ones are kept until that succeeds.", val.extracted_text.c_str());
            else if (val.extracted == "incomplete")
                ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Extracted files: incomplete (%s). Recompile finishes them.",
                                   val.extracted_text.c_str());
            else if (val.extracted == "none" && val.ok) ImGui::TextDisabled("Extracted files: none yet");
        }
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Choose game folder...")) SDL_ShowOpenFolderDialog(picked, NULL, win, NULL, false);
        ImGui::SameLine();
        static const SDL_DialogFileFilter filters[] = {{"Game package", "xci;nsp;nca"}};
        if (ImGui::Button("Choose .xci / .nsp...")) SDL_ShowOpenFileDialog(picked, NULL, win, filters, 1, NULL, false);
        // (an update next to the base in the chosen folder is found by its contents; this is for one kept elsewhere)
        static const SDL_DialogFileFilter update_filters[] = {{"Game update", "nsp"}};
        ImGui::SameLine();
        if (ImGui::Button("Choose update .nsp...")) SDL_ShowOpenFileDialog(picked_update, NULL, win, update_filters, 1, NULL, false);
        if (!upd.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("No update")) {
                g_config.erase("update");
                config_save();
                start_validation(g_config["source"], "");
            }
        }
        if (busy) ImGui::EndDisabled();

        // ---- what to do
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        // The game is built, and from the files chosen now: an extraction from another base / update / keys file
        // (stale) or a half-finished one is rebuilt by Recompile, not played
        bool extraction_old = val_current && (val.extracted == "stale" || val.extracted == "incomplete");
        bool ready = built.empty() && !extraction_old;
        std::string not_ready = !built.empty() ? built
                              : extraction_old ? "the extracted game files don't match the chosen ones: " + val.extracted_text : "";
        {   // (after a build, check again: the state shown must be the new extraction's)
            static bool was_busy;
            if (was_busy && !busy) start_validation(g_config["source"], g_config["update"]);
            was_busy = busy;
        }
        bool need_update = (!g_dev || !update_from.empty()) && rel.ok && rel.newer;
        if (auto_update && !busy && !g_checking && rel.checked) {  // (--update: once, whatever the check found)
            auto_update = false;
            if (need_update) start_workflow(true);
        }
        bool can_build = tools.present && (source_ok || (have_files && src.empty()));
        if (auto_recompile && !busy && can_build) auto_recompile = false, start_workflow(false);
        if (auto_shaders && !busy && ready) auto_shaders = false, start_shaders_workflow();
        if (busy) {
            if (ImGui::Button("Cancel", ImVec2(220, 0))) cancel();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.c_str());
        } else {
            std::string headline = !status.empty() && !g_build_ok_once && !error.empty() ? status
                                 : ready ? (need_update ? "An update is available" : "Ready to play")
                                 : !tools.present ? (g_dev ? "Build the local package first (setup/build.ps1)" : "The tools need to be downloaded")
                                 : !can_build ? "Choose your game files"
                                 : "Recompile to build the game for this version (" + not_ready + ")";
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
                ImGui::SameLine();
                if (ImGui::Button("Prepare shaders", ImVec2(160, 0))) start_shaders_workflow();
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
            if (ready && !stale.empty())
                ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Shaders: %s. Prepare shaders to build them before playing "
                                   "(the game otherwise builds them as it goes).", stale.c_str());
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
        {   // (the self-update waits for this: the restarted launcher is up and drawing)
            static bool started;
            if (!started) started = true, signal_parent("Started");
        }
        if (g_quit_for_update) quit = true;  // (the staged launcher took over the update: it waits for this one to end)
        if (!busy && !show_details) SDL_Delay(15);
    }
    if (g_busy && !g_quit_for_update) {  // (closing for the update: no tool is running, and the new launcher waits)
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
