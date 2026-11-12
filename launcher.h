// Launcher di FluidWin: tutte le app con la ricerca (tasto Win o icona "App" del dock). Metodi di dock::Motore,
// dichiarati in dock.h (che include questo file).
#pragma once

namespace dock {

// ---- launcher: tutte le app, con la ricerca (tasto Win o icona "App" del dock) ----

inline DWORD WINAPI Motore::threadIcone(LPVOID p) {
    Motore* m = (Motore*)p;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (!m->fineIcone) {
        WaitForSingleObject(m->eventoIcone, INFINITE);
        while (!m->fineIcone) {
            std::pair<std::wstring, int> r;
            EnterCriticalSection(&m->csIcone);
            const bool c = !m->iconeDaFare.empty();
            if (c) { r = m->iconeDaFare.front(); m->iconeDaFare.pop_front(); }
            LeaveCriticalSection(&m->csIcone);
            if (!c) break;
            IconaPronta ip; ip.percorso = r.first; ip.lato = r.second;
            ip.ok = pixelIcona(r.first, r.second, ip.w, ip.h, ip.px);
            EnterCriticalSection(&m->csIcone); m->iconePronte.push_back(std::move(ip)); LeaveCriticalSection(&m->csIcone);
            PostMessageW(m->icone, WM_ICONA_PRONTA, 0, 0);
        }
    }
    CoUninitialize();
    return 0;
}

inline auto Motore::chiaveIcona(const std::wstring& percorso, int lato) -> std::wstring { return std::to_wstring(lato) + L"|" + percorso; }

inline DWORD WINAPI Motore::threadSottofondo(LPVOID p) {
    Sottofondo* l = (Sottofondo*)p;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l->fai();
    CoUninitialize();
    if (!PostMessageW(l->dest, WM_LAVORO_FATTO, 0, (LPARAM)l)) delete l;   // FluidWin si sta chiudendo
    return 0;
}

inline auto Motore::inSottofondo(std::function<void()> fai, std::function<void()> applica) -> void {
    Sottofondo* l = new Sottofondo{std::move(fai), std::move(applica), icone};
    // nel pool di thread (vedi pool.h); se non parte, subito qui, come prima
    if (!pool::lancia(threadSottofondo, l, THREAD_PRIORITY_BELOW_NORMAL, true)) { l->fai(); l->applica(); delete l; }
}

inline auto Motore::chiediIcona(const std::wstring& percorso, int lato) -> void {
    if (percorso.empty() || !iconeChieste.insert(chiaveIcona(percorso, lato)).second) return;
    if (!csIconeOk) {
        InitializeCriticalSection(&csIcone); csIconeOk = true;
        eventoIcone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        threadIconeH = CreateThread(nullptr, 0, threadIcone, this, 0, nullptr);
        if (threadIconeH) SetThreadPriority(threadIconeH, THREAD_PRIORITY_BELOW_NORMAL);
    }
    EnterCriticalSection(&csIcone); iconeDaFare.push_back({percorso, lato}); LeaveCriticalSection(&csIcone);
    iconeAttese++;
    SetEvent(eventoIcone);
}

// all'uscita il thread si ferma (dopo l'icona che sta preparando) prima che il Motore sparisca
inline auto Motore::fermaIcone() -> void {
    if (!threadIconeH) return;
    fineIcone = 1; SetEvent(eventoIcone);
    WaitForSingleObject(threadIconeH, 2000);
    CloseHandle(threadIconeH); threadIconeH = nullptr;
}

inline auto Motore::sorgenteIcona(const Voce& v) -> std::wstring {
    if (v.avvio == AVVIO_PILA) return cartellaDownload();   // l'icona della cartella Download
    if (!v.pacchetto.empty()) return v.pacchetto;             // app di pacchetto fissata (vedi Voce::pacchetto)
    return v.fissata ? v.avvio : (v.avvio.rfind(L"shell:AppsFolder\\", 0) == 0 ? v.avvio : v.exe);
}

// le icone arrivate dal thread diventano bitmap (veloce) e vanno a chi le aspetta: true se ne e' arrivata qualcuna
inline auto Motore::raccogliIcone() -> bool {
    if (!csIconeOk) return false;
    std::vector<IconaPronta> arrivate;
    EnterCriticalSection(&csIcone); arrivate.swap(iconePronte); LeaveCriticalSection(&csIcone);
    for (auto& ip : arrivate) {
        iconeAttese--;
        ID2D1Bitmap1* b = ip.ok ? bitmapDaPixel(ip.w, ip.h, ip.px) : nullptr;
        bool presa = false;
        if (ip.lato == 256) {          // dock
            for (auto& v : voci) if (!v.icona && v.avvio != AVVIO_LAUNCHER && v.avvio != AVVIO_CONTROLLO && sorgenteIcona(v) == ip.percorso) { v.icona = b; presa = true; dirty = true; break; }
            // (si potra' richiedere: l'app puo' chiudersi e riaprirsi; se e' fallita si riprova al prossimo aggiornamento)
            iconeChieste.erase(chiaveIcona(ip.percorso, ip.lato));
        } else if (ip.lato == 128) {   // app in background (se non c'e', la piccola foto salvata da Windows)
            if (!b) b = (ID2D1Bitmap1*)fotoIconaSfondo(ip.percorso);
            cacheIcone[ip.percorso] = b; presa = b != nullptr;
            for (auto& a : appSfondo) if (a.exe == ip.percorso) a.icona = b;
            pDirty = true; altra.pDirty = true;
        } else if (ip.lato == 96) {    // launcher
            for (auto& a : tutteApp) if (a.avvio == ip.percorso && !a.icona) { a.icona = b; presa = true; lDirty = true; break; }
        } else if (ip.lato == 64) {    // notifiche
            const std::wstring pre = L"shell:AppsFolder\\";
            if (ip.percorso.rfind(pre, 0) == 0) { iconeN[ip.percorso.substr(pre.size())] = b; presa = true; }
            else if (auto it = iconeNDaExe.find(ip.percorso); it != iconeNDaExe.end()) { iconeN[it->second] = b; presa = true; }
            pDirty = true; altra.pDirty = true;
        }
        if (b && !presa) b->Release();
    }
    return !arrivate.empty();
}

inline auto Motore::launcherDaDisegnare() const -> bool { return lAperto && (lDirty || lAnim); }

inline auto Motore::creaLauncher(HINSTANCE inst) -> bool {
    LW = std::ceil(760 * dpi); LH = LHMAX = std::ceil(580 * dpi); LEXTRA = std::ceil(160 * dpi);
    WNDCLASSW wl{}; wl.lpfnWndProc = procLauncher; wl.hInstance = inst; wl.lpszClassName = L"FluidWin.Launcher";
    wl.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wl);
    launcher = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wl.lpszClassName, L"App",
                               WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
    vetroLauncher = creaVetro(inst);
    BOOL niente = TRUE; DwmSetWindowAttribute(launcher, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fab = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd); dd->GetAdapter(&ad); ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fab);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)LW; sd.Height = (UINT)(LH + LEXTRA); sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    const HRESULT hr = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &scL);
    fab->Release(); ad->Release(); dd->Release();
    if (FAILED(hr)) return false;
    dcomp->CreateTargetForHwnd(launcher, TRUE, &targetL);
    dcomp->CreateVisual(&visualL); visualL->SetContent(scL);
    dcomp->CreateEffectGroup(&effettoL); visualL->SetEffect(effettoL);
    targetL->SetRoot(visualL); dcomp->Commit();
    IDXGISurface* sup = nullptr; scL->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup);
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglioL); sup->Release();
    return bersaglioL != nullptr;
}

// solo le app vere: niente disinstallazioni, documenti, pagine web e guide
inline auto Motore::appUtile(const std::wstring& nome, const std::wstring& par) -> bool {
    const std::wstring n = minuscolo(nome), p = minuscolo(par);
    if (n.find(L"uninstall") != std::wstring::npos || n.find(L"disinstall") != std::wstring::npos) return false;
    if (p.rfind(L"http", 0) == 0) return false;
    const size_t barra = p.find_last_of(L"\\/"), punto = p.rfind(L'.');
    if (barra == std::wstring::npos) return true;   // AppUserModelID (app dello Store e molte app desktop)
    if (punto == std::wstring::npos || punto < barra) return true;
    const std::wstring est = p.substr(punto + 1);
    return est == L"exe" || est == L"msc" || est == L"lnk" || est == L"cpl";
}

inline auto Motore::leggiTutteApp(bool subito) -> void {
    lLetto = adesso();
    if (subito) { Cronometro cr(this, "leggiTutteApp"); applicaTutteApp(elencaTutteApp()); return; }
    if (lavoroTutte) return;
    lavoroTutte = true;
    auto ris = std::make_shared<std::vector<AppL>>();
    inSottofondo([ris] { *ris = elencaTutteApp(); }, [this, ris] { lavoroTutte = false; applicaTutteApp(std::move(*ris)); });
}

inline auto Motore::applicaTutteApp(std::vector<AppL> nuove) -> void {
    // le icone gia' caricate si conservano
    for (auto& a : nuove) for (auto& v : tutteApp) if (v.avvio == a.avvio) { a.icona = v.icona; a.provata = v.provata; v.icona = nullptr; break; }
    for (auto& v : tutteApp) if (v.icona) v.icona->Release();
    tutteApp = std::move(nuove);
    // launcher aperto (l'elenco nuovo arriva mentre si guarda): la scelta e lo scorrimento restano dove sono
    const int sel = lSel; const double scr = lScrollObj;
    filtra();
    if (lAperto) { lSel = std::clamp(sel, 0, std::max(0, (int)filtrate.size() - 1)); lScrollObj = std::clamp(scr, 0.0, lScrollMax()); }
    for (auto& a : tutteApp) if (!a.icona && !a.provata) { a.provata = true; chiediIcona(a.avvio, 96); }
    if (registro) { char b[64]; snprintf(b, 64, "launcher: %d app", (int)tutteApp.size()); scrivi(b); }
}

inline auto Motore::elencaTutteApp() -> std::vector<AppL> {
    std::vector<AppL> nuove;
    IShellItem* cart = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&cart)))) return nuove;
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(cart->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem* it = nullptr;
        while (en->Next(1, &it, nullptr) == S_OK) {
            LPWSTR n = nullptr, par = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &par)) &&
                n && par && n[0] && appUtile(n, par)) {
                AppL a; a.nome = n; a.avvio = std::wstring(L"shell:AppsFolder\\") + par; a.minuscolo = minuscolo(n);
                nuove.push_back(std::move(a));
            }
            CoTaskMemFree(n); CoTaskMemFree(par); it->Release();
        }
        en->Release();
    }
    cart->Release();
    std::sort(nuove.begin(), nuove.end(), [](const AppL& a, const AppL& b) { return lstrcmpiW(a.nome.c_str(), b.nome.c_str()) < 0; });
    nuove.erase(std::unique(nuove.begin(), nuove.end(), [](const AppL& a, const AppL& b) { return a.minuscolo == b.minuscolo; }), nuove.end());
    return nuove;
}

// con la ricerca: prima i nomi che iniziano con il testo, poi quelli che lo contengono
// Ricerca: prima i nomi che iniziano col testo (o una parola che inizia col testo), poi le iniziali delle parole
// ("vsc" -> Visual Studio Code), poi i nomi che lo contengono, infine quelli che ne contengono le lettere in
// ordine ("clcl" -> Calcolatrice)
inline auto Motore::iniziali(const std::wstring& nome, const std::wstring& q) -> bool {
    size_t k = 0;
    for (size_t i = 0; i < nome.size() && k < q.size(); i++)
        if ((i == 0 || nome[i - 1] == L' ' || nome[i - 1] == L'-') && nome[i] == q[k]) k++;
    return k == q.size();
}

inline auto Motore::sottosequenza(const std::wstring& nome, const std::wstring& q) -> bool {
    size_t k = 0;
    for (size_t i = 0; i < nome.size() && k < q.size(); i++) if (nome[i] == q[k]) k++;
    return k == q.size();
}

inline auto Motore::filtra() -> void {
    filtrate.clear();
    const std::wstring q = minuscolo(query);
    std::vector<int> livelli[3];   // iniziali, contiene, lettere in ordine
    for (int i = 0; i < (int)tutteApp.size(); i++) {
        if (q.empty()) { filtrate.push_back(i); continue; }
        const std::wstring& n = tutteApp[i].minuscolo;
        const size_t p = n.find(q);
        if (p == 0 || (p != std::wstring::npos && n[p - 1] == L' ')) filtrate.push_back(i);
        else if (q.size() >= 2 && iniziali(n, q)) livelli[0].push_back(i);
        else if (p != std::wstring::npos) livelli[1].push_back(i);
        else if (q.size() >= 3 && sottosequenza(n, q)) livelli[2].push_back(i);
    }
    for (auto& l : livelli) filtrate.insert(filtrate.end(), l.begin(), l.end());
    lSel = 0; lScrollObj = 0; lDirty = true;
}

// misure della griglia (coordinate del launcher)
inline auto Motore::lColonne() const -> float { return 6; }

inline auto Motore::lCellaW() const -> float { return (LW - 48 * dpi) / lColonne(); }

inline auto Motore::lCellaH() const -> float { return 104 * dpi; }

// zona della barra di scorrimento che si puo' prendere col mouse (larga 16 punti: la barra disegnata e' sottile) e
// il suo cursore adesso
inline auto Motore::lRettBarra() const -> D2D1_RECT_F { return {LW - 18 * dpi, lGrigliaTop(), LW, lGrigliaBottom()}; }

inline auto Motore::lCursoreBarra() const -> D2D1_RECT_F {
    const double mx = lScrollMax();
    const float alt = lGrigliaBottom() - lGrigliaTop(), tot = alt + (float)mx;
    const float h = std::max(30 * dpi, alt * alt / tot), y = lGrigliaTop() + (mx > 0 ? (alt - h) * (float)(lScroll / mx) : 0);
    return {LW - 18 * dpi, y, LW, y + h};
}

inline auto Motore::lGrigliaTop() const -> float { return 84 * dpi; }

inline auto Motore::lGrigliaBottom() const -> float { return LH - 14 * dpi; }

inline auto Motore::lScrollMax() const -> double {
    const int righe = ((int)filtrate.size() + 5) / 6;
    return std::max(0.0, (double)(righe * lCellaH() - (lGrigliaBottom() - lGrigliaTop())));
}

inline auto Motore::lCella(int i) const -> D2D1_RECT_F {
    const float x = 24 * dpi + (i % 6) * lCellaW(), y = lGrigliaTop() + (i / 6) * lCellaH() - (float)lScroll;
    return {x, y, x + lCellaW(), y + lCellaH()};
}

inline auto Motore::lCellaIn(float x, float y) const -> int {
    if (y < lGrigliaTop() || y > lGrigliaBottom()) return -1;
    for (int i = 0; i < (int)filtrate.size(); i++) {
        const auto r = lCella(i);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

// prende il primo piano anche quando lo apre il tasto Win (nessun clic arrivato al nostro processo)
inline auto Motore::prendiFuoco(HWND w) -> void {
    SetForegroundWindow(w);
    if (GetForegroundWindow() == w) return;
    HWND fg = GetForegroundWindow();
    const DWORD a = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, b = GetCurrentThreadId();
    // (mai agganciati a un'app che non risponde: il suo input bloccato bloccherebbe anche quello di FluidWin)
    if (a && a != b && !IsHungAppWindow(fg) && AttachThreadInput(b, a, TRUE)) { SetForegroundWindow(w); SetFocus(w); AttachThreadInput(b, a, FALSE); }
}

// Apre il launcher (o lo chiude, se e' gia' aperto): cresce dall'icona "App" del dock con un piccolo rimbalzo
inline auto Motore::apriLauncher() -> void {
    if (!attivo) return;
    // anche sopra un gioco o un video a tutto schermo (tasto Win): il dock torna a disegnarsi mentre il launcher e' aperto
    if (nascostoSchermoIntero) schermoIntero(false);
    if (lAperto && !lChiusura) { chiudiLauncher(); return; }
    // si sta chiudendo: torna indietro da dove e' arrivato (stessa molla, stessa velocita'), senza ripartire da capo.
    // Prima c'era anche un blocco di 250 ms dopo la chiusura in cui i clic si ignoravano: non serve piu', il clic
    // sull'icona App non chiude piu' il launcher perdendo il fuoco (vedi WM_ACTIVATE)
    if (lAperto && lChiusura) {
        lChiusura = false; lDirty = true; lRipreso = true;
        prendiFuoco(launcher);
        scrivi("launcher: riaperto mentre si chiudeva");
        return;
    }
    if (tutteApp.empty()) leggiTutteApp(true);
    else if (adesso() - lLetto > 600) leggiTutteApp();
    // centro di controllo aperto: prima si chiude lui, poi (TIMER_DOPO) si apre il launcher; mai uno sopra l'altro
    if (!nessunaScheda()) { if (dopo != 1) { chiudiTutte(); apriDopo(1, 0); } return; }
    primaDelLauncher = GetForegroundWindow();
    query.clear(); filtra(); lSopra = -1; lScroll = 0;
    lAperto = true; lChiusura = false; lScala = 0.12; lVel = 0; lAlfa = 0; lT0 = adesso(); lRegione = false; lRipreso = false;
    // sopra il dock, al centro; la finestra scende fino all'icona "App"
    const int x0 = schermo.left + (int)std::round((W - LW) / 2);
    const int y1 = schermo.bottom - H + (int)std::round(yBaseRiposo() - S - PAD - 18 * dpi);
    // schermi bassi (1366x768 al 125%: ~520 punti sopra il dock): il launcher si accorcia invece di uscire in alto
    LH = std::clamp((float)(y1 - schermo.top) - 12 * dpi, 300 * dpi, LHMAX);
    rettLauncher = {x0, y1 - (int)LH, x0 + (int)LW, y1};
    float ax = W / 2.0f;
    const auto pos = posizioniRiposo();
    for (size_t i = 0; i < voci.size(); i++) if (voci[i].avvio == AVVIO_LAUNCHER) ax = pos[i] + S / 2;
    ancora = {(LONG)std::clamp(schermo.left + ax - x0, 40 * dpi, LW - 40 * dpi), (LONG)std::min(LH + LEXTRA - 4, (float)(schermo.bottom - H + yBaseRiposo() - S / 2 - rettLauncher.top))};
    SetWindowRgn(launcher, nullptr, FALSE);
    SetWindowPos(launcher, HWND_TOPMOST, x0, rettLauncher.top, (int)LW, (int)(LH + LEXTRA), SWP_NOACTIVATE);
    posizionaLauncher();
    ShowWindow(vetroLauncher, SW_SHOWNOACTIVATE);
    ShowWindow(launcher, SW_SHOW);
    prendiFuoco(launcher);
    lDirty = true; dirty = true;
    scrivi(GetForegroundWindow() == launcher ? "launcher aperto" : "launcher aperto senza fuoco");
}

inline auto Motore::chiudiLauncher() -> void {
    if (!lAperto || lChiusura) return;
    lChiusura = true; lDirty = true;
    if (lRegione) { SetWindowRgn(launcher, nullptr, FALSE); lRegione = false; }   // per rientrare nell'icona serve tutta la finestra
}

inline auto Motore::nascondiLauncher() -> void {
    // chiuso senza aprire un'app (Esc, Win, clic sul dock): il primo piano torna alla finestra di prima
    if (lAperto && GetForegroundWindow() == launcher && primaDelLauncher && IsWindow(primaDelLauncher) && IsWindowVisible(primaDelLauncher))
        SetForegroundWindow(primaDelLauncher);
    primaDelLauncher = nullptr;
    if (launcher) ShowWindow(launcher, SW_HIDE);
    if (vetroLauncher) ShowWindow(vetroLauncher, SW_HIDE);
    if (lAperto) lChiusoT = adesso();
    lAperto = false; lChiusura = false; lAnim = false; lDirty = false;
    dirty = true;   // il dock puo' tornare a nascondersi
}

// il contenuto (DirectComposition) e il vetro crescono attorno all'icona "App"
inline auto Motore::posizionaLauncher() -> void {
    const double s = lScala;
    const float ax = (float)ancora.x, ay = (float)ancora.y;
    visualL->SetTransform(D2D1::Matrix3x2F::Scale((float)s, (float)s, {ax, ay}));
    effettoL->SetOpacity((float)std::clamp(lAlfa, 0.0, 1.0));
    const RECT& r = rettLauncher;
    const double gx = r.left + ax, gy = r.top + ay;
    const int L = (int)std::lround(gx + (r.left - gx) * s), T = (int)std::lround(gy + (r.top - gy) * s);
    const int R = (int)std::lround(gx + (r.right - gx) * s), B = (int)std::lround(gy + (r.bottom - gy) * s);
    SetWindowPos(vetroLauncher, launcher, L, T, std::max(1, R - L), std::max(1, B - T), SWP_NOACTIVATE);
    SetLayeredWindowAttributes(vetroLauncher, 0, (BYTE)std::lround(std::clamp(lAlfa, 0.0, 1.0) * 255), LWA_ALPHA);
    dcomp->Commit();
}

inline auto Motore::avviaApp(int i) -> void {
    if (i < 0 || i >= (int)filtrate.size()) return;
    const AppL& a = tutteApp[filtrate[i]];
    primaDelLauncher = nullptr;   // il primo piano va all'app che si apre
    chiudiLauncher();
    ShellExecuteW(nullptr, L"open", a.avvio.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

inline auto Motore::rettEnergia() const -> D2D1_RECT_F { return {LW - 64 * dpi, 22 * dpi, LW - 24 * dpi, 62 * dpi}; }

// Menu di Windows per l'app (lo stesso dello Start), con in cima "Aggiungi al dock"
inline auto Motore::menuApp(int i, POINT pt) -> void {
    if (i < 0 || i >= (int)filtrate.size()) return;
    const AppL a = tutteApp[filtrate[i]];
    IShellItem* it = nullptr;
    if (FAILED(SHCreateItemFromParsingName(a.avvio.c_str(), nullptr, IID_PPV_ARGS(&it)))) return;
    IContextMenu* cm = nullptr;
    it->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm));
    // eseguibile e AppUserModelID dell'app, per il collegamento nel dock
    std::wstring exe, id;
    IShellItem2* it2 = nullptr;
    if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2)))) {
        LPWSTR s = nullptr;
        if (SUCCEEDED(it2->GetString(PKEY_Link_TargetParsingPath, &s)) && s) { exe = s; CoTaskMemFree(s); }
        s = nullptr;
        if (SUCCEEDED(it2->GetString(PKEY_AppUserModel_ID, &s)) && s) { id = s; CoTaskMemFree(s); }
        it2->Release();
    }
    it->Release();
    if (!cm) return;
    const UINT ID_DOCK = 0x7F00;
    HMENU m = CreatePopupMenu();
    if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, ID_DOCK - 1, CMF_NORMAL))) {
        cm->QueryInterface(IID_PPV_ARGS(&menuShell2)); cm->QueryInterface(IID_PPV_ARGS(&menuShell3));
        // le voci di Windows, disegnate da noi; in cima il nome dell'app e "Aggiungi al dock"
        std::vector<menu::Voce> voci = {menu::titolo(a.nome), menu::voce(testi::t(L"Aggiungi al dock", L"Add to dock"), ID_DOCK)};
        // "Disinstalla" (la voce di Windows, per le app dello Store e per le altre) in cima, subito visibile
        UINT idDisinstalla = 0;
        for (int i = 0; i < GetMenuItemCount(m) && !idDisinstalla; i++) {
            const UINT id = GetMenuItemID(m, i);
            if (id == (UINT)-1 || id == 0) continue;
            wchar_t v[64] = L"";
            if (SUCCEEDED(cm->GetCommandString(id - 1, GCS_VERBW, nullptr, (LPSTR)v, 64)) && !_wcsicmp(v, L"Uninstall")) idDisinstalla = id;
        }
        if (idDisinstalla) voci.push_back(menu::voce(testi::t(L"Disinstalla…", L"Uninstall…"), idDisinstalla));
        voci.push_back(menu::separatore());
        for (auto& v : menu::daHmenu(m)) if (v.id != idDisinstalla || v.separatore) voci.push_back(v);
        // i sottomenu ("Invia a", "Apri con"...) la shell li riempie solo quando stanno per aprirsi
        auto prepara = [this](HMENU s) {
            if (menuShell3) { LRESULT r = 0; menuShell3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)s, 0, &r); }
            else if (menuShell2) menuShell2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)s, 0);
        };
        const UINT cmd = menu::mostra(voci, pt, launcher, false, prepara);
        if (menuShell3) { menuShell3->Release(); menuShell3 = nullptr; }
        if (menuShell2) { menuShell2->Release(); menuShell2 = nullptr; }
        if (cmd == ID_DOCK) {
            aggiungiAlDock(a.nome, exe, id, a.avvio);
        } else if (cmd > 0) {
            CMINVOKECOMMANDINFOEX ci{}; ci.cbSize = sizeof(ci); ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            ci.hwnd = launcher; ci.lpVerb = MAKEINTRESOURCEA(cmd - 1); ci.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
            ci.nShow = SW_SHOWNORMAL; ci.ptInvoke = pt;
            cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci);
            primaDelLauncher = nullptr;   // ha aperto qualcosa: il primo piano va a quello
            chiudiLauncher();
            appDaAggiornare = true;       // "Aggiungi alla barra" e simili cambiano le icone fisse del dock
        }
    }
    DestroyMenu(m);
    cm->Release();
}

// Collegamento nella cartella delle app fisse della barra (quella che legge il dock)
inline auto Motore::aggiungiAlDock(const std::wstring& nome, const std::wstring& exe, const std::wstring& id, const std::wstring& avvio) -> void {
    wchar_t cart[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, cart))) return;
    std::wstring n = nome;
    for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    const std::wstring file = std::wstring(cart) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\" + n + L".lnk";
    if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return;   // gia' nel dock
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    bool ok = false;
    if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) ok = SUCCEEDED(sl->SetPath(exe.c_str()));
    else {   // app dello Store: il collegamento punta alla voce della cartella App
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(avvio.c_str(), nullptr, &pidl, 0, nullptr))) { ok = SUCCEEDED(sl->SetIDList(pidl)); CoTaskMemFree(pidl); }
    }
    if (ok && !id.empty()) {   // cosi' il dock riconosce le finestre dell'app
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) {
            PROPVARIANT pv; if (SUCCEEDED(InitPropVariantFromString(id.c_str(), &pv))) { ps->SetValue(PKEY_AppUserModel_ID, pv); ps->Commit(); PropVariantClear(&pv); }
            ps->Release();
        }
    }
    IPersistFile* pf = nullptr;
    if (ok && SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(file.c_str(), TRUE); pf->Release(); }
    sl->Release();
    appDaAggiornare = true;
    if (registro) { char b[200]; snprintf(b, 200, "aggiunta al dock: %ls", n.c_str()); scrivi(b); }
}

// Accensione: come il pulsante dello Start
inline auto Motore::menuEnergia(POINT pt) -> void {
    const int s = (int)menu::mostra({menu::voce(testi::t(L"Sospendi", L"Sleep"), 1), menu::voce(testi::t(L"Riavvia", L"Restart"), 2),
                                     menu::voce(testi::t(L"Arresta il sistema", L"Shut down"), 3), menu::separatore(),
                                     menu::voce(testi::t(L"Blocca", L"Lock"), 4), menu::voce(testi::t(L"Disconnetti", L"Sign out"), 5)}, pt, launcher);
    lSopraEnergia = false; lDirty = true;
    if (s <= 0) return;
    chiudiLauncher();
    energia(s);
}

inline auto Motore::energia(int s) -> void {
    if (s == 4) { LockWorkStation(); return; }
    // privilegio di arresto: ce l'ha ogni utente, va solo attivato per questo processo
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    const DWORD motivo = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
    // le app con lavoro non salvato possono fermare riavvio e arresto (nessun EWX_FORCE), come dallo Start
    if (s == 1) SetSuspendState(FALSE, FALSE, FALSE);
    else if (s == 2) ExitWindowsEx(EWX_REBOOT, motivo);
    else if (s == 3) ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | 0x00400000 /* EWX_HYBRID_SHUTDOWN: avvio rapido */, motivo);
    else if (s == 5) ExitWindowsEx(EWX_LOGOFF, 0);
}

inline auto Motore::selezionaVisibile() -> void {
    const auto r = lCella(lSel);
    const float top = lGrigliaTop(), bot = lGrigliaBottom();
    if (r.top < top) lScrollObj -= top - r.top;
    else if (r.bottom > bot) lScrollObj += r.bottom - bot;
    lScrollObj = std::clamp(lScrollObj, 0.0, lScrollMax());
}

// icona "App" nel dock: il logo di Windows 11 (quattro riquadri azzurri sfumati) su vetro chiaro
inline auto Motore::disegnaIconaApp(const D2D1_RECT_F& r, float alfa, bool scuro) -> void {
    const float d = r.right - r.left;
    pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.16f * alfa) : D2D1::ColorF(1, 1, 1, 0.82f * alfa));
    dc->FillRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello);
    pennello->SetColor(D2D1::ColorF(0, 0, 0, (scuro ? 0.3f : 0.08f) * alfa));
    dc->DrawRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello, 1.0f);
    const float lato = d * 0.50f, fessura = d * 0.028f, q = (lato - fessura) / 2;
    const float x0 = (r.left + r.right - lato) / 2, y0 = (r.top + r.bottom - lato) / 2;
    // una sola sfumatura su tutto il logo, dall'azzurro chiaro in alto a sinistra al blu in basso a destra
    const D2D1_GRADIENT_STOP fermate[2] = {{0, D2D1::ColorF(0.33f, 0.80f, 1.00f, alfa)}, {1, D2D1::ColorF(0.00f, 0.42f, 0.86f, alfa)}};
    ID2D1GradientStopCollection* fc = nullptr; ID2D1LinearGradientBrush* sf = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(fermate, 2, &fc)))
        dc->CreateLinearGradientBrush({{x0, y0}, {x0 + lato, y0 + lato}}, fc, &sf);
    if (!sf) pennello->SetColor(D2D1::ColorF(0.10f, 0.58f, 0.95f, alfa));
    for (int k = 0; k < 4; k++) {
        const float x = x0 + (k % 2) * (q + fessura), y = y0 + (k / 2) * (q + fessura);
        const D2D1_RECT_F p{x, y, x + q, y + q};
        const float ra = d * 0.012f;
        if (sf) dc->FillRoundedRectangle({p, ra, ra}, sf); else dc->FillRoundedRectangle({p, ra, ra}, pennello);
    }
    if (sf) sf->Release();
    if (fc) fc->Release();
}

// restituisce false se il launcher si e' appena chiuso
inline auto Motore::renderLauncher() -> bool {
    // scala con una molla: apre con un piccolo rimbalzo (gelatina), rientra nell'icona rapida e senza
    // in chiusura lo stesso movimento all'indietro: rientra nell'icona con la molla e sfuma solo nell'ultimo tratto
    bool anim = molla(lScala, lVel, lChiusura ? 0.0 : 1.0, 240, lChiusura ? 0.8 : 0.55, 0.0005);
    if (lChiusura) {
        lAlfa = std::clamp((lScala - 0.10) / 0.30, 0.0, 1.0);
        if (lAlfa <= 0.02 || !anim) { nascondiLauncher(); return false; }
    } else {
        lAlfa = verso(lAlfa, 1.0, 0.045);
        if (lAlfa != 1.0) anim = true;
    }
    posizionaLauncher();
    if (!lChiusura && !anim && !lRegione) {
        // aperto: fuori dal launcher (sopra il dock) i clic devono passare
        SetWindowRgn(launcher, CreateRectRgn(0, 0, (int)LW, (int)LH), FALSE); lRegione = true;
    }
    const double sc = verso(lScroll, lScrollObj, 0.06);
    if (sc != lScroll) { lScroll = sc; anim = true; }
    raccogliIcone();   // (quelle che arrivano dal thread rimettono lDirty)

    const bool scuro = temaScuro();
    const D2D1_COLOR_F ink = scuro ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.88f);
    dc->SetTarget(bersaglioL);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    disegnaFondo({0.5f, 0.5f, LW - 0.5f, LH - 0.5f}, scuro);

    // pulsante di accensione (sospendi, riavvia, arresta, blocca, disconnetti) a destra della ricerca
    {
        const D2D1_RECT_F re = rettEnergia();
        const float cx = (re.left + re.right) / 2, cy = (re.top + re.bottom) / 2, rr = (re.bottom - re.top) / 2;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, lSopraEnergia ? 0.18f : 0.10f) : D2D1::ColorF(1, 1, 1, lSopraEnergia ? 0.85f : 0.6f));
        dc->FillEllipse({{cx, cy}, rr, rr}, pennello);
        pennello->SetColor(ink);
        simboloEnergia(cx, cy);
    }
    // campo di ricerca
    const D2D1_RECT_F rc = {24 * dpi, 22 * dpi, rettEnergia().left - 10 * dpi, 62 * dpi};
    pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.6f));
    dc->FillRoundedRectangle({rc, 10 * dpi, 10 * dpi}, pennello);
    pennello->SetColor({ink.r, ink.g, ink.b, 0.55f});
    const float lx = rc.left + 22 * dpi, ly = (rc.top + rc.bottom) / 2;
    dc->DrawEllipse({{lx - 2 * dpi, ly - 2 * dpi}, 6 * dpi, 6 * dpi}, pennello, 1.8f * dpi);
    linea(2.5f, 2.5f, 7, 7, lx, ly);
    const D2D1_RECT_F rt = {lx + 18 * dpi, rc.top, rc.right - 14 * dpi, rc.bottom};
    if (query.empty()) scrivi(testi::t(L"Cerca un'app", L"Search for an app"), fmtTitolo, rt);
    else {
        pennello->SetColor(ink);
        scrivi(query, fmtTitolo, rt);
        // cursore dopo il testo
        const float lt = larghezzaTesto(query, fmtTitolo);
        pennello->SetColor(D2D1::ColorF(0.2f, 0.5f, 1.0f, 0.9f));
        dc->DrawLine({rt.left + lt + 2 * dpi, ly - 9 * dpi}, {rt.left + lt + 2 * dpi, ly + 9 * dpi}, pennello, 1.6f * dpi);
    }

    // griglia delle app
    dc->PushAxisAlignedClip({0, lGrigliaTop() - 4 * dpi, LW, lGrigliaBottom()}, D2D1_ANTIALIAS_MODE_ALIASED);
    const float lato = 56 * dpi;
    for (int i = 0; i < (int)filtrate.size(); i++) {
        const auto r = lCella(i);
        if (r.bottom < lGrigliaTop() - lCellaH() || r.top > lGrigliaBottom() + lCellaH()) continue;
        AppL& a = tutteApp[filtrate[i]];
        const bool sopra = i == lSopra, scelta = i == lSel && !query.empty();
        if (sopra || scelta) {
            pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, sopra ? 0.12f : 0.08f) : D2D1::ColorF(1, 1, 1, sopra ? 0.55f : 0.4f));
            const D2D1_RECT_F h = {r.left + 4 * dpi, r.top + 2 * dpi, r.right - 4 * dpi, r.bottom - 2 * dpi};
            dc->FillRoundedRectangle({h, 8 * dpi, 8 * dpi}, pennello);
            if (scelta) { pennello->SetColor(D2D1::ColorF(0.2f, 0.5f, 1.0f, 0.7f)); dc->DrawRoundedRectangle({h, 8 * dpi, 8 * dpi}, pennello, 1.5f * dpi); }
        }
        const float cx = (r.left + r.right) / 2;
        const D2D1_RECT_F ri = {cx - lato / 2, r.top + 12 * dpi, cx + lato / 2, r.top + 12 * dpi + lato};
        if (a.icona) dc->DrawBitmap(a.icona, ri, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else { pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.5f)); dc->FillRoundedRectangle({ri, lato * 0.22f, lato * 0.22f}, pennello); }
        pennello->SetColor(ink);
        scrivi(a.nome, fmtNome, {r.left + 6 * dpi, ri.bottom + 6 * dpi, r.right - 6 * dpi, ri.bottom + 24 * dpi});
    }
    if (filtrate.empty()) { pennello->SetColor({ink.r, ink.g, ink.b, 0.5f}); scrivi(testi::t(L"Nessuna app trovata", L"No apps found"), fmtNome, {0, lGrigliaTop() + 40 * dpi, LW, lGrigliaTop() + 70 * dpi}); }
    dc->PopAxisAlignedClip();
    // barra di scorrimento sottile, solo se serve; col mouse sopra (o trascinandola) si allarga, come su macOS
    if (lScrollMax() > 0) {
        const D2D1_RECT_F b = lCursoreBarra();
        const bool larga = lSopraBarra || lTrascinoBarra;
        const float w = (larga ? 7.0f : 3.0f) * dpi, x1 = LW - 6 * dpi;
        pennello->SetColor({ink.r, ink.g, ink.b, larga ? 0.42f : 0.25f});
        dc->FillRoundedRectangle({{x1 - w, b.top, x1, b.bottom}, w / 2, w / 2}, pennello);
    }
    controllaGpu(dc->EndDraw());
    dc->SetTarget(bersaglio);
    lAnim = anim; lDirty = false;
    return true;
}

inline LRESULT CALLBACK Motore::procLauncher(HWND h, UINT m, WPARAM w, LPARAM l) {
    Motore* d = gIstanza();
    if (!d) return DefWindowProcW(h, m, w, l);
    const float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
    switch (m) {
        case WM_LBUTTONDOWN: {
            // barra di scorrimento: preso il cursore lo si trascina; un clic sul binario salta di una pagina
            const D2D1_RECT_F b = d->lRettBarra();
            if (d->lScrollMax() > 0 && x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
                const D2D1_RECT_F c = d->lCursoreBarra();
                if (y < c.top || y >= c.bottom) {
                    const double pagina = (b.bottom - b.top) * 0.9;
                    d->lScrollObj = std::clamp(d->lScrollObj + (y < c.top ? -pagina : pagina), 0.0, d->lScrollMax());
                } else {
                    d->lTrascinoBarra = true; d->lPresaY = y; d->lPresaScroll = d->lScroll;
                    SetCapture(h);
                }
                d->lDirty = true;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!d->lMouseDentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, h, 0}; TrackMouseEvent(&tm); d->lMouseDentro = true; }
            if (d->lTrascinoBarra) {
                // la barra segue il mouse 1:1 (niente inseguimento: lScroll = lScrollObj)
                const double alt = d->lGrigliaBottom() - d->lGrigliaTop(), c = d->lCursoreBarra().bottom - d->lCursoreBarra().top;
                const double corsa = std::max(1.0, alt - c);
                d->lScrollObj = d->lScroll = std::clamp(d->lPresaScroll + (y - d->lPresaY) * d->lScrollMax() / corsa, 0.0, d->lScrollMax());
                d->lDirty = true;
                return 0;
            }
            {
                const D2D1_RECT_F b = d->lRettBarra();
                const bool sb = d->lScrollMax() > 0 && x >= b.left && x < b.right && y >= b.top && y < b.bottom;
                if (sb != d->lSopraBarra) { d->lSopraBarra = sb; d->lDirty = true; }
            }
            const int s = d->lCellaIn(x, y);
            if (s != d->lSopra) { d->lSopra = s; d->lDirty = true; }
            const D2D1_RECT_F re = d->rettEnergia();
            const bool e = x >= re.left && x < re.right && y >= re.top && y < re.bottom;
            if (e != d->lSopraEnergia) { d->lSopraEnergia = e; d->lDirty = true; }
            return 0;
        }
        case WM_MOUSELEAVE: d->lMouseDentro = false; d->lSopra = -1; d->lSopraEnergia = false; d->lSopraBarra = false; d->lDirty = true; return 0;
        case WM_LBUTTONUP: {
            if (d->lTrascinoBarra) { d->lTrascinoBarra = false; ReleaseCapture(); d->lDirty = true; return 0; }
            { const D2D1_RECT_F b = d->lRettBarra(); if (d->lScrollMax() > 0 && x >= b.left && x < b.right && y >= b.top && y < b.bottom) return 0; }
            if (y > d->LH) { d->chiudiLauncher(); return 0; }   // clic sotto (durante l'animazione): chiude
            if (d->lSopraEnergia) { POINT p{(LONG)x, (LONG)y}; ClientToScreen(h, &p); d->menuEnergia(p); return 0; }
            const int i = d->lCellaIn(x, y);
            if (i >= 0) d->avviaApp(i);
            return 0;
        }
        case WM_RBUTTONUP: {   // il menu di Windows dell'app (amministratore, percorso, disinstalla...)
            const int i = d->lCellaIn(x, y);
            if (i >= 0) { POINT p{(LONG)x, (LONG)y}; ClientToScreen(h, &p); d->menuApp(i, p); }
            return 0;
        }
        // sottomenu del menu di Windows ("Invia a", "Apri con"...): li disegna lui
        case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR:
            if (d->menuShell3) { LRESULT r = 0; if (SUCCEEDED(d->menuShell3->HandleMenuMsg2(m, w, l, &r))) return r; }
            else if (d->menuShell2) { if (SUCCEEDED(d->menuShell2->HandleMenuMsg(m, w, l))) return m == WM_INITMENUPOPUP ? 0 : TRUE; }
            break;
        case WM_MOUSEWHEEL:
            d->lScrollObj = std::clamp(d->lScrollObj - GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA * d->lCellaH() * 0.9, 0.0, d->lScrollMax());
            d->lDirty = true; return 0;
        case WM_CHAR:
            if (w == 27) d->chiudiLauncher();
            // Invio: con una ricerca la prima trovata (o quella scelta con le frecce); senza, quella scelta
            else if (w == 13) d->avviaApp(!d->query.empty() || d->lSopra == d->lSel ? d->lSel : -1);
            else if (w == 8) { if (!d->query.empty()) { d->query.pop_back(); d->filtra(); } }
            else if (w == 127 || (w == 8 && (GetKeyState(VK_CONTROL) & 0x8000))) {   // Ctrl+Backspace: via l'ultima parola
                while (!d->query.empty() && d->query.back() == L' ') d->query.pop_back();
                while (!d->query.empty() && d->query.back() != L' ') d->query.pop_back();
                d->filtra();
            }
            else if (w == 22) {   // Ctrl+V: incolla (solo la prima riga)
                if (OpenClipboard(h)) {
                    if (HANDLE g = GetClipboardData(CF_UNICODETEXT)) {
                        if (const wchar_t* t = (const wchar_t*)GlobalLock(g)) {
                            for (const wchar_t* c = t; *c && *c != L'\r' && *c != L'\n' && d->query.size() < 100; c++) if (*c >= 32) d->query += *c;
                            GlobalUnlock(g);
                        }
                    }
                    CloseClipboard();
                    d->filtra();
                }
            }
            else if (w >= 32) { d->query += (wchar_t)w; d->filtra(); }
            return 0;
        case WM_KEYDOWN: {
            int s = d->lSel;
            // pagine: tante righe quante se ne vedono
            const int righePagina = std::max(1, (int)((d->lGrigliaBottom() - d->lGrigliaTop()) / d->lCellaH()));
            if (w == VK_LEFT) s--; else if (w == VK_RIGHT) s++; else if (w == VK_UP) s -= 6; else if (w == VK_DOWN) s += 6;
            else if (w == VK_PRIOR) s -= 6 * righePagina; else if (w == VK_NEXT) s += 6 * righePagina;
            else if (w == VK_HOME) s = 0; else if (w == VK_END) s = (int)d->filtrate.size() - 1;
            else break;
            // senza ricerca la prima freccia mostra solo la scelta attuale; la scelta si vede come il passaggio del mouse
            if (d->query.empty() && d->lSopra != d->lSel) s = d->lSel;
            d->lSel = std::clamp(s, 0, std::max(0, (int)d->filtrate.size() - 1));
            if (d->query.empty() && !d->filtrate.empty()) d->lSopra = d->lSel;
            d->selezionaVisibile(); d->lDirty = true;
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) {
                // Appena aperto, il primo piano preso agganciandosi all'app di prima a volte torna a lei subito dopo
                // lo sgancio (come per la ricerca): non e' un clic altrove, si riprende una volta invece di chiudersi
                if (d->lAperto && !d->lChiusura && !d->lRipreso && adesso() - d->lT0 < 0.3 &&
                    d->primaDelLauncher && GetForegroundWindow() == d->primaDelLauncher) {
                    d->lRipreso = true;
                    d->scrivi("launcher: primo piano tornato all'app di prima, lo riprende");
                    PostMessageW(h, WM_APP + 3, 0, 0);
                    return 0;
                }
                // il fuoco va al dock perche' si e' cliccata l'icona App: la chiusura la fa quel clic (apriLauncher
                // fa da interruttore). Chiudendo qui, il clic subito dopo lo riaprirebbe
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) && d->mouseSuIconaApp()) return 0;
                d->chiudiLauncher();
            }
            return 0;
        case WM_APP + 3: if (d->lAperto && !d->lChiusura) prendiFuoco(h); return 0;
        case WM_CLOSE: d->chiudiLauncher(); return 0;   // Alt+F4: come Esc (la finestra non si distrugge, si riusa)
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace dock
