// Raggio degli angoli che DWM da' alle finestre. Windows 11 usa 8 px e da solo non si puo' cambiare; se pero' e'
// attiva la mod "Custom Window Corner Radius" di Windhawk (che lo cambia dentro dwm.exe) se ne legge il valore
// dalle sue impostazioni, cosi' dock, menu, capsula, gelatina e genio hanno la stessa forma delle finestre vere.
#pragma once
#include <windows.h>

namespace raggio {

// px a 96 dpi impostati nella mod, 0 se la mod non c'e' o e' disattivata (letto una volta all'avvio)
inline int windhawk() {
    static int v = [] {
        const wchar_t* chiave = L"SOFTWARE\\Windhawk\\Engine\\Mods\\custom-corner-radius";
        DWORD spenta = 1, r = 0, n = sizeof(DWORD);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, chiave, L"Disabled", RRF_RT_REG_DWORD, nullptr, &spenta, &n) != ERROR_SUCCESS || spenta) return 0;
        n = sizeof(DWORD);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk\\Engine\\Mods\\custom-corner-radius\\Settings", L"radius",
                         RRF_RT_REG_DWORD, nullptr, &r, &n) != ERROR_SUCCESS) r = 12;   // 12 = valore predefinito della mod
        return r >= 1 && r <= 100 ? (int)r : 0;
    }();
    return v;
}

// angoli delle finestre "di vetro" di FluidWin (dock, isola, menu, pannelli): quelli di DWM
inline int vetro() { const int w = windhawk(); return w > 0 ? w : 8; }

}  // namespace raggio
