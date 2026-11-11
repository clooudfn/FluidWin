// Notch di FluidWin: in alto al centro dello schermo del dock, nero come il notch del Mac e dell'iPhone e attaccato al
// bordo in alto (ai lati si raccorda al bordo con due curve all'indietro, sotto ha gli angoli tondi).
// A riposo mostra ora e giorno. Si trasforma, con una molla, per mostrare:
//  - il volume, mentre si premono i tasti (al posto dell'indicatore di vetro, osd.h);
//  - le notifiche, che escono dal notch (al posto del banner di vetro, banner.h);
//  - la musica in riproduzione (musica.h): copertina e barrette accanto all'ora; col mouse fermo sopra si apre con
//    titolo, artista e i tasti indietro / play-pausa / avanti.
// Il mouse di solito lo attraversa (sta dove si afferrano le finestre ingrandite): diventa cliccabile solo col mouse
// fermo sopra per un attimo (senza tasti premuti) o mentre mostra una notifica. Un clic sull'ora apre le notifiche.
// Con un'app a schermo intero (video, presentazioni) resta, ma piu' il mouse si avvicina piu' diventa trasparente;
// con un gioco non c'e' proprio (vedi modo()).
//
// Disegno: una finestra a strati di misura fissa (la piu' grande che serve), la forma disegnata al centro con
// Direct2D software in un DIB tenuto da parte; ogni fotogramma e' un UpdateLayeredWindow. La trasparenza col mouse
// cambia solo l'alfa della finestra, senza ridisegnare.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>
#include "fabbriche.h"
#include "vetro.h"
#include "musica.h"
#include "testi.h"
#include "comodita.h"
#include "sensori.h"
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace notch {

enum Modo { NORMALE = 0, SFUMATO = 1, NASCOSTO = 2 };   // schermo intero: niente, app (sfuma col mouse), gioco

class Notch {
public:
    std::function<void()> clic;             // clic sull'ora: le notifiche
    std::function<void()> clicNotifica;     // clic sulla notifica mostrata: apre l'app
    std::function<void()> chiudiNotifica;   // la x sulla notifica: la toglie
    std::function<void()> notificaFinita;   // la notifica non c'e' piu' (chi la manda puo' passare alla prossima)

    bool crea(HINSTANCE inst) {
        WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"FluidWin.Notch";
        wc.hCursor = LoadCursor(nullptr, IDC_HAND);
        wc.cbWndExtra = sizeof(void*);
        RegisterClassW(&wc);
        // trasparente al mouse finche' non serve (vedi 'cliccabile')
        fin = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"",
                              WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
        if (!fin) return false;
        SetWindowLongPtrW(fin, 0, (LONG_PTR)this);
        // scaffale: i file trascinati sul notch ci restano appoggiati (serve OleInitialize nel thread)
        bersaglio.n = this;
        RegisterDragDrop(fin, &bersaglio);
        BOOL niente = TRUE; DwmSetWindowAttribute(fin, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        return true;
    }

    ~Notch() {
        if (fin) RevokeDragDrop(fin);
        svuotaScaffale();
        lettore.ferma();
        libera();
        for (IDWriteTextFormat* f : formati()) if (f) f->Release();
    }

    // font scelti dal dock (Segoe UI Variable o quelli di riserva)
    void font(const wchar_t* testo, const wchar_t* titoli) {
        if (fontTesto == testo && fontTitoli == titoli) return;
        fontTesto = testo; fontTitoli = titoli; fDpi = 0;
    }

    // ora e giorno: ridisegna solo se cambiano
    void testi(const std::wstring& ora, const std::wstring& giorno) {
        if (ora == testoOra && giorno == testoGiorno) return;
        testoOra = ora; testoGiorno = giorno;
        misuraOra(); bersagli(); ridisegna();
    }

    // schermo su cui sta (in alto al centro) e scala del dock
    void posto(const RECT& s, float d) {
        schermo = s; dpi = d;
        superficie(); misuraOra(); bersagli(true); ridisegna();
    }

    // dock acceso o spento
    void mostra(bool si) {
        acceso = si;
        if (si) lettore.avvia();
        visibilita();
    }

    // schermo intero: NORMALE, SFUMATO (app: trasparente col mouse vicino), NASCOSTO (gioco)
    void modo(int m) {
        if (m == modoAtt) return;
        modoAtt = m;
        if (m != NORMALE) { sopra = false; attesa = 0; finisciNotifica(); }   // sopra un'app a schermo intero niente notifiche
        if (m != SFUMATO && alfa != 1) { alfa = 1; }
        visibilita();
        cliccabile(false);
        bersagli(); ridisegna();
    }

    // volume 0..1 dai tasti; false se il notch non si vede (allora lo mostra l'indicatore di vetro)
    bool volume(float liv, bool muto) {
        if (!visibile) return false;
        const bool entra = adesso() >= fineVolume;
        livello = std::clamp(liv, 0.0f, 1.0f); this->muto = muto;
        if (entra) livelloVis = livello;
        fineVolume = adesso() + 1.4;
        bersagli(); anima();
        return true;
    }

    // notifica da mostrare (icona: pixel BGRA premoltiplicati w x h, puo' mancare); false se il notch non la mostra
    bool notifica(const std::wstring& app, const std::wstring& titolo, const std::wstring& testo, const std::vector<UINT>& px, int w, int h) {
        if (!visibile || modoAtt != NORMALE) return false;
        nApp = app; nTitolo = titolo; nTesto = testo; nPx = px; nW = w; nH = h; iconaNuova = true;
        notificaAttiva = true; fineNotifica = adesso() + 5.5;
        bersagli(); anima();
        return true;
    }

    // avviso breve (1,6 s, 2,5 per batteria e cuffie): un simbolo (sensori::Simbolo) e un testo, per esempio
    // "Sempre in primo piano" o "AirPods · 80%"; valore: la batteria (-1 se non si sa). false se il notch non si vede
    bool avviso(const std::wstring& testo, int simbolo, int valore, bool acceso) {
        if (!visibile) return false;
        testoAvviso = testo; avvisoAcceso = acceso; simboloAvviso = simbolo; valoreAvviso = valore;
        larghezzaAvviso = fOra ? larghezza(testo, fPiccolo ? fPiccolo : fOra) : 160 * dpi;
        fineAvviso = adesso() + (simbolo == sensori::S_PUNTINA ? 1.6 : 2.5);
        bersagli(); anima();
        return true;
    }

private:
    enum Scena { ORA, MUSICA, APERTO, VOLUME, NOTIFICA, AVVISO, TIMER, SUONA, SCAFFALE, TIMER_APERTO };

    HWND fin = nullptr;
    bool acceso = false, visibile = false;
    int modoAtt = NORMALE;
    RECT schermo{};
    float dpi = 1, fDpi = 0;
    std::wstring testoOra, testoGiorno;
    const wchar_t *fontTesto = L"Segoe UI Variable Text", *fontTitoli = L"Segoe UI Variable Display";
    IDWriteTextFormat *fOra = nullptr, *fGiorno = nullptr, *fTitolo = nullptr, *fPiccolo = nullptr, *fTesto = nullptr, *fNumero = nullptr, *fGrande = nullptr;
    float lo = 0, lg = 0, largoOra = 0;   // larghezze di ora, giorno e del notch a riposo

    // superficie: DIB fisso con il suo render target software
    HDC mem = nullptr; HBITMAP dib = nullptr; HGDIOBJ vecchio = nullptr;
    int W2 = 0, H2 = 0;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* pen = nullptr;
    ID2D1Bitmap *bCopertina = nullptr, *bIcona = nullptr;

    // forma: larghezza, altezza e raggio inseguono il bersaglio con una molla (rimbalza un poco, come una gelatina)
    struct Molla {
        double x = 0, v = 0, obj = 0;
        bool passo(double dt) {
            const double k = 420, c = 2 * std::sqrt(k) * 0.70;
            v += (-k * (x - obj) - c * v) * dt; x += v * dt;
            if (std::fabs(x - obj) < 0.3 && std::fabs(v) < 3) { x = obj; v = 0; return false; }
            return true;
        }
    };
    Molla mw, mh, mr;
    int scena = ORA, scenaPrima = ORA;
    double tScena = 0, tPrec = 0;
    bool animando = false;
    float alfa = 1;   // alfa della finestra (schermo intero di un'app: piu' trasparente col mouse vicino)

    // contenuti
    float livello = 0, livelloVis = 0; bool muto = false; double fineVolume = 0;
    std::wstring testoAvviso; bool avvisoAcceso = false; double fineAvviso = 0; float larghezzaAvviso = 0;
    int simboloAvviso = 0, valoreAvviso = -1;
    ULONGLONG secondoTimer = 0; UINT versioneSensori = 0;
    bool segnoSilenzio = false, segnoSveglio = false;   // lunetta e tazzina accanto all'ora (comodita.h)
    double tSegnoSilenzio = -10, tSegnoSveglio = -10;   // quando sono comparse o sparite (dissolvenza)
    // (in uscita piu' in fretta: il notch intanto si stringe e il segno non deve arrivare sopra l'ora)
    float alfaSegno(bool acceso, double t0) const { const float k = (float)std::clamp((adesso() - t0) / (acceso ? 0.3 : 0.14), 0.0, 1.0); const float e = k * k * (3 - 2 * k); return acceso ? e : 1 - e; }
    bool notificaAttiva = false; double fineNotifica = 0;
    std::wstring nApp, nTitolo, nTesto; std::vector<UINT> nPx; int nW = 0, nH = 0; bool iconaNuova = false;
    musica::Lettore lettore; musica::Brano brano; UINT versioneBrano = 0xFFFFFFFF; bool copertinaNuova = false;

    // mouse
    bool sopra = false, cliccabileOra = false;
    int attesa = 0, fuori = 0, pulsanteSopra = -1;
    bool sopraX = false, mouseDentro = false;
    ULONGLONG tCima = 0;

    std::vector<IDWriteTextFormat*> formati() const { return {fOra, fGiorno, fTitolo, fPiccolo, fTesto, fNumero, fGrande}; }

    static double adesso() { return vetro::ms() / 1000.0; }

    void visibilita() {
        const bool si = acceso && modoAtt != NASCOSTO && fin;
        if (si == visibile) return;
        visibile = si;
        if (si) {
            KillTimer(fin, 2); SetTimer(fin, 2, 50, nullptr);   // mouse, musica, scadenze
            bersagli(true); ridisegna();
            ShowWindow(fin, SW_SHOWNOACTIVATE);
            SetWindowPos(fin, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else {
            KillTimer(fin, 2); vetro::fermaBattito(fin, 1); animando = false;
            sopra = false; attesa = 0; cliccabile(false);
            finisciNotifica(); fineVolume = 0; fineAvviso = 0;
            ShowWindow(fin, SW_HIDE);
        }
    }

    void cliccabile(bool si) {
        if (!fin || si == cliccabileOra) return;
        cliccabileOra = si;
        LONG_PTR ex = GetWindowLongPtrW(fin, GWL_EXSTYLE);
        SetWindowLongPtrW(fin, GWL_EXSTYLE, si ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT));
    }

    void finisciNotifica() {
        if (!notificaAttiva) return;
        notificaAttiva = false;
        if (notificaFinita) notificaFinita();
    }

    // ---- scene: quale mostrare e quanto grande ----

    int scenaVoluta() const {
        const double t = adesso();
        if (t < fineVolume) return VOLUME;
        if (t < fineAvviso) return AVVISO;
        if (comodita::timerSuona()) return SUONA;
        if (notificaAttiva) return NOTIFICA;
        if (trascinoSopra || t < fineScaffale) return SCAFFALE;
        if (sopra && comodita::timerAttivo()) return TIMER_APERTO;   // col mouse sopra: il timer grande, +1 min e stop
        if (sopra && !scaffale.empty()) return SCAFFALE;
        if (sopra && brano.c) return APERTO;
        if (comodita::timerAttivo()) return TIMER;
        if (brano.c && brano.suona) return MUSICA;
        return ORA;
    }

    void misura(int s, float& w, float& h, float& R) const {
        const float d = dpi;
        switch (s) {
            case MUSICA:   w = largoOra + 62 * d; h = 22 * d; R = 9 * d; break;
            case APERTO:   w = 372 * d; h = 96 * d; R = 26 * d; break;
            case VOLUME:   w = 292 * d; h = 32 * d; R = 14 * d; break;
            case NOTIFICA: w = 372 * d; h = 78 * d; R = 26 * d; break;
            case SCAFFALE: w = 372 * d; h = 96 * d; R = 26 * d; break;
            case AVVISO:   w = std::clamp(larghezzaAvviso + 76 * d, 220 * d, 372 * d); h = 32 * d; R = 14 * d; break;
            case TIMER:    w = largoOra + 104 * d; h = 22 * d; R = 9 * d; break;
            case TIMER_APERTO: w = 312 * d; h = 60 * d; R = 24 * d; break;
            case SUONA:    w = 300 * d; h = 42 * d; R = 20 * d; break;
            default:       w = largoOra; h = 22 * d; R = 9 * d;
                           {   // posto per scaffale e tazzina (a sinistra) e luna (a destra), uguale ai due lati: l'ora resta al centro
                               const float sx = (!scaffale.empty() ? 36 * d : 0) + (segnoSveglio ? 22 * d : 0), dx = segnoSilenzio ? 22 * d : 0;
                               w += 2 * std::max(sx, dx);
                           }
                           if (sopra) { w += 14 * d; h += 5 * d; R = 11 * d; }   // col mouse sopra si gonfia un poco
        }
        w = std::round(w); h = std::round(h); R = std::round(R);
    }

    // aggiorna scena e misure da raggiungere; 'subito' salta l'animazione
    void bersagli(bool subito = false) {
        const int s = scenaVoluta();
        if (s != scena) { scenaPrima = scena; scena = s; tScena = adesso(); }
        float w, h, R; misura(scena, w, h, R);
        mw.obj = w; mh.obj = h; mr.obj = R;
        if (subito || !visibile || mw.x == 0) {
            mw.x = w; mh.x = h; mr.x = R; mw.v = mh.v = mr.v = 0; scenaPrima = scena;
            return;
        }
        if (mw.x != w || mh.x != h || mr.x != R || scenaPrima != scena) anima();
    }

    void anima() {
        if (!visibile || animando) return;
        animando = true; tPrec = adesso();
        vetro::battito(fin, 1);
    }

    void fotogramma() {
        const double t = adesso(), dt = std::clamp(t - tPrec, 0.001, 0.034);
        tPrec = t;
        bool ancora = false;
        for (Molla* m : {&mw, &mh, &mr}) ancora |= m->passo(dt);
        if (std::fabs(livelloVis - livello) > 0.002f) { livelloVis += (livello - livelloVis) * (float)std::min(1.0, dt * 18); ancora = true; }
        else livelloVis = livello;
        if (scenaPrima != scena) { if (t - tScena < 0.30) ancora = true; else scenaPrima = scena; }
        if ((scena == TIMER || scena == TIMER_APERTO) && t - tScena < 0.45) ancora = true;   // comparsa del timer
        if (t - tSegnoSveglio < 0.32 || t - tSegnoSilenzio < 0.32) ancora = true;           // tazzina e luna
        ridisegna();
        if (!ancora) { animando = false; vetro::fermaBattito(fin, 1); }
    }

    // ---- ogni 50 ms: mouse, musica, scadenze ----

    RECT corpo() const {   // la forma sullo schermo (coordinate dello schermo)
        const int x = posX() + (int)((W2 - mw.x) / 2);
        return {x, schermo.top, x + (int)mw.x, schermo.top + (int)mh.x};
    }
    int posX() const { return schermo.left + (schermo.right - schermo.left - W2) / 2; }

    void controlla() {
        if (!visibile) return;
        const double t = adesso();
        bool ridisegnare = false;
        // musica
        const UINT v = lettore.versione();
        if (v != versioneBrano) {
            versioneBrano = v;
            musica::Brano b = lettore.stato();
            if (b.versione != brano.versione || b.copertina.size() != brano.copertina.size()) copertinaNuova = true;
            brano = std::move(b);
            ridisegnare = true;
        }
        // timer: si ridisegna a ogni secondo che cambia; suona: a ogni giro (il cerchio pulsa)
        if (comodita::timerAttivo()) {
            const ULONGLONG sec = (comodita::timerResto() + 999) / 1000;
            if (sec != secondoTimer) { secondoTimer = sec; ridisegnare = true; }
        }
        if (scena == SUONA) ridisegnare = true;
        // microfono e webcam (pallini fuori dal notch)
        if (sensori::Sensori::io().versione != versioneSensori) { versioneSensori = sensori::Sensori::io().versione; ridisegnare = true; }
        // lunetta (non disturbare) e tazzina (tieni sveglio)
        if (comodita::silenzio() != segnoSilenzio) { segnoSilenzio = comodita::silenzio(); tSegnoSilenzio = t; bersagli(); anima(); }
        if (comodita::sveglio() != segnoSveglio) { segnoSveglio = comodita::sveglio(); tSegnoSveglio = t; bersagli(); anima(); }
        // mouse
        POINT p; GetCursorPos(&p);
        RECT c = corpo();
        const int m = (int)(4 * dpi);
        RECT largo = {c.left - m, c.top, c.right + m, c.bottom + m};
        const bool dentro = PtInRect(&largo, p) != FALSE;
        const bool tasti = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        if (modoAtt == SFUMATO) {
            // piu' il mouse si avvicina piu' sparisce (a contatto non si vede): sotto c'e' l'app a schermo intero
            const double dx = std::max({(double)c.left - p.x, 0.0, (double)p.x - c.right});
            const double dy = std::max({(double)c.top - p.y, 0.0, (double)p.y - c.bottom});
            double k = std::clamp(std::sqrt(dx * dx + dy * dy) / (150.0 * dpi), 0.0, 1.0);
            k = k * k * (3 - 2 * k);
            if (std::fabs(k - alfa) > 0.01 || (k == 1 && alfa != 1) || (k == 0 && alfa != 0)) { alfa = (float)k; mostraAlfa(); }
        } else {
            // col mouse fermo sopra per un attimo (senza tasti: non mentre si trascina una finestra) si apre
            if (!sopra) {
                attesa = dentro && !tasti ? attesa + 1 : 0;
                if (attesa >= 6) { sopra = true; fuori = 0; bersagli(); }
            } else {
                fuori = dentro ? 0 : fuori + 1;
                if (fuori >= 5) { sopra = false; attesa = 0; bersagli(); }
            }
        }
        // tasto sotto il mouse (aperto) e x della notifica
        const POINT pc = {p.x - posX(), p.y - schermo.top};
        const int ps = scena == APERTO ? pulsante(pc.x, pc.y) : scena == TIMER_APERTO ? tastoTimerIn(pc.x, pc.y) : -1;
        const bool sx = scena == NOTIFICA && dentro && sullaX(pc.x, pc.y);
        if (ps != pulsanteSopra || sx != sopraX || dentro != mouseDentro) { pulsanteSopra = ps; sopraX = sx; mouseDentro = dentro; ridisegnare = true; }
        // scadenze: volume e notifica (ferma finche' c'e' sopra il mouse)
        if (notificaAttiva && t >= fineNotifica) { if (dentro) fineNotifica = t + 1.5; else finisciNotifica(); }
        // con il tasto premuto e il mouse sul notch (un file trascinato da Esplora file) il notch si fa trovare dal
        // trascinamento: trasparente al mouse, Windows non glielo darebbe mai
        cliccabile(modoAtt == NORMALE && (sopra || (scena == NOTIFICA && dentro) || scena == SUONA || scena == SCAFFALE || (tasti && dentro)));
        const int prima = scena;
        bersagli();
        if (scena != prima) ridisegnare = false;   // ci pensa l'animazione
        if (ridisegnare && !animando) ridisegna();
        // le barrette della musica si muovono: si ridisegna solo il loro rettangolino (tutto il notch costava ~3% di CPU)
        else if (brano.suona && (scena == MUSICA || scena == APERTO) && !animando) ridisegnaBarrette();
        // ogni 2 s di nuovo in cima (un'app sempre in primo piano aperta dopo poteva coprirlo)
        if (GetTickCount64() - tCima > 2000) { tCima = GetTickCount64(); SetWindowPos(fin, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
    }

    // ---- disposizione dei contenuti (coordinate della finestra) ----

    float sinistra(int s) const { float w, h, R; misura(s, w, h, R); return std::round((W2 - w) / 2); }

    // tasti della musica aperta: 0 indietro, 1 play/pausa, 2 avanti
    D2D1_POINT_2F centroTasto(int i) const {
        float w, h, R; misura(APERTO, w, h, R);
        const float L = sinistra(APERTO), d = dpi;
        const float a = L + 100 * d, b = L + w - 22 * d, cx = (a + b) / 2;
        return {cx + (i - 1) * 46 * d, 72 * d};
    }
    // timer aperto: 0 +1 min, 1 stop
    D2D1_RECT_F tastoTimer(int i) const {
        float w, h, R; misura(TIMER_APERTO, w, h, R);
        const float L = sinistra(TIMER_APERTO), d = dpi, cy = h / 2;
        if (i == 1) return {L + w - 54 * d, cy - 17 * d, L + w - 20 * d, cy + 17 * d};
        return {L + w - 124 * d, cy - 15 * d, L + w - 62 * d, cy + 15 * d};
    }
    int tastoTimerIn(int x, int y) const {
        for (int i = 0; i < 2; i++) { const D2D1_RECT_F r = tastoTimer(i); if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i; }
        return -1;
    }

    int pulsante(int x, int y) const {
        for (int i = 0; i < 3; i++) {
            const D2D1_POINT_2F c = centroTasto(i);
            if (std::hypot(x - c.x, y - c.y) <= 17 * dpi) return i;
        }
        return -1;
    }
    bool sullaX(int x, int y) const {
        float w, h, R; misura(NOTIFICA, w, h, R);
        const float L = sinistra(NOTIFICA);
        return x > L + w - 40 * dpi && x < L + w && y < 38 * dpi;
    }

    void clicca(int x, int y) {
        if (scena == SCAFFALE) {
            const int o = oggettoIn(x, y);
            if (o == SVUOTA) { svuotaScaffale(); bersagli(); }
            else if (o >= 0 && o < (int)scaffale.size()) ShellExecuteW(nullptr, L"open", scaffale[o].percorso.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (scena == SUONA) { comodita::silenziaTimer(); bersagli(); return; }   // clic: la sveglia smette
        if (scena == TIMER_APERTO) {   // +1 min o stop; altrove sul timer non succede niente (prima apriva le notifiche)
            const int b = tastoTimerIn(x, y);
            if (b == 0) comodita::aggiungiTimer(60000);
            else if (b == 1) { comodita::fermaTimer(); sopra = false; }
            bersagli(); ridisegna();
            return;
        }
        if (scena == TIMER) return;
        if (scena == NOTIFICA) {
            if (sullaX(x, y)) { if (chiudiNotifica) chiudiNotifica(); }
            else if (clicNotifica) clicNotifica();
            finisciNotifica(); bersagli();
            return;
        }
        if (scena == APERTO) {
            const int b = pulsante(x, y);
            if (b >= 0) {
                lettore.comando(b == 0 ? musica::Lettore::INDIETRO : b == 1 ? musica::Lettore::PLAY_PAUSA : musica::Lettore::AVANTI);
                if (b == 1) brano.suona = !brano.suona;   // subito, senza aspettare la prossima lettura
                ridisegna();
                return;
            }
        }
        if (clic) clic();
    }

    // ---- superficie e formati ----

    void libera() {
        for (ID2D1Bitmap** b : {&bCopertina, &bIcona}) if (*b) { (*b)->Release(); *b = nullptr; }
        for (auto& o : scaffale) if (o.b) { o.b->Release(); o.b = nullptr; }   // (legate al vecchio render target)
        if (pen) { pen->Release(); pen = nullptr; }
        if (rt) { rt->Release(); rt = nullptr; }
        if (mem) { SelectObject(mem, vecchio); DeleteDC(mem); mem = nullptr; }
        if (dib) { DeleteObject(dib); dib = nullptr; }
    }

    void superficie() {
        const int w = (int)std::round(392 * dpi), h = (int)std::round(100 * dpi);   // la scena piu' grande + i raccordi
        if (rt && w == W2 && h == H2) return;
        libera();
        W2 = w; H2 = h;
        ID2D1Factory* f2d = fabbriche::d2d();
        if (!f2d) return;
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W2, -H2, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HDC scr = GetDC(nullptr);
        mem = CreateCompatibleDC(scr);
        dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, scr);
        if (!mem || !dib) { libera(); return; }
        vecchio = SelectObject(mem, dib);
        const D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(f2d->CreateDCRenderTarget(&p, &rt))) { rt = nullptr; libera(); return; }
        RECT rr = {0, 0, W2, H2};
        rt->BindDC(mem, &rr);
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &pen);
        copertinaNuova = iconaNuova = true;
    }

    void preparaFormati() {
        IDWriteFactory* fdw = fabbriche::testo();
        if (!fdw || fDpi == dpi) return;
        for (IDWriteTextFormat* f : formati()) if (f) f->Release();
        fOra = fGiorno = fTitolo = fPiccolo = fTesto = fNumero = fGrande = nullptr;
        const float d = dpi;
        auto crea = [&](const wchar_t* fam, DWRITE_FONT_WEIGHT p, float px, IDWriteTextFormat** f, bool taglia, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
            if (FAILED(fdw->CreateTextFormat(fam, nullptr, p, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px * d, L"it-it", f)) || !*f) return;
            (*f)->SetTextAlignment(al); (*f)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); (*f)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (taglia) {   // testo troppo lungo: finisce con "…"
                IDWriteInlineObject* e = nullptr; fdw->CreateEllipsisTrimmingSign(*f, &e);
                DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; (*f)->SetTrimming(&tr, e);
                if (e) e->Release();
            }
        };
        crea(fontTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, &fOra, false);
        crea(fontTesto, DWRITE_FONT_WEIGHT_MEDIUM, 9.5f, &fGiorno, false);
        crea(fontTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f, &fTitolo, true);
        crea(fontTesto, DWRITE_FONT_WEIGHT_MEDIUM, 11.0f, &fPiccolo, true);
        crea(fontTesto, DWRITE_FONT_WEIGHT_NORMAL, 12.0f, &fTesto, true);
        crea(fontTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f, &fNumero, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
        crea(fontTitoli, DWRITE_FONT_WEIGHT_SEMI_BOLD, 24.0f, &fGrande, false);
        fDpi = dpi;
    }

    void misuraOra() {
        preparaFormati();
        if (!fOra || !fGiorno) return;
        lo = larghezza(testoOra, fOra); lg = larghezza(testoGiorno, fGiorno);
        largoOra = std::round(std::max(200 * dpi, lo + 7 * dpi + lg + 48 * dpi));
    }

    static float larghezza(const std::wstring& s, IDWriteTextFormat* f) {
        IDWriteTextLayout* l = nullptr; float w = 0;
        if (SUCCEEDED(fabbriche::testo()->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 1000, 100, &l))) {
            DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; l->Release();
        }
        return w;
    }

    static void bitmap(ID2D1DCRenderTarget* rt, ID2D1Bitmap*& b, const std::vector<UINT>& px, int w, int h) {
        if (b) { b->Release(); b = nullptr; }
        if (px.empty() || w <= 0 || h <= 0 || px.size() < (size_t)w * h) return;
        rt->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4,
                         D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
    }

    // ---- disegno ----

    void colore(float r, float g, float b, float a) { pen->SetColor(D2D1::ColorF(r, g, b, a)); }

    void scrivi(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float a) {
        if (!f || t.empty()) return;
        colore(1, 1, 1, a);
        rt->DrawText(t.c_str(), (UINT32)t.size(), f, r, pen, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // immagine con gli angoli tondi (copertina, icona)
    void immagine(ID2D1Bitmap* b, D2D1_RECT_F r, float raggio, float a) {
        if (!b) return;
        ID2D1BitmapBrush* bb = nullptr;
        if (FAILED(rt->CreateBitmapBrush(b, &bb)) || !bb) return;
        const D2D1_SIZE_F s = b->GetSize();
        bb->SetTransform(D2D1::Matrix3x2F::Scale((r.right - r.left) / s.width, (r.bottom - r.top) / s.height) * D2D1::Matrix3x2F::Translation(r.left, r.top));
        bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        bb->SetOpacity(a);
        rt->FillRoundedRectangle({r, raggio, raggio}, bb);
        bb->Release();
    }

    void ridisegna() {
        if (!fin || !visibile || testoOra.empty()) return;
        superficie(); preparaFormati();
        if (!rt || !pen || !fOra) return;
        ID2D1Factory* f2d = fabbriche::d2d();
        if (copertinaNuova) { bitmap(rt, bCopertina, brano.copertina, musica::LATO, musica::LATO); copertinaNuova = false; }
        if (iconaNuova) { bitmap(rt, bIcona, nPx, nW, nH); iconaNuova = false; }
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        const float f = std::round(6 * dpi);   // raccordo con il bordo in alto
        const float w = (float)std::max(2.0, mw.x), h = (float)std::max((double)f + 2, mh.x);
        const float R = std::clamp((float)mr.x, 0.0f, std::min(w / 2, h - f));
        const float x0 = (W2 - w) / 2, x1 = x0 + w;
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (f2d && SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            s->BeginFigure({x0 - f, 0}, D2D1_FIGURE_BEGIN_FILLED);
            s->AddArc({{x0, f}, {f, f}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});        // raccordo a sinistra
            s->AddLine({x0, h - R});
            s->AddArc({{x0 + R, h}, {R, R}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            s->AddLine({x1 - R, h});
            s->AddArc({{x1, h - R}, {R, R}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            s->AddLine({x1, f});
            s->AddArc({{x1 + f, 0}, {f, f}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});   // raccordo a destra
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close(); s->Release();
            colore(0, 0, 0, 1);
            rt->FillGeometry(g, pen);
            // contenuti dentro la forma: quello di prima sfuma via, il nuovo compare mentre la forma cambia
            ID2D1Layer* strato = nullptr;
            if (SUCCEEDED(rt->CreateLayer(&strato))) {
                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g), strato);
                const double t = adesso() - tScena;
                if (scenaPrima != scena) {
                    const float a = (float)std::clamp(1 - t / 0.12, 0.0, 1.0);
                    if (a > 0) contenuto(scenaPrima, a);
                    contenuto(scena, (float)std::clamp((t - 0.07) / 0.18, 0.0, 1.0));
                } else contenuto(scena, 1);
                rt->PopLayer();
                strato->Release();
            }
            // pallini fuori dal notch, a destra, come su iPhone: arancione il microfono, verde la webcam
            const sensori::Sensori& se = sensori::Sensori::io();
            if (w < 330 * dpi) {
                float px = x1 + f + 7 * dpi;
                if (!se.micApp.empty()) { colore(1.0f, 0.62f, 0.04f, 1); rt->FillEllipse({{px, 6 * dpi}, 3.5f * dpi, 3.5f * dpi}, pen); px += 10 * dpi; }
                if (!se.camApp.empty()) { colore(0.2f, 0.84f, 0.29f, 1); rt->FillEllipse({{px, 6 * dpi}, 3.5f * dpi, 3.5f * dpi}, pen); }
            }
        }
        if (g) g->Release();
        if (rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) { libera(); return; }
        mostraAlfa();
    }

    // dove stanno le barrette nella scena attuale (coordinate della finestra)
    bool zonaBarrette(D2D1_RECT_F& r, D2D1_POINT_2F& o) const {
        float w, h, R; misura(scena, w, h, R);
        const float L = sinistra(scena), d = dpi;
        if (scena == MUSICA) o = {L + w - 25 * d, h / 2};
        else if (scena == APERTO) o = {L + w - 35 * d, 24 * d};
        else return false;
        r = {std::floor(o.x - 2), std::floor(o.y - 8 * d), std::ceil(o.x + 13 * d + 2), std::ceil(o.y + 8 * d)};
        return true;
    }

    void ridisegnaBarrette() {
        D2D1_RECT_F r; D2D1_POINT_2F o;
        if (!rt || !pen || !visibile || !zonaBarrette(r, o)) return;
        rt->BeginDraw();
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        rt->Clear(D2D1::ColorF(0, 0, 0, 1));   // li' sotto c'e' solo il nero del notch
        barrette(o.x, o.y, 13 * dpi, 1);
        rt->PopAxisAlignedClip();
        if (rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) { libera(); return; }
        const RECT sporco = {(LONG)r.left, (LONG)r.top, (LONG)r.right, (LONG)r.bottom};
        mostraAlfa(&sporco);
    }

    // 'sporco': solo quella parte e' cambiata (Windows copia meno)
    void mostraAlfa(const RECT* sporco = nullptr) {
        if (!mem) return;
        GdiFlush();
        POINT pos = {posX(), schermo.top}, zero = {0, 0}; SIZE sz = {W2, H2};
        BLENDFUNCTION bl = {AC_SRC_OVER, 0, (BYTE)std::lround(255 * std::clamp(alfa, 0.0f, 1.0f)), AC_SRC_ALPHA};
        UPDATELAYEREDWINDOWINFO u{sizeof(u), nullptr, &pos, &sz, mem, &zero, 0, &bl, ULW_ALPHA, sporco};
        UpdateLayeredWindowIndirect(fin, &u);
    }

    void contenuto(int s, float a) {
        if (a <= 0.001f) return;
        float w, h, R; misura(s, w, h, R);
        const float L = sinistra(s), d = dpi;
        switch (s) {
            case ORA:
                ora(L, w, h, a);
                if (!scaffale.empty()) segnoScaffale(L + 18 * d, h / 2, a);
                {   // tazzina e luna: compaiono e spariscono con una dissolvenza e crescendo un poco. Stanno attaccate ai
                    // bordi VERI del notch (che con la molla si allarga o si stringe), non a quelli finali: prima, spegnendone
                    // una, saltava subito verso il centro e poi spariva
                    const float as = alfaSegno(segnoSveglio, tSegnoSveglio), al = alfaSegno(segnoSilenzio, tSegnoSilenzio);
                    const float Lv = (W2 - (float)mw.x) / 2, Rv = Lv + (float)mw.x;
                    if (as > 0.01f) {
                        const D2D1_POINT_2F c = {Lv + (scaffale.empty() ? 17 : 46) * d, h / 2};
                        rt->SetTransform(D2D1::Matrix3x2F::Scale(0.6f + 0.4f * as, 0.6f + 0.4f * as, c));
                        tazzina(c.x, c.y, 0.8f * a * as);
                        rt->SetTransform(D2D1::Matrix3x2F::Identity());
                    }
                    if (al > 0.01f) {
                        const D2D1_POINT_2F c = {Rv - 17 * d, h / 2};
                        rt->SetTransform(D2D1::Matrix3x2F::Scale(0.6f + 0.4f * al, 0.6f + 0.4f * al, c));
                        lunetta(c.x, c.y, 0.8f * a * al);
                        rt->SetTransform(D2D1::Matrix3x2F::Identity());
                    }
                }
                break;
            case AVVISO: {
                const float cx = L + 24 * d, cy = h / 2;
                if (simboloAvviso == sensori::S_BATTERIA) batteriaSimbolo(cx, cy, a, valoreAvviso, avvisoAcceso);
                else if (simboloAvviso == sensori::S_CUFFIE) cuffie(cx, cy, a);
                else puntina(cx, cy, a, avvisoAcceso);
                scrivi(testoAvviso, fPiccolo, {L + 44 * d, 0, L + w - 18 * d, h}, a);
                break;
            }
            case MUSICA: {
                ora(L, w, h, a);
                const float q = 14 * d, y = (h - q) / 2;   // copertina piccola a sinistra, barrette a destra
                if (bCopertina) immagine(bCopertina, {L + 12 * d, y, L + 12 * d + q, y + q}, 3.5f * d, a);
                else nota(L + 12 * d + q / 2, h / 2, a);
                barrette(L + w - 12 * d - 13 * d, h / 2, 13 * d, a);
                break;
            }
            case APERTO: aperto(L, w, h, a); break;
            case SCAFFALE: scaffaleScena(L, w, h, a); break;
            case TIMER: {   // ora al centro, anello del timer a sinistra e il tempo che resta a destra, in arancio
                ora(L, w, h, a);
                // comparsa: l'anello cresce dal centro e il tempo entra scivolando da destra
                const float ka = (float)std::clamp((adesso() - tScena - 0.08) / 0.32, 0.0, 1.0), e = ka * ka * (3 - 2 * ka);
                if (e > 0.01f) anello(L + 20 * d, h / 2, 6.5f * d * e, (float)comodita::timerAvanzamento(), a * e);
                colore(1.0f, 0.62f, 0.04f, a * e);
                const std::wstring r = comodita::durata(comodita::timerResto());
                rt->DrawText(r.c_str(), (UINT32)r.size(), fNumero, {L + w - 70 * d + (1 - e) * 12 * d, 0, L + w - 14 * d + (1 - e) * 12 * d, h}, pen, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                break;
            }
            case TIMER_APERTO: {   // timer grande: anello, tempo che resta, +1 min, stop
                const float cy = h / 2, ka = (float)std::clamp((adesso() - tScena - 0.06) / 0.3, 0.0, 1.0);
                const float e = ka * ka * (3 - 2 * ka);
                anello(L + 34 * d, cy, (8 + 6 * e) * d, (float)comodita::timerAvanzamento(), a);
                colore(1.0f, 0.62f, 0.04f, a);
                const std::wstring r = comodita::durata(comodita::timerResto());
                rt->DrawText(r.c_str(), (UINT32)r.size(), fGrande, {L + 60 * d + (1 - e) * 10 * d, 0, L + w - 126 * d, h}, pen, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                const D2D1_RECT_F rp = tastoTimer(0), rs = tastoTimer(1);
                colore(1, 1, 1, (pulsanteSopra == 0 ? 0.24f : 0.14f) * a);
                rt->FillRoundedRectangle({rp, 15 * d, 15 * d}, pen);
                const DWRITE_TEXT_ALIGNMENT al = fPiccolo->GetTextAlignment();
                fPiccolo->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                scrivi(testi::t(L"+1 min", L"+1 min"), fPiccolo, rp, a);
                fPiccolo->SetTextAlignment(al);
                colore(1.0f, 0.42f, 0.32f, (pulsanteSopra == 1 ? 1.0f : 0.85f) * a);
                rt->FillEllipse({{(rs.left + rs.right) / 2, cy}, 17 * d, 17 * d}, pen);
                colore(1, 1, 1, a);
                rt->FillRoundedRectangle({{(rs.left + rs.right) / 2 - 5 * d, cy - 5 * d, (rs.left + rs.right) / 2 + 5 * d, cy + 5 * d}, 1.5f * d, 1.5f * d}, pen);
                break;
            }
            case SUONA: {   // timer finito: cerchio arancio che pulsa
                const float cx = L + 26 * d, cy = h / 2;
                const float k = (float)(0.5 + 0.5 * std::sin(adesso() * 7));
                colore(1.0f, 0.62f, 0.04f, a * (0.25f + 0.25f * k));
                rt->FillEllipse({{cx, cy}, (11 + 3 * k) * d, (11 + 3 * k) * d}, pen);
                anello(cx, cy, 7.5f * d, 1, a);
                scrivi(testi::t(L"Timer finito", L"Timer done"), fTitolo, {L + 50 * d, 0, L + w - 110 * d, h}, a);
                scrivi(testi::t(L"Clic per fermare", L"Click to stop"), fPiccolo, {L + w - 112 * d, 0, L + w - 16 * d, h}, 0.6f * a);
                break;
            }
            case VOLUME: volumeScena(L, w, h, a); break;
            case NOTIFICA: notificaScena(L, w, h, a); break;
        }
    }

    // ora (piu' grande) e giorno su una riga, con la stessa linea di base; sotto il giorno una linea sottile
    void ora(float L, float w, float h, float a) {
        IDWriteFactory* fdw = fabbriche::testo();
        auto lineaBase = [&](IDWriteTextFormat* fmt, const std::wstring& t, float& alto) {
            IDWriteTextLayout* l = nullptr; float base = 0; alto = 0;
            if (SUCCEEDED(fdw->CreateTextLayout(t.c_str(), (UINT32)t.size(), fmt, 1000, 100, &l))) {
                DWRITE_LINE_METRICS lm{}; UINT32 n = 0; l->GetLineMetrics(&lm, 1, &n);
                base = lm.baseline; alto = lm.height; l->Release();
            }
            return base;
        };
        const float spazio = std::round(7 * dpi);
        const float xt = std::round(L + (w - (lo + spazio + lg)) / 2);
        float altoOra = 0, altoGiorno = 0;
        const float bOra = lineaBase(fOra, testoOra, altoOra), bGiorno = lineaBase(fGiorno, testoGiorno, altoGiorno);
        const float yOra = std::round((h - altoOra) / 2), baseOra = yOra + bOra;
        const float yGiorno = baseOra - 3 * dpi - bGiorno;
        scrivi(testoOra, fOra, {xt, yOra, xt + lo + 2, yOra + altoOra}, a);
        const float xg = xt + lo + spazio;
        scrivi(testoGiorno, fGiorno, {xg, yGiorno, xg + lg + 2, yGiorno + altoGiorno}, 0.66f * a);
        colore(1, 1, 1, 0.38f * a);
        rt->DrawLine({xg, baseOra + 0.5f}, {xg + lg, baseOra + 0.5f}, pen, 1.0f);
    }

    void lunetta(float cx, float cy, float a) {
        ID2D1Factory* f = fabbriche::d2d();
        const float r = 4.6f * dpi;
        ID2D1EllipseGeometry *e1 = nullptr, *e2 = nullptr; ID2D1PathGeometry* g = nullptr;
        f->CreateEllipseGeometry({{cx, cy}, r, r}, &e1);
        f->CreateEllipseGeometry({{cx + 2.8f * dpi, cy - 2.2f * dpi}, r * 0.85f, r * 0.85f}, &e2);
        if (e1 && e2 && SUCCEEDED(f->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr; g->Open(&s);
            e1->CombineWithGeometry(e2, D2D1_COMBINE_MODE_EXCLUDE, nullptr, s);
            s->Close(); s->Release();
            colore(0.62f, 0.55f, 1.0f, a);   // viola, come la luna di Non disturbare su iPhone
            rt->FillGeometry(g, pen);
        }
        for (IUnknown* o : {(IUnknown*)e1, (IUnknown*)e2, (IUnknown*)g}) if (o) o->Release();
    }

    void tazzina(float cx, float cy, float a) {
        const float d = dpi;
        colore(1.0f, 0.72f, 0.35f, a);   // arancio caldo
        rt->FillRoundedRectangle({{cx - 4 * d, cy - 1.5f * d, cx + 2.5f * d, cy + 4.5f * d}, 1.6f * d, 1.6f * d}, pen);
        rt->DrawEllipse({{cx + 3.2f * d, cy + 1.3f * d}, 1.7f * d, 1.7f * d}, pen, 1.1f * d);
        rt->DrawLine({cx - 2 * d, cy - 3 * d}, {cx - 2 * d, cy - 5.5f * d}, pen, 1.0f * d);
        rt->DrawLine({cx + 0.5f * d, cy - 3 * d}, {cx + 0.5f * d, cy - 5.5f * d}, pen, 1.0f * d);
    }

    // puntina (sempre in primo piano): accesa piena, spenta barrata
    void puntina(float cx, float cy, float a, bool acceso) {
        const float d = dpi;
        colore(1, 1, 1, a);
        rt->FillEllipse({{cx, cy - 4 * d}, 4.2f * d, 4.2f * d}, pen);
        rt->DrawLine({cx, cy - 1 * d}, {cx, cy + 7 * d}, pen, 1.6f * d);
        if (!acceso) {
            colore(0, 0, 0, a); rt->DrawLine({cx - 7 * d, cy - 7 * d}, {cx + 7 * d, cy + 7 * d}, pen, 3.2f * d);
            colore(1, 1, 1, a); rt->DrawLine({cx - 6 * d, cy - 6 * d}, {cx + 6 * d, cy + 6 * d}, pen, 1.4f * d);
        }
    }

    // ---- scaffale ----
    // File appoggiati sul notch (riferimenti: i file restano dove sono). Si trascinano sopra da qualsiasi app, si
    // riprendono trascinandoli fuori (uno o "Tutti"); dopo un trascinamento riuscito escono dallo scaffale.
    struct Oggetto { std::wstring percorso, nome; std::vector<UINT> px; int w = 0, h = 0; ID2D1Bitmap* b = nullptr; };
    std::vector<Oggetto> scaffale;
    bool trascinoSopra = false, trascinoMio = false;
    double fineScaffale = 0;
    int premutoX = 0, premutoY = 0, premutoS = -1;
    static constexpr int TUTTI = 1000, SVUOTA = 1001, MAX_VISTI = 6;

    struct Bersaglio : IDropTarget {
        Notch* n = nullptr; bool ok = false;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override { if (r == IID_IUnknown || r == IID_IDropTarget) { *p = this; return S_OK; } *p = nullptr; return E_NOINTERFACE; }
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* o, DWORD, POINTL, DWORD* e) override {
            FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            ok = o && !n->trascinoMio && n->modoAtt == NORMALE && o->QueryGetData(&f) == S_OK;
            if (ok) { n->trascinoSopra = true; n->bersagli(); }
            *e = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* e) override { *e = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
        HRESULT STDMETHODCALLTYPE DragLeave() override { if (n->trascinoSopra) { n->trascinoSopra = false; n->bersagli(); } return S_OK; }
        HRESULT STDMETHODCALLTYPE Drop(IDataObject* o, DWORD, POINTL, DWORD* e) override {
            *e = DROPEFFECT_NONE;
            n->trascinoSopra = false;
            if (ok && o) {
                FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM m{};
                if (SUCCEEDED(o->GetData(&f, &m))) {
                    if (HDROP hd = (HDROP)GlobalLock(m.hGlobal)) {
                        const UINT k = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                        for (UINT i = 0; i < k && i < 50; i++) { wchar_t p[MAX_PATH * 4]; if (DragQueryFileW(hd, i, p, MAX_PATH * 4)) n->appoggia(p); }
                        GlobalUnlock(m.hGlobal);
                    }
                    ReleaseStgMedium(&m);
                    *e = DROPEFFECT_COPY;   // copia: il file resta dov'era
                    n->fineScaffale = adesso() + 1.8;   // si vede un attimo cosa e' entrato
                }
            }
            n->bersagli(); n->ridisegna();
            return S_OK;
        }
    } bersaglio;

    void appoggia(const std::wstring& p) {
        for (auto& o : scaffale) if (!_wcsicmp(o.percorso.c_str(), p.c_str())) return;
        Oggetto o; o.percorso = p;
        const size_t s = p.find_last_of(L"\\/"); o.nome = s == std::wstring::npos ? p : p.substr(s + 1);
        IShellItemImageFactory* fab = nullptr;   // icona o anteprima (poche e piccole: subito)
        if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&fab)))) {
            HBITMAP hb = nullptr;
            const int lato = (int)std::lround(40 * dpi);
            if (SUCCEEDED(fab->GetImage({lato, lato}, SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
                BITMAP bm{}; GetObject(hb, sizeof(bm), &bm);
                if (bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 1024 && bm.bmHeight <= 1024) {
                    o.px.resize((size_t)bm.bmWidth * bm.bmHeight); o.w = bm.bmWidth; o.h = bm.bmHeight;
                    BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
                    HDC scr = GetDC(nullptr); GetDIBits(scr, hb, 0, bm.bmHeight, o.px.data(), &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, scr);
                    bool alfa = false; for (UINT x : o.px) if (x >> 24) { alfa = true; break; }
                    for (UINT& x : o.px) {
                        const UINT a = alfa ? (x >> 24) : 255;
                        x = (a << 24) | ((((x >> 16) & 255) * a / 255) << 16) | ((((x >> 8) & 255) * a / 255) << 8) | ((x & 255) * a / 255);
                    }
                }
                DeleteObject(hb);
            }
            fab->Release();
        }
        scaffale.push_back(std::move(o));
    }

    void svuotaScaffale() { for (auto& o : scaffale) if (o.b) o.b->Release(); scaffale.clear(); }

    // dove sta l'oggetto i dello scaffale (coordinate della finestra), "Tutti" e "Svuota"
    D2D1_RECT_F rettOggetto(int i) const {
        float w, h, R; misura(SCAFFALE, w, h, R);
        const float L = sinistra(SCAFFALE), d = dpi;
        if (i == TUTTI) return {L + w - 150 * d, 6 * d, L + w - 72 * d, 30 * d};
        if (i == SVUOTA) return {L + w - 68 * d, 6 * d, L + w - 18 * d, 30 * d};
        const float cw = (w - 40 * d) / MAX_VISTI;
        return {L + 20 * d + i * cw, 32 * d, L + 20 * d + (i + 1) * cw, 90 * d};
    }
    int oggettoIn(int x, int y) const {
        if (scaffale.empty()) return -1;
        for (int i : {TUTTI, SVUOTA}) { const D2D1_RECT_F r = rettOggetto(i); if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i; }
        for (int i = 0; i < (int)std::min<size_t>(scaffale.size(), MAX_VISTI); i++) {
            const D2D1_RECT_F r = rettOggetto(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        return -1;
    }

    // trascina fuori un oggetto (o tutti): un trascinamento vero di Windows con i file della shell
    void trascinaFuori(int i) {
        std::vector<size_t> quali;
        if (i == TUTTI) for (size_t k = 0; k < scaffale.size(); k++) quali.push_back(k);
        else if (i >= 0 && i < (int)scaffale.size()) quali.push_back(i);
        else return;
        std::vector<PIDLIST_ABSOLUTE> id;
        for (size_t k : quali) { PIDLIST_ABSOLUTE p = nullptr; if (SUCCEEDED(SHParseDisplayName(scaffale[k].percorso.c_str(), nullptr, &p, 0, nullptr)) && p) id.push_back(p); }
        IShellItemArray* arr = nullptr; IDataObject* dati = nullptr;
        if (!id.empty() && SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)id.size(), (PCIDLIST_ABSOLUTE_ARRAY)id.data(), &arr)) && arr &&
            SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dati))) && dati) {
            trascinoMio = true;
            DWORD effetto = DROPEFFECT_NONE;
            const HRESULT r = DoDragDrop(dati, &fonte, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effetto);
            trascinoMio = false;
            if (r == DRAGDROP_S_DROP && effetto != DROPEFFECT_NONE) {   // arrivato: esce dallo scaffale (nel dubbio resta)
                for (auto it = quali.rbegin(); it != quali.rend(); ++it) { if (scaffale[*it].b) scaffale[*it].b->Release(); scaffale.erase(scaffale.begin() + *it); }
            }
            dati->Release();
        }
        if (arr) arr->Release();
        for (auto p : id) CoTaskMemFree(p);
        bersagli(); ridisegna();
    }

    struct Fonte : IDropSource {
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override { if (r == IID_IUnknown || r == IID_IDropSource) { *p = this; return S_OK; } *p = nullptr; return E_NOINTERFACE; }
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD k) override { return esc ? DRAGDROP_S_CANCEL : !(k & MK_LBUTTON) ? DRAGDROP_S_DROP : S_OK; }
        HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
    } fonte;

    // segno nel notch a riposo: un vassoio con il numero dei file
    void segnoScaffale(float cx, float cy, float a) {
        const float d = dpi;
        colore(0.40f, 0.68f, 1.0f, a);
        rt->DrawLine({cx - 8 * d, cy - 1 * d}, {cx - 8 * d, cy + 4 * d}, pen, 1.4f * d);
        rt->DrawLine({cx - 8 * d, cy + 4 * d}, {cx + 1 * d, cy + 4 * d}, pen, 1.4f * d);
        rt->DrawLine({cx + 1 * d, cy + 4 * d}, {cx + 1 * d, cy - 1 * d}, pen, 1.4f * d);
        rt->DrawLine({cx - 3.5f * d, cy - 6 * d}, {cx - 3.5f * d, cy + 1 * d}, pen, 1.4f * d);
        rt->DrawLine({cx - 6 * d, cy - 1.5f * d}, {cx - 3.5f * d, cy + 1 * d}, pen, 1.4f * d);
        rt->DrawLine({cx - 1 * d, cy - 1.5f * d}, {cx - 3.5f * d, cy + 1 * d}, pen, 1.4f * d);
        wchar_t b[8]; swprintf(b, 8, L"%d", (int)scaffale.size());
        colore(1, 1, 1, 0.85f * a);
        rt->DrawText(b, (UINT32)wcslen(b), fPiccolo, {cx + 4 * d, cy - 8 * d, cx + 22 * d, cy + 8 * d}, pen, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void scaffaleScena(float L, float w, float h, float a) {
        const float d = dpi;
        if (scaffale.empty() || trascinoSopra) {   // zona dove lasciare i file
            const D2D1_RECT_F z = {L + 20 * d, (scaffale.empty() ? 14 : 32) * d, L + w - 20 * d, h - 10 * d};
            ID2D1StrokeStyle* tr = nullptr;
            const float trattini[] = {4, 3};
            fabbriche::d2d()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM, 0), trattini, 2, &tr);
            colore(0.40f, 0.68f, 1.0f, (trascinoSopra ? 0.9f : 0.5f) * a);
            rt->DrawRoundedRectangle({z, 14 * d, 14 * d}, pen, 1.5f * d, tr);
            if (tr) tr->Release();
            if (scaffale.empty()) {
                const DWRITE_TEXT_ALIGNMENT al = fTitolo->GetTextAlignment();
                fTitolo->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                scrivi(testi::t(L"Lascia qui i file", L"Drop files here"), fTitolo, z, a);
                fTitolo->SetTextAlignment(al);
                return;
            }
        }
        scrivi(testi::t(L"Scaffale", L"Shelf"), fPiccolo, {L + 22 * d, 6 * d, L + 150 * d, 30 * d}, 0.6f * a);
        scrivi(testi::t(L"Tutti", L"All"), fPiccolo, rettOggetto(TUTTI), 0.85f * a);
        scrivi(testi::t(L"Svuota", L"Clear"), fPiccolo, rettOggetto(SVUOTA), 0.6f * a);
        const int vis = (int)std::min<size_t>(scaffale.size(), MAX_VISTI);
        for (int i = 0; i < vis; i++) {
            Oggetto& o = scaffale[i];
            if (!o.b) bitmap(rt, o.b, o.px, o.w, o.h);
            const D2D1_RECT_F r = rettOggetto(i);
            const float cx = (r.left + r.right) / 2, l = 17 * d;
            if (o.b) {
                const D2D1_SIZE_F s = o.b->GetSize();
                const float f = std::min(2 * l / s.width, 2 * l / s.height);
                rt->DrawBitmap(o.b, {cx - s.width * f / 2, r.top + 2 * d + l - s.height * f / 2, cx + s.width * f / 2, r.top + 2 * d + l + s.height * f / 2}, a);
            }
            std::wstring nome = o.nome;
            if (i == MAX_VISTI - 1 && scaffale.size() > (size_t)MAX_VISTI) { wchar_t b[24]; swprintf(b, 24, L"+%d", (int)(scaffale.size() - MAX_VISTI + 1)); nome = b; }
            DWRITE_TEXT_ALIGNMENT al = fPiccolo->GetTextAlignment();
            fPiccolo->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            scrivi(nome, fPiccolo, {r.left + 2 * d, r.top + 40 * d, r.right - 2 * d, r.bottom}, 0.85f * a);
            fPiccolo->SetTextAlignment(al);
        }
    }

    // anello del timer: grigio pieno, arancio la parte gia' passata (da mezzogiorno in senso orario)
    void anello(float cx, float cy, float r, float k, float a) {
        colore(1, 1, 1, 0.22f * a);
        rt->DrawEllipse({{cx, cy}, r, r}, pen, 2.2f * dpi);
        k = std::clamp(k, 0.0f, 1.0f);
        colore(1.0f, 0.62f, 0.04f, a);
        if (k >= 0.999f) { rt->DrawEllipse({{cx, cy}, r, r}, pen, 2.2f * dpi); return; }
        if (k <= 0.001f) return;
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (FAILED(fabbriche::d2d()->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { if (g) g->Release(); return; }
        const float ang = k * 2 * 3.14159265f;
        s->BeginFigure({cx, cy - r}, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc(D2D1::ArcSegment({cx + r * std::sin(ang), cy - r * std::cos(ang)}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                   k > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
        rt->DrawGeometry(g, pen, 2.2f * dpi); g->Release();
    }

    // batteria: contorno, riempimento (verde in carica, rosso sotto il 20%) e il polo
    void batteriaSimbolo(float cx, float cy, float a, int pc, bool carica) {
        const float d = dpi, w = 18 * d, h = 9 * d, x = cx - w / 2 - 1 * d, y = cy - h / 2;
        colore(1, 1, 1, 0.55f * a);
        rt->DrawRoundedRectangle({{x, y, x + w, y + h}, 2.5f * d, 2.5f * d}, pen, 1.2f * d);
        rt->FillRoundedRectangle({{x + w + 1 * d, cy - 2 * d, x + w + 2.6f * d, cy + 2 * d}, 0.8f * d, 0.8f * d}, pen);
        const float k = std::clamp(pc < 0 ? 0.5f : pc / 100.0f, 0.08f, 1.0f);
        if (carica) colore(0.2f, 0.84f, 0.29f, a); else if (pc >= 0 && pc <= 20) colore(1.0f, 0.27f, 0.23f, a); else colore(1, 1, 1, a);
        rt->FillRoundedRectangle({{x + 2 * d, y + 2 * d, x + 2 * d + (w - 4 * d) * k, y + h - 2 * d}, 1.2f * d, 1.2f * d}, pen);
    }

    // cuffie: archetto e due padiglioni
    void cuffie(float cx, float cy, float a) {
        const float d = dpi;
        colore(1, 1, 1, a);
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(fabbriche::d2d()->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            s->BeginFigure({cx - 7 * d, cy + 2 * d}, D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddArc(D2D1::ArcSegment({cx + 7 * d, cy + 2 * d}, {7 * d, 7 * d}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
            rt->DrawGeometry(g, pen, 1.6f * d);
        }
        if (g) g->Release();
        rt->FillRoundedRectangle({{cx - 8.5f * d, cy + 0.5f * d, cx - 4.5f * d, cy + 7.5f * d}, 1.5f * d, 1.5f * d}, pen);
        rt->FillRoundedRectangle({{cx + 4.5f * d, cy + 0.5f * d, cx + 8.5f * d, cy + 7.5f * d}, 1.5f * d, 1.5f * d}, pen);
    }

    // nota musicale (al posto della copertina che manca)
    void nota(float cx, float cy, float a) {
        const float d = dpi;
        colore(1, 1, 1, 0.85f * a);
        rt->FillEllipse({{cx - 2.5f * d, cy + 3.5f * d}, 2.6f * d, 2.1f * d}, pen);
        rt->DrawLine({cx + 0.0f * d, cy + 3.5f * d}, {cx + 0.0f * d, cy - 5.5f * d}, pen, 1.3f * d);
        rt->DrawLine({cx + 0.0f * d, cy - 5.5f * d}, {cx + 4.0f * d, cy - 3.5f * d}, pen, 1.3f * d);
    }

    // barrette che ballano mentre suona (ferme e basse in pausa)
    void barrette(float x, float cy, float largo, float a) {
        const float d = dpi, lb = 2.4f * d, passo = (largo - lb) / 3;
        const double t = adesso();
        for (int i = 0; i < 4; i++) {
            float k = 0.25f;
            if (brano.suona) k = (float)(0.35 + 0.65 * std::fabs(std::sin(t * (5.1 + i * 1.7) + i * 1.3)));
            const float hb = std::max(2.4f * d, 12 * d * k);
            colore(1, 1, 1, 0.9f * a);
            rt->FillRoundedRectangle({{x + i * passo, cy - hb / 2, x + i * passo + lb, cy + hb / 2}, lb / 2, lb / 2}, pen);
        }
    }

    void aperto(float L, float w, float h, float a) {
        const float d = dpi, q = 62 * d, y = 17 * d;
        const D2D1_RECT_F rc = {L + 20 * d, y, L + 20 * d + q, y + q};
        if (bCopertina) immagine(bCopertina, rc, 12 * d, a);
        else {
            colore(1, 1, 1, 0.12f * a); rt->FillRoundedRectangle({rc, 12 * d, 12 * d}, pen);
            nota((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2, a);
        }
        const float tx = L + 100 * d, tr = L + w - 22 * d;
        scrivi(brano.titolo.empty() ? std::wstring(testi::t(L"In riproduzione", L"Now playing")) : brano.titolo, fTitolo, {tx, 14 * d, tr - 22 * d, 34 * d}, a);
        scrivi(brano.artista, fPiccolo, {tx, 34 * d, tr, 50 * d}, 0.62f * a);
        barrette(tr - 13 * d, 24 * d, 13 * d, a);   // (stessa posizione di zonaBarrette)
        for (int i = 0; i < 3; i++) {
            const D2D1_POINT_2F c = centroTasto(i);
            if (i == pulsanteSopra) { colore(1, 1, 1, 0.16f * a); rt->FillEllipse({c, 17 * d, 17 * d}, pen); }
            colore(1, 1, 1, a);
            if (i == 1) {
                if (brano.suona) {   // pausa
                    rt->FillRoundedRectangle({{c.x - 6 * d, c.y - 8 * d, c.x - 2 * d, c.y + 8 * d}, 1.2f * d, 1.2f * d}, pen);
                    rt->FillRoundedRectangle({{c.x + 2 * d, c.y - 8 * d, c.x + 6 * d, c.y + 8 * d}, 1.2f * d, 1.2f * d}, pen);
                } else triangolo(c.x - 5 * d, c.y, 14 * d, 16 * d);
            } else {
                const float dx = i == 2 ? 1.0f : -1.0f;   // avanti: verso destra
                triangolo(c.x - dx * 8 * d, c.y, 9 * d * dx, 11 * d);   // due triangoli verso la direzione
                triangolo(c.x + dx * 0.5f * d, c.y, 9 * d * dx, 11 * d);
            }
        }
    }

    // triangolo pieno con la base verticale in x e la punta a x+largo (largo negativo: punta a sinistra)
    void triangolo(float x, float cy, float largo, float alto) {
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (FAILED(fabbriche::d2d()->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { if (g) g->Release(); return; }
        s->BeginFigure({x, cy - alto / 2}, D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F p[] = {{x + largo, cy}, {x, cy + alto / 2}};
        s->AddLines(p, 2); s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
        rt->FillGeometry(g, pen); g->Release();
    }

    void volumeScena(float L, float w, float h, float a) {
        const float d = dpi, cy = h / 2, sx = L + 22 * d;
        colore(1, 1, 1, a);
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(fabbriche::d2d()->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {   // altoparlante
            s->BeginFigure({sx - 6 * d, cy - 3.2f * d}, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F p[] = {{sx - 2 * d, cy - 3.2f * d}, {sx + 3 * d, cy - 7.5f * d}, {sx + 3 * d, cy + 7.5f * d}, {sx - 2 * d, cy + 3.2f * d}, {sx - 6 * d, cy + 3.2f * d}};
            s->AddLines(p, 5); s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
            rt->FillGeometry(g, pen);
        }
        if (g) g->Release();
        const float liv = muto ? 0.0f : livelloVis;
        if (muto) {
            rt->DrawLine({sx + 7 * d, cy - 4 * d}, {sx + 14 * d, cy + 4 * d}, pen, 1.7f * d);
            rt->DrawLine({sx + 14 * d, cy - 4 * d}, {sx + 7 * d, cy + 4 * d}, pen, 1.7f * d);
        } else {
            for (int i = 0; i < 2; i++) {
                if (liv < (i == 0 ? 0.01f : 0.5f)) break;
                const float r = (i == 0 ? 6.0f : 10.5f) * d;
                ID2D1PathGeometry* ar = nullptr; ID2D1GeometrySink* as = nullptr;
                if (FAILED(fabbriche::d2d()->CreatePathGeometry(&ar)) || FAILED(ar->Open(&as))) { if (ar) ar->Release(); break; }
                as->BeginFigure({sx + 3 * d + r * 0.6f, cy - r * 0.8f}, D2D1_FIGURE_BEGIN_HOLLOW);
                as->AddArc(D2D1::ArcSegment({sx + 3 * d + r * 0.6f, cy + r * 0.8f}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                as->EndFigure(D2D1_FIGURE_END_OPEN); as->Close(); as->Release();
                rt->DrawGeometry(ar, pen, 1.7f * d); ar->Release();
            }
        }
        const D2D1_RECT_F b = {L + 52 * d, cy - 3.5f * d, L + w - 58 * d, cy + 3.5f * d};
        colore(1, 1, 1, 0.2f * a);
        rt->FillRoundedRectangle({b, 3.5f * d, 3.5f * d}, pen);
        if (liv > 0.005f) {
            colore(1, 1, 1, a);
            rt->FillRoundedRectangle({{b.left, b.top, b.left + std::max(7 * d, (b.right - b.left) * liv), b.bottom}, 3.5f * d, 3.5f * d}, pen);
        }
        wchar_t t[16]; swprintf(t, 16, muto ? testi::t(L"Muto", L"Muted") : L"%d", (int)std::lround(livello * 100));
        scrivi(t, fNumero, {L + w - 56 * d, 0, L + w - 18 * d, h}, a);
    }

    void notificaScena(float L, float w, float h, float a) {
        const float d = dpi, il = 40 * d, ix = L + 20 * d, iy = (h - il) / 2;
        if (bIcona) immagine(bIcona, {ix, iy, ix + il, iy + il}, 9 * d, a);
        else { colore(1, 1, 1, 0.14f * a); rt->FillRoundedRectangle({{ix, iy, ix + il, iy + il}, 9 * d, 9 * d}, pen); }
        const float tx = ix + il + 14 * d, tr = L + w - 22 * d;
        scrivi(nApp.empty() ? std::wstring(testi::t(L"Notifica", L"Notification")) : nApp, fPiccolo, {tx, 12 * d, tr - 26 * d, 28 * d}, 0.6f * a);
        scrivi(nTitolo, fTitolo, {tx, 28 * d, tr, 48 * d}, a);
        scrivi(nTesto, fTesto, {tx, 48 * d, tr, 66 * d}, 0.8f * a);
        if (mouseDentro) {   // la x per toglierla, solo col mouse sopra
            const float cx = L + w - 24 * d, cy = 20 * d;
            colore(1, 1, 1, (sopraX ? 0.26f : 0.13f) * a);
            rt->FillEllipse({{cx, cy}, 10 * d, 10 * d}, pen);
            colore(1, 1, 1, a);
            rt->DrawLine({cx - 3.5f * d, cy - 3.5f * d}, {cx + 3.5f * d, cy + 3.5f * d}, pen, 1.5f * d);
            rt->DrawLine({cx + 3.5f * d, cy - 3.5f * d}, {cx - 3.5f * d, cy + 3.5f * d}, pen, 1.5f * d);
        }
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Notch* n = (Notch*)GetWindowLongPtrW(h, 0);
        if (m == vetro::Battito::io().msg && m) {   // un fotogramma dell'animazione
            vetro::Battito::io().ricevuto(h, (UINT)w);
            if (n && n->animando) n->fotogramma();
            return 0;
        }
        switch (m) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_TIMER: if (n && w == 2) n->controlla(); return 0;
            case WM_LBUTTONDOWN: if (n) { n->premutoX = (short)LOWORD(l); n->premutoY = (short)HIWORD(l); n->premutoS = n->scena == SCAFFALE ? n->oggettoIn(n->premutoX, n->premutoY) : -1; } return 0;
            case WM_MOUSEMOVE:   // trascinamento fuori dallo scaffale
                if (n && n->premutoS != -1 && (w & MK_LBUTTON) && (std::abs((short)LOWORD(l) - n->premutoX) > 5 || std::abs((short)HIWORD(l) - n->premutoY) > 5)) {
                    const int s = n->premutoS; n->premutoS = -1; n->trascinaFuori(s);
                }
                return 0;
            case WM_LBUTTONUP: if (n) { const bool premuto = n->premutoS != -1 || n->scena != SCAFFALE; n->premutoS = -1; if (premuto) n->clicca((short)LOWORD(l), (short)HIWORD(l)); } return 0;
            case WM_CLOSE: return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

}  // namespace notch
