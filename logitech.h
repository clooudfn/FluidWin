// Monitoraggio dei dispositivi Logitech G HUB tramite WebSocket locale (lghub_agent.exe su ws://127.0.0.1:9010).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace logitech {

enum class Tipo { Mouse, Tastiera, Cuffie, Altro };
enum class Stato { Attivo, Riposo, Disconnesso };

struct Dispositivo {
    std::wstring id, nome, connessione;
    Tipo tipo = Tipo::Altro;
    Stato stato = Stato::Disconnesso;
    int batteria = -1;
    bool inCarica = false;
    double oreRestanti = -1.0;
};

struct Riepilogo {
    bool ghubAttivo = false;
    int connessi = 0;
    int batteriaMin = -1;
    bool alcunoInCarica = false;
    std::wstring nomePrincipale;
    std::vector<Dispositivo> dispositivi;
};

namespace dettagli {

inline std::wstring daUtf8(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::wstring minuscolo(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower((wint_t)c);
    return s;
}

inline std::string minuscoloAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

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
    std::string str(const char* k, const char* def = "") const {
        const Json* v = trova(k);
        return (v && v->tipo == Str) ? v->s : std::string(def);
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

inline Tipo mappaTipo(const std::string& devType, const std::string& displayName) {
    if (devType == "MOUSE") return Tipo::Mouse;
    if (devType == "KEYBOARD") return Tipo::Tastiera;
    if (devType == "HEADSET") return Tipo::Cuffie;
    const std::string low = minuscoloAscii(displayName);
    if (low.find("g435") != std::string::npos || low.find("g733") != std::string::npos ||
        low.find("g935") != std::string::npos || low.find("headset") != std::string::npos) {
        return Tipo::Cuffie;
    }
    return Tipo::Altro;
}

inline Stato mappaStato(const std::string& state) {
    if (state == "ACTIVE" || state == "LOADING_RESOURCES") return Stato::Attivo;
    if (state == "ASLEEP") return Stato::Riposo;
    return Stato::Disconnesso;
}

inline std::wstring formattaConnessione(const std::string& conn) {
    if (conn == "WIRELESS") return L"Wireless";
    if (conn == "USB") return L"USB";
    if (conn == "BLUETOOTH") return L"Bluetooth";
    if (conn == "WIRED") return L"Wired";
    if (conn == "LIGHTSPEED") return L"Lightspeed";
    return daUtf8(conn);
}

inline int prioritaStato(Stato s) {
    switch (s) {
        case Stato::Attivo:      return 0;
        case Stato::Riposo:      return 1;
        case Stato::Disconnesso: return 2;
    }
    return 3;
}

inline bool wsRecvText(HINTERNET hWs, std::string& outMsg, bool& peerClosed) {
    outMsg.clear();
    peerClosed = false;
    std::string buf(8192, '\0');
    for (;;) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType{};
        const DWORD err = WinHttpWebSocketReceive(hWs, buf.data(), (DWORD)buf.size(), &bytesRead, &bufType);
        if (err != NO_ERROR) return false;
        if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            peerClosed = true;
            return false;
        }
        if (bytesRead > 0) {
            if (outMsg.size() + bytesRead > 4 * 1024 * 1024) return false;
            outMsg.append(buf.data(), bytesRead);
        }
        if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            return true;
        }
        if (bufType != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
            bufType != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            return false;
        }
    }
}

inline bool wsRequest(HINTERNET hWs, const std::string& msgId, const std::string& path, Json& outResp, bool& peerClosed) {
    std::string req = "{\"msgId\":";
    escapeJsonStr(msgId, req);
    req += ",\"verb\":\"GET\",\"path\":";
    escapeJsonStr(path, req);
    req += "}";

    if (WinHttpWebSocketSend(hWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (PVOID)req.data(), (DWORD)req.size()) != NO_ERROR) {
        return false;
    }

    std::string msg;
    for (int tries = 0; tries < 24; tries++) {
        if (!wsRecvText(hWs, msg, peerClosed)) return false;
        Json j;
        if (!parseJson(msg, j) || j.tipo != Json::Obj) continue;
        // Ignora il messaggio OPTIONS iniziale o eventi asincroni con msgId diverso
        if (j.str("msgId") == msgId) {
            outResp = std::move(j);
            return true;
        }
    }
    return false;
}

inline bool ugualeDispositivo(const Dispositivo& a, const Dispositivo& b) {
    if (a.id != b.id || a.nome != b.nome || a.connessione != b.connessione ||
        a.tipo != b.tipo || a.stato != b.stato || a.batteria != b.batteria || a.inCarica != b.inCarica) {
        return false;
    }
    const bool haOreA = a.oreRestanti > 0.0, haOreB = b.oreRestanti > 0.0;
    if (haOreA != haOreB) return false;
    if (haOreA && std::lround(a.oreRestanti * 10.0) != std::lround(b.oreRestanti * 10.0)) return false;
    return true;
}

inline bool ugualeRiepilogo(const Riepilogo& a, const Riepilogo& b) {
    if (a.ghubAttivo != b.ghubAttivo || a.connessi != b.connessi || a.batteriaMin != b.batteriaMin ||
        a.alcunoInCarica != b.alcunoInCarica || a.nomePrincipale != b.nomePrincipale ||
        a.dispositivi.size() != b.dispositivi.size()) {
        return false;
    }
    for (size_t i = 0; i < a.dispositivi.size(); i++) {
        if (!ugualeDispositivo(a.dispositivi[i], b.dispositivi[i])) return false;
    }
    return true;
}

}  // namespace dettagli

inline Riepilogo interrogaGHub(HANDLE cancelEvent = nullptr) {
    Riepilogo out;
    HINTERNET hSession = WinHttpOpen(L"FluidWin-Logitech/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return out;
    WinHttpSetTimeouts(hSession, 1200, 1200, 1500, 1500);

    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", 9010, 0);
    HINTERNET hRequest = hConnect
        ? WinHttpOpenRequest(hConnect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
        : nullptr;
    HINTERNET hWs = nullptr;
    bool peerClosed = false;

    auto chiudi = [&]() {
        if (hWs) {
            if (!peerClosed) WinHttpWebSocketClose(hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(hWs);
            hWs = nullptr;
        }
        if (hRequest) { WinHttpCloseHandle(hRequest); hRequest = nullptr; }
        if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
        if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
    };

    if (!hRequest) { chiudi(); return out; }

    static constexpr const wchar_t* HDRS = L"Origin: file://\r\nSec-WebSocket-Protocol: json\r\n";
    if (!WinHttpAddRequestHeaders(hRequest, HDRS, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD) ||
        !WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        chiudi();
        return out;
    }

    hWs = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    WinHttpCloseHandle(hRequest);
    hRequest = nullptr;
    if (!hWs) { chiudi(); return out; }

    out.ghubAttivo = true;

    dettagli::Json devResp;
    if (!dettagli::wsRequest(hWs, "1", "/devices/list", devResp, peerClosed)) {
        chiudi();
        return out;
    }

    const dettagli::Json* pay = devResp.trova("payload");
    const dettagli::Json* infos = pay ? pay->trova("deviceInfos") : nullptr;
    if (!infos || infos->tipo != dettagli::Json::Arr) {
        chiudi();
        return out;
    }

    struct Entry {
        Dispositivo dev;
        std::string idUtf8;
    };
    std::vector<Entry> entries;
    entries.reserve(infos->arr.size());

    for (const auto& item : infos->arr) {
        if (item.tipo != dettagli::Json::Obj) continue;
        const std::string idUtf8 = item.str("id");
        const std::string dispName = item.str("displayName");
        const std::string devType = item.str("deviceType");
        const std::string state = item.str("state");
        const std::string conn = item.str("connectionType");

        Dispositivo d;
        d.id = dettagli::daUtf8(idUtf8);
        d.nome = dispName.empty() ? d.id : dettagli::daUtf8(dispName);
        d.connessione = dettagli::formattaConnessione(conn);
        d.tipo = dettagli::mappaTipo(devType, dispName);
        d.stato = dettagli::mappaStato(state);
        entries.push_back({std::move(d), idUtf8});
    }

    for (auto& e : entries) {
        if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0) break;
        if (e.dev.stato == Stato::Disconnesso || e.idUtf8.empty()) continue;

        dettagli::Json batResp;
        if (!dettagli::wsRequest(hWs, "b_" + e.idUtf8, "/battery/" + e.idUtf8 + "/state", batResp, peerClosed)) {
            if (peerClosed) break;
            continue;
        }
        const dettagli::Json* res = batResp.trova("result");
        if (res && res->str("code") == "NO_SUCH_PATH") continue;

        const dettagli::Json* bPay = batResp.trova("payload");
        if (!bPay || bPay->tipo != dettagli::Json::Obj) continue;

        if (const auto* pct = bPay->trova("percentage"); pct && pct->tipo == dettagli::Json::Num) {
            e.dev.batteria = std::clamp((int)std::lround(pct->n), 0, 100);
        }
        if (const auto* chg = bPay->trova("charging"); chg && chg->tipo == dettagli::Json::Bool) {
            e.dev.inCarica = chg->b;
        }
        if (const auto* mil = bPay->trova("mileage"); mil && mil->tipo == dettagli::Json::Num && mil->n > 0.0) {
            e.dev.oreRestanti = mil->n;
        }
    }

    chiudi();

    out.dispositivi.reserve(entries.size());
    for (auto& e : entries) out.dispositivi.push_back(std::move(e.dev));

    std::sort(out.dispositivi.begin(), out.dispositivi.end(), [](const Dispositivo& a, const Dispositivo& b) {
        const int pa = dettagli::prioritaStato(a.stato), pb = dettagli::prioritaStato(b.stato);
        if (pa != pb) return pa < pb;
        const bool ba = a.batteria >= 0, bb = b.batteria >= 0;
        if (ba != bb) return ba;
        const std::wstring na = dettagli::minuscolo(a.nome), nb = dettagli::minuscolo(b.nome);
        if (na != nb) return na < nb;
        return a.id < b.id;
    });

    for (const auto& d : out.dispositivi) {
        if (d.stato == Stato::Disconnesso) continue;
        out.connessi++;
        if (d.inCarica) out.alcunoInCarica = true;
        if (d.batteria >= 0 && (out.batteriaMin < 0 || d.batteria < out.batteriaMin)) {
            out.batteriaMin = d.batteria;
            out.nomePrincipale = d.nome;
        }
    }

    return out;
}

class Monitor {
public:
    std::atomic<uint64_t> versione{0};

    ~Monitor() { ferma(); }

    void avvia(HWND hwndNotifica, UINT msgNotifica) {
        if (threadH) return;
        notificaHwnd = hwndNotifica;
        notificaMsg = msgNotifica;
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

    void aggiornaOra() {
        if (wakeEvent) SetEvent(wakeEvent);
    }

private:
    SRWLOCK lock = SRWLOCK_INIT;
    Riepilogo stato;
    HANDLE stopEvent = nullptr;
    HANDLE wakeEvent = nullptr;
    HANDLE threadH = nullptr;
    HWND notificaHwnd = nullptr;
    UINT notificaMsg = 0;

    void scansiona() {
        Riepilogo nuovo = interrogaGHub(stopEvent);
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return;

        bool cambiato = false;
        AcquireSRWLockExclusive(&lock);
        if (!dettagli::ugualeRiepilogo(stato, nuovo)) {
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
        self->scansiona();
        for (;;) {
            HANDLE waits[2] = {self->stopEvent, self->wakeEvent};
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 10000);
            if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
            self->scansiona();
        }
        return 0;
    }
};

inline bool apriGHub() {
    static constexpr const wchar_t* CANDIDATI[] = {
        L"C:\\Program Files\\LGHUB\\lghub.exe",
        L"C:\\Program Files\\LGHUB\\lghub_system_tray.exe",
        L"C:\\Program Files\\LGHUB\\system_tray\\lghub_system_tray.exe"
    };
    for (const wchar_t* p : CANDIDATI) {
        const DWORD attr = GetFileAttributesW(p);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return (INT_PTR)ShellExecuteW(nullptr, L"open", p, nullptr, nullptr, SW_SHOWNORMAL) > 32;
        }
    }
    return false;
}

}  // namespace logitech
