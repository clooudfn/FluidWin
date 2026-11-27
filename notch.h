// Notch di FluidWin: in alto al centro dello schermo del dock, nero come il notch del Mac e dell'iPhone e attaccato al
// bordo in alto (ai lati si raccorda al bordo con due curve all'indietro, sotto ha gli angoli tondi).
// A riposo mostra ora e giorno. Si trasforma, con una molla, per mostrare:
//  - il volume, mentre si premono i tasti (al posto dell'indicatore di vetro, osd.h);
//  - le notifiche, che escono dal notch (al posto del banner di vetro, banner.h);
//  - la musica in riproduzione (musica.h): copertina e barrette accanto all'ora; col mouse fermo sopra si apre con
//    titolo, artista e i tasti indietro / play-pausa / avanti.
// Il mouse di solito lo attraversa (sta dove si afferrano le finestre ingrandite): diventa cliccabile solo col mouse
// fermo sopra per un attimo (senza tasti premuti) o mentre mostra una notifica. Un clic sull'ora apre le notifiche.
// Con un'app a schermo intero (video, presentazioni, giochi) non c'e' (vedi modo()): il volume lo mostra allora
// l'indicatore di vetro.
// Notch spento nelle impostazioni (1.3.0, "a scomparsa"): a riposo (ora, musica, timer) sta sopra il bordo dello schermo;
// per volume, notifiche, avvisi, sveglia del timer e scaffale scende dall'alto e poi risale. Musica e timer li mostra il
// dock (una capsula all'inizio, con i loro controlli al passaggio del mouse: comandoMusica()).
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

// schermo intero: niente, o un'app a schermo intero (video, presentazione, gioco). Prima sotto un video restava
// visibile e sfumava solo col mouse vicino: coprendo il film dava fastidio, ora sparisce come con i giochi
enum Modo { NORMALE = 0, NASCOSTO = 2 };

class Notch {
public:
    inline static void (*registroScaffale)(const char*, unsigned long) = nullptr;
    std::function<void()> clic;             // clic sull'ora: le notifiche
    std::function<void(const std::wstring&)> apriMusica;   // clic sulla musica: l'app che suona (AUMID dato da Windows)
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

    // schermo intero: NORMALE, NASCOSTO (qualsiasi app a schermo intero: video, presentazioni, giochi)
    void modo(int m) {
        if (m == modoAtt) return;
        modoAtt = m;
        if (m != NORMALE) { sopra = false; attesa = 0; finisciNotifica(); }   // sopra un'app a schermo intero niente notifiche
        visibilita();
        cliccabile(false);
        bersagli(); ridisegna();
    }

    // notch acceso (sempre visibile) o spento dalle impostazioni: spento e' "a scomparsa", compare solo quando ha
    // qualcosa da mostrare (vedi l'inizio del file). Prima spento non si vedeva mai e volume e notifiche passavano al vetro
    // (indicatore e banner): ora il vetro resta solo sopra le app a schermo intero (modo NASCOSTO)
    // Accendendolo scende dal bordo, spegnendolo risale (la stessa molla degli eventi a scomparsa): prima cambiava di colpo
    void abilita(bool si) {
        if (si == abilitato) return;
        abilitato = si;
        if (!si) sopra = false;
        superficie();         // (spento le ali dei bordi animati vogliono una superficie piu' larga)
        sincronizzaSegni();   // luna e tazzina gia' giuste mentre scende, senza aspettare il prossimo giro di controlla
        bersagli(); anima();
        if (!animando) ridisegna();   // (non visibile: niente animazione, solo il disegno)
    }
    bool fisso() const { return abilitato; }
    void aggiornaAngoli() { ridisegna(); }   // impostazione "Angoli" cambiata (vedi ridisegna)
    // col dock in alto il notch sta sul bordo in basso (impostazione "Posizione" del dock)
    bool basso = false;
    void inBasso(bool b) { if (b != basso) { basso = b; ridisegna(); } }

    // musica e timer per la capsula del dock (notch a scomparsa)
    const musica::Brano& musicaOra() const { return brano; }
    // tasti della musica dalla capsula del dock: 0 indietro, 1 play/pausa, 2 avanti
    void comandoMusica(int b) {
        if (b < 0 || b > 2) return;
        lettore.comando(b == 0 ? musica::Lettore::INDIETRO : b == 1 ? musica::Lettore::PLAY_PAUSA : musica::Lettore::AVANTI);
        if (b == 1) brano.suona = !brano.suona;   // subito, senza aspettare la prossima lettura
        // fra poco si rilegge lo stato vero anche se il lettore non ha cambiato versione: un comando rifiutato
        // lasciava il simbolo al contrario per sempre (R20261121-002056-S11)
        risincronizzaAlle = adesso() + 1.5;
        if (visibile && !animando && salita() < 0.5) ridisegna();
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
        // la pausa conta dall'arrivo: stesso istante per la scadenza e per il primo controllo. Prima il primo controllo
        // aggiungeva anche il tempo di prima (R20261121-120306-S3); con 0 invece perdeva quello fra arrivo e primo
        // controllo (R20261121-123420-S3)
        const double ora = adesso();
        notificaAttiva = true; fineNotifica = ora + 5.5; tControllo = ora;
        tOnda = adesso();
        bersagli(); anima();
        return true;
    }

    // c'e' una notifica in mostra (o in attesa che il mouse lasci il notch)
    bool notificaInCorso() const { return notificaAttiva; }

    // avviso breve (1,6 s, 2,5 per batteria e cuffie, 3,2 per AI): un simbolo (sensori::Simbolo) e un testo, per esempio
    // "Sempre in primo piano" o "AirPods · 80%"; valore: la batteria (-1 se non si sa). false se il notch non si vede
    bool avviso(const std::wstring& testo, int simbolo, int valore, bool acceso) {
        if (!visibile) return false;
        testoAvviso = testo; avvisoAcceso = acceso; simboloAvviso = simbolo; valoreAvviso = valore;
        tOnda = adesso();
        larghezzaAvviso = fOra ? larghezza(testo, fPiccolo ? fPiccolo : fOra) : 160 * dpi;
        fineAvviso = adesso() + (simbolo == sensori::S_PUNTINA ? 1.6 : simbolo == sensori::S_AI ? 3.2 : 2.5);
        bersagli(); anima();
        return true;
    }

private:
    enum Scena { ORA, MUSICA, APERTO, VOLUME, NOTIFICA, AVVISO, TIMER, SUONA, SCAFFALE, TIMER_APERTO };
    enum Bordo { CLASSICO, MAREA, ECO, VELO };
    static constexpr double DURATA_ECO = 0.78;
    double tOnda = 0;
    bool sondaFile = false;
    Bordo bordo() const {
        if (abilitato) return CLASSICO;
        if (scena == VOLUME) return MAREA;
        if (scena == SCAFFALE) return VELO;
        if (scena == NOTIFICA) return ECO;
        if (scena == AVVISO) {
            if (simboloAvviso == sensori::S_ALIMENTAZIONE) return avvisoAcceso ? ECO : MAREA;
            if (simboloAvviso == sensori::S_CUFFIE || simboloAvviso == sensori::S_BATTERIA || simboloAvviso == sensori::S_AI) return ECO;
        }
        return CLASSICO;
    }
    RECT zonaFile() const {
        int cx = posX() + W2 / 2, sx = (int)std::round(100 * dpi), h = (int)std::round(18 * dpi);
        return {cx - sx, basso ? schermo.bottom - h : schermo.top, cx + sx, basso ? schermo.bottom : schermo.top + h};
    }

    HWND fin = nullptr;
    bool acceso = false, visibile = false, abilitato = true;
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

    // forma: larghezza, altezza e raggio inseguono il bersaglio con una molla (rimbalza un poco, come una gelatina).
    // 320 e 0,76: piu' morbida di prima (420 e 0,70), con meno rimbalzo (circa 2,5% invece di 4,6%)
    struct Molla {
        double x = 0, v = 0, obj = 0;
        bool passo(double dt) {
            const double k = 320, c = 2 * std::sqrt(k) * 0.76;
            v += (-k * (x - obj) - c * v) * dt; x += v * dt;
            if (std::fabs(x - obj) < 0.3 && std::fabs(v) < 3) { x = obj; v = 0; return false; }
            return true;
        }
    };
    Molla mw, mh, mr;
    Molla my;   // a scomparsa: quanto la forma e' salita sopra il bordo dello schermo (0 = giu', tutta visibile)
    Molla distensione;  // avvicina le ali al limite della superficie durante l'uscita
    int scena = ORA, scenaPrima = ORA;
    double tScena = 0, tPrec = 0;
    bool animando = false;
    D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Identity();   // a scomparsa: la salita sopra il bordo (contenuti)
    float alfa = 1;   // alfa della finestra (sempre piena: UpdateLayeredWindow la usa a ogni fotogramma)

    // contenuti
    float livello = 0, livelloVis = 0; bool muto = false; double fineVolume = 0;
    static constexpr float AV_SIMBOLO = 22, AV_SPAZIO = 9, AV_MARGINE = 18;   // avviso: simbolo, spazio, margini (px a 100%)
    std::wstring testoAvviso; bool avvisoAcceso = false; double fineAvviso = 0; float larghezzaAvviso = 0;
    int simboloAvviso = 0, valoreAvviso = -1;
    ULONGLONG secondoTimer = 0; UINT versioneSensori = 0;
    bool segnoSilenzio = false, segnoSveglio = false;   // lunetta e tazzina accanto all'ora (comodita.h)
    double tSegnoSilenzio = -10, tSegnoSveglio = -10;   // quando sono comparse o sparite (dissolvenza)
    // (in uscita piu' in fretta: il notch intanto si stringe e il segno non deve arrivare sopra l'ora)
    float alfaSegno(bool acceso, double t0) const { const float k = (float)std::clamp((adesso() - t0) / (acceso ? 0.3 : 0.14), 0.0, 1.0); const float e = k * k * (3 - 2 * k); return acceso ? e : 1 - e; }
    bool notificaAttiva = false; double fineNotifica = 0, tControllo = 0;
    std::wstring nApp, nTitolo, nTesto; std::vector<UINT> nPx; int nW = 0, nH = 0; bool iconaNuova = false;
    musica::Lettore lettore; musica::Brano brano; UINT versioneBrano = 0xFFFFFFFF; bool copertinaNuova = false;
    double risincronizzaAlle = 0;   // dopo play/pausa: quando rileggere lo stato vero (vedi comandoMusica)

    // mouse
    bool sopra = false, cliccabileOra = false;
    int attesa = 0, fuori = 0, pulsanteSopra = -1;
    bool sopraX = false, mouseDentro = false;
    ULONGLONG tCima = 0;
    // il mouse sopra conta solo col notch fisso: a scomparsa musica e timer li controlla il dock, e passare sopra una
    // notifica non deve lasciare aperta la musica quando la notifica finisce
    bool sopraVale() const { return sopra && abilitato; }
    static bool aRiposo(int s) { return s == ORA || s == MUSICA || s == TIMER; }

    std::vector<IDWriteTextFormat*> formati() const { return {fOra, fGiorno, fTitolo, fPiccolo, fTesto, fNumero, fGrande}; }

    static double adesso() { return vetro::secondi(); }

    void visibilita() {
        const bool si = acceso && modoAtt != NASCOSTO && fin;   // (spento: a scomparsa, la finestra c'e' lo stesso)
        if (si == visibile) return;
        visibile = si;
        if (si) {
            KillTimer(fin, 2); SetTimer(fin, 2, 50, nullptr);   // mouse, musica, scadenze
            sincronizzaSegni();   // (cambiati mentre era nascosto: controlla non girava)
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
        if (trascinoSopra || (abilitato ? t < fineScaffale : !scaffale.empty())) return SCAFFALE;
        // col mouse sopra (solo col notch fisso): il timer grande (+1 min e stop), lo scaffale, la musica aperta
        const bool sv = sopraVale();
        if (sv && comodita::timerAttivo()) return TIMER_APERTO;
        if (sv && !scaffale.empty()) return SCAFFALE;
        if (sv && brano.c) return APERTO;
        if (comodita::timerAttivo()) return TIMER;
        if (brano.c && brano.suona) return MUSICA;
        return ORA;
    }
    // Microfono e webcam in uso: pallini arancione e verde, come su iPhone. Solo col notch acceso: stanno dentro, a destra,
    // in uno spazio che il notch a riposo (ora, musica, timer) si tiene in piu'. Prima stavano fuori, nel vuoto accanto al
    // notch; col notch spento non ci sono (l'utente non li voleva nel dock)
    int nPunti() const {
        if (!abilitato) return 0;
        const sensori::Sensori& se = sensori::Sensori::io();
        return (se.micApp.empty() ? 0 : 1) + (se.camApp.empty() ? 0 : 1);
    }
    float puntiW() const { const int n = nPunti(); return n ? (8.0f + 8.0f * n) * dpi : 0.0f; }
    // con musica e timer a riposo: posto per tazzina (a sinistra) e luna (a destra), uguale ai due lati. Prima i due
    // segni c'erano solo con l'ora: con un brano in riproduzione non si vedevano mai (non disturbare acceso e nessun segno)
    float segniW() const { return (segnoSveglio || segnoSilenzio) ? 22 * dpi : 0.0f; }
    // s: la scena. Con l'ora i pallini stanno a meta' fra la fine della data e il bordo (stesso spazio dai due lati; con
    // la luna accesa fra lei e il bordo); con musica e timer nel loro spazio a destra
    void puntiSensori(int s, float h, float a) {
        const int n = nPunti();
        if (!n) return;
        const sensori::Sensori& se = sensori::Sensori::io();
        const float d = dpi, Rv = (W2 + (float)mw.x) / 2;   // (il bordo vero, che con la molla si muove)
        float x0 = Rv - puntiW();                            // inizio dello spazio libero a destra
        if (s == ORA) x0 = segnoSilenzio ? Rv - puntiW() - 8 * d : W2 / 2.0f + (lo + std::round(7 * d) + lg) / 2;   // (fine della data, vedi ora)
        float cx = (x0 + Rv) / 2 - (n - 1) * 4 * d;
        if (!se.micApp.empty()) { colore(1.0f, 0.62f, 0.04f, a); rt->FillEllipse({{cx, h / 2}, 2.8f * d, 2.8f * d}, pen); cx += 8 * d; }
        if (!se.camApp.empty()) { colore(0.2f, 0.84f, 0.29f, a); rt->FillEllipse({{cx, h / 2}, 2.8f * d, 2.8f * d}, pen); }
    }

    void misura(int s, float& w, float& h, float& R) const {
        const float d = dpi;
        switch (s) {
            case MUSICA:   w = largoOra + 62 * d + 2 * puntiW() + 2 * segniW(); h = 22 * d; R = 9 * d; break;   // (puntiW: microfono e webcam)
            case APERTO:   w = 372 * d; h = 96 * d; R = 26 * d; break;
            case VOLUME:   w = 292 * d; h = 32 * d; R = 14 * d; break;
            case NOTIFICA: w = 372 * d; h = 78 * d; R = 26 * d; break;
            case SCAFFALE: w = 372 * d; h = 96 * d; R = 26 * d; break;
            // avviso: largo quanto simbolo e testo con un margine uguale ai due lati (vedi AVVISO in disegna)
            case AVVISO:   w = std::clamp(larghezzaAvviso + (AV_SIMBOLO + AV_SPAZIO + 2 * AV_MARGINE) * d, 150 * d, 372 * d); h = 32 * d; R = 14 * d; break;
            case TIMER:    w = largoOra + 104 * d + 2 * puntiW() + 2 * segniW(); h = 22 * d; R = 9 * d; break;
            case TIMER_APERTO: w = 312 * d; h = 60 * d; R = 24 * d; break;
            case SUONA:    w = 300 * d; h = 42 * d; R = 20 * d; break;
            default:       w = largoOra; h = 22 * d; R = 9 * d;
                           {   // posto per scaffale e tazzina (a sinistra) e luna (a destra), uguale ai due lati: l'ora resta al centro
                               const float sx = (!scaffale.empty() ? 36 * d : 0) + (segnoSveglio ? 22 * d : 0), dx = (segnoSilenzio ? 22 * d : 0) + puntiW();
                               w += 2 * std::max(sx, dx);
                           }
                           if (sopraVale()) { w += 14 * d; h += 5 * d; R = 11 * d; }   // col mouse sopra si gonfia un poco
        }
        if (!abilitato) {
            if (s == SCAFFALE && scaffale.empty()) { w = 196 * d; h = 32 * d; }
            if (s == VOLUME) { w = 200 * d; h = 28 * d; }   // (prima 216: compatto, vedi volumeScena)
            if (s == AVVISO && simboloAvviso != sensori::S_PUNTINA) { w = std::min(w, 316 * d); h = 28 * d; }
        }
        w = std::round(w); h = std::round(h); R = std::round(R);
    }

    // aggiorna scena e misure da raggiungere; 'subito' salta l'animazione
    // A scomparsa: verso una scena di riposo il notch prima risale mostrando ancora quella di prima, e solo quando e' tutto
    // sopra il bordo passa (di colpo, non si vede) a quella di riposo; verso un evento scende cambiando forma
    void bersagli(bool subito = false) {
        int s = scenaVoluta();
        const bool via = !abilitato && aRiposo(s);
        const bool salito = my.obj > 0 && my.x >= my.obj - 0.5 && my.v == 0;   // gia' tutto sopra il bordo, fermo
        if (via && !aRiposo(scena) && !subito && visibile && mw.x != 0 && !salito) s = scena;
        const bool scatto = via && salito && s != scena;   // (dietro il bordo: niente dissolvenza ne' molla)
        if (s != scena) { scenaPrima = scena; scena = s; tScena = adesso(); }
        float w, h, R; misura(scena, w, h, R);
        mw.obj = w; mh.obj = h; mr.obj = R;
        my.obj = via ? h + std::round(8 * dpi) : 0;   // (anche i raccordi, alti 6 px)
        distensione.obj = via && bordo() != CLASSICO ? 100.0 : 0.0;
        if (subito || scatto || !visibile || mw.x == 0) {
            mw.x = w; mh.x = h; mr.x = R; mw.v = mh.v = mr.v = 0; scenaPrima = scena;
            my.x = my.obj; my.v = 0;
            distensione.x = distensione.obj; distensione.v = 0;
            return;
        }
        if (mw.x != w || mh.x != h || mr.x != R || my.x != my.obj || scenaPrima != scena) anima();
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
        for (Molla* m : {&mw, &mh, &mr, &my}) ancora |= m->passo(dt);
        // Usa la stessa scala in pixel delle altre molle, anche per la soglia di arresto.
        ancora |= distensione.passo(dt);
        if (std::fabs(livelloVis - livello) > 0.002f) { livelloVis += (livello - livelloVis) * (float)std::min(1.0, dt * 18); ancora = true; }
        else livelloVis = livello;
        if (scenaPrima != scena) { if (t - tScena < 0.32) ancora = true; else scenaPrima = scena; }   // (dissolvenza: 0,06 + 0,24)
        if (bordo() == ECO && t - tOnda < DURATA_ECO) ancora = true;
        if ((scena == TIMER || scena == TIMER_APERTO) && t - tScena < 0.45) ancora = true;   // comparsa del timer
        if (t - tSegnoSveglio < 0.32 || t - tSegnoSilenzio < 0.32) ancora = true;           // tazzina e luna
        ridisegna();
        if (!ancora) { animando = false; vetro::fermaBattito(fin, 1); }
    }

    // ---- ogni 50 ms: mouse, musica, scadenze ----

    RECT corpo() const {   // la forma sullo schermo (coordinate dello schermo)
        const int x = posX() + (int)((W2 - mw.x) / 2);
        const int alto = std::max(0, (int)(mh.x - salita()));   // (a scomparsa la parte salita oltre il bordo non c'e')
        if (basso) return {x, (int)schermo.bottom - alto, x + (int)mw.x, (int)schermo.bottom};
        return {x, schermo.top, x + (int)mw.x, schermo.top + alto};
    }
    double salita() const { return std::max(0.0, my.x); }   // (la molla puo' scendere un poco sotto lo 0: niente stacco)
    int posX() const { return schermo.left + (schermo.right - schermo.left - W2) / 2; }
    int yFin() const { return basso ? (int)schermo.bottom - H2 : (int)schermo.top; }   // cima della finestra sullo schermo
    // col notch in basso i contenuti della scena s stanno contro il bordo in basso della finestra: quanto scendono
    // rispetto al notch in alto (i contenuti si calcolano sempre come in alto)
    float scesa(int s) const { if (!basso) return 0; float w, h, R; misura(s, w, h, R); return (float)H2 - h; }

    // luna e tazzina dallo stato di comodita.h (che vale anche col notch non visibile); true se e' cambiato qualcosa
    bool sincronizzaSegni() {
        const double t = adesso();
        bool cambiato = false;
        if (comodita::silenzio() != segnoSilenzio) { segnoSilenzio = comodita::silenzio(); tSegnoSilenzio = t; cambiato = true; }
        if (comodita::sveglio() != segnoSveglio) { segnoSveglio = comodita::sveglio(); tSegnoSveglio = t; cambiato = true; }
        return cambiato;
    }
    void controlla() {
        if (!visibile) return;
        const double t = adesso();
        bool ridisegnare = false;
        // musica
        const UINT v = lettore.versione();
        if (risincronizzaAlle > 0 && t >= risincronizzaAlle) {
            risincronizzaAlle = 0;
            const bool vero = lettore.stato().suona;
            if (vero != brano.suona) { brano.suona = vero; ridisegnare = true; }
        }
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
        // microfono e webcam (pallini dentro il notch: il loro spazio cambia la sua larghezza)
        if (sensori::Sensori::io().versione != versioneSensori) { versioneSensori = sensori::Sensori::io().versione; ridisegnare = true; bersagli(); anima(); }
        // lunetta (non disturbare) e tazzina (tieni sveglio)
        if (sincronizzaSegni()) { bersagli(); anima(); }
        // mouse
        POINT p; GetCursorPos(&p);
        RECT c = corpo();
        const int m = (int)(4 * dpi);
        RECT largo = {c.left - m, c.top, c.right + m, c.bottom + m};
        if (basso) largo = {c.left - m, c.top - m, c.right + m, c.bottom};   // (in basso si allarga verso l'alto)
        const bool dentro = c.bottom > c.top + 2 && PtInRect(&largo, p) != FALSE;   // (salito sopra il bordo: mai)
        const bool tasti = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        // col mouse fermo sopra per un attimo (senza tasti: non mentre si trascina una finestra) si apre
        if (!sopra) {
            attesa = dentro && !tasti ? attesa + 1 : 0;
            if (attesa >= 6) { sopra = true; fuori = 0; bersagli(); }
        } else {
            fuori = dentro ? 0 : fuori + 1;
            if (fuori >= 5) { sopra = false; attesa = 0; bersagli(); }
        }
        // tasto sotto il mouse (aperto) e x della notifica
        const POINT pc = {p.x - posX(), p.y - yFin() - (int)scesa(scena)};   // (coordinate dei contenuti)
        const int ps = scena == APERTO ? pulsante(pc.x, pc.y) : scena == TIMER_APERTO ? tastoTimerIn(pc.x, pc.y) : -1;
        const bool sx = scena == NOTIFICA && dentro && sullaX(pc.x, pc.y);
        if (ps != pulsanteSopra || sx != sopraX || dentro != mouseDentro) { pulsanteSopra = ps; sopraX = sx; mouseDentro = dentro; ridisegnare = true; }
        // scadenze: volume e notifica (ferma finche' c'e' sopra il mouse)
        // (i 5,5 s contano solo mentre la notifica si vede: coperta da una scena piu' importante, per esempio la sveglia
        // del timer, il tempo si ferma. Prima poteva scadere senza essere mai comparsa, R20261121-002056-S5)
        if (notificaAttiva && scena != NOTIFICA && tControllo > 0) fineNotifica += t - tControllo;
        tControllo = t;
        if (notificaAttiva && t >= fineNotifica) { if (dentro) fineNotifica = t + 1.5; else finisciNotifica(); }
        // con il tasto premuto e il mouse sul notch (un file trascinato da Esplora file) il notch si fa trovare dal
        // trascinamento: trasparente al mouse, Windows non glielo darebbe mai
        // (a scomparsa, salito sopra il bordo: mai cliccabile, la cima dello schermo resta alle finestre)
        const bool giu = c.bottom > c.top + 2;
        const RECT zona = zonaFile();
        const bool vicino = !abilitato && modoAtt == NORMALE && tasti && PtInRect(&zona, p);
        if (vicino != sondaFile) { sondaFile = vicino; ridisegnare = true; }
        cliccabile(modoAtt == NORMALE && (vicino || (giu && (sopraVale() || (scena == NOTIFICA && dentro) || scena == SUONA || scena == SCAFFALE || (tasti && dentro)))));
        const int prima = scena;
        bersagli();
        if (scena != prima) ridisegnare = false;   // ci pensa l'animazione
        if (ridisegnare && !animando) ridisegna();
        // le barrette della musica si muovono: si ridisegna solo il loro rettangolino (tutto il notch costava ~3% di CPU)
        else if (brano.suona && (scena == MUSICA || scena == APERTO) && !animando && salita() < 0.5) ridisegnaBarrette();
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
                // subito, senza aspettare la prossima lettura; poi si rilegge lo stato vero, se l'app rifiuta la pausa
                // (come dalla capsula: R20261121-120306 su S11)
                if (b == 1) { brano.suona = !brano.suona; risincronizzaAlle = adesso() + 1.5; }
                ridisegna();
                return;
            }
        }
        // musica (piccola o aperta, fuori dai tasti): l'app che sta riproducendo in primo piano
        if ((scena == MUSICA || scena == APERTO) && !brano.aumid.empty() && apriMusica) { apriMusica(brano.aumid); return; }
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
        const int w = (int)std::round((abilitato ? 392 : 560) * dpi), h = (int)std::round(100 * dpi);
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
            testi::allinea(*f, al); (*f)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); (*f)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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
        // impostazione "Angoli": raccordi e angoli in basso in proporzione (squadrati: un rettangolo attaccato al bordo)
        const float k = raggio::fattore(), fr = f * k;
        const float R = std::clamp((float)mr.x, 0.0f, std::min(w / 2, h - f)) * k;
        const float x0 = (W2 - w) / 2, x1 = x0 + w;
        // a scomparsa: forma e contenuti salgono insieme sopra il bordo (tutto sopra: niente da disegnare)
        const float su = (float)salita();
        const bool tutta = su < h + 1;
        base = su > 0 ? D2D1::Matrix3x2F::Translation(0, -su) : D2D1::Matrix3x2F::Identity();
        // in basso (dock in alto): la forma si capovolge contro il bordo in basso e scende oltre; i contenuti restano
        // dritti (base sposta solo, vedi contenuto)
        if (basso) base = D2D1::Matrix3x2F::Translation(0, su);
        const D2D1_MATRIX_3X2_F forma = basso ? D2D1::Matrix3x2F(1, 0, 0, -1, 0, (float)H2 + su) : base;
        const Bordo stile = bordo();
        const float apertura = std::clamp(1.0f - su / h, 0.0f, 1.0f);
        if (stile != CLASSICO) base = D2D1::Matrix3x2F::Identity();
        rt->SetTransform(stile == CLASSICO ? forma : (basso ? D2D1::Matrix3x2F(1,0,0,-1,0,(float)H2) : base));
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (tutta && f2d && SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            // (archi solo se hanno una misura: a raggio zero un lato dritto fino all'angolo)
            if (stile != CLASSICO) {
                const float centro = W2 * 0.5f;
                const bool pieno = stile == VELO && !scaffale.empty();
                const float stesa = (float)std::clamp(distensione.x / 100.0, 0.0, 1.0);
                const bool notificaGrande = scena == NOTIFICA;
                // marea (volume, alimentazione): fondo piatto largo quanto i contenuti e ali corte, come un angolo che
                // si raccorda al bordo; prima il fondo era il 39% della misura e le ali larghe 43 px (forma a trapezio
                // con i contenuti sopra le ali)
                const bool marea = stile == MAREA;
                // (marea: il fondo piatto comincia 8 px dentro, la curva ne prende 26: angolo tondo, contenuti fuori)
                const float piano = marea ? w * 0.5f - 8 * dpi : w * (pieno || notificaGrande ? 0.48f : 0.39f);
                const float riposo = piano + (pieno ? 85.0f : notificaGrande ? 70.0f : marea ? 26.0f : 43.0f) * dpi;
                const float fine = std::min(centro - dpi, riposo + (centro - dpi - riposo) * stesa);
                const float ala = std::max(dpi, fine - piano);
                const float fase = (float)std::clamp((adesso() - tOnda) / DURATA_ECO, 0.0, 1.0);
                s->BeginFigure({centro - fine, 0}, D2D1_FIGURE_BEGIN_FILLED);
                for (int j = 0; j <= 160; ++j) {
                    const float x = fine * (j / 80.0f - 1.0f);
                    float k = std::clamp((std::abs(x) - piano) / ala, 0.0f, 1.0f);
                    // marea: la curva gira presto (angolo in basso piccolo e tondo) e poi scende piano verso il bordo
                    // (raccordo concavo lungo); con lo smoothstep semplice il fianco sembrava una rampa dritta
                    if (marea) k = 1 - std::pow(1 - k, 1.6f);
                    const float arco = std::min(1.0f, std::abs(x) / std::max(dpi, piano));
                    float y = h * (1 - (pieno || marea ? 0.0f : 0.08f)*arco*arco) * (1 - k*k*(3-2*k));
                    if (stile == ECO && fase < 1 && k > 0) {
                        const float z = (k - fase) * 6;
                        y += 11 * dpi * std::exp(-z*z) * std::sin(3.14159f*k) * (1-fase);
                    }
                    s->AddLine({centro + x, apertura * y});
                }
            } else {
            s->BeginFigure({x0 - fr, 0}, D2D1_FIGURE_BEGIN_FILLED);
            if (fr >= 0.5f) s->AddArc({{x0, fr}, {fr, fr}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});        // raccordo a sinistra
            s->AddLine({x0, h - R});
            if (R >= 0.5f) s->AddArc({{x0 + R, h}, {R, R}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            s->AddLine({x1 - R, h});
            if (R >= 0.5f) s->AddArc({{x1, h - R}, {R, R}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            s->AddLine({x1, fr});
            if (fr >= 0.5f) s->AddArc({{x1 + fr, 0}, {fr, fr}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});   // raccordo a destra
            }
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close(); s->Release();
            if (stile == CLASSICO) colore(0, 0, 0, 1);
            else colore(0, 0, 0, (stile == VELO ? 0.80f : 0.84f) * std::min(1.0f, apertura * 4.0f));
            rt->FillGeometry(g, pen);
            if (stile == ECO || stile == VELO) {
                const float impulso = stile == ECO ? 1.0f - (float)std::clamp((adesso() - tOnda) / DURATA_ECO, 0.0, 1.0) : 0.0f;
                colore(0.60f, 0.78f, 0.76f, apertura * (0.27f + 0.14f * impulso));
                rt->DrawGeometry(g, pen, 1.2f * dpi);
            }
            // contenuti dentro la forma: quello di prima sfuma via, il nuovo compare mentre la forma cambia
            ID2D1Layer* strato = nullptr;
            if (SUCCEEDED(rt->CreateLayer(&strato))) {
                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g), strato);
                rt->SetTransform(base);
                const double t = adesso() - tScena;
                if (scenaPrima != scena) {
                    // dissolvenze addolcite (smoothstep): prima erano lineari, e quella in uscita durava 0,12 s
                    auto morbida = [](double k) { k = std::clamp(k, 0.0, 1.0); return (float)(k * k * (3 - 2 * k)); };
                    const float a = 1 - morbida(t / 0.16);
                    if (a > 0 && (abilitato || !aRiposo(scenaPrima))) contenuto(scenaPrima, a);
                    contenuto(scena, morbida((t - 0.06) / 0.24));
                } else contenuto(scena, 1);
                rt->PopLayer();
                strato->Release();
            }
        }
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        // Una finestra layered completamente trasparente non riceve DragEnter.
        // Solo durante l'avvicinamento con un tasto premuto si arma il bersaglio OLE.
        if (sondaFile && !abilitato && visibile) {
            RECT z = zonaFile();
            colore(0, 0, 0, 0.008f);
            rt->FillRectangle({(float)(z.left-posX()),(float)(z.top-yFin()),(float)(z.right-posX()),(float)(z.bottom-yFin())}, pen);
        }
        // (i pallini di microfono e webcam non stanno piu' fuori dal notch, nel vuoto: dentro, vedi puntiSensori; col
        // notch spento li mostra il dock, piccoli)
        if (g) g->Release();
        if (rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) { libera(); return; }
        mostraAlfa();
    }

    // dove stanno le barrette nella scena attuale (coordinate della finestra)
    bool zonaBarrette(D2D1_RECT_F& r, D2D1_POINT_2F& o) const {
        float w, h, R; misura(scena, w, h, R);
        const float L = sinistra(scena), d = dpi;
        // (vedi contenutoScena: lo spazio dei pallini e quello di luna e tazzina. Senza segniW il ridisegno parziale
        // metteva un secondo gruppo di barrette 22 px piu' a destra, sopra la luna: R20261121-002056-S12)
        if (scena == MUSICA) o = {L + w - puntiW() - segniW() - 25 * d, h / 2};
        else if (scena == APERTO) o = {L + w - 35 * d, 24 * d};
        else return false;
        o.y += scesa(scena);   // (notch in basso: contro il bordo in basso)
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
        POINT pos = {posX(), yFin()}, zero = {0, 0}; SIZE sz = {W2, H2};
        BLENDFUNCTION bl = {AC_SRC_OVER, 0, (BYTE)std::lround(255 * std::clamp(alfa, 0.0f, 1.0f)), AC_SRC_ALPHA};
        UPDATELAYEREDWINDOWINFO u{sizeof(u), nullptr, &pos, &sz, mem, &zero, 0, &bl, ULW_ALPHA, sporco};
        UpdateLayeredWindowIndirect(fin, &u);
    }

    // contenuti della scena s: in basso scendono contro il bordo in basso (base li sposta, e chi la compone la segue)
    void contenuto(int s, float a) {
        if (!basso) { contenutoScena(s, a); return; }
        const D2D1_MATRIX_3X2_F b0 = base;
        base = D2D1::Matrix3x2F::Translation(0, scesa(s)) * b0;
        rt->SetTransform(base);
        contenutoScena(s, a);
        base = b0;
    }
    // tazzina e luna (ora, musica e timer a riposo): compaiono e spariscono con una dissolvenza e crescendo un poco.
    // Stanno attaccate ai bordi VERI del notch (che con la molla si allarga o si stringe), non a quelli finali: prima,
    // spegnendone una, saltava subito verso il centro e poi spariva. dopoScaffale: la tazzina dopo il segno dello scaffale
    void segni(bool dopoScaffale, float h, float a) {
        const float d = dpi;
        const float as = alfaSegno(segnoSveglio, tSegnoSveglio), al = alfaSegno(segnoSilenzio, tSegnoSilenzio);
        const float Lv = (W2 - (float)mw.x) / 2, Rv = Lv + (float)mw.x;
        if (as > 0.01f) {
            const D2D1_POINT_2F c = {Lv + (dopoScaffale ? 46 : 17) * d, h / 2};
            rt->SetTransform(D2D1::Matrix3x2F::Scale(0.6f + 0.4f * as, 0.6f + 0.4f * as, c) * base);
            tazzina(c.x, c.y, 0.8f * a * as);
            rt->SetTransform(base);
        }
        if (al > 0.01f) {
            const D2D1_POINT_2F c = {Rv - 17 * d - puntiW(), h / 2};   // (a sinistra dei pallini, se ci sono)
            rt->SetTransform(D2D1::Matrix3x2F::Scale(0.6f + 0.4f * al, 0.6f + 0.4f * al, c) * base);
            lunetta(c.x, c.y, 0.8f * a * al);
            rt->SetTransform(base);
        }
    }
    void contenutoScena(int s, float a) {
        if (a <= 0.001f) return;
        float w, h, R; misura(s, w, h, R);
        float L = sinistra(s); const float d = dpi;
        // musica e timer a riposo: i pallini di microfono e webcam a destra, e lo stesso spazio a sinistra (contenuto al centro)
        if (s == MUSICA || s == TIMER) { L += puntiW() + segniW(); w -= 2 * (puntiW() + segniW()); }
        if (s == ORA || s == MUSICA || s == TIMER) { puntiSensori(s, h, a); segni(s == ORA && !scaffale.empty(), h, a); }
        switch (s) {
            case ORA:
                ora(L, w, h, a);
                if (!scaffale.empty()) segnoScaffale(L + 18 * d, h / 2, a);
                break;
            case AVVISO: {
                // simbolo e testo come un blocco solo, centrato nel notch (prima stavano a sinistra di un notch largo almeno
                // 220 px, e un testo corto lasciava un vuoto a destra)
                const float tw = std::min(larghezzaAvviso, w - (AV_SIMBOLO + AV_SPAZIO + 2 * AV_MARGINE) * d);
                const float x0 = L + (w - (AV_SIMBOLO + AV_SPAZIO) * d - tw) / 2;
                const float cx = x0 + AV_SIMBOLO * d / 2, cy = h / 2;
                if (simboloAvviso == sensori::S_BATTERIA || simboloAvviso == sensori::S_ALIMENTAZIONE) batteriaSimbolo(cx, cy, a, valoreAvviso, avvisoAcceso);
                else if (simboloAvviso == sensori::S_CUFFIE) cuffie(cx, cy, a);
                else if (simboloAvviso == sensori::S_AI) scintillaAI(cx, cy, a, valoreAvviso);
                else puntina(cx, cy, a, avvisoAcceso);
                const float xt = x0 + (AV_SIMBOLO + AV_SPAZIO) * d;
                scrivi(testoAvviso, fPiccolo, {xt, 0, std::min(xt + tw + 4 * d, L + w - AV_MARGINE * d / 2), h}, a);
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
        ID2D1GeometrySink* s = nullptr;   // (Open puo' fallire: R20261121-002056-S17)
        if (e1 && e2 && SUCCEEDED(f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
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
    struct Oggetto { std::wstring percorso, nome; std::vector<UINT> px; int w = 0, h = 0; ID2D1Bitmap* b = nullptr; bool provata = false; };
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
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* o, DWORD, POINTL p, DWORD* e) override {
            FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            ok = o && !n->trascinoMio && n->modoAtt == NORMALE && o->QueryGetData(&f) == S_OK;
            return DragOver(0, p, e);
        }
        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL p, DWORD* e) override {
            RECT z = n->zonaFile(), corpo = n->corpo();
            const bool dentro = ok && (n->abilitato || PtInRect(&z, {p.x,p.y}) ||
                (n->trascinoSopra && PtInRect(&corpo, {p.x,p.y})));
            if (dentro != n->trascinoSopra) { n->trascinoSopra = dentro; n->bersagli(); }
            *e = dentro ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragLeave() override { if (n->trascinoSopra) { n->trascinoSopra = false; n->bersagli(); } return S_OK; }
        HRESULT STDMETHODCALLTYPE Drop(IDataObject* o, DWORD, POINTL, DWORD* e) override {
            tracciaScaffale("deposito: ingresso");
            *e = DROPEFFECT_NONE;
            const bool accetta = ok && n->trascinoSopra;
            n->trascinoSopra = false;
            if (accetta && o) {
                FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM m{};
                if (SUCCEEDED(o->GetData(&f, &m))) {
                    // l'handle del blocco, non il puntatore di GlobalLock (audit FW-AUDIT-016, come nel dock)
                    // Tutti i file o nessuno: prima oltre il cinquantesimo si scartavano in silenzio e la sorgente riceveva
                    // comunque COPY (S20). Fino a 200 in tutto; le miniature solo per i primi 12 (le altre si
                    // vedrebbero solo scorrendo), perche' la shell le legge qui, sul thread dell'interfaccia (S8)
                    bool preso = false;
                    if (const HDROP hd = (HDROP)m.hGlobal) {
                        const UINT k = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                        std::vector<std::wstring> percorsi;
                        for (UINT i = 0; i < k && k + n->scaffale.size() <= 200; i++) {
                            const UINT len = DragQueryFileW(hd, i, nullptr, 0);
                            std::wstring p(len + 1, L'\0');
                            if (!len || DragQueryFileW(hd, i, p.data(), len + 1) != len) { percorsi.clear(); break; }
                            p.resize(len); percorsi.push_back(std::move(p));
                        }
                        if (k && percorsi.size() == k) {
                            for (const auto& p : percorsi) n->appoggia(p, n->scaffale.size() < 12);
                            preso = true;
                        }
                    }
                    ReleaseStgMedium(&m);
                    *e = preso ? DROPEFFECT_COPY : DROPEFFECT_NONE;   // copia: il file resta dov'era
                    n->fineScaffale = adesso() + 1.8;   // si vede un attimo cosa e' entrato
                }
            }
            n->bersagli(); n->ridisegna();
            tracciaScaffale("deposito: fine");
            return S_OK;
        }
    } bersaglio;

    static void tracciaScaffale(const char* fase, unsigned long esito = 0) {
        if (registroScaffale) registroScaffale(fase, esito);
    }

    void appoggia(const std::wstring& p, bool conMiniatura = true) {
        for (auto& o : scaffale) if (!_wcsicmp(o.percorso.c_str(), p.c_str())) return;
        Oggetto o; o.percorso = p;
        const size_t s = p.find_last_of(L"\\/"); o.nome = s == std::wstring::npos ? p : p.substr(s + 1);
        if (conMiniatura) miniatura(o);
        scaffale.push_back(std::move(o));
    }
    // icona o anteprima (poche e piccole: subito), una volta per oggetto
    void miniatura(Oggetto& o) {
        o.provata = true;
        const std::wstring& p = o.percorso;
        IShellItemImageFactory* fab = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&fab)))) {
            tracciaScaffale("deposito: lettura miniatura");
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
        tracciaScaffale("uscita: preparazione");
        std::vector<size_t> quali;
        if (i == TUTTI) for (size_t k = 0; k < scaffale.size(); k++) quali.push_back(k);
        else if (i >= 0 && i < (int)scaffale.size()) quali.push_back(i);
        else return;
        std::vector<PIDLIST_ABSOLUTE> id;
        for (size_t k : quali) { PIDLIST_ABSOLUTE p = nullptr; if (SUCCEEDED(SHParseDisplayName(scaffale[k].percorso.c_str(), nullptr, &p, 0, nullptr)) && p) id.push_back(p); }
        IShellItemArray* arr = nullptr; IDataObject* dati = nullptr;
        tracciaScaffale("uscita: percorsi risolti", (unsigned long)id.size());
        HRESULT hr = E_FAIL;
        if (!id.empty() && id.size() == quali.size()) {
            hr = SHCreateShellItemArrayFromIDLists((UINT)id.size(), (PCIDLIST_ABSOLUTE_ARRAY)id.data(), &arr);
            tracciaScaffale("uscita: creazione elenco Shell", (unsigned long)hr);
            if (SUCCEEDED(hr) && arr) {
                hr = arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dati));
                tracciaScaffale("uscita: oggetto dati Shell", (unsigned long)hr);
            }
        }
        if (SUCCEEDED(hr) && dati) {
            trascinoMio = true;
            DWORD effetto = DROPEFFECT_NONE;
            tracciaScaffale("uscita: entra DoDragDrop");
            const HRESULT r = DoDragDrop(dati, &fonte, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effetto);
            tracciaScaffale("uscita: ritorno DoDragDrop", (unsigned long)r);
            tracciaScaffale("uscita: effetto", effetto);
            trascinoMio = false;
            if (r == DRAGDROP_S_DROP && effetto != DROPEFFECT_NONE) {   // arrivato: esce dallo scaffale (nel dubbio resta)
                for (auto it = quali.rbegin(); it != quali.rend(); ++it) { if (scaffale[*it].b) scaffale[*it].b->Release(); scaffale.erase(scaffale.begin() + *it); }
                // le voci che ora entrano fra le visibili prendono la miniatura qui, fuori dal disegno (fra BeginDraw ed
                // EndDraw un provider lento fermava il notch, R20261121-123420-S4)
                for (size_t i = 0; i < scaffale.size() && i < (size_t)MAX_VISTI; i++) if (!scaffale[i].provata) miniatura(scaffale[i]);
            }
            dati->Release();
        }
        if (arr) arr->Release();
        for (auto p : id) CoTaskMemFree(p);
        bersagli(); ridisegna();
        tracciaScaffale("uscita: fine");
    }

    struct Fonte : IDropSource {
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override { if (r == IID_IUnknown || r == IID_IDropSource) { *p = this; return S_OK; } *p = nullptr; return E_NOINTERFACE; }
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD k) override {
            if (esc || !(k & MK_LBUTTON)) tracciaScaffale(esc ? "OLE: annullamento" : "OLE: rilascio");
            return esc ? DRAGDROP_S_CANCEL : !(k & MK_LBUTTON) ? DRAGDROP_S_DROP : S_OK;
        }
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
        if (!abilitato && scaffale.empty()) {
            const auto prima = fTitolo->GetTextAlignment();
            fTitolo->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            scrivi(testi::t(L"Lascia qui i file", L"Drop files here"), fTitolo,
                   {L + 12*d, 0, L + w - 12*d, h}, a);
            fTitolo->SetTextAlignment(prima);
            return;
        }
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
            // (miniatura di un file entrato oltre il dodicesimo: si legge quando diventa visibile, all'uscita delle
            // voci davanti, R20261121-120306-S4)
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

    // scintilla AI (stato: 0 fatto, 1 in attesa, 2 errore)
    void scintillaAI(float cx, float cy, float a, int stato) {
        const float r = 6.5f * dpi, k = r * 0.22f;
        const D2D1_COLOR_F col = stato == 1 ? D2D1::ColorF(1.0f, 0.70f, 0.16f, a)
                               : stato == 2 ? D2D1::ColorF(1.0f, 0.34f, 0.30f, a)
                                            : D2D1::ColorF(0.22f, 0.84f, 0.42f, a);
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(fabbriche::d2d()->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            s->BeginFigure({cx, cy - r}, D2D1_FIGURE_BEGIN_FILLED);
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({cx + k, cy - k}, {cx + r, cy}));
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({cx + k, cy + k}, {cx, cy + r}));
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({cx - k, cy + k}, {cx - r, cy}));
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({cx - k, cy - k}, {cx, cy - r}));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(s->Close())) {
                pen->SetColor(col);
                rt->FillGeometry(g, pen);
            }
        }
        if (s) s->Release();
        if (g) g->Release();
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

    // Notch a scomparsa (impostazione Notch spenta): margine m uguale ai due lati; simbolo (largo 17 px a 100%), spazio,
    // barra, spazio, numero allineato a destra. La barra prende il resto: il numero (o "Muto", piu' lungo in altre
    // lingue) ha lo spazio che gli serve. Prima: simbolo a 22 px dal bordo, barra fra 52 e w-58 e numero in una casella
    // fissa di 38 px, con 30 px vuoti a destra del numero. Con il notch acceso la disposizione resta quella di prima.
    void volumeScena(float L, float w, float h, float a) {
        const bool fisso = abilitato;   // notch acceso: misure di prima
        const float d = dpi, cy = h / 2, m = 14 * d, s = (fisso ? 1.0f : 0.85f) * d;   // s: scala del simbolo
        const float sx = fisso ? L + 22 * d : L + m + 6 * s;            // (il simbolo va da sx-6s a sx+13.5s, onda grande)
        colore(1, 1, 1, a);
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* gs = nullptr;
        if (SUCCEEDED(fabbriche::d2d()->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&gs))) {   // altoparlante
            gs->BeginFigure({sx - 6 * s, cy - 3.2f * s}, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F p[] = {{sx - 2 * s, cy - 3.2f * s}, {sx + 3 * s, cy - 7.5f * s}, {sx + 3 * s, cy + 7.5f * s}, {sx - 2 * s, cy + 3.2f * s}, {sx - 6 * s, cy + 3.2f * s}};
            gs->AddLines(p, 5); gs->EndFigure(D2D1_FIGURE_END_CLOSED); gs->Close(); gs->Release();
            rt->FillGeometry(g, pen);
        }
        if (g) g->Release();
        const float liv = muto ? 0.0f : livelloVis, tratto = (fisso ? 1.7f : 1.6f) * d, xm = fisso ? 14.0f : 13.0f;
        if (muto) {
            rt->DrawLine({sx + 7 * s, cy - 4 * s}, {sx + xm * s, cy + 4 * s}, pen, tratto);
            rt->DrawLine({sx + xm * s, cy - 4 * s}, {sx + 7 * s, cy + 4 * s}, pen, tratto);
        } else {
            for (int i = 0; i < 2; i++) {
                if (liv < (i == 0 ? 0.01f : 0.5f)) break;
                const float r = (i == 0 ? 6.0f : 10.5f) * s;
                ID2D1PathGeometry* ar = nullptr; ID2D1GeometrySink* as = nullptr;
                if (FAILED(fabbriche::d2d()->CreatePathGeometry(&ar)) || FAILED(ar->Open(&as))) { if (ar) ar->Release(); break; }
                as->BeginFigure({sx + 3 * s + r * 0.6f, cy - r * 0.8f}, D2D1_FIGURE_BEGIN_HOLLOW);
                as->AddArc(D2D1::ArcSegment({sx + 3 * s + r * 0.6f, cy + r * 0.8f}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                as->EndFigure(D2D1_FIGURE_END_OPEN); as->Close(); as->Release();
                rt->DrawGeometry(ar, pen, tratto); ar->Release();
            }
        }
        wchar_t t[16]; swprintf(t, 16, muto ? testi::t(L"Muto", L"Muted") : L"%d", (int)std::lround(livello * 100));
        const float destra = fisso ? L + w - 18 * d : L + w - m;
        const float largoNum = fisso ? 0.0f : std::max(larghezza(L"100", fNumero), larghezza(t, fNumero));
        const float bl = fisso ? L + 52 * d : sx + 13.5f * s + 10 * d;
        const float br = fisso ? L + w - 58 * d : std::max(bl + 20 * d, destra - largoNum - 10 * d);
        const float bh = (fisso ? 3.5f : 2.5f) * d;   // mezza altezza della barra
        const D2D1_RECT_F b = {bl, cy - bh, br, cy + bh};
        colore(1, 1, 1, 0.2f * a);
        rt->FillRoundedRectangle({b, bh, bh}, pen);
        if (liv > 0.005f) {
            colore(1, 1, 1, a);
            rt->FillRoundedRectangle({{b.left, b.top, b.left + std::max(2 * bh, (b.right - b.left) * liv), b.bottom}, bh, bh}, pen);
        }
        scrivi(t, fNumero, {fisso ? L + w - 56 * d : br + 6 * d, 0, destra, h}, a);
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
            case WM_LBUTTONDOWN: if (n) { n->premutoX = (short)LOWORD(l); n->premutoY = (short)HIWORD(l) - (int)n->scesa(n->scena); n->premutoS = n->scena == SCAFFALE ? n->oggettoIn(n->premutoX, n->premutoY) : -1; } return 0;
            case WM_MOUSEMOVE:   // trascinamento fuori dallo scaffale
                if (n && n->premutoS != -1 && (w & MK_LBUTTON) && (std::abs((short)LOWORD(l) - n->premutoX) > 5 || std::abs((short)HIWORD(l) - (int)n->scesa(n->scena) - n->premutoY) > 5)) {
                    const int s = n->premutoS; n->premutoS = -1; n->trascinaFuori(s);
                }
                return 0;
            case WM_LBUTTONUP: if (n) { const bool premuto = n->premutoS != -1 || n->scena != SCAFFALE; n->premutoS = -1; if (premuto) n->clicca((short)LOWORD(l), (short)HIWORD(l) - (int)n->scesa(n->scena)); } return 0;
            case WM_CLOSE: return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

}  // namespace notch
