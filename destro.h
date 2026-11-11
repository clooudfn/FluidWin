// Menu del tasto destro di FluidWin sul desktop e nell'elenco dei file di Esplora file, al posto di quello di Windows:
// le stesse voci del menu compatto di Windows 11 (in cima la riga di icone Taglia, Copia, Rinomina, Condividi, Elimina;
// sotto Apri, Apri con, Copia come percorso... e Proprieta'), nello stile di FluidWin; in fondo una freccia che, al
// solo passaggio del mouse, gira e allunga il menu con tutte le altre voci (quelle di "Mostra altre opzioni", anche
// delle altre app: 7-Zip, WinRAR, Git...). Tutto in un posto solo.
// Niente iniezione in Esplora file: un gancio del mouse (fluidwin.cpp) trattiene il tasto destro solo sopra quelle
// viste; qui si trova l'elemento sotto il mouse (desktop: dalle posizioni delle icone, IFolderView::GetItemPosition;
// Esplora file: UI Automation), lo si seleziona come farebbe Windows (IFolderView2) e si prende il menu vero della
// shell (IShellView::GetItemObject -> IContextMenu).
// Se qualcosa non torna mostra() da' false e FluidWin rimanda il clic a Windows. Maiusc+clic destro: quello di Windows.
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

// testo senza "&" (lettere sottolineate) e senza "..." in fondo, minuscolo
inline std::wstring pulito(const std::wstring& testo) {
    std::wstring t;
    for (wchar_t ch : minuscolo(testo)) if (ch != L'&') t.push_back(ch);
    while (!t.empty() && (t.back() == L'.' || t.back() == L'…' || t.back() == L' ')) t.pop_back();
    return t;
}

// pulsante della riga in cima (Taglia, Copia, Incolla, Rinomina, Condividi, Elimina), 0 se non lo e'
inline int pulsante(const std::wstring& verbo, const std::wstring& testo) {
    const std::wstring v = minuscolo(verbo), t = pulito(testo);
    if (v == L"cut" || t == L"taglia" || t == L"cut") return menu::I_TAGLIA;
    if (v == L"copy" || t == L"copia" || t == L"copy") return menu::I_COPIA;
    if (v == L"paste" || t == L"incolla" || t == L"paste") return menu::I_INCOLLA;
    if (v == L"rename" || t == L"rinomina" || t == L"rename") return menu::I_RINOMINA;
    if (v.find(L"modernshare") != std::wstring::npos || t == L"condividi" || t == L"share") return menu::I_CONDIVIDI;
    if (v == L"delete" || t == L"elimina" || t == L"delete") return menu::I_ELIMINA;
    return 0;
}

// icona piccola della voce (menu::Icona), dal verbo o dal testo
inline int simbolo(const std::wstring& verbo, const std::wstring& testo) {
    const std::wstring v = minuscolo(verbo), t = pulito(testo);
    auto inizia = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (t.rfind(x, 0) == 0) return true; return false; };
    auto contiene = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (t.find(x) != std::wstring::npos) return true; return false; };
    if (v == L"properties" || t == L"proprietà" || t == L"properties") return menu::I_PROPRIETA;
    if (v == L"runas" || v == L"runasuser" || inizia({L"esegui come", L"run as"})) return menu::I_ADMIN;
    if (inizia({L"apri nel terminale", L"open in terminal"})) return menu::I_TERMINALE;
    if (inizia({L"apri con", L"open with"})) return menu::I_APRI_CON;
    if (inizia({L"apri percorso", L"open file location", L"apri cartella", L"open folder"})) return menu::I_CARTELLA;
    if (v == L"open" || v == L"opennew" || v == L"explore" || inizia({L"apri", L"open"})) return menu::I_APRI;
    if (inizia({L"aggiungi a preferiti", L"add to favorites", L"rimuovi da preferiti", L"remove from favorites", L"aggiungi ad accesso rapido", L"pin to quick access"})) return menu::I_PREFERITI;
    if (inizia({L"aggiungi a start", L"pin to start", L"rimuovi da start", L"unpin from start"})) return menu::I_START;
    if (inizia({L"aggiungi alla barra", L"pin to taskbar", L"rimuovi dalla barra", L"unpin from taskbar"})) return menu::I_BARRA;
    if (v == L"copyaspath" || inizia({L"copia come percorso", L"copy as path"})) return menu::I_PERCORSO;
    if (contiene({L"copilot"})) return menu::I_COPILOT;
    if (v == L"edit" || inizia({L"modifica", L"edit"})) return menu::I_MODIFICA;
    if (contiene({L"zip", L"comprimi", L"compress", L"estrai", L"extract", L"winrar", L"7-zip"})) return menu::I_ZIP;
    if (v == L"view" || inizia({L"visualizza", L"view"})) return menu::I_VISTA;
    if (v == L"arrange" || inizia({L"ordina", L"sort"})) return menu::I_ORDINA;
    if (v == L"groupby" || inizia({L"raggruppa", L"group"})) return menu::I_GRUPPI;
    if (v == L"refresh" || inizia({L"aggiorna", L"refresh"})) return menu::I_AGGIORNA;
    if (v == L"undo" || v == L"redo" || inizia({L"annulla", L"ripeti", L"undo", L"redo"})) return menu::I_ANNULLA;
    if (inizia({L"nuovo", L"new"})) return menu::I_NUOVO;
    if (inizia({L"impostazioni schermo", L"display settings"})) return menu::I_SCHERMO;
    if (inizia({L"personalizza", L"personalize"})) return menu::I_PERSONALIZZA;
    if (inizia({L"invia a", L"send to", L"dare accesso", L"concedi accesso", L"give access"})) return menu::I_INVIA;
    if (v == L"link" || inizia({L"crea collegamento", L"create shortcut"})) return menu::I_COLLEGAMENTO;
    if (v == L"print" || inizia({L"stampa", L"print"})) return menu::I_STAMPA;
    if (contiene({L"defender", L"analizza", L"scan"})) return menu::I_SCUDO;
    if (inizia({L"imposta come sfondo", L"set as desktop background"})) return menu::I_SCHERMO;
    if (inizia({L"ripristina versioni", L"restore previous"})) return menu::I_ANNULLA;
    return 0;
}

// percorso del file selezionato, se e' uno solo
inline std::wstring percorsoSelezionato(IFolderView2* fv) {
    std::wstring r;
    IShellItemArray* a = nullptr; DWORD n = 0;
    if (fv && SUCCEEDED(fv->GetSelection(FALSE, &a)) && a && SUCCEEDED(a->GetCount(&n)) && n == 1) {
        IShellItem* it = nullptr; LPWSTR s = nullptr;
        if (SUCCEEDED(a->GetItemAt(0, &it)) && it && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &s)) && s) { r = s; CoTaskMemFree(s); }
        if (it) it->Release();
    }
    if (a) a->Release();
    return r;
}

// voce del menu compatto di Windows 11? (file: Apri, Apri con, Copia come percorso, Aggiungi a Preferiti, Modifica con,
// Apri nel Terminale...; sfondo: Visualizza, Ordina per, Raggruppa per, Aggiorna, Annulla, Nuovo, Impostazioni
// schermo, Personalizza...). Il resto va sotto la freccia
inline bool principale(const std::wstring& verbo, const std::wstring& testo, bool file) {
    const std::wstring v = minuscolo(verbo), t = pulito(testo);
    auto inizia = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (t.rfind(x, 0) == 0) return true; return false; };
    auto uguale = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (t == x) return true; return false; };
    if (v == L"properties" || uguale({L"proprietà", L"properties"})) return true;
    if (inizia({L"apri nel terminale", L"open in terminal"})) return true;
    if (file) {
        for (auto x : {L"open", L"opennew", L"openas", L"runas", L"openinnewtab", L"openinnewwindow", L"opennewwindow", L"opennewtab",
                       L"setdesktopwallpaper", L"copyaspath", L"pintohome", L"pintostartscreen", L"unpinfromstartscreen", L"taskbarpin", L"taskbarunpin",
                       L"opencontaining", L"play", L"preview"}) if (v == x) return true;
        if (uguale({L"apri", L"open", L"esegui come amministratore", L"run as administrator", L"copia come percorso", L"copy as path",
                    L"aggiungi a preferiti", L"add to favorites", L"rimuovi da preferiti", L"remove from favorites", L"riproduci", L"play"})) return true;
        if (inizia({L"apri con", L"open with", L"apri in ", L"open in ", L"apri percorso", L"open file location", L"imposta come sfondo", L"set as desktop background",
                    L"comprimi in", L"compress to", L"estrai tutto", L"extract all", L"aggiungi ad accesso rapido", L"pin to quick access", L"aggiungi a start", L"pin to start",
                    L"rimuovi da start", L"unpin from start", L"aggiungi alla barra", L"pin to taskbar", L"rimuovi dalla barra", L"unpin from taskbar",
                    L"modifica con", L"modifica nel", L"modifica in", L"edit with", L"edit in", L"chiedi a copilot", L"ask copilot", L"ruota", L"rotate"})) return true;
        return false;
    }
    for (auto x : {L"view", L"arrange", L"groupby", L"refresh", L"undo", L"redo", L"pastelink", L"display", L"personalize"}) if (v == x) return true;
    if (uguale({L"visualizza", L"view", L"ordina per", L"sort by", L"raggruppa per", L"group by", L"aggiorna", L"refresh", L"nuovo", L"new",
                L"impostazioni schermo", L"display settings", L"personalizza", L"personalize", L"incolla collegamento", L"paste shortcut"})) return true;
    if (inizia({L"annulla", L"ripeti", L"undo", L"redo"})) return true;
    return false;
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
    IShellView* sv = vistaDi(sotto, desktop);
    if (!sv) { if (registro) registro("tasto destro: vista della shell non trovata, menu di Windows"); return false; }
    bool ok = false;
    IFolderView2* fv = nullptr;
    sv->QueryInterface(IID_PPV_ARGS(&fv));
    std::wstring nome;
    int cosa = 0, indiceDesktop = -1;
    if (desktop) {
        // desktop: UI Automation non vede le icone come elementi; si confronta il punto con la posizione di ognuna
        POINT pc = pt; ScreenToClient(sotto, &pc);
        POINT passo{}; if (fv) fv->GetSpacing(&passo);
        int n = 0; if (fv) fv->ItemCount(SVGIO_ALLVIEW, &n);
        for (int i = 0; i < n && i < 5000 && passo.x > 0; i++) {
            PITEMID_CHILD pid = nullptr; POINT pos{};
            if (FAILED(fv->Item(i, &pid)) || !pid) continue;
            const bool c = SUCCEEDED(fv->GetItemPosition(pid, &pos));
            CoTaskMemFree(pid);
            if (c && pc.x >= pos.x && pc.x < pos.x + passo.x && pc.y >= pos.y && pc.y < pos.y + passo.y) { indiceDesktop = i; break; }
        }
        cosa = indiceDesktop >= 0 ? 1 : 0;
    } else {
        cosa = sottoIlPunto(pt, nome);
        if (cosa < 0) { if (fv) fv->Release(); sv->Release(); if (registro) registro("tasto destro: punto non riconosciuto, menu di Windows"); return false; }
    }
    if (cosa == 1 && indiceDesktop >= 0) {
        DWORD stato = 0; PITEMID_CHILD pid = nullptr;
        if (SUCCEEDED(fv->Item(indiceDesktop, &pid)) && pid) { fv->GetSelectionState(pid, &stato); CoTaskMemFree(pid); }
        if (!(stato & SVSI_SELECT)) fv->SelectItem(indiceDesktop, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
    } else if (cosa == 1) {
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
        if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, 0x7EFF, CMF_NORMAL | CMF_CANRENAME))) {
            IContextMenu2* cm2 = nullptr; IContextMenu3* cm3 = nullptr;
            cm->QueryInterface(IID_PPV_ARGS(&cm2)); cm->QueryInterface(IID_PPV_ARGS(&cm3));
            // come il menu di Windows 11: pulsanti in cima, voci principali (Proprieta' per ultima), poi sotto la
            // freccia tutte le altre, nell'ordine di Windows (i sottomenu restano sottomenu)
            std::vector<menu::Voce> pulsanti, principali, proprieta, altre;
            for (auto& v : menu::daHmenu(m)) {
                if (v.titolo) continue;
                if (v.separatore) { if (!altre.empty() && !altre.back().separatore) altre.push_back(v); continue; }
                wchar_t verbo[128] = L"";
                if (v.id > 0 && !v.sotto && FAILED(cm->GetCommandString(v.id - 1, GCS_VERBW, nullptr, (LPSTR)verbo, 128))) verbo[0] = 0;
                if (const int q = pulsante(verbo, v.testo)) { v.pulsante = q; pulsanti.push_back(v); continue; }
                v.simbolo = simbolo(verbo, v.testo);
                if (!wcscmp(verbo, L"properties") || pulito(v.testo) == L"propriet\u00e0" || pulito(v.testo) == L"properties") { proprieta.push_back(v); continue; }
                if (principale(verbo, v.testo, cosa == 1)) principali.push_back(v);
                else { v.extra = true; altre.push_back(v); }
            }
            while (!altre.empty() && altre.back().separatore) altre.pop_back();
            // "Esegui come amministratore" sempre, per programmi, script e collegamenti (Windows a volte lo mette solo
            // fra le altre opzioni, a volte per niente): se c'e' sotto la freccia sale fra le principali, se manca lo
            // aggiunge FluidWin (ShellExecute "runas" sul file)
            const UINT ID_ADMIN = 0x7F00;
            std::wstring file = cosa == 1 ? percorsoSelezionato(fv) : L"";
            if (!file.empty()) {
                std::wstring est = file.substr(file.find_last_of(L'.') == std::wstring::npos ? file.size() : file.find_last_of(L'.'));
                est = minuscolo(est);
                const bool programma = est == L".exe" || est == L".lnk" || est == L".bat" || est == L".cmd" || est == L".msi" || est == L".com";
                bool c = false;
                for (auto& x : principali) if (x.simbolo == menu::I_ADMIN) c = true;
                for (size_t k = 0; k < altre.size() && !c; k++) if (altre[k].simbolo == menu::I_ADMIN) {
                    menu::Voce x = altre[k]; x.extra = false; altre.erase(altre.begin() + k); c = true;
                    principali.insert(principali.begin() + std::min<size_t>(1, principali.size()), x);
                }
                if (programma && !c) {
                    menu::Voce x = menu::voce(testi::t(L"Esegui come amministratore", L"Run as administrator"), ID_ADMIN);
                    x.simbolo = menu::I_ADMIN;
                    principali.insert(principali.begin() + std::min<size_t>(1, principali.size()), x);
                }
            } else file.clear();
            for (auto& x : altre) x.extra = true;
            // l'ordine dei pulsanti come in Windows 11
            std::stable_sort(pulsanti.begin(), pulsanti.end(), [](const menu::Voce& x, const menu::Voce& y) {
                static const int ordine[] = {0, 1, 2, 3, 4, 5, 6};   // I_TAGLIA, I_COPIA, I_INCOLLA, I_RINOMINA, I_CONDIVIDI, I_ELIMINA
                return ordine[x.pulsante] < ordine[y.pulsante];
            });
            std::vector<menu::Voce> voci = pulsanti;
            for (auto& v : principali) voci.push_back(v);
            if (!proprieta.empty()) { if (!principali.empty()) voci.push_back(menu::separatore()); for (auto& v : proprieta) voci.push_back(v); }
            for (auto& v : altre) voci.push_back(v);
            auto prepara = [cm2, cm3](HMENU s) {   // i sottomenu della shell ("Invia a", "Apri con", "Nuovo") si riempiono ora
                if (cm3) { LRESULT r = 0; cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)s, 0, &r); }
                else if (cm2) cm2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)s, 0);
            };
            if (!voci.empty()) {
                ok = true;
                if (registro) registro(cosa == 1 ? "tasto destro: menu di FluidWin (file)" : "tasto destro: menu di FluidWin (sfondo)");
                SetForegroundWindow(proprietario);
                const UINT cmd = menu::mostra(voci, pt, proprietario, false, prepara);
                if (cmd == ID_ADMIN) ShellExecuteW(radice, L"runas", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                else if (cmd > 0) {
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
