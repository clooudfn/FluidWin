// Dati del pannello widget: meteo (Open-Meteo, servizio gratuito senza account: si invia solo la posizione della
// citta') e notizie (feed RSS ufficiale dell'ANSA). Si scaricano in un thread a parte con WinHTTP, al massimo ogni
// 10 minuti e solo quando il pannello si apre; il pannello legge una copia gia' pronta.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <cmath>

namespace widget {

struct Giorno { int codice = 0; double max = 0, min = 0; std::wstring nome; };
struct Notizia { std::wstring titolo, link, quando; };

struct Istantanea {
    bool meteoOk = false;
    double temp = 0, vento = 0, umidita = 0;
    int codice = 0;
    std::vector<Giorno> giorni;
    std::vector<Notizia> notizie;
};

// descrizione del codice meteo WMO (quello che usa Open-Meteo)
inline const wchar_t* descrizione(int c) {
    if (c == 0) return L"Sereno";
    if (c == 1) return L"Quasi sereno";
    if (c == 2) return L"Parzialmente nuvoloso";
    if (c == 3) return L"Coperto";
    if (c == 45 || c == 48) return L"Nebbia";
    if (c >= 51 && c <= 57) return L"Pioviggine";
    if (c >= 61 && c <= 67) return L"Pioggia";
    if (c >= 71 && c <= 77) return L"Neve";
    if (c >= 80 && c <= 82) return L"Rovesci";
    if (c >= 85 && c <= 86) return L"Rovesci di neve";
    if (c >= 95) return L"Temporale";
    return L"";
}

class Dati {
public:
    // la citta' del meteo sta nel registro (HKCU\Software\FluidWin\Impostazioni: MeteoCitta, MeteoLat, MeteoLon),
    // non nel codice; senza, Roma
    std::wstring citta = L"Roma";
    double lat = 41.9028, lon = 12.4964;
    std::atomic<bool> pronto{false};   // arrivati dati nuovi (il pannello si ridisegna)

    Dati() {
        InitializeCriticalSection(&cs);
        const wchar_t* k = L"Software\\FluidWin\\Impostazioni";
        wchar_t b[128]; DWORD n = sizeof(b);
        if (RegGetValueW(HKEY_CURRENT_USER, k, L"MeteoCitta", RRF_RT_REG_SZ, nullptr, b, &n) == ERROR_SUCCESS && b[0]) {
            wchar_t la[32], lo[32]; DWORD nl = sizeof(la), no = sizeof(lo);
            if (RegGetValueW(HKEY_CURRENT_USER, k, L"MeteoLat", RRF_RT_REG_SZ, nullptr, la, &nl) == ERROR_SUCCESS &&
                RegGetValueW(HKEY_CURRENT_USER, k, L"MeteoLon", RRF_RT_REG_SZ, nullptr, lo, &no) == ERROR_SUCCESS) {
                const double a = _wtof(la), o = _wtof(lo);
                if (a >= -90 && a <= 90 && o >= -180 && o <= 180 && (a != 0 || o != 0)) { citta = b; lat = a; lon = o; }
            }
        }
    }

    // chiede dati nuovi se gli ultimi hanno piu' di 10 minuti (o non ci sono)
    void aggiorna() {
        const ULONGLONG ora = GetTickCount64();
        if (inCorso || (ultimo && ora - ultimo < 10 * 60 * 1000)) return;
        inCorso = true; ultimo = ora;
        HANDLE t = CreateThread(nullptr, 0, lavoro, this, 0, nullptr);
        if (t) CloseHandle(t); else inCorso = false;
    }

    Istantanea copia() { EnterCriticalSection(&cs); Istantanea c = dati; LeaveCriticalSection(&cs); return c; }

private:
    CRITICAL_SECTION cs{};
    Istantanea dati;
    std::atomic<bool> inCorso{false};
    ULONGLONG ultimo = 0;

    static std::string scarica(const wchar_t* host, const std::wstring& percorso) {
        std::string out;
        HINTERNET s = WinHttpOpen(L"FluidWin/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
        if (!s) return out;
        WinHttpSetTimeouts(s, 5000, 5000, 8000, 8000);
        HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", percorso.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        DWORD stato = 0, ls = sizeof(stato);
        if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr) &&
            // solo le risposte buone: una pagina d'errore (404, 500...) non va letta come dati
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &stato, &ls, WINHTTP_NO_HEADER_INDEX) &&
            stato == 200) {
            DWORD n = 0;
            while (WinHttpQueryDataAvailable(r, &n) && n > 0 && out.size() < 4 * 1024 * 1024) {
                std::string b(n, '\0'); DWORD letti = 0;
                if (!WinHttpReadData(r, b.data(), n, &letti) || !letti) break;
                out.append(b.data(), letti);
            }
        }
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return out;
    }

    static std::wstring daUtf8(const std::string& s) {
        if (s.empty()) return L"";
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }

    // ---- JSON di Open-Meteo: bastano un numero dopo una chiave e un elenco di numeri ----
    static size_t dopo(const std::string& j, const std::string& chiave, size_t da = 0) {
        const size_t p = j.find("\"" + chiave + "\"", da);
        return p == std::string::npos ? p : j.find(':', p) + 1;
    }
    static double numero(const std::string& j, const std::string& chiave, size_t da, bool& ok) {
        const size_t p = dopo(j, chiave, da);
        if (p == std::string::npos || p == 0) { ok = false; return 0; }
        return strtod(j.c_str() + p, nullptr);
    }
    static std::vector<double> elenco(const std::string& j, const std::string& chiave, size_t da) {
        std::vector<double> v;
        size_t p = dopo(j, chiave, da);
        if (p == std::string::npos || p == 0) return v;
        p = j.find('[', p); const size_t fine = j.find(']', p);
        if (p == std::string::npos || fine == std::string::npos) return v;
        const char* c = j.c_str() + p + 1;
        while (c < j.c_str() + fine) { char* e = nullptr; const double x = strtod(c, &e); if (e == c) break; v.push_back(x); c = e; while (*c == ',' || *c == ' ') c++; }
        return v;
    }

    void meteo(Istantanea& d) {
        wchar_t p[400];
        swprintf(p, 400, L"/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,wind_speed_10m,relative_humidity_2m"
                         L"&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=5", lat, lon);   // (giorni del fuso della citta')
        const std::string j = scarica(L"api.open-meteo.com", p);
        const size_t cur = j.find("\"current\":");
        if (cur == std::string::npos) return;
        bool ok = true;
        d.temp = numero(j, "temperature_2m", cur, ok);
        d.codice = (int)numero(j, "weather_code", cur, ok);
        d.vento = numero(j, "wind_speed_10m", cur, ok);
        d.umidita = numero(j, "relative_humidity_2m", cur, ok);
        const size_t day = j.find("\"daily\":");
        if (day != std::string::npos) {
            const auto cod = elenco(j, "weather_code", day), mx = elenco(j, "temperature_2m_max", day), mn = elenco(j, "temperature_2m_min", day);
            SYSTEMTIME st; GetLocalTime(&st);
            static const wchar_t* gg[7] = {L"Dom", L"Lun", L"Mar", L"Mer", L"Gio", L"Ven", L"Sab"};
            for (size_t i = 0; i < cod.size() && i < mx.size() && i < mn.size(); i++)
                d.giorni.push_back({(int)cod[i], mx[i], mn[i], i == 0 ? L"Oggi" : gg[(st.wDayOfWeek + i) % 7]});
        }
        d.meteoOk = ok;
    }

    // ---- RSS dell'ANSA ----
    static std::string tra(const std::string& s, const std::string& a, const std::string& b, size_t& da) {
        const size_t p = s.find(a, da); if (p == std::string::npos) { da = std::string::npos; return ""; }
        const size_t q = s.find(b, p + a.size()); if (q == std::string::npos) { da = std::string::npos; return ""; }
        da = q + b.size();
        return s.substr(p + a.size(), q - p - a.size());
    }
    static std::wstring pulisci(std::string s) {
        if (s.rfind("<![CDATA[", 0) == 0 && s.size() >= 12) s = s.substr(9, s.size() - 12);
        std::wstring w = daUtf8(s), o;
        for (size_t i = 0; i < w.size(); i++) {
            if (w[i] == L'&') {
                const size_t f = w.find(L';', i);
                if (f != std::wstring::npos && f - i < 10) {
                    const std::wstring e = w.substr(i + 1, f - i - 1);
                    unsigned long c = 0;
                    if (e == L"amp") c = L'&'; else if (e == L"quot") c = L'"'; else if (e == L"apos") c = L'\''; else if (e == L"lt") c = L'<'; else if (e == L"gt") c = L'>';
                    else if (!e.empty() && e[0] == L'#') c = e.size() > 1 && (e[1] == L'x' || e[1] == L'X') ? wcstoul(e.c_str() + 2, nullptr, 16) : wcstoul(e.c_str() + 1, nullptr, 10);
                    if (c && c <= 0x10FFFF) {
                        // oltre 0xFFFF (emoji): due wchar_t (coppia surrogata), prima si troncava
                        if (c > 0xFFFF) { c -= 0x10000; o += (wchar_t)(0xD800 + (c >> 10)); o += (wchar_t)(0xDC00 + (c & 0x3FF)); }
                        else o += (wchar_t)c;
                        i = f; continue;
                    }
                }
            }
            o += (w[i] == L'\n' || w[i] == L'\r' || w[i] == L'\t') ? L' ' : w[i];
        }
        while (!o.empty() && o.back() == L' ') o.pop_back();
        while (!o.empty() && o.front() == L' ') o.erase(o.begin());
        return o;
    }

    void notizie(Istantanea& d) {
        const std::string x = scarica(L"www.ansa.it", L"/sito/ansait_rss.xml");
        size_t p = 0;
        while (d.notizie.size() < 8) {
            const std::string item = tra(x, "<item>", "</item>", p);
            if (p == std::string::npos) break;
            size_t q = 0; Notizia n;
            n.titolo = pulisci(tra(item, "<title>", "</title>", q)); q = 0;
            n.link = pulisci(tra(item, "<link>", "</link>", q)); q = 0;
            const std::string data = tra(item, "<pubDate>", "</pubDate>", q);
            // "Sat, 01 Nov 2026 18:42:00 +0100": basta l'ora
            const size_t due = data.find(':');
            if (due != std::string::npos && due >= 2) n.quando = daUtf8(data.substr(due - 2, 5));
            if (!n.titolo.empty() && n.link.rfind(L"https://", 0) == 0) d.notizie.push_back(n);
        }
    }

    static DWORD WINAPI lavoro(LPVOID p) {
        Dati* me = (Dati*)p;
        Istantanea d;
        me->meteo(d);
        me->notizie(d);
        EnterCriticalSection(&me->cs);
        if (d.meteoOk || me->dati.giorni.empty()) { me->dati.meteoOk = d.meteoOk; me->dati.temp = d.temp; me->dati.codice = d.codice; me->dati.vento = d.vento; me->dati.umidita = d.umidita; me->dati.giorni = d.giorni; }
        if (!d.notizie.empty()) me->dati.notizie = d.notizie;
        const bool vuoto = !d.meteoOk && d.notizie.empty();
        LeaveCriticalSection(&me->cs);
        if (vuoto) me->ultimo = GetTickCount64() - 9 * 60 * 1000;   // niente rete: si riprova fra un minuto
        me->pronto = true;
        me->inCorso = false;
        return 0;
    }
};

}  // namespace widget
