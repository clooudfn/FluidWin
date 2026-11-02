// Scala a cui una finestra si vede davvero sullo schermo (1 al 100%, 1.5 al 150%...).
// Non e' GetDpiForWindow: per le app che non conoscono la scala (le piu' vecchie) Windows risponde 96 (100%) ma poi le
// ingrandisce lui, e la capsula veniva piccola sulla loro barra ingrandita. La scala vera e' quella dello schermo su cui
// sta la finestra: e' quella a cui Windows la mostra, qualunque cosa sappia l'app.
#pragma once
#include <windows.h>

namespace scala {

inline UINT dpiSchermo(HMONITOR mon) {
    typedef HRESULT (WINAPI *Fn)(HMONITOR, int, UINT*, UINT*);
    static Fn f = [] { HMODULE sh = LoadLibraryW(L"shcore.dll"); return sh ? (Fn)GetProcAddress(sh, "GetDpiForMonitor") : (Fn)nullptr; }();
    UINT dx = 96, dy = 96;
    if (!f || !mon || FAILED(f(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) || !dx) return 96;
    return dx;
}

inline float di(HWND h) {
    if (!h || !IsWindow(h)) return 1.0f;
    return dpiSchermo(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)) / 96.0f;
}

}  // namespace scala
