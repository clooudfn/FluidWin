// Impostazioni di FluidWin: valori salvati in HKCU\Software\FluidWin\Impostazioni e la finestra per cambiarli
// (dal menu dell'icona di FluidWin o dal centro di controllo). Ogni cambio si applica subito.
#pragma once
#include "vetro.h"
#include "testi.h"
#include <windowsx.h>
#include <vector>
#include <functional>

namespace impostazioni {

struct Valori {
    bool gelatina = true;         // finestre "di gelatina" quando si trascinano
    bool genio = true;            // riduzione e ripristino con l'effetto genio
    bool chiusura = true;         // finestre che si rimpiccioliscono e svaniscono quando si chiudono
    bool apertura = true;         // finestre aperte fuori dal dock (Start, ricerca...) che crescono dal centro
    bool onda = true;             // onda di luce multicolore sui bordi dello schermo quando si apre la ricerca
    bool volume = true;           // indicatore del volume di FluidWin al posto di quello di Windows
    bool angolo = true;           // panoramica col mouse nell'angolo in alto a sinistra
    bool winTab = true;           // Win+Tab apre la panoramica di FluidWin
    bool winS = true;             // Alt+Tab apre la ricerca di FluidWin (il nome nel registro e' rimasto WinS)
    bool banner = false;          // banner delle notifiche di FluidWin (spegne quelli di Windows, app per app)
    bool menuDestro = true;       // menu del tasto destro di FluidWin su desktop ed Esplora file (destro.h)
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
}

// ---- la finestra ----
class Finestra {
public:
    std::function<void()> cambiato;   // dopo ogni cambio (gia' salvato)
    int raggioWindhawk = 0;           // raggio della mod di Windhawk (0 = nessuna): riga informativa sugli angoli
    void (*registro)(const char*) = nullptr;

    void apri() {
        if (!v.fin && !v.crea(L"FluidWin.Impostazioni", vetro::procedura<Finestra>, this, true, 2, true, false)) return;
        if (aperta) { SetForegroundWindow(v.fin); return; }   // gia' aperta: viene davanti
        v.aggiornaTema();
        const RECT a = vetro::areaPrincipale(true);
        v.dpi = vetro::dpiMonitor(MonitorFromPoint({a.left, a.top}, MONITOR_DEFAULTTOPRIMARY));
        const float d = v.dpi;
        // schermi bassi (1080 px al 150%, 768 px): se in una colonna le voci non entrano, due colonne affiancate
        colonne = 1;
        if (altezza() * d > (a.bottom - a.top) - 40 * d && 2 * 460 * d <= (a.right - a.left) - 40 * d) colonne = 2;
        const int w = (int)std::lround(460 * colonne * d), h = (int)std::lround(altezza() * d);
        v.posiziona((a.left + a.right - w) / 2, (a.top + a.bottom - h) / 2, w, h, HWND_TOP, SWP_SHOWWINDOW);
        if (!fTitolo) {
            fTitolo = v.formato(20 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fVoce = v.formato(13.5f * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fSotto = v.formato(11.5f * d);
            fSezione = v.formato(11.5f * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fCentro = v.formato(12.5f * d, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        aperta = true; sopra = -1; t0 = vetro::ms(); k = 0;
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
            case WM_KEYDOWN: if (w == VK_ESCAPE) chiudi(); return true;
            case WM_CLOSE: chiudi(); return true;
            case WM_NCHITTEST: {   // si trascina dalla fascia del titolo (non dalla x)
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(v.fin, &p);
                const float d = v.dpi;
                if (p.y < 56 * d && p.x < v.W - 56 * d) { r = HTCAPTION; return true; }
                return false;
            }
        }
        return false;
    }

private:
    // nome e spiegazione in italiano e in inglese
    struct Voce { const wchar_t *nome, *name, *spiega, *explain; bool Valori::*campo; };
    const std::vector<Voce> voci = {
        {L"Finestre di gelatina", L"Jelly windows",
         L"Le finestre si deformano mentre le trascini", L"Windows wobble while you drag them", &Valori::gelatina},
        {L"Effetto genio", L"Genie effect",
         L"Riduzione e ripristino che entrano ed escono dal dock", L"Minimize into the dock and come back out of it", &Valori::genio},
        {L"Animazione di chiusura", L"Close animation",
         L"La finestra si rimpicciolisce e svanisce sul posto", L"The window shrinks and fades away in place", &Valori::chiusura},
        {L"Animazione di apertura", L"Open animation",
         L"Le finestre aperte da Start o dalla ricerca crescono dal centro", L"Windows opened from Start or search grow from the center", &Valori::apertura},
        {L"Indicatore del volume", L"Volume indicator",
         L"Al posto di quello di Windows, in alto al centro", L"Instead of the Windows one, at the top center", &Valori::volume},
        {L"Panoramica dall'angolo", L"Overview from the corner",
         L"Mouse nell'angolo in alto a sinistra", L"Move the mouse to the top-left corner", &Valori::angolo},
        {L"Win+Tab apre la panoramica", L"Win+Tab opens the overview",
         L"Al posto della Visualizzazione attività", L"Instead of Task View", &Valori::winTab},
        {L"Alt+Tab apre la ricerca", L"Alt+Tab opens search",
         L"App, file, impostazioni e calcoli (al posto del selettore)", L"Apps, files, settings and math (instead of the switcher)", &Valori::winS},
        {L"Onda di luce della ricerca", L"Search light wave",
         L"I bordi dello schermo si accendono di colori", L"The edges of the screen light up with color", &Valori::onda},
        {L"Notifiche di FluidWin", L"FluidWin notifications",
         L"Escono dal notch; i banner di Windows si spengono", L"They come out of the notch; the Windows banners are turned off", &Valori::banner},
        {L"Menu del tasto destro", L"Right-click menu",
         L"Su desktop ed Esplora file, diviso per categorie (Maiusc+destro: quello di Windows)", L"On the desktop and in File Explorer, by category (Shift+right-click: the Windows one)", &Valori::menuDestro},
    };
    vetro::Finestra v;
    IDWriteTextFormat *fTitolo = nullptr, *fVoce = nullptr, *fSotto = nullptr, *fSezione = nullptr, *fCentro = nullptr;
    bool aperta = false, dentro = false;
    int sopra = -1;
    ULONGLONG t0 = 0;
    double k = 0;
    std::vector<double> anim;
    // zone: 0..n-1 le voci, 100+i la lingua, 200 chiudi
    struct Zona { D2D1_RECT_F r; int id; };
    std::vector<Zona> zone;

    int colonne = 1;
    int righe() const { return (int)((voci.size() + colonne - 1) / colonne); }
    float altezza() const { return 76 + righe() * 58.0f + 130; }
    static bool stato(const Voce& x) { return valori().*(x.campo); }

    int zonaIn(float x, float y) const {
        for (const auto& z : zone) if (x >= z.r.left && x < z.r.right && y >= z.r.top && y < z.r.bottom) return z.id;
        return -1;
    }

    void clic(int id) {
        if (id < 0) return;
        if (id == 200) { chiudi(); return; }
        if (id >= 100) { valori().lingua = id - 100; testi::lingua() = valori().lingua; }
        else if (id < (int)voci.size()) { bool& b = valori().*(voci[id].campo); b = !b; }
        salva();
        if (cambiato) cambiato();
        vetro::battito(v.fin, 1);
        disegna();
    }

    void disegna() {
        if (!aperta || !v.inizia()) return;
        const float d = v.dpi, W = (float)v.W;
        const D2D1_COLOR_F ink = v.inchiostro(), tenue = v.inchiostro(0.6f);
        zone.clear();
        if (anim.size() != voci.size()) { anim.clear(); for (const auto& x : voci) anim.push_back(stato(x) ? 1 : 0); }
        v.testo(testi::t(L"Impostazioni di FluidWin", L"FluidWin settings"), fTitolo, {24 * d, 18 * d, W - 60 * d, 52 * d}, ink);
        // chiudi
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
        float y = 68 * d;
        const float rr = std::max(8.0f, raggio::vetro() - 12.0f) * d;
        const int nr = righe();
        const float colW = W / colonne;
        for (size_t i = 0; i < voci.size(); i++) {
            const int col = (int)i / nr, riga = (int)i % nr;
            const float ry = y + riga * 58 * d;
            const D2D1_RECT_F r = {col * colW + 16 * d, ry, (col + 1) * colW - 16 * d, ry + 54 * d};
            zone.push_back({r, (int)i});
            if (sopra == (int)i) {
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0, 0, 0, 0.04f));
                v.rt->FillRoundedRectangle({r, rr, rr}, v.pen);
            }
            v.testo(testi::t(voci[i].nome, voci[i].name), fVoce, {r.left + 12 * d, r.top + 7 * d, r.right - 70 * d, r.top + 29 * d}, ink);
            v.testo(testi::t(voci[i].spiega, voci[i].explain), fSotto, {r.left + 12 * d, r.top + 28 * d, r.right - 70 * d, r.top + 48 * d}, tenue);
            levetta(r.right - 58 * d, (r.top + r.bottom) / 2, (float)anim[i]);
        }
        y += nr * 58 * d;
        // lingua: due scelte (i nomi restano ciascuno nella sua lingua)
        y += 8 * d;
        v.testo(testi::t(L"LINGUA", L"LANGUAGE"), fSezione, {28 * d, y, W - 28 * d, y + 22 * d}, tenue);
        y += 26 * d;
        {
            const float x0 = 24 * d, larg = W - 48 * d, h = 34 * d;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(0, 0, 0, 0.06f));
            v.rt->FillRoundedRectangle({{x0, y, x0 + larg, y + h}, h / 2, h / 2}, v.pen);
            const wchar_t* nomi[2] = {L"Italiano", L"English"};
            for (int i = 0; i < 2; i++) {
                const D2D1_RECT_F r = {x0 + larg * i / 2 + 3 * d, y + 3 * d, x0 + larg * (i + 1) / 2 - 3 * d, y + h - 3 * d};
                zone.push_back({r, 100 + i});
                if (valori().lingua == i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(1, 1, 1, 0.95f));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                } else if (sopra == 100 + i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                }
                v.testo(nomi[i], fCentro, r, ink);
            }
            y += h + 18 * d;
        }
        const std::wstring info = raggioWindhawk > 0
            ? std::wstring(testi::t(L"Angoli delle finestre: ", L"Window corners: ")) + std::to_wstring(raggioWindhawk) +
              testi::t(L" px (dalla mod di Windhawk)", L" px (from the Windhawk mod)")
            : std::wstring(testi::t(L"Angoli delle finestre: quelli di Windows (piu' tondi con la mod di Windhawk)",
                                    L"Window corners: the Windows ones (rounder with the Windhawk mod)"));
        v.testo(info, fSotto, {28 * d, y, W - 28 * d, y + 20 * d}, tenue);
        v.finisci();
    }

    // interruttore in stile iOS, k = 0 spento .. 1 acceso
    void levetta(float x, float cy, float k) {
        const float d = v.dpi, w = 42 * d, h = 24 * d;
        const D2D1_RECT_F r = {x, cy - h / 2, x + w, cy + h / 2};
        const D2D1_COLOR_F spento = v.scuro ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.14f);
        const D2D1_COLOR_F acceso = D2D1::ColorF(0.20f, 0.78f, 0.35f, 1);
        v.pen->SetColor(D2D1::ColorF(spento.r + (acceso.r - spento.r) * k, spento.g + (acceso.g - spento.g) * k,
                                     spento.b + (acceso.b - spento.b) * k, spento.a + (acceso.a - spento.a) * k));
        v.rt->FillRoundedRectangle({r, h / 2, h / 2}, v.pen);
        v.pen->SetColor(D2D1::ColorF(1, 1, 1, 1));
        v.rt->FillEllipse({{x + h / 2 + (w - h) * k, cy}, h / 2 - 2.5f * d, h / 2 - 2.5f * d}, v.pen);
    }
};

}  // namespace impostazioni
