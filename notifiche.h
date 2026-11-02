// Notifiche di Windows lette con l'API ufficiale UserNotificationListener (Windows.UI.Notifications.Management),
// la stessa che usano gli smartwatch e le app "companion". Funziona solo se nelle impostazioni della privacy
// e' consentito l'accesso alle notifiche; non mostra richieste da solo. MinGW non ha le intestazioni di questa
// classe: gli IID li si chiede all'oggetto stesso (GetIids) e i metodi si chiamano per posizione, nell'ordine
// delle intestazioni ufficiali (verificato su windows.ui.notifications.h e windows.applicationmodel.h).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <string>
#include <vector>
#include <algorithm>

namespace notifiche {

struct Notifica {
    UINT32 id = 0;
    std::wstring app, aumid, titolo, testo;
    INT64 quando = 0;   // FILETIME (100 ns dal 1601, UTC)
};

template <class F> static F metodo(void* o, int i) { return (F)(*(void***)o)[i]; }
static void rilascia(void* o) { if (o) ((IUnknown*)o)->Release(); }
static std::wstring testoDa(HSTRING s) {
    UINT32 n = 0; const wchar_t* p = WindowsGetStringRawBuffer(s, &n);
    std::wstring r(p ? p : L"", p ? n : 0);
    WindowsDeleteString(s);
    return r;
}

typedef HRESULT (STDMETHODCALLTYPE *FnPtr)(void*, void**);
typedef HRESULT (STDMETHODCALLTYPE *FnStr)(void*, HSTRING*);

class Lettore {
public:
    // false se l'accesso alle notifiche non e' consentito (o l'API non c'e')
    bool permesso() { return prepara(); }

    bool leggi(std::vector<Notifica>& out) {
        out.clear();
        if (!prepara()) return false;
        void* op = nullptr;
        // GetNotificationsAsync(NotificationKinds::Toast)
        if (FAILED(metodo<HRESULT (STDMETHODCALLTYPE*)(void*, UINT32, void**)>(ascolta, 10)(ascolta, 1, &op)) || !op) return false;
        static const IID IID_Async = {0x00000036, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};   // IAsyncInfo
        void* info = nullptr; ((IUnknown*)op)->QueryInterface(IID_Async, &info);
        int stato = 0;
        for (int i = 0; i < 400 && info; i++) {
            metodo<HRESULT (STDMETHODCALLTYPE*)(void*, int*)>(info, 7)(info, &stato);   // get_Status
            if (stato != 0) break;
            Sleep(1);
        }
        rilascia(info);
        void* vista = nullptr;
        if (stato == 1) metodo<FnPtr>(op, 8)(op, &vista);   // GetResults
        rilascia(op);
        if (!vista) return false;
        UINT32 n = 0; metodo<HRESULT (STDMETHODCALLTYPE*)(void*, UINT32*)>(vista, 7)(vista, &n);
        for (UINT32 i = 0; i < n && i < 50; i++) {
            void* un = nullptr;
            if (FAILED(metodo<HRESULT (STDMETHODCALLTYPE*)(void*, UINT32, void**)>(vista, 6)(vista, i, &un)) || !un) continue;
            Notifica x;
            metodo<HRESULT (STDMETHODCALLTYPE*)(void*, UINT32*)>(un, 8)(un, &x.id);
            metodo<HRESULT (STDMETHODCALLTYPE*)(void*, INT64*)>(un, 9)(un, &x.quando);
            void* app = nullptr;
            if (SUCCEEDED(metodo<FnPtr>(un, 7)(un, &app)) && app) {
                HSTRING s = nullptr;
                if (SUCCEEDED(metodo<FnStr>(app, 7)(app, &s))) x.aumid = testoDa(s);
                void* di = nullptr;
                if (SUCCEEDED(metodo<FnPtr>(app, 8)(app, &di)) && di) {
                    HSTRING d = nullptr; if (SUCCEEDED(metodo<FnStr>(di, 6)(di, &d))) x.app = testoDa(d);
                    rilascia(di);
                }
                rilascia(app);
            }
            void* no = nullptr;
            if (SUCCEEDED(metodo<FnPtr>(un, 6)(un, &no)) && no) {
                void* vis = nullptr;
                if (SUCCEEDED(metodo<FnPtr>(no, 8)(no, &vis)) && vis) {
                    HSTRING tg = nullptr; WindowsCreateString(L"ToastGeneric", 12, &tg);
                    void* bind = nullptr;
                    metodo<HRESULT (STDMETHODCALLTYPE*)(void*, HSTRING, void**)>(vis, 9)(vis, tg, &bind);
                    WindowsDeleteString(tg);
                    if (bind) {
                        void* testi = nullptr;
                        if (SUCCEEDED(metodo<FnPtr>(bind, 11)(bind, &testi)) && testi) {
                            UINT32 k = 0; metodo<HRESULT (STDMETHODCALLTYPE*)(void*, UINT32*)>(testi, 7)(testi, &k);
                            for (UINT32 j = 0; j < k; j++) {
                                void* el = nullptr;
                                if (FAILED(metodo<HRESULT (STDMETHODCALLTYPE*)(void*, UINT32, void**)>(testi, 6)(testi, j, &el)) || !el) continue;
                                HSTRING t = nullptr;
                                if (SUCCEEDED(metodo<FnStr>(el, 6)(el, &t))) {
                                    std::wstring s = testoDa(t);
                                    if (j == 0) x.titolo = s;
                                    else if (!s.empty()) x.testo += (x.testo.empty() ? L"" : L" ") + s;
                                }
                                rilascia(el);
                            }
                            rilascia(testi);
                        }
                        rilascia(bind);
                    }
                    rilascia(vis);
                }
                rilascia(no);
            }
            rilascia(un);
            for (auto& c : x.titolo) if (c == L'\n' || c == L'\r') c = L' ';
            for (auto& c : x.testo) if (c == L'\n' || c == L'\r') c = L' ';
            out.push_back(std::move(x));
        }
        rilascia(vista);
        std::sort(out.begin(), out.end(), [](const Notifica& a, const Notifica& b) { return a.quando > b.quando; });
        return true;
    }

    void rimuovi(UINT32 id) { if (prepara()) metodo<HRESULT (STDMETHODCALLTYPE*)(void*, UINT32)>(ascolta, 13)(ascolta, id); }
    // ClearNotifications per le app desktop non ha effetto: si toglie una notifica alla volta (questo funziona)
    void cancellaTutte() {
        std::vector<Notifica> tutte;
        if (!leggi(tutte)) return;
        for (const auto& n : tutte) rimuovi(n.id);
    }

private:
    void* ascolta = nullptr;   // IUserNotificationListener
    bool provato = false;
    ULONGLONG tProva = 0;

    bool prepara() {
        if (ascolta) return true;
        // permesso negato: si riprova al massimo ogni 5 s (l'utente puo' averlo appena concesso)
        if (provato && GetTickCount64() - tProva < 5000) return false;
        provato = true; tProva = GetTickCount64();
        HSTRING cl = nullptr; const wchar_t* nome = L"Windows.UI.Notifications.Management.UserNotificationListener";
        WindowsCreateString(nome, (UINT32)wcslen(nome), &cl);
        IInspectable* fab = nullptr;
        const HRESULT hr = RoGetActivationFactory(cl, __uuidof(IInspectable), (void**)&fab);
        WindowsDeleteString(cl);
        if (FAILED(hr) || !fab) return false;
        // l'interfaccia "statics" e' quella diversa da IActivationFactory; il suo primo metodo e' get_Current
        static const IID IID_Fabbrica = {0x00000035, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
        ULONG n = 0; IID* ids = nullptr; fab->GetIids(&n, &ids);
        void* stat = nullptr;
        for (ULONG i = 0; i < n && !stat; i++) if (!IsEqualIID(ids[i], IID_Fabbrica)) fab->QueryInterface(ids[i], &stat);
        CoTaskMemFree(ids); fab->Release();
        if (!stat) return false;
        IInspectable* corrente = nullptr;
        metodo<FnPtr>(stat, 6)(stat, (void**)&corrente);
        rilascia(stat);
        if (!corrente) return false;
        // l'interfaccia predefinita dell'oggetto (la prima che dichiara) e' IUserNotificationListener
        ULONG m = 0; IID* lid = nullptr; corrente->GetIids(&m, &lid);
        if (m > 0) corrente->QueryInterface(lid[0], &ascolta);
        CoTaskMemFree(lid); corrente->Release();
        if (!ascolta) return false;
        int accesso = 0;
        metodo<HRESULT (STDMETHODCALLTYPE*)(void*, int*)>(ascolta, 7)(ascolta, &accesso);   // GetAccessStatus
        if (accesso != 1) { rilascia(ascolta); ascolta = nullptr; return false; }
        return true;
    }
};

} // namespace notifiche
