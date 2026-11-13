// Chiamate verso finestre di altri programmi con un limite di tempo.
// ShowWindow, SetWindowPos e SetWindowLongPtr(GWL_EXSTYLE) su una finestra di un altro processo le mandano dei
// messaggi e aspettano che lei li legga, senza limite: misurato con un processo di prova fermo, ognuna delle tre e'
// rimasta ferma 29 s, cioe' finche' lui non si e' ripreso. Fatte dal thread principale, con un'app bloccata fermavano
// tutto FluidWin (dock, notch, animazioni). IsHungAppWindow non basta: dice "bloccata" solo dopo 5 s senza messaggi.
// Non aspettano invece (0 ms nella stessa prova): SetLayeredWindowAttributes, SetPropW, RedrawWindow senza
// RDW_UPDATENOW, SetForegroundWindow, SwitchToThisWindow, ShowWindowAsync, SetWindowPos con SWP_ASYNCWINDOWPOS.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// La finestra risponde ai messaggi entro 'ms'? Con un'app che Windows da' gia' per bloccata torna subito (ABORTIFHUNG).
// Con una che risponde non costa nulla in piu': la chiamata che segue l'avrebbe aspettata comunque. (Nata per il
// guardiano: con una finestra che non legge mai i messaggi, una nascosta di nvcontainer, NVCapContext, SetWindowLongPtr
// restava bloccato per sempre, guardiano e avvio di FluidWin compresi)
static bool risponde(HWND h, UINT ms) {
    DWORD_PTR r = 0;
    return SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, ms, &r) != 0;
}

// Ripristina una finestra ridotta a icona senza restare appesi a lei: se risponde, come sempre con ShowWindow; se no,
// la richiesta resta in coda (ShowWindowAsync) e l'app la eseguira' quando si riprende. Torna il risultato di
// ShowWindow, FALSE se messa in coda: chi la usa ripiega allora su SwitchToThisWindow (non aspetta, e funziona anche
// con le app avviate come amministratore, che ShowWindow rifiuta)
static BOOL ripristinaAltrui(HWND h) {
    if (risponde(h, 300)) return ShowWindow(h, SW_RESTORE);
    ShowWindowAsync(h, SW_RESTORE);
    return FALSE;
}
