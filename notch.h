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
        BOOL niente = TRUE; DwmSetWindowAttribute(fin, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        return true;
    }

    ~Notch() {
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

private:
    enum Scena { ORA, MUSICA, APERTO, VOLUME, NOTIFICA };

    HWND fin = nullptr;
    bool acceso = false, visibile = false;
    int modoAtt = NORMALE;
    RECT schermo{};
    float dpi = 1, fDpi = 0;
    std::wstring testoOra, testoGiorno;
    const wchar_t *fontTesto = L"Segoe UI Variable Text", *fontTitoli = L"Segoe UI Variable Display";
    IDWriteTextFormat *fOra = nullptr, *fGiorno = nullptr, *fTitolo = nullptr, *fPiccolo = nullptr, *fTesto = nullptr, *fNumero = nullptr;
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
    bool notificaAttiva = false; double fineNotifica = 0;
    std::wstring nApp, nTitolo, nTesto; std::vector<UINT> nPx; int nW = 0, nH = 0; bool iconaNuova = false;
    musica::Lettore lettore; musica::Brano brano; UINT versioneBrano = 0xFFFFFFFF; bool copertinaNuova = false;

    // mouse
    bool sopra = false, cliccabileOra = false;
    int attesa = 0, fuori = 0, pulsanteSopra = -1;
    bool sopraX = false, mouseDentro = false;
    ULONGLONG tCima = 0;

    std::vector<IDWriteTextFormat*> formati() const { return {fOra, fGiorno, fTitolo, fPiccolo, fTesto, fNumero}; }

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
            finisciNotifica(); fineVolume = 0;
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
        if (notificaAttiva) return NOTIFICA;
        if (sopra && brano.c) return APERTO;
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
            default:       w = largoOra; h = 22 * d; R = 9 * d;
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
        const int ps = scena == APERTO ? pulsante(pc.x, pc.y) : -1;
        const bool sx = scena == NOTIFICA && dentro && sullaX(pc.x, pc.y);
        if (ps != pulsanteSopra || sx != sopraX || dentro != mouseDentro) { pulsanteSopra = ps; sopraX = sx; mouseDentro = dentro; ridisegnare = true; }
        // scadenze: volume e notifica (ferma finche' c'e' sopra il mouse)
        if (notificaAttiva && t >= fineNotifica) { if (dentro) fineNotifica = t + 1.5; else finisciNotifica(); }
        cliccabile(modoAtt == NORMALE && (sopra || (scena == NOTIFICA && dentro)));
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
        fOra = fGiorno = fTitolo = fPiccolo = fTesto = fNumero = nullptr;
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
            case ORA: ora(L, w, h, a); break;
            case MUSICA: {
                ora(L, w, h, a);
                const float q = 14 * d, y = (h - q) / 2;   // copertina piccola a sinistra, barrette a destra
                if (bCopertina) immagine(bCopertina, {L + 12 * d, y, L + 12 * d + q, y + q}, 3.5f * d, a);
                else nota(L + 12 * d + q / 2, h / 2, a);
                barrette(L + w - 12 * d - 13 * d, h / 2, 13 * d, a);
                break;
            }
            case APERTO: aperto(L, w, h, a); break;
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
            case WM_LBUTTONUP: if (n) n->clicca((short)LOWORD(l), (short)HIWORD(l)); return 0;
            case WM_CLOSE: return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

}  // namespace notch
