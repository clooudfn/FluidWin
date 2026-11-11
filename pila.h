// Pila di cartella (come le "pile" del Dock del Mac): un clic sull'icona Download del dock apre sopra di lei un
// pannello di vetro con i file piu' recenti della cartella, in griglia, con anteprima (o icona) e nome.
// Clic su un file: si apre. Trascinandolo, si porta dove si vuole (Esplora file, desktop, una chat, un'email...):
// e' un trascinamento vero di Windows (IDataObject della shell). In fondo "Apri in Esplora file".
// Si chiude con Esc, con un clic fuori o perdendo il primo piano.
#pragma once
#include "vetro.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>
#include <thread>
#include <mutex>

namespace pila {

// fonte del trascinamento: finisce al rilascio del tasto, si annulla con Esc
class Fonte : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == IID_IUnknown || r == IID_IDropSource) { *p = this; return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD tasti) override {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(tasti & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

class Pila {
public:
    bool aperta() const { return stato != 0; }

    // icona: il rettangolo dell'icona nel dock (schermo); cartella: il percorso
    void apri(const RECT& icona, const std::wstring& cartella, const std::wstring& nome) {
        if (!v.fin && !v.crea(L"FluidWin.Pila", vetro::procedura<Pila>, this, true)) return;
        dir = cartella; titolo = nome;
        v.aggiornaTema();
        v.dpi = vetro::dpiMonitor(MonitorFromRect(&icona, MONITOR_DEFAULTTONEAREST));
        const float d = v.dpi;
        leggi();
        const int righe = std::max(1, (int)std::min<size_t>(3, (file.size() + COLONNE - 1) / COLONNE));
        const int w = (int)std::lround((COLONNE * CELLA + 24) * d), h = (int)std::lround((48 + righe * ALTA + 50) * d);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(MonitorFromRect(&icona, MONITOR_DEFAULTTONEAREST), &mi);
        x = std::clamp((int)((icona.left + icona.right) / 2 - w / 2), (int)mi.rcWork.left + 8, (int)mi.rcWork.right - w - 8);
        y = icona.top - (int)std::lround(14 * d) - h;
        if (y < mi.rcMonitor.top + 8) y = mi.rcMonitor.top + 8;
        v.posiziona(x, y + (int)std::lround(14 * d), w, h, HWND_TOPMOST, SWP_SHOWWINDOW);
        if (!fTitolo) { fTitolo = v.formato(15 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD); fNome = v.formato(11.5f * d, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER); fLink = v.formato(12.5f * d, DWRITE_FONT_WEIGHT_MEDIUM); }
        caricaAnteprime();
        sopra = premuto = -1;
        stato = 1; t0 = vetro::ms(); k = 0;
        SetForegroundWindow(v.fin);
        vetro::battito(v.fin, 1);
        disegna();
    }

    void chiudi() { if (stato == 1 || stato == 2) { stato = 3; t0 = vetro::ms(); vetro::battito(v.fin, 1); } }

    bool messaggio(HWND, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
        r = 0;
        switch (m) {
            case WM_TIMER: if (w == 1) passo(); return true;
            case WM_APP + 1: raccogli(); return true;   // anteprime pronte
            case WM_PAINT: disegna(); return true;
            case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && !trascinando) chiudi(); return true;
            case WM_KEYDOWN: if (w == VK_ESCAPE) chiudi(); return true;
            case WM_CLOSE: chiudi(); return true;
            case WM_MOUSEMOVE: {
                const int px = GET_X_LPARAM(l), py = GET_Y_LPARAM(l);
                if (!dentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, v.fin, 0}; TrackMouseEvent(&tm); dentro = true; }
                // trascinamento di un file fuori dal pannello
                if (premuto >= 0 && premuto < (int)file.size() && (w & MK_LBUTTON) && (std::abs(px - px0) > 5 || std::abs(py - py0) > 5)) {
                    trascina(premuto); premuto = -1; return true;
                }
                const int s = cellaIn(px, py);
                if (s != sopra) { sopra = s; disegna(); }
                return true;
            }
            case WM_MOUSELEAVE: dentro = false; sopra = -1; disegna(); return true;
            case WM_LBUTTONDOWN: premuto = cellaIn(GET_X_LPARAM(l), GET_Y_LPARAM(l)); px0 = GET_X_LPARAM(l); py0 = GET_Y_LPARAM(l); SetCapture(v.fin); disegna(); return true;
            case WM_LBUTTONUP: {
                ReleaseCapture();
                const int c = cellaIn(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                if (c >= 0 && c == premuto) {
                    const std::wstring p = c == LINK ? dir : file[c].percorso;
                    chiudi();
                    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
                }
                premuto = -1; disegna();
                return true;
            }
        }
        return false;
    }

private:
    static constexpr int COLONNE = 4, LINK = 1000;
    static constexpr float CELLA = 92, ALTA = 100;
    struct File { std::wstring nome, percorso; ID2D1Bitmap* anteprima = nullptr; };
    vetro::Finestra v;
    std::vector<File> file;
    std::wstring dir, titolo;
    IDWriteTextFormat *fTitolo = nullptr, *fNome = nullptr, *fLink = nullptr;
    int stato = 0;   // 0 chiusa, 1 entra, 2 aperta, 3 esce
    ULONGLONG t0 = 0; double k = 0;
    int x = 0, y = 0, sopra = -1, premuto = -1, px0 = 0, py0 = 0;
    bool dentro = false, trascinando = false;
    Fonte fonte;

    void libera() { for (auto& f : file) if (f.anteprima) f.anteprima->Release(); file.clear(); }

    // i 12 file (o cartelle) cambiati piu' di recente, senza nascosti e di sistema
    void leggi() {
        libera();
        std::vector<std::pair<ULONGLONG, File>> tutti;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                File f; f.nome = fd.cFileName; f.percorso = dir + L"\\" + fd.cFileName;
                const ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                const ULONGLONG c = ((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime;
                tutti.push_back({std::max(t, c), std::move(f)});   // (un file scaricato puo' avere la data di modifica vecchia)
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(tutti.begin(), tutti.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < tutti.size() && i < (size_t)COLONNE * 3; i++) file.push_back(std::move(tutti[i].second));
    }

    // anteprime (foto, PDF, video) o icone, dalla shell: in un thread a parte (le anteprime possono costare centinaia
    // di millisecondi), il pannello compare subito e le immagini arrivano appena pronte (WM_APP + 1)
    struct Immagine { std::wstring percorso; std::vector<UINT> px; int w = 0, h = 0; };
    struct Lavoro { std::mutex mx; std::vector<Immagine> pronte; unsigned giro = 0; };
    std::shared_ptr<Lavoro> lavoro = std::make_shared<Lavoro>();
    unsigned giro = 0;

    void caricaAnteprime() {
        const int lato = (int)std::lround(56 * v.dpi);
        std::vector<std::wstring> percorsi; for (auto& f : file) percorsi.push_back(f.percorso);
        const unsigned g = ++giro;
        std::shared_ptr<Lavoro> l = lavoro;
        HWND h = v.fin;
        std::thread([l, percorsi, lato, g, h] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (const auto& p : percorsi) {
                Immagine im; im.percorso = p;
                IShellItemImageFactory* fab = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&fab)))) {
                    HBITMAP hb = nullptr;
                    if (SUCCEEDED(fab->GetImage({lato, lato}, SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
                        BITMAP bm{}; GetObject(hb, sizeof(bm), &bm);
                        if (bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 2048 && bm.bmHeight <= 2048) {
                            im.px.resize((size_t)bm.bmWidth * bm.bmHeight); im.w = bm.bmWidth; im.h = bm.bmHeight;
                            BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
                            HDC scr = GetDC(nullptr); GetDIBits(scr, hb, 0, bm.bmHeight, im.px.data(), &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, scr);
                            bool alfa = false; for (UINT x : im.px) if (x >> 24) { alfa = true; break; }
                            for (UINT& x : im.px) {
                                const UINT a = alfa ? (x >> 24) : 255;
                                x = (a << 24) | ((((x >> 16) & 255) * a / 255) << 16) | ((((x >> 8) & 255) * a / 255) << 8) | ((x & 255) * a / 255);
                            }
                        }
                        DeleteObject(hb);
                    }
                    fab->Release();
                }
                { std::lock_guard<std::mutex> k(l->mx); if (l->giro != g) l->pronte.clear(); l->giro = g; l->pronte.push_back(std::move(im)); }
                PostMessageW(h, WM_APP + 1, 0, 0);
            }
            CoUninitialize();
        }).detach();
    }

    void raccogli() {
        std::vector<Immagine> arrivate;
        { std::lock_guard<std::mutex> k(lavoro->mx); if (lavoro->giro == giro) arrivate.swap(lavoro->pronte); }
        if (arrivate.empty() || !v.rt || stato == 0) return;
        for (auto& im : arrivate) {
            if (im.px.empty()) continue;
            for (auto& f : file) if (f.percorso == im.percorso && !f.anteprima)
                v.rt->CreateBitmap(D2D1::SizeU(im.w, im.h), im.px.data(), im.w * 4,
                                   D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &f.anteprima);
        }
        disegna();
    }

    D2D1_RECT_F rettCella(int i) const {
        const float d = v.dpi, l = 12 * d + (i % COLONNE) * CELLA * d, t = 48 * d + (i / COLONNE) * ALTA * d;
        return {l, t, l + CELLA * d, t + ALTA * d};
    }
    D2D1_RECT_F rettLink() const { const float d = v.dpi; return {12 * d, v.H - 44 * d, v.W - 12 * d, v.H - 10 * d}; }

    int cellaIn(int px, int py) const {
        for (int i = 0; i < (int)file.size(); i++) { const D2D1_RECT_F r = rettCella(i); if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) return i; }
        const D2D1_RECT_F l = rettLink();
        if (px >= l.left && px < l.right && py >= l.top && py < l.bottom) return LINK;
        return -1;
    }

    void trascina(int i) {
        IShellItem* it = nullptr;
        if (FAILED(SHCreateItemFromParsingName(file[i].percorso.c_str(), nullptr, IID_PPV_ARGS(&it)))) return;
        IDataObject* dati = nullptr;
        if (SUCCEEDED(it->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dati))) && dati) {
            ReleaseCapture();
            trascinando = true;
            ShowWindow(v.fin, SW_HIDE);   // (via dal mezzo: si vede dove lo si lascia)
            DWORD effetto = 0;
            DoDragDrop(dati, &fonte, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effetto);
            trascinando = false;
            dati->Release();
            stato = 0; vetro::fermaBattito(v.fin, 1); libera();
        }
        it->Release();
    }

    void passo() {
        const ULONGLONG ora = vetro::ms();
        if (stato == 1) { k = std::min(1.0, (ora - t0) / 200.0); if (k >= 1) { stato = 2; vetro::fermaBattito(v.fin, 1); } }
        else if (stato == 3) {
            k = 1 - std::min(1.0, (ora - t0) / 160.0);
            if (k <= 0) { stato = 0; vetro::fermaBattito(v.fin, 1); ShowWindow(v.fin, SW_HIDE); libera(); return; }
        } else return;
        const double e = stato == 3 ? vetro::uscita(k) : vetro::molla(k);
        SetWindowPos(v.fin, HWND_TOPMOST, x, y + (int)std::lround((1 - e) * 14 * v.dpi), 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
        v.alfa(std::min(1.0, k * 1.5));
    }

    void disegna() {
        if (stato == 0 || !v.inizia()) return;
        const float d = v.dpi;
        const D2D1_COLOR_F ink = v.inchiostro();
        v.testo(titolo, fTitolo, {16 * d, 8 * d, v.W - 16 * d, 40 * d}, ink);
        if (file.empty()) v.testo(testi::t(L"La cartella è vuota", L"The folder is empty"), fNome, {12 * d, 48 * d, v.W - 12 * d, 120 * d}, v.inchiostro(0.6f));
        for (int i = 0; i < (int)file.size(); i++) {
            const D2D1_RECT_F r = rettCella(i);
            if (i == sopra || i == premuto) {
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, i == premuto ? 0.16f : 0.09f) : D2D1::ColorF(0, 0, 0, i == premuto ? 0.10f : 0.05f));
                v.rt->FillRoundedRectangle({{r.left + 3 * d, r.top + 2 * d, r.right - 3 * d, r.bottom - 2 * d}, 10 * d, 10 * d}, v.pen);
            }
            const float cx = (r.left + r.right) / 2, l = 28 * d;
            if (ID2D1Bitmap* b = file[i].anteprima) {
                const D2D1_SIZE_F s = b->GetSize();
                const float f = std::min(2 * l / s.width, 2 * l / s.height), w = s.width * f, h = s.height * f;
                const float cy = r.top + 10 * d + l;
                v.rt->DrawBitmap(b, {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            v.testo(file[i].nome, fNome, {r.left + 4 * d, r.top + 72 * d, r.right - 4 * d, r.top + 92 * d}, ink);
        }
        const D2D1_RECT_F rl = rettLink();
        if (sopra == LINK) { v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.09f) : D2D1::ColorF(0, 0, 0, 0.05f)); v.rt->FillRoundedRectangle({rl, 8 * d, 8 * d}, v.pen); }
        v.testo(testi::t(L"Apri in Esplora file", L"Open in File Explorer"), fLink, {rl.left + 8 * d, rl.top, rl.right, rl.bottom},
                v.scuro ? D2D1::ColorF(0.40f, 0.68f, 1.0f, 1) : D2D1::ColorF(0.0f, 0.42f, 0.95f, 1));
        v.finisci();
    }
};

}  // namespace pila
