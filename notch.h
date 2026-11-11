// Notch di FluidWin: ora e giorno in alto al centro dello schermo del dock.
// Nero come il notch del Mac e dell'iPhone, attaccato al bordo in alto: ai lati si raccorda al bordo con due curve
// all'indietro, sotto ha gli angoli tondi. Su una riga l'ora (piu' grande) e il giorno. Il mouse lo attraversa (sta
// dove si afferrano le finestre ingrandite); un clic apre le notifiche (vedi 'clic').
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <functional>
#include <algorithm>
#include <cmath>
#include "fabbriche.h"

namespace notch {

class Notch {
public:
    std::function<void()> clic;   // clic sul notch

    bool crea(HINSTANCE inst) {
        WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"FluidWin.Notch";
        wc.hCursor = LoadCursor(nullptr, IDC_HAND);
        wc.cbWndExtra = sizeof(void*);
        RegisterClassW(&wc);
        // trasparente al mouse: sta dove si afferrano le finestre ingrandite per trascinarle, non deve rubare quel clic
        fin = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"",
                              WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
        if (!fin) return false;
        SetWindowLongPtrW(fin, 0, (LONG_PTR)this);
        BOOL niente = TRUE; DwmSetWindowAttribute(fin, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        return true;
    }

    ~Notch() {
        for (IDWriteTextFormat* f : {fOra, fGiorno}) if (f) f->Release();
    }

    // font scelti dal dock (Segoe UI Variable o quelli di riserva)
    void font(const wchar_t* testo, const wchar_t* titoli) {
        if (fontTesto == testo && fontTitoli == titoli) return;
        fontTesto = testo; fontTitoli = titoli; fDpi = 0;
    }

    // testi da mostrare: ridisegna solo se cambiano
    void testi(const std::wstring& ora, const std::wstring& giorno) {
        if (ora == testoOra && giorno == testoGiorno) return;
        testoOra = ora; testoGiorno = giorno;
        disegna();
    }

    // schermo su cui sta (in alto al centro) e scala del dock
    void posto(const RECT& s, float d) { schermo = s; dpi = d; disegna(); }

    // visibile o no (nascosto quando il dock e' spento o c'e' un'app a schermo intero)
    void mostra(bool si) {
        visibile = si;
        if (fin) ShowWindow(fin, si ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (si) disegna();
    }

    // ridisegna (a ogni minuto, cambio di schermo o di tema)
    void disegna() {
        if (!fin || testoOra.empty()) return;
        ID2D1Factory* f2d = fabbriche::d2d(); IDWriteFactory* fdw = fabbriche::testo();
        if (!f2d || !fdw) return;
        if (fDpi != dpi) {   // formati alla scala del dock
            for (IDWriteTextFormat* f : {fOra, fGiorno}) if (f) f->Release();
            fOra = fGiorno = nullptr;
            fdw->CreateTextFormat(fontTitoli, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f * dpi, L"it-it", &fOra);
            fdw->CreateTextFormat(fontTesto, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.5f * dpi, L"it-it", &fGiorno);
            for (IDWriteTextFormat* f : {fOra, fGiorno}) if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
            fDpi = dpi;
        }
        if (!fOra || !fGiorno) return;
        // piccolo come quello del Mac: una riga sola, ora (piu' grande) e giorno uno accanto all'altro, un po' piu' largo che alto
        const float lo = larghezza(testoOra, fOra), lg = larghezza(testoGiorno, fGiorno), spazio = std::round(7 * dpi);
        const float f = std::round(6 * dpi);                       // raccordo con il bordo in alto
        const float w = std::round(std::max(200 * dpi, lo + spazio + lg + 48 * dpi)), h = std::round(22 * dpi), R = std::round(9 * dpi);
        const int W2 = (int)(w + 2 * f), H2 = (int)h;
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W2, -H2, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
        HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) { if (dib) DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, scr); return; }
        HGDIOBJ vecchio = SelectObject(mem, dib);
        ID2D1DCRenderTarget* rt = nullptr;
        const D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (SUCCEEDED(f2d->CreateDCRenderTarget(&p, &rt))) {
            RECT rr = {0, 0, W2, H2};
            rt->BindDC(mem, &rr);
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
            if (SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
                const float x0 = f, x1 = f + w;
                s->BeginFigure({0, 0}, D2D1_FIGURE_BEGIN_FILLED);
                s->AddArc({{x0, f}, {f, f}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});        // raccordo a sinistra
                s->AddLine({x0, h - R});
                s->AddArc({{x0 + R, h}, {R, R}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
                s->AddLine({x1 - R, h});
                s->AddArc({{x1, h - R}, {R, R}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
                s->AddLine({x1, f});
                s->AddArc({{x1 + f, 0}, {f, f}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});   // raccordo a destra
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close(); s->Release();
                ID2D1SolidColorBrush* b = nullptr; rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b);
                if (b) {
                    rt->FillGeometry(g, b);
                    b->SetColor(D2D1::ColorF(1, 1, 1, 1));
                    const float xt = x0 + (w - (lo + spazio + lg)) / 2;
                    // Ora e giorno occupano la stessa altezza: l'ora va dalla cima delle cifre alla linea di base; il
                    // giorno, piu' piccolo, sale e sotto ha una linea sottile alla stessa linea di base dell'ora
                    auto lineaBase = [&](IDWriteTextFormat* fmt, const std::wstring& t, float& alto) {
                        IDWriteTextLayout* l = nullptr; float base = 0; alto = 0;
                        if (SUCCEEDED(fdw->CreateTextLayout(t.c_str(), (UINT32)t.size(), fmt, 1000, 100, &l))) {
                            DWRITE_LINE_METRICS lm{}; UINT32 n = 0; l->GetLineMetrics(&lm, 1, &n);
                            base = lm.baseline; alto = lm.height; l->Release();
                        }
                        return base;
                    };
                    float altoOra = 0, altoGiorno = 0;
                    const float bOra = lineaBase(fOra, testoOra, altoOra), bGiorno = lineaBase(fGiorno, testoGiorno, altoGiorno);
                    const float yOra = std::round((h - altoOra) / 2), baseOra = yOra + bOra;     // linea di base dell'ora
                    const float yGiorno = baseOra - 3 * dpi - bGiorno;                            // il giorno 3 px piu' su
                    rt->DrawText(testoOra.c_str(), (UINT32)testoOra.size(), fOra, {xt, yOra, xt + lo + 2, yOra + altoOra}, b);
                    b->SetColor(D2D1::ColorF(1, 1, 1, 0.66f));
                    const float xg = xt + lo + spazio;
                    rt->DrawText(testoGiorno.c_str(), (UINT32)testoGiorno.size(), fGiorno, {xg, yGiorno, xg + lg + 2, yGiorno + altoGiorno}, b);
                    b->SetColor(D2D1::ColorF(1, 1, 1, 0.38f));
                    rt->DrawLine({xg, baseOra + 0.5f}, {xg + lg, baseOra + 0.5f}, b, 1.0f);
                    b->Release();
                }
            }
            if (g) g->Release();
            rt->EndDraw(); rt->Release();
        }
        GdiFlush();
        POINT pos = {schermo.left + (schermo.right - schermo.left - W2) / 2, schermo.top}, zero = {0, 0}; SIZE sz = {W2, H2};
        BLENDFUNCTION bl = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(fin, scr, &pos, &sz, mem, &zero, 0, &bl, ULW_ALPHA);
        SelectObject(mem, vecchio); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, scr);
        if (visibile) SetWindowPos(fin, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

private:
    HWND fin = nullptr;
    bool visibile = false;
    RECT schermo{};
    float dpi = 1, fDpi = 0;
    std::wstring testoOra, testoGiorno;
    const wchar_t *fontTesto = L"Segoe UI Variable Text", *fontTitoli = L"Segoe UI Variable Display";
    IDWriteTextFormat *fOra = nullptr, *fGiorno = nullptr;

    static float larghezza(const std::wstring& s, IDWriteTextFormat* f) {
        IDWriteTextLayout* l = nullptr; float w = 0;
        if (SUCCEEDED(fabbriche::testo()->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 1000, 100, &l))) {
            DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; l->Release();
        }
        return w;
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Notch* n = (Notch*)GetWindowLongPtrW(h, 0);
        switch (m) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_LBUTTONUP: if (n && n->clic) n->clic(); return 0;
            case WM_CLOSE: return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

}  // namespace notch
