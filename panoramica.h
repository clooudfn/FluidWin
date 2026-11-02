// Panoramica delle finestre (come Mission Control): Win+Tab o il mouse nell'angolo in alto a sinistra, e tutte le
// finestre aperte si dispongono a griglia sopra lo sfondo sfocato. Clic su una finestra per portarla in primo piano,
// Esc o clic fuori per tornare com'era. Le finestre sono miniature dal vivo di DWM (DwmRegisterThumbnail): nessuna
// finestra vera viene spostata o toccata.
#pragma once
#include "vetro.h"
#include "menu.h"
#include <windowsx.h>
#include <vector>
#include <functional>

namespace panoramica {

class Panoramica {
public:
    std::function<bool(HWND)> idonea;   // finestra d'app normale
    bool aperta() const { return inCorso; }

    // Apre la panoramica e ne gestisce i messaggi finche' non si chiude (come un menu). Se e' gia' aperta la chiude.
    void apri() {
        if (inCorso) { chiudendo = true; scelta = nullptr; return; }
        if (!v.fin && !v.crea(L"FluidWin.Panoramica", vetro::procedura<Panoramica>, this, true, 1)) return;
        raccogli();
        if (el.empty()) return;
        v.aggiornaTema();
        schermo = vetro::areaPrincipale();
        v.posiziona(schermo.left, schermo.top, schermo.right - schermo.left, schermo.bottom - schermo.top, HWND_TOPMOST, SWP_SHOWWINDOW);
        if (!fTitolo) fTitolo = v.formato(12.5f * v.dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        disponi();
        // dal basso verso l'alto nell'ordine di sovrapposizione: le miniature registrate dopo stanno sopra
        std::vector<Elemento*> perZ; for (auto& e : el) perZ.push_back(&e);
        std::sort(perZ.begin(), perZ.end(), [](const Elemento* a, const Elemento* b) { return a->z > b->z; });
        for (Elemento* e : perZ) if (FAILED(DwmRegisterThumbnail(v.fin, e->h, &e->th))) e->th = nullptr;
        prima = GetForegroundWindow();
        inCorso = true; chiudendo = false; scelta = nullptr; sopra = -1; k = 0;
        v.alfa(1);
        SetForegroundWindow(v.fin);
        ciclo();
        for (auto& e : el) if (e.th) DwmUnregisterThumbnail(e.th);
        el.clear();
        ShowWindow(v.fin, SW_HIDE);
        inCorso = false;
        if (scelta && IsWindow(scelta)) {
            if (IsIconic(scelta)) ShowWindow(scelta, SW_RESTORE);
            SwitchToThisWindow(scelta, TRUE);
        } else if (prima && IsWindow(prima)) SetForegroundWindow(prima);
    }

    bool messaggio(HWND, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
        r = 0;
        switch (m) {
            case WM_PAINT: disegna(); return true;
            case WM_MOUSEMOVE: {
                const int s = elementoIn(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                if (s != sopra) { sopra = s; disegna(); }
                return true;
            }
            case WM_LBUTTONUP: {
                if (chiudendo || k < 0.6) return true;
                const int s = elementoIn(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                scelta = s >= 0 ? el[s].h : nullptr;
                chiudendo = true;
                return true;
            }
            case WM_RBUTTONUP: chiudendo = true; scelta = nullptr; return true;
            case WM_KEYDOWN:
                if (w == VK_ESCAPE) { chiudendo = true; scelta = nullptr; }
                else if (w == VK_RETURN && sopra >= 0) { chiudendo = true; scelta = el[sopra].h; }
                else if (w == VK_RIGHT || w == VK_TAB || w == VK_DOWN) { sopra = (sopra + 1) % (int)el.size(); disegna(); }
                else if (w == VK_LEFT || w == VK_UP) { sopra = (sopra - 1 + (int)el.size()) % (int)el.size(); disegna(); }
                return true;
            case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && inCorso) { chiudendo = true; scelta = nullptr; } return true;
            case WM_CLOSE: if (inCorso) { chiudendo = true; scelta = nullptr; } return true;
        }
        return false;
    }

private:
    struct Elemento {
        HWND h = nullptr;
        HTHUMBNAIL th = nullptr;
        RECT da{};          // dov'e' la finestra (parte visibile, coordinate della panoramica)
        RECT fin{};         // rettangolo intero della finestra (la miniatura lo comprende tutto)
        D2D1_RECT_F a{};    // dove va nella griglia (parte visibile)
        std::wstring titolo;
        int z = 0;          // 0 = in cima
    };
    vetro::Finestra v;
    IDWriteTextFormat* fTitolo = nullptr;
    std::vector<Elemento> el;
    RECT schermo{};
    HWND prima = nullptr, scelta = nullptr;
    bool inCorso = false, chiudendo = false;
    int sopra = -1;
    double k = 0;   // 0 finestre al loro posto, 1 griglia

    void raccogli() {
        el.clear();
        const RECT mon = vetro::areaPrincipale();
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (!IsWindowVisible(h) || IsIconic(h) || (idonea && !idonea(h))) continue;
            DWORD cl = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof(cl));
            if (cl) continue;
            Elemento e; e.h = h; e.z = (int)el.size();
            GetWindowRect(h, &e.fin);
            RECT vis{}; if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) vis = e.fin;
            RECT x{}; if (!IntersectRect(&x, &vis, &mon)) continue;   // solo lo schermo principale
            if (vis.right - vis.left < 60 || vis.bottom - vis.top < 40) continue;
            e.da = {vis.left - mon.left, vis.top - mon.top, vis.right - mon.left, vis.bottom - mon.top};
            OffsetRect(&e.fin, -mon.left, -mon.top);
            wchar_t t[256] = L""; GetWindowTextW(h, t, 256); e.titolo = t;
            el.push_back(e);
        }
        // ordine di lettura (dall'alto a sinistra), come le finestre stavano sullo schermo: meno incroci
        std::stable_sort(el.begin(), el.end(), [](const Elemento& a, const Elemento& b) {
            const int ra = (a.da.top + a.da.bottom) / 2 / 300, rb = (b.da.top + b.da.bottom) / 2 / 300;
            return ra != rb ? ra < rb : a.da.left < b.da.left;
        });
    }

    // griglia: righe e colonne che lasciano le miniature piu' grandi possibile
    void disponi() {
        const float d = v.dpi, W = (float)v.W, H = (float)v.H;
        const float x0 = 48 * d, y0 = 56 * d, x1 = W - 48 * d, y1 = H - 120 * d;   // in basso resta il posto del dock
        const int n = (int)el.size();
        int migliorC = 1; float migliore = 0;
        for (int c = 1; c <= n; c++) {
            const int r = (n + c - 1) / c;
            const float cw = (x1 - x0) / c, ch = (y1 - y0) / r;
            float tot = 0;
            for (const auto& e : el) {
                const float w = (float)(e.da.right - e.da.left), h = (float)(e.da.bottom - e.da.top);
                const float s = std::min({1.0f, (cw - 36 * d) / w, (ch - 48 * d) / h});
                tot += std::max(0.0f, s) * std::max(0.0f, s) * w * h;
            }
            if (tot > migliore) { migliore = tot; migliorC = c; }
        }
        const int C = migliorC, R = (n + C - 1) / C;
        const float cw = (x1 - x0) / C, ch = (y1 - y0) / R;
        for (int i = 0; i < n; i++) {
            Elemento& e = el[i];
            const int r = i / C, c = i % C;
            const int inRiga = std::min(C, n - r * C);   // l'ultima riga, se incompleta, va al centro
            const float ox = (C - inRiga) * cw / 2;
            const float w = (float)(e.da.right - e.da.left), h = (float)(e.da.bottom - e.da.top);
            const float s = std::max(0.05f, std::min({1.0f, (cw - 36 * d) / w, (ch - 48 * d) / h}));
            const float cx = x0 + ox + cw * (c + 0.5f), cy = y0 + ch * (r + 0.5f) - 10 * d;
            e.a = {cx - w * s / 2, cy - h * s / 2, cx + w * s / 2, cy + h * s / 2};
        }
    }

    // rettangolo della parte visibile adesso (fra il posto vero e la griglia)
    D2D1_RECT_F attuale(const Elemento& e, double t) const {
        const float f = (float)t;
        return {e.da.left + (e.a.left - e.da.left) * f, e.da.top + (e.a.top - e.da.top) * f,
                e.da.right + (e.a.right - e.da.right) * f, e.da.bottom + (e.a.bottom - e.da.bottom) * f};
    }

    int elementoIn(int x, int y) const {
        for (int i = 0; i < (int)el.size(); i++) {
            const auto r = attuale(el[i], k);
            if (x >= r.left - 8 && x < r.right + 8 && y >= r.top - 8 && y < r.bottom + 8) return i;
        }
        return -1;
    }

    void aggiornaMiniature() {
        for (const auto& e : el) {
            if (!e.th) continue;
            const D2D1_RECT_F r = attuale(e, k);
            // la miniatura e' la finestra intera (bordi invisibili compresi): si allarga in proporzione
            const float sx = (r.right - r.left) / std::max(1L, e.da.right - e.da.left), sy = (r.bottom - r.top) / std::max(1L, e.da.bottom - e.da.top);
            DWM_THUMBNAIL_PROPERTIES p{};
            p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            p.rcDestination = {(LONG)std::lround(r.left - (e.da.left - e.fin.left) * sx), (LONG)std::lround(r.top - (e.da.top - e.fin.top) * sy),
                               (LONG)std::lround(r.right + (e.fin.right - e.da.right) * sx), (LONG)std::lround(r.bottom + (e.fin.bottom - e.da.bottom) * sy)};
            p.fVisible = TRUE; p.opacity = 255; p.fSourceClientAreaOnly = FALSE;
            DwmUpdateThumbnailProperties(e.th, &p);
        }
    }

    void disegna() {
        if (!v.inizia(0)) return;
        const float d = v.dpi;
        // velo scuro sopra lo sfondo sfocato, che si scurisce mentre le finestre si dispongono
        v.pen->SetColor(D2D1::ColorF(0, 0, 0, (float)(0.30 * vetro::uscita(k))));
        v.rt->FillRectangle({0, 0, (float)v.W, (float)v.H}, v.pen);
        if (sopra >= 0 && sopra < (int)el.size() && k > 0.5) {
            const auto r = attuale(el[sopra], k);
            const float rr = std::max(8.0f, raggio::vetro() * (float)((r.right - r.left) / std::max(1L, el[sopra].da.right - el[sopra].da.left))) + 4 * d;
            v.pen->SetColor(D2D1::ColorF(0.25f, 0.6f, 1.0f, 0.95f));
            v.rt->DrawRoundedRectangle({{r.left - 5 * d, r.top - 5 * d, r.right + 5 * d, r.bottom + 5 * d}, rr, rr}, v.pen, 3 * d);
        }
        // titoli sotto le miniature
        if (k > 0.85) {
            const float a = (float)((k - 0.85) / 0.15);
            for (int i = 0; i < (int)el.size(); i++) {
                const auto r = attuale(el[i], k);
                const std::wstring& t = el[i].titolo;
                if (t.empty()) continue;
                const float lw = std::min(v.larghezza(t, fTitolo) + 24 * d, std::max(120 * d, r.right - r.left));
                const float cx = (r.left + r.right) / 2, y = r.bottom + 12 * d;
                const D2D1_RECT_F p = {cx - lw / 2, y, cx + lw / 2, y + 26 * d};
                v.pen->SetColor(D2D1::ColorF(0, 0, 0, (i == sopra ? 0.55f : 0.35f) * a));
                v.rt->FillRoundedRectangle({p, 13 * d, 13 * d}, v.pen);
                v.testo(t, fTitolo, {p.left + 10 * d, p.top, p.right - 10 * d, p.bottom}, D2D1::ColorF(1, 1, 1, 0.95f * a));
            }
        }
        v.finisci();
    }

    void ciclo() {
        LARGE_INTEGER f, c0; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c0);
        auto ora = [&]() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (c.QuadPart - c0.QuadPart) / (double)f.QuadPart; };
        double tC = -1;
        MSG msg;
        while (true) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); return; }
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            const double t = ora();
            if (chiudendo && tC < 0) tC = t;
            double nuovo;
            if (tC < 0) nuovo = vetro::uscita(t / 0.30);                       // apertura: verso la griglia
            else nuovo = std::min(k, 1.0) * (1 - vetro::uscita((t - tC) / 0.26));   // chiusura: tutte tornano al loro posto
            if (tC >= 0 && t - tC >= 0.26) { k = 0; aggiornaMiniature(); DwmFlush(); return; }
            const bool cambia = std::abs(nuovo - k) > 1e-4;
            k = nuovo;
            if (cambia) { aggiornaMiniature(); disegna(); }
            // le animazioni di FluidWin (il ciclo principale e' fermo finche' la panoramica e' aperta): il loro
            // fotogramma aspetta gia' il vblank, al posto di DwmFlush
            if (menu::animazioni()) menu::passoAnimazioni();
            else if (cambia) DwmFlush();
            else MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        }
    }
};

}  // namespace panoramica
