// Dock in stile macOS per Windows: vetro acrilico sotto, icone disegnate con Direct2D sopra (DirectComposition).
// Mostra le app aggiunte alla barra delle applicazioni e quelle aperte; ingrandimento "a lente" al passaggio del
// mouse, etichetta col nome, pallino sotto le app aperte, rimbalzo all'avvio. Riserva lo spazio in basso come
// barra delle applicazioni (appbar), cosi' le finestre ingrandite non ci finiscono sotto.
// A destra un'"isola di stato" con lo stesso vetro: widget, icone nascoste, rete, volume, data e ora
// (le icone della barra di Windows, che resta nascosta, ridisegnate in stile iOS).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <windowsx.h>
#include <iphlpapi.h>
#include <appmodel.h>   // pacchetti delle app dello Store (aumidDalManifesto)
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include "raggio.h"
#include "sistema.h"
#include "vetro.h"
#include "gpu.h"
#include "pool.h"
#include "altrui.h"
#include "widget.h"
#include "jumplist.h"
#include "anteprime.h"
#include "calendario.h"
#include "wifi.h"
#include "indice.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <initializer_list>
#include <unordered_map>
#include <mutex>
#include <unordered_set>
#include <deque>
#include <functional>
#include <memory>
#include <tlhelp32.h>
#include <wincodec.h>
#include <knownfolders.h>
#include <powrprof.h>
#include "notifiche.h"
#include "menu.h"
#include "notch.h"
#include "comodita.h"
#include "mixer.h"
#include "pila.h"
#include "appunti.h"
#include <ole2.h>
#include <knownfolders.h>
#include "testi.h"

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

namespace dock {

static const UINT WM_APPBAR = WM_APP + 20;

// Spazio riservato al dock (impostazione): il dock si registra come "appbar", come la barra di Windows, e le finestre
// ingrandite si fermano sopra. Il suo HWND sta nel registro finche' la prenotazione c'e': se FluidWin termina in modo
// anomalo, il guardiano (o il prossimo avvio, con ripristinaTutte) la toglie, altrimenti resterebbe un vuoto in basso
static const wchar_t* const CHIAVE_APPBAR = L"Software\\FluidWin";
static void togliAppbarRimasta() {
    DWORD h = 0, n = sizeof(h);
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"Appbar", RRF_RT_REG_DWORD, nullptr, &h, &n) != ERROR_SUCCESS) return;
    APPBARDATA ab{}; ab.cbSize = sizeof(ab); ab.hWnd = (HWND)(ULONG_PTR)h;
    // solo se quell'HWND non c'e' piu' o e' ancora il dock: un numero riusato da un'altra finestra (magari la barra di
    // un altro programma) non va toccato
    wchar_t c[64] = L"";
    if (!IsWindow(ab.hWnd) || (GetClassNameW(ab.hWnd, c, 64) && !wcscmp(c, L"FluidWin.DockIcone"))) SHAppBarMessage(ABM_REMOVE, &ab);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"Appbar");
}
static const UINT_PTR TIMER_INVIO = 7;   // "icone nascoste": Invio poco dopo Win+B
static const UINT_PTR TIMER_BORDO = 8;   // dock nascosto: controlla se il mouse arriva sul bordo in basso
static const UINT_PTR TIMER_DOPO = 9;    // launcher o centro di controllo da aprire quando l'altro ha finito di chiudersi
static const UINT_PTR TIMER_CITTA = 10;  // (finestra del pannello) ricerca della citta' del meteo: attesa dopo l'ultimo tasto e risultati
static const UINT_PTR TIMER_FILE_L = 11; // (finestra del launcher) file e cartelle: la ricerca parte 0,2 s dopo l'ultimo tasto
static const UINT_PTR TIMER_DESKTOP = 12; // "Mostra il desktop" / "Ripristina le finestre": una finestra ogni 45 ms
static const UINT_PTR TIMER_ANTEPRIME = 13; // anteprime delle finestre: quando aprirle e chiuderle (ogni 80 ms, solo col mouse sul dock o aperte)
static const UINT_PTR TIMER_TESTA = 14;     // testa del dock (musica e timer): ridisegno a ~16 fotogrammi al secondo, solo mentre c'e'
static const wchar_t* const AVVIO_LAUNCHER = L"*launcher";   // la voce "App" del dock
static const wchar_t* const AVVIO_CONTROLLO = L"*controllo";   // l'ultima voce: il centro di controllo
static const wchar_t* const AVVIO_PILA = L"*pila";             // pila della cartella Download (prima del cestino)
static const wchar_t* const AVVIO_CESTINO = L"*cestino";       // cestino (prima del centro di controllo)
// voci di fondo, sempre in quest'ordine dopo le app aperte, con un separatore prima
static bool voceDiFondo(const std::wstring& a) { return a == AVVIO_PILA || a == AVVIO_CESTINO || a == AVVIO_CONTROLLO; }

static std::wstring cartellaDownload() {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

struct Voce {
    std::wstring nome;
    std::wstring exe;          // percorso dell'eseguibile, minuscolo (per riconoscere le finestre)
    std::wstring aumid;        // AppUserModelID, se c'e'
    std::wstring avvio;        // cosa aprire al clic (collegamento .lnk o shell:AppsFolder\...)
    // app installate con Squirrel (Discord, Slack...): il collegamento apre "<cartella>\Update.exe --processStart X.exe"
    // o il lanciatore "<cartella>\X.exe", ma le finestre sono di "<cartella>\app-<versione>\X.exe". Qui "<cartella>\app-" e "\X.exe" (minuscoli), vuoti
    // per le altre app (vedi exeSquirrel)
    std::wstring sqPrima, sqDopo;
    // app di pacchetto (Teams...) fissata: "shell:AppsFolder\<AUMID>", per aprirla e per l'icona. Il collegamento
    // punta all'exe di una versione precisa (WindowsApps\<nome>_<versione>_...), che sparisce a ogni aggiornamento
    std::wstring pacchetto;
    // collegamento di Squirrel a una versione cancellata (vedi squirrelVersione): l'exe della versione di adesso, da
    // aprire e da cui prendere l'icona al posto del collegamento
    std::wstring ripiego;
    std::wstring argomenti;    // del collegamento: per riconoscere i doppi veri e per aprire il ripiego (R20261121-230321-S1, S3)
    // il collegamento indica come icona un file che non c'e' piu' (Chromium e Brave usano un .ico del profilo, che
    // sparisce cambiando o togliendo il profilo): la shell dava l'icona generica di Windows. L'icona si prende dall'exe
    bool iconaRotta = false;
    bool fissata = false;      // aggiunta alla barra
    ID2D1Bitmap1* icona = nullptr;
    std::vector<HWND> finestre;
    double rimbalzoT0 = -1;    // istante di inizio del rimbalzo (avvio), -1 = fermo
    double sobbalzoT0 = -1;    // piccolo sobbalzo (una finestra caduta nel dock la' sopra, 1.3.0), -1 = fermo
    double sobbalzoDopo = -1;  // ... chiesto: parte al prossimo fotogramma del dock, dopo questi secondi (-1 = niente)
    float sobbalzoAmp = 0;     // ... quanto (1 l'icona dell'app, meno le vicine)
    double scala = 1;          // 0..1: le icone nuove crescono, quelle delle app chiuse si restringono (niente scatti)
    bool uscente = false;      // app chiusa: si restringe e poi sparisce
    D2D1_RECT_F rett{};        // ultima posizione disegnata (coordinate della finestra delle icone)
    double dx = 0;             // scostamento orizzontale che torna a 0: l'icona scivola al suo posto nuovo (riordino)
};

static std::wstring minuscolo(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// Collegamento di Squirrel: prima = "<cartella>\app-", dopo = "\x.exe". Due forme:
//  - "<cartella>\update.exe" con argomenti "--processStart X.exe" (installazioni piu' vecchie);
//  - "<cartella>\X.exe", un piccolo lanciatore accanto a update.exe che avvia app-<versione>\X.exe (Discord oggi; il
//    collegamento passa da una forma all'altra con gli aggiornamenti).
// exe e' gia' minuscolo. false se non e' un collegamento di Squirrel
static bool exeSquirrel(const std::wstring& exe, const std::wstring& argomenti, std::wstring& prima, std::wstring& dopo) {
    const size_t barra = exe.find_last_of(L'\\');
    if (barra == std::wstring::npos) return false;
    if (exe.compare(barra + 1, std::wstring::npos, L"update.exe")) {
        // lanciatore: c'e' update.exe nella stessa cartella
        if (GetFileAttributesW((exe.substr(0, barra + 1) + L"update.exe").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        prima = exe.substr(0, barra + 1) + L"app-";
        dopo = exe.substr(barra);
        return true;
    }
    const std::wstring a = minuscolo(argomenti);
    size_t i = a.find(L"--processstart");
    if (i == std::wstring::npos) return false;
    i += 14;
    while (i < a.size() && (a[i] == L' ' || a[i] == L'=')) i++;
    std::wstring nome;
    if (i < a.size() && a[i] == L'"') { const size_t f = a.find(L'"', i + 1); nome = a.substr(i + 1, f == std::wstring::npos ? f : f - i - 1); }
    else nome = a.substr(i, a.find(L' ', i) == std::wstring::npos ? std::wstring::npos : a.find(L' ', i) - i);
    if (nome.empty() || nome.find_first_of(L"\\/") != std::wstring::npos) return false;
    prima = exe.substr(0, barra + 1) + L"app-";
    dopo = L"\\" + nome;
    return true;
}

// Collegamento di Squirrel che punta direttamente a una versione: "<cartella>\app-1.0.9187\discord.exe". Succede
// fissando l'app mentre e' aperta; al primo aggiornamento Squirrel cancella quella cartella e il collegamento resta
// senza destinazione (icona vuota, il clic non apre nulla, le finestre della versione nuova facevano un'icona a
// parte). exe e' gia' minuscolo. Se e' cosi': prima/dopo come exeSquirrel e in 'attuale' l'exe della versione
// installata adesso (la cartella app-* con la versione piu' alta che lo contiene, confrontata per numeri: prima la
// data di modifica, che puo' preferire una versione vecchia rimasta, R20261121-230321-S2); false altrimenti
// "1.0.9269" -> {1, 0, 9269}; 'suffisso' se dopo i numeri c'e' altro ("1.0.0-beta": versione preliminare, che a pari
// numeri vale meno della definitiva, R20261121-230855-S1)
static std::vector<unsigned long> numeriVersione(const wchar_t* s, bool& suffisso) {
    std::vector<unsigned long> n;
    while (*s) {
        if (!iswdigit(*s)) { if (*s != L'.') break; s++; continue; }
        wchar_t* fine = nullptr; n.push_back(wcstoul(s, &fine, 10)); s = fine;
    }
    suffisso = *s != 0;
    return n;
}
static bool squirrelVersione(const std::wstring& exe, std::wstring& prima, std::wstring& dopo, std::wstring& attuale) {
    const size_t barra = exe.find_last_of(L'\\');
    if (barra == std::wstring::npos || barra < 6) return false;
    const size_t cart = exe.find_last_of(L'\\', barra - 1);   // inizio di "\app-<versione>"
    if (cart == std::wstring::npos || exe.compare(cart + 1, 4, L"app-")) return false;
    const std::wstring base = exe.substr(0, cart + 1);
    if (GetFileAttributesW((base + L"update.exe").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    prima = base + L"app-"; dopo = exe.substr(barra);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((base + L"app-*").c_str(), &fd);
    std::vector<unsigned long> vMax; bool sMax = false;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            bool suff = false;
            const std::vector<unsigned long> ver = numeriVersione(fd.cFileName + 4, suff);   // (dopo "app-")
            if (ver.empty()) continue;
            std::wstring c = base; c += fd.cFileName; c += dopo; c = minuscolo(c);
            const bool meglio = attuale.empty() || ver > vMax || (ver == vMax && sMax && !suff);
            if (meglio && GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) { vMax = ver; sMax = suff; attuale = c; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return true;
}

// Le app fissate alla barra di Windows, nel suo ordine. La lista vera e' in HKCU\...\Explorer\Taskband, valore
// Favorites: una sequenza di {1 byte, lunghezza a 4 byte, PIDL}, chiusa da 0xFF (misurato su Windows 11 25H2,
// FavoritesVersion 3). Ogni PIDL si fa leggere alla shell: un collegamento della cartella User Pinned\TaskBar
// (percorso) oppure un'app di pacchetto (il suo AUMID). Le app di pacchetto fissate (WhatsApp, Store, Outlook...)
// stanno solo qui, senza collegamento: prima il dock leggeva solo la cartella e non le mostrava. Sola lettura.
// false se il valore manca o ha una forma diversa (allora vale solo la cartella, come prima)
struct FissataWindows { std::wstring lnk, aumid, nome; };
static bool fissateWindows(std::vector<FissataWindows>& out) {
    out.clear();
    static const wchar_t* chiave = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
    DWORD n = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, chiave, L"Favorites", RRF_RT_REG_BINARY, nullptr, nullptr, &n) != ERROR_SUCCESS || n < 1 || n > 1024 * 1024) return false;
    std::vector<BYTE> b(n);
    if (RegGetValueW(HKEY_CURRENT_USER, chiave, L"Favorites", RRF_RT_REG_BINARY, nullptr, b.data(), &n) != ERROR_SUCCESS) return false;
    b.resize(n);
    size_t i = 0;
    while (i < b.size()) {
        if (b[i] == 0xFF) return true;   // fine della lista
        if (i + 5 > b.size()) return false;
        DWORD cb = 0; memcpy(&cb, &b[i + 1], 4);
        const size_t o = i + 5;
        if (cb < 2 || cb > b.size() - o) return false;
        // il PIDL deve stare tutto dentro i suoi cb byte e finire con il terminatore (la shell lo legge fino a quello)
        size_t j = o; bool chiuso = false;
        while (j + 2 <= o + cb) {
            USHORT l = 0; memcpy(&l, &b[j], 2);
            if (l == 0) { chiuso = true; break; }
            if (l < 3 || j + l > o + cb) break;
            j += l;
        }
        if (!chiuso) return false;
        PIDLIST_ABSOLUTE pidl = (PIDLIST_ABSOLUTE)CoTaskMemAlloc(cb);
        if (!pidl) return false;
        memcpy(pidl, &b[o], cb);
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&si))) && si) {
            FissataWindows f;
            wchar_t* s = nullptr;
            if (SUCCEEDED(si->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &s)) && s) {
                std::wstring p = s; CoTaskMemFree(s); s = nullptr;
                if (p.rfind(L"shell:AppsFolder\\", 0) == 0) p = p.substr(17);
                if (p.size() > 4 && minuscolo(p.substr(p.size() - 4)) == L".lnk") f.lnk = p;
                else if (p.find(L'!') != std::wstring::npos && p.find(L'\\') == std::wstring::npos) f.aumid = p;
            }
            if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s) { f.nome = s; CoTaskMemFree(s); }
            si->Release();
            if (!f.lnk.empty() || !f.aumid.empty()) out.push_back(std::move(f));
        }
        CoTaskMemFree(pidl);
        i = o + cb;
    }
    return true;   // (finita senza 0xFF: si tiene quello che c'era)
}

// exe di un'app di pacchetto senza la versione: "...\windowsapps\msteams_26276.513_x64__8wekyb3d8bbwe\ms-teams.exe"
// -> "...\windowsapps\msteams__8wekyb3d8bbwe\ms-teams.exe" (nome e editore restano). Le altre restano come sono
static std::wstring senzaVersione(const std::wstring& exe) {
    const size_t p = exe.find(L"\\windowsapps\\");
    if (p == std::wstring::npos) return exe;
    const size_t a = p + 13, b = exe.find(L'\\', a);
    if (b == std::wstring::npos) return exe;
    const size_t u = exe.find(L'_', a), e = exe.rfind(L"__", b);
    if (u == std::wstring::npos || e == std::wstring::npos || u >= e || e < a) return exe;
    return exe.substr(0, u) + exe.substr(e);
}

// la finestra con questo eseguibile e' dell'app 'voce' (stesso exe, anche di un'altra versione se e' un'app di
// pacchetto, o la versione di Squirrel che il collegamento apre)
static bool stessoExe(const std::wstring& voce, const std::wstring& prima, const std::wstring& dopo, const std::wstring& exe) {
    if (!voce.empty() && voce == exe) return true;
    if (!voce.empty() && voce.find(L"\\windowsapps\\") != std::wstring::npos && senzaVersione(voce) == senzaVersione(exe)) return true;
    if (prima.empty() || exe.size() <= prima.size() + dopo.size()) return false;
    if (exe.compare(0, prima.size(), prima) || exe.compare(exe.size() - dopo.size(), dopo.size(), dopo)) return false;
    // in mezzo solo la versione, senza altre cartelle
    return exe.find(L'\\', prima.size()) == exe.size() - dopo.size();
}

// AppUserModelID di una finestra (come la raggruppa la barra di Windows)
static std::wstring aumidFinestra(HWND h) {
    std::wstring r;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) r = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    return r;
}

static std::wstring exeProcesso(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    // (percorsi lunghi: si riprova con un buffer piu' grande fino a 32K caratteri; prima oltre MAX_PATH la finestra
    // restava senza exe e spariva dal dock, R20261121-001416-S13)
    std::wstring r;
    // (260, 1040, 4160, 16640, poi 32768 esatti: prima il passo per quattro saltava l'ultimo, R20261121-115814 su S13)
    for (DWORD cap = MAX_PATH;; cap = std::min<DWORD>(cap * 4, 32768)) {
        std::wstring buf(cap, L'\0'); DWORD n = cap;
        if (QueryFullProcessImageNameW(p, 0, buf.data(), &n)) { buf.resize(n); r = std::move(buf); break; }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || cap >= 32768) break;
    }
    CloseHandle(p);
    return minuscolo(r);
}

// Nome leggibile di un eseguibile (descrizione del file), altrimenti il nome del file
static std::wstring nomeExe(const std::wstring& exe) {
    DWORD z = 0, sz = GetFileVersionInfoSizeW(exe.c_str(), &z);
    if (sz) {
        std::vector<BYTE> buf(sz);
        if (GetFileVersionInfoW(exe.c_str(), 0, sz, buf.data())) {
            struct Lingua { WORD l, c; } *lg = nullptr; UINT n = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&lg, &n) && n >= sizeof(Lingua)) {
                wchar_t q[64]; swprintf(q, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", lg->l, lg->c);
                wchar_t* d = nullptr; UINT dl = 0;
                if (VerQueryValueW(buf.data(), q, (void**)&d, &dl) && dl > 1 && d[0]) return d;
            }
        }
    }
    std::wstring f = exe.substr(exe.find_last_of(L"\\/") + 1);
    const size_t p = f.rfind(L'.'); if (p != std::wstring::npos) f = f.substr(0, p);
    if (!f.empty()) f[0] = (wchar_t)towupper(f[0]);
    return f;
}

// Scorciatoia Win+tasto: apre i pannelli di sistema (widget, impostazioni rapide, notifiche) come le icone della barra
static void tastiWin(WORD vk) {
    INPUT in[4]{};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN; in[1].ki.wVk = vk;
    in[2].ki.wVk = vk; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

class Motore {
public:
    HWND icone = nullptr, vetro = nullptr, vetroStato = nullptr, vetroOra = nullptr;   // vetroOra: la capsula di data e ora
    bool attivo = false;
    long diagMosse = 0, diagFotogrammi = 0;   // diagnostica: movimenti del mouse ricevuti, fotogrammi disegnati
    double diagLente() const { return lente; }
    // riduzione a icona col genio di FluidWin (clic sull'icona dell'app gia' in primo piano); false = non possibile
    bool (*riduci)(HWND) = nullptr;
    // foto per l'animazione di chiusura, prima di chiudere una finestra dal menu del dock (false: niente animazione)
    bool (*preparaChiusura)(HWND) = nullptr;
    // il dock segue il mouse sul bordo in basso degli altri schermi (impostazione; spento resta sul principale). Si
    // imposta prima di inizializza
    bool altriSchermi = true;
    void impostaAltriSchermi(bool si) {
        if (si == altriSchermi) return;
        altriSchermi = si; bordoAltro = nullptr;
        // spento con il dock su un altro schermo: torna sul principale
        const HMONITOR principale = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!si && monitor != principale) { if (registro) scrivi("altri schermi spento: torna sul principale"); cambiaSchermo(principale); }
    }

    // centro dello schermo del dock, ricordato fra un avvio e l'altro
    static void ricordaSchermo(const RECT& s) {
        const DWORD x = (DWORD)((s.left + s.right) / 2), y = (DWORD)((s.top + s.bottom) / 2);
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"SchermoDockX", REG_DWORD, &x, sizeof(x));
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"SchermoDockY", REG_DWORD, &y, sizeof(y));
    }
    static HMONITOR schermoRicordato() {
        DWORD x = 0, y = 0, n = sizeof(DWORD);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"SchermoDockX", RRF_RT_REG_DWORD, nullptr, &x, &n) != ERROR_SUCCESS) return nullptr;
        n = sizeof(DWORD);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"SchermoDockY", RRF_RT_REG_DWORD, nullptr, &y, &n) != ERROR_SUCCESS) return nullptr;
        return MonitorFromPoint({(LONG)(int)x, (LONG)(int)y}, MONITOR_DEFAULTTONULL);   // schermo staccato: nessuno
    }
    void (*registro)(const char*) = nullptr;   // diagnostica (registro di FluidWin con --log)
    void scrivi(const char* s) const { if (registro) registro(s); }

    // diagnostica: nel registro le operazioni del thread principale che durano piu' di 6 ms (fotogrammi persi)
    struct Cronometro {
        const Motore* m; const char* nome; double t0;
        Cronometro(const Motore* mm, const char* n) : m(mm), nome(n), t0(adesso()) {}
        ~Cronometro() { const double d = (adesso() - t0) * 1000; if (d > 6 && m->registro) { char b[96]; snprintf(b, 96, "lento: %s %.1f ms", nome, d); m->scrivi(b); } }
    };

    bool inizializza(HINSTANCE inst) {
        gIstanza() = this;
        // lo schermo dove stava il dock l'ultima volta (se c'e' ancora), altrimenti il principale
        HMONITOR mon = altriSchermi ? schermoRicordato() : nullptr;
        if (!mon) mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        monitor = mon;
        MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
        schermo = mi.rcMonitor;
        ::vetro::schermoDock() = schermo; sis.schermo(schermo);
        // Misure sempre alla scala dello schermo principale, anche se il dock nasce su un altro: cosi' passa da uno
        // schermo all'altro subito, senza ricreare niente. Prima si faceva con la scala del suo schermo e ogni
        // passaggio a uno con un'altra scala riavviava FluidWin (2 s senza dock, con la barra di Windows che ricompariva)
        dpi = ::vetro::dpiMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
        S = latoIcone * dpi; M = S * INGRANDITE;   // icone a riposo e ingrandite (impostazione: impostaLatoIcone)
        PAD = 8 * dpi; GAP = distanzaIcone * dpi; MARGINE = 8 * dpi;   // (GAP: impostazione, impostaDistanzaIcone)
        RAGGIO = raggio::vetro() * dpi;   // come gli angoli DWM del vetro: fondo e vetro devono coincidere
        altezzaDock = S + 2 * PAD;
        W = lunghezzaBordo();   // (ai lati: l'altezza dello schermo; lato si sceglie prima di inizializza)
        H = (int)std::ceil(altezzaDock + MARGINE + (M - S) + 64 * dpi);   // spazio per ingrandimento, rimbalzo ed etichetta

        // vetri: materiale acrilico di Windows 11 (sfocatura vera), sotto alle icone
        WNDCLASSW wv{}; wv.lpfnWndProc = procVetro; wv.hInstance = inst; wv.lpszClassName = L"FluidWin.DockVetro";
        RegisterClassW(&wv);
        vetro = creaVetro(inst);
        vetroStato = creaVetro(inst);
        vetroOra = creaVetro(inst);

        // icone: finestra larga quanto lo schermo, disegnata con Direct2D; la regione limita i clic al dock
        WNDCLASSW wi{}; wi.lpfnWndProc = procIcone; wi.hInstance = inst; wi.lpszClassName = L"FluidWin.DockIcone";
        wi.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wi);
        icone = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                wi.lpszClassName, L"Dock", WS_POPUP, origine().x, origine().y, finW(), finH(),
                                nullptr, nullptr, inst, nullptr);
        BOOL niente = TRUE; DwmSetWindowAttribute(icone, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        if (!grafica()) return false;
        if (!creaPannello(inst)) return false;   // scheda 0
        scambiaScheda(); const bool seconda = creaPannello(inst); scambiaScheda();   // scheda 1 (sta in ltra)
        if (!seconda) return false;
        if (!creaLauncher(inst)) return false;
        notch.clic = [this] { apriCentro(P_NOTIFICHE); };   // un clic sul notch: le notifiche
        notch.apriMusica = [this](const std::wstring& a) { apriAppMusica(a); };   // un clic sulla musica: la sua app
        notch.crea(inst);
        bersaglioDrop.d = this;
        { const HRESULT hr = RegisterDragDrop(icone, &bersaglioDrop);   // file trascinati sul cestino (serve OleInitialize)
          if (registro) { char b[64]; snprintf(b, 64, "trascinamento sul dock: 0x%08lx", (unsigned long)hr); scrivi(b); } }
        notch.font(fontTesto, fontTitoli);

        // nessuno spazio riservato: le finestre ingrandite prendono tutto lo schermo e il dock si nasconde sotto il bordo
        posizionaFinestra();

        // la barra di Windows si nasconde da sola (impostazione ufficiale), si rimette com'era all'uscita
        // (solo se FluidWin ha salvato com'era: senza, dopo un crash il guardiano non potrebbe rimetterla)
        APPBARDATA tb{}; tb.cbSize = sizeof(tb);
        statoBarraPrima = (UINT)SHAppBarMessage(ABM_GETSTATE, &tb);
        if (barraSalvata) { tb.lParam = statoBarraPrima | ABS_AUTOHIDE; SHAppBarMessage(ABM_SETSTATE, &tb); }

        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&audio));
        aggiornaStato(true);
        leggiOrdineFissate();   // (l'ordine scelto trascinando le icone, #23)
        caricaNascoste();       // (le app nascoste dal dock, PR #20)
        aggiornaApp();
        attivo = true;
        ShowWindow(vetro, SW_SHOWNOACTIVATE);

        ShowWindow(icone, SW_SHOWNOACTIVATE);
        notch.inBasso(lato == 1);   // (col dock in alto il notch scende dal bordo in basso)
        notch.posto(schermo, dpi);
        notch.mostra(true);
        dirty = true;
        // comandi delle app (jump list): il thread li legge subito, cosi' sono pronti al primo tasto destro
        if (!jumpList.avvia()) scrivi("dock: thread delle jump list non partito, niente comandi delle app nei menu");
        return true;
    }

    // anteprime delle finestre sulle icone (impostazione "Anteprime delle finestre"; spenta: chiude quelle aperte)
    void impostaAnteprime(bool a) { anteprimeAccese = a; if (!a) ant.chiudi(); }

    // all'uscita, dopo chiudi: aspetta i thread del widget (vedi widget::Dati::ferma) e quello delle jump list
    // (jumplist::Indice::ferma), ognuno al massimo ms. false: almeno uno non ha finito
    bool fermaWidget(DWORD ms) {
        const bool a = widD.ferma(ms);
        const bool b = jumpList.ferma(ms);
        const bool c = wlan.ferma(ms);   // (anche le reti Wi-Fi: una scansione o un collegamento in corso)
        return a && b && c;
    }

    void chiudi() {
        fermaIcone();
        fermaNotifiche();
        if (!attivo) return;
        attivo = false;
        aggiornaAppbar();   // lo spazio riservato si libera (le finestre ingrandite tornano fino in fondo)
        // (solo se il dock l'aveva cambiata: senza, si annullerebbe una scelta fatta intanto dall'utente, R20261121-115814-S2)
        if (barraSalvata) { APPBARDATA tb{}; tb.cbSize = sizeof(tb); tb.lParam = statoBarraPrima; SHAppBarMessage(ABM_SETSTATE, &tb); }
        ShowWindow(icone, SW_HIDE); ShowWindow(vetro, SW_HIDE); ShowWindow(vetroStato, SW_HIDE); ShowWindow(vetroOra, SW_HIDE); notch.mostra(false);
        nascondiSchede(); nascondiLauncher();
    }

    // Da chiamare quando cambiano le finestre aperte (lo fa FluidWin con gli eventi)
    void segnaCambiamento() { appDaAggiornare = true; }

    // Lingua dell'interfaccia cambiata: date, nomi delle voci fisse e pannelli si rifanno nella lingua nuova
    void linguaCambiata() {
        testoOra.clear();   // le date si ricalcolano anche se l'ora non e' cambiata
        aggiornaStato(true);
        appDaAggiornare = true; dirty = true; pDirty = true; lDirty = true;
    }

    // true se il dock deve disegnare il prossimo fotogramma (ingrandimento, rimbalzo, etichetta in movimento)
    bool serveFotogramma() const { return attivo && !nascostoSchermoIntero && !perso && (dirty || inMovimento || pannelloDaDisegnare() || launcherDaDisegnare()); }

    // Il dock va sullo schermo 'verso' (nullptr: schermi cambiati, cioe' risoluzione cambiata, monitor collegato o
    // tolto: resta su quello dov'era se c'e' ancora, altrimenti torna sul principale). Le misure restano quelle dello
    // schermo principale (anche su uno con un'altra scala: cambiarle vorrebbe dire ricreare tutto)
    void cambiaSchermo(HMONITOR verso = nullptr) {
        if (!verso) {
            const POINT c = {(schermo.left + schermo.right) / 2, (schermo.top + schermo.bottom) / 2};
            verso = MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY);
        }
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(verso, &mi)) return;
        // (uno schermo con un'altra scala non cambia niente: le misure restano quelle dello schermo principale)
        monitor = verso;
        schermo = mi.rcMonitor;
        ::vetro::schermoDock() = schermo; sis.schermo(schermo);
        const int w = lunghezzaBordo();
        if (w != W && w > 0) ridimensiona(w, H);
        posizionaFinestra();
        if (appbarOk) aggiornaAppbar();   // lo spazio riservato passa al nuovo schermo
        rettVetro = rettVetroStato = rettVetroOra = RECT{};          // i vetri si rimettono al prossimo fotogramma
        regioneDock = regioneIsola = regioneEtichetta = RECT{};
        if (registro) { char b[96]; snprintf(b, 96, "ora su %ld,%ld-%ld,%ld", schermo.left, schermo.top, schermo.right, schermo.bottom); scrivi(b); }
        // nascondersi o no dipende dalle finestre del nuovo schermo
        controllaFinestre(true);
        notch.posto(schermo, dpi);   // il notch va in alto al centro del nuovo schermo
        ricordaSchermo(schermo);
        dirty = true;
    }

    // superficie della finestra delle icone a una misura nuova (schermo di un'altra larghezza, icone di un'altra misura)
    void ridimensiona(int w, int h) {
        if (!sc || !dc || w <= 0 || h <= 0) return;
        dc->SetTarget(nullptr);
        if (bersaglio) { bersaglio->Release(); bersaglio = nullptr; }
        // (w lunghezza lungo il bordo, h spessore logico: la superficie e' quella della finestra, girata ai lati)
        const int fw = verticale() ? h + extraLato() : w, fh = verticale() ? w : h;
        if (SUCCEEDED(sc->ResizeBuffers(0, (UINT)fw, (UINT)fh, DXGI_FORMAT_UNKNOWN, 0))) { W = w; H = h; }
        IDXGISurface* sup = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup))) {
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                                 D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglio); sup->Release();
        }
        dc->SetTarget(bersaglio);
        if (!bersaglio) perso = true;   // superficie non ricreata: si riparte da capo (riavvio di FluidWin)
    }

    // Come su macOS: il mouse fermo sul bordo in basso di un altro schermo porta li' il dock. Prima scende sotto il
    // bordo dello schermo dov'e', poi risale su quello nuovo
    void spostaVerso(HMONITOR m) {
        if (!m || m == monitor || spostamento) return;
        spostamento = m;
        chiudiTutte(); if (lAperto) chiudiLauncher();
        if (registro) scrivi("si sposta su un altro schermo");
        dirty = true;
    }

    // la GPU e' stata persa (driver aggiornato o riavviato, errore della scheda video): FluidWin si riavvia da solo
    bool perso = false;
    bool gpuPersa() { if (!perso && dev && dev->GetDeviceRemovedReason() != S_OK) perso = true; return perso; }
    void controllaGpu(HRESULT hr) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == (HRESULT)D2DERR_RECREATE_TARGET) perso = true;
    }

    // Disegna un fotogramma per l'istante t; vsync = attende il vblank (se nessun altro lo fa)
    void disegna(double t, bool vsync) {
        if (!attivo || nascostoSchermoIntero || perso) return;
        if (appDaAggiornare && t - ultimoAggiornamento > 0.25) aggiornaApp();
        dtFotogramma = std::clamp(t - ultimoT, 1.0 / 240, 1.0 / 30);   // le animazioni non dipendono dalla frequenza
        ultimoT = t;
        // una sola attesa del vblank: sull'ultimo Present del fotogramma
        const bool p = pannelloDaDisegnare(), lnc = launcherDaDisegnare();
        if (dirty || inMovimento) {
            render(t); diagFotogrammi++;
            controllaGpu(sc->Present(vsync && !p && !lnc ? 1 : 0, 0));
            dirty = false;
        }
        if (p) {
            animaSchede();
            int ultima = -1;   // il vblank si aspetta sull'ultimo Present
            for (int s : {0, 1}) { usa(s); if (pannelloTipo != P_NESSUNO && (pDirty || pAnimazione)) ultima = s; }
            for (int s : {0, 1}) {
                usa(s);
                if (pannelloTipo == P_NESSUNO || !(pDirty || pAnimazione)) continue;
                if (renderPannello()) controllaGpu(scP->Present(vsync && !lnc && s == ultima ? 1 : 0, 0));
            }
        }
        if (lnc && renderLauncher()) controllaGpu(scL->Present(vsync ? 1 : 0, 0));
    }

    // ogni 500 ms (timer di FluidWin)
    void timer() {
        Cronometro cr(this, "timer del dock");
        if (appDaAggiornare) { aggiornaApp(); dirty = true; }
        if (vetroDaRiposizionare) posizionaVetri();
        aggiornaStato(false);
        // cestino pieno o vuoto (ogni 2 s)
        if (giroCestino++ % 4 == 0) {
            SHQUERYRBINFO q{}; q.cbSize = sizeof(q);
            const bool p = SUCCEEDED(SHQueryRecycleBinW(nullptr, &q)) && q.i64NumItems > 0;
            if (p != cestinoPieno) { cestinoPieno = p; dirty = true; }
        }
        // appunti aperti: si aggiornano quando si copia qualcosa
        if (pannelloTipo == P_APPUNTI && !pChiusura && appunti::Cronologia::io().versione != versioneAppunti) {
            versioneAppunti = appunti::Cronologia::io().versione;
            const float nuova = altezzaVista(P_APPUNTI);
            if (nuova != PH) { PH = nuova; pAnimazione = true; }
            pSopra = pPremuto = -1; evidP.clear(); pDirty = true;
        }
        // mixer aperto: si rilegge ogni secondo (app che iniziano o smettono di suonare, volumi cambiati da fuori)
        if (pannelloTipo == P_MIXER && !pChiusura && trascinaMixer < 0 && ++giroMixer % 2 == 0) rileggiMixer();
        // reti Wi-Fi aperte: la copia nuova del thread (l'altezza segue le reti) e una scansione ogni 10 s
        if (pannelloTipo == P_WIFI && !pChiusura) {
            if (wlan.pronto.exchange(false)) {
                copiaWifi();
                const float nuova = altezzaVista(P_WIFI);
                if (nuova != PH) { PH = nuova; pAnimazione = true; }
                // (anche il premuto: le reti possono aver cambiato ordine, e un clic iniziato sulla rete A non deve
                // collegare la rete B finita nella stessa riga. R20261119-145819-S1. E le zone: fino al ridisegno sono
                // quelle dell'elenco vecchio, e una pressione in quel momento prenderebbe l'indice di A per la riga dove
                // ora sta B (R20261119-150434). Senza zone fino al prossimo fotogramma un clic non fa niente)
                pSopra = pPremuto = -1; elementi.clear(); pDirty = true;
            }
            if (adesso() - wlCercataT > 10) { wlCercataT = adesso(); wlan.cerca(); }
        }
        // centro di controllo aperto: il tempo del timer e lo stato del microfono si aggiornano
        if (pannelloTipo == P_CONTROLLO && (comodita::timerAttivo() || comodita::timerSuona() || sensori::Sensori::io().versione != versioneSensoriCentro
                                            || sensori::Sensori::io().versioneTempo != versioneTempoCentro)) {
            versioneSensoriCentro = sensori::Sensori::io().versione; versioneTempoCentro = sensori::Sensori::io().versioneTempo; pDirty = true;
            if (!pChiusura) riadattaControllo();   // la batteria puo' comparire o sparire (ripresa dalla sospensione)
        }
        // poco dopo l'avvio si preparano icone ed elenco delle app dello Store: il primo pannello si apre subito
        if (conteggioTimer == 6) leggiAppSfondo();
        if (conteggioTimer == 12 && tutteApp.empty()) leggiTutteApp();
        raccogliIcone();
        // rete di sicurezza: il pannello si chiude quando perde il fuoco anche se WM_ACTIVATE non arriva
        if (!nessunaScheda() && !schedaFinestra(GetForegroundWindow())) {
            usa(0); const double ta = pannelloTipo != P_NESSUNO ? pT0 : 0; usa(1); const double tb = pannelloTipo != P_NESSUNO ? pT0 : 0;
            // (solo se il pannello aveva avuto il primo piano e Windows non glielo ha rimbalzato via: su alcuni PC non glielo
            // da' mai o lo toglie subito, e prima si richiudeva da solo dopo 0,6 s. Li' lo chiude un clic fuori, vedi
            // controllaClicFuori, o Esc)
            usa(0);
            if (adesso() - std::max(ta, tb) > 0.6 && pFuocoAvuto && !pRipreso) chiudiTutte();
        }
        if (lAperto && !lChiusura && GetForegroundWindow() != launcher && adesso() - lT0 > 0.6) chiudiLauncher();
    }

    // Dove deve entrare il genio per questa finestra: l'icona della sua app nel dock (coordinate schermo)
    // Riduzione "caduta nel dock" (1.3.0): il riquadro della finestra ha toccato le icone. Quella dell'app sobbalza un
    // poco, le due vicine per parte meno; le icone di fondo e App no (non sono "colpite" dalle finestre)
    void sobbalza(HWND h) {
        const Voce* v = voceDi(h);
        if (!v) return;
        int k = -1;
        for (size_t i = 0; i < voci.size(); i++) if (&voci[i] == v) k = (int)i;
        if (k < 0) return;
        static const float AMP[3] = {1.0f, 0.5f, 0.22f};
        for (int d = -2; d <= 2; d++) {
            const int j = k + d;
            if (j < 1 || j >= (int)primaFine || voci[j].uscente) continue;
            // (il tempo lo da' render, l'orologio dei fotogrammi del dock: il dock puo' essere fermo da un pezzo)
            voci[j].sobbalzoDopo = std::abs(d) * 0.035;   // (le vicine un attimo dopo: l'onda si allarga)
            voci[j].sobbalzoAmp = AMP[std::abs(d)];
        }
        dirty = true;
    }

    bool iconaPer(HWND h, RECT& out) {
        if (!attivo || nascostoSchermoIntero) return false;
        // l'elenco si rifa' solo se la finestra non c'e' ancora: si chiama mentre una finestra si sta riducendo,
        // e rifarlo tutto adesso ritarderebbe la partenza dell'animazione
        Voce* v = voceDi(h);
        if (!v) { aggiornaApp(); v = voceDi(h); }
        if (!v) return false;
        for (size_t i = 0; i < voci.size(); i++) if (&voci[i] == v) return rettIcona(i, out);
        return false;
    }

    // Finestra appena comparsa: e' di un'app lanciata da poco dal dock? Se si', dove sta la sua icona (da li' esce
    // col genio). Ogni lancio vale per una sola finestra.
    bool aperturaDaDock(HWND h, RECT& out) {
        const double t = adesso();
        lanci.erase(std::remove_if(lanci.begin(), lanci.end(), [&](const Lancio& l) { return t - l.t > 20; }), lanci.end());
        if (lanci.empty() || !attivo || nascostoSchermoIntero) return false;
        const std::wstring exe = exeProcesso(h), id = aumidFinestra(h);
        for (size_t k = 0; k < lanci.size(); k++) {
            const Lancio& l = lanci[k];
            if (!(stessoExe(l.exe, l.sqPrima, l.sqDopo, exe) || (!l.aumid.empty() && l.aumid == id))) continue;
            const std::wstring c = l.chiave;
            lanci.erase(lanci.begin() + k);
            for (size_t i = 0; i < voci.size(); i++) if (!voci[i].uscente && chiave(voci[i]) == c) return rettIcona(i, out);
            return false;
        }
        return false;
    }

    // La piu' bassa delle finestre del dock (uno dei due vetri): il genio va subito sotto, cosi' gli passa dietro
    HWND finestraPiuBassa() const {
        HWND ultima = icone;
        int trovati = 0;
        for (HWND x = GetWindow(icone, GW_HWNDNEXT); x && trovati < 3; x = GetWindow(x, GW_HWNDNEXT))
            if (x == vetro || x == vetroStato || x == vetroOra) { ultima = x; trovati++; }
        return ultima;
    }

    // Da chiamare quando cambia la finestra in primo piano o la sua posizione: con una finestra ingrandita
    // (o grande quanto lo schermo) il dock si nasconde e riesce solo portando il mouse sul bordo in basso
    void controllaFinestre(bool forza = false) {
        if (!attivo) return;
        HWND fg = GetForegroundWindow();
        if (!fg) return;
        wchar_t c[64] = L""; GetClassNameW(fg, c, 64);
        // le nostre finestre e quelle di passaggio di Windows (Alt+Tab, Visualizzazione attivita', Start): resta com'e'
        static const wchar_t* passaggio[] = {L"Shell_TrayWnd", L"ForegroundStaging", L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame",
                                             L"Windows.UI.Core.CoreWindow", L"TaskSwitcherWnd", L"#32768"};
        if (!wcsncmp(c, L"FluidWin.", 9)) return;
        for (auto x : passaggio) if (!wcscmp(c, x)) return;
        bool n = false, nI;
        char perche[200] = "";
        // il monitor si chiede ogni volta: Windows rinnova questi identificativi quando lo schermo si spegne e si
        // riaccende o cambia risoluzione, e uno salvato all'avvio non corrisponderebbe piu' a nulla
        monitor = MonitorFromRect(&schermo, MONITOR_DEFAULTTOPRIMARY);
        if (wcscmp(c, L"Progman") && wcscmp(c, L"WorkerW")) {
            // la finestra stessa e la sua proprietaria (alcune app hanno una proprietaria invisibile: si guarda solo se si vede)
            for (HWND r : {GetAncestor(fg, GA_ROOT), GetAncestor(fg, GA_ROOTOWNER)}) {
                if (!r || n || !IsWindowVisible(r) || IsIconic(r) || MonitorFromWindow(r, MONITOR_DEFAULTTONULL) != monitor) continue;
                RECT w; GetWindowRect(r, &w);
                snprintf(perche, sizeof(perche), "%p fg %p zoom %d rett %ld,%ld-%ld,%ld schermo %ld,%ld-%ld,%ld", (void*)r, (void*)fg,
                         (int)IsZoomed(r), w.left, w.top, w.right, w.bottom, schermo.left, schermo.top, schermo.right, schermo.bottom);
                // alcune app ingrandite lasciano 1-2 px liberi sul bordo della barra di Windows a scomparsa
                // (con lo spazio riservato le finestre ingrandite si fermano sopra il dock: non lo coprono)
                n = (IsZoomed(r) && !spazioRiservato) || (w.left <= schermo.left + 2 && w.top <= schermo.top + 2 && w.right >= schermo.right - 2 && w.bottom >= schermo.bottom - 2);
                // come i dock "intelligenti": si nasconde anche quando la finestra in primo piano gli finisce sopra
                if (!n) {
                    RECT vis{}; if (FAILED(DwmGetWindowAttribute(r, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) vis = w;
                    const float topDock = yBaseRiposo() - S - PAD;
                    const RECT dockR = aSchermo({zonaDockL, topDock, zonaDockR, (float)H});
                    const RECT isolaR = isola.right > isola.left ? aSchermo({isola.left, topDock, isola.right, (float)H}) : RECT{};
                    RECT x;
                    n = IntersectRect(&x, &vis, &dockR) || IntersectRect(&x, &vis, &isolaR);
                }
            }
        }
        // a scomparsa spento: il dock resta su sopra qualsiasi finestra (le app a schermo intero lo tolgono a parte,
        // vedi schermoIntero)
        if (!autoNascondi) n = false;
        // app a schermo intero (vedi schermoIntero): giu' anche col dock fisso, che altrimenti coprirebbe il video
        if (appSchermoIntero) { n = true; if (!*perche) snprintf(perche, sizeof(perche), "app a schermo intero"); }
        // orario e centro di controllo restano sempre giu' e risalgono solo col mouse sul bordo in basso
        nI = true;
        if (!forza && n == vuoleNascondersi && nI == vuoleNascondersiI) return;
        vuoleNascondersi = n; vuoleNascondersiI = nI; dirty = true;
        if (registro) { char b[360]; snprintf(b, 360, "a scomparsa: %s (primo piano %ls) %s", n ? "finestra a schermo intero" : "solo isola", c, perche); scrivi(b); }
        if (nI) SetTimer(icone, TIMER_BORDO, 40, nullptr);
        else KillTimer(icone, TIMER_BORDO);
        if (!n) { rivelato = false; fuoriDal = -1; }
        if (!nI) { rivelatoI = false; fuoriDalI = -1; }
    }

    // tipo: 0 niente, 1 app a schermo intero (video, presentazione), 2 gioco
    // Solo coi giochi il dock si toglie del tutto (li' il mouse sul bordo e' normale). Con le altre app a schermo intero
    // (browser con F11, ChatGPT a tutto schermo, video) scende sotto il bordo e risale col mouse in fondo, come sotto una
    // finestra ingrandita (vedi controllaFinestre): prima si toglieva anche li' e il bordo non lo faceva piu' risalire
    void schermoIntero(int tipo) {
        notch.modo(tipo == 0 ? notch::NORMALE : notch::NASCOSTO);   // video, presentazioni e giochi: via
        if ((tipo == 1) != appSchermoIntero) {
            appSchermoIntero = tipo == 1;
            if (registro) scrivi(appSchermoIntero ? "app a schermo intero: dock sotto il bordo, risale col mouse" : "app a schermo intero finita");
            controllaFinestre(true);
        }
        const bool si = tipo == 2;
        if (si == nascostoSchermoIntero) return;
        nascostoSchermoIntero = si;
        for (HWND h : {icone, vetro}) {
            if (!si && h == vetro && GetPropW(h, VETRO_FUORI)) continue;   // (fuori dallo schermo: lo rimette aggiornaVetro)
            ShowWindow(h, si ? SW_HIDE : SW_SHOWNOACTIVATE);
        }
        if (si) { nascondiSchede(); nascondiLauncher(); }
        if (!si) dirty = true;
    }

    notch::Notch& ilNotch() { return notch; }
    // fascia del dock sullo schermo m (dal bordo al suo lato interno, lunga quanto il bordo), per i menu: vuota se li'
    // non c'e' o resta giu'. E' quella a riposo (dock su, icone non ingrandite), non quella del momento: un menu aperto
    // mentre il dock risale finiva poi sotto di lui (R20261120-145131-S2)
    RECT fasciaSu(HMONITOR m) const {
        if (!attivo || nascostoSchermoIntero || MonitorFromRect(&schermo, MONITOR_DEFAULTTONEAREST) != m) return RECT{};
        const bool restaGiu = vuoleNascondersi && !rivelato && nessunaScheda() && !dockSu && !lAperto;
        if (restaGiu) return RECT{};
        return aSchermo({0, std::floor(yBaseRiposo() - S - PAD - 2 * dpi), (float)W, (float)H});
    }
    // lato del dock se r (schermo) e' una sua icona (il centro nella sua fascia a riposo), altrimenti 0: la curva del genio
    // e la caduta vanno verso quel lato (verso la barra di Windows, in basso, se il bersaglio e' lei)
    int latoPer(const RECT& r) const {
        if (!attivo || lato == 0) return 0;
        const RECT f = aSchermo({0, yBaseRiposo() - S - PAD - 2 * dpi, (float)W, (float)H});
        const POINT c = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        return PtInRect(&f, c) ? lato : 0;
    }

    // impostazioni: notch acceso o spento, e ora e data in fondo al dock, dopo il centro di controllo
    void impostaNotch(bool si) { notch.abilita(si); dirty = true; }   // (spento: musica e timer nella testa del dock)
    // impostazione "Angoli": raggio del fondo (il vetro lo cambia raggio::riapplica), notch e capsule
    void impostaAngoli() { RAGGIO = raggio::vetro() * dpi; notch.aggiornaAngoli(); dirty = true; }
    // impostazione "Posizione": il dock passa su un altro bordo dello schermo (pannelli, launcher e anteprime aperti si
    // chiudono: sono messi rispetto al lato di prima). Col dock in alto il notch scende dal bordo in basso
    void impostaLato(int l) {
        l = std::clamp(l, 0, 3);
        if (l == lato) return;
        if (!attivo) { lato = l; return; }   // (all'avvio lo legge inizializza)
        chiudiTutte(); if (lAperto) chiudiLauncher(); ant.chiudi();
        // spazio riservato: si toglie dal bordo di prima e si rifa' su quello nuovo
        if (appbarOk) { const bool s = spazioRiservato; spazioRiservato = false; aggiornaAppbar(); spazioRiservato = s; }
        lato = l;
        ridimensiona(lunghezzaBordo(), H);   // (anche con la stessa lunghezza: ai lati la superficie e' girata)
        posizionaFinestra();
        rettVetro = rettVetroStato = rettVetroOra = RECT{};
        regioneDock = regioneIsola = regioneEtichetta = RECT{};
        if (spazioRiservato) aggiornaAppbar();
        notch.inBasso(lato == 1); notch.posto(schermo, dpi);
        controllaFinestre(true);
        if (registro) { char b[64]; snprintf(b, 64, "dock: lato %d", lato); scrivi(b); }
        dirty = true;
    }
    void impostaOraNelDock(bool si) {
        if (si == oraNelDock) return;
        oraNelDock = si;
        if (!si) { isola = {}; for (auto& r : rettStato) r = {}; statoSopra = statoPremuto = -1; }
        dirty = true;
    }
    // impostazioni del dock: a scomparsa (spento: resta su anche sopra le finestre ingrandite, che restano grandi
    // quanto lo schermo; sotto le app a schermo intero e i giochi si toglie comunque) e opacita' del fondo (0.2..1:
    // vetro e velo, le icone restano piene)
    void impostaAutoNascondi(bool si) {
        if (si == autoNascondi) return;
        autoNascondi = si;
        controllaFinestre(true);
    }
    // impostazione: le finestre ingrandite si fermano sopra il dock (spazio riservato, appbar) o vanno fino in fondo
    void impostaSpazio(bool si) {
        if (si == spazioRiservato) return;
        spazioRiservato = si;
        aggiornaAppbar();
        controllaFinestre(true);
    }
    // impostazione: lato delle icone a riposo (px a 100%). Con loro cambiano le ingrandite (stessa proporzione),
    // l'altezza del dock, la finestra delle icone e lo spazio riservato
    void impostaLatoIcone(int px) {
        px = std::clamp(px, 24, 96);
        if (px == latoIcone) return;
        latoIcone = px;
        if (!attivo) return;   // (all'avvio le misure le prende inizializza)
        S = latoIcone * dpi; M = S * INGRANDITE;
        altezzaDock = S + 2 * PAD;
        const int h = (int)std::ceil(altezzaDock + MARGINE + (M - S) + 64 * dpi);
        if (h != H) ridimensiona(W, h);
        posizionaFinestra();
        rettVetro = rettVetroStato = rettVetroOra = RECT{};
        regioneDock = regioneIsola = regioneEtichetta = RECT{};
        if (appbarOk) aggiornaAppbar();
        controllaFinestre(true);
        dirty = true;
    }
    // impostazione: distanza fra le icone (px a 100%). Anche i separatori si allargano con lei; la finestra delle
    // icone e' larga quanto lo schermo, quindi basta ridisporre
    void impostaDistanzaIcone(int px) {
        px = std::clamp(px, 2, 32);
        if (px == distanzaIcone) return;
        distanzaIcone = px;
        GAP = distanzaIcone * dpi;
        dirty = true;
    }
    // impostazione: colore del fondo del dock. 0 = automatico (chiaro o scuro col tema), altrimenti 0x01RRGGBB.
    // Il colore tinge il velo sopra il vetro; puntini, separatori e testi diventano chiari o scuri a seconda di quanto
    // e' chiaro il colore (render: scuro)
    void impostaColore(DWORD c) {
        const bool aut = !(c & 0x01000000);
        const D2D1_COLOR_F col = D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1);
        if (aut == coloreAuto && (aut || (col.r == coloreFondo.r && col.g == coloreFondo.g && col.b == coloreFondo.b))) return;
        coloreAuto = aut; coloreFondo = col;
        fondoScuro = 0.2126f * col.r + 0.7152f * col.g + 0.0722f * col.b < 0.55f;
        // il vetro acrilico sotto: chiaro o scuro come il colore (automatico: come il tema)
        BOOL s = coloreAuto ? temaScuro() : fondoScuro;
        for (HWND v : {vetro, vetroStato, vetroOra}) if (v) DwmSetWindowAttribute(v, DWMWA_USE_IMMERSIVE_DARK_MODE, &s, sizeof(s));
        dirty = true;
    }
    void impostaOpacita(float a) {
        a = std::clamp(a, 0.2f, 1.0f);
        if (std::fabs(a - opacita) < 0.001f) return;
        opacita = a;
        for (HWND v : {vetro, vetroStato, vetroOra}) if (v) SetLayeredWindowAttributes(v, 0, (BYTE)std::lround(a * 255), LWA_ALPHA);
        dirty = true;
    }
    // eseguibile di un'app che il dock conosce (per AppUserModelID o per nome): l'icona vera delle sue notifiche
    std::wstring exeDiApp(const std::wstring& aumid, const std::wstring& nome) const {
        for (const auto& v : voci) {
            if (v.exe.empty() || v.avvio.empty() || v.avvio[0] == L'*') continue;
            if ((!aumid.empty() && !_wcsicmp(v.aumid.c_str(), aumid.c_str())) || (!nome.empty() && !_wcsicmp(v.nome.c_str(), nome.c_str()))) return v.exe;
        }
        for (const auto& a : tutteApp)   // (anche le app del launcher che non sono nel dock)
            if (!nome.empty() && !_wcsicmp(a.nome.c_str(), nome.c_str()) && a.avvio.size() > 4 && !_wcsicmp(a.avvio.c_str() + a.avvio.size() - 4, L".exe")) return a.avvio;
        return L"";
    }
    // scala con cui e' fatto il dock e quella di adesso del suo schermo (cambiata dalle Impostazioni: si riparte)
    // (il dock e' fatto alla scala dello schermo principale, su qualsiasi schermo stia)
    bool scalaCambiata() const { return attivo && std::fabs(::vetro::dpiMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)) - dpi) > 0.01f; }
    // numeri delle notifiche per app (dal banner, ogni secondo e mezzo): si ridisegna solo se cambiano
    void numeriNotificheNuovi(const std::unordered_map<std::wstring, int>& c) { if (c != numeriNotifiche) { numeriNotifiche = c; dirty = true; } }   // volume e notifiche li mostra il notch (fluidwin.cpp)

    std::vector<std::pair<std::wstring, std::wstring>> elencoNascoste() const {
        std::vector<std::pair<std::wstring, std::wstring>> r;
        r.reserve(appNascoste.size());
        for (const auto& x : appNascoste) r.emplace_back(x.chiave, x.nome);
        return r;
    }
    void nascondiApp(const std::wstring& exe, const std::wstring& aumid, const std::wstring& nome) {
        const std::wstring c = chiaveNascosta(exe, aumid);
        if (c == L"exe:") return;
        for (const auto& x : appNascoste) if (x.chiave == c) return;
        appNascoste.push_back({c, nome.empty() ? c : nome});
        salvaNascoste();
        appDaAggiornare = true; dirty = true;
        aggiornaApp();
    }
    // (chiave per valore: dal sottomenu arriva appNascoste[k].chiave, e remove_if, spostando gli elementi, cambiava il
    // termine del confronto a meta' strada, R20261120-022133-S3)
    void mostraAppNascosta(const std::wstring chiave) {
        if (chiave.empty()) appNascoste.clear();
        else appNascoste.erase(std::remove_if(appNascoste.begin(), appNascoste.end(),
                                              [&](const AppNascosta& x) { return x.chiave == chiave; }),
                               appNascoste.end());
        salvaNascoste();
        appDaAggiornare = true; dirty = true;
        aggiornaApp();
    }

private:
    static Motore*& gIstanza() { static Motore* m = nullptr; return m; }

    // app lanciate dal dock e non ancora comparse (per il genio d'apertura)
    struct Lancio { std::wstring chiave, exe, aumid; double t; std::wstring sqPrima, sqDopo; };
    std::vector<Lancio> lanci;
    void segnaLancio(const Voce& v) { lanci.push_back({chiave(v), v.exe, v.aumid, adesso(), v.sqPrima, v.sqDopo}); }

    // icona i del dock a riposo (senza ingrandimento), in coordinate schermo: cosi' il bersaglio del genio non si
    // muove durante l'animazione. Dock nascosto: risale per qualche istante, il tempo che la finestra entri o esca
    bool rettIcona(size_t i, RECT& out) {
        if (vuoleNascondersi) { rivelaFino = adesso() + 1.4; dirty = true; posizionaVetri(); }
        const auto pos = posizioniRiposo();
        if (i >= pos.size()) return false;
        out = aSchermo({pos[i], yBaseRiposo() - S, pos[i] + S, yBaseRiposo()});   // (sul lato del dock)
        return true;
    }

    // ---- misure (pixel) ----
    float dpi = 1, S = 48, M = 74, PAD = 8, GAP = 6, MARGINE = 8, RAGGIO = 8, altezzaDock = 64;
    RECT schermo{};
    int W = 0, H = 0;
    UINT statoBarraPrima = 0;
public:
    bool barraSalvata = true;   // StatoBarra scritto nel registro (fluidwin.cpp): senza, la barra non va a scomparsa
private:

    // ---- stato ----
    std::vector<Voce> voci;
    size_t primaNonFissata = 0;      // indice della prima app aperta ma non aggiunta alla barra (separatore)
    size_t primaFine = 0;            // indice della prima voce di fondo (pila, cestino, centro di controllo): altro separatore
    std::vector<std::wstring> ordineAperte;   // le app aperte restano nel posto in cui sono comparse la prima volta
    struct AppNascosta { std::wstring chiave, nome; };
    std::vector<HWND> finestreNascoste;   // finestre delle app nascoste dal dock (ultimo aggiornaApp): per "Mostra il desktop"
    std::vector<AppNascosta> appNascoste;

    static std::wstring chiaveNascosta(const std::wstring& exe, const std::wstring& aumid) {
        if (!aumid.empty()) return L"aumid:" + minuscolo(aumid);
        return L"exe:" + minuscolo(exe);
    }
    bool eNascosta(const std::wstring& exe, const std::wstring& aumid) const {
        const std::wstring c = chiaveNascosta(exe, aumid);
        const std::wstring ce = !exe.empty() ? L"exe:" + minuscolo(exe) : std::wstring();
        for (const auto& x : appNascoste)
            if (x.chiave == c || (!ce.empty() && x.chiave == ce)) return true;
        return false;
    }
    void caricaNascoste() {
        appNascoste.clear();
        DWORD n = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"AppNascoste", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &n) != ERROR_SUCCESS || n < sizeof(wchar_t)) return;
        std::vector<wchar_t> b(n / sizeof(wchar_t) + 2, 0);
        DWORD sz = n;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"AppNascoste", RRF_RT_REG_MULTI_SZ, nullptr, b.data(), &sz) != ERROR_SUCCESS) return;
        for (const wchar_t* p = b.data(); *p; p += wcslen(p) + 1) {
            const std::wstring s(p);
            const size_t t = s.find(L'\t');
            if (t == std::wstring::npos || t == 0) continue;
            const std::wstring k = s.substr(0, t), nm = s.substr(t + 1);
            if (k.empty() || nm.empty()) continue;
            if (std::any_of(appNascoste.begin(), appNascoste.end(), [&](const AppNascosta& x) { return x.chiave == k; })) continue;   // (doppioni)
            appNascoste.push_back({k, nm});
        }
    }
    void salvaNascoste() const {
        if (appNascoste.empty()) {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"AppNascoste");
            return;
        }
        std::vector<wchar_t> b;
        for (const auto& x : appNascoste) {
            const std::wstring s = x.chiave + L"\t" + x.nome;
            b.insert(b.end(), s.begin(), s.end());
            b.push_back(L'\0');
        }
        b.push_back(L'\0');
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"AppNascoste", REG_MULTI_SZ,
                        b.data(), (DWORD)(b.size() * sizeof(wchar_t)));
    }
    bool dirty = true, inMovimento = false, appDaAggiornare = true, nascostoSchermoIntero = false, vetroDaRiposizionare = false;
    bool appSchermoIntero = false;   // app (non gioco) a schermo intero: dock giu', risale dal bordo (schermoIntero)
    double ultimoAggiornamento = -10, ultimoT = 0, dtFotogramma = 1.0 / 180;
    bool mouseDentro = false, mouseSulDock = false; float mouseX = 0, mouseY = 0;
    double lente = 0, lenteVel = 0;  // 0 = icone normali, 1 = ingrandimento pieno (molla)
    // a scomparsa: con una finestra a schermo intero il dock scende sotto il bordo e riesce col mouse in fondo
    HMONITOR monitor = nullptr;
    HMONITOR spostamento = nullptr;   // schermo verso cui il dock sta andando (prima scende, poi si sposta e risale)
    HMONITOR bordoAltro = nullptr; double bordoAltroDa = -1;   // mouse sul bordo in basso di un altro schermo, da quando
    bool vuoleNascondersiI = false;   // l'isola sta sempre nascosta, il dock solo sotto le finestre
    // impostazione: ora e data in fondo al dock, dopo il centro di controllo (larghezzaCoda). Per il mouse fa da
    // "isola": isola e rettStato[ORA] sono il suo posto, cosi' etichetta (data lunga) e clic (notifiche) sono quelli di prima
    bool oraNelDock = false;
    // Testa del dock (1.3.0, notch a scomparsa): all'inizio del dock, speculare a ora e data in fondo, una capsula con la
    // musica (copertina e barrette), il timer in corso (anello e tempo che resta) e i segni di "Tieni sveglio" e "Non
    // disturbare". Col mouse sopra musica e timer mostrano i loro tasti (vedi statoTesta). Con il notch fisso non c'e'
    double testaK = 0;                // 0..1: comparsa (la larghezza entra nella disposizione, il dock si allarga)
    float largTesta = 0;              // larghezza dei contenuti (l'ultima, anche mentre sparisce)
    bool testaMusica = false, testaTimer = false, testaSveglio = false, testaSilenzio = false;   // cosa mostra (deciso in render)
    double tSveglio = -10, tSilenzio = -10;   // quando si sono accesi (rimbalzo e onda del segno)
    double kMus = 0, kTim = 0, vkMus = 0, vkTim = 0;   // 0..1: musica e timer allargati coi tasti (mouse sopra), con le velocita' delle molle
    double vTestaK = 0, pSv = 0, vSv = 0, pSi = 0, vSi = 0, pMu = 0, vMu = 0, pTi = 0, vTi = 0;   // presenze delle parti (molle)
    float scTesta = 1;                // scala con la lente del dock (come le icone vicine)
    D2D1_RECT_F rettTesta{}, rettMus{}, rettTim{};   // posti per il mouse: tutta la testa, musica, timer (vuoti se non ci sono)
    D2D1_RECT_F tastoTesta[5]{};      // indietro, play/pausa, avanti, +1 min, stop
    int tastoPremuto = -1;            // tasto premuto (-2: la testa fuori dai tasti, -1: niente)
    double evTesta = 0;
    ID2D1Bitmap* bTesta = nullptr; UINT versioneTesta = 0xFFFFFFFF;   // copertina del brano
    double ultimaMusica = -100;       // quando suonava l'ultima volta (in pausa resta qualche secondo)
    int periodoTesta = 0;             // TIMER_TESTA (ms, 0 spento): 60 con le barrette che ballano, 250 per i secondi del timer
    bool autoNascondi = true;         // impostazione: il dock scende sotto le finestre ingrandite o che lo coprono
    float opacita = 1;                // impostazione: opacita' del fondo del dock (vetro e velo), 0.2..1
    int latoIcone = 42;               // impostazione: lato delle icone a riposo, px a 100%
    int distanzaIcone = 6;            // impostazione: distanza fra le icone, px a 100% (GAP)
    static constexpr float INGRANDITE = 66.0f / 42.0f;   // icone ingrandite dalla lente: proporzione fissa con quelle a riposo
    bool coloreAuto = true, fondoScuro = false;           // impostazione: colore del fondo (vedi impostaColore)
    D2D1_COLOR_F coloreFondo{};
    bool spazioRiservato = false;     // impostazione: spazio riservato in basso (appbar)...
    bool appbarOk = false;            // ... e registrato davvero presso Windows

    // registra (o toglie) il dock come appbar sul bordo in basso del suo schermo: altezza del dock con un margine sopra
    // uguale a quello sotto. Si richiama quando cambia lo schermo e quando Windows sposta le appbar (ABN_POSCHANGED)
    // Explorer riavviato: la sua registrazione dell'appbar non c'e' piu'. Si rifa' da capo (prima appbarOk restava vero
    // e le finestre ingrandite tornavano sopra il dock, R20261121-001416-S8)
public:
    void explorerRiavviato() {
        if (!appbarOk) return;
        // TaskbarCreated arriva anche senza un vero riavvio di Explorer, con la registrazione ancora valida: ABM_NEW
        // fallirebbe e appbarOk resterebbe falso con lo spazio riservato. Si toglie prima (innocuo se non c'e' piu')
        // e si rifa' da capo (R20261121-115814-S4)
        APPBARDATA ab{}; ab.cbSize = sizeof(ab); ab.hWnd = icone;
        SHAppBarMessage(ABM_REMOVE, &ab);
        appbarOk = false;
        aggiornaAppbar();
    }
private:
    void aggiornaAppbar() {
        APPBARDATA ab{}; ab.cbSize = sizeof(ab); ab.hWnd = icone;
        if (!spazioRiservato || !attivo || !icone) {
            if (appbarOk) {
                SHAppBarMessage(ABM_REMOVE, &ab); appbarOk = false;
                RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"Appbar");
                scrivi("spazio del dock tolto: le finestre ingrandite arrivano in fondo");
            }
            return;
        }
        if (!appbarOk) {
            ab.uCallbackMessage = WM_APPBAR;
            if (!SHAppBarMessage(ABM_NEW, &ab)) { scrivi("spazio del dock: registrazione non riuscita"); return; }
            appbarOk = true;
            const DWORD v = (DWORD)(ULONG_PTR)icone;
            RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"Appbar", REG_DWORD, &v, sizeof(v));
        }
        const int alto = (int)std::ceil(S + 2 * PAD + 2 * MARGINE);
        // (sul lato del dock: in alto e ai lati lo spazio si riserva su quel bordo)
        static const UINT BORDI[4] = {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT};
        ab.uEdge = BORDI[std::clamp(lato, 0, 3)];
        switch (lato) {
            case 1: ab.rc = {schermo.left, schermo.top, schermo.right, schermo.top + alto}; break;
            case 2: ab.rc = {schermo.left, schermo.top, schermo.left + alto, schermo.bottom}; break;
            case 3: ab.rc = {schermo.right - alto, schermo.top, schermo.right, schermo.bottom}; break;
            default: ab.rc = {schermo.left, schermo.bottom - alto, schermo.right, schermo.bottom};
        }
        SHAppBarMessage(ABM_QUERYPOS, &ab);   // (Windows puo' spostarla accanto ad altre appbar sullo stesso bordo)
        switch (lato) {
            case 1: ab.rc.bottom = ab.rc.top + alto; break;
            case 2: ab.rc.right = ab.rc.left + alto; break;
            case 3: ab.rc.left = ab.rc.right - alto; break;
            default: ab.rc.top = ab.rc.bottom - alto;
        }
        SHAppBarMessage(ABM_SETPOS, &ab);
        if (registro) { char b[120]; snprintf(b, 120, "spazio del dock: %ld px sul lato %d (%ld,%ld-%ld,%ld)", (long)alto, lato, ab.rc.left, ab.rc.top, ab.rc.right, ab.rc.bottom); scrivi(b); }
    }
    bool vuoleNascondersi = false, rivelato = false, rivelatoI = false;   // dock e isola di stato risalgono separati
    double yNascosto = 0, velNascosto = 0;   // px sotto la posizione normale (dock)
    double yNascostoI = 0, velNascostoI = 0; // ... e isola di stato
    double rivelaFino = 0, fuoriDal = -1, fuoriDalI = -1;   // comparsa temporanea (per il genio) e da quando il mouse e' lontano
    float zonaDockL = 0, zonaDockR = 0;      // dove sta il dock (coordinate finestra), per capire a chi e' vicino il mouse
    // tirato dal mouse: il fondo si allunga un poco verso il puntatore
    double tiroX = 0, tiroY = 0, velTiroX = 0, velTiroY = 0;
    int sopra = -1;                  // icona sotto il mouse
    int premuta = -1;
    // Riordino trascinando (issue #23): premuta un'icona e spostato il mouse di qualche pixel, l'icona segue il mouse e
    // le vicine scivolano a farle posto. Le app aggiunte alla barra fra loro, le altre aperte fra loro; App e le icone
    // di fondo restano ferme. L'ordine delle aggiunte si salva in HKCU\Software\FluidWin (OrdineFissate, i loro
    // collegamenti; i file di Windows non si toccano), quello delle altre resta per la sessione (ordineAperte)
    float premutaX = 0;              // x della pressione (coordinate della finestra delle icone)
    bool trascinando = false;
    int trascinata = -1;             // indice della voce trascinata
    float presa = 0.5f;              // dove la si e' presa, 0..1 della larghezza dell'icona
    std::vector<std::wstring> ordineFissate;   // collegamenti delle app aggiunte, nell'ordine scelto (vuoto: quello di Windows)
    RECT rettVetro{}, rettVetroStato{}, rettVetroOra{}, regioneDock{}, regioneIsola{}, regioneEtichetta{};
    D2D1_RECT_F rettEtichetta{};
    // etichetta (una sola, per il dock o per l'isola di stato)
    double etichettaAlfa = 0; std::wstring etichettaTesto; float etichettaCx = 0, etichettaTop = 0;

    // ---- isola di stato ----
    // isola: il centro di controllo (un solo pulsante, con rete e volume a colpo d'occhio) e, a parte, data e ora
    enum { CONTROLLO, ORA, N_STATO };
    D2D1_RECT_F pillolaControllo{}, pillolaOra{};
    D2D1_RECT_F rettStato[N_STATO]{}, isola{};
    double evidenza[N_STATO]{};      // 0..1, sfondo dell'elemento sotto il mouse
    int statoSopra = -1, statoPremuto = -1;
    std::wstring testoOra, testoData, testoDataLunga, testoNotch;
    const wchar_t *fontTesto = L"Segoe UI Variable Text", *fontTitoli = L"Segoe UI Variable Display";   // scelti in grafica()
    notch::Notch notch;                  // ora e data in alto al centro, come il notch del Mac e dell'iPhone (notch.h)
    std::wstring misuratoOra, misuratoData; float larghezzaOra = 0;   // ultima misura dei due testi
    float volume = 0; bool muto = false, audioOk = false;
    int rete = 0; std::wstring nomeRete;   // 0 = nessuna, 1 = cavo, 2 = Wi-Fi
    int conteggioTimer = 0;
    IMMDeviceEnumerator* audio = nullptr;

    // ---- grafica ----
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* sc = nullptr; IDCompositionDevice* dcomp = nullptr; IDCompositionTarget* target = nullptr; IDCompositionVisual* visual = nullptr;
    ID2D1Factory1* f2d = nullptr; ID2D1Device* d2dev = nullptr; ID2D1DeviceContext* dc = nullptr; ID2D1Bitmap1* bersaglio = nullptr;
    IDWriteFactory* fdw = nullptr; IDWriteTextFormat *testo = nullptr, *fmtOra = nullptr, *fmtData = nullptr;
    ID2D1SolidColorBrush* pennello = nullptr;
    ID2D1StrokeStyle* tratto = nullptr;   // estremi arrotondati, per i simboli

    // tema chiaro/scuro: dal registro al massimo 4 volte al secondo (prima lo si leggeva a ogni fotogramma)
    static bool temaScuro(bool rileggi = false) {
        static bool scuro = false; static ULONGLONG letto = 0;
        const ULONGLONG ora = GetTickCount64();
        if (!rileggi && letto && ora - letto < 250) return scuro;
        DWORD v = 1, n = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
        scuro = v == 0; letto = ora;
        return scuro;
    }

    HWND creaVetro(HINSTANCE inst) {
        HWND v = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                 L"FluidWin.DockVetro", L"", WS_POPUP | WS_THICKFRAME, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
        SetLayeredWindowAttributes(v, 0, 255, LWA_ALPHA);
        MARGINS mg{-1, -1, -1, -1}; DwmExtendFrameIntoClientArea(v, &mg);
        BOOL scuro = temaScuro(); DwmSetWindowAttribute(v, DWMWA_USE_IMMERSIVE_DARK_MODE, &scuro, sizeof(scuro));
        int tipo = 3; DwmSetWindowAttribute(v, DWMWA_SYSTEMBACKDROP_TYPE, &tipo, sizeof(tipo));
        raggio::registra(v);   // angoli: quelli dell'impostazione "Angoli" (anche cambiandola dopo)
        BOOL niente = TRUE; DwmSetWindowAttribute(v, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        return v;
    }

    bool grafica() {
        // la device di Direct3D condivisa con genio e gelatina (gpu.h): una sola per tutto FluidWin
        dev = gpu::condivisa().dev; ctx = gpu::condivisa().ctx;
        if (!dev) return false;
        dev->AddRef(); ctx->AddRef();
        // Ogni passo puo' fallire: un errore da' false (FluidWin va avanti senza dock), mai un puntatore nullo usato
        // alla riga dopo (audit FW-AUDIT-004). Gli oggetti locali si rilasciano qui; quelli nei membri restano fino
        // all'uscita (succede una volta, all'avvio)
        IDXGIDevice1* dd = nullptr;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dd)) || !dd) return false;
        dd->SetMaximumFrameLatency(1);
        IDXGIFactory2* fab = nullptr;
        if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&fab))) { dd->Release(); return false; }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = finW(); sd.Height = finH(); sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        const HRESULT hsc = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &sc);
        fab->Release();
        if (FAILED(hsc) || FAILED(DCompositionCreateDevice(dd, __uuidof(IDCompositionDevice), (void**)&dcomp))) { dd->Release(); return false; }
        if (FAILED(dcomp->CreateTargetForHwnd(icone, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual))) { dd->Release(); return false; }
        visual->SetContent(sc); target->SetRoot(visual); dcomp->Commit();

        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)&f2d))) { dd->Release(); return false; }
        const HRESULT hd2 = f2d->CreateDevice(dd, &d2dev);
        dd->Release();
        if (FAILED(hd2) || FAILED(d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
        IDXGISurface* sup = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup)) || !sup) return false;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        const HRESULT hbm = dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglio); sup->Release();
        if (FAILED(hbm)) return false;
        dc->SetTarget(bersaglio);
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &pennello))) return false;
        if (FAILED(f2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                      D2D1_LINE_JOIN_ROUND), nullptr, 0, &tratto))) return false;

        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&fdw)) || !fdw) return false;
        bool fontOk = true;   // un formato non creato: niente dock (lo usano tutti i disegni senza controlli)
        auto formato = [&](const wchar_t* font, DWRITE_FONT_WEIGHT peso, float px, IDWriteTextFormat** out) {
            if (FAILED(fdw->CreateTextFormat(font, nullptr, peso, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px * dpi, L"it-it", out)) || !*out) {
                // ripiego su Segoe UI (c'e' sempre): solo se manca anche quello il dock non parte
                if (FAILED(fdw->CreateTextFormat(L"Segoe UI", nullptr, peso, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px * dpi, L"it-it", out)) || !*out) { fontOk = false; return; }
            }
            testi::allinea(*out, DWRITE_TEXT_ALIGNMENT_CENTER);   // (in arabo da destra a sinistra, vedi testi.h)
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        // font dell'interfaccia: Inter se e' installato (il piu' vicino a quello di iOS), altrimenti Segoe UI Variable
        const wchar_t *fTesto = L"Segoe UI Variable Text", *fTitoli = L"Segoe UI Variable Display";
        {
            IDWriteFontCollection* sys = nullptr; fdw->GetSystemFontCollection(&sys, TRUE);
            auto esiste = [&](const wchar_t* n) { UINT32 i = 0; BOOL e = FALSE; if (sys) sys->FindFamilyName(n, &i, &e); return e == TRUE; };
            for (const wchar_t* n : {L"Inter Variable", L"Inter Variable Text", L"Inter"}) if (esiste(n)) { fTesto = fTitoli = n; break; }
            for (const wchar_t* n : {L"Inter Variable Display", L"Inter Display"}) if (esiste(n)) { fTitoli = n; break; }
            if (sys) sys->Release();
            if (registro) { char b[160]; snprintf(b, 160, "font: testo %ls, titoli %ls", fTesto, fTitoli); scrivi(b); }
        fontTesto = fTesto; fontTitoli = fTitoli;
        }
        formato(fTesto, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, &testo);
        formato(fTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0f, &fmtOra);
        formato(fTesto, DWRITE_FONT_WEIGHT_NORMAL, 11.5f, &fmtData);
        // testi dei pannelli: allineati a sinistra, con "..." se non entrano
        auto sinistra = [&](const wchar_t* font, DWRITE_FONT_WEIGHT peso, float px, IDWriteTextFormat** out) {
            formato(font, peso, px, out); testi::allinea(*out, DWRITE_TEXT_ALIGNMENT_LEADING);
        };
        sinistra(fTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0f, &fmtTitolo);
        sinistra(fTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 34.0f, &fmtGrande);
        sinistra(fTesto, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, &fmtVoce);
        sinistra(fTesto, DWRITE_FONT_WEIGHT_NORMAL, 11.5f, &fmtSotto);
        sinistra(fTesto, DWRITE_FONT_WEIGHT_MEDIUM, 13.0f, &fmtNotizia);   // titoli delle notizie: a capo, al massimo due righe
        formato(fTesto, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, &fmtNome);
        if (!fontOk) return false;
        fmtNotizia->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); fmtNotizia->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmtNotizia->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f * dpi, 13.5f * dpi);   // righe fisse: ne entrano esattamente due
        const DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        for (IDWriteTextFormat* f : {fmtNome, fmtSotto, fmtVoce, fmtNotizia}) {
            IDWriteInlineObject* punti = nullptr;
            if (SUCCEEDED(fdw->CreateEllipsisTrimmingSign(f, &punti))) { f->SetTrimming(&tr, punti); punti->Release(); }
        }
        return true;
    }

    // ---- icone ----
    // pixel dell'icona (premoltiplicati), senza Direct2D: si puo' chiamare anche da un altro thread
    static bool pixelIcona(const std::wstring& percorso, int lato, int& w, int& h, std::vector<UINT>& px) {
        IShellItemImageFactory* fab = nullptr;
        if (FAILED(SHCreateItemFromParsingName(percorso.c_str(), nullptr, IID_PPV_ARGS(&fab)))) return false;
        HBITMAP hb = nullptr;
        HRESULT hr = fab->GetImage({lato, lato}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb);
        fab->Release();
        if (FAILED(hr) || !hb) return false;
        BITMAP bm{}; GetObject(hb, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight;
        px.assign((size_t)w * h, 0);
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        HDC scr = GetDC(nullptr); GetDIBits(scr, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, scr);
        DeleteObject(hb);
        bool alfa = false;
        for (UINT p : px) if (p >> 24) { alfa = true; break; }
        // GetImage restituisce colori non premoltiplicati: Direct2D li vuole premoltiplicati
        for (UINT& p : px) {
            UINT a = alfa ? (p >> 24) : 255;
            UINT r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
            p = (a << 24) | (r << 16) | (g << 8) | b;
        }
        return true;
    }

    ID2D1Bitmap1* bitmapDaPixel(int w, int h, const std::vector<UINT>& px) {
        ID2D1Bitmap1* out = nullptr;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, &out);
        return out;
    }

    ID2D1Bitmap1* caricaIcona(const std::wstring& percorso, int lato = 256) {
        Cronometro cr(this, "caricaIcona");
        int w = 0, h = 0; std::vector<UINT> px;
        if (!pixelIcona(percorso, lato, w, h, px)) return nullptr;
        ID2D1Bitmap1* out = nullptr;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, &out);
        return out;
    }

    Voce* voceDi(HWND h) {
        for (auto& v : voci) if (!v.uscente) for (HWND f : v.finestre) if (f == h) return &v;
        return nullptr;
    }

    static bool finestraDaDock(HWND h) {
        if (!IsWindowVisible(h) || GetAncestor(h, GA_ROOT) != h) return false;
        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) return false;
        if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
        DWORD cl = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof(cl));
        if (cl) return false;
        wchar_t c[64]; GetClassNameW(h, c, 64);
        if (!wcscmp(c, L"Shell_TrayWnd") || !wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW") ||
            !wcscmp(c, L"FluidWin.DockIcone") || !wcscmp(c, L"FluidWin.DockVetro") || !wcscmp(c, L"Windows.UI.Core.CoreWindow")) return false;
        // senza titolo di solito non e' una finestra d'app, ma con WS_EX_APPWINDOW la barra di Windows la mostra lo
        // stesso: alcune app (Mailbird, Avalonia) non danno un titolo alla finestra principale, e senza questa il dock
        // non la vedeva e a ogni clic rilanciava l'app
        if (GetWindowTextLengthW(h) == 0 && !(ex & WS_EX_APPWINDOW)) return false;
        return true;
    }

    static std::wstring chiave(const Voce& v) { return v.exe.empty() ? v.avvio : v.exe; }
    // cosa apre il clic: il pacchetto (collegamento dello Store vecchio), l'exe di adesso (Squirrel, versione
    // cancellata) o il collegamento
    static const std::wstring& daAprire(const Voce& v) { return !v.pacchetto.empty() ? v.pacchetto : !v.ripiego.empty() ? v.ripiego : v.avvio; }
    // ... e con quali argomenti: solo per il ripiego, che sostituisce il collegamento e ne perderebbe gli argomenti
    // (il collegamento e il pacchetto li hanno gia', R20261121-230321-S3)
    static const wchar_t* argomentiDaAprire(const Voce& v) {
        return v.pacchetto.empty() && !v.ripiego.empty() && !v.argomenti.empty() ? v.argomenti.c_str() : nullptr;
    }
    // la voce trascinata, senza ambiguita': le aggiunte per collegamento (due collegamenti allo stesso exe, per esempio
    // due profili di un browser, sono due icone: con chiave() si ritrovava l'ultima, R20261120-013029-S1); le altre per
    // chiave, come le raggruppa aggiornaApp
    static std::wstring identita(const Voce& v) { return v.fissata ? L"F:" + v.avvio : L"A:" + chiave(v); }

    // cache di aggiornaApp (gira a ogni cambio di primo piano)
    std::vector<Voce> fissateCache;                 // le app fissate (senza icone ne' finestre)
    ULONGLONG fissateT = 0, fissateTBand = 0; double fissateLette = -1e9; bool fissateOk = false;
public:
    // diagnostica delle app fissate (ultima lettura): lista di Windows letta, voci per tipo, scartate
    struct DiagFissate { bool lista = false; int collegamenti = 0, pacchetto = 0, squirrel = 0, morte = 0, doppie = 0, scartate = 0, iconeRotte = 0; } diagFissate;
private:
    std::unordered_map<HWND, std::pair<DWORD, std::wstring>> exeFinestre;   // finestra -> (processo, eseguibile)
    std::unordered_map<std::wstring, std::wstring> nomiExe;                 // eseguibile -> nome leggibile

    // App aggiunte alla barra (i loro collegamenti) + app con finestre aperte
    void aggiornaApp() {
        Cronometro cr(this, "aggiornaApp");
        appDaAggiornare = false; ultimoAggiornamento = ultimoT;
        // icona premuta (e trascinata): la si ritrova per nome, gli indici possono cambiare
        const std::wstring chiavePremuta = (premuta >= 0 && premuta < (int)voci.size()) ? chiave(voci[premuta]) : L"";
        const std::wstring chiaveTrascinata = (trascinando && trascinata >= 0 && trascinata < (int)voci.size()) ? identita(voci[trascinata]) : L"";
        std::vector<Voce> nuove;
        // 0) prima icona: il launcher di tutte le app (disegnata dal dock, senza finestre)
        { Voce l; l.fissata = true; l.avvio = AVVIO_LAUNCHER; l.nome = L"App"; nuove.push_back(std::move(l)); }
        // 1) aggiunte alla barra, nell'ordine dei collegamenti. Leggerle (ogni .lnk aperto dalla shell) costa: si
        // rileggono solo se la cartella e' cambiata (collegamento aggiunto, tolto o rinominato) o dopo un minuto
        wchar_t cart[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, cart))) {
            std::wstring dir = std::wstring(cart) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\";
            WIN32_FILE_ATTRIBUTE_DATA ad{};
            const bool letta = GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &ad);
            const ULONGLONG tCart = letta ? ((ULONGLONG)ad.ftLastWriteTime.dwHighDateTime << 32) | ad.ftLastWriteTime.dwLowDateTime : 0;
            // (anche la lista di Windows: fissare un'app di pacchetto non tocca la cartella, solo la chiave Taskband)
            ULONGLONG tBand = 0;
            { HKEY k = nullptr; FILETIME ft{};
              if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband", 0, KEY_READ, &k) == ERROR_SUCCESS) {
                  if (RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &ft) == ERROR_SUCCESS)
                      tBand = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
                  RegCloseKey(k);
              } }
            const bool valida = fissateOk && letta && tCart == fissateT && tBand == fissateTBand && adesso() - fissateLette < 60;
            if (valida) for (const Voce& f : fissateCache) nuove.push_back(f);
            // (l'ora dell'ultima lettura cambia solo quando si rileggono davvero i collegamenti: prima ogni consultazione
            // la rinnovava, e un .lnk cambiato senza toccare la cartella restava vecchio per sempre, R20261121-001416-S14)
            fissateT = tCart; fissateTBand = tBand; if (!valida) fissateLette = adesso();
            WIN32_FIND_DATAW fd; HANDLE fh = valida ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
            std::vector<std::pair<ULONGLONG, std::wstring>> lnk;
            if (fh != INVALID_HANDLE_VALUE) {
                do lnk.push_back({((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime, dir + fd.cFileName});
                while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
            std::sort(lnk.begin(), lnk.end());
            // Ordine e voci come la barra di Windows (fissateWindows): prima la sua lista (collegamenti che ci sono
            // ancora e app di pacchetto), poi i collegamenti che stanno solo nella cartella (quelli di "Aggiungi al
            // dock", e i resti lasciati da installazioni e aggiornamenti: questi si scartano sotto se non aprono nulla
            // o sono doppi). Senza la lista (valore mancante o di forma diversa) solo la cartella, come prima
            struct Candidata { std::wstring lnk, aumid, nome; bool daWindows; };
            std::vector<Candidata> cand;
            std::vector<FissataWindows> win;
            const bool winOk = !valida && fissateWindows(win);
            if (winOk)
                for (auto& f : win) {
                    if (!f.lnk.empty()) {
                        bool c = false;
                        for (auto& l : lnk) if (!_wcsicmp(l.second.c_str(), f.lnk.c_str())) { c = true; break; }
                        if (!c) continue;   // (tolto dalla cartella, per esempio con "Rimuovi dal dock")
                    }
                    cand.push_back({f.lnk, f.aumid, f.nome, true});
                }
            if (!valida)
                for (auto& l : lnk) {
                    bool gia = false;
                    for (auto& c : cand) if (!_wcsicmp(c.lnk.c_str(), l.second.c_str())) { gia = true; break; }
                    if (!gia) cand.push_back({l.second, L"", L"", false});
                }
            if (!valida) { diagFissate = {}; diagFissate.lista = winOk; }
            auto scrivi1 = [&](const char* cosa, const std::wstring& n) { if (registro) { char b[300]; snprintf(b, 300, "fissata %s: %ls", cosa, n.c_str()); scrivi(b); } };
            // destinazione che non esiste (e niente con cui sostituirla): il clic non aprirebbe nulla
            auto morta = [](const Voce& x) {
                return x.avvio.size() > 4 && !x.exe.empty() && x.pacchetto.empty() && x.ripiego.empty() &&
                       GetFileAttributesW(x.exe.c_str()) == INVALID_FILE_ATTRIBUTES;
            };
            for (auto& c : cand) {
                Voce v; v.fissata = true;
                if (c.lnk.empty()) {   // app di pacchetto fissata in Windows: si apre e prende l'icona dalla cartella delle app
                    v.avvio = L"shell:AppsFolder\\" + c.aumid; v.aumid = c.aumid; v.nome = c.nome.empty() ? c.aumid : c.nome;
                    diagFissate.pacchetto++; scrivi1("app di pacchetto", v.avvio);
                    bool doppia = false;
                    for (size_t k = 1; k < nuove.size(); k++) if (nuove[k].aumid == v.aumid) { doppia = true; break; }
                    if (doppia) { diagFissate.doppie++; scrivi1("doppia, scartata", v.avvio); continue; }
                    nuove.push_back(std::move(v));
                    continue;
                }
                const std::wstring& p = c.lnk;
                v.avvio = p;
                v.nome = p.substr(p.find_last_of(L'\\') + 1); v.nome = v.nome.substr(0, v.nome.size() - 4);
                IShellLinkW* sl = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
                    IPersistFile* pf = nullptr;
                    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Load(p.c_str(), STGM_READ))) {
                        wchar_t dest[MAX_PATH] = L"";
                        if (SUCCEEDED(sl->GetPath(dest, MAX_PATH, nullptr, SLGP_RAWPATH))) {
                            // (espanso per intero: con un risultato piu' lungo del buffer, o un errore, il buffer non
                            // e' una stringa valida, R20261121-001416-S4)
                            const DWORD serve = ExpandEnvironmentStringsW(dest, nullptr, 0);
                            std::wstring esp(serve ? serve : 1, L'\0');
                            const DWORD fatti = serve ? ExpandEnvironmentStringsW(dest, esp.data(), serve) : 0;
                            esp.resize(fatti && fatti <= serve ? fatti - 1 : 0);
                            v.exe = minuscolo(esp.empty() ? std::wstring(dest) : esp);
                            wchar_t arg[1024] = L"";
                            if (SUCCEEDED(sl->GetArguments(arg, 1024))) { v.argomenti = arg; exeSquirrel(v.exe, arg, v.sqPrima, v.sqDopo); }
                        }
                        wchar_t ic[MAX_PATH] = L""; int ii = 0;
                        if (SUCCEEDED(sl->GetIconLocation(ic, MAX_PATH, &ii)) && ic[0]) {
                            wchar_t icEsp[MAX_PATH] = L"";
                            const DWORD k = ExpandEnvironmentStringsW(ic, icEsp, MAX_PATH);
                            v.iconaRotta = k && k <= MAX_PATH && GetFileAttributesW(icEsp) == INVALID_FILE_ATTRIBUTES;
                        }
                        IPropertyStore* ps = nullptr;
                        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) {
                            PROPVARIANT pv; PropVariantInit(&pv);
                            if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) v.aumid = pv.pwszVal;
                            PropVariantClear(&pv); ps->Release();
                        }
                    }
                    if (pf) pf->Release();
                    sl->Release();
                }
                // Esplora file: il collegamento non ha un exe (punta a una cartella della shell), solo l'AUMID; le sue
                // finestre invece non hanno AUMID. Senza questo comparivano due icone: "File Explorer" e "Windows Explorer"
                if (v.exe.empty() && v.aumid == L"Microsoft.Windows.Explorer") {
                    wchar_t w[MAX_PATH]; if (GetWindowsDirectoryW(w, MAX_PATH)) v.exe = minuscolo(std::wstring(w) + L"\\explorer.exe");
                }
                // app di pacchetto aggiornata dopo che la si e' fissata: l'exe del collegamento non c'e' piu' (niente
                // icona, il clic non la apre). Si apre (e prende l'icona) dalla cartella delle app, che vale per ogni
                // versione. Solo in questo caso: la prima volta avvioPacchetto elenca tutte le app (~1 s)
                if (v.exe.find(L"\\windowsapps\\") != std::wstring::npos && GetFileAttributesW(v.exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    v.pacchetto = avvioPacchetto(v.exe);
                    if (v.aumid.empty() && v.pacchetto.size() > 17) v.aumid = v.pacchetto.substr(17);   // dopo "shell:AppsFolder\"
                }
                // Squirrel fissato mentre era aperto: il collegamento punta a "app-<versione>\x.exe", cancellata dal
                // primo aggiornamento. Si riconoscono le finestre di ogni versione e si apre quella di adesso
                if (!v.exe.empty() && v.sqPrima.empty() && v.pacchetto.empty() && GetFileAttributesW(v.exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    std::wstring att;
                    if (squirrelVersione(v.exe, v.sqPrima, v.sqDopo, att)) { v.ripiego = att; diagFissate.squirrel++; scrivi1("Squirrel di una versione cancellata", v.nome + L" -> " + att); }
                }
                const bool vuota = morta(v);
                if (vuota) diagFissate.morte++;
                // fuori dalla lista di Windows e senza destinazione: un resto (la barra di Windows non la mostra)
                if (vuota && winOk && !c.daWindows) { diagFissate.scartate++; scrivi1("senza destinazione e fuori dalla lista di Windows, scartata", v.nome); continue; }
                // doppia: stesso AUMID di una gia' presa, e una delle due senza destinazione (prima quella prendeva le
                // finestre e l'icona giusta restava vuota accanto) oppure stessa destinazione e stessi argomenti. Due
                // collegamenti vivi con lo stesso AUMID ma argomenti diversi (due profili di un browser) restano due
                // icone (R20261121-230321-S1: prima bastava l'AUMID, e un profilo spariva)
                size_t doppia = 0;
                if (!v.aumid.empty())
                    for (size_t k = 1; k < nuove.size(); k++) {
                        const Voce& x = nuove[k];
                        if (x.aumid != v.aumid) continue;
                        if (vuota || morta(x) || (x.exe == v.exe && x.argomenti == v.argomenti)) { doppia = k; break; }
                    }
                if (doppia) {
                    diagFissate.doppie++;
                    if (morta(nuove[doppia]) && !vuota) { scrivi1("doppia, sostituisce quella senza destinazione", v.nome); nuove[doppia] = std::move(v); }
                    else scrivi1("doppia, scartata", v.nome);
                    continue;
                }
                diagFissate.collegamenti++;
                if (v.iconaRotta) diagFissate.iconeRotte++;
                scrivi1(c.daWindows ? "dalla barra di Windows" : "solo nella cartella", v.nome + (vuota ? L" (senza destinazione)" : L"") + (v.iconaRotta ? L" (file dell'icona mancante: icona dall'exe)" : L""));
                nuove.push_back(std::move(v));
            }
            if (!valida) { fissateCache.assign(nuove.begin() + 1, nuove.end()); fissateOk = letta; }
        }
        const size_t fissate = nuove.size();
        // aggiunte nell'ordine scelto trascinandole (#23); quelle non ancora in elenco (aggiunte dopo) in fondo, nel loro
        if (!ordineFissate.empty())
            std::stable_sort(nuove.begin() + 1, nuove.begin() + fissate, [&](const Voce& a, const Voce& b) {
                return std::find(ordineFissate.begin(), ordineFissate.end(), a.avvio) < std::find(ordineFissate.begin(), ordineFissate.end(), b.avvio);
            });
        // 2) finestre aperte: si agganciano all'app fissata corrispondente, o creano una nuova voce
        struct Ctx { std::vector<HWND> v; } ctxF;
        EnumWindows([](HWND h, LPARAM l) -> BOOL { if (finestraDaDock(h)) ((Ctx*)l)->v.push_back(h); return TRUE; }, (LPARAM)&ctxF);
        // eseguibile di ogni finestra: si chiede al sistema una volta sola per finestra (stesso processo)
        for (auto it = exeFinestre.begin(); it != exeFinestre.end();)
            it = std::find(ctxF.v.begin(), ctxF.v.end(), it->first) == ctxF.v.end() ? exeFinestre.erase(it) : std::next(it);
        finestreNascoste.clear();
        for (HWND h : ctxF.v) {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            auto ef = exeFinestre.find(h);
            if (ef == exeFinestre.end() || ef->second.first != pid) ef = exeFinestre.insert_or_assign(h, std::make_pair(pid, exeProcesso(h))).first;
            const std::wstring& exe = ef->second.second;
            const std::wstring id = aumidFinestra(h);
            if (exe.empty()) continue;
            Voce* trovata = nullptr;
            // app nascosta (PR #20): si aggancia solo a un'app aggiunta alla barra (quelle non si nascondono). Prima il
            // filtro veniva dopo la ricerca, e una nascosta che condivide l'exe con un'altra aperta ci finiva dentro,
            // a seconda dell'ordine delle finestre (R20261120-022133-S1). Le sue finestre restano per "Mostra il desktop"
            const bool nascosta = eNascosta(exe, id);
            // prima l'AUMID in tutto l'elenco, poi l'exe: due app web dello stesso browser (AUMID diversi) finivano
            // nella prima icona con quell'exe, e "Chiudi tutte le finestre" prendeva anche l'altra (R20261121-001416-S6).
            // Il ripiego sull'exe resta com'era quando l'AUMID non trova niente (niente icone doppie, issue #2)
            if (!id.empty())
                for (auto& v : nuove) {
                    if (nascosta && !v.fissata) continue;
                    if (!v.aumid.empty() && id == v.aumid) { trovata = &v; break; }
                }
            if (!trovata)
                for (auto& v : nuove) {
                    if (nascosta && !v.fissata) continue;
                    if (stessoExe(v.exe, v.sqPrima, v.sqDopo, exe)) { trovata = &v; break; }
                }
            if (!trovata && nascosta) { finestreNascoste.push_back(h); continue; }
            if (!trovata) {
                Voce v; v.exe = exe; v.aumid = id;
                auto nm = nomiExe.find(exe);   // il nome leggibile si legge dal file una volta sola
                if (nm == nomiExe.end()) nm = nomiExe.emplace(exe, nomeExe(exe)).first;
                v.nome = nm->second;
                v.avvio = !id.empty() && exe.find(L"applicationframehost") != std::wstring::npos ? L"shell:AppsFolder\\" + id : exe;
                nuove.push_back(std::move(v));
                trovata = &nuove.back();
            }
            trovata->finestre.push_back(h);
        }
        // ordine stabile delle app aperte non fissate: quello di prima comparsa, non l'ordine delle finestre
        for (size_t i = fissate; i < nuove.size(); i++)
            if (std::find(ordineAperte.begin(), ordineAperte.end(), chiave(nuove[i])) == ordineAperte.end()) ordineAperte.push_back(chiave(nuove[i]));
        std::stable_sort(nuove.begin() + fissate, nuove.end(), [&](const Voce& a, const Voce& b) {
            return std::find(ordineAperte.begin(), ordineAperte.end(), chiave(a)) < std::find(ordineAperte.begin(), ordineAperte.end(), chiave(b));
        });
        // in fondo, come su macOS: la pila dei Download, il cestino e per ultimo il centro di controllo (disegnati dal dock)
        { Voce p; p.fissata = true; p.avvio = AVVIO_PILA; p.nome = testi::t(L"Download", L"Downloads"); nuove.push_back(std::move(p)); }
        { Voce c; c.fissata = true; c.avvio = AVVIO_CESTINO; c.nome = testi::t(L"Cestino", L"Recycle Bin"); nuove.push_back(std::move(c)); }
        { Voce c; c.fissata = true; c.avvio = AVVIO_CONTROLLO; c.nome = testi::t(L"Centro di controllo", L"Control Center"); nuove.push_back(std::move(c)); }
        // icone, rimbalzi e dimensioni gia' esistenti si conservano; le voci nuove crescono da zero
        const bool primaVolta = voci.empty();
        std::vector<bool> ritrovata(voci.size(), false);
        for (auto& v : nuove) {
            bool vecchiaTrovata = false;
            for (size_t j = 0; j < voci.size(); j++) {
                Voce& vecchia = voci[j];
                if (ritrovata[j]) continue;   // anche una voce che si stava restringendo: se l'app si riapre torna a crescere
                if (vecchia.avvio == v.avvio || (!v.exe.empty() && vecchia.exe == v.exe)) {
                    v.icona = vecchia.icona; vecchia.icona = nullptr;
                    v.rimbalzoT0 = v.finestre.empty() ? vecchia.rimbalzoT0 : -1;   // l'app si e' aperta: fine del rimbalzo
                    v.sobbalzoT0 = vecchia.sobbalzoT0; v.sobbalzoDopo = vecchia.sobbalzoDopo; v.sobbalzoAmp = vecchia.sobbalzoAmp;
                    v.scala = vecchia.scala; v.dx = vecchia.dx;
                    ritrovata[j] = true; vecchiaTrovata = true;
                    break;
                }
            }
            if (!v.icona && v.avvio != AVVIO_LAUNCHER && v.avvio != AVVIO_CONTROLLO && v.avvio != AVVIO_CESTINO) {
                // all'avvio subito (non c'e' ancora niente da animare); poi dal thread delle icone, senza fermare il dock
                if (primaVolta) v.icona = caricaIcona(sorgenteIcona(v));
                else chiediIcona(sorgenteIcona(v), 256);
                if (!vecchiaTrovata && !primaVolta) v.scala = 0;   // voce nuova: cresce da zero
            }
        }
        // le app chiuse restano al loro posto mentre si restringono
        size_t fissateTot = fissate;
        for (size_t j = 0; j < voci.size(); j++) {
            if (ritrovata[j]) continue;
            Voce& vecchia = voci[j];
            if (vecchia.uscente && vecchia.scala < 0.02) continue;
            vecchia.uscente = true; vecchia.finestre.clear(); vecchia.rimbalzoT0 = -1;
            size_t pos = std::min(j, nuove.size());
            if (vecchia.fissata) { pos = std::min(pos, fissateTot); fissateTot++; }
            else pos = std::max(pos, fissateTot);
            Voce u = std::move(vecchia); vecchia.icona = nullptr;   // l'icona passa alla voce uscente: non va liberata qui sotto
            nuove.insert(nuove.begin() + pos, std::move(u));
        }
        // (un'app che si chiude puo' essere finita dopo le voci di fondo: loro restano in fondo, nel loro ordine)
        std::stable_partition(nuove.begin(), nuove.end(), [](const Voce& v) { return !voceDiFondo(v.avvio); });
        primaFine = nuove.size();
        for (size_t i = 0; i < nuove.size(); i++) if (voceDiFondo(nuove[i].avvio)) { primaFine = i; break; }
        for (auto& vecchia : voci) if (vecchia.icona) vecchia.icona->Release();
        voci = std::move(nuove);
        primaNonFissata = fissateTot;
        premuta = -1;
        if (!chiavePremuta.empty())
            for (size_t i = 0; i < voci.size(); i++) if (!voci[i].uscente && chiave(voci[i]) == chiavePremuta) premuta = (int)i;
        if (trascinando) {   // (l'app trascinata si e' chiusa nel frattempo: il trascinamento finisce)
            trascinata = -1;
            for (size_t i = 0; i < voci.size(); i++) if (!voci[i].uscente && identita(voci[i]) == chiaveTrascinata) { trascinata = (int)i; break; }
            if (trascinata < 0) fineTrascina();
        }
        sopra = -1;   // si ricalcola al prossimo fotogramma
        dirty = true;
    }

    // ---- riordino trascinando (#23) ----
    // x di ogni icona come la disegnerebbe adesso render (lente compresa, senza gli scostamenti dx)
    std::vector<float> posizioniAdesso() const { return disponi(dimensioni(posizioniRiposo())); }
    // gruppo della voce i: [inizio, fine) delle aggiunte alla barra o delle altre aperte (vuoto: non si sposta)
    std::pair<size_t, size_t> gruppo(int i) const {
        if (i < 1 || i >= (int)primaFine || voci[i].uscente) return {0, 0};
        return (size_t)i < primaNonFissata ? std::make_pair((size_t)1, primaNonFissata) : std::make_pair(primaNonFissata, primaFine);
    }
    bool trascinabile(int i) const { const auto [a, b] = gruppo(i); return b > a + 1; }

    void iniziaTrascina() {
        trascinando = true; trascinata = premuta;
        premuta = -1;   // (da qui conta trascinata: niente clic al rilascio)
        const D2D1_RECT_F& r = voci[trascinata].rett;
        presa = r.right > r.left ? std::clamp((premutaX - r.left) / (r.right - r.left), 0.0f, 1.0f) : 0.5f;
        ant.chiudi();
        if (registro) { char b[200]; snprintf(b, 200, "dock: trascino %ls", voci[trascinata].nome.c_str()); scrivi(b); }
        dirty = true;
    }

    // la voce trascinata passa davanti alle vicine quando il suo centro supera il loro; le vicine scivolano (dx)
    void passoTrascina() {
        if (trascinata < 0 || trascinata >= (int)voci.size()) return;
        const auto [a, b] = gruppo(trascinata);
        if (b <= a) return;
        for (int giri = 0; giri < 40; giri++) {
            const auto riposo = posizioniRiposo();
            const float c = mouseX - presa * S + S / 2;   // centro dell'icona trascinata
            int j = -1;
            if ((size_t)trascinata + 1 < b && c > riposo[trascinata + 1] + S / 2) j = trascinata + 1;
            else if ((size_t)trascinata > a && c < riposo[trascinata - 1] + S / 2) j = trascinata - 1;
            if (j < 0) break;
            const float primaX = voci[j].rett.left;   // dove si vede adesso la vicina (ultimo disegno, lente compresa)
            std::swap(voci[trascinata], voci[j]);
            const auto dopo = posizioniAdesso();
            voci[trascinata].dx = primaX - dopo[trascinata];   // (la vicina e' ora all'indice trascinata): parte da li'
            trascinata = j;
            ricordaOrdine();
            dirty = true;
        }
    }

    // l'ordine di adesso diventa quello che aggiornaApp rispetta
    void ricordaOrdine() {
        std::vector<std::wstring> f;
        for (size_t i = 1; i < primaNonFissata && i < voci.size(); i++) if (voci[i].fissata) f.push_back(voci[i].avvio);
        // (anche quelle in elenco ma non trovate adesso, per esempio collegamenti spariti per un attimo: restano in fondo)
        for (const auto& k : ordineFissate) if (std::find(f.begin(), f.end(), k) == f.end()) f.push_back(k);
        ordineFissate = std::move(f);
        // aperte: le chiavi del gruppo, nell'ordine nuovo, al posto della prima di loro in ordineAperte
        std::vector<std::wstring> g;
        for (size_t i = primaNonFissata; i < primaFine && i < voci.size(); i++) g.push_back(chiave(voci[i]));
        size_t pos = ordineAperte.size();
        for (size_t k = 0; k < ordineAperte.size(); k++)
            if (std::find(g.begin(), g.end(), ordineAperte[k]) != g.end()) { pos = std::min(pos, k); }
        std::vector<std::wstring> r;
        for (size_t k = 0; k < ordineAperte.size(); k++) {
            if (k == pos) r.insert(r.end(), g.begin(), g.end());
            if (std::find(g.begin(), g.end(), ordineAperte[k]) == g.end()) r.push_back(ordineAperte[k]);
        }
        if (pos == ordineAperte.size()) r.insert(r.end(), g.begin(), g.end());
        ordineAperte = std::move(r);
    }

    // fine: l'icona si posa al suo posto scivolando da dove la si e' lasciata; l'ordine delle aggiunte si salva
    void fineTrascina() {
        if (!trascinando) return;
        trascinando = false;
        if (trascinata >= 0 && trascinata < (int)voci.size()) {
            voci[trascinata].dx = voci[trascinata].rett.left - posizioniAdesso()[trascinata];   // da dove la si vede
            const bool fissata = (size_t)trascinata < primaNonFissata;
            if (registro) { char b[200]; snprintf(b, 200, "dock: %ls posata al posto %d", voci[trascinata].nome.c_str(), trascinata); scrivi(b); }
            if (fissata) salvaOrdineFissate();
        }
        trascinata = -1; premuta = -1;
        if (GetCapture() == icone) ReleaseCapture();
        // il WM_MOUSELEAVE arrivato durante il trascinamento e' stato ignorato: se il mouse ora e' fuori, si esce adesso;
        // se e' dentro, si riarma l'avviso di uscita
        POINT p; GetCursorPos(&p);
        if (WindowFromPoint(p) != icone) { mouseDentro = false; mouseSulDock = false; sopra = -1; statoSopra = -1; }
        else { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, icone, 0}; TrackMouseEvent(&tm); }
        dirty = true;
    }

    // REG_MULTI_SZ: un collegamento per stringa
    void salvaOrdineFissate() const {
        std::wstring s;
        for (const auto& k : ordineFissate) { s += k; s.push_back(L'\0'); }
        s.push_back(L'\0');
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"OrdineFissate", REG_MULTI_SZ, s.data(), (DWORD)(s.size() * sizeof(wchar_t)));
    }
    void leggiOrdineFissate() {
        ordineFissate.clear();
        DWORD n = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"OrdineFissate", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &n) != ERROR_SUCCESS || n < 4 || n > 256 * 1024) return;
        // (due zeri in piu' in fondo: la lista finisce anche se il valore e' stato scritto senza il doppio zero)
        std::vector<wchar_t> b(n / sizeof(wchar_t) + 2, L'\0');
        DWORD cap = (DWORD)((b.size() - 2) * sizeof(wchar_t));
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"OrdineFissate", RRF_RT_REG_MULTI_SZ, nullptr, b.data(), &cap) != ERROR_SUCCESS) return;
        for (const wchar_t* p = b.data(); p < b.data() + b.size() && *p; p += wcslen(p) + 1) ordineFissate.emplace_back(p);
    }

    // ---- lato dello schermo (impostazione "Posizione", 1.3.0, Fase 3) ----
    // Il dock si calcola sempre come se stesse in basso: x lungo il dock (0..W, W = lunghezza del bordo), y di traverso
    // (0..H, il bordo dello schermo a y = H). Queste funzioni lo portano sul lato scelto: sfondi, separatori, pallini e
    // ritagli si girano con la trasformazione base(); icone, testi e simboli restano dritti (dritto: si sposta solo il
    // loro centro). Ai lati la finestra e' piu' spessa di extraLato, lo spazio dell'etichetta accanto alle icone
    int lato = 0;   // 0 in basso, 1 in alto, 2 a sinistra, 3 a destra
    bool verticale() const { return lato >= 2; }
    int extraLato() const { return verticale() ? (int)std::ceil(360 * dpi) : 0; }
    int Hf() const { return H + extraLato(); }
    int finW() const { return verticale() ? Hf() : W; }
    int finH() const { return verticale() ? W : H; }
    int lunghezzaBordo() const { return verticale() ? (int)(schermo.bottom - schermo.top) : (int)(schermo.right - schermo.left); }
    D2D1_MATRIX_3X2_F base() const {   // logico -> finestra (Matrix3x2F: m11, m12, m21, m22, dx, dy)
        switch (lato) {
            case 1: return D2D1::Matrix3x2F(1, 0, 0, -1, 0, (float)H);
            case 2: return D2D1::Matrix3x2F(0, 1, -1, 0, (float)H, 0);
            case 3: return D2D1::Matrix3x2F(0, 1, 1, 0, (float)(Hf() - H), 0);
            default: return D2D1::Matrix3x2F::Identity();
        }
    }
    D2D1_POINT_2F fis(float x, float y) const {
        switch (lato) {
            case 1: return {x, H - y};
            case 2: return {H - y, x};
            case 3: return {y + (Hf() - H), x};
            default: return {x, y};
        }
    }
    D2D1_RECT_F fis(const D2D1_RECT_F& r) const {
        const D2D1_POINT_2F a = fis(r.left, r.top), b = fis(r.right, r.bottom);
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
    }
    // finestra -> logico (il mouse)
    void logico(float X, float Y, float& x, float& y) const {
        switch (lato) {
            case 1: x = X; y = H - Y; break;
            case 2: x = Y; y = H - X; break;
            case 3: x = Y; y = X - (Hf() - H); break;
            default: x = X; y = Y;
        }
    }
    // angolo in alto a sinistra della finestra delle icone sullo schermo
    POINT origine() const {
        switch (lato) {
            case 1: case 2: return {schermo.left, schermo.top};
            case 3: return {schermo.right - Hf(), schermo.top};
            default: return {schermo.left, schermo.bottom - H};
        }
    }
    // rettangolo logico -> schermo
    RECT aSchermo(const D2D1_RECT_F& r) const {
        const D2D1_RECT_F f = fis(r); const POINT o = origine();
        return {o.x + (LONG)std::lround(f.left), o.y + (LONG)std::lround(f.top), o.x + (LONG)std::lround(f.right), o.y + (LONG)std::lround(f.bottom)};
    }
    // punto dello schermo -> logico
    void logicoSchermo(POINT pt, float& x, float& y) const { const POINT o = origine(); logico((float)(pt.x - o.x), (float)(pt.y - o.y), x, y); }
    // disegno dritto attorno al centro logico (cx, cy), con una scala; piano() torna alla trasformazione del lato
    void dritto(float cx, float cy, float s = 1) {
        const D2D1_POINT_2F p = fis(cx, cy);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {cx, cy}) * D2D1::Matrix3x2F::Translation(p.x - cx, p.y - cy));
    }
    void piano() { dc->SetTransform(base()); }
    void posizionaFinestra() { const POINT o = origine(); SetWindowPos(icone, HWND_TOPMOST, o.x, o.y, finW(), finH(), SWP_NOACTIVATE); }
    // il punto p dello schermo e' sul bordo del dock (o oltre, verso l'esterno) dello schermo r?
    bool sulBordo(POINT p, const RECT& r) const {
        switch (lato) {
            case 1: return p.y <= r.top + 1;
            case 2: return p.x <= r.left + 1;
            case 3: return p.x >= r.right - 2;
            default: return p.y >= r.bottom - 2;
        }
    }
    // dove si apre un menu del dock (pt: il clic, schermo): fuori dal dock, dal suo lato interno. In basso sopra il clic
    // (verso l'alto), in alto sotto il dock, ai lati accanto (a destra il menu lo sposta a sinistra, vedi menu::areaLibera).
    // Prima era sempre 100 px a sinistra del clic: col dock a sinistra finiva fuori dallo schermo, o su quello accanto
    POINT puntoMenu(POINT pt) const {
        const POINT o = origine();
        const D2D1_POINT_2F bi = fis(0, yBaseRiposo() - S - PAD - 2 * dpi);   // bordo interno del dock a riposo
        switch (lato) {
            case 1: return {pt.x - (LONG)(100 * dpi), o.y + (LONG)std::lround(bi.y + 6 * dpi)};
            case 2: return {o.x + (LONG)std::lround(bi.x + 6 * dpi), pt.y - (LONG)(20 * dpi)};
            case 3: return {o.x + (LONG)std::lround(bi.x - 6 * dpi), pt.y - (LONG)(20 * dpi)};
            default: return {pt.x - (LONG)(100 * dpi), pt.y - (LONG)(14 * dpi)};
        }
    }
    // posizione lungo il bordo del punto p dello schermo (x logica)
    float lungoBordo(POINT p) const { return verticale() ? (float)(p.y - schermo.top) : (float)(p.x - schermo.left); }

    // ---- disposizione ----
    float yBaseRiposo() const { return H - MARGINE - PAD; }                       // bordo inferiore delle icone, dock visibile
    float yBaseIcone() const { return yBaseRiposo() + (float)yNascosto; }        // ... e dove sono adesso (coordinate finestra)

    // molla smorzata a piccoli passi (zeta < 1 = rimbalza, effetto gelatina); true se si muove ancora
    bool molla(double& x, double& v, double obiettivo, double k, double zeta, double eps = 0.05) const {
        const int n = 4; const double dt = dtFotogramma / n, c = 2 * std::sqrt(k) * zeta;
        for (int i = 0; i < n; i++) { v += (k * (obiettivo - x) - c * v) * dt; x += v * dt; }
        if (std::abs(obiettivo - x) < eps && std::abs(v) < eps * 10) { x = obiettivo; v = 0; return false; }
        return true;
    }

    // dock nascosto: il mouse sul bordo in basso lo fa risalire; quando si allontana torna giu'
    // un clic fuori dal centro di controllo lo chiude anche se non ha mai avuto il primo piano (su alcuni PC Windows non
    // glielo da' e WM_ACTIVATE non arriva): non dipende piu' dal fuoco per chiudersi, e quindi nemmeno per restare aperto
    bool clicPrima = false;
    void controllaClicFuori() {
        const bool clic = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
        const bool nuovo = clic && !clicPrima;
        clicPrima = clic;
        if (!nuovo || nessunaScheda() || pChiusura || adesso() - pT0 < 0.15) return;
        POINT pt; GetCursorPos(&pt);
        RECT rp{}; if (pannello && GetWindowRect(pannello, &rp) && PtInRect(&rp, pt)) return;
        const RECT is = rettIsolaSchermo(); if (PtInRect(&is, pt)) return;   // l'icona del centro: decide il suo clic
        if (schedaFinestra(WindowFromPoint(pt))) return;
        scrivi("centro di controllo: clic fuori, si chiude");
        chiudiTutte();
    }

    void controllaBordo() {
        controllaClicFuori();
        POINT pt; GetCursorPos(&pt);
        // un altro schermo: col mouse fermo sul suo bordo dal lato del dock (e nessun tasto premuto, per non spostarlo
        // trascinando una finestra) il dock va li'
        if (!(pt.x >= schermo.left && pt.x < schermo.right && pt.y >= schermo.top && pt.y < schermo.bottom)) {
            const HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            const bool tasti = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
            if (m && altriSchermi && !tasti && !nascostoSchermoIntero && GetMonitorInfoW(m, &mi) && sulBordo(pt, mi.rcMonitor)) {
                if (m != bordoAltro) { bordoAltro = m; bordoAltroDa = adesso(); }
                else if (adesso() - bordoAltroDa > 0.2) { bordoAltro = nullptr; spostaVerso(m); }
            } else bordoAltro = nullptr;
            // il dock e l'isola rivelati sullo schermo lasciato tornano giu' come al solito (il mouse e' lontano)
        } else bordoAltro = nullptr;
        float x = 0, ly = 0; logicoSchermo(pt, x, ly);   // (x lungo il bordo del dock, ly di traverso)
        // sul bordo in basso le zone sono solo due: sotto il dock (risale il dock) e da sotto l'isola di stato fino
        // all'angolo destro (risale l'isola). Lo spazio vuoto fra i due e i lati non fanno nulla
        // (isola tolta: mai. L'ora nel dock fa da isola per il mouse ma sta nel dock: la fa risalire la zona del dock)
        const bool zonaIsola = !oraNelDock && isola.right > isola.left && x >= isola.left - 10 * dpi;
        const bool zonaDock = x >= zonaDockL - 10 * dpi && x <= zonaDockR + 10 * dpi;
        if (sulBordo(pt, schermo) && PtInRect(&schermo, pt) && (zonaIsola || zonaDock)) {
            bool &r = zonaIsola ? rivelatoI : rivelato;
            // risalendo torna sopra a tutto (anche alle finestre "sempre in primo piano" ingrandite dopo di lui)
            if (!r) {
                r = true; (zonaIsola ? fuoriDalI : fuoriDal) = -1; dirty = true; posizionaVetri();
                if (registro) { char b[120]; snprintf(b, 120, "bordo: risale %s (mouse %ld,%ld, nascosto per schermo intero %d)", zonaIsola ? "l'isola" : "il dock", pt.x, pt.y, (int)nascostoSchermoIntero); scrivi(b); }
            }
        }
        const bool alto = ly >= yBaseRiposo() - M - PAD - 30 * dpi;   // (vicino al dock, dal lato interno)
        if (rivelato) {
            // "sopra al dock" dalla posizione vera del mouse (mouseDentro puo' restare vero se Windows non avvisa dell'uscita)
            const bool sopraIcone = WindowFromPoint(pt) == icone;
            const bool vicino = (sopraIcone && mouseSulDock) || (alto && x >= zonaDockL - 40 * dpi && x <= zonaDockR + 40 * dpi);
            // (pannello, impostazioni o anteprime delle finestre aperti: il dock resta su; le anteprime senza il dock
            // sotto restavano sospese a mezz'aria)
            // (anche durante il riordino delle icone: il mouse puo' salire, il dock non deve scendere, R20261120-013029-S2)
            if (vicino || lAperto || !nessunaScheda() || dockSu || ant.aperta() || trascinando) fuoriDal = -1;
            else if (fuoriDal < 0) fuoriDal = adesso();
            else if (adesso() - fuoriDal > 0.35) { rivelato = false; fuoriDal = -1; dirty = true; }
        }
        if (rivelatoI) {
            const bool vicino = (WindowFromPoint(pt) == icone && !mouseSulDock) || (alto && zonaIsola);
            if (vicino || !nessunaScheda() || dockSu) fuoriDalI = -1;
            else if (fuoriDalI < 0) fuoriDalI = adesso();
            else if (adesso() - fuoriDalI > 0.35) { rivelatoI = false; fuoriDalI = -1; dirty = true; }
        }
    }

    // il separatore compare e scompare insieme alla prima app aperta non fissata
    float larghezzaSeparatore() const {
        if (primaNonFissata == 0 || primaNonFissata >= voci.size()) return 0;
        double k = 0; for (size_t i = primaNonFissata; i < voci.size(); i++) k = std::max(k, voci[i].scala);
        return (float)((GAP * 2 + 1) * k);
    }

    // separatore prima dell'icona i: fra fissate e aperte, e prima delle voci di fondo (uno solo se coincidono)
    float sepPrima(size_t i) const {
        if (i == 0) return 0;
        if (i == primaNonFissata) return larghezzaSeparatore();
        if (i == primaFine) return (float)(GAP * 2 + 1);
        return 0;
    }

    // ora e data in fondo al dock: separatore e testo (larghezzaOra la misura render quando il testo cambia)
    // (ai lati l'ora e la data stanno una sopra l'altra nello spessore del dock: lungo il dock conta l'altezza delle due righe)
    float larghezzaCoda() const {
        if (!oraNelDock || larghezzaOra <= 0) return 0;
        return GAP * 2 + 1 + (verticale() ? (2 * 20 * dpi + 6 * dpi) * kOra() : (larghezzaOra + 16 * dpi) * kOra());
    }
    // testa del dock (musica e timer): capsula e separatore, scalati con la comparsa
    float larghezzaTesta() const { return testaK > 0.001 && largTesta > 0 ? (float)((largTesta + GAP * 2 + 1) * testaK) : 0; }
    // le due righe dell'ora (~34 px) devono stare nell'altezza delle icone: con icone piccole il testo si rimpicciolisce
    float scalaOra() const { return std::clamp((S - 6 * dpi) / (34 * dpi), 0.7f, 1.0f); }
    // ... e ai lati anche la larghezza del testo nello spessore del dock
    float kOra() const { return verticale() ? std::min(scalaOra(), (S - 2 * dpi) / std::max(1.0f, larghezzaOra + 16 * dpi)) : scalaOra(); }

    // x di ogni icona a riposo; lo spazio prima di un'icona cresce e cala con lei
    std::vector<float> posizioniRiposo() const { return disponi(std::vector<float>(voci.size(), S)); }

    // dato il lato di ogni icona (gia' ingrandito), restituisce la x sinistra di ognuna, dock centrato
    std::vector<float> disponi(const std::vector<float>& lato, float* totale = nullptr) const {
        std::vector<float> x(voci.size());
        float tot = 0;
        for (size_t i = 0; i < voci.size(); i++) {
            if (i > 0) tot += GAP * (float)voci[i].scala;
            tot += sepPrima(i);
            tot += lato[i] * (float)voci[i].scala;
        }
        tot += larghezzaCoda();   // ora e data in fondo (impostazione): il dock resta centrato con loro
        const float testa = larghezzaTesta();   // musica e timer all'inizio (notch a scomparsa)
        tot += testa;
        float cur = W / 2.0f - tot / 2.0f + testa;
        for (size_t i = 0; i < voci.size(); i++) {
            if (i > 0) cur += GAP * (float)voci[i].scala;
            cur += sepPrima(i);
            x[i] = cur; cur += lato[i] * (float)voci[i].scala;
        }
        if (totale) *totale = tot;
        return x;
    }

    // lato di ogni icona con l'ingrandimento "a lente" attorno al mouse (prima della scala di comparsa)
    std::vector<float> dimensioni(const std::vector<float>& riposo) const {
        std::vector<float> d(voci.size(), S);
        if (lente <= 0.001) return d;
        const float raggio = S * 2.6f;
        for (size_t i = 0; i < voci.size(); i++) {
            const float c = riposo[i] + S * (float)voci[i].scala / 2;
            const float dist = std::min(std::abs(mouseX - c) / raggio, 1.0f);
            const float k = (std::cos(dist * (float)M_PI) + 1) / 2;   // 1 sotto il mouse, 0 a distanza
            d[i] = S + (M - S) * k * (float)lente;
        }
        return d;
    }

    // avvicinamento esponenziale indipendente dalla frequenza dei fotogrammi (tau in secondi)
    double verso(double attuale, double obiettivo, double tau) const {
        const double r = obiettivo + (attuale - obiettivo) * std::exp(-dtFotogramma / tau);
        return std::abs(r - obiettivo) < 0.004 ? obiettivo : r;
    }

    // ---- disegno ----
    void render(double t) {
        // chiaro o scuro: dal tema, o da quanto e' chiaro il colore scelto per il dock (puntini, separatori, testi)
        const bool scuro = coloreAuto ? temaScuro() : fondoScuro;
        bool movimento = false;

        // comparsa e scomparsa delle icone
        for (auto& v : voci) {
            const double o = v.uscente ? 0.0 : 1.0;
            if (v.scala != o) { v.scala = verso(v.scala, o, 0.07); movimento = true; }
        }
        for (size_t i = 0; i < voci.size();) {
            if (voci[i].uscente && voci[i].scala <= 0) {
                if (voci[i].icona) voci[i].icona->Release();
                voci.erase(voci.begin() + i);
                if (i < primaNonFissata) primaNonFissata--;
                if (i < primaFine) primaFine--;
                if (trascinando && (int)i < trascinata) trascinata--;   // (la trascinata non e' mai uscente)
                premuta = -1; sopra = -1;
            } else i++;
        }

        // riordino (#23): le icone spostate scivolano al posto nuovo (la trascinata no: segue il mouse)
        for (size_t i = 0; i < voci.size(); i++) {
            Voce& v = voci[i];
            if (v.dx == 0 || (trascinando && (int)i == trascinata)) continue;
            v.dx = std::abs(v.dx) < 0.5 ? 0 : verso(v.dx, 0, 0.07);
            movimento = true;
        }

        // molla dell'ingrandimento: solo col mouse sul dock (non sull'isola di stato)
        const double obiettivo = (mouseDentro && mouseSulDock) ? 1.0 : 0.0, dt = dtFotogramma / 2;
        const double kMolla = 520, smorz = 2 * std::sqrt(kMolla) * 0.95;
        for (int i = 0; i < 2; i++) { const double acc = kMolla * (obiettivo - lente) - smorz * lenteVel; lenteVel += acc * dt; lente += lenteVel * dt; }
        if (std::abs(obiettivo - lente) < 0.001 && std::abs(lenteVel) < 0.01) { lente = obiettivo; lenteVel = 0; }
        if (lente != obiettivo || lenteVel != 0) movimento = true;

        // a scomparsa: scende piano sotto il bordo, risale con un piccolo rimbalzo (gelatina)
        const double fuori = altezzaDock + MARGINE + 6 * dpi;
        // spostamento su un altro schermo: scende tutto e, arrivato sotto il bordo, passa sull'altro e risale
        if (spostamento && yNascosto >= fuori - 1 && yNascostoI >= fuori - 1) {
            const HMONITOR m = spostamento; spostamento = nullptr;
            cambiaSchermo(m);
            yNascosto = yNascostoI = fuori; velNascosto = velNascostoI = 0;
            rivelato = true; fuoriDal = -1;   // risale anche se sul nuovo schermo dovrebbe stare nascosto (il mouse e' li')
            movimento = true;
        }
        // con un pannello del centro di controllo aperto (anche dal notch) il dock resta su; scende quando si chiude
        const bool giu = spostamento || (vuoleNascondersi && !rivelato && t > rivelaFino && !lAperto && nessunaScheda() && !dockSu);
        if (giu) movimento |= molla(yNascosto, velNascosto, fuori, 260, 0.9);
        else movimento |= molla(yNascosto, velNascosto, 0, 340, 0.42);
        const bool giuI = spostamento || (vuoleNascondersiI && !rivelatoI && nessunaScheda() && !dockSu);
        if (giuI) movimento |= molla(yNascostoI, velNascostoI, fuori, 260, 0.9);
        else movimento |= molla(yNascostoI, velNascostoI, 0, 340, 0.42);
        if (vuoleNascondersi && t <= rivelaFino) movimento = true;   // allo scadere della comparsa temporanea deve riscendere

        // ora e data in fondo al dock: si rimisurano solo quando cambia il testo (la larghezza entra nella disposizione)
        if (oraNelDock && (testoOra != misuratoOra || testoData != misuratoData)) {
            misuratoOra = testoOra; misuratoData = testoData;
            larghezzaOra = std::max(larghezzaTesto(testoOra, fmtOra), larghezzaTesto(testoData, fmtData));
        }

        // testa del dock (notch a scomparsa): cosa mostra e quanto e' larga, prima della disposizione
        movimento |= statoTesta(t);

        // il dock resta centrato e si allarga attorno alle icone ingrandite
        const auto riposo = posizioniRiposo();
        const auto dim = dimensioni(riposo);
        float tot = 0;
        const auto xs = disponi(dim, &tot);
        const float sinistra = W / 2.0f - tot / 2.0f;
        const float yb = yBaseIcone();

        // tirato dal mouse: il fondo si allunga un poco verso il puntatore e torna con una piccola oscillazione
        double tx = 0, ty = 0;
        if (mouseDentro && mouseSulDock) {
            tx = std::clamp((mouseX - W / 2.0f) / (tot / 2 + PAD), -1.0f, 1.0f) * 7 * dpi;
            ty = 5 * dpi;
        }
        movimento |= molla(tiroX, velTiroX, tx, 180, 0.38);
        movimento |= molla(tiroY, velTiroY, ty, 180, 0.38);

        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        piano();   // (il lato dello schermo: vedi base)
        // fondo del dock, sopra al vetro acrilico e con la sua stessa forma. Mentre risale si allunga con la
        // velocita' e all'arrivo si schiaccia un poco
        const float allunga = (float)std::clamp(-velNascosto * 0.010, -3.0 * dpi, 7.0 * dpi);
        const float baseT = std::round(yb - S - PAD - allunga), fondoB = std::round(yb + PAD);
        const float fondoL = std::round(sinistra - PAD + (float)std::min(0.0, tiroX));
        const float fondoR = std::round(sinistra + tot + PAD + (float)std::max(0.0, tiroX));
        const float fondoT = std::round(baseT - (float)tiroY);
        disegnaFondo({fondoL, fondoT, fondoR, fondoB}, scuro, opacita, coloreAuto ? nullptr : &coloreFondo);
        aggiornaVetro(vetro, rettVetro, fondoL, fondoT, fondoR, fondoB);
        zonaDockL = fondoL; zonaDockR = fondoR;

        // icone (la trascinata per ultima, sopra le altre)
        const bool conTrascinata = trascinando && trascinata >= 0 && trascinata < (int)voci.size();
        for (size_t k = 0; k < voci.size(); k++) {
            const size_t i = !conTrascinata ? k : k < (size_t)trascinata ? k : k + 1 < voci.size() ? k + 1 : (size_t)trascinata;
            Voce& v = voci[i];
            const bool presa_ = conTrascinata && (int)i == trascinata;
            const float sc = (float)v.scala, d = dim[i] * sc;
            // trascinata: dove la tiene il mouse (dentro il dock); le altre: al loro posto piu' lo scostamento che scivola
            const float x = presa_ ? std::clamp(mouseX - presa * d, sinistra - d / 2, sinistra + tot - d / 2) : xs[i] + (float)v.dx;
            if (sepPrima(i) > 0) {
                const float sep = sepPrima(i), sx = xs[i] - (sep + GAP * sc) / 2;
                pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.22f * sep / (GAP * 2 + 1)));
                dc->DrawLine({sx, yb - S * 0.85f}, {sx, yb - S * 0.15f}, pennello, 1.0f);
            }
            float salto = 0;
            if (v.rimbalzoT0 >= 0) {
                const double e = t - v.rimbalzoT0;
                if (e > 3.0) v.rimbalzoT0 = -1;
                else { salto = (float)(std::abs(std::sin(e * M_PI / 0.55)) * S * 0.45 * std::exp(-e * 0.35)); movimento = true; }
            }
            if (v.sobbalzoDopo >= 0) { v.sobbalzoT0 = t + v.sobbalzoDopo; v.sobbalzoDopo = -1; }
            if (v.sobbalzoT0 >= 0 && t >= v.sobbalzoT0) {   // sobbalzo: un saltello che si smorza in ~0,6 s
                const double e = t - v.sobbalzoT0;
                if (e > 0.6) v.sobbalzoT0 = -1;
                else { salto += (float)(std::abs(std::sin(e * M_PI / 0.2)) * S * 0.14 * v.sobbalzoAmp * std::exp(-e * 5.5)); movimento = true; }
            } else if (v.sobbalzoT0 >= 0) movimento = true;   // (le vicine aspettano il loro turno)
            if (presa_) salto = 6 * dpi;   // trascinata: un poco sollevata
            const float premuto = (premuta == (int)i) ? 0.86f : 1.0f;   // un attimo piu' scura e piccola mentre la premi
            const float dd = d * premuto;
            D2D1_RECT_F r = {x + (d - dd) / 2, yb - dd - salto, x + (d - dd) / 2 + dd, yb - salto};
            v.rett = {x, yb - d, x + d, yb};
            if (d < 0.5f) continue;
            const float alfa = (premuto < 1 ? 0.8f : 1.0f) * std::min(1.0f, sc * 1.5f);
            dritto((r.left + r.right) / 2, (r.top + r.bottom) / 2);   // (icona e numero dritti anche ai lati e in alto)
            if (v.icona) dc->DrawBitmap(v.icona, r, alfa, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else if (v.avvio == AVVIO_LAUNCHER) disegnaIconaApp(r, alfa, scuro);
            else if (v.avvio == AVVIO_CONTROLLO) disegnaIconaControllo(r, alfa, scuro);
            else if (v.avvio == AVVIO_CESTINO) disegnaIconaCestino(r, alfa, scuro, dropSopra == (int)i);
            else {
                pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.9f * alfa));
                dc->FillRoundedRectangle({r, dd * 0.22f, dd * 0.22f}, pennello);
            }
            // numero delle notifiche (come su Mac e iPhone): pallino rosso in alto a destra
            if (const int n = numeroDi(v); n > 0 && dd > 4) {
                wchar_t b[8]; if (n > 99) wcscpy(b, L"99+"); else swprintf(b, 8, L"%d", n);
                const float rb = std::max(9.0f * dpi, dd * 0.17f), lb = std::max(rb * 2, larghezzaTesto(b, fmtNome) + 9 * dpi);
                const float bx = r.right - lb * 0.72f, by = r.top - rb * 0.25f;
                pennello->SetColor(D2D1::ColorF(1.0f, 0.23f, 0.19f, alfa));
                dc->FillRoundedRectangle({{bx, by, bx + lb, by + 2 * rb}, rb, rb}, pennello);
                pennello->SetColor(D2D1::ColorF(1, 1, 1, alfa));
                scrivi(b, fmtNome, {bx, by, bx + lb, by + 2 * rb});
            }
            piano();
            // pallino delle app aperte: uno per finestra, al massimo tre
            // (uno anche sotto App e il centro di controllo mentre sono aperti)
            if (!v.finestre.empty() || (v.avvio == AVVIO_CONTROLLO && !nessunaScheda()) || (v.avvio == AVVIO_LAUNCHER && lAperto && !lChiusura)) {
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.85f * sc) : D2D1::ColorF(0, 0, 0, 0.62f * sc));
                const int punti = (int)std::clamp<size_t>(v.finestre.size(), 1, 3);
                for (int k = 0; k < punti; k++) {
                    const float dx = (k - (punti - 1) / 2.0f) * 6.0f * dpi;
                    dc->FillEllipse({{x + d / 2 + dx, yb + PAD * 0.55f}, 2.0f * dpi, 2.0f * dpi}, pennello);
                }
            }
        }

        // ora e data in fondo al dock (impostazione): un separatore, poi l'ora e sotto la data, dopo il centro di controllo
        if (const float coda = larghezzaCoda(); coda > 0) {
            const float x0 = sinistra + tot - coda;
            pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.22f));
            dc->DrawLine({x0 + GAP + 0.5f, yb - S * 0.85f}, {x0 + GAP + 0.5f, yb - S * 0.15f}, pennello, 1.0f);
            const D2D1_RECT_F r = {x0 + GAP * 2 + 1, yb - S, x0 + coda, yb};
            isola = {r.left, fondoT, r.right + PAD, fondoB};   // il suo posto per il mouse (vedi oraNelDock)
            rettStato[ORA] = r; rettStato[CONTROLLO] = {};
            const double e = verso(evidenza[ORA], (statoSopra == ORA && mouseDentro) ? 1.0 : 0.0, 0.05);
            if (e != evidenza[ORA]) movimento = true;
            evidenza[ORA] = e;
            const float s = statoPremuto == ORA ? 0.92f : 1.0f;   // si "schiaccia" un poco alla pressione
            const float k = kOra();                              // icone piccole (e ai lati, testo largo): piu' piccolo (vedi kOra)
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            const float h2 = 17 * dpi;                           // altezza di una riga (a grandezza piena)
            // capsula attorno al testo: la stessa per il fondo e per l'evidenza del mouse
            const float pw = (larghezzaOra / 2 + 8 * dpi) * k * s, ph = (h2 + 3 * dpi) * k * s;
            const D2D1_ROUNDED_RECT capsula = {{cx - pw, cy - ph, cx + pw, cy + ph}, std::min(9 * dpi, ph) * raggio::fattore(), std::min(9 * dpi, ph) * raggio::fattore()};
            dritto(cx, cy);   // (capsula e testo dritti: ai lati stanno nello spessore del dock)
            // fondo: compare solo quanto il vetro del dock e' trasparente (opacita' nelle impostazioni), cosi' l'ora si legge
            // anche sopra una finestra chiara; col vetro pieno non c'e'. Lo dice la trasparenza della finestra del vetro
            {
                BYTE av = 255; DWORD fl = 0;
                if (!GetLayeredWindowAttributes(vetro, nullptr, &av, &fl) || !(fl & LWA_ALPHA)) av = 255;
                const float quanto = std::clamp((1 - av / 255.0f) / 0.8f, 0.0f, 1.0f);
                if (quanto > 0.02f) {
                    pennello->SetColor(scuro ? D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.5f * quanto) : D2D1::ColorF(1, 1, 1, 0.6f * quanto));
                    dc->FillRoundedRectangle(capsula, pennello);
                }
            }
            if (e > 0.01) {
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f * (float)e) : D2D1::ColorF(1, 1, 1, 0.55f * (float)e));
                dc->FillRoundedRectangle(capsula, pennello);
            }
            // testo disegnato a grandezza piena e rimpicciolito attorno al centro (pressione e icone piccole)
            dritto(cx, cy, s * k);
            const D2D1_COLOR_F ink = scuro ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.86f);
            const float lt = larghezzaOra / 2 + 8 * dpi;
            pennello->SetColor(ink);
            dc->DrawText(testoOra.c_str(), (UINT32)testoOra.size(), fmtOra, {cx - lt, cy - h2 - 1 * dpi, cx + lt, cy + 1 * dpi}, pennello);
            pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.72f});
            dc->DrawText(testoData.c_str(), (UINT32)testoData.size(), fmtData, {cx - lt, cy + 1 * dpi, cx + lt, cy + h2 - 1 * dpi}, pennello);
            piano();
        }

        // testa del dock (musica e timer, notch a scomparsa): all'inizio, prima della prima icona
        movimento |= disegnaTestaDock(sinistra, yb, scuro, t);

        // icona sotto il mouse ricalcolata sulle posizioni appena disegnate (il dock si muove anche col mouse fermo)
        sopra = (mouseDentro && mouseSulDock) ? iconaInPunto(mouseX, mouseY) : -1;

        // (l'isola di stato in basso a destra non c'e' piu': il centro di controllo e' l'ultima icona del dock e l'ora sta
        // nel notch in alto al centro)

        // etichetta: nome dell'app, oppure descrizione dell'elemento di stato
        bool etichettaVoluta = false;
        // (non con le anteprime aperte: le coprirebbe, e il nome e' gia' nei loro titoli)
        if (sopra >= 0 && premuta < 0 && !trascinando && !ant.aperta()) {
            const Voce& v = voci[sopra];
            etichettaTesto = v.nome; etichettaCx = (v.rett.left + v.rett.right) / 2; etichettaTop = v.rett.top - 12 * dpi;
            etichettaVoluta = true;
        } else if (statoSopra >= 0 && statoPremuto < 0 && nessunaScheda()) {   // con un pannello aperto l'etichetta lo coprirebbe
            etichettaTesto = descrizioneStato(statoSopra);
            etichettaCx = (rettStato[statoSopra].left + rettStato[statoSopra].right) / 2; etichettaTop = isola.top - 10 * dpi;
            etichettaVoluta = true;
        } else if (mouseDentro && inTesta(mouseX, mouseY) && tastoPremuto == -1 && nessunaScheda() && !ant.aperta()) {
            // testa: titolo e artista del brano, "Timer", o i segni accesi
            const musica::Brano& br = notch.musicaOra();
            if (dentro(rettMus, mouseX, mouseY)) {
                etichettaTesto = br.titolo.empty() ? testi::t(L"Musica", L"Music") : br.artista.empty() ? br.titolo : br.titolo + L" · " + br.artista;
                etichettaCx = (rettMus.left + rettMus.right) / 2;
            } else if (dentro(rettTim, mouseX, mouseY)) {
                etichettaTesto = testi::t(L"Timer", L"Timer");
                etichettaCx = (rettTim.left + rettTim.right) / 2;
            } else {
                etichettaTesto.clear();
                if (testaSveglio) etichettaTesto = testi::t(L"Tieni sveglio acceso", L"Keep awake on");
                if (testaSilenzio) etichettaTesto += (etichettaTesto.empty() ? L"" : L" · ") + std::wstring(testi::t(L"Non disturbare acceso", L"Do not disturb on"));
                etichettaCx = mouseX;
            }
            etichettaTop = rettTesta.top - 12 * dpi;
            etichettaVoluta = !etichettaTesto.empty();
        }
        const double et = verso(etichettaAlfa, etichettaVoluta ? 1.0 : 0.0, 0.025);
        if (et != etichettaAlfa) movimento = true;
        etichettaAlfa = et;
        if (etichettaAlfa > 0.01 && !etichettaTesto.empty()) disegnaEtichetta(scuro);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());   // (lo stesso contesto disegna launcher e pannelli: senza il giro del lato)
        controllaGpu(dc->EndDraw());
        inMovimento = movimento;

        // la regione della finestra limita i clic al dock (con le icone ingrandite) e all'isola di stato
        float regTop = fondoT;
        for (auto& v : voci) regTop = std::min(regTop, v.rett.top);
        if (rettTesta.right > rettTesta.left) regTop = std::min(regTop, rettTesta.top - 6 * dpi);   // (testa ingrandita con la lente)
        // l'icona trascinata e' disegnata 6 px piu' su del suo rett (che resta quello per il mouse): dentro anche lei
        // (R20261120-013029-S3)
        if (trascinando && trascinata >= 0 && trascinata < (int)voci.size()) regTop = std::min(regTop, voci[trascinata].rett.top - 6 * dpi);
        // solo se cambia: ogni SetWindowRgn genera un WM_MOUSEMOVE, che farebbe ridisegnare il dock all'infinito
        // la regione ritaglia anche il disegno: l'etichetta, finche' e' visibile, deve starci dentro
        // (dock nascosto: regioni vuote, nessun clic rubato alle finestre a schermo intero)
        // (rettangoli logici portati sul lato della finestra; l'etichetta e' gia' nelle coordinate della finestra)
        auto rettFin = [&](float l, float t, float r, float b) -> RECT {
            if (t >= b || l >= r) return RECT{};
            const D2D1_RECT_F f = fis(D2D1_RECT_F{l, t, r, b});
            return {(int)std::floor(f.left), (int)std::floor(f.top), (int)std::ceil(f.right), (int)std::ceil(f.bottom)};
        };
        const RECT rd = rettFin(fondoL - 2, std::min(regTop - 2, (float)H), fondoR + 2, (float)H);
        const RECT rs = isola.right > isola.left ? rettFin(isola.left - 2, std::min(isola.top - 2, (float)H), isola.right + 2, (float)H) : RECT{};
        RECT re{};
        if (etichettaAlfa > 0.01) re = {(int)rettEtichetta.left - 2, (int)rettEtichetta.top - 2, (int)rettEtichetta.right + 2, (int)rettEtichetta.bottom + 2};
        if (!EqualRect(&rd, &regioneDock) || !EqualRect(&rs, &regioneIsola) || !EqualRect(&re, &regioneEtichetta)) {
            regioneDock = rd; regioneIsola = rs; regioneEtichetta = re;
            HRGN rg = CreateRectRgnIndirect(&rd), ri = CreateRectRgnIndirect(&rs);
            CombineRgn(rg, rg, ri, RGN_OR); DeleteObject(ri);
            if (!IsRectEmpty(&re)) { HRGN rt = CreateRectRgnIndirect(&re); CombineRgn(rg, rg, rt, RGN_OR); DeleteObject(rt); }
            SetWindowRgn(icone, rg, FALSE);
        }
    }

    // ---- testa del dock: musica, timer, tieni sveglio e non disturbare col notch a scomparsa ----
    // Tre parti, da sinistra: i segni di "Tieni sveglio" (tazzina) e "Non disturbare" (luna), che entrano con un piccolo
    // rimbalzo e un'onda; la musica (copertina e barrette); il timer (anello e tempo). Col mouse sopra la musica si
    // allarga con indietro / play-pausa / avanti, il timer con +1 min e stop: si controllano dal dock, senza il notch

    // cosa mostra, presenze, allargamenti, scala con la lente, larghezza, copertina e ritmo del ridisegno; true se anima.
    // Ogni parte (tazzina, luna, musica, timer) entra ed esce con una molla senza rimbalzo (prima la larghezza cambiava
    // di colpo all'accendersi di un segno e il dock scattava)
    bool statoTesta(double t) {
        const musica::Brano& br = notch.musicaOra();
        if (br.c && br.suona) ultimaMusica = t;
        const bool fisso = notch.fisso();
        // in pausa resta 8 s (e finche' ci sono sopra il mouse o un tasto premuto): pausa e ripresa non fanno ballare il dock
        const bool sullaMusica = mouseDentro && testaMusica && dentro(rettMus, mouseX, mouseY);
        testaMusica = !fisso && br.c && (br.suona || t - ultimaMusica < 8 || sullaMusica || (tastoPremuto >= 0 && tastoPremuto <= 2));
        testaTimer = !fisso && comodita::timerAttivo();
        const bool sv = !fisso && comodita::sveglio(), si = !fisso && comodita::silenzio();
        if (sv && !testaSveglio) tSveglio = t;      // (comparsa: rimbalzo e onda)
        if (si && !testaSilenzio) tSilenzio = t;
        testaSveglio = sv; testaSilenzio = si;
        const bool vuole = testaMusica || testaTimer || testaSveglio || testaSilenzio;
        bool anima = false;
        // molla critica (niente rimbalzo), ~0,35 s: comparsa della testa e di ogni parte
        auto entra = [&](double& p, double& v, bool voluto, double k = 120) { anima |= molla(p, v, voluto ? 1.0 : 0.0, k, 1.0, 0.002); };
        entra(testaK, vTestaK, vuole);
        entra(pSv, vSv, testaSveglio); entra(pSi, vSi, testaSilenzio);
        entra(pMu, vMu, testaMusica); entra(pTi, vTi, testaTimer);
        // allargamenti col mouse sopra (o con un tasto premuto), un poco piu' svelti
        entra(kMus, vkMus, testaMusica && ((mouseDentro && dentro(rettMus, mouseX, mouseY)) || (tastoPremuto >= 0 && tastoPremuto <= 2)), 200);
        entra(kTim, vkTim, testaTimer && ((mouseDentro && dentro(rettTim, mouseX, mouseY)) || tastoPremuto == 3 || tastoPremuto == 4), 200);
        // scala con la lente del dock, come un'icona al suo posto: col mouse vicino cresce insieme alle icone
        scTesta = 1;
        if (lente > 0.001 && rettTesta.right > rettTesta.left && mouseDentro) {
            // distanza dal bordo piu' vicino, 0 sopra la capsula: e' larga, e dal centro (come un'icona) i tasti a destra
            // la facevano rimpicciolire mentre ci si andava sopra
            const float raggio = S * 2.6f;
            const float fuori = std::max({0.0f, rettTesta.left - mouseX, mouseX - rettTesta.right});
            const float dist = std::min(fuori / raggio, 1.0f);
            const float kk = (std::cos(dist * (float)M_PI) + 1) / 2;
            scTesta = 1 + (M / S - 1) * kk * (float)lente;
        }
        // larghezza: le parti pesano quanto la loro presenza (si allargano e stringono con la molla)
        const float d = dpi * scTesta;
        float w = 0, prima = 0;   // prima: quanto c'e' gia' a sinistra (lo spazio di 12 px compare con la parte)
        auto parte = [&](double p, float largo) {
            if (p <= 0.001) return;
            w += (float)p * largo + 12 * d * (float)p * std::min(1.0f, prima);
            prima += (float)p;
        };
        w += (float)(pSv + pSi) * 20 * d;   // (tazzina e luna una accanto all'altra, senza spazio)
        prima = std::min(1.0f, (float)(pSv + pSi));
        parte(pMu, larghezzaMusica());
        parte(pTi, larghezzaTimer());
        if (w > 0) largTesta = std::round(w + 20 * d);
        if (testaK <= 0.001 && !vuole) { rettTesta = rettMus = rettTim = {}; tastoPremuto = -1; }
        if (t - tSveglio < 0.7 || t - tSilenzio < 0.7) anima = true;   // rimbalzo e onda dei segni
        // copertina (si rifa' quando cambia il brano)
        const UINT ver = br.versione ^ (UINT)br.copertina.size();
        if (testaMusica && ver != versioneTesta) {
            versioneTesta = ver;
            if (bTesta) { bTesta->Release(); bTesta = nullptr; }
            if (br.copertina.size() >= (size_t)musica::LATO * musica::LATO)
                dc->CreateBitmap(D2D1::SizeU(musica::LATO, musica::LATO), br.copertina.data(), musica::LATO * 4,
                                 D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bTesta);
        }
        // barrette che ballano: ~16 fotogrammi al secondo; solo il timer: 4 (i secondi). Niente se non si vede
        const int periodo = testaK <= 0.001 ? 0 : pMu > 0.001 && br.suona ? 60 : pTi > 0.001 ? 250 : 0;
        if (periodo != periodoTesta) {
            periodoTesta = periodo;
            if (periodo) SetTimer(icone, TIMER_TESTA, periodo, nullptr); else KillTimer(icone, TIMER_TESTA);
        }
        return anima;
    }
    // larghezze piene di musica e timer (con i tasti in proporzione all'allargamento), alla scala attuale
    float larghezzaMusica() const { const float d = dpi * scTesta; return S * scTesta * 0.62f + 22 * d + (float)kMus * (3 * 26 + 6) * d; }
    float cifreTimer() {   // larghezza del tempo a grandezza 1 (cifre a larghezza fissa mentre i secondi scorrono)
        std::wstring z = comodita::durata(comodita::timerResto());
        for (wchar_t& c : z) if (c >= L'0' && c <= L'9') c = L'0';
        return larghezzaTesto(z, fmtOra);
    }
    float larghezzaTimer() { const float d = dpi * scTesta; return 24 * d + cifreTimer() * scTesta + (float)kTim * (44 + 6 + 26 + 4) * d; }

    static bool dentro(const D2D1_RECT_F& r, float x, float y) { return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

    // (ogni 500 ms da aggiornaStato) brano, pausa, timer, segni o notch fisso cambiati, o pausa da piu' di 8 s
    UINT firmaTesta = 0xFFFFFFFF;
    bool testaCambiata() {
        const musica::Brano& br = notch.musicaOra();
        // anche col dock giu' (nessun disegno): gli 8 s della pausa contano da quando ha smesso davvero (R20261120-141156-S2)
        if (br.c && br.suona) ultimaMusica = adesso();
        const UINT f = (notch.fisso() ? 1u : 0u) | (br.c ? 2u : 0u) | (br.suona ? 4u : 0u) | (comodita::timerAttivo() ? 8u : 0u) |
                       (comodita::sveglio() ? 16u : 0u) | (comodita::silenzio() ? 32u : 0u) | (br.versione << 6);
        const bool scaduta = testaMusica && !br.suona && adesso() - ultimaMusica >= 8;
        if (f == firmaTesta && !scaduta) return false;
        firmaTesta = f;
        return true;
    }

    bool inTesta(float x, float y) const {
        return rettTesta.right > rettTesta.left && x >= rettTesta.left && x < rettTesta.right && y >= rettTesta.top - 6 * dpi && y <= rettTesta.bottom + PAD;
    }
    // tasto della testa sotto il mouse: 0 indietro, 1 play/pausa, 2 avanti, 3 +1 min, 4 stop; -1 nessuno
    // il punto logico (x, y) e' sul tasto r? Il tasto e' disegnato dritto attorno al suo centro (dritto): si confronta
    // sullo schermo, cosi' ai lati la pillola "+1 min", larga e bassa, si prende dove la si vede (R20261120-190507-S1)
    bool sulTasto(const D2D1_RECT_F& r, float x, float y) const {
        const D2D1_POINT_2F p = fis(x, y), c = fis((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        return std::abs(p.x - c.x) <= (r.right - r.left) / 2 && std::abs(p.y - c.y) <= (r.bottom - r.top) / 2;
    }
    int tastoTestaIn(float x, float y) const {
        for (int i = 0; i < 5; i++) {
            // (solo ad allargamento finito: prima i tasti stanno gia' al posto finale, oltre lo spazio concesso, e potevano
            // finire sopra il timer, R20261120-145131-S1)
            const bool vale = i <= 2 ? kMus > 0.98 : kTim > 0.98;
            if (vale && sulTasto(tastoTesta[i], x, y)) return i;
        }
        return -1;
    }
    void premiTastoTesta(int b) {
        if (b >= 0 && b <= 2) notch.comandoMusica(b);
        else if (b == 3) comodita::aggiungiTimer(60000);
        else if (b == 4) comodita::fermaTimer();
        dirty = true;
    }

    // tazzina (tieni sveglio) e luna (non disturbare), come nel notch, col colore del dock
    void segnoTesta(bool tazza, float cx, float cy, D2D1_COLOR_F ink) {
        const float d = dpi;
        pennello->SetColor(ink);
        if (tazza) {
            dc->FillRoundedRectangle({{cx - 5 * d, cy - 2 * d, cx + 3 * d, cy + 5 * d}, 1.6f * d, 1.6f * d}, pennello);
            dc->DrawEllipse({{cx + 4 * d, cy + 1.2f * d}, 2.2f * d, 2.2f * d}, pennello, 1.3f * d);
            dc->DrawLine({cx - 2.5f * d, cy - 4 * d}, {cx - 2.5f * d, cy - 6.5f * d}, pennello, 1.1f * d);
            dc->DrawLine({cx + 0.5f * d, cy - 4 * d}, {cx + 0.5f * d, cy - 6.5f * d}, pennello, 1.1f * d);
            return;
        }
        ID2D1EllipseGeometry *a = nullptr, *b = nullptr; ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(f2d->CreateEllipseGeometry({{cx, cy}, 6 * d, 6 * d}, &a)) && SUCCEEDED(f2d->CreateEllipseGeometry({{cx + 3.6f * d, cy - 2.6f * d}, 5 * d, 5 * d}, &b)) &&
            SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            if (SUCCEEDED(a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, s)) && SUCCEEDED(s->Close())) dc->FillGeometry(g, pennello);
        }
        if (s) s->Release();
        if (g) g->Release();
        if (b) b->Release();
        if (a) a->Release();
    }

    // un tasto rotondo della testa (evidenza col mouse sopra, piu' scuro premuto)
    void fondoTasto(int i, bool scuro, float a) {
        const D2D1_RECT_F& r = tastoTesta[i];
        const bool sopraT = mouseDentro && sulTasto(r, mouseX, mouseY);
        if (!sopraT && tastoPremuto != i) return;
        const float q = tastoPremuto == i ? 0.22f : 0.12f;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, q * a) : D2D1::ColorF(0, 0, 0, q * 0.6f * a));
        const float rr = (r.bottom - r.top) / 2;
        dc->FillRoundedRectangle({r, rr, rr}, pennello);
    }

    // disegna la testa all'inizio del dock (x0: dove comincia), alla scala della lente (scTesta); true se sta animando.
    // Ogni parte e' ritagliata alla sua larghezza del momento e sfuma con la sua presenza: entrando e uscendo si scopre
    // e si copre, senza salti
    bool disegnaTestaDock(float x0, float yb, bool scuro, double t) {
        if (larghezzaTesta() <= 0) { rettTesta = rettMus = rettTim = {}; return false; }
        const float k = (float)std::clamp(testaK, 0.0, 1.0), cw = largTesta * k, sc = scTesta, d = dpi * sc, Ss = S * sc;
        const D2D1_RECT_F r = {x0, yb - Ss, x0 + cw, yb};
        rettTesta = r;
        // separatore fra la testa e la prima icona
        pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.22f * k));
        const float sx = x0 + cw + (GAP + 0.5f) * k;
        dc->DrawLine({sx, yb - S * 0.85f}, {sx, yb - S * 0.15f}, pennello, 1.0f);
        // capsula: fondo come ora e data in coda (solo col vetro trasparente), evidenza col mouse sopra
        const float cy = (r.top + r.bottom) / 2, ph = Ss * 0.42f;
        const D2D1_ROUNDED_RECT capsula = {{r.left, cy - ph, r.right, cy + ph}, std::min(10 * d, ph) * raggio::fattore(), std::min(10 * d, ph) * raggio::fattore()};
        const double e = verso(evTesta, (mouseDentro && inTesta(mouseX, mouseY)) ? 1.0 : 0.0, 0.05);
        bool anima = e != evTesta;
        evTesta = e;
        {
            BYTE av = 255; DWORD fl = 0;
            if (!GetLayeredWindowAttributes(vetro, nullptr, &av, &fl) || !(fl & LWA_ALPHA)) av = 255;
            const float quanto = std::clamp((1 - av / 255.0f) / 0.8f, 0.0f, 1.0f);
            if (quanto > 0.02f) {
                pennello->SetColor(scuro ? D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.5f * quanto * k) : D2D1::ColorF(1, 1, 1, 0.6f * quanto * k));
                dc->FillRoundedRectangle(capsula, pennello);
            }
        }
        if (e > 0.01) {
            pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f * (float)e * k) : D2D1::ColorF(1, 1, 1, 0.55f * (float)e * k));
            dc->FillRoundedRectangle(capsula, pennello);
        }
        // contenuti: entrano con la comparsa, tagliati alla capsula mentre si allarga
        dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const D2D1_COLOR_F ink0 = scuro ? D2D1::ColorF(1, 1, 1, 0.92f * k) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.86f * k);
        float x = r.left + 10 * d, prima = 0;
        // segni: rimbalzo (scala con un poco di oltre) e un'onda che si allarga e sfuma, per 0,7 s dalla comparsa
        for (int i = 0; i < 2; i++) {
            const float p = (float)std::clamp(i == 0 ? pSv : pSi, 0.0, 1.0);
            if (p <= 0.01f) continue;
            const D2D1_COLOR_F ink = {ink0.r, ink0.g, ink0.b, ink0.a * p};
            const double et = t - (i == 0 ? tSveglio : tSilenzio);
            const float cx = x + 10 * d * p;
            float sp = 1;   // rimbalzo della comparsa
            if (et < 0.7) {
                const float u = (float)std::clamp(et / 0.45, 0.0, 1.0), c1 = 1.70158f, c3 = c1 + 1;
                sp = 0.4f + 0.6f * (1 + c3 * (u - 1) * (u - 1) * (u - 1) + c1 * (u - 1) * (u - 1));   // easeOutBack
                const float o = (float)std::clamp(et / 0.7, 0.0, 1.0);
                pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.5f * (1 - o)});
                dc->DrawEllipse({{cx, cy}, (7 + 9 * o) * d, (7 + 9 * o) * d}, pennello, 1.2f * d);
            }
            dritto(cx, cy, sc * sp * (0.6f + 0.4f * p));
            segnoTesta(i == 0, cx, cy, ink);   // (disegnato a grandezza 1 attorno al centro: lo scala la trasformazione)
            piano();
            x += 20 * d * p;
        }
        prima = std::min(1.0f, (float)(pSv + pSi));
        if (pMu > 0.001) {
            const float p = (float)std::clamp(pMu, 0.0, 1.0);
            x += 12 * d * p * prima;
            const float x1 = x, largo = larghezzaMusica();
            const D2D1_COLOR_F ink = {ink0.r, ink0.g, ink0.b, ink0.a * p};
            dc->PushAxisAlignedClip({x1, r.top, x1 + largo * p, r.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const float q = Ss * 0.62f;
            const D2D1_RECT_F rc = {x, cy - q / 2, x + q, cy + q / 2};
            dritto((rc.left + rc.right) / 2, cy);   // (copertina dritta)
            ID2D1BitmapBrush* bb = nullptr;
            if (bTesta && SUCCEEDED(dc->CreateBitmapBrush(bTesta, &bb)) && bb) {
                const D2D1_SIZE_F bs = bTesta->GetSize();
                bb->SetTransform(D2D1::Matrix3x2F::Scale(q / bs.width, q / bs.height) * D2D1::Matrix3x2F::Translation(rc.left, rc.top));
                bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                bb->SetOpacity(k * p);
                dc->FillRoundedRectangle({rc, q * 0.22f, q * 0.22f}, bb);
                bb->Release();
            } else {   // senza copertina: riquadro con una nota
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.14f * k * p) : D2D1::ColorF(0, 0, 0, 0.10f * k * p));
                dc->FillRoundedRectangle({rc, q * 0.22f, q * 0.22f}, pennello);
                const float nx = (rc.left + rc.right) / 2, ny = cy;
                pennello->SetColor(ink);
                dc->FillEllipse({{nx - 2.5f * d, ny + 3.5f * d}, 2.6f * d, 2.1f * d}, pennello);
                dc->DrawLine({nx, ny + 3.5f * d}, {nx, ny - 5.5f * d}, pennello, 1.3f * d);
                dc->DrawLine({nx, ny - 5.5f * d}, {nx + 4 * d, ny - 3.5f * d}, pennello, 1.3f * d);
            }
            piano();
            // barrette: ballano mentre suona, ferme e basse in pausa (come nel notch)
            const musica::Brano& br = notch.musicaOra();
            const float bx = rc.right + 8 * d, lb = 2.4f * d, passo = (14 * d - lb) / 3;
            dritto(bx + 7 * d, cy);   // (barrette in piedi anche ai lati)
            pennello->SetColor(ink);
            for (int i = 0; i < 4; i++) {
                float kb = 0.25f;
                if (br.suona) kb = (float)(0.35 + 0.65 * std::fabs(std::sin(t * (5.1 + i * 1.7) + i * 1.3)));
                const float hb = std::max(2.4f * d, 12 * d * kb);
                dc->FillRoundedRectangle({{bx + i * passo, cy - hb / 2, bx + i * passo + lb, cy + hb / 2}, lb / 2, lb / 2}, pennello);
            }
            piano();
            x = bx + 14 * d;
            // tasti (col mouse sopra): indietro, play/pausa, avanti; disegnati solo nello spazio gia' concesso
            const float km = (float)std::clamp(kMus, 0.0, 1.0);
            dc->PushAxisAlignedClip({x, r.top, x + km * (3 * 26 + 6) * d, r.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int i = 0; i < 3; i++) {
                const float tc = x + 6 * d + (13 + 26 * i) * d;
                tastoTesta[i] = {tc - 12 * d, cy - 12 * d, tc + 12 * d, cy + 12 * d};
                if (km < 0.02f) continue;
                fondoTasto(i, scuro, km * k * p);
                dritto(tc, cy);   // (simboli dritti: ai lati i tasti stanno in colonna)
                pennello->SetColor({ink.r, ink.g, ink.b, ink.a * km});
                const float tr = 4.2f * d;
                auto triangolo = [&](float ax, bool destra) {   // punta a destra o a sinistra
                    ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* sk = nullptr;
                    if (SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                        const float dx = destra ? tr : -tr;
                        sk->BeginFigure({ax - dx, cy - tr}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({ax + dx, cy}); sk->AddLine({ax - dx, cy + tr});
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                        if (SUCCEEDED(sk->Close())) dc->FillGeometry(g, pennello);
                    }
                    if (sk) sk->Release();
                    if (g) g->Release();
                };
                if (i == 1) {
                    if (br.suona) {   // pausa: due barre
                        dc->FillRoundedRectangle({{tc - 4 * d, cy - 5 * d, tc - 1.3f * d, cy + 5 * d}, 1 * d, 1 * d}, pennello);
                        dc->FillRoundedRectangle({{tc + 1.3f * d, cy - 5 * d, tc + 4 * d, cy + 5 * d}, 1 * d, 1 * d}, pennello);
                    } else triangolo(tc + 1 * d, true);
                } else {
                    const bool destra = i == 2;
                    triangolo(tc + (destra ? -2.5f : 2.5f) * d, destra);
                    triangolo(tc + (destra ? 2.5f : -2.5f) * d, destra);
                }
                piano();
            }
            dc->PopAxisAlignedClip();
            dc->PopAxisAlignedClip();
            x = x1 + largo * p;
            rettMus = p > 0.5f ? D2D1_RECT_F{x1 - 6 * d, r.top, x + 6 * d, r.bottom} : D2D1_RECT_F{};
            prima = std::min(1.0f, prima + p);
        } else rettMus = {};
        if (pTi > 0.001) {
            const float p = (float)std::clamp(pTi, 0.0, 1.0);
            x += 12 * d * p * prima;
            const float x1 = x, largo = larghezzaTimer();
            dc->PushAxisAlignedClip({x1, r.top, x1 + largo * p, r.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const D2D1_COLOR_F ink = {ink0.r, ink0.g, ink0.b, ink0.a * p};
            // anello che si svuota e il tempo che resta, in arancio come nel notch
            const float rr = 7 * d, ax = x + rr + 2 * d;
            dritto(ax, cy);   // (l'arco parte sempre dall'alto)
            pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.22f * k * p) : D2D1::ColorF(0, 0, 0, 0.16f * k * p));
            dc->DrawEllipse({{ax, cy}, rr, rr}, pennello, 2.2f * d);
            const float av = std::clamp((float)comodita::timerAvanzamento(), 0.0f, 1.0f);
            pennello->SetColor(D2D1::ColorF(1.0f, 0.62f, 0.04f, k * p));
            if (av >= 0.999f) dc->DrawEllipse({{ax, cy}, rr, rr}, pennello, 2.2f * d);
            else if (av > 0.001f) {
                ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* sk = nullptr;
                if (SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                    const float ang = av * 2 * (float)M_PI;
                    sk->BeginFigure({ax, cy - rr}, D2D1_FIGURE_BEGIN_HOLLOW);
                    sk->AddArc(D2D1::ArcSegment({ax + rr * std::sin(ang), cy - rr * std::cos(ang)}, {rr, rr}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                                av > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                    sk->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sk->Close())) dc->DrawGeometry(g, pennello, 2.2f * d);
                }
                if (sk) sk->Release();
                if (g) g->Release();
            }
            piano();
            // tempo che resta (scritto a grandezza 1 e scalato attorno al suo inizio)
            const std::wstring cifre = comodita::durata(comodita::timerResto());
            const float tw = cifreTimer();
            const float xt = ax + rr + 6 * d;
            // (dritto attorno al centro della scritta: ai lati sta in mezzo allo spessore del dock)
            const float tcx = xt + tw * sc / 2;
            dritto(tcx, cy, sc);
            dc->DrawText(cifre.c_str(), (UINT32)cifre.size(), fmtOra, {tcx - tw / 2, cy - 10 * dpi, tcx + tw / 2, cy + 10 * dpi}, pennello);
            piano();
            x = xt + tw * sc;
            // tasti (col mouse sopra): +1 min e stop, disegnati solo nello spazio gia' concesso
            const float kt = (float)std::clamp(kTim, 0.0, 1.0);
            tastoTesta[3] = {x + 6 * d, cy - 11 * d, x + 50 * d, cy + 11 * d};
            tastoTesta[4] = {x + 56 * d, cy - 12 * d, x + 80 * d, cy + 12 * d};
            if (kt > 0.02f) {
                dc->PushAxisAlignedClip({x, r.top, x + kt * (44 + 6 + 26 + 4) * d, r.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                const D2D1_RECT_F& rp = tastoTesta[3];
                const bool sopraP = mouseDentro && sulTasto(rp, mouseX, mouseY);
                dritto((rp.left + rp.right) / 2, cy);   // (pillola e scritta dritte anche ai lati)
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, (tastoPremuto == 3 ? 0.30f : sopraP ? 0.22f : 0.14f) * kt * k * p)
                                         : D2D1::ColorF(0, 0, 0, (tastoPremuto == 3 ? 0.18f : sopraP ? 0.13f : 0.08f) * kt * k * p));
                dc->FillRoundedRectangle({rp, 11 * d, 11 * d}, pennello);
                pennello->SetColor({ink.r, ink.g, ink.b, ink.a * kt});
                const std::wstring piu = testi::t(L"+1 min", L"+1 min");
                const float pcx = (rp.left + rp.right) / 2;
                dritto(pcx, cy, sc);
                dc->DrawText(piu.c_str(), (UINT32)piu.size(), fmtData, {pcx - 22 * dpi, cy - 11 * dpi, pcx + 22 * dpi, cy + 11 * dpi}, pennello);
                piano();
                const D2D1_RECT_F& rs = tastoTesta[4];
                const float scx = (rs.left + rs.right) / 2;
                pennello->SetColor(D2D1::ColorF(1.0f, 0.42f, 0.32f, (tastoPremuto == 4 ? 1.0f : 0.85f) * kt * k * p));
                dc->FillEllipse({{scx, cy}, 11 * d, 11 * d}, pennello);
                pennello->SetColor(D2D1::ColorF(1, 1, 1, kt * k * p));
                dc->FillRoundedRectangle({{scx - 3.5f * d, cy - 3.5f * d, scx + 3.5f * d, cy + 3.5f * d}, 1.2f * d, 1.2f * d}, pennello);
                dc->PopAxisAlignedClip();
            }
            dc->PopAxisAlignedClip();
            x = x1 + largo * p;
            rettTim = p > 0.5f ? D2D1_RECT_F{x1 - 6 * d, r.top, x + 6 * d, r.bottom} : D2D1_RECT_F{};
        } else rettTim = {};
        dc->PopAxisAlignedClip();
        if (t - tSveglio < 0.7 || t - tSilenzio < 0.7) anima = true;
        return anima;
    }

    // velo chiaro, bordo luminoso e sottile contorno scuro: stessa forma del vetro sotto
    // fondo di vetro (velo e bordi sopra il vetro acrilico); a: opacita' (quella del dock, impostazione; 1 i pannelli)
    // tinta: colore scelto per il dock (nullptr: chiaro o scuro col tema), piu' coprente perche' si veda
    void disegnaFondo(const D2D1_RECT_F& f, bool scuro, float a = 1, const D2D1_COLOR_F* tinta = nullptr) {
        pennello->SetColor(tinta ? D2D1::ColorF(tinta->r, tinta->g, tinta->b, 0.62f * a)
                                 : scuro ? D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.35f * a) : D2D1::ColorF(1, 1, 1, 0.38f * a));
        dc->FillRoundedRectangle({f, RAGGIO, RAGGIO}, pennello);
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.14f * a) : D2D1::ColorF(1, 1, 1, 0.75f * a));
        dc->DrawRoundedRectangle({{f.left + 0.5f, f.top + 0.5f, f.right - 0.5f, f.bottom - 0.5f}, std::max(0.0f, RAGGIO - 0.5f), std::max(0.0f, RAGGIO - 0.5f)}, pennello, 1.0f);
        pennello->SetColor(D2D1::ColorF(0, 0, 0, (scuro ? 0.45f : 0.10f) * a));
        dc->DrawRoundedRectangle({{f.left - 0.5f, f.top - 0.5f, f.right + 0.5f, f.bottom + 0.5f}, RAGGIO + 0.5f, RAGGIO + 0.5f}, pennello, 1.0f);
    }

    void disegnaEtichetta(bool scuro) {
        const float a = (float)etichettaAlfa;
        IDWriteTextLayout* lay = nullptr;
        if (FAILED(fdw->CreateTextLayout(etichettaTesto.c_str(), (UINT32)etichettaTesto.size(), testo, 800, 40, &lay))) return;
        DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm);
        float lw = tm.widthIncludingTrailingWhitespace + 22 * dpi;
        const float lh = 28 * dpi;
        // Disegnata dritta nelle coordinate della finestra, dal lato interno dell'icona: sopra (dock in basso), sotto
        // (in alto), a destra o a sinistra (ai lati, dove la finestra ha extraLato in piu' per lei). Dentro lo schermo
        // anche per gli elementi vicino al bordo
        const D2D1_POINT_2F p = fis(etichettaCx, etichettaTop);
        D2D1_RECT_F rr{};
        if (verticale()) {
            lw = std::min(lw, (float)extraLato() - 6 * dpi);   // (un titolo lunghissimo si taglia: lo spazio e' quello)
            const float cy = std::clamp(p.y, lh / 2 + 6 * dpi, finH() - lh / 2 - 6 * dpi);
            const float x0 = lato == 2 ? p.x : p.x - lw;
            rr = {x0, cy - lh / 2, x0 + lw, cy + lh / 2};
            lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        } else {
            const float cx = std::clamp(p.x, lw / 2 + 6 * dpi, W - lw / 2 - 6 * dpi), top = lato == 1 ? p.y : p.y - lh;
            rr = {cx - lw / 2, top, cx + lw / 2, top + lh};
        }
        rettEtichetta = rr;
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        pennello->SetColor(scuro ? D2D1::ColorF(0.18f, 0.18f, 0.2f, 0.92f * a) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.95f * a));
        dc->FillRoundedRectangle({rr, 7 * dpi * raggio::fattore(), 7 * dpi * raggio::fattore()}, pennello);
        pennello->SetColor(D2D1::ColorF(0, 0, 0, 0.15f * a));
        dc->DrawRoundedRectangle({rr, 7 * dpi * raggio::fattore(), 7 * dpi * raggio::fattore()}, pennello, 1.0f);
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, a) : D2D1::ColorF(0.1f, 0.1f, 0.1f, a));
        lay->SetMaxWidth(lw); lay->SetMaxHeight(lh);
        dc->DrawTextLayout({rr.left, rr.top}, lay, pennello);
        piano();
        lay->Release();
    }

    // ---- isola di stato ----
    float larghezzaTesto(const std::wstring& s, IDWriteTextFormat* f) {
        IDWriteTextLayout* lay = nullptr;
        if (FAILED(fdw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 1000, 100, &lay))) return 0;
        DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm); lay->Release();
        return tm.widthIncludingTrailingWhitespace;
    }

    // disegna l'isola a destra, alla stessa altezza del dock; restituisce true se sta animando
    bool disegnaIsola(bool scuro, float top, float bottom) {
        const float P = 6 * dpi, GAP_ISOLE = 8 * dpi;
        // larghezza della pillola dell'ora: si rimisura solo quando cambia il testo (prima a ogni fotogramma)
        if (testoOra != misuratoOra || testoData != misuratoData) {
            misuratoOra = testoOra; misuratoData = testoData;
            larghezzaOra = std::max(larghezzaTesto(testoOra, fmtOra), larghezzaTesto(testoData, fmtData));
        }
        const float largOra = larghezzaOra + 22 * dpi;
        const float largControllo = 3 * 30 * dpi;   // simbolo del centro di controllo, rete, volume
        const float destra = std::round(W - 12 * dpi);
        pillolaOra = {std::round(destra - 2 * P - largOra), top, destra, bottom};
        pillolaControllo = {std::round(pillolaOra.left - GAP_ISOLE - 2 * P - largControllo), top, std::round(pillolaOra.left - GAP_ISOLE), bottom};
        isola = {pillolaControllo.left, top, destra, bottom};
        rettStato[CONTROLLO] = {pillolaControllo.left + P, top + P, pillolaControllo.right - P, bottom - P};
        rettStato[ORA] = {pillolaOra.left + P, top + P, pillolaOra.right - P, bottom - P};
        disegnaFondo(pillolaControllo, scuro);
        disegnaFondo(pillolaOra, scuro);
        aggiornaVetro(vetroStato, rettVetroStato, pillolaControllo.left, top, pillolaControllo.right, bottom);
        aggiornaVetro(vetroOra, rettVetroOra, pillolaOra.left, top, pillolaOra.right, bottom);

        bool movimento = false;
        const D2D1_COLOR_F inchiostro = scuro ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.86f);
        for (int i = 0; i < N_STATO; i++) {
            const double e = verso(evidenza[i], (i == statoSopra && mouseDentro) ? 1.0 : 0.0, 0.05);
            if (e != evidenza[i]) movimento = true;
            evidenza[i] = e;
            const float s = (i == statoPremuto) ? 0.92f : 1.0f;   // si "schiaccia" un poco alla pressione, come su iOS
            const D2D1_RECT_F& r = rettStato[i];
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            if (e > 0.01) {
                const float hw = (r.right - r.left) / 2 * s, hh = (r.bottom - r.top) / 2 * s;
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f * (float)e) : D2D1::ColorF(1, 1, 1, 0.55f * (float)e));
                dc->FillRoundedRectangle({{cx - hw, cy - hh, cx + hw, cy + hh}, std::max(0.0f, RAGGIO - 2 * dpi), std::max(0.0f, RAGGIO - 2 * dpi)}, pennello);
            }
            dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {cx, cy}));
            pennello->SetColor(inchiostro);
            switch (i) {
                case CONTROLLO: {   // tre simboli in un solo pulsante: centro di controllo, rete, volume
                    const float passo = (r.right - r.left) / 3;
                    simboloControllo(r.left + passo * 0.5f, cy);
                    simboloRete(r.left + passo * 1.5f, cy, inchiostro);
                    pennello->SetColor(inchiostro);
                    simboloVolume(r.left + passo * 2.5f, cy);
                    break;
                }
                case ORA: {
                    const float h2 = 17 * dpi;
                    dc->DrawText(testoOra.c_str(), (UINT32)testoOra.size(), fmtOra, {r.left, cy - h2 - 1 * dpi, r.right, cy + 1 * dpi}, pennello);
                    pennello->SetColor({inchiostro.r, inchiostro.g, inchiostro.b, inchiostro.a * 0.72f});
                    dc->DrawText(testoData.c_str(), (UINT32)testoData.size(), fmtData, {r.left, cy + 1 * dpi, r.right, cy + h2 - 1 * dpi}, pennello);
                    break;
                }
            }
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        return movimento;
    }

    std::wstring descrizioneStato(int i) const {
        switch (i) {
            case CONTROLLO: {
                std::wstring s = rete == 0 ? testi::t(L"Nessuna connessione", L"No connection") : nomeRete;
                if (!audioOk) return s + testi::t(L" \u00b7 nessun audio", L" \u00b7 no audio");
                if (muto) return s + testi::t(L" \u00b7 audio disattivato", L" \u00b7 sound off");
                wchar_t b[32]; swprintf(b, 32, L" \u00b7 volume %d%%", (int)std::lround(volume * 100)); return s + b;
            }
            case ORA:      return testoDataLunga;
        }
        return L"";
    }

    // simboli disegnati a mano, tratti morbidi in stile SF Symbols (coordinate in px a 96 dpi, scalate)
    void linea(float x1, float y1, float x2, float y2, float cx, float cy, float sp = 1.8f) {
        dc->DrawLine({cx + x1 * dpi, cy + y1 * dpi}, {cx + x2 * dpi, cy + y2 * dpi}, pennello, sp * dpi, tratto);
    }

    void arco(float cx, float cy, float r, float a0, float a1, float sp = 1.8f) {
        ID2D1PathGeometry* g = nullptr;
        if (FAILED(f2d->CreatePathGeometry(&g))) return;
        ID2D1GeometrySink* s = nullptr;
        if (FAILED(g->Open(&s))) { g->Release(); return; }   // (R20261121-001416-S5)
        const float rr = r * dpi;
        s->BeginFigure({cx + rr * std::cos(a0), cy + rr * std::sin(a0)}, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc(D2D1::ArcSegment({cx + rr * std::cos(a1), cy + rr * std::sin(a1)}, {rr, rr}, 0,
                                   D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
        dc->DrawGeometry(g, pennello, sp * dpi, tratto);
        g->Release();
    }

    void simboloWidget(float cx, float cy) {
        const float l = 6.5f * dpi, g = 2.5f * dpi, r = 2.0f * dpi;
        for (int i = 0; i < 4; i++) {
            const float x = cx - l - g / 2 + (i % 2) * (l + g), y = cy - l - g / 2 + (i / 2) * (l + g);
            dc->FillRoundedRectangle({{x, y, x + l, y + l}, r, r}, pennello);
        }
    }

    // centro di controllo: due interruttori uno sopra l'altro (pallino a destra e a sinistra), come su macOS
    void simboloControllo(float cx, float cy) {
        const float w = 7.5f * dpi, h = 3.2f * dpi;
        for (int k = 0; k < 2; k++) {
            const float y = cy + (k ? 4.2f : -4.2f) * dpi;
            dc->DrawRoundedRectangle({{cx - w, y - h, cx + w, y + h}, h, h}, pennello, 1.4f * dpi);
            dc->FillEllipse({{cx + (k ? -w + h : w - h), y}, h - 0.8f * dpi, h - 0.8f * dpi}, pennello);
        }
    }

    void simboloFreccia(float cx, float cy) {
        linea(-5, 2.5f, 0, -2.5f, cx, cy, 2.0f);
        linea(0, -2.5f, 5, 2.5f, cx, cy, 2.0f);
    }

    void simboloRete(float cx, float cy, D2D1_COLOR_F inchiostro) {
        const float pi = (float)M_PI;
        if (rete == 2) {   // Wi-Fi: tre archi e un punto
            const float by = cy + 6 * dpi;
            dc->FillEllipse({{cx, by - 0.5f * dpi}, 1.6f * dpi, 1.6f * dpi}, pennello);
            for (float r : {5.5f, 9.5f, 13.5f}) arco(cx, by, r, -pi * 0.75f, -pi * 0.25f, 2.0f);
            return;
        }
        // cavo: "<···>" come su macOS; senza connessione sbiadito e barrato
        if (rete == 0) pennello->SetColor({inchiostro.r, inchiostro.g, inchiostro.b, inchiostro.a * 0.35f});
        linea(-5, -5, -9, 0, cx, cy); linea(-9, 0, -5, 5, cx, cy);
        linea(5, -5, 9, 0, cx, cy); linea(9, 0, 5, 5, cx, cy);
        for (float x : {-3.0f, 0.0f, 3.0f}) dc->FillEllipse({{cx + x * dpi, cy}, 1.2f * dpi, 1.2f * dpi}, pennello);
        if (rete == 0) { pennello->SetColor(inchiostro); linea(-8, 8, 8, -8, cx, cy, 2.0f); }
    }

    void simboloVolume(float cx, float cy) {
        const float x0 = cx - 4 * dpi;   // l'altoparlante sta a sinistra, le onde a destra
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            auto P = [&](float x, float y) { return D2D1::Point2F(x0 + x * dpi, cy + y * dpi); };
            s->BeginFigure(P(-6, -2.8f), D2D1_FIGURE_BEGIN_FILLED);
            D2D1_POINT_2F pt[] = {P(-3, -2.8f), P(1.5f, -6.5f), P(1.5f, 6.5f), P(-3, 2.8f), P(-6, 2.8f)};
            s->AddLines(pt, 5);
            s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
            dc->FillGeometry(g, pennello);
            dc->DrawGeometry(g, pennello, 1.0f * dpi, tratto);   // angoli ammorbiditi
        }
        if (g) g->Release();
        if (!audioOk || muto) {
            linea(6, -3, 11, 2, x0, cy); linea(11, -3, 6, 2, x0, cy);
            return;
        }
        const float a = (float)M_PI * 0.27f;
        const int onde = volume <= 0.001f ? 0 : volume < 0.34f ? 1 : volume < 0.67f ? 2 : 3;
        const float raggi[3] = {4.5f, 8.0f, 11.5f};
        for (int i = 0; i < onde; i++) arco(x0 + 1.5f * dpi, cy, raggi[i], -a, a);
    }

    // ora, volume e rete; ogni 500 ms (rete ogni 2 s). Ridisegna solo se qualcosa e' cambiato
    void aggiornaStato(bool tutto) {
        bool cambiato = false;
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t b[96];
        swprintf(b, 96, L"%02d:%02d", st.wHour, st.wMinute);
        if (testoOra != b) {
            testoOra = b; cambiato = true;
            const wchar_t* loc = testi::locale();
            if (GetDateFormatEx(loc, 0, &st, testi::t(L"ddd d MMM", L"ddd, MMM d"), b, 96, nullptr)) testoData = b;
            if (GetDateFormatEx(loc, 0, &st, testi::t(L"dddd d MMMM yyyy", L"dddd, MMMM d, yyyy"), b, 96, nullptr)) { testoDataLunga = b; if (!testoDataLunga.empty()) testoDataLunga[0] = (wchar_t)towupper(testoDataLunga[0]); }
            // con un calendario non gregoriano scelto in Windows la data del pannello (sopra il calendario) e' nel suo
            // (issue #24); ora, data breve del dock e notch restano quelle di sempre
            if (const calendario::Mese& cm = calendario::meseUtente(loc); cm.ok && !cm.dataLunga.empty()) testoDataLunga = cm.dataLunga;
            if (GetDateFormatEx(loc, 0, &st, testi::t(L"dddd d MMMM", L"dddd, MMMM d"), b, 96, nullptr)) { testoNotch = b; if (!testoNotch.empty()) testoNotch[0] = (wchar_t)towupper(testoNotch[0]); }
            notch.testi(testoOra, testoNotch);   // il notch mostra ora e data
        }
        const float v0 = volume; const bool m0 = muto, ok0 = audioOk;
        leggiVolume();
        cambiato |= (std::abs(v0 - volume) > 0.004f || m0 != muto || ok0 != audioOk);
        if (tutto || ++conteggioTimer % 4 == 0) {
            const int r0 = rete; leggiRete(); cambiato |= r0 != rete;
        }
        // testa del dock: musica e timer cambiano senza che il dock lo sappia (li legge il notch): si ridisegna qui
        if (testaCambiata()) dirty = true;
        usa(0);   // controllo, widget: la scheda 0
        if (cambiato) { dirty = true; if (pannelloTipo == P_CONTROLLO) pDirty = true; }
        if (pannelloTipo == P_CONTROLLO && !pChiusura) {   // l'uscita audio puo' cambiare (cuffie collegate)
            const std::wstring u0 = nomeUscita; leggiUscita(); if (u0 != nomeUscita) pDirty = true;
        }
        if (widD.pronto.exchange(false) && pannelloTipo == P_WIDGET) { wid = widD.copia(); pDirty = true; }
        // radio e luminosita' lette dal thread: il pannello si ridisegna (e si allunga se la luminosita' e' regolabile)
        if (sis.pronto.exchange(false) && pannelloTipo == P_CONTROLLO && !pChiusura) { riadattaControllo(); pDirty = true; }
    }

    IAudioEndpointVolume* volumeDispositivo() {
        if (!audio) return nullptr;
        IMMDevice* d = nullptr;
        if (FAILED(audio->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return nullptr;
        IAudioEndpointVolume* v = nullptr;
        d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, (void**)&v);
        d->Release();
        return v;
    }

    void leggiVolume() {
        IAudioEndpointVolume* v = volumeDispositivo();
        audioOk = v != nullptr;
        if (!v) return;
        float l = 0; BOOL m = FALSE;
        v->GetMasterVolumeLevelScalar(&l); v->GetMute(&m);
        volume = l; muto = m;
        v->Release();
    }

    void cambiaVolume(float delta) {
        IAudioEndpointVolume* v = volumeDispositivo();
        if (!v) return;
        float l = 0; v->GetMasterVolumeLevelScalar(&l);
        l = std::clamp(std::round((l + delta) * 50) / 50, 0.0f, 1.0f);   // passi del 2%
        v->SetMasterVolumeLevelScalar(l, nullptr);
        if (delta > 0) v->SetMute(FALSE, nullptr);
        v->Release();
        leggiVolume(); dirty = true;
    }

    // connessione attiva: una scheda accesa con un gateway (Wi-Fi o cavo)
    void leggiRete() {
        Cronometro cr(this, "leggiRete");
        ULONG n = 16 * 1024;
        std::vector<BYTE> buf(n);
        const ULONG fl = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG r = GetAdaptersAddresses(AF_UNSPEC, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &n);
        if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(n); r = GetAdaptersAddresses(AF_UNSPEC, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &n); }
        rete = 0; nomeRete.clear();
        if (r != NO_ERROR) return;
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            const int tipo = a->IfType == IF_TYPE_IEEE80211 ? 2 : 1;
            if (tipo > rete || rete == 0) { rete = tipo; nomeRete = a->FriendlyName ? a->FriendlyName : testi::t(L"Rete", L"Network"); }
            if (tipo == 1) break;   // col cavo collegato e' quella in uso
        }
    }

    void azioneStato(int i) {
        switch (i) {
            case CONTROLLO: apriPannello(P_CONTROLLO); break;   // pannelli nostri: quelli di Windows fanno risalire la barra
            case ORA:      apriCentro(P_NOTIFICHE); break;     // notifiche e calendario, come un clic sul notch
        }
    }

    // ---- pannelli in stile iOS (al posto di quelli di Windows, che si agganciano alla barra e la fanno risalire) ----
    enum { P_NESSUNO, P_SFONDO, P_CONTROLLO, P_NOTIFICHE, P_WIDGET, P_MIXER, P_APPUNTI, P_WIFI };
    // reti Wi-Fi (vista P_WIFI, wifi.h): ultima copia del thread, per il disegno
    wifi::Wifi wlan;
    std::vector<wifi::Rete> wlReti;
    int wlStato = wifi::S_AVVIO, wlEsito = wifi::E_NIENTE;
    std::string wlEsitoDi;
    bool wlCerca = false;
    double wlCercataT = -100;   // ultima scansione chiesta (la vista aperta ne chiede una ogni 10 s)
    double wifiK = -1;          // pallina dell'interruttore Wi-Fi della vista: 0 spento .. 1 acceso (animata)
    void copiaWifi() { wlReti = wlan.reti(wlStato, wlEsito, wlEsitoDi, wlCerca); }
    float altezzaWifi() const;
    int righeWifi() const;   // reti che entrano nella vista (stesso numero per altezza, disegno e zone)
    void disegnaWifi(bool scuro, D2D1_COLOR_F ink);
    void simboloSegnale(float cx, float cy, int livello, D2D1_COLOR_F c);
    void simboloLucchetto(float cx, float cy);
    widget::Dati widD;                   // meteo e notizie (scaricati in un thread a parte)
    widget::Istantanea wid;              // l'ultima copia, per il disegno
    jumplist::Indice jumpList;           // comandi delle app (tasto destro sulle icone), letti dai file di Windows
    IDWriteTextFormat* fmtNotizia = nullptr;
    enum Azione { A_APP, A_TUTTE, A_RETE, A_BLUETOOTH, A_VOLUME, A_MUTO, A_AUDIO, A_SCHERMO, A_ENERGIA, A_IMPOSTAZIONI,
                  A_NOTIFICA, A_RIMUOVI, A_CANCELLA, A_IMPOST_NOTIFICHE, A_TEMA, A_FLUIDWIN, A_LUMINOSITA,
                  A_WIDGET, A_SFONDO, A_NOTIZIA, A_NOTIFICHE, A_INDIETRO, A_SVEGLIO, A_SILENZIO, A_TIMER, A_MICROFONO,
                  A_MIXER_VOL, A_MIXER_MUTO, A_MIXER_PRED, A_ALTRE_USCITE,
                  A_APPUNTI, A_APPUNTI_VOCE, A_APPUNTI_FISSA, A_APPUNTI_TOGLI, A_APPUNTI_SVUOTA,
                  A_W_CITTA, A_W_LUOGO, A_W_UNITA, A_W_ANNULLA, A_W_FONTE,
                  A_WIFI_RETI, A_WIFI_RETE, A_WIFI_RADIO, A_WIFI_SCOLLEGA, A_WIFI_IMPOSTAZIONI, A_WIFI_PRIVACY };
    // Ricerca della citta' nella scheda del meteo (widget): aperta o no, avanzamento dell'animazione 0..1 (la scheda si
    // allunga e il contenuto cambia), testo scritto, testo gia' chiesto, istante dell'ultimo tasto (si cerca 0,3 s
    // dopo, non a ogni lettera), luoghi trovati con lo stato della ricerca, riga scelta con le frecce. Stanno fuori
    // dalle schede (scambiaScheda): il widget vive solo nella scheda 0
    bool wCitta = false;
    double wK = 0, wCambiato = 0, wUnitaPos = -1;
    std::wstring wTesto, wCercato;
    std::vector<widget::Luogo> wLuoghi;
    int wStato = widget::R_VUOTA, wSel = 0;
    int wRighe = 6;   // risultati che entrano nel pannello (schermi bassi: meno di 6; vedi disegnaWidget)
    void apriRicercaCitta();
    void chiudiRicercaCitta();
    void passoCitta();
    void cercaCitta();
    void carattereCitta(wchar_t c);
    void scegliLuogo(int i);
    void menuFonte(const D2D1_RECT_F& r);
    sistema::Sistema sis;                // Wi-Fi, Bluetooth e luminosita' (letti e cambiati in un thread a parte)
    double temaK = -1;                   // simbolo del tema: 0 sole (chiaro) .. 1 luna (scuro), animato
    bool trascinaLum = false;
    // notifiche (pannello dell'ora)
    notifiche::Lettore lettoreN;           // (sul thread principale: solo per togliere notifiche, che e' veloce)
    std::vector<notifiche::Notifica> listaN;
    bool permessoN = false;
    // Leggere le notifiche aspetta Windows ~1 s (GetNotificationsAsync, anche con poche notifiche): lo fa un thread a
    // parte. Il pannello si apre subito con l'ultimo elenco letto e si aggiorna quando arriva quello nuovo
    // (WM_NOTIFICHE_LETTE). Prima si leggeva al clic e tutto FluidWin restava fermo un secondo
    static const UINT WM_NOTIFICHE_LETTE = WM_APP + 32;
    CRITICAL_SECTION csN{}; bool csNOk = false;
    HANDLE eventoN = nullptr, threadNH = nullptr;
    std::atomic<LONG> fineN{0};             // (atomico: lo scrive il thread principale mentre il thread lo legge, R20261113-010114-S3)
    volatile LONG genN = 0;                 // genN: numero dell'ultima richiesta
    std::vector<notifiche::Notifica> lettaN; bool permessoLetto = false; LONG genLetta = 0;   // dal thread (sotto csN)
    LONG genValida = 0;                     // letture partite prima di questa richiesta: vecchie (notifiche tolte nel frattempo)
    bool letteN = false;                    // arrivata almeno una lettura (prima il pannello mostra "…")
    ULONGLONG chiestaN = 0;                 // ultima richiesta (il mouse sull'ora la anticipa, non a raffica)
    static DWORD WINAPI threadNotifiche(LPVOID p);
    void chiediNotifiche();
    void notificheLette();
    void fermaNotifiche();
    std::unordered_map<std::wstring, ID2D1Bitmap1*> iconeN;   // per AppUserModelID (nullptr = provata, nessuna icona)
    std::unordered_map<std::wstring, std::wstring> iconeNDaExe;   // eseguibile chiesto al posto dell'AppUserModelID -> AppUserModelID
    IDWriteTextFormat* fmtGrande = nullptr;
    struct AppSfondo { std::wstring nome, exe, avvio, chiaveReg; ID2D1Bitmap* icona = nullptr; };
    struct Elemento { D2D1_RECT_F r; Azione azione; int indice; };

    HWND pannello = nullptr, vetroPannello = nullptr;
    IDXGISwapChain1* scP = nullptr; IDCompositionTarget* targetP = nullptr; IDCompositionVisual* visualP = nullptr;
    IDCompositionEffectGroup* effettoP = nullptr; ID2D1Bitmap1* bersaglioP = nullptr;
    IDWriteTextFormat *fmtTitolo = nullptr, *fmtVoce = nullptr, *fmtSotto = nullptr, *fmtNome = nullptr;
    int pannelloTipo = P_NESSUNO, pannelloChiuso = P_NESSUNO;
    bool pDirty = false, pAnimazione = false, pChiusura = false, pMouseDentro = false, trascinaVolume = false;
    double pT0 = 0, pChiusoT = -10, pK = -1;
    // passaggio alle impostazioni e ritorno: -1 il contenuto svanisce sul posto (il vetro lo prende la finestra delle
    // impostazioni, che cresce da qui), 1 ricompare sul posto (le impostazioni ci sono appena rientrate), 0 niente
    int pDissolvi = 0; double pDissT0 = 0; float pOpContenuto = 1;
    float PW = 0, PH = 0, PWMAX = 0, PHMAX = 0;
    float phDisponibile = 0;             // spazio sopra l'isola all'ultima apertura (vedi altezzaMassimaP)
    RECT rettPannello{};                 // posizione finale sullo schermo
    std::vector<AppSfondo> appSfondo;
    std::vector<Elemento> elementi;      // zone cliccabili del pannello, ricalcolate a ogni disegno
    std::vector<double> evidP;           // evidenziazione (0..1) di ogni zona
    int pSopra = -1, pPremuto = -1;
    // ---- due schede, come carte: 0 = centro di controllo (anche widget e app in background), 1 = notifiche ----
    // I membri dei pannelli (pannello, PW, elementi...) sono quelli della scheda "corrente"; l'altra sta in ltra
    // e usa(slot) le scambia: cosi' tutto il codice dei pannelli lavora sempre sulla scheda giusta.
    double posDietro = 0, posObj = 0;    // 0 davanti, 1 dietro (posDietro va verso posObj)
    bool gira = false;                   // sta tornando davanti girando da sinistra (rimescolata)
    bool davanti = true;                 // sopra l'altra nell'ordine delle finestre
    double chiudiAlle = -1;              // chiusura ritardata: le due carte non si chiudono insieme
    struct Scheda {
        HWND pannello = nullptr, vetroPannello = nullptr;
        IDXGISwapChain1* scP = nullptr; IDCompositionTarget* targetP = nullptr; IDCompositionVisual* visualP = nullptr;
        IDCompositionEffectGroup* effettoP = nullptr; ID2D1Bitmap1* bersaglioP = nullptr;
        int pannelloTipo = 0, pannelloChiuso = 0;
        bool pDirty = false, pAnimazione = false, pChiusura = false, pMouseDentro = false, trascinaVolume = false, trascinaLum = false;
        double pT0 = 0, pChiusoT = -10, pK = -1;
        int pDissolvi = 0; double pDissT0 = 0; float pOpContenuto = 1;
        float PW = 0, PH = 0;
        RECT rettPannello{};
        std::vector<Elemento> elementi; std::vector<double> evidP;
        int pSopra = -1, pPremuto = -1;
        double posDietro = 0, posObj = 0; bool gira = false, davanti = true; double chiudiAlle = -1;
    } altra;
    int slotCorrente = 0;
    // ---- viste del centro di controllo, tutte nella stessa finestra ----
    // Il centro di controllo e' una finestra sola: "In background", "Widget" e "Notifiche" ci si aprono dentro. La vista
    // nuova nasce in un cerchio che si allarga dal punto toccato, mentre quella di prima si allontana (piu' piccola e
    // sfumata) e la finestra cambia altezza con una molla; tornando indietro ("<" nella testata) il cerchio si richiude
    // verso il pulsante da cui era nato e il centro di controllo torna avanti
    bool pRipreso = false;                // primo piano gia' ripreso una volta dopo l'apertura (vedi WM_ACTIVATE)
    bool pFuocoAvuto = false;             // il pannello ha avuto il primo piano almeno una volta da quando e' aperto
    double phVis = 0, phVel = 0;          // altezza che si vede: va verso PH (quella della vista) con una molla
    int vistaPrima = P_NESSUNO; float phPrima = 0; double tVista = -1; bool vistaAvanti = true;
    D2D1_POINT_2F origineVista{}, origineRitorno{};
    D2D1::Matrix3x2F baseVista = D2D1::Matrix3x2F::Identity();   // trasformazione della vista che si sta disegnando
    static constexpr float TESTA = 44;     // testata delle viste interne (punti): "<" e titolo
    static constexpr double DURATA_VISTA = 0.46;
    void scambiaScheda();
    void usa(int slot);
    static int slotDiTipo(int);
    int slotDiFinestra(HWND h) const;
    bool schedaFinestra(HWND h) const;
    bool nessunaScheda() const;
    std::wstring nomeUscita;             // dispositivo audio in uso
    std::unordered_map<std::wstring, ID2D1Bitmap*> cacheIcone;

    static double adesso();

    bool pannelloDaDisegnare() const;

    bool creaPannello(HINSTANCE inst);

    void apriPannello(int tipo);

    void preparaVista(int tipo);

    float altezzaVista(int tipo) const;

    void cambiaVista(int tipo, D2D1_POINT_2F origine);
    void tornaIndietro();
    bool inTransizione() const;

    void mescola(int slot);

    void ordinaSchede();

    void animaSchede();

    void chiudiTutte();

    void nascondiSchede();

    void chiudiPannello();

    void cediPannello();

    float altezzaControllo() const;

    bool controlloCompatto() const;
    int righeControllo() const;
    bool rigaLuminosita() const;
    bool rigaBatteria() const;
    // altezza del centro di controllo rifatta se le sue righe sono cambiate a pannello aperto
    void riadattaControllo() {
        const float nuova = std::min(altezzaControllo(), altezzaMassimaP());
        if (nuova != PH) {   // (col dock in alto resta fermo il bordo in alto, sotto il dock)
            PH = nuova;
            if (lato == 1) rettPannello.bottom = rettPannello.top + (int)PH; else rettPannello.top = rettPannello.bottom - (int)PH;
            posizionaPannello(pK < 0 ? 1 : pK);
        }
    }

    void nascondiPannello();

    void posizionaPannello(double k);

    bool renderPannello();

    void disegnaVista(int tipo, float h, bool scuro, D2D1_COLOR_F ink, const D2D1::Matrix3x2F& t);

    void disegnaTesta(int tipo, bool scuro, D2D1_COLOR_F ink);

    int aggiungi(const D2D1_RECT_F& r, Azione a, int indice);

    UINT menuScheda(const std::vector<menu::Voce>& v, POINT pt, HWND h);

    double evidenzia(int e, const D2D1_RECT_F& r, bool scuro, float raggio);

    void scrivi(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r);

    void chevronDestra(float x, float cy);

    void disegnaSfondo(bool scuro, D2D1_COLOR_F ink);

    // ---- notifiche e calendario (clic sull'ora) ----
    static constexpr float N_TESTA = 84, N_CAL = 176, N_TITOLO = 40, N_SCHEDA = 70, N_PIEDE = 48;
    float altezzaMassimaP() const;
    int schedeVisibili() const;
    float altezzaNotifiche() const;

    void ricaricaNotifiche();

    static std::wstring quandoN(INT64 t);

    void disegnaNotifiche(bool scuro, D2D1_COLOR_F ink);

    // riquadro con cerchio colorato, titolo e sottotitolo (rete, Bluetooth)
    // margine: spazio a destra dei testi (la piastrella Wi-Fi lo allarga per la freccia delle reti)
    void piastrella(const D2D1_RECT_F& r, Azione a, bool acceso, const std::wstring& titolo, const std::wstring& sotto,
                    bool scuro, D2D1_COLOR_F ink, float margine = 8) {
        const int e = aggiungi(r, a, 0);
        const float rp = std::max(6.0f, raggio::vetro() - 10.0f) * dpi;   // concentrica con l'angolo del pannello
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.05f));
        dc->FillRoundedRectangle({r, rp, rp}, pennello);
        evidenzia(e, r, scuro, rp);
        const float cx = r.left + 32 * dpi, cy = (r.top + r.bottom) / 2;
        pennello->SetColor(acceso ? D2D1::ColorF(0.0f, 0.478f, 1.0f, 1.0f) : (scuro ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.10f)));
        dc->FillEllipse({{cx, cy}, 18 * dpi, 18 * dpi}, pennello);
        const D2D1_COLOR_F simbolo = acceso ? D2D1::ColorF(1, 1, 1, 1) : ink;
        pennello->SetColor(simbolo);
        if (a == A_RETE) simboloRete(cx, cy, simbolo);
        else if (a == A_BLUETOOTH) simboloBluetooth(cx, cy);
        else if (a == A_TEMA) simboloTema(cx, cy);
        else if (a == A_SVEGLIO) simboloCaffe(cx, cy);
        else if (a == A_SILENZIO) simboloLuna(cx, cy);
        else if (a == A_TIMER) simboloTimer(cx, cy);
        else if (a == A_MICROFONO) simboloMicrofono(cx, cy, acceso);
        else simboloGoccia(cx, cy);
        pennello->SetColor(ink);
        scrivi(titolo, fmtVoce, {cx + 26 * dpi, cy - 18 * dpi, r.right - margine * dpi, cy});
        pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.62f});
        scrivi(sotto, fmtSotto, {cx + 26 * dpi, cy, r.right - margine * dpi, cy + 17 * dpi});
    }

    void disegnaControllo(bool scuro, D2D1_COLOR_F ink);

    // ---- widget: meteo e notizie ----
    void disegnaWidget(bool scuro, D2D1_COLOR_F ink);

    void simboloMeteo(int c, float cx, float cy, float s);

    void simboloBluetooth(float cx, float cy);

    void simboloTema(float cx, float cy);

    void simboloLuna(float cx, float cy);

    void simboloCaffe(float cx, float cy);

    void simboloTimer(float cx, float cy);

    void simboloMicrofono(float cx, float cy, bool acceso);
    UINT versioneSensoriCentro = 0, versioneTempoCentro = 0;   // per ridisegnare il riquadro del microfono quando cambia
    // ---- numeri delle notifiche, cestino, pila ----
    std::unordered_map<std::wstring, int> numeriNotifiche;   // AppUserModelID (minuscolo) -> quante
    bool cestinoPieno = false; int giroCestino = 0;
    int dropSopra = -1;                                      // icona sotto un trascinamento di file (il cestino)
    pila::Pila pilaDownload;
    int numeroDi(const Voce& v) const {
        if (numeriNotifiche.empty() || v.avvio.empty() || v.avvio[0] == L'*') return 0;
        const std::wstring a = minuscolo(v.aumid);
        int n = 0;
        for (const auto& [k, c] : numeriNotifiche) if ((!a.empty() && k == a) || (!v.exe.empty() && k == v.exe)) n += c;
        return n;
    }
    void apriPila(const Voce& v) {
        if (pilaDownload.aperta()) { pilaDownload.chiudi(); return; }
        const RECT r = aSchermo(v.rett);   // (sul lato del dock)
        pilaDownload.apri(r, cartellaDownload(), v.nome, lato);
    }
    // trascinamento di file sul dock: sul cestino si spostano nel Cestino di Windows (come trascinarli sull'icona del desktop)
    struct Bersaglio : IDropTarget {
        Motore* d = nullptr; bool file = false;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override { if (r == IID_IUnknown || r == IID_IDropTarget) { *p = this; return S_OK; } *p = nullptr; return E_NOINTERFACE; }
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* o, DWORD, POINTL pt, DWORD* e) override {
            FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            file = o && o->QueryGetData(&f) == S_OK;
            return DragOver(0, pt, e);
        }
        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* e) override {
            float x = 0, y = 0; d->logicoSchermo({pt.x, pt.y}, x, y);
            d->mouseDentro = true; d->mouseSulDock = true; d->mouseX = x; d->mouseY = y;   // la lente segue il trascinamento
            const int i = d->iconaInPunto(x, y);
            const int s = (file && i >= 0 && d->voci[i].avvio == AVVIO_CESTINO) ? i : -1;
            // (solo se la sorgente permette lo spostamento: *e porta gli effetti che consente)
            const int c = (*e & DROPEFFECT_MOVE) ? s : -1;
            if (c != d->dropSopra) d->dropSopra = c;
            d->dirty = true;
            *e = c >= 0 ? DROPEFFECT_MOVE : DROPEFFECT_NONE;   // (aspetto del cursore: "sposta" nel Cestino)
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragLeave() override { d->dropSopra = -1; d->mouseDentro = false; d->mouseSulDock = false; d->sopra = -1; d->dirty = true; return S_OK; }
        HRESULT STDMETHODCALLTYPE Drop(IDataObject* o, DWORD k, POINTL pt, DWORD* e) override {
            DragOver(k, pt, e);
            const bool cestino = d->dropSopra >= 0;
            DragLeave();
            if (!cestino || !o) { *e = DROPEFFECT_NONE; return S_OK; }
            FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM m{};
            if (FAILED(o->GetData(&f, &m))) { *e = DROPEFFECT_NONE; return S_OK; }
            std::wstring lista;   // percorsi separati da \0, con \0\0 in fondo (come vuole SHFileOperation)
            // DragQueryFile vuole l'handle del blocco, non il puntatore di GlobalLock: con memoria mobile i due sono
            // diversi e non si trovava nessun file (audit FW-AUDIT-016)
            // Ogni percorso intero, di qualsiasi lunghezza: uno non letto per intero annulla tutto. Prima un buffer fisso
            // poteva troncare un nome lungo o saltarlo, e il Cestino riceveva una lista diversa da quella trascinata
            // (R20261121-001416-S2)
            if (const HDROP hd = (HDROP)m.hGlobal) {
                const UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; i++) {
                    const UINT len = DragQueryFileW(hd, i, nullptr, 0);
                    std::wstring p(len + 1, L'\0');
                    if (!len || DragQueryFileW(hd, i, p.data(), len + 1) != len) { lista.clear(); break; }
                    p.resize(len);
                    lista += p; lista.push_back(L'\0');
                }
            }
            ReleaseStgMedium(&m);
            if (lista.empty()) { *e = DROPEFFECT_NONE; return S_OK; }
            lista.push_back(L'\0');
            if (d->registro) d->scrivi("file trascinati sul cestino: nel Cestino di Windows");
            // nel Cestino (FOF_ALLOWUNDO: si possono ripristinare), in un thread a parte per non fermare il dock
            auto* copia = new std::wstring(std::move(lista));
            const HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                std::wstring* l = (std::wstring*)p;
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = l->c_str(); op.fFlags = FOF_ALLOWUNDO;
                SHFileOperationW(&op);
                CoUninitialize(); delete l; return 0;
            }, copia, 0, nullptr);
            if (!t) { delete copia; if (d->registro) d->scrivi("cestino: thread non partito, file non toccati"); *e = DROPEFFECT_NONE; return S_OK; }
            CloseHandle(t);
            d->giroCestino = 0;
            // Spostamento "ottimizzato": i file li sposta FluidWin. Alla sorgente si risponde NONE e si scrive
            // CFSTR_PERFORMEDDROPEFFECT = NONE: con MOVE (com'era prima) Esplora file considera lo spostamento non fatto
            // e cancella lui gli originali, senza Cestino (documentazione della shell, "Handling Optimized Move
            // Operations"; audit FW-AUDIT-017)
            if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
                if (DWORD* v = (DWORD*)GlobalLock(g)) { *v = DROPEFFECT_NONE; GlobalUnlock(g); }
                FORMATETC pf = {(CLIPFORMAT)RegisterClipboardFormatW(L"Performed DropEffect"), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                STGMEDIUM sm{}; sm.tymed = TYMED_HGLOBAL; sm.hGlobal = g;
                if (FAILED(o->SetData(&pf, &sm, TRUE))) GlobalFree(g);   // (con TRUE, se riesce, la memoria e' sua)
            }
            *e = DROPEFFECT_NONE;
            return S_OK;
        }
    } bersaglioDrop;
    // mixer del volume (vista P_MIXER, mixer.h): indice delle righe = uscita * 1000 + app + 1 (0 = l'uscita)
    mixer::Mixer mix;
    int trascinaMixer = -1;
    int giroMixer = 0;
    float altezzaMixer() const;
    void disegnaMixer(bool scuro, D2D1_COLOR_F ink);
    void mixerDaMouse(float x);
    void rileggiMixer();
    void menuUscite(const D2D1_RECT_F& r);
    // cronologia degli appunti (vista P_APPUNTI, appunti.h): anteprime delle immagini per id della voce
    std::unordered_map<UINT, ID2D1Bitmap1*> immaginiAppunti;
    UINT versioneAppunti = 0;
    float altezzaAppunti() const;
    void disegnaAppunti(bool scuro, D2D1_COLOR_F ink);
    void simboloAppunti(float cx, float cy);

    void simboloGoccia(float cx, float cy);

    void simboloSole(float cx, float cy);

    void simboloEnergia(float cx, float cy);

    void simboloIngranaggio(float cx, float cy);

    void simboloCampana(float cx, float cy);

    // ---- dati dei pannelli ----
    static std::wstring risolviCartella(const std::wstring& p);

    ID2D1Bitmap* bitmapDaPng(const BYTE* d, DWORD n);

    // icona grande dall'eseguibile (dal thread delle icone: intanto nullptr, cioe' il segnaposto); se non si puo'
    // leggere (app dello Store), la piccola foto salvata da Windows (fotoIconaSfondo)
    std::unordered_map<std::wstring, std::wstring> chiaviSfondo;   // eseguibile -> sottochiave di NotifyIconSettings
    ID2D1Bitmap* iconaSfondo(const std::wstring& exe, HKEY, const wchar_t* sotto);

    ID2D1Bitmap* fotoIconaSfondo(const std::wstring& exe);

    static std::wstring avvioPacchetto(const std::wstring& exe);

    static std::wstring nomeDaTooltip(std::wstring t);

    // Le icone di notifica che Windows ricorda, solo di programmi in esecuzione adesso. Leggerle costa centinaia di
    // millisecondi (tutti i processi del sistema, il registro, l'elenco delle app dello Store): lo fa un thread a
    // parte, e intanto il pannello mostra l'elenco di prima
    bool lavoroSfondo = false;
    void leggiAppSfondo();

    float altezzaSfondo() const;

    static std::vector<AppSfondo> elencaAppSfondo();

    void leggiUscita();

    void impostaVolume(float l, bool togliMuto);

    void volumeDaMouse(float x);

    void luminositaDaMouse(float x);

public:
    void aggiornaTemaVetri();
private:

    static void apriPagina(const wchar_t* uri);

    void pannelloWindows();

    void apriAppSfondo(const AppSfondo& a);

    void eseguiPannello(const Elemento& e);

    int elementoIn(float x, float y) const;

    static LRESULT CALLBACK procPannello(HWND h, UINT m, WPARAM w, LPARAM l);

    int statoInPunto(float x, float y) const {
        for (int i = 0; i < N_STATO; i++) {
            const auto& r = rettStato[i];
            if (x >= r.left && x < r.right && y >= isola.top && y <= isola.bottom) return i;
        }
        return -1;
    }

    bool sullIsola(float x, float y) const { return isola.right > isola.left && x >= isola.left - 2 && x <= isola.right + 2 && y >= isola.top - 2; }

    // il vetro acrilico segue il fondo, con la stessa forma. Tagliato sullo schermo del dock: scendendo il fondo esce
    // dal bordo, e ai lati oltre il bordo c'e' spesso l'altro schermo (dock a destra del secondario, a sinistra del
    // principale): il vetro ci finiva sopra come una striscia staccata. Tutto fuori: nascosto, segnato con
    // VETRO_FUORI, e rimesso quando rientra (con WS_THICKFRAME Windows non lo farebbe piu' piccolo della misura minima
    // di una finestra). Solo quelli nascosti cosi' si rimostrano: vetroStato e vetroOra non sono mai visibili
    static constexpr const wchar_t* VETRO_FUORI = L"FluidWin.VetroFuori";
    void aggiornaVetro(HWND h, RECT& ultimo, float l, float t, float r, float b) {
        const RECT pieno = aSchermo({l, t, r, b});   // (sul lato del dock)
        RECT nuovo{};
        // (sotto i 16 px Windows lo allarga alla sua misura minima, 14 px misurati, verso l'altro schermo: nascosto)
        if (!IntersectRect(&nuovo, &pieno, &schermo) || nuovo.right - nuovo.left < 16 || nuovo.bottom - nuovo.top < 16) {
            ultimo = {};
            if (IsWindowVisible(h)) {
                SetPropW(h, VETRO_FUORI, (HANDLE)1);
                SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
            }
            return;
        }
        const bool rimetti = GetPropW(h, VETRO_FUORI) && attivo && !nascostoSchermoIntero;
        if (!EqualRect(&nuovo, &ultimo) || rimetti) {
            ultimo = nuovo;
            SetWindowPos(h, icone, nuovo.left, nuovo.top, nuovo.right - nuovo.left, nuovo.bottom - nuovo.top,
                         SWP_NOACTIVATE | SWP_NOREDRAW | (rimetti ? SWP_SHOWWINDOW : 0));
            if (rimetti) RemovePropW(h, VETRO_FUORI);
            vetroDaRiposizionare = true;   // al prossimo timer si rimette in ordine anche la finestra delle icone
        }
    }

    void posizionaVetri() {
        vetroDaRiposizionare = false;
        SetWindowPos(icone, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        for (HWND v : {vetro, vetroStato, vetroOra}) SetWindowPos(v, icone, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // ---- launcher: tutte le app, con la ricerca (tasto Win o icona "App" del dock) ----
    struct AppL { std::wstring nome, avvio, minuscolo; ID2D1Bitmap1* icona = nullptr; bool provata = false; };
    HWND launcher = nullptr, vetroLauncher = nullptr;
    IDXGISwapChain1* scL = nullptr; IDCompositionTarget* targetL = nullptr; IDCompositionVisual* visualL = nullptr;
    IDCompositionEffectGroup* effettoL = nullptr; ID2D1Bitmap1* bersaglioL = nullptr;
    float LW = 0, LH = 0, LEXTRA = 0;      // misure del launcher; sotto, lo spazio fino all'icona da cui esce
    float LHMAX = 0;                       // altezza della sua superficie (LH si adatta allo schermo a ogni apertura)
    HWND primaDelLauncher = nullptr;       // finestra in primo piano prima dell'apertura
    bool lRipreso = false;                 // primo piano gia' ripreso una volta dopo l'apertura (vedi WM_ACTIVATE)
    RECT rettLauncher{};                   // il launcher vero e proprio (la finestra arriva piu' in basso, fino all'icona)
    POINT ancora{};                        // icona "App": da qui cresce e qui rientra (coordinate della finestra)
    std::vector<AppL> tutteApp;
    std::vector<int> filtrate;
    std::wstring query;
    bool lAperto = false, lChiusura = false, lDirty = false, lAnim = false, lRegione = false, lMouseDentro = false;
    double lScala = 0.15, lVel = 0, lAlfa = 0, lT0 = 0, lChiusoT = -10, lLetto = -1e9;
    double lScroll = 0, lScrollObj = 0;
    // barra di scorrimento: si trascina (la si prende dove si vuole), un clic sul binario salta di una pagina
    bool lTrascinoBarra = false, lSopraBarra = false;
    float lPresaY = 0; double lPresaScroll = 0;
    int lSel = 0, lSopra = -1;
    bool lSopraEnergia = false;
    // File e cartelle nel launcher (indice.h; issue #14: "la ricerca mostra solo le app"). Mentre si scrive, 0,2 s dopo
    // l'ultimo tasto, un thread chiede all'indice di Windows; sotto la griglia compare la sezione con i risultati del
    // testo lFileTesto e, in fondo, "Cerca in Esplora file". lFileGen scarta i risultati di un testo vecchio; lFileH e'
    // l'altezza della sezione, animata (la griglia si accorcia); lSelFile la riga scelta con le frecce (-1: nella
    // griglia; lFileRighe() = la riga di Esplora file); iconeFile per percorso (lato 48 nel thread delle icone)
    std::vector<indice::File> lFile;
    std::wstring lFileTesto;
    unsigned lFileGen = 0;
    bool lFileInCorso = false, lFileAttesa = false;   // una ricerca nell'indice alla volta (vedi cercaFileLauncher)
    double lFileH = 0;
    int lSelFile = -1, lSopraFile = -1;
    std::unordered_map<std::wstring, ID2D1Bitmap1*> iconeFile;
    static constexpr float LF_TESTA = 32, LF_RIGA = 44, LF_ESPLORA = 40;
    static constexpr int LF_MAX = 6;
    int lFileRighe() const;
    float lFileAlto() const;
    int lFileIn(float x, float y) const;
    void cercaFileLauncher();
    void apriFile(int k);
    void menuFile(int k, POINT pt);
    bool disegnaFileLauncher(bool scuro, D2D1_COLOR_F ink);
    double lFileOp = 1;   // opacita' delle righe dei file: 1 se sono del testo scritto, 0,45 se di uno vecchio (animata)
    bool lSelTastiera = false;   // l'app scelta l'ha scelta la tastiera (si tiene in vista); rotella, barra e testo nuovo no
    IContextMenu2* menuShell2 = nullptr; IContextMenu3* menuShell3 = nullptr;   // durante il menu di Windows di un'app
    // Icone: le prepara un thread a parte (ognuna costa decine di ms), il disegno non le aspetta mai: intanto si
    // vede un segnaposto. Le chiedono il dock (256 px), le app in background (128), il launcher (96) e le notifiche
    // (64); la richiesta e' (percorso, lato). Quando una e' pronta il thread avvisa la finestra del dock, e
    // raccogliIcone la consegna a chi la aspetta
    static const UINT WM_ICONA_PRONTA = WM_APP + 30;
    struct IconaPronta { std::wstring percorso; int lato = 0, w = 0, h = 0; std::vector<UINT> px; bool ok = false; };
    CRITICAL_SECTION csIcone{}; bool csIconeOk = false;
    std::vector<IconaPronta> iconePronte;
    std::deque<std::pair<std::wstring, int>> iconeDaFare;
    std::unordered_set<std::wstring> iconeChieste;   // lato + percorso: gia' in coda (non si chiede due volte)
    HANDLE eventoIcone = nullptr, threadIconeH = nullptr;
    std::atomic<LONG> fineIcone{0};   // (atomico come fineN, R20261113-010114-S3)
    int iconeAttese = 0;

    static DWORD WINAPI threadIcone(LPVOID p);

    static std::wstring chiaveIcona(const std::wstring& percorso, int lato);

    // Lavori lenti (elenchi della shell, tutti i processi del sistema) in un thread a parte. 'fai' gira nel thread e
    // non deve toccare il Motore; 'applica' gira poi nel thread principale, con un messaggio alla finestra del dock
    static const UINT WM_LAVORO_FATTO = WM_APP + 31;
    struct Sottofondo { std::function<void()> fai, applica; HWND dest; };
    static DWORD WINAPI threadSottofondo(LPVOID p);
    void inSottofondo(std::function<void()> fai, std::function<void()> applica);

    void chiediIcona(const std::wstring& percorso, int lato);

    void fermaIcone();

    static std::wstring sorgenteIcona(const Voce& v);

    bool raccogliIcone();

    bool launcherDaDisegnare() const;

    bool creaLauncher(HINSTANCE inst);

    static bool appUtile(const std::wstring& nome, const std::wstring& par);

    // L'elenco di tutte le app (cartella App della shell) costa centinaia di millisecondi: lo legge un thread a parte.
    // subito = adesso, nel thread principale (solo la prima volta che serve e non c'e' ancora niente da mostrare)
    bool lavoroTutte = false;
    void leggiTutteApp(bool subito = false);

    void applicaTutteApp(std::vector<AppL> nuove);

    static std::vector<AppL> elencaTutteApp();

    static bool iniziali(const std::wstring& nome, const std::wstring& q);
    static bool sottosequenza(const std::wstring& nome, const std::wstring& q);
    void filtra();

    float lColonne() const;
    float lCellaW() const;
    float lCellaH() const;
    D2D1_RECT_F lRettBarra() const;
    D2D1_RECT_F lCursoreBarra() const;
    float lGrigliaTop() const;
    float lGrigliaBottom() const;
    double lScrollMax() const;
    D2D1_RECT_F lCella(int i) const;
    int lCellaIn(float x, float y) const;

    static void prendiFuoco(HWND w);

public:
    void volumeTasto(int verso, float& livello, bool& silenzio);

    std::vector<std::pair<std::wstring, std::wstring>> elencoApp();

    D2D1_RECT_F rettCentro() const;
    RECT rettIsolaSchermo() const;
    bool isolaVisibile() const;

    void apriPannelloIsola(int tipo);

    void apriCentro(int tipo);

    // apertura in attesa che l'altro (launcher o centro di controllo) abbia finito di chiudersi: 1 launcher, 2 centro
    int dopo = 0, dopoTipo = 0; double dopoT0 = 0;
    void apriDopo(int cosa, int tipo);
    void suTimerDopo();

    // apre le impostazioni di FluidWin (dal centro di controllo): da = rettangolo del vetro del pannello sullo schermo,
    // da cui le impostazioni crescono
    void (*impostazioni)(const RECT* da) = nullptr;
    void (*esci)() = nullptr;   // "Esci da FluidWin" (issue #16): l'uscita normale, che rimette Windows com'era
    // le impostazioni aperte dal centro di controllo si sono richiuse nel suo rettangolo: il centro ricompare li'
    void riapriControllo();
    // impostazioni aperte dal centro di controllo: il dock resta su come col pannello aperto (scende quando si chiudono)
    bool dockSu = false;
    void tieniSu(bool b) { if (dockSu != b) { dockSu = b; dirty = true; } }
    // un genio verso il dock (o fuori dal dock) e' in corso: su ancora per 0,25 s (lo si chiama a ogni fotogramma del
    // genio, quindi scende 0,25 s dopo che la finestra e' entrata)
    void tieniPerGenio() {
        const double fino = adesso() + 0.25;
        if (fino > rivelaFino) { const bool prima = adesso() <= rivelaFino; rivelaFino = fino; if (!prima) { dirty = true; posizionaVetri(); } }
    }

    void apriLauncher();

private:
    void chiudiLauncher();

    void nascondiLauncher();

    void posizionaLauncher();

    void avviaApp(int i);

    D2D1_RECT_F rettEnergia() const;

    void menuApp(int i, POINT pt);

    void aggiungiAlDock(const std::wstring& nome, const std::wstring& exe, const std::wstring& id, const std::wstring& avvio);

    void menuEnergia(POINT pt);

    static void energia(int s);

    void selezionaVisibile();

    void disegnaIconaApp(const D2D1_RECT_F& r, float alfa, bool scuro);

    // l'icona del centro di controllo: la stessa piastrella di vetro di "App", con i due interruttori di iOS (quello in
    // alto acceso, azzurro; quello in basso spento)
    // cestino: stesso fondo dell'icona del centro di controllo, secchio con coperchio (pieno o vuoto, uguale:
    // l'utente non voleva i fogli che spuntavano).
    // Con dei file trascinati sopra il coperchio si alza un poco
    void disegnaIconaCestino(const D2D1_RECT_F& r, float alfa, bool scuro, bool sopraDrop) {
        const float d = r.right - r.left;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.16f * alfa) : D2D1::ColorF(1, 1, 1, 0.82f * alfa));
        dc->FillRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello);
        pennello->SetColor(D2D1::ColorF(0, 0, 0, (scuro ? 0.3f : 0.08f) * alfa));
        dc->DrawRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello, 1.0f);
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        const D2D1_COLOR_F ink = scuro ? D2D1::ColorF(1, 1, 1, 0.9f * alfa) : D2D1::ColorF(0.25f, 0.25f, 0.3f, alfa);
        const float bw = d * 0.20f, top = cy - d * 0.12f, bot = cy + d * 0.27f, sp = std::max(1.2f, d * 0.035f);
        pennello->SetColor(ink);
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {   // secchio un poco piu' stretto in basso
            s->BeginFigure({cx - bw, top}, D2D1_FIGURE_BEGIN_HOLLOW);
            const D2D1_POINT_2F p[] = {{cx - bw * 0.8f, bot}, {cx + bw * 0.8f, bot}, {cx + bw, top}};
            s->AddLines(p, 3); s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
            dc->DrawGeometry(g, pennello, sp * 1.4f, tratto);
        }
        if (g) g->Release();
        for (int k = -1; k <= 1; k++) dc->DrawLine({cx + k * bw * 0.42f, top + d * 0.07f}, {cx + k * bw * 0.34f, bot - d * 0.06f}, pennello, sp, tratto);
        const float alza = sopraDrop ? d * 0.07f : 0;
        D2D1_MATRIX_3X2_F prima{}; dc->GetTransform(&prima);   // (quella dell'icona dritta: il coperchio si gira dentro di lei)
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(sopraDrop ? -14.0f : 0.0f, {cx - bw * 1.2f, top - d * 0.04f}) * D2D1::Matrix3x2F::Translation(0, -alza) * prima);
        dc->DrawLine({cx - bw * 1.2f, top - d * 0.04f}, {cx + bw * 1.2f, top - d * 0.04f}, pennello, sp * 1.5f, tratto);
        dc->DrawLine({cx - bw * 0.3f, top - d * 0.10f}, {cx + bw * 0.3f, top - d * 0.10f}, pennello, sp * 1.5f, tratto);
        dc->SetTransform(prima);
    }

    void disegnaIconaControllo(const D2D1_RECT_F& r, float alfa, bool scuro) {
        const float d = r.right - r.left;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.16f * alfa) : D2D1::ColorF(1, 1, 1, 0.82f * alfa));
        dc->FillRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello);
        pennello->SetColor(D2D1::ColorF(0, 0, 0, (scuro ? 0.3f : 0.08f) * alfa));
        dc->DrawRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello, 1.0f);
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        const float lw = d * 0.27f, lh = d * 0.115f, rk = lh * 0.78f;   // mezza larghezza, mezza altezza, pomello
        for (int k = 0; k < 2; k++) {
            const float y = cy + (k == 0 ? -1 : 1) * d * 0.15f;
            const D2D1_RECT_F p = {cx - lw, y - lh, cx + lw, y + lh};
            if (k == 0) pennello->SetColor(D2D1::ColorF(0.0f, 0.478f, 1.0f, alfa));
            else pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.28f * alfa) : D2D1::ColorF(0, 0, 0, 0.16f * alfa));
            dc->FillRoundedRectangle({p, lh, lh}, pennello);
            pennello->SetColor(D2D1::ColorF(1, 1, 1, alfa));
            dc->FillEllipse({{k == 0 ? p.right - lh : p.left + lh, y}, rk, rk}, pennello);
        }
    }

    bool renderLauncher();

    static LRESULT CALLBACK procLauncher(HWND h, UINT m, WPARAM w, LPARAM l);

    // ---- mouse ----
    // il mouse e' sull'icona App del dock?
    bool mouseSuIconaApp() const {
        POINT pt; GetCursorPos(&pt);
        float x = 0, y = 0; logicoSchermo(pt, x, y);
        const int i = iconaInPunto(x, y);
        return i >= 0 && voci[i].avvio == AVVIO_LAUNCHER;
    }
    int iconaInPunto(float x, float y) const {
        for (size_t i = 0; i < voci.size(); i++) {
            if (voci[i].uscente) continue;
            const auto& r = voci[i].rett;
            if (x >= r.left - GAP / 2 && x < r.right + GAP / 2 && y >= r.top - 4 && y <= r.bottom + PAD + 4) return (int)i;
        }
        return -1;
    }

    // "e' davanti" come la vede l'utente, non solo per Windows: il primo piano a volte e' un popup della stessa app
    // (menu, suggerimenti), il desktop o una finestra nostra, e la finestra dell'app e' comunque quella in cima
    bool appInPrimoPiano(const Voce& v, HWND& quale) {
        HWND fg = GetForegroundWindow();
        for (HWND c : {fg, fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr})
            if (c && std::find(v.finestre.begin(), v.finestre.end(), c) != v.finestre.end()) { quale = c; return !IsIconic(c); }
        // primo piano "di nessuno" (desktop, barra, FluidWin) o popup dello stesso processo dell'app:
        // conta la prima finestra vera dall'alto
        wchar_t c[64] = L""; if (fg) GetClassNameW(fg, c, 64);
        bool diNessuno = !fg || !IsWindowVisible(fg) || !wcsncmp(c, L"FluidWin.", 9) || !wcscmp(c, L"Progman") ||
                         !wcscmp(c, L"WorkerW") || !wcscmp(c, L"Shell_TrayWnd");
        DWORD pidFg = 0; if (fg) GetWindowThreadProcessId(fg, &pidFg);
        for (HWND h : v.finestre) { DWORD p = 0; GetWindowThreadProcessId(h, &p); if (pidFg && p == pidFg) diNessuno = true; }
        if (registro) { char b[160]; snprintf(b, 160, "clic dock: primo piano %ls non e' dell'app%s", c, diNessuno ? " (di nessuno)" : ""); scrivi(b); }
        if (!diNessuno) return false;
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (!IsWindowVisible(h) || IsIconic(h)) continue;
            const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            // anche le finestre "ponte" dei popup WinUI: risultano visibili ma non si attivano e sono vuote
            if (ex & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE)) continue;
            RECT r; if (!GetWindowRect(h, &r) || r.right - r.left < 2 || r.bottom - r.top < 2) continue;
            BOOL nascosta = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &nascosta, sizeof(nascosta));
            if (nascosta) continue;
            wchar_t k[64] = L""; GetClassNameW(h, k, 64);
            if (!wcsncmp(k, L"FluidWin.", 9) || !wcscmp(k, L"Progman") || !wcscmp(k, L"WorkerW")) continue;
            // una finestra di dialogo dell'app conta come l'app
            HWND radice = GetAncestor(h, GA_ROOTOWNER);
            for (HWND x : {h, radice})
                if (std::find(v.finestre.begin(), v.finestre.end(), x) != v.finestre.end()) { quale = x; return true; }
            if (registro) { char b[120]; snprintf(b, 120, "clic dock: in cima c'e' %ls", k); scrivi(b); }
            return false;
        }
        return false;
    }

    void clic(int i) {
        if (i < 0 || i >= (int)voci.size() || voci[i].uscente) return;
        Voce& v = voci[i];
        if (v.avvio == AVVIO_LAUNCHER) { apriLauncher(); return; }
        if (v.avvio == AVVIO_CONTROLLO) { apriCentro(P_CONTROLLO); return; }
        if (v.avvio == AVVIO_PILA) { apriPila(v); return; }
        if (v.avvio == AVVIO_CESTINO) { ShellExecuteW(nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL); return; }
        if (v.finestre.empty()) {
            segnaLancio(v);   // la sua prima finestra uscira' dall'icona col genio
            ShellExecuteW(nullptr, L"open", daAprire(v).c_str(), argomentiDaAprire(v), nullptr, SW_SHOWNORMAL);
            v.rimbalzoT0 = ultimoT;   // rimbalza finche' l'app non si apre
            appDaAggiornare = true;
            return;
        }
        // app gia' in primo piano: un altro clic la riduce a icona (col genio, come col pulsante "_")
        HWND fg = nullptr;
        if (appInPrimoPiano(v, fg)) {
            if (!riduci || !riduci(fg)) PostMessageW(fg, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            return;
        }
        // altrimenti: se e' ridotta a icona la ripristina, se no la porta in primo piano
        HWND scelta = nullptr;
        for (HWND h : v.finestre) if (IsIconic(h)) { scelta = h; break; }
        // Le app avviate come amministratore (Gestione attivita', o qualsiasi app con "Esegui come amministratore"):
        // Windows non lascia a FluidWin ripristinarle con ShowWindow (errore 203, protezione UIPI) e il clic non faceva
        // niente. SwitchToThisWindow, quello di Alt+Tab e della barra di Windows, invece funziona
        if (scelta) { portaDavanti(scelta); return; }
        // la finestra piu' in alto fra quelle dell'app
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
            if (std::find(v.finestre.begin(), v.finestre.end(), h) != v.finestre.end()) { scelta = h; break; }
        if (scelta) portaDavanti(scelta);
    }

    // porta in primo piano una finestra dell'app, ripristinandola se e' ridotta a icona
    // (ripristinaAltrui: un'app bloccata non ferma il dock; il ripiego e' lo stesso delle app amministratore)
    void portaDavanti(HWND h) {
        if (IsIconic(h)) {
            if (!ripristinaAltrui(h) && IsIconic(h)) {
                SwitchToThisWindow(h, TRUE);
                if (registro) scrivi("clic dock: finestra che non risponde o di un'app amministratore, ripristinata come fa Alt+Tab");
            } else SetForegroundWindow(h);
        } else if (!SetForegroundWindow(h)) SwitchToThisWindow(h, TRUE);
    }

    // Clic sulla musica (notch o testa del dock): l'app che sta riproducendo in primo piano. Windows la indica con un
    // AppUserModelID: quello vero per le app di pacchetto e per chi lo dichiara (Firefox), altrimenti il nome dell'exe
    // ("Spotify.exe") o un nome breve ("Chrome", "MSEdge"). Si cerca in ordine: le icone del dock, poi le finestre
    // aperte; senza finestre (app chiusa nell'area di notifica) la si riapre come farebbe Windows. Dei browser si porta
    // davanti la finestra, non la scheda che suona: Windows non dice quale sia
public:
    void apriAppMusica(const std::wstring& aumidMusica) {
        if (aumidMusica.empty()) return;
        const std::wstring a = minuscolo(aumidMusica);
        std::wstring aSenzaExe = a;
        if (aSenzaExe.size() > 4 && aSenzaExe.compare(aSenzaExe.size() - 4, 4, L".exe") == 0) aSenzaExe.resize(aSenzaExe.size() - 4);
        auto nomeFile = [](const std::wstring& exe) { const size_t s = exe.find_last_of(L"\\/"); return s == std::wstring::npos ? exe : exe.substr(s + 1); };
        auto corrisponde = [&](const std::wstring& aumidFin, const std::wstring& exe) {
            if (!aumidFin.empty() && minuscolo(aumidFin) == a) return true;
            if (exe.empty()) return false;
            std::wstring n = nomeFile(exe);   // (exe gia' minuscolo)
            if (n == a) return true;
            if (n.size() > 4) n.resize(n.size() - 4);
            return n == aSenzaExe;
        };
        // 1) le icone del dock con finestre
        for (Voce& v : voci) {
            if (v.uscente || v.finestre.empty() || !corrisponde(v.aumid, v.exe)) continue;
            HWND scelta = nullptr;
            for (HWND h = GetTopWindow(nullptr); h && !scelta; h = GetWindow(h, GW_HWNDNEXT))
                if (std::find(v.finestre.begin(), v.finestre.end(), h) != v.finestre.end()) scelta = h;
            // nessuna delle sue finestre esiste ancora (chiusa appena prima del clic, l'evento non e' ancora arrivato):
            // si passa alle altre strade invece di chiamare un HWND scaduto (R20261121-140048-S8)
            if (!scelta) continue;
            if (registro) scrivi("musica: clic, app nel dock in primo piano");
            portaDavanti(scelta);
            return;
        }
        // 2) le finestre aperte (anche di app nascoste dal dock), dalla piu' in alto
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || GetWindowTextLengthW(h) == 0) continue;
            BOOL nascosta = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &nascosta, sizeof(nascosta));
            if (nascosta && !IsIconic(h)) continue;
            if (!corrisponde(aumidFinestra(h), exeProcesso(h))) continue;
            if (registro) scrivi("musica: clic, finestra aperta in primo piano");
            portaDavanti(h);
            return;
        }
        // 3) nessuna finestra: icona fissata, app di pacchetto, o l'exe del processo che suona (le app che vivono
        // nell'area di notifica, come Spotify, al secondo avvio mostrano la finestra di quello gia' aperto)
        for (int i = 0; i < (int)voci.size(); i++)
            if (!voci[i].uscente && voci[i].finestre.empty() && corrisponde(voci[i].aumid, voci[i].exe)) { if (registro) scrivi("musica: clic, app fissata riaperta"); clic(i); return; }
        if (a.find(L'!') != std::wstring::npos) {
            if (registro) scrivi("musica: clic, app di pacchetto riaperta");
            ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + aumidMusica).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        const std::wstring exe = exeInEsecuzione(a.size() > 4 && a.compare(a.size() - 4, 4, L".exe") == 0 ? a : a + L".exe");
        if (!exe.empty()) {
            if (registro) scrivi("musica: clic, app senza finestre riaperta dal suo exe");
            ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (registro) scrivi("musica: clic, app che suona non trovata");
    }
private:
    // percorso completo di un processo in esecuzione con questo nome di file (minuscolo), vuoto se non c'e'. Se ci sono
    // processi con lo stesso nome in cartelle diverse non si sa quale suoni: vuoto anche allora, per non avviare un
    // programma diverso dal lettore (R20261121-140048-S2)
    static std::wstring exeInEsecuzione(const std::wstring& nome) {
        std::wstring r;
        const HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (s == INVALID_HANDLE_VALUE) return r;
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        bool ambiguo = false;
        for (BOOL ok = Process32FirstW(s, &pe); ok && !ambiguo; ok = Process32NextW(s, &pe)) {
            if (minuscolo(pe.szExeFile) != nome) continue;
            // (un omonimo di cui non si legge il percorso, per esempio di un amministratore, potrebbe essere proprio il
            // lettore: anche quello rende la scelta incerta, R20261121-141020 conferma di S2)
            bool letto = false;
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                std::wstring buf(32768, L'\0'); DWORD n = (DWORD)buf.size();
                if (QueryFullProcessImageNameW(p, 0, buf.data(), &n)) {
                    buf.resize(n); letto = true;
                    if (r.empty()) r = std::move(buf);
                    else if (minuscolo(buf) != minuscolo(r)) ambiguo = true;
                }
                CloseHandle(p);
            }
            if (!letto) ambiguo = true;
        }
        CloseHandle(s);
        return ambiguo ? std::wstring() : r;
    }

    // ---- anteprime delle finestre sopra le icone (anteprime.h; issue #24) ----
    // Mouse fermo 0,5 s su un'icona con finestre aperte: le anteprime si aprono; spostandosi su un'altra icona con
    // finestre passano a quella (0,15 s); uscendo da icona e anteprime si chiudono dopo 0,3 s. Niente anteprime con un
    // tasto premuto, un menu aperto, il launcher o il centro di controllo aperti. Il controllo gira solo mentre serve
    anteprime::Anteprime ant;
    bool anteprimeAccese = true, antTimer = false;
    int antCandidata = -1;
    double antSopraT = 0, antFuoriT = -1;

    // finestre di un'icona dalla piu' in alto (come l'elenco del menu)
    std::vector<HWND> finestreInOrdine(const Voce& v) const {
        std::vector<HWND> r;
        for (HWND h = GetTopWindow(nullptr); h && r.size() < v.finestre.size(); h = GetWindow(h, GW_HWNDNEXT))
            if (std::find(v.finestre.begin(), v.finestre.end(), h) != v.finestre.end()) r.push_back(h);
        return r;
    }

    void apriAnteprime(int i) {
        if (i < 0 || i >= (int)voci.size()) return;
        const Voce& v = voci[i];
        if (!ant.scegli) {
            ant.scegli = [this](HWND h) { if (IsWindow(h)) portaDavanti(h); };
            ant.chiudiFinestra = [this](HWND h) {
                if (!IsWindow(h)) return;
                if (preparaChiusura) preparaChiusura(h);   // (foto per l'animazione di chiusura, come "Chiudi" del menu)
                PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
            };
        }
        // Le schede escono da dietro l'icona a riposo (senza la lente: v.rett e' quella ingrandita sotto il mouse, piu'
        // alta, e il quadratino partiva sopra l'icona invece che da dietro), sotto la finestra delle icone; stanno
        // sopra il piu' alto dei due bordi, cosi' non coprono l'icona ingrandita
        const auto pos = posizioniRiposo();
        if ((size_t)i >= pos.size()) return;
        const RECT ic = aSchermo({pos[i], yBaseRiposo() - S, pos[i] + S, yBaseRiposo()});
        // bordo interno: il piu' interno fra l'icona a riposo e quella ingrandita (una y, o ai lati una x)
        const D2D1_POINT_2F pc = fis(pos[i], std::min(yBaseRiposo() - S, v.rett.top));
        const POINT o = origine();
        const LONG cima = verticale() ? o.x + std::lround(pc.x) : o.y + std::lround(pc.y);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromPoint({(ic.left + ic.right) / 2, (ic.top + ic.bottom) / 2}, MONITOR_DEFAULTTONEAREST), &mi)) return;
        ant.apri(finestreInOrdine(v), ic, cima, mi.rcWork, i, icone, lato);
        antFuoriT = -1;
        if (registro) { char b[64]; snprintf(b, 64, "dock: anteprime di %zu finestre", v.finestre.size()); scrivi(b); }
    }

    void controllaAnteprime() {
        const double ora = adesso();
        const bool libero = premuta < 0 && !trascinando && menu::aperti == 0 && !lAperto && nessunaScheda() && attivo;
        const int s = (mouseDentro && mouseSulDock && libero) ? sopra : -1;
        const bool conFinestre = s >= 0 && s < (int)voci.size() && !voci[s].uscente && !voci[s].finestre.empty() &&
                                 voci[s].avvio != AVVIO_LAUNCHER && voci[s].avvio != AVVIO_CONTROLLO;
        if (s != antCandidata) { antCandidata = s; antSopraT = ora; }
        if (ant.aperta()) {
            ant.riconcilia();   // (via le schede delle finestre chiuse, anche con il mouse sopra)
            if (!ant.aperta()) return;
            if (!libero) { ant.chiudi(); return; }
            if (ant.mouseSopra() || s == ant.icona()) { antFuoriT = -1; return; }
            if (conFinestre && ora - antSopraT > 0.15) { apriAnteprime(s); return; }   // un'altra icona: passa a quella
            if (antFuoriT < 0) antFuoriT = ora;
            else if (ora - antFuoriT > 0.3) { ant.chiudi(); antFuoriT = -1; }
            return;
        }
        if (anteprimeAccese && conFinestre && ora - antSopraT >= 0.5) { apriAnteprime(s); return; }
        if (!mouseDentro) { KillTimer(icone, TIMER_ANTEPRIME); antTimer = false; }   // (mouse via e niente aperto: basta controlli)
    }

    // ---- "Mostra il desktop" (issue #24: "non trovo come ridurre tutte le finestre") ----
    // Le finestre delle app del dock visibili e non ridotte, dalla piu' in alto: si riducono in quest'ordine come con
    // il clic sull'icona dell'app in primo piano (riduci: copia preparata, poi il genio), una ogni 45 ms perche' si veda una
    // cascata e non dieci animazioni insieme. desktopRidotte le ricorda per "Ripristina le finestre", che le riapre
    // dall'ultima (la piu' in fondo) alla prima, cosi' quella che era davanti torna davanti. Comandi postati: un'app
    // bloccata non ferma il dock. Le finestre di app elevate non si riducono (UIPI), come con il dock in generale
    std::vector<HWND> desktopRidotte, desktopCoda;
    bool desktopRiduce = true;

    std::vector<HWND> finestreDelDock() const {
        std::vector<HWND> tutte;
        for (const auto& v : voci) if (!v.uscente) tutte.insert(tutte.end(), v.finestre.begin(), v.finestre.end());
        // anche quelle delle app nascoste dal dock: l'icona non c'e', ma le finestre sono sullo schermo (R20261120-022133-S2)
        tutte.insert(tutte.end(), finestreNascoste.begin(), finestreNascoste.end());
        std::vector<HWND> r;   // in ordine di z, dalla piu' in alto
        for (HWND h = GetTopWindow(nullptr); h && r.size() < 200; h = GetWindow(h, GW_HWNDNEXT))
            if (std::find(tutte.begin(), tutte.end(), h) != tutte.end() && IsWindowVisible(h) && !IsIconic(h)) r.push_back(h);
        return r;
    }
    bool desktopRipristinabile() const {
        for (HWND h : desktopRidotte) if (IsWindow(h) && IsIconic(h)) return true;
        return false;
    }
    void mostraDesktop() {
        // le ridotte da un "Mostra il desktop" di prima e ancora ridotte restano nell'elenco da ripristinare (prima un
        // secondo comando le perdeva, R20261121-001416-S9)
        std::vector<HWND> nuove = finestreDelDock();
        if (nuove.empty()) return;
        std::vector<HWND> tenute;
        for (HWND h : desktopRidotte) if (IsWindow(h) && IsIconic(h) && std::find(nuove.begin(), nuove.end(), h) == nuove.end()) tenute.push_back(h);
        desktopRidotte = std::move(tenute);
        desktopRidotte.insert(desktopRidotte.end(), nuove.begin(), nuove.end());
        desktopCoda = std::move(nuove); desktopRiduce = true;
        SetTimer(icone, TIMER_DESKTOP, 45, nullptr);
        passoDesktop();   // (la prima subito)
        if (registro) { char b[64]; snprintf(b, 64, "dock: mostra il desktop, %zu finestre", desktopRidotte.size()); scrivi(b); }
    }
    void ripristinaDesktop() {
        desktopCoda.assign(desktopRidotte.rbegin(), desktopRidotte.rend());
        desktopRidotte.clear();
        desktopRiduce = false;
        if (registro) { char b[64]; snprintf(b, 64, "dock: ripristina le finestre, %zu", desktopCoda.size()); scrivi(b); }
        SetTimer(icone, TIMER_DESKTOP, 45, nullptr);
        passoDesktop();
    }
    void passoDesktop() {
        while (!desktopCoda.empty()) {
            const HWND h = desktopCoda.front();
            desktopCoda.erase(desktopCoda.begin());
            // (chiuse o gia' nello stato giusto nel frattempo: si passa alla prossima senza aspettare)
            if (!IsWindow(h) || (desktopRiduce ? IsIconic(h) : !IsIconic(h))) continue;
            // riduzione: con la copia preparata come per il clic sull'icona (riduci = riduciDalDock), altrimenti
            // SC_MINIMIZE postato arrivava senza niente di pronto e la riduzione era quella nativa, senza genio
            // (R20261119-205727-S3); se la copia non si puo' fare (genio spento, pausa), il comando normale
            if (!desktopRiduce) PostMessageW(h, WM_SYSCOMMAND, SC_RESTORE, 0);
            else if (!riduci || !riduci(h)) PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            break;
        }
        if (desktopCoda.empty()) KillTimer(icone, TIMER_DESKTOP);
    }

    void menu(int i, POINT pt) {
        ant.chiudi();   // (il menu prende il posto delle anteprime)
        // Spazio vuoto del dock, launcher e centro di controllo: le impostazioni di FluidWin. Prima il tasto destro li'
        // non faceva niente, e le impostazioni si aprivano solo dal centro di controllo, dalla ricerca o dall'icona
        // nell'area di notifica (nascosta con la barra di Windows): difficili da trovare
        const bool suApp = i >= 0 && i < (int)voci.size() && voci[i].avvio != AVVIO_LAUNCHER && voci[i].avvio != AVVIO_CONTROLLO;
        if (!suApp) {
            if (i >= 0 && i < (int)voci.size() && voci[i].uscente) return;
            SetForegroundWindow(icone);
            // "Mostra il desktop" con finestre da ridurre; "Ripristina le finestre" se quelle ridotte cosi' sono ancora
            // ridotte e non ce ne sono altre aperte (issue #24)
            std::vector<menu::Voce> vv = {menu::titolo(L"FluidWin")};
            // (tutte e due quando servono: una finestra rimasta aperta, per esempio elevata, toglieva il ripristino delle
            // altre, R20261121-001416-S9)
            const bool aperte = !finestreDelDock().empty();
            if (desktopRipristinabile()) vv.push_back(menu::voce(testi::t(L"Ripristina le finestre", L"Restore windows"), 3));
            if (aperte) vv.push_back(menu::voce(testi::t(L"Mostra il desktop", L"Show desktop"), 2));
            // app nascoste dal dock (PR #20): un sottomenu per rimetterle, una per una o tutte
            HMENU sub = nullptr;
            if (!appNascoste.empty()) {
                sub = CreatePopupMenu();
                for (size_t k = 0; k < appNascoste.size(); k++) {
                    std::wstring et;
                    for (wchar_t c : appNascoste[k].nome) { if (c == L'&') et += L"&&"; et += c; }
                    AppendMenuW(sub, MF_STRING, 1000 + (UINT)k, et.c_str());
                }
                if (appNascoste.size() > 1) {
                    AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(sub, MF_STRING, 999, testi::t(L"Mostra tutte", L"Show all"));
                }
                menu::Voce vn = menu::voce(testi::t(L"App nascoste", L"Hidden apps"), 0);
                vn.sotto = sub;
                vv.push_back(vn);
            }
            if (vv.size() > 1) vv.push_back(menu::separatore());
            vv.push_back(menu::voce(testi::t(L"Impostazioni di FluidWin…", L"FluidWin settings…"), 1));
            // (con la barra di Windows nascosta l'icona nell'area di notifica non si vede: qui l'uscita si trova, #16)
            if (esci) vv.push_back(menu::voce(testi::t(L"Esci da FluidWin", L"Quit FluidWin"), 4));
            const int s = (int)menu::mostra(vv, puntoMenu(pt), icone, lato == 0);
            if (sub) DestroyMenu(sub);
            if (s == 1 && impostazioni) impostazioni(nullptr);
            else if (s == 4 && esci) esci();
            else if (s == 2) mostraDesktop();
            else if (s == 3) ripristinaDesktop();
            else if (s == 999) mostraAppNascosta(L"");
            else if (s >= 1000 && s - 1000 < (int)appNascoste.size()) mostraAppNascosta(appNascoste[s - 1000].chiave);
            return;
        }
        if (voci[i].uscente) return;
        // copia: mentre il menu e' aperto (ha un suo ciclo dei messaggi) l'elenco delle app puo' essere rifatto da
        // capo, e un riferimento a voci[i] resterebbe a puntare a memoria liberata
        const Voce v = voci[i];
        if (v.avvio == AVVIO_CESTINO || v.avvio == AVVIO_PILA) {   // cestino e pila: menu loro
            const bool c = v.avvio == AVVIO_CESTINO;
            std::vector<menu::Voce> vv = {menu::titolo(v.nome), menu::voce(c ? testi::t(L"Apri", L"Open") : testi::t(L"Apri la cartella", L"Open folder"), 1)};
            if (c) { auto x = menu::voce(testi::t(L"Svuota il cestino…", L"Empty Recycle Bin…"), 2); x.disattivata = !cestinoPieno; vv.push_back(menu::separatore()); vv.push_back(x); }
            SetForegroundWindow(icone);
            const int s = (int)menu::mostra(vv, puntoMenu(pt), icone, lato == 0);
            if (s == 1) ShellExecuteW(nullptr, L"open", c ? L"shell:RecycleBinFolder" : cartellaDownload().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (s == 2) { SHEmptyRecycleBinW(icone, nullptr, 0); cestinoPieno = false; giroCestino = 0; dirty = true; }   // (chiede conferma Windows)
            return;
        }
        // eseguibile vero sul disco (non quelli protetti delle app dello Store)
        const bool exeVero = !v.exe.empty() && v.exe.find(L"\\windowsapps\\") == std::wstring::npos &&
                             GetFileAttributesW(v.exe.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool store = v.avvio.rfind(L"shell:", 0) == 0;
        const bool collegamento = v.fissata && v.avvio.size() > 4 && minuscolo(v.avvio.substr(v.avvio.size() - 4)) == L".lnk";
        std::vector<menu::Voce> voci = {menu::titolo(v.nome)};
        // con piu' finestre, le finestre per titolo (dalla piu' recente; spuntata quella in primo piano): una si sceglie
        // e viene davanti. Il clic sull'icona porta solo quella piu' in alto
        std::vector<HWND> elenco;
        if (v.finestre.size() > 1) {
            for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
                if (std::find(v.finestre.begin(), v.finestre.end(), h) != v.finestre.end()) elenco.push_back(h);
            const HWND fg = GetForegroundWindow();
            for (size_t k = 0; k < elenco.size(); k++) {
                wchar_t t[256] = L""; GetWindowTextW(elenco[k], t, 256);
                std::wstring s = t[0] ? t : v.nome;
                if (s.size() > 60) s = s.substr(0, 59) + L"…";
                auto x = menu::voce(s, 100 + (UINT)k);
                x.spuntata = elenco[k] == fg;
                voci.push_back(x);
            }
            voci.push_back(menu::separatore());
        }
        // Comandi dell'app (jump list), come nel tasto destro della barra di Windows: le sue categorie ("Recenti" di
        // Steam...) e poi i suoi compiti ("Nuova finestra in incognito"...). Issue #24: prima mancavano. Il file si
        // trova con l'AppUserModelID (collegamento fissato, pacchetto, finestre aperte) o, se l'app non lo espone, con
        // l'eseguibile; vedi jumplist.h. Gli id dei comandi partono da 1000
        std::vector<const jumplist::Comando*> comandi;
        jumplist::Elenco jl;   // (copia: i pixel delle icone che il menu usa restano validi fino alla fine di menu)
        {
            Cronometro cr(this, "comandi dell'app (jump list)");
            std::vector<std::wstring> ids = {v.aumid};
            if (v.pacchetto.size() > 17) ids.push_back(v.pacchetto.substr(17));   // dopo "shell:AppsFolder\"
            for (HWND h : v.finestre) if (IsWindow(h)) ids.push_back(aumidFinestra(h));
            std::vector<std::wstring> altri;
            for (const auto& x : this->voci) {
                if (x.uscente || chiave(x) == chiave(v)) continue;
                altri.push_back(x.aumid);
                if (x.pacchetto.size() > 17) altri.push_back(x.pacchetto.substr(17));
            }
            const char* come = "";
            jl = jumpList.per(ids, v.exe, altri, &come);   // (copia gia' pronta dal thread: niente file letti qui)
            jumpList.richiedi();                            // (e il thread ricontrolla, per la prossima volta)
            for (const auto& c : jl.categorie) {
                const size_t prima = voci.size();
                // (categoria senza nome, per esempio scartato perche' in rete: niente intestazione vuota, le voci restano;
                // R20261119-141543-S1)
                if (c.compiti || !c.nome.empty()) voci.push_back(menu::titolo(c.compiti ? testi::t(L"Attività", L"Tasks") : c.nome));
                else if (voci.size() > 1 && !voci.back().separatore) voci.push_back(menu::separatore());
                size_t n = 0;
                for (const auto& x : c.voci) {
                    if (x.separatore) { if (!voci.back().separatore && !voci.back().titolo) voci.push_back(menu::separatore()); continue; }
                    if (!c.compiti && ++n > 10) break;   // (le categorie come "Recenti" possono essere lunghe: come Windows, 10)
                    std::wstring t = x.titolo;
                    if (t.size() > 60) t = t.substr(0, 59) + L"…";
                    auto mv = menu::voce(t, 1000 + (UINT)comandi.size());
                    if (!x.pixel.empty()) { mv.pixel = x.pixel.data(); mv.latoPixel = jumplist::LATO_ICONA; }
                    comandi.push_back(&x);
                    voci.push_back(mv);
                }
                while (voci.size() > prima && (voci.back().separatore || voci.back().titolo)) voci.pop_back();   // categoria vuota o separatore in fondo
            }
            if (!comandi.empty()) voci.push_back(menu::separatore());
            if (registro) { char b[120]; snprintf(b, 120, "dock: %zu comandi dell'app nel menu (trovati con %s)", comandi.size(), comandi.empty() ? "niente" : come); scrivi(b); }
        }
        voci.push_back(menu::voce(testi::t(L"Apri nuova finestra", L"Open new window"), 1));
        if (!store || exeVero) voci.push_back(menu::voce(testi::t(L"Esegui come amministratore", L"Run as administrator"), 3));
        if (exeVero) voci.push_back(menu::voce(testi::t(L"Apri percorso file", L"Open file location"), 4));
        if (collegamento) { voci.push_back(menu::separatore()); voci.push_back(menu::voce(testi::t(L"Rimuovi dal dock", L"Remove from dock"), 5)); }
        else if (!v.fissata) {
            voci.push_back(menu::separatore());
            voci.push_back(menu::voce(testi::t(L"Aggiungi al dock", L"Add to dock"), 6));
            voci.push_back(menu::voce(testi::t(L"Nascondi dal dock", L"Hide from dock"), 7));
        }
        if (!v.finestre.empty()) {
            voci.push_back(menu::separatore());
            voci.push_back(menu::voce(v.finestre.size() > 1 ? testi::t(L"Chiudi tutte le finestre", L"Close all windows") : testi::t(L"Chiudi", L"Close"), 2));
        }
        SetForegroundWindow(icone);   // cosi' la tastiera (Esc, frecce) arriva al menu
        // processo di ogni finestra prima del menu: mentre il menu e' aperto una finestra puo' chiudersi e il suo HWND
        // passare a un'altra app, che "Chiudi" non deve toccare (R20261121-001416-S7)
        std::vector<DWORD> pidPrima;
        for (HWND h : v.finestre) { DWORD p = 0; GetWindowThreadProcessId(h, &p); pidPrima.push_back(p); }
        auto stessa = [&](size_t k) { DWORD p = 0; return IsWindow(v.finestre[k]) && GetWindowThreadProcessId(v.finestre[k], &p) && p == pidPrima[k]; };
        // sopra il dock, centrato sull'icona (il punto e' quello del clic)
        const int s = (int)menu::mostra(voci, puntoMenu(pt), icone, lato == 0);
        if (s >= 1000 && s - 1000 < (int)comandi.size()) {
            // come dalla barra di Windows; la finestra che ne nasce e' un lancio dell'app (rimbalzo, animazione d'apertura)
            segnaLancio(v);
            const bool ok = jumplist::avvia(*comandi[s - 1000]);
            if (registro) { char b[80]; snprintf(b, 80, "dock: comando dell'app %d avviato: %d", s - 1000, (int)ok); scrivi(b); }
            return;
        }
        if (s == 1) {
            // (stessa scelta del clic normale: collegamento Store vecchio -> il pacchetto, R20261121-001416-S15)
            segnaLancio(v); ShellExecuteW(nullptr, L"open", daAprire(v).c_str(), argomentiDaAprire(v), nullptr, SW_SHOWNORMAL);
            for (auto& x : this->voci) if (!x.uscente && chiave(x) == chiave(v)) x.rimbalzoT0 = ultimoT;   // la voce di adesso
            dirty = true;
        }
        else if (s == 2) {
            // come "Chiudi finestra" della barra di Windows: il comando di sistema (lo stesso del menu della finestra e di
            // Alt+F4), non WM_CLOSE diretto, che alcune app trattano diversamente
            for (size_t k = 0; k < v.finestre.size(); k++) {
                const HWND h = v.finestre[k];
                if (!stessa(k)) continue;
                wchar_t c[64] = L""; GetClassNameW(h, c, 64);
                if (registro) { char b[200]; snprintf(b, sizeof(b), "chiudi dal dock: %p (%ls)%s", (void*)h, c, IsIconic(h) ? " ridotta" : ""); registro(b); }
                if (preparaChiusura) preparaChiusura(h);   // la foto per l'animazione di chiusura (come Alt+F4)
                PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
            }
        }
        else if (s == 3) ShellExecuteW(nullptr, L"runas", (collegamento || !exeVero ? v.avvio : v.exe).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (s == 4) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + v.exe + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else if (s == 5) { DeleteFileW(v.avvio.c_str()); appDaAggiornare = true; dirty = true; }   // come "Rimuovi dalla barra"
        else if (s == 6) aggiungiAlDock(v.nome, exeVero ? v.exe : L"", v.aumid, v.avvio);
        else if (s == 7) nascondiApp(v.exe, v.aumid, v.nome);
        else if (s >= 100 && s - 100 < (int)elenco.size() && IsWindow(elenco[s - 100])) {
            const auto it = std::find(v.finestre.begin(), v.finestre.end(), elenco[s - 100]);
            if (it == v.finestre.end() || stessa((size_t)(it - v.finestre.begin()))) portaDavanti(elenco[s - 100]);
        }
    }

    static LRESULT CALLBACK procVetro(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_PAINT) { PAINTSTRUCT ps; HDC d = BeginPaint(h, &ps); FillRect(d, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH)); EndPaint(h, &ps); return 0; }
        if (m == WM_NCCALCSIZE && w) return 0;
        if (m == WM_NCHITTEST) return HTTRANSPARENT;
        if (m == WM_CLOSE) return 0;   // (Alt+F4 o un programma che chiede di chiudere: le finestre del dock restano)
        return DefWindowProcW(h, m, w, l);
    }

    static LRESULT CALLBACK procIcone(HWND h, UINT m, WPARAM w, LPARAM l) {
        Motore* d = gIstanza();
        if (!d) return DefWindowProcW(h, m, w, l);
        switch (m) {
            case WM_MOUSEMOVE: {
                d->diagMosse++;
                if (!d->mouseDentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, h, 0}; TrackMouseEvent(&tm); }
                // movimenti finti (stessa posizione, per esempio dopo un cambio di regione): niente da ridisegnare
                float lx = 0, ly = 0; d->logico((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l), lx, ly);   // (coordinate "come in basso": vedi base)
                if (d->mouseDentro && d->mouseX == lx && d->mouseY == ly) return 0;
                d->mouseDentro = true;
                d->mouseX = lx; d->mouseY = ly;
                const bool isola = d->sullIsola(d->mouseX, d->mouseY);
                // (l'ora in fondo al dock e' parte del dock: l'ingrandimento resta, altrimenti il dock si stringerebbe
                // e l'ora scapperebbe da sotto il mouse, per poi tornare)
                d->mouseSulDock = !isola || d->oraNelDock;
                const int statoPrima = d->statoSopra;
                d->statoSopra = isola ? d->statoInPunto(d->mouseX, d->mouseY) : -1;
                // mouse arrivato sull'ora: le notifiche si cominciano a leggere, cosi' al clic sono quasi pronte
                if (d->statoSopra == ORA && statoPrima != ORA && GetTickCount64() - d->chiestaN > 3000) d->chiediNotifiche();
                // anteprime delle finestre: il controllo gira mentre il mouse e' sul dock (vedi controllaAnteprime)
                if (d->anteprimeAccese && !d->antTimer) { SetTimer(h, TIMER_ANTEPRIME, 80, nullptr); d->antTimer = true; }
                // riordino (#23): icona premuta e mouse spostato di 8 px -> la si trascina
                if (!d->trascinando && d->premuta >= 0 && (w & MK_LBUTTON) && std::abs(d->mouseX - d->premutaX) > 8 * d->dpi &&
                    d->trascinabile(d->premuta))
                    d->iniziaTrascina();
                if (d->trascinando) d->passoTrascina();
                d->dirty = true;
                return 0;
            }
            case WM_CAPTURECHANGED: if (d->trascinando && (HWND)l != h) d->fineTrascina(); return 0;   // (Esc, un'altra finestra)
            case WM_MOUSELEAVE:
                if (d->trascinando) return 0;   // (con la cattura il mouse puo' uscire: il trascinamento continua)
                d->mouseDentro = false; d->mouseSulDock = false; d->sopra = -1; d->premuta = -1;
                d->statoSopra = -1; d->statoPremuto = -1; d->tastoPremuto = -1; d->dirty = true;
                return 0;
            case WM_LBUTTONDOWN: {
                float x = 0, y = 0; d->logico((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l), x, y);
                if (d->inTesta(x, y)) { d->tastoPremuto = d->tastoTestaIn(x, y); if (d->tastoPremuto < 0) d->tastoPremuto = -2; d->ant.chiudi(); d->dirty = true; return 0; }   // musica e timer
                if (d->sullIsola(x, y)) d->statoPremuto = d->statoInPunto(x, y);
                else {
                    d->premuta = d->iconaInPunto(x, y);
                    // (cattura: il trascinamento segue il mouse anche se esce un poco dal dock)
                    if (d->premuta >= 0) { d->premutaX = x; SetCapture(h); }
                }
                d->ant.chiudi();   // (clic su un'icona: le anteprime se ne vanno)
                d->dirty = true; return 0;
            }
            case WM_LBUTTONUP: {
                float x = 0, y = 0; d->logico((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l), x, y);
                if (d->trascinando) { d->fineTrascina(); d->statoPremuto = -1; return 0; }   // (posata: niente clic)
                if (GetCapture() == h) ReleaseCapture();
                if (d->tastoPremuto != -1) {   // testa: il tasto premuto, se il mouse e' ancora sopra
                    const int b = d->tastoPremuto;
                    d->tastoPremuto = -1;
                    if (b >= 0 && d->tastoTestaIn(x, y) == b) d->premiTastoTesta(b);
                    // sulla musica ma fuori dai tasti (copertina, titolo): l'app che sta riproducendo
                    else if (b == -2 && d->testaMusica && dentro(d->rettMus, x, y) && d->tastoTestaIn(x, y) < 0) d->apriAppMusica(d->notch.musicaOra().aumid);
                    d->premuta = -1; d->statoPremuto = -1; d->dirty = true;
                    return 0;
                }
                if (d->statoPremuto >= 0) {
                    const int i = d->statoInPunto(x, y);
                    if (i == d->statoPremuto) d->azioneStato(i);
                } else {
                    const int i = d->iconaInPunto(x, y);
                    if (i >= 0 && i == d->premuta) d->clic(i);
                }
                d->premuta = -1; d->statoPremuto = -1; d->dirty = true;
                return 0;
            }
            case WM_MOUSEWHEEL: {   // rotella sul volume: +-2% per scatto
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(h, &pt);
                // (sull'icona del centro di controllo, come prima sulla pillola dell'isola)
                float x = 0, y = 0; d->logico((float)pt.x, (float)pt.y, x, y);
                const int iv = d->iconaInPunto(x, y);
                if (iv >= 0 && d->voci[iv].avvio == AVVIO_CONTROLLO) d->cambiaVolume(GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA * 0.02f);
                return 0;
            }
            case WM_TIMER:
                if (w == TIMER_BORDO) { d->controllaBordo(); return 0; }
                if (w == TIMER_DOPO) { d->suTimerDopo(); return 0; }
                if (w == TIMER_DESKTOP) { d->passoDesktop(); return 0; }
                if (w == TIMER_ANTEPRIME) { d->controllaAnteprime(); return 0; }
                if (w == TIMER_TESTA) { if (d->yNascosto < 1) d->dirty = true; return 0; }   // (dock giu': niente da ridisegnare)
                if (w == TIMER_INVIO) {
                    KillTimer(h, TIMER_INVIO);
                    // solo se il fuoco e' davvero sulla barra di Windows: l'Invio non deve finire in un'altra app
                    wchar_t c[64] = L""; GetClassNameW(GetForegroundWindow(), c, 64);
                    if (!wcscmp(c, L"Shell_TrayWnd")) {
                        INPUT in[2]{}; in[0].type = in[1].type = INPUT_KEYBOARD;
                        in[0].ki.wVk = in[1].ki.wVk = VK_RETURN; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(2, in, sizeof(INPUT));
                    }
                }
                return 0;
            case WM_RBUTTONUP: {
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                float x = 0, y = 0; d->logico((float)pt.x, (float)pt.y, x, y);
                if (d->sullIsola(x, y)) return 0;
                const int i = d->iconaInPunto(x, y);
                ClientToScreen(h, &pt); d->menu(i, pt); return 0;
            }
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_CLOSE: return 0;   // (le finestre del dock restano)
            case WM_ICONA_PRONTA: d->raccogliIcone(); return 0;
            case WM_NOTIFICHE_LETTE: d->notificheLette(); return 0;
            case WM_APPBAR: if (w == ABN_POSCHANGED && d->appbarOk) d->aggiornaAppbar(); return 0;   // un'altra appbar si e' spostata
            case WM_LAVORO_FATTO: { if (!consegna::riprendi(h, m, (const void*)l)) return 0; Sottofondo* s = (Sottofondo*)l; s->applica(); delete s; return 0; }
            case WM_DISPLAYCHANGE: d->cambiaSchermo(); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

} // namespace dock

// i metodi del centro di controllo e del launcher, in file a parte
#include "centro.h"
#include "launcher.h"
