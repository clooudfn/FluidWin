// Capsule dei tasti: sopra i tre tasti di ogni finestra aperta (riduci, ingrandisci, chiudi) FluidWin mette una sua
// piccola finestra con una capsula di vetro e tre tasti tondi disegnati a mano. I tasti originali restano dove sono,
// coperti: i clic sulla capsula diventano i normali comandi di sistema (WM_SYSCOMMAND), nessun codice entra nell'app.
// Dove sono i tasti lo dicono la barra del titolo (WM_GETTITLEBARINFOEX) o, per le app che la disegnano da sole
// (Chrome, Discord...), il loro hit test (WM_NCHITTEST). Ogni capsula sta subito sopra la sua finestra nell'ordine
// di sovrapposizione: le finestre davanti la coprono come coprono i tasti veri.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <uiautomationclient.h>
#include "stampa.h"
#include "scala.h"
#include "suggerimento.h"
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>

namespace capsula {

// Lettura dello schermo in un thread a parte. Un BitBlt dallo schermo aspetta il compositore (~6 ms anche per pochi
// pixel): il controllo del colore ogni 50 ms lo faceva sul thread principale (il 16% del suo tempo, a scatti). Ora
// il thread principale chiede un rettangolo e usa l'ultima lettura arrivata; il thread lo rilegge subito.
// Ogni rettangolo chiesto ha la sua lettura: la capsula in primo piano (ogni 50 ms) e le riletture delle altre (una
// finestra appena diventata inattiva, che sfuma) non si rubano il posto
class LettoreSchermo {
public:
    ~LettoreSchermo() { ferma(); }

    // l'ultima lettura di r, se e' recente (eta' ms); intanto ne chiede una nuova
    // (avvisa: quando la nuova e' pronta arriva un messaggio WM_APP + 20 a "finestra")
    HWND finestra = nullptr;
    // dopo: valgono solo le letture con un numero piu' alto (fatte dopo che ultimo() valeva dopo)
    ULONGLONG ultimo() { if (!avvia()) return 0; EnterCriticalSection(&cs); const ULONGLONG n = contatore; LeaveCriticalSection(&cs); return n; }
    bool leggi(const RECT& r, std::vector<UINT>& out, ULONGLONG eta, bool avvisa = false, ULONGLONG dopo = 0) {
        if (!avvia()) return false;
        bool ok = false;
        const ULONGLONG ora = GetTickCount64();
        EnterCriticalSection(&cs);
        Lettura* l = nullptr;
        for (auto& x : letture) if (EqualRect(&x.r, &r)) l = &x;
        if (!l) {
            // (quelle non piu' chieste da qualche secondo se ne vanno)
            letture.erase(std::remove_if(letture.begin(), letture.end(), [&](const Lettura& x) { return ora - x.chiesta > 3000; }), letture.end());
            letture.push_back({r, {}, 0, 0, false, false}); l = &letture.back();
        }
        if (!l->px.empty() && ora - l->quando <= eta && l->numero > dopo) { out = l->px; ok = true; }
        l->chiesta = ora; l->daLeggere = true; if (avvisa) l->avvisa = true;
        LeaveCriticalSection(&cs);
        SetEvent(evento);
        return ok;
    }

    void ferma() {
        if (!thread) return;
        fine = 1; SetEvent(evento);
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread); thread = nullptr;
    }

private:
    CRITICAL_SECTION cs{};
    HANDLE evento = nullptr, thread = nullptr;
    bool pronto = false;
    volatile LONG fine = 0;
    struct Lettura { RECT r; std::vector<UINT> px; ULONGLONG quando, chiesta; bool daLeggere; bool avvisa = false; ULONGLONG numero = 0; };   // quando = letta, chiesta = ultima richiesta; avvisa = a lettura fatta, un messaggio a "finestra"
    std::vector<Lettura> letture;
    ULONGLONG contatore = 0;   // letture fatte finora (sotto cs): ognuna ha il suo numero

    bool avvia() {
        if (pronto) return thread != nullptr;
        pronto = true;
        InitializeCriticalSection(&cs);
        evento = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread = evento ? CreateThread(nullptr, 0, lavoro, this, 0, nullptr) : nullptr;
        return thread != nullptr;
    }

    static DWORD WINAPI lavoro(LPVOID p) {
        LettoreSchermo* l = (LettoreSchermo*)p;
        while (!l->fine) {
            WaitForSingleObject(l->evento, INFINITE);
            // tutte le letture chieste dopo l'ultima fatta, una per volta
            for (;;) {
            if (l->fine) break;
            RECT r{}; bool trovata = false;
            EnterCriticalSection(&l->cs);
            for (auto& x : l->letture) if (x.daLeggere) { r = x.r; trovata = true; x.daLeggere = false; break; }
            LeaveCriticalSection(&l->cs);
            if (!trovata) break;
            const int w = r.right - r.left, h = r.bottom - r.top;
            if (w <= 0 || h <= 0 || w * h > 4096 * 64) continue;
            std::vector<UINT> buf;
            HDC s = GetDC(nullptr), mem = CreateCompatibleDC(s);
            BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
            void* bits = nullptr; HBITMAP dib = CreateDIBSection(s, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (dib && bits) {
                HGDIOBJ o = SelectObject(mem, dib);
                if (BitBlt(mem, 0, 0, w, h, s, r.left, r.top, SRCCOPY)) { GdiFlush(); buf.assign((const UINT*)bits, (const UINT*)bits + (size_t)w * h); }
                SelectObject(mem, o);
            }
            if (dib) DeleteObject(dib);
            DeleteDC(mem); ReleaseDC(nullptr, s);
            if (buf.empty()) continue;
            for (UINT& c : buf) c &= 0xFFFFFF;
            EnterCriticalSection(&l->cs);
            bool avvisa = false;
            for (auto& x : l->letture) if (EqualRect(&x.r, &r)) { x.px.swap(buf); x.quando = GetTickCount64(); x.numero = ++l->contatore; avvisa = x.avvisa; x.avvisa = false; break; }
            LeaveCriticalSection(&l->cs);
            if (avvisa && l->finestra) PostMessageW(l->finestra, WM_APP + 20, 0, 0);   // chi aspettava questa lettura la usa subito
            }
        }
        return 0;
    }
};

class Motore {
public:
    bool (*riduci)(HWND) = nullptr;            // riduzione col genio (false = non possibile)
    bool (*chiudendo)(HWND) = nullptr;         // prima di chiudere: foto per l'animazione di chiusura (true = pronta)
    void (*ingrandendo)(HWND) = nullptr;       // prima di ingrandire/ripristinare: foto dello stato di prima
    bool (*fotoPronta)(HWND) = nullptr;        // la riduzione partita sara' col genio (foto con la capsula disegnata)
    bool (*idonea)(HWND) = nullptr;            // finestra d'app normale (con barra del titolo)
    void (*registro)(const char*) = nullptr;
    float raggioAngoli = 8;   // raggio degli angoli delle finestre (px a 96 dpi): 8 = quello di Windows 11
    float raggioTasto = 11;   // raggio dei tre cerchi (px a 96 dpi), uguale in tutte le app

    // cambiata la misura della capsula (impostazioni): tutte si rimisurano
    void rimisuraTutte() { for (auto& [h, c] : capsule) { nascondi(c, "nuova misura"); c.provata = false; } programma(); }

    // capsula spenta dalle impostazioni: restano i tasti originali delle app
    void spegni(bool si) { spenta = si; if (si) for (auto& [h, c] : capsule) nascondi(c, "spenta"); else programma(); }
    bool accesa() const { return !spenta; }

    bool inizializza(HINSTANCE inst) {
        gIstanza() = this;
        istanza = inst;
        WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"FluidWin.Capsula";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        WNDCLASSW wt{}; wt.lpfnWndProc = procTempi; wt.hInstance = inst; wt.lpszClassName = L"FluidWin.CapsulaTempi";
        RegisterClassW(&wt);
        tempi = CreateWindowExW(0, wt.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        if (tempi) SetTimer(tempi, 3, 50, nullptr);   // colore della barra in primo piano (controlloColore)
        uia = creaUia();
        if (tempi) { avviaSonde(); lettore.finestra = tempi; }
        return tempi && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f2d));
    }

    void chiudi() { lettore.ferma(); for (auto& [h, c] : capsule) DestroyWindow(c.fin); capsule.clear(); }

    // qualcosa e' cambiato (finestra in primo piano, comparsa, chiusura): si ricontrolla tutto fra poco.
    // rinvia = sposta il controllo piu' avanti (finestra ancora in movimento); senza, un controllo gia' previsto
    // non viene mai ritardato (le finestre che si muovono di continuo non lo bloccano)
    void programma(UINT ms = 60, bool rinvia = false) {
        if (!tempi) return;
        const ULONGLONG quando = GetTickCount64() + ms;
        if (previsto && !rinvia && previsto <= quando) return;
        previsto = quando;
        SetTimer(tempi, 1, ms, nullptr);
    }

    // la finestra si e' mossa o ha cambiato dimensione: la sua capsula sparisce e ricompare quando si ferma
    void cambiata(HWND h) {
        auto it = capsule.find(h);
        if (it == capsule.end()) { if (GetAncestor(h, GA_ROOT) == h) programma(200); return; }
        RECT v; GetWindowRect(h, &v);
        if (EqualRect(&v, &it->second.ultimoVisibile) && !IsIconic(h)) return;   // solo z-order o niente di visibile
        Capsula& c = it->second;
        if (c.segue && !IsIconic(h)) { c.ultimoVisibile = v; return; }   // la sposta FluidWin, insieme alla copia animata
        if (c.segue) { c.segue = false; c.seguiIniz = false; c.modo = 0; SetWindowPos(c.fin, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
        // ingrandita o ripristinata e l'animazione la fa FluidWin: la capsula resta e la sposta lui (segui)
        if (zoomEsterno && c.visibile && c.misurata && !c.trascinando && !IsIconic(h) && (IsZoomed(h) != FALSE) != c.zoom) {
            c.segue = true; c.ultimoVisibile = v;
            return;
        }
        // ridimensionamento (o spostamento senza gelatina) in corso: invece di sparire, e lasciar vedere i tasti
        // originali, la capsula segue l'angolo in alto a destra. A fine trascinamento un controllo silenzioso
        if (c.trascinando && c.visibile && c.misurata && c.insetOk && !IsIconic(h) && !IsZoomed(h)) {
            const RECT vis = {v.left + c.inset.left, v.top + c.inset.top, v.right - c.inset.right, v.bottom - c.inset.bottom};
            const int w = c.tasti.right - c.tasti.left, hh = c.tasti.bottom - c.tasti.top;
            c.tasti = {vis.right - (LONG)c.bordoDx - w, vis.top + (LONG)c.bordoSu, vis.right - (LONG)c.bordoDx, vis.top + (LONG)c.bordoSu + hh};
            c.ultimoVisibile = v;
            SetWindowPos(c.fin, nullptr, c.tasti.left, c.tasti.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }
        // ingrandita o ripristinata: Windows anima la finestra dal vecchio rettangolo al nuovo. Invece di sparire la
        // capsula vola con l'angolo in alto a destra; finita l'animazione un controllo silenzioso la sistema
        if (c.visibile && c.misurata && c.insetOk && !c.trascinando && !IsIconic(h) && (IsZoomed(h) != FALSE) != c.zoom) {
            // la meta non dal rettangolo di DWM, che in alcune app (Esplora file) arriva in ritardo: ingrandita = area di
            // lavoro del monitor, normale = rettangolo della finestra meno i bordi invisibili misurati a finestra normale
            RECT vis = rettVisibile(h);
            if (IsZoomed(h)) {
                MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) vis = mi.rcWork;
            } else if (c.insetNOk) vis = {v.left + c.insetN.left, v.top + c.insetN.top, v.right - c.insetN.right, v.bottom - c.insetN.bottom};
            const int w = c.tasti.right - c.tasti.left, hh = c.tasti.bottom - c.tasti.top;
            if (!c.volo) c.da = {c.tasti.left, c.tasti.top};   // gia' in volo (piu' avvisi di fila): cambia solo la meta
            c.tasti = {vis.right - (LONG)c.bordoDx - w, vis.top + (LONG)c.bordoSu, vis.right - (LONG)c.bordoDx, vis.top + (LONG)c.bordoSu + hh};
            if (!c.volo) c.voloT0 = GetTickCount64();
            c.volo = true;
            c.ultimoVisibile = v;
            c.verificaAlle = c.voloT0 + DURATA_VOLO + 80;
            SetTimer(tempi, 2, 8, nullptr);
            if (registro) { char b[96]; snprintf(b, 96, "capsula %p vola verso %ld,%ld", (void*)h, c.tasti.left, c.tasti.top); scrivi(b); }
            return;
        }
        nascondi(c, "si e' mossa");
        if (!it->second.trascinando) programma(220, true);
    }

    // FluidWin anima lui ingrandimento e ripristino (con la copia della finestra) e sposta la capsula fotogramma per
    // fotogramma sull'angolo della copia: sopra a tutto, perche' la copia sta in un overlay sopra le finestre
    bool zoomEsterno = false;
    // k = avanzamento dell'angolo (0 partenza, 1 arrivo): la pillola passa dalla posizione che aveva nello stato di
    // prima a quella che avra' nel nuovo, cosi' a fine animazione e' gia' esattamente al suo posto
    void segui(HWND h, const RECT& vis, double k = 1) {
        auto it = capsule.find(h);
        if (it == capsule.end() || !it->second.misurata) return;
        Capsula& c = it->second;
        c.segue = true; c.volo = false;
        if (!c.seguiIniz) {
            c.seguiIniz = true;
            float capR, capT; scartoPillola(c, c.pdxV, c.pdyV, capR, capT);
            // le misure dello stato nuovo, se la finestra c'e' gia' stata (ingrandita o normale)
            const bool z = IsZoomed(h) != FALSE;
            auto rc = ricordo[z ? 1 : 0].find(h);
            if (rc != ricordo[z ? 1 : 0].end()) adotta(c, rc->second);
            else c.zoom = z;
            // durante l'animazione la capsula vera e' solo la pillola: il fondo sopra i tasti originali e' dipinto
            // nelle foto (copriFoto) e si stira con loro
            c.modo = 1; disegna(c);
        }
        float dxN, dyN, capR, capT; scartoPillola(c, dxN, dyN, capR, capT);
        const float kk = (float)std::clamp(k, 0.0, 1.0);
        const int w = c.tasti.right - c.tasti.left, hh = c.tasti.bottom - c.tasti.top;
        const int x = (int)std::lround(vis.right - (c.pdxV + (dxN - c.pdxV) * kk) - capR);
        const int y = (int)std::lround(vis.top + (c.pdyV + (dyN - c.pdyV) * kk) - capT);
        c.tasti = {x, y, x + w, y + hh};
        SetWindowPos(c.fin, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        c.visibile = true;
    }
    // animazione finita (o non partita): torna al suo posto sopra la finestra e un controllo silenzioso la sistema
    void fineSegui(HWND h) {
        auto it = capsule.find(h);
        if (it == capsule.end() || !it->second.segue) return;
        Capsula& c = it->second;
        c.segue = false; c.seguiIniz = false;
        if (c.modo) { c.modo = 0; disegna(c); }
        SetWindowPos(c.fin, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        GetWindowRect(h, &c.ultimoVisibile);
        c.verificaAlle = GetTickCount64() + 40;
        programma(60, true);
    }

    // un passo del volo di tutte le capsule che stanno seguendo un ingrandimento o un ripristino
    void passoVolo() {
        const ULONGLONG ora = GetTickCount64();
        bool ancora = false;
        for (auto& [h, c] : capsule) {
            if (!c.volo) continue;
            const double k = std::min(1.0, (ora - c.voloT0) / (double)DURATA_VOLO), e = 1 - (1 - k) * (1 - k) * (1 - k);
            const int x = (int)std::lround(c.da.x + (c.tasti.left - c.da.x) * e), y = (int)std::lround(c.da.y + (c.tasti.top - c.da.y) * e);
            SetWindowPos(c.fin, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (k >= 1) { c.volo = false; programma((UINT)(c.verificaAlle - ora + 20), true); }
            else ancora = true;
        }
        if (!ancora) KillTimer(tempi, 2);
    }

    // trascinamento: la capsula resta finche' la finestra non si muove davvero (con la gelatina la nasconde lei,
    // quando al posto della finestra c'e' la foto che ha gia' la capsula disegnata sopra)
    void spostamento(HWND h, bool inizio) {
        auto it = capsule.find(h);
        if (it == capsule.end()) return;
        it->second.trascinando = inizio;
        if (!inizio) { if (it->second.visibile) it->second.verificaAlle = GetTickCount64() + 300; programma(250, true); }
    }

    // finestra ingrandita "staccata" trascinandola: i tasti si rimisurano subito, servono alla foto della gelatina
    void rimisura(HWND h) {
        if (dentroAggiorna) { programma(30); return; }   // (vedi aggiorna)
        auto it = capsule.find(h);
        if (it == capsule.end()) return;
        Capsula& c = it->second;
        nascondi(c, "rimisura");
        if (!c.misurata) { c.provata = true; c.misurata = misura(c); return; }
        // Durante il trascinamento l'app non ha ancora aggiornato la sua barra (le misure dei tasti verrebbero
        // sbagliate o non arriverebbero): i tasti restano ancorati all'angolo in alto a destra della parte visibile.
        // La misura vera si rifa' quando la finestra viene lasciata
        RECT v = rettVisibile(h), fr; GetWindowRect(h, &fr);
        c.inset = {v.left - fr.left, v.top - fr.top, fr.right - v.right, fr.bottom - v.bottom};
        const int w = c.tasti.right - c.tasti.left, hh = c.tasti.bottom - c.tasti.top;
        const int dx = (int)c.bordoDx, dy = (int)c.bordoSu;
        c.tasti = {v.right - dx - w, v.top + dy, v.right - dx, v.top + dy + hh};
        c.zoom = IsZoomed(h);
        c.provata = true;
        c.ultimoVisibile = fr;
        if (registro) { char b[120]; snprintf(b, 120, "capsula %p ancorata dopo lo sgancio: %ld,%ld %dx%d", (void*)h, c.tasti.left, c.tasti.top, w, hh); scrivi(b); }
    }

    void sospendi(bool si) { sospese = si; if (si) for (auto& [h, c] : capsule) nascondi(c, "sospese"); else programma(); }

    void distrutta(HWND h) {
        auto it = capsule.find(h);
        // a meta' di aggiorna (arrivata mentre UI Automation aspettava un'altra app): si toglie alla fine
        if (dentroAggiorna) { if (it != capsule.end()) nascondi(it->second, "distrutta"); daDistruggere.push_back(h); return; }
        if (it != capsule.end()) { DestroyWindow(it->second.fin); capsule.erase(it); }
        ricordo[0].erase(h); ricordo[1].erase(h);
    }

    // ogni 500 ms (timer di FluidWin): rete di sicurezza, rimette a posto quello che fosse rimasto indietro
    void controllo() {
        aggiorna();
        controlloColore();
    }

    // Il colore della barra della finestra in primo piano si ricontrolla di continuo (ogni 50 ms, solo dallo schermo:
    // pochi pixel, l'app non se ne accorge). Cosi' il fondo segue la barra anche quando cambia senza nessun avviso
    // (dissolvenze di Windows, dialoghi che scuriscono la finestra, apertura)
    // Un clic dentro una finestra la riporta in cima, e cosi' sale sopra la sua capsula (si vedono i tasti originali).
    // All'avviso di Windows (cambio d'ordine) e a ogni controllo la capsula della finestra in primo piano torna
    // subito sopra di lei
    void riordina() {
        if (spenta || sospese) return;
        HWND h = GetForegroundWindow();
        auto it = capsule.find(h);
        if (it == capsule.end()) return;
        Capsula& c = it->second;
        if (!c.misurata || !c.visibile || c.segue || c.volo || c.trascinando) return;
        HWND sopraDi = GetWindow(h, GW_HWNDPREV);
        if (sopraDi == c.fin) return;
        const bool inCima = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        SetWindowPos(c.fin, sopraDi ? sopraDi : (inCima ? HWND_TOPMOST : HWND_TOP), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (registro) { char b[80]; snprintf(b, 80, "capsula %p rimessa sopra la sua finestra", (void*)h); scrivi(b); }
    }

    void controlloColore() {
        if (spenta || sospese) return;
        const double t0 = adessoMs();
        struct Fine { Motore* m; double t0; ~Fine() { const double d = adessoMs() - t0; if (d > 8 && m->registro) { char b[80]; snprintf(b, 80, "capsule: controllo del colore %.1f ms", d); m->scrivi(b); } } } fine{this, t0};
        riordina();
        auto it = capsule.find(GetForegroundWindow());
        if (it == capsule.end()) return;
        Capsula& c = it->second;
        if (!c.misurata || !c.visibile || c.segue || c.volo || c.trascinando) return;
        const COLORREF prima = c.fondo; const auto colPrima = c.colonna;
        leggiColore(c, false, true);
        if (c.fondo != prima || c.colonna != colPrima) {
            disegna(c);
            if (registro) { char b[96]; snprintf(b, 96, "capsula %p: colore della barra corretto", (void*)c.bersaglio); scrivi(b); }
        }
    }

private:
    struct Sonda;
    struct Capsula {
        std::shared_ptr<Sonda> sonda;   // misura in corso (o finita e non ancora usata) nel thread della sonda
        HWND fin = nullptr, bersaglio = nullptr;
        RECT tasti{}, ultimoVisibile{};   // ultimoVisibile = GetWindowRect quando si sono misurati i tasti
        float bordoDx = 0, bordoSu = 0;   // distanza dei tasti dal bordo destro e da quello alto della parte visibile, alla misura
        // in coordinate della capsula: raggio dei cerchi, loro centro verticale, bordo sinistro e basso dei tasti originali
        float r = 10, cy = 16, oL = 0, oB = 32;
        bool zoom = false;
        RECT inset{};   // GetWindowRect - rettangolo visibile, a finestra normale
        std::vector<UINT> colonna;   // la barra subito a sinistra della capsula, riga per riga (fondo senza aloni)
        bool insetOk = false;        // inset letto almeno una volta (per le finestre senza bordi invisibili vale 0,0,0,0)
        ULONGLONG verificaAlle = 0;  // misurata con i bordi ricordati: a quest'ora si ricontrolla con quelli veri
        // tasti trovati con UI Automation: posizione rispetto all'angolo in alto a destra, riusata finche' la finestra
        // non cambia fra ingrandita e normale (cosi' l'accessibilita' si interroga una volta sola)
        bool viaUia = false, uiaZoom = false;
        // tasti trovati con l'hit test (barre disegnate dall'app): posizione rispetto all'angolo, per verificarla al volo
        bool viaColpi = false, colpiZoom = false;
        int colpiDx = 0, colpiDy = 0, colpiW = 0, colpiH = 0;
        int uiaDx = 0, uiaDy = 0, uiaW = 0, uiaH = 0, tentativiUia = 0;
        int riprove = 0;   // misure fallite di fila con la finestra ferma
        ULONGLONG prossimoColore = 0;   // prima di allora niente nuova lettura del colore (riletture distanziate)
        ULONGLONG coloreDopo = 0;       // valgono solo le letture dello schermo con un numero piu' alto (non quelle di prima di un cambio)
        COLORREF fondo = RGB(240, 240, 240);
        bool coloreVero = false, provata = false, misurata = false, visibile = false, trascinando = false, attiva = false, mouseDentro = false;
        int sopra = -1, premuto = -1;   // 0 riduci, 1 ingrandisci, 2 chiudi
        int campioni = 0;               // letture del colore riuscite (le prime possono cadere sulla dissolvenza di apertura)
        ULONGLONG forzata = 0;          // fino a quando mostrarla anche se la finestra e' ancora trasparente
        RECT insetN{}; bool insetNOk = false;   // inset misurato l'ultima volta a finestra NON ingrandita
        bool segue = false;             // la sposta FluidWin con la copia animata (ingrandimento/ripristino)
        int modo = 0;                   // cosa disegnare: 0 tutto, 1 solo la pillola, 2 solo il fondo sopra i tasti originali
        bool seguiIniz = false;         // segui gia' partito: misure dello stato nuovo adottate, scarto di prima ricordato
        LONG barraPixel[2] = {0, 0};    // fondo della barra visto dai pixel (dall'alto della finestra), da normale [0] e ingrandita [1]
        ULONGLONG nata = 0;             // quando FluidWin ha visto la finestra la prima volta (animazione di apertura)
        float pdxV = 0, pdyV = 0;       // pillola rispetto all'angolo della finestra, nello stato di prima
        bool volo = false;              // sta seguendo l'animazione di ingrandimento/ripristino, da 'da' a tasti
        POINT da{};
        ULONGLONG voloT0 = 0;
    };
    static const int DURATA_VOLO = 220;   // ms: circa quanto dura l'animazione di Windows

    static Motore*& gIstanza() { static Motore* m = nullptr; return m; }

    HINSTANCE istanza = nullptr;
    HWND tempi = nullptr;
    ULONGLONG previsto = 0;   // quando e' previsto il prossimo controllo (0 = nessuno)
    bool sospese = false, spenta = false;
    std::unordered_map<HWND, Capsula> capsule;
    // ultima misura riuscita di ogni finestra da normale [0] e da ingrandita [1]: all'ingrandimento la capsula sa gia'
    // dove andra' a stare
    std::unordered_map<HWND, Capsula> ricordo[2];
    void ricorda(const Capsula& c) { if (c.misurata || c.coloreVero) ricordo[c.zoom ? 1 : 0][c.bersaglio] = c; }

    // altezza della barra (dai pixel) per app (eseguibile + classe), da normale [0] e ingrandita [1]: una finestra
    // nuova della stessa app (un'altra finestra di Brave) sa gia' dove centrare la capsula, prima di poter leggere i pixel
    std::unordered_map<std::wstring, LONG> barraApp[2];
    static std::wstring chiaveApp(HWND h) {
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        std::wstring exe;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t b[MAX_PATH]; DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(p, 0, b, &n)) exe = b;
            CloseHandle(p);
        }
        if (exe.empty()) return L"";
        wchar_t k[64] = L""; GetClassNameW(h, k, 64);
        return exe + L"|" + k;
    }

    // misure (non posizione, non stato) prese da un ricordo
    static void adotta(Capsula& c, const Capsula& m) {
        const int w = m.tasti.right - m.tasti.left, hh = m.tasti.bottom - m.tasti.top;
        c.tasti.right = c.tasti.left + w; c.tasti.bottom = c.tasti.top + hh;
        c.bordoDx = m.bordoDx; c.bordoSu = m.bordoSu; c.r = m.r; c.cy = m.cy; c.oL = m.oL; c.oB = m.oB; c.zoom = m.zoom;
        c.colonna = m.colonna; c.fondo = m.fondo; c.coloreVero = m.coloreVero;
    }

    // dove sta la pillola rispetto all'angolo in alto a destra della parte visibile della finestra (dx dal bordo destro,
    // dy dal bordo alto), e il suo bordo destro/alto dentro la finestra della capsula
    void scartoPillola(const Capsula& c, float& dx, float& dy, float& capR, float& capT) const {
        const float W = (float)(c.tasti.right - c.tasti.left), Hh = (float)(c.tasti.bottom - c.tasti.top);
        float cx[3], r; geometria(c, W, Hh, cx, r);
        capR = cx[2] + r * 1.55f; capT = c.cy - r * 1.45f;
        dx = c.bordoDx + (W - capR); dy = c.bordoSu + capT;
    }
    ID2D1Factory* f2d = nullptr;
    IUIAutomation* uia = nullptr;

    // Tasti disegnati dentro la pagina (Discord e altre app Electron): per Windows sono semplice contenuto, li
    // vede solo l'accessibilita'. Da destra verso sinistra, i primi tre pulsanti contigui nell'angolo in alto.
    // Solo per le finestre Chromium: alla prima domanda Chromium accende l'accessibilita' e risponde dalla seconda.
    // Scala della finestra (1 al 100%, 1.5 al 150%...): tutte le distanze in pixel delle misure sono pensate al 100% e
    // si moltiplicano per questa. Senza, al 150% la barra del titolo e i tasti (piu' alti e larghi) uscivano dalle
    // zone cercate e la capsula finiva nel posto sbagliato
    static float scalaDi(HWND h) { return scala::di(h); }
    static int P(float sc, int n) { return (int)std::lround(n * sc); }

    // Le misure dei tasti chiedono molte cose all'app (barra del titolo, decine di hit test, accessibilita'): ogni
    // domanda aspetta che l'app risponda, fino a 30-40 ms se e' occupata. Fatte sul thread principale fermavano tutto
    // FluidWin (misure da 18-76 ms nel registro: animazioni e gelatina a scatti). Le fa un thread a parte, la "sonda",
    // su una copia di quello che serve; il risultato torna al thread principale con un messaggio, e li' si finisce la
    // misura (pixel della barra, colore, forma della capsula).
    struct Sonda {
        HWND h = nullptr; RECT v{}, fr{}; float sc = 1; bool zoom = false;
        bool verifica = false;   // controllo a finestra ferma (il risultato si confronta, non si adotta direttamente)
        // ricordi della capsula: entrano e, aggiornati, tornano indietro
        bool viaUia = false, uiaZoom = false, viaColpi = false, colpiZoom = false;
        int colpiDx = 0, colpiDy = 0, colpiW = 0, colpiH = 0, uiaDx = 0, uiaDy = 0, uiaW = 0, uiaH = 0, tentativiUia = 0;
        // risultato
        bool ok = false; RECT t{}; LONG barra = 0; int colpi = 0; double ms = 0; char scarto[140] = "";
        // la striscia in alto a destra della finestra letta dallo schermo dalla sonda: i pixel della barra si prendono
        // da qui invece di un BitBlt sul thread principale (aspetta il compositore, ~6 ms)
        std::vector<UINT> banda; RECT bandaR{}; bool bandaOk = false;
        std::atomic<bool> fatta{false};
    };
    static void prendiRicordi(Sonda& s, const Capsula& c) {
        s.viaUia = c.viaUia; s.uiaZoom = c.uiaZoom; s.viaColpi = c.viaColpi; s.colpiZoom = c.colpiZoom;
        s.colpiDx = c.colpiDx; s.colpiDy = c.colpiDy; s.colpiW = c.colpiW; s.colpiH = c.colpiH;
        s.uiaDx = c.uiaDx; s.uiaDy = c.uiaDy; s.uiaW = c.uiaW; s.uiaH = c.uiaH; s.tentativiUia = c.tentativiUia;
    }
    static void rendiRicordi(Capsula& c, const Sonda& s) {
        c.viaUia = s.viaUia; c.uiaZoom = s.uiaZoom; c.viaColpi = s.viaColpi; c.colpiZoom = s.colpiZoom;
        c.colpiDx = s.colpiDx; c.colpiDy = s.colpiDy; c.colpiW = s.colpiW; c.colpiH = s.colpiH;
        c.uiaDx = s.uiaDx; c.uiaDy = s.uiaDy; c.uiaW = s.uiaW; c.uiaH = s.uiaH; c.tentativiUia = s.tentativiUia;
    }

    // UI Automation: CLSID_CUIAutomation / IID_IUIAutomation (scritti qui: non tutte le librerie di MinGW li hanno)
    static IUIAutomation* creaUia() {
        static const GUID clsid = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
        static const GUID iid = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
        IUIAutomation* u = nullptr;
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, (void**)&u))) u = nullptr;
        return u;
    }

    // Il thread della sonda: uno solo, sempre lo stesso, con il suo UI Automation in un appartamento multithread (come
    // consiglia Microsoft per UIA: il thread principale non resta piu' fermo ad aspettare l'accessibilita' di Discord).
    // Le misure in coda si fanno una dopo l'altra; finita una, un messaggio al thread principale
    struct CodaSonde {
        std::mutex mx; std::condition_variable cv;
        std::deque<std::shared_ptr<Sonda>> lavori;
        HWND avvisa = nullptr;
    };
    CodaSonde* coda = nullptr;   // mai liberata: il thread vive quanto FluidWin

    void avviaSonde() {
        coda = new CodaSonde; coda->avvisa = tempi;
        CodaSonde* q = coda;
        std::thread([q] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // le capsule compaiono presto
            IUIAutomation* u = creaUia();
            for (;;) {
                std::shared_ptr<Sonda> s;
                {
                    std::unique_lock<std::mutex> lk(q->mx);
                    q->cv.wait(lk, [q] { return !q->lavori.empty(); });
                    s = std::move(q->lavori.front()); q->lavori.pop_front();
                }
                if (IsWindow(s->h)) sonda(*s, u);
                s->fatta.store(true, std::memory_order_release);
                PostMessageW(q->avvisa, WM_APP + 20, 0, 0);
            }
        }).detach();
    }

    // chiede alla sonda di misurare la capsula c (verifica: controllo a finestra ferma, su una copia senza "forzata").
    // false: nessuna sonda (allora si misura qui)
    bool chiediSonda(Capsula& c, bool verifica) {
        if (!coda) return false;
        auto s = std::make_shared<Sonda>();
        if (verifica) { Capsula prova = c; prova.forzata = 0; prepara(prova, *s); }
        else prepara(c, *s);
        s->verifica = verifica;
        c.sonda = s;
        { std::lock_guard<std::mutex> lk(coda->mx); coda->lavori.push_back(std::move(s)); }
        coda->cv.notify_one();
        return true;
    }

    // la misura della sonda di c, se e' finita e del tipo giusto (altrimenti nullptr; una del tipo sbagliato si butta)
    std::shared_ptr<Sonda> sondaFinita(Capsula& c, bool verifica) {
        if (!c.sonda || !c.sonda->fatta.load(std::memory_order_acquire)) return nullptr;
        std::shared_ptr<Sonda> s = std::move(c.sonda);
        return s->verifica == verifica ? s : nullptr;
    }

    static bool tastiUia(Sonda& s, IUIAutomation* uia, const RECT& v, RECT& t) {
        if (!uia || s.tentativiUia >= 4) return false;
        wchar_t cl[64] = L""; GetClassNameW(s.h, cl, 64);
        if (wcscmp(cl, L"Chrome_WidgetWin_1") || IsHungAppWindow(s.h)) return false;
        s.tentativiUia++;
        const float sc = s.sc;
        RECT tt{}; int n = 0, x = v.right - P(sc, 4), ultimo = x;
        const int y = v.top + P(sc, 12);
        while (n < 3 && x > v.right - P(sc, 300)) {
            const POINT p = {x, y};
            HWND sotto = WindowFromPoint(p);
            if (!sotto || GetAncestor(sotto, GA_ROOT) != s.h) return false;   // angolo coperto da un'altra finestra
            IUIAutomationElement* e = nullptr;
            if (FAILED(uia->ElementFromPoint(p, &e)) || !e) return false;
            CONTROLTYPEID tipo = 0; RECT r{};
            e->get_CurrentControlType(&tipo); e->get_CurrentBoundingRectangle(&r); e->Release();
            const int w = r.right - r.left, hh = r.bottom - r.top;
            if (tipo == UIA_ButtonControlTypeId && PtInRect(&r, p) && w >= P(sc, 16) && w <= P(sc, 90) && hh >= P(sc, 16) && hh <= P(sc, 70) &&
                (n == 0 || std::abs(hh - (tt.bottom - tt.top)) <= P(sc, 3))) {
                if (n == 0) tt = r; else UnionRect(&tt, &tt, &r);
                n++; x = ultimo = r.left - 1;
            } else if ((n == 0 && x > v.right - P(sc, 40)) || (n > 0 && ultimo - x < P(sc, 12))) x -= 3;   // margine o spazio fra due tasti
            else break;
        }
        if (n < 3) return false;
        t = tt; s.viaUia = true; s.tentativiUia = 0;
        s.uiaZoom = s.zoom;
        s.uiaDx = v.right - tt.right; s.uiaDy = tt.top - v.top; s.uiaW = tt.right - tt.left; s.uiaH = tt.bottom - tt.top;
        return true;
    }

    void scrivi(const char* s) const { if (registro) registro(s); }

    static RECT rettVisibile(HWND h) {
        RECT v{}; if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &v, sizeof(v)))) GetWindowRect(h, &v);
        return v;
    }

    static inline thread_local int colpiFatti = 0;   // hit test chiesti da questo thread (per il registro: quanto costa una misura)
    static double adessoMs() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }

    static bool tastoColpo(LRESULT r) { return r == HTCLOSE || r == HTMAXBUTTON || r == HTMINBUTTON; }

    // I tasti trovati l'ultima volta (stessa distanza dall'angolo in alto a destra) ci sono ancora? Si guardano il
    // chiudi, il riduci e un punto appena a sinistra (non deve essere un tasto): 3-4 hit test invece di ~70
    static bool tastiAncoraLi(const Sonda& c, HWND h, const RECT& v, RECT& t) {
        const float sc = c.sc;
        const RECT r = {v.right - c.colpiDx - c.colpiW, v.top + c.colpiDy, v.right - c.colpiDx, v.top + c.colpiDy + c.colpiH};
        if (r.left <= v.left || r.right - r.left < P(sc, 40)) return false;
        const int y = v.top + P(sc, 10);
        const LRESULT destra = colpo(h, r.right - P(sc, 6), y), sinistra = colpo(h, r.left + P(sc, 4), y);
        if (destra != HTCLOSE || !tastoColpo(sinistra)) return false;
        if (tastoColpo(colpo(h, r.left - P(sc, 30), y))) return false;   // c'e' un tasto in piu' a sinistra: misura completa
        if (!tastoColpo(colpo(h, r.right - P(sc, 14), r.bottom - P(sc, 3)))) return false;
        t = r;
        return true;
    }

    static LRESULT colpo(HWND h, int x, int y) {
        colpiFatti++;
        DWORD_PTR r = HTNOWHERE;
        if (!SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM(x, y), SMTO_ABORTIFHUNG | SMTO_BLOCK, 30, &r)) return -1;
        return (LRESULT)r;
    }

    static bool temaScuro() {
        DWORD v = 1, n = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
        return v == 0;
    }

    void nascondi(Capsula& c, const char* motivo = "") {
        if (c.visibile && c.mouseDentro) sugg.nascondi();
        if (c.visibile) {
            ShowWindow(c.fin, SW_HIDE); c.visibile = false;
            if (registro) { char b[96]; snprintf(b, 96, "capsula %p nascosta (%s)", (void*)c.bersaglio, motivo); scrivi(b); }
        }
        c.sopra = c.premuto = -1;
    }

    // finestra da coprire con la capsula adesso?
    // Contrasto elevato di Windows acceso: la capsula (vetro, colori suoi) non lo rispetterebbe, restano i tasti
    // originali. Letto al massimo ogni 2 s
    static bool contrastoElevato() {
        static ULONGLONG letto = 0; static bool si = false;
        const ULONGLONG ora = GetTickCount64();
        if (ora - letto > 2000) {
            HIGHCONTRASTW hc{}; hc.cbSize = sizeof(hc);
            si = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
            letto = ora;
        }
        return si;
    }

    bool daMostrare(HWND h) const {
        if (sospese || spenta || contrastoElevato() || !IsWindowVisible(h) || IsIconic(h) || (idonea && !idonea(h))) return false;
        DWORD cl = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof(cl));
        if (cl) return false;
        auto it = capsule.find(h);
        if (it != capsule.end() && it->second.forzata > GetTickCount64()) return true;   // sta ricomparendo: la capsula c'e' gia'
        // trasparente in questo momento (genio, gelatina, apertura): niente capsula
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) {
            BYTE a = 255; DWORD fl = 0; COLORREF k;
            if (GetLayeredWindowAttributes(h, &k, &a, &fl) && (fl & LWA_ALPHA) && a < 250) return false;
        }
        return true;
    }

    // Prima parte della misura (thread principale): rettangolo visibile, scala, stato e ricordi della capsula
    void prepara(Capsula& c, Sonda& s) {
        HWND h = c.bersaglio;
        // appena tornata visibile (fine di gelatina o genio) DWM da' ancora il rettangolo di quando era trasparente:
        // si usano i bordi letti l'ultima volta a finestra normale
        RECT v = rettVisibile(h), fr; GetWindowRect(h, &fr);
        if (c.forzata > GetTickCount64() && c.insetOk) {
            v = {fr.left + c.inset.left, fr.top + c.inset.top, fr.right - c.inset.right, fr.bottom - c.inset.bottom};
            c.verificaAlle = GetTickCount64() + 1800;   // quando DWM si e' assestato, un controllo con i bordi veri
        } else {
            c.inset = {v.left - fr.left, v.top - fr.top, fr.right - v.right, fr.bottom - v.bottom}; c.insetOk = true;
            if (!IsZoomed(h)) { c.insetN = c.inset; c.insetNOk = true; }
        }
        s.h = h; s.v = v; s.fr = fr; s.sc = scalaDi(h); s.zoom = IsZoomed(h) != FALSE;
        prendiRicordi(s, c);
    }

    // Seconda parte (qualsiasi thread, di solito la sonda): tutto quello che si chiede all'app. Dove sono i tre tasti,
    // fin dove arrivano in basso e dove finisce la barra del titolo per l'hit test. Usa solo s e uia
    static bool sonda(Sonda& s, IUIAutomation* uia) {
        const double t0 = adessoMs(); colpiFatti = 0;
        const bool ok = sondaDentro(s, uia);
        if (ok) leggiBanda(s);
        s.colpi = colpiFatti; s.ms = adessoMs() - t0; s.ok = ok;
        return ok;
    }

    // la parte in alto a destra della finestra com'e' sullo schermo: fin dove arrivano le letture della barra
    // (fondoBarra e il colore, a sinistra dei tasti) per qualsiasi app, alla scala della finestra
    static void leggiBanda(Sonda& s) {
        const RECT v = s.v;
        const RECT r = {std::max(v.left, v.right - P(s.sc, 400)), v.top, v.right, std::min(v.bottom, v.top + P(s.sc, 130))};
        const int w = r.right - r.left, hh = r.bottom - r.top;
        if (w <= 0 || hh <= 0) return;
        HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -hh, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr; HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            HGDIOBJ o = SelectObject(mem, dib);
            if (BitBlt(mem, 0, 0, w, hh, scr, r.left, r.top, SRCCOPY)) {
                GdiFlush();
                s.banda.assign((const UINT*)bits, (const UINT*)bits + (size_t)w * hh);
                s.bandaR = r; s.bandaOk = true;
            }
            SelectObject(mem, o);
        }
        if (dib) DeleteObject(dib);
        DeleteDC(mem); ReleaseDC(nullptr, scr);
    }

    // durante applica: la striscia della sonda, da cui pixelGrezzi prende i pixel dello schermo se ci stanno
    const Sonda* bandaUsa = nullptr;
    static bool sondaDentro(Sonda& s, IUIAutomation* uia) {
        const HWND h = s.h; const RECT v = s.v; const float sc = s.sc;
        TITLEBARINFOEX tb{}; tb.cbSize = sizeof(tb);
        DWORD_PTR ok = 0;
        RECT t{};
        // misura accettabile: dentro la finestra e con le dimensioni di tre tasti
        auto valida = [&](RECT r) {
            IntersectRect(&r, &r, &v);
            const int w = r.right - r.left, hh = r.bottom - r.top;
            return w >= P(sc, 40) && hh >= P(sc, 18) && hh <= P(sc, 90) && w <= P(sc, 400);
        };
        bool classica = false;
        if (SendMessageTimeoutW(h, WM_GETTITLEBARINFOEX, 0, (LPARAM)&tb, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &ok) &&
            tb.rgrect[5].right > tb.rgrect[5].left && !(tb.rgstate[5] & STATE_SYSTEM_INVISIBLE)) {
            t = tb.rgrect[5];
            for (int i : {2, 3}) if (tb.rgrect[i].right > tb.rgrect[i].left && !(tb.rgstate[i] & STATE_SYSTEM_INVISIBLE)) UnionRect(&t, &t, &tb.rgrect[i]);
            // Chromium ingrandito risponde con i tasti di DWM, che non disegna: se non tornano si prova l'hit test
            classica = valida(t);
        }
        if (classica) {
        } else if (s.viaUia && s.uiaZoom == s.zoom) {
            t = {v.right - s.uiaDx - s.uiaW, v.top + s.uiaDy, v.right - s.uiaDx, v.top + s.uiaDy + s.uiaH};
        } else if (s.viaColpi && s.colpiZoom == s.zoom && tastiAncoraLi(s, h, v, t)) {
            // tasti gia' trovati con l'hit test in questo stato: stanno ancora li' (verificato con pochi hit test)
        } else {
            // barra del titolo disegnata dall'app: da destra verso sinistra, dove iniziano e finiscono i tasti
            const int y = v.top + P(sc, 10);
            // (le coordinate possono essere negative: monitor a sinistra o sopra quello principale)
            // (passi di 3 px a qualsiasi scala: i tasti sono piu' larghi, il numero di hit test cresce col 150% ma
            // un passo piu' lungo rischierebbe di saltare il margine fra due tasti)
            int destra = 0, sinistra = 0, vuoto = 0;
            bool trovato = false, chiudi = false;
            for (int x = v.right - 3; x > v.right - P(sc, 320) && x > v.left; x -= 3) {
                const LRESULT r = colpo(h, x, y);
                if (r < 0) return false;
                if (r == HTCLOSE || r == HTMAXBUTTON || r == HTMINBUTTON) {
                    if (!trovato) { destra = x; trovato = true; }
                    sinistra = x; vuoto = 0; chiudi |= r == HTCLOSE;
                } else if (trovato && (vuoto += 3) > P(sc, 24)) break;
            }
            if (chiudi) {
                int basso = y;
                for (int yy = y; yy < v.top + P(sc, 80); yy += 2) {
                    const LRESULT r = colpo(h, destra - P(sc, 8), yy);
                    if (r != HTCLOSE && r != HTMAXBUTTON && r != HTMINBUTTON) break;
                    basso = yy;
                }
                t = {sinistra - 1, v.top + 1, destra + 3, basso + 2};   // il passo era di 2 px: l'ultimo tratto e' ancora tasto
                // si ricorda dove stanno rispetto all'angolo in alto a destra: la prossima volta basta verificarlo
                s.viaColpi = true; s.colpiZoom = s.zoom;
                s.colpiDx = v.right - t.right; s.colpiDy = t.top - v.top; s.colpiW = t.right - t.left; s.colpiH = t.bottom - t.top;
            } else if (!tastiUia(s, uia, v, t)) return false;
        }
        // il tasto chiudi vero puo' essere piu' alto di quanto dice l'app (o di quanto si e' misurato): se sotto la
        // capsula resta un pezzo di tasto, col mouse sopra diventa rosso. Si scende finche' l'hit test dice "tasto"
        for (int yy = t.bottom, k = 0; k < P(sc, 12); yy++, k++) {
            const LRESULT r = colpo(h, t.right - P(sc, 8), yy);
            if (r != HTCLOSE && r != HTMAXBUTTON && r != HTMINBUTTON) break;
            t.bottom = yy + 1;
        }
        IntersectRect(&t, &t, &v);   // la capsula non esce mai dalla finestra
        const int w = t.right - t.left, hh = t.bottom - t.top;
        if (w < P(sc, 40) || hh < P(sc, 18) || hh > P(sc, 90) || w > P(sc, 400)) {
            snprintf(s.scarto, sizeof(s.scarto), "capsula %p: misura scartata %dx%d (finestra %ld,%ld-%ld,%ld)", (void*)h, w, hh, v.left, v.top, v.right, v.bottom);
            return false;
        }
        // centro: quello della barra del titolo vera. Alcune app hanno i tasti piu' bassi della barra (allineati in
        // alto): centrata su di loro la capsula restava in alto. Si scende appena a sinistra dei tasti finche' l'hit
        // test dice "barra del titolo"
        LONG barra = t.bottom;
        for (int yy = t.bottom; yy < v.top + P(sc, 90); yy += 2) { if (colpo(h, t.left - P(sc, 6), yy) != HTCAPTION) break; barra = yy + 2; }
        s.t = t; s.barra = barra;
        return true;
    }

    // Misura completa sul thread principale (per quando serve subito: finestra staccata da ingrandita, la foto della
    // gelatina ha bisogno della capsula adesso)
    bool misura(Capsula& c) {
        Sonda s;
        prepara(c, s);
        sonda(s, uia);
        return applica(c, s);
    }

    // Terza parte (thread principale): dai tasti trovati alla capsula. Pixel della barra, centro, forma, colore
    bool applica(Capsula& c, const Sonda& s) {
        struct Usa { const Sonda*& p; Usa(const Sonda*& q, const Sonda* s) : p(q) { p = s; } ~Usa() { p = nullptr; } } usa(bandaUsa, &s);
        rendiRicordi(c, s);
        if (!s.ok) { if (s.scarto[0] && registro) scrivi(s.scarto); return false; }
        HWND h = c.bersaglio;
        const RECT v = s.v, t = s.t;
        const float sc = s.sc;
        LONG barra = s.barra;
        // La capsula ha sempre la stessa misura (non quella dei tasti dell'app, che cambia da app ad app e fra
        // finestra ingrandita e normale): la sua finestra copre i tasti originali e quanto serve in piu' per lei.
        // Le parti non disegnate sono trasparenti anche al mouse
        c.r = raggioTasto * sc;
        const int pw = (int)std::ceil(c.r * 9.3f), ph = (int)std::ceil(c.r * 2.9f);
        c.zoom = IsZoomed(h);
        const int ra = c.zoom ? 0 : (int)std::ceil(raggioAngoli * sc);
        // barre che all'hit test sono "contenuto" (Esplora file e altre app moderne): il bordo inferiore lo si vede dai
        // pixel, dove il colore della barra cambia (due strisce uguali: niente icone di mezzo)
        // e barre che all'hit test continuano un po' sotto a quella che si vede (Brave: 46 px contro 40): vince il pixel
        // (se l'altezza della barra in questo stato e' gia' nota, niente PrintWindow: con una sovrapposizione nuova
        // sopra, per esempio quella di Windows mentre si trascina una finestra, ne servirebbe uno ogni volta)
        const int st = IsZoomed(h) ? 1 : 0;
        // Finestra appena aperta: Windows la sta ancora facendo comparire (animazione di apertura, contenuto non ancora
        // disegnato) e i pixel letti adesso sono sbagliati (barra di 35-86 px invece di 31 in 17 prime misure su 41).
        // Nei primi 450 ms niente pixel: vale l'altezza gia' vista per la stessa app, se no l'hit test; a 500 ms una
        // verifica con i pixel veri (sposta la capsula solo se serve)
        const ULONGLONG adesso = GetTickCount64();
        const bool appenaNata = c.nata && adesso - c.nata < 450;
        const std::wstring app = chiaveApp(h);
        LONG pixel = appenaNata ? 0 : fondoBarra(h, t.left - 3, t.top, t.bottom, v.top + P(sc, 90), c.barraPixel[st] == 0);
        if (appenaNata && !c.verificaAlle) { c.verificaAlle = c.nata + 500; programma((UINT)(c.verificaAlle - adesso + 30)); }
        // finestra trasparente (fine del genio o della gelatina) o coperta: i pixel non si leggono, vale quello trovato
        // l'ultima volta nello stesso stato (senza, la capsula si centrava sull'hit test e poi scattava)
        if (pixel > 0) { c.barraPixel[st] = pixel - v.top; if (!app.empty()) barraApp[st][app] = c.barraPixel[st]; }
        else if (c.barraPixel[st] > 0) pixel = v.top + c.barraPixel[st];
        else if (auto ba = barraApp[st].find(app); !app.empty() && ba != barraApp[st].end()) pixel = v.top + ba->second;
        if (barra <= t.bottom + 3) barra = std::max(barra, pixel);
        else if (pixel >= t.bottom - 1 && pixel < barra) barra = pixel;
        const LONG centro = barra > t.bottom + 3 ? (v.top + barra) / 2 : (t.top + t.bottom) / 2;
        if (registro) { char b[160]; snprintf(b, 160, "capsula %p: cima %ld tasti %ld-%ld barra %ld centro %ld (rel. %ld)", (void*)h, v.top, t.top - v.top, t.bottom - v.top, barra - v.top, centro - v.top, centro - v.top); scrivi(b); }
        RECT T = {std::min(t.left, t.right - pw - P(sc, 8) - ra), std::max(v.top, std::min(t.top, centro - ph / 2 - 1)), t.right, 0};
        c.cy = std::max((float)(centro - T.top), ph / 2.0f + 1);   // a ridosso del bordo alto: un po' piu' in basso
        T.bottom = std::max(t.bottom, T.top + (LONG)std::ceil(c.cy + ph / 2.0f) + 2);
        c.tasti = T;
        c.oL = (float)(t.left - T.left); c.oB = (float)(t.bottom - T.top);
        // il rettangolo visibile di DWM salta di qualche pixel quando la finestra diventa trasparente (gelatina,
        // genio): la forma della capsula si calcola da quello letto adesso, cosi' resta ferma
        c.bordoDx = (float)(v.right - T.right); c.bordoSu = (float)(T.top - v.top);
        GetWindowRect(h, &c.ultimoVisibile);
        // finestra appena ingrandita o ridimensionata: durante l'animazione di Windows il colore letto puo' essere
        // sbagliato (nero). Si tiene quello che aveva e lo si rilegge nei controlli successivi, a finestra ferma
        c.campioni = 0;
        if (!c.coloreVero) leggiColore(c);
        return true;
    }

    // Prima riga sotto 'da' (fino ad 'a') in cui la barra cambia colore, guardando due strisce verticali a x e x-8;
    // 'da' e' il basso dei tasti originali. 0 se non si vede (finestra coperta, icone, nessun cambio)
    // Sovrapposizioni trasparenti al mouse che stanno sopra a tutto (l'indicatore delle prestazioni di NVIDIA:
    // grande quanto lo schermo, nell'ordine risulta perfino dietro le finestre, ma si disegna sopra). Dove ci sono,
    // lo schermo non mostra solo la finestra
    // L'elenco delle sovrapposizioni si rilegge al massimo ogni mezzo secondo: scorrere tutte le finestre del
    // sistema costa, e i colori si controllano fino a 20 volte al secondo
    struct Sovrapposizione { HWND h; RECT r; };
    std::vector<Sovrapposizione> sovrapposte;
    ULONGLONG sovrapposteAlle = 0;
    // Sovrapposizioni gia' confrontate con PrintWindow sopra una barra: se lo schermo coincideva, per 5 minuti basta
    // lo schermo (prima si rifaceva il confronto a ogni misura, con un PrintWindow di tutta la finestra ogni volta);
    // se non coincideva si riprova fra 10 secondi
    struct Fiducia { RECT r; ULONGLONG fino; bool ok; };
    std::unordered_map<HWND, Fiducia> fiducie;
    LettoreSchermo lettore;

    HWND sovrapposta(HWND h, const RECT& r) {
        const ULONGLONG ora = GetTickCount64();
        if (ora - sovrapposteAlle > 500) {
            sovrapposteAlle = ora;
            sovrapposte.clear();
            EnumWindows([](HWND w, LPARAM l) -> BOOL {
                if (!IsWindowVisible(w)) return TRUE;
                const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
                if (!(ex & WS_EX_LAYERED) || !(ex & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE))) return TRUE;
                wchar_t k[64] = L""; GetClassNameW(w, k, 64);
                if (!wcsncmp(k, L"FluidWin.", 9)) return TRUE;
                BOOL nascosta = FALSE; DwmGetWindowAttribute(w, DWMWA_CLOAKED, &nascosta, sizeof(nascosta));
                RECT a; if (nascosta || !GetWindowRect(w, &a) || IsRectEmpty(&a)) return TRUE;
                ((std::vector<Sovrapposizione>*)l)->push_back({w, a});
                return TRUE;
            }, (LPARAM)&sovrapposte);
            for (auto it = fiducie.begin(); it != fiducie.end();) it = IsWindow(it->first) ? std::next(it) : fiducie.erase(it);
        }
        for (const auto& s : sovrapposte) { RECT x; if (s.h != h && IntersectRect(&x, &s.r, &r)) return s.h; }
        return nullptr;
    }

    // Pixel (BGRA) di un pezzo della finestra h, in coordinate schermo. Dallo schermo se li' si vede solo lei,
    // altrimenti chiesti alla finestra stessa (PrintWindow: la disegna intera, anche se coperta)
    // stampa = si puo' chiedere alla finestra con PrintWindow (la fa ridisegnare: solo quando serve davvero)
    // asincrono = dallo schermo con il lettore in sottofondo (l'ultima lettura, senza aspettare il compositore)
    bool pixel(HWND h, int x, int y, int w, int hh, std::vector<UINT>& out, bool stampa = true, bool asincrono = false, ULONGLONG eta = 600, bool avvisa = false, ULONGLONG dopo = 0) {
        if (w <= 0 || hh <= 0) return false;
        out.assign((size_t)w * hh, 0);
        const RECT r = {x, y, x + w, y + hh};
        HWND so = WindowFromPoint({x + w / 2, y + hh / 2});
        // finestra trasparente in questo momento (gelatina, genio, ingrandimento) o coperta da un'altra: niente
        // lettura, si riprova quando torna visibile (chiederla a lei la farebbe sfarfallare)
        BYTE alfa = 255; DWORD fl = 0; COLORREF ck;
        const bool trasparente = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) && GetLayeredWindowAttributes(h, &ck, &alfa, &fl) && (fl & LWA_ALPHA) && alfa < 250;
        if (trasparente || !so || GetAncestor(so, GA_ROOT) != h) return false;
        HWND sopra = sovrapposta(h, r);
        if (!sopra) return asincrono ? lettore.leggi(r, out, eta, avvisa, dopo) : pixelGrezzi(h, x, y, w, hh, out, true);
        RECT sr{}; GetWindowRect(sopra, &sr);
        auto fi = fiducie.find(sopra);
        const bool valida = fi != fiducie.end() && fi->second.fino > GetTickCount64() && EqualRect(&fi->second.r, &sr);
        if (valida && fi->second.ok) return asincrono ? lettore.leggi(r, out, eta, avvisa, dopo) : pixelGrezzi(h, x, y, w, hh, out, true);
        if (!stampa || asincrono) return false;   // (le letture in sottofondo non fanno mai PrintWindow: aspettano)
        if (!pixelGrezzi(h, x, y, w, hh, out, false)) return false;
        if (!valida) {
            std::vector<UINT> s;
            bool uguali = pixelGrezzi(h, x, y, w, hh, s, true);
            for (size_t i = 0; i < s.size() && uguali; i++)
                for (int k = 0; k < 24; k += 8) if (std::abs((int)((s[i] >> k) & 255) - (int)((out[i] >> k) & 255)) > 6) { uguali = false; break; }
            fiducie[sopra] = {sr, GetTickCount64() + (uguali ? 300000 : 10000), uguali};
            if (registro) { char b[120]; snprintf(b, 120, "capsula %p: sovrapposizione %p %s", (void*)h, (void*)sopra, uguali ? "lontana, basta lo schermo" : "sopra la barra"); scrivi(b); }
        }
        return true;
    }

    // dallo schermo (schermo = true) o dalla finestra con PrintWindow (con un limite di tempo)
    bool pixelGrezzi(HWND h, int x, int y, int w, int hh, std::vector<UINT>& out, bool schermo) {
        out.assign((size_t)w * hh, 0);
        RECT f; GetWindowRect(h, &f);
        const int W = schermo ? w : f.right - f.left, H = schermo ? hh : f.bottom - f.top;
        if (W <= 0 || H <= 0 || (!schermo && (x < f.left || y < f.top || x + w > f.right || y + hh > f.bottom))) return false;
        std::vector<UINT> buf;
        bool ok = false;
        // dalla striscia gia' letta dalla sonda, se il rettangolo ci sta (niente attesa del compositore qui)
        const RECT chiesto = {x, y, x + w, y + hh}; RECT dentro{};
        if (schermo && bandaUsa && bandaUsa->bandaOk && IntersectRect(&dentro, &chiesto, &bandaUsa->bandaR) && EqualRect(&dentro, &chiesto)) {
            const RECT& b = bandaUsa->bandaR; const int bw = b.right - b.left;
            buf.resize((size_t)w * hh);
            for (int yy = 0; yy < hh; yy++) memcpy(buf.data() + (size_t)yy * w, bandaUsa->banda.data() + (size_t)(y - b.top + yy) * bw + (x - b.left), (size_t)w * 4);
            ok = true;
        } else if (schermo) {
            HDC s = GetDC(nullptr), mem = CreateCompatibleDC(s);
            BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
            void* bits = nullptr; HBITMAP dib = CreateDIBSection(s, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (dib && bits) {
                HGDIOBJ o = SelectObject(mem, dib);
                ok = BitBlt(mem, 0, 0, w, hh, s, x, y, SRCCOPY);
                GdiFlush();
                if (ok) buf.assign((const UINT*)bits, (const UINT*)bits + (size_t)W * H);
                SelectObject(mem, o);
            }
            if (dib) DeleteObject(dib);
            DeleteDC(mem); ReleaseDC(nullptr, s);
        } else ok = stampa::finestra(h, W, H, buf);
        if (ok) {
            const int ox = schermo ? 0 : x - f.left, oy = schermo ? 0 : y - f.top;
            bool nero = true;
            for (int yy = 0; yy < hh; yy++) for (int xx = 0; xx < w; xx++) {
                const UINT p = buf[(size_t)(oy + yy) * W + ox + xx] & 0xFFFFFF;
                out[(size_t)yy * w + xx] = p; if (p) nero = false;
            }
            if (!schermo && nero) ok = false;   // alcune app non si lasciano disegnare: niente
        }
        if (registro && !schermo) { char b[96]; snprintf(b, 96, "capsula %p: pixel dalla finestra (%s)", (void*)h, ok ? "ok" : "niente"); scrivi(b); }
        return ok;
    }

    LONG fondoBarra(HWND h, int x, LONG cima, LONG da, LONG a, bool stampa) {
        if (a <= da) return 0;
        const int n = (int)(a - cima);
        std::vector<UINT> buf;
        LONG trovato = 0;
        if (pixel(h, x - 8, cima, 9, n, buf, stampa)) {
            const UINT* px = buf.data();
            auto diversi = [](UINT p, UINT q, int soglia) {
                for (int k = 0; k < 24; k += 8) if (std::abs((int)((p >> k) & 255) - (int)((q >> k) & 255)) > soglia) return true;
                return false;
            };
            const int r0 = std::max(0, (int)(da - cima) - 3);   // riga di riferimento: barra appena sopra il fondo dei tasti
            const UINT ref = px[r0 * 9 + 8];
            if (!diversi(ref, px[r0 * 9], 6))
                for (int y = r0 + 1; y < n; y++)
                    if (diversi(px[y * 9 + 8], ref, 8) && diversi(px[y * 9], ref, 8)) { trovato = cima + y; break; }
        }
        return trovato;
    }

    // colore della barra del titolo appena a sinistra dei tasti: con lui si coprono i tasti originali.
    // Dallo schermo se li' si vede solo la finestra, altrimenti chiesto a lei (vedi pixel()).
    // asincrono: l'ultima lettura del lettore in sottofondo (gia' c'e' un colore: aggiornarlo puo' aspettare 50 ms)
    void leggiColore(Capsula& c, bool stampa = true, bool asincrono = false, ULONGLONG eta = 600, bool avvisa = false, ULONGLONG dopo = 0) {
        // La striscia verticale della barra subito a sinistra della capsula: il fondo copia sfumature e righe della barra
        // (un colore solo faceva un alone). Si prendono due strisce a 8 px di distanza: se non coincidono li' c'e'
        // un'icona o un testo dell'app, e si resta al colore unico. Il colore unico e' quello della striscia
        // all'altezza del centro della capsula (fuori dalla sua finestra, che altrimenti si leggerebbe)
        const int hh = c.tasti.bottom - c.tasti.top, xs = c.tasti.left - 3;
        std::vector<UINT> px;
        if (hh > 0 && pixel(c.bersaglio, xs - 8, c.tasti.top, 9, hh, px, stampa, asincrono, eta, avvisa, dopo)) {
            bool uguali = true;
            for (int y = 0; y < hh && uguali; y++) {
                const UINT a = px[y * 9 + 8], b = px[y * 9];
                for (int k = 0; k < 24; k += 8) if (std::abs((int)((a >> k) & 255) - (int)((b >> k) & 255)) > 10) { uguali = false; break; }
            }
            if (uguali) { c.colonna.resize(hh); for (int y = 0; y < hh; y++) c.colonna[y] = px[y * 9 + 8] | 0xFF000000u; }
            else c.colonna.clear();
            const UINT k = px[std::clamp((int)c.cy, 0, hh - 1) * 9 + 8];
            c.fondo = RGB((k >> 16) & 255, (k >> 8) & 255, k & 255); c.coloreVero = true; c.campioni++;
            return;
        }
        if (!c.coloreVero) c.fondo = temaScuro() ? RGB(32, 32, 32) : RGB(243, 243, 243);
    }

    // Le chiamate di UI Automation (misura dei tasti di Discord e simili) vanno a un altro processo, e mentre si
    // aspetta il thread smista i messaggi: un evento puo' chiedere di nuovo aggiorna, o distruggere una capsula, a
    // meta' di questa. aggiorna non rientra (si riprova fra poco) e le distruzioni aspettano la fine
    int dentroAggiorna = 0;
    std::vector<HWND> daDistruggere;

    int contaMisure = 0, contaColori = 0, contaFinestre = 0;   // diagnostica dell'ultimo aggiorna
    double tDisegno = 0, tPos = 0, tColore = 0, tApplica = 0, tScelta = 0;   // ... e dove e' andato il tempo (ms)

    void aggiorna() {
        if (dentroAggiorna) { programma(30); return; }
        dentroAggiorna++;
        const double t0 = adessoMs(); contaMisure = contaColori = contaFinestre = 0; tDisegno = tPos = tColore = tApplica = tScelta = 0;
        aggiornaTutte();
        const double d = adessoMs() - t0;
        if (d > 8 && registro) { char b[220]; snprintf(b, 220, "capsule: aggiorna %.1f ms (%d finestre, %d misure, %d letture del colore; disegno %.1f, posto %.1f, colore %.1f, misure %.1f, scelta %.1f)", d, contaFinestre, contaMisure, contaColori, tDisegno, tPos, tColore, tApplica, tScelta); scrivi(b); }
        dentroAggiorna--;
        std::vector<HWND> via; via.swap(daDistruggere);
        for (HWND h : via) distrutta(h);
    }

    void aggiornaTutte() {
        KillTimer(tempi, 1);
        previsto = 0;
        HWND fg = GetForegroundWindow();
        // dall'alto verso il basso: cosi' si sa anche dove mettere ogni capsula nell'ordine di sovrapposizione
        std::vector<HWND> viste;
        bool rileggi = false;
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
            const double tm0 = adessoMs(); const bool mostra = daMostrare(h); tScelta += adessoMs() - tm0;
            if (!mostra) continue;
            viste.push_back(h); contaFinestre++;
            auto it = capsule.find(h);
            if (it == capsule.end()) {
                Capsula n; n.bersaglio = h; n.nata = GetTickCount64();
                n.fin = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"FluidWin.Capsula", L"", WS_POPUP,
                                        0, 0, 1, 1, nullptr, nullptr, istanza, nullptr);
                if (!n.fin) continue;
                BOOL niente = TRUE; DwmSetWindowAttribute(n.fin, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
                SetWindowLongPtrW(n.fin, GWLP_USERDATA, (LONG_PTR)h);
                it = capsule.emplace(h, n).first;
            }
            Capsula& c = it->second;
            if (c.trascinando || c.volo || c.segue) continue;
            // rettangolo della finestra (non quello "visibile" di DWM, che salta di qualche pixel quando la finestra
            // diventa o smette di essere trasparente per gelatina e genio: farebbe sparire e ricomparire la capsula)
            RECT v; GetWindowRect(h, &v);
            const bool attiva = h == fg;
            bool ridisegna = attiva != c.attiva;
            c.attiva = attiva;
            // attiva <-> inattiva: Windows cambia il colore della barra con una dissolvenza; letto adesso sarebbe a meta'
            // (fondo grigio attorno alla capsula). Si rilegge ancora qualche volta, anche a dissolvenza finita
            if (ridisegna) { c.campioni = 0; rileggi = true; }
            // ricontrollo a finestra ferma (dopo una misura fatta coi bordi ricordati): se la capsula risulta fuori posto
            // la si sposta, altrimenti non si tocca (niente sfarfallio)
            if (c.verificaAlle && GetTickCount64() >= c.verificaAlle && c.misurata && c.provata && EqualRect(&v, &c.ultimoVisibile)) {
                // la misura la fa la sonda: qui si chiede, e al suo messaggio si confronta
                std::shared_ptr<Sonda> s = sondaFinita(c, true);
                if (!s && !c.sonda && !chiediSonda(c, true)) { s = std::make_shared<Sonda>(); Capsula p = c; p.forzata = 0; prepara(p, *s); sonda(*s, uia); }
                if (s) {
                    c.verificaAlle = 0;
                    Capsula prova = c; prova.forzata = 0;
                    if (EqualRect(&s->fr, &v) && applica(prova, *s) &&
                        (!EqualRect(&prova.tasti, &c.tasti) || prova.bordoDx != c.bordoDx || prova.bordoSu != c.bordoSu || prova.zoom != c.zoom)) {
                        c.tasti = prova.tasti; c.bordoDx = prova.bordoDx; c.bordoSu = prova.bordoSu; c.inset = prova.inset; c.insetOk = true;
                        c.r = prova.r; c.cy = prova.cy; c.oL = prova.oL; c.oB = prova.oB; c.zoom = prova.zoom;
                        ridisegna = true; c.visibile = false;   // si ridisegna e si rimette al posto giusto
                        if (registro) { char b[96]; snprintf(b, 96, "capsula %p corretta dopo il controllo", (void*)h); scrivi(b); }
                    }
                }
            } else if (c.verificaAlle && GetTickCount64() < c.verificaAlle) programma((UINT)(c.verificaAlle - GetTickCount64() + 30));
            if (!c.provata || !EqualRect(&v, &c.ultimoVisibile)) {
                if (!EqualRect(&v, &c.ultimoVisibile)) c.riprove = 0;
                nascondi(c, "rimisura");
                // la misura la fa la sonda in sottofondo: la prima volta si chiede e si passa alle altre finestre; al suo
                // messaggio (aggiorna di nuovo) si finisce qui. Se nel frattempo la finestra si e' mossa, si richiede
                if (c.sonda && !c.sonda->fatta.load(std::memory_order_acquire)) continue;   // in corso
                std::shared_ptr<Sonda> s = sondaFinita(c, false);
                if (s && !EqualRect(&s->fr, &v)) s = nullptr;
                if (!s) {
                    if (chiediSonda(c, false)) continue;
                    s = std::make_shared<Sonda>(); prepara(c, *s); sonda(*s, uia);   // (nessuna sonda: qui)
                }
                c.provata = true; contaMisure++;
                { const double ta0 = adessoMs(); c.misurata = applica(c, *s); tApplica += adessoMs() - ta0; }
                if (registro) { char b[120]; snprintf(b, 120, "capsula %p: misura con %d hit test in %.1f ms (in sottofondo)", (void*)h, s->colpi, s->ms); scrivi(b); }
                if (!c.misurata) {
                    c.ultimoVisibile = v;
                    if (registro) { char b[80]; snprintf(b, 80, "capsula %p: tasti non trovati", (void*)h); scrivi(b); }
                    // Subito dopo un ingrandimento (o con Chromium che ha appena acceso l'accessibilita') le misure
                    // possono non essere ancora pronte: si riprova qualche volta, poi solo se la finestra cambia
                    if (c.riprove < 4 || (c.tentativiUia > 0 && c.tentativiUia < 4)) { c.riprove++; c.provata = false; programma(500, true); }
                    continue;
                }
                ridisegna = true;
            } else if (c.campioni < 4 || ridisegna) {
                // il colore si rilegge piu' volte nei primi secondi e a ogni cambio di primo piano (il punto letto e'
                // fuori dalla capsula, quindi anche con la capsula visibile)
                const COLORREF prima = c.fondo;
                // Dallo schermo serve una lettura recente, a dissolvenza finita: la fa il lettore in sottofondo (un BitBlt
                // dallo schermo aspetta il compositore, ~6 ms l'uno: a ogni cambio di finestra attiva erano 12-18 ms
                // fermi sul thread principale). Quando ha letto avvisa, e qui si usa la lettura: solo se fatta dopo il
                // cambio (numero della lettura, non l'ora: il contatore di Windows va a scatti di 15 ms) e recente.
                // Riletture a 120 ms e poi ogni 250 ms: l'ultima e' a dissolvenza finita.
                // (La prima lettura di una capsula, senza ancora nessun colore vero, resta qui: puo' servire PrintWindow)
                const ULONGLONG ora = GetTickCount64();
                if (ridisegna) { c.prossimoColore = 0; c.coloreDopo = lettore.ultimo(); }
                if (!c.coloreVero) { const double tc0 = adessoMs(); leggiColore(c); tColore += adessoMs() - tc0; contaColori++; }
                else if (ora >= c.prossimoColore) {
                    const int letti = c.campioni;
                    leggiColore(c, false, true, 150, true, c.coloreDopo);
                    if (c.campioni > letti) {
                        // seconda rilettura presto (la dissolvenza di Windows dura ~200 ms), poi ogni 250 ms
                        const ULONGLONG passo = c.campioni == 1 ? 120 : 250;
                        contaColori++; c.prossimoColore = ora + passo; c.coloreDopo = lettore.ultimo(); if (c.campioni < 4) programma((UINT)passo + 10);
                    }
                } else programma((UINT)(c.prossimoColore - ora + 10));
                if (c.fondo != prima) ridisegna = true;
            }
            if (!c.misurata) continue;
            if (ridisegna || !c.visibile) { const double td0 = adessoMs(); disegna(c); tDisegno += adessoMs() - td0; }
            if (c.zoom == (IsZoomed(h) != FALSE) && !c.verificaAlle && c.forzata <= GetTickCount64()) ricorda(c);
            // subito sopra la sua finestra (sopra a quella che le sta davanti, se c'e', non va)
            const bool inCima = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            HWND sopraDi = GetWindow(h, GW_HWNDPREV);
            if (!c.visibile || sopraDi != c.fin) {
                // gia' al suo posto nell'ordine (anche da nascosta): solo mostrarla. "Dopo se stessa" Windows lo rifiuta
                const bool alPosto = sopraDi == c.fin;
                HWND dopo = alPosto ? nullptr : (sopraDi ? sopraDi : (inCima ? HWND_TOPMOST : HWND_TOP));
                const double tp0 = adessoMs();
                const BOOL ok = SetWindowPos(c.fin, dopo, c.tasti.left, c.tasti.top, c.tasti.right - c.tasti.left, c.tasti.bottom - c.tasti.top,
                                             SWP_NOACTIVATE | SWP_SHOWWINDOW | (alPosto ? SWP_NOZORDER : 0));
                tPos += adessoMs() - tp0;
                c.visibile = ok && IsWindowVisible(c.fin);
                if (!c.visibile && registro) { char b[80]; snprintf(b, 80, "capsula %p non mostrata (errore %lu)", (void*)h, GetLastError()); scrivi(b); }
            }
        }
        if (rileggi) programma(300);   // il colore della barra a dissolvenza finita
        // finestre sparite, ridotte o non piu' adatte
        for (auto it = capsule.begin(); it != capsule.end();) {
            if (std::find(viste.begin(), viste.end(), it->first) != viste.end() || it->second.segue) { ++it; continue; }
            if (!IsWindow(it->first)) { DestroyWindow(it->second.fin); it = capsule.erase(it); continue; }
            nascondi(it->second, "non da mostrare"); ++it;
        }
    }

    // tre cerchi: centri e raggio (coordinate della capsula), allineati a destra come i tasti originali
    // La capsula finisce 6 px prima del bordo della finestra; con angoli molto tondi si sposta a sinistra quanto
    // basta perche' la sua estremita' destra resti dentro la curva dell'angolo (concentrica, 3 px di distacco)
    void geometria(const Capsula& c, float W, float, float cx[3], float& r) const {
        r = c.r;
        const float passo = r * 3.1f, rc = r * 1.45f;
        const float dx = c.bordoDx, dy = c.bordoSu;
        float xr = W + dx - 6.0f - rc;   // centro della semicirconferenza destra della capsula
        const float sc = scalaDi(c.bersaglio), ra = c.zoom ? 0 : raggioAngoli * sc;
        const float ccx = W + dx - ra, ccy = ra - dy, sopra = ccy - c.cy, lim = ra - rc - 3.0f;
        if (sopra > 0 && xr > ccx) xr = std::min(xr, lim > sopra ? ccx + std::sqrt(lim * lim - sopra * sopra) : ccx);
        float destra = xr - r * 0.1f;
        // angolo della capsula concentrico con quello della finestra: stesso distacco in alto e a destra
        const float g = c.cy - rc + dy;
        if (ra - g > rc + 1) destra = std::min(W + dx - g, W - 1.0f) - r * 1.55f;
        for (int i = 0; i < 3; i++) cx[i] = destra - (2 - i) * passo;
    }

    // contorno della capsula: a sinistra mezzo cerchio, a destra l'angolo in alto segue la curva dell'angolo della
    // finestra (raggio = raggio finestra - distacco) e quello in basso prende il resto dell'altezza
    ID2D1PathGeometry* forma(const Capsula& c, const D2D1_RECT_F& cap) const {
        const float h = cap.bottom - cap.top, rc = h / 2;
        const float sc = scalaDi(c.bersaglio), ra = c.zoom ? 0 : raggioAngoli * sc;
        const float g = cap.top + c.bordoSu;
        float ra1 = rc, ra2 = rc;
        if (ra - g > rc + 1) { ra1 = std::min(ra - g, h - 4.0f); ra2 = std::max(4.0f, h - ra1); }
        ID2D1PathGeometry* p = nullptr; ID2D1GeometrySink* s = nullptr;
        if (FAILED(f2d->CreatePathGeometry(&p)) || FAILED(p->Open(&s))) { if (p) p->Release(); return nullptr; }
        const D2D1_SIZE_F sr = {rc, rc}, s1 = {ra1, ra1}, s2 = {ra2, ra2};
        s->BeginFigure({cap.left + rc, cap.top}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine({cap.right - ra1, cap.top});
        s->AddArc({{cap.right, cap.top + ra1}, s1, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({cap.right, cap.bottom - ra2});
        s->AddArc({{cap.right - ra2, cap.bottom}, s2, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({cap.left + rc, cap.bottom});
        s->AddArc({{cap.left + rc, cap.top}, sr, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close(); s->Release();
        return p;
    }

    // Tasto sotto il punto. L'area cliccabile e' piu' grande del cerchio disegnato (22 px di diametro, piccolo per un
    // bersaglio in alto a destra dove si va di fretta): tutta l'altezza della capsula e meta' dello spazio verso i
    // tasti vicini (ai lati, fino a un passo intero)
    int tastoIn(const Capsula& c, int x, int y) const {
        const float W = (float)(c.tasti.right - c.tasti.left), Hh = (float)(c.tasti.bottom - c.tasti.top);
        float cx[3], r; geometria(c, W, Hh, cx, r);
        if (y < 0 || y >= Hh) return -1;
        const float passo = cx[1] - cx[0], meta = passo / 2;
        if (x < cx[0] - std::max(meta, r + 4) || x > cx[2] + std::max(meta, r + 4)) return -1;
        int migliore = -1; float dmin = 1e9f;
        for (int i = 0; i < 3; i++) { const float d = std::abs(x - cx[i]); if (d < dmin) { dmin = d; migliore = i; } }
        return migliore;
    }

    // suggerimento del tasto i (sotto di lui) o, sul tasto ingrandisci della finestra in primo piano, Snap Layouts di
    // Windows (la capsula copre il tasto vero, che lo mostrerebbe al passaggio del mouse)
    void suggerisci(Capsula& c, int i) {
        if (i < 0 || !c.visibile) return;
        HWND h = c.bersaglio;
        if (i == 1 && GetForegroundWindow() == h && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
            // Win+Z: il menu dei layout di Snap per la finestra attiva, vicino al suo tasto ingrandisci
            INPUT in[4]{};
            for (auto& x : in) x.type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_LWIN; in[1].ki.wVk = 'Z';
            in[2].ki.wVk = 'Z'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
            in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, in, sizeof(INPUT));
            return;
        }
        const float W = (float)(c.tasti.right - c.tasti.left), Hh = (float)(c.tasti.bottom - c.tasti.top);
        float cx[3], r; geometria(c, W, Hh, cx, r);
        const wchar_t* testi[3] = {L"Riduci a icona", IsZoomed(h) ? L"Ripristina" : L"Ingrandisci", L"Chiudi"};
        const float sc = scalaDi(h);
        sugg.mostra(testi[i], c.tasti.left + (int)cx[i], c.tasti.top + (int)(c.cy + r + 8 * sc), sc);
    }
    suggerimento::Suggerimento sugg;

    void disegna(Capsula& c) {
        disegnaE(c, [&](HDC scr, HDC mem, const void*, int W, int Hh) {
            POINT pos = {c.tasti.left, c.tasti.top}, zero = {0, 0}; SIZE sz = {W, Hh};
            BLENDFUNCTION bl = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(c.fin, scr, &pos, &sz, mem, &zero, 0, &bl, ULW_ALPHA);
        });
    }

public:
    // La capsula di questa finestra disegnata anche sulla sua foto (gelatina, genio): mentre la finestra vera e'
    // nascosta si vedono comunque i tasti nuovi. px = foto della parte visibile, w x h, BGRA opaca.
    void sopraFoto(HWND h, const RECT& visibile, int w, int hh, unsigned int* px) {
        auto it = capsule.find(h);
        if (spenta || it == capsule.end() || !it->second.misurata) return;
        Capsula c = it->second;   // copia: il colore del fondo si prende dalla foto stessa
        const int ox = c.tasti.left - visibile.left, oy = c.tasti.top - visibile.top;
        coloreDaFoto(c, ox, oy, w, hh, px);
        dipingi(c, ox, oy, w, hh, px);
    }

    // Dove sopraFoto dipinge la capsula sulla foto di quella finestra (pixel della foto, rettangolo visibile 'visibile'):
    // sulle immagini vive della gelatina quella parte si ricopia dalla foto. false: nessuna capsula
    bool areaFoto(HWND h, const RECT& visibile, RECT& out) const {
        auto it = capsule.find(h);
        if (spenta || it == capsule.end() || !it->second.misurata) return false;
        const RECT& t = it->second.tasti;
        out = {t.left - visibile.left, t.top - visibile.top, t.right - visibile.left, t.bottom - visibile.top};
        return out.right > out.left && out.bottom > out.top;
    }

    // Il colore della barra preso dalla foto, subito a sinistra della capsula (come leggiColore dallo schermo): il fondo
    // combacia con quello che si vede anche se la foto e' stata fatta a meta' di una dissolvenza attiva/inattiva
    static void coloreDaFoto(Capsula& c, int ox, int oy, int w, int hh, const unsigned int* px) {
        const int H = c.tasti.bottom - c.tasti.top, x = ox - 3;
        if (x < 8 || x >= w || oy < 0 || oy + H > hh || H <= 0) return;
        bool uguali = true;
        for (int y = 0; y < H && uguali; y++) {
            const unsigned int a = px[(size_t)(oy + y) * w + x], b = px[(size_t)(oy + y) * w + x - 8];
            for (int k = 0; k < 24; k += 8) if (std::abs((int)((a >> k) & 255) - (int)((b >> k) & 255)) > 10) { uguali = false; break; }
        }
        if (uguali) { c.colonna.resize(H); for (int y = 0; y < H; y++) c.colonna[y] = px[(size_t)(oy + y) * w + x] | 0xFF000000u; }
        else c.colonna.clear();
        const unsigned int k = px[(size_t)(oy + std::clamp((int)c.cy, 0, H - 1)) * w + x];
        c.fondo = RGB((k >> 16) & 255, (k >> 8) & 255, k & 255); c.coloreVero = true;
    }

    // Ingrandimento/ripristino: sulla foto della finestra (rettangolo visibile 'visibile') solo il fondo che copre i
    // tasti originali, al suo posto nell'angolo in alto a destra. La pillola la fa la capsula vera, che segue l'angolo
    void copriFoto(HWND h, const RECT& visibile, int w, int hh, unsigned int* px) {
        auto it = capsule.find(h);
        if (spenta || it == capsule.end() || !it->second.misurata) return;
        Capsula c = it->second;   // copia: posizione per questo rettangolo, angolo sempre tondo come la copia animata
        const int W = c.tasti.right - c.tasti.left, Hh = c.tasti.bottom - c.tasti.top;
        c.tasti = {visibile.right - (LONG)c.bordoDx - W, visibile.top + (LONG)c.bordoSu, visibile.right - (LONG)c.bordoDx, visibile.top + (LONG)c.bordoSu + Hh};
        c.zoom = false; c.modo = 2;
        coloreDaFoto(c, c.tasti.left - visibile.left, c.tasti.top - visibile.top, w, hh, px);
        dipingi(c, c.tasti.left - visibile.left, c.tasti.top - visibile.top, w, hh, px);
    }

private:
    void dipingi(Capsula& c, int ox, int oy, int w, int hh, unsigned int* px) {
        disegnaE(c, [&](HDC, HDC, const void* bits, int W, int Hh) {
            const unsigned int* s = (const unsigned int*)bits;
            for (int y = 0; y < Hh; y++) {
                const int fy = oy + y; if (fy < 0 || fy >= hh) continue;
                for (int x = 0; x < W; x++) {
                    const int fx = ox + x; if (fx < 0 || fx >= w) continue;
                    const unsigned int p = s[(size_t)y * W + x], a = p >> 24;
                    unsigned int& d = px[(size_t)fy * w + fx];
                    unsigned int o = 0xFF000000u;
                    for (int k = 0; k < 24; k += 8)   // "sopra": colore della capsula (premoltiplicato) + foto * (1 - alfa)
                        o |= std::min(255u, ((p >> k) & 255) + ((d >> k) & 255) * (255 - a) / 255) << k;
                    d = o;
                }
            }
        });
    }

public:
    // la finestra vera sta ricomparendo (fine della gelatina o del genio): la capsula torna subito, senza aspettare
    void mostraPer(HWND h) {
        auto it = capsule.find(h);
        if (it == capsule.end()) { programma(); return; }
        it->second.forzata = GetTickCount64() + 1500;
        it->second.trascinando = false;
        it->second.campioni = 0;   // colore della barra da rileggere (quando sara' di nuovo opaca): puo' essere di una dissolvenza
        aggiorna();
        programma(400);
    }

    // la finestra vera e' appena stata nascosta (al suo posto c'e' la foto, con la capsula disegnata sopra)
    void nascondiPer(HWND h) {
        auto it = capsule.find(h);
        if (it != capsule.end()) nascondi(it->second, "gelatina");
    }

private:
    template <class F> void disegnaE(Capsula& c, F fine) {
        const int W = c.tasti.right - c.tasti.left, Hh = c.tasti.bottom - c.tasti.top;
        if (W <= 0 || Hh <= 0) return;
        // diagnostica: disegni lenti nel registro
        struct Tempo { const Motore* m; double t0; ~Tempo() { const double d = adessoMs() - t0; if (d > 4 && m->registro) { char b[64]; snprintf(b, 64, "capsule: disegno %.1f ms", d); m->scrivi(b); } } } tempo{this, adessoMs()};
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -Hh, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
        HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) { if (dib) DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, scr); return; }   // memoria finita
        HGDIOBJ vecchio = SelectObject(mem, dib);
        ID2D1DCRenderTarget* rt = nullptr;
        const D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (SUCCEEDED(f2d->CreateDCRenderTarget(&p, &rt))) {
            RECT rr = {0, 0, W, Hh};
            rt->BindDC(mem, &rr);
            rt->BeginDraw();
            const float fr = GetRValue(c.fondo) / 255.f, fg = GetGValue(c.fondo) / 255.f, fb = GetBValue(c.fondo) / 255.f;
            const bool scuro = (fr * 0.3f + fg * 0.59f + fb * 0.11f) < 0.5f;
            ID2D1SolidColorBrush* b = nullptr; rt->CreateSolidColorBrush(D2D1::ColorF(fr, fg, fb, 1), &b);
            // fondo: la striscia della barra allungata in orizzontale (se c'e'), altrimenti il colore unico
            ID2D1Brush* fondoB = b; ID2D1Bitmap* bmCol = nullptr; ID2D1BitmapBrush* bbCol = nullptr;
            if ((int)c.colonna.size() == Hh &&
                SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(1, Hh), c.colonna.data(), 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmCol)) &&
                SUCCEEDED(rt->CreateBitmapBrush(bmCol, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), &bbCol)))
                fondoB = bbCol;
            // copre i tasti originali con il colore della barra. Il rettangolo si allarga oltre i bordi della
            // capsula: cosi' l'unico angolo arrotondato che resta dentro e' quello in alto a destra, che coincide
            // con l'angolo della finestra (prima era vivo e "usciva" dall'arrotondamento di Windows)
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            if (c.modo != 1) {
                const float sc = scalaDi(c.bersaglio), ra = c.zoom ? 0.0f : raggioAngoli * sc;
                const float dx = c.bordoDx, dy = c.bordoSu;
                rt->PushAxisAlignedClip({c.oL, 0, (float)W, c.oB}, D2D1_ANTIALIAS_MODE_ALIASED);   // solo sopra ai tasti originali
                // un pixel dentro al bordo della finestra (in alto e a destra): la linea sottile che Windows disegna
                // attorno alle finestre resta intera, invece di interrompersi sotto la capsula
                const float bd = c.zoom ? 0.0f : std::max(1.0f, std::round(sc));
                rt->FillRoundedRectangle({{-ra * 2, -dy + bd, W + dx - bd, Hh + ra * 2}, std::max(0.0f, ra - bd), std::max(0.0f, ra - bd)}, fondoB);
                rt->PopAxisAlignedClip();
            }
            if (c.modo != 2) {   // (modo 2: solo il fondo, da dipingere nelle foto dell'ingrandimento)
            float cx[3], r; geometria(c, (float)W, (float)Hh, cx, r);
            const float cy = c.cy;
            // capsula di vetro attorno ai tre tasti, su un fondo del colore della barra (dove esce dai tasti originali)
            const D2D1_RECT_F cap = {cx[0] - r - r * 0.55f, cy - r - r * 0.45f, cx[2] + r + r * 0.55f, cy + r + r * 0.45f};
            if (ID2D1PathGeometry* fo = forma(c, cap)) {
                rt->FillGeometry(fo, fondoB);
                b->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.60f));
                rt->FillGeometry(fo, b);
                fo->Release();
            }
            // il bordo mezzo pixel dentro, per una linea netta
            if (ID2D1PathGeometry* fb = forma(c, {cap.left + 0.5f, cap.top + 0.5f, cap.right - 0.5f, cap.bottom - 0.5f})) {
                b->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.09f));
                rt->DrawGeometry(fb, b, 1.0f);
                fb->Release();
            }
            // finestra non in primo piano: simboli piu' tenui, come su macOS
            const float forza = c.attiva || c.sopra >= 0 ? 1.0f : 0.45f;
            const D2D1_COLOR_F ink = scuro ? D2D1::ColorF(1, 1, 1, 0.88f * forza) : D2D1::ColorF(0.1f, 0.1f, 0.12f, 0.80f * forza);
            for (int i = 0; i < 3; i++) {
                const bool s = i == c.sopra, pr = i == c.premuto;
                const D2D1_ELLIPSE e = {{cx[i], cy}, r, r};
                if (i == 2 && (s || pr)) b->SetColor(pr ? D2D1::ColorF(0.85f, 0.25f, 0.22f, 1) : D2D1::ColorF(1.0f, 0.37f, 0.34f, 1));
                else b->SetColor(scuro ? D2D1::ColorF(1, 1, 1, s ? (pr ? 0.30f : 0.22f) : 0.10f * forza) : D2D1::ColorF(0, 0, 0, s ? (pr ? 0.16f : 0.10f) : 0.05f * forza));
                rt->FillEllipse(e, b);
                b->SetColor(i == 2 && (s || pr) ? D2D1::ColorF(1, 1, 1, 1) : ink);
                const float g = r * 0.42f, sp = std::max(1.2f, r * 0.13f);
                if (i == 0) rt->DrawLine({cx[i] - g, cy}, {cx[i] + g, cy}, b, sp);
                else if (i == 1) {
                    if (IsZoomed(c.bersaglio)) {   // ripristina: due quadrati sovrapposti
                        const float q = g * 0.8f;
                        rt->DrawRectangle({cx[i] - q, cy - q + g * 0.35f, cx[i] + q - g * 0.35f, cy + q}, b, sp * 0.9f);
                        rt->DrawLine({cx[i] - q + g * 0.35f, cy - q}, {cx[i] + q, cy - q}, b, sp * 0.9f);
                        rt->DrawLine({cx[i] + q, cy - q}, {cx[i] + q, cy + q - g * 0.35f}, b, sp * 0.9f);
                    } else rt->DrawRoundedRectangle({{cx[i] - g * 0.85f, cy - g * 0.85f, cx[i] + g * 0.85f, cy + g * 0.85f}, g * 0.25f, g * 0.25f}, b, sp * 0.9f);
                } else {
                    rt->DrawLine({cx[i] - g * 0.8f, cy - g * 0.8f}, {cx[i] + g * 0.8f, cy + g * 0.8f}, b, sp);
                    rt->DrawLine({cx[i] + g * 0.8f, cy - g * 0.8f}, {cx[i] - g * 0.8f, cy + g * 0.8f}, b, sp);
                }
            }
            }
            b->Release();
            if (bbCol) bbCol->Release();
            if (bmCol) bmCol->Release();
            rt->EndDraw();
            rt->Release();
        }
        GdiFlush();
        fine(scr, mem, (const void*)bits, W, Hh);
        SelectObject(mem, vecchio); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, scr);
    }

    void esegui(Capsula& c, int i) {
        HWND h = c.bersaglio;
        if (!h || !IsWindow(h)) return;
        // ingrandisci/ripristina: la capsula resta e segue la finestra (vedi cambiata)
        if (i == 1) { c.sopra = c.premuto = -1; disegna(c); if (ingrandendo) ingrandendo(h); PostMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0); return; }
        // con l'animazione di FluidWin pronta (la sua foto, con la capsula, e' gia' sopra) la capsula resta finche' la
        // finestra se ne va: nasconderla subito lascerebbe vedere per qualche fotogramma i tasti originali dell'app
        // (senza rimisura: nasconderebbe la capsula; e' la stessa strada della riduzione dal dock)
        // (riduzione senza genio, con la copia DWM nel livello sopra a tutto: quella non ha la capsula, si nasconde)
        const bool coperta = i == 0 ? riduci && riduci(h) && fotoPronta && fotoPronta(h) : chiudendo && chiudendo(h);
        if (coperta) { c.sopra = c.premuto = -1; disegna(c); }
        else { nascondi(c, "clic"); c.provata = false; }
        if (i == 0) { if (!coperta) PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0); }
        else PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
        programma(400, true);
    }

    static LRESULT CALLBACK procTempi(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_TIMER && w == 1) { if (Motore* d = gIstanza()) d->aggiorna(); return 0; }
        if (m == WM_TIMER && w == 2) { if (Motore* d = gIstanza()) d->passoVolo(); return 0; }
        if (m == WM_TIMER && w == 3) { if (Motore* d = gIstanza()) d->controlloColore(); return 0; }
        if (m == WM_APP + 20) { if (Motore* d = gIstanza()) d->aggiorna(); return 0; }   // una sonda ha finito
        return DefWindowProcW(h, m, w, l);
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_CLOSE) return 0;   // Alt+F4 o un programma che chiede di chiudere: le capsule restano
        Motore* d = gIstanza();
        if (!d) return DefWindowProcW(h, m, w, l);
        auto it = d->capsule.find((HWND)GetWindowLongPtrW(h, GWLP_USERDATA));
        if (it == d->capsule.end()) return DefWindowProcW(h, m, w, l);
        Capsula& c = it->second;
        switch (m) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_MOUSEMOVE: {
                if (!c.mouseDentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, h, 0}; TrackMouseEvent(&tm); c.mouseDentro = true; }
                const int s = d->tastoIn(c, GET_X_LPARAM(l), GET_Y_LPARAM(l));
                if (s != c.sopra) {
                    c.sopra = s; d->disegna(c);
                    // suggerimento (o Snap Layouts) dopo mezzo secondo fermi sullo stesso tasto
                    KillTimer(h, 7); d->sugg.nascondi();
                    if (s >= 0 && c.premuto < 0) SetTimer(h, 7, 550, nullptr);
                }
                return 0;
            }
            case WM_TIMER:
                if (w == 7) { KillTimer(h, 7); if (c.sopra >= 0 && c.premuto < 0) d->suggerisci(c, c.sopra); return 0; }
                break;
            case WM_MOUSELEAVE:
                c.mouseDentro = false;
                KillTimer(h, 7); d->sugg.nascondi();
                if (c.sopra >= 0 || c.premuto >= 0) { c.sopra = c.premuto = -1; if (c.visibile) d->disegna(c); }
                return 0;
            case WM_LBUTTONDOWN:
                KillTimer(h, 7); d->sugg.nascondi();
                c.premuto = d->tastoIn(c, GET_X_LPARAM(l), GET_Y_LPARAM(l)); SetCapture(h); d->disegna(c); return 0;
            case WM_LBUTTONUP: {
                ReleaseCapture();
                const int s = d->tastoIn(c, GET_X_LPARAM(l), GET_Y_LPARAM(l)), p = c.premuto;
                c.premuto = -1;
                if (s >= 0 && s == p) d->esegui(c, s); else if (c.visibile) d->disegna(c);
                return 0;
            }
            case WM_NCHITTEST: return HTCLIENT;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

} // namespace capsula
