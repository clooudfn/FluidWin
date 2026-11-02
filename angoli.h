// Angoli piu' tondi per tutte le finestre, senza toccare DWM (che li disegna sempre da 8 px).
// Sopra ogni angolo visibile di una finestra si mettono due finestrelle trasparenti ai clic:
//  - la prima proietta cio' che sta DIETRO l'angolo, con le miniature vive di DWM (quelle di Alt+Tab): un'altra
//    finestra, il desktop o uno sfondo animato si vedono muoversi. Le miniature sono rettangolari: la curva e' fatta
//    di strisce alte 1 px, piu' un pixel per riga con opacita' parziale (curva sfumata, niente gradini)
//  - la seconda, sopra, disegna l'ombra morbida e il bordino sottile lungo la nuova curva (nell'angolo "tagliato"
//    l'ombra di Windows non c'e')
// Le miniature si aggiornano sul posto e si ricreano solo se cambia la finestra dietro (le nuove prima di togliere le
// vecchie: niente sfarfallio). Mentre la finestra si muove gli angoli spariscono e tornano quando si ferma.
// Gli angoli coperti da altre finestre non si creano (meno lavoro per DWM).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include "scala.h"
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace angoli {

class Motore {
public:
    bool (*idonea)(HWND) = nullptr;            // finestra d'app normale (con barra del titolo)
    void (*registro)(const char*) = nullptr;
    int raggio = 16;                           // px a 96 dpi

    bool inizializza(HINSTANCE inst) {
        gIstanza() = this;
        istanza = inst;
        WNDCLASSW wc{}; wc.hInstance = inst; wc.lpszClassName = L"FluidWin.Angolo";
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT { return m == WM_CLOSE ? 0 : DefWindowProcW(h, m, w, l); };   // mai distrutti da WM_CLOSE
        RegisterClassW(&wc);
        WNDCLASSW wt{}; wt.lpfnWndProc = procTempi; wt.hInstance = inst; wt.lpszClassName = L"FluidWin.AngoliTempi";
        RegisterClassW(&wt);
        tempi = CreateWindowExW(0, wt.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        return tempi != nullptr;
    }

    void chiudi() { for (auto& [h, f] : finestre) liberaFinestra(f); finestre.clear(); }

    // ricontrollo fra poco; rinvia = sposta piu' avanti un controllo gia' previsto (finestra ancora in movimento)
    void programma(UINT ms = 60, bool rinvia = false) {
        if (!tempi) return;
        const ULONGLONG quando = GetTickCount64() + ms;
        if (previsto && !rinvia && previsto <= quando) return;
        previsto = quando;
        SetTimer(tempi, 1, ms, nullptr);
    }

    // la finestra si e' mossa o ha cambiato dimensione: gli angoli spariscono subito (niente angolino che insegue)
    void cambiata(HWND h) {
        auto it = finestre.find(h);
        if (it == finestre.end()) { if (GetAncestor(h, GA_ROOT) == h) programma(200); return; }
        RECT w; GetWindowRect(h, &w);
        if (EqualRect(&w, &it->second.finRect) && !IsIconic(h)) return;   // solo z-order (o solo lo stile)
        nascondiTutti(it->second);
        if (!it->second.trascinando) programma(150, true);
    }

    void spostamento(HWND h, bool inizio) {
        auto it = finestre.find(h);
        if (it == finestre.end()) return;
        it->second.trascinando = inizio;
        if (!inizio) programma(150, true);
    }

    // la finestra vera e' stata nascosta (gelatina, genio): al suo posto c'e' la foto, che ha gia' gli angoli tondi
    void nascondiPer(HWND h) { auto it = finestre.find(h); if (it != finestre.end()) nascondiTutti(it->second); }

    // la finestra vera sta ricomparendo: gli angoli tornano subito
    void mostraPer(HWND h) {
        auto it = finestre.find(h);
        if (it != finestre.end()) { it->second.forzata = GetTickCount64() + 1500; it->second.trascinando = false; }
        aggiorna();
    }

    void distrutta(HWND h) {
        auto it = finestre.find(h);
        if (it != finestre.end()) { liberaFinestra(it->second); finestre.erase(it); }
    }

    void sospendi(bool si) { sospesi = si; if (si) for (auto& [h, f] : finestre) nascondiTutti(f); else programma(); }

    void controllo() { aggiorna(); }

    // la finestra (e i suoi angoli) cosi' come stanno: per capire se una finestra e' nostra
    bool nostra(HWND w) const {
        wchar_t c[32] = L""; GetClassNameW(w, c, 32);
        return !wcscmp(c, L"FluidWin.Angolo");
    }

private:
    struct Striscia { RECT dest; BYTE opacita; };
    struct Angolo {
        HWND fin = nullptr, ombra = nullptr;
        std::vector<HWND> strati;
        std::vector<HTHUMBNAIL> mini;
        std::vector<RECT> firma;   // posizione dell'angolo e delle finestre dietro all'ultimo aggiornamento
        bool visibile = false, esteso = false;
        RECT posato{};
    };
    struct Fin {
        Angolo a[4];
        RECT posato{};          // rettangolo visibile su cui sono posati gli angoli
        RECT finRect{};         // GetWindowRect in quel momento (per capire se si e' mossa davvero)
        RECT bordi{}, candidato{};   // bordi invisibili misurati a finestra stabile
        bool bordiOk = false;
        bool trascinando = false, attiva = false;
        ULONGLONG forzata = 0;
    };

    static Motore*& gIstanza() { static Motore* m = nullptr; return m; }

    HINSTANCE istanza = nullptr;
    HWND tempi = nullptr;
    ULONGLONG previsto = 0;
    bool sospesi = false;
    std::unordered_map<HWND, Fin> finestre;
    int Rcalc = 0, Ecalc = -1;              // raggio e sporgenza in pixel per cui sono calcolate strisce e ombre
    std::vector<Striscia> strisce[2][4];    // [allargato][angolo]
    std::vector<UINT> ombre[2][4];          // pixel (premoltiplicati) dell'ombra di ogni angolo

    static RECT visibile(HWND h) {
        RECT v{}; if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &v, sizeof(v)))) GetWindowRect(h, &v);
        return v;
    }

    void scrivi(const char* s) const { if (registro) registro(s); }

    // Rettangolo visibile stabile: quello della finestra meno i bordi invisibili. Il rettangolo "visibile" di DWM
    // salta di ~7 px (e in ritardo) quando la finestra diventa o smette di essere trasparente per gelatina e genio:
    // gli angoli lo prenderebbero per uno spostamento e sparirebbero per un attimo. I bordi si accettano solo se
    // misurati uguali due volte di fila, a finestra non trasparente
    RECT rettStabile(HWND h, Fin& f) {
        RECT w; GetWindowRect(h, &w);
        const RECT v = visibile(h);
        const RECT b = {v.left - w.left, v.top - w.top, w.right - v.right, w.bottom - v.bottom};
        const bool plausibile = b.left >= 0 && b.top >= 0 && b.right >= 0 && b.bottom >= 0 &&
                                b.left <= 24 && b.top <= 24 && b.right <= 24 && b.bottom <= 24;
        if (plausibile && !GetPropW(h, L"FluidWin.Nascosta")) {
            if (EqualRect(&b, &f.candidato)) { f.bordi = b; f.bordiOk = true; }
            f.candidato = b;
        }
        if (!f.bordiOk) return v;
        return {w.left + f.bordi.left, w.top + f.bordi.top, w.right - f.bordi.right, w.bottom - f.bordi.bottom};
    }

    // Ombra di Windows 11 misurata sullo schermo (opacita' del nero a d px dal bordo della finestra attiva):
    // sopra leggera e corta, ai lati media, sotto forte e lunga (la luce viene dall'alto)
    static double ombraSopra(double d) { return d < 0 ? 0 : 0.063 * std::exp(-std::max(0.0, d - 1) / 5.0); }
    static double ombraLato(double d)  { return d < 0 ? 0 : 0.115 * std::exp(-std::max(0.0, d - 1) / 8.5); }
    static double ombraSotto(double d) { return d < 0 ? 0 : 0.21 * std::exp(-std::max(0.0, d - 1) / 27.0); }

    // Strisce e ombra di un angolo. La finestrella e' grande R+E: sporge di E px fuori dalla finestra.
    //  - dentro la finestra (fuori dalla curva nuova): la miniatura di cio' che c'e' dietro, piena, e sopra la nostra
    //    ombra e il bordino
    //  - fuori dalla finestra: l'ombra vera di Windows segue la curva da 8 px e farebbe una punta grigia. Non la si
    //    ridisegna (non si sa quanto e' scura su quello sfondo): si mette sopra la miniatura di cio' che c'e' dietro,
    //    SENZA ombra, con un'opacita' che la schiarisce quanto basta. Ombra vera x (1 - opacita') = ombra di un angolo
    //    da R px: il rapporto dipende solo dalla forma, quindi va bene su qualsiasi sfondo e anche da inattiva
    // "esteso" = false (dietro non c'e' la stessa cosa dentro e fuori): solo la parte dentro, R x R
    static double decadimento(double peso, bool sotto) { return (1 - peso) * 8.5 + peso * (sotto ? 27.0 : 5.0); }

    void prepara(int R, int E) {
        if (R == Rcalc && E == Ecalc) return;
        Rcalc = R; Ecalc = E;
        const int R8 = (int)std::lround(8.0 * R / std::max(1, raggio));   // raggio di Windows ai dpi di R
        for (int est = 0; est < 2; est++) for (int k = 0; k < 4; k++) {
            const int e = est ? E : 0, P = R + e;
            auto& st = strisce[est][k];
            st.clear();
            const bool destra = k & 1, sotto = k & 2;
            // righe dentro la finestra: la parte esterna alla curva, e un pixel sfumato
            for (int j = 0; j < R; j++) {
                const double dy = R - (j + 0.5);
                const double fuori = R - std::sqrt(std::max(0.0, (double)R * R - dy * dy));
                const int pieni = (int)std::floor(fuori);
                const double resto = fuori - pieni;
                const int ly = sotto ? R - 1 - j : e + j;
                if (pieni > 0) { const int lx = destra ? R - pieni : e; st.push_back({{lx, ly, lx + pieni, ly + 1}, 255}); }
                if (resto > 0.04) { const int lx = destra ? R - pieni - 1 : e + pieni; st.push_back({{lx, ly, lx + 1, ly + 1}, (BYTE)std::lround(resto * 255)}); }
            }
            // centri delle due curve (nuova e di Windows) in coordinate locali
            const double cx = destra ? R - R : e + R, cy = sotto ? R - R : e + R;
            const double cx8 = destra ? R - R8 : e + R8, cy8 = sotto ? R - R8 : e + R8;
            auto fuoriDa = [&](double px, double py, double ccx, double ccy, double r, double& ox, double& oy) {
                ox = destra ? std::max(0.0, px - ccx) : std::max(0.0, ccx - px);
                oy = sotto ? std::max(0.0, py - ccy) : std::max(0.0, ccy - py);
                return std::hypot(ox, oy) - r;
            };
            // fuori dalla finestra: celle di 2 px dove l'ombra vera va schiarita
            if (e > 0) for (int y = 0; y < P; y += 2) for (int x = 0; x < P; x += 2) {
                const double px = x + 1, py = y + 1;
                const bool dentroX = destra ? px < R : px > e, dentroY = sotto ? py < R : py > e;
                if (dentroX && dentroY) continue;   // dentro la finestra: gia' fatto sopra
                double ox, oy, ox8, oy8;
                const double d = fuoriDa(px, py, cx, cy, R, ox, oy), d8 = fuoriDa(px, py, cx8, cy8, R8, ox8, oy8);
                if (d - d8 < 0.05) continue;   // l'ombra vera e' gia' quella giusta (lungo i lati)
                const double dist = std::hypot(ox, oy), peso = dist > 0 ? oy * oy / (dist * dist) : 0;
                const double alfa = 1 - std::exp(-(d - std::max(0.0, d8)) / decadimento(peso, sotto));
                if (alfa < 0.03) continue;
                st.push_back({{x, y, std::min(P, x + 2), std::min(P, y + 2)}, (BYTE)std::lround(std::min(1.0, alfa) * 255)});
            }
            // nostra ombra (solo dentro la finestra, fuori dalla curva) e bordino lungo la curva
            auto& om = ombre[est][k];
            om.assign((size_t)P * P, 0);
            for (int y = 0; y < P; y++) for (int x = 0; x < P; x++) {
                const double px = x + 0.5, py = y + 0.5;
                const bool dentroX = destra ? px < R : px > e, dentroY = sotto ? py < R : py > e;
                if (!(dentroX && dentroY)) continue;
                double ox, oy;
                const double d = fuoriDa(px, py, cx, cy, R, ox, oy);
                if (d <= -0.5) continue;
                const double dist = std::hypot(ox, oy), peso = dist > 0 ? oy * oy / (dist * dist) : 0;
                // intensita' misurate dell'ombra di Windows: lato 0,115, sopra 0,063, sotto 0,21
                const double forza = (1 - peso) * 0.115 + peso * (sotto ? 0.21 : 0.063);
                double al = d > 0 ? forza * std::exp(-d / decadimento(peso, sotto)) : 0;
                if (ox > 0 && oy > 0) al = std::min(1.0, al + std::max(0.0, 1.0 - std::abs(d - 0.5)) * 0.22);
                om[(size_t)y * P + x] = (UINT)std::lround(std::min(1.0, al) * 255) << 24;
            }
        }
    }

    // finestre sotto h che coprono il punto p, dall'alto in basso, fino alla prima opaca (o al desktop)
    std::vector<HWND> dietro(HWND bersaglio, POINT p) const {
        std::vector<HWND> v;
        for (HWND h = GetWindow(bersaglio, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (!IsWindowVisible(h) || IsIconic(h)) continue;
            DWORD cl = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof(cl));
            if (cl) continue;
            RECT r; GetWindowRect(h, &r);
            if (!PtInRect(&r, p)) continue;
            const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            if (ex & WS_EX_TRANSPARENT) continue;   // overlay trasparenti ai clic (anche i nostri)
            wchar_t c[64] = L""; GetClassNameW(h, c, 64);
            if (!wcsncmp(c, L"FluidWin.", 9)) continue;
            v.push_back(h);
            const bool desktop = !wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW");
            if (!desktop && opaca(h)) break;
            if (!wcscmp(c, L"Progman")) break;
        }
        return v;
    }

    // opaca: non layered, oppure layered con un'opacita' unica piena (Chromium, Electron: Claude, Brave...)
    static bool opaca(HWND h) {
        if (!(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED)) return true;
        BYTE a = 0; DWORD fl = 0; COLORREF k;
        return GetLayeredWindowAttributes(h, &k, &a, &fl) && (fl & LWA_ALPHA) && !(fl & LWA_COLORKEY) && a == 255;
    }

    bool daMostrare(HWND h) const {
        if (sospesi || !IsWindowVisible(h) || IsIconic(h) || IsZoomed(h) || (idonea && !idonea(h))) return false;
        DWORD cl = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof(cl));
        if (cl) return false;
        auto it = finestre.find(h);
        if (it != finestre.end() && it->second.forzata > GetTickCount64()) return true;
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) {   // trasparente adesso (genio, gelatina)
            BYTE a = 255; DWORD fl = 0; COLORREF k;
            if (GetLayeredWindowAttributes(h, &k, &a, &fl) && (fl & LWA_ALPHA) && a < 250) return false;
        }
        return true;
    }

    // l'angolo k si vede? (il suo punto interno non e' coperto da un'altra finestra)
    bool angoloVisibile(HWND h, const RECT& v, int k, int R) const {
        const bool destra = k & 1, sotto = k & 2;
        const POINT p = {destra ? v.right - R / 2 : v.left + R / 2, sotto ? v.bottom - R / 2 : v.top + R / 2};
        HWND w = WindowFromPoint(p);
        if (!w) return false;
        w = GetAncestor(w, GA_ROOT);
        if (w == h) return true;
        wchar_t c[32] = L""; GetClassNameW(w, c, 32);
        return !wcsncmp(c, L"FluidWin.", 9);   // capsula o angoli (nostri): sono sopra di lei, va bene
    }

    void nascondi(Angolo& a) {
        if (!a.visibile) return;
        ShowWindow(a.fin, SW_HIDE); ShowWindow(a.ombra, SW_HIDE);
        a.visibile = false;
    }
    void nascondiTutti(Fin& f) { for (auto& a : f.a) nascondi(a); }

    void liberaAngolo(Angolo& a) {
        for (HTHUMBNAIL t : a.mini) if (t) DwmUnregisterThumbnail(t);
        a.mini.clear(); a.strati.clear(); a.firma.clear();
        if (a.fin) DestroyWindow(a.fin);
        if (a.ombra) DestroyWindow(a.ombra);
        a.fin = a.ombra = nullptr; a.visibile = false;
    }
    void liberaFinestra(Fin& f) { for (auto& a : f.a) liberaAngolo(a); }

    void creaAngolo(Angolo& a, HWND h) {
        const DWORD top = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) ? WS_EX_TOPMOST : 0;
        a.fin = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | top,
                                L"FluidWin.Angolo", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, istanza, nullptr);
        SetLayeredWindowAttributes(a.fin, 0, 255, LWA_ALPHA);
        a.ombra = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | top,
                                  L"FluidWin.Angolo", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, istanza, nullptr);
        for (HWND w : {a.fin, a.ombra}) {
            BOOL niente = TRUE; DwmSetWindowAttribute(w, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
            int nessuno = 1; DwmSetWindowAttribute(w, DWMWA_WINDOW_CORNER_PREFERENCE, &nessuno, sizeof(nessuno));
        }
    }

    void disegnaOmbra(Angolo& a, int k, int x0, int y0, int R, bool est, double forza) {
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), R, -R, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
        HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) { if (dib) DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, scr); return; }   // memoria finita
        HGDIOBJ vecchio = SelectObject(mem, dib);
        // finestra non in primo piano: Windows le da' un'ombra piu' leggera
        UINT* px = (UINT*)bits;
        const auto& om = ombre[est][k];
        for (size_t i = 0; i < om.size(); i++) px[i] = (UINT)std::lround((om[i] >> 24) * forza) << 24;
        POINT pos = {x0, y0}, zero = {0, 0}; SIZE sz = {R, R};
        BLENDFUNCTION bl = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(a.ombra, scr, &pos, &sz, mem, &zero, 0, &bl, ULW_ALPHA);
        SelectObject(mem, vecchio); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, scr);
    }

    // la finestra appena sopra h, saltando la sua capsula e i suoi angoli (gli angoli vanno sopra la capsula)
    HWND sopraDi(HWND h, const Fin& f) const {
        HWND s = GetWindow(h, GW_HWNDPREV);
        while (s) {
            wchar_t c[32] = L""; GetClassNameW(s, c, 32);
            bool mio = false;
            for (const auto& a : f.a) if (s == a.fin || s == a.ombra) mio = true;
            if (!mio && !wcscmp(c, L"FluidWin.Capsula") && (HWND)GetWindowLongPtrW(s, GWLP_USERDATA) == h) mio = true;
            if (!mio) break;
            s = GetWindow(s, GW_HWNDPREV);
        }
        return s;
    }

    void aggiorna() {
        KillTimer(tempi, 1);
        previsto = 0;
        std::vector<HWND> viste;
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (!daMostrare(h)) continue;
            Fin& f = finestre[h];
            const RECT v = rettStabile(h, f);
            const UINT dpi = (UINT)std::lround(scala::di(h) * 96);   // scala a cui si vede (vedi scala.h)
            const int R = (int)std::lround(raggio * (dpi ? dpi : 96) / 96.0);
            if (v.right - v.left < 4 * R || v.bottom - v.top < 4 * R) continue;
            viste.push_back(h);
            if (f.trascinando) continue;
            GetWindowRect(h, &f.finRect);
            const int E = (int)std::lround(16 * (dpi ? dpi : 96) / 96.0);   // sporgenza fuori dalla finestra (la punta d'ombra)
            prepara(R, E);
            const bool spostata = !EqualRect(&v, &f.posato);
            f.posato = v;
            const bool attiva = GetForegroundWindow() == h;
            const bool cambiaAttiva = attiva != f.attiva;
            f.attiva = attiva;
            const HWND base = sopraDi(h, f);
            for (int k = 0; k < 4; k++) {
                Angolo& a = f.a[k];
                if (!angoloVisibile(h, v, k, R)) { nascondi(a); continue; }
                if (!a.fin) creaAngolo(a, h);
                const bool destra = k & 1, sotto = k & 2;
                const POINT p = {destra ? v.right - 1 : v.left, sotto ? v.bottom - 1 : v.top};
                const std::vector<HWND> strati = dietro(h, p);
                if (strati.empty()) { nascondi(a); continue; }
                // allargato fuori dalla finestra solo se anche li' dietro c'e' la stessa cosa (altrimenti si
                // mostrerebbe la finestra sbagliata) e se si resta sullo schermo
                const POINT pe = {destra ? v.right - 1 + E : v.left - E, sotto ? v.bottom - 1 + E : v.top - E};
                const HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
                // (lo schiarimento dell'ombra funziona solo con un unico strato opaco dietro)
                const bool est = mon && MonitorFromPoint(pe, MONITOR_DEFAULTTONULL) == mon && strati.size() == 1 && dietro(h, pe) == strati;
                const int e = est ? E : 0, P = R + e;
                const int x0 = destra ? v.right - R : v.left - e, y0 = sotto ? v.bottom - R : v.top - e;
                const auto& st = strisce[est][k];
                // finestre dietro cambiate: miniature nuove create PRIMA di togliere le vecchie (niente buchi)
                if (strati != a.strati || a.mini.empty() || est != a.esteso) {
                    std::vector<HTHUMBNAIL> nuove;
                    for (size_t s = 0; s < st.size(); s++) for (int l = (int)strati.size() - 1; l >= 0; l--) {
                        HTHUMBNAIL th = nullptr;
                        nuove.push_back(SUCCEEDED(DwmRegisterThumbnail(a.fin, strati[l], &th)) ? th : nullptr);
                    }
                    for (HTHUMBNAIL th : a.mini) if (th) DwmUnregisterThumbnail(th);
                    a.mini = nuove; a.strati = strati; a.firma.clear(); a.esteso = est;
                }
                // quale pezzo della finestra dietro, dove, con che opacita': solo se qualcosa si e' spostato
                std::vector<RECT> firma = {{x0, y0, P, (LONG)a.mini.size()}};
                for (HWND s : a.strati) { RECT sr; GetWindowRect(s, &sr); firma.push_back(sr); }
                const bool uguale = firma.size() == a.firma.size() && std::equal(firma.begin(), firma.end(), a.firma.begin(),
                                        [](const RECT& x, const RECT& y) { return EqualRect(&x, &y) != 0; });
                a.firma = firma;
                size_t i = 0;
                if (!uguale) for (const Striscia& s : st) for (int l = (int)a.strati.size() - 1; l >= 0; l--, i++) {
                    if (i >= a.mini.size() || !a.mini[i]) continue;
                    RECT sr; GetWindowRect(a.strati[l], &sr);
                    DWM_THUMBNAIL_PROPERTIES pr{};
                    pr.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
                    pr.rcDestination = s.dest;
                    pr.rcSource = {x0 + s.dest.left - sr.left, y0 + s.dest.top - sr.top, x0 + s.dest.right - sr.left, y0 + s.dest.bottom - sr.top};
                    pr.fVisible = TRUE; pr.opacity = s.opacita; pr.fSourceClientAreaOnly = FALSE;
                    DwmUpdateThumbnailProperties(a.mini[i], &pr);
                }
                const RECT pos = {x0, y0, x0 + P, y0 + P};
                const bool mossa = !EqualRect(&pos, &a.posato);
                if (spostata || mossa || cambiaAttiva || !a.visibile) {
                    a.posato = pos;
                    disegnaOmbra(a, k, x0, y0, P, est, attiva ? 1.0 : 0.6);
                }
                // ordine: ombra sopra le miniature, entrambe subito sopra la finestra (e la sua capsula)
                if (mossa || !a.visibile || GetWindow(a.ombra, GW_HWNDPREV) != base || GetWindow(a.fin, GW_HWNDPREV) != a.ombra) {
                    const HWND dopo = base ? base : ((GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_TOP);
                    SetWindowPos(a.fin, dopo, x0, y0, P, P, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    SetWindowPos(a.ombra, dopo, x0, y0, P, P, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    a.visibile = true;
                }
            }
        }
        // finestre sparite, ridotte, ingrandite o non piu' adatte
        for (auto it = finestre.begin(); it != finestre.end();) {
            if (std::find(viste.begin(), viste.end(), it->first) != viste.end()) { ++it; continue; }
            if (!IsWindow(it->first)) { liberaFinestra(it->second); it = finestre.erase(it); continue; }
            nascondiTutti(it->second); ++it;
        }
    }

    static LRESULT CALLBACK procTempi(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_TIMER && w == 1) { if (Motore* d = gIstanza()) d->aggiorna(); return 0; }
        return DefWindowProcW(h, m, w, l);
    }
};

} // namespace angoli
