// Piccole comodita' del centro di controllo che valgono per tutto FluidWin:
//  - Tieni sveglio: il PC non va in sospensione e lo schermo non si spegne (SetThreadExecutionState, la stessa cosa
//    che fa un lettore video), per un'ora, due o finche' non lo si spegne;
//  - Timer: conto alla rovescia (lo mostra il notch); alla fine suona la sveglia di Windows (Media\Alarm01.wav) per al
//    massimo 30 s o finche' non lo si ferma con un clic sul notch o dal centro di controllo;
//  - Non disturbare: le notifiche non compaiono (ne' nel notch ne' col banner di vetro) ma restano nel centro
//    notifiche; per un'ora, tre o fino a domani mattina. Le notifiche di sistema (Defender...) le mostra sempre Windows.
// Lo stato non si salva: al riavvio di FluidWin e' tutto spento. Si usa solo dal thread principale.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <cwchar>
#include <mmsystem.h>
#include "testi.h"

namespace comodita {

constexpr ULONGLONG SEMPRE = ~0ULL;   // finche' non lo si spegne

struct Stato {
    ULONGLONG svegliaFino = 0, silenzioFino = 0;   // GetTickCount64 di fine; 0 spento
    bool sveglioApplicato = false;
    ULONGLONG timerFino = 0, timerDurata = 0;      // timer in corso
    bool timerSuona = false; ULONGLONG suonaFino = 0;   // timer finito: suona
};
inline Stato& stato() { static Stato s; return s; }

inline bool sveglio() { return stato().svegliaFino != 0; }
inline bool silenzio() { return stato().silenzioFino != 0; }

// millisecondi da adesso fino a domani alle 'ora' (ora locale)
inline ULONGLONG finoADomani(int ora) {
    SYSTEMTIME st; GetLocalTime(&st);
    const int adesso = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
    return (ULONGLONG)(24 * 3600 - adesso + ora * 3600) * 1000;
}

// applica la sveglia a Windows (solo quando cambia)
inline void applica() {
    Stato& s = stato();
    const bool si = s.svegliaFino != 0;
    if (si == s.sveglioApplicato) return;
    s.sveglioApplicato = si;
    SetThreadExecutionState(si ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
}

// durata in ms (0 spegne, SEMPRE finche' non lo si spegne)
inline void tieniSveglio(ULONGLONG ms) { stato().svegliaFino = ms == 0 ? 0 : ms == SEMPRE ? SEMPRE : GetTickCount64() + ms; applica(); }
inline void nonDisturbare(ULONGLONG ms) { stato().silenzioFino = ms == 0 ? 0 : ms == SEMPRE ? SEMPRE : GetTickCount64() + ms; }

inline bool timerAttivo() { return stato().timerFino != 0; }
inline bool timerSuona() { return stato().timerSuona; }
inline ULONGLONG timerResto() { const ULONGLONG t = GetTickCount64(), f = stato().timerFino; return f > t ? f - t : 0; }
inline double timerAvanzamento() { const Stato& s = stato(); return s.timerDurata ? 1.0 - (double)timerResto() / s.timerDurata : 0; }

inline void silenziaTimer() {
    if (!stato().timerSuona) return;
    stato().timerSuona = false;
    PlaySoundW(nullptr, nullptr, 0);
}
inline void avviaTimer(ULONGLONG ms) { silenziaTimer(); stato().timerDurata = ms; stato().timerFino = ms ? GetTickCount64() + ms : 0; }
inline void fermaTimer() { stato().timerFino = 0; silenziaTimer(); }

// "4:32" o "1:04:32"
inline std::wstring durata(ULONGLONG ms) {
    const ULONGLONG s = (ms + 999) / 1000;
    wchar_t b[32];
    if (s >= 3600) swprintf(b, 32, L"%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    else swprintf(b, 32, L"%llu:%02llu", s / 60, s % 60);
    return b;
}

// a ogni giro del timer principale: spegne quello che e' scaduto
inline void controlla() {
    Stato& s = stato();
    const ULONGLONG t = GetTickCount64();
    if (s.timerFino && t >= s.timerFino) {   // fine del timer: suona
        s.timerFino = 0; s.timerSuona = true; s.suonaFino = t + 30000;
        wchar_t p[MAX_PATH]; GetWindowsDirectoryW(p, MAX_PATH); wcscat(p, L"\\Media\\Alarm01.wav");
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES || !PlaySoundW(p, nullptr, SND_FILENAME | SND_ASYNC | SND_LOOP | SND_NODEFAULT))
            MessageBeep(MB_ICONASTERISK);
    }
    if (s.timerSuona && t >= s.suonaFino) silenziaTimer();
    if (s.svegliaFino && s.svegliaFino != SEMPRE && t >= s.svegliaFino) s.svegliaFino = 0;
    if (s.silenzioFino && s.silenzioFino != SEMPRE && t >= s.silenzioFino) s.silenzioFino = 0;
    applica();
}

// "Fino alle 15:30", "Finche' non lo spegni" o "Spento"
inline std::wstring descrivi(ULONGLONG fino) {
    if (!fino) return testi::t(L"Spento", L"Off");
    if (fino == SEMPRE) return testi::t(L"Finché non lo spegni", L"Until turned off");
    const ULONGLONG t = GetTickCount64();
    const ULONGLONG resto = fino > t ? fino - t : 0;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart += resto * 10000;   // da ms a unita' da 100 ns
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME su, sl; FileTimeToSystemTime(&ft, &su); SystemTimeToTzSpecificLocalTime(nullptr, &su, &sl);
    wchar_t b[64];
    swprintf(b, 64, testi::t(L"Fino alle %d:%02d", L"Until %d:%02d"), sl.wHour, sl.wMinute);
    return b;
}

}  // namespace comodita
