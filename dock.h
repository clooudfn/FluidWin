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

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

namespace dock {

static const UINT WM_APPBAR = WM_APP + 20;
static const UINT_PTR TIMER_INVIO = 7;   // "icone nascoste": Invio poco dopo Win+B
static const UINT_PTR TIMER_BORDO = 8;   // dock nascosto: controlla se il mouse arriva sul bordo in basso
static const wchar_t* const AVVIO_LAUNCHER = L"*launcher";   // la voce "App" del dock

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
        ShowWindow(vetroStato, SW_SHOWNOACTIVATE);
        ShowWindow(vetroOra, SW_SHOWNOACTIVATE);
        ShowWindow(icone, SW_SHOWNOACTIVATE);
        dirty = true;
        return true;
    }

    void chiudi() {
        fermaIcone();
        if (!attivo) return;
        attivo = false;
        APPBARDATA tb{}; tb.cbSize = sizeof(tb); tb.lParam = statoBarraPrima;
        SHAppBarMessage(ABM_SETSTATE, &tb);
        ShowWindow(icone, SW_HIDE); ShowWindow(vetro, SW_HIDE); ShowWindow(vetroStato, SW_HIDE); ShowWindow(vetroOra, SW_HIDE);
        nascondiSchede(); nascondiLauncher();
    }

    // Da chiamare quando cambiano le finestre aperte (lo fa FluidWin con gli eventi)
    void segnaCambiamento() { appDaAggiornare = true; }

    // true se il dock deve disegnare il prossimo fotogramma (ingrandimento, rimbalzo, etichetta in movimento)
    bool serveFotogramma() const { return attivo && !nascostoSchermoIntero && !perso && (dirty || inMovimento || pannelloDaDisegnare() || launcherDaDisegnare()); }

    // Schermi cambiati (risoluzione dello schermo principale, monitor collegato o tolto): il dock torna in basso sullo
    // schermo principale con le misure nuove (prima restava dove e grande com'era all'avvio)
    void cambiaSchermo() {
        monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(monitor, &mi)) return;
        schermo = mi.rcMonitor;
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
        if (registro) { char b[96]; snprintf(b, 96, "schermi cambiati: dock su %ld,%ld-%ld,%ld", schermo.left, schermo.top, schermo.right, schermo.bottom); scrivi(b); }
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
            if (adesso() - std::max(ta, tb) > 0.6) chiudiTutte();
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
    void controllaFinestre() {
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
        monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
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
        if (n == vuoleNascondersi && nI == vuoleNascondersiI) return;
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
        for (HWND h : {icone, vetro, vetroStato, vetroOra}) ShowWindow(h, si ? SW_HIDE : SW_SHOWNOACTIVATE);
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
    std::wstring testoOra, testoData, testoDataLunga;
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
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) return false;
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
            if (!v.icona && v.avvio != AVVIO_LAUNCHER) {
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
    void controllaBordo() {
        POINT pt; GetCursorPos(&pt);
        const float x = (float)(pt.x - schermo.left);
        // sul bordo in basso le zone sono solo due: sotto il dock (risale il dock) e da sotto l'isola di stato fino
        // all'angolo destro (risale l'isola). Lo spazio vuoto fra i due e i lati non fanno nulla
        const bool zonaIsola = x >= isola.left - 10 * dpi;
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
        const bool giu = vuoleNascondersi && !rivelato && t > rivelaFino && !lAperto;
        if (giu) movimento |= molla(yNascosto, velNascosto, fuori, 260, 0.9);
        else movimento |= molla(yNascosto, velNascosto, 0, 340, 0.42);
        const bool giuI = vuoleNascondersiI && !rivelatoI && nessunaScheda();
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
            else {
                pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.9f * alfa));
                dc->FillRoundedRectangle({r, dd * 0.22f, dd * 0.22f}, pennello);
            }
            // pallino delle app aperte
            if (!v.finestre.empty()) {
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.85f * sc) : D2D1::ColorF(0, 0, 0, 0.62f * sc));
                dc->FillEllipse({{x + d / 2, yb + PAD * 0.55f}, 2.0f * dpi, 2.0f * dpi}, pennello);
            }
        }

        // icona sotto il mouse ricalcolata sulle posizioni appena disegnate (il dock si muove anche col mouse fermo)
        sopra = (mouseDentro && mouseSulDock) ? iconaInPunto(mouseX, mouseY) : -1;

        {   // l'isola di stato ha la sua altezza (nascosta e rivelata per conto suo)
            const float ybI = yBaseRiposo() + (float)yNascostoI;
            const float allungaI = (float)std::clamp(-velNascostoI * 0.010, -3.0 * dpi, 7.0 * dpi);
            movimento |= disegnaIsola(scuro, std::round(ybI - S - PAD - allungaI), std::round(ybI + PAD));
        }

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
        const RECT rs = {(int)isola.left - 2, std::min((int)isola.top - 2, H), (int)isola.right + 2, H};
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
                std::wstring s = rete == 0 ? L"Nessuna connessione" : nomeRete;
                if (!audioOk) return s + L" \u00b7 nessun audio";
                if (muto) return s + L" \u00b7 audio disattivato";
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
            if (GetDateFormatEx(L"it-IT", 0, &st, L"ddd d MMM", b, 96, nullptr)) testoData = b;
            if (GetDateFormatEx(L"it-IT", 0, &st, L"dddd d MMMM yyyy", b, 96, nullptr)) { testoDataLunga = b; if (!testoDataLunga.empty()) testoDataLunga[0] = (wchar_t)towupper(testoDataLunga[0]); }
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
            if (tipo > rete || rete == 0) { rete = tipo; nomeRete = a->FriendlyName ? a->FriendlyName : L"Rete"; }
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
                  A_WIDGET, A_SFONDO, A_NOTIZIA };
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
    void scambiaScheda() {
        Scheda& a = altra;
        std::swap(pannello, a.pannello); std::swap(vetroPannello, a.vetroPannello); std::swap(scP, a.scP); std::swap(targetP, a.targetP);
        std::swap(visualP, a.visualP); std::swap(effettoP, a.effettoP); std::swap(bersaglioP, a.bersaglioP);
        std::swap(pannelloTipo, a.pannelloTipo); std::swap(pannelloChiuso, a.pannelloChiuso);
        std::swap(pDirty, a.pDirty); std::swap(pAnimazione, a.pAnimazione); std::swap(pChiusura, a.pChiusura); std::swap(pMouseDentro, a.pMouseDentro);
        std::swap(trascinaVolume, a.trascinaVolume); std::swap(trascinaLum, a.trascinaLum);
        std::swap(pT0, a.pT0); std::swap(pChiusoT, a.pChiusoT); std::swap(pK, a.pK); std::swap(PW, a.PW); std::swap(PH, a.PH);
        std::swap(rettPannello, a.rettPannello); std::swap(elementi, a.elementi); std::swap(evidP, a.evidP);
        std::swap(pSopra, a.pSopra); std::swap(pPremuto, a.pPremuto);
        std::swap(posDietro, a.posDietro); std::swap(posObj, a.posObj); std::swap(gira, a.gira); std::swap(davanti, a.davanti); std::swap(chiudiAlle, a.chiudiAlle);
    }
    void usa(int slot) { if (slot != slotCorrente) { scambiaScheda(); slotCorrente = slot; } }
    static int slotDiTipo(int tipo) { return tipo == P_NOTIFICHE ? 1 : 0; }
    int slotDiFinestra(HWND h) const { return (h == pannello || h == vetroPannello) ? slotCorrente : 1 - slotCorrente; }
    bool schedaFinestra(HWND h) const { return h && (h == pannello || h == vetroPannello || h == altra.pannello || h == altra.vetroPannello); }
    bool nessunaScheda() const { return pannelloTipo == P_NESSUNO && altra.pannelloTipo == P_NESSUNO; }
    std::wstring nomeUscita;             // dispositivo audio in uso
    std::unordered_map<std::wstring, ID2D1Bitmap*> cacheIcone;

    static double adesso() {
        static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart / f;
    }

    bool pannelloDaDisegnare() const {
        return (pannelloTipo != P_NESSUNO && (pDirty || pAnimazione)) || (altra.pannelloTipo != P_NESSUNO && (altra.pDirty || altra.pAnimazione));
    }

    bool creaPannello(HINSTANCE inst) {
        PWMAX = std::ceil(336 * dpi); PHMAX = std::ceil(760 * dpi);
        WNDCLASSW wp{}; wp.lpfnWndProc = procPannello; wp.hInstance = inst; wp.lpszClassName = L"FluidWin.Pannello";
        wp.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wp);
        pannello = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wp.lpszClassName, L"FluidWin",
                                   WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
        vetroPannello = creaVetro(inst);
        BOOL niente = TRUE; DwmSetWindowAttribute(pannello, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fab = nullptr;
        dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd); dd->GetAdapter(&ad); ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fab);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = (UINT)PWMAX; sd.Height = (UINT)PHMAX; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        const HRESULT hr = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &scP);
        fab->Release(); ad->Release(); dd->Release();
        if (FAILED(hr)) return false;
        dcomp->CreateTargetForHwnd(pannello, TRUE, &targetP);
        dcomp->CreateVisual(&visualP); visualP->SetContent(scP);
        dcomp->CreateEffectGroup(&effettoP); visualP->SetEffect(effettoP);   // dissolvenza in apertura e chiusura
        targetP->SetRoot(visualP); dcomp->Commit();
        IDXGISurface* sup = nullptr; scP->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglioP); sup->Release();
        return bersaglioP != nullptr;
    }

    void apriPannello(int tipo) {
        const int slot = slotDiTipo(tipo);
        usa(1 - slot);
        const bool altraAperta = pannelloTipo != P_NESSUNO && !pChiusura;
        usa(slot);
        if (pannelloTipo == tipo && !pChiusura) {
            if (!davanti && altraAperta) { mescola(slot); return; }   // era dietro: torna davanti girando da sinistra
            chiudiPannello(); return;                                   // era davanti: si chiude
        }
        // il clic sul pulsante ha appena chiuso lo stesso pannello (perdendo il fuoco): resta chiuso, come un interruttore
        if (pannelloChiuso == tipo && adesso() - pChiusoT < 0.3) return;
        if (tipo == P_SFONDO) leggiAppSfondo();
        if (tipo == P_CONTROLLO) { leggiUscita(); sis.avvia(); sis.rileggi(); }
        pannelloTipo = tipo; pChiusura = false; pT0 = adesso(); pK = -1;
        pSopra = pPremuto = -1; evidP.clear(); trascinaVolume = false; pMouseDentro = false;
        PW = PWMAX;
        if (tipo == P_SFONDO) {
            PH = altezzaSfondo();   // con l'elenco di adesso; quello nuovo arriva dal thread e la corregge
        } else if (tipo == P_WIDGET) {
            widD.aggiorna(); wid = widD.copia();
            PH = std::round(560 * dpi);
        } else if (tipo == P_NOTIFICHE) {
            Cronometro cr(this, "notifiche");
            permessoN = lettoreN.leggi(listaN);
            PH = altezzaNotifiche();
        } else PH = altezzaControllo();
        // sopra l'isola, allineato al suo bordo destro
        const int x1 = schermo.left + (int)isola.right, y1 = schermo.bottom - H + (int)isola.top - (int)std::round(10 * dpi);
        rettPannello = {x1 - (int)PW, y1 - (int)PH, x1, y1};
        posDietro = posObj = 0; gira = false; davanti = true; chiudiAlle = -1;
        posizionaPannello(0);
        ShowWindow(vetroPannello, SW_SHOWNOACTIVATE);
        ShowWindow(pannello, SW_SHOW);
        // l'altra carta, se e' aperta, va dietro: un po' piu' piccola, se ne vede solo il bordo sinistro
        if (altraAperta) { usa(1 - slot); davanti = false; posObj = 1; gira = false; chiudiAlle = -1; pAnimazione = true; usa(slot); }
        SetForegroundWindow(pannello);   // permesso: l'ultimo clic e' arrivato al nostro processo
        ordinaSchede(); usa(slot);
        pDirty = true;
    }

    // La carta dietro torna davanti: esce a sinistra, passa sopra l'altra a meta' strada e si rimette al suo posto
    // (in senso antiorario); l'altra intanto scivola dietro
    void mescola(int slot) {
        usa(slot);
        if (pannelloTipo == P_NESSUNO || pChiusura || davanti) return;
        posObj = 0; gira = true; pAnimazione = true; pDirty = true; chiudiAlle = -1;
        usa(1 - slot); posObj = 1; gira = false; pAnimazione = true;
        usa(slot);
        SetForegroundWindow(pannello);
        ordinaSchede(); usa(slot);   // l'attivazione la porterebbe subito in cima: resta sotto fino a meta' giro
    }

    // ordine delle finestre: la carta davanti (contenuto, poi il suo vetro), poi quella dietro
    void ordinaSchede() {
        usa(0); const bool primaZero = davanti || altra.pannelloTipo == P_NESSUNO;
        HWND c0 = pannello, v0 = vetroPannello, c1 = altra.pannello, v1 = altra.vetroPannello;
        HWND a = primaZero ? c0 : c1, av = primaZero ? v0 : v1, b = primaZero ? c1 : c0, bv = primaZero ? v1 : v0;
        const UINT f = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        SetWindowPos(a, HWND_TOPMOST, 0, 0, 0, 0, f);
        SetWindowPos(av, a, 0, 0, 0, 0, f);
        SetWindowPos(b, av, 0, 0, 0, 0, f);
        SetWindowPos(bv, b, 0, 0, 0, 0, f);
    }

    // a ogni fotogramma: le carte vanno verso la loro posa, partono le chiusure ritardate, chi gira passa sopra
    void animaSchede() {
        const double ora = adesso();
        for (int s : {0, 1}) {
            usa(s);
            if (pannelloTipo == P_NESSUNO) continue;
            if (chiudiAlle > 0 && ora >= chiudiAlle) { chiudiAlle = -1; chiudiPannello(); usa(s); }
            if (chiudiAlle > 0) pAnimazione = true;   // in attesa della sua chiusura
            if (posDietro != posObj) {
                const double passo = dtFotogramma / 0.30;
                posDietro = std::abs(posObj - posDietro) <= passo ? posObj : posDietro + (posObj > posDietro ? passo : -passo);
                pAnimazione = true;
                if (posDietro == posObj) gira = false;
            }
        }
        for (int s : {0, 1}) {
            usa(s);
            if (pannelloTipo == P_NESSUNO || !gira || davanti || posDietro > 0.5) continue;
            davanti = true; usa(1 - s); davanti = false;   // a meta' giro passa sopra
            ordinaSchede();
            break;
        }
    }

    // chiusura di tutte e due (clic altrove, Esc): prima quella davanti, l'altra poco dopo, ognuna con la sua animazione
    void chiudiTutte() {
        int fronte = -1;
        for (int s : {0, 1}) { usa(s); if (pannelloTipo != P_NESSUNO && !pChiusura && davanti) fronte = s; }
        for (int s : {0, 1}) {
            usa(s);
            if (pannelloTipo == P_NESSUNO || pChiusura || s == fronte) continue;
            if (fronte < 0) chiudiPannello(); else { chiudiAlle = adesso() + 0.08; pAnimazione = true; }
        }
        if (fronte >= 0) { usa(fronte); chiudiPannello(); }
    }

    void nascondiSchede() { for (int s : {0, 1}) { usa(s); nascondiPannello(); } }

    void chiudiPannello() {
        if (pannelloTipo == P_NESSUNO || pChiusura) return;
        pChiusura = true; pT0 = adesso(); pDirty = true; chiudiAlle = -1;
        if (trascinaVolume || trascinaLum) { trascinaVolume = trascinaLum = false; ReleaseCapture(); }
        // si chiude quella davanti e l'altra resta: torna davanti al suo posto (senza giro)
        const int s = slotCorrente;
        usa(1 - s);
        if (pannelloTipo != P_NESSUNO && !pChiusura && chiudiAlle < 0 && !davanti) {
            posObj = 0; gira = false; davanti = true; pAnimazione = true;
            SetForegroundWindow(pannello);
        }
        usa(s);
    }

    float altezzaControllo() const { return std::round((370 + (sis.luminosita >= 0 ? 52 : 0)) * dpi); }

    void nascondiPannello() {
        if (pannello) ShowWindow(pannello, SW_HIDE);
        if (vetroPannello) ShowWindow(vetroPannello, SW_HIDE);
        if (pannelloTipo != P_NESSUNO) { pannelloChiuso = pannelloTipo; pChiusoT = adesso(); }
        pannelloTipo = P_NESSUNO; pChiusura = false; pAnimazione = false; pDirty = false;
        posDietro = posObj = 0; gira = false; davanti = true; chiudiAlle = -1;
    }

    // k = 0..1 (apertura). Il centro di controllo nasce piccolo sopra la sua pillola e cresce (senza piegarsi), le
    // notifiche entrano da destra. Da carta dietro: un po' piu' piccola e spostata a sinistra (se ne vede il bordo);
    // mentre torna davanti gira larga da sinistra. Il vetro copre esattamente il contenuto trasformato.
    void posizionaPannello(double k) {
        const RECT& r = rettPannello;
        const float pw = (float)(r.right - r.left), ph = (float)(r.bottom - r.top);
        D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
        float tx = 0;
        double alfa = std::min(1.0, k * 2.2);
        if (pannelloTipo == P_NOTIFICHE) { tx = (float)((1 - k) * 56 * dpi); alfa = std::clamp(k, 0.0, 1.0); }
        else {
            const float s = (float)(0.10 + 0.90 * k);
            const float ax = std::clamp((float)(schermo.left + (pillolaControllo.left + pillolaControllo.right) / 2 - r.left), 0.0f, pw);
            m = D2D1::Matrix3x2F::Scale(s, s, {ax, ph});   // dal bordo in basso, sopra la pillola
        }
        const float pd = (float)(posDietro * posDietro * (3 - 2 * posDietro));
        const float sb = 1 - 0.06f * pd;
        m = m * D2D1::Matrix3x2F::Scale(sb, sb, {pw / 2, ph / 2});
        tx += -40 * dpi * pd;
        if (gira) tx += -(float)(pw * 0.62 * std::sin(M_PI * posDietro));
        alfa *= 1 - 0.18 * pd;
        const int x = r.left + (int)std::lround(tx);
        SetWindowPos(pannello, nullptr, x, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_NOZORDER);
        visualP->SetTransform(m);
        const D2D1_POINT_2F a = m.TransformPoint({0, 0}), b = m.TransformPoint({pw, ph});
        SetWindowPos(vetroPannello, nullptr, x + (int)std::lround(a.x), r.top + (int)std::lround(a.y),
                     std::max(1, (int)std::lround(b.x - a.x)), std::max(1, (int)std::lround(b.y - a.y)), SWP_NOACTIVATE | SWP_NOZORDER);
        SetLayeredWindowAttributes(vetroPannello, 0, (BYTE)std::lround(std::clamp(alfa, 0.0, 1.0) * 255), LWA_ALPHA);
        effettoP->SetOpacity((float)std::clamp(alfa, 0.0, 1.0));
        dcomp->Commit();
    }

    // restituisce false se il pannello si e' appena chiuso (niente da presentare)
    bool renderPannello() {
        const double ora = adesso();
        double k;
        const bool cresce = pannelloTipo != P_NOTIFICHE;   // il centro di controllo cresce dalla pillola
        const bool posaInMoto = posDietro != posObj || chiudiAlle > 0;
        if (!pChiusura) {
            // (niente rimbalzo oltre 1: il contenuto uscirebbe dalla sua finestra e verrebbe tagliato)
            const double x = std::clamp((ora - pT0) / (cresce ? 0.25 : 0.19), 0.0, 1.0);
            k = 1 - std::pow(1 - x, cresce ? 4 : 3);
            pAnimazione = x < 1 || posaInMoto;
        } else {
            const double x = std::clamp((ora - pT0) / (cresce ? 0.17 : 0.13), 0.0, 1.0);
            k = 1 - x * x; pAnimazione = x < 1 || posaInMoto;
            if (x >= 1) { nascondiPannello(); return false; }
        }
        posizionaPannello(k); pK = k;

        const bool scuro = temaScuro();
        const D2D1_COLOR_F ink = scuro ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.88f);
        dc->SetTarget(bersaglioP);
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        disegnaFondo({0.5f, 0.5f, PW - 0.5f, PH - 0.5f}, scuro);
        elementi.clear();
        if (pannelloTipo == P_SFONDO) disegnaSfondo(scuro, ink);
        else if (pannelloTipo == P_NOTIFICHE) disegnaNotifiche(scuro, ink);
        else if (pannelloTipo == P_WIDGET) disegnaWidget(scuro, ink);
        else disegnaControllo(scuro, ink);
        controllaGpu(dc->EndDraw());
        dc->SetTarget(bersaglio);
        pDirty = false;
        return true;
    }

    int aggiungi(const D2D1_RECT_F& r, Azione a, int indice) { elementi.push_back({r, a, indice}); return (int)elementi.size() - 1; }

    // Menu sopra una scheda. Durante il suo ciclo dei messaggi l'altra scheda puo' ricevere messaggi e scambiare i
    // membri dei pannelli (usa): alla fine si torna sulla scheda di partenza
    UINT menuScheda(const std::vector<menu::Voce>& v, POINT pt, HWND h) {
        const int s = slotCorrente;
        const UINT r = menu::mostra(v, pt, h);
        usa(s);
        return r;
    }

    // sfondo morbido dell'elemento sotto il mouse (e piu' scuro mentre e' premuto)
    double evidenzia(int e, const D2D1_RECT_F& r, bool scuro, float raggio) {
        if ((int)evidP.size() <= e) evidP.resize(e + 1, 0.0);
        const double o = (e == pPremuto) ? 1.6 : (e == pSopra && pMouseDentro) ? 1.0 : 0.0;
        const double v = evidP[e] + (o - evidP[e]) * (1 - std::exp(-dtFotogramma / 0.05));
        evidP[e] = std::abs(v - o) < 0.01 ? o : v;
        if (evidP[e] != o) pAnimazione = true;
        if (evidP[e] > 0.01) {
            pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)evidP[e]) : D2D1::ColorF(0, 0, 0, 0.06f * (float)evidP[e]));
            dc->FillRoundedRectangle({r, raggio, raggio}, pennello);
        }
        return evidP[e];
    }

    void scrivi(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r) {
        dc->DrawText(s.c_str(), (UINT32)s.size(), f, r, pennello, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void chevronDestra(float x, float cy) { linea(-2, -4, 2, 0, x, cy, 1.8f); linea(2, 0, -2, 4, x, cy, 1.8f); }

    // "App in background": icone di notifica delle app in esecuzione, in una griglia come su iOS
    void disegnaSfondo(bool scuro, D2D1_COLOR_F ink) {
        const float m = 16 * dpi;
        pennello->SetColor(ink);
        scrivi(L"In background", fmtTitolo, {m, 12 * dpi, PW - m, 40 * dpi});
        const float tw = (PW - 2 * m) / 4, th = 86 * dpi, top = 48 * dpi;
        if (appSfondo.empty()) {
            pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.6f});
            scrivi(L"Nessuna app in background", fmtSotto, {m, top, PW - m, top + 40 * dpi});
        }
        for (size_t i = 0; i < appSfondo.size() && i < 24; i++) {
            const float x = m + (i % 4) * tw, y = top + (i / 4) * th;
            const D2D1_RECT_F r = {x + 2 * dpi, y, x + tw - 2 * dpi, y + th - 6 * dpi};
            const int e = aggiungi(r, A_APP, (int)i);
            evidenzia(e, r, scuro, 6 * dpi);
            const float s = (e == pPremuto) ? 0.9f : 1.0f;   // si "schiaccia" alla pressione
            const float cx = (r.left + r.right) / 2, cy = r.top + 30 * dpi, l = 22 * dpi * s;
            if (appSfondo[i].icona) dc->DrawBitmap(appSfondo[i].icona, D2D1::RectF(cx - l, cy - l, cx + l, cy + l), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else { pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.9f)); dc->FillRoundedRectangle({{cx - l, cy - l, cx + l, cy + l}, 10 * dpi, 10 * dpi}, pennello); }
            pennello->SetColor(ink);
            scrivi(appSfondo[i].nome, fmtNome, {r.left + 3 * dpi, cy + 26 * dpi, r.right - 3 * dpi, cy + 44 * dpi});
        }
        // piede: il pannello di Windows, per i menu col tasto destro delle singole app
        const float py = PH - 46 * dpi;
        pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.10f));
        dc->DrawLine({m, py}, {PW - m, py}, pennello, 1.0f);
        const D2D1_RECT_F rp = {m - 8 * dpi, py + 6 * dpi, PW - m + 8 * dpi, PH - 8 * dpi};
        evidenzia(aggiungi(rp, A_TUTTE, 0), rp, scuro, 6 * dpi);
        pennello->SetColor(ink);
        scrivi(L"Pannello di Windows", fmtVoce, {m, rp.top, PW - m - 20 * dpi, rp.bottom});
        chevronDestra(PW - m - 4 * dpi, (rp.top + rp.bottom) / 2);
    }

    // ---- notifiche e calendario (clic sull'ora) ----
    static constexpr float N_TESTA = 84, N_CAL = 176, N_TITOLO = 40, N_SCHEDA = 70, N_PIEDE = 48;
    int schedeVisibili() const {
        const int spazio = (int)((PHMAX / dpi - N_TESTA - N_CAL - N_TITOLO - N_PIEDE - 10) / N_SCHEDA);
        return std::max(1, std::min((int)listaN.size(), spazio));
    }
    float altezzaNotifiche() const {
        const float schede = (!permessoN || listaN.empty()) ? 56 : schedeVisibili() * N_SCHEDA + ((int)listaN.size() > schedeVisibili() ? 22 : 0);
        return std::min(PHMAX, std::round((N_TESTA + N_CAL + N_TITOLO + schede + N_PIEDE) * dpi));
    }

    // dopo aver tolto notifiche: elenco nuovo e pannello della nuova altezza (il bordo in basso resta fermo)
    void ricaricaNotifiche() {
        permessoN = lettoreN.leggi(listaN);
        const float nuova = altezzaNotifiche();
        if (nuova != PH) { PH = nuova; rettPannello.top = rettPannello.bottom - (int)PH; posizionaPannello(pK < 0 ? 1 : pK); }
        pSopra = pPremuto = -1; evidP.clear(); pDirty = true;
    }

    static std::wstring quandoN(INT64 t) {
        FILETIME f; GetSystemTimeAsFileTime(&f);
        const INT64 ora = ((INT64)f.dwHighDateTime << 32) | f.dwLowDateTime;
        const INT64 s = std::max<INT64>(0, (ora - t) / 10000000);
        wchar_t b[32];
        if (s < 60) return L"adesso";
        if (s < 3600) { swprintf(b, 32, L"%d min fa", (int)(s / 60)); return b; }
        if (s < 86400) { swprintf(b, 32, L"%d h fa", (int)(s / 3600)); return b; }
        FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)}, lt; SYSTEMTIME st;
        FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"d MMM", b, 32, nullptr);
        return b;
    }

    void disegnaNotifiche(bool scuro, D2D1_COLOR_F ink) {
        const float m = 16 * dpi;
        const D2D1_COLOR_F tenue = {ink.r, ink.g, ink.b, ink.a * 0.6f};
        const D2D1_COLOR_F scheda = scuro ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(1, 1, 1, 0.5f);
        // ora grande e data
        pennello->SetColor(ink);
        scrivi(testoOra, fmtGrande, {m, 8 * dpi, PW - m, 52 * dpi});
        std::wstring data = testoDataLunga; if (!data.empty()) data[0] = (wchar_t)towupper(data[0]);
        pennello->SetColor(tenue);
        scrivi(data, fmtVoce, {m + 2 * dpi, 52 * dpi, PW - m, 72 * dpi});

        // calendario del mese, la settimana inizia di lunedi', oggi in un cerchio blu
        const float ct = N_TESTA * dpi, cb = ct + (N_CAL - 10) * dpi;
        pennello->SetColor(scheda);
        dc->FillRoundedRectangle({{m, ct, PW - m, cb}, 12 * dpi, 12 * dpi}, pennello);
        SYSTEMTIME oggi; GetLocalTime(&oggi);
        wchar_t mese[64] = L""; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &oggi, L"MMMM yyyy", mese, 64, nullptr);
        if (mese[0]) mese[0] = (wchar_t)towupper(mese[0]);
        pennello->SetColor(ink);
        scrivi(mese, fmtVoce, {m + 12 * dpi, ct + 8 * dpi, PW - m, ct + 28 * dpi});
        SYSTEMTIME primo = oggi; primo.wDay = 1;
        FILETIME fp; SystemTimeToFileTime(&primo, &fp); FileTimeToSystemTime(&fp, &primo);   // giorno della settimana del primo
        const int inizio = (primo.wDayOfWeek + 6) % 7;   // 0 = lunedi'
        static const int giorniMese[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int giorni = giorniMese[oggi.wMonth - 1];
        if (oggi.wMonth == 2 && ((oggi.wYear % 4 == 0 && oggi.wYear % 100 != 0) || oggi.wYear % 400 == 0)) giorni = 29;
        const float cw = (PW - 2 * m - 16 * dpi) / 7, x0 = m + 8 * dpi, rh = 18 * dpi, y0 = ct + 54 * dpi;
        for (int g = 0; g < 7; g++) {   // iniziali dei giorni nella lingua dell'utente (lunedi' 1/1/2024)
            SYSTEMTIME s{}; s.wYear = 2024; s.wMonth = 1; s.wDay = (WORD)(1 + g);
            wchar_t n[16] = L""; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &s, L"ddd", n, 16, nullptr);
            std::wstring iniziale(1, (wchar_t)towupper(n[0]));
            pennello->SetColor(tenue);
            scrivi(iniziale, fmtNome, {x0 + g * cw, ct + 32 * dpi, x0 + (g + 1) * cw, ct + 48 * dpi});
        }
        for (int d = 1; d <= giorni; d++) {
            const int cella = inizio + d - 1;
            const float x = x0 + (cella % 7) * cw, y = y0 + (cella / 7) * rh;
            if (d == oggi.wDay) {
                pennello->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1));
                dc->FillEllipse({{x + cw / 2, y + rh / 2}, 9 * dpi, 9 * dpi}, pennello);
                pennello->SetColor(D2D1::ColorF(1, 1, 1, 1));
            } else pennello->SetColor((cella % 7) >= 5 ? tenue : ink);
            scrivi(std::to_wstring(d), fmtNome, {x, y, x + cw, y + rh});
        }

        // notifiche
        const float nt = (N_TESTA + N_CAL) * dpi;
        pennello->SetColor(ink);
        scrivi(L"Notifiche", fmtTitolo, {m, nt, PW - m, nt + 28 * dpi});
        if (permessoN && !listaN.empty()) {
            const float lw = larghezzaTesto(L"Cancella tutto", fmtSotto) + 16 * dpi;
            const D2D1_RECT_F rc = {PW - m - lw, nt + 3 * dpi, PW - m, nt + 25 * dpi};
            evidenzia(aggiungi(rc, A_CANCELLA, 0), rc, scuro, 11 * dpi);
            pennello->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1));
            scrivi(L"Cancella tutto", fmtSotto, {rc.left + 8 * dpi, rc.top, rc.right, rc.bottom});
        }
        float y = nt + N_TITOLO * dpi;
        if (!permessoN || listaN.empty()) {
            pennello->SetColor(scheda);
            dc->FillRoundedRectangle({{m, y, PW - m, y + 46 * dpi}, 12 * dpi, 12 * dpi}, pennello);
            pennello->SetColor(tenue);
            scrivi(permessoN ? L"Nessuna notifica" : L"Consenti l'accesso alle notifiche nelle impostazioni",
                   fmtSotto, {m + 14 * dpi, y, PW - m - 10 * dpi, y + 46 * dpi});
        } else {
            const int quante = schedeVisibili();
            for (int i = 0; i < quante; i++) {
                const notifiche::Notifica& n = listaN[i];
                const D2D1_RECT_F r = {m, y, PW - m, y + (N_SCHEDA - 8) * dpi};
                pennello->SetColor(scheda);
                dc->FillRoundedRectangle({r, 12 * dpi, 12 * dpi}, pennello);
                const int e = aggiungi(r, A_NOTIFICA, i);
                evidenzia(e, r, scuro, 12 * dpi);
                // icona dell'app (dal suo AppUserModelID): la prepara il thread delle icone, intanto l'iniziale
                auto it = iconeN.find(n.aumid);
                if (it == iconeN.end()) {
                    it = iconeN.emplace(n.aumid, nullptr).first;
                    if (!n.aumid.empty()) chiediIcona(L"shell:AppsFolder\\" + n.aumid, 64);
                }
                const D2D1_RECT_F ri = {r.left + 12 * dpi, r.top + 12 * dpi, r.left + 40 * dpi, r.top + 40 * dpi};
                if (it != iconeN.end() && it->second) dc->DrawBitmap(it->second, ri, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                else {
                    pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.6f));
                    dc->FillRoundedRectangle({ri, 7 * dpi, 7 * dpi}, pennello);
                    pennello->SetColor(D2D1::ColorF(1, 1, 1, 1));
                    scrivi(std::wstring(1, n.app.empty() ? L'?' : (wchar_t)towupper(n.app[0])), fmtNome, ri);
                }
                const float tx = r.left + 50 * dpi;
                const std::wstring q = quandoN(n.quando);
                const float qw = larghezzaTesto(q, fmtSotto);
                const bool sopra = pMouseDentro && (pSopra == e || (pSopra == e + 1));
                pennello->SetColor(tenue);
                scrivi(n.app, fmtSotto, {tx, r.top + 8 * dpi, r.right - qw - 34 * dpi, r.top + 24 * dpi});
                if (!sopra) scrivi(q, fmtSotto, {r.right - qw - 12 * dpi, r.top + 8 * dpi, r.right, r.top + 24 * dpi});
                pennello->SetColor(ink);
                scrivi(n.titolo.empty() ? n.app : n.titolo, fmtVoce, {tx, r.top + 24 * dpi, r.right - 12 * dpi, r.top + 42 * dpi});
                pennello->SetColor(tenue);
                scrivi(n.testo, fmtSotto, {tx, r.top + 42 * dpi, r.right - 12 * dpi, r.bottom - 4 * dpi});
                // X per togliere la notifica, solo col mouse sopra
                const D2D1_RECT_F rx = {r.right - 30 * dpi, r.top + 6 * dpi, r.right - 6 * dpi, r.top + 30 * dpi};
                const int ex = aggiungi(rx, A_RIMUOVI, i);
                if (sopra) {
                    const double h = evidenzia(ex, rx, scuro, 12 * dpi);
                    pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.14f + 0.1f * (float)h) : D2D1::ColorF(0, 0, 0, 0.08f + 0.06f * (float)h));
                    dc->FillEllipse({{(rx.left + rx.right) / 2, (rx.top + rx.bottom) / 2}, 10 * dpi, 10 * dpi}, pennello);
                    pennello->SetColor(ink);
                    linea(-3.5f, -3.5f, 3.5f, 3.5f, (rx.left + rx.right) / 2, (rx.top + rx.bottom) / 2, 1.5f);
                    linea(3.5f, -3.5f, -3.5f, 3.5f, (rx.left + rx.right) / 2, (rx.top + rx.bottom) / 2, 1.5f);
                }
                y += N_SCHEDA * dpi;
            }
            if ((int)listaN.size() > quante) {
                wchar_t b[48]; swprintf(b, 48, L"+ altre %d", (int)listaN.size() - quante);
                pennello->SetColor(tenue);
                scrivi(b, fmtSotto, {m + 4 * dpi, y - 4 * dpi, PW - m, y + 16 * dpi});
            }
        }

        // piede: impostazioni delle notifiche
        const float py = PH - 46 * dpi;
        pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.10f));
        dc->DrawLine({m, py}, {PW - m, py}, pennello, 1.0f);
        const D2D1_RECT_F rp = {m - 8 * dpi, py + 6 * dpi, PW - m + 8 * dpi, PH - 8 * dpi};
        evidenzia(aggiungi(rp, A_IMPOST_NOTIFICHE, 0), rp, scuro, 6 * dpi);
        pennello->SetColor(ink);
        scrivi(L"Impostazioni notifiche", fmtVoce, {m, rp.top, PW - m - 20 * dpi, rp.bottom});
        chevronDestra(PW - m - 4 * dpi, (rp.top + rp.bottom) / 2);
    }

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

    // "Centro di controllo": rete, Bluetooth, volume a capsula, uscita audio, scorciatoie
    void disegnaControllo(bool scuro, D2D1_COLOR_F ink) {
        const float m = 16 * dpi, larg = PW - 2 * m, mezzo = (larg - 10 * dpi) / 2;
        float y = m;
        // Wi-Fi: interruttore vero se c'e' la scheda; senza (PC col cavo) lo stato della rete, e il clic apre le impostazioni
        const int wf = sis.wifi, bt = sis.bluetooth;
        if (wf >= 0) piastrella({m, y, m + mezzo, y + 64 * dpi}, A_RETE, wf == 1, L"Wi-Fi",
                                wf == 1 ? (rete == 2 ? nomeRete : L"Acceso") : L"Spento", scuro, ink);
        else piastrella({m, y, m + mezzo, y + 64 * dpi}, A_RETE, rete != 0, rete == 2 ? L"Wi-Fi" : rete == 1 ? L"Ethernet" : L"Rete",
                        rete ? L"Connessa" : L"Non connessa", scuro, ink);
        piastrella({m + mezzo + 10 * dpi, y, PW - m, y + 64 * dpi}, A_BLUETOOTH, bt == 1, L"Bluetooth",
                   bt < 0 ? L"Non disponibile" : bt == 1 ? L"Acceso" : L"Spento", scuro, ink);
        y += 64 * dpi + 10 * dpi;
        const bool tScuro = sistema::Sistema::temaScuro();
        piastrella({m, y, m + mezzo, y + 64 * dpi}, A_TEMA, tScuro, L"Tema scuro", tScuro ? L"Attivo" : L"Spento", scuro, ink);
        piastrella({m + mezzo + 10 * dpi, y, PW - m, y + 64 * dpi}, A_FLUIDWIN, false, L"FluidWin", L"Impostazioni", scuro, ink);

        // volume: capsula riempita di bianco come su iOS; si trascina, la rotella regola, l'altoparlante disattiva
        y += 64 * dpi + 14 * dpi;
        const D2D1_RECT_F rv = {m, y, PW - m, y + 44 * dpi};
        const float rr = 22 * dpi, v = (audioOk && !muto) ? volume : 0.0f;
        aggiungi(rv, A_VOLUME, 0);
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.08f));
        dc->FillRoundedRectangle({rv, rr, rr}, pennello);
        dc->PushAxisAlignedClip({rv.left, rv.top, rv.left + larg * v, rv.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        pennello->SetColor(D2D1::ColorF(1, 1, 1, scuro ? 0.92f : 0.97f));
        dc->FillRoundedRectangle({rv, rr, rr}, pennello);
        dc->PopAxisAlignedClip();
        const D2D1_RECT_F rm = {rv.left, rv.top, rv.left + 48 * dpi, rv.bottom};
        const int em = aggiungi(rm, A_MUTO, 0);
        const float sm = (em == pPremuto) ? 0.88f : 1.0f;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sm, sm, {rv.left + 26 * dpi, (rv.top + rv.bottom) / 2}));
        pennello->SetColor(scuro && v < 0.12f ? ink : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.85f));
        simboloVolume(rv.left + 26 * dpi, (rv.top + rv.bottom) / 2);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        wchar_t pc[16]; swprintf(pc, 16, muto ? L"muto" : L"%d%%", (int)std::lround(volume * 100));
        pennello->SetColor(scuro && v < 0.9f ? ink : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.7f));
        scrivi(pc, fmtNome, {rv.right - 56 * dpi, rv.top, rv.right - 8 * dpi, rv.bottom});

        // luminosita' del monitor (solo se si puo' regolare via DDC/CI)
        const int lum = sis.luminosita;
        if (lum >= 0) {
            y += 44 * dpi + 8 * dpi;
            const D2D1_RECT_F rl = {m, y, PW - m, y + 44 * dpi};
            const float vl = lum / 100.0f;
            aggiungi(rl, A_LUMINOSITA, 0);
            pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.08f));
            dc->FillRoundedRectangle({rl, rr, rr}, pennello);
            dc->PushAxisAlignedClip({rl.left, rl.top, rl.left + larg * vl, rl.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            pennello->SetColor(D2D1::ColorF(1, 1, 1, scuro ? 0.92f : 0.97f));
            dc->FillRoundedRectangle({rl, rr, rr}, pennello);
            dc->PopAxisAlignedClip();
            pennello->SetColor(scuro && vl < 0.12f ? ink : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.85f));
            simboloSole(rl.left + 26 * dpi, (rl.top + rl.bottom) / 2);
            wchar_t pl[16]; swprintf(pl, 16, L"%d%%", lum);
            pennello->SetColor(scuro && vl < 0.9f ? ink : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.7f));
            scrivi(pl, fmtNome, {rl.right - 56 * dpi, rl.top, rl.right - 8 * dpi, rl.bottom});
        }

        // uscita audio
        y += 44 * dpi + 8 * dpi;
        const D2D1_RECT_F ru = {m - 8 * dpi, y, PW - m + 8 * dpi, y + 36 * dpi};
        evidenzia(aggiungi(ru, A_AUDIO, 0), ru, scuro, 6 * dpi);
        pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.62f});
        scrivi(L"Uscita", fmtSotto, {m, y, m + 60 * dpi, y + 36 * dpi});
        pennello->SetColor(ink);
        scrivi(nomeUscita.empty() ? L"Nessun dispositivo" : nomeUscita, fmtVoce, {m + 52 * dpi, y, PW - m - 20 * dpi, y + 36 * dpi});
        chevronDestra(PW - m - 4 * dpi, y + 18 * dpi);

        // scorciatoie tonde
        y += 36 * dpi + 16 * dpi;
        const wchar_t* nomi[4] = {L"Widget", L"In background", L"Schermo", L"Impostazioni"};
        const Azione az[4] = {A_WIDGET, A_SFONDO, A_SCHERMO, A_IMPOSTAZIONI};
        for (int i = 0; i < 4; i++) {
            const float cx = m + larg * (i + 0.5f) / 4, cy = y + 26 * dpi;
            const D2D1_RECT_F r = {cx - 36 * dpi, y - 4 * dpi, cx + 36 * dpi, y + 76 * dpi};
            const int e = aggiungi(r, az[i], 0);
            const double h = evidenzia(e, {0, 0, 0, 0}, scuro, 0);   // solo il valore: l'evidenziazione e' il cerchio
            const float s = (e == pPremuto) ? 0.9f : 1.0f;
            pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f + 0.06f * (float)h) : D2D1::ColorF(0, 0, 0, 0.06f + 0.05f * (float)h));
            dc->FillEllipse({{cx, cy}, 26 * dpi * s, 26 * dpi * s}, pennello);
            pennello->SetColor(ink);
            if (i == 0) simboloWidget(cx, cy); else if (i == 1) simboloFreccia(cx, cy); else if (i == 2) simboloSole(cx, cy); else simboloIngranaggio(cx, cy);
            pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.75f});
            scrivi(nomi[i], fmtNome, {cx - 40 * dpi, cy + 30 * dpi, cx + 40 * dpi, cy + 48 * dpi});
        }
    }

    // ---- widget: meteo e notizie ----
    void disegnaWidget(bool scuro, D2D1_COLOR_F ink) {
        const float m = 16 * dpi, rp = std::max(6.0f, raggio::vetro() - 10.0f) * dpi;
        const D2D1_COLOR_F tenue = {ink.r, ink.g, ink.b, ink.a * 0.62f};
        // meteo: una scheda azzurra come l'app Meteo di iOS
        const D2D1_RECT_F rm = {m, m, PW - m, m + 168 * dpi};
        ID2D1GradientStopCollection* stop = nullptr;
        const D2D1_GRADIENT_STOP gs[2] = {{0, D2D1::ColorF(0.24f, 0.52f, 0.92f, 1)}, {1, D2D1::ColorF(0.42f, 0.70f, 0.98f, 1)}};
        dc->CreateGradientStopCollection(gs, 2, &stop);
        ID2D1LinearGradientBrush* sf = nullptr;
        if (stop) dc->CreateLinearGradientBrush({{rm.left, rm.top}, {rm.left, rm.bottom}}, stop, &sf);
        if (sf) { dc->FillRoundedRectangle({rm, rp, rp}, sf); sf->Release(); }
        if (stop) stop->Release();
        const D2D1_COLOR_F bianco = D2D1::ColorF(1, 1, 1, 1), biancoT = D2D1::ColorF(1, 1, 1, 0.8f);
        pennello->SetColor(bianco);
        scrivi(widD.citta, fmtVoce, {rm.left + 16 * dpi, rm.top + 12 * dpi, rm.right - 16 * dpi, rm.top + 32 * dpi});
        if (wid.meteoOk) {
            wchar_t t[16]; swprintf(t, 16, L"%d°", (int)std::lround(wid.temp));
            scrivi(t, fmtGrande, {rm.left + 16 * dpi, rm.top + 30 * dpi, rm.left + 140 * dpi, rm.top + 74 * dpi});
            pennello->SetColor(bianco);
            simboloMeteo(wid.codice, rm.right - 40 * dpi, rm.top + 44 * dpi, 1.4f);
            pennello->SetColor(biancoT);
            wchar_t d[96]; swprintf(d, 96, L"%ls · umidità %d%% · %d km/h", widget::descrizione(wid.codice), (int)std::lround(wid.umidita), (int)std::lround(wid.vento));
            scrivi(d, fmtSotto, {rm.left + 16 * dpi, rm.top + 76 * dpi, rm.right - 16 * dpi, rm.top + 94 * dpi});
            // i prossimi giorni
            const size_t n = std::min<size_t>(5, wid.giorni.size());
            for (size_t i = 0; i < n; i++) {
                const float cw = (rm.right - rm.left - 16 * dpi) / 5, cx = rm.left + 8 * dpi + cw * (i + 0.5f), y = rm.top + 104 * dpi;
                const auto& g = wid.giorni[i];
                pennello->SetColor(biancoT);
                scrivi(g.nome, fmtNome, {cx - cw / 2, y, cx + cw / 2, y + 14 * dpi});
                pennello->SetColor(bianco);
                simboloMeteo(g.codice, cx, y + 27 * dpi, 0.8f);
                wchar_t mm[32]; swprintf(mm, 32, L"%d°  %d°", (int)std::lround(g.max), (int)std::lround(g.min));
                scrivi(mm, fmtNome, {cx - cw / 2, y + 40 * dpi, cx + cw / 2, y + 56 * dpi});
            }
        } else {
            pennello->SetColor(biancoT);
            scrivi(L"Meteo in arrivo…", fmtSotto, {rm.left + 16 * dpi, rm.top + 40 * dpi, rm.right - 16 * dpi, rm.top + 60 * dpi});
        }
        // notizie
        float y = rm.bottom + 16 * dpi;
        pennello->SetColor(ink);
        scrivi(L"Notizie", fmtTitolo, {m, y, PW - m, y + 22 * dpi});
        pennello->SetColor(tenue);
        scrivi(L"ANSA", fmtSotto, {PW - m - 36 * dpi, y + 2 * dpi, PW - m, y + 20 * dpi});
        y += 28 * dpi;
        if (wid.notizie.empty()) { pennello->SetColor(tenue); scrivi(L"Notizie in arrivo…", fmtSotto, {m, y, PW - m, y + 20 * dpi}); }
        for (size_t i = 0; i < wid.notizie.size() && y + 56 * dpi < PH - m; i++) {
            const auto& nz = wid.notizie[i];
            const D2D1_RECT_F r = {m - 8 * dpi, y, PW - m + 8 * dpi, y + 56 * dpi};
            evidenzia(aggiungi(r, A_NOTIZIA, (int)i), r, scuro, std::min(rp, 12 * dpi));
            pennello->SetColor(ink);
            // due righe al massimo: quello che non entra finisce con i puntini
            IDWriteTextLayout* lay = nullptr;
            if (SUCCEEDED(fdw->CreateTextLayout(nz.titolo.c_str(), (UINT32)nz.titolo.size(), fmtNotizia, PW - 2 * m - 44 * dpi, 34 * dpi, &lay))) {
                dc->DrawTextLayout({m, y + 6 * dpi}, lay, pennello, D2D1_DRAW_TEXT_OPTIONS_CLIP); lay->Release();
            }
            pennello->SetColor(tenue);
            scrivi(nz.quando, fmtSotto, {PW - m - 36 * dpi, y + 6 * dpi, PW - m, y + 22 * dpi});
            y += 58 * dpi;
        }
    }

    // simboli del meteo (scala 1 = circa 24 px)
    void simboloMeteo(int c, float cx, float cy, float s) {
        const float d = dpi * s;
        auto sole = [&](float x, float y, float r) {
            dc->FillEllipse({{x, y}, r * d, r * d}, pennello);
            for (int i = 0; i < 8; i++) {
                const float a = i * (float)M_PI / 4;
                dc->DrawLine({x + std::cos(a) * (r + 2.5f) * d, y + std::sin(a) * (r + 2.5f) * d}, {x + std::cos(a) * (r + 5) * d, y + std::sin(a) * (r + 5) * d}, pennello, 1.6f * d, tratto);
            }
        };
        auto nuvola = [&](float x, float y) {
            dc->FillEllipse({{x - 5 * d, y + 2 * d}, 6 * d, 5 * d}, pennello);
            dc->FillEllipse({{x + 1 * d, y - 2 * d}, 7 * d, 7 * d}, pennello);
            dc->FillEllipse({{x + 7 * d, y + 2 * d}, 5.5f * d, 5 * d}, pennello);
            dc->FillRoundedRectangle({{x - 11 * d, y + 1 * d, x + 12.5f * d, y + 7 * d}, 3 * d, 3 * d}, pennello);
        };
        if (c == 0) { sole(cx, cy, 6); return; }
        if (c <= 2) { sole(cx - 4 * d, cy - 4 * d, 4.5f); nuvola(cx + 2 * d, cy + 3 * d); return; }   // sole e nuvola
        nuvola(cx, cy - 2 * d);
        if (c == 45 || c == 48) { for (int i = 0; i < 2; i++) dc->DrawLine({cx - 9 * d, cy + (9 + i * 4) * d}, {cx + 9 * d, cy + (9 + i * 4) * d}, pennello, 1.6f * d, tratto); return; }
        if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) { for (int i = -1; i <= 1; i++) dc->DrawLine({cx + i * 5 * d, cy + 8 * d}, {cx + i * 5 * d - 2 * d, cy + 13 * d}, pennello, 1.6f * d, tratto); return; }
        if ((c >= 71 && c <= 77) || c == 85 || c == 86) { for (int i = -1; i <= 1; i++) dc->FillEllipse({{cx + i * 5 * d, cy + 11 * d}, 1.5f * d, 1.5f * d}, pennello); return; }
        if (c >= 95) {   // fulmine
            dc->DrawLine({cx + 1 * d, cy + 6 * d}, {cx - 2 * d, cy + 11 * d}, pennello, 1.8f * d, tratto);
            dc->DrawLine({cx - 2 * d, cy + 11 * d}, {cx + 2 * d, cy + 11 * d}, pennello, 1.8f * d, tratto);
            dc->DrawLine({cx + 2 * d, cy + 11 * d}, {cx - 1 * d, cy + 16 * d}, pennello, 1.8f * d, tratto);
        }
    }

    void simboloBluetooth(float cx, float cy) {
        linea(-4, -4.5f, 4, 3.5f, cx, cy, 1.6f); linea(4, 3.5f, 0, 7.5f, cx, cy, 1.6f); linea(0, 7.5f, 0, -7.5f, cx, cy, 1.6f);
        linea(0, -7.5f, 4, -3.5f, cx, cy, 1.6f); linea(4, -3.5f, -4, 4.5f, cx, cy, 1.6f);
    }

    // Sole che diventa luna (e viceversa) quando cambia il tema: i raggi rientrano, un'ombra rotonda scivola sul
    // disco fino a lasciare una falce, e il tutto ruota un poco
    void simboloTema(float cx, float cy) {
        const double obj = sistema::Sistema::temaScuro() ? 1.0 : 0.0;
        if (temaK < 0) temaK = obj;
        const double passo = dtFotogramma / 0.5;   // mezzo secondo da un capo all'altro, a velocita' costante (la curva la fa k)
        temaK = std::abs(obj - temaK) <= passo ? obj : temaK + (obj > temaK ? passo : -passo);
        if (temaK != obj) pAnimazione = true;
        const float k = (float)(temaK * temaK * (3 - 2 * temaK));   // morbida all'inizio e alla fine
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(-90.0f * (1 - k), {cx, cy}));
        // raggi del sole: si accorciano fino a sparire
        if (k < 0.98f) {
            const float r0 = (6.6f + 1.5f * k) * dpi, r1 = (9.2f - 2.6f * k) * dpi;
            for (int i = 0; i < 8; i++) {
                const float a = i * (float)M_PI / 4, c = std::cos(a), s = std::sin(a);
                if (r1 > r0 + 0.3f * dpi) dc->DrawLine({cx + c * r0, cy + s * r0}, {cx + c * r1, cy + s * r1}, pennello, 1.8f * dpi, tratto);
            }
        }
        // disco: piccolo da sole, piu' grande da luna; l'ombra arriva da in alto a destra
        const float r = (4.2f + 3.3f * k) * dpi;
        const float ox = cx + (r * 2.2f) - (r * 2.2f - 4.5f * dpi) * k, oy = cy - (r * 1.6f) + (r * 1.6f - 3.5f * dpi) * k;
        ID2D1EllipseGeometry *a = nullptr, *b = nullptr; ID2D1PathGeometry* g = nullptr;
        f2d->CreateEllipseGeometry({{cx, cy}, r, r}, &a);
        f2d->CreateEllipseGeometry({{ox, oy}, r * 0.86f, r * 0.86f}, &b);
        if (a && b && SUCCEEDED(f2d->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr; g->Open(&s);
            a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, s);
            s->Close(); s->Release();
            dc->FillGeometry(g, pennello);
        }
        for (IUnknown* o : std::initializer_list<IUnknown*>{a, b, g}) if (o) o->Release();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void simboloLuna(float cx, float cy) {
        // falce: un cerchio meno un cerchio spostato in alto a destra
        const float r = 7.5f * dpi;
        ID2D1EllipseGeometry *a = nullptr, *b = nullptr; ID2D1PathGeometry* g = nullptr;
        f2d->CreateEllipseGeometry({{cx, cy}, r, r}, &a);
        f2d->CreateEllipseGeometry({{cx + 4.5f * dpi, cy - 3.5f * dpi}, r * 0.85f, r * 0.85f}, &b);
        if (a && b && SUCCEEDED(f2d->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr; g->Open(&s);
            a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, s);
            s->Close(); s->Release();
            dc->FillGeometry(g, pennello);
        }
        for (IUnknown* o : std::initializer_list<IUnknown*>{a, b, g}) if (o) o->Release();
    }

    // goccia: il simbolo di FluidWin
    void simboloGoccia(float cx, float cy) {
        ID2D1PathGeometry* g = nullptr;
        if (FAILED(f2d->CreatePathGeometry(&g))) return;
        ID2D1GeometrySink* s = nullptr; g->Open(&s);
        const float r = 6 * dpi;
        s->BeginFigure({cx, cy - 10 * dpi}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddBezier(D2D1::BezierSegment({cx + 2 * dpi, cy - 6 * dpi}, {cx + r, cy - 2 * dpi}, {cx + r, cy + 2.5f * dpi}));
        s->AddArc(D2D1::ArcSegment({cx - r, cy + 2.5f * dpi}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        s->AddBezier(D2D1::BezierSegment({cx - r, cy - 2 * dpi}, {cx - 2 * dpi, cy - 6 * dpi}, {cx, cy - 10 * dpi}));
        s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
        dc->FillGeometry(g, pennello); g->Release();
    }

    void simboloSole(float cx, float cy) {
        dc->DrawEllipse({{cx, cy}, 3.8f * dpi, 3.8f * dpi}, pennello, 1.8f * dpi);
        for (int i = 0; i < 8; i++) {
            const float a = i * (float)M_PI / 4, c = std::cos(a), s = std::sin(a);
            linea(c * 6.8f, s * 6.8f, c * 8.8f, s * 8.8f, cx, cy, 1.8f);
        }
    }

    void simboloEnergia(float cx, float cy) {
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(f2d->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr; g->Open(&s);
            const float r = 7 * dpi, a0 = -(float)M_PI / 2 + 0.65f, a1 = -(float)M_PI / 2 - 0.65f;
            s->BeginFigure({cx + r * std::cos(a0), cy + r * std::sin(a0)}, D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddArc(D2D1::ArcSegment({cx + r * std::cos(a1), cy + r * std::sin(a1)}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
            s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
            dc->DrawGeometry(g, pennello, 1.9f * dpi, tratto);
            g->Release();
        }
        linea(0, -9, 0, -2, cx, cy, 1.9f);
    }

    void simboloIngranaggio(float cx, float cy) {
        dc->DrawEllipse({{cx, cy}, 5.6f * dpi, 5.6f * dpi}, pennello, 2.2f * dpi);
        dc->DrawEllipse({{cx, cy}, 2.0f * dpi, 2.0f * dpi}, pennello, 1.4f * dpi);
        for (int i = 0; i < 8; i++) {
            const float a = i * (float)M_PI / 4 + (float)M_PI / 8, c = std::cos(a), s = std::sin(a);
            linea(c * 6.6f, s * 6.6f, c * 8.6f, s * 8.6f, cx, cy, 2.6f);
        }
    }

    // ---- dati dei pannelli ----
    // "{GUID cartella nota}\resto" -> percorso vero (cosi' Windows salva i percorsi delle icone di notifica)
    static std::wstring risolviCartella(const std::wstring& p) {
        if (p.size() < 39 || p[0] != L'{') return p;
        const size_t fine = p.find(L'}');
        if (fine == std::wstring::npos) return p;
        GUID g; if (FAILED(CLSIDFromString(p.substr(0, fine + 1).c_str(), &g))) return p;
        PWSTR base = nullptr; std::wstring r = p;
        if (SUCCEEDED(SHGetKnownFolderPath(g, 0, nullptr, &base))) r = std::wstring(base) + p.substr(fine + 1);
        CoTaskMemFree(base);
        return r;
    }

    ID2D1Bitmap* bitmapDaPng(const BYTE* d, DWORD n) {
        IWICImagingFactory* f = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
        IWICStream* s = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
        ID2D1Bitmap1* out = nullptr;
        if (SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromMemory((BYTE*)d, n)) &&
            SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
            SUCCEEDED(f->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            dc->CreateBitmapFromWicBitmap(cv, (const D2D1_BITMAP_PROPERTIES1*)nullptr, &out);
        for (IUnknown* u : std::initializer_list<IUnknown*>{cv, fr, dec, s, f}) if (u) u->Release();
        return out;
    }

    // icona grande dall'eseguibile (dal thread delle icone: intanto nullptr, cioe' il segnaposto); se non si puo'
    // leggere (app dello Store), la piccola foto salvata da Windows (fotoIconaSfondo)
    std::unordered_map<std::wstring, std::wstring> chiaviSfondo;   // eseguibile -> sottochiave di NotifyIconSettings
    ID2D1Bitmap* iconaSfondo(const std::wstring& exe, HKEY, const wchar_t* sotto) {
        chiaviSfondo[exe] = sotto;
        auto it = cacheIcone.find(exe);
        if (it != cacheIcone.end()) return it->second;
        chiediIcona(exe, 128);
        return nullptr;
    }

    ID2D1Bitmap* fotoIconaSfondo(const std::wstring& exe) {
        auto k = chiaviSfondo.find(exe);
        if (k == chiaviSfondo.end()) return nullptr;
        HKEY chiave = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &chiave) != ERROR_SUCCESS) return nullptr;
        ID2D1Bitmap* b = nullptr;
        DWORD n = 0;
        if (RegGetValueW(chiave, k->second.c_str(), L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &n) == ERROR_SUCCESS && n && n < 4 * 1024 * 1024) {
            std::vector<BYTE> png(n);
            if (RegGetValueW(chiave, k->second.c_str(), L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, png.data(), &n) == ERROR_SUCCESS) b = bitmapDaPng(png.data(), n);
        }
        RegCloseKey(chiave);
        return b;
    }

    // app dello Store: dal percorso ...\WindowsApps\Nome_versione_arch__editore\... all'AppUserModelID
    // (solo dal lavoro in sottofondo delle app in background: uno alla volta, quindi la cache non e' contesa)
    static std::wstring avvioPacchetto(const std::wstring& exe) {
        static std::unordered_map<std::wstring, std::wstring> aumidPacchetti;   // famiglia del pacchetto -> AppUserModelID
        const size_t p = exe.find(L"\\windowsapps\\");
        if (p == std::wstring::npos) return L"";
        const size_t a = p + 13, b = exe.find(L'\\', a);
        const std::wstring pac = exe.substr(a, b - a);
        const size_t u = pac.find(L'_'), e = pac.rfind(L"__");
        if (u == std::wstring::npos || e == std::wstring::npos) return L"";
        const std::wstring famiglia = pac.substr(0, u) + L"_" + pac.substr(e + 2);
        if (aumidPacchetti.empty()) {   // elenco delle app installate (una volta sola)
            IShellItem* cart = nullptr;
            if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&cart)))) {
                IEnumShellItems* en = nullptr;
                if (SUCCEEDED(cart->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
                    IShellItem* it = nullptr;
                    while (en->Next(1, &it, nullptr) == S_OK) {
                        LPWSTR n = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &n))) {
                            const std::wstring id = n; const size_t k = id.find(L'!');
                            if (k != std::wstring::npos) aumidPacchetti.emplace(minuscolo(id.substr(0, k)), id);
                            CoTaskMemFree(n);
                        }
                        it->Release();
                    }
                    en->Release();
                }
                cart->Release();
            }
        }
        auto it = aumidPacchetti.find(famiglia);
        return it == aumidPacchetti.end() ? L"" : L"shell:AppsFolder\\" + it->second;
    }

    // il tooltip e' il nome che l'app mostra di se': si tolgono stato ("Steam - sincronizzazione") e versioni ("B26.0730.1")
    static std::wstring nomeDaTooltip(std::wstring t) {
        for (const wchar_t* sep : {L" - ", L"\n", L"\r"}) { const size_t p = t.find(sep); if (p != std::wstring::npos) t = t.substr(0, p); }
        // ultime parole con cifre (versioni) e spazi in fondo
        for (;;) {
            while (!t.empty() && t.back() == L' ') t.pop_back();
            const size_t sp = t.rfind(L' ');
            if (sp == std::wstring::npos || t.find_first_of(L"0123456789", sp) == std::wstring::npos) break;
            t = t.substr(0, sp);
        }
        return t;
    }

    // Le icone di notifica che Windows ricorda, solo di programmi in esecuzione adesso. Leggerle costa centinaia di
    // millisecondi (tutti i processi del sistema, il registro, l'elenco delle app dello Store): lo fa un thread a
    // parte, e intanto il pannello mostra l'elenco di prima
    bool lavoroSfondo = false;
    void leggiAppSfondo() {
        if (lavoroSfondo) return;
        lavoroSfondo = true;
        auto ris = std::make_shared<std::vector<AppSfondo>>();
        inSottofondo([ris] { *ris = elencaAppSfondo(); }, [this, ris] {
            lavoroSfondo = false;
            for (auto& a : *ris) a.icona = iconaSfondo(a.exe, nullptr, a.chiaveReg.c_str());
            appSfondo = std::move(*ris);
            // pannello aperto: si allunga o si accorcia con le righe di icone (il bordo in basso resta fermo)
            const int s = slotCorrente;
            usa(0);
            if (pannelloTipo == P_SFONDO && !pChiusura) {
                const float nuova = altezzaSfondo();
                if (nuova != PH) { PH = nuova; rettPannello.top = rettPannello.bottom - (int)PH; posizionaPannello(pK < 0 ? 1 : pK); }
                pSopra = pPremuto = -1; evidP.clear(); pDirty = true;
            }
            usa(s);
        });
    }

    float altezzaSfondo() const {
        const int righe = std::max(1, (int)std::min<size_t>(6, (appSfondo.size() + 3) / 4));
        return std::round((48 + righe * 86 + 52) * dpi);
    }

    static std::vector<AppSfondo> elencaAppSfondo() {
        std::unordered_set<std::wstring> attivi;
        HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        if (sn != INVALID_HANDLE_VALUE && Process32FirstW(sn, &pe)) do {
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t b[MAX_PATH]; DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(p, 0, b, &n)) attivi.insert(minuscolo(b));
                CloseHandle(p);
            }
        } while (Process32NextW(sn, &pe));
        if (sn != INVALID_HANDLE_VALUE) CloseHandle(sn);
        wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH); const std::wstring proprio = minuscolo(buf);
        GetWindowsDirectoryW(buf, MAX_PATH); const std::wstring dirWin = minuscolo(buf) + L"\\";

        std::vector<AppSfondo> nuove;
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            wchar_t sub[256];
            for (DWORD i = 0;; i++) {
                DWORD n = 256;
                if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                wchar_t percorso[1024]; DWORD sz = sizeof(percorso);
                if (RegGetValueW(k, sub, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, percorso, &sz) != ERROR_SUCCESS) continue;
                const std::wstring exe = minuscolo(risolviCartella(percorso));
                if (!attivi.count(exe) || exe == dirWin + L"explorer.exe") continue;   // explorer: volume, rete... gia' nell'isola
                if (std::any_of(nuove.begin(), nuove.end(), [&](const AppSfondo& a) { return a.exe == exe; })) continue;
                AppSfondo a; a.exe = exe;
                wchar_t tip[256] = L""; DWORD ts = sizeof(tip);
                RegGetValueW(k, sub, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, tip, &ts);
                a.nome = exe == proprio ? L"FluidWin" : exe.find(L"securityhealthsystray") != std::wstring::npos ? L"Sicurezza di Windows" : nomeDaTooltip(tip);
                if (a.nome.empty()) a.nome = nomeExe(exe);
                a.chiaveReg = sub;   // l'icona la chiede poi il thread principale
                // come si apre: rilanciarle e' il modo ufficiale (le app da area di notifica mostrano la loro finestra)
                if (exe == proprio) a.avvio = L"*fluidwin";
                else if (exe.find(L"securityhealthsystray") != std::wstring::npos) a.avvio = L"windowsdefender:";
                else if (exe.rfind(dirWin, 0) == 0) a.avvio = L"";                    // componenti di Windows: pannello nativo
                else if (exe.find(L"\\windowsapps\\") != std::wstring::npos) a.avvio = avvioPacchetto(exe);
                else a.avvio = exe;
                nuove.push_back(std::move(a));
            }
            RegCloseKey(k);
        }
        std::sort(nuove.begin(), nuove.end(), [](const AppSfondo& a, const AppSfondo& b) { return minuscolo(a.nome) < minuscolo(b.nome); });
        return nuove;
    }

    void leggiUscita() {
        static const PROPERTYKEY NOME_DISPOSITIVO = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        nomeUscita.clear();
        IMMDevice* d = nullptr;
        if (!audio || FAILED(audio->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(NOME_DISPOSITIVO, &v)) && v.vt == VT_LPWSTR && v.pwszVal) nomeUscita = v.pwszVal;
            PropVariantClear(&v); ps->Release();
        }
        d->Release();
    }

    void impostaVolume(float l, bool togliMuto) {
        IAudioEndpointVolume* v = volumeDispositivo();
        if (!v) return;
        v->SetMasterVolumeLevelScalar(std::clamp(l, 0.0f, 1.0f), nullptr);
        if (togliMuto) v->SetMute(FALSE, nullptr);
        v->Release();
        leggiVolume(); dirty = true; pDirty = true;
    }

    void volumeDaMouse(float x) {
        for (auto& e : elementi) if (e.azione == A_VOLUME) {
            impostaVolume((x - e.r.left) / (e.r.right - e.r.left), true);
            return;
        }
    }

    void luminositaDaMouse(float x) {
        for (auto& e : elementi) if (e.azione == A_LUMINOSITA) {
            sis.impostaLuminosita((int)std::lround(std::clamp((x - e.r.left) / (e.r.right - e.r.left), 0.0f, 1.0f) * 100));
            pDirty = true;
            return;
        }
    }

public:
    // tema cambiato (dal centro di controllo o dalle Impostazioni): i vetri seguono il chiaro/scuro
    void aggiornaTemaVetri() {
        BOOL s = temaScuro(true);
        for (HWND v : {vetro, vetroStato, vetroOra, vetroPannello, altra.vetroPannello, vetroLauncher}) if (v) DwmSetWindowAttribute(v, DWMWA_USE_IMMERSIVE_DARK_MODE, &s, sizeof(s));
        dirty = true; pDirty = true; altra.pDirty = true; lDirty = true;
    }
private:

    static void apriPagina(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

    // il pannello delle icone nascoste di Windows (fa risalire la barra): resta per i menu delle singole app
    void pannelloWindows() { tastiWin('B'); SetTimer(icone, TIMER_INVIO, 220, nullptr); }

    void apriAppSfondo(const AppSfondo& a) {
        if (a.avvio == L"*fluidwin") {   // FluidWin stesso: il suo menu (pausa, esci)
            nascondiPannello();   // il menu blocca il disegno finche' resta aperto: niente dissolvenza a meta'
            PostMessageW(FindWindowW(L"FluidWin.Messaggi", nullptr), WM_APP + 1, 1, WM_LBUTTONUP);
        } else if (a.avvio.empty()) {
            pannelloWindows();
        } else {
            const std::wstring dir = a.avvio.find(L'\\') != std::wstring::npos && a.avvio.rfind(L"shell:", 0) != 0
                                     ? a.avvio.substr(0, a.avvio.find_last_of(L'\\')) : L"";
            ShellExecuteW(nullptr, L"open", a.avvio.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
        }
    }

    void eseguiPannello(const Elemento& e) {
        switch (e.azione) {
            case A_APP:          chiudiPannello(); if (e.indice < (int)appSfondo.size()) apriAppSfondo(appSfondo[e.indice]); break;
            case A_TUTTE:        chiudiPannello(); pannelloWindows(); break;
            // Wi-Fi e Bluetooth: interruttori (se la scheda c'e'); il tasto destro apre comunque le impostazioni
            case A_RETE:
                if (sis.wifi >= 0) { sis.impostaRadio(1, sis.wifi != 1); pDirty = true; }
                else { chiudiPannello(); apriPagina(L"ms-settings:network-status"); }
                break;
            case A_BLUETOOTH:
                if (sis.bluetooth >= 0) { sis.impostaRadio(3, sis.bluetooth != 1); pDirty = true; }
                else { chiudiPannello(); apriPagina(L"ms-settings:bluetooth"); }
                break;
            case A_TEMA:
                sistema::Sistema::impostaTema(!sistema::Sistema::temaScuro());
                aggiornaTemaVetri(); dirty = true; pDirty = true;
                break;
            case A_FLUIDWIN:     chiudiPannello(); if (impostazioni) impostazioni(); break;
            case A_WIDGET:       nascondiPannello(); apriPannello(P_WIDGET); break;
            case A_SFONDO:       nascondiPannello(); apriPannello(P_SFONDO); break;
            case A_NOTIZIA:      if (e.indice < (int)wid.notizie.size()) { chiudiPannello(); apriPagina(wid.notizie[e.indice].link.c_str()); } break;
            case A_LUMINOSITA:   break;
            case A_AUDIO: {   // le uscite audio in un menu: si sceglie quella predefinita
                const auto lista = sistema::Sistema::uscite();
                std::vector<menu::Voce> voci = {menu::titolo(L"Uscita audio")};
                for (size_t i = 0; i < lista.size(); i++) { auto x = menu::voce(lista[i].nome, (UINT)(i + 1)); x.spuntata = lista[i].predefinita; voci.push_back(x); }
                voci.push_back(menu::separatore());
                voci.push_back(menu::voce(L"Impostazioni audio", 1000));
                POINT pt = {(LONG)e.r.left, (LONG)e.r.bottom}; ClientToScreen(pannello, &pt);
                const UINT s = menuScheda(voci, pt, pannello);
                if (s == 1000) { chiudiPannello(); apriPagina(L"ms-settings:sound"); }
                else if (s >= 1 && s <= lista.size()) { sistema::Sistema::impostaUscita(lista[s - 1].id); leggiUscita(); leggiVolume(); pDirty = true; dirty = true; }
                break;
            }
            case A_SCHERMO:      chiudiPannello(); apriPagina(L"ms-settings:display"); break;
            case A_ENERGIA:      chiudiPannello(); apriPagina(L"ms-settings:powersleep"); break;
            case A_IMPOSTAZIONI: chiudiPannello(); apriPagina(L"ms-settings:"); break;
            case A_NOTIFICA:
                // come toccare una notifica: si apre l'app e la notifica se ne va
                if (e.indice < (int)listaN.size()) {
                    const notifiche::Notifica n = listaN[e.indice];
                    chiudiPannello();
                    if (!n.aumid.empty()) ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + n.aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    lettoreN.rimuovi(n.id);
                }
                break;
            case A_RIMUOVI:
                if (e.indice < (int)listaN.size()) { lettoreN.rimuovi(listaN[e.indice].id); ricaricaNotifiche(); }
                break;
            case A_CANCELLA: lettoreN.cancellaTutte(); ricaricaNotifiche(); break;
            case A_IMPOST_NOTIFICHE: chiudiPannello(); apriPagina(permessoN ? L"ms-settings:notifications" : L"ms-settings:privacy-notifications"); break;
            case A_MUTO: {
                IAudioEndpointVolume* v = volumeDispositivo();
                if (v) { BOOL m = FALSE; v->GetMute(&m); v->SetMute(!m, nullptr); v->Release(); }
                leggiVolume(); dirty = true; pDirty = true;
                break;
            }
            case A_VOLUME: break;
        }
    }

    int elementoIn(float x, float y) const {
        for (int i = (int)elementi.size() - 1; i >= 0; i--) {   // gli ultimi aggiunti stanno sopra (es. l'altoparlante sul volume)
            const auto& r = elementi[i].r;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        return -1;
    }

    static LRESULT CALLBACK procPannello(HWND h, UINT m, WPARAM w, LPARAM l) {
        Motore* d = gIstanza();
        if (!d) return DefWindowProcW(h, m, w, l);
        // da qui in poi si lavora sulla scheda di questa finestra. Una finestra non ancora registrata (in creazione:
        // CreateWindowEx manda gia' dei messaggi) non deve scambiare niente
        if (h == d->altra.pannello) d->usa(1 - d->slotCorrente);
        else if (h != d->pannello) return DefWindowProcW(h, m, w, l);
        const float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
        // carta dietro: un clic la riporta davanti (rimescolando), il resto del mouse non le arriva
        if (!d->davanti && d->pannelloTipo != P_NESSUNO && !d->pChiusura && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) {
            if (m == WM_LBUTTONUP) d->mescola(d->slotCorrente);
            return 0;
        }
        switch (m) {
            case WM_MOUSEMOVE:
                if (!d->pMouseDentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, h, 0}; TrackMouseEvent(&tm); d->pMouseDentro = true; }
                if (d->trascinaVolume) d->volumeDaMouse(x);
                else if (d->trascinaLum) d->luminositaDaMouse(x);
                else { const int s = d->elementoIn(x, y); if (s != d->pSopra) { d->pSopra = s; d->pDirty = true; } }
                return 0;
            case WM_MOUSELEAVE: d->pMouseDentro = false; d->pSopra = -1; d->pDirty = true; return 0;
            case WM_LBUTTONDOWN: {
                const int e = d->elementoIn(x, y);
                d->pPremuto = e; d->pDirty = true;
                if (e >= 0 && d->elementi[e].azione == A_VOLUME) { d->trascinaVolume = true; SetCapture(h); d->volumeDaMouse(x); }
                if (e >= 0 && d->elementi[e].azione == A_LUMINOSITA) { d->trascinaLum = true; SetCapture(h); d->luminositaDaMouse(x); }
                return 0;
            }
            case WM_LBUTTONUP: {
                if (d->trascinaVolume || d->trascinaLum) { d->trascinaVolume = d->trascinaLum = false; ReleaseCapture(); }
                else { const int e = d->elementoIn(x, y); if (e >= 0 && e == d->pPremuto) d->eseguiPannello(d->elementi[e]); }
                d->pPremuto = -1; d->pDirty = true;
                return 0;
            }
            case WM_RBUTTONUP: {   // app in background: menu con l'alternativa del pannello di Windows
                const int e = d->elementoIn(x, y);
                if (e >= 0 && d->elementi[e].azione == A_RETE) { d->chiudiPannello(); apriPagina(L"ms-settings:network-status"); return 0; }
                if (e >= 0 && d->elementi[e].azione == A_BLUETOOTH) { d->chiudiPannello(); apriPagina(L"ms-settings:bluetooth"); return 0; }
                if (e < 0 || d->elementi[e].azione != A_APP) return 0;
                POINT pt{(LONG)x, (LONG)y}; ClientToScreen(h, &pt);
                const int indice = d->elementi[e].indice;
                const std::wstring exe = indice >= 0 && indice < (int)d->appSfondo.size() ? d->appSfondo[indice].exe : L"";
                const int s = (int)d->menuScheda({menu::voce(L"Apri", 1), menu::separatore(),
                                                   menu::voce(L"Altre opzioni (pannello di Windows)", 2)}, pt, h);
                // nel frattempo l'elenco puo' essere stato riletto (in ordine di nome): si ritrova la stessa app
                if (s == 1) {
                    for (size_t k = 0; k < d->appSfondo.size(); k++)
                        if (d->appSfondo[k].exe == exe) { d->eseguiPannello(Elemento{{}, A_APP, (int)k}); break; }
                }
                else if (s == 2) { d->chiudiPannello(); d->pannelloWindows(); }
                return 0;
            }
            case WM_MOUSEWHEEL:
                if (d->pannelloTipo == P_CONTROLLO) d->cambiaVolume(GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA * 0.02f), d->pDirty = true;
                return 0;
            case WM_KEYDOWN: if (w == VK_ESCAPE) d->chiudiTutte(); return 0;
            case WM_CLOSE: d->chiudiTutte(); return 0;   // Alt+F4: come Esc (la finestra non si distrugge, si riusa)
            // il fuoco va altrove (non all'altra carta): si chiudono tutte e due, una dopo l'altra
            case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && !d->schedaFinestra((HWND)l)) d->chiudiTutte(); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    int statoInPunto(float x, float y) const {
        for (int i = 0; i < N_STATO; i++) {
            const auto& r = rettStato[i];
            if (x >= r.left && x < r.right && y >= isola.top && y <= isola.bottom) return i;
        }
        return -1;
    }

    bool sullIsola(float x, float y) const { return x >= isola.left - 2 && x <= isola.right + 2 && y >= isola.top - 2; }

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
    HWND primaDelLauncher = nullptr;       // finestra in primo piano prima dell'apertura
    RECT rettLauncher{};                   // il launcher vero e proprio (la finestra arriva piu' in basso, fino all'icona)
    POINT ancora{};                        // icona "App": da qui cresce e qui rientra (coordinate della finestra)
    std::vector<AppL> tutteApp;
    std::vector<int> filtrate;
    std::wstring query;
    bool lAperto = false, lChiusura = false, lDirty = false, lAnim = false, lRegione = false, lMouseDentro = false;
    double lScala = 0.15, lVel = 0, lAlfa = 0, lT0 = 0, lChiusoT = -10, lLetto = -1e9;
    double lScroll = 0, lScrollObj = 0;
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

    static DWORD WINAPI threadIcone(LPVOID p) {
        Motore* m = (Motore*)p;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        while (!m->fineIcone) {
            WaitForSingleObject(m->eventoIcone, INFINITE);
            while (!m->fineIcone) {
                std::pair<std::wstring, int> r;
                EnterCriticalSection(&m->csIcone);
                const bool c = !m->iconeDaFare.empty();
                if (c) { r = m->iconeDaFare.front(); m->iconeDaFare.pop_front(); }
                LeaveCriticalSection(&m->csIcone);
                if (!c) break;
                IconaPronta ip; ip.percorso = r.first; ip.lato = r.second;
                ip.ok = pixelIcona(r.first, r.second, ip.w, ip.h, ip.px);
                EnterCriticalSection(&m->csIcone); m->iconePronte.push_back(std::move(ip)); LeaveCriticalSection(&m->csIcone);
                PostMessageW(m->icone, WM_ICONA_PRONTA, 0, 0);
            }
        }
        CoUninitialize();
        return 0;
    }

    static std::wstring chiaveIcona(const std::wstring& percorso, int lato) { return std::to_wstring(lato) + L"|" + percorso; }

    // Lavori lenti (elenchi della shell, tutti i processi del sistema) in un thread a parte. 'fai' gira nel thread e
    // non deve toccare il Motore; 'applica' gira poi nel thread principale, con un messaggio alla finestra del dock
    static const UINT WM_LAVORO_FATTO = WM_APP + 31;
    struct Sottofondo { std::function<void()> fai, applica; HWND dest; };
    static DWORD WINAPI threadSottofondo(LPVOID p) {
        Sottofondo* l = (Sottofondo*)p;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        l->fai();
        CoUninitialize();
        if (!PostMessageW(l->dest, WM_LAVORO_FATTO, 0, (LPARAM)l)) delete l;   // FluidWin si sta chiudendo
        return 0;
    }
    void inSottofondo(std::function<void()> fai, std::function<void()> applica) {
        Sottofondo* l = new Sottofondo{std::move(fai), std::move(applica), icone};
        HANDLE t = CreateThread(nullptr, 0, threadSottofondo, l, 0, nullptr);
        if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
        else { l->fai(); l->applica(); delete l; }   // senza thread: subito, come prima
    }

    void chiediIcona(const std::wstring& percorso, int lato) {
        if (percorso.empty() || !iconeChieste.insert(chiaveIcona(percorso, lato)).second) return;
        if (!csIconeOk) {
            InitializeCriticalSection(&csIcone); csIconeOk = true;
            eventoIcone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            threadIconeH = CreateThread(nullptr, 0, threadIcone, this, 0, nullptr);
            if (threadIconeH) SetThreadPriority(threadIconeH, THREAD_PRIORITY_BELOW_NORMAL);
        }
        EnterCriticalSection(&csIcone); iconeDaFare.push_back({percorso, lato}); LeaveCriticalSection(&csIcone);
        iconeAttese++;
        SetEvent(eventoIcone);
    }

    // all'uscita il thread si ferma (dopo l'icona che sta preparando) prima che il Motore sparisca
    void fermaIcone() {
        if (!threadIconeH) return;
        fineIcone = 1; SetEvent(eventoIcone);
        WaitForSingleObject(threadIconeH, 2000);
        CloseHandle(threadIconeH); threadIconeH = nullptr;
    }

    static std::wstring sorgenteIcona(const Voce& v) {
        return v.fissata ? v.avvio : (v.avvio.rfind(L"shell:AppsFolder\\", 0) == 0 ? v.avvio : v.exe);
    }

    // le icone arrivate dal thread diventano bitmap (veloce) e vanno a chi le aspetta: true se ne e' arrivata qualcuna
    bool raccogliIcone() {
        if (!csIconeOk) return false;
        std::vector<IconaPronta> arrivate;
        EnterCriticalSection(&csIcone); arrivate.swap(iconePronte); LeaveCriticalSection(&csIcone);
        for (auto& ip : arrivate) {
            iconeAttese--;
            ID2D1Bitmap1* b = ip.ok ? bitmapDaPixel(ip.w, ip.h, ip.px) : nullptr;
            bool presa = false;
            if (ip.lato == 256) {          // dock
                for (auto& v : voci) if (!v.icona && v.avvio != AVVIO_LAUNCHER && sorgenteIcona(v) == ip.percorso) { v.icona = b; presa = true; dirty = true; break; }
                // (si potra' richiedere: l'app puo' chiudersi e riaprirsi; se e' fallita si riprova al prossimo aggiornamento)
                iconeChieste.erase(chiaveIcona(ip.percorso, ip.lato));
            } else if (ip.lato == 128) {   // app in background (se non c'e', la piccola foto salvata da Windows)
                if (!b) b = (ID2D1Bitmap1*)fotoIconaSfondo(ip.percorso);
                cacheIcone[ip.percorso] = b; presa = b != nullptr;
                for (auto& a : appSfondo) if (a.exe == ip.percorso) a.icona = b;
                pDirty = true; altra.pDirty = true;
            } else if (ip.lato == 96) {    // launcher
                for (auto& a : tutteApp) if (a.avvio == ip.percorso && !a.icona) { a.icona = b; presa = true; lDirty = true; break; }
            } else if (ip.lato == 64) {    // notifiche
                const std::wstring pre = L"shell:AppsFolder\\";
                if (ip.percorso.rfind(pre, 0) == 0) { iconeN[ip.percorso.substr(pre.size())] = b; presa = true; }
                pDirty = true; altra.pDirty = true;
            }
            if (b && !presa) b->Release();
        }
        return !arrivate.empty();
    }

    bool launcherDaDisegnare() const { return lAperto && (lDirty || lAnim); }

    bool creaLauncher(HINSTANCE inst) {
        LW = std::ceil(760 * dpi); LH = std::ceil(580 * dpi); LEXTRA = std::ceil(160 * dpi);
        WNDCLASSW wl{}; wl.lpfnWndProc = procLauncher; wl.hInstance = inst; wl.lpszClassName = L"FluidWin.Launcher";
        wl.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wl);
        launcher = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wl.lpszClassName, L"App",
                                   WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
        vetroLauncher = creaVetro(inst);
        BOOL niente = TRUE; DwmSetWindowAttribute(launcher, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fab = nullptr;
        dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd); dd->GetAdapter(&ad); ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fab);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = (UINT)LW; sd.Height = (UINT)(LH + LEXTRA); sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        const HRESULT hr = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &scL);
        fab->Release(); ad->Release(); dd->Release();
        if (FAILED(hr)) return false;
        dcomp->CreateTargetForHwnd(launcher, TRUE, &targetL);
        dcomp->CreateVisual(&visualL); visualL->SetContent(scL);
        dcomp->CreateEffectGroup(&effettoL); visualL->SetEffect(effettoL);
        targetL->SetRoot(visualL); dcomp->Commit();
        IDXGISurface* sup = nullptr; scL->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglioL); sup->Release();
        return bersaglioL != nullptr;
    }

    // solo le app vere: niente disinstallazioni, documenti, pagine web e guide
    static bool appUtile(const std::wstring& nome, const std::wstring& par) {
        const std::wstring n = minuscolo(nome), p = minuscolo(par);
        if (n.find(L"uninstall") != std::wstring::npos || n.find(L"disinstall") != std::wstring::npos) return false;
        if (p.rfind(L"http", 0) == 0) return false;
        const size_t barra = p.find_last_of(L"\\/"), punto = p.rfind(L'.');
        if (barra == std::wstring::npos) return true;   // AppUserModelID (app dello Store e molte app desktop)
        if (punto == std::wstring::npos || punto < barra) return true;
        const std::wstring est = p.substr(punto + 1);
        return est == L"exe" || est == L"msc" || est == L"lnk" || est == L"cpl";
    }

    // L'elenco di tutte le app (cartella App della shell) costa centinaia di millisecondi: lo legge un thread a parte.
    // subito = adesso, nel thread principale (solo la prima volta che serve e non c'e' ancora niente da mostrare)
    bool lavoroTutte = false;
    void leggiTutteApp(bool subito = false) {
        lLetto = adesso();
        if (subito) { Cronometro cr(this, "leggiTutteApp"); applicaTutteApp(elencaTutteApp()); return; }
        if (lavoroTutte) return;
        lavoroTutte = true;
        auto ris = std::make_shared<std::vector<AppL>>();
        inSottofondo([ris] { *ris = elencaTutteApp(); }, [this, ris] { lavoroTutte = false; applicaTutteApp(std::move(*ris)); });
    }

    void applicaTutteApp(std::vector<AppL> nuove) {
        // le icone gia' caricate si conservano
        for (auto& a : nuove) for (auto& v : tutteApp) if (v.avvio == a.avvio) { a.icona = v.icona; a.provata = v.provata; v.icona = nullptr; break; }
        for (auto& v : tutteApp) if (v.icona) v.icona->Release();
        tutteApp = std::move(nuove);
        // launcher aperto (l'elenco nuovo arriva mentre si guarda): la scelta e lo scorrimento restano dove sono
        const int sel = lSel; const double scr = lScrollObj;
        filtra();
        if (lAperto) { lSel = std::clamp(sel, 0, std::max(0, (int)filtrate.size() - 1)); lScrollObj = std::clamp(scr, 0.0, lScrollMax()); }
        for (auto& a : tutteApp) if (!a.icona && !a.provata) { a.provata = true; chiediIcona(a.avvio, 96); }
        if (registro) { char b[64]; snprintf(b, 64, "launcher: %d app", (int)tutteApp.size()); scrivi(b); }
    }

    static std::vector<AppL> elencaTutteApp() {
        std::vector<AppL> nuove;
        IShellItem* cart = nullptr;
        if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&cart)))) return nuove;
        IEnumShellItems* en = nullptr;
        if (SUCCEEDED(cart->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
            IShellItem* it = nullptr;
            while (en->Next(1, &it, nullptr) == S_OK) {
                LPWSTR n = nullptr, par = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &par)) &&
                    n && par && n[0] && appUtile(n, par)) {
                    AppL a; a.nome = n; a.avvio = std::wstring(L"shell:AppsFolder\\") + par; a.minuscolo = minuscolo(n);
                    nuove.push_back(std::move(a));
                }
                CoTaskMemFree(n); CoTaskMemFree(par); it->Release();
            }
            en->Release();
        }
        cart->Release();
        std::sort(nuove.begin(), nuove.end(), [](const AppL& a, const AppL& b) { return lstrcmpiW(a.nome.c_str(), b.nome.c_str()) < 0; });
        nuove.erase(std::unique(nuove.begin(), nuove.end(), [](const AppL& a, const AppL& b) { return a.minuscolo == b.minuscolo; }), nuove.end());
        return nuove;
    }

    // con la ricerca: prima i nomi che iniziano con il testo, poi quelli che lo contengono
    void filtra() {
        filtrate.clear();
        const std::wstring q = minuscolo(query);
        std::vector<int> dopo;
        for (int i = 0; i < (int)tutteApp.size(); i++) {
            if (q.empty()) { filtrate.push_back(i); continue; }
            const size_t p = tutteApp[i].minuscolo.find(q);
            if (p == 0 || (p != std::wstring::npos && tutteApp[i].minuscolo[p - 1] == L' ')) filtrate.push_back(i);
            else if (p != std::wstring::npos) dopo.push_back(i);
        }
        filtrate.insert(filtrate.end(), dopo.begin(), dopo.end());
        lSel = 0; lScrollObj = 0; lDirty = true;
    }

    // misure della griglia (coordinate del launcher)
    float lColonne() const { return 6; }
    float lCellaW() const { return (LW - 48 * dpi) / lColonne(); }
    float lCellaH() const { return 104 * dpi; }
    float lGrigliaTop() const { return 84 * dpi; }
    float lGrigliaBottom() const { return LH - 14 * dpi; }
    double lScrollMax() const {
        const int righe = ((int)filtrate.size() + 5) / 6;
        return std::max(0.0, (double)(righe * lCellaH() - (lGrigliaBottom() - lGrigliaTop())));
    }
    D2D1_RECT_F lCella(int i) const {
        const float x = 24 * dpi + (i % 6) * lCellaW(), y = lGrigliaTop() + (i / 6) * lCellaH() - (float)lScroll;
        return {x, y, x + lCellaW(), y + lCellaH()};
    }
    int lCellaIn(float x, float y) const {
        if (y < lGrigliaTop() || y > lGrigliaBottom()) return -1;
        for (int i = 0; i < (int)filtrate.size(); i++) {
            const auto r = lCella(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        return -1;
    }

    // prende il primo piano anche quando lo apre il tasto Win (nessun clic arrivato al nostro processo)
    static void prendiFuoco(HWND w) {
        SetForegroundWindow(w);
        if (GetForegroundWindow() == w) return;
        HWND fg = GetForegroundWindow();
        const DWORD a = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, b = GetCurrentThreadId();
        // (mai agganciati a un'app che non risponde: il suo input bloccato bloccherebbe anche quello di FluidWin)
        if (a && a != b && !IsHungAppWindow(fg) && AttachThreadInput(b, a, TRUE)) { SetForegroundWindow(w); SetFocus(w); AttachThreadInput(b, a, FALSE); }
    }

public:
    // tasti del volume (intercettati da FluidWin): +1 alza, -1 abbassa (passi del 2%, come Windows), 0 muto
    void volumeTasto(int verso, float& livello, bool& silenzio) {
        if (verso) cambiaVolume(verso * 0.02f);
        else { IAudioEndpointVolume* v = volumeDispositivo(); if (v) { BOOL m = FALSE; v->GetMute(&m); v->SetMute(!m, nullptr); v->Release(); } leggiVolume(); dirty = true; }
        usa(0); if (pannelloTipo == P_CONTROLLO) pDirty = true;
        livello = volume; silenzio = muto;
    }

    // tutte le app (nome, comando d'avvio), per la ricerca rapida
    std::vector<std::pair<std::wstring, std::wstring>> elencoApp() {
        if (tutteApp.empty()) leggiTutteApp(true);              // la prima volta serve subito
        else if (adesso() - lLetto > 600) leggiTutteApp();      // poi si rinfresca in sottofondo
        std::vector<std::pair<std::wstring, std::wstring>> r;
        for (const auto& a : tutteApp) r.push_back({a.nome, a.avvio});
        return r;
    }

    // l'isola sullo schermo (per i banner delle notifiche, che escono da li')
    RECT rettIsolaSchermo() const {
        // a riposo (anche quando e' nascosta in basso)
        const float top = yBaseRiposo() - S - PAD, bottom = yBaseRiposo() + PAD;
        return {schermo.left + (LONG)isola.left, schermo.bottom - H + (LONG)top, schermo.left + (LONG)isola.right, schermo.bottom - H + (LONG)bottom};
    }
    bool isolaVisibile() const { return attivo && !nascostoSchermoIntero && (!vuoleNascondersiI || rivelatoI); }

    // apre le impostazioni di FluidWin (dal centro di controllo)
    void (*impostazioni)() = nullptr;

    // apre un pannello dell'isola da fuori (1 app in background, 2 centro di controllo, 3 notifiche, 4 widget)
    void apriPannelloIsola(int tipo) { if (attivo && tipo >= P_SFONDO && tipo <= P_WIDGET) apriPannello(tipo); }

    // Apre il launcher (o lo chiude, se e' gia' aperto): cresce dall'icona "App" del dock con un piccolo rimbalzo
    void apriLauncher() {
        if (!attivo || nascostoSchermoIntero) return;
        if (lAperto && !lChiusura) { chiudiLauncher(); return; }
        if (adesso() - lChiusoT < 0.25) return;   // lo stesso clic che lo ha appena chiuso (perdendo il fuoco)
        if (tutteApp.empty()) leggiTutteApp(true);
        else if (adesso() - lLetto > 600) leggiTutteApp();
        if (!nessunaScheda()) nascondiSchede();
        primaDelLauncher = GetForegroundWindow();
        query.clear(); filtra(); lSopra = -1; lScroll = 0;
        lAperto = true; lChiusura = false; lScala = 0.12; lVel = 0; lAlfa = 0; lT0 = adesso(); lRegione = false;
        // sopra il dock, al centro; la finestra scende fino all'icona "App"
        const int x0 = schermo.left + (int)std::round((W - LW) / 2);
        const int y1 = schermo.bottom - H + (int)std::round(yBaseRiposo() - S - PAD - 18 * dpi);
        rettLauncher = {x0, y1 - (int)LH, x0 + (int)LW, y1};
        float ax = W / 2.0f;
        const auto pos = posizioniRiposo();
        for (size_t i = 0; i < voci.size(); i++) if (voci[i].avvio == AVVIO_LAUNCHER) ax = pos[i] + S / 2;
        ancora = {(LONG)std::clamp(schermo.left + ax - x0, 40 * dpi, LW - 40 * dpi), (LONG)std::min(LH + LEXTRA - 4, (float)(schermo.bottom - H + yBaseRiposo() - S / 2 - rettLauncher.top))};
        SetWindowRgn(launcher, nullptr, FALSE);
        SetWindowPos(launcher, HWND_TOPMOST, x0, rettLauncher.top, (int)LW, (int)(LH + LEXTRA), SWP_NOACTIVATE);
        posizionaLauncher();
        ShowWindow(vetroLauncher, SW_SHOWNOACTIVATE);
        ShowWindow(launcher, SW_SHOW);
        prendiFuoco(launcher);
        lDirty = true; dirty = true;
        scrivi(GetForegroundWindow() == launcher ? "launcher aperto" : "launcher aperto senza fuoco");
    }

private:
    void chiudiLauncher() {
        if (!lAperto || lChiusura) return;
        lChiusura = true; lDirty = true;
        if (lRegione) { SetWindowRgn(launcher, nullptr, FALSE); lRegione = false; }   // per rientrare nell'icona serve tutta la finestra
    }

    void nascondiLauncher() {
        // chiuso senza aprire un'app (Esc, Win, clic sul dock): il primo piano torna alla finestra di prima
        if (lAperto && GetForegroundWindow() == launcher && primaDelLauncher && IsWindow(primaDelLauncher) && IsWindowVisible(primaDelLauncher))
            SetForegroundWindow(primaDelLauncher);
        primaDelLauncher = nullptr;
        if (launcher) ShowWindow(launcher, SW_HIDE);
        if (vetroLauncher) ShowWindow(vetroLauncher, SW_HIDE);
        if (lAperto) lChiusoT = adesso();
        lAperto = false; lChiusura = false; lAnim = false; lDirty = false;
        dirty = true;   // il dock puo' tornare a nascondersi
    }

    // il contenuto (DirectComposition) e il vetro crescono attorno all'icona "App"
    void posizionaLauncher() {
        const double s = lScala;
        const float ax = (float)ancora.x, ay = (float)ancora.y;
        visualL->SetTransform(D2D1::Matrix3x2F::Scale((float)s, (float)s, {ax, ay}));
        effettoL->SetOpacity((float)std::clamp(lAlfa, 0.0, 1.0));
        const RECT& r = rettLauncher;
        const double gx = r.left + ax, gy = r.top + ay;
        const int L = (int)std::lround(gx + (r.left - gx) * s), T = (int)std::lround(gy + (r.top - gy) * s);
        const int R = (int)std::lround(gx + (r.right - gx) * s), B = (int)std::lround(gy + (r.bottom - gy) * s);
        SetWindowPos(vetroLauncher, launcher, L, T, std::max(1, R - L), std::max(1, B - T), SWP_NOACTIVATE);
        SetLayeredWindowAttributes(vetroLauncher, 0, (BYTE)std::lround(std::clamp(lAlfa, 0.0, 1.0) * 255), LWA_ALPHA);
        dcomp->Commit();
    }

    void avviaApp(int i) {
        if (i < 0 || i >= (int)filtrate.size()) return;
        const AppL& a = tutteApp[filtrate[i]];
        primaDelLauncher = nullptr;   // il primo piano va all'app che si apre
        chiudiLauncher();
        ShellExecuteW(nullptr, L"open", a.avvio.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    D2D1_RECT_F rettEnergia() const { return {LW - 64 * dpi, 22 * dpi, LW - 24 * dpi, 62 * dpi}; }

    // Menu di Windows per l'app (lo stesso dello Start), con in cima "Aggiungi al dock"
    void menuApp(int i, POINT pt) {
        if (i < 0 || i >= (int)filtrate.size()) return;
        const AppL a = tutteApp[filtrate[i]];
        IShellItem* it = nullptr;
        if (FAILED(SHCreateItemFromParsingName(a.avvio.c_str(), nullptr, IID_PPV_ARGS(&it)))) return;
        IContextMenu* cm = nullptr;
        it->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm));
        // eseguibile e AppUserModelID dell'app, per il collegamento nel dock
        std::wstring exe, id;
        IShellItem2* it2 = nullptr;
        if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2)))) {
            LPWSTR s = nullptr;
            if (SUCCEEDED(it2->GetString(PKEY_Link_TargetParsingPath, &s)) && s) { exe = s; CoTaskMemFree(s); }
            s = nullptr;
            if (SUCCEEDED(it2->GetString(PKEY_AppUserModel_ID, &s)) && s) { id = s; CoTaskMemFree(s); }
            it2->Release();
        }
        it->Release();
        if (!cm) return;
        const UINT ID_DOCK = 0x7F00;
        HMENU m = CreatePopupMenu();
        if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, ID_DOCK - 1, CMF_NORMAL))) {
            cm->QueryInterface(IID_PPV_ARGS(&menuShell2)); cm->QueryInterface(IID_PPV_ARGS(&menuShell3));
            // le voci di Windows, disegnate da noi; in cima il nome dell'app e "Aggiungi al dock"
            std::vector<menu::Voce> voci = {menu::titolo(a.nome), menu::voce(L"Aggiungi al dock", ID_DOCK), menu::separatore()};
            for (auto& v : menu::daHmenu(m)) voci.push_back(v);
            // i sottomenu ("Invia a", "Apri con"...) la shell li riempie solo quando stanno per aprirsi
            auto prepara = [this](HMENU s) {
                if (menuShell3) { LRESULT r = 0; menuShell3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)s, 0, &r); }
                else if (menuShell2) menuShell2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)s, 0);
            };
            const UINT cmd = menu::mostra(voci, pt, launcher, false, prepara);
            if (menuShell3) { menuShell3->Release(); menuShell3 = nullptr; }
            if (menuShell2) { menuShell2->Release(); menuShell2 = nullptr; }
            if (cmd == ID_DOCK) {
                aggiungiAlDock(a.nome, exe, id, a.avvio);
            } else if (cmd > 0) {
                CMINVOKECOMMANDINFOEX ci{}; ci.cbSize = sizeof(ci); ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                ci.hwnd = launcher; ci.lpVerb = MAKEINTRESOURCEA(cmd - 1); ci.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                ci.nShow = SW_SHOWNORMAL; ci.ptInvoke = pt;
                cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci);
                primaDelLauncher = nullptr;   // ha aperto qualcosa: il primo piano va a quello
                chiudiLauncher();
                appDaAggiornare = true;       // "Aggiungi alla barra" e simili cambiano le icone fisse del dock
            }
        }
        DestroyMenu(m);
        cm->Release();
    }

    // Collegamento nella cartella delle app fisse della barra (quella che legge il dock)
    void aggiungiAlDock(const std::wstring& nome, const std::wstring& exe, const std::wstring& id, const std::wstring& avvio) {
        wchar_t cart[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, cart))) return;
        std::wstring n = nome;
        for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
        const std::wstring file = std::wstring(cart) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\" + n + L".lnk";
        if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return;   // gia' nel dock
        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
        bool ok = false;
        if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) ok = SUCCEEDED(sl->SetPath(exe.c_str()));
        else {   // app dello Store: il collegamento punta alla voce della cartella App
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(avvio.c_str(), nullptr, &pidl, 0, nullptr))) { ok = SUCCEEDED(sl->SetIDList(pidl)); CoTaskMemFree(pidl); }
        }
        if (ok && !id.empty()) {   // cosi' il dock riconosce le finestre dell'app
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) {
                PROPVARIANT pv; if (SUCCEEDED(InitPropVariantFromString(id.c_str(), &pv))) { ps->SetValue(PKEY_AppUserModel_ID, pv); ps->Commit(); PropVariantClear(&pv); }
                ps->Release();
            }
        }
        IPersistFile* pf = nullptr;
        if (ok && SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(file.c_str(), TRUE); pf->Release(); }
        sl->Release();
        appDaAggiornare = true;
        if (registro) { char b[200]; snprintf(b, 200, "aggiunta al dock: %ls", n.c_str()); scrivi(b); }
    }

    // Accensione: come il pulsante dello Start
    void menuEnergia(POINT pt) {
        const int s = (int)menu::mostra({menu::voce(L"Sospendi", 1), menu::voce(L"Riavvia", 2), menu::voce(L"Arresta il sistema", 3),
                                         menu::separatore(), menu::voce(L"Blocca", 4), menu::voce(L"Disconnetti", 5)}, pt, launcher);
        lSopraEnergia = false; lDirty = true;
        if (s <= 0) return;
        chiudiLauncher();
        energia(s);
    }

    static void energia(int s) {
        if (s == 4) { LockWorkStation(); return; }
        // privilegio di arresto: ce l'ha ogni utente, va solo attivato per questo processo
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
            TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
            AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
            CloseHandle(tok);
        }
        const DWORD motivo = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
        // le app con lavoro non salvato possono fermare riavvio e arresto (nessun EWX_FORCE), come dallo Start
        if (s == 1) SetSuspendState(FALSE, FALSE, FALSE);
        else if (s == 2) ExitWindowsEx(EWX_REBOOT, motivo);
        else if (s == 3) ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | 0x00400000 /* EWX_HYBRID_SHUTDOWN: avvio rapido */, motivo);
        else if (s == 5) ExitWindowsEx(EWX_LOGOFF, 0);
    }

    void selezionaVisibile() {
        const auto r = lCella(lSel);
        const float top = lGrigliaTop(), bot = lGrigliaBottom();
        if (r.top < top) lScrollObj -= top - r.top;
        else if (r.bottom > bot) lScrollObj += r.bottom - bot;
        lScrollObj = std::clamp(lScrollObj, 0.0, lScrollMax());
    }

    // icona "App" nel dock: griglia di pallini colorati su vetro chiaro
    void disegnaIconaApp(const D2D1_RECT_F& r, float alfa, bool scuro) {
        const float d = r.right - r.left;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.16f * alfa) : D2D1::ColorF(1, 1, 1, 0.82f * alfa));
        dc->FillRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello);
        pennello->SetColor(D2D1::ColorF(0, 0, 0, (scuro ? 0.3f : 0.08f) * alfa));
        dc->DrawRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello, 1.0f);
        static const D2D1_COLOR_F colori[9] = {
            {1.00f, 0.37f, 0.34f, 1}, {1.00f, 0.62f, 0.20f, 1}, {1.00f, 0.80f, 0.20f, 1},
            {0.30f, 0.82f, 0.40f, 1}, {0.20f, 0.68f, 0.98f, 1}, {0.36f, 0.45f, 0.98f, 1},
            {0.69f, 0.42f, 0.95f, 1}, {1.00f, 0.42f, 0.70f, 1}, {0.55f, 0.58f, 0.62f, 1}};
        const float passo = d * 0.22f, raggio = d * 0.075f;
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        for (int k = 0; k < 9; k++) {
            D2D1_COLOR_F c = colori[k]; c.a = alfa;
            pennello->SetColor(c);
            dc->FillEllipse({{cx + (k % 3 - 1) * passo, cy + (k / 3 - 1) * passo}, raggio, raggio}, pennello);
        }
    }

    // restituisce false se il launcher si e' appena chiuso
    bool renderLauncher() {
        // scala con una molla: apre con un piccolo rimbalzo (gelatina), rientra nell'icona rapida e senza
        // in chiusura lo stesso movimento all'indietro: rientra nell'icona con la molla e sfuma solo nell'ultimo tratto
        bool anim = molla(lScala, lVel, lChiusura ? 0.0 : 1.0, 240, lChiusura ? 0.8 : 0.55, 0.0005);
        if (lChiusura) {
            lAlfa = std::clamp((lScala - 0.10) / 0.30, 0.0, 1.0);
            if (lAlfa <= 0.02 || !anim) { nascondiLauncher(); return false; }
        } else {
            lAlfa = verso(lAlfa, 1.0, 0.045);
            if (lAlfa != 1.0) anim = true;
        }
        posizionaLauncher();
        if (!lChiusura && !anim && !lRegione) {
            // aperto: fuori dal launcher (sopra il dock) i clic devono passare
            SetWindowRgn(launcher, CreateRectRgn(0, 0, (int)LW, (int)LH), FALSE); lRegione = true;
        }
        const double sc = verso(lScroll, lScrollObj, 0.06);
        if (sc != lScroll) { lScroll = sc; anim = true; }
        raccogliIcone();   // (quelle che arrivano dal thread rimettono lDirty)

        const bool scuro = temaScuro();
        const D2D1_COLOR_F ink = scuro ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.88f);
        dc->SetTarget(bersaglioL);
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        disegnaFondo({0.5f, 0.5f, LW - 0.5f, LH - 0.5f}, scuro);

        // pulsante di accensione (sospendi, riavvia, arresta, blocca, disconnetti) a destra della ricerca
        {
            const D2D1_RECT_F re = rettEnergia();
            const float cx = (re.left + re.right) / 2, cy = (re.top + re.bottom) / 2, rr = (re.bottom - re.top) / 2;
            pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, lSopraEnergia ? 0.18f : 0.10f) : D2D1::ColorF(1, 1, 1, lSopraEnergia ? 0.85f : 0.6f));
            dc->FillEllipse({{cx, cy}, rr, rr}, pennello);
            pennello->SetColor(ink);
            simboloEnergia(cx, cy);
        }
        // campo di ricerca
        const D2D1_RECT_F rc = {24 * dpi, 22 * dpi, rettEnergia().left - 10 * dpi, 62 * dpi};
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.6f));
        dc->FillRoundedRectangle({rc, 10 * dpi, 10 * dpi}, pennello);
        pennello->SetColor({ink.r, ink.g, ink.b, 0.55f});
        const float lx = rc.left + 22 * dpi, ly = (rc.top + rc.bottom) / 2;
        dc->DrawEllipse({{lx - 2 * dpi, ly - 2 * dpi}, 6 * dpi, 6 * dpi}, pennello, 1.8f * dpi);
        linea(2.5f, 2.5f, 7, 7, lx, ly);
        const D2D1_RECT_F rt = {lx + 18 * dpi, rc.top, rc.right - 14 * dpi, rc.bottom};
        if (query.empty()) scrivi(L"Cerca un'app", fmtTitolo, rt);
        else {
            pennello->SetColor(ink);
            scrivi(query, fmtTitolo, rt);
            // cursore dopo il testo
            const float lt = larghezzaTesto(query, fmtTitolo);
            pennello->SetColor(D2D1::ColorF(0.2f, 0.5f, 1.0f, 0.9f));
            dc->DrawLine({rt.left + lt + 2 * dpi, ly - 9 * dpi}, {rt.left + lt + 2 * dpi, ly + 9 * dpi}, pennello, 1.6f * dpi);
        }

        // griglia delle app
        dc->PushAxisAlignedClip({0, lGrigliaTop() - 4 * dpi, LW, lGrigliaBottom()}, D2D1_ANTIALIAS_MODE_ALIASED);
        const float lato = 56 * dpi;
        for (int i = 0; i < (int)filtrate.size(); i++) {
            const auto r = lCella(i);
            if (r.bottom < lGrigliaTop() - lCellaH() || r.top > lGrigliaBottom() + lCellaH()) continue;
            AppL& a = tutteApp[filtrate[i]];
            const bool sopra = i == lSopra, scelta = i == lSel && !query.empty();
            if (sopra || scelta) {
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, sopra ? 0.12f : 0.08f) : D2D1::ColorF(1, 1, 1, sopra ? 0.55f : 0.4f));
                const D2D1_RECT_F h = {r.left + 4 * dpi, r.top + 2 * dpi, r.right - 4 * dpi, r.bottom - 2 * dpi};
                dc->FillRoundedRectangle({h, 8 * dpi, 8 * dpi}, pennello);
                if (scelta) { pennello->SetColor(D2D1::ColorF(0.2f, 0.5f, 1.0f, 0.7f)); dc->DrawRoundedRectangle({h, 8 * dpi, 8 * dpi}, pennello, 1.5f * dpi); }
            }
            const float cx = (r.left + r.right) / 2;
            const D2D1_RECT_F ri = {cx - lato / 2, r.top + 12 * dpi, cx + lato / 2, r.top + 12 * dpi + lato};
            if (a.icona) dc->DrawBitmap(a.icona, ri, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else { pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.5f)); dc->FillRoundedRectangle({ri, lato * 0.22f, lato * 0.22f}, pennello); }
            pennello->SetColor(ink);
            scrivi(a.nome, fmtNome, {r.left + 6 * dpi, ri.bottom + 6 * dpi, r.right - 6 * dpi, ri.bottom + 24 * dpi});
        }
        if (filtrate.empty()) { pennello->SetColor({ink.r, ink.g, ink.b, 0.5f}); scrivi(L"Nessuna app trovata", fmtNome, {0, lGrigliaTop() + 40 * dpi, LW, lGrigliaTop() + 70 * dpi}); }
        dc->PopAxisAlignedClip();
        // barra di scorrimento sottile, solo se serve
        const double mx = lScrollMax();
        if (mx > 0) {
            const float alt = lGrigliaBottom() - lGrigliaTop(), tot = alt + (float)mx;
            const float h = std::max(30 * dpi, alt * alt / tot), y = lGrigliaTop() + (alt - h) * (float)(lScroll / mx);
            pennello->SetColor({ink.r, ink.g, ink.b, 0.25f});
            dc->FillRoundedRectangle({{LW - 9 * dpi, y, LW - 6 * dpi, y + h}, 1.5f * dpi, 1.5f * dpi}, pennello);
        }
        controllaGpu(dc->EndDraw());
        dc->SetTarget(bersaglio);
        lAnim = anim; lDirty = false;
        return true;
    }

    static LRESULT CALLBACK procLauncher(HWND h, UINT m, WPARAM w, LPARAM l) {
        Motore* d = gIstanza();
        if (!d) return DefWindowProcW(h, m, w, l);
        const float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
        switch (m) {
            case WM_MOUSEMOVE: {
                if (!d->lMouseDentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, h, 0}; TrackMouseEvent(&tm); d->lMouseDentro = true; }
                const int s = d->lCellaIn(x, y);
                if (s != d->lSopra) { d->lSopra = s; d->lDirty = true; }
                const D2D1_RECT_F re = d->rettEnergia();
                const bool e = x >= re.left && x < re.right && y >= re.top && y < re.bottom;
                if (e != d->lSopraEnergia) { d->lSopraEnergia = e; d->lDirty = true; }
                return 0;
            }
            case WM_MOUSELEAVE: d->lMouseDentro = false; d->lSopra = -1; d->lSopraEnergia = false; d->lDirty = true; return 0;
            case WM_LBUTTONUP: {
                if (y > d->LH) { d->chiudiLauncher(); return 0; }   // clic sotto (durante l'animazione): chiude
                if (d->lSopraEnergia) { POINT p{(LONG)x, (LONG)y}; ClientToScreen(h, &p); d->menuEnergia(p); return 0; }
                const int i = d->lCellaIn(x, y);
                if (i >= 0) d->avviaApp(i);
                return 0;
            }
            case WM_RBUTTONUP: {   // il menu di Windows dell'app (amministratore, percorso, disinstalla...)
                const int i = d->lCellaIn(x, y);
                if (i >= 0) { POINT p{(LONG)x, (LONG)y}; ClientToScreen(h, &p); d->menuApp(i, p); }
                return 0;
            }
            // sottomenu del menu di Windows ("Invia a", "Apri con"...): li disegna lui
            case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR:
                if (d->menuShell3) { LRESULT r = 0; if (SUCCEEDED(d->menuShell3->HandleMenuMsg2(m, w, l, &r))) return r; }
                else if (d->menuShell2) { if (SUCCEEDED(d->menuShell2->HandleMenuMsg(m, w, l))) return m == WM_INITMENUPOPUP ? 0 : TRUE; }
                break;
            case WM_MOUSEWHEEL:
                d->lScrollObj = std::clamp(d->lScrollObj - GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA * d->lCellaH() * 0.9, 0.0, d->lScrollMax());
                d->lDirty = true; return 0;
            case WM_CHAR:
                if (w == 27) d->chiudiLauncher();
                else if (w == 13) d->avviaApp(!d->query.empty() || d->lSopra == d->lSel ? d->lSel : -1);   // Invio: la scelta visibile
                else if (w == 8) { if (!d->query.empty()) { d->query.pop_back(); d->filtra(); } }
                else if (w >= 32) { d->query += (wchar_t)w; d->filtra(); }
                return 0;
            case WM_KEYDOWN: {
                int s = d->lSel;
                if (w == VK_LEFT) s--; else if (w == VK_RIGHT) s++; else if (w == VK_UP) s -= 6; else if (w == VK_DOWN) s += 6;
                else break;
                // senza ricerca la prima freccia mostra solo la scelta attuale; la scelta si vede come il passaggio del mouse
                if (d->query.empty() && d->lSopra != d->lSel) s = d->lSel;
                d->lSel = std::clamp(s, 0, std::max(0, (int)d->filtrate.size() - 1));
                if (d->query.empty() && !d->filtrate.empty()) d->lSopra = d->lSel;
                d->selezionaVisibile(); d->lDirty = true;
                return 0;
            }
            case WM_ACTIVATE:
                if (LOWORD(w) == WA_INACTIVE) d->chiudiLauncher();
                return 0;
            case WM_CLOSE: d->chiudiLauncher(); return 0;   // Alt+F4: come Esc (la finestra non si distrugge, si riusa)
        }
        return DefWindowProcW(h, m, w, l);
    }

    // ---- mouse ----
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
        if (scelta) { ShowWindow(scelta, SW_RESTORE); SetForegroundWindow(scelta); return; }
        // la finestra piu' in alto fra quelle dell'app
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
            if (std::find(v.finestre.begin(), v.finestre.end(), h) != v.finestre.end()) { scelta = h; break; }
        if (scelta) SetForegroundWindow(scelta);
    }

    void menu(int i, POINT pt) {
        if (i < 0 || i >= (int)voci.size() || voci[i].uscente) return;
        // copia: mentre il menu e' aperto (ha un suo ciclo dei messaggi) l'elenco delle app puo' essere rifatto da
        // capo, e un riferimento a voci[i] resterebbe a puntare a memoria liberata
        const Voce v = voci[i];
        if (v.avvio == AVVIO_LAUNCHER) return;
        // eseguibile vero sul disco (non quelli protetti delle app dello Store)
        const bool exeVero = !v.exe.empty() && v.exe.find(L"\\windowsapps\\") == std::wstring::npos &&
                             GetFileAttributesW(v.exe.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool store = v.avvio.rfind(L"shell:", 0) == 0;
        const bool collegamento = v.fissata && v.avvio.size() > 4 && minuscolo(v.avvio.substr(v.avvio.size() - 4)) == L".lnk";
        std::vector<menu::Voce> voci = {menu::titolo(v.nome), menu::voce(L"Apri nuova finestra", 1)};
        if (!store || exeVero) voci.push_back(menu::voce(L"Esegui come amministratore", 3));
        if (exeVero) voci.push_back(menu::voce(L"Apri percorso file", 4));
        if (collegamento) { voci.push_back(menu::separatore()); voci.push_back(menu::voce(L"Rimuovi dal dock", 5)); }
        else if (!v.fissata) { voci.push_back(menu::separatore()); voci.push_back(menu::voce(L"Aggiungi al dock", 6)); }
        if (!v.finestre.empty()) {
            voci.push_back(menu::separatore());
            voci.push_back(menu::voce(v.finestre.size() > 1 ? L"Chiudi tutte le finestre" : L"Chiudi", 2));
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
                if (d->statoInPunto((float)pt.x, (float)pt.y) == CONTROLLO) d->cambiaVolume(GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA * 0.02f);
                return 0;
            }
            case WM_TIMER:
                if (w == TIMER_BORDO) { d->controllaBordo(); return 0; }
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
