// Indicatore del volume al posto di quello di Windows: una pillola di vetro che scende in alto al centro mentre
// si premono i tasti del volume, poi sparisce. I tasti li intercetta il gancio di tastiera di FluidWin (che cambia
// il volume da se'): quello di Windows non compare piu'. Se FluidWin si chiude il gancio sparisce e torna quello
// di Windows.
#pragma once
#include "vetro.h"

namespace osd {

class Indicatore {
public:
    // La finestra (vetro e disegno Direct2D, ~5-9 MB) si crea al primo uso: con il notch acceso il volume lo mostra lui,
    // e questo indicatore serve solo senza notch o sopra un gioco a schermo intero
    bool inizializza() { return true; }

    // livello 0..1; muto. Ricomincia il conto alla rovescia se e' gia' a schermo
    void mostra(float livello, bool muto) {
        if (!v.fin && !v.crea(L"FluidWin.Volume", vetro::procedura<Indicatore>, this, false)) return;
        livelloObj = livello; this->muto = muto;
        const ULONGLONG ora = vetro::ms();
        if (stato == 0) {   // da nascosto: entra
            v.aggiornaTema();
            const RECT a = vetro::areaPrincipale();
            v.dpi = vetro::dpiMonitor(MonitorFromPoint({a.left, a.top}, MONITOR_DEFAULTTOPRIMARY));
            const int w = (int)std::lround(300 * v.dpi), h = (int)std::lround(52 * v.dpi);
            x = (a.left + a.right - w) / 2; y = a.top + (int)std::lround(14 * v.dpi);
            v.posiziona(x, y - (int)std::lround(12 * v.dpi), w, h, HWND_TOPMOST, SWP_SHOWWINDOW);
            if (!fmt) fmt = v.formato(13.5f * v.dpi, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
            livelloVis = livello;
            t0 = ora; stato = 1;
            vetro::battito(v.fin, 1);
        } else if (stato == 3) { stato = 1; t0 = ora - (ULONGLONG)(k * 180); }   // stava uscendo: torna
        tFine = ora + 1400;
        disegna();
    }

    void chiudi() { if (v.fin) vetro::fermaBattito(v.fin, 1); if (fmt) fmt->Release(); fmt = nullptr; v.chiudi(); }

    bool messaggio(HWND, UINT m, WPARAM w, LPARAM, LRESULT& r) {
        if (m == WM_TIMER && w == 1) { passo(); r = 0; return true; }
        if (m == WM_PAINT) { disegna(); r = 0; return true; }
        if (m == WM_MOUSEACTIVATE) { r = MA_NOACTIVATE; return true; }
        return false;
    }

private:
    vetro::Finestra v;
    IDWriteTextFormat* fmt = nullptr;
    int stato = 0;   // 0 nascosto, 1 entra, 2 fermo, 3 esce
    ULONGLONG t0 = 0, tFine = 0;
    double k = 0;
    float livelloObj = 0, livelloVis = 0;
    bool muto = false;
    int x = 0, y = 0;

    void passo() {
        const ULONGLONG ora = vetro::ms();
        if (stato == 1) { k = std::min(1.0, (ora - t0) / 180.0); if (k >= 1) stato = 2; }
        else if (stato == 2 && ora >= tFine) { stato = 3; t0 = ora; }
        else if (stato == 3) {
            k = 1 - std::min(1.0, (ora - t0) / 220.0);
            if (k <= 0) { stato = 0; vetro::fermaBattito(v.fin, 1); ShowWindow(v.fin, SW_HIDE); return; }
        }
        livelloVis += (livelloObj - livelloVis) * 0.35f;   // la barra scorre fino al valore nuovo
        const double e = vetro::uscita(k);
        SetWindowPos(v.fin, HWND_TOPMOST, x, y - (int)std::lround((1 - e) * 12 * v.dpi), 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
        v.alfa(e);
        disegna();
    }

    void disegna() {
        if (!v.inizia()) return;
        const float d = v.dpi, W = (float)v.W, H = (float)v.H, cy = H / 2;
        const D2D1_COLOR_F ink = v.inchiostro();
        // altoparlante
        const float sx = 22 * d;
        v.pen->SetColor(ink);
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(v.f2d->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr; g->Open(&s);
            s->BeginFigure({sx - 6 * d, cy - 3.5f * d}, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F p[] = {{sx - 2 * d, cy - 3.5f * d}, {sx + 3 * d, cy - 8 * d}, {sx + 3 * d, cy + 8 * d}, {sx - 2 * d, cy + 3.5f * d}, {sx - 6 * d, cy + 3.5f * d}};
            s->AddLines(p, 5); s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
            v.rt->FillGeometry(g, v.pen); g->Release();
        }
        const float liv = muto ? 0.0f : livelloVis;
        if (muto) {
            v.rt->DrawLine({sx + 7 * d, cy - 4 * d}, {sx + 15 * d, cy + 4 * d}, v.pen, 1.8f * d);
            v.rt->DrawLine({sx + 15 * d, cy - 4 * d}, {sx + 7 * d, cy + 4 * d}, v.pen, 1.8f * d);
        } else {
            for (int i = 0; i < 2; i++) {
                if (liv < (i == 0 ? 0.01f : 0.5f)) break;
                const float r = (i == 0 ? 6.5f : 11.0f) * d;
                ID2D1PathGeometry* a = nullptr;
                if (FAILED(v.f2d->CreatePathGeometry(&a))) break;
                ID2D1GeometrySink* s = nullptr; a->Open(&s);
                s->BeginFigure({sx + 3 * d + r * 0.6f, cy - r * 0.8f}, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddArc(D2D1::ArcSegment({sx + 3 * d + r * 0.6f, cy + r * 0.8f}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
                v.rt->DrawGeometry(a, v.pen, 1.8f * d); a->Release();
            }
        }
        // barra
        const D2D1_RECT_F b = {56 * d, cy - 4 * d, W - 64 * d, cy + 4 * d};
        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.12f));
        v.rt->FillRoundedRectangle({b, 4 * d, 4 * d}, v.pen);
        if (liv > 0.005f) {
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0.1f, 0.1f, 0.12f, 0.85f));
            v.rt->FillRoundedRectangle({{b.left, b.top, b.left + std::max(8 * d, (b.right - b.left) * liv), b.bottom}, 4 * d, 4 * d}, v.pen);
        }
        wchar_t t[16]; swprintf(t, 16, muto ? testi::t(L"Muto", L"Muted") : L"%d", (int)std::lround(livelloObj * 100));
        v.testo(t, fmt, {W - 60 * d, 0, W - 18 * d, H}, ink);
        v.finisci();
    }
};

}  // namespace osd
