// FluidWin - Windows reimmaginato: dock, capsula dei tasti, finestre di gelatina e animazioni in stile macOS/iOS,
// senza iniettare codice nelle app.
//
// Come funziona:
//  - ascolta gli eventi delle finestre con SetWinEventHook (fuori dal processo delle app)
//  - quando si preme "riduci a icona" (il tasto dell'app o quello della capsula, o l'icona nel dock) fotografa la
//    finestra e la rende trasparente: si riduce sotto la sua foto, che entra nell'icona del dock con l'effetto genio
//    (disegnato dalla GPU in un overlay); da ridotta la finestra resta trasparente, pronta per il ripristino
//  - ingrandimento e ripristino, apertura (dal dock col genio, altrimenti crescendo dal centro) e chiusura sono
//    animati da FluidWin (le animazioni native di Windows sono spente sulle finestre delle app); le riduzioni non
//    anticipate restano quelle di Windows
//  - un processo guardiano rimette a posto finestre e barra di Windows se FluidWin termina in modo anomalo

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <vector>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <tlhelp32.h>
#include "genio.h"
#include "viva.h"
#include <memory>
#include "scala.h"
#include "dock.h"
#include "menu.h"
#include "impostazioni.h"
#include "osd.h"
#include "panoramica.h"
#include "ricerca.h"
#include "banner.h"

// ---------------------------------------------------------------------------------------------
// Parametri

static const wchar_t* PROP_NASCOSTA = L"FluidWin.Nascosta";   // finestra resa trasparente da noi
static const wchar_t* PROP_TRANSIZ  = L"FluidWin.Transizioni"; // animazioni native spente da noi
static const wchar_t* PROP_ALFA     = L"FluidWin.AlfaApp";     // trasparenza originale delle finestre gia' layered
static const UINT     WM_TRAY       = WM_APP + 1;

struct Molla {
    double risposta;     // secondi per un'oscillazione (come "response" di SwiftUI)
    double smorzamento;  // 1 = nessun rimbalzo, < 1 = leggero rimbalzo
    double velocita;     // velocita' iniziale, in frazioni del tragitto al secondo (0 = parte da ferma)
    double omega() const { return 2.0 * M_PI / risposta; }
    // distanza dall'arrivo (1 all'inizio, 0 all'arrivo) al tempo t
    double distanza(double t) const {
        const double w = omega(), z = smorzamento, v0 = -velocita;
        if (z < 1.0) {
            const double wd = w * std::sqrt(1.0 - z * z);
            return std::exp(-z * w * t) * (std::cos(wd * t) + ((v0 + z * w) / wd) * std::sin(wd * t));
        }
        return std::exp(-w * t) * (1.0 + (v0 + w) * t);
    }
    double valore(double t) const { return 1.0 - distanza(t); }
    // limite superiore della distanza ancora da percorrere (frazione del tragitto) al tempo t
    double residuo(double t) const {
        const double w = omega(), z = smorzamento, v0 = -velocita;
        if (z < 1.0) { const double wd = w * std::sqrt(1.0 - z * z); return std::exp(-z * w * t) * (1.0 + std::abs(v0 + z * w) / wd); }
        return std::exp(-w * t) * (1.0 + std::abs(v0 + w) * t);
    }
};

static const Molla  MOLLA_RIDUCI     {0.36, 1.00, 0.0};
// tragitti lunghi: quasi senza rimbalzo (strisciando al pixel si vedrebbe); parte gia' lanciata per non sembrare in ritardo
static const Molla  MOLLA_RIPRISTINA {0.36, 0.94, 3.0};
// ingrandisci e ripristina: l'angolo in alto a destra (quello della capsula) va subito al suo posto, il resto della
// finestra lo segue piu' lento e arriva molleggiando, come una gelatina tirata da quell'angolo
static const Molla  MOLLA_ANGOLO     {0.14, 1.00, 3.0};
static const Molla  MOLLA_RESTO      {0.46, 0.62, 0.0};
static const int    LARGHEZZA_BARRA  = 72;     // px: dimensione della finestra quando "entra" nella barra
static const double SFUMA_OMBRA      = 0.080;  // s: mentre il pulsante e' premuto l'ombra vera sfuma sotto la copia
static const double ANNULLA_DOPO     = 0.150;  // s: pulsante rilasciato senza riduzione = clic annullato
static const double RAMPA_FINESTRA   = 0.150;  // s: la finestra vera (e la sua ombra) ricompare sotto la miniatura
static const double SFUMA_FOTO       = 0.120;  // s: la foto del genio sfuma (comincia a meta' della rampa)
static const double ALZA_OMBRA       = 0.20;   // s: l'ombra della gelatina passa da posata a sollevata (e indietro)
static const double DISSOLVENZA_MINI = 0.050;  // s: la miniatura sfuma sulla finestra vera
static const double ATTESA_STILE     = 0.150;  // s: poi si toglie lo stile layered
static const double DURATA_GENIO_RIDUCI     = 0.50;  // s
static const double DURATA_GENIO_RIPRISTINA = 0.45;  // s
static const int    LARGHEZZA_ICONA         = 48;    // px: la "bocca" sulla barra in cui entra il genio
static const double DURATA_CRESCITA         = 0.45;  // s: finestra aperta fuori dal dock che cresce dal centro...
static const double SCALA_CRESCITA          = 0.08;  // ... partendo da questa frazione della sua misura

// ---------------------------------------------------------------------------------------------
// Stato

enum class Tipo { Pronta, Riduci, Ripristina };

struct Anim {
    HWND       hwnd = nullptr;
    HTHUMBNAIL thumb = nullptr;
    Tipo       tipo = Tipo::Pronta;
    Molla      molla{};
    RECT       da{}, a{};        // rettangoli visibili di partenza e arrivo (coordinate schermo)
    RECT       sorgente{};       // parte visibile della finestra dentro la miniatura
    double     t0 = 0;
    // Pronta: 0 premuto, 1 rilasciato (in attesa della riduzione)
    // Riduci: 0 molla
    // Ripristina: 0 molla, 1 compare la finestra vera, 2 sfuma la miniatura, 3 attesa prima di togliere lo stile
    int        fase = 0;
    double     tFase = 0;
    double     ampiezza = 1;     // spostamento massimo in pixel, per capire quando la molla e' ferma
    RECT       attuale{};
    // effetto genio: al posto della miniatura si disegna la foto della finestra deformata dalla GPU
    bool       genio = false;
    int        fotogrammiGenio = 0;  // la miniatura resta sotto i primi fotogrammi del genio, finche' e' sicuramente a schermo
    double     pGenio = 0;           // punto raggiunto dal genio (0 finestra, 1 icona): un nuovo clic lo inverte da qui
    bool       cresce = false;       // apertura fuori dal dock: la foto cresce dal centro della finestra (senza icona)
    RECT       fin{};                // Pronta: GetWindowRect alla pressione (se cambia, la finestra si e' mossa davvero)
    bool       zoom = false;         // ingrandimento o ripristino: la copia va dal vecchio rettangolo al nuovo, con la capsula
    // ingrandimento: la miniatura sta in una finestrella sua grande quanto lei (non layered: DWM le arrotonda gli
    // angoli come alle finestre vere e le da' l'ombra), invece che nell'overlay a tutto schermo
    HWND       vetrina = nullptr;
    // ... poi, appena c'e' la foto della finestra alla nuova misura, la disegna la GPU come la gelatina: griglia
    // tirata dall'angolo in alto a destra, angoli tondi e ombra nello shader, tutto nello stesso fotogramma
    genio::Immagine fotoZ;
    genio::Immagine fotoPrima;       // lo stato di prima (se lo si sapeva in anticipo: tasto della capsula): sfuma nella nuova
    int        passiZ = 0;           // 0 foto da fare, 1..2 foto a schermo sotto la finestrella, 3 animazione
    std::vector<genio::Vertice> magliaZ, ombraZ;
    // La foto alla misura nuova si rifa' in sottofondo durante l'animazione: le app come Chromium ridisegnano la
    // pagina alla misura nuova solo dopo qualche fotogramma, e la prima foto (fatta subito) ha ancora l'impaginazione
    // vecchia, ancorata in alto a sinistra (contenuto spostato, che non segue la finestra). La foto nuova arriva
    // sfumando sopra quella di prima
    stampa::Lavoro* rilettura = nullptr;   // foto in corso nel thread
    RECT wrRilettura{};                    // rettangolo della finestra quando e' partita
    int riletture = 0;
    genio::Immagine fotoZVecchia;          // la foto sostituita, che sfuma
    double tCambioZ = -1, alfaVecchia = 0;
    // La foto di prima (impaginazione vecchia, giusta, solo rimpicciolita o allargata) resta sopra finche' la pagina
    // alla misura nuova non si e' assestata (due riletture di fila uguali), poi sfuma
    std::vector<UINT> firmaZ;              // pochi pixel campione dell'ultima rilettura
    bool assestata = false;
    double tAssestata = -1, alfaPrima = 1;
};

// libera tutte le foto di un'animazione di ingrandimento (e la foto in corso, se c'e')
static void liberaFotoZoom(Anim& a) {
    a.fotoZ.libera(); a.fotoPrima.libera(); a.fotoZVecchia.libera();
    if (a.rilettura) { stampa::abbandona(a.rilettura); a.rilettura = nullptr; }
}

struct InfoFinestra {
    RECT finestra{};   // GetWindowRect
    RECT visibile{};   // DWMWA_EXTENDED_FRAME_BOUNDS
    bool valida = false;
    // bordi invisibili (visibile dentro finestra), misurati quando lo stile della finestra e' stabile: subito dopo
    // che diventa o smette di essere layered DWM riporta per un po' il rettangolo sbagliato (circa 7 px di differenza)
    RECT bordi{};
    bool bordiOk = false;
    RECT bordiZ{};         // ... e da ingrandita (sono diversi: in alto 0 da normale, 8 da ingrandita)
    bool bordiZOk = false;
    double tStile = -10;   // ultimo cambio dello stile layered fatto da noi
    bool zoom = false;     // ingrandita all'ultima lettura (se cambia: animazione di ingrandimento/ripristino)
};

static HWND gOverlay, gMessaggi;
static POINT gOrigine;                      // angolo in alto a sinistra dello schermo virtuale
static bool gOverlayVisibile = false;
static bool gPausa = false;
// liste e non vector: durante un passo si tengono puntatori agli elementi (foto, griglie) e se ne possono
// aggiungere altri (apertura dal dock, ingrandimento): un vector riallocandosi li lascerebbe appesi
static std::list<Anim> gAnim;
static std::unordered_map<HWND, InfoFinestra> gInfo;
static std::unordered_set<HWND> gNascoste;
static double gFreq;
static FILE* gLog;
static genio::Motore gGenio;
static bool gGenioOk = false;
static std::unordered_map<HWND, genio::Immagine> gFoto;   // foto delle finestre ridotte col genio, per il ripristino
static dock::Motore gDock;
static bool gDockOk = false;
// px a 96 dpi: angoli delle finestre di Windows 11 (quelli di DWM). Per arrotondarli di piu' c'e' solo la mod di
// Windhawk "Custom Window Corner Radius": FluidWin non li simula piu' (due metodi insieme si sovrapponevano)
static const int RAGGIO_ANGOLI = 8;
static const wchar_t* CHIAVE_FLUIDWIN = L"Software\\FluidWin";   // stato originale della barra di Windows, per il recupero

static double gRaggioBase = RAGGIO_ANGOLI;   // px a 96 dpi: angoli delle finestre (quelli di Windhawk, se c'e')

// angoli di una finestra in pixel, ai DPI del suo monitor (le sue foto sono a quei DPI)
// scala a cui la finestra si vede (1 al 100%, 1.5 al 150%...; vedi scala.h): le distanze in pixel pensate al 100% si
// moltiplicano per questa
static double scalaFinestra(HWND h) { return scala::di(h); }
static double raggioDi(HWND h) { return gRaggioBase * scalaFinestra(h); }

static void liberaFoto(HWND h) {
    auto it = gFoto.find(h);
    if (it != gFoto.end()) { it->second.libera(); gFoto.erase(it); }
}

static double ora() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart / gFreq; }

static void riavvia(const char* perche);
// DPI dello schermo principale (96 = 100%) e quelli che aveva all'avvio
static UINT dpiPrincipale() { return (UINT)std::lround(vetro::dpiMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)) * 96); }
static UINT gDpiAvvio = 96;

static void logf(const char* fmt, ...) {
    if (!gLog) return;
    va_list a; va_start(a, fmt);
    fprintf(gLog, "%10.3f ", ora()); vfprintf(gLog, fmt, a); fputc('\n', gLog); fflush(gLog);
    va_end(a);
}

// ---------------------------------------------------------------------------------------------
// Finestre

static bool finestraApp(HWND h) {
    if (!IsWindow(h) || GetAncestor(h, GA_ROOT) != h) return false;
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE), ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (st & WS_CHILD) return false;
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    if ((st & WS_CAPTION) != WS_CAPTION) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    wchar_t c[64]; GetClassNameW(h, c, 64);
    static const wchar_t* escluse[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
                                       L"#32768", L"TaskManagerWindow", L"Windows.UI.Core.CoreWindow",
                                       L"XamlExplorerHostIslandWindow", L"ForegroundStaging", L"MultitaskingViewFrame"};
    for (auto e : escluse) if (!wcscmp(c, e)) return false;
    return true;
}

static bool leggiRettangoli(HWND h, InfoFinestra& i) {
    if (IsIconic(h)) return false;
    RECT w; if (!GetWindowRect(h, &w)) return false;
    if (w.left <= -32000 || w.right - w.left < 40 || w.bottom - w.top < 40) return false;
    RECT v; if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &v, sizeof(v)))) v = w;
    const bool instabile = GetPropW(h, PROP_NASCOSTA) || ora() - i.tStile < 1.5;
    const bool z = IsZoomed(h) != FALSE;
    RECT& bo = z ? i.bordiZ : i.bordi;
    bool& boOk = z ? i.bordiZOk : i.bordiOk;
    if (instabile && boOk) {
        v = {w.left + bo.left, w.top + bo.top, w.right - bo.right, w.bottom - bo.bottom};
    } else if (instabile && z) {
        // ingrandita senza bordi ancora misurati: e' l'area di lavoro del suo monitor
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) v = mi.rcWork;
    } else if (!instabile) {
        const RECT b = {v.left - w.left, v.top - w.top, w.right - v.right, w.bottom - v.bottom};
        if (b.left >= 0 && b.top >= 0 && b.right >= 0 && b.bottom >= 0 && b.left <= 24 && b.top <= 24 && b.right <= 24 && b.bottom <= 24) {
            bo = b; boOk = true;
        }
    }
    i.finestra = w; i.visibile = v; i.valida = true; i.zoom = IsZoomed(h) != FALSE;
    return true;
}

static void segnaCambioStile(HWND h) {
    auto it = gInfo.find(h);
    if (it != gInfo.end()) it->second.tStile = ora();
}

// Ingrandimento e ripristino li anima FluidWin (con la capsula attaccata): sulle finestre delle app le animazioni
// native restano spente finche' FluidWin e' attivo, non in pausa e con le animazioni accese
// "Effetti di animazione" di Windows (Accessibilita' > Effetti visivi): spenti, FluidWin non muove le finestre (genio,
// gelatina, crescita, chiusura, ingrandimento, onda); restano le dissolvenze dei suoi pannelli. Riletto a ogni
// WM_SETTINGCHANGE
static bool gMovimento = true;
static void leggiMovimento() {
    BOOL si = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &si, 0)) si = TRUE;
    gMovimento = si != FALSE;
}
static bool zoomAnimato() { return !gPausa && gGenioOk && gMovimento && impostazioni::valori().genio; }

static void spegniTransizioniNative(HWND h) {
    if (GetPropW(h, PROP_TRANSIZ)) return;
    BOOL si = TRUE;
    if (SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &si, sizeof(si))))
        SetPropW(h, PROP_TRANSIZ, (HANDLE)1);
}

static InfoFinestra* traccia(HWND h) {
    auto it = gInfo.find(h);
    if (it != gInfo.end()) return &it->second;
    if (!finestraApp(h)) return nullptr;
    InfoFinestra& i = gInfo[h];
    leggiRettangoli(h, i);
    if (zoomAnimato()) spegniTransizioniNative(h);
    return &i;
}

// forza = riaccese comunque (pausa, animazioni spente, uscita); senza, sulle finestre seguite restano spente
static void riaccendiTransizioniNative(HWND h, bool forza = false) {
    if (!GetPropW(h, PROP_TRANSIZ)) return;
    if (!forza && zoomAnimato() && gInfo.count(h)) return;
    BOOL no = FALSE; DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &no, sizeof(no));
    RemovePropW(h, PROP_TRANSIZ);
}

// pausa, ripresa o animazioni accese/spente nelle impostazioni: tutte le finestre seguite si adeguano
static void aggiornaTransizioni() {
    for (auto& [h, i] : gInfo) {
        if (!IsWindow(h)) continue;
        if (zoomAnimato()) spegniTransizioniNative(h);
        else if (!GetPropW(h, PROP_NASCOSTA)) riaccendiTransizioniNative(h, true);
    }
}

// Finestre gia' layered per conto loro (Chromium, Electron: Claude, Brave...): si possono nascondere solo se usano
// una trasparenza unica (SetLayeredWindowAttributes con LWA_ALPHA), che poi si rimette com'era
static bool layeredDaSola(HWND h) {
    return (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) && !GetPropW(h, PROP_NASCOSTA);
}
static bool nascondibile(HWND h) {
    if (!layeredDaSola(h)) return true;
    COLORREF k; BYTE a = 0; DWORD fl = 0;
    return GetLayeredWindowAttributes(h, &k, &a, &fl) && (fl & LWA_ALPHA) && !(fl & LWA_COLORKEY) && a > 0;
}

// Rende la finestra vera trasparente (con l'alfa indicato); false se non si puo'
static bool rendiLayered(HWND h, BYTE alfa) {
    if (!GetPropW(h, PROP_NASCOSTA)) {
        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) {
            // gia' layered dall'app: si ricorda la sua trasparenza (1..255) e lo stile resta suo
            COLORREF k; BYTE a = 0; DWORD fl = 0;
            if (!GetLayeredWindowAttributes(h, &k, &a, &fl) || !(fl & LWA_ALPHA) || (fl & LWA_COLORKEY) || a == 0) return false;
            SetPropW(h, PROP_ALFA, (HANDLE)(INT_PTR)a);
            SetPropW(h, PROP_NASCOSTA, (HANDLE)1);
            gNascoste.insert(h);
        } else {
            SetPropW(h, PROP_NASCOSTA, (HANDLE)1);
            SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
            segnaCambioStile(h);
            gNascoste.insert(h);
        }
    }
    // la trasparenza dell'app e' il massimo: la finestra non diventa mai piu' opaca di com'era
    if (const INT_PTR orig = (INT_PTR)GetPropW(h, PROP_ALFA)) alfa = (BYTE)(alfa * orig / 255);
    if (!SetLayeredWindowAttributes(h, 0, alfa, LWA_ALPHA)) {
        if (!GetPropW(h, PROP_ALFA)) SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        RemovePropW(h, PROP_NASCOSTA); RemovePropW(h, PROP_ALFA);
        gNascoste.erase(h);
        return false;
    }
    return true;
}

static void alfaFinestra(HWND h, BYTE alfa) {
    if (!GetPropW(h, PROP_NASCOSTA)) return;
    if (const INT_PTR orig = (INT_PTR)GetPropW(h, PROP_ALFA)) alfa = (BYTE)(alfa * orig / 255);
    SetLayeredWindowAttributes(h, 0, alfa, LWA_ALPHA);
}

// Rimette la finestra com'era: opaca (o con la trasparenza sua), senza lo stile layered aggiunto da noi
// e con le animazioni native
static void rendiVisibileFinestra(HWND h) {
    riaccendiTransizioniNative(h);
    if (!GetPropW(h, PROP_NASCOSTA)) { gNascoste.erase(h); return; }
    if (const INT_PTR orig = (INT_PTR)GetPropW(h, PROP_ALFA)) {
        SetLayeredWindowAttributes(h, 0, (BYTE)orig, LWA_ALPHA);   // layered per conto suo: resta layered
        RemovePropW(h, PROP_ALFA);
    } else {
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        segnaCambioStile(h);
        // alcune app (il nuovo Blocco note) dopo il cambio di stile possono lasciare un pezzo non ridisegnato: si
        // chiede di ridisegnare tutto (senza aspettare l'app). Solo qui: le layered di loro non cambiano stile
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
    }
    RemovePropW(h, PROP_NASCOSTA);
    gNascoste.erase(h);
}

// Rimette la barra di Windows com'era prima del dock (se FluidWin si e' chiuso male lo fa il guardiano)
static void ripristinaBarra() {
    DWORD stato = 0, n = sizeof(stato);
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_FLUIDWIN, L"StatoBarra", RRF_RT_REG_DWORD, nullptr, &stato, &n) != ERROR_SUCCESS) return;
    APPBARDATA tb{}; tb.cbSize = sizeof(tb); tb.lParam = stato;
    SHAppBarMessage(ABM_SETSTATE, &tb);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_FLUIDWIN, L"StatoBarra");
}

static void salvaStatoBarra() {
    APPBARDATA tb{}; tb.cbSize = sizeof(tb);
    const DWORD stato = (DWORD)SHAppBarMessage(ABM_GETSTATE, &tb);
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_FLUIDWIN, L"StatoBarra", REG_DWORD, &stato, sizeof(stato));
}

// Con il dock la barra di Windows sparisce del tutto (non risale neanche col mouse in fondo allo schermo):
// la sua finestra viene solo nascosta, Explorer continua a funzionare. All'uscita, o dal guardiano, ricompare.
static void mostraBarreWindows(bool si) {
    for (const wchar_t* cls : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        HWND t = nullptr;
        while ((t = FindWindowExW(nullptr, t, cls, nullptr))) ShowWindowAsync(t, si ? SW_SHOWNA : SW_HIDE);
    }
}

// La finestra risponde ai messaggi entro 'ms'? SetWindowLongPtr su una finestra di un altro programma aspetta che lei
// risponda, senza limite: con una finestra che non li legge mai (una nascosta di nvcontainer, NVCapContext) si restava
// bloccati per sempre, guardiano e avvio di FluidWin compresi
static bool risponde(HWND h, UINT ms) {
    DWORD_PTR r = 0;
    return SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, ms, &r) != 0;
}

// Usata dal guardiano e all'avvio: rimette a posto qualsiasi finestra lasciata nascosta o senza animazioni
static void ripristinaTutte() {
    ripristinaBarra();
    mostraBarreWindows(true);
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        if (GetPropW(h, PROP_NASCOSTA)) {
            if (const INT_PTR orig = (INT_PTR)GetPropW(h, PROP_ALFA)) {
                SetLayeredWindowAttributes(h, 0, (BYTE)orig, LWA_ALPHA);
                RemovePropW(h, PROP_ALFA);
            } else {
                SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);   // (questo non aspetta la finestra: torna visibile)
                if (risponde(h, 300)) SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
            }
            RemovePropW(h, PROP_NASCOSTA);
        }
        if (GetPropW(h, PROP_TRANSIZ)) {
            BOOL no = FALSE; DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &no, sizeof(no));
            RemovePropW(h, PROP_TRANSIZ);
        }
        // finestre classiche a cui FluidWin ha tolto i tasti originali (WS_SYSMENU): tornano com'erano
        if (GetPropW(h, L"FluidWin.SenzaTasti")) {
            if (risponde(h, 300)) SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | WS_SYSMENU);
            SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            RemovePropW(h, L"FluidWin.SenzaTasti");
        }
        return TRUE;
    }, 0);
}

// ---------------------------------------------------------------------------------------------
// Barra delle applicazioni: dove "va" la finestra quando si riduce

// Barra delle applicazioni del monitor della finestra; false se su quel monitor non c'e'
static bool barraDi(HWND h, RECT& barra, MONITORINFO& mi) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    barra = {0, 0, 0, 0};
    for (const wchar_t* cls : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        HWND t = nullptr;
        while ((t = FindWindowExW(nullptr, t, cls, nullptr))) {
            RECT r; GetWindowRect(t, &r);
            if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) == mon) barra = r;
        }
    }
    return barra.right > barra.left;
}

// Dove entra il genio: una "bocca" al centro della barra. Solo con la barra orizzontale in basso,
// l'unico caso per cui la curva e' pensata; altrimenti si usa l'animazione a molla.
static bool iconaBarra(HWND h, RECT& out) {
    if (gDockOk && gDock.iconaPer(h, out)) return true;   // con il dock: l'icona dell'app
    RECT barra; MONITORINFO mi;
    if (!barraDi(h, barra, mi)) return false;
    if (barra.right - barra.left <= barra.bottom - barra.top) return false;            // verticale
    if (barra.top < (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2) return false;         // in alto
    const int cx = (barra.left + barra.right) / 2;
    out = {cx - LARGHEZZA_ICONA / 2, barra.top + 6, cx + LARGHEZZA_ICONA / 2, barra.bottom - 6};
    return true;
}

static RECT puntoBarra(HWND h) {
    RECT barra; MONITORINFO mi;
    if (!barraDi(h, barra, mi))   // nessuna barra trovata: centro del bordo inferiore
        barra = {mi.rcMonitor.left, mi.rcMonitor.bottom - 48, mi.rcMonitor.right, mi.rcMonitor.bottom};
    RECT ic;
    if (gDockOk && gDock.iconaPer(h, ic)) barra = ic;   // con il dock: verso l'icona dell'app
    // piccola copia con le stesse proporzioni della finestra, centrata sulla barra
    RECT v{}; if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &v, sizeof(v))) || IsIconic(h)) {
        auto it = gInfo.find(h); if (it != gInfo.end() && it->second.valida) v = it->second.visibile; else v = {0, 0, 4, 3};
    }
    const double prop = std::clamp((double)(v.bottom - v.top) / std::max(1L, v.right - v.left), 0.3, 2.0);
    const int w = LARGHEZZA_BARRA, hh = (int)std::lround(LARGHEZZA_BARRA * prop);
    const int cx = (barra.left + barra.right) / 2, cy = (barra.top + barra.bottom) / 2;
    return {cx - w / 2, cy - hh / 2, cx - w / 2 + w, cy - hh / 2 + hh};
}

// ---------------------------------------------------------------------------------------------
// Pausa automatica con giochi e app a schermo intero

static const char* gPercheSchermoIntero = "";   // perche' l'ultimo controllo ha detto "schermo intero" (registro)

static bool schermoInteroAttivo() {
    QUERY_USER_NOTIFICATION_STATE s;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    // In primo piano una finestra di FluidWin (ricerca, panoramica, impostazioni, copertura di un'animazione): non e'
    // mai un'app a schermo intero. Prima qui si chiedeva lo stato a Windows, che diceva "occupato" per l'app ingrandita
    // rimasta dietro: il dock spariva aprendo la ricerca e subito dopo un ripristino col genio, poi ricompariva
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t c[64] = L""; GetClassNameW(fg, c, 64);
    if (!wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW")) return false;
    // la finestra in primo piano copre esattamente tutto il suo schermo?
    RECT r; GetWindowRect(fg, &r);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& m = mi.rcMonitor;
    const bool tuttoSchermo = r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
    // giochi in Direct3D esclusivo e modalita' presentazione: lo dice Windows. "Occupato" invece Windows lo dice anche
    // per una finestra solo ingrandita (con la sua barra nascosta copre tutto): vale solo se la finestra in primo piano
    // copre davvero tutto lo schermo e non e' un'app ingrandita (video e giochi a tutto schermo non sono "ingranditi")
    const bool ingrandita = IsZoomed(fg) && finestraApp(fg);
    if (!gOverlayVisibile && !gGenio.visibile && SUCCEEDED(SHQueryUserNotificationState(&s))) {
        if (s == QUNS_RUNNING_D3D_FULL_SCREEN) { gPercheSchermoIntero = "gioco Direct3D a schermo intero"; return true; }
        if (s == QUNS_PRESENTATION_MODE) { gPercheSchermoIntero = "modalita' presentazione"; return true; }
        if (s == QUNS_BUSY && tuttoSchermo && !ingrandita) { gPercheSchermoIntero = "app a tutto schermo (occupato)"; return true; }
    }
    if (finestraApp(fg) || !tuttoSchermo) return false;
    gPercheSchermoIntero = "finestra senza bordi grande quanto lo schermo";
    return true;
}

// A schermo intero c'e' un gioco? Lo dice Windows per i giochi Direct3D esclusivi; per gli altri (quasi tutti oggi
// sono finestre senza bordi) si guarda il nome dell'eseguibile nell'elenco dei giochi che Windows ha riconosciuto
// (Game Bar: HKCU\System\GameConfigStore\Children\*\MatchedExeFullPath) e in quello dell'utente
// (HKCU\Software\FluidWin\Giochi, piu' righe: nomi come "gioco.exe"). Il nome si legge dall'istantanea dei processi
// di Windows, senza aprire il processo del gioco (gli anti-cheat controllano chi lo apre).
static std::wstring minuscolo(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

static const std::unordered_set<std::wstring>& giochiNoti() {
    static std::unordered_set<std::wstring> el;
    static ULONGLONG letto = 0;
    if (letto && GetTickCount64() - letto < 60000) return el;   // si rilegge ogni minuto (giochi installati nel frattempo)
    letto = GetTickCount64();
    el.clear();
    HKEY c = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &c) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < 2000; i++) {
            wchar_t nome[128]; DWORD ln = 128;
            if (RegEnumKeyExW(c, i, nome, &ln, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t p[MAX_PATH * 2] = L""; DWORD n = sizeof(p) - sizeof(wchar_t);
            if (RegGetValueW(c, nome, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, p, &n) == ERROR_SUCCESS) {
                const wchar_t* b = wcsrchr(p, L'\\');
                el.insert(minuscolo(b ? b + 1 : p));
            }
        }
        RegCloseKey(c);
    }
    std::vector<wchar_t> b(8192); DWORD n = (DWORD)(b.size() * sizeof(wchar_t)) - 4;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", L"Giochi", RRF_RT_REG_MULTI_SZ, nullptr, b.data(), &n) == ERROR_SUCCESS)
        for (const wchar_t* p = b.data(); *p; p += wcslen(p) + 1) el.insert(minuscolo(p));
    static size_t prima = (size_t)-1;
    if (el.size() != prima) { prima = el.size(); logf("giochi riconosciuti (Game Bar e lista dell'utente): %zu", prima); }
    return el;
}

static std::wstring nomeProcesso(DWORD pid) {
    static DWORD ultimo = 0; static std::wstring nome;
    if (pid == ultimo) return nome;
    ultimo = pid; nome.clear();
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return nome;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (pe.th32ProcessID == pid) { nome = minuscolo(pe.szExeFile); break; }
    CloseHandle(s);
    return nome;
}

// da chiamare quando schermoInteroAttivo() ha appena detto si'
static bool giocoASchermoIntero() {
    if (!strcmp(gPercheSchermoIntero, "gioco Direct3D a schermo intero")) return true;
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (!pid) return false;
    const std::wstring n = nomeProcesso(pid);
    return !n.empty() && giochiNoti().count(n) > 0;
}

// ---------------------------------------------------------------------------------------------
// Sempre in primo piano (Ctrl+Win+T): la finestra attiva resta sopra le altre anche lavorando con altre finestre,
// finche' non si ripreme. E' lo stile "sempre in primo piano" di Windows (WS_EX_TOPMOST), messo da fuori con
// SetWindowPos: niente iniezione. All'uscita di FluidWin le finestre tornano normali.
static std::unordered_set<HWND> gInPrimoPiano;
static const int TASTO_PRIMO_PIANO = 1;

static void primoPiano() {
    HWND h = GetForegroundWindow();
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (!h || pid == GetCurrentProcessId()) return;
    wchar_t c[64] = L""; GetClassNameW(h, c, 64);
    if (!wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW") || !wcscmp(c, L"Shell_TrayWnd")) return;
    const bool era = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    SetWindowPos(h, era ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (era) gInPrimoPiano.erase(h); else gInPrimoPiano.insert(h);
    wchar_t t[128] = L""; GetWindowTextW(h, t, 128);
    std::wstring testo = era ? testi::t(L"Non più in primo piano", L"No longer on top") : testi::t(L"Sempre in primo piano", L"Always on top");
    if (t[0]) testo += std::wstring(L" · ") + t;
    logf("sempre in primo piano: %s (%ls)", era ? "tolto" : "messo", c);
    if (gDockOk) gDock.ilNotch().avviso(testo, sensori::S_PUNTINA, -1, !era);
}

static void togliPrimoPiano() {
    for (HWND h : gInPrimoPiano)
        if (IsWindow(h) && risponde(h, 300)) SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    gInPrimoPiano.clear();
}

// ---------------------------------------------------------------------------------------------
// Overlay e miniature

static void aggiornaOrigine() {
    gOrigine = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
    if (gOverlayVisibile)
        SetWindowPos(gOverlay, HWND_TOPMOST, gOrigine.x, gOrigine.y,
                     GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), SWP_NOACTIVATE);
}

// Come per il genio: fuori dalle animazioni l'overlay e' piccolo e fuori schermo, per non sembrare un'app a schermo intero
static void mostraOverlay(bool si) {
    if (si == gOverlayVisibile) return;
    gOverlayVisibile = si;
    if (si) SetWindowPos(gOverlay, HWND_TOPMOST, gOrigine.x, gOrigine.y, GetSystemMetrics(SM_CXVIRTUALSCREEN),
                         GetSystemMetrics(SM_CYVIRTUALSCREEN), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else { ShowWindow(gOverlay, SW_HIDE); SetWindowPos(gOverlay, nullptr, -32000, -32000, 1, 1, SWP_NOACTIVATE | SWP_NOZORDER); }
}

static RECT sorgenteDa(const InfoFinestra& i) {
    return {i.visibile.left - i.finestra.left, i.visibile.top - i.finestra.top,
            i.visibile.right - i.finestra.left, i.visibile.bottom - i.finestra.top};
}

static bool impostaMiniatura(Anim& a, const RECT& dest, BYTE opacita) {
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcDestination = {dest.left - gOrigine.x, dest.top - gOrigine.y, dest.right - gOrigine.x, dest.bottom - gOrigine.y};
    if (a.vetrina) {
        SetWindowPos(a.vetrina, HWND_TOPMOST, dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        p.rcDestination = {0, 0, dest.right - dest.left, dest.bottom - dest.top};
    }
    p.rcSource = a.sorgente;
    p.opacity = opacita;
    p.fVisible = TRUE;
    p.fSourceClientAreaOnly = FALSE;
    return SUCCEEDED(DwmUpdateThumbnailProperties(a.thumb, &p));
}

static void chiudiMiniatura(Anim& a) {
    if (a.thumb) DwmUnregisterThumbnail(a.thumb);
    a.thumb = nullptr;
    if (a.vetrina) { DestroyWindow(a.vetrina); a.vetrina = nullptr; }
}

// le finestre di FluidWin non si distruggono con WM_CLOSE (Alt+F4, o un programma che chiede di chiudere):
// FluidWin le riusa, e distrutte smetterebbe di funzionare. Si chiude solo la finestra dei messaggi (uscita pulita)
static LRESULT CALLBACK procSenzaChiusura(HWND h, UINT m, WPARAM w, LPARAM l) { return m == WM_CLOSE ? 0 : DefWindowProcW(h, m, w, l); }

static LRESULT CALLBACK procVetrina(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) return 0;
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

static HWND creaVetrina() {
    static bool registrata = false;
    if (!registrata) {
        WNDCLASSW wc{}; wc.lpfnWndProc = procVetrina; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FluidWin.Vetrina";
        RegisterClassW(&wc); registrata = true;
    }
    HWND v = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"FluidWin.Vetrina", L"",
                             WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!v) return nullptr;
    const DWM_WINDOW_CORNER_PREFERENCE tondo = DWMWCP_ROUND;
    DwmSetWindowAttribute(v, DWMWA_WINDOW_CORNER_PREFERENCE, &tondo, sizeof(tondo));
    BOOL si = TRUE; DwmSetWindowAttribute(v, DWMWA_TRANSITIONS_FORCEDISABLED, &si, sizeof(si));
    return v;
}

static Anim* animDi(HWND h) {
    for (auto& a : gAnim) if (a.hwnd == h) return &a;
    return nullptr;
}

static void annullaAnimDi(HWND h) {
    for (auto it = gAnim.begin(); it != gAnim.end();) {
        if (it->hwnd == h) {
            // ingrandimento interrotto (riduzione, chiusura...): finestra e capsula tornano normali
            if (it->zoom) { if (IsWindow(h)) rendiVisibileFinestra(h); liberaFotoZoom(*it); }
            chiudiMiniatura(*it); it = gAnim.erase(it);
        } else ++it;
    }
}

static LONG distanzaMassima(const RECT& x, const RECT& y) {
    return std::max({std::abs(x.left - y.left), std::abs(x.top - y.top), std::abs(x.right - y.right), std::abs(x.bottom - y.bottom), 1L});
}

// Istante in cui il prossimo fotogramma composto arrivera' sullo schermo: le posizioni si calcolano su questo,
// cosi' il movimento resta regolare anche se il nostro ciclo ha un ritardo
static double tempoProssimoFotogramma() {
    DWM_TIMING_INFO ti{}; ti.cbSize = sizeof(ti);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod) {
        const double periodo = ti.qpcRefreshPeriod / gFreq, adesso = ora();
        double vb = ti.qpcVBlank / gFreq;
        while (vb <= adesso) vb += periodo;
        return vb + periodo;   // aggiornato ora, composto al prossimo vblank, mostrato a quello dopo
    }
    return ora();
}

static Anim* nuovaAnim(HWND h, Tipo tipo, const RECT& da, const RECT& a, const InfoFinestra& info, BYTE opacita) {
    Anim n;
    n.hwnd = h; n.tipo = tipo; n.da = da; n.a = a; n.attuale = da;
    n.t0 = n.tFase = tempoProssimoFotogramma();
    n.ampiezza = (double)distanzaMassima(da, a);
    n.molla = tipo == Tipo::Ripristina ? MOLLA_RIPRISTINA : MOLLA_RIDUCI;
    n.sorgente = sorgenteDa(info);
    if (FAILED(DwmRegisterThumbnail(gOverlay, h, &n.thumb))) { logf("miniatura non registrata %p", h); return nullptr; }
    impostaMiniatura(n, da, opacita);
    mostraOverlay(true);
    gAnim.push_back(n);
    logf("anim %d su %p da %ld,%ld-%ld,%ld a %ld,%ld-%ld,%ld", (int)tipo, h, da.left, da.top, da.right, da.bottom,
         a.left, a.top, a.right, a.bottom);
    return &gAnim.back();
}

// Interpola centro e dimensione separatamente. Vicino all'arrivo la dimensione si blocca su quella esatta:
// la miniatura allora si sposta soltanto, senza essere ridimensionata (e quindi sfocata) di frazioni di pixel,
// e non c'e' ne' tremolio da arrotondamento ne' uno scatto di nitidezza quando arriva la finestra vera.
static RECT interpola(const RECT& x, const RECT& y, double p) {
    const double wx = x.right - x.left, hx = x.bottom - x.top, wy = y.right - y.left, hy = y.bottom - y.top;
    double w = wx + (wy - wx) * p, h = hx + (hy - hx) * p;
    if (std::abs(w - wy) < 1.5) w = wy;
    if (std::abs(h - hy) < 1.5) h = hy;
    const double cxx = (x.left + x.right) / 2.0, cyx = (x.top + x.bottom) / 2.0;
    const double cxy = (y.left + y.right) / 2.0, cyy = (y.top + y.bottom) / 2.0;
    double cx = cxx + (cxy - cxx) * p, cy = cyx + (cyy - cyx) * p;
    if (std::abs(cx - cxy) < 0.75) cx = cxy;
    if (std::abs(cy - cyy) < 0.75) cy = cyy;
    const LONG W = (LONG)std::lround(w), H = (LONG)std::lround(h);
    const LONG L = (w == wy && cx == cxy) ? y.left : (LONG)std::lround(cx - w / 2.0);
    const LONG T = (h == hy && cy == cyy) ? y.top : (LONG)std::lround(cy - h / 2.0);
    return {L, T, L + W, T + H};
}

static double morbido(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }

// Il pulsante non ha ridotto la finestra: la copia torna finestra vera (ombra in dissolvenza) e si smonta
static void annullaPronta(Anim& a, double t) {
    logf("clic annullato %p", a.hwnd);
    a.tipo = Tipo::Ripristina;
    if (a.genio) { a.fase = 0; a.t0 = t - (1.0 - std::cbrt(a.pGenio)) * DURATA_GENIO_RIPRISTINA; return; }   // ripristino invertito: riprende
    a.fase = 1; a.tFase = t;
}

// ---------------------------------------------------------------------------------------------
// Finestre gelatinose: mentre si trascina una finestra, al suo posto si disegna la sua foto su una griglia
// elastica. Il punto afferrato segue il mouse; il resto della finestra lo insegue con molle sempre piu'
// morbide man mano che ci si allontana, e oscilla un poco quando si ferma.

static const int GEL_C = 14, GEL_R = 11;   // vertici della griglia

struct Gelatina {
    HWND hwnd = nullptr;
    genio::Immagine foto;
    RECT inizio{};                // rettangolo visibile all'inizio (dimensioni della foto)
    RECT inizioFin{};             // GetWindowRect all'inizio: movimenti e dimensioni si confrontano su questo, che non
                                  // salta quando la finestra diventa trasparente (quello "visibile" di DWM si')
    RECT bordi{};                 // differenza fra i due (bordi invisibili per il ridimensionamento)
    double u0 = 0.5, v0 = 0.05;   // punto afferrato (0..1 sulla finestra)
    std::vector<double> x, y, vx, vy;
    // -1 in attesa del primo spostamento, 0 trascinata, 1 si assesta, 2 ricompare la finestra vera,
    // 3 la foto sfuma, 4 attesa prima di togliere lo stile layered
    int fase = -1;
    int fotogrammi = 0;
    bool nascosta = false;        // finestra vera resa trasparente
    bool rinascondi = false;      // ripresa mentre ricompariva: la finestra torna trasparente fra un paio di fotogrammi
    bool lasciata = false;        // fine del trascinamento
    double tFase = 0, tUltimo = 0;
    double alza = 0;              // 0..1: l'ombra passa da "posata" a "sollevata" mentre la si tiene (e torna lasciandola)
    std::vector<genio::Vertice> maglia, ombra;
    // contenuto vivo (Windows.Graphics.Capture): mentre la si trascina si vede la finestra com'e' adesso. 'viva' parte
    // come copia della foto e riceve le immagini nuove
    std::shared_ptr<viva::Cattura> cattura;   // condivisa col thread che la avvia
    bool vivaControllata = false; double tViva = 0;
    genio::Immagine viva;
    bool vivaPronta = false;      // e' arrivata almeno un'immagine viva: si disegna 'viva' invece di 'foto'
    int immaginiVive = 0;
};
static std::list<Gelatina> gGelatine;

static Gelatina* gelatinaDi(HWND h) {
    for (auto& g : gGelatine) if (g.hwnd == h) return &g;
    return nullptr;
}

// Contenuto vivo della gelatina: parte all'inizio del trascinamento (e quando la si riafferra), si ferma quando
// ricompare la finestra vera. Si avvia in sottofondo (35-55 ms dentro Windows): fino ad allora si vede la foto.
// Se non si puo' (Windows vecchio, finestra protetta, misure diverse) resta la foto
static void avviaViva(Gelatina& g) {
    if (g.cattura) return;   // in avvio o gia' attiva
    // finestre che chiedono di non essere catturate (password manager, app protette): si rispetta, resta la foto
    DWORD aff = WDA_NONE;
    if (GetWindowDisplayAffinity(g.hwnd, &aff) && aff != WDA_NONE) { logf("gelatina %p: finestra protetta, niente contenuto vivo", g.hwnd); return; }
    if (!g.viva.tex && !gGenio.copia(g.foto, g.viva)) return;
    g.cattura = std::make_shared<viva::Cattura>();
    g.vivaControllata = false; g.tViva = ora();
    viva::avviaInSottofondo(g.cattura, gGenio.dispositivo(), g.hwnd);
}

static void fermaViva(Gelatina& g) {
    if (!g.cattura) return;
    if (g.cattura->attiva()) logf("gelatina %p: contenuto vivo fermato (%d immagini)", g.hwnd, g.immaginiVive);
    g.cattura.reset();   // ancora in avvio: la chiude il thread dell'avvio quando ha finito
}

// appena pronta: le immagini devono combaciare con la foto pixel per pixel
static bool controllaViva(Gelatina& g) {
    if (g.vivaControllata) return true;
    const int st = g.cattura->statoAvvio();
    if (st == 0) return false;   // ancora in avvio
    if (st < 0) { logf("gelatina %p: contenuto vivo non disponibile", g.hwnd); g.cattura.reset(); return false; }
    if (g.cattura->larghezza() != g.viva.w || g.cattura->altezza() != g.viva.h) {
        logf("gelatina %p: contenuto vivo %dx%d diverso dalla foto %dx%d, resta la foto", g.hwnd, g.cattura->larghezza(), g.cattura->altezza(), g.viva.w, g.viva.h);
        g.cattura.reset(); return false;
    }
    g.vivaControllata = true;
    logf("gelatina %p: contenuto vivo pronto dopo %.0f ms", g.hwnd, (ora() - g.tViva) * 1000);
    return true;
}

// l'ultima immagine arrivata (se ce n'e' una nuova) va in 'viva'
static void aggiornaViva(Gelatina& g) {
    if (!g.cattura || !g.viva.tex || !controllaViva(g)) return;
    ID3D11DeviceContext* ctx = gGenio.contesto();
    if (!g.cattura->copia(ctx, g.viva.tex, 0, 0, RECT{0, 0, g.viva.w, g.viva.h})) return;
    gGenio.rifaiMips(g.viva);
    g.vivaPronta = true; g.immaginiVive++;
}

// rettangolo visibile della gelatina adesso: quello della finestra meno i bordi invisibili misurati all'inizio
static RECT rettGelatina(const Gelatina& g) {
    RECT f{}; GetWindowRect(g.hwnd, &f);
    return {f.left + g.bordi.left, f.top + g.bordi.top, f.right - g.bordi.right, f.bottom - g.bordi.bottom};
}

// Trascinamento iniziato da finestra ingrandita: Windows la rimette normale sotto al mouse solo dopo qualche pixel.
// La gelatina parte allora (al primo spostamento a finestra gia' normale e ridisegnata)
static HWND gDaIngrandita = nullptr;
static double gSganciata = 0;
// finestra in un trascinamento di Windows (tra MOVESIZESTART e MOVESIZEEND)
static HWND gInSpostamento = nullptr;
// Trascinamento fatto dall'app per conto suo (senza il ciclo di spostamento di Windows, quindi senza MOVESIZESTART):
// si riconosce da una finestra che cambia posto, con la stessa misura, mentre il tasto sinistro e' premuto. La fine
// si riconosce dal tasto rilasciato (controllo del timer da 60 ms)
static std::unordered_map<HWND, RECT> gUltimoPosto;
static HWND gTrascinoApp = nullptr;

static void suInizioSpostamento(HWND h) {
    gInSpostamento = h;
    POINT pt; GetCursorPos(&pt);
    if ((!impostazioni::valori().gelatina || !gMovimento) && !gelatinaDi(h)) return;   // gelatina spenta nelle impostazioni
    if (IsZoomed(h) && !gelatinaDi(h)) { gDaIngrandita = h; gSganciata = 0; return; }
    if (Gelatina* g = gelatinaDi(h)) {
        // ripresa mentre oscillava o stava ricomparendo: si riafferra dal nuovo punto, con la stessa foto
        // (rifarla adesso, con la finestra appena tornata normale, verrebbe nera)
        if (g->fase == -1) return;
        const RECT v = rettGelatina(*g);
        g->u0 = std::clamp((pt.x - v.left) / (double)std::max(1L, v.right - v.left), 0.0, 1.0);
        g->v0 = std::clamp((pt.y - v.top) / (double)std::max(1L, v.bottom - v.top), 0.0, 1.0);
        g->lasciata = false;
        if (g->fase >= 2) { g->rinascondi = true; g->fotogrammi = 0; }   // la foto torna a schermo, poi sparisce la finestra
        g->fase = 0;
        avviaViva(*g);   // si era fermato quando era ricomparsa la finestra vera
        logf("gelatina ripresa %p", h);
        return;
    }
    // ogni rinuncia finisce nel registro col suo motivo (per capire le finestre che non diventano di gelatina)
    auto no = [h](const char* perche) { logf("gelatina no %p: %s", h, perche); };
    if (gPausa || !gGenioOk) return;
    if (!finestraApp(h)) { no("non e' una finestra d'app"); return; }
    if (IsZoomed(h) || IsIconic(h)) { no("ingrandita o ridotta"); return; }
    if (animDi(h)) { no("in animazione"); return; }
    if (layeredDaSola(h) && !nascondibile(h)) { no("trasparente per conto suo"); return; }
    if (schermoInteroAttivo()) { no("schermo intero"); return; }
    InfoFinestra* i = traccia(h);
    if (!i || !leggiRettangoli(h, *i)) { no("rettangoli non letti"); return; }
    if (!PtInRect(&i->visibile, pt)) return;   // ridimensionamento dal bordo esterno
    Gelatina g;
    g.hwnd = h; g.inizio = i->visibile;
    GetWindowRect(h, &g.inizioFin);
    g.bordi = {std::clamp(g.inizio.left - g.inizioFin.left, 0L, 24L), std::clamp(g.inizio.top - g.inizioFin.top, 0L, 24L),
               std::clamp(g.inizioFin.right - g.inizio.right, 0L, 24L), std::clamp(g.inizioFin.bottom - g.inizio.bottom, 0L, 24L)};
    const double t1 = ora();
    gGenio.fotoDaSchermo = false;
    if (!gGenio.cattura(h, g.inizio, g.foto, true)) { no("foto non riuscita"); return; }
    if (gGenio.fotoDaSchermo) logf("gelatina %p: foto presa dallo schermo", h);
    const double w = g.inizio.right - g.inizio.left, hh = g.inizio.bottom - g.inizio.top;
    g.u0 = (pt.x - g.inizio.left) / w; g.v0 = (pt.y - g.inizio.top) / hh;
    const int n = GEL_C * GEL_R;
    g.x.resize(n); g.y.resize(n); g.vx.assign(n, 0); g.vy.assign(n, 0);
    for (int r = 0; r < GEL_R; r++) for (int c = 0; c < GEL_C; c++) {
        g.x[r * GEL_C + c] = g.inizio.left + w * c / (GEL_C - 1);
        g.y[r * GEL_C + c] = g.inizio.top + hh * r / (GEL_R - 1);
    }
    g.tUltimo = ora();
    gGelatine.push_back(std::move(g));
    avviaViva(gGelatine.back());
    logf("gelatina pronta %p (foto %.1f ms)", h, (ora() - t1) * 1000);
}

static void suFineSpostamento(HWND h) {
    if (h == gDaIngrandita) gDaIngrandita = nullptr;
    if (h == gInSpostamento) gInSpostamento = nullptr;
    if (Gelatina* g = gelatinaDi(h)) { g->lasciata = true; if (g->fase == 0) { g->fase = 1; g->tFase = ora(); } }
}

static void liberaGelatina(Gelatina& g) {
    if (g.nascosta && IsWindow(g.hwnd)) rendiVisibileFinestra(g.hwnd);
    fermaViva(g); g.viva.libera();
    g.foto.libera();
}

static void passoGelatine(double t, std::vector<genio::Elemento>& elementi) {
    for (auto it = gGelatine.begin(); it != gGelatine.end();) {
        Gelatina& g = *it;
        bool finita = !IsWindow(g.hwnd) || IsIconic(g.hwnd);
        const RECT v = finita ? g.inizio : rettGelatina(g);
        const double w = v.right - v.left, hh = v.bottom - v.top;
        const double dt = std::clamp(t - g.tUltimo, 1.0 / 240, 1.0 / 30);
        g.tUltimo = t;

        if (!finita && g.fase == -1) {
            RECT f; GetWindowRect(g.hwnd, &f);
            // la finestra cambia dimensione: e' un ridimensionamento, niente gelatina
            if (f.right - f.left != g.inizioFin.right - g.inizioFin.left || f.bottom - f.top != g.inizioFin.bottom - g.inizioFin.top) {
                finita = true; logf("gelatina: ridimensionamento %p", g.hwnd);
            }
            else if (f.left != g.inizioFin.left || f.top != g.inizioFin.top) { g.fase = 0; logf("gelatina in movimento %p", g.hwnd); }
            else if (g.lasciata) finita = true;   // solo un clic sulla barra del titolo
        }
        if (!finita && g.fase >= 0) {
            // ombra: si solleva in 0,2 s mentre la si tiene, si riposa in 0,2 s quando la si lascia
            g.alza = std::clamp(g.alza + (g.fase == 0 ? dt : -dt) / ALZA_OMBRA, 0.0, 1.0);
            // molle: ogni vertice insegue il suo posto sulla finestra vera; vicino al punto afferrato la molla e'
            // rigida (e quel punto resta incollato al mouse), lontano e' morbida e oscilla
            const double diag = std::hypot(w, hh);
            // (molle uguali a ogni misura di finestra: renderle piu' rigide per le finestre grandi, per gli schermi
            // larghi, cambiava la gelatina di quasi tutte le finestre sugli schermi da 1440p e l'utente non la voleva
            // cosi'. Si adattano solo alla scala: al 100% e' identica a prima)
            const double sc = scalaFinestra(g.hwnd);
            const double maxDist = 80 * sc;   // deformazione massima (80 punti)
            const int passi = 4; const double h2 = dt / passi;
            bool ferma = true;
            for (int r = 0; r < GEL_R; r++) for (int c = 0; c < GEL_C; c++) {
                const int k = r * GEL_C + c;
                const double u = (double)c / (GEL_C - 1), vv = (double)r / (GEL_R - 1);
                const double ox = v.left + u * w, oy = v.top + vv * hh;
                const double d = std::hypot((u - g.u0) * w, (vv - g.v0) * hh) / std::max(1.0, diag);
                const double rig = 300 + 1500 * std::exp(-(d * d) / (0.35 * 0.35));
                const double sm = 2 * std::sqrt(rig) * 0.50;
                for (int s = 0; s < passi; s++) {
                    g.vx[k] += (rig * (ox - g.x[k]) - sm * g.vx[k]) * h2; g.x[k] += g.vx[k] * h2;
                    g.vy[k] += (rig * (oy - g.y[k]) - sm * g.vy[k]) * h2; g.y[k] += g.vy[k] * h2;
                }
                // il punto afferrato e il suo intorno restano esattamente sotto il mouse
                const double colla = std::exp(-(d * d) / (0.06 * 0.06));
                g.x[k] += (ox - g.x[k]) * colla; g.y[k] += (oy - g.y[k]) * colla;
                // mai oltre maxDist dal posto giusto, anche lanciandola velocissima
                const double dx = g.x[k] - ox, dy = g.y[k] - oy, dist = std::hypot(dx, dy);
                if (dist > maxDist) { g.x[k] = ox + dx * maxDist / dist; g.y[k] = oy + dy * maxDist / dist; }
                if (dist > 0.6 * sc || std::hypot(g.vx[k], g.vy[k]) > 10 * sc) ferma = false;
            }
            if (g.fase == 1 && ((ferma && g.alza <= 0) || t - g.tFase > 2.5)) {
                for (int k = 0; k < GEL_C * GEL_R; k++) {
                    g.x[k] = v.left + w * (k % GEL_C) / (GEL_C - 1); g.y[k] = v.top + hh * (k / GEL_C) / (GEL_R - 1);
                    g.vx[k] = g.vy[k] = 0;
                }
                g.fase = 2; g.tFase = t;
                fermaViva(g);   // ricompare la finestra vera: per la dissolvenza basta l'ultima immagine
            }
        }
        double alfaFoto = 1.0, alfaOmbra = g.fase >= 2 ? 0.0 : 1.0;
        if (!finita && g.fase == 2) {
            // ferma: la finestra vera (con la sua ombra) ricompare sotto la foto, poi la foto sfuma. La nostra ombra
            // sfuma mentre torna quella vera, cosi' non c'e' un attimo senza
            const double k = morbido((t - g.tFase) / RAMPA_FINESTRA);
            alfaOmbra = 1.0 - k;
            if (g.nascosta) alfaFinestra(g.hwnd, (BYTE)std::lround(k * 255));
            if (k >= 1.0) { g.fase = 3; g.tFase = t; }
        } else if (!finita && g.fase == 3) {
            const double k = morbido((t - g.tFase) / (DISSOLVENZA_MINI * 2));
            alfaFoto = 1.0 - k;
            if (k >= 1.0) { g.fase = 4; g.tFase = t; }
        } else if (!finita && g.fase == 4) {
            // la finestra resta pronta (trasparente al 100%, quindi uguale) ancora un secondo: se la si riafferra
            // subito la gelatina riparte con la stessa foto
            alfaFoto = 0;
            if (t - g.tFase >= 1.0) finita = true;
        }

        if (finita) { logf("gelatina finita %p", g.hwnd); liberaGelatina(g); it = gGelatine.erase(it); continue; }
        if (g.fase >= 0 && g.fase <= 3) {
            // la finestra vera sparisce solo quando la foto e' gia' a schermo da un paio di fotogrammi (niente lampi)
            if (g.rinascondi && ++g.fotogrammi > 2) {
                g.rinascondi = false;
                alfaFinestra(g.hwnd, 0);
            }
            if (!g.nascosta && ++g.fotogrammi > 2) {
                if (rendiLayered(g.hwnd, 0)) g.nascosta = true;
                else { logf("gelatina: finestra non nascondibile %p", g.hwnd); liberaGelatina(g); it = gGelatine.erase(it); continue; }
            }
            // griglia della foto e, sotto, quella dell'ombra: un anello in piu' attorno, 80 px fuori (la sfumatura e'
            // lunga; lo spostamento verso il basso lo fa lo shader)
            g.maglia.resize(GEL_C * GEL_R);
            for (int k = 0; k < GEL_C * GEL_R; k++)
                g.maglia[k] = {(float)g.x[k], (float)g.y[k], (float)(k % GEL_C) / (GEL_C - 1), (float)(k / GEL_C) / (GEL_R - 1)};
            const int OC = GEL_C + 2, OR = GEL_R + 2;
            const double fw = g.foto.w, fh = g.foto.h, M = 80 * scalaFinestra(g.hwnd);
            g.ombra.resize(OC * OR);
            for (int r = 0; r < OR; r++) for (int c = 0; c < OC; c++) {
                const int ci = std::clamp(c - 1, 0, GEL_C - 1), ri = std::clamp(r - 1, 0, GEL_R - 1), k = ri * GEL_C + ci;
                const double sx = c == 0 ? -1 : (c == OC - 1 ? 1 : 0), sy = r == 0 ? -1 : (r == OR - 1 ? 1 : 0);
                const double u = (double)ci / (GEL_C - 1) + sx * M / fw, vv = (double)ri / (GEL_R - 1) + sy * M / fh;
                g.ombra[r * OC + c] = {(float)(g.x[k] + sx * M), (float)(g.y[k] + sy * M), (float)u, (float)vv};
            }
            if (g.fase <= 1) aggiornaViva(g);   // trascinata o in assestamento: la finestra com'e' adesso
            const genio::Immagine* img = g.vivaPronta ? &g.viva : &g.foto;
            if (alfaFoto > 0) {
                genio::Elemento o{img, v, v, 0, alfaFoto * alfaOmbra};   // l'ombra vera torna con la finestra
                o.maglia = g.ombra.data(); o.colonne = OC; o.righe = OR; o.ombra = true; o.alza = morbido(g.alza);
                o.raggio = raggioDi(g.hwnd);
                elementi.push_back(o);
                genio::Elemento e{img, v, v, 0, alfaFoto};
                e.maglia = g.maglia.data(); e.colonne = GEL_C; e.righe = GEL_R; e.raggio = o.raggio;
                elementi.push_back(e);
            }
        }
        ++it;
    }
}

// ---------------------------------------------------------------------------------------------
// Apertura: una finestra appena comparsa resta trasparente finche' l'app non l'ha disegnata (la foto non e' piu'
// nera). Poi, se e' la prima finestra di un'app lanciata dal dock, esce dalla sua icona col genio, come un
// ripristino; se e' stata aperta in un altro modo (Start, ricerca, collegamenti, un'altra app) cresce dal centro.

struct Apertura {
    HWND hwnd; RECT icona; double t0; bool dalDock;
    double prova = -1;                 // ultima foto tentata
    stampa::Lavoro* lavoro = nullptr;  // foto in corso nel thread (l'app appena partita puo' rispondere tardi)
    RECT wr{};                         // rettangolo della finestra quando e' partita la foto
};
static std::vector<Apertura> gAperture;

static bool aperturaDi(HWND h) {
    for (auto& a : gAperture) if (a.hwnd == h) return true;
    return false;
}

// Finestre comparse ancora nascoste da DWM (le app di Windows come Impostazioni: la cornice compare "velata" e si
// svela quando il contenuto e' pronto): l'apertura si anima allo svelamento. Svelamenti di finestre non appena
// comparse (cambio di desktop virtuale) non contano
static std::unordered_map<HWND, double> gNateVelate;

// Finestre rese trasparenti appena create, in attesa di comparire. All'evento di comparsa sarebbe tardi: arriva
// qualche millisecondo dopo, quando DWM l'ha gia' mostrata (un lampo della finestra vuota) o ha gia' avviato la
// sua dissolvenza (la finestra vera si vedrebbe sotto la crescita).
// Lo stile layered lo cambia un thread: il cambio aspetta che l'app risponda, e un'app appena creata e' spesso
// ancora occupata ad avviarsi (Mappa caratteri: 400 ms con FluidWin fermo). Finche' il thread non ha finito
// ("in corso") la finestra non si tocca: chi vuole rimostrarla lo segna e lo fa la risposta del thread.
// Se l'app mostra la finestra prima di rispondere al thread (Paint: 256 ms), la finestra si vedrebbe per un attimo:
// alla creazione, quando non si vede ancora, si fotografa lo schermo dove comparira' ("copertura"), e finche' il
// thread non ha finito la si disegna sopra di lei (l'overlay sta sopra). La copertura esiste solo in quell'attesa.
static const UINT WM_NATA_NASCOSTA = WM_APP + 12;   // wParam finestra, lParam 1 se resa trasparente
struct Attesa { double t; bool pronta = false; bool annulla = false; genio::Immagine copertura{}; RECT rc{}; };
static std::unordered_map<HWND, Attesa> gInAttesa;

// foto dello schermo in r (quello che c'e' dietro una finestra che sta per comparire)
static bool fotoSchermo(const RECT& r, genio::Immagine& out) {
    const int w = r.right - r.left, hh = r.bottom - r.top;
    if (w <= 0 || hh <= 0 || (long long)w * hh > 16000000) return false;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits && mem) {
        HGDIOBJ vecchio = SelectObject(mem, dib);
        if (BitBlt(mem, 0, 0, w, hh, scr, r.left, r.top, SRCCOPY)) {
            GdiFlush();
            auto rit = gGenio.ritocco; gGenio.ritocco = nullptr;   // (niente capsula: e' lo sfondo)
            ok = gGenio.carica(nullptr, r, r, (const UINT*)bits, out);
            gGenio.ritocco = rit;
        }
        SelectObject(mem, vecchio);
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    return ok;
}

// coperture da disegnare adesso: thread ancora in corso, finestra gia' visibile e ancora dove era alla creazione
// (se l'app l'ha spostata, la foto dello sfondo starebbe nel posto sbagliato: meglio niente)
static bool coperturaServe(HWND h, const Attesa& a) {
    if (a.pronta || !a.copertura.srv || !IsWindowVisible(h)) return false;
    RECT wr; return GetWindowRect(h, &wr) && EqualRect(&wr, &a.rc);
}
static bool copertureAttive() {
    for (const auto& [h, a] : gInAttesa) if (coperturaServe(h, a)) return true;
    return false;
}
static void elementiCoperture(std::vector<genio::Elemento>& el) {
    for (auto& [h, a] : gInAttesa) {
        if (!coperturaServe(h, a)) continue;
        genio::Elemento e{&a.copertura, a.rc, a.rc, 0.0, 1.0};
        e.raggio = 0;   // rettangolo pieno: e' lo sfondo
        el.push_back(e);
    }
}
static volatile LONG gNascondendo = 0;   // thread ancora in corso (all'uscita si aspettano un poco)

static DWORD WINAPI nascondiNata(LPVOID p) {
    const HWND h = (HWND)p;
    BOOL ok = FALSE;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    // l'app ha 3 s per rispondere (si puo' star ancora avviando); una finestra che non legge mai i messaggi (vedi
    // risponde) non si tocca: SetWindowLongPtr la aspetterebbe per sempre, tenendo occupato il thread
    if (!(ex & WS_EX_LAYERED) && risponde(h, 3000)) {
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        ok = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) && SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
        if (!ok && IsWindow(h)) SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    PostMessageW(gMessaggi, WM_NATA_NASCOSTA, (WPARAM)h, ok);
    InterlockedDecrement(&gNascondendo);
    return 0;
}

static void suCreata(HWND h) {
    if (gPausa || !gGenioOk || !gMovimento || !impostazioni::valori().apertura || !finestraApp(h)) return;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return;   // scheda staccata trascinando (browser): niente animazione
    // (layered di suo: alla creazione non si tocca, ci si pensa alla comparsa)
    if (gInAttesa.count(h) || GetPropW(h, PROP_NASCOSTA) || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED)) return;
    spegniTransizioniNative(h);
    SetPropW(h, PROP_NASCOSTA, (HANDLE)1);   // prima del thread: se FluidWin si chiudesse male, il guardiano la rimette a posto
    InterlockedIncrement(&gNascondendo);
    // nel pool di thread (vedi pool.h): "lungo" perche' aspetta l'app, che puo' essere ancora occupata ad avviarsi
    if (!pool::lancia(nascondiNata, h, THREAD_PRIORITY_NORMAL, true)) {
        InterlockedDecrement(&gNascondendo); RemovePropW(h, PROP_NASCOSTA); riaccendiTransizioniNative(h); return;
    }
    Attesa& a = gInAttesa[h] = {ora()};
    // Foto dello sfondo subito, senza aspettare il thread (un'attesa di pochi ms ne dura 15: la risoluzione dei timer
    // di Windows, e intanto la finestra compariva). Se il thread finisce prima, la copertura non si disegna mai.
    // (anche se risulta gia' visibile: DWM la mostra solo a un fotogramma successivo, la foto ha ancora lo sfondo)
    if (GetWindowRect(h, &a.rc)) {
        const double t1 = ora();
        const bool visibile = IsWindowVisible(h);
        if (!fotoSchermo(a.rc, a.copertura)) a.copertura.libera();
        logf("finestra nata %p: copertura %s in %.1f ms%s", h, a.copertura.srv ? "pronta" : "non riuscita",
             (ora() - t1) * 1000, visibile ? " (gia' visibile)" : "");
    }
}

// risposta del thread
static void nataNascosta(HWND h, bool ok) {
    auto it = gInAttesa.find(h);
    if (it != gInAttesa.end()) it->second.copertura.libera();   // (serve solo mentre il thread lavora)
    if (!ok) {   // non e' diventata trasparente (o non c'e' piu'): resta com'e'
        if (IsWindow(h)) RemovePropW(h, PROP_NASCOSTA);
        if (it != gInAttesa.end()) gInAttesa.erase(it);
        return;
    }
    gNascoste.insert(h);
    segnaCambioStile(h);
    if (it == gInAttesa.end() || it->second.annulla) {   // nel frattempo si e' rinunciato all'animazione
        if (it != gInAttesa.end()) gInAttesa.erase(it);
        if (IsWindow(h) && !animDi(h) && !aperturaDi(h) && !gelatinaDi(h)) rendiVisibileFinestra(h);
        return;
    }
    if (aperturaDi(h)) gInAttesa.erase(it);   // gia' comparsa: ora e' dell'apertura
    else it->second.pronta = true;
}

static bool inCorsoDiNascondere(HWND h) { auto it = gInAttesa.find(h); return it != gInAttesa.end() && !it->second.pronta; }

// rimette visibile una finestra; se il thread la sta ancora rendendo trasparente, lo fara' la sua risposta
static void lasciaVedere(HWND h) {
    auto it = gInAttesa.find(h);
    if (it != gInAttesa.end() && !it->second.pronta) { it->second.annulla = true; return; }
    if (it != gInAttesa.end()) gInAttesa.erase(it);
    if (IsWindow(h)) rendiVisibileFinestra(h);
}

static void svuotaAperture() {
    for (auto& a : gAperture) { stampa::abbandona(a.lavoro); lasciaVedere(a.hwnd); }
    gAperture.clear();
}

static void suNuovaFinestra(HWND h) {
    const bool inAttesa = gInAttesa.count(h) > 0, inCorso = inCorsoDiNascondere(h);
    if (inAttesa && !inCorso) gInAttesa.erase(h);
    // senza animazione una finestra resa trasparente alla creazione torna subito com'era
    auto rinuncia = [&]() {
        if (inCorso) { gInAttesa[h].annulla = true; return; }
        if (inAttesa && IsWindow(h) && !animDi(h) && !gelatinaDi(h) && !aperturaDi(h)) rendiVisibileFinestra(h);
    };
    const auto& imp = impostazioni::valori();
    if (gPausa || !gGenioOk || !gMovimento || GetAncestor(h, GA_ROOT) != h || !finestraApp(h) || IsIconic(h)) return rinuncia();
    if (animDi(h) || gelatinaDi(h) || aperturaDi(h) || (!inAttesa && GetPropW(h, PROP_NASCOSTA))) return;
    BOOL velata = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &velata, sizeof(velata));
    if (velata) {
        if (gNateVelate.size() > 64) gNateVelate.clear();   // (finestre mai svelate: non si accumulano)
        gNateVelate[h] = ora();
        if (inAttesa && !inCorso) gInAttesa[h] = {ora(), true};   // resta trasparente fino allo svelamento
        return;
    }
    RECT ic{};
    const bool dalDock = imp.genio && gDockOk && gDock.aperturaDaDock(h, ic);
    if (!dalDock && !imp.apertura) return rinuncia();
    if (!dalDock && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return rinuncia();   // scheda staccata trascinando
    if (!MonitorFromWindow(h, MONITOR_DEFAULTTONULL)) return rinuncia();   // fuori da tutti gli schermi: non si vedrebbe
    if (schermoInteroAttivo() || (!inAttesa && !nascondibile(h))) return rinuncia();
    // (in corso: trasparente la rende il thread; l'apertura aspetta la sua risposta)
    if (!inCorso) {
        spegniTransizioniNative(h);   // niente comparsa nativa: la anima FluidWin
        if (!rendiLayered(h, 0)) { riaccendiTransizioniNative(h); return rinuncia(); }
    }
    gAperture.push_back({h, ic, ora(), dalDock});
    logf("apertura %s %p%s", dalDock ? "dal dock" : "fuori dal dock", h, inCorso ? " (si sta rendendo trasparente)" : inAttesa ? " (trasparente dalla creazione)" : "");
}

static void suSvelata(HWND h) {
    auto it = gNateVelate.find(h);
    if (it == gNateVelate.end()) return;
    const double quando = it->second;
    gNateVelate.erase(it);
    if (ora() - quando < 10) suNuovaFinestra(h);
}

// ogni fotogramma: appena la finestra ha un contenuto (foto in un thread) parte l'animazione
static void passoAperture(double t) {
    for (auto it = gAperture.begin(); it != gAperture.end();) {
        Apertura& ap = *it;
        const HWND h = ap.hwnd;
        const double e = ora() - ap.t0;
        InfoFinestra* i = IsWindow(h) && !IsIconic(h) ? traccia(h) : nullptr;
        if (!i || !leggiRettangoli(h, *i)) {
            stampa::abbandona(ap.lavoro);
            lasciaVedere(h);
            it = gAperture.erase(it); continue;
        }
        if (e < 0.06) { ++it; continue; }   // lascia il tempo all'app di disegnare il primo fotogramma
        if (!inCorsoDiNascondere(h) && !GetPropW(h, PROP_NASCOSTA)) {   // il thread non e' riuscito a renderla trasparente
            logf("apertura %p: non resa trasparente, niente animazione", h);
            stampa::abbandona(ap.lavoro);
            it = gAperture.erase(it); continue;
        }
        if (stampa::pronta(ap.lavoro)) {
            std::vector<UINT> px;
            RECT wr{}; GetWindowRect(h, &wr);
            // (la finestra si e' spostata o ridimensionata durante la foto: se ne fa un'altra)
            const bool presa = stampa::prendi(ap.lavoro, px) && EqualRect(&wr, &ap.wr);
            ap.lavoro = nullptr;
            liberaFoto(h);
            if (presa && gGenio.carica(h, i->visibile, wr, px.data(), gFoto[h])) {
                Anim n;
                n.hwnd = h; n.tipo = Tipo::Ripristina; n.genio = true;
                n.da = i->visibile; n.t0 = n.tFase = t;
                if (ap.dalDock) { n.a = ap.icona; n.pGenio = 1.0; }
                else { n.a = i->visibile; n.pGenio = 0; n.cresce = true; }
                gAnim.push_back(n);
                logf("apertura %s %p: animazione dopo %.0f ms", ap.dalDock ? "dal dock" : "fuori dal dock", h, e * 1000);
                it = gAperture.erase(it);
                continue;
            }
            liberaFoto(h);   // ancora nera: l'app non ha disegnato
        }
        // l'app non disegna ancora (o non risponde): si mostra com'e', senza animazione. Dal dock si aspetta di piu'
        // (il genio dall'icona vale l'attesa); aperta in altri modi al massimo 0,35 s di finestra invisibile
        if (e > (ap.dalDock ? 0.8 : 0.35)) {
            logf("apertura %p: foto non riuscita, niente animazione", h);
            stampa::abbandona(ap.lavoro);
            lasciaVedere(h);
            it = gAperture.erase(it);
            continue;
        }
        // una foto ogni 40 ms al massimo (ognuna e' un PrintWindow di tutta la finestra, che l'app deve disegnare)
        if (!ap.lavoro && !inCorsoDiNascondere(h) && ora() - ap.prova >= 0.04 && GetWindowRect(h, &ap.wr)) {
            ap.prova = ora();
            ap.lavoro = stampa::avvia(h, ap.wr.right - ap.wr.left, ap.wr.bottom - ap.wr.top);
        }
        ++it;
    }
}

// ---------------------------------------------------------------------------------------------
// Un passo di tutte le animazioni per l'istante t (quando il fotogramma sara' visibile)

// Curva del tempo del genio: parte e arriva morbida, come su macOS
static double curvaGenio(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}

static double inversaCurvaGenio(double p) {
    p = std::clamp(p, 0.0, 1.0);
    return p < 0.5 ? std::cbrt(p / 4) : 1 - std::cbrt(2 * (1 - p)) / 2;
}

// Chiusura: la finestra si rimpicciolisce un poco sul posto e svanisce. La foto si fa quando si chiede di chiuderla
// (pulsante chiudi dell'app o della capsula, Alt+F4); l'animazione parte solo quando la finestra sparisce davvero:
// se l'app chiede "salvare?" e resta aperta, dopo qualche secondo la foto si butta e non succede niente.
struct Chiusura { HWND h = nullptr; genio::Immagine foto; RECT da{}; double tIntento = 0, t0 = -1; };
static std::vector<Chiusura> gChiusure;
static const double DURATA_CHIUSURA = 0.30;

static bool chiusureAttive() { for (const auto& c : gChiusure) if (c.t0 >= 0) return true; return false; }

static bool preparaChiusura(HWND h) {
    if (gPausa || !gGenioOk || !gMovimento || !impostazioni::valori().chiusura || !h || IsIconic(h) || !finestraApp(h) || schermoInteroAttivo()) return false;
    for (auto& c : gChiusure) if (c.h == h) { c.tIntento = ora(); return true; }
    InfoFinestra* i = traccia(h);
    if (!i || !leggiRettangoli(h, *i)) return false;
    Chiusura c; c.h = h; c.da = i->visibile; c.tIntento = ora();
    gGenio.fotoDaSchermo = false;
    if (!gGenio.cattura(h, c.da, c.foto, true)) { logf("chiusura: foto non riuscita %p", h); return false; }
    spegniTransizioniNative(h);   // la dissolvenza di Windows non si somma alla nostra
    gChiusure.push_back(c);
    logf("chiusura preparata %p schermo=%d", h, (int)gGenio.fotoDaSchermo);
    return true;
}

// la finestra e' sparita (nascosta o distrutta): parte l'animazione
static void avviaChiusura(HWND h) {
    for (auto& c : gChiusure) if (c.h == h && c.t0 < 0) { c.t0 = ora(); logf("chiusura animata %p", h); PostMessageW(gMessaggi, WM_NULL, 0, 0); }
}

// foto di chiusure mai avvenute (l'app e' rimasta aperta): via dopo 4 s
static void pulisciChiusure() {
    const double t = ora();
    for (auto it = gChiusure.begin(); it != gChiusure.end();) {
        if (it->t0 < 0 && t - it->tIntento > 4.0) { if (IsWindow(it->h)) riaccendiTransizioniNative(it->h); it->foto.libera(); it = gChiusure.erase(it); }
        else ++it;
    }
}

static void passoChiusure(double t, std::vector<genio::Elemento>& elementi) {
    for (auto it = gChiusure.begin(); it != gChiusure.end();) {
        if (it->t0 < 0) { ++it; continue; }
        // prima si gonfia appena (il rimbalzo), poi si rimpicciolisce e svanisce, sempre centrata dov'era
        const double k = std::clamp((t - it->t0) / DURATA_CHIUSURA, 0.0, 1.0);
        double s, e;
        if (k < 0.25) { s = 1 + 0.03 * std::sin(k / 0.25 * 1.5707963); e = 0; }
        else { const double u = (k - 0.25) / 0.75; e = 1 - std::pow(1 - u, 3); s = 1.03 - 0.17 * e; }
        const double cx = (it->da.left + it->da.right) / 2.0, cy = (it->da.top + it->da.bottom) / 2.0;
        const double w = (it->da.right - it->da.left) * s / 2, hh = (it->da.bottom - it->da.top) * s / 2;
        const RECT q = {(LONG)std::lround(cx - w), (LONG)std::lround(cy - hh), (LONG)std::lround(cx + w), (LONG)std::lround(cy + hh)};
        if (k < 1) elementi.push_back({&it->foto, q, q, 0.0, 1.0 - e});
        if (k >= 1) { it->foto.libera(); it = gChiusure.erase(it); } else ++it;
    }
}

static void elementiZoom(Anim& a, double pv, double pl, double alfa, std::vector<genio::Elemento>& el);

// Pochi pixel campione (una griglia 48 x 27 della parte visibile) per capire se due foto sono uguali
static std::vector<UINT> firmaDi(const std::vector<UINT>& px, const RECT& wr, const RECT& v) {
    std::vector<UINT> f;
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    const int x0 = std::max(0, (int)(v.left - wr.left)), y0 = std::max(0, (int)(v.top - wr.top));
    const int vw = std::min((int)(v.right - v.left), ww - x0), vh = std::min((int)(v.bottom - v.top), wh - y0);
    if (vw <= 0 || vh <= 0 || px.size() != (size_t)ww * wh) return f;
    for (int j = 0; j < 27; j++) for (int i = 0; i < 48; i++)
        f.push_back(px[(size_t)(y0 + (vh - 1) * j / 26) * ww + x0 + (vw - 1) * i / 47] & 0xFFFFFF);
    return f;
}
static bool firmeUguali(const std::vector<UINT>& a, const std::vector<UINT>& b) {
    if (a.empty() || a.size() != b.size()) return false;
    size_t diversi = 0;
    for (size_t i = 0; i < a.size(); i++)
        for (int k = 0; k < 24; k += 8) if (std::abs((int)((a[i] >> k) & 255) - (int)((b[i] >> k) & 255)) > 24) { diversi++; break; }
    return diversi * 50 <= a.size();   // al massimo il 2% (un cursore che lampeggia, un'animazione piccola)
}

// Ingrandimento/ripristino: la foto alla misura nuova si rifa' in sottofondo a 60, 130, 200, 300 e 420 ms dall'inizio
// (vedi Anim::rilettura), finche' due riletture di fila non sono uguali: allora la pagina si e' assestata, le
// riletture finiscono e la foto di prima sfuma. Una foto nuova diversa prende il posto di quella di prima, che sfuma
// sopra di lei in 120 ms
static void rileggiFotoZoom(Anim& a, double t) {
    static const double QUANDO[] = {0.06, 0.13, 0.20, 0.30, 0.42};
    static const int RILETTURE = 5;
    if (a.fotoZVecchia.srv) {
        const double k = (t - a.tCambioZ) / 0.12;
        a.alfaVecchia = 1.0 - morbido(k);
        if (k >= 1.0) { a.fotoZVecchia.libera(); a.alfaVecchia = 0; }
    }
    a.alfaPrima = a.assestata ? 1.0 - morbido((t - a.tAssestata) / 0.15) : 1.0;
    if (a.rilettura) {
        if (!stampa::pronta(a.rilettura)) return;
        std::vector<UINT> px;
        const bool ok = stampa::prendi(a.rilettura, px);
        a.rilettura = nullptr;
        const std::vector<UINT> firma = ok ? firmaDi(px, a.wrRilettura, a.a) : std::vector<UINT>{};
        const bool uguale = ok && firmeUguali(firma, a.firmaZ);
        if (ok) a.firmaZ = firma;
        bool caricata = false;
        const double t1 = ora();
        if (ok && !uguale) {   // pagina cambiata: la foto nuova al posto di quella di prima
            genio::Immagine nuova;
            caricata = gGenio.carica(a.hwnd, a.a, a.wrRilettura, px.data(), nuova);
            if (caricata) {
                a.fotoZVecchia.libera();   // (se ne stava ancora sfumando una: non serve piu')
                a.fotoZVecchia = a.fotoZ; a.fotoZ = nuova;
                a.tCambioZ = t; a.alfaVecchia = 1;
            }
        }
        if ((uguale || a.riletture >= RILETTURE) && !a.assestata) { a.assestata = true; a.tAssestata = t; }
        if (gLog) logf("ingrandimento: rilettura %d a %.0f ms: %s (%.1f ms)", a.riletture, (t - a.t0) * 1000,
                       !ok ? "non riuscita" : uguale ? "uguale, pagina assestata" : caricata ? "cambiata, foto nuova" : "cambiata, non caricata",
                       (ora() - t1) * 1000);
        return;
    }
    if (a.assestata || a.riletture >= RILETTURE) { if (!a.assestata) { a.assestata = true; a.tAssestata = t; } return; }
    if (t - a.t0 < QUANDO[a.riletture]) return;
    RECT wr; if (!GetWindowRect(a.hwnd, &wr)) return;
    a.riletture++;
    a.wrRilettura = wr;
    a.rilettura = stampa::avvia(a.hwnd, wr.right - wr.left, wr.bottom - wr.top);
}

// a che punto del volo e' il genio del ripristino (0 partenza, 1 arrivo)
static double x0Genio(const Anim& a, double t) { return std::clamp((t - a.t0) / DURATA_GENIO_RIPRISTINA, 0.0, 1.0); }

static void passo(double t) {
    std::vector<genio::Elemento> elementi;
    elementiCoperture(elementi);   // sotto a tutto il resto: sono solo lo sfondo dietro finestre appena nate
    bool serveGenio = false;
    for (auto it = gAnim.begin(); it != gAnim.end();) {
        Anim& a = *it;
        bool finita = !IsWindow(a.hwnd);
        const size_t primoElemento = elementi.size();   // (alla fine: gli angoli ai DPI di questa finestra)

        // la finestra vera puo' assestarsi: inseguiamo la sua posizione (aggiornata dagli eventi)
        if (auto fi = gInfo.find(a.hwnd); !finita && a.tipo != Tipo::Riduci && fi != gInfo.end()) {
            const InfoFinestra& i = fi->second;
            if (i.valida && a.genio) {
                a.da = i.visibile;
            } else if (i.valida && !EqualRect(&i.visibile, &a.a)) {
                a.a = i.visibile; a.sorgente = sorgenteDa(i);
                if (a.tipo == Tipo::Pronta) a.da = a.attuale = a.a;
            }
        }

        if (finita) {
        } else if (a.genio) {
            auto foto = gFoto.find(a.hwnd);
            if (foto == gFoto.end()) {
                finita = true;
            } else if (a.tipo == Tipo::Pronta) {
                // ripristino interrotto da un clic per ridurre: la foto resta ferma dov'era finche' arriva la riduzione
                elementi.push_back({&foto->second, a.da, a.a, a.pGenio, 1.0 - morbido((a.pGenio - 0.92) / 0.08)});
                if (a.fase == 1 && t - a.tFase > ANNULLA_DOPO) annullaPronta(a, t);
                else if (t - a.t0 > 5.0) annullaPronta(a, t);
            } else if (a.tipo == Tipo::Riduci) {
                const double x = (t - a.t0) / DURATA_GENIO_RIDUCI, p = curvaGenio(x);
                a.pGenio = p;
                elementi.push_back({&foto->second, a.da, a.a, p, 1.0 - morbido((p - 0.85) / 0.15)});
                if (++a.fotogrammiGenio > 3) chiudiMiniatura(a);
                if (x >= 1.0) finita = true;
            } else if (a.cresce && a.fase == 0) {
                // aperta fuori dal dock: la foto cresce dal centro della finestra fino alla sua misura, apparendo
                // molla: parte veloce, supera di poco la misura (~3%) e ci torna, senza altre oscillazioni visibili
                const double x = std::clamp((t - a.t0) / DURATA_CRESCITA, 0.0, 1.0);
                const double s = SCALA_CRESCITA + (1.0 - SCALA_CRESCITA) * (1.0 - std::exp(-7.0 * x) * std::cos(5.4 * x));
                const double cx = (a.da.left + a.da.right) / 2.0, cy = (a.da.top + a.da.bottom) / 2.0;
                const double mw = (a.da.right - a.da.left) * s / 2.0, mh = (a.da.bottom - a.da.top) * s / 2.0;
                const RECT r = x >= 1.0 ? a.da : RECT{(LONG)std::lround(cx - mw), (LONG)std::lround(cy - mh), (LONG)std::lround(cx + mw), (LONG)std::lround(cy + mh)};
                genio::Elemento el{&foto->second, r, r, 0.0, morbido(x / 0.4)};
                el.raggio = raggioDi(a.hwnd) * s;
                elementi.push_back(el);
                if (gLog && ++a.fotogrammiGenio <= 4) logf("crescita: fotogramma per t=%.3f scala=%.2f", t, s);
                if (x >= 1.0) { a.fase = 1; a.tFase = t; }
            } else if (a.fase == 0) {
                // La foto del genio e' quella fatta alla riduzione (anche minuti prima): appena la finestra e' ripristinata
                // (ancora trasparente) se ne fa una nuova in sottofondo e la si mette al suo posto a meta' volo. Cosi'
                // all'arrivo il contenuto e' quello di adesso e la finestra vera prende il posto della foto senza salti
                // (una foto nuova ogni ~80 ms finche' vola: il contenuto si muove anche durante il volo)
                if (!a.rilettura && a.riletture < 6 && t - a.t0 > 0.04 + 0.08 * a.riletture && x0Genio(a, t) < 0.9 && !IsIconic(a.hwnd)) {
                    RECT wr; if (GetWindowRect(a.hwnd, &wr)) { a.riletture++; a.wrRilettura = wr; a.rilettura = stampa::avvia(a.hwnd, wr.right - wr.left, wr.bottom - wr.top); }
                } else if (a.rilettura && stampa::pronta(a.rilettura)) {
                    std::vector<UINT> px; RECT wr{}; GetWindowRect(a.hwnd, &wr);
                    const bool presa = stampa::prendi(a.rilettura, px) && EqualRect(&wr, &a.wrRilettura);
                    a.rilettura = nullptr;
                    InfoFinestra* fi = presa ? traccia(a.hwnd) : nullptr;
                    genio::Immagine nuova;
                    if (fi && fi->valida && gGenio.carica(a.hwnd, fi->visibile, wr, px.data(), nuova)) {
                        foto->second.libera(); foto->second = nuova;
                        if (gLog && a.riletture == 1) logf("genio ripristino: foto rifatta a %.0f ms", (t - a.t0) * 1000);
                    }
                }
                // al ripristino parte subito veloce e frena all'arrivo: cosi' risponde al clic senza attese
                const double x = std::clamp((t - a.t0) / DURATA_GENIO_RIPRISTINA, 0.0, 1.0);
                const double p = std::pow(1.0 - x, 3);
                a.pGenio = p;
                elementi.push_back({&foto->second, a.da, a.a, p, 1.0 - morbido((p - 0.92) / 0.08)});
                if (gLog && ++a.fotogrammiGenio <= 6) logf("genio ripristino: fotogramma per t=%.3f p=%.3f", t, p);
                if (x >= 1.0) { a.fase = 1; a.tFase = t; }
            } else if (a.fase == 1 || a.fase == 2) {
                // la finestra vera, con la sua ombra, ricompare sotto la foto in 150 ms (ombra morbida, non uno scatto);
                // a meta' la foto comincia a sfumare (in 120 ms): niente attesa in piu' con la foto ferma sullo schermo
                a.pGenio = 0;
                const double e = t - a.tFase;
                const double k = morbido(e / RAMPA_FINESTRA);
                const double kf = morbido((e - RAMPA_FINESTRA / 2) / SFUMA_FOTO);
                if (a.fase == 1) alfaFinestra(a.hwnd, (BYTE)std::lround(k * 255));
                if (k >= 1.0) a.fase = 2;   // (finestra vera piena)
                elementi.push_back({&foto->second, a.da, a.a, 0.0, 1.0 - kf});
                if (kf >= 1.0) { a.fase = 3; a.tFase = t; }
            } else if (t - a.tFase >= ATTESA_STILE) {
                finita = true;
            }
        } else if (a.tipo == Tipo::Pronta) {
            if (gFoto.count(a.hwnd)) serveGenio = true;   // l'overlay del genio resta pronto, vuoto
            // pulsante premuto: copia identica sopra la finestra, l'ombra vera sfuma sotto di lei
            impostaMiniatura(a, a.a, 255);
            alfaFinestra(a.hwnd, (BYTE)std::lround((1.0 - morbido((t - a.t0) / SFUMA_OMBRA)) * 255));
            if (a.fase == 1 && t - a.tFase > ANNULLA_DOPO) annullaPronta(a, t);
            else if (t - a.t0 > 5.0) annullaPronta(a, t);   // sicurezza: pressione troppo lunga
        } else if (a.zoom && a.fase == 0 && a.passiZ == 0) {
            // la finestrella ferma alla vecchia misura (angoli tondi di DWM, stabili) o la foto di prima coprono
            // l'attesa della foto nuova
            if (!a.vetrina) elementiZoom(a, 0, 0, 1.0, elementi);
            if (++a.fotogrammiGenio >= 2) {
                const double t1 = ora();
                const bool ok = gGenio.cattura(a.hwnd, a.a, a.fotoZ);
                logf("ingrandimento: foto %s in %.1f ms", ok ? "fatta" : "non riuscita", (ora() - t1) * 1000);
                if (!ok) finita = true;   // senza foto: la finestra compare subito al nuovo posto
                else if (!a.vetrina) { a.passiZ = 3; a.t0 = t; }   // nessuna finestrella da togliere: si parte
                else a.passiZ = 1;
            }
        } else if (a.zoom && a.fase == 0 && a.passiZ < 3) {
            // la foto ferma alla vecchia misura, sotto la finestrella, per un paio di fotogrammi: poi la finestrella
            // sparisce e si parte senza stacchi
            elementiZoom(a, 0, 0, 1.0, elementi);
            if (++a.passiZ == 3) { chiudiMiniatura(a); a.t0 = t; }
        } else if (a.zoom && a.fase == 0) {
            rileggiFotoZoom(a, t);
            // l'angolo in alto a destra con la molla veloce, il resto della foto sempre piu' gelatinoso
            const double dt = std::max(0.0, t - a.t0);
            const double pv = MOLLA_ANGOLO.valore(dt), pl = MOLLA_RESTO.valore(dt);
            auto mix = [](LONG x, LONG y, double p) { return (LONG)std::lround(x + (y - x) * p); };
            a.attuale = {mix(a.da.left, a.a.left, pl), mix(a.da.top, a.a.top, pv), mix(a.da.right, a.a.right, pv), mix(a.da.bottom, a.a.bottom, pl)};
            // "Arrivata" quando manca poco (6 punti), non quando la molla e' ferma al pixel: l'ultimo tratto del rimbalzo
            // durava ~0,3 s in cui sullo schermo c'era ancora una foto ferma (contenuto bloccato). La foto finisce il suo
            // movimento mentre sfuma sopra la finestra vera (fase 2)
            const bool ferma = std::max(MOLLA_ANGOLO.residuo(dt), MOLLA_RESTO.residuo(dt)) * a.ampiezza < 6.0 * scalaFinestra(a.hwnd) || dt > 1.5;
            elementiZoom(a, pv, pl, 1.0, elementi);
            if (ferma) {
                // arrivata: la finestra vera torna subito normale (non layered: angoli suoi) sotto la foto, che sfuma
                rendiVisibileFinestra(a.hwnd);
                a.fase = 2; a.tFase = t;
                // (da qui sotto c'e' la finestra vera: niente piu' riletture)
                if (a.rilettura) { stampa::abbandona(a.rilettura); a.rilettura = nullptr; }
                a.fotoZVecchia.libera(); a.alfaVecchia = 0; a.alfaPrima = 0;
                logf("ingrandimento fermo dopo %.0f ms", dt * 1000);
            }
        } else if (a.zoom && a.fase == 2) {
            const double k = morbido((t - a.tFase) / (DISSOLVENZA_MINI * 2));
            const double dt = std::max(0.0, t - a.t0);   // la foto continua il suo movimento mentre sfuma
            elementiZoom(a, MOLLA_ANGOLO.valore(dt), MOLLA_RESTO.valore(dt), 1.0 - k, elementi);
            if (k >= 1.0) {
                a.fase = 3; a.tFase = t;
            }
        } else if (a.fase == 0) {
            const double dt = std::max(0.0, t - a.t0);
            const double p = a.molla.valore(dt);
            a.attuale = interpola(a.da, a.a, p);
            if (a.tipo == Tipo::Riduci) {
                const double op = p < 0.5 ? 1.0 : std::max(0.0, 1.0 - (p - 0.5) / 0.42);
                if (!impostaMiniatura(a, a.attuale, (BYTE)std::lround(op * 255)) || op <= 0.0 || dt > 1.5) finita = true;
            } else {
                const double op = 0.35 + 0.65 * std::min(1.0, p / 0.25);
                // ferma solo quando la miniatura e' gia' esattamente sulla finestra: cosi' alla fine non c'e' nessuno scatto
                const bool ferma = (EqualRect(&a.attuale, &a.a) && a.molla.residuo(dt) * a.ampiezza < 1.5) || dt > 1.5;
                if (ferma) { impostaMiniatura(a, a.a, 255); a.fase = 1; a.tFase = t; logf("molla ferma dopo %.0f ms", dt * 1000); }
                else if (!impostaMiniatura(a, a.attuale, (BYTE)std::lround(op * 255))) finita = true;
            }
        } else if (a.fase == 1) {
            // la finestra vera, con la sua ombra, ricompare sotto la miniatura identica
            const double k = morbido((t - a.tFase) / RAMPA_FINESTRA);
            impostaMiniatura(a, a.a, 255);
            alfaFinestra(a.hwnd, (BYTE)std::lround(k * 255));
            if (k >= 1.0) { a.fase = 2; a.tFase = t; }
        } else if (a.fase == 2) {
            // la miniatura sfuma: copre le piccole differenze di bordo tra copia e originale
            const double k = morbido((t - a.tFase) / DISSOLVENZA_MINI);
            impostaMiniatura(a, a.a, (BYTE)std::lround((1.0 - k) * 255));
            if (k >= 1.0) {
                chiudiMiniatura(a); a.fase = 3; a.tFase = t;
            }
        } else if (t - a.tFase >= ATTESA_STILE) {
            finita = true;
        }
        for (size_t k = primoElemento; k < elementi.size(); k++) if (elementi[k].raggio < 0) elementi[k].raggio = raggioDi(a.hwnd);

        if (finita) {
            if (gLog) logf("animazione finita %p (tipo %d%s%s)", a.hwnd, (int)a.tipo, a.genio ? ", genio" : "", a.zoom ? ", ingrandimento" : "");
            if (a.tipo != Tipo::Riduci) rendiVisibileFinestra(a.hwnd);
            if (a.genio && a.tipo == Tipo::Ripristina) liberaFoto(a.hwnd);
            chiudiMiniatura(a);
            liberaFotoZoom(a);
            it = gAnim.erase(it);
        } else ++it;
    }
    passoGelatine(t, elementi);
    passoAperture(t);
    passoChiusure(t, elementi);
    bool miniature = false;
    for (auto& a : gAnim) if (a.thumb && !a.vetrina) miniature = true;   // quelle con la finestrella sua non usano l'overlay
    mostraOverlay(miniature);
    if (gGenioOk) {
        if (!elementi.empty() || serveGenio) {
            const double t1 = ora();
            if (!gGenio.visibile) gGenio.sotto = gDockOk ? gDock.finestraPiuBassa() : nullptr;
            gGenio.mostra(true); gGenio.disegna(elementi);
            if (gLog && ora() - t1 > 0.012) logf("disegno lento: %.1f ms", (ora() - t1) * 1000);
        }
        // finite le animazioni l'overlay va sempre svuotato e nascosto, anche se in questo passo ha appena disegnato
        if (gGenio.visibile && ((gAnim.empty() && gGelatine.empty() && !chiusureAttive()) || (elementi.empty() && !serveGenio))) { gGenio.svuota(); gGenio.mostra(false); }
    }
}

// ---------------------------------------------------------------------------------------------
// Eventi

// La parte visibile di h si vede intera sullo schermo: dentro un solo monitor, opaca e senza altre finestre sopra
// (la capsula e gli angoli di FluidWin si', sono gia' quello che si vede). Allora la sua foto si prende dallo schermo
static bool scopertaSulloSchermo(HWND h, const RECT& v) {
    auto no = [h](const char* perche) { logf("foto %p non dallo schermo: %s", h, perche); return false; };
    if (!IsWindowVisible(h) || IsIconic(h) || GetPropW(h, PROP_NASCOSTA)) return no("nascosta o trasparente");
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) {
        BYTE alfa = 255; DWORD fl = 0;
        if (!GetLayeredWindowAttributes(h, nullptr, &alfa, &fl) || ((fl & LWA_ALPHA) && alfa < 255)) return no("layered");
    }
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromRect(&v, MONITOR_DEFAULTTONULL), &mi)) return no("fuori dagli schermi");
    RECT x;
    if (!IntersectRect(&x, &v, &mi.rcMonitor) || !EqualRect(&x, &v)) return no("a cavallo di due schermi");
    for (HWND w = GetWindow(h, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (!IsWindowVisible(w)) continue;
        BOOL nascosta = FALSE; DwmGetWindowAttribute(w, DWMWA_CLOAKED, &nascosta, sizeof(nascosta));
        RECT r; if (nascosta || !GetWindowRect(w, &r)) continue;
        // finestre con una regione (il dock: larga quanto lo schermo, ma si vede solo dove sono dock e isola, e da
        // nascosto la regione e' vuota): conta solo la regione
        RECT rg; const int tipo = GetWindowRgnBox(w, &rg);
        if (tipo == NULLREGION) continue;
        if (tipo == SIMPLEREGION || tipo == COMPLEXREGION) { OffsetRect(&rg, r.left, r.top); r = rg; }
        if (!IntersectRect(&x, &r, &v)) continue;
        wchar_t k[64] = L""; GetClassNameW(w, k, 64);
        if (!wcscmp(k, L"FluidWin.Capsula") || !wcscmp(k, L"FluidWin.Angolo")) continue;
        if (gLog) { char b[120]; snprintf(b, 120, "coperta da %ls", k); return no(b); }
        return false;
    }
    return true;
}

// Copia identica sopra la finestra e finestra resa trasparente sotto di lei: la riduzione che segue sara' col genio
static bool preparaCopia(HWND h, InfoFinestra& i) {
    if (schermoInteroAttivo()) return false;
    // finestra appena trascinata (la gelatina sta ancora finendo): la sua foto, gia' con la capsula, serve al genio.
    // Rifarla adesso non si puo': la finestra e' ancora trasparente, o lo e' appena stata (la foto verrebbe nera)
    bool daGelatina = false;
    for (auto it = gGelatine.begin(); it != gGelatine.end(); ++it) {
        if (it->hwnd != h) continue;
        if (it->fase < 2) return false;   // ancora trascinata
        liberaFoto(h);
        // con il contenuto vivo, l'ultima immagine (com'e' la finestra adesso) invece della foto di inizio trascinamento
        genio::Immagine& usata = it->vivaPronta ? it->viva : it->foto;
        genio::Immagine& altra = it->vivaPronta ? it->foto : it->viva;
        altra.libera();
        if (it->nascosta) { gFoto[h] = usata; alfaFinestra(h, 255); daGelatina = true; }
        else usata.libera();
        gGelatine.erase(it);
        logf("genio: foto presa dalla gelatina %p", h);
        break;
    }
    RECT ic;
    if (!daGelatina) {
        if (!nascondibile(h)) return false;   // non potremmo renderla trasparente
        if (GetPropW(h, PROP_NASCOSTA)) return false;   // gia' in mano a un'altra animazione
        // foto per l'effetto genio (pochi millisecondi), se la barra e' in basso. Va fatta PRIMA di rendere
        // la finestra layered: subito dopo la sua superficie viene ricreata e la foto verrebbe nera.
        gGenio.fotoDaSchermo = false;
        if (gGenioOk && iconaBarra(h, ic) && !gGenio.cattura(h, i.visibile, gFoto[h], true)) liberaFoto(h);
    }
    if (!nuovaAnim(h, Tipo::Pronta, i.visibile, i.visibile, i, 255)) { liberaFoto(h); return false; }
    if (Anim* a = animDi(h)) GetWindowRect(h, &a->fin);
    spegniTransizioniNative(h);     // la riduzione sara' istantanea, sotto la copia gia' pronta
    if (!rendiLayered(h, 255)) { annullaAnimDi(h); riaccendiTransizioniNative(h); liberaFoto(h); return false; }
    logf("pronta %p genio=%d schermo=%d", h, (int)gFoto.count(h), (int)gGenio.fotoDaSchermo);
    return true;
}

// Clic per ridurre mentre la finestra sta ancora ricomparendo col genio: invece di lasciare la riduzione
// a Windows (senza animazione) la foto si ferma e poi torna indietro da dove e' arrivata
static bool invertiRipristino(HWND h) {
    Anim* a = animDi(h);
    // (una finestra che sta crescendo non ha un'icona da cui e' uscita: si riduce come vuole Windows)
    if (!a || a->tipo != Tipo::Ripristina || !a->genio || a->cresce || !gFoto.count(h)) return false;
    a->tipo = Tipo::Pronta; a->fase = 0; a->t0 = a->tFase = ora();
    alfaFinestra(h, 0);
    logf("ripristino invertito %p a p=%.2f", h, a->pGenio);
    return true;
}

// Inizio cattura del mouse: se e' la pressione del pulsante "riduci a icona", prepariamo la copia
static void suCattura(HWND figlia) {
    if (gPausa) return;
    HWND h = GetAncestor(figlia, GA_ROOT);
    if (!h || IsIconic(h)) return;
    // Quasi ogni clic in qualsiasi app inizia una cattura del mouse: all'app si chiede qualcosa (messaggi che possono
    // aspettarla fino a 40 ms ciascuno) solo se il clic e' nella fascia della barra del titolo, dove stanno i tasti
    InfoFinestra* i = traccia(h);
    if (!i || !leggiRettangoli(h, *i)) return;
    POINT pt; GetCursorPos(&pt);
    if (!PtInRect(&i->visibile, pt) || pt.y > i->visibile.top + 120 * scalaFinestra(h)) return;
    // pulsante chiudi dell'app: foto per l'animazione di chiusura (parte solo se la finestra si chiude davvero)
    {
        DWORD_PTR colpo = 0;
        if (SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y), SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &colpo) && colpo == HTCLOSE) { preparaChiusura(h); return; }
    }
    if (!impostazioni::valori().genio || !gMovimento) return;
    const Anim* in = animDi(h);
    if (in && !(in->tipo == Tipo::Ripristina && in->genio)) return;
    // Barre del titolo classiche: il rettangolo dei pulsanti disegnati da DWM lo da' WM_GETTITLEBARINFOEX
    // (l'hit test classico usa una geometria diversa e sbaglia ai bordi). Barre disegnate dall'app
    // (Chromium, app moderne): rettangolo vuoto, ma il loro hit test risponde HTMINBUTTON in modo preciso.
    TITLEBARINFOEX tb{}; tb.cbSize = sizeof(tb);
    DWORD_PTR ok = 0;
    const bool classica = SendMessageTimeoutW(h, WM_GETTITLEBARINFOEX, 0, (LPARAM)&tb, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &ok) &&
                          tb.rgrect[2].right > tb.rgrect[2].left;
    if (classica) {
        if (!PtInRect(&tb.rgrect[2], pt)) return;
    } else {
        DWORD_PTR colpo = 0;
        if (!SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y), SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &colpo)) return;
        if (colpo != HTMINBUTTON) return;
    }
    if (in) invertiRipristino(h); else preparaCopia(h, *i);
}

// Riduzione chiesta dal dock (clic sull'icona dell'app gia' in primo piano): stessa copia del pulsante "_",
// poi la riduzione la chiediamo noi. false = non possibile, il dock usa la riduzione normale.
static bool riduciDalDock(HWND h) {
    if (gPausa || !gMovimento || !impostazioni::valori().genio || !h || IsIconic(h)) { logf("dock: riduzione nativa %p (pausa o genio spento)", h); return false; }
    if (animDi(h)) {
        if (!invertiRipristino(h)) { logf("dock: riduzione nativa %p (gia' in animazione)", h); return false; }
    } else {
        InfoFinestra* i = traccia(h);
        if (!i || !leggiRettangoli(h, *i) || !preparaCopia(h, *i)) { logf("dock: riduzione nativa %p (copia non pronta)", h); return false; }
    }
    // nessun pulsante premuto: se la riduzione non arriva entro ~0,4 s la copia si smonta da sola
    if (Anim* a = animDi(h)) { a->fase = 1; a->tFase = ora() + 0.25; }
    PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    PostMessageW(gMessaggi, WM_NULL, 0, 0);
    logf("riduzione dal dock %p", h);
    return true;
}

static void suFineCattura(HWND figlia) {
    HWND h = GetAncestor(figlia, GA_ROOT);
    Anim* a = animDi(h);
    if (a && a->tipo == Tipo::Pronta && a->fase == 0) {
        a->fase = 1; a->tFase = ora();
        logf("fine cattura %p", h);
    }
}

// Finestre ridotte senza passare da noi (tasti Win, menu della finestra, app che si riduce da sola): la riduzione
// resta quella di Windows, ma a riduzione finita la finestra si prepara come le nostre, cosi' ricompare col genio
static std::unordered_set<HWND> gDaAdottare;

static void adotta(HWND h) {
    if (gPausa || !gMovimento || !impostazioni::valori().genio || !IsWindow(h) || !IsIconic(h) || animDi(h) || !finestraApp(h)) return;
    if (!nascondibile(h)) return;   // layered per conto suo in un modo che non si puo' ripristinare
    spegniTransizioniNative(h);
    if (!rendiLayered(h, 0)) { riaccendiTransizioniNative(h); return; }
    logf("adottata %p", h);
}

static void suRiduci(HWND h) {
    Anim* a = animDi(h);
    if (!a || a->tipo == Tipo::Ripristina) {        // riduzione non anticipata: resta l'animazione nativa
        if (a) annullaAnimDi(h);
        if (!gPausa && finestraApp(h)) { gDaAdottare.insert(h); SetTimer(gMessaggi, 3, 450, nullptr); }
        logf("riduzione nativa %p", h);
        return;
    }
    if (a->tipo != Tipo::Pronta) return;
    if (a->genio) {
        // ripristino invertito: la foto riparte verso l'icona dal punto in cui era
        a->tipo = Tipo::Riduci; a->fase = 0; a->fotogrammiGenio = 0;
        a->t0 = tempoProssimoFotogramma() - inversaCurvaGenio(a->pGenio) * DURATA_GENIO_RIDUCI;
        RECT ic; if (iconaBarra(h, ic)) a->a = ic;
        alfaFinestra(h, 0);
        logf("riduci %p genio invertito da p=%.2f", h, a->pGenio);
        if (gDockOk) SetTimer(gMessaggi, 2, 80, nullptr);
        return;
    }
    // la copia e' gia' sopra la finestra: da qui va verso la barra
    a->tipo = Tipo::Riduci; a->fase = 0;
    a->da = a->attuale = a->a;
    a->t0 = tempoProssimoFotogramma();
    RECT ic;
    if (gGenioOk && gFoto.count(h) && iconaBarra(h, ic)) {
        a->genio = true; a->a = ic; a->fotogrammiGenio = 0;
    } else {
        liberaFoto(h);
        a->a = puntoBarra(h);
        a->ampiezza = (double)distanzaMassima(a->da, a->a);
        a->molla = MOLLA_RIDUCI;
    }
    alfaFinestra(h, 0);   // resta trasparente anche da ridotta, pronta per il ripristino
    logf("riduci %p genio=%d bersaglio %ld,%ld-%ld,%ld", h, (int)a->genio, a->a.left, a->a.top, a->a.right, a->a.bottom);
    if (gDockOk) SetTimer(gMessaggi, 2, 80, nullptr);   // poco dopo: se il focus e' finito alla barra di Windows, attiva la finestra successiva
}

static void suRipristina(HWND h) {
    const bool nostra = GetPropW(h, PROP_NASCOSTA) != nullptr;
    gDaAdottare.erase(h);
    // stava ancora entrando nel dock col genio: torna indietro dal punto in cui era, senza ripartire dall'icona
    double pDa = 1.0;
    if (const Anim* a = animDi(h); a && a->tipo == Tipo::Riduci && a->genio) pDa = a->pGenio;
    annullaAnimDi(h);
    if (!nostra) { logf("ripristino nativo %p", h); liberaFoto(h); return; }   // ridotta con l'animazione nativa
    // raffica di ripristini (per esempio Win+D premuto di nuovo): dalla terza in poi niente animazione
    static double ultimoRipristino = 0; static int raffica = 0;
    const double adesso = ora();
    raffica = adesso - ultimoRipristino < 0.25 ? raffica + 1 : 0; ultimoRipristino = adesso;
    InfoFinestra* i = traccia(h);
    if (gPausa || !gMovimento || !i || raffica >= 2 || schermoInteroAttivo() || !leggiRettangoli(h, *i)) {
        logf("ripristino senza animazione %p (pausa=%d info=%d raffica=%d)", h, (int)gPausa, (int)(i != nullptr), raffica);
        liberaFoto(h); rendiVisibileFinestra(h); return;
    }
    RECT ic;
    // ridotta da Windows (adottata): la foto per il genio si fa adesso, mentre la finestra e' ancora trasparente
    if (gGenioOk && !gFoto.count(h) && iconaBarra(h, ic)) {
        const double t1 = ora();
        if (gGenio.cattura(h, i->visibile, gFoto[h])) logf("foto al ripristino %p in %.1f ms", h, (ora() - t1) * 1000);
        else { liberaFoto(h); logf("foto al ripristino non riuscita %p", h); }
    }
    if (gGenioOk && gFoto.count(h) && iconaBarra(h, ic)) {
        Anim n;
        n.hwnd = h; n.tipo = Tipo::Ripristina; n.genio = true;
        n.da = i->visibile; n.a = ic; n.pGenio = pDa;
        n.t0 = n.tFase = tempoProssimoFotogramma() - (1.0 - std::cbrt(pDa)) * DURATA_GENIO_RIPRISTINA;
        gAnim.push_back(n);
        logf("ripristino genio %p da p=%.2f", h, pDa);
        return;
    }
    liberaFoto(h);
    if (!nuovaAnim(h, Tipo::Ripristina, puntoBarra(h), i->visibile, *i, 89)) rendiVisibileFinestra(h);
}

// Un fotogramma dell'ingrandimento disegnato dalla GPU. Ogni punto della foto va dal vecchio rettangolo al nuovo
// mescolando le due molle secondo la distanza dall'angolo in alto a destra: l'angolo segue la molla veloce, quello
// opposto la gelatinosa, e in mezzo i bordi si curvano. pv/pl = molla veloce/lenta (0 partenza, 1 arrivo)
static const int ZOOM_C = 12, ZOOM_R = 9;
static void elementiZoom(Anim& a, double pv, double pl, double alfa, std::vector<genio::Elemento>& el) {
    const double dw = a.da.right - a.da.left, dh = a.da.bottom - a.da.top, aw = a.a.right - a.a.left, ah = a.a.bottom - a.a.top;
    a.magliaZ.resize(ZOOM_C * ZOOM_R);
    for (int r = 0; r < ZOOM_R; r++) for (int c = 0; c < ZOOM_C; c++) {
        const double u = (double)c / (ZOOM_C - 1), v = (double)r / (ZOOM_R - 1);
        // in orizzontale conta soprattutto quanto si e' lontani dal bordo destro, in verticale dal bordo alto: le
        // righe restano quasi dritte (contenuto non storto); la piccola parte incrociata da' l'arco della gelatina
        const double wx = std::min(1.0, (1.0 - u) * 0.88 + v * 0.12), wy = std::min(1.0, v * 0.88 + (1.0 - u) * 0.12);
        const double qx = pv + (pl - pv) * wx, qy = pv + (pl - pv) * wy;
        const double x0 = a.da.left + u * dw, y0 = a.da.top + v * dh, x1 = a.a.left + u * aw, y1 = a.a.top + v * ah;
        a.magliaZ[r * ZOOM_C + c] = {(float)(x0 + (x1 - x0) * qx), (float)(y0 + (y1 - y0) * qy), (float)u, (float)v};
    }
    // ombra: un anello attorno alla griglia, come per la gelatina
    const int OC = ZOOM_C + 2, OR = ZOOM_R + 2;
    const genio::Immagine& base = a.fotoZ.srv ? a.fotoZ : a.fotoPrima;   // la nuova puo' non esserci ancora
    const double fw = std::max(1, base.w), fh = std::max(1, base.h), M = 80;
    a.ombraZ.resize(OC * OR);
    for (int r = 0; r < OR; r++) for (int c = 0; c < OC; c++) {
        const int ci = std::clamp(c - 1, 0, ZOOM_C - 1), ri = std::clamp(r - 1, 0, ZOOM_R - 1);
        const genio::Vertice& b = a.magliaZ[ri * ZOOM_C + ci];
        const double sx = c == 0 ? -1 : (c == OC - 1 ? 1 : 0), sy = r == 0 ? -1 : (r == OR - 1 ? 1 : 0);
        a.ombraZ[r * OC + c] = {(float)(b.x + sx * M), (float)(b.y + sy * M), (float)(b.u + sx * M / fw), (float)(b.v + sy * M / fh)};
    }
    // angoli: sempre 24 px e tondi a schermo (raggio delle finestre), riportati alla scala della foto nei due versi
    const double pm = (pv + pl) / 2, larg = std::max(1.0, dw + (aw - dw) * pm), alt = std::max(1.0, dh + (ah - dh) * pm);
    const double R = raggioDi(a.hwnd);
    genio::Elemento o{&base, a.a, a.a, 0, alfa};
    o.maglia = a.ombraZ.data(); o.colonne = OC; o.righe = OR; o.ombra = true; o.raggio = R * fw / larg;
    el.push_back(o);
    genio::Elemento e{&a.fotoZ, a.a, a.a, 0, alfa};
    e.maglia = a.magliaZ.data(); e.colonne = ZOOM_C; e.righe = ZOOM_R; e.raggio = R * fw / larg; e.raggioY = R * fh / alt;
    el.push_back(e);
    // la foto sostituita da una rilettura sfuma sopra la nuova (vedi rileggiFotoZoom)
    if (a.fotoZVecchia.srv && a.alfaVecchia > 0) {
        genio::Elemento v{&a.fotoZVecchia, a.a, a.a, 0, alfa * a.alfaVecchia};
        v.maglia = a.magliaZ.data(); v.colonne = ZOOM_C; v.righe = ZOOM_R;
        v.raggio = R * a.fotoZVecchia.w / larg; v.raggioY = R * a.fotoZVecchia.h / alt;
        el.push_back(v);
    }
    // lo stato di prima sopra, che sfuma nel nuovo nella prima meta' del tragitto (niente impaginazione "sfasata")
    if (a.fotoPrima.srv) {
        const double sfuma = a.alfaPrima;   // resta finche' la pagina nuova non si e' assestata (vedi rileggiFotoZoom)
        if (sfuma > 0) {
            genio::Elemento p{&a.fotoPrima, a.a, a.a, 0, alfa * sfuma};
            p.maglia = a.magliaZ.data(); p.colonne = ZOOM_C; p.righe = ZOOM_R;
            p.raggio = R * a.fotoPrima.w / larg; p.raggioY = R * a.fotoPrima.h / alt;
            el.push_back(p);
        }
    }
}

// Ingrandita o ripristinata (le animazioni native sono spente: e' gia' al nuovo posto). Come per riduzione e
// ripristino dal dock: la finestra vera resta trasparente, al suo posto la sua miniatura viva va con una molla dal
// vecchio rettangolo al nuovo, e la capsula sta attaccata al suo angolo fotogramma per fotogramma
static void avviaZoom(HWND h, const RECT& da, InfoFinestra& i) {
    auto no = [&](const char* perche) { logf("ingrandimento senza animazione %p: %s", h, perche); };
    if (!zoomAnimato()) return no("animazioni spente");
    if (animDi(h) || gelatinaDi(h) || aperturaDi(h)) return no("gia' animata");
    if (h == gDaIngrandita || h == gInSpostamento) return no("trascinata");
    // (niente controllo dello "schermo intero": con la barra di Windows nascosta per Windows lo e' ogni finestra ingrandita)
    if (IsIconic(h) || !IsWindowVisible(h)) return no("non visibile");
    if (!nascondibile(h)) return no("non nascondibile");
    Anim n;
    n.hwnd = h; n.tipo = Tipo::Ripristina; n.zoom = true;
    n.da = n.attuale = da; n.a = i.visibile;
    n.t0 = n.tFase = tempoProssimoFotogramma();
    n.ampiezza = (double)distanzaMassima(da, i.visibile);
    n.sorgente = sorgenteDa(i);
    // senza foto di prima, mentre si fa quella nuova la copia viva della finestra sta in una finestrella ferma
    // alla vecchia misura; con la foto di prima la disegna subito la GPU
    if (!n.fotoPrima.srv) {
        n.vetrina = creaVetrina();
        if (!n.vetrina) return no("finestrella non creata");
        if (FAILED(DwmRegisterThumbnail(n.vetrina, h, &n.thumb))) { DestroyWindow(n.vetrina); return no("miniatura non pronta"); }
        impostaMiniatura(n, da, 255);
    }
    gAnim.push_back(n);
    if (!rendiLayered(h, 0)) { annullaAnimDi(h); return no("trasparenza rifiutata"); }
    logf("ingrandimento animato %p (%s)", h, i.zoom ? "ingrandita" : "ripristinata");
}

static void gestisciEvento(DWORD ev, HWND h);

static void CALLBACK suEvento(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG figlio, DWORD, DWORD) {
    if (!h || obj != OBJID_WINDOW || figlio != CHILDID_SELF) return;
    const double t = ora();
    gestisciEvento(ev, h);
    // gli eventi vengono gestiti dentro GetMessage, che pero' non ritorna finche' non arriva un messaggio vero:
    // se e' partita un'animazione, svegliamo subito il ciclo principale (altrimenti aspetterebbe il timer)
    if (!gAnim.empty() || !gGelatine.empty() || !gAperture.empty() || chiusureAttive() || copertureAttive()) PostMessageW(gMessaggi, WM_NULL, 0, 0);
    if (gLog && ora() - t > 0.010) logf("evento lento 0x%lx su %p: %.1f ms", ev, h, (ora() - t) * 1000);
}

static void gestisciEvento(DWORD ev, HWND h) {
    switch (ev) {
        case EVENT_SYSTEM_MOVESIZESTART:
            suInizioSpostamento(h); break;
        case EVENT_SYSTEM_MOVESIZEEND:
            suFineSpostamento(h); break;
        case EVENT_SYSTEM_CAPTURESTART:  suCattura(h); break;
        case EVENT_SYSTEM_CAPTUREEND:    suFineCattura(h); break;
        case EVENT_SYSTEM_MINIMIZESTART:
            suRiduci(h); break;
        case EVENT_SYSTEM_MINIMIZEEND:
            logf("ripristina %p", h); suRipristina(h);
            break;
        case EVENT_OBJECT_SHOW: {
            // ricomparsa senza passare dal ripristino (per esempio un'app riaperta dall'area di notifica): mai trasparente
            if (gNascoste.count(h) && !animDi(h) && !aperturaDi(h) && !gelatinaDi(h) && !IsIconic(h) && !gInAttesa.count(h)) { logf("ricomparsa %p", h); rendiVisibileFinestra(h); }
            suNuovaFinestra(h);   // finestra che si apre: esce dall'icona del dock o cresce dal centro
            // la barra di Windows prova a ricomparire (Explorer riavviato, pannelli di sistema): resta nascosta
            wchar_t c[32] = L""; GetClassNameW(h, c, 32);
            if (gDockOk && (!wcscmp(c, L"Shell_TrayWnd") || !wcscmp(c, L"Shell_SecondaryTrayWnd"))) {
                // (succede a ogni riduzione e ripristino: Windows la rimette per 7-8 ms, in posizione "a scomparsa", cioe'
                // 2 px in fondo allo schermo per un fotogramma; misurato con barra.ps1. Impedirlo vorrebbe dire cambiare
                // stile o trasparenza alla barra di Explorer da fuori: non vale il rischio. Nel registro un riepilogo)
                ShowWindowAsync(h, SW_HIDE);
                static int volte = 0; static double ultimo = -1e9; volte++;
                if (ora() - ultimo > 30) { logf("barra di Windows ricomparsa e rinascosta (%d volte)", volte); ultimo = ora(); volte = 0; }
                break;
            }
            [[fallthrough]];
        }
        case EVENT_SYSTEM_FOREGROUND:
            traccia(h);
            if (gDockOk && GetAncestor(h, GA_ROOT) == h) { gDock.segnaCambiamento(); gDock.controllaFinestre(); }
            break;
        case EVENT_OBJECT_HIDE:
            if (!gChiusure.empty()) avviaChiusura(h);
            break;
        case EVENT_OBJECT_UNCLOAKED:
            suSvelata(h);
            break;
        case EVENT_OBJECT_CREATE:
            suCreata(h);
            break;
        case EVENT_OBJECT_DESTROY:
            if (!gChiusure.empty()) avviaChiusura(h);
            annullaAnimDi(h); gInfo.erase(h); gNascoste.erase(h); gDaAdottare.erase(h); liberaFoto(h); gUltimoPosto.erase(h); gNateVelate.erase(h);
            if (auto at = gInAttesa.find(h); at != gInAttesa.end()) { at->second.copertura.libera(); gInAttesa.erase(at); }
            if (h == gTrascinoApp) gTrascinoApp = nullptr;
            if (h == gInSpostamento) gInSpostamento = nullptr;
            for (auto it = gGelatine.begin(); it != gGelatine.end();) if (it->hwnd == h) { it->foto.libera(); it = gGelatine.erase(it); } else ++it;
            if (gDockOk && GetAncestor(h, GA_ROOT) == h) gDock.segnaCambiamento();
            break;
        case EVENT_OBJECT_LOCATIONCHANGE: {
            if (gDockOk && h == GetForegroundWindow()) gDock.controllaFinestre();   // ingrandita o rimpicciolita
            if (h == gDaIngrandita && !IsZoomed(h)) {
                // staccata dallo schermo intero: 150 ms perche' l'app si ridisegni alla nuova misura e DWM aggiorni i bordi,
                // poi tasti e gelatina (prima la foto avrebbe ancora il contenuto impaginato da ingrandita, spostato)
                if (gSganciata == 0) gSganciata = ora();
                else if (ora() - gSganciata > 0.15) {
                    suInizioSpostamento(h);
                    if (gelatinaDi(h) || ora() - gSganciata > 1.0) { logf("staccata da ingrandita %p gelatina=%d", h, (int)(gelatinaDi(h) != nullptr)); gDaIngrandita = nullptr; }
                }
            }
            if (GetAncestor(h, GA_ROOT) == h) {   // trascinamento fatto dall'app (vedi gTrascinoApp)
                RECT w; GetWindowRect(h, &w);
                auto up = gUltimoPosto.find(h);
                if (up != gUltimoPosto.end() && !gInSpostamento && !gTrascinoApp && gMovimento && impostazioni::valori().gelatina) {
                    const RECT& p0 = up->second;
                    const bool spostata = (w.left != p0.left || w.top != p0.top) && w.right - w.left == p0.right - p0.left && w.bottom - w.top == p0.bottom - p0.top;
                    if (spostata && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) && h == GetForegroundWindow() && !gelatinaDi(h) && !animDi(h) &&
                        !IsZoomed(h) && !IsIconic(h) && w.left > -30000 && finestraApp(h)) {
                        logf("trascinamento fatto dall'app %p", h);
                        gTrascinoApp = h;
                        suInizioSpostamento(h);
                        if (!gelatinaDi(h)) { gTrascinoApp = nullptr; gInSpostamento = nullptr; }
                    }
                }
                gUltimoPosto[h] = w;
            }
            auto it = gInfo.find(h);
            if (it == gInfo.end()) break;
            if (gLog && animDi(h)) {
                RECT w; GetWindowRect(h, &w);
                logf("posizione %p %ld,%ld-%ld,%ld ridotta=%d", h, w.left, w.top, w.right, w.bottom, (int)IsIconic(h));
            }
            const bool zoomPrima = it->second.zoom, validaPrima = it->second.valida;
            const RECT visPrima = it->second.visibile;
            if (!leggiRettangoli(h, it->second)) break;
            if (validaPrima && zoomPrima != it->second.zoom) {
                Anim* z = animDi(h);
                if (!z) avviaZoom(h, visPrima, it->second);
                else if (z->zoom) {
                    // cambiata di nuovo mentre si anima: si riparte (foto nuova) dalla forma che ha adesso
                    const RECT daQui = z->fase == 0 && z->passiZ >= 3 ? z->attuale : (z->fase == 0 ? z->da : visPrima);
                    annullaAnimDi(h); avviaZoom(h, daQui, it->second);
                }
            }
            // la finestra si muove mentre il pulsante e' premuto (es. trascinata): la copia non deve restare indietro
            Anim* a = animDi(h);
            // (si guarda GetWindowRect: quello "visibile" di DWM salta di qualche pixel quando cambia lo stile layered)
            RECT wr; GetWindowRect(h, &wr);
            if (a && a->tipo == Tipo::Pronta && a->fase < 2 && !a->genio && !EqualRect(&wr, &a->fin)) annullaPronta(*a, ora());
            break;
        }
    }
}

// Rete di sicurezza: nessuna finestra deve restare invisibile per errore
// Una finestra ripristinata resta trasparente per i pochi millisecondi prima che arrivi il suo evento:
// si interviene solo se la situazione dura da piu' di un secondo.
static std::unordered_map<HWND, ULONGLONG> gSospette;

static void controlloSicurezza() {
    const ULONGLONG adesso = GetTickCount64();
    std::vector<HWND> daMostrare;
    // rese trasparenti alla creazione: si aspetta che compaiano, ma non per sempre (finestre mai mostrate)
    // (quelle col thread ancora in corso restano: la risposta arriva comunque)
    for (auto it = gInAttesa.begin(); it != gInAttesa.end();)
        it = it->second.pronta && (ora() - it->second.t > 5.0 || !IsWindow(it->first)) ? gInAttesa.erase(it) : std::next(it);
    for (HWND h : gNascoste) {
        if (!IsWindow(h)) { daMostrare.push_back(h); continue; }
        if (gInAttesa.count(h)) { gSospette.erase(h); continue; }
        if (animDi(h) || gelatinaDi(h) || aperturaDi(h) || IsIconic(h)) { gSospette.erase(h); continue; }
        auto [it, nuova] = gSospette.try_emplace(h, adesso);
        if (!nuova && adesso - it->second > 1000) daMostrare.push_back(h);   // visibile ma ancora trasparente: errore
    }
    for (auto it = gSospette.begin(); it != gSospette.end();)
        it = gNascoste.count(it->first) ? std::next(it) : gSospette.erase(it);
    for (HWND h : daMostrare) { logf("sicurezza: rimostro %p", h); rendiVisibileFinestra(h); }
}

// ---------------------------------------------------------------------------------------------
// Icona nell'area di notifica

static NOTIFYICONDATAW gIcona;

static void impostaPausa(bool p) {
    gPausa = p;
    for (auto& a : gAnim) { if (a.tipo != Tipo::Riduci) rendiVisibileFinestra(a.hwnd); chiudiMiniatura(a); liberaFotoZoom(a); }
    gAnim.clear(); mostraOverlay(false);
    for (auto& g : gGelatine) liberaGelatina(g);
    gGelatine.clear();
    svuotaAperture();
    // (quelle che il thread sta ancora rendendo trasparenti le rimostra la sua risposta)
    for (auto it = gInAttesa.begin(); it != gInAttesa.end();) {
        if (!it->second.pronta) { it->second.annulla = true; ++it; continue; }
        if (IsWindow(it->first)) rendiVisibileFinestra(it->first);
        it = gInAttesa.erase(it);
    }
    gNateVelate.clear();
    if (gGenioOk && gGenio.visibile) { gGenio.svuota(); gGenio.mostra(false); }
    if (p) {
        for (HWND h : std::vector<HWND>(gNascoste.begin(), gNascoste.end())) rendiVisibileFinestra(h);
        for (auto& [h, f] : gFoto) f.libera();
        gFoto.clear();
    }
    aggiornaTransizioni();
    wcscpy(gIcona.szTip, p ? testi::t(L"FluidWin (in pausa)", L"FluidWin (paused)") : L"FluidWin");
    Shell_NotifyIconW(NIM_MODIFY, &gIcona);
}

// ---------------------------------------------------------------------------------------------
// Tasto Win da solo: apre il launcher di FluidWin invece dello Start. Hook di tastiera di basso livello su un
// thread a parte (deve rispondere subito), come fa PowerToys: guarda soltanto se il tasto premuto e' Win, non
// registra e non invia altro. Win insieme ad altri tasti (Win+E, Win+D...) resta quello di Windows.

static const UINT WM_APRI_LAUNCHER = WM_APP + 5;
static std::atomic<bool> gLauncherTasto{false};   // false con un gioco a schermo intero o senza dock: lo Start resta normale
static HHOOK gHookTasti = nullptr;
static DWORD gThreadTasti = 0;
static bool gWinGiu = false, gWinConAltro = false, gWinMaschera = false, gAltMaschera = false, gF4Preso = false;
static const UINT WM_VOLUME = WM_APP + 6, WM_PANORAMICA = WM_APP + 7, WM_RICERCA = WM_APP + 8, WM_IMPOSTAZIONI = WM_APP + 9, WM_PANNELLO = WM_APP + 10, WM_CHIUDI_FG = WM_APP + 11;
// scorciatoie di FluidWin (dalle impostazioni): tasti del volume, Win+Tab (panoramica), Alt+Tab (ricerca)
static std::atomic<bool> gTastiVolume{false}, gTastoPanoramica{false}, gTastoRicerca{false};

// Battito del thread principale: lo aggiorna un timer ogni 250 ms (e ogni fotogramma). Se e' fermo da piu' di un
// secondo (FluidWin bloccato) l'hook non si tiene piu' i tasti: volume, Win, Alt+Tab e Alt+F4 tornano a Windows,
// invece di essere inghiottiti e non fare nulla
static std::atomic<ULONGLONG> gBattito{0};
static bool uiViva() { return GetTickCount64() - gBattito.load() < 1000; }

static LRESULT CALLBACK suTasto(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        const bool win = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
        const bool giu = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
        const bool viva = uiViva();
        if (!(k->flags & LLKHF_INJECTED)) {
            // tasti del volume: li gestisce FluidWin (cambia il volume e mostra il suo indicatore); quello di Windows
            // non compare. Si trattengono anche i rilasci, cosi' Windows non vede niente
            if ((k->vkCode == VK_VOLUME_UP || k->vkCode == VK_VOLUME_DOWN || k->vkCode == VK_VOLUME_MUTE) && gTastiVolume && gLauncherTasto && viva) {
                if (giu) PostMessageW(gMessaggi, WM_VOLUME, k->vkCode == VK_VOLUME_UP ? 1 : k->vkCode == VK_VOLUME_DOWN ? 2 : 0, 0);
                return 1;
            }
            // Win+Tab: panoramica di FluidWin. Al rilascio di Win serve la "maschera" (sotto), altrimenti Windows vedrebbe
            // Win premuto e rilasciato da solo e aprirebbe lo Start. (Win+S e Win+Maiusc+S restano di Windows)
            if (gWinGiu && gLauncherTasto && k->vkCode == VK_TAB && gTastoPanoramica && viva) {
                if (giu) PostMessageW(gMessaggi, WM_PANORAMICA, 0, 0);
                gWinConAltro = true; gWinMaschera = true;
                return 1;
            }
            // Alt+Tab: la ricerca di FluidWin al posto del selettore di Windows (una volta per pressione di Alt). Anche qui
            // al rilascio di Alt va la maschera: Alt premuto e rilasciato da solo aprirebbe i menu delle app.
            // Sempre, finche' FluidWin gira (anche con un'app a schermo intero o senza dock): il selettore di Windows
            // non deve comparire mai. (Con Alt+Maiusc+Tab e Ctrl+Alt+Tab lo stesso: hanno Alt premuto)
            if (!gWinGiu && gTastoRicerca && k->vkCode == VK_TAB && (viva || gAltMaschera) && ((k->flags & LLKHF_ALTDOWN) || gAltMaschera)) {
                if (giu && !gAltMaschera) PostMessageW(gMessaggi, WM_RICERCA, 0, 0);
                gAltMaschera = true;
                return 1;
            }
            // Alt+F4 su una finestra d'app: lo fa FluidWin (foto per l'animazione, poi la stessa chiusura di Windows)
            if (k->vkCode == VK_F4 && (gF4Preso || ((k->flags & LLKHF_ALTDOWN) && giu && !gWinGiu && gLauncherTasto && viva))) {
                if (giu && !gF4Preso) {
                    HWND fg = GetAncestor(GetForegroundWindow(), GA_ROOT);
                    if (!fg || !finestraApp(fg)) return CallNextHookEx(nullptr, code, w, l);   // desktop e altro: Alt+F4 di Windows
                    PostMessageW(gMessaggi, WM_CHIUDI_FG, (WPARAM)fg, 0);
                    gF4Preso = true; gAltMaschera = true;
                } else if (!giu) gF4Preso = false;
                return 1;
            }
            if (gAltMaschera && !giu && (k->vkCode == VK_LMENU || k->vkCode == VK_RMENU || k->vkCode == VK_MENU)) {
                gAltMaschera = false;
                INPUT in[3]{};
                for (auto& i : in) i.type = INPUT_KEYBOARD;
                in[0].ki.wVk = 0xE8; in[1].ki.wVk = 0xE8; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                in[2].ki.wVk = (WORD)k->vkCode; in[2].ki.dwFlags = KEYEVENTF_KEYUP | (k->vkCode == VK_RMENU ? KEYEVENTF_EXTENDEDKEY : 0);
                SendInput(3, in, sizeof(INPUT));
                return 1;
            }
            if (win && giu) { if (!gWinGiu) { gWinGiu = true; gWinConAltro = false; gWinMaschera = false; } }
            else if (win) {
                const bool solo = gWinGiu && !gWinConAltro;
                gWinGiu = false;
                if (gWinMaschera) {
                    gWinMaschera = false;
                    INPUT in[3]{};
                    for (auto& i : in) i.type = INPUT_KEYBOARD;
                    in[0].ki.wVk = 0xE8; in[1].ki.wVk = 0xE8; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    in[2].ki.wVk = (WORD)k->vkCode; in[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
                    SendInput(3, in, sizeof(INPUT));
                    return 1;
                }
                // (anche con un gioco o un video a tutto schermo: Win apre il launcher di FluidWin, non lo Start)
                if (solo && gDockOk && viva) {
                    // Windows apre lo Start al rilascio di Win se nel frattempo non e' stato premuto altro: prima del
                    // rilascio (trattenuto qui e reinviato) si inserisce un tasto "maschera" che non fa nulla (0xE8)
                    INPUT in[3]{};
                    for (auto& i : in) i.type = INPUT_KEYBOARD;
                    in[0].ki.wVk = 0xE8; in[1].ki.wVk = 0xE8; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    in[2].ki.wVk = (WORD)k->vkCode; in[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
                    SendInput(3, in, sizeof(INPUT));
                    PostMessageW(gMessaggi, WM_APRI_LAUNCHER, 0, 0);
                    return 1;
                }
            } else if (gWinGiu) gWinConAltro = true;
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// Windows stacca in silenzio un hook di basso livello che una volta risponde troppo tardi (PC sotto sforzo), e da
// li' in poi Win, Alt+Tab e i tasti del volume tornerebbero a Windows fino al riavvio di FluidWin. Per questo il
// thread ha priorita' alta e ogni 15 s, se in quel momento non c'e' nessun tasto in mano a FluidWin, rimette l'hook.
static DWORD WINAPI threadTasti(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    gHookTasti = SetWindowsHookExW(WH_KEYBOARD_LL, suTasto, GetModuleHandleW(nullptr), 0);
    SetTimer(nullptr, 0, 15000, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_TIMER && !gWinGiu && !gAltMaschera && !gF4Preso && !gWinMaschera) {
            if (gHookTasti) UnhookWindowsHookEx(gHookTasti);
            gHookTasti = SetWindowsHookExW(WH_KEYBOARD_LL, suTasto, GetModuleHandleW(nullptr), 0);
        }
    }
    if (gHookTasti) UnhookWindowsHookEx(gHookTasti);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Indicatore del volume, panoramica, ricerca, banner delle notifiche e impostazioni

static osd::Indicatore gOsd;
static bool gOsdOk = false;
static panoramica::Panoramica gPanoramica;
static ricerca::Ricerca gRicerca;
static banner::Banner gBanner;
static impostazioni::Finestra gImpostazioni;

static void apriImpostazioni() {
    gImpostazioni.raggioWindhawk = raggio::windhawk();
    gImpostazioni.apri();
}

// applica le impostazioni (all'avvio e a ogni cambio dalla finestra)
static void applicaImpostazioni() {
    const impostazioni::Valori& v = impostazioni::valori();
    gTastiVolume = v.volume && gOsdOk;
    gTastoPanoramica = v.winTab;
    gTastoRicerca = v.winS;
    gBanner.accendi(v.banner);
    aggiornaTransizioni();
    // lingua cambiata: i testi si leggono al momento di disegnare, basta ridisegnare (date e nomi del dock si rifanno)
    static int linguaPrima = -1;
    if (v.lingua != linguaPrima) {
        if (linguaPrima >= 0 && gDockOk) gDock.linguaCambiata();
        linguaPrima = v.lingua;
        wcscpy(gIcona.szTip, gPausa ? testi::t(L"FluidWin (in pausa)", L"FluidWin (paused)") : L"FluidWin");
        if (gIcona.hWnd) Shell_NotifyIconW(NIM_MODIFY, &gIcona);
    }
    logf("impostazioni: gelatina=%d genio=%d volume=%d angolo=%d wintab=%d wins=%d banner=%d lingua=%d",
         v.gelatina, v.genio, v.volume, v.angolo, v.winTab, v.winS, v.banner, v.lingua);
}

// Angolo in alto a sinistra dello schermo principale: col mouse fermo li' (e nessun tasto premuto) si apre la
// panoramica. Si riarma quando il mouse se ne va.
static void controllaAngolo() {
    static int fermo = 0; static bool armato = true;
    if (!impostazioni::valori().angolo || !gLauncherTasto || gPanoramica.aperta()) { fermo = 0; return; }
    POINT pt; GetCursorPos(&pt);
    const RECT s = vetro::areaPrimaria();
    const bool dentro = pt.x <= s.left + 1 && pt.y <= s.top + 1 && pt.x >= s.left - 1 && pt.y >= s.top - 1;
    if (!dentro) { fermo = 0; armato = true; return; }
    if (!armato || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;
    if (++fermo >= 2) { fermo = 0; armato = false; PostMessageW(gMessaggi, WM_PANORAMICA, 0, 0); }
}

static int gTickBarra = 0;   // controlli consecutivi (500 ms) con il fuoco sulla barra di Windows

static bool fuocoSullaBarra() {
    wchar_t c[64] = L""; GetClassNameW(GetForegroundWindow(), c, 64);
    return !wcscmp(c, L"Shell_TrayWnd");
}

static void attivaFinestraInCima() {
    for (HWND f = GetTopWindow(nullptr); f; f = GetWindow(f, GW_HWNDNEXT)) {
        DWORD cl = 0; DwmGetWindowAttribute(f, DWMWA_CLOAKED, &cl, sizeof(cl));
        if (finestraApp(f) && IsWindowVisible(f) && !IsIconic(f) && !cl) { SwitchToThisWindow(f, TRUE); break; }
    }
}

static LRESULT CALLBACK procMessaggi(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Explorer riavviato (o andato in crash): la sua area di notifica e' nuova e vuota, l'icona di FluidWin va rimessa
    static const UINT WM_BARRA_CREATA = RegisterWindowMessageW(L"TaskbarCreated");
    if (m == WM_BARRA_CREATA && WM_BARRA_CREATA) {
        Shell_NotifyIconW(NIM_ADD, &gIcona);
        logf("Explorer riavviato: icona di FluidWin rimessa");
        return 0;
    }
    // prova del riavvio (solo con --log): come se la GPU fosse stata persa
    if (m == WM_APP + 60 && gLog) { if (w == 1) gGenio.perso = true; else gDock.perso = true; PostMessageW(h, WM_NULL, 0, 0); return 0; }
    if (m == WM_NATA_NASCOSTA) { nataNascosta((HWND)w, l != 0); return 0; }
    switch (m) {
        case WM_TRAY:
            if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP) {
                POINT pt; GetCursorPos(&pt);
                SetForegroundWindow(h);   // cosi' la tastiera (Esc, frecce) arriva al menu
                const int scelta = (int)menu::mostra({menu::titolo(L"FluidWin"), menu::separatore(),
                                                      menu::voce(testi::t(L"Impostazioni…", L"Settings…"), 3),
                                                      menu::voce(gPausa ? testi::t(L"Riprendi animazioni", L"Resume animations")
                                                                        : testi::t(L"Metti in pausa", L"Pause"), 1),
                                                      menu::separatore(),
                                                      menu::voce(testi::t(L"Esci", L"Exit"), 2)}, pt, h, true);
                if (GetForegroundWindow() == h) attivaFinestraInCima();   // il fuoco non resta alla nostra finestra invisibile
                if (scelta == 1) impostaPausa(!gPausa);
                else if (scelta == 2) PostQuitMessage(0);
                else if (scelta == 3) apriImpostazioni();
            }
            return 0;
        case WM_DISPLAYCHANGE:
            aggiornaOrigine(); if (gGenioOk) gGenio.aggiornaSchermo();
            SetTimer(h, 12, 800, nullptr);   // la scala nuova si legge quando Windows ha finito di cambiarla
            return 0;
        // un programma chiede a FluidWin di chiudersi (per esempio taskkill senza /F): uscita pulita, come da "Esci"
        case WM_CLOSE: logf("chiusura chiesta da fuori"); PostQuitMessage(0); return 0;
        case WM_SETTINGCHANGE:
            if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet") && gDockOk) gDock.aggiornaTemaVetri();   // tema chiaro/scuro
            SetTimer(h, 12, 800, nullptr);   // (cambiando la scala da Impostazioni arriva anche questo)
            {   // effetti di animazione di Windows accesi o spenti
                const bool prima = gMovimento;
                leggiMovimento();
                if (prima != gMovimento) { logf("effetti di animazione di Windows: %s", gMovimento ? "accesi" : "spenti"); aggiornaTransizioni(); }
            }
            return 0;
        case WM_APRI_LAUNCHER: if (gDockOk) gDock.apriLauncher(); return 0;
        case WM_HOTKEY: if (w == TASTO_PRIMO_PIANO) primoPiano(); return 0;
        case WM_VOLUME:
            if (gDockOk) {
                float liv = 0; bool muto = false;
                gDock.volumeTasto(w == 1 ? 1 : w == 2 ? -1 : 0, liv, muto);
                // nel notch; se non si vede (gioco a schermo intero) l'indicatore di vetro
                const bool nelNotch = gDock.ilNotch().volume(liv, muto);
                if (gLog) logf("volume %.2f%s: %s", liv, muto ? " muto" : "", nelNotch ? "notch" : "indicatore di vetro");
                if (!nelNotch && gOsdOk) gOsd.mostra(liv, muto);
            }
            return 0;
        case WM_PANORAMICA:
            if (gRicerca.aperta()) gRicerca.chiudi(false);
            logf("panoramica");
            gPanoramica.apri();   // ha un suo ciclo dei messaggi finche' resta aperta
            return 0;
        case WM_RICERCA: logf("ricerca"); gRicerca.apri(); return 0;
        case WM_IMPOSTAZIONI: apriImpostazioni(); return 0;
        case WM_PANNELLO: if (gDockOk) gDock.apriPannelloIsola((int)w); return 0;
        case WM_CHIUDI_FG:   // Alt+F4: foto, poi la chiusura normale (quella che farebbe Windows)
            if (IsWindow((HWND)w)) { preparaChiusura((HWND)w); PostMessageW((HWND)w, WM_SYSCOMMAND, SC_CLOSE, 0); }
            return 0;
        case WM_TIMER:
            if (w == 13) { gBattito = GetTickCount64(); return 0; }   // battito per l'hook della tastiera (uiViva)
            if (w == 12) {
                // Scala dello schermo principale cambiata (Impostazioni > Schermo, o un portatile attaccato a un monitor
                // con un'altra scala): dock, pannelli, launcher e capsule hanno misure calcolate alla scala di prima.
                // Come per la GPU persa, FluidWin si riavvia e ricrea tutto alla scala nuova
                KillTimer(h, 12);
                const UINT d = dpiPrincipale();
                if (d != gDpiAvvio) { logf("scala cambiata: %u -> %u dpi", gDpiAvvio, d); riavvia("la scala dello schermo e' cambiata"); }
                return 0;
            }
            if (w == 7) {
                controllaAngolo();
                if (!gChiusure.empty()) pulisciChiusure();
                // fine di un trascinamento fatto dall'app: tasto sinistro rilasciato
                if (gTrascinoApp && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { HWND f = gTrascinoApp; gTrascinoApp = nullptr; suFineSpostamento(f); }
                // fine del trascinamento di Windows mai arrivata (finestra chiusa, evento perso): dopo 2 s col tasto su si dimentica
                static int suSenzaFine = 0;
                suSenzaFine = gInSpostamento && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? suSenzaFine + 1 : 0;
                if (suSenzaFine > 33) { gInSpostamento = nullptr; suSenzaFine = 0; }
                return 0;
            }
            if (w == 2) {
                // come su macOS: ridotta una finestra si attiva quella successiva. Se il focus restasse alla barra
                // di Windows (nascosta automaticamente) la barra comparirebbe sopra il dock.
                KillTimer(h, 2);
                if (fuocoSullaBarra()) attivaFinestraInCima();
                return 0;
            }
            if (w == 3) {
                KillTimer(h, 3);
                for (HWND f : gDaAdottare) adotta(f);
                gDaAdottare.clear();
                return 0;
            }
            // il fuoco resta sulla barra di Windows (per esempio chiuso il pannello delle icone nascoste con Esc):
            // finche' ce l'ha, la barra non si nasconde e copre il dock. Dopo ~1 s, col mouse altrove, si torna all'app.
            if (gDockOk) {
                POINT pt; GetCursorPos(&pt);
                RECT rb{}; GetWindowRect(FindWindowW(L"Shell_TrayWnd", nullptr), &rb);
                gTickBarra = (fuocoSullaBarra() && !PtInRect(&rb, pt)) ? gTickBarra + 1 : 0;
                if (gTickBarra >= 3) { logf("fuoco rimasto sulla barra di Windows: torno all'app"); attivaFinestraInCima(); gTickBarra = 0; }
            }
            if (gLog) {
                static bool prima = false; const bool si = schermoInteroAttivo();
                if (si != prima) {
                    prima = si; wchar_t k[64] = L""; GetClassNameW(GetForegroundWindow(), k, 64);
                    logf("schermo intero %s (primo piano %ls)%s%s", si ? "si': dock nascosto" : "finito", k, si ? ": " : "", si ? gPercheSchermoIntero : "");
                }
            }
            // GPU persa anche senza niente da disegnare (si scopre al primo Present, ma si controlla anche qui)
            if ((gGenioOk && gGenio.gpuPersa()) || (gDockOk && gDock.gpuPersa())) riavvia("la GPU e' stata persa");
            comodita::controlla();   // tieni sveglio, non disturbare e timer scaduti
            sensori::Sensori::io().controlla();   // microfono, webcam, batteria, cuffie
            if (gLog) {
                static std::vector<std::wstring> micPrima, camPrima;
                const sensori::Sensori& se = sensori::Sensori::io();
                if (se.micApp != micPrima || se.camApp != camPrima) {
                    micPrima = se.micApp; camPrima = se.camApp;
                    std::wstring m, c; for (auto& x : micPrima) m += x + L" "; for (auto& x : camPrima) c += x + L" ";
                    logf("microfono in uso: [%ls] webcam: [%ls]", m.c_str(), c.c_str());
                }
            }
            controlloSicurezza(); if (gDockOk) {
                const bool si = schermoInteroAttivo(); gLauncherTasto = !si;
                const int tipo = si ? (giocoASchermoIntero() ? 2 : 1) : 0;
                static int tipoPrima = 0;
                if (gLog && tipo != tipoPrima) logf("notch: %s", tipo == 2 ? "gioco a schermo intero, nascosto" : tipo == 1 ? "app a schermo intero, sfuma col mouse" : "normale");
                tipoPrima = tipo;
                gDock.schermoIntero(tipo); gDock.controllaFinestre(); gDock.timer(); if (gLog) logf("dock: mosse=%ld fotogrammi=%ld lente=%.2f", gDock.diagMosse, gDock.diagFotogrammi, gDock.diagLente()); } return 0;
        // spegnimento o disconnessione: Windows chiude il processo appena si torna da qui, e chiude anche il guardiano,
        // quindi i banner di Windows vanno rimessi adesso (senza, restavano spenti finche' FluidWin non veniva
        // riavviato e chiuso normalmente)
        case WM_ENDSESSION: if (w) { impostaPausa(true); ripristinaTutte(); banner::ripristinaBannerWindows(); } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Guardiano: processo separato che rimette a posto le finestre se FluidWin termina in modo anomalo

static int guardiano(DWORD pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (p) { WaitForSingleObject(p, INFINITE); CloseHandle(p); }
    ripristinaTutte();
    banner::ripristinaBannerWindows();   // FluidWin non c'e' piu': tornano i banner di Windows
    return 0;
}

static HANDLE gGuardiano = nullptr;   // il suo processo: in un riavvio voluto si ferma prima (vedi riavvia)

static void avviaGuardiano() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t cmd[MAX_PATH + 64]; swprintf(cmd, MAX_PATH + 64, L"\"%ls\" --guardiano %lu", exe, GetCurrentProcessId());
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); gGuardiano = pi.hProcess;
    }
}

// Riavvio di FluidWin (la GPU e' stata persa: dock e genio non si possono piu' disegnare). Parte un FluidWin nuovo,
// che aspetta la fine di questo; questo si chiude normalmente, rimettendo tutto a posto. Il guardiano si ferma prima:
// altrimenti, finito questo, rimetterebbe in vista la barra di Windows proprio mentre il nuovo la nasconde
static bool gRiavvio = false;
static void riavvia(const char* perche) {
    if (gRiavvio) return;
    gRiavvio = true;
    logf("riavvio: %s", perche);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t cmd[MAX_PATH + 96]; swprintf(cmd, MAX_PATH + 96, L"\"%ls\" --dopo %lu%ls", exe, GetCurrentProcessId(), gLog ? L" --log" : L"");
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi;
    // (se non riesce non si riprova a ogni fotogramma: FluidWin resta com'e', senza dock ne' genio)
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { logf("riavvio non riuscito (%lu)", GetLastError()); return; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (gGuardiano) { TerminateProcess(gGuardiano, 0); CloseHandle(gGuardiano); gGuardiano = nullptr; }
    PostQuitMessage(0);
}

static LONG WINAPI suCrash(EXCEPTION_POINTERS*) { ripristinaTutte(); banner::ripristinaBannerWindows(); return EXCEPTION_CONTINUE_SEARCH; }

// ---------------------------------------------------------------------------------------------
// Un fotogramma: animazioni e dock

static bool daDisegnare() { return !gAnim.empty() || !gGelatine.empty() || !gAperture.empty() || chiusureAttive() || copertureAttive() || (gDockOk && gDock.serveFotogramma()); }

static void unFotogramma() {
    gBattito = GetTickCount64();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const double t = tempoProssimoFotogramma();
    if (!gAnim.empty() || !gGelatine.empty() || !gAperture.empty() || chiusureAttive() || copertureAttive()) passo(t);
    // il ritmo lo da' una sola attesa del vblank per fotogramma: il genio se e' a schermo, altrimenti il dock,
    // altrimenti DwmFlush
    bool atteso = gGenio.visibile && !gGenio.perso;
    if (gDockOk && gDock.serveFotogramma()) { gDock.disegna(t, !atteso); atteso = true; }
    if (!atteso) DwmFlush();
    if (!daDisegnare()) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    // GPU persa (driver aggiornato o riavviato): dock e genio non si disegnano piu', FluidWin si riavvia
    if ((gGenioOk && gGenio.perso) || (gDockOk && gDock.perso)) riavvia("la GPU e' stata persa");
}

// ---------------------------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // icone e collegamenti del dock passano dalla shell
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); gFreq = (double)f.QuadPart;

    int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool riavviato = false, vuoleLog = false;
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--guardiano") && i + 1 < argc) { const DWORD pid = wcstoul(argv[i + 1], nullptr, 10); LocalFree(argv); return guardiano(pid); }
        // riavvio (vedi riavvia): prima si aspetta che il FluidWin di prima abbia finito di rimettere tutto a posto
        if (!wcscmp(argv[i], L"--dopo") && i + 1 < argc) {
            riavviato = true;
            if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(argv[i + 1], nullptr, 10))) { WaitForSingleObject(p, 15000); CloseHandle(p); }
        }
        if (!wcscmp(argv[i], L"--log")) vuoleLog = true;
    }
    if (argv) LocalFree(argv);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\FluidWin.Istanza");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    // il registro si apre solo ora: una seconda copia avviata per sbaglio svuoterebbe quello della prima
    // (dopo un riavvio continua quello di prima)
    if (vuoleLog) { wchar_t p[MAX_PATH]; GetTempPathW(MAX_PATH, p); wcscat(p, L"fluidwin.log"); gLog = _wfopen(p, riavviato ? L"a" : L"w"); }
    if (riavviato) logf("FluidWin riavviato");

    ripristinaTutte();          // recupero da un'eventuale chiusura anomala precedente
    avviaGuardiano();
    SetUnhandledExceptionFilter(suCrash);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    WNDCLASSW wc{}; wc.lpfnWndProc = procSenzaChiusura; wc.hInstance = inst; wc.lpszClassName = L"FluidWin.Overlay";
    RegisterClassW(&wc);
    gOverlay = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                               WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(gOverlay, 0, 255, LWA_ALPHA);
    aggiornaOrigine();
    gGenioOk = gGenio.inizializza(inst);   // senza GPU disponibile resta l'animazione a molla
    if (gGenioOk) viva::proteggi(gGenio.contesto());   // la GPU del genio la usa anche la cattura del contenuto vivo
    logf("genio disponibile: %d", (int)gGenioOk);
    salvaStatoBarra();                      // prima che il dock nasconda la barra di Windows
    gDock.riduci = riduciDalDock;
    gDock.registro = [](const char* s) { logf("dock: %s", s); };
    impostazioni::carica();   // prima di tutto: la lingua dei testi (dock compreso)
    gDockOk = gDock.inizializza(inst);
    if (!gDockOk) { gDock.chiudi(); ripristinaBarra(); }
    else { mostraBarreWindows(false); gDock.controllaFinestre(); }
    logf("dock disponibile: %d", (int)gDockOk);

    WNDCLASSW wm{}; wm.lpfnWndProc = procMessaggi; wm.hInstance = inst; wm.lpszClassName = L"FluidWin.Messaggi";
    RegisterClassW(&wm);
    gMessaggi = CreateWindowExW(0, wm.lpszClassName, L"FluidWin", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    SetTimer(gMessaggi, 1, 500, nullptr);
    // Ctrl+Win+T: sempre in primo piano (se un altro programma la usa gia', resta a lui)
    if (!RegisterHotKey(gMessaggi, TASTO_PRIMO_PIANO, MOD_CONTROL | MOD_WIN | MOD_NOREPEAT, 'T'))
        logf("Ctrl+Win+T usata da un altro programma (errore %lu): niente sempre in primo piano", GetLastError());
    gBattito = GetTickCount64(); SetTimer(gMessaggi, 13, 250, nullptr);   // battito per l'hook della tastiera
    gDpiAvvio = dpiPrincipale();
    leggiMovimento();
    if (!gMovimento) logf("effetti di animazione di Windows spenti: niente movimenti delle finestre");
    logf("schermo principale: %d x %d, scala %u%%", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), gDpiAvvio * 100 / 96);

    gIcona = {};
    gIcona.cbSize = sizeof(gIcona); gIcona.hWnd = gMessaggi; gIcona.uID = 1;
    gIcona.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; gIcona.uCallbackMessage = WM_TRAY;
    gIcona.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!gIcona.hIcon) gIcona.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(gIcona.szTip, L"FluidWin");
    Shell_NotifyIconW(NIM_ADD, &gIcona);

    gGenio.scoperta = scopertaSulloSchermo;
    // gelatina e genio con gli angoli delle finestre: quelli della mod di Windhawk, se c'e', altrimenti quelli di Windows
    const int rw = raggio::windhawk(), ra = rw > 0 ? rw : RAGGIO_ANGOLI;
    gGenio.raggioAngoli = ra * GetDpiForSystem() / 96.0f;   // (riserva: ogni foto usa i DPI della sua finestra, raggioDi)
    gRaggioBase = ra;
    logf("raggio angoli %d (Windhawk %d)", ra, rw);

    // indicatore del volume, panoramica, ricerca, banner e impostazioni
    impostazioni::carica();
    gOsdOk = gOsd.inizializza();
    logf("indicatore del volume: %d (errore %lu), thread principale %lu", (int)gOsdOk, gOsdOk ? 0 : GetLastError(), GetCurrentThreadId());
    gPanoramica.idonea = finestraApp;
    gRicerca.elencoApp = []() { return gDockOk ? gDock.elencoApp() : std::vector<std::pair<std::wstring, std::wstring>>{}; };
    gRicerca.apriImpostazioni = apriImpostazioni;
    gRicerca.conOnda = []() { return gMovimento && impostazioni::valori().onda; };
    gRicerca.registro = [](const char* s) { logf("%s", s); };
    gBanner.isola = []() { return gDockOk ? gDock.rettIsolaSchermo() : vetro::areaPrincipale(true); };
    gBanner.silenzio = []() { return schermoInteroAttivo() || comodita::silenzio(); };   // anche con Non disturbare
    // le notifiche escono dal notch (se si vede; altrimenti il banner di vetro)
    gBanner.nelNotch = [](const notifiche::Notifica& n, const std::vector<UINT>& px, int w, int h) {
        return gDockOk && gDock.ilNotch().notifica(n.app, n.titolo, n.testo, px, w, h);
    };
    gDock.ilNotch().clicNotifica = []() { gBanner.apriAttuale(); };
    gDock.ilNotch().chiudiNotifica = []() { gBanner.togliAttuale(); };
    gDock.ilNotch().notificaFinita = []() { gBanner.notchFinito(); };
    giochiNoti();   // (nel registro: quanti giochi conosce)
    // batteria e cuffie Bluetooth: un avviso nel notch
    sensori::Sensori::io().avviso = [](const std::wstring& t, int s, int v, bool a) {
        logf("avviso: %ls", t.c_str());
        if (gDockOk) gDock.ilNotch().avviso(t, s, v, a);
    };
    gImpostazioni.cambiato = applicaImpostazioni;
    gImpostazioni.registro = [](const char* s) { logf("%s", s); };
    gDock.impostazioni = apriImpostazioni;
    applicaImpostazioni();
    // banner di FluidWin spenti: quelli di Windows tornano com'erano anche se l'ultima chiusura e' stata brusca
    // (FluidWin e guardiano chiusi insieme, per esempio togliendo la corrente)
    if (!impostazioni::valori().banner) banner::ripristinaBannerWindows();
    SetTimer(gMessaggi, 7, 60, nullptr);   // angolo della panoramica

    if (gDockOk) {
        gLauncherTasto = !schermoInteroAttivo();
        CloseHandle(CreateThread(nullptr, 0, threadTasti, nullptr, 0, &gThreadTasti));
    }

    // finestre gia' aperte; quelle gia' ridotte a icona (prima dell'avvio, o rimesse a posto da ripristinaTutte)
    // si preparano subito come le nostre: anche il loro primo ripristino sara' col genio
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        traccia(h);
        if (IsIconic(h)) adotta(h);
        return TRUE;
    }, 0);

    const DWORD fl = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    HWINEVENTHOOK hooks[] = {
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, suEvento, 0, 0, fl),
        SetWinEventHook(EVENT_SYSTEM_CAPTURESTART, EVENT_SYSTEM_CAPTUREEND, nullptr, suEvento, 0, 0, fl),
        SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr, suEvento, 0, 0, fl),
        SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, suEvento, 0, 0, fl),
        // CREATE: comparsa nativa spenta in tempo; HIDE: chiusure animate
        SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, suEvento, 0, 0, fl),
        SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, suEvento, 0, 0, fl),
        SetWinEventHook(EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, suEvento, 0, 0, fl),   // aperture delle app di Windows
    };

    // i menu e la panoramica hanno un loro ciclo dei messaggi: mentre sono aperti fanno avanzare loro le animazioni
    menu::animazioniInCorso = daDisegnare;
    menu::passoAnimazioni = unFotogramma;

    // ciclo principale: messaggi quando non c'e' niente da animare, un passo per fotogramma del compositore quando si anima
    MSG msg;
    bool attivo = true;
    while (attivo) {
        if (!daDisegnare()) {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        // durante le animazioni al massimo 3 ms di eventi per fotogramma: una raffica di eventi
        // (per esempio al ripristino di una finestra) non deve ritardare il disegno
        const double inizioEventi = ora();
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { attivo = false; break; }
            const double tm = ora();
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (gLog && ora() - tm > 0.015) {
                wchar_t k[64] = L""; if (msg.hwnd) GetClassNameW(msg.hwnd, k, 64);
                logf("messaggio lento 0x%x (%ls, %llu): %.1f ms", msg.message, k, (unsigned long long)msg.wParam, (ora() - tm) * 1000);
            }
            if (daDisegnare() && ora() - inizioEventi > 0.003) break;
        }
        if (attivo && daDisegnare()) unFotogramma();
    }

    for (auto h : hooks) if (h) UnhookWinEvent(h);
    if (gThreadTasti) PostThreadMessageW(gThreadTasti, WM_QUIT, 0, 0);
    impostaPausa(true);
    if (gDockOk) gDock.chiudi();
    banner::ripristinaBannerWindows();   // senza FluidWin tornano i banner di Windows (alla prossima apertura si rispengono)
    togliPrimoPiano();
    if (gOsdOk) gOsd.chiudi();
    // finestre appena create che un thread sta rendendo trasparenti: si aspetta che finisca (poco: con un'app
    // bloccata il thread non cambia niente finche' non risponde), poi le rimette a posto ripristinaTutte
    for (int i = 0; i < 50 && gNascondendo > 0; i++) Sleep(10);
    ripristinaTutte();
    Shell_NotifyIconW(NIM_DELETE, &gIcona);
    if (gLog) fclose(gLog);
    ReleaseMutex(mutex);
    return 0;
}
