// Menu del tasto destro di FluidWin sul desktop e nell'elenco dei file di Esplora file, al posto di quello di Windows
// (niente piu' "Mostra altre opzioni"): un solo menu di vetro con tutte le voci, anche quelle delle altre app
// (7-Zip, Git, antivirus...), divise per categoria: Apri, Modifica, Nuovo, Visualizza, Condividi, Comprimi, Altro e
// in fondo Proprieta'.
// Niente iniezione in Esplora file: un gancio del mouse (fluidwin.cpp) trattiene il tasto destro solo sopra quelle
// viste; qui si trova l'elemento sotto il mouse con UI Automation, lo si seleziona come farebbe Windows (IFolderView2)
// e si prende il menu vero della shell (IShellView::GetItemObject -> IContextMenu), lo stesso del menu classico.
// Se qualcosa non torna (punto non riconosciuto, nomi doppi, finestra non trovata) mostra() da' false e FluidWin
// ripete il clic destro a Windows, che mostra il suo menu. Maiusc+clic destro: sempre quello di Windows.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlguid.h>
#include <uiautomation.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cwctype>
#include "menu.h"
#include "testi.h"

namespace destro {

// la finestra sotto il punto e' una vista di file della shell (desktop o Esplora file)? Veloce: la usa il gancio
inline bool vistaShell(POINT pt) {
    HWND h = WindowFromPoint(pt);
    if (!h) return false;
    wchar_t c[64] = L"", p[64] = L"", r[64] = L"";
    GetClassNameW(h, c, 64);
    HWND padre = GetParent(h);
    if (!padre) return false;
    GetClassNameW(padre, p, 64);
    if (wcscmp(p, L"SHELLDLL_DefView")) return false;
    GetClassNameW(GetAncestor(h, GA_ROOT), r, 64);
    if (!wcscmp(c, L"DirectUIHWND") && !wcscmp(r, L"CabinetWClass")) return true;                                  // Esplora file
    if (!wcscmp(c, L"SysListView32") && (!wcscmp(r, L"Progman") || !wcscmp(r, L"WorkerW"))) return true;          // desktop
    return false;
}

namespace dettagli {

inline std::wstring minuscolo(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// la vista della shell (IShellView) della finestra sotto il punto
inline IShellView* vistaDi(HWND sotto, bool desktop) {
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw))) || !sw) return nullptr;
    IShellView* trovata = nullptr;
    auto daDispatch = [&](IDispatch* d) {
        IServiceProvider* sp = nullptr; IShellBrowser* sb = nullptr; IShellView* sv = nullptr;
        if (d && SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&sp))) && sp &&
            SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) && sb &&
            SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
            HWND hv = nullptr; sv->GetWindow(&hv);
            // (con le schede di Esplora file piu' viste hanno la stessa finestra: vale quella che contiene il punto)
            if (hv && IsWindowVisible(hv) && (hv == sotto || IsChild(hv, sotto))) { trovata = sv; sv = nullptr; }
        }
        if (sv) sv->Release();
        if (sb) sb->Release();
        if (sp) sp->Release();
    };
    if (desktop) {
        VARIANT vd; VariantInit(&vd); vd.vt = VT_I4; vd.lVal = CSIDL_DESKTOP;
        VARIANT vv; VariantInit(&vv);
        long hw = 0; IDispatch* d = nullptr;
        if (SUCCEEDED(sw->FindWindowSW(&vd, &vv, SWC_DESKTOP, &hw, SWFO_NEEDDISPATCH, &d)) && d) { daDispatch(d); d->Release(); }
    } else {
        long n = 0; sw->get_Count(&n);
        for (long i = 0; i < n && !trovata; i++) {
            VARIANT vi; VariantInit(&vi); vi.vt = VT_I4; vi.lVal = i;
            IDispatch* d = nullptr;
            if (SUCCEEDED(sw->Item(vi, &d)) && d) { daDispatch(d); d->Release(); }
        }
    }
    sw->Release();
    return trovata;
}

// cosa c'e' sotto il punto, secondo UI Automation: 1 un elemento dell'elenco (nome), 0 lo sfondo, -1 altro (intestazioni...)
inline int sottoIlPunto(POINT pt, std::wstring& nome) {
    static IUIAutomation* ua = nullptr;
    if (!ua) CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&ua);
    if (!ua) return -1;
    IUIAutomationElement* e = nullptr;
    if (FAILED(ua->ElementFromPoint(pt, &e)) || !e) return -1;
    IUIAutomationTreeWalker* tw = nullptr; ua->get_ControlViewWalker(&tw);
    int r = 0;
    for (int livello = 0; livello < 7 && e; livello++) {
        CONTROLTYPEID t = 0; e->get_CurrentControlType(&t);
        if (t == UIA_ListItemControlTypeId || t == UIA_DataItemControlTypeId) {
            BSTR b = nullptr;
            if (SUCCEEDED(e->get_CurrentName(&b)) && b) { nome = b; SysFreeString(b); }
            r = nome.empty() ? -1 : 1;
            break;
        }
        // (il nome di un file e' un campo di testo dentro l'elemento: si risale fino all'elemento)
        if (t == UIA_HeaderControlTypeId || t == UIA_HeaderItemControlTypeId || t == UIA_ScrollBarControlTypeId) { r = -1; break; }
        if (t == UIA_ListControlTypeId || t == UIA_PaneControlTypeId || t == UIA_WindowControlTypeId) { r = 0; break; }
        IUIAutomationElement* su = nullptr;
        if (!tw || FAILED(tw->GetParentElement(e, &su))) su = nullptr;
        e->Release(); e = su;
    }
    if (e) e->Release();
    if (tw) tw->Release();
    return r;
}

enum Categoria { APRI, MODIFICA, NUOVO, VISUALIZZA, CONDIVIDI, COMPRIMI, ALTRO, PROPRIETA, QUANTE };

inline Categoria categoria(const std::wstring& verbo, const std::wstring& testo) {
    const std::wstring v = minuscolo(verbo);
    // testo senza "&" (le lettere sottolineate) e senza "..." in fondo
    std::wstring t;
    for (wchar_t ch : minuscolo(testo)) if (ch != L'&') t.push_back(ch);
    while (!t.empty() && (t.back() == L'.' || t.back() == L'…' || t.back() == L' ')) t.pop_back();
    auto contiene = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (t.find(x) != std::wstring::npos) return true; return false; };
    auto inizia = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (t.rfind(x, 0) == 0) return true; return false; };
    auto uguale = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (t == x) return true; return false; };
    // prima il verbo (quando Windows lo da')
    if (v == L"properties") return PROPRIETA;
    if (v.find(L"zip") != std::wstring::npos || v.find(L"compress") != std::wstring::npos || v.find(L"extract") != std::wstring::npos) return COMPRIMI;
    for (auto x : {L"open", L"opennew", L"openas", L"openwith", L"runas", L"runasuser", L"edit", L"print", L"play", L"preview", L"explore",
                   L"opencontaining", L"openinnewtab", L"openinnewwindow", L"opennewwindow", L"opennewtab"}) if (v == x) return APRI;
    for (auto x : {L"cut", L"copy", L"paste", L"pastelink", L"delete", L"rename", L"link", L"copyaspath", L"undo", L"redo"}) if (v == x) return MODIFICA;
    for (auto x : {L"view", L"arrange", L"groupby", L"refresh"}) if (v == x) return VISUALIZZA;
    if (v.find(L"modernshare") != std::wstring::npos || v.find(L"share") != std::wstring::npos || v.find(L"sendto") != std::wstring::npos) return CONDIVIDI;
    // poi il testo (italiano e inglese)
    if (uguale({L"proprietà", L"properties"})) return PROPRIETA;
    if (contiene({L"zip", L"7-zip", L"winrar", L"compr", L"estrai", L"extract", L"archiv"})) return COMPRIMI;
    if (uguale({L"apri", L"open", L"modifica", L"edit", L"stampa", L"print", L"riproduci", L"play", L"esegui come amministratore", L"run as administrator"}) ||
        inizia({L"apri con", L"open with", L"apri in", L"apri nel", L"open in", L"modifica con", L"modifica nel", L"modifica in",
                L"edit with", L"edit in"})) return APRI;
    if (uguale({L"taglia", L"copia", L"incolla", L"elimina", L"rinomina", L"crea collegamento", L"copia come percorso", L"incolla collegamento",
                L"cut", L"copy", L"paste", L"delete", L"rename", L"create shortcut", L"copy as path", L"paste shortcut"}) ||
        inizia({L"annulla", L"ripeti", L"undo", L"redo"})) return MODIFICA;
    if (inizia({L"nuovo", L"new"})) return NUOVO;
    if (uguale({L"visualizza", L"ordina per", L"raggruppa per", L"aggiorna", L"view", L"sort by", L"group by", L"refresh"})) return VISUALIZZA;
    if (contiene({L"invia a", L"send to", L"condividi", L"share"})) return CONDIVIDI;
    return ALTRO;
}

inline const wchar_t* nomeCategoria(int c) {
    switch (c) {
        case APRI: return testi::t(L"Apri", L"Open");
        case MODIFICA: return testi::t(L"Modifica", L"Edit");
        case NUOVO: return testi::t(L"Nuovo", L"New");
        case VISUALIZZA: return testi::t(L"Visualizza", L"View");
        case CONDIVIDI: return testi::t(L"Condividi e invia", L"Share and send");
        case COMPRIMI: return testi::t(L"Comprimi ed estrai", L"Compress and extract");
        case ALTRO: return testi::t(L"Altro", L"More");
    }
    return L"";
}

}  // namespace dettagli

// Mostra il menu di FluidWin per un clic destro in pt. proprietario: una finestra di FluidWin per il menu.
// false: non si e' potuto (FluidWin rimanda il clic a Windows)
inline bool mostra(POINT pt, HWND proprietario, const std::function<void(const char*)>& registro) {
    using namespace dettagli;
    HWND sotto = WindowFromPoint(pt);
    if (!sotto || !vistaShell(pt)) {
        if (registro) {
            wchar_t c[64] = L"", p[64] = L"", r[64] = L"";
            if (sotto) { GetClassNameW(sotto, c, 64); GetClassNameW(GetParent(sotto), p, 64); GetClassNameW(GetAncestor(sotto, GA_ROOT), r, 64); }
            char b[260]; snprintf(b, 260, "tasto destro: %ld,%ld non e' una vista di file (%ls in %ls, finestra %ls)", pt.x, pt.y, c, p, r); registro(b);
        }
        return false;
    }
    HWND radice = GetAncestor(sotto, GA_ROOT);
    wchar_t rc[64] = L""; GetClassNameW(radice, rc, 64);
    const bool desktop = wcscmp(rc, L"CabinetWClass") != 0;
    std::wstring nome;
    const int cosa = sottoIlPunto(pt, nome);
    if (cosa < 0) { if (registro) registro("tasto destro: punto non riconosciuto, menu di Windows"); return false; }
    IShellView* sv = vistaDi(sotto, desktop);
    if (!sv) { if (registro) registro("tasto destro: vista della shell non trovata, menu di Windows"); return false; }
    bool ok = false;
    IFolderView2* fv = nullptr;
    sv->QueryInterface(IID_PPV_ARGS(&fv));
    if (cosa == 1) {
        // l'elemento col nome visto da UI Automation; se non e' gia' fra i selezionati diventa l'unico selezionato
        int indice = -1, quanti = 0;
        if (fv) {
            int n = 0; fv->ItemCount(SVGIO_ALLVIEW, &n);
            for (int i = 0; i < n && n < 20000; i++) {
                IShellItem* it = nullptr;
                if (FAILED(fv->GetItem(i, IID_PPV_ARGS(&it))) || !it) continue;
                LPWSTR s = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s) { if (nome == s) { indice = i; quanti++; } CoTaskMemFree(s); }
                it->Release();
            }
        }
        if (indice < 0 || quanti != 1) {   // nomi doppi (estensioni nascoste) o non trovato: meglio il menu di Windows
            if (fv) fv->Release();
            sv->Release();
            if (registro) registro(quanti > 1 ? "tasto destro: piu' file con lo stesso nome, menu di Windows" : "tasto destro: file non trovato, menu di Windows");
            return false;
        }
        DWORD stato = 0;
        PITEMID_CHILD pid = nullptr;
        if (SUCCEEDED(fv->Item(indice, &pid)) && pid) { fv->GetSelectionState(pid, &stato); CoTaskMemFree(pid); }
        if (!(stato & SVSI_SELECT)) fv->SelectItem(indice, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
    }
    IContextMenu* cm = nullptr;
    sv->GetItemObject(cosa == 1 ? SVGIO_SELECTION : SVGIO_BACKGROUND, IID_PPV_ARGS(&cm));
    if (cm) {
        HMENU m = CreatePopupMenu();
        if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL | CMF_CANRENAME))) {
            IContextMenu2* cm2 = nullptr; IContextMenu3* cm3 = nullptr;
            cm->QueryInterface(IID_PPV_ARGS(&cm2)); cm->QueryInterface(IID_PPV_ARGS(&cm3));
            // le voci di Windows divise per categoria (i sottomenu restano sottomenu)
            std::vector<menu::Voce> perCat[QUANTE];
            for (auto& v : menu::daHmenu(m)) {
                if (v.separatore || v.titolo) continue;
                wchar_t verbo[128] = L"";
                if (v.id > 0 && !v.sotto && FAILED(cm->GetCommandString(v.id - 1, GCS_VERBW, nullptr, (LPSTR)verbo, 128))) verbo[0] = 0;
                perCat[categoria(verbo, v.testo)].push_back(v);
            }
            std::vector<menu::Voce> voci;
            for (int c = 0; c < PROPRIETA; c++) {
                if (perCat[c].empty()) continue;
                if (!voci.empty()) voci.push_back(menu::separatore());
                voci.push_back(menu::titolo(nomeCategoria(c)));
                for (auto& v : perCat[c]) voci.push_back(v);
            }
            if (!perCat[PROPRIETA].empty()) { voci.push_back(menu::separatore()); for (auto& v : perCat[PROPRIETA]) voci.push_back(v); }
            auto prepara = [cm2, cm3](HMENU s) {   // i sottomenu della shell ("Invia a", "Apri con", "Nuovo") si riempiono ora
                if (cm3) { LRESULT r = 0; cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)s, 0, &r); }
                else if (cm2) cm2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)s, 0);
            };
            if (!voci.empty()) {
                ok = true;
                if (registro) registro(cosa == 1 ? "tasto destro: menu di FluidWin (file)" : "tasto destro: menu di FluidWin (sfondo)");
                SetForegroundWindow(proprietario);
                const UINT cmd = menu::mostra(voci, pt, proprietario, false, prepara);
                if (cmd > 0) {
                    SetForegroundWindow(radice);   // (rinomina, incolla... agiscono sulla vista)
                    CMINVOKECOMMANDINFOEX ci{}; ci.cbSize = sizeof(ci); ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                    ci.hwnd = radice; ci.lpVerb = MAKEINTRESOURCEA(cmd - 1); ci.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                    ci.nShow = SW_SHOWNORMAL; ci.ptInvoke = pt;
                    cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci);
                }
            }
            if (cm3) cm3->Release();
            if (cm2) cm2->Release();
        }
        DestroyMenu(m);
        cm->Release();
    }
    if (fv) fv->Release();
    sv->Release();
    return ok;
}

}  // namespace destro
