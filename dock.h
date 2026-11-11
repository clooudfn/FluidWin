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
#include "widget.h"
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
#include "testi.h"

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

namespace dock {

static const UINT WM_APPBAR = WM_APP + 20;
static const UINT_PTR TIMER_INVIO = 7;   // "icone nascoste": Invio poco dopo Win+B
static const UINT_PTR TIMER_BORDO = 8;   // dock nascosto: controlla se il mouse arriva sul bordo in basso
static const UINT_PTR TIMER_DOPO = 9;    // launcher o centro di controllo da aprire quando l'altro ha finito di chiudersi
static const wchar_t* const AVVIO_LAUNCHER = L"*launcher";   // la voce "App" del dock
static const wchar_t* const AVVIO_CONTROLLO = L"*controllo";   // l'ultima voce: il centro di controllo

struct Voce {
    std::wstring nome;
    std::wstring exe;          // percorso dell'eseguibile, minuscolo (per riconoscere le finestre)
    std::wstring aumid;        // AppUserModelID, se c'e'
    std::wstring avvio;        // cosa aprire al clic (collegamento .lnk o shell:AppsFolder\...)
    bool fissata = false;      // aggiunta alla barra
    ID2D1Bitmap1* icona = nullptr;
    std::vector<HWND> finestre;
    double rimbalzoT0 = -1;    // istante di inizio del rimbalzo (avvio), -1 = fermo
    double scala = 1;          // 0..1: le icone nuove crescono, quelle delle app chiuse si restringono (niente scatti)
    bool uscente = false;      // app chiusa: si restringe e poi sparisce
    D2D1_RECT_F rett{};        // ultima posizione disegnata (coordinate della finestra delle icone)
};

static std::wstring minuscolo(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

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
    wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
    std::wstring r = QueryFullProcessImageNameW(p, 0, buf, &n) ? buf : L"";
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
        const HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        monitor = mon;
        MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
        schermo = mi.rcMonitor;
        ::vetro::schermoDock() = schermo; sis.schermo(schermo);
        UINT dx = 96, dy = 96; dpi = 1.0f;
        if (HMODULE sh = LoadLibraryW(L"shcore.dll")) {
            typedef HRESULT (WINAPI *Fn)(HMONITOR, int, UINT*, UINT*);
            if (auto f = (Fn)GetProcAddress(sh, "GetDpiForMonitor")) if (SUCCEEDED(f(mon, 0, &dx, &dy))) dpi = dx / 96.0f;
        }
        S = 42 * dpi; M = 66 * dpi;   // icone a riposo e ingrandite
        PAD = 8 * dpi; GAP = 6 * dpi; MARGINE = 8 * dpi;
        RAGGIO = raggio::vetro() * dpi;   // come gli angoli DWM del vetro: fondo e vetro devono coincidere
        altezzaDock = S + 2 * PAD;
        W = schermo.right - schermo.left;
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
                                wi.lpszClassName, L"Dock", WS_POPUP, schermo.left, schermo.bottom - H, W, H,
                                nullptr, nullptr, inst, nullptr);
        BOOL niente = TRUE; DwmSetWindowAttribute(icone, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        if (!grafica()) return false;
        if (!creaPannello(inst)) return false;   // scheda 0
        scambiaScheda(); const bool seconda = creaPannello(inst); scambiaScheda();   // scheda 1 (sta in ltra)
        if (!seconda) return false;
        if (!creaLauncher(inst)) return false;
        notch.clic = [this] { apriCentro(P_NOTIFICHE); };   // un clic sul notch: le notifiche
        notch.crea(inst);
        notch.font(fontTesto, fontTitoli);

        // nessuno spazio riservato: le finestre ingrandite prendono tutto lo schermo e il dock si nasconde sotto il bordo
        SetWindowPos(icone, HWND_TOPMOST, schermo.left, schermo.bottom - H, W, H, SWP_NOACTIVATE);

        // la barra di Windows si nasconde da sola (impostazione ufficiale), si rimette com'era all'uscita
        APPBARDATA tb{}; tb.cbSize = sizeof(tb);
        statoBarraPrima = (UINT)SHAppBarMessage(ABM_GETSTATE, &tb);
        tb.lParam = statoBarraPrima | ABS_AUTOHIDE;
        SHAppBarMessage(ABM_SETSTATE, &tb);

        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&audio));
        aggiornaStato(true);
        aggiornaApp();
        attivo = true;
        ShowWindow(vetro, SW_SHOWNOACTIVATE);

        ShowWindow(icone, SW_SHOWNOACTIVATE);
        notch.posto(schermo, dpi);
        notch.mostra(true);
        dirty = true;
        return true;
    }

    void chiudi() {
        fermaIcone();
        if (!attivo) return;
        attivo = false;
        APPBARDATA tb{}; tb.cbSize = sizeof(tb); tb.lParam = statoBarraPrima;
        SHAppBarMessage(ABM_SETSTATE, &tb);
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
        monitor = verso;
        schermo = mi.rcMonitor;
        ::vetro::schermoDock() = schermo; sis.schermo(schermo);
        const int w = schermo.right - schermo.left;
        if (w != W && w > 0 && sc && dc) {
            dc->SetTarget(nullptr);
            if (bersaglio) { bersaglio->Release(); bersaglio = nullptr; }
            if (SUCCEEDED(sc->ResizeBuffers(0, (UINT)w, (UINT)H, DXGI_FORMAT_UNKNOWN, 0))) W = w;
            IDXGISurface* sup = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup))) {
                D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglio); sup->Release();
            }
            dc->SetTarget(bersaglio);
            if (!bersaglio) perso = true;   // superficie non ricreata: si riparte da capo (riavvio di FluidWin)
        }
        SetWindowPos(icone, HWND_TOPMOST, schermo.left, schermo.bottom - H, W, H, SWP_NOACTIVATE);
        rettVetro = rettVetroStato = rettVetroOra = RECT{};          // i vetri si rimettono al prossimo fotogramma
        regioneDock = regioneIsola = regioneEtichetta = RECT{};
        if (registro) { char b[96]; snprintf(b, 96, "ora su %ld,%ld-%ld,%ld", schermo.left, schermo.top, schermo.right, schermo.bottom); scrivi(b); }
        // nascondersi o no dipende dalle finestre del nuovo schermo
        controllaFinestre(true);
        notch.posto(schermo, dpi);   // il notch va in alto al centro del nuovo schermo
        dirty = true;
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
            if (!((!l.exe.empty() && l.exe == exe) || (!l.aumid.empty() && l.aumid == id))) continue;
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
                n = IsZoomed(r) || (w.left <= schermo.left + 2 && w.top <= schermo.top + 2 && w.right >= schermo.right - 2 && w.bottom >= schermo.bottom - 2);
                // come i dock "intelligenti": si nasconde anche quando la finestra in primo piano gli finisce sopra
                if (!n) {
                    RECT vis{}; if (FAILED(DwmGetWindowAttribute(r, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) vis = w;
                    const LONG topDock = schermo.bottom - H + (LONG)(yBaseRiposo() - S - PAD);
                    const RECT dockR = {schermo.left + (LONG)zonaDockL, topDock, schermo.left + (LONG)zonaDockR, schermo.bottom};
                    const RECT isolaR = {schermo.left + (LONG)isola.left, topDock, schermo.left + (LONG)isola.right, schermo.bottom};
                    RECT x;
                    n = IntersectRect(&x, &vis, &dockR) || IntersectRect(&x, &vis, &isolaR);
                }
            }
        }
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

    void schermoIntero(bool si) {
        if (si == nascostoSchermoIntero) return;
        nascostoSchermoIntero = si;
        for (HWND h : {icone, vetro}) ShowWindow(h, si ? SW_HIDE : SW_SHOWNOACTIVATE);
        notch.mostra(!si);
        if (si) { nascondiSchede(); nascondiLauncher(); }
        if (!si) dirty = true;
    }

private:
    static Motore*& gIstanza() { static Motore* m = nullptr; return m; }

    // app lanciate dal dock e non ancora comparse (per il genio d'apertura)
    struct Lancio { std::wstring chiave, exe, aumid; double t; };
    std::vector<Lancio> lanci;
    void segnaLancio(const Voce& v) { lanci.push_back({chiave(v), v.exe, v.aumid, adesso()}); }

    // icona i del dock a riposo (senza ingrandimento), in coordinate schermo: cosi' il bersaglio del genio non si
    // muove durante l'animazione. Dock nascosto: risale per qualche istante, il tempo che la finestra entri o esca
    bool rettIcona(size_t i, RECT& out) {
        if (vuoleNascondersi) { rivelaFino = adesso() + 1.4; dirty = true; posizionaVetri(); }
        const auto pos = posizioniRiposo();
        if (i >= pos.size()) return false;
        out = {(LONG)(schermo.left + pos[i]), (LONG)(schermo.bottom - H + yBaseRiposo() - S),
               (LONG)(schermo.left + pos[i] + S), (LONG)(schermo.bottom - H + yBaseRiposo())};
        return true;
    }

    // ---- misure (pixel) ----
    float dpi = 1, S = 48, M = 74, PAD = 8, GAP = 6, MARGINE = 8, RAGGIO = 8, altezzaDock = 64;
    RECT schermo{};
    int W = 0, H = 0;
    UINT statoBarraPrima = 0;

    // ---- stato ----
    std::vector<Voce> voci;
    size_t primaNonFissata = 0;      // indice della prima app aperta ma non aggiunta alla barra (separatore)
    std::vector<std::wstring> ordineAperte;   // le app aperte restano nel posto in cui sono comparse la prima volta
    bool dirty = true, inMovimento = false, appDaAggiornare = true, nascostoSchermoIntero = false, vetroDaRiposizionare = false;
    double ultimoAggiornamento = -10, ultimoT = 0, dtFotogramma = 1.0 / 180;
    bool mouseDentro = false, mouseSulDock = false; float mouseX = 0, mouseY = 0;
    double lente = 0, lenteVel = 0;  // 0 = icone normali, 1 = ingrandimento pieno (molla)
    // a scomparsa: con una finestra a schermo intero il dock scende sotto il bordo e riesce col mouse in fondo
    HMONITOR monitor = nullptr;
    HMONITOR spostamento = nullptr;   // schermo verso cui il dock sta andando (prima scende, poi si sposta e risale)
    HMONITOR bordoAltro = nullptr; double bordoAltroDa = -1;   // mouse sul bordo in basso di un altro schermo, da quando
    bool vuoleNascondersiI = false;   // l'isola sta sempre nascosta, il dock solo sotto le finestre
    bool vuoleNascondersi = false, rivelato = false, rivelatoI = false;   // dock e isola di stato risalgono separati
    double yNascosto = 0, velNascosto = 0;   // px sotto la posizione normale (dock)
    double yNascostoI = 0, velNascostoI = 0; // ... e isola di stato
    double rivelaFino = 0, fuoriDal = -1, fuoriDalI = -1;   // comparsa temporanea (per il genio) e da quando il mouse e' lontano
    float zonaDockL = 0, zonaDockR = 0;      // dove sta il dock (coordinate finestra), per capire a chi e' vicino il mouse
    // tirato dal mouse: il fondo si allunga un poco verso il puntatore
    double tiroX = 0, tiroY = 0, velTiroX = 0, velTiroY = 0;
    int sopra = -1;                  // icona sotto il mouse
    int premuta = -1;
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
        int angoli = 2; DwmSetWindowAttribute(v, DWMWA_WINDOW_CORNER_PREFERENCE, &angoli, sizeof(angoli));
        BOOL niente = TRUE; DwmSetWindowAttribute(v, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        return v;
    }

    bool grafica() {
        // la device di Direct3D condivisa con genio e gelatina (gpu.h): una sola per tutto FluidWin
        dev = gpu::condivisa().dev; ctx = gpu::condivisa().ctx;
        if (!dev) return false;
        dev->AddRef(); ctx->AddRef();
        IDXGIDevice1* dd = nullptr; dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dd);
        dd->SetMaximumFrameLatency(1);
        IDXGIFactory2* fab = nullptr;
        if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&fab))) return false;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(fab->CreateSwapChainForComposition(dev, &sd, nullptr, &sc))) return false;
        fab->Release();
        if (FAILED(DCompositionCreateDevice(dd, __uuidof(IDCompositionDevice), (void**)&dcomp))) return false;
        dcomp->CreateTargetForHwnd(icone, TRUE, &target);
        dcomp->CreateVisual(&visual); visual->SetContent(sc); target->SetRoot(visual); dcomp->Commit();

        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)&f2d))) return false;
        if (FAILED(f2d->CreateDevice(dd, &d2dev))) return false;
        dd->Release();
        d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        IDXGISurface* sup = nullptr; sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglio); sup->Release();
        dc->SetTarget(bersaglio);
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &pennello);
        f2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_LINE_JOIN_ROUND), nullptr, 0, &tratto);

        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&fdw);
        auto formato = [&](const wchar_t* font, DWRITE_FONT_WEIGHT peso, float px, IDWriteTextFormat** out) {
            fdw->CreateTextFormat(font, nullptr, peso, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px * dpi, L"it-it", out);
            (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
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
            formato(font, peso, px, out); (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        };
        sinistra(fTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0f, &fmtTitolo);
        sinistra(fTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 34.0f, &fmtGrande);
        sinistra(fTesto, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, &fmtVoce);
        sinistra(fTesto, DWRITE_FONT_WEIGHT_NORMAL, 11.5f, &fmtSotto);
        sinistra(fTesto, DWRITE_FONT_WEIGHT_MEDIUM, 13.0f, &fmtNotizia);   // titoli delle notizie: a capo, al massimo due righe
        fmtNotizia->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); fmtNotizia->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmtNotizia->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f * dpi, 13.5f * dpi);   // righe fisse: ne entrano esattamente due
        formato(fTesto, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, &fmtNome);
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
        if (GetWindowTextLengthW(h) == 0) return false;
        return true;
    }

    static std::wstring chiave(const Voce& v) { return v.exe.empty() ? v.avvio : v.exe; }

    // cache di aggiornaApp (gira a ogni cambio di primo piano)
    std::vector<Voce> fissateCache;                 // le app fissate (senza icone ne' finestre)
    ULONGLONG fissateT = 0; double fissateLette = -1e9; bool fissateOk = false;
    std::unordered_map<HWND, std::pair<DWORD, std::wstring>> exeFinestre;   // finestra -> (processo, eseguibile)
    std::unordered_map<std::wstring, std::wstring> nomiExe;                 // eseguibile -> nome leggibile

    // App aggiunte alla barra (i loro collegamenti) + app con finestre aperte
    void aggiornaApp() {
        Cronometro cr(this, "aggiornaApp");
        appDaAggiornare = false; ultimoAggiornamento = ultimoT;
        // icona premuta: la si ritrova per nome, gli indici possono cambiare
        const std::wstring chiavePremuta = (premuta >= 0 && premuta < (int)voci.size()) ? chiave(voci[premuta]) : L"";
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
            const bool valida = fissateOk && letta && tCart == fissateT && adesso() - fissateLette < 60;
            if (valida) for (const Voce& f : fissateCache) nuove.push_back(f);
            fissateT = tCart; fissateLette = adesso();
            WIN32_FIND_DATAW fd; HANDLE fh = valida ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
            std::vector<std::pair<ULONGLONG, std::wstring>> lnk;
            if (fh != INVALID_HANDLE_VALUE) {
                do lnk.push_back({((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime, dir + fd.cFileName});
                while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
            std::sort(lnk.begin(), lnk.end());
            for (auto& [t, p] : lnk) {
                Voce v; v.fissata = true; v.avvio = p;
                v.nome = p.substr(p.find_last_of(L'\\') + 1); v.nome = v.nome.substr(0, v.nome.size() - 4);
                IShellLinkW* sl = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
                    IPersistFile* pf = nullptr;
                    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Load(p.c_str(), STGM_READ))) {
                        wchar_t dest[MAX_PATH] = L"";
                        if (SUCCEEDED(sl->GetPath(dest, MAX_PATH, nullptr, SLGP_RAWPATH))) {
                            wchar_t esp[MAX_PATH]; ExpandEnvironmentStringsW(dest, esp, MAX_PATH);
                            v.exe = minuscolo(esp);
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
                nuove.push_back(std::move(v));
            }
            if (!valida) { fissateCache.assign(nuove.begin() + 1, nuove.end()); fissateOk = letta; }
        }
        const size_t fissate = nuove.size();
        // 2) finestre aperte: si agganciano all'app fissata corrispondente, o creano una nuova voce
        struct Ctx { std::vector<HWND> v; } ctxF;
        EnumWindows([](HWND h, LPARAM l) -> BOOL { if (finestraDaDock(h)) ((Ctx*)l)->v.push_back(h); return TRUE; }, (LPARAM)&ctxF);
        // eseguibile di ogni finestra: si chiede al sistema una volta sola per finestra (stesso processo)
        for (auto it = exeFinestre.begin(); it != exeFinestre.end();)
            it = std::find(ctxF.v.begin(), ctxF.v.end(), it->first) == ctxF.v.end() ? exeFinestre.erase(it) : std::next(it);
        for (HWND h : ctxF.v) {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            auto ef = exeFinestre.find(h);
            if (ef == exeFinestre.end() || ef->second.first != pid) ef = exeFinestre.insert_or_assign(h, std::make_pair(pid, exeProcesso(h))).first;
            const std::wstring& exe = ef->second.second;
            const std::wstring id = aumidFinestra(h);
            if (exe.empty()) continue;
            Voce* trovata = nullptr;
            for (auto& v : nuove) {
                if ((!id.empty() && !v.aumid.empty() && id == v.aumid) || (!v.exe.empty() && v.exe == exe)) { trovata = &v; break; }
            }
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
        // ultima icona: il centro di controllo (sempre in fondo, come il Cestino su macOS; disegnata dal dock)
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
                    v.scala = vecchia.scala;
                    ritrovata[j] = true; vecchiaTrovata = true;
                    break;
                }
            }
            if (!v.icona && v.avvio != AVVIO_LAUNCHER && v.avvio != AVVIO_CONTROLLO) {
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
        // (un'app che si chiude puo' essere finita dopo il centro di controllo: lui resta l'ultimo)
        for (size_t i = 0; i + 1 < nuove.size(); i++)
            if (nuove[i].avvio == AVVIO_CONTROLLO) { std::rotate(nuove.begin() + i, nuove.begin() + i + 1, nuove.end()); break; }
        for (auto& vecchia : voci) if (vecchia.icona) vecchia.icona->Release();
        voci = std::move(nuove);
        primaNonFissata = fissateTot;
        premuta = -1;
        if (!chiavePremuta.empty())
            for (size_t i = 0; i < voci.size(); i++) if (!voci[i].uscente && chiave(voci[i]) == chiavePremuta) premuta = (int)i;
        sopra = -1;   // si ricalcola al prossimo fotogramma
        dirty = true;
    }

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
        // un altro schermo: col mouse fermo sul suo bordo in basso (e nessun tasto premuto, per non spostarlo
        // trascinando una finestra) il dock va li'
        if (!(pt.x >= schermo.left && pt.x < schermo.right && pt.y >= schermo.top && pt.y < schermo.bottom)) {
            const HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            const bool tasti = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
            if (m && !tasti && !nascostoSchermoIntero && GetMonitorInfoW(m, &mi) && pt.y >= mi.rcMonitor.bottom - 2) {
                if (m != bordoAltro) { bordoAltro = m; bordoAltroDa = adesso(); }
                else if (adesso() - bordoAltroDa > 0.2) { bordoAltro = nullptr; spostaVerso(m); }
            } else bordoAltro = nullptr;
            // il dock e l'isola rivelati sullo schermo lasciato tornano giu' come al solito (il mouse e' lontano)
        } else bordoAltro = nullptr;
        const float x = (float)(pt.x - schermo.left);
        // sul bordo in basso le zone sono solo due: sotto il dock (risale il dock) e da sotto l'isola di stato fino
        // all'angolo destro (risale l'isola). Lo spazio vuoto fra i due e i lati non fanno nulla
        const bool zonaIsola = isola.right > isola.left && x >= isola.left - 10 * dpi;   // (isola tolta: mai)
        const bool zonaDock = x >= zonaDockL - 10 * dpi && x <= zonaDockR + 10 * dpi;
        if (pt.y >= schermo.bottom - 2 && pt.x >= schermo.left && pt.x < schermo.right && (zonaIsola || zonaDock)) {
            bool &r = zonaIsola ? rivelatoI : rivelato;
            // risalendo torna sopra a tutto (anche alle finestre "sempre in primo piano" ingrandite dopo di lui)
            if (!r) { r = true; (zonaIsola ? fuoriDalI : fuoriDal) = -1; dirty = true; posizionaVetri(); }
        }
        const bool alto = pt.y >= schermo.bottom - H + (LONG)(yBaseRiposo() - M - PAD - 30 * dpi);
        if (rivelato) {
            // "sopra al dock" dalla posizione vera del mouse (mouseDentro puo' restare vero se Windows non avvisa dell'uscita)
            const bool sopraIcone = WindowFromPoint(pt) == icone;
            const bool vicino = (sopraIcone && mouseSulDock) || (alto && x >= zonaDockL - 40 * dpi && x <= zonaDockR + 40 * dpi);
            if (vicino || lAperto) fuoriDal = -1;
            else if (fuoriDal < 0) fuoriDal = adesso();
            else if (adesso() - fuoriDal > 0.35) { rivelato = false; fuoriDal = -1; dirty = true; }
        }
        if (rivelatoI) {
            const bool vicino = (WindowFromPoint(pt) == icone && !mouseSulDock) || (alto && zonaIsola);
            if (vicino || !nessunaScheda()) fuoriDalI = -1;
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

    // x di ogni icona a riposo; lo spazio prima di un'icona cresce e cala con lei
    std::vector<float> posizioniRiposo() const { return disponi(std::vector<float>(voci.size(), S)); }

    // dato il lato di ogni icona (gia' ingrandito), restituisce la x sinistra di ognuna, dock centrato
    std::vector<float> disponi(const std::vector<float>& lato, float* totale = nullptr) const {
        std::vector<float> x(voci.size());
        float tot = 0;
        for (size_t i = 0; i < voci.size(); i++) {
            if (i > 0) tot += GAP * (float)voci[i].scala;
            if (i == primaNonFissata && i > 0) tot += larghezzaSeparatore();
            tot += lato[i] * (float)voci[i].scala;
        }
        float cur = W / 2.0f - tot / 2.0f;
        for (size_t i = 0; i < voci.size(); i++) {
            if (i > 0) cur += GAP * (float)voci[i].scala;
            if (i == primaNonFissata && i > 0) cur += larghezzaSeparatore();
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
        const bool scuro = temaScuro();
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
                premuta = -1; sopra = -1;
            } else i++;
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
        const bool giu = spostamento || (vuoleNascondersi && !rivelato && t > rivelaFino && !lAperto);
        if (giu) movimento |= molla(yNascosto, velNascosto, fuori, 260, 0.9);
        else movimento |= molla(yNascosto, velNascosto, 0, 340, 0.42);
        const bool giuI = spostamento || (vuoleNascondersiI && !rivelatoI && nessunaScheda());
        if (giuI) movimento |= molla(yNascostoI, velNascostoI, fuori, 260, 0.9);
        else movimento |= molla(yNascostoI, velNascostoI, 0, 340, 0.42);
        if (vuoleNascondersi && t <= rivelaFino) movimento = true;   // allo scadere della comparsa temporanea deve riscendere

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
        // fondo del dock, sopra al vetro acrilico e con la sua stessa forma. Mentre risale si allunga con la
        // velocita' e all'arrivo si schiaccia un poco
        const float allunga = (float)std::clamp(-velNascosto * 0.010, -3.0 * dpi, 7.0 * dpi);
        const float baseT = std::round(yb - S - PAD - allunga), fondoB = std::round(yb + PAD);
        const float fondoL = std::round(sinistra - PAD + (float)std::min(0.0, tiroX));
        const float fondoR = std::round(sinistra + tot + PAD + (float)std::max(0.0, tiroX));
        const float fondoT = std::round(baseT - (float)tiroY);
        disegnaFondo({fondoL, fondoT, fondoR, fondoB}, scuro);
        aggiornaVetro(vetro, rettVetro, fondoL, fondoT, fondoR, fondoB);
        zonaDockL = fondoL; zonaDockR = fondoR;

        // icone
        for (size_t i = 0; i < voci.size(); i++) {
            Voce& v = voci[i];
            const float sc = (float)v.scala, d = dim[i] * sc, x = xs[i];
            if (i == primaNonFissata && i > 0) {
                const float sep = larghezzaSeparatore(), sx = x - (sep + GAP * sc) / 2;
                pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.22f * sep / (GAP * 2 + 1)));
                dc->DrawLine({sx, yb - S * 0.85f}, {sx, yb - S * 0.15f}, pennello, 1.0f);
            }
            float salto = 0;
            if (v.rimbalzoT0 >= 0) {
                const double e = t - v.rimbalzoT0;
                if (e > 3.0) v.rimbalzoT0 = -1;
                else { salto = (float)(std::abs(std::sin(e * M_PI / 0.55)) * S * 0.45 * std::exp(-e * 0.35)); movimento = true; }
            }
            const float premuto = (premuta == (int)i) ? 0.86f : 1.0f;   // un attimo piu' scura e piccola mentre la premi
            const float dd = d * premuto;
            D2D1_RECT_F r = {x + (d - dd) / 2, yb - dd - salto, x + (d - dd) / 2 + dd, yb - salto};
            v.rett = {x, yb - d, x + d, yb};
            if (d < 0.5f) continue;
            const float alfa = (premuto < 1 ? 0.8f : 1.0f) * std::min(1.0f, sc * 1.5f);
            if (v.icona) dc->DrawBitmap(v.icona, r, alfa, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else if (v.avvio == AVVIO_LAUNCHER) disegnaIconaApp(r, alfa, scuro);
            else if (v.avvio == AVVIO_CONTROLLO) disegnaIconaControllo(r, alfa, scuro);
            else {
                pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.9f * alfa));
                dc->FillRoundedRectangle({r, dd * 0.22f, dd * 0.22f}, pennello);
            }
            // pallino delle app aperte
            // (anche sotto App e il centro di controllo mentre sono aperti)
            if (!v.finestre.empty() || (v.avvio == AVVIO_CONTROLLO && !nessunaScheda()) || (v.avvio == AVVIO_LAUNCHER && lAperto && !lChiusura)) {
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.85f * sc) : D2D1::ColorF(0, 0, 0, 0.62f * sc));
                dc->FillEllipse({{x + d / 2, yb + PAD * 0.55f}, 2.0f * dpi, 2.0f * dpi}, pennello);
            }
        }

        // icona sotto il mouse ricalcolata sulle posizioni appena disegnate (il dock si muove anche col mouse fermo)
        sopra = (mouseDentro && mouseSulDock) ? iconaInPunto(mouseX, mouseY) : -1;

        // (l'isola di stato in basso a destra non c'e' piu': il centro di controllo e' l'ultima icona del dock e l'ora sta
        // nel notch in alto al centro)

        // etichetta: nome dell'app, oppure descrizione dell'elemento di stato
        bool etichettaVoluta = false;
        if (sopra >= 0 && premuta < 0) {
            const Voce& v = voci[sopra];
            etichettaTesto = v.nome; etichettaCx = (v.rett.left + v.rett.right) / 2; etichettaTop = v.rett.top - 12 * dpi;
            etichettaVoluta = true;
        } else if (statoSopra >= 0 && statoPremuto < 0 && nessunaScheda()) {   // con un pannello aperto l'etichetta lo coprirebbe
            etichettaTesto = descrizioneStato(statoSopra);
            etichettaCx = (rettStato[statoSopra].left + rettStato[statoSopra].right) / 2; etichettaTop = isola.top - 10 * dpi;
            etichettaVoluta = true;
        }
        const double et = verso(etichettaAlfa, etichettaVoluta ? 1.0 : 0.0, 0.025);
        if (et != etichettaAlfa) movimento = true;
        etichettaAlfa = et;
        if (etichettaAlfa > 0.01 && !etichettaTesto.empty()) disegnaEtichetta(scuro);
        controllaGpu(dc->EndDraw());
        inMovimento = movimento;

        // la regione della finestra limita i clic al dock (con le icone ingrandite) e all'isola di stato
        float regTop = fondoT;
        for (auto& v : voci) regTop = std::min(regTop, v.rett.top);
        // solo se cambia: ogni SetWindowRgn genera un WM_MOUSEMOVE, che farebbe ridisegnare il dock all'infinito
        // la regione ritaglia anche il disegno: l'etichetta, finche' e' visibile, deve starci dentro
        // (dock nascosto: regioni vuote, nessun clic rubato alle finestre a schermo intero)
        const RECT rd = {(int)fondoL - 2, std::min((int)regTop - 2, H), (int)fondoR + 2, H};
        const RECT rs = isola.right > isola.left ? RECT{(int)isola.left - 2, std::min((int)isola.top - 2, H), (int)isola.right + 2, H} : RECT{};
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

    // velo chiaro, bordo luminoso e sottile contorno scuro: stessa forma del vetro sotto
    void disegnaFondo(const D2D1_RECT_F& f, bool scuro) {
        pennello->SetColor(scuro ? D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.35f) : D2D1::ColorF(1, 1, 1, 0.38f));
        dc->FillRoundedRectangle({f, RAGGIO, RAGGIO}, pennello);
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(1, 1, 1, 0.75f));
        dc->DrawRoundedRectangle({{f.left + 0.5f, f.top + 0.5f, f.right - 0.5f, f.bottom - 0.5f}, RAGGIO - 0.5f, RAGGIO - 0.5f}, pennello, 1.0f);
        pennello->SetColor(D2D1::ColorF(0, 0, 0, scuro ? 0.45f : 0.10f));
        dc->DrawRoundedRectangle({{f.left - 0.5f, f.top - 0.5f, f.right + 0.5f, f.bottom + 0.5f}, RAGGIO + 0.5f, RAGGIO + 0.5f}, pennello, 1.0f);
    }

    void disegnaEtichetta(bool scuro) {
        const float a = (float)etichettaAlfa;
        IDWriteTextLayout* lay = nullptr;
        if (FAILED(fdw->CreateTextLayout(etichettaTesto.c_str(), (UINT32)etichettaTesto.size(), testo, 800, 40, &lay))) return;
        DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm);
        const float lw = tm.widthIncludingTrailingWhitespace + 22 * dpi, lh = 28 * dpi;
        // dentro lo schermo anche per gli elementi vicino al bordo destro
        const float cx = std::clamp(etichettaCx, lw / 2 + 6 * dpi, W - lw / 2 - 6 * dpi), top = etichettaTop - lh;
        D2D1_RECT_F rr = {cx - lw / 2, top, cx + lw / 2, top + lh};
        rettEtichetta = rr;
        pennello->SetColor(scuro ? D2D1::ColorF(0.18f, 0.18f, 0.2f, 0.92f * a) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.95f * a));
        dc->FillRoundedRectangle({rr, 7 * dpi, 7 * dpi}, pennello);
        pennello->SetColor(D2D1::ColorF(0, 0, 0, 0.15f * a));
        dc->DrawRoundedRectangle({rr, 7 * dpi, 7 * dpi}, pennello, 1.0f);
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, a) : D2D1::ColorF(0.1f, 0.1f, 0.1f, a));
        lay->SetMaxWidth(lw); lay->SetMaxHeight(lh);
        dc->DrawTextLayout({rr.left, rr.top}, lay, pennello);
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
                dc->FillRoundedRectangle({{cx - hw, cy - hh, cx + hw, cy + hh}, RAGGIO - 2 * dpi, RAGGIO - 2 * dpi}, pennello);
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
        ID2D1GeometrySink* s = nullptr; g->Open(&s);
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
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(f2d->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr; g->Open(&s);
            auto P = [&](float x, float y) { return D2D1::Point2F(x0 + x * dpi, cy + y * dpi); };
            s->BeginFigure(P(-6, -2.8f), D2D1_FIGURE_BEGIN_FILLED);
            D2D1_POINT_2F pt[] = {P(-3, -2.8f), P(1.5f, -6.5f), P(1.5f, 6.5f), P(-3, 2.8f), P(-6, 2.8f)};
            s->AddLines(pt, 5);
            s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
            dc->FillGeometry(g, pennello);
            dc->DrawGeometry(g, pennello, 1.0f * dpi, tratto);   // angoli ammorbiditi
            g->Release();
        }
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
            if (GetDateFormatEx(loc, 0, &st, testi::t(L"dddd d MMMM", L"dddd, MMMM d"), b, 96, nullptr)) { testoNotch = b; if (!testoNotch.empty()) testoNotch[0] = (wchar_t)towupper(testoNotch[0]); }
            notch.testi(testoOra, testoNotch);   // il notch mostra ora e data
        }
        const float v0 = volume; const bool m0 = muto, ok0 = audioOk;
        leggiVolume();
        cambiato |= (std::abs(v0 - volume) > 0.004f || m0 != muto || ok0 != audioOk);
        if (tutto || ++conteggioTimer % 4 == 0) {
            const int r0 = rete; leggiRete(); cambiato |= r0 != rete;
        }
        usa(0);   // controllo, widget: la scheda 0
        if (cambiato) { dirty = true; if (pannelloTipo == P_CONTROLLO) pDirty = true; }
        if (pannelloTipo == P_CONTROLLO && !pChiusura) {   // l'uscita audio puo' cambiare (cuffie collegate)
            const std::wstring u0 = nomeUscita; leggiUscita(); if (u0 != nomeUscita) pDirty = true;
        }
        if (widD.pronto.exchange(false) && pannelloTipo == P_WIDGET) { wid = widD.copia(); pDirty = true; }
        // radio e luminosita' lette dal thread: il pannello si ridisegna (e si allunga se la luminosita' e' regolabile)
        if (sis.pronto.exchange(false) && pannelloTipo == P_CONTROLLO && !pChiusura) {
            const float nuova = altezzaControllo();
            if (nuova != PH) { PH = nuova; rettPannello.top = rettPannello.bottom - (int)PH; posizionaPannello(pK < 0 ? 1 : pK); }
            pDirty = true;
        }
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
            case ORA:      apriPannello(P_NOTIFICHE); break;   // notifiche e calendario, nostri (non piu' quello di Windows)
        }
    }

    // ---- pannelli in stile iOS (al posto di quelli di Windows, che si agganciano alla barra e la fanno risalire) ----
    enum { P_NESSUNO, P_SFONDO, P_CONTROLLO, P_NOTIFICHE, P_WIDGET };
    widget::Dati widD;                   // meteo e notizie (scaricati in un thread a parte)
    widget::Istantanea wid;              // l'ultima copia, per il disegno
    IDWriteTextFormat* fmtNotizia = nullptr;
    enum Azione { A_APP, A_TUTTE, A_RETE, A_BLUETOOTH, A_VOLUME, A_MUTO, A_AUDIO, A_SCHERMO, A_ENERGIA, A_IMPOSTAZIONI,
                  A_NOTIFICA, A_RIMUOVI, A_CANCELLA, A_IMPOST_NOTIFICHE, A_TEMA, A_FLUIDWIN, A_LUMINOSITA,
                  A_WIDGET, A_SFONDO, A_NOTIZIA, A_NOTIFICHE, A_INDIETRO };
    sistema::Sistema sis;                // Wi-Fi, Bluetooth e luminosita' (letti e cambiati in un thread a parte)
    double temaK = -1;                   // simbolo del tema: 0 sole (chiaro) .. 1 luna (scuro), animato
    bool trascinaLum = false;
    // notifiche (pannello dell'ora)
    notifiche::Lettore lettoreN;
    std::vector<notifiche::Notifica> listaN;
    bool permessoN = false;
    std::unordered_map<std::wstring, ID2D1Bitmap1*> iconeN;   // per AppUserModelID (nullptr = provata, nessuna icona)
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

    float altezzaControllo() const;

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
    void piastrella(const D2D1_RECT_F& r, Azione a, bool acceso, const std::wstring& titolo, const std::wstring& sotto,
                    bool scuro, D2D1_COLOR_F ink) {
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
        else simboloGoccia(cx, cy);
        pennello->SetColor(ink);
        scrivi(titolo, fmtVoce, {cx + 26 * dpi, cy - 18 * dpi, r.right - 8 * dpi, cy});
        pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.62f});
        scrivi(sotto, fmtSotto, {cx + 26 * dpi, cy, r.right - 8 * dpi, cy + 17 * dpi});
    }

    void disegnaControllo(bool scuro, D2D1_COLOR_F ink);

    // ---- widget: meteo e notizie ----
    void disegnaWidget(bool scuro, D2D1_COLOR_F ink);

    void simboloMeteo(int c, float cx, float cy, float s);

    void simboloBluetooth(float cx, float cy);

    void simboloTema(float cx, float cy);

    void simboloLuna(float cx, float cy);

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

    // il vetro acrilico segue il fondo, con la stessa forma
    void aggiornaVetro(HWND h, RECT& ultimo, float l, float t, float r, float b) {
        RECT nuovo = {schermo.left + (int)l, schermo.bottom - H + (int)t, schermo.left + (int)r, schermo.bottom - H + (int)b};
        if (!EqualRect(&nuovo, &ultimo)) {
            ultimo = nuovo;
            SetWindowPos(h, icone, nuovo.left, nuovo.top, nuovo.right - nuovo.left, nuovo.bottom - nuovo.top, SWP_NOACTIVATE | SWP_NOREDRAW);
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
    volatile LONG fineIcone = 0;
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

    // apre le impostazioni di FluidWin (dal centro di controllo)
    void (*impostazioni)() = nullptr;

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
        const int i = iconaInPunto((float)(pt.x - schermo.left), (float)(pt.y - (schermo.bottom - H)));
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
        if (v.finestre.empty()) {
            segnaLancio(v);   // la sua prima finestra uscira' dall'icona col genio
            ShellExecuteW(nullptr, L"open", v.avvio.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
        if (scelta) {
            if (!ShowWindow(scelta, SW_RESTORE) && IsIconic(scelta)) {
                SwitchToThisWindow(scelta, TRUE);
                if (registro) scrivi("clic dock: finestra di un'app amministratore, ripristinata come fa Alt+Tab");
            } else SetForegroundWindow(scelta);
            return;
        }
        // la finestra piu' in alto fra quelle dell'app
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
            if (std::find(v.finestre.begin(), v.finestre.end(), h) != v.finestre.end()) { scelta = h; break; }
        if (scelta && !SetForegroundWindow(scelta)) SwitchToThisWindow(scelta, TRUE);
    }

    void menu(int i, POINT pt) {
        if (i < 0 || i >= (int)voci.size() || voci[i].uscente) return;
        // copia: mentre il menu e' aperto (ha un suo ciclo dei messaggi) l'elenco delle app puo' essere rifatto da
        // capo, e un riferimento a voci[i] resterebbe a puntare a memoria liberata
        const Voce v = voci[i];
        if (v.avvio == AVVIO_LAUNCHER || v.avvio == AVVIO_CONTROLLO) return;
        // eseguibile vero sul disco (non quelli protetti delle app dello Store)
        const bool exeVero = !v.exe.empty() && v.exe.find(L"\\windowsapps\\") == std::wstring::npos &&
                             GetFileAttributesW(v.exe.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool store = v.avvio.rfind(L"shell:", 0) == 0;
        const bool collegamento = v.fissata && v.avvio.size() > 4 && minuscolo(v.avvio.substr(v.avvio.size() - 4)) == L".lnk";
        std::vector<menu::Voce> voci = {menu::titolo(v.nome), menu::voce(testi::t(L"Apri nuova finestra", L"Open new window"), 1)};
        if (!store || exeVero) voci.push_back(menu::voce(testi::t(L"Esegui come amministratore", L"Run as administrator"), 3));
        if (exeVero) voci.push_back(menu::voce(testi::t(L"Apri percorso file", L"Open file location"), 4));
        if (collegamento) { voci.push_back(menu::separatore()); voci.push_back(menu::voce(testi::t(L"Rimuovi dal dock", L"Remove from dock"), 5)); }
        else if (!v.fissata) { voci.push_back(menu::separatore()); voci.push_back(menu::voce(testi::t(L"Aggiungi al dock", L"Add to dock"), 6)); }
        if (!v.finestre.empty()) {
            voci.push_back(menu::separatore());
            voci.push_back(menu::voce(v.finestre.size() > 1 ? testi::t(L"Chiudi tutte le finestre", L"Close all windows") : testi::t(L"Chiudi", L"Close"), 2));
        }
        SetForegroundWindow(icone);   // cosi' la tastiera (Esc, frecce) arriva al menu
        // sopra il dock, centrato sull'icona (il punto e' quello del clic)
        const int s = (int)menu::mostra(voci, {pt.x - (LONG)(100 * dpi), pt.y - (LONG)(14 * dpi)}, icone, true);
        if (s == 1) {
            segnaLancio(v); ShellExecuteW(nullptr, L"open", v.avvio.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            for (auto& x : this->voci) if (!x.uscente && chiave(x) == chiave(v)) x.rimbalzoT0 = ultimoT;   // la voce di adesso
            dirty = true;
        }
        else if (s == 2) { for (HWND h : v.finestre) if (IsWindow(h)) PostMessageW(h, WM_CLOSE, 0, 0); }
        else if (s == 3) ShellExecuteW(nullptr, L"runas", (collegamento || !exeVero ? v.avvio : v.exe).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (s == 4) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + v.exe + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else if (s == 5) { DeleteFileW(v.avvio.c_str()); appDaAggiornare = true; dirty = true; }   // come "Rimuovi dalla barra"
        else if (s == 6) aggiungiAlDock(v.nome, exeVero ? v.exe : L"", v.aumid, v.avvio);
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
                if (d->mouseDentro && d->mouseX == (float)GET_X_LPARAM(l) && d->mouseY == (float)GET_Y_LPARAM(l)) return 0;
                d->mouseDentro = true;
                d->mouseX = (float)GET_X_LPARAM(l); d->mouseY = (float)GET_Y_LPARAM(l);
                const bool isola = d->sullIsola(d->mouseX, d->mouseY);
                d->mouseSulDock = !isola;
                d->statoSopra = isola ? d->statoInPunto(d->mouseX, d->mouseY) : -1;
                d->dirty = true;
                return 0;
            }
            case WM_MOUSELEAVE:
                d->mouseDentro = false; d->mouseSulDock = false; d->sopra = -1; d->premuta = -1;
                d->statoSopra = -1; d->statoPremuto = -1; d->dirty = true;
                return 0;
            case WM_LBUTTONDOWN: {
                const float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
                if (d->sullIsola(x, y)) d->statoPremuto = d->statoInPunto(x, y);
                else d->premuta = d->iconaInPunto(x, y);
                d->dirty = true; return 0;
            }
            case WM_LBUTTONUP: {
                const float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
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
                const int iv = d->iconaInPunto((float)pt.x, (float)pt.y);
                if (iv >= 0 && d->voci[iv].avvio == AVVIO_CONTROLLO) d->cambiaVolume(GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA * 0.02f);
                return 0;
            }
            case WM_TIMER:
                if (w == TIMER_BORDO) { d->controllaBordo(); return 0; }
                if (w == TIMER_DOPO) { d->suTimerDopo(); return 0; }
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
                if (d->sullIsola((float)pt.x, (float)pt.y)) return 0;
                const int i = d->iconaInPunto((float)pt.x, (float)pt.y);
                ClientToScreen(h, &pt); d->menu(i, pt); return 0;
            }
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_CLOSE: return 0;   // (le finestre del dock restano)
            case WM_ICONA_PRONTA: d->raccogliIcone(); return 0;
            case WM_LAVORO_FATTO: { Sottofondo* s = (Sottofondo*)l; s->applica(); delete s; return 0; }
            case WM_DISPLAYCHANGE: d->cambiaSchermo(); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

} // namespace dock

// i metodi del centro di controllo e del launcher, in file a parte
#include "centro.h"
#include "launcher.h"
