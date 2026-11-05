// Suggerimento al passaggio del mouse (tooltip) nello stile di FluidWin: una piccola pillola di vetro con una parola,
// sotto il punto indicato. Non prende mai il fuoco ne' i clic.
#pragma once
#include "vetro.h"

namespace suggerimento {

class Suggerimento {
public:
    // mostra il testo centrato sotto (x, y) (coordinate schermo); dpi = scala dello schermo li'
    void mostra(const std::wstring& t, int x, int y, float dpi) {
        if (!v.fin) {
            if (!v.crea(L"FluidWin.Suggerimento", vetro::procedura<Suggerimento>, this, false, 2, true, true)) return;
            const LONG_PTR ex = GetWindowLongPtrW(v.fin, GWL_EXSTYLE);
            SetWindowLongPtrW(v.fin, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);   // i clic passano sotto
        }
        if (aperto && t == testo) return;
        testo = t; v.dpi = dpi; v.aggiornaTema();
        if (fmt) fmt->Release();
        fmt = v.formato(12 * dpi, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
        const int w = (int)std::ceil(v.larghezza(testo, fmt) + 22 * dpi), h = (int)std::lround(26 * dpi);
        v.posiziona(x - w / 2, y, w, h, HWND_TOPMOST, SWP_SHOWWINDOW);
        aperto = true; t0 = vetro::ms();
        v.alfa(0);
        vetro::battito(v.fin, 1);
        disegna();
    }
    void nascondi() {
        if (!aperto) return;
        aperto = false;
        vetro::fermaBattito(v.fin, 1);
        ShowWindow(v.fin, SW_HIDE);
    }
    bool aperto = false;

    bool messaggio(HWND, UINT m, WPARAM w, LPARAM, LRESULT& r) {
        r = 0;
        if (m == WM_TIMER && w == 1) {   // comparsa in 120 ms
            const double k = std::min(1.0, (vetro::ms() - t0) / 120.0);
            v.alfa(vetro::uscita(k));
            if (k >= 1) vetro::fermaBattito(v.fin, 1);
            return true;
        }
        if (m == WM_PAINT) { disegna(); return true; }
        if (m == WM_NCHITTEST) { r = HTTRANSPARENT; return true; }
        return false;
    }

private:
    vetro::Finestra v;
    IDWriteTextFormat* fmt = nullptr;
    std::wstring testo;
    ULONGLONG t0 = 0;

    void disegna() {
        if (!aperto || !v.inizia()) return;
        v.testo(testo, fmt, {0, 0, (float)v.W, (float)v.H}, v.inchiostro());
        v.finisci();
    }
};

}  // namespace suggerimento
