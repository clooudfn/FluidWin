// PrintWindow con un limite di tempo. PrintWindow manda un messaggio alla finestra e aspetta la sua risposta senza
// limite: con un'app bloccata o lentissima fermerebbe tutto FluidWin (dock, capsule, animazioni). La fa un thread a
// parte; se entro 'limite' ms non ha finito si rinuncia, e il thread finisce da solo quando l'app risponde,
// liberando tutto lui. Con un'app che Windows da' gia' per bloccata ("Non risponde") non si prova nemmeno.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <vector>

namespace stampa {

struct Lavoro { HWND h; int w, hh; std::vector<UINT> px; BOOL ok; HANDLE fatto; LONG rif; };

// Finestre con una foto ancora in corso (thread fermo dentro PrintWindow perche' l'app non risponde): finche' non
// finisce non se ne avvia un'altra, altrimenti thread e memoria (una DIB grande quanto la finestra per ognuno) si
// accumulerebbero. (Mai liberato: un thread fermo puo' finire anche durante la chiusura di FluidWin)
struct InCorso { CRITICAL_SECTION cs; std::vector<HWND> h; InCorso() { InitializeCriticalSection(&cs); } };
inline InCorso& inCorso() { static InCorso* i = new InCorso; return *i; }
inline bool prenota(HWND h) {
    InCorso& c = inCorso(); EnterCriticalSection(&c.cs);
    const bool libera = std::find(c.h.begin(), c.h.end(), h) == c.h.end();
    if (libera) c.h.push_back(h);
    LeaveCriticalSection(&c.cs);
    return libera;
}
inline void libera(HWND h) {
    InCorso& c = inCorso(); EnterCriticalSection(&c.cs);
    c.h.erase(std::remove(c.h.begin(), c.h.end(), h), c.h.end());
    LeaveCriticalSection(&c.cs);
}

// chi finisce per ultimo (thread o chiamante) libera
inline void lascia(Lavoro* l) { if (InterlockedDecrement(&l->rif) == 0) { CloseHandle(l->fatto); delete l; } }

inline DWORD WINAPI thread(LPVOID p) {
    Lavoro* l = (Lavoro*)p;
    BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), l->w, -l->hh, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits && mem) {
        HGDIOBJ vecchio = SelectObject(mem, dib);
        l->ok = PrintWindow(l->h, mem, PW_RENDERFULLCONTENT);
        GdiFlush();
        if (l->ok) l->px.assign((const UINT*)bits, (const UINT*)bits + (size_t)l->w * l->hh);
        SelectObject(mem, vecchio);
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    libera(l->h);
    SetEvent(l->fatto);
    lascia(l);
    return 0;
}

// Foto in sottofondo: parte adesso e non si aspetta (nullptr = non partita). Poi pronta() a ogni fotogramma e,
// quando lo e', prendi(); se non serve piu', abbandona(). Tutta la finestra (w x hh = GetWindowRect), BGRA
inline Lavoro* avvia(HWND h, int w, int hh, int priorita = THREAD_PRIORITY_NORMAL) {
    if (w <= 0 || hh <= 0 || !IsWindow(h) || IsHungAppWindow(h)) return nullptr;
    if (!prenota(h)) return nullptr;   // ce n'e' gia' una ferma su questa finestra
    Lavoro* l = new Lavoro{h, w, hh, {}, FALSE, CreateEventW(nullptr, TRUE, FALSE, nullptr), 2};
    HANDLE t = l->fatto ? CreateThread(nullptr, 0, thread, l, 0, nullptr) : nullptr;
    if (!t) { if (l->fatto) CloseHandle(l->fatto); delete l; libera(h); return nullptr; }
    SetThreadPriority(t, priorita);
    CloseHandle(t);
    return l;
}
inline bool pronta(Lavoro* l) { return l && WaitForSingleObject(l->fatto, 0) == WAIT_OBJECT_0; }
inline bool prendi(Lavoro* l, std::vector<UINT>& px) {
    if (!l) return false;
    const bool ok = pronta(l) && l->ok && l->px.size() == (size_t)l->w * l->hh;
    if (ok) px.swap(l->px);
    lascia(l);
    return ok;
}
inline void abbandona(Lavoro* l) { if (l) lascia(l); }   // il thread, se e' ancora fermo, libera lui quando finisce

// Tutta la finestra h (w x hh = GetWindowRect), pixel BGRA riga per riga. false: non riuscita o troppo lenta
inline bool finestra(HWND h, int w, int hh, std::vector<UINT>& px, DWORD limite = 300) {
    Lavoro* l = avvia(h, w, hh, GetThreadPriority(GetCurrentThread()));
    if (!l) return false;
    WaitForSingleObject(l->fatto, limite);
    return prendi(l, px);
}

}  // namespace stampa
