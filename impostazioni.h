// Impostazioni di FluidWin: valori salvati in HKCU\Software\FluidWin\Impostazioni e la finestra per cambiarli
// (dal menu dell'icona di FluidWin o dal centro di controllo). Ogni cambio si applica subito.
#pragma once
#include "vetro.h"
#include "testi.h"
#include <windowsx.h>
#include <vector>
#include <functional>
#include <string>

namespace impostazioni {

struct Valori {
    bool gelatina = true;         // finestre "di gelatina" quando si trascinano
    bool genio = true;            // riduzione e ripristino con l'effetto genio
    bool chiusura = true;         // finestre che si rimpiccioliscono e svaniscono quando si chiudono
    bool apertura = true;         // finestre aperte fuori dal dock (Start, ricerca...) che crescono dal centro
    bool onda = true;             // onda di luce multicolore sui bordi dello schermo quando si apre la ricerca
    bool volume = true;           // indicatore del volume di FluidWin al posto di quello di Windows
    bool angolo = true;           // panoramica col mouse nell'angolo in alto a sinistra
    bool winTab = true;           // Alt+Tab apre la panoramica di FluidWin (fino alla 1.2.0 era Win+Tab: il nome e' rimasto)
    bool winS = true;             // Win+Tab apre la ricerca di FluidWin (il nome nel registro e' rimasto WinS)
    bool banner = false;          // banner delle notifiche di FluidWin (spegne quelli di Windows, app per app)
    bool menuDestro = true;       // menu del tasto destro di FluidWin su desktop ed Esplora file (destro.h)
    bool notch = true;            // il notch in alto al centro (spento: volume e notifiche col vetro, niente musica e timer)
    bool oraDock = false;         // ora e data in fondo al dock, dopo il centro di controllo
    int lingua = 0;               // lingua dell'interfaccia (testi::ITALIANO o testi::INGLESE)
};

inline Valori& valori() { static Valori v; return v; }

inline const wchar_t* CHIAVE = L"Software\\FluidWin\\Impostazioni";

inline DWORD leggiDword(const wchar_t* nome, DWORD predefinito) {
    DWORD v = predefinito, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE, nome, RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS) return predefinito;
    return v;
}

inline void scriviDword(const wchar_t* nome, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE, nome, REG_DWORD, &v, sizeof(v));
}

inline void carica() {
    Valori& v = valori();
    v.gelatina = leggiDword(L"Gelatina", 1); v.genio = leggiDword(L"Genio", 1); v.chiusura = leggiDword(L"Chiusura", 1);
    v.apertura = leggiDword(L"Apertura", 1); v.onda = leggiDword(L"OndaRicerca", 1);
    v.volume = leggiDword(L"Volume", 1); v.angolo = leggiDword(L"AngoloPanoramica", 1); v.winTab = leggiDword(L"WinTab", 1);
    v.winS = leggiDword(L"WinS", 1); v.banner = leggiDword(L"Banner", 0); v.menuDestro = leggiDword(L"MenuDestro", 1);
    v.notch = leggiDword(L"Notch", 1); v.oraDock = leggiDword(L"OraDock", 0);
    v.lingua = leggiDword(L"Lingua", (DWORD)testi::predefinita()) == testi::INGLESE ? testi::INGLESE : testi::ITALIANO;
    testi::lingua() = v.lingua;
    // la capsula dei tasti non c'e' piu' (1.1.1): via i suoi valori rimasti dalle versioni prima
    RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE, L"Capsula");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE, L"CapsulaMisura");
}

inline void salva() {
    const Valori& v = valori();
    scriviDword(L"Gelatina", v.gelatina); scriviDword(L"Genio", v.genio); scriviDword(L"Chiusura", v.chiusura);
    scriviDword(L"Apertura", v.apertura); scriviDword(L"OndaRicerca", v.onda);
    scriviDword(L"Volume", v.volume); scriviDword(L"AngoloPanoramica", v.angolo); scriviDword(L"WinTab", v.winTab);
    scriviDword(L"WinS", v.winS); scriviDword(L"Banner", v.banner); scriviDword(L"MenuDestro", v.menuDestro); scriviDword(L"Lingua", (DWORD)v.lingua);
    scriviDword(L"Notch", v.notch); scriviDword(L"OraDock", v.oraDock);
}

// ---- la finestra ----
// A sinistra la barra delle sezioni (come le Impostazioni di Windows 11), a destra la sezione scelta: titolo, voci in
// riquadri arrotondati (nome, spiegazione, interruttore) e righe informative. Il cambio di sezione e' animato.
class Finestra {
public:
    std::function<void()> cambiato;   // dopo ogni cambio (gia' salvato)
    int raggioWindhawk = 0;           // raggio della mod di Windhawk (0 = nessuna): riga informativa sugli angoli
    void (*registro)(const char*) = nullptr;
    std::function<bool()> copiaDiagnostica;   // mette negli appunti la diagnostica (fluidwin.cpp); true se riuscito

    void apri() {
        if (!v.fin && !v.crea(L"FluidWin.Impostazioni", vetro::procedura<Finestra>, this, true, 2, true, false)) return;
        if (aperta) { SetForegroundWindow(v.fin); return; }   // gia' aperta: viene davanti
        v.aggiornaTema();
        const RECT a = vetro::areaPrincipale(true);
        v.dpi = vetro::dpiMonitor(MonitorFromPoint({a.left, a.top}, MONITOR_DEFAULTTOPRIMARY));
        const float d = v.dpi;
        // misura fissa; su schermi piccoli quanto ci sta (le sezioni sono corte, entrano comunque)
        const int w = (int)std::lround(std::min(LARGO * d, (float)(a.right - a.left) - 40 * d));
        const int h = (int)std::lround(std::min(ALTO * d, (float)(a.bottom - a.top) - 40 * d));
        v.posiziona((a.left + a.right - w) / 2, (a.top + a.bottom - h) / 2, w, h, HWND_TOP, SWP_SHOWWINDOW);
        if (!fTitolo) {
            fTitolo = v.formato(22 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fVoce = v.formato(13.5f * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fSotto = v.formato(11.5f * d);
            fSezione = v.formato(13 * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fCentro = v.formato(12.5f * d, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
            fMarchio = v.formato(16 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
        aperta = true; sopra = -1; t0 = vetro::ms(); k = 0; kSez = 1;
        v.alfa(0);
        vetro::battito(v.fin, 1);
        SetForegroundWindow(v.fin);
        disegna();
    }

    void chiudi() { if (v.fin) { vetro::fermaBattito(v.fin, 1); ShowWindow(v.fin, SW_HIDE); } aperta = false; }

    bool messaggio(HWND, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
        r = 0;
        switch (m) {
            case WM_TIMER:
                if (w == 1) {
                    k = std::min(1.0, (vetro::ms() - t0) / 180.0);
                    v.alfa(vetro::uscita(k));
                    bool ancora = k < 1;
                    if (kSez < 1) { kSez = std::min(1.0, (vetro::ms() - tSez) / 180.0); ancora = true; }
                    if (copiataAlle && vetro::ms() - copiataAlle > 2500) copiataAlle = 0;   // "Copiata" torna "Copia"
                    else if (copiataAlle) ancora = true;
                    for (size_t i = 0; i < voci.size(); i++) {
                        const double obj = stato(voci[i]) ? 1.0 : 0.0;
                        if (i < anim.size() && std::abs(anim[i] - obj) > 0.01) { anim[i] += (obj - anim[i]) * 0.3; ancora = true; }
                        else if (i < anim.size()) anim[i] = obj;
                    }
                    disegna();
                    if (!ancora) vetro::fermaBattito(v.fin, 1);
                }
                return true;
            case WM_PAINT: disegna(); return true;
            case WM_MOUSEMOVE: {
                if (!dentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, v.fin, 0}; TrackMouseEvent(&tm); dentro = true; }
                const int s = zonaIn((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
                if (s != sopra) { sopra = s; disegna(); }
                return true;
            }
            case WM_MOUSELEAVE: dentro = false; sopra = -1; disegna(); return true;
            case WM_LBUTTONUP: clic(zonaIn((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l))); return true;
            case WM_KEYDOWN:
                if (w == VK_ESCAPE) chiudi();
                else if (w == VK_DOWN || w == VK_UP) vaiASezione((sezione + (w == VK_DOWN ? 1 : SEZIONI - 1)) % SEZIONI);   // frecce: sezioni
                return true;
            case WM_CLOSE: chiudi(); return true;
            case WM_NCHITTEST: {   // si trascina dalla fascia in alto (non dalla x) e dalla testata della barra laterale
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(v.fin, &p);
                const float d = v.dpi;
                if ((p.y < 60 * d && p.x < v.W - 56 * d && zonaIn((float)p.x, (float)p.y) < 0)) { r = HTCAPTION; return true; }
                return false;
            }
        }
        return false;
    }

private:
    static constexpr float LARGO = 780, ALTO = 500, BARRA = 240;
    enum { S_GENERALE, S_ANIMAZIONI, S_NOTCH, S_TASTIERA, SEZIONI };
    // nome e spiegazione in italiano e in inglese, sezione
    struct Voce { const wchar_t *nome, *name, *spiega, *explain; bool Valori::*campo; int sezione; };
    const std::vector<Voce> voci = {
        {L"Finestre di gelatina", L"Jelly windows",
         L"Le finestre si deformano mentre le trascini", L"Windows wobble while you drag them", &Valori::gelatina, S_ANIMAZIONI},
        {L"Effetto genio", L"Genie effect",
         L"Riduzione e ripristino che entrano ed escono dal dock", L"Minimize into the dock and come back out of it", &Valori::genio, S_ANIMAZIONI},
        {L"Animazione di apertura", L"Open animation",
         L"Le finestre aperte da Start o dalla ricerca crescono dal centro", L"Windows opened from Start or search grow from the center", &Valori::apertura, S_ANIMAZIONI},
        {L"Animazione di chiusura", L"Close animation",
         L"La finestra si rimpicciolisce e svanisce sul posto", L"The window shrinks and fades away in place", &Valori::chiusura, S_ANIMAZIONI},
        {L"Onda di luce della ricerca", L"Search light wave",
         L"I bordi dello schermo si accendono di colori", L"The edges of the screen light up with color", &Valori::onda, S_ANIMAZIONI},
        {L"Notch", L"Notch",
         L"In alto al centro: ora, volume, notifiche, musica e timer", L"At the top center: time, volume, notifications, music, timer", &Valori::notch, S_NOTCH},
        {L"Ora e data nel dock", L"Time and date in the dock",
         L"In fondo al dock, dopo il centro di controllo", L"At the end of the dock, after the control center", &Valori::oraDock, S_NOTCH},
        {L"Notifiche di FluidWin", L"FluidWin notifications",
         L"Escono dal notch, con il numero sulle icone del dock", L"They come out of the notch, with badges on the dock", &Valori::banner, S_NOTCH},
        {L"Volume nel notch", L"Volume in the notch",
         L"I tasti del volume mostrano il livello nel notch", L"The volume keys show the level in the notch", &Valori::volume, S_NOTCH},
        {L"Alt+Tab apre la panoramica", L"Alt+Tab opens the overview",
         L"Tieni Alt e premi Tab per scegliere la finestra, come prima", L"Hold Alt and press Tab to pick a window, as always", &Valori::winTab, S_TASTIERA},
        {L"Win+Tab apre la ricerca", L"Win+Tab opens search",
         L"App, file, impostazioni e calcoli (al posto della Visualizzazione attività)", L"Apps, files, settings and math (instead of Task View)", &Valori::winS, S_TASTIERA},
        {L"Panoramica dall'angolo", L"Overview from the corner",
         L"Mouse nell'angolo in alto a sinistra", L"Move the mouse to the top-left corner", &Valori::angolo, S_TASTIERA},
        {L"Menu del tasto destro", L"Right-click menu",
         L"Su desktop ed Esplora file, nello stile di FluidWin", L"On the desktop and in File Explorer, in FluidWin style", &Valori::menuDestro, S_TASTIERA},
    };
    vetro::Finestra v;
    IDWriteTextFormat *fTitolo = nullptr, *fVoce = nullptr, *fSotto = nullptr, *fSezione = nullptr, *fCentro = nullptr, *fMarchio = nullptr;
    bool aperta = false, dentro = false;
    int sopra = -1, sezione = S_GENERALE;
    ULONGLONG t0 = 0, tSez = 0, copiataAlle = 0;
    double k = 0, kSez = 1;
    std::vector<double> anim;
    // zone: 0..n-1 le voci, 100+i la lingua, 200 chiudi, 300+i le sezioni
    struct Zona { D2D1_RECT_F r; int id; };
    std::vector<Zona> zone;

    static bool stato(const Voce& x) { return valori().*(x.campo); }

    int zonaIn(float x, float y) const {
        for (const auto& z : zone) if (x >= z.r.left && x < z.r.right && y >= z.r.top && y < z.r.bottom) return z.id;
        return -1;
    }

    void vaiASezione(int s) {
        if (s == sezione) return;
        sezione = s; kSez = 0; tSez = vetro::ms();
        vetro::battito(v.fin, 1);
        disegna();
    }

    void clic(int id) {
        if (id < 0) return;
        if (id == 200) { chiudi(); return; }
        if (id == 400) {   // diagnostica negli appunti
            if (copiaDiagnostica && copiaDiagnostica()) { copiataAlle = vetro::ms(); vetro::battito(v.fin, 1); }
            disegna();
            return;
        }
        if (id >= 300) { vaiASezione(id - 300); return; }
        if (id >= 100) { valori().lingua = id - 100; testi::lingua() = valori().lingua; }
        else if (id < (int)voci.size()) { bool& b = valori().*(voci[id].campo); b = !b; }
        salva();
        if (cambiato) cambiato();
        vetro::battito(v.fin, 1);
        disegna();
    }

    // versione di FluidWin, dalle risorse dell'eseguibile
    static std::wstring versione() {
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
        DWORD h = 0, n = GetFileVersionInfoSizeW(p, &h);
        std::wstring r;
        if (n) {
            std::vector<BYTE> b(n);
            VS_FIXEDFILEINFO* f = nullptr; UINT lf = 0;
            if (GetFileVersionInfoW(p, 0, n, b.data()) && VerQueryValueW(b.data(), L"\\", (void**)&f, &lf) && f) {
                wchar_t s[32]; swprintf(s, 32, L"%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS), HIWORD(f->dwFileVersionLS));
                r = s;
            }
        }
        return r;
    }

    static const wchar_t* nomeSezione(int s) {
        switch (s) {
            case S_GENERALE: return testi::t(L"Generale", L"General");
            case S_ANIMAZIONI: return testi::t(L"Animazioni", L"Animations");
            case S_NOTCH: return testi::t(L"Notch e notifiche", L"Notch & notifications");
            case S_TASTIERA: return testi::t(L"Tastiera e mouse", L"Keyboard and mouse");
        }
        return L"";
    }

    // icone della barra laterale, a linee
    void iconaSezione(int s, float cx, float cy, D2D1_COLOR_F c) {
        const float d = v.dpi, sp = 1.6f * d;
        v.pen->SetColor(c);
        auto l = [&](float x1, float y1, float x2, float y2) { v.rt->DrawLine({cx + x1 * d, cy + y1 * d}, {cx + x2 * d, cy + y2 * d}, v.pen, sp); };
        switch (s) {
            case S_GENERALE:   // ingranaggio semplice: cerchio con i denti
                v.rt->DrawEllipse({{cx, cy}, 4.5f * d, 4.5f * d}, v.pen, sp);
                for (int i = 0; i < 8; i++) { const float a = i * 3.14159265f / 4; l(6 * std::cos(a), 6 * std::sin(a), 8.5f * std::cos(a), 8.5f * std::sin(a)); }
                break;
            case S_ANIMAZIONI: // scintilla e una piccola
                l(-2, -8, -2, 4); l(-8, -2, 4, -2); l(-5.5f, -5.5f, 1.5f, 1.5f); l(1.5f, -5.5f, -5.5f, 1.5f);
                l(5, 3, 5, 8); l(2.5f, 5.5f, 7.5f, 5.5f); break;
            case S_NOTCH:      // pillola del notch attaccata al bordo
                l(-9, -7, 9, -7);
                v.rt->FillRoundedRectangle({{cx - 6 * d, cy - 7 * d, cx + 6 * d, cy - 2 * d}, 2.5f * d, 2.5f * d}, v.pen);
                v.rt->DrawRoundedRectangle({{cx - 8 * d, cy + 1 * d, cx + 8 * d, cy + 7 * d}, 2 * d, 2 * d}, v.pen, sp); break;
            case S_TASTIERA:   // tastiera
                v.rt->DrawRoundedRectangle({{cx - 9 * d, cy - 5.5f * d, cx + 9 * d, cy + 5.5f * d}, 2 * d, 2 * d}, v.pen, sp);
                for (int i = 0; i < 4; i++) v.rt->FillEllipse({{cx + (-5.5f + i * 3.7f) * d, cy - 1.5f * d}, 0.9f * d, 0.9f * d}, v.pen);
                l(-4, 2.5f, 4, 2.5f); break;
        }
    }

    void disegna() {
        if (!aperta || !v.inizia()) return;
        const float d = v.dpi, W = (float)v.W, H = (float)v.H, B = BARRA * d;
        const D2D1_COLOR_F ink = v.inchiostro(), tenue = v.inchiostro(0.6f);
        const D2D1_COLOR_F blu = v.scuro ? D2D1::ColorF(0.40f, 0.68f, 1.0f, 1) : D2D1::ColorF(0.0f, 0.42f, 0.95f, 1);
        zone.clear();
        if (anim.size() != voci.size()) { anim.clear(); for (const auto& x : voci) anim.push_back(stato(x) ? 1 : 0); }
        const float rr = std::max(8.0f, raggio::vetro() - 12.0f) * d;

        // ---- barra laterale ----
        v.pen->SetColor(v.scuro ? D2D1::ColorF(0, 0, 0, 0.18f) : D2D1::ColorF(0, 0, 0, 0.035f));
        v.rt->FillRectangle({0, 0, B, H}, v.pen);
        v.testo(L"FluidWin", fMarchio, {24 * d, 22 * d, B - 12 * d, 44 * d}, ink);
        const std::wstring ver = versione();
        v.testo(ver.empty() ? std::wstring(testi::t(L"Impostazioni", L"Settings")) : testi::t(L"Impostazioni · ", L"Settings · ") + ver,
                fSotto, {24 * d, 44 * d, B - 12 * d, 62 * d}, tenue);
        for (int s = 0; s < SEZIONI; s++) {
            const D2D1_RECT_F r = {12 * d, (84 + s * 44) * d, B - 12 * d, (84 + s * 44 + 38) * d};
            zone.push_back({r, 300 + s});
            const bool scelta = s == sezione;
            if (scelta || sopra == 300 + s) {
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, scelta ? 0.10f : 0.05f) : D2D1::ColorF(0, 0, 0, scelta ? 0.06f : 0.035f));
                v.rt->FillRoundedRectangle({r, 8 * d, 8 * d}, v.pen);
            }
            if (scelta) {   // segno blu a sinistra, come in Windows 11
                v.pen->SetColor(blu);
                v.rt->FillRoundedRectangle({{r.left + 2 * d, r.top + 11 * d, r.left + 5 * d, r.bottom - 11 * d}, 1.5f * d, 1.5f * d}, v.pen);
            }
            iconaSezione(s, r.left + 26 * d, (r.top + r.bottom) / 2, scelta ? blu : ink);
            v.testo(nomeSezione(s), fSezione, {r.left + 48 * d, r.top, r.right - 6 * d, r.bottom}, ink);
        }

        // ---- chiudi ----
        {
            const D2D1_RECT_F r = {W - 52 * d, 18 * d, W - 20 * d, 50 * d};
            zone.push_back({r, 200});
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, sopra == 200 ? 0.16f : 0.08f) : D2D1::ColorF(0, 0, 0, sopra == 200 ? 0.10f : 0.05f));
            v.rt->FillEllipse({{(r.left + r.right) / 2, (r.top + r.bottom) / 2}, 15 * d, 15 * d}, v.pen);
            v.pen->SetColor(ink);
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            v.rt->DrawLine({cx - 4.5f * d, cy - 4.5f * d}, {cx + 4.5f * d, cy + 4.5f * d}, v.pen, 1.6f * d);
            v.rt->DrawLine({cx + 4.5f * d, cy - 4.5f * d}, {cx - 4.5f * d, cy + 4.5f * d}, v.pen, 1.6f * d);
        }

        // ---- sezione (entra con una piccola scivolata) ----
        const double e = vetro::uscita(kSez);
        const float sx = (float)((1 - e) * 14 * d);
        const D2D1_COLOR_F inkS = {ink.r, ink.g, ink.b, ink.a * (float)e}, tenueS = {tenue.r, tenue.g, tenue.b, tenue.a * (float)e};
        const float L = B + 28 * d + sx, R = W - 28 * d + sx;
        v.testo(nomeSezione(sezione), fTitolo, {L, 20 * d, R - 50 * d, 54 * d}, inkS);
        float y = 72 * d;
        auto riquadro = [&](const D2D1_RECT_F& r, bool evid) {
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (evid ? 0.10f : 0.06f) * (float)e) : D2D1::ColorF(1, 1, 1, (evid ? 0.75f : 0.55f) * (float)e));
            v.rt->FillRoundedRectangle({r, rr, rr}, v.pen);
        };
        auto riga = [&](const std::wstring& titolo, const std::wstring& testo) {   // riga informativa (senza interruttore)
            const D2D1_RECT_F r = {L, y, R, y + 54 * d};
            riquadro(r, false);
            v.testo(titolo, fVoce, {r.left + 16 * d, r.top + 7 * d, r.right - 16 * d, r.top + 29 * d}, inkS);
            v.testo(testo, fSotto, {r.left + 16 * d, r.top + 28 * d, r.right - 16 * d, r.top + 48 * d}, tenueS);
            y += 60 * d;
        };
        for (size_t i = 0; i < voci.size(); i++) {
            if (voci[i].sezione != sezione) continue;
            const D2D1_RECT_F r = {L, y, R, y + 54 * d};
            zone.push_back({r, (int)i});
            riquadro(r, sopra == (int)i);
            v.testo(testi::t(voci[i].nome, voci[i].name), fVoce, {r.left + 16 * d, r.top + 7 * d, r.right - 74 * d, r.top + 29 * d}, inkS);
            v.testo(testi::t(voci[i].spiega, voci[i].explain), fSotto, {r.left + 16 * d, r.top + 28 * d, r.right - 74 * d, r.top + 48 * d}, tenueS);
            levetta(r.right - 58 * d, (r.top + r.bottom) / 2, (float)anim[i], (float)e);
            y += 60 * d;
        }
        if (sezione == S_GENERALE) {
            // lingua: due scelte (i nomi restano ciascuno nella sua lingua)
            const D2D1_RECT_F rl = {L, y, R, y + 64 * d};
            riquadro(rl, false);
            v.testo(testi::t(L"Lingua", L"Language"), fVoce, {rl.left + 16 * d, rl.top, rl.left + 160 * d, rl.bottom}, inkS);
            const float x0 = rl.left + 170 * d, larg = rl.right - 16 * d - x0, h = 34 * d, yl = rl.top + 15 * d;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)e) : D2D1::ColorF(0, 0, 0, 0.06f * (float)e));
            v.rt->FillRoundedRectangle({{x0, yl, x0 + larg, yl + h}, h / 2, h / 2}, v.pen);
            const wchar_t* nomi[2] = {L"Italiano", L"English"};
            for (int i = 0; i < 2; i++) {
                const D2D1_RECT_F r = {x0 + larg * i / 2 + 3 * d, yl + 3 * d, x0 + larg * (i + 1) / 2 - 3 * d, yl + h - 3 * d};
                zone.push_back({r, 100 + i});
                if (valori().lingua == i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * (float)e) : D2D1::ColorF(1, 1, 1, 0.95f * (float)e));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                } else if (sopra == 100 + i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                }
                v.testo(nomi[i], fCentro, r, inkS);
            }
            y += 70 * d;
            riga(testi::t(L"Angoli delle finestre", L"Window corners"), raggioWindhawk > 0
                ? std::wstring(testi::t(L"Arrotondati a ", L"Rounded to ")) + std::to_wstring(raggioWindhawk) + testi::t(L" px dalla mod di Windhawk", L" px by the Windhawk mod")
                : std::wstring(testi::t(L"Quelli di Windows (piu' tondi con la mod di Windhawk)", L"The Windows ones (rounder with the Windhawk mod)")));
            riga(L"FluidWin" + (ver.empty() ? std::wstring() : L" " + ver), testi::t(L"Windows reimmaginato · github.com/clooudfn/FluidWin", L"Windows reimagined · github.com/clooudfn/FluidWin"));
            // diagnostica: versione, Windows, schermi, scheda video, impostazioni (niente dati personali), per chi
            // segnala un problema; il pulsante "Copia" a destra
            {
                const float yr = y;
                riga(testi::t(L"Diagnostica", L"Diagnostics"), testi::t(L"Sistema, schermi, scheda video e impostazioni, da incollare in una segnalazione",
                                                                         L"System, screens, graphics card and settings, to paste into a report"));
                const std::wstring tb = copiataAlle ? testi::t(L"Copiata", L"Copied") : testi::t(L"Copia", L"Copy");
                const float lw = v.larghezza(tb, fCentro) + 30 * d;
                const D2D1_RECT_F rb = {R - 16 * d - lw, yr + 12 * d, R - 16 * d, yr + 42 * d};
                zone.push_back({rb, 400});
                v.pen->SetColor(copiataAlle ? D2D1::ColorF(0.20f, 0.78f, 0.35f, (float)e) : D2D1::ColorF(blu.r, blu.g, blu.b, (sopra == 400 ? 1.0f : 0.88f) * (float)e));
                v.rt->FillRoundedRectangle({rb, 15 * d, 15 * d}, v.pen);
                v.testo(tb, fCentro, rb, D2D1::ColorF(1, 1, 1, (float)e));
            }
        }
        if (sezione == S_NOTCH)
            riga(testi::t(L"Nel notch, sempre", L"Always in the notch"),
                 testi::t(L"Musica, timer, scaffale dei file, microfono e webcam, batteria e cuffie", L"Music, timer, file shelf, microphone and camera, battery and headphones"));
        if (sezione == S_TASTIERA) {
            riga(testi::t(L"Ctrl+Win+T", L"Ctrl+Win+T"), testi::t(L"Tiene la finestra attiva sempre in primo piano (di nuovo per toglierlo)", L"Keeps the active window always on top (again to undo)"));
            riga(testi::t(L"Maiusc+clic destro", L"Shift+right-click"), testi::t(L"Il menu di Windows, anche con il menu di FluidWin acceso", L"The Windows menu, even with the FluidWin menu on"));
        }
        v.finisci();
    }

    // interruttore in stile iOS, k = 0 spento .. 1 acceso; a = trasparenza
    void levetta(float x, float cy, float k, float a = 1) {
        const float d = v.dpi, w = 42 * d, h = 24 * d;
        const D2D1_RECT_F r = {x, cy - h / 2, x + w, cy + h / 2};
        const D2D1_COLOR_F spento = v.scuro ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.14f);
        const D2D1_COLOR_F acceso = D2D1::ColorF(0.20f, 0.78f, 0.35f, 1);
        v.pen->SetColor(D2D1::ColorF(spento.r + (acceso.r - spento.r) * k, spento.g + (acceso.g - spento.g) * k,
                                     spento.b + (acceso.b - spento.b) * k, (spento.a + (acceso.a - spento.a) * k) * a));
        v.rt->FillRoundedRectangle({r, h / 2, h / 2}, v.pen);
        v.pen->SetColor(D2D1::ColorF(1, 1, 1, a));
        v.rt->FillEllipse({{x + h / 2 + (w - h) * k, cy}, h / 2 - 2.5f * d, h / 2 - 2.5f * d}, v.pen);
    }
};

}  // namespace impostazioni
