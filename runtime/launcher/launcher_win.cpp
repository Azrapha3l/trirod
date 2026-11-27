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
//
//   Trirod-Launcher.exe --prepare --data <folder> [--game=<source>] [--game-update=<.nsp>] [--keys=<prod.keys>]
//   Trirod-Launcher.exe --play --data <folder>
//     RecompLauncher's setup step and Play (recomplauncher.json, docs/RELEASE-WORKFLOW.md "RecompLauncher"), with no
//     window. --prepare runs the same build as Recompile and prints "progress <0..1> <text>" lines on stdout, the
//     tools' output on stderr, and the reason last when it fails (exit 1). --play starts the built Trirod.exe, waits
//     for it and exits with its status (PLAY_* below when it can't). Under RecompLauncher (RECOMPLAUNCHER=1) the
//     launcher never checks GitHub for updates (RecompLauncher installs the releases), and the data folder defaults
//     to RECOMPLAUNCHER_DATA.
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
static bool g_headless;  // --prepare: no window; the build log goes to stderr, progress lines to stdout

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
    if (g_console && !g_headless) fprintf(stderr, "%s\n", t.c_str());
    else if (g_headless) {  // (a GUI program: the inherited handle, not the CRT's stderr)
        std::string l = t + "\n";
        DWORD n;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), l.data(), (DWORD)l.size(), &n, NULL);
    }
}
static void set_status(const std::string &s) { std::lock_guard<std::mutex> l(g_mu); g_status = s; }
static Step *step(const std::string &id) {
    for (auto &s : g_steps) if (s.id == id) return &s;
    return NULL;
}

// --prepare: one line on stdout (RecompLauncher's setup step reads "progress <0..1> <text>")
static void stdout_line(const std::string &t) {
    std::string l = t + "\n";
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), l.data(), (DWORD)l.size(), &n, NULL);
}
// The whole build as one fraction (g_mu held). The weights only shape the bar: the stages take very different times
// on different PCs, and the code stages are quick when the generated objects are reused.
static void report_progress(bool force) {
    if (!g_headless) return;
    static const struct { const char *id; double weight; } WEIGHT[] = {
        {"files", 0.25}, {"translate", 0.2}, {"compile", 0.35}, {"shaders", 0.2}};
    double f = 0;
    const Step *cur = NULL;
    for (auto &w : WEIGHT) {
        const Step *s = step(w.id);
        if (!s) continue;
        f += w.weight * (s->state == StepState::Pending ? 0 : s->state == StepState::Running ? s->progress : 1);
        if (s->state == StepState::Running) cur = s;
    }
    static ULONGLONG last;
    static std::string last_title;
    ULONGLONG now = GetTickCount64();
    // (trbuild reports every few megabytes: four lines a second are plenty, but every new stage is shown)
    if (!force && now - last < 250 && cur && cur->title == last_title) return;
    last = now;
    last_title = cur ? cur->title : "";
    char num[16];
    snprintf(num, sizeof num, "%.3f", f > 1 ? 1.0 : f);
    stdout_line(std::string("progress ") + num + " " +
                (!cur ? std::string("Building the game")
                      : cur->text.empty() || cur->text == cur->title ? cur->title : cur->title + ": " + cur->text));
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
    if (kind == "stage" || kind == "progress") report_progress(kind == "stage");
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
    // (the same names as the idle list the window shows before a build: the player sees one list either way)
    g_steps.push_back({"files", "1. Reading your game files"});
    g_steps.push_back({"translate", "2. Translating game code for PC"});
    g_steps.push_back({"compile", "3. Compiling and linking the generated code"});
    g_steps.push_back({"shaders", "4. Preparing shaders for the graphics device"});
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
    // (the update goes even with no source: trbuild refuses it with why, instead of building the old version)
    if (!update.empty()) files.insert(files.end(), {"--update", update});
    if (!source.empty() && file_exists(g_config["keys"])) files.insert(files.end(), {"--keys", g_config["keys"]});
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
    if (!update.empty()) argv.insert(argv.end(), {"--update", update});  // (alone: trbuild says it needs the base)
    if (!source.empty() && file_exists(g_config["keys"])) argv.insert(argv.end(), {"--keys", g_config["keys"]});
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

// ================================================================== RecompLauncher (no window)
static bool single_instance(DWORD wait_ms);
// recomplauncher.json names these exit statuses for the player; Trirod.exe's own statuses pass through --play.
enum { PLAY_NOT_BUILT = 20, PLAY_NO_START = 21, PLAY_BUSY = 22 };

// --prepare: RecompLauncher's setup step. The same build as Recompile (workflow), from the files RecompLauncher
// passes, which are also stored in trirod-launcher.ini so this launcher's own window shows the same choice. Without
// --game (the player's copy is gone: recomplauncher.json keeps_copy) it builds from what extracted\ already holds.
static int prepare_mode(const std::string &source, const std::string &update, const std::string &keys) {
    g_headless = true;
    auto fail = [](const std::string &why) {
        stdout_line("Setup failed: " + why);  // (printed last: RecompLauncher shows the player the last lines)
        return 1;
    };
    if (!single_instance(0)) return fail("Trirod Launcher is open for this game: close it, then set up again");
    ToolsInfo tools = read_tools();
    if (!tools.present) return fail("the build tools are missing (" + tools.why + "): reinstall the game");
    // (an update alone was dropped here and the old extraction rebuilt: it patches the base package's own data, so it
    // can't be applied to extracted\; trbuild says the same for the window, UPDATE_NEEDS_BASE)
    if (source.empty() && !update.empty())
        return fail("the update needs the base game package too: pick your base .xci / .nsp together with the update");
    if (!source.empty()) {
        g_config["source"] = source;
        g_config["update"] = update;
        if (!keys.empty()) g_config["keys"] = keys; else g_config.erase("keys");
    } else g_config.erase("source"), g_config.erase("update");  // (no copy given: it is gone, so extracted\ is the source)
    config_save();
    reset_steps(false);
    open_build_log();
    if (g_config["source"].empty() && !file_exists(g_paths.extracted() + "\\exefs\\main"))
        return fail("no copy of the game was given, and no game files from an earlier setup are kept: pick your copy");
    // Built by these tools from the same files: nothing to do (RecompLauncher runs this step after every release)
    if (build_state(tools).empty() && file_exists(g_paths.install_json())) {
        validate(g_config["source"], g_config["update"]);
        bool same = g_config["source"].empty() ? g_valid.ok : g_valid.ok && g_valid.extracted == "current";
        if (same) {
            log_line(3, "already built for these tools from " + (g_config["source"].empty() ? g_paths.extracted() : g_config["source"]));
            stdout_line("progress 1 Already built");
            return 0;
        }
    }
    // (trirod-install.json is the step's "done" file: gone while building, so a build cut short never looks finished)
    DeleteFileW(widen(g_paths.install_json()).c_str());
    g_busy = true;
    workflow(false, Release());
    std::string error;
    {
        std::lock_guard<std::mutex> l(g_mu);
        error = g_error;
    }
    if (!error.empty() || !file_exists(g_paths.install_json())) {
        stdout_line("Full log: " + g_paths.logs() + "\\last-build.log");
        return fail(error.empty() ? "the build did not finish" : error);
    }
    stdout_line("progress 1 Ready to play");
    return 0;
}

// --play: RecompLauncher's Play. Starts the built game as the window's Play does, and stays until it closes, so
// RecompLauncher sees the session end (it backs up the saves then) and the game's exit status.
static int play_mode() {
    if (!single_instance(0)) return PLAY_BUSY;
    std::string state = build_state(read_tools());
    if (!state.empty()) return PLAY_NOT_BUILT;
    std::wstring exe = widen(g_paths.game_exe());
    std::wstring cmd = L"\"" + exe + L"\"" + (g_console ? L" --console" : L"");
    std::wstring env = child_env();
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(),
                        widen(g_paths.data).c_str(), &si, &pi))
        return PLAY_NO_START;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// ================================================================== the window
static void style() {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 0;
    s.FrameRounding = 7;
    s.GrabRounding = 7;
    s.ChildRounding = 10;
    s.PopupRounding = 8;
    s.ScrollbarRounding = 8;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 7);
    s.WindowPadding = ImVec2(17, 14);
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 1;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.045f, 0.052f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.085f, 0.090f, 0.96f);
    c[ImGuiCol_Border] = ImVec4(0.59f, 0.47f, 0.25f, 0.92f);
    c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.15f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.22f, 0.22f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.27f, 0.25f, 1);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.18f, 0.19f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.26f, 0.25f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.31f, 0.28f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.26f, 0.86f, 0.77f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.87f, 0.66f, 0.18f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.78f, 0.23f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.88f, 0.65f, 0.05f, 1);
    c[ImGuiCol_Header] = ImVec4(0.13f, 0.20f, 0.20f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.30f, 0.29f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.36f, 0.33f, 1);
}

static const ImVec4 GOLD(0.96f, 0.73f, 0.13f, 1.0f);
static const ImVec4 AQUA(0.24f, 0.85f, 0.77f, 1.0f);
static SDL_Texture *g_launcher_logo;
static SDL_Texture *g_launcher_skin;
static int g_skin_text_child_id;
static float g_launcher_min_font_size = 24.0f;

static ImVec2 skin_point(float x, float y, float sx, float sy) {
    ImVec2 origin = ImGui::GetWindowPos();
    return ImVec2(origin.x + x * sx, origin.y + y * sy);
}

static void skin_text(const char *text, float x, float y, float width, float height,
                      float sx, float sy, ImU32 color, ImFont *font = NULL, float font_size = 0.0f,
                      bool centered = false) {
    if (font) ImGui::PushFont(font);
    ImGui::SetCursorPos(ImVec2(x * sx, y * sy));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushID(++g_skin_text_child_id);
    ImGui::BeginChild("##skin-text", ImVec2(width * sx, height * sy), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (font && font_size > 0.0f) {
        float render_size = std::max(font_size * std::min(sx, sy), g_launcher_min_font_size);
        ImGui::SetWindowFontScale(render_size / font->FontSize);
        if (centered) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 text_size = ImGui::CalcTextSize(text);
            ImGui::SetCursorPos(ImVec2(std::max(0.0f, (avail.x - text_size.x) * 0.5f),
                                       std::max(0.0f, (avail.y - text_size.y) * 0.5f)));
        }
        ImGui::TextWrapped("%s", text);
        ImGui::SetWindowFontScale(1.0f);
    } else {
        ImGui::TextWrapped("%s", text);
    }
    ImGui::EndChild();
    ImGui::PopID();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (font) ImGui::PopFont();
}

static bool skin_button(const char *label, const char *id, float x, float y, float w, float h,
                        float sx, float sy, bool enabled, ImFont *font = NULL, float font_size = 0.0f,
                        ImU32 color = IM_COL32(238, 221, 177, 255)) {
    ImGui::SetCursorPos(ImVec2(x * sx, y * sy));
    if (!enabled) ImGui::BeginDisabled();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w * sx, h * sy));
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    bool hovered = enabled && ImGui::IsItemHovered();
    bool active = enabled && ImGui::IsItemActive();
    if (!enabled) ImGui::EndDisabled();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (!enabled) dl->AddRectFilled(a, b, IM_COL32(8, 13, 22, 105), 7.0f);
    if (hovered) {
        dl->AddRectFilled(a, b, IM_COL32(255, 246, 202, 48), 7.0f);
        dl->AddRect(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2), IM_COL32(255, 241, 184, 230), 7.0f, 0, 3.0f);
    }
    if (active) dl->AddRectFilled(a, b, IM_COL32(15, 20, 27, 48), 7.0f);
    float fs = font_size > 0.0f ? font_size * std::min(sx, sy) : 18.0f * std::min(sx, sy);
    fs = std::max(fs, g_launcher_min_font_size);
    ImFont *f = font ? font : ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
    ImU32 c = enabled ? color : IM_COL32(165, 162, 151, 220);
    dl->AddText(f, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), c, label);
    return pressed;
}

static void draw_brand_mark(ImVec2 pos, float size) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_launcher_logo) {
        dl->AddImage((ImTextureID)(intptr_t)g_launcher_logo, pos, ImVec2(pos.x + size, pos.y + size));
        return;
    }
    float w = size * 0.5f;
    ImVec2 a(pos.x + w, pos.y), b(pos.x, pos.y + size), c(pos.x + size, pos.y + size);
    dl->AddTriangleFilled(a, b, c, IM_COL32(232, 184, 55, 255));
    ImVec2 d(pos.x + w, pos.y + size * 0.36f), e(pos.x + size * 0.24f, pos.y + size * 0.80f), f(pos.x + size * 0.76f, pos.y + size * 0.80f);
    dl->AddTriangleFilled(d, e, f, IM_COL32(31, 43, 42, 255));
    dl->AddTriangleFilled(ImVec2(pos.x + size * 0.75f, pos.y + size * 0.10f),
                          ImVec2(pos.x + size * 0.53f, pos.y + size * 0.48f),
                          ImVec2(pos.x + size * 0.97f, pos.y + size * 0.48f), IM_COL32(60, 208, 191, 255));
    dl->AddTriangle(ImVec2(pos.x + w, pos.y), b, c, IM_COL32(255, 226, 132, 255), 1.5f);
}

static void draw_background_motifs() {
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float start = p.x + s.x * 0.59f;
    for (int i = 0; i < 23; i++) {
        float x = start + (float)((i * 137) % (int)std::max(1.0f, s.x * 0.39f));
        float y = p.y + 12.0f + (float)((i * 83) % (int)std::max(1.0f, s.y * 0.44f));
        float z = 7.0f + (float)(i % 4) * 4.0f;
        ImU32 col = i % 3 == 0 ? IM_COL32(66, 192, 181, 34) : IM_COL32(226, 183, 83, 32);
        dl->AddTriangle(ImVec2(x, y), ImVec2(x + z, y + z * 1.35f), ImVec2(x - z * 0.8f, y + z), col, 1.0f);
    }
    float x = p.x + s.x * 0.80f, y = p.y + 10.0f, z = std::min(170.0f, s.x * 0.16f);
    dl->AddTriangle(ImVec2(x, y), ImVec2(x - z * 0.54f, y + z), ImVec2(x + z * 0.54f, y + z), IM_COL32(212, 172, 72, 18), 2.0f);
    dl->AddRect(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + s.x - 2, p.y + s.y - 2), IM_COL32(125, 105, 67, 180), 7.0f, 0, 1.5f);
    dl->AddRect(ImVec2(p.x + 6, p.y + 6), ImVec2(p.x + s.x - 6, p.y + s.y - 6), IM_COL32(58, 78, 76, 140), 6.0f, 0, 1.0f);
}

static void draw_ornate_child_frame() {
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 outer = IM_COL32(181, 145, 76, 245), inner = IM_COL32(83, 70, 47, 245);
    dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + s.x - 1, p.y + s.y - 1), IM_COL32(10, 14, 16, 70), 7.0f);
    dl->AddRect(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + s.x - 1, p.y + s.y - 1), outer, 7.0f, 0, 2.0f);
    dl->AddRect(ImVec2(p.x + 5, p.y + 5), ImVec2(p.x + s.x - 5, p.y + s.y - 5), inner, 5.0f, 0, 1.0f);
    dl->AddLine(ImVec2(p.x + 18, p.y + 4), ImVec2(p.x + s.x - 18, p.y + 4), IM_COL32(237, 203, 132, 190), 1.0f);
    dl->AddLine(ImVec2(p.x + 18, p.y + s.y - 4), ImVec2(p.x + s.x - 18, p.y + s.y - 4), IM_COL32(63, 53, 39, 220), 1.0f);
    const float k = 17.0f;
    for (int i = 0; i < 4; i++) {
        float x = i & 1 ? p.x + s.x - 7.0f : p.x + 7.0f;
        float y = i & 2 ? p.y + s.y - 7.0f : p.y + 7.0f;
        float dx = i & 1 ? -k : k, dy = i & 2 ? -k : k;
        dl->AddTriangleFilled(ImVec2(x, y), ImVec2(x + dx, y), ImVec2(x, y + dy), IM_COL32(82, 66, 39, 220));
        dl->AddLine(ImVec2(x, y + dy), ImVec2(x, y), IM_COL32(230, 195, 114, 245), 2.0f);
        dl->AddLine(ImVec2(x, y), ImVec2(x + dx, y), IM_COL32(230, 195, 114, 245), 2.0f);
        dl->AddCircleFilled(ImVec2(x, y), 2.7f, IM_COL32(248, 220, 153, 255));
        dl->AddCircle(ImVec2(x, y), 4.0f, IM_COL32(121, 91, 46, 235), 8, 1.0f);
    }
}

static void draw_section_plate(const char *label, ImFont *serif = NULL) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = std::min(ImGui::GetContentRegionAvail().x * 0.70f, 310.0f);
    float x = p.x + (ImGui::GetContentRegionAvail().x - width) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddTriangleFilled(ImVec2(x - 8, p.y + 7), ImVec2(x, p.y + 1), ImVec2(x, p.y + 29), IM_COL32(111, 85, 43, 245));
    dl->AddTriangleFilled(ImVec2(x + width + 8, p.y + 7), ImVec2(x + width, p.y + 1), ImVec2(x + width, p.y + 29), IM_COL32(111, 85, 43, 245));
    dl->AddRectFilled(ImVec2(x, p.y + 1), ImVec2(x + width, p.y + 29), IM_COL32(42, 39, 31, 240), 4.0f);
    dl->AddRect(ImVec2(x, p.y + 1), ImVec2(x + width, p.y + 29), IM_COL32(180, 145, 74, 235), 4.0f, 0, 1.5f);
    dl->AddLine(ImVec2(p.x + 8, p.y + 15), ImVec2(x - 8, p.y + 15), IM_COL32(190, 151, 76, 215), 1.0f);
    dl->AddLine(ImVec2(x + width + 8, p.y + 15), ImVec2(p.x + ImGui::GetContentRegionAvail().x - 8, p.y + 15), IM_COL32(190, 151, 76, 215), 1.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - width) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, GOLD);
    if (serif) ImGui::PushFont(serif);
    ImGui::TextUnformatted(label);
    if (serif) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2));
}

static bool launcher_button(const char *label, bool enabled, ImVec2 size, bool primary, ImFont *font, ImU32 accent) {
    if (font) ImGui::PushFont(font);
    if (size.x == 0.0f) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y == 0.0f) size.y = ImGui::GetFrameHeight();
    if (!enabled) ImGui::BeginDisabled();
    bool pressed = ImGui::InvisibleButton(label, size);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    bool hovered = enabled && ImGui::IsItemHovered();
    bool active = enabled && ImGui::IsItemActive();
    if (!enabled) ImGui::EndDisabled();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float alpha = enabled ? 1.0f : 0.42f;
    float round = primary ? 9.0f : 6.0f;
    if (primary) {
        if (hovered) dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), IM_COL32(255, 220, 106, 95), round + 3, 0, 3.0f);
        dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 2), IM_COL32(103, 69, 15, (int)(255 * alpha)), round + 1);
        dl->AddRectFilled(a, b, IM_COL32(245, 203, 87, (int)(255 * alpha)), round);
        ImVec2 lo(a.x + 4, a.y + 4), hi(b.x - 4, b.y - 4);
        dl->AddRectFilledMultiColor(lo, hi,
            active ? IM_COL32(196, 132, 20, (int)(255 * alpha)) : IM_COL32(255, 238, 153, (int)(255 * alpha)),
            active ? IM_COL32(191, 126, 16, (int)(255 * alpha)) : IM_COL32(255, 221, 103, (int)(255 * alpha)),
            IM_COL32(192, 127, 17, (int)(255 * alpha)), IM_COL32(225, 165, 36, (int)(255 * alpha)));
        dl->AddRect(lo, hi, IM_COL32(255, 244, 186, (int)(230 * alpha)), round - 2, 0, 1.0f);
        dl->AddLine(ImVec2(lo.x + 12, lo.y + 2), ImVec2(hi.x - 12, lo.y + 2), IM_COL32(255, 249, 205, (int)(180 * alpha)), 1.0f);
    } else {
        dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 2), IM_COL32(24, 29, 31, (int)(255 * alpha)), round + 1);
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), accent, round + 1, 0, 2.0f);
        ImVec2 lo(a.x + 3, a.y + 3), hi(b.x - 3, b.y - 3);
        ImU32 top = hovered ? IM_COL32(76, 91, 91, (int)(255 * alpha)) : IM_COL32(56, 67, 68, (int)(255 * alpha));
        ImU32 bottom = active ? IM_COL32(28, 37, 39, (int)(255 * alpha)) : IM_COL32(39, 47, 48, (int)(255 * alpha));
        dl->AddRectFilledMultiColor(lo, hi, top, top, bottom, bottom);
        dl->AddRect(lo, hi, IM_COL32(139, 153, 145, (int)(125 * alpha)), round - 2, 0, 1.0f);
        dl->AddLine(ImVec2(a.x + 12, a.y + 4), ImVec2(b.x - 12, a.y + 4), IM_COL32(211, 199, 154, (int)(115 * alpha)), 1.0f);
    }
    float gem_x = primary ? 13.0f : 8.0f, gem_y = primary ? 13.0f : 8.0f;
    for (int side = 0; side < 2; side++) {
        float x = side ? b.x - gem_x : a.x + gem_x;
        float y = (a.y + b.y) * 0.5f;
        dl->AddQuadFilled(ImVec2(x, y - gem_y), ImVec2(x + gem_x * 0.45f, y), ImVec2(x, y + gem_y), ImVec2(x - gem_x * 0.45f, y),
                          primary ? IM_COL32(255, 248, 198, (int)(225 * alpha)) : accent);
    }
    ImFont *label_font = font ? font : ImGui::GetFont();
    float label_size = std::max(label_font->FontSize, g_launcher_min_font_size);
    ImVec2 text_size = label_font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
    ImVec2 text_pos((a.x + b.x - text_size.x) * 0.5f, (a.y + b.y - text_size.y) * 0.5f);
    ImU32 text_col = primary ? IM_COL32(45, 34, 13, (int)(255 * alpha)) : IM_COL32(235, 228, 205, (int)(255 * alpha));
    dl->AddText(label_font, label_size, text_pos, text_col, label);
    if (font) ImGui::PopFont();
    return pressed;
}

static bool primary_button(const char *label, bool enabled = true, ImVec2 size = ImVec2(220, 0), ImFont *display = NULL) {
    return launcher_button(label, enabled, size, true, display, IM_COL32(246, 203, 95, 255));
}

static bool stone_button(const char *label, ImVec2 size, bool enabled = true, ImU32 accent = IM_COL32(184, 150, 79, 255)) {
    return launcher_button(label, enabled, size, false, NULL, accent);
}

static void draw_steps(const std::vector<Step> &steps) {
    for (auto &s : steps) {
        ImVec4 c(0.60f, 0.62f, 0.59f, 1);
        switch (s.state) {
        case StepState::Running: c = ImVec4(0.98f, 0.79f, 0.37f, 1); break;
        case StepState::Done: c = ImVec4(0.46f, 0.91f, 0.72f, 1); break;
        case StepState::Failed: c = ImVec4(1, 0.38f, 0.33f, 1); break;
        case StepState::Warned: case StepState::Skipped: c = ImVec4(1, 0.77f, 0.31f, 1); break;
        case StepState::Pending: break;
        }
        ImVec2 icon = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float cx = icon.x + 15.0f, cy = icon.y + 15.0f;
        const float r = 14.0f;
        ImVec2 top(cx, cy - r), right(cx + r, cy), bottom(cx, cy + r), left(cx - r, cy);
        if (s.state == StepState::Done) {
            dl->AddQuadFilled(top, right, bottom, left, IM_COL32(40, 174, 131, 255));
            dl->AddQuad(top, right, bottom, left, IM_COL32(139, 255, 221, 255), 1.8f);
        } else {
            ImU32 edge = s.state == StepState::Running ? IM_COL32(255, 211, 105, 255)
                       : s.state == StepState::Failed ? IM_COL32(252, 108, 93, 255)
                       : s.state == StepState::Warned || s.state == StepState::Skipped ? IM_COL32(234, 184, 69, 255)
                       : IM_COL32(164, 177, 165, 255);
            float thickness = s.state == StepState::Running ? 2.7f : 2.0f;
            dl->AddQuad(top, right, bottom, left, edge, thickness);
        }
        std::string title = s.title;
        if (title.size() >= 3 && title[0] >= '0' && title[0] <= '9' && title[1] == '.' && title[2] == ' ')
            title.erase(0, 3);
        int percent = s.state == StepState::Done ? 100
                    : (int)std::lround(std::max(0.0, std::min(1.0, s.progress)) * 100.0);
        title += " (" + std::to_string(percent) + "%)";
        ImVec4 title_color = s.state == StepState::Pending ? ImVec4(0.69f, 0.71f, 0.72f, 1) : c;
        ImGui::SetCursorScreenPos(ImVec2(icon.x + 40, icon.y + 2));
        ImGui::PushTextWrapPos();
        ImGui::TextColored(title_color, "%s", title.c_str());
        ImGui::PopTextWrapPos();
        // Keep the stage column to one stable row per step. Progress and
        // compiler details belong in the details pane, not beneath the stage.
        ImGui::SetCursorScreenPos(ImVec2(icon.x, icon.y + 36));
    }
}

static std::string shader_summary() {
    Json j;
    if (!json_parse(read_file(g_paths.shader_report()), j) || j["format"].str() != "trirod-shaderprep") return "";
    std::string st = j["status"].str();
    if (st == "skipped") return "Shaders: left to the game (" + j["reason"].str() + ")";
    char b[400];
    snprintf(b, sizeof b, "%.30s | %d pipelines | %d SPIR-V shaders",
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
    bool prepare = false, play_only = false, data_given = false;  // --prepare / --play: RecompLauncher's, no window
    std::string game, game_update, keys;
    // "--name=value" or "--name value" (RecompLauncher passes optional files as one argument, flag and path together)
    auto value = [&](int &i, const char *name, std::string &out) {
        size_t n = strlen(name);
        if (!strncmp(argv[i], name, n) && argv[i][n] == '=') return out = argv[i] + n + 1, true;
        if (!strcmp(argv[i], name) && i + 1 < argc) return out = argv[++i], true;
        return false;
    };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prepare")) prepare = true;
        else if (!strcmp(argv[i], "--play")) play_only = true;
        else if (value(i, "--game", game) || value(i, "--game-update", game_update) || value(i, "--keys", keys)) {}
        else if (!strcmp(argv[i], "--dev")) g_dev = true;
        else if (!strcmp(argv[i], "--recompile")) auto_recompile = true;
        else if (!strcmp(argv[i], "--prepare-shaders")) auto_shaders = true;
        else if (!strcmp(argv[i], "--console")) g_console = true;
        else if (!strcmp(argv[i], "--update")) auto_update = true;
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) g_paths.data = full_path(argv[++i]), data_given = true;
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
    // Started by RecompLauncher: it installs every release itself, so never two updaters on one folder, and the data
    // folder is its {data} (RECOMPLAUNCHER_DATA) also when the player skips its Easy Setup and gets this window
    const char *rl = getenv("RECOMPLAUNCHER"), *rl_data = getenv("RECOMPLAUNCHER_DATA");
    bool recomplauncher = rl && !strcmp(rl, "1");
    if (recomplauncher && !data_given && rl_data && *rl_data) g_paths.data = full_path(rl_data);
    if (prepare || play_only) {
        const char *e = getenv("EOWRECOMP_CONSOLE");
        if (e && *e && strcmp(e, "0")) g_console = true;  // (only passed on to Trirod.exe: stderr is RecompLauncher's)
        CreateDirectoryW(widen(g_paths.data).c_str(), NULL);
        config_load();
        if (prepare) return prepare_mode(game.empty() ? "" : full_path(game), game_update.empty() ? "" : full_path(game_update),
                                         keys.empty() ? "" : full_path(keys));
        return play_mode();
    }
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
    SDL_Window *win = SDL_CreateWindow("TriRod", (int)(1280 * scale), (int)(720 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!ren) {
        MessageBoxA(NULL, SDL_GetError(), "Trirod Launcher", MB_ICONERROR);
        return 1;
    }
    SDL_SetWindowMinimumSize(win, (int)(1080 * scale), (int)(608 * scale));
    SDL_SetRenderVSync(ren, 1);
    {
        std::string ico = g_paths.install + "\\icon.png";
        SDL_Surface *s = file_exists(ico) ? SDL_LoadPNG(ico.c_str()) : NULL;
        if (s) {
            SDL_SetWindowIcon(win, s);
            SDL_DestroySurface(s);
        }
    }
    {
        std::string skin = g_paths.install + "\\launcher-skin.png";
        SDL_Surface *s = file_exists(skin) ? SDL_LoadPNG(skin.c_str()) : NULL;
        if (s) {
            g_launcher_skin = SDL_CreateTextureFromSurface(ren, s);
            if (g_launcher_skin) SDL_SetTextureScaleMode(g_launcher_skin, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(s);
        }
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
    std::string ui_serif = narrow(windir) + "\\Fonts\\georgiab.ttf";
    ImFont *font = file_exists(ui_font) ? io.Fonts->AddFontFromFileTTF(ui_font.c_str(), 17 * scale) : NULL;
    ImFont *step_font = file_exists(ui_font) ? io.Fonts->AddFontFromFileTTF(ui_font.c_str(), 24 * scale) : NULL;
    ImFont *log_font = file_exists(ui_font) ? io.Fonts->AddFontFromFileTTF(ui_font.c_str(), 26 * scale) : NULL;
    ImFont *big = file_exists(ui_bold) ? io.Fonts->AddFontFromFileTTF(ui_bold.c_str(), 30 * scale) : NULL;
    ImFont *display = file_exists(ui_serif) ? io.Fonts->AddFontFromFileTTF(ui_serif.c_str(), 31 * scale) : NULL;
    ImFont *serif = file_exists(ui_serif) ? io.Fonts->AddFontFromFileTTF(ui_serif.c_str(), 16 * scale) : NULL;
    if (!font) font = io.Fonts->AddFontDefault();
    g_launcher_min_font_size = step_font ? step_font->FontSize : font->FontSize;
    io.FontDefault = step_font ? step_font : font;
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    SDL_Log("[launcher] renderer %s, install %s, data %s%s", SDL_GetRendererName(ren), g_paths.install.c_str(),
            g_paths.data.c_str(), g_dev ? ", development mode" : "");

    ToolsInfo tools = read_tools();
    Uint64 tools_read_at = SDL_GetTicks();
    // (an update chosen alone is checked too: the check is what says it needs its base package)
    if (tools.present && (!g_config["source"].empty() || !g_config["update"].empty() ||
                          file_exists(g_paths.extracted() + "\\exefs\\main")))
        start_validation(g_config["source"], g_config["update"]);
    if ((!g_dev && !recomplauncher) || !update_from.empty()) {  // (--update-from: a release on disk, also in development mode)
        g_checking = true;
        std::thread([tools, update_from] {
            Release r = update_from.empty() ? check_release(tools) : check_release_local(update_from, tools);
            std::lock_guard<std::mutex> l(g_mu);
            g_release = r;
            g_checking = false;
        }).detach();
    }
    // (details off at start: the panel then shows the next step to take, not an empty log; a build turns them on)
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

        std::string src = g_config["source"], upd = g_config["update"];
        bool val_current = val.done && val.for_source == src && val.for_update == upd;
        bool extraction_old = val_current && (val.extracted == "stale" || val.extracted == "incomplete");
        bool ready = built.empty() && !extraction_old;
        std::string not_ready = !built.empty() ? built
                              : extraction_old ? "the extracted game files don't match the chosen ones: " + val.extracted_text : "";
        {   // (after a build, check again: the state shown must be the new extraction's)
            static bool was_busy;
            if (was_busy && !busy) start_validation(g_config["source"], g_config["update"]);
            if (!was_busy && busy) show_details = true;  // (a build shows its log as it goes)
            was_busy = busy;
        }
        bool need_update = ((!g_dev && !recomplauncher) || !update_from.empty()) && rel.ok && rel.newer;
        if (auto_update && !busy && !g_checking && rel.checked) {  // (--update: once, whatever the check found)
            auto_update = false;
            if (need_update) start_workflow(true);
        }
        // (extracted files alone can be rebuilt; with an update chosen too they can't: the update needs its base package)
        bool can_build = tools.present && (source_ok || (have_files && src.empty() && upd.empty()));
        if (auto_recompile && !busy && can_build) auto_recompile = false, start_workflow(false);
        if (auto_shaders && !busy && ready) auto_shaders = false, start_shaders_workflow();
        std::string headline = !status.empty() && !g_build_ok_once && !error.empty() ? status
                             : ready ? (need_update ? "An update is available" : "Ready to play")
                             : !tools.present ? (g_dev ? "Build the local package first (setup/build.ps1)" : "The tools need to be downloaded")
                             // (said as the next thing to do: the window is a 1-2-3, choose -> build -> play)
                             : !can_build ? (val.running ? std::string("Checking your game files...")
                                             : "Step 1: choose your base game (.xci / .nsp, with prod.keys next to it). "
                                               "Optional: add the v1.1.0 update with UPDATE NSP.")
                             : need_update ? "Update to install the latest version (" + not_ready + ")"
                             : "Step 2: press BUILD GAME (it takes a while, once per version). Then PLAY. (" +
                                   not_ready + ")";

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##main", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        g_skin_text_child_id = 0;
        const float sx = io.DisplaySize.x / 1678.0f, sy = io.DisplaySize.y / 943.0f;
        ImDrawList *canvas = ImGui::GetWindowDrawList();
        if (g_launcher_skin) canvas->AddImage((ImTextureID)(intptr_t)g_launcher_skin, skin_point(0, 0, sx, sy), skin_point(1678, 943, sx, sy));
        else draw_background_motifs();

        std::string version = version_label(tools.present ? tools.version : TRIROD_VERSION);
        skin_text("TriRod", 200, 51, 242, 72, sx, sy, IM_COL32(246, 226, 182, 255), display, 52);
        skin_text(version.c_str(), 443, 75, 150, 40, sx, sy, IM_COL32(224, 190, 111, 255), serif, 24);
        skin_text("Fan project, not affiliated with Nintendo. Requires your own legally obtained game copy.",
                  202, 120, 1200, 34, sx, sy, IM_COL32(199, 201, 204, 255), font, 24);
        ImU32 state_col = busy ? IM_COL32(114, 219, 240, 255) : ready ? IM_COL32(227, 198, 107, 255) : IM_COL32(242, 181, 84, 255);
        const char *state_label = busy ? "Working" : ready ? "This app is ready" : "Setup needed";
        skin_text(state_label, 202, 153, 540, 34, sx, sy, state_col, font, 24);
        if (g_dev && update_from.empty()) skin_text("Development build - local tools", 747, 153, 360, 32, sx, sy, IM_COL32(136, 214, 237, 255), font, 24);
        else if (g_checking) skin_text("Checking for updates...", 747, 153, 360, 32, sx, sy, IM_COL32(214, 219, 219, 255), font, 24);
        else if (rel.newer) skin_text(("Update: " + version_label(rel.version)).c_str(), 747, 153, 360, 32, sx, sy, IM_COL32(255, 216, 122, 255), font, 24);

        skin_text("YOUR GAME FILES", 370, 200, 250, 34, sx, sy, IM_COL32(245, 220, 163, 255), serif, 23, true);
        const std::string source_line = src.empty() ? (have_files ? g_paths.data : "Choose a game folder or package") : src;
        skin_text(source_line.c_str(), 176, 245, 640, 42, sx, sy, IM_COL32(248, 237, 211, 255), font, 24);
        const char *base_state = source_ok || (have_files && src.empty()) ? "OK" : "Not selected";
        skin_text((std::string("Base Game: ") + base_state).c_str(), 178, 292, 585, 32, sx, sy,
                  source_ok || (have_files && src.empty()) ? IM_COL32(84, 232, 207, 255) : IM_COL32(226, 213, 177, 255), font, 24);
        std::string update_file = upd;
        size_t update_leaf = update_file.find_last_of("/\\");
        if (update_leaf != std::string::npos) update_file = update_file.substr(update_leaf + 1);
        std::string update_line = upd.empty() ? "Update (optional): none chosen" : "Update: " + update_file;
        skin_text(update_line.c_str(), 178, 331, 410, 42, sx, sy, IM_COL32(245, 230, 194, 255), font, 24);
        // (a failed check is not necessarily the keys: an unsupported version or an update without its base fail the
        // same check, so this says "keys" only when the reason does, and the reason itself is shown under Next step.
        // An ok check names "base game ..." for packages, read with the keys, and "extracted game files" without them.)
        bool keys_named = val.text.find("keys") != std::string::npos || val.text.find("decrypt") != std::string::npos ||
                          val.text.find("ticket") != std::string::npos;
        std::string keys_line = val.running ? "prod.keys: checking..."
                              : !val_current ? "prod.keys: checked when you build"
                              : val.ok ? (val.text.rfind("base game", 0) == 0 ? "prod.keys: OK" : "prod.keys: not needed (extracted files)")
                              : keys_named ? "prod.keys: problem (see Next step)" : "prod.keys: not checked yet (see Next step)";
        skin_text(keys_line.c_str(),
                  178, 375, 620, 32, sx, sy, val_current && val.ok ? IM_COL32(84, 232, 207, 255) : IM_COL32(242, 226, 191, 255), font, 24);

        static const SDL_DialogFileFilter filters[] = {{"Game package", "xci;nsp;nca"}};
        static const SDL_DialogFileFilter update_filters[] = {{"Game update", "nsp"}};
        if (skin_button("CHOOSE GAME FOLDER...", "##pick-folder", 168, 422, 316, 65, sx, sy, !busy, serif, 24))
            SDL_ShowOpenFolderDialog(picked, NULL, win, NULL, false);
        if (skin_button("CHOOSE XCI / NSP...", "##pick-package", 516, 422, 302, 65, sx, sy, !busy, serif, 24))
            SDL_ShowOpenFileDialog(picked, NULL, win, filters, 1, NULL, false);
        if (skin_button("UPDATE NSP", "##pick-update", 596, 331, 174, 42, sx, sy, !busy, font, 24, IM_COL32(105, 212, 197, 255)))
            SDL_ShowOpenFileDialog(picked_update, NULL, win, update_filters, 1, NULL, false);
        if (!upd.empty() && skin_button("CLEAR", "##remove-update", 776, 331, 76, 42, sx, sy, !busy, font, 24, IM_COL32(255, 151, 125, 255))) {
            g_config.erase("update"); config_save(); start_validation(g_config["source"], "");
        }

        const char *main_action = busy ? "WORKING" : "PLAY";
        if (skin_button(main_action, "##main-action", 1055, 204, 412, 111, sx, sy,
                        !busy && ready, display, 52,
                        IM_COL32(69, 48, 17, 255))) {
            if (play(play_error)) quit = !g_dev;
        }
        if (busy) {
            if (skin_button("CANCEL", "##cancel", 979, 336, 272, 63, sx, sy, true, serif, 24)) cancel();
        } else {
            const char *build_action = need_update ? "UPDATE" : "BUILD GAME";
            if (skin_button(build_action, "##recompile", 979, 336, 272, 63, sx, sy, !busy && (need_update || can_build), serif, 24))
                start_workflow(need_update);
            if (skin_button("PREPARE SHADERS", "##prepare-shaders", 1269, 336, 274, 63, sx, sy, !busy && ready, serif, 24,
                            IM_COL32(185, 249, 244, 255))) start_shaders_workflow();
        }
        if (need_update && ready) {
            std::string update_label = "Update available: " + version_label(rel.version);
            if (skin_button(update_label.c_str(), "##install-update", 1000, 411, 560, 44, sx, sy, true, font, 24, IM_COL32(247, 204, 103, 255)))
                start_workflow(true);
        } else {
            const char *summary_line = busy ? (status.empty() ? "Working..." : status.c_str()) : ready ? "Ready to play"
                                     : need_update ? "Choose Update to install the latest version"
                                     : can_build ? "Next: press BUILD GAME" : "Next: choose your game files";
            skin_text(summary_line, 1090, 411, 500, 44, sx, sy,
                      ready ? IM_COL32(218, 230, 143, 255) : IM_COL32(255, 218, 147, 255), font, 27);
        }
        std::string summary = shader_summary();
        if (summary.empty()) summary = g_dev && update_from.empty() ? "Development build: shader preparation status appears here" : "Vulkan device, pipelines and SPIR-V shader status";
        canvas->AddRectFilled(skin_point(1102, 457, sx, sy), skin_point(1564, 528, sx, sy), IM_COL32(8, 32, 39, 205), 8.0f * std::min(sx, sy));
        skin_text(summary.c_str(), 1117, 461, 432, 63, sx, sy, IM_COL32(139, 241, 244, 255), font, 24);

        ImGui::SetCursorPos(ImVec2(86 * sx, 555 * sy));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##skin-steps", ImVec2(906 * sx, 276 * sy), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        if (step_font) ImGui::PushFont(step_font);
        if (steps.empty()) {
            static const std::vector<Step> idle_steps = {
                {"files", "1. Reading your game files"},
                {"translate", "2. Translating game code for PC"},
                {"compile", "3. Compiling and linking the generated code"},
                {"shaders", "4. Preparing shaders for the graphics device"}
            };
            draw_steps(idle_steps);
        } else draw_steps(steps);
        if (step_font) ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        canvas->AddLine(skin_point(1011, 558, sx, sy), skin_point(1011, 837, sx, sy), IM_COL32(172, 165, 142, 100), 1.0f * std::min(sx, sy));
        ImGui::SetCursorPos(ImVec2(1030 * sx, 556 * sy));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(238, 236, 222, 255));
        if (log_font) ImGui::PushFont(log_font);
        ImGui::BeginChild("##skin-log", ImVec2(548 * sx, 280 * sy), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        if (!busy && val_current && !val.ok) {  // (first, in both views: it is what the player must fix before anything)
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1, 0.48f, 0.40f, 1), "Can't use the chosen files: %s", val.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
        if (show_details) {
            for (int i = 0; i < (int)log.size(); ++i) {
                const LogLine &l = log[(size_t)i];
                ImVec4 c = l.kind == 1 ? ImVec4(0.52f, 0.91f, 1.00f, 1) : l.kind == 2 ? ImVec4(1, 0.55f, 0.47f, 1)
                         : l.kind == 3 ? ImVec4(0.58f, 0.98f, 0.78f, 1) : ImVec4(0.93f, 0.92f, 0.87f, 1);
                ImGui::PushTextWrapPos(); ImGui::TextColored(c, "%s", l.text.c_str()); ImGui::PopTextWrapPos();
            }
            if (follow && busy) ImGui::SetScrollHereY(1.0f);
        } else {
            ImGui::TextColored(AQUA, "%s", busy ? "Current activity" : ready ? "Ready to play" : "Next step");
            if (busy && !status.empty()) ImGui::TextWrapped("%s", status.c_str());
            else if (ready) ImGui::TextWrapped("%s", summary.c_str());
            else ImGui::TextWrapped("%s", headline.c_str());
            if (g_checking) ImGui::TextDisabled("Checking for updates...");
            else if (!rel.ok && !g_dev) ImGui::TextWrapped("Update check: %s", rel.why.c_str());
            if (need_update) ImGui::TextColored(GOLD, "Release %s is available.", version_label(rel.version).c_str());
        }
        if (!g_notice.empty()) ImGui::TextWrapped("%s", g_notice.c_str());
        if (!error.empty()) ImGui::TextColored(ImVec4(1, 0.48f, 0.40f, 1), "%s", error.c_str());
        if (!play_error.empty()) ImGui::TextColored(ImVec4(1, 0.48f, 0.40f, 1), "%s", play_error.c_str());
        if (ready && !stale.empty()) ImGui::TextWrapped("Shader cache note: %s", stale.c_str());
        ImGui::EndChild();
        if (log_font) ImGui::PopFont();
        ImGui::PopStyleColor(); ImGui::PopStyleVar();

        ImGui::SetCursorPos(ImVec2(78 * sx, 873 * sy));
        if (step_font) ImGui::PushFont(step_font);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, AQUA);
        ImGui::Checkbox("SHOW DETAILS", &show_details);
        ImGui::PopStyleColor();
        if (step_font) ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the detailed build and shader log");
        if (skin_button("OPEN LOGS FOLDER", "##open-logs", 350, 865, 270, 58, sx, sy, true, serif, 24)) {
            CreateDirectoryW(widen(g_paths.logs()).c_str(), NULL); open_folder(g_paths.logs());
        }
        if (skin_button("OPEN DATA FOLDER", "##open-data", 650, 865, 266, 58, sx, sy, true, serif, 24)) open_folder(g_paths.data);
        ImGui::End();
        ImGui::PopStyleVar();
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
    if (g_launcher_logo) SDL_DestroyTexture(g_launcher_logo);
    if (g_launcher_skin) SDL_DestroyTexture(g_launcher_skin);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
