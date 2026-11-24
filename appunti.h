// Cronologia degli appunti: memoria condivisa fra il centro di controllo e la finestra di vetro (Win+V).
// Le voci non fissate restano solo in memoria; le fissate si salvano in %LOCALAPPDATA%\FluidWin\appunti_fissati.bin.
// Rispetta i segni di riservatezza dei gestori di password.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <iterator>
#include <cwctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "vetro.h"
#include "testi.h"
#include "menu.h"

namespace appunti {

inline std::wstring trimSpazi(const std::wstring& s) {
    size_t ini = 0;
    while (ini < s.size() && iswspace(s[ini])) ini++;
    size_t fin = s.size();
    while (fin > ini && iswspace(s[fin - 1])) fin--;
    return s.substr(ini, fin - ini);
}

inline bool estraiColoreHex(const std::wstring& s, D2D1_COLOR_F& out) {
    const std::wstring t = trimSpazi(s);
    if (t.empty() || t[0] != L'#') return false;
    const size_t n = t.size();
    if (n != 4 && n != 7 && n != 9) return false;
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
        if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
        return -1;
    };
    int r = 0, g = 0, b = 0, a = 255;
    if (n == 4) {
        const int h1 = hex(t[1]), h2 = hex(t[2]), h3 = hex(t[3]);
        if (h1 < 0 || h2 < 0 || h3 < 0) return false;
        r = h1 * 17;
        g = h2 * 17;
        b = h3 * 17;
    } else {
        auto coppia = [&](size_t i) -> int {
            const int hi = hex(t[i]), lo = hex(t[i + 1]);
            return (hi < 0 || lo < 0) ? -1 : (hi * 16 + lo);
        };
        r = coppia(1);
        g = coppia(3);
        b = coppia(5);
        if (n == 9) a = coppia(7);
        if (r < 0 || g < 0 || b < 0 || a < 0) return false;
    }
    out = D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    return true;
}

inline bool eUrl(const std::wstring& s) {
    const std::wstring t = trimSpazi(s);
    if (t.size() <= 10) return false;
    for (wchar_t c : t) {
        if (iswspace(c)) return false;
    }
    auto iniziaCon = [&](const wchar_t* pref, size_t len) -> bool {
        if (t.size() < len) return false;
        for (size_t i = 0; i < len; i++) {
            if (towlower(t[i]) != pref[i]) return false;
        }
        return true;
    };
    return iniziaCon(L"https://", 8) || iniziaCon(L"http://", 7);
}

inline bool eFileImmagine(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos && dot < sep) return false;
    const wchar_t* ext = path.c_str() + dot;
    static constexpr const wchar_t* EXT[] = {
        L".png", L".jpg", L".jpeg", L".bmp", L".webp", L".gif", L".ico"
    };
    for (const wchar_t* e : EXT) {
        if (_wcsicmp(ext, e) == 0) return true;
    }
    return false;
}

struct Voce {
    UINT id = 0;
    bool immagine = false;
    bool eFile = false;
    bool fissata = false;
    std::wstring testo;            // testo intero (al massimo 256 KB)
    std::wstring anteprimaTesto;   // testo normalizzato su una riga/due per la scheda
    std::vector<std::wstring> filePercorsi;
    std::vector<BYTE> dib;         // immagine: CF_DIB originale per incollare senza perdita
    int w = 0, h = 0;              // misure reali dell'immagine
    std::vector<UINT> anteprima;   // pixel BGRA premoltiplicati aw x ah (al massimo 320 px di larghezza)
    int aw = 0, ah = 0;
    std::wstring sorgente;         // nome dell'app che ha copiato (senza .exe)
    INT64 quando = 0;              // FILETIME (usato dal centro di controllo)
    ULONGLONG tick = 0;            // GetTickCount64() per il tempo relativo
};

inline bool haAnteprimaImmagine(const Voce& v) {
    return v.immagine || (v.eFile && !v.anteprima.empty());
}

class Cronologia {
public:
    static Cronologia& io() { static Cronologia c; return c; }
    std::vector<Voce> voci;   // fissate in cima, poi le piu' recenti
    UINT versione = 0;

    void avvia(HWND h) {
        if (finestra && finestra != h) RemoveClipboardFormatListener(finestra);
        finestra = h;
        if (finestra) AddClipboardFormatListener(finestra);
        if (!fissatiCaricati) {
            fissatiCaricati = true;
            caricaFissati();
        }
    }

    void ferma() {
        if (finestra) RemoveClipboardFormatListener(finestra);
        finestra = nullptr;
        if (!voci.empty()) {
            voci.clear();
            versione++;
        }
        fissatiCaricati = false;
    }

    void svuota() {
        if (!voci.empty()) {
            voci.clear();
            versione++;
        }
        salvaFissati();
    }

    void svuotaNonFissate() {
        const size_t prima = voci.size();
        voci.erase(std::remove_if(voci.begin(), voci.end(), [](const Voce& v) { return !v.fissata; }), voci.end());
        if (voci.size() != prima) versione++;
    }

    void togli(UINT id) {
        auto it = std::find_if(voci.begin(), voci.end(), [id](const Voce& v) { return v.id == id; });
        if (it == voci.end()) return;
        const bool eraFissata = it->fissata;
        voci.erase(it);
        versione++;
        if (eraFissata) salvaFissati();
    }

    void fissa(UINT id) {
        auto it = std::find_if(voci.begin(), voci.end(), [id](const Voce& v) { return v.id == id; });
        if (it == voci.end()) return;
        Voce v = std::move(*it);
        voci.erase(it);
        v.fissata = !v.fissata;
        inserisciNelGruppo(std::move(v));
        pulisciLimite();
        versione++;
        salvaFissati();
    }

    // WM_CLIPBOARDUPDATE
    bool cambiato() {
        const DWORD seq = GetClipboardSequenceNumber();
        if (haSequenzaInterna && seq == ultimaSequenzaInterna) return false;

        const std::wstring app = nomeSorgente();
        if (!apri()) return false;

        static const UINT ESCLUDI = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
        static const UINT IGNORA = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
        static const UINT CRONO = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");

        bool vietato = (ESCLUDI && IsClipboardFormatAvailable(ESCLUDI) != FALSE) ||
                       (IGNORA && IsClipboardFormatAvailable(IGNORA) != FALSE);
        // col segno presente si archivia solo se si legge un DWORD diverso da 0: illeggibile vale come vietato
        if (!vietato && CRONO && IsClipboardFormatAvailable(CRONO)) {
            vietato = true;
            if (HANDLE hc = GetClipboardData(CRONO)) {
                if (GlobalSize(hc) >= sizeof(DWORD)) {
                    if (const DWORD* p = (const DWORD*)GlobalLock(hc)) {
                        vietato = (*p == 0);
                        GlobalUnlock(hc);
                    }
                }
            }
        }

        Voce v;
        bool ok = false;
        if (!vietato && IsClipboardFormatAvailable(CF_HDROP)) {
            if (HANDLE hf = GetClipboardData(CF_HDROP)) {
                if (HDROP hd = static_cast<HDROP>(GlobalLock(hf))) {
                    const UINT tot = DragQueryFileW(hd, 0xFFFFFFFFu, nullptr, 0);
                    const UINT lim = std::min<UINT>(tot, MAX_FILE_PERCORSI);
                    v.filePercorsi.reserve(lim);
                    for (UINT i = 0; i < lim; i++) {
                        const UINT len = DragQueryFileW(hd, i, nullptr, 0);
                        if (len > 0 && len <= (MAX_FILE_PATH_BYTES / sizeof(wchar_t))) {
                            std::wstring p(len, L'\0');
                            if (DragQueryFileW(hd, i, &p[0], len + 1) == len) {
                                v.filePercorsi.push_back(std::move(p));
                            }
                        }
                    }
                    GlobalUnlock(hf);
                }
            }
            if (!v.filePercorsi.empty()) {
                v.eFile = true;
                for (size_t i = 0; i < v.filePercorsi.size(); i++) {
                    if (i > 0) v.testo.push_back(L'\n');
                    v.testo += v.filePercorsi[i];
                }
                v.anteprimaTesto = anteprimaFile(v.filePercorsi);
                if (v.filePercorsi.size() == 1 && eFileImmagine(v.filePercorsi[0])) {
                    creaAnteprimaDaFile(v.filePercorsi[0], v);
                }
                ok = true;
            }
        }
        if (!vietato && !ok) {
            constexpr UINT FORMATI_DIB[2] = { CF_DIB, CF_DIBV5 };
            constexpr SIZE_T MAX_DIB = 16 * 1024 * 1024;
            for (UINT fmtDib : FORMATI_DIB) {
                if (!IsClipboardFormatAvailable(fmtDib)) continue;
                v.dib.clear();
                if (HANDLE hd = GetClipboardData(fmtDib)) {
                    const SIZE_T n = GlobalSize(hd);
                    if (n > sizeof(BITMAPINFOHEADER) && n <= MAX_DIB) {
                        if (const BYTE* p = (const BYTE*)GlobalLock(hd)) {
                            v.dib.assign(p, p + n);
                            GlobalUnlock(hd);
                        }
                    }
                }
                if (!v.dib.empty() && creaAnteprima(v)) {
                    v.immagine = true;
                    ok = true;
                    break;
                }
                v.dib.clear();
                v.anteprima.clear();
                v.w = v.h = v.aw = v.ah = 0;
                v.immagine = false;
            }
        }
        if (!vietato && !ok && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE ht = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* p = (const wchar_t*)GlobalLock(ht)) {
                    constexpr size_t MAX_CARATTERI = (256 * 1024) / sizeof(wchar_t);
                    const size_t n = wcsnlen(p, GlobalSize(ht) / sizeof(wchar_t));
                    v.testo.assign(p, std::min<size_t>(n, MAX_CARATTERI));
                    GlobalUnlock(ht);
                }
            }
            ok = std::any_of(v.testo.begin(), v.testo.end(), [](wchar_t ch) { return !iswspace(ch); });
            if (ok) v.anteprimaTesto = normalizzaAnteprima(v.testo);
        }
        CloseClipboard();
        if (!ok) return false;

        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        v.quando = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        v.tick = GetTickCount64();
        v.sorgente = app;

        bool eraFissata = false;
        for (size_t i = 0; i < voci.size(); i++) {
            const bool stesso = (voci[i].immagine == v.immagine) && (voci[i].eFile == v.eFile) &&
                (v.immagine ? (voci[i].dib == v.dib)
                            : (v.eFile ? (voci[i].filePercorsi == v.filePercorsi)
                                       : (voci[i].testo == v.testo)));
            if (stesso) {
                eraFissata = voci[i].fissata;
                voci.erase(voci.begin() + (ptrdiff_t)i);
                break;
            }
        }
        v.fissata = eraFissata;
        v.id = ++ultimoId;
        inserisciNelGruppo(std::move(v));
        pulisciLimite();
        versione++;
        if (eraFissata) salvaFissati();
        return true;
    }

    // rimette negli appunti la voce scelta (e la porta in cima al suo gruppo)
    bool ricopia(UINT id, bool spostaInCima = true) {
        auto it = std::find_if(voci.begin(), voci.end(), [id](const Voce& v) { return v.id == id; });
        if (it == voci.end()) return false;

        HGLOBAL g = nullptr;
        UINT fmt = CF_UNICODETEXT;
        if (it->eFile && !it->filePercorsi.empty()) {
            size_t totalChars = 1;
            for (const auto& path : it->filePercorsi) totalChars += path.size() + 1;
            const size_t n = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);
            g = GlobalAlloc(GMEM_MOVEABLE, n);
            if (!g) return false;
            void* p = GlobalLock(g);
            if (!p) { GlobalFree(g); return false; }
            DROPFILES* df = static_cast<DROPFILES*>(p);
            df->pFiles = sizeof(DROPFILES);
            df->pt = {0, 0};
            df->fNC = FALSE;
            df->fWide = TRUE;
            wchar_t* dst = reinterpret_cast<wchar_t*>(static_cast<BYTE*>(p) + sizeof(DROPFILES));
            for (const auto& path : it->filePercorsi) {
                memcpy(dst, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
                dst += path.size() + 1;
            }
            *dst = L'\0';
            GlobalUnlock(g);
            fmt = CF_HDROP;
        } else {
            const void* dati = it->immagine ? (const void*)it->dib.data() : (const void*)it->testo.c_str();
            const size_t n = it->immagine ? it->dib.size() : (it->testo.size() + 1) * sizeof(wchar_t);
            g = n ? GlobalAlloc(GMEM_MOVEABLE, n) : nullptr;
            if (!g) return false;
            void* p = GlobalLock(g);
            if (!p) { GlobalFree(g); return false; }
            memcpy(p, dati, n);
            GlobalUnlock(g);
            fmt = it->immagine ? CF_DIB : CF_UNICODETEXT;
        }

        if (!apri()) { GlobalFree(g); return false; }
        bool ok = false;
        if (EmptyClipboard()) ok = SetClipboardData(fmt, g) != nullptr;
        if (!ok) GlobalFree(g);
        CloseClipboard();
        if (ok) {
            ultimaSequenzaInterna = GetClipboardSequenceNumber();
            haSequenzaInterna = true;
            if (spostaInCima) {
                Voce v = std::move(*it);
                const bool eraFissata = v.fissata;
                voci.erase(it);
                FILETIME ft{};
                GetSystemTimeAsFileTime(&ft);
                v.quando = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
                v.tick = GetTickCount64();
                inserisciNelGruppo(std::move(v));
                versione++;
                if (eraFissata) salvaFissati();
            }
        }
        return ok;
    }

    bool copiaTestoSemplice(const std::wstring& t) {
        const size_t n = (t.size() + 1) * sizeof(wchar_t);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
        if (!g) return false;
        void* p = GlobalLock(g);
        if (!p) { GlobalFree(g); return false; }
        memcpy(p, t.c_str(), n);
        GlobalUnlock(g);
        if (!apri()) { GlobalFree(g); return false; }
        bool ok = false;
        if (EmptyClipboard()) ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
        if (!ok) GlobalFree(g);
        CloseClipboard();
        if (ok) {
            ultimaSequenzaInterna = GetClipboardSequenceNumber();
            haSequenzaInterna = true;
        }
        return ok;
    }

    static size_t inizioPixelDib(const BYTE* p, size_t n) {
        if (!p || n < sizeof(BITMAPINFOHEADER)) return 0;
        BITMAPINFOHEADER bh{};
        memcpy(&bh, p, sizeof(bh));
        if (bh.biSize < sizeof(BITMAPINFOHEADER) || bh.biSize > n) return 0;
        if (bh.biWidth <= 0 || bh.biWidth > 32768 || bh.biHeight == 0 || bh.biHeight < -32768 || bh.biHeight > 32768) return 0;
        const unsigned bpp = bh.biBitCount;
        if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
        uint64_t off = bh.biSize;
        if (bh.biCompression == BI_BITFIELDS) {
            if (bpp != 16 && bpp != 32) return 0;
            if (bh.biSize == sizeof(BITMAPINFOHEADER)) off += 12;
        } else if (bh.biCompression != BI_RGB) return 0;
        const uint64_t massimo = bpp <= 8 ? (1u << bpp) : 256;
        const uint64_t colori = bh.biClrUsed ? bh.biClrUsed : (bpp <= 8 ? massimo : 0);
        if (colori > massimo) return 0;
        off += colori * 4;
        const uint64_t riga = ((uint64_t)bh.biWidth * bpp + 31) / 32 * 4;
        const uint64_t alta = (uint64_t)(bh.biHeight < 0 ? -(int64_t)bh.biHeight : bh.biHeight);
        if (off + riga * alta > n) return 0;
        return (size_t)off;
    }

private:
    static constexpr uint32_t VERSIONE_FISSATI = 2;
    static constexpr uint32_t MAX_FISSATI = 50;
    static constexpr uint32_t MAX_SORG_BYTES = 4096;
    static constexpr uint32_t MAX_TESTO_BYTES = 512 * 1024;
    static constexpr uint32_t MAX_DIB_BYTES = 16 * 1024 * 1024;
    static constexpr uint32_t MAX_ANT_BYTES = 4 * 1024 * 1024;
    static constexpr uint32_t MAX_FILE_PERCORSI = 64;
    static constexpr uint32_t MAX_FILE_PATH_BYTES = 4096;
    static constexpr size_t MAX_FILE_FISSATI = 64 * 1024 * 1024;

    using FnCryptProtectData = BOOL (WINAPI*)(
        DATA_BLOB*, LPCWSTR, DATA_BLOB*, PVOID, CRYPTPROTECT_PROMPTSTRUCT*, DWORD, DATA_BLOB*);
    using FnCryptUnprotectData = BOOL (WINAPI*)(
        DATA_BLOB*, LPWSTR*, DATA_BLOB*, PVOID, CRYPTPROTECT_PROMPTSTRUCT*, DWORD, DATA_BLOB*);

    HWND finestra = nullptr;
    UINT ultimoId = 0;
    DWORD ultimaSequenzaInterna = 0;
    bool haSequenzaInterna = false;
    bool fissatiCaricati = false;

    static std::wstring percorsoFissati(bool creaDir = false) {
        wchar_t base[MAX_PATH] = L"";
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base)) || !base[0]) return {};
        const std::wstring dir = std::wstring(base) + L"\\FluidWin";
        if (creaDir) CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\appunti_fissati.bin";
    }

    static bool scriviTutto(HANDLE h, const void* dati, size_t n) {
        const BYTE* p = (const BYTE*)dati;
        while (n > 0) {
            const DWORD chunk = (DWORD)std::min<size_t>(n, 0x40000000u);
            DWORD scritti = 0;
            if (!WriteFile(h, p, chunk, &scritti, nullptr) || scritti == 0) return false;
            p += scritti;
            n -= scritti;
        }
        return true;
    }

    static bool leggiTutto(HANDLE h, void* dati, size_t n) {
        BYTE* p = (BYTE*)dati;
        while (n > 0) {
            const DWORD chunk = (DWORD)std::min<size_t>(n, 0x40000000u);
            DWORD letti = 0;
            if (!ReadFile(h, p, chunk, &letti, nullptr) || letti == 0) return false;
            p += letti;
            n -= letti;
        }
        return true;
    }

    static bool caricaDpapi(FnCryptProtectData& fnProt, FnCryptUnprotectData& fnUnprot) {
        static HMODULE mod = LoadLibraryW(L"crypt32.dll");
        static FnCryptProtectData pProt = mod
            ? reinterpret_cast<FnCryptProtectData>(reinterpret_cast<void*>(GetProcAddress(mod, "CryptProtectData")))
            : nullptr;
        static FnCryptUnprotectData pUnprot = mod
            ? reinterpret_cast<FnCryptUnprotectData>(reinterpret_cast<void*>(GetProcAddress(mod, "CryptUnprotectData")))
            : nullptr;
        fnProt = pProt;
        fnUnprot = pUnprot;
        return fnProt != nullptr && fnUnprot != nullptr;
    }

    static DATA_BLOB entropiaDpapi() {
        static constexpr char ENTROPIA[] = "FluidWin.Appunti.v1";
        return DATA_BLOB{ (DWORD)(sizeof(ENTROPIA) - 1), const_cast<BYTE*>(reinterpret_cast<const BYTE*>(ENTROPIA)) };
    }

    static bool dpapiProteggi(const std::vector<BYTE>& in, std::vector<BYTE>& out) {
        out.clear();
        if (in.empty() || in.size() > MAX_FILE_FISSATI) return false;
        FnCryptProtectData fnProt = nullptr;
        FnCryptUnprotectData fnUnprot = nullptr;
        if (!caricaDpapi(fnProt, fnUnprot)) return false;

        DATA_BLOB blobIn{ (DWORD)in.size(), const_cast<BYTE*>(in.data()) };
        DATA_BLOB entropia = entropiaDpapi();
        DATA_BLOB blobOut{ 0, nullptr };
        const BOOL ok = fnProt(&blobIn, nullptr, &entropia, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blobOut);
        if (!ok || !blobOut.pbData || blobOut.cbData == 0) {
            if (blobOut.pbData) {
                SecureZeroMemory(blobOut.pbData, blobOut.cbData);
                LocalFree(blobOut.pbData);
            }
            return false;
        }
        out.assign(blobOut.pbData, blobOut.pbData + blobOut.cbData);
        SecureZeroMemory(blobOut.pbData, blobOut.cbData);
        LocalFree(blobOut.pbData);
        return true;
    }

    static bool dpapiSproteggi(const BYTE* data, size_t len, std::vector<BYTE>& out) {
        out.clear();
        if (!data || len == 0 || len > MAX_FILE_FISSATI) return false;
        FnCryptProtectData fnProt = nullptr;
        FnCryptUnprotectData fnUnprot = nullptr;
        if (!caricaDpapi(fnProt, fnUnprot)) return false;

        DATA_BLOB blobIn{ (DWORD)len, const_cast<BYTE*>(data) };
        DATA_BLOB entropia = entropiaDpapi();
        DATA_BLOB blobOut{ 0, nullptr };
        const BOOL ok = fnUnprot(&blobIn, nullptr, &entropia, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blobOut);
        if (!ok || !blobOut.pbData || blobOut.cbData == 0) {
            if (blobOut.pbData) {
                SecureZeroMemory(blobOut.pbData, blobOut.cbData);
                LocalFree(blobOut.pbData);
            }
            return false;
        }
        out.assign(blobOut.pbData, blobOut.pbData + blobOut.cbData);
        SecureZeroMemory(blobOut.pbData, blobOut.cbData);
        LocalFree(blobOut.pbData);
        return true;
    }

    static void appendBytes(std::vector<BYTE>& buf, const void* dati, size_t n) {
        if (n == 0) return;
        const BYTE* p = static_cast<const BYTE*>(dati);
        buf.insert(buf.end(), p, p + n);
    }

    template <typename T>
    static void appendVal(std::vector<BYTE>& buf, const T& val) {
        appendBytes(buf, &val, sizeof(T));
    }

    static bool readBytes(const BYTE* data, size_t len, size_t& pos, void* dst, size_t n) {
        if (n > len || pos > len - n) return false;
        if (n > 0) memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }

    template <typename T>
    static bool readVal(const BYTE* data, size_t len, size_t& pos, T& val) {
        return readBytes(data, len, pos, &val, sizeof(T));
    }

    static bool parseFissatiPayload(const BYTE* data, size_t len, std::vector<Voce>& caricate) {
        caricate.clear();
        size_t pos = 0;
        char magic[4]{};
        uint32_t ver = 0, count = 0;
        if (!readBytes(data, len, pos, magic, 4) || !readVal(data, len, pos, ver) || !readVal(data, len, pos, count)) {
            return false;
        }
        if (memcmp(magic, "FWCB", 4) != 0 || (ver != 1u && ver != VERSIONE_FISSATI) || count > MAX_FISSATI) {
            return false;
        }

        FILETIME ftOra{};
        GetSystemTimeAsFileTime(&ftOra);
        const INT64 oraFt = (INT64)(((uint64_t)ftOra.dwHighDateTime << 32) | (uint64_t)ftOra.dwLowDateTime);
        const ULONGLONG oraTick = GetTickCount64();

        caricate.reserve(count);
        const uint8_t maxTipo = (ver >= 2u) ? 2u : 1u;
        for (uint32_t i = 0; i < count; i++) {
            uint8_t tipo = 0;
            uint64_t quando = 0;
            uint32_t sorgLen = 0;
            if (!readVal(data, len, pos, tipo) || !readVal(data, len, pos, quando) || !readVal(data, len, pos, sorgLen)) {
                return false;
            }
            if (tipo > maxTipo || sorgLen > MAX_SORG_BYTES || (sorgLen % sizeof(wchar_t)) != 0) {
                return false;
            }

            Voce v;
            v.immagine = (tipo == 1u);
            v.eFile = (tipo == 2u);
            v.fissata = true;
            v.quando = (INT64)quando;
            const ULONGLONG diffMs = (oraFt > v.quando && v.quando > 0) ? (ULONGLONG)((oraFt - v.quando) / 10000LL) : 0ULL;
            v.tick = (oraTick > diffMs) ? (oraTick - diffMs) : 0ULL;

            if (sorgLen > 0) {
                v.sorgente.resize(sorgLen / sizeof(wchar_t));
                if (!readBytes(data, len, pos, &v.sorgente[0], sorgLen)) return false;
            }

            if (tipo == 0u) {
                uint32_t testoLen = 0;
                if (!readVal(data, len, pos, testoLen) || testoLen > MAX_TESTO_BYTES || (testoLen % sizeof(wchar_t)) != 0) {
                    return false;
                }
                if (testoLen > 0) {
                    v.testo.resize(testoLen / sizeof(wchar_t));
                    if (!readBytes(data, len, pos, &v.testo[0], testoLen)) return false;
                }
                v.anteprimaTesto = normalizzaAnteprima(v.testo);
            } else if (tipo == 1u) {
                int32_t w = 0, hImg = 0, aw = 0, ah = 0;
                uint32_t dibLen = 0, antLen = 0;
                if (!readVal(data, len, pos, w) || !readVal(data, len, pos, hImg)
                    || !readVal(data, len, pos, aw) || !readVal(data, len, pos, ah)
                    || !readVal(data, len, pos, dibLen)) {
                    return false;
                }
                if (w <= 0 || w > 32768 || hImg <= 0 || hImg > 32768 || aw <= 0 || aw > 1024 || ah <= 0 || ah > 1024) {
                    return false;
                }
                if (dibLen <= sizeof(BITMAPINFOHEADER) || dibLen > MAX_DIB_BYTES) return false;
                v.dib.resize(dibLen);
                if (!readBytes(data, len, pos, v.dib.data(), dibLen)) return false;
                if (inizioPixelDib(v.dib.data(), v.dib.size()) == 0) return false;

                if (!readVal(data, len, pos, antLen) || antLen > MAX_ANT_BYTES || (antLen % sizeof(UINT)) != 0) {
                    return false;
                }
                if ((uint64_t)aw * (uint64_t)ah * sizeof(UINT) != (uint64_t)antLen) return false;
                v.anteprima.resize(antLen / sizeof(UINT));
                if (antLen > 0 && !readBytes(data, len, pos, v.anteprima.data(), antLen)) return false;
                v.w = (int)w;
                v.h = (int)hImg;
                v.aw = (int)aw;
                v.ah = (int)ah;
            } else {
                uint32_t fileCount = 0;
                if (!readVal(data, len, pos, fileCount) || fileCount == 0 || fileCount > MAX_FILE_PERCORSI) {
                    return false;
                }
                v.filePercorsi.reserve(fileCount);
                for (uint32_t j = 0; j < fileCount; j++) {
                    uint32_t pathLen = 0;
                    if (!readVal(data, len, pos, pathLen) || pathLen == 0 || pathLen > MAX_FILE_PATH_BYTES || (pathLen % sizeof(wchar_t)) != 0) {
                        return false;
                    }
                    std::wstring p(pathLen / sizeof(wchar_t), L'\0');
                    if (!readBytes(data, len, pos, &p[0], pathLen)) return false;
                    v.filePercorsi.push_back(std::move(p));
                }
                for (size_t j = 0; j < v.filePercorsi.size(); j++) {
                    if (j > 0) v.testo.push_back(L'\n');
                    v.testo += v.filePercorsi[j];
                }
                v.anteprimaTesto = anteprimaFile(v.filePercorsi);
                if (v.filePercorsi.size() == 1 && eFileImmagine(v.filePercorsi[0])) {
                    creaAnteprimaDaFile(v.filePercorsi[0], v);
                }
            }

            caricate.push_back(std::move(v));
        }

        return true;
    }

    void salvaFissati() const {
        std::vector<const Voce*> fissate;
        for (const Voce& v : voci) {
            if (v.fissata) fissate.push_back(&v);
        }
        if (fissate.empty()) {
            const std::wstring path = percorsoFissati(false);
            if (!path.empty()) DeleteFileW(path.c_str());
            return;
        }
        const std::wstring path = percorsoFissati(true);
        if (path.empty()) return;

        std::vector<BYTE> plain;
        static constexpr char MAGIA[4] = {'F', 'W', 'C', 'B'};
        const uint32_t ver = VERSIONE_FISSATI;
        const uint32_t count = (uint32_t)std::min<size_t>(fissate.size(), MAX_FISSATI);
        appendBytes(plain, MAGIA, 4);
        appendVal(plain, ver);
        appendVal(plain, count);

        for (uint32_t i = 0; i < count; i++) {
            const Voce& v = *fissate[i];
            const uint8_t tipo = v.immagine ? 1u : ((v.eFile && !v.filePercorsi.empty()) ? 2u : 0u);
            const uint64_t quando = (uint64_t)v.quando;
            const uint32_t sorgLen = (uint32_t)std::min<size_t>(v.sorgente.size() * sizeof(wchar_t), MAX_SORG_BYTES);
            appendVal(plain, tipo);
            appendVal(plain, quando);
            appendVal(plain, sorgLen);
            if (sorgLen > 0) appendBytes(plain, v.sorgente.data(), sorgLen);

            if (tipo == 0u) {
                const uint32_t testoLen = (uint32_t)std::min<size_t>(v.testo.size() * sizeof(wchar_t), MAX_TESTO_BYTES);
                appendVal(plain, testoLen);
                if (testoLen > 0) appendBytes(plain, v.testo.data(), testoLen);
            } else if (tipo == 1u) {
                const int32_t w = (int32_t)v.w;
                const int32_t hImg = (int32_t)v.h;
                const int32_t aw = (int32_t)v.aw;
                const int32_t ah = (int32_t)v.ah;
                const uint32_t dibLen = (uint32_t)v.dib.size();
                const uint32_t antLen = (uint32_t)(v.anteprima.size() * sizeof(UINT));
                appendVal(plain, w);
                appendVal(plain, hImg);
                appendVal(plain, aw);
                appendVal(plain, ah);
                appendVal(plain, dibLen);
                if (dibLen > 0) appendBytes(plain, v.dib.data(), dibLen);
                appendVal(plain, antLen);
                if (antLen > 0) appendBytes(plain, v.anteprima.data(), antLen);
            } else {
                const uint32_t fileCount = (uint32_t)std::min<size_t>(v.filePercorsi.size(), MAX_FILE_PERCORSI);
                appendVal(plain, fileCount);
                for (uint32_t j = 0; j < fileCount; j++) {
                    const uint32_t pathLen = (uint32_t)std::min<size_t>(v.filePercorsi[j].size() * sizeof(wchar_t), MAX_FILE_PATH_BYTES);
                    appendVal(plain, pathLen);
                    if (pathLen > 0) appendBytes(plain, v.filePercorsi[j].data(), pathLen);
                }
            }
        }

        std::vector<BYTE> enc;
        const bool protOk = dpapiProteggi(plain, enc);
        if (!plain.empty()) SecureZeroMemory(plain.data(), plain.size());
        if (!protOk) return;

        const std::wstring tmp = path + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;

        static constexpr char MAGIA_ENC[4] = {'F', 'W', 'C', 'E'};
        const bool ok = scriviTutto(h, MAGIA_ENC, 4) && scriviTutto(h, enc.data(), enc.size());
        CloseHandle(h);
        if (ok) {
            if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
        } else {
            DeleteFileW(tmp.c_str());
        }
    }

    void caricaFissati() {
        const std::wstring path = percorsoFissati(false);
        if (path.empty()) return;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(h, &fileSize) || fileSize.QuadPart < 4 || (uint64_t)fileSize.QuadPart > MAX_FILE_FISSATI) {
            CloseHandle(h);
            return;
        }
        std::vector<BYTE> raw((size_t)fileSize.QuadPart);
        const bool letto = leggiTutto(h, raw.data(), raw.size());
        CloseHandle(h);
        if (!letto) return;

        std::vector<Voce> caricate;
        bool ok = false;
        bool migra = false;

        if (raw.size() >= 4 && memcmp(raw.data(), "FWCE", 4) == 0) {
            std::vector<BYTE> plain;
            if (dpapiSproteggi(raw.data() + 4, raw.size() - 4, plain)) {
                ok = parseFissatiPayload(plain.data(), plain.size(), caricate);
                if (!plain.empty()) SecureZeroMemory(plain.data(), plain.size());
            }
        } else if (raw.size() >= 4 && memcmp(raw.data(), "FWCB", 4) == 0) {
            ok = parseFissatiPayload(raw.data(), raw.size(), caricate);
            if (!raw.empty()) SecureZeroMemory(raw.data(), raw.size());
            migra = ok;
        }

        if (!ok || caricate.empty()) return;

        for (Voce& v : caricate) v.id = ++ultimoId;
        voci.insert(voci.begin(), std::make_move_iterator(caricate.begin()), std::make_move_iterator(caricate.end()));
        versione++;
        if (migra) salvaFissati();
    }

    bool apri() {
        for (int i = 0; i < 5; i++) {
            if (OpenClipboard(finestra)) return true;
            Sleep(15);
        }
        return false;
    }

    void inserisciNelGruppo(Voce&& v) {
        if (v.fissata) {
            voci.insert(voci.begin(), std::move(v));
        } else {
            auto pos = std::find_if(voci.begin(), voci.end(), [](const Voce& x) { return !x.fissata; });
            voci.insert(pos, std::move(v));
        }
    }

    void pulisciLimite() {
        int nonFissate = 0;
        for (size_t i = 0; i < voci.size();) {
            if (!voci[i].fissata) {
                if (++nonFissate > 20) {
                    voci.erase(voci.begin() + (ptrdiff_t)i);
                    continue;
                }
            }
            i++;
        }
    }

    static std::wstring nomeBaseFile(const std::wstring& path) {
        size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos || pos + 1 >= path.size()) return path;
        return path.substr(pos + 1);
    }

    static std::wstring anteprimaFile(const std::vector<std::wstring>& percorsi) {
        if (percorsi.empty()) return {};
        if (percorsi.size() == 1) return nomeBaseFile(percorsi[0]);
        wchar_t buf[64]{};
        swprintf(buf, 64, testi::t(L"%zu file: ", L"%zu files: "), percorsi.size());
        std::wstring out = buf;
        const size_t mostra = std::min<size_t>(percorsi.size(), 3);
        for (size_t i = 0; i < mostra; i++) {
            if (i > 0) out += L", ";
            out += nomeBaseFile(percorsi[i]);
        }
        if (percorsi.size() > mostra) out += L"…";
        return out;
    }

    static std::wstring normalizzaAnteprima(const std::wstring& s) {
        std::wstring out;
        out.reserve(std::min<size_t>(s.size(), 260));
        bool spazio = false;
        for (wchar_t c : s) {
            if (out.size() >= 260) break;
            if (c == L'\r' || c == L'\n' || c == L'\t' || iswspace(c)) {
                if (!spazio && !out.empty()) out.push_back(L' ');
                spazio = true;
            } else {
                out.push_back(c);
                spazio = false;
            }
        }
        while (!out.empty() && out.back() == L' ') out.pop_back();
        return out;
    }

    static std::wstring descrizioneDaExe(const wchar_t* path) {
        DWORD dummy = 0;
        const DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
        if (!sz || sz > 2 * 1024 * 1024) return {};
        std::vector<BYTE> buf(sz);
        if (!GetFileVersionInfoW(path, 0, sz, buf.data())) return {};
        struct LangCP { WORD lang, cp; } *tr = nullptr;
        UINT trLen = 0;
        std::vector<std::wstring> prefissi;
        if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &trLen) && tr && trLen >= sizeof(LangCP)) {
            wchar_t p[64];
            swprintf(p, 64, L"\\StringFileInfo\\%04x%04x\\", tr[0].lang, tr[0].cp);
            prefissi.push_back(p);
        }
        prefissi.push_back(L"\\StringFileInfo\\040904b0\\");
        prefissi.push_back(L"\\StringFileInfo\\040904e4\\");
        for (const wchar_t* campo : { L"FileDescription", L"ProductName" }) {
            for (const auto& pre : prefissi) {
                wchar_t* val = nullptr; UINT vLen = 0;
                if (VerQueryValueW(buf.data(), (pre + campo).c_str(), (void**)&val, &vLen) && val && vLen > 1) {
                    std::wstring s = trimSpazi(val);
                    if (s.size() >= 2 && s.size() <= 48 && _wcsicmp(s.c_str(), L"Electron") != 0 &&
                        _wcsicmp(s.c_str(), L"Chromium") != 0 && _wcsicmp(s.c_str(), L"Microsoft Windows Operating System") != 0)
                        return s;
                }
            }
        }
        return {};
    }

    static std::wstring nomeExeDaFinestra(HWND hw) {
        if (!hw) return {};
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        if (!pid) return {};
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) return {};
        wchar_t buf[MAX_PATH] = L"";
        DWORD len = MAX_PATH;
        std::wstring nome;
        if (QueryFullProcessImageNameW(hp, 0, buf, &len) && len > 0) {
            const wchar_t* base = wcsrchr(buf, L'\\');
            nome = base ? (base + 1) : buf;
            std::wstring stemMin = nome;
            for (wchar_t& ch : stemMin) ch = (wchar_t)towlower(ch);
            if (stemMin.size() > 4 && stemMin.compare(stemMin.size() - 4, 4, L".exe") == 0) {
                stemMin.resize(stemMin.size() - 4);
                nome.resize(nome.size() - 4);
            }
            static constexpr struct { const wchar_t *k, *v; } ALIAS[] = {
                {L"leagueclientux", L"League of Legends"}, {L"leagueclient", L"League of Legends"},
                {L"leagueclientuxrender", L"League of Legends"}, {L"riotclientux", L"Riot Client"},
                {L"riotclientservices", L"Riot Client"}, {L"steamwebhelper", L"Steam"}, {L"steam", L"Steam"},
                {L"explorer", L"File Explorer"}, {L"windowsterminal", L"Windows Terminal"},
                {L"code", L"VS Code"}, {L"msedge", L"Edge"}, {L"chrome", L"Chrome"}, {L"firefox", L"Firefox"},
                {L"brave", L"Brave"}, {L"opera", L"Opera"}, {L"vivaldi", L"Vivaldi"},
                {L"discord", L"Discord"}, {L"spotify", L"Spotify"}, {L"obsidian", L"Obsidian"},
                {L"1password", L"1Password"}, {L"notepad", L"Notepad"}, {L"winword", L"Word"},
                {L"excel", L"Excel"}, {L"powerpnt", L"PowerPoint"}, {L"olk", L"Outlook"},
                {L"outlook", L"Outlook"}, {L"ms-teams", L"Teams"}, {L"epicgameslauncher", L"Epic Games"},
                {L"upc", L"Ubisoft Connect"}, {L"battle.net", L"Battle.net"}, {L"galaxyclient", L"GOG Galaxy"}
            };
            bool trovato = false;
            for (const auto& a : ALIAS) { if (stemMin == a.k) { nome = a.v; trovato = true; break; } }
            if (!trovato) {
                std::wstring desc = descrizioneDaExe(buf);
                if (!desc.empty()) nome = std::move(desc);
                else if (!nome.empty()) nome[0] = (wchar_t)towupper(nome[0]);
            }
        }
        CloseHandle(hp);
        return nome;
    }

    static std::wstring nomeSorgente() {
        std::wstring s = nomeExeDaFinestra(GetClipboardOwner());
        if (s.empty()) s = nomeExeDaFinestra(GetForegroundWindow());
        return s;
    }

    static bool creaAnteprima(Voce& v) {
        const size_t off = inizioPixelDib(v.dib.data(), v.dib.size());
        if (!off) return false;
        const BITMAPINFOHEADER* bh = (const BITMAPINFOHEADER*)v.dib.data();
        v.w = bh->biWidth;
        v.h = std::abs(bh->biHeight);
        const double k = std::min(1.0, std::min(180.0 / v.h, 320.0 / v.w));
        v.aw = std::max(1, (int)std::lround(v.w * k));
        v.ah = std::max(1, (int)std::lround(v.h * k));
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), v.aw, -v.ah, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
        HBITMAP b = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool ok = false;
        if (b && bits) {
            HGDIOBJ vec = SelectObject(mem, b);
            SetStretchBltMode(mem, HALFTONE);
            SetBrushOrgEx(mem, 0, 0, nullptr);
            ok = StretchDIBits(mem, 0, 0, v.aw, v.ah, 0, 0, v.w, v.h,
                               v.dib.data() + off, (const BITMAPINFO*)bh, DIB_RGB_COLORS, SRCCOPY) > 0;
            GdiFlush();
            if (ok) {
                v.anteprima.assign((UINT*)bits, (UINT*)bits + (size_t)v.aw * v.ah);
                for (UINT& px : v.anteprima) px |= 0xFF000000u;
            }
            SelectObject(mem, vec);
        }
        if (b) DeleteObject(b);
        DeleteDC(mem);
        ReleaseDC(nullptr, scr);
        return ok;
    }

    static bool creaAnteprimaDaFile(const std::wstring& path, Voce& v) {
        if (path.empty()) return false;
        IWICImagingFactory* wic = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* conv = nullptr;

        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (SUCCEEDED(hr)) {
            hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &dec);
        }

        UINT frameIdx = 0;
        if (SUCCEEDED(hr)) {
            const size_t dot = path.find_last_of(L'.');
            if (dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".ico") == 0) {
                UINT count = 0;
                if (SUCCEEDED(dec->GetFrameCount(&count)) && count > 1) {
                    uint64_t maxArea = 0;
                    for (UINT i = 0; i < count; i++) {
                        IWICBitmapFrameDecode* f = nullptr;
                        if (SUCCEEDED(dec->GetFrame(i, &f)) && f) {
                            UINT fw = 0, fh = 0;
                            if (SUCCEEDED(f->GetSize(&fw, &fh))) {
                                const uint64_t area = (uint64_t)fw * (uint64_t)fh;
                                if (area > maxArea) {
                                    maxArea = area;
                                    frameIdx = i;
                                }
                            }
                            f->Release();
                        }
                    }
                }
            }
            hr = dec->GetFrame(frameIdx, &frame);
        }

        UINT srcW = 0, srcH = 0;
        if (SUCCEEDED(hr)) hr = frame->GetSize(&srcW, &srcH);
        if (SUCCEEDED(hr) && (srcW == 0 || srcH == 0 || srcW > 32768 || srcH > 32768)) hr = E_FAIL;

        int aw = 0, ah = 0;
        if (SUCCEEDED(hr)) {
            const double k = std::min(1.0, std::min(180.0 / (double)srcH, 320.0 / (double)srcW));
            aw = std::max(1, (int)std::lround((double)srcW * k));
            ah = std::max(1, (int)std::lround((double)srcH * k));
        }

        IWICBitmapSource* src = nullptr;
        if (SUCCEEDED(hr)) {
            if ((UINT)aw != srcW || (UINT)ah != srcH) {
                hr = wic->CreateBitmapScaler(&scaler);
                if (SUCCEEDED(hr)) hr = scaler->Initialize(frame, (UINT)aw, (UINT)ah, WICBitmapInterpolationModeFant);
                if (SUCCEEDED(hr)) src = scaler;
            } else {
                src = frame;
            }
        }

        if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr)) {
            hr = conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }

        std::vector<UINT> px;
        if (SUCCEEDED(hr)) {
            px.resize((size_t)aw * (size_t)ah);
            const UINT stride = (UINT)aw * 4u;
            const UINT bufSize = (UINT)(px.size() * sizeof(UINT));
            hr = conv->CopyPixels(nullptr, stride, bufSize, reinterpret_cast<BYTE*>(px.data()));
        }

        if (conv) conv->Release();
        if (scaler) scaler->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (wic) wic->Release();

        if (SUCCEEDED(hr) && !px.empty()) {
            v.w = (int)srcW;
            v.h = (int)srcH;
            v.aw = aw;
            v.ah = ah;
            v.anteprima = std::move(px);
            return true;
        }
        return false;
    }
};

inline bool salvaDibComePng(const std::vector<BYTE>& dib, const std::wstring& path) {
    if (dib.empty() || path.empty()) return false;
    const size_t off = Cronologia::inizioPixelDib(dib.data(), dib.size());
    if (!off) return false;
    BITMAPINFOHEADER bh{};
    memcpy(&bh, dib.data(), sizeof(bh));
    const int w = bh.biWidth;
    const int h = std::abs(bh.biHeight);
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > 64ULL * 1024 * 1024) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<UINT> bgra;
    bool dibOk = false;
    HDC scr = GetDC(nullptr);
    HDC mem = scr ? CreateCompatibleDC(scr) : nullptr;
    void* bits = nullptr;
    HBITMAP bmp = (scr && mem) ? CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bmp && bits) {
        HGDIOBJ vec = SelectObject(mem, bmp);
        dibOk = StretchDIBits(mem, 0, 0, w, h, 0, 0, w, h,
                              dib.data() + off, reinterpret_cast<const BITMAPINFO*>(dib.data()),
                              DIB_RGB_COLORS, SRCCOPY) > 0;
        GdiFlush();
        if (dibOk) {
            const size_t count = (size_t)w * (size_t)h;
            bgra.assign(static_cast<const UINT*>(bits), static_cast<const UINT*>(bits) + count);
        }
        SelectObject(mem, vec);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    if (scr) ReleaseDC(nullptr, scr);
    if (!dibOk || bgra.empty()) return false;

    bool haAlfa = false;
    for (UINT px : bgra) {
        if ((px & 0xFF000000u) != 0) {
            haAlfa = true;
            break;
        }
    }

    IWICImagingFactory* wic = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)w, (UINT)h);

    WICPixelFormatGUID fmt = haAlfa ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);

    if (SUCCEEDED(hr)) {
        if (IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR)) {
            const UINT stride = (UINT)w * 3u;
            std::vector<BYTE> bgr((size_t)stride * (size_t)h);
            BYTE* dst = bgr.data();
            for (UINT px : bgra) {
                dst[0] = (BYTE)(px & 0xFFu);
                dst[1] = (BYTE)((px >> 8) & 0xFFu);
                dst[2] = (BYTE)((px >> 16) & 0xFFu);
                dst += 3;
            }
            hr = frame->WritePixels((UINT)h, stride, (UINT)bgr.size(), bgr.data());
        } else if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
            if (!haAlfa) {
                for (UINT& px : bgra) px |= 0xFF000000u;
            }
            const UINT stride = (UINT)w * 4u;
            hr = frame->WritePixels((UINT)h, stride, (UINT)(bgra.size() * sizeof(UINT)),
                                    reinterpret_cast<BYTE*>(bgra.data()));
        } else {
            hr = E_FAIL;
        }
    }

    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();

    if (props) props->Release();
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (wic) wic->Release();
    return SUCCEEDED(hr);
}

// ---- Finestra di vetro degli appunti (Win+V) ----
class Appunti {
public:
    ~Appunti() { distruggi(); }

    void cattura() {
        if (Cronologia::io().cambiato() && aperta()) {
            aggiornaFiltro();
            disegna();
        }
    }

    bool aperta() const { return stato == 1 || stato == 2; }

    void alterna() {
        if (aperta()) chiudi();
        else apri();
    }

    void apri() {
        if (!v.fin && !v.crea(L"FluidWin.Appunti", vetro::procedura<Appunti>, this, true, 2, true, true)) return;
        HWND fg = GetForegroundWindow();
        if (fg && fg != v.fin) precedente = fg;

        v.aggiornaTema();
        POINT pt{};
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) {
            mi.rcWork = vetro::areaPrincipale(true);
        }
        v.dpi = vetro::dpiMonitor(mon);
        const float d = v.dpi;
        assicuraFormati(d);

        const int margine = (int)std::lround(12 * d);
        const int workW = (int)(mi.rcWork.right - mi.rcWork.left);
        const int workH = (int)(mi.rcWork.bottom - mi.rcWork.top);
        destW = std::min((int)std::lround(384 * d), std::max(240, workW - margine * 2));
        destH = std::min((int)std::lround(484 * d), std::max(260, workH - margine * 2));

        int x = pt.x - destW / 2;
        int y = pt.y - (int)std::lround(28 * d);
        const int minX = (int)mi.rcWork.left + margine;
        const int maxX = std::max(minX, (int)mi.rcWork.right - margine - destW);
        const int minY = (int)mi.rcWork.top + margine;
        const int maxY = std::max(minY, (int)mi.rcWork.bottom - margine - destH);
        destX = std::clamp(x, minX, maxX);
        destY = std::clamp(y, minY, maxY);

        ricerca.clear();
        filtro = F_TUTTI;
        sopraFiltro = -1;
        scrollY = 0;
        sel = 0;
        sopra = -1;
        sopraAzione = 0;
        sopraSvuota = false;
        menuAperto = false;
        aggiornaFiltro();

        KillTimer(v.fin, TIMER_INCOLLA);
        stato = 1;
        tAnim = vetro::ms();
        const int offsetY = (int)std::lround(10 * d);
        v.posiziona(destX, destY + offsetY, destW, destH, HWND_TOPMOST, SWP_SHOWWINDOW);
        v.alfa(0);
        prendiFuoco();
        vetro::battito(v.fin, 1);
        disegna();
    }

    void chiudi(bool anima = true) {
        if (stato == 0 || (stato == 3 && anima)) return;
        if (!anima) {
            vetro::fermaBattito(v.fin, 1);
            stato = 0;
            if (v.fin) ShowWindow(v.fin, SW_HIDE);
            return;
        }
        stato = 3;
        tAnim = vetro::ms();
        vetro::battito(v.fin, 1);
    }

    void distruggi() {
        if (v.fin) {
            vetro::fermaBattito(v.fin, 1);
            KillTimer(v.fin, TIMER_INCOLLA);
        }
        liberaBitmap();
        liberaFormati();
        v.chiudi();
        stato = 0;
    }

    bool messaggio(HWND hw, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
        r = 0;
        switch (m) {
            case WM_TIMER:
                if (w == 1) { passoAnimazione(); return true; }
                if (w == TIMER_INCOLLA) {
                    KillTimer(hw, TIMER_INCOLLA);
                    inviaCtrlV();
                    return true;
                }
                return false;

            case WM_PAINT:
                disegna();
                return true;

            case WM_ACTIVATE:
                if (LOWORD(w) == WA_INACTIVE && aperta() && !menuAperto) {
                    chiudi(true);
                }
                return true;

            case WM_CLOSE:
                chiudi(true);
                return true;

            case WM_KEYDOWN:
                if (!aperta()) return true;
                if (w == VK_ESCAPE) {
                    chiudi(true);
                    return true;
                }
                if (w == VK_UP) {
                    if (!filtrate.empty()) {
                        sel = std::max(0, sel - 1);
                        assicuraVisibile(sel);
                        disegna();
                    }
                    return true;
                }
                if (w == VK_DOWN) {
                    if (!filtrate.empty()) {
                        sel = std::min((int)filtrate.size() - 1, sel + 1);
                        assicuraVisibile(sel);
                        disegna();
                    }
                    return true;
                }
                if (w == VK_RETURN) {
                    if (sel >= 0 && sel < (int)filtrate.size()) {
                        incolla(filtrate[sel], false);
                    }
                    return true;
                }
                if (w == VK_BACK) {
                    if (!ricerca.empty()) {
                        if (GetKeyState(VK_CONTROL) & 0x8000) ricerca.clear();
                        else ricerca.pop_back();
                        aggiornaFiltro();
                        disegna();
                    }
                    return true;
                }
                return false;

            case WM_CHAR: {
                if (!aperta()) return true;
                if (w < 32 || w == 127 || (GetKeyState(VK_CONTROL) & 0x8000)) return true;
                if (ricerca.size() < 128) {
                    ricerca.push_back((wchar_t)w);
                    aggiornaFiltro();
                    disegna();
                }
                return true;
            }

            case WM_MOUSEWHEEL: {
                if (!aperta()) return true;
                const int delta = GET_WHEEL_DELTA_WPARAM(w);
                const float passo = 42.0f * v.dpi;
                scrollY = std::clamp(scrollY - (delta / 120.0f) * passo, 0.0f, maxScroll());
                aggiornaHover(ultimoMouseX, ultimoMouseY);
                disegna();
                return true;
            }

            case WM_MOUSEMOVE: {
                if (!aperta()) return true;
                ultimoMouseX = (float)GET_X_LPARAM(l);
                ultimoMouseY = (float)GET_Y_LPARAM(l);
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hw, 0};
                TrackMouseEvent(&tme);
                if (aggiornaHover(ultimoMouseX, ultimoMouseY)) disegna();
                return true;
            }

            case WM_MOUSELEAVE:
                if (sopra != -1 || sopraAzione != 0 || sopraSvuota || sopraFiltro != -1) {
                    sopra = -1;
                    sopraAzione = 0;
                    sopraSvuota = false;
                    sopraFiltro = -1;
                    disegna();
                }
                return true;

            case WM_LBUTTONUP: {
                if (!aperta()) return true;
                const float mx = (float)GET_X_LPARAM(l);
                const float my = (float)GET_Y_LPARAM(l);
                if (puntoIn(rettSvuota(), mx, my) && haNonFissate()) {
                    Cronologia::io().svuotaNonFissate();
                    aggiornaFiltro();
                    disegna();
                    return true;
                }
                for (int i = 0; i < 4; i++) {
                    if (puntoIn(rettFiltro(i), mx, my)) {
                        if (filtro != i) {
                            filtro = i;
                            scrollY = 0.0f;
                            aggiornaFiltro();
                            disegna();
                        }
                        return true;
                    }
                }
                int idx = -1, az = 0;
                if (colpisceLista(mx, my, idx, az) && idx >= 0 && idx < (int)filtrate.size()) {
                    const UINT id = filtrate[idx];
                    if (az == 1) {
                        Cronologia::io().fissa(id);
                        aggiornaFiltro();
                        disegna();
                    } else if (az == 2) {
                        Cronologia::io().togli(id);
                        aggiornaFiltro();
                        disegna();
                    } else {
                        incolla(id, false);
                    }
                }
                return true;
            }

            case WM_RBUTTONUP: {
                if (!aperta()) return true;
                const float mx = (float)GET_X_LPARAM(l);
                const float my = (float)GET_Y_LPARAM(l);
                int idx = -1, az = 0;
                if (!colpisceLista(mx, my, idx, az) || idx < 0 || idx >= (int)filtrate.size()) return true;
                const UINT id = filtrate[idx];
                const Voce* voce = trovaVoce(id);
                if (!voce) return true;

                sel = idx;
                disegna();

                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ClientToScreen(hw, &pt);
                std::vector<menu::Voce> vociMenu = {
                    menu::voce(testi::t(L"Incolla", L"Paste"), 1),
                    menu::voce(testi::t(L"Copia soltanto", L"Copy only"), 2),
                    menu::voce(voce->fissata ? testi::t(L"Sblocca", L"Unpin") : testi::t(L"Fissa", L"Pin"), 3),
                };
                if (!voce->immagine && !voce->eFile && eUrl(voce->testo)) {
                    vociMenu.push_back(menu::voce(testi::t(L"Apri nel browser", L"Open in browser"), 5));
                }
                if (voce->immagine) {
                    vociMenu.push_back(menu::voce(testi::t(L"Salva come PNG…", L"Save as PNG…"), 6));
                }
                if (voce->eFile && !voce->filePercorsi.empty()) {
                    vociMenu.push_back(menu::voce(testi::t(L"Copia percorso", L"Copy path"), 7));
                    vociMenu.push_back(menu::voce(testi::t(L"Apri percorso file", L"Open file location"), 8));
                }
                vociMenu.push_back(menu::separatore());
                vociMenu.push_back(menu::voce(testi::t(L"Elimina", L"Delete"), 4));

                menuAperto = true;
                const UINT scelta = menu::mostra(vociMenu, pt, hw);
                menuAperto = false;

                if (scelta == 1) {
                    incolla(id, false);
                } else if (scelta == 2) {
                    incolla(id, true);
                } else if (scelta == 3) {
                    Cronologia::io().fissa(id);
                    aggiornaFiltro();
                    SetForegroundWindow(hw);
                    disegna();
                } else if (scelta == 4) {
                    Cronologia::io().togli(id);
                    aggiornaFiltro();
                    SetForegroundWindow(hw);
                    disegna();
                } else if (scelta == 5) {
                    const Voce* vCur = trovaVoce(id);
                    if (vCur && !vCur->immagine && !vCur->eFile && eUrl(vCur->testo)) {
                        const std::wstring url = trimSpazi(vCur->testo);
                        chiudi(false);
                        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                } else if (scelta == 6) {
                    const Voce* vCur = trovaVoce(id);
                    if (vCur && vCur->immagine && !vCur->dib.empty()) {
                        std::vector<BYTE> dibCopia = vCur->dib;
                        wchar_t filePath[MAX_PATH] = L"clipboard.png";
                        OPENFILENAMEW ofn{};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hw;
                        ofn.lpstrFilter = L"PNG (*.png)\0*.png\0";
                        ofn.lpstrFile = filePath;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.lpstrDefExt = L"png";
                        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                        menuAperto = true;
                        const BOOL okSalva = GetSaveFileNameW(&ofn);
                        menuAperto = false;
                        if (okSalva) salvaDibComePng(dibCopia, filePath);
                    }
                    if (aperta() && GetForegroundWindow() != hw) {
                        chiudi(true);
                    }
                } else if (scelta == 7) {
                    const Voce* vCur = trovaVoce(id);
                    if (vCur && vCur->eFile && !vCur->testo.empty()) {
                        Cronologia::io().copiaTestoSemplice(vCur->testo);
                        chiudi(false);
                    }
                } else if (scelta == 8) {
                    const Voce* vCur = trovaVoce(id);
                    if (vCur && vCur->eFile && !vCur->filePercorsi.empty()) {
                        const std::wstring arg = L"/select,\"" + vCur->filePercorsi[0] + L"\"";
                        chiudi(false);
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                } else if (aperta() && GetForegroundWindow() != hw) {
                    chiudi(true);
                }
                return true;
            }
        }
        return false;
    }

private:
    enum Filtro { F_TUTTI = 0, F_FISSATI = 1, F_IMMAGINI = 2, F_LINK = 3 };

    static constexpr UINT TIMER_INCOLLA = 2;
    static constexpr double DURATA_APRI_MS = 160.0;
    static constexpr double DURATA_CHIUDI_MS = 120.0;

    vetro::Finestra v;
    IDWriteTextFormat *fTitolo = nullptr, *fTesto = nullptr, *fMeta = nullptr, *fPiccolo = nullptr, *fVuoto = nullptr;
    float fDpi = 0;

    std::unordered_map<UINT, ID2D1Bitmap*> bmpCache;
    unsigned bmpGiro = 0;

    HWND precedente = nullptr;
    int stato = 0;   // 0 chiusa, 1 apertura, 2 aperta, 3 chiusura
    ULONGLONG tAnim = 0;
    int destX = 0, destY = 0, destW = 384, destH = 484;

    std::wstring ricerca;
    int filtro = F_TUTTI;
    int sopraFiltro = -1;
    std::vector<UINT> filtrate;
    float scrollY = 0;
    int sel = 0;
    int sopra = -1;
    int sopraAzione = 0;   // 0 scheda, 1 fissa, 2 elimina
    bool sopraSvuota = false;
    bool menuAperto = false;
    float ultimoMouseX = -1, ultimoMouseY = -1;

    void liberaBitmap() {
        for (auto& kv : bmpCache) {
            if (kv.second) kv.second->Release();
        }
        bmpCache.clear();
    }

    void liberaFormati() {
        for (IDWriteTextFormat** f : {&fTitolo, &fTesto, &fMeta, &fPiccolo, &fVuoto}) {
            if (*f) {
                (*f)->Release();
                *f = nullptr;
            }
        }
        fDpi = 0;
    }

    void assicuraFormati(float d) {
        if (fTitolo && std::abs(fDpi - d) < 0.01f) return;
        liberaFormati();
        fDpi = d;
        fTitolo = v.formato(16.0f * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        fTesto = v.formato(13.0f * d, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        fMeta = v.formato(11.0f * d, DWRITE_FONT_WEIGHT_NORMAL);
        fPiccolo = v.formato(12.0f * d, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
        fVuoto = v.formato(13.0f * d, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }

    void prendiFuoco() {
        HWND fg = GetForegroundWindow();
        DWORD cur = GetCurrentThreadId();
        DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        bool att = (tid && tid != cur) ? (AttachThreadInput(cur, tid, TRUE) != FALSE) : false;
        SetForegroundWindow(v.fin);
        SetActiveWindow(v.fin);
        SetFocus(v.fin);
        if (att) AttachThreadInput(cur, tid, FALSE);
    }

    void passoAnimazione() {
        const double dt = (double)(vetro::ms() - tAnim);
        const float d = v.dpi;
        if (stato == 1) {
            const double p = std::clamp(dt / DURATA_APRI_MS, 0.0, 1.0);
            const double k = vetro::uscita(p);
            const int oy = (int)std::lround((1.0 - k) * 10.0 * d);
            SetWindowPos(v.fin, HWND_TOPMOST, destX, destY + oy, destW, destH, SWP_NOACTIVATE);
            v.alfa(k);
            disegna();
            if (p >= 1.0) {
                stato = 2;
                v.alfa(1.0);
                vetro::fermaBattito(v.fin, 1);
            }
        } else if (stato == 3) {
            const double p = std::clamp(dt / DURATA_CHIUDI_MS, 0.0, 1.0);
            const double k = vetro::uscita(p);
            const int oy = (int)std::lround(k * 8.0 * d);
            SetWindowPos(v.fin, HWND_TOPMOST, destX, destY + oy, destW, destH, SWP_NOACTIVATE);
            v.alfa(1.0 - k);
            if (p >= 1.0) {
                stato = 0;
                vetro::fermaBattito(v.fin, 1);
                ShowWindow(v.fin, SW_HIDE);
            }
        } else {
            vetro::fermaBattito(v.fin, 1);
        }
    }

    static bool contieneSenzaMaiusc(const std::wstring& testo, const std::wstring& ago) {
        if (ago.empty()) return true;
        auto it = std::search(
            testo.begin(), testo.end(),
            ago.begin(), ago.end(),
            [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }
        );
        return it != testo.end();
    }

    static const Voce* trovaVoce(UINT id) {
        const auto& voci = Cronologia::io().voci;
        for (const auto& vc : voci) {
            if (vc.id == id) return &vc;
        }
        return nullptr;
    }

    static bool haNonFissate() {
        for (const auto& vc : Cronologia::io().voci) {
            if (!vc.fissata) return true;
        }
        return false;
    }

    void aggiornaFiltro() {
        filtrate.clear();
        const auto& voci = Cronologia::io().voci;
        for (const auto& vc : voci) {
            if (filtro == F_FISSATI && !vc.fissata) continue;
            if (filtro == F_IMMAGINI && !haAnteprimaImmagine(vc)) continue;
            if (filtro == F_LINK && (vc.immagine || vc.eFile || !eUrl(vc.testo))) continue;
            if (ricerca.empty()) {
                filtrate.push_back(vc.id);
            } else {
                const bool matchTesto = !vc.immagine && (contieneSenzaMaiusc(vc.testo, ricerca) || contieneSenzaMaiusc(vc.anteprimaTesto, ricerca));
                const bool matchApp = contieneSenzaMaiusc(vc.sorgente, ricerca);
                const bool matchImg = haAnteprimaImmagine(vc) && (contieneSenzaMaiusc(L"image", ricerca) || contieneSenzaMaiusc(L"immagine", ricerca));
                const bool matchFile = vc.eFile && contieneSenzaMaiusc(L"file", ricerca);
                if (matchTesto || matchApp || matchImg || matchFile) filtrate.push_back(vc.id);
            }
        }
        if (filtrate.empty()) sel = 0;
        else sel = std::clamp(sel, 0, (int)filtrate.size() - 1);
        scrollY = std::clamp(scrollY, 0.0f, maxScroll());
    }

    D2D1_RECT_F rettSvuota() const {
        const float d = v.dpi;
        const float pw = 66.0f * d, ph = 26.0f * d;
        const float right = (float)v.W - 14.0f * d;
        const float top = (48.0f * d - ph) * 0.5f;
        return {right - pw, top, right, top + ph};
    }

    D2D1_RECT_F rettRicerca() const {
        const float d = v.dpi;
        return {12.0f * d, 48.0f * d, (float)v.W - 12.0f * d, 84.0f * d};
    }

    D2D1_RECT_F rettFiltro(int idx) const {
        const float d = v.dpi;
        const float gap = 6.0f * d;
        const float pw = ((float)v.W - 24.0f * d - gap * 3.0f) * 0.25f;
        const float left = 12.0f * d + idx * (pw + gap);
        return {left, 90.0f * d, left + pw, 116.0f * d};
    }

    D2D1_RECT_F rettLista() const {
        const float d = v.dpi;
        return {0.0f, 124.0f * d, (float)v.W, (float)v.H - 8.0f * d};
    }

    float altezzaScheda(const Voce& vc) const {
        return (haAnteprimaImmagine(vc) ? 108.0f : 78.0f) * v.dpi;
    }

    float altezzaTotaleLista() const {
        const float gap = 6.0f * v.dpi;
        float tot = 0.0f;
        for (size_t i = 0; i < filtrate.size(); i++) {
            const Voce* vc = trovaVoce(filtrate[i]);
            if (!vc) continue;
            tot += altezzaScheda(*vc);
            if (i + 1 < filtrate.size()) tot += gap;
        }
        return tot;
    }

    float maxScroll() const {
        const D2D1_RECT_F area = rettLista();
        const float vis = std::max(0.0f, area.bottom - area.top);
        return std::max(0.0f, altezzaTotaleLista() - vis);
    }

    void assicuraVisibile(int idx) {
        if (idx < 0 || idx >= (int)filtrate.size()) return;
        const float gap = 6.0f * v.dpi;
        float y = 0.0f, h = 0.0f;
        for (int i = 0; i <= idx; i++) {
            const Voce* vc = trovaVoce(filtrate[i]);
            h = vc ? altezzaScheda(*vc) : 78.0f * v.dpi;
            if (i < idx) y += h + gap;
        }
        const D2D1_RECT_F area = rettLista();
        const float vis = std::max(1.0f, area.bottom - area.top);
        if (y < scrollY) scrollY = y;
        else if (y + h > scrollY + vis) scrollY = y + h - vis;
        scrollY = std::clamp(scrollY, 0.0f, maxScroll());
    }

    static bool puntoIn(const D2D1_RECT_F& r, float x, float y) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }

    void rettPulsantiScheda(const D2D1_RECT_F& card, D2D1_RECT_F& rPin, D2D1_RECT_F& rDel) const {
        const float d = v.dpi;
        const float s = 24.0f * d;
        const float top = card.top + 8.0f * d;
        rDel = {card.right - 8.0f * d - s, top, card.right - 8.0f * d, top + s};
        rPin = {rDel.left - 4.0f * d - s, top, rDel.left - 4.0f * d, top + s};
    }

    bool colpisceLista(float mx, float my, int& outIdx, int& outAzione) const {
        outIdx = -1;
        outAzione = 0;
        const D2D1_RECT_F area = rettLista();
        if (!puntoIn(area, mx, my)) return false;

        const float d = v.dpi;
        const float gap = 6.0f * d;
        float y = area.top - scrollY;
        for (int i = 0; i < (int)filtrate.size(); i++) {
            const Voce* vc = trovaVoce(filtrate[i]);
            if (!vc) continue;
            const float h = altezzaScheda(*vc);
            const D2D1_RECT_F card = {12.0f * d, y, (float)v.W - 12.0f * d, y + h};
            if (puntoIn(card, mx, my)) {
                outIdx = i;
                D2D1_RECT_F rPin{}, rDel{};
                rettPulsantiScheda(card, rPin, rDel);
                if (puntoIn(rPin, mx, my)) outAzione = 1;
                else if (puntoIn(rDel, mx, my)) outAzione = 2;
                return true;
            }
            y += h + gap;
        }
        return false;
    }

    bool aggiornaHover(float mx, float my) {
        const bool sv = puntoIn(rettSvuota(), mx, my);
        int fi = -1;
        for (int i = 0; i < 4; i++) {
            if (puntoIn(rettFiltro(i), mx, my)) {
                fi = i;
                break;
            }
        }
        int idx = -1, az = 0;
        colpisceLista(mx, my, idx, az);
        const bool cambiato = (sv != sopraSvuota) || (fi != sopraFiltro) || (idx != sopra) || (az != sopraAzione);
        sopraSvuota = sv;
        sopraFiltro = fi;
        sopra = idx;
        sopraAzione = az;
        return cambiato;
    }

    void incolla(UINT id, bool soloCopia = false) {
        if (!Cronologia::io().ricopia(id, true)) return;
        chiudi(false);
        if (!soloCopia && !(GetKeyState(VK_SHIFT) & 0x8000) && precedente && IsWindow(precedente)) {
            SetForegroundWindow(precedente);
            SetTimer(v.fin, TIMER_INCOLLA, 45, nullptr);
        }
    }

    static void inviaCtrlV() {
        INPUT in[4]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_CONTROL;
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = 'V';
        in[2].type = INPUT_KEYBOARD;
        in[2].ki.wVk = 'V';
        in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD;
        in[3].ki.wVk = VK_CONTROL;
        in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
    }

    static std::wstring tempoRelativo(INT64 quando) {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        const INT64 ora = (INT64)(((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime);
        const ULONGLONG sec = (ora > quando && quando > 0) ? (ULONGLONG)((ora - quando) / 10000000LL) : 0ULL;
        if (sec < 60) return testi::t(L"Adesso", L"Just now");
        wchar_t buf[32]{};
        if (sec < 3600) {
            swprintf(buf, 32, testi::t(L"%llu min fa", L"%llum ago"), (unsigned long long)(sec / 60));
            return buf;
        }
        if (sec < 86400) {
            swprintf(buf, 32, testi::t(L"%llu h fa", L"%lluh ago"), (unsigned long long)(sec / 3600));
            return buf;
        }
        swprintf(buf, 32, testi::t(L"%llu g fa", L"%llud ago"), (unsigned long long)(sec / 86400));
        return buf;
    }

    ID2D1Bitmap* ottieniBitmap(const Voce& vc) {
        if (!v.rt || vc.anteprima.empty() || vc.aw <= 0 || vc.ah <= 0) return nullptr;
        auto it = bmpCache.find(vc.id);
        if (it != bmpCache.end()) return it->second;
        ID2D1Bitmap* bmp = nullptr;
        const HRESULT hr = v.rt->CreateBitmap(
            D2D1::SizeU((UINT32)vc.aw, (UINT32)vc.ah),
            vc.anteprima.data(),
            (UINT32)(vc.aw * 4),
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &bmp
        );
        if (SUCCEEDED(hr) && bmp) {
            bmpCache[vc.id] = bmp;
            return bmp;
        }
        return nullptr;
    }

    void disegnaPuntina(float cx, float cy, float d, bool piena, D2D1_COLOR_F col) {
        if (!v.f2d || !v.rt) return;
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(v.f2d->CreatePathGeometry(&geo)) || !geo) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(geo->Open(&sink)) && sink) {
            // piccola puntina inclinata di 45 gradi
            sink->BeginFigure({cx - 1.5f * d, cy - 5.0f * d}, piena ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine({cx + 4.5f * d, cy + 1.0f * d});
            sink->AddLine({cx + 1.5f * d, cy + 2.5f * d});
            sink->AddLine({cx - 0.5f * d, cy + 0.5f * d});
            sink->AddLine({cx - 3.0f * d, cy - 2.0f * d});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            v.pen->SetColor(col);
            if (piena) v.rt->FillGeometry(geo, v.pen);
            else v.rt->DrawGeometry(geo, v.pen, 1.3f * d);
            v.rt->DrawLine({cx - 1.0f * d, cy + 1.0f * d}, {cx - 4.5f * d, cy + 4.5f * d}, v.pen, 1.4f * d);
        }
        geo->Release();
    }

    void disegna() {
        if (!v.rt || !v.fin) return;
        if (v.giroRt != bmpGiro) {
            liberaBitmap();
            bmpGiro = v.giroRt;
        }
        // pulizia delle bitmap di voci eliminate
        const auto& tutte = Cronologia::io().voci;
        for (auto it = bmpCache.begin(); it != bmpCache.end();) {
            const bool esiste = std::any_of(tutte.begin(), tutte.end(), [&](const Voce& vc) { return vc.id == it->first; });
            if (!esiste) {
                if (it->second) it->second->Release();
                it = bmpCache.erase(it);
            } else {
                ++it;
            }
        }

        if (!v.inizia(1.0f)) return;
        const float d = v.dpi;
        const bool sc = v.scuro;
        const D2D1_COLOR_F ink = v.inchiostro(1.0f);
        const D2D1_COLOR_F muted = v.inchiostro(0.55f);

        // riflesso speculare in cima (Liquid Glass)
        {
            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f)},
                {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)}
            };
            ID2D1GradientStopCollection* gsc = nullptr;
            if (SUCCEEDED(v.rt->CreateGradientStopCollection(stops, 2, &gsc)) && gsc) {
                ID2D1LinearGradientBrush* sheen = nullptr;
                if (SUCCEEDED(v.rt->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties({0.0f, 0.0f}, {0.0f, 56.0f * d}), gsc, &sheen)) && sheen) {
                    v.rt->FillRectangle({0.0f, 0.0f, (float)v.W, 56.0f * d}, sheen);
                    sheen->Release();
                }
                gsc->Release();
            }
        }

        // intestazione (48 * dpi)
        {
            const std::wstring titolo = testi::t(L"Appunti", L"Clipboard");
            const D2D1_RECT_F rTit = {16.0f * d, 0.0f, (float)v.W - 100.0f * d, 48.0f * d};
            v.testo(titolo, fTitolo, rTit, ink);

            const float tw = v.larghezza(titolo, fTitolo);
            wchar_t cnt[24]{};
            swprintf(cnt, 24, L"%zu", filtrate.size());
            const D2D1_RECT_F rCnt = {16.0f * d + tw + 8.0f * d, 2.0f * d, (float)v.W - 92.0f * d, 48.0f * d};
            v.testo(cnt, fMeta, rCnt, muted);

            const D2D1_RECT_F rSv = rettSvuota();
            const bool abil = haNonFissate();
            const D2D1_COLOR_F bgSv = !abil
                ? D2D1::ColorF(sc ? 1.0f : 0.0f, sc ? 1.0f : 0.0f, sc ? 1.0f : 0.0f, 0.04f)
                : sopraSvuota
                    ? (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f))
                    : (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f));
            v.pen->SetColor(bgSv);
            v.rt->FillRoundedRectangle({rSv, 13.0f * d, 13.0f * d}, v.pen);
            v.testo(testi::t(L"Svuota", L"Clear"), fPiccolo, rSv, abil ? ink : v.inchiostro(0.32f));
        }

        // barra di ricerca (36 * dpi)
        {
            const D2D1_RECT_F rRic = rettRicerca();
            v.pen->SetColor(sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f));
            v.rt->FillRoundedRectangle({rRic, 8.0f * d, 8.0f * d}, v.pen);
            v.pen->SetColor(sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f));
            v.rt->DrawRoundedRectangle({rRic, 8.0f * d, 8.0f * d}, v.pen, 1.0f);

            // lente di ricerca
            const float lx = rRic.left + 15.0f * d;
            const float ly = (rRic.top + rRic.bottom) * 0.5f - 1.0f * d;
            v.pen->SetColor(muted);
            v.rt->DrawEllipse({{lx, ly}, 4.8f * d, 4.8f * d}, v.pen, 1.4f * d);
            v.rt->DrawLine({lx + 3.4f * d, ly + 3.4f * d}, {lx + 6.8f * d, ly + 6.8f * d}, v.pen, 1.5f * d);

            const D2D1_RECT_F rTesto = {rRic.left + 30.0f * d, rRic.top, rRic.right - 10.0f * d, rRic.bottom};
            if (ricerca.empty()) {
                v.testo(testi::t(L"Cerca negli appunti…", L"Search clipboard…"), fMeta, rTesto, muted);
            } else {
                v.testo(ricerca, fMeta, rTesto, ink);
            }
        }

        // barra filtri (pillole Liquid Glass)
        {
            const wchar_t* etichette[4] = {
                testi::t(L"Tutti", L"All"),
                testi::t(L"Fissati", L"Pinned"),
                testi::t(L"Immagini", L"Images"),
                testi::t(L"Link", L"Links")
            };
            for (int i = 0; i < 4; i++) {
                const D2D1_RECT_F rp = rettFiltro(i);
                const bool attivo = (filtro == i);
                const bool hov = (sopraFiltro == i);
                const D2D1_COLOR_F bg = attivo
                    ? (sc ? D2D1::ColorF(0.32f, 0.58f, 0.96f, 0.28f) : D2D1::ColorF(0.0f, 0.45f, 0.88f, 0.16f))
                    : hov
                        ? (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f))
                        : (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.04f));
                const D2D1_COLOR_F bd = attivo
                    ? (sc ? D2D1::ColorF(0.45f, 0.72f, 1.0f, 0.52f) : D2D1::ColorF(0.0f, 0.45f, 0.88f, 0.38f))
                    : (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.07f));
                v.pen->SetColor(bg);
                v.rt->FillRoundedRectangle({rp, 13.0f * d, 13.0f * d}, v.pen);
                v.pen->SetColor(bd);
                v.rt->DrawRoundedRectangle({rp, 13.0f * d, 13.0f * d}, v.pen, 1.0f);
                v.testo(etichette[i], fPiccolo, rp, attivo ? ink : (hov ? v.inchiostro(0.85f) : muted));
            }
        }

        // lista scorrevole delle schede
        const D2D1_RECT_F area = rettLista();
        v.rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (filtrate.empty()) {
            const std::wstring msg = Cronologia::io().voci.empty()
                ? testi::t(L"Nessun elemento negli appunti", L"Clipboard history is empty")
                : testi::t(L"Nessun risultato", L"No matches");
            const D2D1_RECT_F rMsg = {24.0f * d, area.top + (area.bottom - area.top) * 0.38f, (float)v.W - 24.0f * d, area.bottom};
            v.testo(msg, fVuoto, rMsg, muted);
        } else {
            const float gap = 6.0f * d;
            float y = area.top - scrollY;
            for (int i = 0; i < (int)filtrate.size(); i++) {
                const Voce* vc = trovaVoce(filtrate[i]);
                if (!vc) continue;
                const float h = altezzaScheda(*vc);
                const D2D1_RECT_F card = {12.0f * d, y, (float)v.W - 12.0f * d, y + h};

                if (card.bottom >= area.top && card.top <= area.bottom) {
                    const bool isSel = (i == sel);
                    const bool isHov = (i == sopra);
                    const D2D1_COLOR_F fill = isHov
                        ? (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.075f))
                        : isSel
                            ? (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.095f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.055f))
                            : (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f));
                    v.pen->SetColor(fill);
                    v.rt->FillRoundedRectangle({card, 10.0f * d, 10.0f * d}, v.pen);

                    const D2D1_COLOR_F bordo = isSel
                        ? (sc ? D2D1::ColorF(0.38f, 0.68f, 1.0f, 0.55f) : D2D1::ColorF(0.0f, 0.45f, 0.88f, 0.45f))
                        : (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.07f));
                    v.pen->SetColor(bordo);
                    v.rt->DrawRoundedRectangle({card, 10.0f * d, 10.0f * d}, v.pen, 1.0f);

                    const float pad = 12.0f * d;
                    const float rightLimit = (isHov || vc->fissata) ? (card.right - 66.0f * d) : (card.right - pad);

                    std::wstring meta;
                    if (!vc->sorgente.empty()) meta = vc->sorgente + L" · ";
                    meta += tempoRelativo(vc->quando);

                    if (haAnteprimaImmagine(*vc)) {
                        const float maxW = 96.0f * d, maxH = 84.0f * d;
                        const float scala = std::min(maxW / std::max(1, vc->aw), maxH / std::max(1, vc->ah));
                        const float iw = std::max(12.0f * d, vc->aw * scala);
                        const float ih = std::max(12.0f * d, vc->ah * scala);
                        const float ix = card.left + pad;
                        const float iy = card.top + (h - ih) * 0.5f;
                        const D2D1_RECT_F rImg = {ix, iy, ix + iw, iy + ih};
                        if (ID2D1Bitmap* bmp = ottieniBitmap(*vc)) {
                            ID2D1RoundedRectangleGeometry* clipGeo = nullptr;
                            ID2D1Layer* layer = nullptr;
                            if (v.f2d && SUCCEEDED(v.f2d->CreateRoundedRectangleGeometry({rImg, 5.0f * d, 5.0f * d}, &clipGeo)) && clipGeo
                                && SUCCEEDED(v.rt->CreateLayer(nullptr, &layer)) && layer) {
                                v.rt->PushLayer(D2D1::LayerParameters(rImg, clipGeo), layer);
                                v.rt->DrawBitmap(bmp, rImg, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                                v.rt->PopLayer();
                            } else {
                                v.rt->DrawBitmap(bmp, rImg, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                            }
                            if (layer) layer->Release();
                            if (clipGeo) clipGeo->Release();
                        }
                        v.pen->SetColor(sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f));
                        v.rt->DrawRoundedRectangle({rImg, 5.0f * d, 5.0f * d}, v.pen, 1.0f);

                        const float tx = ix + iw + 12.0f * d;
                        wchar_t dim[48]{};
                        swprintf(dim, 48, L"%d × %d", vc->w, vc->h);
                        if (vc->eFile) {
                            const std::wstring& nome = vc->anteprimaTesto.empty() ? vc->testo : vc->anteprimaTesto;
                            v.testo(nome, fTesto, {tx, card.top + 14.0f * d, rightLimit, card.top + 52.0f * d}, ink);
                            v.testo(dim, fMeta, {tx, card.bottom - 48.0f * d, card.right - pad, card.bottom - 30.0f * d}, muted);
                            v.testo(meta, fMeta, {tx, card.bottom - 28.0f * d, card.right - pad, card.bottom - 10.0f * d}, muted);
                        } else {
                            const std::wstring titoloImg = std::wstring(testi::t(L"Immagine · ", L"Image · ")) + dim;
                            v.testo(titoloImg, fTesto, {tx, card.top + 22.0f * d, rightLimit, card.top + 48.0f * d}, ink);
                            v.testo(meta, fMeta, {tx, card.bottom - 30.0f * d, card.right - pad, card.bottom - 10.0f * d}, muted);
                        }
                    } else if (vc->eFile) {
                        const float bx = card.left + pad;
                        const float by = card.top + 14.0f * d;
                        const D2D1_RECT_F rBadge = {bx, by, bx + 22.0f * d, by + 22.0f * d};
                        v.pen->SetColor(sc ? D2D1::ColorF(0.38f, 0.68f, 1.0f, 0.18f) : D2D1::ColorF(0.0f, 0.45f, 0.88f, 0.12f));
                        v.rt->FillRoundedRectangle({rBadge, 6.0f * d, 6.0f * d}, v.pen);
                        v.pen->SetColor(sc ? D2D1::ColorF(0.55f, 0.80f, 1.0f, 0.90f) : D2D1::ColorF(0.0f, 0.42f, 0.85f, 0.88f));
                        const D2D1_RECT_F rTab = {bx + 4.5f * d, by + 5.5f * d, bx + 10.5f * d, by + 8.5f * d};
                        const D2D1_RECT_F rFld = {bx + 4.5f * d, by + 7.5f * d, bx + 17.5f * d, by + 16.5f * d};
                        v.rt->FillRoundedRectangle({rTab, 1.2f * d, 1.2f * d}, v.pen);
                        v.rt->DrawRoundedRectangle({rFld, 2.0f * d, 2.0f * d}, v.pen, 1.3f * d);

                        std::wstring pref;
                        if (vc->filePercorsi.size() > 1) {
                            wchar_t fb[32]{};
                            swprintf(fb, 32, testi::t(L"%zu file · ", L"%zu files · "), vc->filePercorsi.size());
                            pref = fb;
                        } else {
                            pref = L"File · ";
                        }
                        const float tx = card.left + pad + 28.0f * d;
                        const std::wstring& snippet = vc->anteprimaTesto.empty() ? vc->testo : vc->anteprimaTesto;
                        v.testo(snippet, fTesto, {tx, card.top + 10.0f * d, rightLimit, card.top + 48.0f * d}, ink);
                        v.testo(pref + meta, fMeta, {tx, card.bottom - 26.0f * d, card.right - pad, card.bottom - 8.0f * d}, muted);
                    } else {
                        D2D1_COLOR_F colHex{};
                        const bool isCol = estraiColoreHex(vc->testo, colHex);
                        const bool isLink = !isCol && eUrl(vc->testo);
                        float tx = card.left + pad;
                        if (isCol) {
                            const float r = 10.0f * d;
                            const float cx = tx + r;
                            const float cy = card.top + 27.0f * d;
                            v.pen->SetColor(colHex);
                            v.rt->FillEllipse({{cx, cy}, r, r}, v.pen);
                            v.pen->SetColor(sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f));
                            v.rt->DrawEllipse({{cx, cy}, r, r}, v.pen, 1.0f);
                            tx += 28.0f * d;
                        }
                        const D2D1_COLOR_F linkCol = sc
                            ? D2D1::ColorF(0.48f, 0.76f, 1.0f, 0.98f)
                            : D2D1::ColorF(0.0f, 0.42f, 0.85f, 0.98f);
                        const std::wstring& snippet = vc->anteprimaTesto.empty() ? vc->testo : vc->anteprimaTesto;
                        v.testo(snippet, fTesto, {tx, card.top + 10.0f * d, rightLimit, card.top + 48.0f * d}, isLink ? linkCol : ink);
                        if (isLink) {
                            const std::wstring pref = L"Link · ";
                            const float pw = v.larghezza(pref, fMeta);
                            v.testo(pref, fMeta, {tx, card.bottom - 26.0f * d, card.right - pad, card.bottom - 8.0f * d}, linkCol);
                            v.testo(meta, fMeta, {tx + pw, card.bottom - 26.0f * d, card.right - pad, card.bottom - 8.0f * d}, muted);
                        } else {
                            v.testo(meta, fMeta, {tx, card.bottom - 26.0f * d, card.right - pad, card.bottom - 8.0f * d}, muted);
                        }
                    }

                    // pulsanti azione (Pin e Elimina) in alto a destra
                    D2D1_RECT_F rPin{}, rDel{};
                    rettPulsantiScheda(card, rPin, rDel);
                    const D2D1_COLOR_F oro = sc ? D2D1::ColorF(1.0f, 0.78f, 0.22f, 0.95f) : D2D1::ColorF(0.85f, 0.50f, 0.0f, 0.95f);

                    if (isHov || vc->fissata) {
                        if (isHov) {
                            const bool hovPin = (sopraAzione == 1);
                            v.pen->SetColor(hovPin
                                ? (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.11f))
                                : (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f)));
                            v.rt->FillRoundedRectangle({rPin, 6.0f * d, 6.0f * d}, v.pen);
                        }
                        const float pcx = (rPin.left + rPin.right) * 0.5f;
                        const float pcy = (rPin.top + rPin.bottom) * 0.5f;
                        disegnaPuntina(pcx, pcy, d, vc->fissata, vc->fissata ? oro : v.inchiostro(0.75f));
                    }

                    if (isHov) {
                        const bool hovDel = (sopraAzione == 2);
                        v.pen->SetColor(hovDel
                            ? D2D1::ColorF(0.95f, 0.26f, 0.22f, sc ? 0.35f : 0.22f)
                            : (sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f)));
                        v.rt->FillRoundedRectangle({rDel, 6.0f * d, 6.0f * d}, v.pen);
                        const float dcx = (rDel.left + rDel.right) * 0.5f;
                        const float dcy = (rDel.top + rDel.bottom) * 0.5f;
                        const float arm = 3.8f * d;
                        v.pen->SetColor(hovDel ? D2D1::ColorF(1.0f, 0.36f, 0.32f, 1.0f) : v.inchiostro(0.80f));
                        v.rt->DrawLine({dcx - arm, dcy - arm}, {dcx + arm, dcy + arm}, v.pen, 1.4f * d);
                        v.rt->DrawLine({dcx + arm, dcy - arm}, {dcx - arm, dcy + arm}, v.pen, 1.4f * d);
                    }
                }
                y += h + gap;
            }
        }

        // barra di scorrimento sottile
        const float ms = maxScroll();
        if (ms > 0.5f) {
            const float vis = area.bottom - area.top;
            const float tot = vis + ms;
            const float thumbH = std::max(28.0f * d, vis * (vis / tot));
            const float thumbY = area.top + (scrollY / ms) * (vis - thumbH);
            const D2D1_RECT_F thumb = {(float)v.W - 5.5f * d, thumbY, (float)v.W - 2.5f * d, thumbY + thumbH};
            v.pen->SetColor(sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.22f));
            v.rt->FillRoundedRectangle({thumb, 1.5f * d, 1.5f * d}, v.pen);
        }

        v.rt->PopAxisAlignedClip();

        // bordo interno 1 px
        const float rag = v.raggio();
        v.pen->SetColor(sc ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f));
        v.rt->DrawRoundedRectangle({{0.5f, 0.5f, (float)v.W - 0.5f, (float)v.H - 0.5f}, rag, rag}, v.pen, 1.0f);

        v.finisci();
    }
};

}  // namespace appunti
