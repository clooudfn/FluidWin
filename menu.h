// Menu in stile FluidWin al posto di quelli di Windows (TrackPopupMenu): vetro acrilico, velo chiaro o scuro come i
// pannelli, testo in Inter, voce sotto il mouse evidenziata in blu con angoli arrotondati, sottomenu, tastiera.
// Si usa come TrackPopupMenu con TPM_RETURNCMD: mostra() resta in attesa e restituisce l'id scelto (0 = annullato).
// Le voci possono arrivare da un HMENU qualsiasi, anche quello della shell (menu col tasto destro delle app).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include "raggio.h"
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>

namespace menu {

// Mentre un menu (o la panoramica) e' aperto il ciclo principale di FluidWin e' fermo: le animazioni (genio,
// gelatina, dock) le fa avanzare il ciclo del menu, con queste due funzioni di FluidWin. Il passo aspetta lui il vblank
inline bool (*animazioniInCorso)() = nullptr;
inline void (*passoAnimazioni)() = nullptr;
inline bool animazioni() { return animazioniInCorso && passoAnimazioni && animazioniInCorso(); }

struct Voce {
    std::wstring testo;
    UINT id = 0;
    bool separatore = false, disattivata = false, titolo = false, spuntata = false;
    HMENU sotto = nullptr;   // sottomenu (preso da un HMENU)
};

static Voce voce(const std::wstring& t, UINT id) { Voce v; v.testo = t; v.id = id; return v; }
static Voce separatore() { Voce v; v.separatore = true; return v; }
static Voce titolo(const std::wstring& t) { Voce v; v.testo = t; v.titolo = true; return v; }

// Voci di un HMENU (anche della shell). Le voci senza testo (disegnate dall'app) si saltano, cosi' come i
// separatori doppi o in fondo.
static std::vector<Voce> daHmenu(HMENU m) {
    std::vector<Voce> v;
    const int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        wchar_t buf[256] = L"";
        MENUITEMINFOW mi{}; mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = buf; mi.cch = 255;
        if (!GetMenuItemInfoW(m, i, TRUE, &mi)) continue;
        Voce x;
        if (mi.fType & MFT_SEPARATOR) {
            if (v.empty() || v.back().separatore) continue;
            x.separatore = true; v.push_back(x); continue;
        }
        if (!buf[0]) continue;
        // "&Apri" -> "Apri"; il testo dopo il tab (scorciatoie) si toglie
        std::wstring t;
        for (const wchar_t* p = buf; *p && *p != L'\t'; p++) { if (*p == L'&' && p[1] != L'&') continue; t += *p; }
        x.testo = t; x.id = mi.wID; x.sotto = mi.hSubMenu;
        x.disattivata = (mi.fState & MFS_DISABLED) != 0;
        x.spuntata = (mi.fState & MFS_CHECKED) != 0;
        v.push_back(x);
    }
    while (!v.empty() && v.back().separatore) v.pop_back();
    return v;
}

class Menu {
public:
    // Mostra il menu con l'angolo in alto a sinistra in pt (o sopra pt, se versoAlto); restituisce l'id scelto.
    // prepara(sottomenu) viene chiamata prima di aprire un sottomenu preso da un HMENU (la shell li riempie li').
    static UINT mostra(const std::vector<Voce>& voci, POINT pt, HWND proprietario, bool versoAlto = false,
                       std::function<void(HMENU)> prepara = nullptr) {
        Menu m(voci, proprietario, prepara);
        return m.esegui(pt, versoAlto, nullptr);
    }

private:
    std::vector<Voce> voci;
    std::vector<D2D1_RECT_F> rett;
    HWND fin = nullptr, proprietario = nullptr;
    std::function<void(HMENU)> prepara;
    ID2D1Factory* f2d = nullptr; ID2D1HwndRenderTarget* rt = nullptr; ID2D1SolidColorBrush* pen = nullptr;
    IDWriteFactory* fdw = nullptr; IDWriteTextFormat *fmt = nullptr, *fmtTitolo = nullptr;
    int sopra = -1, W = 0, H = 0;
    float dpi = 1;
    bool scuro = false, fine = false;
    UINT scelta = 0;
    BYTE alfa = 0;
    Menu* padre = nullptr;
    int aperto = -1;   // voce con il sottomenu aperto
    bool inSotto = false;

    Menu(const std::vector<Voce>& v, HWND p, std::function<void(HMENU)> pr) : voci(v), proprietario(p), prepara(pr) {}
    ~Menu() {
        for (IUnknown* o : std::initializer_list<IUnknown*>{pen, rt, fmt, fmtTitolo}) if (o) o->Release();
        if (fin) DestroyWindow(fin);
    }

    // Fabbriche di Direct2D e DirectWrite: una sola per tutti i menu (crearle a ogni menu lasciava indietro un paio di
    // handle ogni volta). Non si liberano mai: servono fino all'uscita
    static ID2D1Factory* fabbrica2d() {
        static ID2D1Factory* f = [] { ID2D1Factory* x = nullptr; D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &x); return x; }();
        return f;
    }
    static IDWriteFactory* fabbricaTesto() {
        static IDWriteFactory* f = [] { IDWriteFactory* x = nullptr; DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&x); return x; }();
        return f;
    }

    static bool temaScuro() {
        DWORD v = 1, n = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
        return v == 0;
    }

    static const wchar_t* famiglia(IDWriteFactory* f) {
        IDWriteFontCollection* c = nullptr; f->GetSystemFontCollection(&c, FALSE);
        const wchar_t* scelta = L"Segoe UI Variable Text";
        for (const wchar_t* n : {L"Inter Variable", L"Inter Variable Text", L"Inter"}) {
            UINT32 i = 0; BOOL e = FALSE; if (c) c->FindFamilyName(n, &i, &e);
            if (e) { scelta = n; break; }
        }
        if (c) c->Release();
        return scelta;
    }

    float altezza(const Voce& v) const { return (v.separatore ? 9 : v.titolo ? 26 : 30) * dpi; }

    void crea(POINT pt, bool versoAlto) {
        static bool registrata = false;
        if (!registrata) {
            WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FluidWin.Menu";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);   // l'ombra (arrotondata) la mette gia' Windows 11
            RegisterClassW(&wc); registrata = true;
        }
        const HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        UINT dx = 96, dy = 96;
        if (HMODULE sh = GetModuleHandleW(L"shcore.dll") ? GetModuleHandleW(L"shcore.dll") : LoadLibraryW(L"shcore.dll")) {
            typedef HRESULT (WINAPI *Fn)(HMONITOR, int, UINT*, UINT*);
            if (auto f = (Fn)GetProcAddress(sh, "GetDpiForMonitor")) if (SUCCEEDED(f(mon, 0, &dx, &dy))) dpi = dx / 96.0f;
        }
        scuro = temaScuro();
        fdw = fabbricaTesto();
        if (!fdw) return;
        static const wchar_t* fam = famiglia(fdw);   // (il font scelto non cambia: si cerca una volta sola)
        fdw->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f * dpi, L"it-it", &fmt);
        fdw->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * dpi, L"it-it", &fmtTitolo);
        for (IDWriteTextFormat* f : {fmt, fmtTitolo}) {
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        // misure: larghezza della voce piu' lunga
        float largo = 180 * dpi, alto = 12 * dpi;
        for (const auto& v : voci) {
            alto += altezza(v);
            if (v.separatore) continue;
            IDWriteTextLayout* l = nullptr;
            if (SUCCEEDED(fdw->CreateTextLayout(v.testo.c_str(), (UINT32)v.testo.size(), v.titolo ? fmtTitolo : fmt, 2000, 100, &l))) {
                DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm); l->Release();
                largo = std::max(largo, tm.widthIncludingTrailingWhitespace + 64 * dpi);
            }
        }
        W = (int)std::ceil(std::min(largo, 420 * dpi)); H = (int)std::ceil(alto);
        // dentro lo schermo
        MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
        int x = pt.x, y = versoAlto ? pt.y - H : pt.y;
        if (x + W > mi.rcWork.right) x = std::max((int)mi.rcWork.left, (padre ? (int)padre->sinistraFin() - W : (int)mi.rcWork.right - W));
        if (y + H > mi.rcWork.bottom) y = mi.rcWork.bottom - H;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
        fin = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"FluidWin.Menu", L"", WS_POPUP,
                              x, y, W, H, proprietario, nullptr, GetModuleHandleW(nullptr), this);
        SetLayeredWindowAttributes(fin, 0, 0, LWA_ALPHA);
        // vetro acrilico di Windows 11 sotto al disegno, angoli arrotondati, niente animazioni native
        MARGINS mg{-1, -1, -1, -1}; DwmExtendFrameIntoClientArea(fin, &mg);
        BOOL sc = scuro; DwmSetWindowAttribute(fin, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &sc, sizeof(sc));
        int tipo = 3; DwmSetWindowAttribute(fin, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &tipo, sizeof(tipo));
        int angoli = 2; DwmSetWindowAttribute(fin, DWMWA_WINDOW_CORNER_PREFERENCE, &angoli, sizeof(angoli));
        BOOL niente = TRUE; DwmSetWindowAttribute(fin, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        f2d = fabbrica2d();
        if (!f2d || !fdw) return;
        f2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                                    D2D1::HwndRenderTargetProperties(fin, D2D1::SizeU(W, H)), &rt);
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // sul vetro il ClearType farebbe aloni colorati
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &pen);
        // rettangoli delle voci
        float yy = 6 * dpi;
        for (const auto& v : voci) { const float a = altezza(v); rett.push_back({6 * dpi, yy, W - 6 * dpi, yy + a}); yy += a; }
    }

    float sinistraFin() const { RECT r; GetWindowRect(fin, &r); return (float)r.left; }

    bool selezionabile(int i) const { return i >= 0 && i < (int)voci.size() && !voci[i].separatore && !voci[i].titolo && !voci[i].disattivata; }

    int voceIn(int x, int y) const {
        for (int i = 0; i < (int)rett.size(); i++)
            if (x >= rett[i].left && x < rett[i].right && y >= rett[i].top && y < rett[i].bottom) return selezionabile(i) ? i : -1;
        return -1;
    }

    void disegna() {
        if (!rt) return;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        // velo sopra il vetro, come i pannelli del dock
        pen->SetColor(scuro ? D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.45f) : D2D1::ColorF(1, 1, 1, 0.55f));
        rt->FillRectangle({0, 0, (float)W, (float)H}, pen);
        const D2D1_COLOR_F ink = scuro ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.9f);
        for (int i = 0; i < (int)voci.size(); i++) {
            const Voce& v = voci[i];
            const D2D1_RECT_F& r = rett[i];
            if (v.separatore) {
                pen->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.10f));
                const float cy = (r.top + r.bottom) / 2;
                rt->DrawLine({r.left + 8 * dpi, cy}, {r.right - 8 * dpi, cy}, pen, 1.0f);
                continue;
            }
            const bool evid = i == sopra || i == aperto;
            if (evid) {
                pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1));
                // concentrica con l'angolo del vetro (6 px piu' dentro), al massimo a pillola
                const float re = std::min(std::max(6.0f, raggio::vetro() - 6.0f) * dpi, (r.bottom - r.top) / 2);
                rt->FillRoundedRectangle({r, re, re}, pen);
            }
            D2D1_COLOR_F c = evid ? D2D1::ColorF(1, 1, 1, 1) : ink;
            if (v.disattivata && !v.titolo) c.a *= 0.4f;
            if (v.titolo) c.a *= 0.55f;
            pen->SetColor(c);
            const D2D1_RECT_F rtxt = {r.left + 24 * dpi, r.top, r.right - 24 * dpi, r.bottom};
            rt->DrawText(v.testo.c_str(), (UINT32)v.testo.size(), v.titolo ? fmtTitolo : fmt, v.titolo ? D2D1_RECT_F{r.left + 10 * dpi, r.top, r.right, r.bottom} : rtxt,
                         pen, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (v.spuntata) {   // segno di spunta a sinistra
                const float cx = r.left + 12 * dpi, cy = (r.top + r.bottom) / 2;
                rt->DrawLine({cx - 4 * dpi, cy}, {cx - 1 * dpi, cy + 3 * dpi}, pen, 1.6f * dpi);
                rt->DrawLine({cx - 1 * dpi, cy + 3 * dpi}, {cx + 4 * dpi, cy - 3 * dpi}, pen, 1.6f * dpi);
            }
            if (v.sotto) {   // freccia del sottomenu
                const float cx = r.right - 12 * dpi, cy = (r.top + r.bottom) / 2;
                rt->DrawLine({cx - 2 * dpi, cy - 4 * dpi}, {cx + 2 * dpi, cy}, pen, 1.5f * dpi);
                rt->DrawLine({cx + 2 * dpi, cy}, {cx - 2 * dpi, cy + 4 * dpi}, pen, 1.5f * dpi);
            }
        }
        rt->EndDraw();
        ValidateRect(fin, nullptr);
    }

    void scegli(int i) {
        if (!selezionabile(i)) return;
        if (voci[i].sotto) { apriSotto(i); return; }
        scelta = voci[i].id; fine = true;
    }

    // sottomenu a destra della voce: ha un suo ciclo; se restituisce un id, il menu intero si chiude
    void apriSotto(int i) {
        HMENU hs = voci[i].sotto;
        if (prepara) prepara(hs);
        auto sv = daHmenu(hs);
        if (sv.empty()) return;
        aperto = i; disegna();
        Menu s(sv, fin, prepara);
        s.padre = this;
        RECT r; GetWindowRect(fin, &r);
        inSotto = true;   // il sottomenu prende il mouse: per questo menu non e' un motivo per chiudersi
        const UINT id = s.esegui({r.right - (LONG)(4 * dpi), r.top + (LONG)rett[i].top - (LONG)(6 * dpi)}, false, this);
        inSotto = false;
        aperto = -1;
        if (id) { scelta = id; fine = true; }
        else { SetCapture(fin); disegna(); }
    }

    UINT esegui(POINT pt, bool versoAlto, Menu* p) {
        padre = p;
        crea(pt, versoAlto);
        if (!fin || !rt) return 0;   // (grafica non disponibile: niente menu, come se fosse stato annullato)
        ShowWindow(fin, SW_SHOWNOACTIVATE);
        disegna();
        SetCapture(fin);
        const ULONGLONG t0 = GetTickCount64();
        MSG msg;
        while (!fine) {
            // dissolvenza d'apertura (120 ms)
            if (alfa < 255) {
                alfa = (BYTE)std::min<ULONGLONG>(255, (GetTickCount64() - t0) * 255 / 120);
                SetLayeredWindowAttributes(fin, 0, alfa, LWA_ALPHA);
                if (!animazioni() && MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT) == WAIT_TIMEOUT) continue;
            }
            // animazioni di FluidWin in corso: prima i messaggi in coda, poi un fotogramma (che aspetta il vblank)
            if (animazioni() && !PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) { passoAnimazioni(); continue; }
            if (!GetMessageW(&msg, nullptr, 0, 0)) { PostQuitMessage((int)msg.wParam); break; }
            if (gestisci(msg)) continue;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        ReleaseCapture();
        ShowWindow(fin, SW_HIDE);
        return scelta;
    }

    // messaggi di mouse e tastiera del menu (anche quelli del sottomenu passano di qui finche' e' aperto)
    bool gestisci(const MSG& msg) {
        switch (msg.message) {
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
                POINT p{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
                if (msg.hwnd != fin) MapWindowPoints(msg.hwnd, fin, &p, 1);
                RECT cr; GetClientRect(fin, &cr);
                const bool dentro = PtInRect(&cr, p);
                if (msg.message == WM_MOUSEMOVE) {
                    if (!dentro && padre) {   // mouse tornato sul menu padre: il sottomenu si chiude
                        POINT s = p; MapWindowPoints(fin, padre->fin, &s, 1);
                        RECT pr; GetClientRect(padre->fin, &pr);
                        if (PtInRect(&pr, s) && padre->voceIn(s.x, s.y) != padre->aperto) { fine = true; scelta = 0; }
                    }
                    const int s = dentro ? voceIn(p.x, p.y) : -1;
                    if (s != sopra) {
                        sopra = s; disegna();
                        if (s >= 0 && voci[s].sotto) apriSotto(s);   // i sottomenu si aprono al passaggio
                    }
                    return true;
                }
                if (!dentro) {   // clic fuori: si chiude tutto (il clic non passa sotto, come i menu di Windows)
                    if (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN) {
                        fine = true; scelta = 0;
                        // sul menu padre si chiude solo questo sottomenu
                        if (padre) { POINT s = p; MapWindowPoints(fin, padre->fin, &s, 1); RECT pr; GetClientRect(padre->fin, &pr); if (!PtInRect(&pr, s)) padre->fine = true; }
                    }
                    return true;
                }
                if (msg.message == WM_LBUTTONUP || msg.message == WM_RBUTTONUP) scegli(voceIn(p.x, p.y));
                return true;
            }
            case WM_KEYDOWN: case WM_SYSKEYDOWN: {
                const WPARAM k = msg.wParam;
                if (k == VK_ESCAPE || (k == VK_LEFT && padre)) { fine = true; scelta = 0; return true; }
                if (k == VK_DOWN || k == VK_UP) {
                    int s = sopra;
                    for (int n = 0; n < (int)voci.size(); n++) {
                        s = (s + (k == VK_DOWN ? 1 : -1) + (int)voci.size()) % (int)voci.size();
                        if (selezionabile(s)) break;
                    }
                    sopra = s; disegna(); return true;
                }
                if (k == VK_RETURN || k == VK_SPACE || (k == VK_RIGHT && sopra >= 0 && voci[sopra].sotto)) { scegli(sopra); return true; }
                return true;
            }
            case WM_CHAR: case WM_SYSCHAR: case WM_KEYUP: case WM_SYSKEYUP: return true;
            case WM_CAPTURECHANGED: break;
            case WM_ACTIVATEAPP: if (!msg.wParam) { fine = true; scelta = 0; } break;
        }
        return false;
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        Menu* me = (Menu*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (m) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_PAINT: if (me) me->disegna(); else ValidateRect(h, nullptr); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_NCCALCSIZE: if (w) return 0; break;
            case WM_CANCELMODE: if (me) { me->fine = true; me->scelta = 0; } return 0;
            case WM_CLOSE: if (me) { me->fine = true; me->scelta = 0; } return 0;   // (la finestra la distrugge il menu)
            // il mouse non e' piu' nostro (Alt+Tab, un'altra finestra in primo piano): il menu si chiude
            case WM_CAPTURECHANGED: if (me && !me->inSotto && !me->fine && (HWND)l != h) { me->fine = true; me->scelta = 0; } return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

static UINT mostra(const std::vector<Voce>& voci, POINT pt, HWND proprietario, bool versoAlto = false,
                   std::function<void(HMENU)> prepara = nullptr) {
    return Menu::mostra(voci, pt, proprietario, versoAlto, prepara);
}

} // namespace menu
