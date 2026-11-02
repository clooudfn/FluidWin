// Base comune per le finestre "di vetro" di FluidWin fuori dal dock (indicatore del volume, banner delle notifiche,
// panoramica, ricerca, impostazioni): vetro acrilico di Windows 11 con gli angoli di DWM, disegno Direct2D sopra e
// dissolvenza con l'alfa della finestra. E' lo stesso schema dei menu (menu.h).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <cmath>
#include <algorithm>
#include "raggio.h"

namespace vetro {

inline bool temaScuro() {
    DWORD v = 1, n = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
    return v == 0;
}

// Inter se c'e' (il font di sistema scelto per FluidWin), altrimenti quello di Windows 11
inline const wchar_t* famiglia(IDWriteFactory* f) {
    IDWriteFontCollection* c = nullptr; f->GetSystemFontCollection(&c, FALSE);
    const wchar_t* scelta = L"Segoe UI Variable Text";
    for (const wchar_t* n : {L"Inter Variable", L"Inter Variable Text", L"Inter"}) {
        UINT32 i = 0; BOOL e = FALSE; if (c) c->FindFamilyName(n, &i, &e);
        if (e) { scelta = n; break; }
    }
    if (c) c->Release();
    return scelta;
}

inline float dpiMonitor(HMONITOR mon) {
    UINT dx = 96, dy = 96;
    if (HMODULE sh = GetModuleHandleW(L"shcore.dll") ? GetModuleHandleW(L"shcore.dll") : LoadLibraryW(L"shcore.dll")) {
        typedef HRESULT (WINAPI *Fn)(HMONITOR, int, UINT*, UINT*);
        if (auto f = (Fn)GetProcAddress(sh, "GetDpiForMonitor")) if (SUCCEEDED(f(mon, 0, &dx, &dy))) return dx / 96.0f;
    }
    return 1.0f;
}

inline RECT areaPrincipale(bool lavoro = false) {
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return lavoro ? mi.rcWork : mi.rcMonitor;
}

class Finestra {
public:
    HWND fin = nullptr;
    ID2D1Factory* f2d = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* pen = nullptr;
    IDWriteFactory* fdw = nullptr;
    const wchar_t* fam = L"Segoe UI Variable Text";
    int W = 0, H = 0;
    float dpi = 1;
    bool scuro = false;

    // angoli: preferenza di DWM (1 = squadrati, 2 = arrotondati, 3 = piccoli); attivabile: prende la tastiera
    // inCima: sempre sopra le altre finestre (indicatori, pannelli); senza, e' una finestra normale (impostazioni)
    bool crea(const wchar_t* classe, WNDPROC proc, void* io, bool attivabile, int angoli = 2, bool acrilico = true, bool inCima = true) {
        WNDCLASSW wc{};
        if (!GetClassInfoW(GetModuleHandleW(nullptr), classe, &wc)) {
            wc = {}; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = classe;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
        }
        DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | (inCima ? WS_EX_TOPMOST : 0);
        if (!attivabile) ex |= WS_EX_NOACTIVATE;
        fin = CreateWindowExW(ex, classe, L"FluidWin", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), io);
        if (!fin) return false;
        SetLayeredWindowAttributes(fin, 0, 0, LWA_ALPHA);
        scuro = temaScuro();
        MARGINS mg{-1, -1, -1, -1}; DwmExtendFrameIntoClientArea(fin, &mg);
        BOOL sc = scuro; DwmSetWindowAttribute(fin, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &sc, sizeof(sc));
        int tipo = acrilico ? 3 : 1; DwmSetWindowAttribute(fin, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &tipo, sizeof(tipo));
        DwmSetWindowAttribute(fin, DWMWA_WINDOW_CORNER_PREFERENCE, &angoli, sizeof(angoli));
        BOOL niente = TRUE; DwmSetWindowAttribute(fin, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f2d))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&fdw))) return false;
        fam = famiglia(fdw);
        // superficie subito (1x1): una finestra creata e mai mostrata, senza superficie, teneva il thread
        // occupato al 100% e bloccava i timer di FluidWin finche' non compariva la prima volta
        W = H = 1; creaRt();
        return rt != nullptr;
    }

    // tema chiaro/scuro riletto a ogni apertura (si puo' cambiare dal centro di controllo)
    void aggiornaTema() {
        scuro = temaScuro();
        BOOL sc = scuro; DwmSetWindowAttribute(fin, 20, &sc, sizeof(sc));
    }

    void posiziona(int x, int y, int w, int h, HWND dopo = HWND_TOPMOST, UINT extra = 0) {
        SetWindowPos(fin, dopo, x, y, w, h, SWP_NOACTIVATE | extra);
        const HMONITOR mon = MonitorFromWindow(fin, MONITOR_DEFAULTTONEAREST);
        dpi = dpiMonitor(mon);
        if (w != W || h != H || !rt) {
            W = w; H = h;
            if (rt) rt->Resize(D2D1::SizeU(W, H));
            else creaRt();
        }
    }

    void alfa(double k) { SetLayeredWindowAttributes(fin, 0, (BYTE)std::lround(std::clamp(k, 0.0, 1.0) * 255), LWA_ALPHA); }

    IDWriteTextFormat* formato(float pt, DWRITE_FONT_WEIGHT peso = DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, bool aCapo = false) {
        IDWriteTextFormat* f = nullptr;
        fdw->CreateTextFormat(fam, nullptr, peso, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, pt, L"it-it", &f);
        if (!f) return nullptr;
        f->SetTextAlignment(al);
        f->SetParagraphAlignment(aCapo ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(aCapo ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!aCapo) {
            IDWriteInlineObject* puntini = nullptr; fdw->CreateEllipsisTrimmingSign(f, &puntini);
            DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            f->SetTrimming(&tr, puntini);
            if (puntini) puntini->Release();
        }
        return f;
    }

    float larghezza(const std::wstring& s, IDWriteTextFormat* f) {
        IDWriteTextLayout* l = nullptr; float w = 0;
        if (SUCCEEDED(fdw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 200, &l))) {
            DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm); w = tm.widthIncludingTrailingWhitespace; l->Release();
        }
        return w;
    }

    void testo(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c) {
        if (!f || s.empty()) return;
        pen->SetColor(c);
        rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, pen, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    D2D1_COLOR_F inchiostro(float a = 1) const {
        return scuro ? D2D1::ColorF(1, 1, 1, 0.92f * a) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.9f * a);
    }

    // inizio del disegno: trasparente, con il velo chiaro o scuro sopra il vetro (come pannelli e menu)
    bool inizia(float velo = 1) {
        if (!rt) return false;
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (velo > 0) {
            pen->SetColor(scuro ? D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.45f * velo) : D2D1::ColorF(1, 1, 1, 0.55f * velo));
            rt->FillRectangle({0, 0, (float)W, (float)H}, pen);
        }
        return true;
    }

    void finisci() {
        if (rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) { pen->Release(); pen = nullptr; rt->Release(); rt = nullptr; creaRt(); }
        ValidateRect(fin, nullptr);
    }

    // raggio degli angoli del vetro in px (quello di DWM, che la mod di Windhawk puo' cambiare)
    float raggio() const { return raggio::vetro() * dpi; }

    void chiudi() {
        if (pen) pen->Release();
        if (rt) rt->Release();
        if (fdw) fdw->Release();
        if (f2d) f2d->Release();
        pen = nullptr; rt = nullptr; fdw = nullptr; f2d = nullptr;
        if (fin) DestroyWindow(fin);
        fin = nullptr;
    }

private:
    void creaRt() {
        f2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                                    D2D1::HwndRenderTargetProperties(fin, D2D1::SizeU(std::max(1, W), std::max(1, H))), &rt);
        if (rt) {
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // sul vetro il ClearType farebbe aloni colorati
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &pen);
        }
    }
};

// procedura comune: WM_NCCALCSIZE (niente bordi), WM_ERASEBKGND; il resto lo gestisce l'oggetto
template <class T> LRESULT CALLBACK procedura(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    T* io = (T*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCCALCSIZE: if (w) return 0; break;
        case WM_ERASEBKGND: return 1;
        // Alt+F4 (o un programma che chiede di chiudere): la finestra si chiude come col suo Esc, ma non si distrugge
        // mai (FluidWin la riusa: distrutta, non si sarebbe piu' riaperta fino al riavvio)
        case WM_CLOSE: { LRESULT r = 0; if (!(io && io->messaggio(h, m, w, l, r))) ShowWindow(h, SW_HIDE); return 0; }
        case WM_PAINT: {
            // la zona da ridisegnare si svuota sempre, anche se l'oggetto non ha ancora disegnato (finestra mai
            // mostrata, senza superficie): altrimenti WM_PAINT tornerebbe all'infinito e, avendo la precedenza,
            // bloccherebbe i WM_TIMER di tutto il thread (le animazioni di FluidWin)
            // da nascosta non si disegna: il disegno stesso (Direct2D su una finestra nascosta) la rinvalidava
            LRESULT r = 0;
            if (io && IsWindowVisible(h)) io->messaggio(h, m, w, l, r);
            PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps);   // (ValidateRect da solo non bastava)
            return 0;
        }
    }
    if (io) { LRESULT r = 0; if (io->messaggio(h, m, w, l, r)) return r; }
    return DefWindowProcW(h, m, w, l);
}

// curve per le animazioni
inline double uscita(double x) { x = std::clamp(x, 0.0, 1.0); return 1 - std::pow(1 - x, 3); }
inline double molla(double x) {   // arriva con un piccolo rimbalzo
    x = std::clamp(x, 0.0, 1.0);
    return 1 - std::exp(-6.5 * x) * std::cos(7.5 * x);
}

}  // namespace vetro
