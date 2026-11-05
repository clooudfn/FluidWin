// Lavori brevi nel pool di thread di Windows invece di un thread nuovo ogni volta (foto delle finestre, finestre
// appena nate, lavori del dock): prima a riposo c'erano ~150 thread. Un lavoro che puo' restare fermo a lungo (una
// foto di un'app che non risponde) lo dice al pool, che ne aggiunge altri se servono.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pool {

// f(p) nel pool, con la priorita' indicata (rimessa com'era alla fine). false = non partito (allora niente f)
inline bool lancia(DWORD (WINAPI* f)(LPVOID), void* p, int priorita = THREAD_PRIORITY_NORMAL, bool lungo = false) {
    struct Lavoro { DWORD (WINAPI* f)(LPVOID); void* p; int priorita; bool lungo; };
    Lavoro* l = new Lavoro{f, p, priorita, lungo};
    const BOOL ok = TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE ist, PVOID x) {
        Lavoro* l = (Lavoro*)x;
        if (l->lungo) CallbackMayRunLong(ist);
        const HANDLE t = GetCurrentThread();
        const int prima = GetThreadPriority(t);
        if (l->priorita != prima) SetThreadPriority(t, l->priorita);
        l->f(l->p);
        if (l->priorita != prima) SetThreadPriority(t, prima);
        delete l;
    }, l, nullptr);
    if (!ok) delete l;
    return ok != FALSE;
}

}  // namespace pool
