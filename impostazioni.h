// Impostazioni di FluidWin: valori salvati in HKCU\Software\FluidWin\Impostazioni e la finestra per cambiarli
// (dal menu dell'icona di FluidWin o dal centro di controllo). Ogni cambio si applica subito.
#pragma once
#include "vetro.h"
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
    v.winS = leggiDword(L"WinS", 1); v.banner = leggiDword(L"Banner", 0);
    // la capsula dei tasti non c'e' piu' (1.1.1): via i suoi valori rimasti dalle versioni prima
    RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE, L"Capsula");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE, L"CapsulaMisura");
}

inline void salva() {
    const Valori& v = valori();
    scriviDword(L"Gelatina", v.gelatina); scriviDword(L"Genio", v.genio); scriviDword(L"Chiusura", v.chiusura);
    scriviDword(L"Apertura", v.apertura); scriviDword(L"OndaRicerca", v.onda);
    scriviDword(L"Volume", v.volume); scriviDword(L"AngoloPanoramica", v.angolo); scriviDword(L"WinTab", v.winTab);
    scriviDword(L"WinS", v.winS); scriviDword(L"Banner", v.banner);
}

// ---- la finestra ----
class Finestra {
public:
    std::function<void()> cambiato;   // dopo ogni cambio (gia' salvato)
    std::wstring infoAngoli;          // riga informativa sugli angoli (raggio e da dove viene)
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
    struct Voce { const wchar_t* nome; const wchar_t* spiega; bool Valori::*campo; };
    const std::vector<Voce> voci = {
        {L"Finestre di gelatina", L"Le finestre si deformano mentre le trascini", &Valori::gelatina},
        {L"Effetto genio", L"Riduzione e ripristino che entrano ed escono dal dock", &Valori::genio},
        {L"Animazione di chiusura", L"La finestra si rimpicciolisce e svanisce sul posto", &Valori::chiusura},
        {L"Animazione di apertura", L"Le finestre aperte da Start o dalla ricerca crescono dal centro", &Valori::apertura},
        {L"Indicatore del volume", L"Al posto di quello di Windows, in alto al centro", &Valori::volume},
        {L"Panoramica dall'angolo", L"Mouse nell'angolo in alto a sinistra", &Valori::angolo},
        {L"Win+Tab apre la panoramica", L"Al posto della Visualizzazione attività", &Valori::winTab},
        {L"Alt+Tab apre la ricerca", L"App, file, impostazioni e calcoli (al posto del selettore)", &Valori::winS},
        {L"Onda di luce della ricerca", L"I bordi dello schermo si accendono di colori", &Valori::onda},
        {L"Banner delle notifiche FluidWin", L"Escono dall'isola; quelli di Windows si spengono", &Valori::banner},
    };
    vetro::Finestra v;
    IDWriteTextFormat *fTitolo = nullptr, *fVoce = nullptr, *fSotto = nullptr, *fSezione = nullptr, *fCentro = nullptr;
    bool aperta = false, dentro = false;
    int sopra = -1;
    ULONGLONG t0 = 0;
    double k = 0;
    std::vector<double> anim;
    // zone: 0..n-1 le voci, 200 chiudi
    struct Zona { D2D1_RECT_F r; int id; };
    std::vector<Zona> zone;

    int colonne = 1;
    int righe() const { return (int)((voci.size() + colonne - 1) / colonne); }
    float altezza() const { return 76 + righe() * 58.0f + 64; }
    static bool stato(const Voce& x) { return valori().*(x.campo); }

    int zonaIn(float x, float y) const {
        for (const auto& z : zone) if (x >= z.r.left && x < z.r.right && y >= z.r.top && y < z.r.bottom) return z.id;
        return -1;
    }

    void clic(int id) {
        if (id < 0) return;
        if (id == 200) { chiudi(); return; }
        if (id < (int)voci.size()) { bool& b = valori().*(voci[id].campo); b = !b; }
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
        v.testo(L"Impostazioni di FluidWin", fTitolo, {24 * d, 18 * d, W - 60 * d, 52 * d}, ink);
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
            v.testo(voci[i].nome, fVoce, {r.left + 12 * d, r.top + 7 * d, r.right - 70 * d, r.top + 29 * d}, ink);
            v.testo(voci[i].spiega, fSotto, {r.left + 12 * d, r.top + 28 * d, r.right - 70 * d, r.top + 48 * d}, tenue);
            levetta(r.right - 58 * d, (r.top + r.bottom) / 2, (float)anim[i]);
        }
        y += nr * 58 * d;
        y += 14 * d;
        v.testo(infoAngoli, fSotto, {28 * d, y, W - 28 * d, y + 20 * d}, tenue);
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
