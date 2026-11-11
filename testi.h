// Lingua dell'interfaccia di FluidWin: italiano o inglese, scelta nelle impostazioni (al primo avvio quella di
// Windows). Solo i testi che si vedono cambiano lingua; il codice resta in italiano.
// Uso: testi::t(L"Impostazioni", L"Settings"), letto al momento di disegnare: cambiando lingua basta ridisegnare.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace testi {

enum Lingua { ITALIANO = 0, INGLESE = 1 };

inline int& lingua() { static int l = ITALIANO; return l; }
inline bool inglese() { return lingua() == INGLESE; }

// il testo nella lingua scelta
inline const wchar_t* t(const wchar_t* it, const wchar_t* en) { return inglese() ? en : it; }

// nome della lingua per date e orari (GetDateFormatEx)
inline const wchar_t* locale() { return inglese() ? L"en-US" : L"it-IT"; }

// lingua al primo avvio: italiano se Windows e' in italiano, altrimenti inglese
inline int predefinita() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN ? ITALIANO : INGLESE; }

}  // namespace testi
