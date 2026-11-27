// Caricatore dei widget personalizzati da JSON (%LOCALAPPDATA%\FluidWin\widgets\*.json).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wplugin {

static constexpr size_t MAX_FILE_BYTES = 256 * 1024;
static constexpr size_t MAX_ITEMS = 32;
static constexpr size_t MAX_WIDGETS = 16;
static constexpr size_t MAX_CONFIG = 16;
static constexpr size_t MAX_CONFIG_OPTIONS = 5;
static constexpr size_t MAX_CONFIG_COLOR_OPTIONS = 8;

struct Colore {
    float r = 0, g = 0, b = 0;
    bool valido = false;
};

struct Elemento {
    std::wstring titolo, sotto, badge, url, copia;
    Colore colore;
    int progresso = -1;
};

struct ConfigVoce {
    std::wstring chiave, titolo, spiega, tipo;
    bool valBool = false;
    std::wstring valStr;
    double valNum = 0.0, minNum = 0.0, maxNum = 100.0, stepNum = 1.0;
    std::wstring unita;
    std::vector<std::wstring> opzioni;
};

struct Widget {
    std::wstring id, nome, sottotitolo, tooltip, stato, piedeTesto, piedeUrl, filePath;
    std::wstring iconaGlyph, iconaTesto, iconaAnimazione;
    Colore iconaColore, iconaGlow, statoColore;
    int iconaAnello = -1;
    bool abilitato = true;
    bool haToggle = false;
    bool toggleStato = false;
    std::wstring toggleChiave;
    std::vector<Elemento> elementi;
    std::vector<ConfigVoce> config;
};

struct Riepilogo {
    std::vector<Widget> widgets;
};

inline bool parseColore(const std::string& hex, float* r, float* g, float* b) {
    size_t i = 0, j = hex.size();
    while (i < j && (hex[i] == ' ' || hex[i] == '\t')) i++;
    while (j > i && (hex[j - 1] == ' ' || hex[j - 1] == '\t')) j--;
    if (j - i != 7 || hex[i] != '#') return false;
    auto hexNib = [](char c, int& out) -> bool {
        if (c >= '0' && c <= '9') { out = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { out = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { out = c - 'A' + 10; return true; }
        return false;
    };
    int vals[6]{};
    for (size_t k = 0; k < 6; k++) {
        if (!hexNib(hex[i + 1 + k], vals[k])) return false;
    }
    if (r) *r = ((vals[0] << 4) | vals[1]) / 255.0f;
    if (g) *g = ((vals[2] << 4) | vals[3]) / 255.0f;
    if (b) *b = ((vals[4] << 4) | vals[5]) / 255.0f;
    return true;
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
    for (wchar_t& c : s) c = (wchar_t)towlower((wint_t)c);
    return s;
}

inline std::wstring trim(const std::wstring& s) {
    size_t i = 0, j = s.size();
    while (i < j && iswspace((wint_t)s[i])) i++;
    while (j > i && iswspace((wint_t)s[j - 1])) j--;
    return s.substr(i, j - i);
}

inline std::wstring troncaIconaTesto(const std::wstring& s) {
    std::wstring t = trim(s);
    std::wstring out;
    size_t cp = 0;
    for (size_t i = 0; i < t.size() && cp < 4; i++) {
        const wchar_t c = t[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < t.size() && t[i + 1] >= 0xDC00 && t[i + 1] <= 0xDFFF) {
                out.push_back(c);
                out.push_back(t[++i]);
                cp++;
            }
        } else if (c < 0xDC00 || c > 0xDFFF) {
            out.push_back(c);
            cp++;
        }
    }
    return out;
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

inline bool leggiFile(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE f = INVALID_HANDLE_VALUE;
    for (int tent = 0; tent < 2; tent++) {
        f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_SHARING_VIOLATION || tent > 0) return false;
        Sleep(15);
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)MAX_FILE_BYTES) {
        CloseHandle(f);
        return false;
    }
    const size_t n = (size_t)sz.QuadPart;
    out.resize(n);
    DWORD letti = 0, tot = 0;
    while (tot < n && ReadFile(f, out.data() + tot, (DWORD)(n - tot), &letti, nullptr) && letti > 0) {
        tot += letti;
    }
    out.resize(tot);
    CloseHandle(f);
    return tot > 0;
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

inline std::wstring sanificaId(const std::wstring& s, const wchar_t* ripiego = L"widget") {
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
    return out.empty() ? std::wstring(ripiego) : out;
}

inline bool finisceConJson(const std::wstring& nome) {
    if (nome.size() < 6) return false;
    return _wcsicmp(nome.c_str() + nome.size() - 5, L".json") == 0;
}

inline bool finisceConConfigJson(const std::wstring& nome) {
    if (nome.size() < 12) return false;
    return _wcsicmp(nome.c_str() + nome.size() - 12, L".config.json") == 0;
}

inline bool finisceConActionJson(const std::wstring& nome) {
    if (nome.size() < 12) return false;
    return _wcsicmp(nome.c_str() + nome.size() - 12, L".action.json") == 0;
}

inline Colore parseColoreObj(const std::string& s) {
    Colore c;
    c.valido = parseColore(s, &c.r, &c.g, &c.b);
    return c;
}

inline std::wstring normalizzaColoreOpt(const std::wstring& s) {
    const std::wstring t = trim(s);
    if (t.empty()) return L"";
    if (_wcsicmp(t.c_str(), L"auto") == 0) return L"auto";
    if (t.size() != 7 || t[0] != L'#') return L"";
    std::wstring out = L"#";
    for (size_t k = 1; k < 7; k++) {
        const wchar_t c = t[k];
        if (c >= L'0' && c <= L'9') out.push_back(c);
        else if (c >= L'a' && c <= L'f') out.push_back((wchar_t)(c - L'a' + L'A'));
        else if (c >= L'A' && c <= L'F') out.push_back(c);
        else return L"";
    }
    return out;
}

// ---- parser JSON minimale ----

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
    void imposta(const std::string& k, Json v) {
        if (tipo != Obj) { tipo = Obj; obj.clear(); }
        for (auto& [kk, vv] : obj) {
            if (kk == k) { vv = std::move(v); return; }
        }
        obj.emplace_back(k, std::move(v));
    }
    std::string str(const char* k, const char* def = "") const {
        const Json* v = trova(k);
        return (v && v->tipo == Str) ? v->s : std::string(def);
    }
    int optPercent(const char* k) const {
        const Json* v = trova(k);
        if (!v || v->tipo != Num || !std::isfinite(v->n)) return -1;
        return std::clamp((int)std::lround(v->n), 0, 100);
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
        if (c != '\\') { out.push_back(c); continue; }
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
    if (depth > 32) return false;
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
            if (!std::isfinite(v.n)) {
                out += "null";
                break;
            }
            const double val = (v.n == 0.0) ? 0.0 : v.n;
            char b[64];
            if (std::floor(val) == val && std::abs(val) < 9e15) {
                snprintf(b, 64, "%.0f", val);
            } else {
                snprintf(b, 64, "%.15g", val);
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

inline bool parseJson(const std::string& in, Json& out) {
    size_t p = 0;
    if (in.size() >= 3 && (unsigned char)in[0] == 0xEF && (unsigned char)in[1] == 0xBB && (unsigned char)in[2] == 0xBF) p = 3;
    if (!parseVal(in, p, out, 0)) return false;
    saltaSpazi(in, p);
    return p == in.size();
}

inline bool ugualeColore(const Colore& a, const Colore& b) {
    if (a.valido != b.valido) return false;
    if (!a.valido) return true;
    return std::lround(a.r * 255.0f) == std::lround(b.r * 255.0f) &&
           std::lround(a.g * 255.0f) == std::lround(b.g * 255.0f) &&
           std::lround(a.b * 255.0f) == std::lround(b.b * 255.0f);
}

inline bool ugualeElemento(const Elemento& a, const Elemento& b) {
    return a.titolo == b.titolo && a.sotto == b.sotto && a.badge == b.badge &&
           a.url == b.url && a.copia == b.copia && a.progresso == b.progresso &&
           ugualeColore(a.colore, b.colore);
}

inline double snapSlider(double val, double mn, double mx, double step) {
    if (!std::isfinite(mn)) mn = 0.0;
    if (!std::isfinite(mx) || mx <= mn) mx = mn + 100.0;
    if (!std::isfinite(step) || step <= 0.0) step = 1.0;
    if (!std::isfinite(val)) val = mn;
    double v = std::clamp(val, mn, mx);
    v = mn + std::round((v - mn) / step) * step;
    v = std::clamp(v, mn, mx);
    if (std::abs(v - std::round(v)) < 1e-6 && step >= 1e-4) {
        v = std::round(v);
    } else if (step >= 1e-5 && std::abs(v) < 1e9) {
        v = std::round(v * 1e6) / 1e6;
    }
    v = std::clamp(v, mn, mx);
    if (v == 0.0) v = 0.0;
    return v;
}

inline bool ugualeConfigVoce(const ConfigVoce& a, const ConfigVoce& b) {
    return a.chiave == b.chiave && a.titolo == b.titolo && a.spiega == b.spiega &&
           a.tipo == b.tipo && a.valBool == b.valBool && a.valStr == b.valStr &&
           std::abs(a.valNum - b.valNum) <= 1e-6 &&
           std::abs(a.minNum - b.minNum) <= 1e-6 &&
           std::abs(a.maxNum - b.maxNum) <= 1e-6 &&
           std::abs(a.stepNum - b.stepNum) <= 1e-6 &&
           a.unita == b.unita &&
           a.opzioni == b.opzioni;
}

inline bool ugualeWidget(const Widget& a, const Widget& b) {
    if (a.id != b.id || a.nome != b.nome || a.sottotitolo != b.sottotitolo ||
        a.tooltip != b.tooltip || a.stato != b.stato || a.piedeTesto != b.piedeTesto ||
        a.piedeUrl != b.piedeUrl || a.filePath != b.filePath ||
        a.iconaGlyph != b.iconaGlyph || a.iconaTesto != b.iconaTesto ||
        a.iconaAnimazione != b.iconaAnimazione ||
        a.iconaAnello != b.iconaAnello || a.abilitato != b.abilitato ||
        a.haToggle != b.haToggle || a.toggleStato != b.toggleStato || a.toggleChiave != b.toggleChiave ||
        !ugualeColore(a.iconaColore, b.iconaColore) || !ugualeColore(a.iconaGlow, b.iconaGlow) ||
        !ugualeColore(a.statoColore, b.statoColore) ||
        a.elementi.size() != b.elementi.size() || a.config.size() != b.config.size()) {
        return false;
    }
    for (size_t i = 0; i < a.elementi.size(); i++) {
        if (!ugualeElemento(a.elementi[i], b.elementi[i])) return false;
    }
    for (size_t i = 0; i < a.config.size(); i++) {
        if (!ugualeConfigVoce(a.config[i], b.config[i])) return false;
    }
    return true;
}

inline bool ugualeRiepilogo(const Riepilogo& a, const Riepilogo& b) {
    if (a.widgets.size() != b.widgets.size()) return false;
    for (size_t i = 0; i < a.widgets.size(); i++) {
        if (!ugualeWidget(a.widgets[i], b.widgets[i])) return false;
    }
    return true;
}

}  // namespace dettagli

inline std::wstring cartellaWidget() {
    std::wstring base = dettagli::envDir(L"LOCALAPPDATA");
    if (base.empty()) {
        const std::wstring prof = dettagli::envDir(L"USERPROFILE");
        if (!prof.empty()) base = prof + L"\\AppData\\Local";
    }
    if (base.empty()) return L"";
    return base + L"\\FluidWin\\widgets";
}

inline const wchar_t* const CHIAVE_WIDGET_PLUGIN = L"Software\\FluidWin\\WidgetPlugin";

inline bool widgetAbilitato(const std::wstring& id) {
    const std::wstring sid = dettagli::sanificaId(id);
    DWORD v = 1, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_WIDGET_PLUGIN, sid.c_str(), RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS) {
        return v != 0;
    }
    return true;
}

inline void impostaWidgetAbilitato(const std::wstring& id, bool si) {
    const std::wstring sid = dettagli::sanificaId(id);
    const DWORD v = si ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_WIDGET_PLUGIN, sid.c_str(), REG_DWORD, &v, sizeof(v));
}

inline std::wstring percorsoConfig(const std::wstring& id) {
    const std::wstring dir = cartellaWidget();
    if (dir.empty()) return L"";
    return dir + L"\\" + dettagli::sanificaId(id) + L".config.json";
}

inline std::wstring percorsoAzione(const std::wstring& id) {
    const std::wstring dir = cartellaWidget();
    if (dir.empty()) return L"";
    return dir + L"\\" + dettagli::sanificaId(id) + L".action.json";
}

inline bool scriviAzioneToggle(const std::wstring& id, const std::wstring& chiave, bool valore) {
    if (id.empty()) return false;
    const std::wstring path = percorsoAzione(id);
    if (path.empty()) return false;
    const std::wstring k = dettagli::trim(chiave).empty() ? std::wstring(L"toggle") : chiave;
    std::string json = "{\"action\":\"toggle\",\"key\":";
    dettagli::escapeJsonStr(dettagli::inUtf8(k), json);
    json += ",\"value\":";
    json += (valore ? "true" : "false");
    json += ",\"ts\":";
    json += std::to_string((unsigned long long)GetTickCount64());
    json += "}\n";
    return dettagli::scriviAtomico(path, json);
}

namespace dettagli {

inline bool salvaConfigVal(const std::wstring& id, const std::wstring& chiave, Json val) {
    if (id.empty() || chiave.empty()) return false;
    const std::wstring dir = cartellaWidget();
    if (dir.empty()) return false;
    const std::wstring path = dir + L"\\" + sanificaId(id) + L".config.json";
    Json root = Json::makeObj();
    std::string raw;
    if (leggiFile(path, raw)) {
        Json parsed;
        if (parseJson(raw, parsed) && parsed.tipo == Json::Obj) root = std::move(parsed);
    }
    root.imposta(inUtf8(chiave), std::move(val));
    return scriviAtomico(path, dumpJson(root, true));
}

}  // namespace dettagli

inline bool salvaConfigBool(const std::wstring& id, const std::wstring& chiave, bool val) {
    return dettagli::salvaConfigVal(id, chiave, dettagli::Json::makeBool(val));
}

inline bool salvaConfigStr(const std::wstring& id, const std::wstring& chiave, const std::wstring& val) {
    return dettagli::salvaConfigVal(id, chiave, dettagli::Json::makeStr(dettagli::inUtf8(val)));
}

inline bool salvaConfigNum(const std::wstring& id, const std::wstring& chiave, double valore) {
    if (!std::isfinite(valore)) return false;
    return dettagli::salvaConfigVal(id, chiave, dettagli::Json::makeNum(valore == 0.0 ? 0.0 : valore));
}

inline bool parseWidgetJson(const std::string& jsonText, const std::wstring& fallbackId, Widget& out) {
    dettagli::Json root;
    if (!dettagli::parseJson(jsonText, root) || root.tipo != dettagli::Json::Obj) return false;

    const std::wstring rawId = dettagli::trim(dettagli::daUtf8(root.str("id")));
    out = Widget{};
    out.id = dettagli::sanificaId(rawId.empty() ? fallbackId : rawId);
    out.nome = dettagli::trim(dettagli::daUtf8(root.str("name")));
    if (out.nome.empty()) out.nome = out.id;
    out.sottotitolo = dettagli::trim(dettagli::daUtf8(root.str("subtitle")));
    out.tooltip = dettagli::trim(dettagli::daUtf8(root.str("tooltip")));
    out.stato = dettagli::trim(dettagli::daUtf8(root.str("status")));
    out.statoColore = dettagli::parseColoreObj(root.str("statusColor"));

    if (const auto* tg = root.trova("toggle"); tg && tg->tipo == dettagli::Json::Obj) {
        out.haToggle = true;
        std::wstring chiave;
        for (wchar_t c : dettagli::trim(dettagli::daUtf8(tg->str("key")))) {
            if (c >= 0x20 && c != 0x7F) chiave.push_back(c);
        }
        chiave = dettagli::trim(chiave);
        if (chiave.size() > 48) {
            chiave.resize(48);
            if (chiave.back() >= 0xD800 && chiave.back() <= 0xDBFF) chiave.pop_back();
        }
        out.toggleChiave = chiave.empty() ? std::wstring(L"toggle") : std::move(chiave);

        const auto* onVal = tg->trova("on");
        const auto* valVal = tg->trova("value");
        if (onVal && onVal->tipo == dettagli::Json::Bool) {
            out.toggleStato = onVal->b;
        } else if ((!onVal || onVal->tipo == dettagli::Json::Null) && valVal && valVal->tipo == dettagli::Json::Bool) {
            out.toggleStato = valVal->b;
        } else {
            out.toggleStato = false;
        }
    }

    if (const auto* ic = root.trova("icon"); ic && ic->tipo == dettagli::Json::Obj) {
        out.iconaGlyph = dettagli::minuscolo(dettagli::trim(dettagli::daUtf8(ic->str("glyph"))));
        out.iconaTesto = dettagli::troncaIconaTesto(dettagli::daUtf8(ic->str("text")));
        out.iconaColore = dettagli::parseColoreObj(ic->str("color"));
        out.iconaGlow = dettagli::parseColoreObj(ic->str("glowColor"));
        out.iconaAnello = ic->optPercent("ring");
        if (const auto* an = ic->trova("animate")) {
            if (an->tipo == dettagli::Json::Str) {
                const std::wstring anim = dettagli::minuscolo(dettagli::trim(dettagli::daUtf8(an->s)));
                if (anim == L"pulse" || anim == L"breathe" || anim == L"wave" || anim == L"spin") {
                    out.iconaAnimazione = anim;
                }
            } else if (an->tipo == dettagli::Json::Bool) {
                out.iconaAnimazione = an->b ? L"pulse" : L"";
            }
        }
    }

    if (const auto* items = root.trova("items"); items && items->tipo == dettagli::Json::Arr) {
        for (const auto& it : items->arr) {
            if (out.elementi.size() >= MAX_ITEMS) break;
            if (it.tipo != dettagli::Json::Obj) continue;
            Elemento el;
            el.titolo = dettagli::trim(dettagli::daUtf8(it.str("title")));
            el.sotto = dettagli::trim(dettagli::daUtf8(it.str("subtitle")));
            el.badge = dettagli::trim(dettagli::daUtf8(it.str("badge")));
            el.url = dettagli::trim(dettagli::daUtf8(it.str("url")));
            el.copia = dettagli::daUtf8(it.str("copy"));
            el.colore = dettagli::parseColoreObj(it.str("color"));
            el.progresso = it.optPercent("progress");
            if (el.titolo.empty() && el.sotto.empty() && el.badge.empty()) continue;
            out.elementi.push_back(std::move(el));
        }
    }

    if (const auto* ft = root.trova("footer"); ft && ft->tipo == dettagli::Json::Obj) {
        out.piedeTesto = dettagli::trim(dettagli::daUtf8(ft->str("label")));
        out.piedeUrl = dettagli::trim(dettagli::daUtf8(ft->str("url")));
    }

    if (const auto* cfg = root.trova("config"); cfg && cfg->tipo == dettagli::Json::Arr) {
        for (const auto& it : cfg->arr) {
            if (out.config.size() >= MAX_CONFIG) break;
            if (it.tipo != dettagli::Json::Obj) continue;
            const std::wstring chiave = dettagli::trim(dettagli::daUtf8(it.str("key")));
            if (chiave.empty()) continue;
            const bool dup = std::any_of(out.config.begin(), out.config.end(), [&](const ConfigVoce& x) {
                return x.chiave == chiave;
            });
            if (dup) continue;
            const std::wstring tipo = dettagli::minuscolo(dettagli::trim(dettagli::daUtf8(it.str("type"))));
            if (tipo != L"toggle" && tipo != L"choice" && tipo != L"color" && tipo != L"slider") continue;

            ConfigVoce cv;
            cv.chiave = chiave;
            cv.titolo = dettagli::trim(dettagli::daUtf8(it.str("label")));
            if (cv.titolo.empty()) cv.titolo = cv.chiave;
            cv.spiega = dettagli::trim(dettagli::daUtf8(it.str("description")));
            cv.tipo = tipo;

            const auto* def = it.trova("default");
            if (cv.tipo == L"toggle") {
                cv.valBool = (def && def->tipo == dettagli::Json::Bool) ? def->b : false;
            } else if (cv.tipo == L"choice") {
                if (const auto* opts = it.trova("options"); opts && opts->tipo == dettagli::Json::Arr) {
                    for (const auto& op : opts->arr) {
                        if (cv.opzioni.size() >= MAX_CONFIG_OPTIONS) break;
                        if (op.tipo != dettagli::Json::Str) continue;
                        const std::wstring wop = dettagli::trim(dettagli::daUtf8(op.s));
                        if (!wop.empty()) cv.opzioni.push_back(wop);
                    }
                }
                const std::wstring defStr = (def && def->tipo == dettagli::Json::Str)
                    ? dettagli::trim(dettagli::daUtf8(def->s))
                    : L"";
                if (!defStr.empty() && (cv.opzioni.empty() ||
                    std::find(cv.opzioni.begin(), cv.opzioni.end(), defStr) != cv.opzioni.end())) {
                    cv.valStr = defStr;
                } else if (!cv.opzioni.empty()) {
                    cv.valStr = cv.opzioni.front();
                }
            } else if (cv.tipo == L"color") {
                if (const auto* opts = it.trova("options"); opts && opts->tipo == dettagli::Json::Arr) {
                    for (const auto& op : opts->arr) {
                        if (cv.opzioni.size() >= MAX_CONFIG_COLOR_OPTIONS) break;
                        if (op.tipo != dettagli::Json::Str) continue;
                        const std::wstring norm = dettagli::normalizzaColoreOpt(dettagli::daUtf8(op.s));
                        if (!norm.empty() && std::find(cv.opzioni.begin(), cv.opzioni.end(), norm) == cv.opzioni.end()) {
                            cv.opzioni.push_back(norm);
                        }
                    }
                }
                if (cv.opzioni.empty()) {
                    cv.opzioni = {L"auto", L"#30D158", L"#32D2FF", L"#0A84FF", L"#BF5AF2", L"#FF9F0A", L"#FF453A"};
                }
                const std::wstring defNorm = (def && def->tipo == dettagli::Json::Str)
                    ? dettagli::normalizzaColoreOpt(dettagli::daUtf8(def->s))
                    : L"";
                if (!defNorm.empty() && std::find(cv.opzioni.begin(), cv.opzioni.end(), defNorm) != cv.opzioni.end()) {
                    cv.valStr = defNorm;
                } else {
                    cv.valStr = cv.opzioni.front();
                }
            } else {
                const auto* pMin = it.trova("min");
                const auto* pMax = it.trova("max");
                const auto* pStep = it.trova("step");
                cv.minNum = (pMin && pMin->tipo == dettagli::Json::Num && std::isfinite(pMin->n)) ? pMin->n : 0.0;
                cv.maxNum = (pMax && pMax->tipo == dettagli::Json::Num && std::isfinite(pMax->n)) ? pMax->n : 100.0;
                if (cv.maxNum <= cv.minNum) cv.maxNum = cv.minNum + 100.0;
                cv.stepNum = (pStep && pStep->tipo == dettagli::Json::Num && std::isfinite(pStep->n)) ? pStep->n : 1.0;
                if (cv.stepNum <= 0.0) cv.stepNum = 1.0;
                const double defNum = (def && def->tipo == dettagli::Json::Num && std::isfinite(def->n)) ? def->n : cv.minNum;
                cv.valNum = dettagli::snapSlider(defNum, cv.minNum, cv.maxNum, cv.stepNum);
                std::wstring u = dettagli::trim(dettagli::daUtf8(it.str("unit")));
                if (u.size() > 8) {
                    u.resize(8);
                    if (u.back() >= 0xD800 && u.back() <= 0xDBFF) u.pop_back();
                }
                cv.unita = std::move(u);
            }
            out.config.push_back(std::move(cv));
        }
    }

    if (!out.config.empty()) {
        const std::wstring cfgPath = percorsoConfig(out.id);
        std::string cfgRaw;
        dettagli::Json cfgJson;
        if (!cfgPath.empty() && dettagli::leggiFile(cfgPath, cfgRaw) &&
            dettagli::parseJson(cfgRaw, cfgJson) && cfgJson.tipo == dettagli::Json::Obj) {
            for (auto& cv : out.config) {
                const std::string keyUtf8 = dettagli::inUtf8(cv.chiave);
                const auto* ov = cfgJson.trova(keyUtf8.c_str());
                if (!ov) continue;
                if (cv.tipo == L"toggle" && ov->tipo == dettagli::Json::Bool) {
                    cv.valBool = ov->b;
                } else if (cv.tipo == L"choice" && ov->tipo == dettagli::Json::Str) {
                    const std::wstring wv = dettagli::trim(dettagli::daUtf8(ov->s));
                    if (!wv.empty() && (cv.opzioni.empty() ||
                        std::find(cv.opzioni.begin(), cv.opzioni.end(), wv) != cv.opzioni.end())) {
                        cv.valStr = wv;
                    }
                } else if (cv.tipo == L"color" && ov->tipo == dettagli::Json::Str) {
                    const std::wstring wv = dettagli::normalizzaColoreOpt(dettagli::daUtf8(ov->s));
                    if (!wv.empty() && std::find(cv.opzioni.begin(), cv.opzioni.end(), wv) != cv.opzioni.end()) {
                        cv.valStr = wv;
                    }
                } else if (cv.tipo == L"slider" && ov->tipo == dettagli::Json::Num && std::isfinite(ov->n)) {
                    cv.valNum = dettagli::snapSlider(ov->n, cv.minNum, cv.maxNum, cv.stepNum);
                }
            }
        }
    }

    out.abilitato = widgetAbilitato(out.id);
    return true;
}

inline Riepilogo caricaTutti(HANDLE stopEvent = nullptr) {
    Riepilogo out;
    const std::wstring dir = cartellaWidget();
    if (dir.empty()) return out;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    std::vector<std::wstring> nomiFile;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring nome = fd.cFileName;
        if (dettagli::finisceConJson(nome) && !dettagli::finisceConConfigJson(nome) &&
            !dettagli::finisceConActionJson(nome)) {
            nomiFile.push_back(nome);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(nomiFile.begin(), nomiFile.end(), [](const std::wstring& a, const std::wstring& b) {
        const std::wstring la = dettagli::minuscolo(a), lb = dettagli::minuscolo(b);
        return la != lb ? la < lb : a < b;
    });

    std::unordered_set<std::wstring> visti;
    for (const std::wstring& fn : nomiFile) {
        if (out.widgets.size() >= MAX_WIDGETS) break;
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        const std::wstring fullPath = dir + L"\\" + fn;
        std::string contenuto;
        if (!dettagli::leggiFile(fullPath, contenuto)) continue;
        const std::wstring stem = fn.size() > 5 ? fn.substr(0, fn.size() - 5) : fn;
        Widget w;
        if (!parseWidgetJson(contenuto, stem, w)) continue;
        w.filePath = fullPath;
        if (!visti.insert(w.id).second) continue;
        out.widgets.push_back(std::move(w));
    }

    std::sort(out.widgets.begin(), out.widgets.end(), [](const Widget& a, const Widget& b) {
        const std::wstring na = dettagli::minuscolo(a.nome), nb = dettagli::minuscolo(b.nome);
        if (na != nb) return na < nb;
        if (a.nome != b.nome) return a.nome < b.nome;
        return a.id < b.id;
    });
    return out;
}

inline bool urlSicuro(const std::wstring& raw) {
    const std::wstring u = dettagli::trim(raw);
    if (u.empty()) return false;
    for (wchar_t c : u) if (c < 0x20) return false;
    if (_wcsnicmp(u.c_str(), L"http://", 7) == 0 && u.size() > 7) return true;
    if (_wcsnicmp(u.c_str(), L"https://", 8) == 0 && u.size() > 8) return true;
    // Solo percorsi locali assoluti esistenti (niente nomi di comandi relativi come "cmd" o "powershell")
    const bool driveAssoluto = u.size() >= 3 && iswalpha((wint_t)u[0]) && u[1] == L':' && (u[2] == L'\\' || u[2] == L'/');
    const bool uncAssoluto = u.size() >= 3 && u[0] == L'\\' && u[1] == L'\\';
    if ((driveAssoluto || uncAssoluto) && GetFileAttributesW(u.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return false;
}

inline bool eseguiAzione(const std::wstring& url, const std::wstring& copia, HWND owner = nullptr) {
    if (!copia.empty()) {
        bool ok = false;
        if (OpenClipboard(owner)) {
            EmptyClipboard();
            if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (copia.size() + 1) * sizeof(wchar_t))) {
                if (void* p = GlobalLock(g)) {
                    memcpy(p, copia.c_str(), (copia.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(g);
                    ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
                }
                if (!ok) GlobalFree(g);
            }
            CloseClipboard();
        }
        return ok;
    }
    const std::wstring u = dettagli::trim(url);
    if (!urlSicuro(u)) return false;
    return (INT_PTR)ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

inline bool apriCartellaWidget() {
    const std::wstring dir = cartellaWidget();
    if (dir.empty()) return false;
    dettagli::creaCartelle(dir);
    return (INT_PTR)ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

inline bool creaEsempio() {
    const std::wstring dir = cartellaWidget();
    if (dir.empty()) return false;
    dettagli::creaCartelle(dir);

    static const char* SAMPLE_JSON =
        "{\n"
        "  \"id\": \"example-status\",\n"
        "  \"name\": \"Dev Environment\",\n"
        "  \"subtitle\": \"Local services & build cache\",\n"
        "  \"tooltip\": \"Dev Environment \\u00b7 Healthy (84%)\",\n"
        "  \"status\": \"Healthy\",\n"
        "  \"statusColor\": \"#30D158\",\n"
        "  \"icon\": {\n"
        "    \"glyph\": \"ring\",\n"
        "    \"text\": \"84%\",\n"
        "    \"color\": \"#30D158\",\n"
        "    \"ring\": 84\n"
        "  },\n"
        "  \"items\": [\n"
        "    {\n"
        "      \"title\": \"Local API Gateway\",\n"
        "      \"subtitle\": \"127.0.0.1:8080 \\u00b7 Click to copy URL\",\n"
        "      \"badge\": \"OK\",\n"
        "      \"color\": \"#30D158\",\n"
        "      \"progress\": 84,\n"
        "      \"copy\": \"http://127.0.0.1:8080\"\n"
        "    },\n"
        "    {\n"
        "      \"title\": \"Build Cache\",\n"
        "      \"subtitle\": \"4.2 GB of 8.0 GB used\",\n"
        "      \"badge\": \"52%\",\n"
        "      \"color\": \"#0A84FF\",\n"
        "      \"progress\": 52,\n"
        "      \"url\": \"https://github.com/clooudfn/FluidWin\"\n"
        "    }\n"
        "  ],\n"
        "  \"config\": [\n"
        "    {\n"
        "      \"key\": \"compactMode\",\n"
        "      \"label\": \"Compact mode\",\n"
        "      \"description\": \"Show condensed service rows\",\n"
        "      \"type\": \"toggle\",\n"
        "      \"default\": false\n"
        "    },\n"
        "    {\n"
        "      \"key\": \"unit\",\n"
        "      \"label\": \"Temperature unit\",\n"
        "      \"description\": \"Display unit for thermal metrics\",\n"
        "      \"type\": \"choice\",\n"
        "      \"default\": \"\\u00b0C\",\n"
        "      \"options\": [\"\\u00b0C\", \"\\u00b0F\"]\n"
        "    }\n"
        "  ],\n"
        "  \"footer\": {\n"
        "    \"label\": \"Open widgets folder\"\n"
        "  }\n"
        "}\n";

    static const char* SAMPLE_README =
        "FluidWin Custom Widgets\n"
        "=======================\n\n"
        "Drop any UTF-8 .json file (max 256 KB) into this folder. FluidWin watches\n"
        "this directory and reloads widgets live with no code execution.\n"
        "Up to 16 widgets are loaded, each with up to 32 items.\n\n"
        "Schema (all fields optional):\n"
        "  id           Unique widget ID (defaults to sanitized filename)\n"
        "  name         Dock tooltip and panel title (defaults to id)\n"
        "  subtitle     Muted line under panel title\n"
        "  tooltip      Live hover label on the dock\n"
        "  status       Header status pill text\n"
        "  statusColor  #RRGGBB hex color for the status pill\n"
        "  toggle       { key, on } interactive switch in the flyout header:\n"
        "                 key:     action key string (default \"toggle\", max 48 chars)\n"
        "                 on:      boolean switch state (or fallback \"value\")\n"
        "                 Clicking writes <id>.action.json without executing any shell command.\n"
        "  icon         { glyph, text, color, glowColor, ring, animate }\n"
        "                 glyph:     ring | battery | spark | cpu | thermometer | tailscale | dot | text\n"
        "                 text:      up to 4 chars shown inside the dock tile (e.g. \"42\\u00b0\", \"VPN\")\n"
        "                 color:     #RRGGBB hex accent color\n"
        "                 glowColor: optional #RRGGBB hex override for animated glow halo\n"
        "                 ring:      0..100 outer progress arc (omit for ringless icon)\n"
        "                 animate:   none | pulse | breathe | wave | spin\n"
        "  items        Array of cards: { title, subtitle, badge, color, progress, url, copy }\n"
        "                 color:    #RRGGBB dot & bar accent\n"
        "                 progress: 0..100 bottom bar\n"
        "                 copy:     text copied to clipboard on click\n"
        "                 url:      http://, https://, or existing local file/folder path\n"
        "  config       Array (max 16) rendered in Settings > Widgets > <widget>:\n"
        "                 { key, label, description, type, default, options, min, max, step, unit }\n"
        "                 type:    \"toggle\" (bool) | \"choice\" (string) | \"color\" (hex/auto swatch) | \"slider\" (number)\n"
        "                 options: string array (max 5 for \"choice\"; max 8 of \"auto\" or #RRGGBB for \"color\")\n"
        "                 slider:  min (0), max (100), step (1), default (min), unit (max 8 chars)\n"
        "                 User changes persist to <id>.config.json ({ \"<key>\": value })\n"
        "                 in this folder and override defaults on reload.\n"
        "  footer       { label, url } (defaults to opening this folder if url is omitted)\n";

    const bool okJson = dettagli::scriviAtomico(dir + L"\\example-status.json", SAMPLE_JSON);
    const bool okReadme = dettagli::scriviAtomico(dir + L"\\README.txt", SAMPLE_README);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return okJson && okReadme;
}

class Monitor {
public:
    std::atomic<uint64_t> versione{0};

    ~Monitor() {
        ferma();
        DeleteCriticalSection(&scanCs);
    }

    Monitor() {
        InitializeCriticalSection(&scanCs);
    }

    void avvia(HWND hwndNotifica, UINT msgNotifica) {
        if (threadH) return;
        notificaHwnd = hwndNotifica;
        notificaMsg = msgNotifica;
        const std::wstring dir = cartellaWidget();
        if (!dir.empty()) dettagli::creaCartelle(dir);
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stopEvent || !wakeEvent) {
            if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
            if (wakeEvent) { CloseHandle(wakeEvent); wakeEvent = nullptr; }
            return;
        }
        threadH = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
        if (!threadH) {
            CloseHandle(stopEvent); stopEvent = nullptr;
            CloseHandle(wakeEvent); wakeEvent = nullptr;
        }
    }

    bool ferma(DWORD ms = 2000) {
        if (!threadH) {
            if (stopEvent) { CloseHandle(stopEvent); stopEvent = nullptr; }
            if (wakeEvent) { CloseHandle(wakeEvent); wakeEvent = nullptr; }
            return true;
        }
        SetEvent(stopEvent);
        const DWORD w = WaitForSingleObject(threadH, ms);
        const bool ok = (w == WAIT_OBJECT_0);
        if (ok) {
            CloseHandle(threadH);
            threadH = nullptr;
            CloseHandle(stopEvent); stopEvent = nullptr;
            CloseHandle(wakeEvent); wakeEvent = nullptr;
        }
        return ok;
    }

    Riepilogo copia() {
        AcquireSRWLockShared(&lock);
        Riepilogo r = stato;
        ReleaseSRWLockShared(&lock);
        return r;
    }

    void ricaricaOra() {
        if (wakeEvent) SetEvent(wakeEvent);
    }

    // Sincronizza subito sul thread chiamante (es. dopo creaEsempio o cambio abilitazione)
    void sincronizzaSubito() {
        scansiona(false);
    }

    void impostaAbilitazione(const std::wstring& id, bool si) {
        impostaWidgetAbilitato(id, si);
        const std::wstring sid = dettagli::sanificaId(id);
        bool cambiato = false;
        AcquireSRWLockExclusive(&lock);
        for (auto& w : stato.widgets) {
            if (w.id == sid && w.abilitato != si) {
                w.abilitato = si;
                cambiato = true;
            }
        }
        ReleaseSRWLockExclusive(&lock);
        if (cambiato) versione.fetch_add(1, std::memory_order_relaxed);
        ricaricaOra();
    }

private:
    SRWLOCK lock = SRWLOCK_INIT;
    CRITICAL_SECTION scanCs{};
    Riepilogo stato;
    HANDLE stopEvent = nullptr;
    HANDLE wakeEvent = nullptr;
    HANDLE threadH = nullptr;
    HWND notificaHwnd = nullptr;
    UINT notificaMsg = 0;

    void scansiona(bool notifica = true) {
        EnterCriticalSection(&scanCs);
        Riepilogo nuovo = caricaTutti(notifica ? stopEvent : nullptr);
        if (notifica && stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            LeaveCriticalSection(&scanCs);
            return;
        }
        bool cambiato = false;
        AcquireSRWLockExclusive(&lock);
        if (!dettagli::ugualeRiepilogo(stato, nuovo)) {
            stato = std::move(nuovo);
            cambiato = true;
        }
        ReleaseSRWLockExclusive(&lock);
        LeaveCriticalSection(&scanCs);

        if (cambiato) {
            versione.fetch_add(1, std::memory_order_relaxed);
            if (notifica && notificaHwnd && notificaMsg) PostMessageW(notificaHwnd, notificaMsg, 0, 0);
        }
    }

    static DWORD WINAPI threadProc(LPVOID p) {
        Monitor* self = (Monitor*)p;
        self->scansiona();
        const std::wstring dir = cartellaWidget();
        HANDLE hWatch = !dir.empty()
            ? FindFirstChangeNotificationW(dir.c_str(), FALSE,
                  FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE)
            : INVALID_HANDLE_VALUE;

        for (;;) {
            if (hWatch != INVALID_HANDLE_VALUE) {
                HANDLE waits[3] = {self->stopEvent, self->wakeEvent, hWatch};
                const DWORD w = WaitForMultipleObjects(3, waits, FALSE, 5000);
                if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
                if (w == WAIT_OBJECT_0 + 2) {
                    // Breve attesa per evitare letture parziali mentre un processo sta scrivendo il JSON
                    if (WaitForSingleObject(self->stopEvent, 40) == WAIT_OBJECT_0) break;
                    if (!FindNextChangeNotification(hWatch)) {
                        FindCloseChangeNotification(hWatch);
                        hWatch = INVALID_HANDLE_VALUE;
                    }
                }
            } else {
                HANDLE waits[2] = {self->stopEvent, self->wakeEvent};
                const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5000);
                if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
                if (!dir.empty()) {
                    dettagli::creaCartelle(dir);
                    hWatch = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
                }
            }
            self->scansiona();
        }
        if (hWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(hWatch);
        return 0;
    }
};

}  // namespace wplugin
