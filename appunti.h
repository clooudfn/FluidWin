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
#include <cstdint>
#include <cstring>

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

public:
    // Dove cominciano i pixel di un CF_DIB, solo se GDI puo' leggerlo tutto senza uscire dal buffer; 0 se no.
    // Il CF_DIB lo scrive un'altra app qualsiasi: prima si controllava solo che l'intestazione ci fosse, e StretchDIBits
    // riceveva misure e tavolozza piu' grandi dei byte veri (letture fuori dal buffer, audit FW-AUDIT-010). Si accettano
    // solo i formati non compressi (BI_RGB, BI_BITFIELDS): gli altri (RLE, JPEG, PNG) negli appunti sono rarissimi e la
    // loro misura in byte non si puo' controllare senza decodificarli. Conti a 64 bit: niente traboccamenti
    static size_t inizioPixelDib(const BYTE* p, size_t n) {
        if (!p || n < sizeof(BITMAPINFOHEADER)) return 0;
        BITMAPINFOHEADER bh;
        memcpy(&bh, p, sizeof(bh));
        if (bh.biSize < sizeof(BITMAPINFOHEADER) || bh.biSize > n) return 0;
        // misure: (anche -32768..-1 per le immagini dall'alto; mai LONG_MIN, che std::abs non sa rappresentare)
        if (bh.biWidth <= 0 || bh.biWidth > 32768 || bh.biHeight == 0 || bh.biHeight < -32768 || bh.biHeight > 32768) return 0;
        const unsigned bpp = bh.biBitCount;
        if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
        uint64_t off = bh.biSize;
        if (bh.biCompression == BI_BITFIELDS) {
            if (bpp != 16 && bpp != 32) return 0;
            if (bh.biSize == sizeof(BITMAPINFOHEADER)) off += 12;   // le tre maschere dopo l'intestazione (V4/V5 le hanno dentro)
        } else if (bh.biCompression != BI_RGB) return 0;
        // tavolozza: GDI ne legge biClrUsed colori (o tutti quelli possibili se e' 0)
        const uint64_t massimo = bpp <= 8 ? (1u << bpp) : 256;
        const uint64_t colori = bh.biClrUsed ? bh.biClrUsed : (bpp <= 8 ? massimo : 0);
        if (colori > massimo) return 0;
        off += colori * 4;
        const uint64_t riga = ((uint64_t)bh.biWidth * bpp + 31) / 32 * 4;   // le righe sono allineate a 4 byte
        const uint64_t alta = (uint64_t)(bh.biHeight < 0 ? -(int64_t)bh.biHeight : bh.biHeight);
        if (off + riga * alta > n) return 0;
        return (size_t)off;
    }

private:
    // anteprima alta al massimo 96 px
    static bool anteprima(Voce& v) {
        const size_t off = inizioPixelDib(v.dib.data(), v.dib.size());
        if (!off) return false;
        const BITMAPINFOHEADER* bh = (const BITMAPINFOHEADER*)v.dib.data();
        v.w = bh->biWidth; v.h = std::abs(bh->biHeight);
        const double k = std::min(1.0, std::min(96.0 / v.h, 160.0 / v.w));
        v.aw = std::max(1, (int)(v.w * k)); v.ah = std::max(1, (int)(v.h * k));
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
