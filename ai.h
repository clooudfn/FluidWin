// Monitoraggio degli agenti AI locali (hook CLI, sessioni Antigravity/Claude Code, Ollama/LM Studio e processi).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uiautomation.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ai {

enum class Stato { Inattivo, Occupato, InAttesa, Completato, Errore };
enum class Fonte { Hook, Antigravity, Claude, Codex, Ollama, LmStudio, Processo };

struct Agente {
    std::wstring id, nome, titolo, modello, strumento, cwd;
    Fonte fonte = Fonte::Processo;
    Stato stato = Stato::Inattivo;
    uint64_t tokIn = 0, tokOut = 0, tokCache = 0;
    uint64_t ctxTok = 0, ctxMax = 0;
    DWORD pid = 0;
    double aggiornatoT = 0;
    bool daHook = false;
};

struct Riepilogo {
    Stato statoGlobale = Stato::Inattivo;
    int attivi = 0, occupati = 0, inAttesa = 0, completati = 0;
    uint64_t tokInTot = 0, tokOutTot = 0, tokCacheTot = 0;
    std::vector<Agente> agenti;
};

inline std::wstring formattaToken(uint64_t n) {
    wchar_t b[32];
    if (n < 1000ULL) {
        swprintf(b, 32, L"%llu", (unsigned long long)n);
    } else if (n < 999950ULL) {
        swprintf(b, 32, L"%.1fk", (double)n / 1000.0);
    } else if (n < 999950000ULL) {
        swprintf(b, 32, L"%.1fM", (double)n / 1000000.0);
    } else {
        swprintf(b, 32, L"%.1fB", (double)n / 1000000000.0);
    }
    return b;
}

inline std::wstring formattaTempoFa(double updatedEpoch, double nowEpoch) {
    if (updatedEpoch <= 0.0 || nowEpoch <= 0.0) return L"";
    const double s = std::max(0.0, nowEpoch - updatedEpoch);
    if (s < 5.0) return L"";
    wchar_t b[32];
    if (s < 60.0) swprintf(b, 32, L"%ds", (int)s);
    else if (s < 3600.0) swprintf(b, 32, L"%dm", (int)(s / 60.0));
    else if (s < 86400.0) swprintf(b, 32, L"%dh", (int)(s / 3600.0));
    else swprintf(b, 32, L"%dd", (int)(s / 86400.0));
    return b;
}

namespace dettagli {

inline std::wstring daUtf8(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string inUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring minuscolo(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

inline std::wstring envDir(const wchar_t* nome) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(nome, buf, 32768);
    if (!n || n >= 32768) return L"";
    std::wstring r(buf, n);
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

inline void creaCartelle(const std::wstring& dir) {
    if (dir.empty()) return;
    for (size_t i = 0; i < dir.size(); i++) {
        if ((dir[i] == L'\\' || dir[i] == L'/') && i > 2) {
            CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

inline double fileTimeAEpoch(FILETIME ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0.0;
    return (double)(u.QuadPart - 116444736000000000ULL) / 1e7;
}

inline double adessoEpoch() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return fileTimeAEpoch(ft);
}

inline std::wstring percorsoExe() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (!n) return L"";
        if (n < buf.size() - 1) {
            buf.resize(n);
            return buf;
        }
        if (buf.size() >= 32768) return L"";
        buf.resize(buf.size() * 2);
    }
}

inline bool leggiFile(const std::wstring& path, std::string& out, size_t maxBytes = 1024 * 1024) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) {
        CloseHandle(f);
        return sz.QuadPart == 0;
    }
    const size_t n = (size_t)std::min<LONGLONG>(sz.QuadPart, (LONGLONG)maxBytes);
    out.resize(n);
    DWORD letti = 0, tot = 0;
    while (tot < n && ReadFile(f, out.data() + tot, (DWORD)(n - tot), &letti, nullptr) && letti > 0) {
        tot += letti;
    }
    out.resize(tot);
    CloseHandle(f);
    return true;
}

inline bool scriviAtomico(const std::wstring& path, const std::string& dati) {
    const size_t sl = path.find_last_of(L"\\/");
    if (sl != std::wstring::npos) creaCartelle(path.substr(0, sl));
    const std::wstring tmp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD tot = 0;
    bool ok = true;
    while (tot < dati.size()) {
        DWORD wr = 0;
        if (!WriteFile(f, dati.data() + tot, (DWORD)(dati.size() - tot), &wr, nullptr) || !wr) {
            ok = false;
            break;
        }
        tot += wr;
    }
    if (ok) FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

inline std::wstring sanificaId(const std::wstring& s, const wchar_t* ripiego) {
    std::wstring out;
    out.reserve(std::min<size_t>(s.size(), 64));
    for (wchar_t c : s) {
        if (out.size() >= 64) break;
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_') {
            out.push_back(c);
        } else {
            out.push_back(L'_');
        }
    }
    while (!out.empty() && out.back() == L'_') out.pop_back();
    return out.empty() ? std::wstring(ripiego) : out;
}

// ---- parser e serializzatore JSON minimale ----

struct Json {
    enum Tipo { Null, Bool, Num, Str, Arr, Obj };
    Tipo tipo = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    static Json makeNull() { return Json{}; }
    static Json makeBool(bool v) { Json j; j.tipo = Bool; j.b = v; return j; }
    static Json makeNum(double v) { Json j; j.tipo = Num; j.n = v; return j; }
    static Json makeStr(std::string v) { Json j; j.tipo = Str; j.s = std::move(v); return j; }
    static Json makeArr() { Json j; j.tipo = Arr; return j; }
    static Json makeObj() { Json j; j.tipo = Obj; return j; }

    const Json* trova(const char* k) const {
        if (tipo != Obj) return nullptr;
        for (const auto& [kk, vv] : obj) if (kk == k) return &vv;
        return nullptr;
    }
    Json* trovaMut(const char* k) {
        if (tipo != Obj) return nullptr;
        for (auto& [kk, vv] : obj) if (kk == k) return &vv;
        return nullptr;
    }
    std::string str(const char* k, const char* def = "") const {
        const Json* v = trova(k);
        return (v && v->tipo == Str) ? v->s : std::string(def);
    }
    double num(const char* k, double def = 0.0) const {
        const Json* v = trova(k);
        return (v && v->tipo == Num) ? v->n : def;
    }
    uint64_t u64(const char* k, uint64_t def = 0) const {
        const Json* v = trova(k);
        return (v && v->tipo == Num && v->n >= 0) ? (uint64_t)v->n : def;
    }
    void imposta(const std::string& k, Json v) {
        if (tipo != Obj) { tipo = Obj; obj.clear(); }
        for (auto& [kk, vv] : obj) {
            if (kk == k) { vv = std::move(v); return; }
        }
        obj.emplace_back(k, std::move(v));
    }
    void rimuovi(const std::string& k) {
        if (tipo != Obj) return;
        obj.erase(std::remove_if(obj.begin(), obj.end(), [&](const auto& p) { return p.first == k; }), obj.end());
    }
};

inline void aggiungiUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

inline bool hex4(const std::string& in, size_t p, uint32_t& out) {
    if (p + 4 > in.size()) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < 4; i++) {
        const char c = in[p + i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

inline void saltaSpazi(const std::string& in, size_t& p) {
    while (p < in.size() && (in[p] == ' ' || in[p] == '\t' || in[p] == '\r' || in[p] == '\n')) p++;
}

inline bool parseStr(const std::string& in, size_t& p, std::string& out) {
    out.clear();
    if (p >= in.size() || in[p] != '"') return false;
    p++;
    while (p < in.size()) {
        const char c = in[p++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (p >= in.size()) return false;
        const char e = in[p++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t u = 0;
                if (!hex4(in, p, u)) return false;
                p += 4;
                if (u >= 0xD800 && u <= 0xDBFF && p + 6 <= in.size() && in[p] == '\\' && in[p + 1] == 'u') {
                    uint32_t lo = 0;
                    if (hex4(in, p + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        p += 6;
                        u = 0x10000 + (((u - 0xD800) << 10) | (lo - 0xDC00));
                    }
                }
                aggiungiUtf8(out, u);
                break;
            }
            default: out.push_back(e); break;
        }
    }
    return false;
}

inline bool parseVal(const std::string& in, size_t& p, Json& out, int depth) {
    if (depth > 64) return false;
    saltaSpazi(in, p);
    if (p >= in.size()) return false;
    const char c = in[p];
    if (c == '"') {
        out = Json::makeStr("");
        return parseStr(in, p, out.s);
    }
    if (c == '{') {
        p++;
        out = Json::makeObj();
        saltaSpazi(in, p);
        if (p < in.size() && in[p] == '}') { p++; return true; }
        for (;;) {
            saltaSpazi(in, p);
            std::string k;
            if (!parseStr(in, p, k)) return false;
            saltaSpazi(in, p);
            if (p >= in.size() || in[p] != ':') return false;
            p++;
            Json v;
            if (!parseVal(in, p, v, depth + 1)) return false;
            out.obj.emplace_back(std::move(k), std::move(v));
            saltaSpazi(in, p);
            if (p >= in.size()) return false;
            if (in[p] == '}') { p++; return true; }
            if (in[p] != ',') return false;
            p++;
        }
    }
    if (c == '[') {
        p++;
        out = Json::makeArr();
        saltaSpazi(in, p);
        if (p < in.size() && in[p] == ']') { p++; return true; }
        for (;;) {
            Json v;
            if (!parseVal(in, p, v, depth + 1)) return false;
            out.arr.push_back(std::move(v));
            saltaSpazi(in, p);
            if (p >= in.size()) return false;
            if (in[p] == ']') { p++; return true; }
            if (in[p] != ',') return false;
            p++;
        }
    }
    if (in.compare(p, 4, "true") == 0) { p += 4; out = Json::makeBool(true); return true; }
    if (in.compare(p, 5, "false") == 0) { p += 5; out = Json::makeBool(false); return true; }
    if (in.compare(p, 4, "null") == 0) { p += 4; out = Json::makeNull(); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char* start = in.c_str() + p;
        char* end = nullptr;
        const double v = strtod(start, &end);
        if (end == start) return false;
        p = (size_t)(end - in.c_str());
        out = Json::makeNum(v);
        return true;
    }
    return false;
}

inline bool parseJson(const std::string& in, Json& out) {
    size_t p = 0;
    if (in.size() >= 3 && (unsigned char)in[0] == 0xEF && (unsigned char)in[1] == 0xBB && (unsigned char)in[2] == 0xBF) p = 3;
    if (!parseVal(in, p, out, 0)) return false;
    saltaSpazi(in, p);
    return p == in.size();
}

inline void escapeJsonStr(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, 8, "\\u%04x", (unsigned)c);
                    out += b;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
}

inline void dumpJsonImpl(const Json& v, std::string& out, bool pretty, int indent) {
    auto nl = [&](int ind) {
        out.push_back('\n');
        out.append((size_t)ind * 2, ' ');
    };
    switch (v.tipo) {
        case Json::Null: out += "null"; break;
        case Json::Bool: out += (v.b ? "true" : "false"); break;
        case Json::Num: {
            char b[64];
            if (std::isfinite(v.n) && std::floor(v.n) == v.n && std::abs(v.n) < 9e15) {
                snprintf(b, 64, "%.0f", v.n);
            } else {
                snprintf(b, 64, "%.15g", v.n);
            }
            out += b;
            break;
        }
        case Json::Str: escapeJsonStr(v.s, out); break;
        case Json::Arr:
            out.push_back('[');
            if (!v.arr.empty()) {
                for (size_t i = 0; i < v.arr.size(); i++) {
                    if (i > 0) out.push_back(',');
                    if (pretty) nl(indent + 1);
                    dumpJsonImpl(v.arr[i], out, pretty, indent + 1);
                }
                if (pretty) nl(indent);
            }
            out.push_back(']');
            break;
        case Json::Obj:
            out.push_back('{');
            if (!v.obj.empty()) {
                for (size_t i = 0; i < v.obj.size(); i++) {
                    if (i > 0) out.push_back(',');
                    if (pretty) nl(indent + 1);
                    escapeJsonStr(v.obj[i].first, out);
                    out += (pretty ? ": " : ":");
                    dumpJsonImpl(v.obj[i].second, out, pretty, indent + 1);
                }
                if (pretty) nl(indent);
            }
            out.push_back('}');
            break;
    }
}

inline std::string dumpJson(const Json& v, bool pretty) {
    std::string out;
    dumpJsonImpl(v, out, pretty, 0);
    if (pretty) out.push_back('\n');
    return out;
}

inline int prioritaStato(Stato s) {
    switch (s) {
        case Stato::Occupato:   return 0;
        case Stato::InAttesa:   return 1;
        case Stato::Errore:     return 2;
        case Stato::Completato: return 3;
        case Stato::Inattivo:   return 4;
    }
    return 5;
}

inline Fonte fonteDaStringa(const std::string& s) {
    std::string l = s;
    for (char& c : l) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (l == "antigravity") return Fonte::Antigravity;
    if (l == "claude") return Fonte::Claude;
    if (l == "codex") return Fonte::Codex;
    if (l == "ollama") return Fonte::Ollama;
    if (l == "lmstudio") return Fonte::LmStudio;
    return Fonte::Hook;
}

inline std::wstring nomeFonte(Fonte f) {
    switch (f) {
        case Fonte::Hook:        return L"Hook";
        case Fonte::Antigravity: return L"AGY";
        case Fonte::Claude:      return L"Claude";
        case Fonte::Codex:       return L"Codex";
        case Fonte::Ollama:      return L"Ollama";
        case Fonte::LmStudio:    return L"LM Studio";
        case Fonte::Processo:    return L"Process";
    }
    return L"Agent";
}

inline Stato statoDaStringa(const std::string& s) {
    if (s == "busy") return Stato::Occupato;
    if (s == "waiting") return Stato::InAttesa;
    if (s == "done") return Stato::Completato;
    if (s == "error") return Stato::Errore;
    return Stato::Inattivo;
}

inline const char* stringaDaStato(Stato s) {
    switch (s) {
        case Stato::Occupato:   return "busy";
        case Stato::InAttesa:   return "waiting";
        case Stato::Completato: return "done";
        case Stato::Errore:     return "error";
        case Stato::Inattivo:   return "idle";
    }
    return "idle";
}

inline bool processoVivo(DWORD pid) {
    if (pid == 0) return false;
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
    const DWORD w = WaitForSingleObject(p, 0);
    CloseHandle(p);
    return w == WAIT_TIMEOUT;
}

inline std::wstring leggiTitoloPbtxt(const std::wstring& path) {
    std::string txt;
    if (!leggiFile(path, txt, 65536) || txt.empty()) return L"";
    const size_t pos = txt.find("title:");
    if (pos == std::string::npos) return L"";
    size_t p = pos + 6;
    while (p < txt.size() && (txt[p] == ' ' || txt[p] == '\t')) p++;
    if (p >= txt.size() || txt[p] != '"') return L"";
    p++;
    std::string utf8;
    while (p < txt.size()) {
        const char c = txt[p++];
        if (c == '"') break;
        if (c != '\\') { utf8.push_back(c); continue; }
        if (p >= txt.size()) break;
        const char e = txt[p++];
        if (e >= '0' && e <= '7') {
            unsigned v = (unsigned)(e - '0');
            for (int k = 0; k < 2 && p < txt.size() && txt[p] >= '0' && txt[p] <= '7'; k++) {
                v = v * 8 + (unsigned)(txt[p++] - '0');
            }
            utf8.push_back((char)v);
        } else if (e == 'n') utf8.push_back('\n');
        else if (e == 't') utf8.push_back('\t');
        else if (e == 'r') {}
        else utf8.push_back(e);
    }
    return daUtf8(utf8);
}

inline bool httpGetLocale(INTERNET_PORT porta, const wchar_t* percorso, std::string& out) {
    out.clear();
    HINTERNET s = WinHttpOpen(L"FluidWin-AI", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 250, 250, 250, 250);
    HINTERNET c = WinHttpConnect(s, L"127.0.0.1", porta, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", percorso, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    bool ok = false;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, szStatus = sizeof(status);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &szStatus, WINHTTP_NO_HEADER_INDEX) && status == 200) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail > 0 && out.size() + avail <= 512 * 1024) {
                const size_t base = out.size();
                out.resize(base + avail);
                DWORD rd = 0;
                if (!WinHttpReadData(r, out.data() + base, avail, &rd) || rd == 0) {
                    out.resize(base);
                    break;
                }
                out.resize(base + rd);
            }
            ok = true;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

inline std::wstring leafPath(const std::wstring& p) {
    if (p.empty()) return L"";
    size_t end = p.size();
    while (end > 0 && (p[end - 1] == L'\\' || p[end - 1] == L'/')) end--;
    if (end == 0) return L"";
    const size_t sl = p.find_last_of(L"\\/", end - 1);
    return sl == std::wstring::npos ? p.substr(0, end) : p.substr(sl + 1, end - sl - 1);
}

struct ProcInfo {
    DWORD parentPid = 0;
    std::wstring exeLower;
};

inline std::unordered_map<DWORD, ProcInfo> mappaProcessi() {
    std::unordered_map<DWORD, ProcInfo> res;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return res;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        res[pe.th32ProcessID] = {pe.th32ParentProcessID, minuscolo(pe.szExeFile)};
    }
    CloseHandle(snap);
    return res;
}

inline bool isShellWrapper(const std::wstring& exeLower) {
    return exeLower == L"cmd.exe" || exeLower == L"powershell.exe" || exeLower == L"pwsh.exe" ||
           exeLower == L"conhost.exe" || exeLower == L"sh.exe" || exeLower == L"bash.exe";
}

inline DWORD risolviPidPadreHook() {
    const auto procs = mappaProcessi();
    DWORD cur = GetCurrentProcessId();
    DWORD firstParent = 0;
    std::unordered_set<DWORD> seen;
    for (int depth = 0; depth < 16; depth++) {
        seen.insert(cur);
        auto it = procs.find(cur);
        if (it == procs.end()) break;
        const DWORD parent = it->second.parentPid;
        if (parent == 0 || parent == cur || seen.count(parent)) break;
        if (firstParent == 0) firstParent = parent;
        auto pit = procs.find(parent);
        if (pit == procs.end()) break;
        if (!isShellWrapper(pit->second.exeLower)) return parent;
        cur = parent;
    }
    return firstParent;
}

inline bool finestraValida(HWND h) {
    if (!IsWindowVisible(h)) return false;
    if (GetWindow(h, GW_OWNER) != nullptr) return false;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(h) <= 0) return false;
    wchar_t cls[64] = L"";
    if (GetClassNameW(h, cls, 64) > 0 && wcsncmp(cls, L"FluidWin.", 9) == 0) return false;

    using PfnDwmGetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static const PfnDwmGetWindowAttribute fnDwm = []() -> PfnDwmGetWindowAttribute {
        HMODULE m = GetModuleHandleW(L"dwmapi.dll");
        if (!m) m = LoadLibraryW(L"dwmapi.dll");
        return m ? (PfnDwmGetWindowAttribute)(void*)GetProcAddress(m, "DwmGetWindowAttribute") : nullptr;
    }();
    if (fnDwm) {
        DWORD cloaked = 0;
        if (SUCCEEDED(fnDwm(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked != 0 && !IsIconic(h)) {
            return false;
        }
    }
    return true;
}

}  // namespace dettagli

inline uint64_t stimaFinestraContesto(const std::wstring& modello, Fonte fonte = Fonte::Processo) {
    if (modello.empty()) return 0;
    const std::wstring m = dettagli::minuscolo(modello);
    if (m.find(L"gemini") != std::wstring::npos) return 1000000ULL;
    if (m.find(L"claude") != std::wstring::npos) {
        if (m.find(L"[1m]") != std::wstring::npos || m.find(L"-1m") != std::wstring::npos) return 1000000ULL;
        return 200000ULL;
    }
    if (m.find(L"codex") != std::wstring::npos || m.find(L"gpt") != std::wstring::npos) return 200000ULL;
    if (fonte == Fonte::Antigravity || m.find(L"antigravity") != std::wstring::npos ||
        m.find(L"model_placeholder") != std::wstring::npos) {
        return 1000000ULL;
    }
    return 128000ULL;
}

inline std::wstring formattaModello(const std::wstring& raw) {
    auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    size_t start = 0;
    while (start < raw.size() && isSpace(raw[start])) start++;
    size_t end = raw.size();
    while (end > start && isSpace(raw[end - 1])) end--;
    if (start >= end) return L"";

    std::wstring s = raw.substr(start, end - start);
    for (;;) {
        while (!s.empty() && isSpace(s.back())) s.pop_back();
        if (s.empty()) break;
        const std::wstring low = dettagli::minuscolo(s);
        bool changed = false;

        if (low.size() > 4 && low.compare(low.size() - 4, 4, L"[1m]") == 0) {
            s.resize(s.size() - 4);
            changed = true;
        } else {
            static constexpr const wchar_t* TIERS[] = {L"medium", L"high", L"low", L"minimal", L"default", L"auto"};
            for (const wchar_t* tier : TIERS) {
                const size_t tl = wcslen(tier);
                if (low.size() > tl + 1 && low.compare(low.size() - tl, tl, tier) == 0) {
                    const wchar_t sep = low[low.size() - tl - 1];
                    if (sep == L'-' || sep == L'_' || sep == L':' || sep == L' ') {
                        s.resize(s.size() - tl - 1);
                        changed = true;
                        break;
                    }
                }
            }
        }

        if (!changed && s.size() > 9 && (s[s.size() - 9] == L'-' || s[s.size() - 9] == L'_')) {
            bool allDig = true;
            for (size_t i = s.size() - 8; i < s.size(); i++) {
                if (s[i] < L'0' || s[i] > L'9') { allDig = false; break; }
            }
            if (allDig) {
                s.resize(s.size() - 9);
                changed = true;
            }
        }
        if (!changed) break;
    }

    while (!s.empty() && isSpace(s.back())) s.pop_back();
    if (s.empty()) return raw.substr(start, end - start);

    std::vector<std::wstring> rawToks;
    size_t i = 0;
    auto isSep = [](wchar_t c) { return c == L'-' || c == L'_' || c == L':' || c == L'/' || c == L' ' || c == L'\t'; };
    while (i < s.size()) {
        while (i < s.size() && isSep(s[i])) i++;
        const size_t t0 = i;
        while (i < s.size() && !isSep(s[i])) i++;
        if (i > t0) rawToks.push_back(s.substr(t0, i - t0));
    }
    if (rawToks.empty()) return s;

    auto isSingleDigit = [](const std::wstring& w) {
        return w.size() == 1 && w[0] >= L'0' && w[0] <= L'9';
    };
    std::vector<std::wstring> toks;
    toks.reserve(rawToks.size());
    for (size_t k = 0; k < rawToks.size();) {
        if (k + 1 < rawToks.size() && isSingleDigit(rawToks[k]) && isSingleDigit(rawToks[k + 1])) {
            toks.push_back(rawToks[k] + L"." + rawToks[k + 1]);
            k += 2;
        } else {
            toks.push_back(std::move(rawToks[k]));
            k++;
        }
    }

    auto formatTok = [](const std::wstring& t) -> std::wstring {
        const std::wstring low = dettagli::minuscolo(t);
        static constexpr const wchar_t* ACR[] = {
            L"gpt", L"ai", L"llm", L"oss", L"mlx", L"gguf", L"awq", L"api", L"ui", L"cli", L"fp8", L"fp16", L"bf16", L"int4", L"int8"
        };
        for (const wchar_t* a : ACR) {
            if (low == a) {
                std::wstring up = low;
                for (wchar_t& c : up) c = (wchar_t)towupper((wint_t)c);
                return up;
            }
        }
        if (low.size() >= 2 && low.back() == L'b') {
            bool hasDigit = false, valid = true;
            for (size_t j = 0; j + 1 < low.size(); j++) {
                if (low[j] >= L'0' && low[j] <= L'9') hasDigit = true;
                else if (low[j] != L'.') { valid = false; break; }
            }
            if (hasDigit && valid) {
                std::wstring sz = low;
                sz.back() = L'B';
                return sz;
            }
        }

        bool hasAlpha = false, allUpper = true;
        for (wchar_t c : t) {
            if (iswalpha((wint_t)c)) {
                hasAlpha = true;
                if (iswlower((wint_t)c)) allUpper = false;
            }
        }
        std::wstring out = (hasAlpha && allUpper) ? low : t;
        if (!out.empty() && iswlower((wint_t)out[0])) out[0] = (wchar_t)towupper((wint_t)out[0]);
        return out;
    };

    std::wstring res;
    std::wstring prev;
    for (const auto& t : toks) {
        std::wstring ft = formatTok(t);
        if (!res.empty()) {
            const bool gptVer = (prev == L"GPT" && !ft.empty() && ft[0] >= L'0' && ft[0] <= L'9');
            res.push_back(gptVer ? L'-' : L' ');
        }
        res += ft;
        prev = std::move(ft);
    }
    return res;
}

inline std::wstring cartellaHook() {
    const std::wstring base = dettagli::envDir(L"LOCALAPPDATA");
    if (base.empty()) return L"";
    return base + L"\\FluidWin\\ai-agents";
}

inline bool gestisciHookCli(int argc, wchar_t** argv) {
    if (!argv || argc < 3 || wcscmp(argv[1], L"--ai-hook") != 0) return false;

    const std::wstring sourceW = argv[2] ? argv[2] : L"";
    const std::wstring eventArg = (argc >= 4 && argv[3]) ? argv[3] : L"";

    std::string stdinData;
    const HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn && hIn != INVALID_HANDLE_VALUE) {
        const DWORD ft = GetFileType(hIn);
        if (ft != FILE_TYPE_CHAR && ft != FILE_TYPE_UNKNOWN) {
            constexpr size_t MAX_IN = 256 * 1024;
            char buf[4096];
            DWORD nr = 0;
            while (stdinData.size() < MAX_IN && ReadFile(hIn, buf, (DWORD)std::min<size_t>(sizeof(buf), MAX_IN - stdinData.size()), &nr, nullptr) && nr > 0) {
                stdinData.append(buf, nr);
            }
        }
    }

    dettagli::Json inJson;
    const bool hasJson = !stdinData.empty() && dettagli::parseJson(stdinData, inJson) && inJson.tipo == dettagli::Json::Obj;

    std::string id = hasJson ? inJson.str("conversationId") : "";
    if (id.empty() && hasJson) id = inJson.str("session_id");
    if (id.empty() && hasJson) id = inJson.str("sessionId");
    if (id.empty() && hasJson) id = inJson.str("id");
    if (id.empty()) id = "default";

    std::string ev = hasJson ? inJson.str("hookEventName") : "";
    if (ev.empty() && hasJson) ev = inJson.str("hook_event_name");
    if (ev.empty()) ev = dettagli::inUtf8(eventArg);

    std::wstring consoleTitle;
    DWORD consolePid = 0;
    const std::wstring dir = cartellaHook();
    if (!dir.empty()) {
        const std::wstring fileName = dettagli::sanificaId(sourceW, L"hook") + L"-" +
                                      dettagli::sanificaId(dettagli::daUtf8(id), L"default") + L".json";
        const std::wstring dst = dir + L"\\" + fileName;

        if (ev == "SessionEnd") {
            DeleteFileW(dst.c_str());
        } else {
            dettagli::Json prev;
            std::string prevRaw;
            const bool hasPrev = dettagli::leggiFile(dst, prevRaw, 128 * 1024) &&
                                 dettagli::parseJson(prevRaw, prev) && prev.tipo == dettagli::Json::Obj;

            std::string model = hasJson ? inJson.str("modelName") : "";
            if (model.empty() && hasJson) model = inJson.str("model");
            if (model.empty() && hasPrev) model = prev.str("model");

            std::string cwd = hasJson ? inJson.str("cwd") : "";
            if (cwd.empty() && hasPrev) cwd = prev.str("cwd");

            std::string title;
            if (dettagli::minuscolo(sourceW) == L"antigravity" && id != "default") {
                const std::wstring profile = dettagli::envDir(L"USERPROFILE");
                if (!profile.empty()) {
                    const std::wstring ann = dettagli::leggiTitoloPbtxt(
                        profile + L"\\.gemini\\antigravity-cli\\annotations\\" + dettagli::daUtf8(id) + L".pbtxt");
                    if (!ann.empty()) title = dettagli::inUtf8(ann);
                }
            }
            if (title.empty() && hasJson) title = inJson.str("lastUserInput");
            if (title.empty() && hasJson) title = inJson.str("prompt");
            if (title.size() > 120) {
                title.resize(117);
                title += "...";
            }
            if (title.empty() && hasPrev) title = prev.str("title");

            std::string tool;
            if (hasJson) {
                if (const auto* tc = inJson.trova("toolCall"); tc && tc->tipo == dettagli::Json::Obj) tool = tc->str("name");
                if (tool.empty()) tool = inJson.str("tool_name");
            }
            if (tool.empty() && hasPrev && ev != "PostToolUse" && ev != "PostInvocation" &&
                ev != "PreInvocation" && ev != "UserPromptSubmit" && ev != "Stop" && ev != "SessionStart") {
                tool = prev.str("tool");
            }

            std::string termReason = hasJson ? inJson.str("terminationReason") : "";
            std::string errMsg;
            if (hasJson) {
                if (const auto* ep = inJson.trova("error")) {
                    if (ep->tipo == dettagli::Json::Str) errMsg = ep->s;
                    else if (ep->tipo == dettagli::Json::Bool && ep->b) errMsg = "error";
                    else if (ep->tipo == dettagli::Json::Obj && !ep->obj.empty()) errMsg = "error";
                }
            }

            Stato st = hasPrev ? dettagli::statoDaStringa(prev.str("status")) : Stato::Inattivo;
            if (ev == "SessionStart") {
                st = Stato::Inattivo;
                tool.clear();
            } else if (ev == "PreInvocation" || ev == "UserPromptSubmit" || ev == "PreToolUse") {
                st = Stato::Occupato;
            } else if (ev == "PostToolUse" || ev == "PostInvocation") {
                st = Stato::Occupato;
                tool.clear();
            } else if (ev == "Notification") {
                st = Stato::InAttesa;
            } else if (ev == "Stop") {
                st = (!errMsg.empty() || termReason == "ERROR") ? Stato::Errore : Stato::Completato;
                tool.clear();
            }

            uint64_t pidVal = hasJson ? inJson.u64("pid") : 0;
            if (pidVal == 0) pidVal = dettagli::risolviPidPadreHook();
            if (pidVal == 0 && hasPrev) pidVal = prev.u64("pid");

            dettagli::Json rec = dettagli::Json::makeObj();
            rec.imposta("id", dettagli::Json::makeStr(id));
            rec.imposta("source", dettagli::Json::makeStr(dettagli::inUtf8(sourceW)));
            rec.imposta("status", dettagli::Json::makeStr(dettagli::stringaDaStato(st)));
            if (!ev.empty()) rec.imposta("event", dettagli::Json::makeStr(ev));
            if (!title.empty()) rec.imposta("title", dettagli::Json::makeStr(title));
            if (!model.empty()) rec.imposta("model", dettagli::Json::makeStr(model));
            if (!tool.empty()) rec.imposta("tool", dettagli::Json::makeStr(tool));
            if (!cwd.empty()) rec.imposta("cwd", dettagli::Json::makeStr(cwd));
            if (pidVal > 0) rec.imposta("pid", dettagli::Json::makeNum((double)pidVal));
            rec.imposta("updatedAt", dettagli::Json::makeNum(dettagli::adessoEpoch()));

            dettagli::scriviAtomico(dst, dettagli::dumpJson(rec, false));
            consolePid = (DWORD)pidVal;
            if (!title.empty()) consoleTitle = dettagli::daUtf8(title);
            else if (!cwd.empty()) consoleTitle = dettagli::leafPath(dettagli::daUtf8(cwd));
        }
    }

    const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) != FILE_TYPE_UNKNOWN) {
        DWORD wr = 0;
        WriteFile(hOut, "{}\n", 3, &wr, nullptr);
    }
    if (!consoleTitle.empty()) {
        const bool agganciato = (consolePid > 0 && AttachConsole(consolePid)) || AttachConsole(ATTACH_PARENT_PROCESS);
        if (agganciato) {
            SetConsoleTitleW(consoleTitle.c_str());
            FreeConsole();
        }
    }
    return true;
}

// ---- installazione opt-in degli hook CLI ----

inline bool hookAntigravityInstallati() {
    const std::wstring profile = dettagli::envDir(L"USERPROFILE");
    if (profile.empty()) return false;
    std::string raw;
    if (!dettagli::leggiFile(profile + L"\\.gemini\\config\\hooks.json", raw, 512 * 1024) || raw.empty()) return false;
    dettagli::Json root;
    if (!dettagli::parseJson(raw, root) || root.tipo != dettagli::Json::Obj) return false;
    const auto* entry = root.trova("fluidwin-ai-monitor");
    if (!entry || entry->tipo != dettagli::Json::Obj) return false;
    if (const auto* en = entry->trova("enabled"); en && en->tipo == dettagli::Json::Bool && !en->b) return false;
    return true;
}

inline bool installaHookAntigravity(bool abilita) {
    const std::wstring profile = dettagli::envDir(L"USERPROFILE");
    if (profile.empty()) return false;
    const std::wstring path = profile + L"\\.gemini\\config\\hooks.json";
    dettagli::Json root = dettagli::Json::makeObj();
    std::string raw;
    if (dettagli::leggiFile(path, raw, 512 * 1024) && !raw.empty()) {
        if (!dettagli::parseJson(raw, root) || root.tipo != dettagli::Json::Obj) return false;
    }

    if (!abilita) {
        root.rimuovi("fluidwin-ai-monitor");
        return dettagli::scriviAtomico(path, dettagli::dumpJson(root, true));
    }

    const std::string exe = dettagli::inUtf8(dettagli::percorsoExe());
    if (exe.empty()) return false;
    auto handler = [&](const char* ev) {
        dettagli::Json h = dettagli::Json::makeObj();
        h.imposta("type", dettagli::Json::makeStr("command"));
        h.imposta("command", dettagli::Json::makeStr("\"" + exe + "\" --ai-hook antigravity " + ev));
        h.imposta("timeout", dettagli::Json::makeNum(5));
        return h;
    };
    auto flatArr = [&](const char* ev) {
        dettagli::Json a = dettagli::Json::makeArr();
        a.arr.push_back(handler(ev));
        return a;
    };
    auto groupedArr = [&](const char* ev) {
        dettagli::Json grp = dettagli::Json::makeObj();
        grp.imposta("matcher", dettagli::Json::makeStr("*"));
        grp.imposta("hooks", flatArr(ev));
        dettagli::Json a = dettagli::Json::makeArr();
        a.arr.push_back(std::move(grp));
        return a;
    };

    dettagli::Json spec = dettagli::Json::makeObj();
    spec.imposta("SessionStart", flatArr("SessionStart"));
    spec.imposta("PreInvocation", flatArr("PreInvocation"));
    spec.imposta("PostInvocation", flatArr("PostInvocation"));
    spec.imposta("PostToolUse", groupedArr("PostToolUse"));
    spec.imposta("Stop", flatArr("Stop"));
    root.imposta("fluidwin-ai-monitor", std::move(spec));
    return dettagli::scriviAtomico(path, dettagli::dumpJson(root, true));
}

inline bool hookClaudeInstallati() {
    const std::wstring profile = dettagli::envDir(L"USERPROFILE");
    if (profile.empty()) return false;
    std::string raw;
    if (!dettagli::leggiFile(profile + L"\\.claude\\settings.json", raw, 512 * 1024) || raw.empty()) return false;
    dettagli::Json root;
    if (!dettagli::parseJson(raw, root) || root.tipo != dettagli::Json::Obj) return false;
    const auto* hooks = root.trova("hooks");
    if (!hooks || hooks->tipo != dettagli::Json::Obj) return false;
    for (const auto& [ev, arr] : hooks->obj) {
        (void)ev;
        if (arr.tipo != dettagli::Json::Arr) continue;
        for (const auto& grp : arr.arr) {
            if (grp.tipo != dettagli::Json::Obj) continue;
            const auto* hs = grp.trova("hooks");
            if (!hs || hs->tipo != dettagli::Json::Arr) continue;
            for (const auto& h : hs->arr) {
                if (h.tipo == dettagli::Json::Obj && h.str("command").find("--ai-hook claude") != std::string::npos) return true;
            }
        }
    }
    return false;
}

inline bool installaHookClaude(bool abilita) {
    const std::wstring profile = dettagli::envDir(L"USERPROFILE");
    if (profile.empty()) return false;
    const std::wstring path = profile + L"\\.claude\\settings.json";
    dettagli::Json root = dettagli::Json::makeObj();
    std::string raw;
    if (dettagli::leggiFile(path, raw, 512 * 1024) && !raw.empty()) {
        if (!dettagli::parseJson(raw, root) || root.tipo != dettagli::Json::Obj) return false;
    }

    if (auto* hooks = root.trovaMut("hooks"); hooks && hooks->tipo == dettagli::Json::Obj) {
        for (auto itEv = hooks->obj.begin(); itEv != hooks->obj.end();) {
            auto& arr = itEv->second;
            if (arr.tipo == dettagli::Json::Arr) {
                for (auto itGrp = arr.arr.begin(); itGrp != arr.arr.end();) {
                    if (auto* hs = itGrp->trovaMut("hooks"); hs && hs->tipo == dettagli::Json::Arr) {
                        hs->arr.erase(std::remove_if(hs->arr.begin(), hs->arr.end(), [](const dettagli::Json& h) {
                            return h.tipo == dettagli::Json::Obj && h.str("command").find("--ai-hook claude") != std::string::npos;
                        }), hs->arr.end());
                        if (hs->arr.empty()) { itGrp = arr.arr.erase(itGrp); continue; }
                    }
                    ++itGrp;
                }
            }
            if (arr.tipo == dettagli::Json::Arr && arr.arr.empty()) itEv = hooks->obj.erase(itEv);
            else ++itEv;
        }
        if (hooks->obj.empty()) root.rimuovi("hooks");
    }

    if (!abilita) {
        return dettagli::scriviAtomico(path, dettagli::dumpJson(root, true));
    }

    const std::string exe = dettagli::inUtf8(dettagli::percorsoExe());
    if (exe.empty()) return false;
    auto* hooks = root.trovaMut("hooks");
    if (!hooks || hooks->tipo != dettagli::Json::Obj) {
        root.imposta("hooks", dettagli::Json::makeObj());
        hooks = root.trovaMut("hooks");
    }

    const char* eventi[] = {"SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Notification", "Stop", "SessionEnd"};
    for (const char* ev : eventi) {
        auto* arr = hooks->trovaMut(ev);
        if (!arr || arr->tipo != dettagli::Json::Arr) {
            hooks->imposta(ev, dettagli::Json::makeArr());
            arr = hooks->trovaMut(ev);
        }
        dettagli::Json h = dettagli::Json::makeObj();
        h.imposta("type", dettagli::Json::makeStr("command"));
        h.imposta("command", dettagli::Json::makeStr("\"" + exe + "\" --ai-hook claude " + ev));
        h.imposta("timeout", dettagli::Json::makeNum(5));

        dettagli::Json grp = dettagli::Json::makeObj();
        if (strcmp(ev, "PreToolUse") == 0 || strcmp(ev, "PostToolUse") == 0) {
            grp.imposta("matcher", dettagli::Json::makeStr("*"));
        }
        dettagli::Json hs = dettagli::Json::makeArr();
        hs.arr.push_back(std::move(h));
        grp.imposta("hooks", std::move(hs));
        arr->arr.push_back(std::move(grp));
    }
    return dettagli::scriviAtomico(path, dettagli::dumpJson(root, true));
}

// ---- monitor in sottofondo ----

class Monitor {
public:
    std::atomic<uint64_t> versione{0};

    Monitor() { InitializeSRWLock(&lock); }
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    ~Monitor() { ferma(); }

    void avvia(HWND hwndNotifica = nullptr, UINT msgNotifica = 0) {
        if (thread) return;
        notificaHwnd = hwndNotifica;
        notificaMsg = msgNotifica;
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent) return;
        thread = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
        if (thread) SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    }

    bool ferma(DWORD ms = 2000) {
        if (!thread) {
            if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
            return true;
        }
        if (stopEvent) SetEvent(stopEvent);
        const bool ok = WaitForSingleObject(thread, ms) == WAIT_OBJECT_0;
        if (ok) {
            CloseHandle(thread);
            thread = nullptr;
            if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
        }
        return ok;
    }

    Riepilogo copia() const {
        AcquireSRWLockShared(&lock);
        Riepilogo r = stato;
        ReleaseSRWLockShared(&lock);
        return r;
    }

    void pulisciCompletati() {
        const std::wstring dir = cartellaHook();
        if (!dir.empty()) {
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const std::wstring p = dir + L"\\" + fd.cFileName;
                    std::string raw;
                    dettagli::Json j;
                    if (dettagli::leggiFile(p, raw, 128 * 1024) && dettagli::parseJson(raw, j) && j.tipo == dettagli::Json::Obj) {
                        const std::string st = j.str("status");
                        if (st == "done" || st == "error") DeleteFileW(p.c_str());
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }

        AcquireSRWLockExclusive(&lock);
        const double now = dettagli::adessoEpoch();
        for (const auto& a : stato.agenti) {
            if (!a.daHook && (a.stato == Stato::Completato || a.stato == Stato::Errore)) {
                completatiRimossi[a.id] = std::max(a.aggiornatoT, now);
            }
        }
        stato.agenti.erase(std::remove_if(stato.agenti.begin(), stato.agenti.end(), [](const Agente& a) {
            return a.daHook && (a.stato == Stato::Completato || a.stato == Stato::Errore);
        }), stato.agenti.end());
        for (auto& a : stato.agenti) {
            if (!a.daHook && (a.stato == Stato::Completato || a.stato == Stato::Errore)) a.stato = Stato::Inattivo;
        }
        ricalcolaRiepilogo(stato);
        ReleaseSRWLockExclusive(&lock);
        versione.fetch_add(1, std::memory_order_relaxed);
    }

private:
    struct TranscriptCache {
        uint64_t size = 0;
        FILETIME mtime{};
        uint64_t offset = 0;
        uint64_t tokIn = 0, tokOut = 0, tokCache = 0, lastCtxTok = 0;
        std::string model, lastType, lastTool;
        bool lastHasToolCalls = false;
    };

    struct CpuSample {
        uint64_t procTime = 0;
        double cpuPct = 0.0;
        double primoAvvistamento = 0.0;
    };

    mutable SRWLOCK lock = SRWLOCK_INIT;
    Riepilogo stato;
    HANDLE thread = nullptr;
    HANDLE stopEvent = nullptr;
    HWND notificaHwnd = nullptr;
    UINT notificaMsg = 0;

    std::unordered_map<std::wstring, TranscriptCache> transcriptCache;
    std::unordered_map<std::wstring, double> completatiRimossi;
    std::unordered_map<DWORD, CpuSample> cpuSamples;
    uint64_t lastCpuWall100ns = 0;

    static void ricalcolaRiepilogo(Riepilogo& r) {
        std::sort(r.agenti.begin(), r.agenti.end(), [](const Agente& a, const Agente& b) {
            const int pa = dettagli::prioritaStato(a.stato), pb = dettagli::prioritaStato(b.stato);
            if (pa != pb) return pa < pb;
            if (a.aggiornatoT != b.aggiornatoT) return a.aggiornatoT > b.aggiornatoT;
            return a.id < b.id;
        });

        r.statoGlobale = Stato::Inattivo;
        r.attivi = (int)r.agenti.size();
        r.occupati = 0;
        r.inAttesa = 0;
        r.completati = 0;
        r.tokInTot = 0;
        r.tokOutTot = 0;
        r.tokCacheTot = 0;
        for (const auto& a : r.agenti) {
            if (dettagli::prioritaStato(a.stato) < dettagli::prioritaStato(r.statoGlobale)) r.statoGlobale = a.stato;
            if (a.stato == Stato::Occupato) r.occupati++;
            else if (a.stato == Stato::InAttesa) r.inAttesa++;
            else if (a.stato == Stato::Completato) r.completati++;
            r.tokInTot += a.tokIn;
            r.tokOutTot += a.tokOut;
            r.tokCacheTot += a.tokCache;
        }
    }

    static bool ugualeRiepilogo(const Riepilogo& a, const Riepilogo& b) {
        if (a.statoGlobale != b.statoGlobale || a.attivi != b.attivi || a.occupati != b.occupati ||
            a.inAttesa != b.inAttesa || a.completati != b.completati ||
            a.tokInTot != b.tokInTot || a.tokOutTot != b.tokOutTot || a.tokCacheTot != b.tokCacheTot ||
            a.agenti.size() != b.agenti.size()) return false;
        for (size_t i = 0; i < a.agenti.size(); i++) {
            const Agente& x = a.agenti[i], & y = b.agenti[i];
            if (x.id != y.id || x.nome != y.nome || x.titolo != y.titolo || x.modello != y.modello ||
                x.strumento != y.strumento || x.cwd != y.cwd || x.fonte != y.fonte || x.stato != y.stato ||
                x.tokIn != y.tokIn || x.tokOut != y.tokOut || x.tokCache != y.tokCache ||
                x.ctxTok != y.ctxTok || x.ctxMax != y.ctxMax ||
                x.pid != y.pid || x.daHook != y.daHook) return false;
        }
        return true;
    }

    static void leggiDropbox(double now, std::vector<Agente>& out) {
        const std::wstring dir = cartellaHook();
        if (dir.empty()) return;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            std::string raw;
            dettagli::Json j;
            if (!dettagli::leggiFile(p, raw, 128 * 1024) || !dettagli::parseJson(raw, j) || j.tipo != dettagli::Json::Obj) continue;

            const double updated = j.num("updatedAt", dettagli::fileTimeAEpoch(fd.ftLastWriteTime));
            const DWORD pid = (DWORD)j.u64("pid", 0);
            if ((updated > 0 && now - updated > 15.0 * 60.0) || (pid > 0 && !dettagli::processoVivo(pid))) {
                DeleteFileW(p.c_str());
                continue;
            }

            Agente a;
            a.daHook = true;
            a.id = dettagli::daUtf8(j.str("id"));
            if (a.id.empty()) a.id = fd.cFileName;
            a.fonte = dettagli::fonteDaStringa(j.str("source"));
            a.nome = dettagli::nomeFonte(a.fonte);
            a.titolo = dettagli::daUtf8(j.str("title"));
            a.modello = dettagli::daUtf8(j.str("model"));
            a.ctxMax = stimaFinestraContesto(a.modello, a.fonte);
            a.strumento = dettagli::daUtf8(j.str("tool"));
            a.cwd = dettagli::daUtf8(j.str("cwd"));
            a.stato = dettagli::statoDaStringa(j.str("status"));
            a.pid = pid;
            a.aggiornatoT = updated;
            out.push_back(std::move(a));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    void aggiornaTranscriptAntigravity(const std::wstring& uuid, const std::wstring& path, TranscriptCache& tc, double& mtimeEpoch) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
        const uint64_t fsize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        mtimeEpoch = dettagli::fileTimeAEpoch(fad.ftLastWriteTime);
        if (fsize == tc.size && CompareFileTime(&fad.ftLastWriteTime, &tc.mtime) == 0) return;

        if (fsize < tc.offset) tc = TranscriptCache{};
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;

        LARGE_INTEGER off{};
        off.QuadPart = (LONGLONG)tc.offset;
        if (SetFilePointerEx(f, off, nullptr, FILE_BEGIN)) {
            const uint64_t daLeggere = fsize > tc.offset ? std::min<uint64_t>(fsize - tc.offset, 16ULL * 1024 * 1024) : 0;
            if (daLeggere > 0) {
                std::string chunk((size_t)daLeggere, '\0');
                DWORD nr = 0, tot = 0;
                while (tot < daLeggere && ReadFile(f, chunk.data() + tot, (DWORD)(daLeggere - tot), &nr, nullptr) && nr > 0) {
                    tot += nr;
                }
                chunk.resize(tot);
                const size_t lastNl = chunk.rfind('\n');
                if (lastNl != std::string::npos) {
                    tc.offset += lastNl + 1;
                    size_t pos = 0;
                    while (pos <= lastNl) {
                        const size_t nl = chunk.find('\n', pos);
                        const size_t len = (nl == std::string::npos) ? (lastNl - pos) : (nl - pos);
                        std::string line = chunk.substr(pos, len);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        pos = (nl == std::string::npos) ? lastNl + 1 : nl + 1;
                        if (line.empty()) continue;

                        dettagli::Json obj;
                        if (!dettagli::parseJson(line, obj) || obj.tipo != dettagli::Json::Obj) continue;
                        const std::string tp = obj.str("type");
                        if (!tp.empty()) tc.lastType = tp;
                        if (tp == "PLANNER_RESPONSE") {
                            const std::string m = obj.str("model");
                            if (!m.empty()) tc.model = m;
                            const uint64_t inTok = obj.u64("input_tokens");
                            const uint64_t cacheTok = obj.u64("cache_read_tokens");
                            tc.tokIn += inTok;
                            tc.tokCache += cacheTok;
                            tc.tokOut += obj.u64("output_tokens");
                            if (inTok + cacheTok > 0) tc.lastCtxTok = inTok + cacheTok;
                            if (const auto* calls = obj.trova("tool_calls"); calls && calls->tipo == dettagli::Json::Arr && !calls->arr.empty()) {
                                tc.lastHasToolCalls = true;
                                tc.lastTool = calls->arr.back().str("name");
                            } else {
                                tc.lastHasToolCalls = false;
                                tc.lastTool.clear();
                            }
                        } else if (tp != "USER_INPUT") {
                            tc.lastHasToolCalls = false;
                        }
                    }
                }
            }
        }
        CloseHandle(f);
        tc.size = fsize;
        tc.mtime = fad.ftLastWriteTime;
        (void)uuid;
    }

    bool leggiAntigravity(double now, const std::vector<DWORD>& agyPids,
                          std::vector<Agente>& hookAgenti, std::vector<Agente>& out) {
        const std::wstring profile = dettagli::envDir(L"USERPROFILE");
        if (profile.empty()) return false;
        const std::wstring root = profile + L"\\.gemini\\antigravity-cli";
        const std::wstring presDir = root + L"\\presence";

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((presDir + L"\\*.lock").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;

        std::unordered_set<DWORD> usedPids;
        for (const auto& ha : hookAgenti) {
            if (ha.pid != 0) usedPids.insert(ha.pid);
        }
        std::vector<DWORD> freePids;
        freePids.reserve(agyPids.size());
        for (DWORD p : agyPids) {
            if (!usedPids.count(p)) freePids.push_back(p);
        }

        struct PassiveAg {
            double lockCreated = 0.0;
            Agente a;
        };
        std::vector<PassiveAg> passivi;
        std::vector<Agente*> hookSenzaPid;
        std::unordered_set<std::wstring> liveIds;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring lockPath = presDir + L"\\" + fd.cFileName;
            HANDLE lk = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lk != INVALID_HANDLE_VALUE) {
                CloseHandle(lk);
                continue;
            }
            if (GetLastError() != ERROR_SHARING_VIOLATION) continue;

            std::wstring uuid = fd.cFileName;
            if (uuid.size() > 5) uuid.resize(uuid.size() - 5);
            liveIds.insert(uuid);

            const std::wstring annTitle = dettagli::leggiTitoloPbtxt(root + L"\\annotations\\" + uuid + L".pbtxt");
            const std::wstring trPath = root + L"\\brain\\" + uuid + L"\\.system_generated\\logs\\transcript.jsonl";
            double mtimeEpoch = dettagli::fileTimeAEpoch(fd.ftLastWriteTime);
            TranscriptCache& tc = transcriptCache[uuid];
            aggiornaTranscriptAntigravity(uuid, trPath, tc, mtimeEpoch);

            const double age = std::max(0.0, now - mtimeEpoch);
            Stato st = Stato::Inattivo;
            if (tc.lastType == "PLANNER_RESPONSE" && !tc.lastHasToolCalls && age >= 2.5) {
                st = age < 120.0 ? Stato::Completato : Stato::Inattivo;
            } else if ((tc.lastType == "PLANNER_RESPONSE" && tc.lastHasToolCalls) ||
                       tc.lastType == "GENERIC" || tc.lastType == "SYSTEM_MESSAGE" ||
                       tc.lastType == "USER_INPUT" || age < 3.5) {
                st = Stato::Occupato;
            }

            if (st == Stato::Occupato) {
                AcquireSRWLockExclusive(&lock);
                completatiRimossi.erase(uuid);
                ReleaseSRWLockExclusive(&lock);
            } else if (st == Stato::Completato) {
                AcquireSRWLockShared(&lock);
                auto it = completatiRimossi.find(uuid);
                const bool nascosto = (it != completatiRimossi.end() && mtimeEpoch <= it->second);
                ReleaseSRWLockShared(&lock);
                if (nascosto) st = Stato::Inattivo;
            }

            Agente* hook = nullptr;
            for (auto& ha : hookAgenti) {
                if (ha.id == uuid) { hook = &ha; break; }
            }
            if (hook) {
                hook->tokIn = std::max(hook->tokIn, tc.tokIn);
                hook->tokOut = std::max(hook->tokOut, tc.tokOut);
                hook->tokCache = std::max(hook->tokCache, tc.tokCache);
                if (tc.lastCtxTok > 0) hook->ctxTok = tc.lastCtxTok;
                if (hook->modello.empty() && !tc.model.empty()) hook->modello = dettagli::daUtf8(tc.model);
                if (!annTitle.empty()) hook->titolo = annTitle;
                if (hook->fonte == Fonte::Hook) { hook->fonte = Fonte::Antigravity; hook->nome = L"AGY"; }
                hook->ctxMax = !hook->modello.empty() ? stimaFinestraContesto(hook->modello, hook->fonte)
                                                      : (hook->ctxTok > 0 ? 1000000ULL : 0ULL);
                if (hook->pid == 0) hookSenzaPid.push_back(hook);
                continue;
            }

            Agente a;
            a.id = uuid;
            a.fonte = Fonte::Antigravity;
            a.nome = L"AGY";
            a.titolo = !annTitle.empty() ? annTitle : uuid.substr(0, std::min<size_t>(uuid.size(), 8));
            a.modello = dettagli::daUtf8(tc.model);
            if (st == Stato::Occupato && !tc.lastTool.empty()) a.strumento = dettagli::daUtf8(tc.lastTool);
            a.stato = st;
            a.tokIn = tc.tokIn;
            a.tokOut = tc.tokOut;
            a.tokCache = tc.tokCache;
            a.ctxTok = tc.lastCtxTok;
            a.ctxMax = !a.modello.empty() ? stimaFinestraContesto(a.modello, a.fonte)
                                          : (a.ctxTok > 0 ? 1000000ULL : 0ULL);
            a.aggiornatoT = mtimeEpoch;
            passivi.push_back({dettagli::fileTimeAEpoch(fd.ftCreationTime), std::move(a)});
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        std::sort(passivi.begin(), passivi.end(), [](const PassiveAg& x, const PassiveAg& y) {
            if (x.lockCreated != y.lockCreated) return x.lockCreated < y.lockCreated;
            return x.a.id < y.a.id;
        });

        size_t freeIdx = 0;
        size_t wrapIdx = 0;
        auto prendiAgyPid = [&]() -> DWORD {
            if (freeIdx < freePids.size()) return freePids[freeIdx++];
            if (!agyPids.empty()) return agyPids[wrapIdx++ % agyPids.size()];
            return 0;
        };
        for (Agente* hk : hookSenzaPid) {
            if (hk->pid == 0) hk->pid = prendiAgyPid();
        }
        for (auto& p : passivi) {
            if (p.a.pid == 0) p.a.pid = prendiAgyPid();
            out.push_back(std::move(p.a));
        }

        for (auto it = transcriptCache.begin(); it != transcriptCache.end();) {
            if (!liveIds.count(it->first)) it = transcriptCache.erase(it);
            else ++it;
        }
        return !liveIds.empty();
    }

    void leggiClaudeJsonl(const std::wstring& path, const std::wstring& stem, double mtimeEpoch, double now,
                          std::vector<Agente>& hookAgenti, std::vector<Agente>& out) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER fsz{};
        if (!GetFileSizeEx(f, &fsz) || fsz.QuadPart <= 0) { CloseHandle(f); return; }

        constexpr uint64_t TAIL_MAX = 65536;
        const bool partial = (uint64_t)fsz.QuadPart > TAIL_MAX;
        if (partial) {
            LARGE_INTEGER off{};
            off.QuadPart = fsz.QuadPart - (LONGLONG)TAIL_MAX;
            SetFilePointerEx(f, off, nullptr, FILE_BEGIN);
        }
        const size_t toRead = (size_t)std::min<LONGLONG>(fsz.QuadPart, (LONGLONG)TAIL_MAX);
        std::string tail(toRead, '\0');
        DWORD nr = 0, tot = 0;
        while (tot < toRead && ReadFile(f, tail.data() + tot, (DWORD)(toRead - tot), &nr, nullptr) && nr > 0) tot += nr;
        tail.resize(tot);
        CloseHandle(f);

        size_t pos = 0;
        if (partial) {
            const size_t firstNl = tail.find('\n');
            if (firstNl == std::string::npos) return;
            pos = firstNl + 1;
        }

        struct UsageRec { uint64_t in = 0, out = 0, cache = 0; bool assistant = false; };
        std::vector<std::pair<std::string, UsageRec>> usages;
        std::string sessionId, cwd, model, title, lastType, lastStopReason, lastTool;
        bool lastHasToolUse = false;

        while (pos < tail.size()) {
            const size_t nl = tail.find('\n', pos);
            const size_t len = (nl == std::string::npos) ? (tail.size() - pos) : (nl - pos);
            std::string line = tail.substr(pos, len);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pos = (nl == std::string::npos) ? tail.size() : nl + 1;
            if (line.empty()) continue;

            dettagli::Json obj;
            if (!dettagli::parseJson(line, obj) || obj.tipo != dettagli::Json::Obj) continue;
            if (sessionId.empty()) sessionId = obj.str("sessionId");
            if (const std::string c = obj.str("cwd"); !c.empty()) cwd = c;
            if (const std::string sl = obj.str("slug"); !sl.empty()) title = sl;
            if (const std::string sm = obj.str("summary"); !sm.empty()) title = sm;

            const std::string tp = obj.str("type");
            if (!tp.empty()) lastType = tp;
            if (const auto* msg = obj.trova("message"); msg && msg->tipo == dettagli::Json::Obj) {
                const std::string m = msg->str("model");
                if (!m.empty()) model = m;
                lastStopReason = msg->str("stop_reason");
                lastHasToolUse = (lastStopReason == "tool_use");
                lastTool.clear();
                if (const auto* content = msg->trova("content"); content && content->tipo == dettagli::Json::Arr) {
                    for (const auto& blk : content->arr) {
                        if (blk.tipo == dettagli::Json::Obj && blk.str("type") == "tool_use") {
                            lastHasToolUse = true;
                            lastTool = blk.str("name");
                        }
                    }
                }
                if (const auto* u = msg->trova("usage"); u && u->tipo == dettagli::Json::Obj) {
                    UsageRec ur;
                    ur.in = u->u64("input_tokens");
                    ur.out = u->u64("output_tokens");
                    ur.cache = u->u64("cache_read_input_tokens") + u->u64("cache_creation_input_tokens");
                    ur.assistant = (tp == "assistant");
                    std::string mid = msg->str("id");
                    if (mid.empty()) mid = std::to_string(usages.size());
                    auto it = std::find_if(usages.begin(), usages.end(), [&](const auto& p) { return p.first == mid; });
                    if (it != usages.end()) it->second = ur;
                    else usages.emplace_back(mid, ur);
                }
            }
        }

        if (sessionId.empty()) sessionId = dettagli::inUtf8(stem);
        uint64_t tokIn = 0, tokOut = 0, tokCache = 0, ctxTok = 0;
        for (const auto& [k, u] : usages) {
            (void)k;
            tokIn += u.in;
            tokOut += u.out;
            tokCache += u.cache;
            if (u.assistant && (u.in + u.cache) > 0) ctxTok = u.in + u.cache;
        }
        if (ctxTok == 0) {
            for (const auto& [k, u] : usages) {
                (void)k;
                if ((u.in + u.cache) > 0) ctxTok = u.in + u.cache;
            }
        }

        const double age = std::max(0.0, now - mtimeEpoch);
        Stato st = Stato::Inattivo;
        if (age < 4.0 || (lastType == "assistant" && lastHasToolUse) || lastType == "user") {
            st = Stato::Occupato;
        } else if (lastType == "assistant") {
            st = age < 120.0 ? Stato::Completato : Stato::Inattivo;
        }

        const std::wstring idW = dettagli::daUtf8(sessionId);
        if (st == Stato::Occupato) {
            AcquireSRWLockExclusive(&lock);
            completatiRimossi.erase(idW);
            ReleaseSRWLockExclusive(&lock);
        } else if (st == Stato::Completato) {
            AcquireSRWLockShared(&lock);
            auto it = completatiRimossi.find(idW);
            const bool nascosto = (it != completatiRimossi.end() && mtimeEpoch <= it->second);
            ReleaseSRWLockShared(&lock);
            if (nascosto) st = Stato::Inattivo;
        }

        const std::wstring cwdW = dettagli::daUtf8(cwd);
        const std::wstring titoloW = !title.empty() ? dettagli::daUtf8(title)
                                   : !cwdW.empty() ? dettagli::leafPath(cwdW)
                                   : idW.substr(0, std::min<size_t>(idW.size(), 8));

        for (auto& ha : hookAgenti) {
            if (ha.id == idW) {
                ha.tokIn = std::max(ha.tokIn, tokIn);
                ha.tokOut = std::max(ha.tokOut, tokOut);
                ha.tokCache = std::max(ha.tokCache, tokCache);
                if (ctxTok > 0) ha.ctxTok = ctxTok;
                if (ha.modello.empty() && !model.empty()) ha.modello = dettagli::daUtf8(model);
                if (ha.titolo.empty() && !titoloW.empty()) ha.titolo = titoloW;
                if (ha.cwd.empty() && !cwdW.empty()) ha.cwd = cwdW;
                if (ha.fonte == Fonte::Hook) { ha.fonte = Fonte::Claude; ha.nome = L"Claude"; }
                ha.ctxMax = stimaFinestraContesto(ha.modello, ha.fonte);
                return;
            }
        }

        Agente a;
        a.id = idW;
        a.fonte = Fonte::Claude;
        a.nome = L"Claude";
        a.titolo = titoloW;
        a.modello = dettagli::daUtf8(model);
        if (st == Stato::Occupato && !lastTool.empty()) a.strumento = dettagli::daUtf8(lastTool);
        a.cwd = cwdW;
        a.stato = st;
        a.tokIn = tokIn;
        a.tokOut = tokOut;
        a.tokCache = tokCache;
        a.ctxTok = ctxTok;
        a.ctxMax = stimaFinestraContesto(a.modello, a.fonte);
        a.aggiornatoT = mtimeEpoch;
        out.push_back(std::move(a));
    }

    void leggiClaude(double now, std::vector<Agente>& hookAgenti, std::vector<Agente>& out) {
        const std::wstring profile = dettagli::envDir(L"USERPROFILE");
        if (profile.empty()) return;
        const std::wstring base = profile + L"\\.claude\\projects";

        auto scanDir = [&](const std::wstring& dir, int depth, auto& self) -> void {
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                const std::wstring full = dir + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (depth < 2) self(full, depth + 1, self);
                    continue;
                }
                const std::wstring fn = fd.cFileName;
                if (fn.size() <= 6 || dettagli::minuscolo(fn.substr(fn.size() - 6)) != L".jsonl") continue;
                const double mt = dettagli::fileTimeAEpoch(fd.ftLastWriteTime);
                if (now - mt > 600.0) continue;
                leggiClaudeJsonl(full, fn.substr(0, fn.size() - 6), mt, now, hookAgenti, out);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        };
        scanDir(base, 1, scanDir);
    }

    struct ProcSnap {
        DWORD pid = 0;
        std::wstring exeLower;
        double cpuPct = 0.0;
        double primoAvvistamento = 0.0;
        bool hasWindow = false;
    };

    void raccogliProcessi(double now, std::vector<ProcSnap>& procs, DWORD& ollamaPid, DWORD& lmsPid, double& lmsCpu) {
        ollamaPid = 0;
        lmsPid = 0;
        lmsCpu = 0.0;

        FILETIME ftNow{};
        GetSystemTimeAsFileTime(&ftNow);
        const uint64_t now100ns = ((uint64_t)ftNow.dwHighDateTime << 32) | ftNow.dwLowDateTime;
        const uint64_t dt100ns = (lastCpuWall100ns > 0 && now100ns > lastCpuWall100ns) ? (now100ns - lastCpuWall100ns) : 0;
        lastCpuWall100ns = now100ns;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;

        std::unordered_map<DWORD, CpuSample> nextSamples;
        std::unordered_set<DWORD> candPids;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            const std::wstring exe = dettagli::minuscolo(pe.szExeFile);
            const bool isOllama = (exe == L"ollama.exe");
            const bool isLms = (exe == L"lm studio.exe" || exe == L"lms.exe");
            const bool isAi = (exe == L"claude.exe" || exe == L"antigravity.exe" || exe == L"agy.exe" ||
                               exe == L"codex.exe" || exe == L"gemini.exe" || exe == L"cursor.exe" || exe == L"windsurf.exe");
            if (!isOllama && !isLms && !isAi) continue;

            double cpu = 0.0;
            double firstSeen = now;
            if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                FILETIME ct{}, et{}, kt{}, ut{};
                if (GetProcessTimes(ph, &ct, &et, &kt, &ut)) {
                    const uint64_t pt = (((uint64_t)kt.dwHighDateTime << 32) | kt.dwLowDateTime) +
                                        (((uint64_t)ut.dwHighDateTime << 32) | ut.dwLowDateTime);
                    auto it = cpuSamples.find(pe.th32ProcessID);
                    if (it != cpuSamples.end()) {
                        firstSeen = it->second.primoAvvistamento;
                        if (dt100ns > 0 && pt >= it->second.procTime) {
                            cpu = 100.0 * (double)(pt - it->second.procTime) / (double)dt100ns;
                        }
                    } else {
                        const double ctEp = dettagli::fileTimeAEpoch(ct);
                        if (ctEp > 0) firstSeen = ctEp;
                    }
                    nextSamples[pe.th32ProcessID] = {pt, cpu, firstSeen};
                }
                CloseHandle(ph);
            }

            if (isOllama && !ollamaPid) ollamaPid = pe.th32ProcessID;
            if (isLms) {
                if (!lmsPid || cpu > lmsCpu) { lmsPid = pe.th32ProcessID; lmsCpu = cpu; }
            }
            if (isAi) {
                candPids.insert(pe.th32ProcessID);
                procs.push_back({pe.th32ProcessID, exe, cpu, firstSeen, false});
            }
        }
        CloseHandle(snap);
        cpuSamples = std::move(nextSamples);

        if (!candPids.empty()) {
            struct Ctx {
                const std::unordered_set<DWORD>* pids;
                std::unordered_set<DWORD> winPids;
            } ctx{&candPids, {}};
            EnumWindows([](HWND hw, LPARAM l) -> BOOL {
                auto* c = (Ctx*)l;
                if (!IsWindowVisible(hw) || GetWindow(hw, GW_OWNER) || GetWindowTextLengthW(hw) == 0) return TRUE;
                DWORD pid = 0;
                GetWindowThreadProcessId(hw, &pid);
                if (c->pids->count(pid)) c->winPids.insert(pid);
                return TRUE;
            }, (LPARAM)&ctx);
            for (auto& p : procs) p.hasWindow = ctx.winPids.count(p.pid) != 0;
        }
    }

    static void leggiOllama(DWORD pid, double now, std::vector<Agente>& out) {
        if (!pid) return;
        std::string body;
        if (!dettagli::httpGetLocale(11434, L"/api/ps", body) || body.empty()) return;
        dettagli::Json j;
        if (!dettagli::parseJson(body, j) || j.tipo != dettagli::Json::Obj) return;
        const auto* models = j.trova("models");
        if (!models || models->tipo != dettagli::Json::Arr) return;
        for (const auto& m : models->arr) {
            if (m.tipo != dettagli::Json::Obj) continue;
            std::string name = m.str("name");
            if (name.empty()) name = m.str("model");
            if (name.empty()) continue;
            Agente a;
            a.id = L"ollama:" + dettagli::daUtf8(name);
            a.fonte = Fonte::Ollama;
            a.nome = L"Ollama";
            a.titolo = dettagli::daUtf8(name);
            a.modello = a.titolo;
            a.stato = Stato::Occupato;
            a.pid = pid;
            a.aggiornatoT = now;
            out.push_back(std::move(a));
        }
    }

    static void leggiLmStudio(DWORD pid, double cpuPct, double now, std::vector<Agente>& out) {
        if (!pid) return;
        std::string body;
        if (!dettagli::httpGetLocale(1234, L"/v1/models", body) || body.empty()) return;
        dettagli::Json j;
        if (!dettagli::parseJson(body, j) || j.tipo != dettagli::Json::Obj) return;
        const auto* data = j.trova("data");
        if (!data || data->tipo != dettagli::Json::Arr) return;
        for (const auto& m : data->arr) {
            if (m.tipo != dettagli::Json::Obj) continue;
            const std::string id = m.str("id");
            if (id.empty()) continue;
            Agente a;
            a.id = L"lmstudio:" + dettagli::daUtf8(id);
            a.fonte = Fonte::LmStudio;
            a.nome = L"LM Studio";
            a.titolo = dettagli::daUtf8(id);
            a.modello = a.titolo;
            a.stato = cpuPct > 1.5 ? Stato::Occupato : Stato::Inattivo;
            a.pid = pid;
            a.aggiornatoT = now;
            out.push_back(std::move(a));
        }
    }

    static void aggiungiProcessi(const std::vector<ProcSnap>& procs, bool antigravityLive, std::vector<Agente>& out) {
        struct Gruppo {
            std::wstring exeLower, nome;
            Fonte fonte = Fonte::Processo;
            DWORD bestPid = 0;
            bool bestHasWin = false;
            double bestCpu = -1.0;
            double maxCpu = 0.0;
            double firstSeen = 0.0;
        };
        std::vector<Gruppo> gruppi;
        for (const auto& p : procs) {
            if (antigravityLive && p.exeLower == L"agy.exe") continue;
            auto it = std::find_if(gruppi.begin(), gruppi.end(), [&](const Gruppo& g) { return g.exeLower == p.exeLower; });
            if (it == gruppi.end()) {
                Gruppo g;
                g.exeLower = p.exeLower;
                if (p.exeLower == L"claude.exe") { g.fonte = Fonte::Claude; g.nome = L"Claude"; }
                else if (p.exeLower == L"antigravity.exe") { g.fonte = Fonte::Antigravity; g.nome = L"AGY"; }
                else if (p.exeLower == L"agy.exe") { g.fonte = Fonte::Antigravity; g.nome = L"AGY"; }
                else if (p.exeLower == L"codex.exe") { g.fonte = Fonte::Codex; g.nome = L"Codex"; }
                else if (p.exeLower == L"gemini.exe") { g.fonte = Fonte::Processo; g.nome = L"Gemini CLI"; }
                else if (p.exeLower == L"cursor.exe") { g.fonte = Fonte::Processo; g.nome = L"Cursor"; }
                else if (p.exeLower == L"windsurf.exe") { g.fonte = Fonte::Processo; g.nome = L"Windsurf"; }
                else { g.fonte = Fonte::Processo; g.nome = p.exeLower; }
                g.bestPid = p.pid;
                g.bestHasWin = p.hasWindow;
                g.bestCpu = p.cpuPct;
                g.maxCpu = p.cpuPct;
                g.firstSeen = p.primoAvvistamento;
                gruppi.push_back(std::move(g));
            } else {
                it->maxCpu = std::max(it->maxCpu, p.cpuPct);
                it->firstSeen = std::min(it->firstSeen, p.primoAvvistamento);
                if ((p.hasWindow && !it->bestHasWin) || (p.hasWindow == it->bestHasWin && p.cpuPct > it->bestCpu)) {
                    it->bestPid = p.pid;
                    it->bestHasWin = p.hasWindow;
                    it->bestCpu = p.cpuPct;
                }
            }
        }
        for (const auto& g : gruppi) {
            Agente a;
            a.id = L"proc:" + g.exeLower;
            a.fonte = g.fonte;
            a.nome = g.nome;
            a.titolo = g.nome;
            a.stato = g.maxCpu > 1.5 ? Stato::Occupato : Stato::Inattivo;
            a.pid = g.bestPid;
            a.aggiornatoT = g.firstSeen;
            out.push_back(std::move(a));
        }
    }

    void scansiona() {
        const double now = dettagli::adessoEpoch();
        std::vector<Agente> hookAgenti;
        leggiDropbox(now, hookAgenti);

        std::vector<ProcSnap> procs;
        DWORD ollamaPid = 0, lmsPid = 0;
        double lmsCpu = 0.0;
        raccogliProcessi(now, procs, ollamaPid, lmsPid, lmsCpu);

        std::vector<ProcSnap> agyProcs;
        for (const auto& p : procs) {
            if (p.exeLower == L"agy.exe") agyProcs.push_back(p);
        }
        std::sort(agyProcs.begin(), agyProcs.end(), [](const ProcSnap& x, const ProcSnap& y) {
            if (x.primoAvvistamento != y.primoAvvistamento) return x.primoAvvistamento < y.primoAvvistamento;
            return x.pid < y.pid;
        });
        std::vector<DWORD> agyPids;
        agyPids.reserve(agyProcs.size());
        for (const auto& p : agyProcs) agyPids.push_back(p.pid);

        std::vector<Agente> tutti;
        const bool agLive = leggiAntigravity(now, agyPids, hookAgenti, tutti);
        leggiClaude(now, hookAgenti, tutti);

        for (auto& ha : hookAgenti) {
            if (ha.fonte == Fonte::Antigravity && ha.pid == 0 && !agyPids.empty()) {
                ha.pid = agyPids.front();
            }
            tutti.push_back(std::move(ha));
        }

        leggiOllama(ollamaPid, now, tutti);
        leggiLmStudio(lmsPid, lmsCpu, now, tutti);
        aggiungiProcessi(procs, agLive, tutti);

        Riepilogo nuovo;
        nuovo.agenti = std::move(tutti);
        ricalcolaRiepilogo(nuovo);

        bool cambiato = false;
        AcquireSRWLockExclusive(&lock);
        if (!ugualeRiepilogo(stato, nuovo)) {
            stato = std::move(nuovo);
            cambiato = true;
        }
        ReleaseSRWLockExclusive(&lock);

        if (cambiato) {
            versione.fetch_add(1, std::memory_order_relaxed);
            if (notificaHwnd && notificaMsg) PostMessageW(notificaHwnd, notificaMsg, 0, 0);
        }
    }

    static DWORD WINAPI threadProc(LPVOID p) {
        Monitor* self = (Monitor*)p;
        const std::wstring dir = cartellaHook();
        if (!dir.empty()) dettagli::creaCartelle(dir);

        HANDLE hDir = !dir.empty()
            ? FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE)
            : INVALID_HANDLE_VALUE;

        self->scansiona();

        for (;;) {
            HANDLE waits[2] = {self->stopEvent, hDir};
            const DWORD count = (hDir != INVALID_HANDLE_VALUE && hDir != nullptr) ? 2 : 1;
            const DWORD w = WaitForMultipleObjects(count, waits, FALSE, 1500);
            if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
            if (w == WAIT_OBJECT_0 + 1) {
                WaitForSingleObject(self->stopEvent, 40);
                if (WaitForSingleObject(self->stopEvent, 0) == WAIT_OBJECT_0) break;
                FindNextChangeNotification(hDir);
            }
            self->scansiona();
        }

        if (hDir != INVALID_HANDLE_VALUE && hDir != nullptr) FindCloseChangeNotification(hDir);
        return 0;
    }
};

namespace dettagli {

inline bool isCarattereParola(wchar_t c) {
    if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return true;
    if (c < 0xC0) return false;
    if ((c >= 0x2000 && c <= 0x2BFF) || (c >= 0xD800 && c <= 0xDFFF) ||
        (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFFF0 && c <= 0xFFFF)) {
        return false;
    }
    return !iswspace((wint_t)c);
}

inline std::wstring pulisciTitoloTab(const std::wstring& raw) {
    std::wstring s = minuscolo(raw);
    size_t lead = 0;
    while (lead < s.size() && !isCarattereParola(s[lead])) lead++;
    s.erase(0, lead);
    while (!s.empty() && iswspace((wint_t)s.back())) s.pop_back();
    return s;
}

inline std::wstring cartellaBreve(const std::wstring& cwd) {
    std::wstring folderLower = minuscolo(leafPath(cwd));
    if (folderLower.size() == 2 && folderLower[1] == L':') folderLower.clear();
    return folderLower;
}

struct FinestraCandidata {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring exeLower;
    std::wstring titleLower;
    int score = 0;
};

inline std::vector<FinestraCandidata> valutaFinestreAgente(const Agente& a) {
    const auto procMap = mappaProcessi();

    std::unordered_map<DWORD, int> ancestorDepth;
    if (a.pid != 0) {
        DWORD cur = a.pid;
        for (int depth = 0; depth < 8 && cur != 0; depth++) {
            if (ancestorDepth.count(cur)) break;
            auto it = procMap.find(cur);
            if (depth > 0 && it != procMap.end()) {
                const std::wstring& ex = it->second.exeLower;
                if (ex == L"explorer.exe" || ex == L"services.exe" || ex == L"svchost.exe" ||
                    ex == L"wininit.exe" || ex == L"winlogon.exe") {
                    break;
                }
            }
            ancestorDepth[cur] = depth;
            if (it == procMap.end()) break;
            const DWORD parent = it->second.parentPid;
            if (parent == cur) break;
            cur = parent;
        }
    }

    struct EnumCtx {
        const std::unordered_map<DWORD, ProcInfo>* procs;
        std::vector<FinestraCandidata> wins;
    } ctx{&procMap, {}};

    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (!finestraValida(h)) return TRUE;
        auto* c = (EnumCtx*)l;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        const int len = GetWindowTextLengthW(h);
        std::wstring title((size_t)len + 1, L'\0');
        const int got = GetWindowTextW(h, title.data(), len + 1);
        title.resize(got > 0 ? (size_t)got : 0);
        std::wstring exeLower;
        if (auto it = c->procs->find(pid); it != c->procs->end()) exeLower = it->second.exeLower;
        c->wins.push_back({h, pid, std::move(exeLower), minuscolo(title), 0});
        return TRUE;
    }, (LPARAM)&ctx);

    if (ctx.wins.empty()) return {};

    const std::wstring titoloLower = minuscolo(a.titolo);
    const std::wstring folderLower = cartellaBreve(a.cwd);
    const std::wstring nomeLower = minuscolo(a.nome);

    bool anyAncestorMatched = false;
    for (const auto& w : ctx.wins) {
        if (ancestorDepth.count(w.pid)) { anyAncestorMatched = true; break; }
    }

    auto appBonus = [&](const std::wstring& exe) -> int {
        if (exe.empty()) return 0;
        switch (a.fonte) {
            case Fonte::Antigravity:
                if (exe == L"antigravity.exe") return 35;
                if (exe == L"windowsterminal.exe" || exe == L"wezterm-gui.exe" ||
                    exe == L"alacritty.exe" || exe == L"code.exe") return 25;
                return 0;
            case Fonte::Claude:
                if (exe == L"claude.exe" || exe == L"dia.exe") return 35;
                if (exe == L"windowsterminal.exe" || exe == L"code.exe") return 25;
                return 0;
            case Fonte::Codex:
                if (exe == L"windowsterminal.exe" || exe == L"code.exe") return 25;
                return 0;
            case Fonte::LmStudio:
                if (exe == L"lm studio.exe") return 35;
                return 0;
            case Fonte::Ollama:
                if (exe == L"ollama app.exe") return 35;
                if (exe == L"windowsterminal.exe") return 20;
                return 0;
            case Fonte::Processo:
            case Fonte::Hook: {
                if (nomeLower.empty()) return 0;
                if (exe == nomeLower) return 30;
                std::wstring stem = exe;
                if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, L".exe") == 0) stem.resize(stem.size() - 4);
                if (!stem.empty() && stem == nomeLower) return 30;
                return 0;
            }
        }
        return 0;
    };

    for (auto& w : ctx.wins) {
        int score = 0;
        if (auto it = ancestorDepth.find(w.pid); it != ancestorDepth.end()) {
            score += 100 - it->second * 4;
        }
        if (!titoloLower.empty() && w.titleLower.find(titoloLower) != std::wstring::npos) {
            score += 60;
        }
        if (!folderLower.empty() && w.titleLower.find(folderLower) != std::wstring::npos) {
            score += 40;
        }
        if (!anyAncestorMatched || ancestorDepth.count(w.pid)) {
            score += appBonus(w.exeLower);
        }
        w.score = score;
    }

    std::stable_sort(ctx.wins.begin(), ctx.wins.end(), [](const FinestraCandidata& x, const FinestraCandidata& y) {
        return x.score > y.score;
    });
    return ctx.wins;
}

}  // namespace dettagli

inline HWND trovaFinestraAgente(const Agente& a) {
    const auto wins = dettagli::valutaFinestreAgente(a);
    return (!wins.empty() && wins.front().score > 0) ? wins.front().hwnd : nullptr;
}

inline bool attivaTabTerminale(HWND hwnd, const Agente& a) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    const std::wstring titoloLower = dettagli::minuscolo(a.titolo);
    const std::wstring folderLower = dettagli::cartellaBreve(a.cwd);
    if (titoloLower.empty() && folderLower.size() < 3) return false;

    std::vector<std::wstring> tokens;
    for (size_t i = 0; i < titoloLower.size();) {
        while (i < titoloLower.size() && !dettagli::isCarattereParola(titoloLower[i])) i++;
        const size_t start = i;
        while (i < titoloLower.size() && dettagli::isCarattereParola(titoloLower[i])) i++;
        if (i - start >= 3) {
            std::wstring tok = titoloLower.substr(start, i - start);
            if (std::find(tokens.begin(), tokens.end(), tok) == tokens.end()) {
                tokens.push_back(std::move(tok));
            }
        }
    }

    IUIAutomation* uia = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, (void**)&uia);
    bool didCoInit = false;
    if (hr == CO_E_NOTINITIALIZED) {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            didCoInit = true;
            hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, (void**)&uia);
        }
    }
    if (FAILED(hr) || !uia) {
        if (uia) uia->Release();
        if (didCoInit) CoUninitialize();
        return false;
    }

    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* cond = nullptr;
    IUIAutomationElementArray* tabs = nullptr;
    IUIAutomationElement* bestTab = nullptr;
    int bestScore = 0;

    if (SUCCEEDED(uia->ElementFromHandle((UIA_HWND)hwnd, &root)) && root) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = UIA_TabItemControlTypeId;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) && cond) {
            if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &tabs)) && tabs) {
                int count = 0;
                if (SUCCEEDED(tabs->get_Length(&count)) && count > 0) {
                    for (int i = 0; i < count; i++) {
                        IUIAutomationElement* el = nullptr;
                        if (FAILED(tabs->GetElement(i, &el)) || !el) continue;

                        BSTR bstr = nullptr;
                        std::wstring tabLower;
                        if (SUCCEEDED(el->get_CurrentName(&bstr)) && bstr) {
                            tabLower = dettagli::pulisciTitoloTab(std::wstring(bstr, SysStringLen(bstr)));
                            SysFreeString(bstr);
                        }

                        int score = 0;
                        if (!tabLower.empty()) {
                            if (!titoloLower.empty()) {
                                if (tabLower == titoloLower) {
                                    score += 100;
                                } else if (titoloLower.size() >= 3 && tabLower.size() >= 4 &&
                                           (tabLower.find(titoloLower) != std::wstring::npos ||
                                            titoloLower.find(tabLower) != std::wstring::npos)) {
                                    score += 75;
                                }
                                for (const std::wstring& tok : tokens) {
                                    if (tabLower.find(tok) != std::wstring::npos) score += 18;
                                }
                            }
                            if (folderLower.size() >= 3 && tabLower.find(folderLower) != std::wstring::npos) {
                                score += 45;
                            }
                        }

                        if (score > bestScore) {
                            if (bestTab) bestTab->Release();
                            bestTab = el;
                            bestScore = score;
                        } else {
                            el->Release();
                        }
                    }
                }
            }
        }
        VariantClear(&v);
    }

    bool activated = false;
    if (bestTab && bestScore >= 35) {
        IUIAutomationSelectionItemPattern* sel = nullptr;
        if (SUCCEEDED(bestTab->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                                   IID_IUIAutomationSelectionItemPattern,
                                                   (void**)&sel)) && sel) {
            activated = SUCCEEDED(sel->Select());
            sel->Release();
        }
        if (!activated) {
            IUIAutomationInvokePattern* inv = nullptr;
            if (SUCCEEDED(bestTab->GetCurrentPatternAs(UIA_InvokePatternId,
                                                       IID_IUIAutomationInvokePattern,
                                                       (void**)&inv)) && inv) {
                activated = SUCCEEDED(inv->Invoke());
                inv->Release();
            }
        }
    }

    if (bestTab) bestTab->Release();
    if (tabs) tabs->Release();
    if (cond) cond->Release();
    if (root) root->Release();
    uia->Release();
    if (didCoInit) CoUninitialize();
    return activated;
}

inline HWND trovaEAttivaFinestraAgente(const Agente& a) {
    const auto wins = dettagli::valutaFinestreAgente(a);
    if (wins.empty()) return nullptr;

    const auto& top = wins.front();
    const HWND bestHwnd = (top.score > 0) ? top.hwnd : nullptr;
    if (bestHwnd && top.exeLower != L"windowsterminal.exe") {
        return bestHwnd;
    }

    const bool hasMatchText = !a.titolo.empty() || dettagli::cartellaBreve(a.cwd).size() >= 3;
    if (hasMatchText) {
        if (bestHwnd && top.exeLower == L"windowsterminal.exe" && attivaTabTerminale(bestHwnd, a)) {
            return bestHwnd;
        }
        for (const auto& w : wins) {
            if (w.hwnd == bestHwnd || w.exeLower != L"windowsterminal.exe") continue;
            if (attivaTabTerminale(w.hwnd, a)) return w.hwnd;
        }
    }
    return bestHwnd;
}

// ---- avvio nuove chat AI ----

struct Avviabile {
    std::wstring id, nome, exe, args;
    bool usaTerminale = true;
};

inline std::wstring trovaEseguibile(const wchar_t* cmd, const std::vector<std::wstring>& extraPaths) {
    for (const auto& p : extraPaths) {
        if (p.empty()) continue;
        const DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return p;
    }
    if (cmd && *cmd) {
        wchar_t buf[MAX_PATH] = L"";
        const DWORD n = SearchPathW(nullptr, cmd, nullptr, MAX_PATH, buf, nullptr);
        if (n > 0 && n < MAX_PATH) {
            const DWORD attr = GetFileAttributesW(buf);
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return std::wstring(buf, n);
            }
        }
    }
    return L"";
}

inline std::vector<Avviabile> elencoAvviabili() {
    std::vector<Avviabile> out;
    const std::wstring localApp = dettagli::envDir(L"LOCALAPPDATA");
    const std::wstring profile = dettagli::envDir(L"USERPROFILE");
    const std::wstring appData = dettagli::envDir(L"APPDATA");

    auto cerca = [](std::initializer_list<const wchar_t*> cmds, const std::vector<std::wstring>& extra) -> std::wstring {
        std::wstring p = trovaEseguibile(nullptr, extra);
        if (!p.empty()) return p;
        for (const wchar_t* c : cmds) {
            p = trovaEseguibile(c, {});
            if (!p.empty()) return p;
        }
        return L"";
    };

    // 1. Antigravity CLI
    {
        std::vector<std::wstring> extra;
        if (!localApp.empty()) extra.push_back(localApp + L"\\agy\\bin\\agy.exe");
        if (std::wstring p = cerca({L"agy.exe", L"agy.cmd"}, extra); !p.empty()) {
            out.push_back({L"antigravity", L"Antigravity CLI", std::move(p), L"", true});
        }
    }
    // 2. Claude Code
    {
        std::vector<std::wstring> extra;
        if (!profile.empty()) extra.push_back(profile + L"\\.local\\bin\\claude.exe");
        if (!appData.empty()) extra.push_back(appData + L"\\npm\\claude.cmd");
        if (std::wstring p = cerca({L"claude.exe", L"claude.cmd"}, extra); !p.empty()) {
            out.push_back({L"claude", L"Claude Code", std::move(p), L"", true});
        }
    }
    // 3. Codex
    {
        std::vector<std::wstring> extra;
        if (!appData.empty()) extra.push_back(appData + L"\\npm\\codex.cmd");
        if (std::wstring p = cerca({L"codex.exe", L"codex.cmd"}, extra); !p.empty()) {
            out.push_back({L"codex", L"Codex", std::move(p), L"", true});
        }
    }
    // 4. Gemini CLI
    {
        std::vector<std::wstring> extra;
        if (!appData.empty()) extra.push_back(appData + L"\\npm\\gemini.cmd");
        if (std::wstring p = cerca({L"gemini.exe", L"gemini.cmd"}, extra); !p.empty()) {
            out.push_back({L"gemini", L"Gemini CLI", std::move(p), L"", true});
        }
    }
    // 5. Ollama (interactive PowerShell tab showing available models, stays open for `ollama run ...`)
    {
        std::vector<std::wstring> extra;
        if (!localApp.empty()) extra.push_back(localApp + L"\\Programs\\Ollama\\ollama.exe");
        if (const std::wstring ollama = cerca({L"ollama.exe"}, extra); !ollama.empty()) {
            std::wstring ps = trovaEseguibile(L"powershell.exe", {});
            if (ps.empty()) ps = L"powershell.exe";
            std::wstring qOllama = ollama;
            for (size_t pos = 0; (pos = qOllama.find(L'\'', pos)) != std::wstring::npos; pos += 2) {
                qOllama.insert(pos, 1, L'\'');
            }
            out.push_back({L"ollama", L"Ollama", std::move(ps),
                           L"-NoLogo -NoExit -Command \"& '" + qOllama + L"' list\"", true});
        }
    }
    // 6. LM Studio
    {
        std::vector<std::wstring> extra;
        if (!localApp.empty()) {
            extra.push_back(localApp + L"\\Programs\\LM Studio\\LM Studio.exe");
            extra.push_back(localApp + L"\\LM-Studio\\LM Studio.exe");
        }
        if (std::wstring p = trovaEseguibile(nullptr, extra); !p.empty()) {
            out.push_back({L"lmstudio", L"LM Studio", std::move(p), L"", false});
        }
    }
    return out;
}

inline const Avviabile* primoTerminale(const std::vector<Avviabile>& lista) {
    for (const auto& av : lista) {
        if (av.usaTerminale) return &av;
    }
    return nullptr;
}

inline std::wstring cartellaPredefinita(const Riepilogo& r) {
    for (const auto& a : r.agenti) {
        if (a.cwd.empty()) continue;
        const DWORD attr = GetFileAttributesW(a.cwd.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return a.cwd;
    }
    const std::wstring profile = dettagli::envDir(L"USERPROFILE");
    if (!profile.empty()) {
        const DWORD attr = GetFileAttributesW(profile.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return profile;
    }
    return L"";
}

inline bool avviaNuovaChat(const Avviabile& av, const std::wstring& dir = L"") {
    if (av.exe.empty()) return false;

    std::wstring dirOk;
    if (!dir.empty()) {
        const DWORD attr = GetFileAttributesW(dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) dirOk = dir;
    }

    if (av.usaTerminale) {
        std::vector<std::wstring> wtExtra;
        const std::wstring localApp = dettagli::envDir(L"LOCALAPPDATA");
        if (!localApp.empty()) wtExtra.push_back(localApp + L"\\Microsoft\\WindowsApps\\wt.exe");
        const std::wstring wt = trovaEseguibile(L"wt.exe", wtExtra);
        if (!wt.empty()) {
            auto quotaCli = [](const std::wstring& s) -> std::wstring {
                std::wstring out = L"\"";
                size_t bs = 0;
                for (wchar_t c : s) {
                    if (c == L'\\') {
                        bs++;
                    } else if (c == L'"') {
                        out.append(bs * 2 + 1, L'\\');
                        out.push_back(L'"');
                        bs = 0;
                    } else {
                        out.append(bs, L'\\');
                        out.push_back(c);
                        bs = 0;
                    }
                }
                out.append(bs * 2, L'\\');
                out.push_back(L'"');
                return out;
            };
            auto endsWithI = [](const std::wstring& s, const wchar_t* suf) {
                const size_t sl = wcslen(suf);
                return s.size() >= sl && dettagli::minuscolo(s.substr(s.size() - sl)) == suf;
            };
            const bool isBatch = endsWithI(av.exe, L".cmd") || endsWithI(av.exe, L".bat");

            std::wstring wtArgs = L"-w new";
            if (!dirOk.empty()) wtArgs += L" -d " + quotaCli(dirOk);
            if (isBatch) wtArgs += L" cmd.exe /k " + quotaCli(av.exe);
            else wtArgs += L" " + quotaCli(av.exe);
            if (!av.args.empty()) wtArgs += L" " + av.args;

            return (INT_PTR)ShellExecuteW(nullptr, L"open", wt.c_str(), wtArgs.c_str(),
                                          dirOk.empty() ? nullptr : dirOk.c_str(), SW_SHOWNORMAL) > 32;
        }
    }

    return (INT_PTR)ShellExecuteW(nullptr, L"open", av.exe.c_str(),
                                  av.args.empty() ? nullptr : av.args.c_str(),
                                  dirOk.empty() ? nullptr : dirOk.c_str(), SW_SHOWNORMAL) > 32;
}

inline std::wstring scegliCartella(HWND owner, const std::wstring& iniziale = L"") {
    bool didCoInit = false;
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileOpenDialog, (void**)&dlg);
    if (hr == CO_E_NOTINITIALIZED) {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            didCoInit = true;
            hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileOpenDialog, (void**)&dlg);
        }
    }
    if (FAILED(hr) || !dlg) {
        if (dlg) dlg->Release();
        if (didCoInit) CoUninitialize();
        return L"";
    }

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    if (!iniziale.empty()) {
        IShellItem* initItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(iniziale.c_str(), nullptr,
                                                 IID_IShellItem, (void**)&initItem)) && initItem) {
            dlg->SetFolder(initItem);
            initItem->Release();
        }
    }

    std::wstring result;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }

    dlg->Release();
    if (didCoInit) CoUninitialize();
    return result;
}

}  // namespace ai
