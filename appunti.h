// Cronologia degli appunti (centro di controllo, vista "Appunti"): gli ultimi testi e immagini copiati, con un clic si
// rimettono negli appunti. Solo in memoria, mai su disco: chiudendo FluidWin sparisce. Si ascoltano gli appunti con
// AddClipboardFormatListener (WM_CLIPBOARDUPDATE alla finestra dei messaggi).
// Privacy: quello che le app segnano come da non tenere (gestori di password, "ExcludeClipboardContentFromMonitor
// Processing" e "CanIncludeInClipboardHistory" = 0, le stesse regole della cronologia di Windows) non entra.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

namespace appunti {

struct Voce {
    UINT id = 0;
    bool immagine = false;
    std::wstring testo;            // testo (al massimo 20.000 caratteri)
    std::vector<BYTE> dib;         // immagine: CF_DIB com'era
    int w = 0, h = 0;              // misure dell'immagine
    std::vector<UINT> anteprima;   // pixel BGRA premoltiplicati aw x ah (immagini)
    int aw = 0, ah = 0;
    INT64 quando = 0;              // FILETIME
};

class Cronologia {
public:
    static Cronologia& io() { static Cronologia c; return c; }
    std::vector<Voce> voci;   // la piu' recente per prima
    UINT versione = 0;

    void avvia(HWND h) { finestra = h; AddClipboardFormatListener(h); }
    void ferma() { if (finestra) RemoveClipboardFormatListener(finestra); finestra = nullptr; svuota(); }

    void svuota() { voci.clear(); versione++; }
    void togli(UINT id) { voci.erase(std::remove_if(voci.begin(), voci.end(), [id](const Voce& v) { return v.id == id; }), voci.end()); versione++; }

    // WM_CLIPBOARDUPDATE
    void cambiato() {
        if (ignora > 0) { ignora--; return; }   // l'abbiamo rimesso noi
        if (!apri()) return;
        static const UINT ESCLUDI = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
        static const UINT CRONO = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
        bool vietato = IsClipboardFormatAvailable(ESCLUDI) != FALSE;
        if (!vietato && IsClipboardFormatAvailable(CRONO)) {
            if (HANDLE hc = GetClipboardData(CRONO)) { if (const DWORD* p = (const DWORD*)GlobalLock(hc)) { vietato = *p == 0; GlobalUnlock(hc); } }
        }
        Voce v;
        bool c = false;
        if (!vietato && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE ht = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* p = (const wchar_t*)GlobalLock(ht)) {
                    const size_t n = wcsnlen(p, GlobalSize(ht) / sizeof(wchar_t));
                    v.testo.assign(p, std::min<size_t>(n, 20000));
                    GlobalUnlock(ht);
                }
            }
            c = std::any_of(v.testo.begin(), v.testo.end(), [](wchar_t ch) { return !iswspace(ch); });
        } else if (!vietato && IsClipboardFormatAvailable(CF_DIB)) {
            if (HANDLE hd = GetClipboardData(CF_DIB)) {
                const SIZE_T n = GlobalSize(hd);
                if (n > sizeof(BITMAPINFOHEADER) && n < 64 * 1024 * 1024) {
                    if (const BYTE* p = (const BYTE*)GlobalLock(hd)) { v.dib.assign(p, p + n); GlobalUnlock(hd); }
                }
            }
            v.immagine = true;
            c = !v.dib.empty() && anteprima(v);
        }
        CloseClipboard();
        if (!c) return;
        FILETIME ft; GetSystemTimeAsFileTime(&ft); v.quando = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        // gia' copiato: torna in cima (niente doppioni)
        for (size_t i = 0; i < voci.size(); i++)
            if (voci[i].immagine == v.immagine && (v.immagine ? voci[i].dib == v.dib : voci[i].testo == v.testo)) { voci.erase(voci.begin() + i); break; }
        v.id = ++ultimoId;
        voci.insert(voci.begin(), std::move(v));
        // al massimo 25 voci, di cui 6 immagini (le immagini pesano)
        int imm = 0;
        for (size_t i = 0; i < voci.size();) {
            if (voci[i].immagine && ++imm > 6) { voci.erase(voci.begin() + i); continue; }
            i++;
        }
        if (voci.size() > 25) voci.resize(25);
        versione++;
    }

    // rimette negli appunti la voce scelta (e la porta in cima)
    bool ricopia(UINT id) {
        auto it = std::find_if(voci.begin(), voci.end(), [id](const Voce& v) { return v.id == id; });
        if (it == voci.end() || !finestra || !apri()) return false;
        EmptyClipboard();
        bool ok = false;
        if (it->immagine) {
            if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, it->dib.size())) {
                memcpy(GlobalLock(g), it->dib.data(), it->dib.size()); GlobalUnlock(g);
                ok = SetClipboardData(CF_DIB, g) != nullptr; if (!ok) GlobalFree(g);
            }
        } else if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (it->testo.size() + 1) * sizeof(wchar_t))) {
            memcpy(GlobalLock(g), it->testo.c_str(), (it->testo.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr; if (!ok) GlobalFree(g);
        }
        CloseClipboard();
        if (ok) {
            ignora++;
            Voce v = std::move(*it); voci.erase(it);
            FILETIME ft; GetSystemTimeAsFileTime(&ft); v.quando = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            voci.insert(voci.begin(), std::move(v));
            versione++;
        }
        return ok;
    }

private:
    HWND finestra = nullptr;
    UINT ultimoId = 0;
    int ignora = 0;

    bool apri() {
        for (int i = 0; i < 5; i++) { if (OpenClipboard(finestra)) return true; Sleep(15); }   // un'altra app li tiene aperti
        return false;
    }

    // anteprima alta al massimo 96 px (GDI disegna qualsiasi CF_DIB, anche i formati strani)
    static bool anteprima(Voce& v) {
        const BITMAPINFOHEADER* bh = (const BITMAPINFOHEADER*)v.dib.data();
        if (bh->biSize < sizeof(BITMAPINFOHEADER) || bh->biWidth <= 0 || bh->biHeight == 0) return false;
        v.w = bh->biWidth; v.h = std::abs(bh->biHeight);
        const double k = std::min(1.0, std::min(96.0 / v.h, 160.0 / v.w));
        v.aw = std::max(1, (int)(v.w * k)); v.ah = std::max(1, (int)(v.h * k));
        // dove cominciano i pixel: dopo l'intestazione, le maschere (BI_BITFIELDS) e la tavolozza
        size_t off = bh->biSize;
        if (bh->biCompression == BI_BITFIELDS && bh->biSize == sizeof(BITMAPINFOHEADER)) off += 12;
        const DWORD colori = bh->biClrUsed ? bh->biClrUsed : (bh->biBitCount <= 8 ? (1u << bh->biBitCount) : 0);
        off += colori * 4;
        if (off >= v.dib.size()) return false;
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), v.aw, -v.ah, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
        HBITMAP b = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool ok = false;
        if (b && bits) {
            HGDIOBJ vec = SelectObject(mem, b);
            SetStretchBltMode(mem, HALFTONE); SetBrushOrgEx(mem, 0, 0, nullptr);
            ok = StretchDIBits(mem, 0, 0, v.aw, v.ah, 0, 0, v.w, v.h, v.dib.data() + off, (const BITMAPINFO*)bh, DIB_RGB_COLORS, SRCCOPY) > 0;
            GdiFlush();
            if (ok) { v.anteprima.assign((UINT*)bits, (UINT*)bits + (size_t)v.aw * v.ah); for (UINT& p : v.anteprima) p |= 0xFF000000; }
            SelectObject(mem, vec);
        }
        if (b) DeleteObject(b);
        DeleteDC(mem); ReleaseDC(nullptr, scr);
        return ok;
    }
};

}  // namespace appunti
