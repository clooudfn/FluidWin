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
    // Consegna non riuscita con la finestra ancora viva (memoria finita per un momento, coda piena): si riprova per
    // qualche secondo, perche' il completamento azzera lo stato "in corso" del launcher e perso bloccava le ricerche
    // successive (R20261121-122837-S3, R20261121-123050-S1). Finestra sparita: FluidWin si sta chiudendo.
    // Senza limite finche' la finestra vive: dopo 5 s si buttava il completamento, e la ricerca restava "in corso"
    // fino al riavvio (R20261121-140048-S5); dopo il primo secondo un tentativo ogni mezzo secondo
    for (int i = 0; !consegna::posta(l->dest, WM_LAVORO_FATTO, 0, l); i++) {
        if (!IsWindow(l->dest)) { delete l; break; }
        Sleep(i < 10 ? 100 : 500);
    }
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
    if (!v.ripiego.empty()) return v.ripiego;                 // Squirrel, versione cancellata (vedi Voce::ripiego)
    if (v.iconaRotta && !v.exe.empty() && GetFileAttributesW(v.exe.c_str()) != INVALID_FILE_ATTRIBUTES) return v.exe;   // (vedi Voce::iconaRotta)
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
            for (auto& v : voci) if (!v.icona && v.avvio != AVVIO_LAUNCHER && v.avvio != AVVIO_GIOCHI && v.avvio != AVVIO_CONTROLLO && sorgenteIcona(v) == ip.percorso) { v.icona = b; presa = true; dirty = true; break; }
            // (si potra' richiedere: l'app puo' chiudersi e riaprirsi; se e' fallita si riprova al prossimo aggiornamento)
            iconeChieste.erase(chiaveIcona(ip.percorso, ip.lato));
        } else if (ip.lato == 128) {   // app in background (se non c'e', la piccola foto salvata da Windows)
            if (!b) b = (ID2D1Bitmap1*)fotoIconaSfondo(ip.percorso);
            cacheIcone[ip.percorso] = b; presa = b != nullptr;
            for (auto& a : appSfondo) if (a.exe == ip.percorso) a.icona = b;
            pDirty = true; altra.pDirty = true;
        } else if (ip.lato == 96) {    // launcher
            auto assegna = [&](std::vector<AppL>& el) {
                for (auto& a : el) {
                    if (sorgenteIconaL(a) == ip.percorso && !a.icona) {
                        if (b && presa) b->AddRef();
                        a.icona = b; presa = true; lDirty = true;
                    }
                }
            };
            assegna(tutteApp);
            assegna(tuttiGiochi);
            iconeChieste.erase(chiaveIcona(ip.percorso, ip.lato));
        } else if (ip.lato == 48) {    // file e cartelle del launcher (anche "nessuna": non si richiede a ogni ricerca)
            auto& v = iconeFile[ip.percorso];
            if (!v) { v = b; presa = b != nullptr; }
            iconeChieste.erase(chiaveIcona(ip.percorso, ip.lato));
            lDirty = true;
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
    // ogni passo puo' fallire: false (niente launcher), mai un puntatore nullo usato dopo (audit FW-AUDIT-004)
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fab = nullptr;
    HRESULT hr = dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd);
    if (SUCCEEDED(hr)) hr = dd->GetAdapter(&ad);
    if (SUCCEEDED(hr)) hr = ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fab);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = (UINT)LW; sd.Height = (UINT)(LH + LEXTRA); sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &scL);
    }
    if (fab) fab->Release();
    if (ad) ad->Release();
    if (dd) dd->Release();
    if (FAILED(hr)) return false;
    if (FAILED(dcomp->CreateTargetForHwnd(launcher, TRUE, &targetL)) || FAILED(dcomp->CreateVisual(&visualL)) ||
        FAILED(dcomp->CreateEffectGroup(&effettoL))) return false;
    visualL->SetContent(scL); visualL->SetEffect(effettoL);
    targetL->SetRoot(visualL); dcomp->Commit();
    IDXGISurface* sup = nullptr;
    if (FAILED(scL->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup)) || !sup) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglioL); sup->Release();
    if (bersaglioL) DragAcceptFiles(launcher, TRUE);
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
    if (!lModoGiochi) {
        const int sel = lSel; const double scr = lScrollObj;
        filtra();
        if (lAperto) { lSel = std::clamp(sel, 0, std::max(0, (int)filtrate.size() - 1)); lScrollObj = std::clamp(scr, 0.0, lScrollMax()); }
    }
    for (auto& a : tutteApp) if (!a.icona && !a.provata && (!mostraGiochi || !appartieneAiGiochi(a))) { a.provata = true; chiediIcona(sorgenteIconaL(a), 96); }
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

static inline bool finisceCon(const std::wstring& s, const wchar_t* suff) {
    const size_t n = wcslen(suff);
    return s.size() >= n && !_wcsicmp(s.c_str() + s.size() - n, suff);
}

static inline bool esisteFile(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static inline bool esisteCartella(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static inline std::wstring normalizzaPercorso(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() == 2 && p[1] == L':') p.push_back(L'\\');
    return p;
}

static inline std::wstring chiaveTitolo(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s) if (iswalnum(c)) r.push_back((wchar_t)towlower(c));
    return r;
}

static inline std::wstring leggiValoreReg(HKEY k, const wchar_t* nome) {
    DWORD tipo = 0, cb = 0;
    if (RegQueryValueExW(k, nome, nullptr, &tipo, nullptr, &cb) != ERROR_SUCCESS || !cb) return L"";
    if (tipo != REG_SZ && tipo != REG_EXPAND_SZ) return L"";
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(k, nome, nullptr, &tipo, (BYTE*)buf.data(), &cb) != ERROR_SUCCESS) return L"";
    if (tipo == REG_EXPAND_SZ) {
        const DWORD n = ExpandEnvironmentStringsW(buf.data(), nullptr, 0);
        if (n > 0) {
            std::vector<wchar_t> esp(n + 1, L'\0');
            if (ExpandEnvironmentStringsW(buf.data(), esp.data(), n + 1)) return esp.data();
        }
    }
    return buf.data();
}

static inline std::wstring leggiFileUtf8(const std::wstring& percorso) {
    HANDLE h = CreateFileW(percorso.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz{};
    std::string bytes;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 4 * 1024 * 1024) {
        bytes.resize((size_t)sz.QuadPart);
        DWORD letto = 0;
        if (!ReadFile(h, bytes.data(), (DWORD)bytes.size(), &letto, nullptr)) bytes.clear();
        else bytes.resize(letto);
    }
    CloseHandle(h);
    if (bytes.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), n);
    return out;
}

static inline bool coppiaVdf(const std::wstring& riga, std::wstring& chiave, std::wstring& val) {
    auto leggi = [&](size_t& i, std::wstring& out) -> bool {
        while (i < riga.size() && riga[i] != L'"') i++;
        if (i >= riga.size()) return false;
        i++;
        out.clear();
        while (i < riga.size() && riga[i] != L'"') {
            if (riga[i] == L'\\' && i + 1 < riga.size()) { out.push_back(riga[i + 1]); i += 2; }
            else out.push_back(riga[i++]);
        }
        if (i >= riga.size()) return false;
        i++;
        return true;
    };
    size_t i = 0;
    return leggi(i, chiave) && leggi(i, val);
}

static inline std::wstring rimuoviVirgolette(std::wstring s) {
    while (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}

static inline std::wstring estraiExeDaComando(const std::wstring& cmd) {
    size_t i = 0;
    while (i < cmd.size() && iswspace(cmd[i])) i++;
    if (i >= cmd.size()) return L"";
    if (cmd[i] == L'"') {
        const size_t f = cmd.find(L'"', i + 1);
        return f == std::wstring::npos ? cmd.substr(i + 1) : cmd.substr(i + 1, f - i - 1);
    }
    const size_t f = cmd.find_first_of(L" \t", i);
    return f == std::wstring::npos ? cmd.substr(i) : cmd.substr(i, f - i);
}

static inline bool leggiStringaJson(const std::wstring& txt, size_t& i, std::wstring& out) {
    if (i >= txt.size() || txt[i] != L'"') return false;
    i++;
    out.clear();
    while (i < txt.size() && txt[i] != L'"') {
        if (txt[i] == L'\\' && i + 1 < txt.size()) {
            const wchar_t c = txt[i + 1];
            if (c == L'"' || c == L'\\' || c == L'/') out.push_back(c);
            else if (c == L'n') out.push_back(L'\n');
            else if (c == L'r') out.push_back(L'\r');
            else if (c == L't') out.push_back(L'\t');
            else out.push_back(c);
            i += 2;
        } else {
            out.push_back(txt[i++]);
        }
    }
    if (i >= txt.size()) return false;
    i++;
    return true;
}

static inline size_t trovaChiaveJson(const std::wstring& txt, const wchar_t* chiave) {
    const std::wstring ago = std::wstring(L"\"") + chiave + L"\"";
    size_t pos = 0;
    while ((pos = txt.find(ago, pos)) != std::wstring::npos) {
        if (pos > 0 && txt[pos - 1] == L'\\') { pos += ago.size(); continue; }
        size_t i = pos + ago.size();
        while (i < txt.size() && iswspace(txt[i])) i++;
        if (i < txt.size() && txt[i] == L':') {
            i++;
            while (i < txt.size() && iswspace(txt[i])) i++;
            return i;
        }
        pos += ago.size();
    }
    return std::wstring::npos;
}

static inline std::wstring valoreJson(const std::wstring& txt, const wchar_t* chiave) {
    size_t i = trovaChiaveJson(txt, chiave);
    if (i == std::wstring::npos) return L"";
    std::wstring out;
    return leggiStringaJson(txt, i, out) ? out : L"";
}

static inline bool boolJson(const std::wstring& txt, const wchar_t* chiave) {
    const size_t i = trovaChiaveJson(txt, chiave);
    if (i == std::wstring::npos) return false;
    return txt.compare(i, 4, L"true") == 0;
}

static inline std::vector<std::wstring> arrayStringheJson(const std::wstring& txt, const wchar_t* chiave) {
    std::vector<std::wstring> r;
    size_t i = trovaChiaveJson(txt, chiave);
    if (i == std::wstring::npos || i >= txt.size() || txt[i] != L'[') return r;
    i++;
    while (i < txt.size()) {
        while (i < txt.size() && (iswspace(txt[i]) || txt[i] == L',')) i++;
        if (i >= txt.size() || txt[i] == L']') break;
        if (txt[i] == L'"') {
            std::wstring s;
            if (!leggiStringaJson(txt, i, s)) break;
            r.push_back(std::move(s));
        } else {
            i++;
        }
    }
    return r;
}

static inline void cercaExeRic(const std::wstring& dir, int prof, std::wstring& migliore, ULONGLONG& maxMigliore, std::wstring& ripiego, ULONGLONG& maxRipiego) {
    static const wchar_t* const scarta[] = {
        L"crash", L"unins", L"setup", L"redist", L"dxsetup", L"vcredist",
        L"crs-", L"unitycrash", L"easyanticheat", L"battleye", L"ue4prereq", L"ueprereq"
    };
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (prof < 2) cercaExeRic(dir + L"\\" + fd.cFileName, prof + 1, migliore, maxMigliore, ripiego, maxRipiego);
            continue;
        }
        const std::wstring fn = minuscolo(fd.cFileName);
        if (fn.size() <= 4 || fn.compare(fn.size() - 4, 4, L".exe")) continue;
        bool salta = false;
        for (const wchar_t* s : scarta) if (fn.find(s) != std::wstring::npos) { salta = true; break; }
        if (salta) continue;
        const ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        const std::wstring p = dir + L"\\" + fd.cFileName;
        if (fn.find(L"launcher") == std::wstring::npos) {
            if (migliore.empty() || sz > maxMigliore) { maxMigliore = sz; migliore = p; }
        } else {
            if (ripiego.empty() || sz > maxRipiego) { maxRipiego = sz; ripiego = p; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static inline std::wstring trovaExeGioco(const std::wstring& dir) {
    if (dir.empty() || !esisteCartella(dir)) return L"";
    std::wstring migliore, ripiego;
    ULONGLONG maxMigliore = 0, maxRipiego = 0;
    cercaExeRic(dir, 0, migliore, maxMigliore, ripiego, maxRipiego);
    return !migliore.empty() ? migliore : ripiego;
}

static inline std::wstring trovaIconaSteamCache(const std::wstring& steamPath, const std::wstring& appid) {
    if (steamPath.empty() || appid.empty()) return L"";
    const std::wstring dir = steamPath + L"\\appcache\\librarycache\\" + appid;
    if (esisteCartella(dir)) {
        std::wstring ico, hashImg, altraImg;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::wstring fn = minuscolo(fd.cFileName);
                if (fn.find(L"header") != std::wstring::npos || fn.find(L"library_") != std::wstring::npos || fn.find(L"logo") != std::wstring::npos) continue;
                const std::wstring full = dir + L"\\" + fd.cFileName;
                if (finisceCon(fn, L".ico")) { ico = full; break; }
                if (finisceCon(fn, L".jpg") || finisceCon(fn, L".png")) {
                    const size_t dot = fn.rfind(L'.');
                    const std::wstring stem = dot == std::wstring::npos ? fn : fn.substr(0, dot);
                    bool esadec = stem.size() >= 16;
                    for (wchar_t c : stem) if (!iswxdigit(c)) { esadec = false; break; }
                    if (esadec && hashImg.empty()) hashImg = full;
                    else if (altraImg.empty()) altraImg = full;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!ico.empty()) return ico;
        if (!hashImg.empty()) return hashImg;
        if (!altraImg.empty()) return altraImg;
    }
    const std::wstring legacy = steamPath + L"\\appcache\\librarycache\\" + appid + L"_icon.jpg";
    return esisteFile(legacy) ? legacy : L"";
}

inline auto Motore::leggiGiochiNascosti() -> std::unordered_set<std::wstring> {
    std::unordered_set<std::wstring> s;
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiNascosti", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb <= sizeof(wchar_t)) return s;
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiNascosti", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &cb) != ERROR_SUCCESS) return s;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) s.insert(minuscolo(p));
    return s;
}

inline auto Motore::salvaGiochiNascosti(const std::unordered_set<std::wstring>& s) -> void {
    std::vector<wchar_t> buf;
    for (const auto& x : s) {
        if (x.empty()) continue;
        buf.insert(buf.end(), x.begin(), x.end());
        buf.push_back(L'\0');
    }
    if (buf.empty()) { RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiNascosti"); return; }
    buf.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiNascosti", REG_MULTI_SZ, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
}

inline auto Motore::leggiGiochiAggiunti() -> std::vector<std::pair<std::wstring, std::wstring>> {
    std::vector<std::pair<std::wstring, std::wstring>> el;
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiAggiunti", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb <= sizeof(wchar_t)) return el;
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiAggiunti", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &cb) != ERROR_SUCCESS) return el;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        const std::wstring r = p;
        const size_t tab = r.find(L'\t');
        if (tab == std::wstring::npos || tab == 0 || tab + 1 >= r.size()) continue;
        el.push_back({r.substr(0, tab), r.substr(tab + 1)});
    }
    return el;
}

inline auto Motore::salvaGiochiAggiunti(const std::vector<std::pair<std::wstring, std::wstring>>& el) -> void {
    std::vector<wchar_t> buf;
    for (const auto& [nome, percorso] : el) {
        if (nome.empty() || percorso.empty()) continue;
        buf.insert(buf.end(), nome.begin(), nome.end());
        buf.push_back(L'\t');
        buf.insert(buf.end(), percorso.begin(), percorso.end());
        buf.push_back(L'\0');
    }
    if (buf.empty()) { RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiAggiunti"); return; }
    buf.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiAggiunti", REG_MULTI_SZ, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
}

inline auto Motore::leggiMappaGiochi(const wchar_t* valore) -> std::unordered_map<std::wstring, std::wstring> {
    std::unordered_map<std::wstring, std::wstring> m;
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, valore, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb <= sizeof(wchar_t)) return m;
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, valore, RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &cb) != ERROR_SUCCESS) return m;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        const std::wstring r = p;
        const size_t tab = r.find(L'\t');
        if (tab == std::wstring::npos || tab == 0 || tab + 1 >= r.size()) continue;
        m[minuscolo(r.substr(0, tab))] = r.substr(tab + 1);
    }
    return m;
}

inline auto Motore::salvaMappaGiochi(const wchar_t* valore, const std::unordered_map<std::wstring, std::wstring>& m) -> void {
    std::vector<wchar_t> buf;
    for (const auto& [chiave, dato] : m) {
        std::wstring pulito;
        pulito.reserve(dato.size());
        for (wchar_t ch : dato) {
            if (ch != L'\t' && ch != L'\r' && ch != L'\n') pulito.push_back(ch);
        }
        if (chiave.empty() || pulito.empty()) continue;
        const std::wstring k = minuscolo(chiave);
        buf.insert(buf.end(), k.begin(), k.end());
        buf.push_back(L'\t');
        buf.insert(buf.end(), pulito.begin(), pulito.end());
        buf.push_back(L'\0');
    }
    if (buf.empty()) { RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, valore); return; }
    buf.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, valore, REG_MULTI_SZ, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
}

inline auto Motore::leggiIconeGiochi() -> std::unordered_map<std::wstring, std::wstring> {
    return leggiMappaGiochi(L"IconeGiochi");
}

inline auto Motore::salvaIconeGiochi(const std::unordered_map<std::wstring, std::wstring>& m) -> void {
    salvaMappaGiochi(L"IconeGiochi", m);
}

inline auto Motore::leggiNomiGiochi() -> std::unordered_map<std::wstring, std::wstring> {
    return leggiMappaGiochi(L"NomiGiochi");
}

inline auto Motore::salvaNomiGiochi(const std::unordered_map<std::wstring, std::wstring>& m) -> void {
    salvaMappaGiochi(L"NomiGiochi", m);
}

inline auto Motore::leggiGiochiPreferiti() -> std::unordered_set<std::wstring> {
    std::unordered_set<std::wstring> s;
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiPreferiti", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb <= sizeof(wchar_t)) return s;
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiPreferiti", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &cb) != ERROR_SUCCESS) return s;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) s.insert(minuscolo(p));
    return s;
}

inline auto Motore::salvaGiochiPreferiti(const std::unordered_set<std::wstring>& s) -> void {
    std::vector<wchar_t> buf;
    for (const auto& x : s) {
        if (x.empty()) continue;
        const std::wstring k = minuscolo(x);
        buf.insert(buf.end(), k.begin(), k.end());
        buf.push_back(L'\0');
    }
    if (buf.empty()) { RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiPreferiti"); return; }
    buf.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiPreferiti", REG_MULTI_SZ, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
}

inline auto Motore::leggiGiochiRecenti() -> std::unordered_map<std::wstring, ULONGLONG> {
    std::unordered_map<std::wstring, ULONGLONG> m;
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiRecenti", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb <= sizeof(wchar_t)) return m;
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiRecenti", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &cb) != ERROR_SUCCESS) return m;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        const std::wstring r = p;
        const size_t tab = r.find(L'\t');
        if (tab == std::wstring::npos || tab == 0 || tab + 1 >= r.size()) continue;
        wchar_t* fine = nullptr;
        const ULONGLONG ts = wcstoull(r.c_str() + tab + 1, &fine, 10);
        if (fine && fine != r.c_str() + tab + 1 && ts > 0) m[minuscolo(r.substr(0, tab))] = ts;
    }
    return m;
}

inline auto Motore::salvaGiochiRecenti(const std::unordered_map<std::wstring, ULONGLONG>& m) -> void {
    std::vector<wchar_t> buf;
    for (const auto& [chiave, ts] : m) {
        if (chiave.empty() || ts == 0) continue;
        const std::wstring k = minuscolo(chiave);
        const std::wstring v = std::to_wstring(ts);
        buf.insert(buf.end(), k.begin(), k.end());
        buf.push_back(L'\t');
        buf.insert(buf.end(), v.begin(), v.end());
        buf.push_back(L'\0');
    }
    if (buf.empty()) { RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiRecenti"); return; }
    buf.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiRecenti", REG_MULTI_SZ, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
}

inline auto Motore::leggiOpzioniGiochi() -> std::unordered_map<std::wstring, std::pair<bool, std::wstring>> {
    std::unordered_map<std::wstring, std::pair<bool, std::wstring>> m;
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"OpzioniGiochi", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb <= sizeof(wchar_t)) return m;
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"OpzioniGiochi", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &cb) != ERROR_SUCCESS) return m;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        const std::wstring r = p;
        const size_t t1 = r.find(L'\t');
        if (t1 == std::wstring::npos || t1 == 0) continue;
        const size_t t2 = r.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) continue;
        const bool admin = (t2 == t1 + 2 && r[t1 + 1] == L'1');
        std::wstring args = r.substr(t2 + 1);
        if (!admin && args.empty()) continue;
        m[minuscolo(r.substr(0, t1))] = {admin, std::move(args)};
    }
    return m;
}

inline auto Motore::salvaOpzioniGiochi(const std::unordered_map<std::wstring, std::pair<bool, std::wstring>>& m) -> void {
    std::vector<wchar_t> buf;
    for (const auto& [chiave, opt] : m) {
        if (chiave.empty()) continue;
        std::wstring pulito;
        pulito.reserve(opt.second.size());
        for (wchar_t ch : opt.second) {
            if (ch != L'\t' && ch != L'\r' && ch != L'\n') pulito.push_back(ch);
        }
        if (!opt.first && pulito.empty()) continue;
        const std::wstring k = minuscolo(chiave);
        buf.insert(buf.end(), k.begin(), k.end());
        buf.push_back(L'\t');
        buf.push_back(opt.first ? L'1' : L'0');
        buf.push_back(L'\t');
        buf.insert(buf.end(), pulito.begin(), pulito.end());
        buf.push_back(L'\0');
    }
    if (buf.empty()) { RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"OpzioniGiochi"); return; }
    buf.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"OpzioniGiochi", REG_MULTI_SZ, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
}

inline auto Motore::leggiOrdineRecenti() -> bool {
    DWORD val = 1, cb = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiOrdineRecenti", RRF_RT_REG_DWORD, nullptr, &val, &cb) != ERROR_SUCCESS) return true;
    return val != 0;
}

inline auto Motore::salvaOrdineRecenti(bool rec) -> void {
    const DWORD val = rec ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"GiochiOrdineRecenti", REG_DWORD, &val, sizeof(val));
}

inline auto Motore::leggiChiaveSteamGridDB() -> std::wstring {
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"SteamGridDBKey", RRF_RT_REG_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb <= sizeof(wchar_t)) return L"";
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"SteamGridDBKey", RRF_RT_REG_SZ, nullptr, buf.data(), &cb) != ERROR_SUCCESS) return L"";
    return buf.data();
}

inline auto Motore::salvaChiaveSteamGridDB(const std::wstring& k) -> void {
    std::wstring pulita;
    pulita.reserve(k.size());
    for (wchar_t ch : k) {
        if (ch != L'\t' && ch != L'\r' && ch != L'\n') pulita.push_back(ch);
    }
    const size_t ini = pulita.find_first_not_of(L' ');
    if (ini == std::wstring::npos) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"SteamGridDBKey");
        return;
    }
    const size_t fin = pulita.find_last_not_of(L' ');
    pulita = pulita.substr(ini, fin - ini + 1);
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPBAR, L"SteamGridDBKey", REG_SZ, pulita.c_str(), (DWORD)((pulita.size() + 1) * sizeof(wchar_t)));
}

static inline std::wstring cartellaCacheIconeGiochi() {
    wchar_t base[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) return L"";
    const std::wstring d1 = std::wstring(base) + L"\\FluidWin";
    const std::wstring d2 = d1 + L"\\IconeGiochi";
    CreateDirectoryW(d1.c_str(), nullptr);
    CreateDirectoryW(d2.c_str(), nullptr);
    return esisteCartella(d2) ? d2 : L"";
}

static inline std::wstring hashChiaveGioco(const std::wstring& s) {
    unsigned long long h = 14695981039346656037ULL;
    for (wchar_t c : s) {
        const unsigned short u = (unsigned short)c;
        h ^= (unsigned char)(u & 0xFF);
        h *= 1099511628211ULL;
        h ^= (unsigned char)(u >> 8);
        h *= 1099511628211ULL;
    }
    wchar_t buf[24];
    swprintf(buf, 24, L"%016llx", h);
    return buf;
}

static inline std::wstring utf8AWide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static inline std::wstring codificaUrl(const std::wstring& s) {
    if (s.empty()) return L"";
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return L"";
    std::string u((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), u.data(), n, nullptr, nullptr);
    static const wchar_t* const hex = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(u.size() * 2);
    for (unsigned char b : u) {
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
            b == '-' || b == '_' || b == '.' || b == '~') {
            out.push_back((wchar_t)b);
        } else {
            out.push_back(L'%');
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 0x0F]);
        }
    }
    return out;
}

static inline std::string httpPrendi(const std::wstring& url, const std::wstring& bearer = L"", size_t maxBytes = 8 * 1024 * 1024) {
    if (url.empty()) return {};
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = L"";
    std::vector<wchar_t> path(url.size() + 16, L'\0');
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path.data();
    uc.dwUrlPathLength = (DWORD)path.size();
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return {};
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) return {};

    struct HttpGuard {
        HINTERNET s = nullptr, c = nullptr, r = nullptr;
        ~HttpGuard() {
            if (r) WinHttpCloseHandle(r);
            if (c) WinHttpCloseHandle(c);
            if (s) WinHttpCloseHandle(s);
        }
    } h;

    h.s = WinHttpOpen(L"FluidWin", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.s) return {};
    WinHttpSetTimeouts(h.s, 5000, 5000, 8000, 12000);
    h.c = WinHttpConnect(h.s, host, uc.nPort, 0);
    if (!h.c) return {};
    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    h.r = WinHttpOpenRequest(h.c, L"GET", path.data(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!h.r) return {};

    const std::wstring hdr = bearer.empty() ? L"" : (L"Authorization: Bearer " + bearer + L"\r\n");
    const BOOL okSend = WinHttpSendRequest(
        h.r,
        hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
        hdr.empty() ? 0 : (DWORD)-1L,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!okSend || !WinHttpReceiveResponse(h.r, nullptr)) return {};

    DWORD stato = 0, ls = sizeof(stato);
    if (!WinHttpQueryHeaders(h.r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &stato, &ls, WINHTTP_NO_HEADER_INDEX) || stato != 200) {
        return {};
    }

    std::string out;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(h.r, &avail) && avail > 0) {
        if (out.size() + (size_t)avail > maxBytes) return {};
        const size_t prev = out.size();
        out.resize(prev + (size_t)avail);
        DWORD letti = 0;
        if (!WinHttpReadData(h.r, out.data() + prev, avail, &letti) || !letti) {
            out.resize(prev);
            break;
        }
        out.resize(prev + (size_t)letti);
    }
    return out;
}

static inline std::wstring primoIdInArrayJson(const std::wstring& txt, const wchar_t* arrayKey) {
    size_t i = trovaChiaveJson(txt, arrayKey);
    if (i == std::wstring::npos || i >= txt.size() || txt[i] != L'[') return L"";
    i++;
    int depth = 1;
    while (i < txt.size() && depth > 0) {
        if (txt[i] == L'"') {
            std::wstring s;
            if (!leggiStringaJson(txt, i, s)) break;
            size_t j = i;
            while (j < txt.size() && iswspace(txt[j])) j++;
            if (j < txt.size() && txt[j] == L':') {
                j++;
                while (j < txt.size() && iswspace(txt[j])) j++;
                if (s == L"id" && j < txt.size() && iswdigit(txt[j])) {
                    std::wstring id;
                    while (j < txt.size() && iswdigit(txt[j])) id.push_back(txt[j++]);
                    return id;
                }
            }
        } else {
            if (txt[i] == L'[') depth++;
            else if (txt[i] == L']') depth--;
            i++;
        }
    }
    return L"";
}

static inline std::wstring migliorUrlInDataJson(const std::wstring& txt) {
    size_t i = trovaChiaveJson(txt, L"data");
    if (i == std::wstring::npos || i >= txt.size() || txt[i] != L'[') return L"";
    i++;
    int depth = 1;
    std::wstring migliore;
    int migliorPunti = 0;
    auto valuta = [](const std::wstring& u) -> int {
        if (u.rfind(L"https://", 0) != 0 && u.rfind(L"http://", 0) != 0) return 0;
        const size_t q = u.find_first_of(L"?#");
        const std::wstring base = minuscolo(q == std::wstring::npos ? u : u.substr(0, q));
        if (finisceCon(base, L".png") || finisceCon(base, L".ico")) return 3;
        if (finisceCon(base, L".jpg") || finisceCon(base, L".jpeg")) return 2;
        return 1;
    };
    while (i < txt.size() && depth > 0) {
        if (txt[i] == L'"') {
            std::wstring s;
            if (!leggiStringaJson(txt, i, s)) break;
            size_t j = i;
            while (j < txt.size() && iswspace(txt[j])) j++;
            if (j < txt.size() && txt[j] == L':') {
                j++;
                while (j < txt.size() && iswspace(txt[j])) j++;
                if (s == L"url" && j < txt.size() && txt[j] == L'"') {
                    std::wstring val;
                    if (leggiStringaJson(txt, j, val)) {
                        const int p = valuta(val);
                        if (p > migliorPunti) {
                            migliorPunti = p;
                            migliore = std::move(val);
                            if (migliorPunti >= 3) return migliore;
                        }
                    }
                    i = j;
                    continue;
                }
            }
        } else {
            if (txt[i] == L'[') depth++;
            else if (txt[i] == L']') depth--;
            i++;
        }
    }
    return migliore;
}

static inline std::wstring estensioneDaMagic(const std::string& b, bool soloJpgPng = false) {
    if (b.size() >= 8 && (unsigned char)b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return L".png";
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) return L".jpg";
    if (soloJpgPng) return L"";
    if (b.size() >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0) return L".ico";
    if (b.size() >= 12 && memcmp(b.data(), "RIFF", 4) == 0 && memcmp(b.data() + 8, "WEBP", 4) == 0) return L".webp";
    return L"";
}

static inline bool scriviFileBinario(const std::wstring& percorso, const std::string& bytes) {
    if (percorso.empty() || bytes.empty()) return false;
    const std::wstring tmp = percorso + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t scritti = 0;
    bool ok = true;
    while (scritti < bytes.size()) {
        DWORD n = 0;
        const DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - scritti, 1024 * 1024);
        if (!WriteFile(h, bytes.data() + scritti, chunk, &n, nullptr) || n == 0) { ok = false; break; }
        scritti += (size_t)n;
    }
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), percorso.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

inline auto Motore::scaricaIconaGiocoOnline(const AppL& a, const std::wstring& apiKey) -> std::wstring {
    const std::wstring cacheDir = cartellaCacheIconeGiochi();
    if (cacheDir.empty()) return L"";
    const std::wstring base = cacheDir + L"\\" + hashChiaveGioco(chiaveGioco(a));

    auto estraiCifre = [](const std::wstring& s, size_t pos) -> std::wstring {
        std::wstring out;
        while (pos < s.size() && iswdigit(s[pos])) out.push_back(s[pos++]);
        return out;
    };

    const std::wstring avv = minuscolo(a.avvio);
    std::wstring platform, pid;
    if (avv.rfind(L"steam://rungameid/", 0) == 0) {
        platform = L"steam";
        pid = estraiCifre(avv, 18);
    } else if (avv.rfind(L"uplay://launch/", 0) == 0) {
        platform = L"uplay";
        pid = estraiCifre(avv, 15);
    } else if (avv.rfind(L"goggalaxy://opengameview/", 0) == 0) {
        platform = L"gog";
        pid = estraiCifre(avv, 25);
    }

    auto salva = [&](const std::string& bytes, bool soloJpgPng) -> std::wstring {
        const std::wstring ext = estensioneDaMagic(bytes, soloJpgPng);
        if (ext.empty()) return L"";
        const std::wstring dest = base + ext;
        return scriviFileBinario(dest, bytes) ? dest : L"";
    };

    if (!apiKey.empty()) {
        auto prendiUrlSgdb = [&](const std::wstring& ep) -> std::wstring {
            const std::string body = httpPrendi(L"https://www.steamgriddb.com/api/v2/" + ep, apiKey, 2 * 1024 * 1024);
            return body.empty() ? L"" : migliorUrlInDataJson(utf8AWide(body));
        };

        std::wstring imgUrl;
        if (!platform.empty() && !pid.empty()) {
            imgUrl = prendiUrlSgdb(L"icons/" + platform + L"/" + pid);
            if (imgUrl.empty()) imgUrl = prendiUrlSgdb(L"grids/" + platform + L"/" + pid + L"?dimensions=512x512,1024x1024");
        }
        if (imgUrl.empty() && !a.nome.empty()) {
            const std::string sr = httpPrendi(L"https://www.steamgriddb.com/api/v2/search/autocomplete/" + codificaUrl(a.nome), apiKey, 2 * 1024 * 1024);
            const std::wstring gid = sr.empty() ? L"" : primoIdInArrayJson(utf8AWide(sr), L"data");
            if (!gid.empty()) {
                imgUrl = prendiUrlSgdb(L"icons/game/" + gid);
                if (imgUrl.empty()) imgUrl = prendiUrlSgdb(L"grids/game/" + gid + L"?dimensions=512x512,1024x1024");
            }
        }
        if (!imgUrl.empty()) {
            const std::string imgBytes = httpPrendi(imgUrl);
            const std::wstring salvato = salva(imgBytes, false);
            if (!salvato.empty()) return salvato;
        }
    }

    std::wstring steamId = (platform == L"steam") ? pid : L"";
    if (steamId.empty() && !a.nome.empty()) {
        const std::string sr = httpPrendi(L"https://store.steampowered.com/api/storesearch/?term=" + codificaUrl(a.nome) + L"&l=english&cc=US", L"", 2 * 1024 * 1024);
        if (!sr.empty()) steamId = primoIdInArrayJson(utf8AWide(sr), L"items");
    }
    if (!steamId.empty()) {
        const std::wstring urls[2] = {
            L"https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/" + steamId + L"/library_600x900.jpg",
            L"https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/" + steamId + L"/header.jpg"
        };
        for (const auto& u : urls) {
            const std::string imgBytes = httpPrendi(u);
            const std::wstring salvato = salva(imgBytes, true);
            if (!salvato.empty()) return salvato;
        }
    }
    return L"";
}

struct CandidatoIcona {
    std::wstring thumbUrl;
    std::wstring fullUrl;
};

static inline int punteggioUrlIcona(const std::wstring& u) {
    if (u.rfind(L"https://", 0) != 0 && u.rfind(L"http://", 0) != 0) return 0;
    const size_t q = u.find_first_of(L"?#");
    const std::wstring base = minuscolo(q == std::wstring::npos ? u : u.substr(0, q));
    if (finisceCon(base, L".png") || finisceCon(base, L".ico")) return 3;
    if (finisceCon(base, L".jpg") || finisceCon(base, L".jpeg") || finisceCon(base, L".webp")) return 2;
    return 1;
}

static inline void estraiCandidatiDataJson(const std::wstring& json,
                                           std::vector<CandidatoIcona>& out,
                                           std::unordered_set<std::wstring>& visti,
                                           int limite) {
    if (limite <= 0) return;
    size_t i = trovaChiaveJson(json, L"data");
    if (i == std::wstring::npos || i >= json.size() || json[i] != L'[') return;
    i++;
    int arrDepth = 1;
    int objDepth = 0;
    std::wstring curUrl, curThumb;
    std::vector<std::pair<int, CandidatoIcona>> trovati;

    while (i < json.size() && arrDepth > 0) {
        const wchar_t ch = json[i];
        if (ch == L'"') {
            std::wstring s;
            if (!leggiStringaJson(json, i, s)) break;
            if (arrDepth == 1 && objDepth == 1) {
                size_t j = i;
                while (j < json.size() && iswspace(json[j])) j++;
                if (j < json.size() && json[j] == L':') {
                    j++;
                    while (j < json.size() && iswspace(json[j])) j++;
                    if ((s == L"url" || s == L"thumb") && j < json.size() && json[j] == L'"') {
                        std::wstring val;
                        if (leggiStringaJson(json, j, val)) {
                            if (s == L"url") curUrl = std::move(val);
                            else curThumb = std::move(val);
                        }
                        i = j;
                        continue;
                    }
                }
            }
        } else if (ch == L'{') {
            if (arrDepth == 1 && objDepth == 0) {
                curUrl.clear();
                curThumb.clear();
            }
            objDepth++;
            i++;
        } else if (ch == L'}') {
            if (objDepth > 0) {
                objDepth--;
                if (arrDepth == 1 && objDepth == 0) {
                    const int pUrl = punteggioUrlIcona(curUrl);
                    if (pUrl > 0) {
                        std::wstring th = punteggioUrlIcona(curThumb) > 0 ? curThumb : curUrl;
                        trovati.push_back({pUrl, {std::move(th), std::move(curUrl)}});
                    }
                    curUrl.clear();
                    curThumb.clear();
                }
            }
            i++;
        } else if (ch == L'[') {
            arrDepth++;
            i++;
        } else if (ch == L']') {
            arrDepth--;
            i++;
        } else {
            i++;
        }
    }

    std::stable_sort(trovati.begin(), trovati.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    int aggiunti = 0;
    for (auto& [punti, cand] : trovati) {
        (void)punti;
        if (aggiunti >= limite) break;
        if (visti.insert(cand.fullUrl).second) {
            out.push_back(std::move(cand));
            aggiunti++;
        }
    }
}

static inline auto estraiIdArrayJson(const std::wstring& json, const wchar_t* chiaveArray, int maxId) -> std::vector<std::wstring> {
    std::vector<std::wstring> out;
    if (maxId <= 0) return out;
    size_t i = trovaChiaveJson(json, chiaveArray);
    if (i == std::wstring::npos || i >= json.size() || json[i] != L'[') return out;
    i++;
    int arrDepth = 1;
    int objDepth = 0;
    std::unordered_set<std::wstring> visti;

    while (i < json.size() && arrDepth > 0 && (int)out.size() < maxId) {
        const wchar_t ch = json[i];
        if (ch == L'"') {
            std::wstring s;
            if (!leggiStringaJson(json, i, s)) break;
            if (arrDepth == 1 && objDepth == 1 && s == L"id") {
                size_t j = i;
                while (j < json.size() && iswspace(json[j])) j++;
                if (j < json.size() && json[j] == L':') {
                    j++;
                    while (j < json.size() && iswspace(json[j])) j++;
                    if (j < json.size() && iswdigit(json[j])) {
                        std::wstring id;
                        while (j < json.size() && iswdigit(json[j])) id.push_back(json[j++]);
                        if (!id.empty() && visti.insert(id).second) out.push_back(std::move(id));
                        i = j;
                        continue;
                    }
                }
            }
        } else if (ch == L'{') {
            objDepth++;
            i++;
        } else if (ch == L'}') {
            if (objDepth > 0) objDepth--;
            i++;
        } else if (ch == L'[') {
            arrDepth++;
            i++;
        } else if (ch == L']') {
            arrDepth--;
            i++;
        } else {
            i++;
        }
    }
    return out;
}

template <class Gioco>
static inline auto cercaCandidatiIconaOnline(const Gioco& a, const std::wstring& apiKey, int maxRis = 16) -> std::vector<CandidatoIcona> {
    std::vector<CandidatoIcona> out;
    if (maxRis <= 0) return out;
    std::unordered_set<std::wstring> visti;

    auto estraiCifre = [](const std::wstring& s, size_t pos) -> std::wstring {
        std::wstring r;
        while (pos < s.size() && iswdigit(s[pos])) r.push_back(s[pos++]);
        return r;
    };

    const std::wstring avv = minuscolo(a.avvio);
    std::wstring platform, pid;
    if (avv.rfind(L"steam://rungameid/", 0) == 0) {
        platform = L"steam";
        pid = estraiCifre(avv, 18);
    } else if (avv.rfind(L"uplay://launch/", 0) == 0) {
        platform = L"uplay";
        pid = estraiCifre(avv, 15);
    } else if (avv.rfind(L"goggalaxy://opengameview/", 0) == 0) {
        platform = L"gog";
        pid = estraiCifre(avv, 25);
    }

    if (!apiKey.empty()) {
        auto aggiungiSgdb = [&](const std::wstring& ep, int lim) {
            const int spazio = std::min(lim, maxRis - (int)out.size());
            if (spazio <= 0) return;
            const std::string body = httpPrendi(L"https://www.steamgriddb.com/api/v2/" + ep, apiKey, 2 * 1024 * 1024);
            if (!body.empty()) estraiCandidatiDataJson(utf8AWide(body), out, visti, spazio);
        };

        if (!platform.empty() && !pid.empty()) {
            aggiungiSgdb(L"icons/" + platform + L"/" + pid, 12);
            aggiungiSgdb(L"grids/" + platform + L"/" + pid + L"?dimensions=512x512,1024x1024", 6);
        }
        if ((int)out.size() < 4 && !a.nome.empty()) {
            const std::string sr = httpPrendi(L"https://www.steamgriddb.com/api/v2/search/autocomplete/" + codificaUrl(a.nome), apiKey, 2 * 1024 * 1024);
            const std::wstring gid = sr.empty() ? L"" : primoIdInArrayJson(utf8AWide(sr), L"data");
            if (!gid.empty()) {
                aggiungiSgdb(L"icons/game/" + gid, 12);
                aggiungiSgdb(L"grids/game/" + gid + L"?dimensions=512x512,1024x1024", 6);
            }
        }
    }

    if ((apiKey.empty() || (int)out.size() < 4) && (int)out.size() < maxRis) {
        auto aggiungiSteamUrl = [&](const std::wstring& u) {
            if ((int)out.size() >= maxRis) return;
            if (visti.insert(u).second) out.push_back({u, u});
        };
        auto aggiungiSteamId = [&](const std::wstring& sid, bool conCapsule) {
            if (sid.empty() || (int)out.size() >= maxRis) return;
            const std::wstring pref = L"https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/" + sid + L"/";
            aggiungiSteamUrl(pref + L"library_600x900.jpg");
            aggiungiSteamUrl(pref + L"header.jpg");
            if (conCapsule) aggiungiSteamUrl(pref + L"capsule_616x353.jpg");
        };

        if (platform == L"steam" && !pid.empty()) {
            aggiungiSteamId(pid, true);
        }
        if ((int)out.size() < maxRis && !a.nome.empty()) {
            const std::string sr = httpPrendi(L"https://store.steampowered.com/api/storesearch/?term=" + codificaUrl(a.nome) + L"&l=english&cc=US", L"", 2 * 1024 * 1024);
            if (!sr.empty()) {
                const auto ids = estraiIdArrayJson(utf8AWide(sr), L"items", 4);
                for (const auto& sid : ids) {
                    if ((int)out.size() >= maxRis) break;
                    aggiungiSteamId(sid, false);
                }
            }
        }
    }

    return out;
}

static inline bool decodificaImmagineMemoria(const std::string& bytes, int lato, std::vector<BYTE>& out) {
    out.clear();
    if (bytes.empty() || lato <= 0 || bytes.size() > 32 * 1024 * 1024) return false;

    IWICImagingFactory* wic = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapClipper* cl = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && wic &&
        SUCCEEDED(wic->CreateStream(&stream)) && stream &&
        SUCCEEDED(stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && dec) {
        UINT cnt = 0;
        dec->GetFrameCount(&cnt);
        UINT best = 0;
        ULONGLONG bestArea = 0;
        for (UINT idx = 0; idx < cnt; idx++) {
            IWICBitmapFrameDecode* tf = nullptr;
            if (SUCCEEDED(dec->GetFrame(idx, &tf)) && tf) {
                UINT tw = 0, th = 0;
                if (SUCCEEDED(tf->GetSize(&tw, &th)) && (ULONGLONG)tw * th > bestArea) {
                    bestArea = (ULONGLONG)tw * th;
                    best = idx;
                }
                tf->Release();
            }
        }
        UINT fw = 0, fh = 0;
        if (SUCCEEDED(dec->GetFrame(best, &fr)) && fr && SUCCEEDED(fr->GetSize(&fw, &fh)) && fw && fh) {
            IWICBitmapSource* src = fr;
            if (fw != fh) {
                const UINT q = std::min(fw, fh);
                const WICRect rc{(INT)(fw - q) / 2, (INT)(fh - q) / 2, (INT)q, (INT)q};
                if (SUCCEEDED(wic->CreateBitmapClipper(&cl)) && cl && SUCCEEDED(cl->Initialize(fr, &rc))) src = cl;
                else src = nullptr;
            }
            if (src && SUCCEEDED(wic->CreateBitmapScaler(&sc)) && sc) {
                HRESULT hrScale = sc->Initialize(src, (UINT)lato, (UINT)lato, WICBitmapInterpolationModeFant);
                if (FAILED(hrScale)) hrScale = sc->Initialize(src, (UINT)lato, (UINT)lato, WICBitmapInterpolationModeCubic);
                if (SUCCEEDED(hrScale) &&
                    SUCCEEDED(wic->CreateFormatConverter(&cv)) && cv &&
                    SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                    const size_t stride = (size_t)lato * 4;
                    out.resize(stride * (size_t)lato);
                    if (SUCCEEDED(cv->CopyPixels(nullptr, (UINT)stride, (UINT)out.size(), out.data()))) {
                        ok = true;
                    } else {
                        out.clear();
                    }
                }
            }
        }
    }

    for (IUnknown* u : {(IUnknown*)cv, (IUnknown*)sc, (IUnknown*)cl, (IUnknown*)fr, (IUnknown*)dec, (IUnknown*)stream, (IUnknown*)wic}) {
        if (u) u->Release();
    }
    return ok;
}

inline auto Motore::leggiTuttiGiochi(bool subito) -> void {
    gLetto = adesso();
    if (subito) { Cronometro cr(this, "leggiTuttiGiochi"); applicaTuttiGiochi(elencaTuttiGiochi()); return; }
    if (lavoroGiochi) return;
    lavoroGiochi = true;
    auto ris = std::make_shared<std::vector<AppL>>();
    inSottofondo([ris] { *ris = elencaTuttiGiochi(); }, [this, ris] { lavoroGiochi = false; applicaTuttiGiochi(std::move(*ris)); });
}

inline auto Motore::ordinaGiochi() -> void {
    const bool rec = lOrdineRecenti;
    std::sort(tuttiGiochi.begin(), tuttiGiochi.end(), [rec](const AppL& a, const AppL& b) {
        if (a.preferito != b.preferito) return a.preferito > b.preferito;
        if (rec && a.ultimoAvvio != b.ultimoAvvio) return a.ultimoAvvio > b.ultimoAvvio;
        const int c = lstrcmpiW(a.nome.c_str(), b.nome.c_str());
        if (c != 0) return c < 0;
        if (a.avvio != b.avvio) return a.avvio < b.avvio;
        return a.argomenti < b.argomenti;
    });
}

inline auto Motore::applicaTuttiGiochi(std::vector<AppL> nuovi) -> void {
    for (auto& a : nuovi) {
        const std::wstring s = sorgenteIconaL(a);
        for (auto& v : tuttiGiochi) {
            if (v.avvio == a.avvio && v.argomenti == a.argomenti && sorgenteIconaL(v) == s) {
                a.icona = v.icona; a.provata = v.provata; v.icona = nullptr; break;
            }
        }
    }
    for (auto& v : tuttiGiochi) if (v.icona) v.icona->Release();
    tuttiGiochi = std::move(nuovi);
    lOrdineRecenti = leggiOrdineRecenti();
    ordinaGiochi();
    aggiornaGiochiInEsecuzione();
    {
        const int sel = lSel; const double scr = lScrollObj;
        filtra();
        if (lAperto) { lSel = std::clamp(sel, 0, std::max(0, (int)filtrate.size() - 1)); lScrollObj = std::clamp(scr, 0.0, lScrollMax()); }
    }
    for (auto& a : tuttiGiochi) if (!a.icona && !a.provata) { a.provata = true; chiediIcona(sorgenteIconaL(a), 96); }
    if (!lModoGiochi) for (auto& a : tutteApp) if (!a.icona && !a.provata && (!mostraGiochi || !appartieneAiGiochi(a))) { a.provata = true; chiediIcona(sorgenteIconaL(a), 96); }
    if (registro) { char b[64]; snprintf(b, 64, "launcher giochi: %d", (int)tuttiGiochi.size()); scrivi(b); }
}

inline auto Motore::elencaTuttiGiochi() -> std::vector<AppL> {
    std::unordered_map<std::wstring, AppL> steam;
    std::unordered_map<std::wstring, AppL> ubisoft;
    struct RiotCand {
        std::wstring nome, product, patchline, cartella, iconaFile, uninstall;
    };
    std::unordered_map<std::wstring, RiotCand> riotCand;
    std::vector<AppL> blizzardCand;

    auto pulisciIcona = [](std::wstring s) -> std::wstring {
        s = rimuoviVirgolette(std::move(s));
        const size_t virg = s.rfind(L',');
        if (virg != std::wstring::npos && virg + 1 < s.size()) {
            bool cifre = true;
            for (size_t k = virg + 1; k < s.size(); k++) {
                if (k == virg + 1 && s[k] == L'-') continue;
                if (!iswdigit(s[k])) { cifre = false; break; }
            }
            if (cifre) {
                std::wstring prima = s.substr(0, virg);
                while (!prima.empty() && prima.back() == L' ') prima.pop_back();
                prima = rimuoviVirgolette(std::move(prima));
                if (esisteFile(prima) || finisceCon(prima, L".ico") || finisceCon(prima, L".exe")) s = std::move(prima);
            }
        }
        return s;
    };

    auto raccogliRiotSub = [&](const wchar_t* nomeSub, HKEY hSub) {
        const std::wstring suff = nomeSub + 10;
        const size_t dot = suff.find(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= suff.size()) return;
        const std::wstring product = suff.substr(0, dot);
        const std::wstring patchline = suff.substr(dot + 1);
        if (_wcsicmp(product.c_str(), L"Riot_Client") == 0) return;
        const std::wstring key = minuscolo(suff);
        RiotCand& rc = riotCand[key];
        if (rc.product.empty()) rc.product = product;
        if (rc.patchline.empty()) rc.patchline = patchline;
        const std::wstring dName = leggiValoreReg(hSub, L"DisplayName");
        const std::wstring dIcon = pulisciIcona(leggiValoreReg(hSub, L"DisplayIcon"));
        const std::wstring dLoc = normalizzaPercorso(rimuoviVirgolette(leggiValoreReg(hSub, L"InstallLocation")));
        const std::wstring uninst = leggiValoreReg(hSub, L"UninstallString");
        if (rc.nome.empty() && !dName.empty()) rc.nome = dName;
        if (rc.cartella.empty() && !dLoc.empty()) rc.cartella = dLoc;
        if (rc.iconaFile.empty() && esisteFile(dIcon)) rc.iconaFile = dIcon;
        if (rc.uninstall.empty() && !uninst.empty()) rc.uninstall = uninst;
    };

    const wchar_t* const radiciReg[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
    };
    for (const wchar_t* rad : radiciReg) {
        for (REGSAM vista : {(REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY}) {
            HKEY hRad = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, rad, 0, KEY_READ | vista, &hRad) != ERROR_SUCCESS) continue;
            wchar_t nomeSub[256];
            for (DWORD idx = 0; ; idx++) {
                DWORD len = 256;
                if (RegEnumKeyExW(hRad, idx, nomeSub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                HKEY hSub = nullptr;
                if (RegOpenKeyExW(hRad, nomeSub, 0, KEY_READ | vista, &hSub) != ERROR_SUCCESS) continue;
                if (wcsncmp(nomeSub, L"Steam App ", 10) == 0) {
                    const std::wstring appid = nomeSub + 10;
                    if (!appid.empty() && appid != L"228980") {
                        const std::wstring dName = leggiValoreReg(hSub, L"DisplayName");
                        const std::wstring dIcon = pulisciIcona(leggiValoreReg(hSub, L"DisplayIcon"));
                        const std::wstring dLoc = normalizzaPercorso(rimuoviVirgolette(leggiValoreReg(hSub, L"InstallLocation")));
                        AppL& cur = steam[minuscolo(appid)];
                        if (cur.avvio.empty()) cur.avvio = L"steam://rungameid/" + appid;
                        if (cur.nome.empty() && !dName.empty()) cur.nome = dName;
                        if (cur.cartella.empty() && !dLoc.empty()) cur.cartella = dLoc;
                        if (cur.iconaFile.empty() && esisteFile(dIcon)) cur.iconaFile = dIcon;
                    }
                } else if (wcsncmp(nomeSub, L"Uplay Install ", 14) == 0) {
                    const std::wstring id = nomeSub + 14;
                    if (!id.empty()) {
                        const std::wstring dName = leggiValoreReg(hSub, L"DisplayName");
                        const std::wstring dIcon = pulisciIcona(leggiValoreReg(hSub, L"DisplayIcon"));
                        const std::wstring dLoc = normalizzaPercorso(rimuoviVirgolette(leggiValoreReg(hSub, L"InstallLocation")));
                        AppL& cur = ubisoft[minuscolo(id)];
                        if (cur.avvio.empty()) cur.avvio = L"uplay://launch/" + id + L"/0";
                        if (cur.nome.empty() && !dName.empty()) cur.nome = dName;
                        if (cur.cartella.empty() && !dLoc.empty()) cur.cartella = dLoc;
                        if (cur.iconaFile.empty() && esisteFile(dIcon)) cur.iconaFile = dIcon;
                    }
                } else if (wcsncmp(nomeSub, L"Riot Game ", 10) == 0) {
                    raccogliRiotSub(nomeSub, hSub);
                } else {
                    const std::wstring pub = minuscolo(leggiValoreReg(hSub, L"Publisher"));
                    if (pub.find(L"blizzard entertainment") != std::wstring::npos) {
                        const std::wstring dName = leggiValoreReg(hSub, L"DisplayName");
                        if (!dName.empty() && _wcsicmp(dName.c_str(), L"Battle.net") != 0 &&
                            _wcsicmp(dName.c_str(), L"Blizzard Battle.net") != 0 &&
                            _wcsicmp(dName.c_str(), L"Blizzard Battle.net App") != 0) {
                            const std::wstring dLoc = normalizzaPercorso(rimuoviVirgolette(leggiValoreReg(hSub, L"InstallLocation")));
                            const std::wstring dIcon = pulisciIcona(leggiValoreReg(hSub, L"DisplayIcon"));
                            if (!dLoc.empty()) {
                                AppL b;
                                b.nome = dName;
                                b.cartella = dLoc;
                                b.iconaFile = dIcon;
                                blizzardCand.push_back(std::move(b));
                            }
                        }
                    }
                }
                RegCloseKey(hSub);
            }
            RegCloseKey(hRad);
        }
    }

    {
        HKEY hCu = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ, &hCu) == ERROR_SUCCESS) {
            wchar_t nomeSub[256];
            for (DWORD idx = 0; ; idx++) {
                DWORD len = 256;
                if (RegEnumKeyExW(hCu, idx, nomeSub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                if (wcsncmp(nomeSub, L"Riot Game ", 10) != 0) continue;
                HKEY hSub = nullptr;
                if (RegOpenKeyExW(hCu, nomeSub, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;
                raccogliRiotSub(nomeSub, hSub);
                RegCloseKey(hSub);
            }
            RegCloseKey(hCu);
        }
    }

    std::wstring steamPath;
    {
        HKEY hSt = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hSt) == ERROR_SUCCESS) {
            steamPath = normalizzaPercorso(leggiValoreReg(hSt, L"SteamPath"));
            RegCloseKey(hSt);
        }
    }

    std::vector<std::wstring> librerie, xboxRoots;
    std::unordered_set<std::wstring> libViste;
    auto aggiungiLib = [&](std::wstring p) {
        p = normalizzaPercorso(std::move(p));
        if (p.empty() || !esisteCartella(p)) return;
        if (libViste.insert(minuscolo(p)).second) librerie.push_back(std::move(p));
    };
    if (!steamPath.empty()) {
        aggiungiLib(steamPath);
        const std::wstring vdf = leggiFileUtf8(steamPath + L"\\steamapps\\libraryfolders.vdf");
        size_t pos = 0;
        while (pos < vdf.size()) {
            size_t fine = vdf.find(L'\n', pos);
            if (fine == std::wstring::npos) fine = vdf.size();
            std::wstring riga = vdf.substr(pos, fine - pos);
            if (!riga.empty() && riga.back() == L'\r') riga.pop_back();
            pos = fine + 1;
            std::wstring k, v;
            if (coppiaVdf(riga, k, v) && !_wcsicmp(k.c_str(), L"path")) aggiungiLib(std::move(v));
        }
    }

    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        const wchar_t rad[4] = {(wchar_t)(L'A' + i), L':', L'\\', L'\0'};
        if (GetDriveTypeW(rad) != DRIVE_FIXED) continue;
        const std::wstring sl = std::wstring(rad) + L"SteamLibrary";
        if (esisteCartella(sl)) aggiungiLib(sl);
        const std::wstring xb = std::wstring(rad) + L"Xbox Games";
        if (esisteCartella(xb)) xboxRoots.push_back(xb);
    }

    for (const auto& lib : librerie) {
        const std::wstring apps = lib + L"\\steamapps";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((apps + L"\\appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring txt = leggiFileUtf8(apps + L"\\" + fd.cFileName);
            std::wstring appid, name, installdir;
            size_t pos = 0;
            while (pos < txt.size()) {
                size_t fine = txt.find(L'\n', pos);
                if (fine == std::wstring::npos) fine = txt.size();
                std::wstring riga = txt.substr(pos, fine - pos);
                if (!riga.empty() && riga.back() == L'\r') riga.pop_back();
                pos = fine + 1;
                std::wstring k, v;
                if (!coppiaVdf(riga, k, v)) continue;
                if (!_wcsicmp(k.c_str(), L"appid")) appid = std::move(v);
                else if (!_wcsicmp(k.c_str(), L"name")) name = std::move(v);
                else if (!_wcsicmp(k.c_str(), L"installdir")) installdir = std::move(v);
            }
            if (appid.empty() || appid == L"228980") continue;
            const std::wstring cart = installdir.empty() ? L"" : normalizzaPercorso(apps + L"\\common\\" + installdir);
            AppL& cur = steam[minuscolo(appid)];
            if (cur.avvio.empty()) cur.avvio = L"steam://rungameid/" + appid;
            if (cur.nome.empty()) cur.nome = !name.empty() ? name : installdir;
            if ((cur.cartella.empty() || !esisteCartella(cur.cartella)) && !cart.empty() && esisteCartella(cart)) cur.cartella = cart;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::vector<AppL> nuove;
    std::unordered_set<std::wstring> coperte;
    for (auto& [idMin, a] : steam) {
        if (a.nome.empty()) continue;
        if (!a.cartella.empty()) coperte.insert(minuscolo(normalizzaPercorso(a.cartella)));
        if (a.iconaFile.empty()) {
            a.iconaFile = trovaIconaSteamCache(steamPath, idMin);
            if (a.iconaFile.empty() && !a.cartella.empty()) a.iconaFile = trovaExeGioco(a.cartella);
        }
        a.minuscolo = minuscolo(a.nome);
        nuove.push_back(std::move(a));
    }

    std::wstring progData;
    {
        wchar_t pd[MAX_PATH] = L"";
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, pd)) && pd[0])
            progData = normalizzaPercorso(pd);
    }

    {
        std::vector<std::wstring> epicDirs;
        std::unordered_set<std::wstring> epicDirViste;
        auto aggiungiEpicDir = [&](std::wstring d) {
            d = normalizzaPercorso(std::move(d));
            if (d.empty() || !esisteCartella(d)) return;
            if (epicDirViste.insert(minuscolo(d)).second) epicDirs.push_back(std::move(d));
        };
        const wchar_t* const epicReg[] = {
            L"SOFTWARE\\WOW6432Node\\Epic Games\\EpicGamesLauncher",
            L"SOFTWARE\\Epic Games\\EpicGamesLauncher"
        };
        for (const wchar_t* r : epicReg) {
            for (REGSAM vista : {(REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY}) {
                HKEY hEp = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, r, 0, KEY_READ | vista, &hEp) != ERROR_SUCCESS) continue;
                const std::wstring ad = normalizzaPercorso(rimuoviVirgolette(leggiValoreReg(hEp, L"AppDataPath")));
                RegCloseKey(hEp);
                if (!ad.empty()) aggiungiEpicDir(ad + L"\\Manifests");
            }
        }
        if (!progData.empty()) aggiungiEpicDir(progData + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests");
        else aggiungiEpicDir(L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests");

        std::unordered_set<std::wstring> epicVisti;
        for (const auto& dir : epicDirs) {
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((dir + L"\\*.item").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::wstring txt = leggiFileUtf8(dir + L"\\" + fd.cFileName);
                if (txt.empty() || boolJson(txt, L"bIsIncompleteInstall")) continue;
                const std::wstring displayName = valoreJson(txt, L"DisplayName");
                const std::wstring appName = valoreJson(txt, L"AppName");
                if (displayName.empty() || appName.empty()) continue;
                const std::wstring mainGame = valoreJson(txt, L"MainGameAppName");
                if (!mainGame.empty() && _wcsicmp(mainGame.c_str(), appName.c_str()) != 0) continue;
                const auto cats = arrayStringheJson(txt, L"AppCategories");
                bool haGiochi = false, haMotore = false;
                for (const auto& c : cats) {
                    const std::wstring cm = minuscolo(c);
                    if (cm == L"games") haGiochi = true;
                    else if (cm == L"engines" || cm == L"plugins") haMotore = true;
                }
                if (haMotore && !haGiochi) continue;
                const std::wstring installLoc = normalizzaPercorso(valoreJson(txt, L"InstallLocation"));
                if (!esisteCartella(installLoc)) continue;
                if (!epicVisti.insert(minuscolo(appName)).second) continue;
                const std::wstring ns = valoreJson(txt, L"CatalogNamespace");
                const std::wstring cid = valoreJson(txt, L"CatalogItemId");
                const std::wstring launchExe = valoreJson(txt, L"LaunchExecutable");
                AppL a;
                a.nome = displayName;
                if (!ns.empty() && !cid.empty())
                    a.avvio = L"com.epicgames.launcher://apps/" + ns + L"%3A" + cid + L"%3A" + appName + L"?action=launch&silent=true";
                else
                    a.avvio = L"com.epicgames.launcher://apps/" + appName + L"?action=launch&silent=true";
                a.cartella = installLoc;
                coperte.insert(minuscolo(installLoc));
                if (!launchExe.empty()) {
                    const std::wstring cand = normalizzaPercorso(installLoc + L"\\" + launchExe);
                    if (esisteFile(cand)) a.iconaFile = cand;
                }
                if (a.iconaFile.empty()) a.iconaFile = trovaExeGioco(installLoc);
                a.minuscolo = minuscolo(a.nome);
                nuove.push_back(std::move(a));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    {
        const wchar_t* const ubiReg[] = {
            L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs",
            L"SOFTWARE\\Ubisoft\\Launcher\\Installs"
        };
        for (const wchar_t* r : ubiReg) {
            for (REGSAM vista : {(REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY}) {
                HKEY hUbi = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, r, 0, KEY_READ | vista, &hUbi) != ERROR_SUCCESS) continue;
                wchar_t nomeSub[256];
                for (DWORD idx = 0; ; idx++) {
                    DWORD len = 256;
                    if (RegEnumKeyExW(hUbi, idx, nomeSub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    const std::wstring id = nomeSub;
                    if (id.empty()) continue;
                    HKEY hSub = nullptr;
                    if (RegOpenKeyExW(hUbi, nomeSub, 0, KEY_READ | vista, &hSub) != ERROR_SUCCESS) continue;
                    const std::wstring dir = normalizzaPercorso(rimuoviVirgolette(leggiValoreReg(hSub, L"InstallDir")));
                    RegCloseKey(hSub);
                    if (!esisteCartella(dir)) continue;
                    AppL& cur = ubisoft[minuscolo(id)];
                    if (cur.avvio.empty()) cur.avvio = L"uplay://launch/" + id + L"/0";
                    if (cur.cartella.empty() || !esisteCartella(cur.cartella)) cur.cartella = dir;
                    if (cur.nome.empty()) {
                        const size_t bar = dir.find_last_of(L'\\');
                        cur.nome = bar == std::wstring::npos ? dir : dir.substr(bar + 1);
                    }
                }
                RegCloseKey(hUbi);
            }
        }
        for (auto& [idMin, a] : ubisoft) {
            (void)idMin;
            if (a.nome.empty() || !esisteCartella(a.cartella)) continue;
            coperte.insert(minuscolo(a.cartella));
            if (a.iconaFile.empty() || !esisteFile(a.iconaFile)) a.iconaFile = trovaExeGioco(a.cartella);
            a.minuscolo = minuscolo(a.nome);
            nuove.push_back(std::move(a));
        }
    }

    {
        const wchar_t* const gogReg[] = {
            L"SOFTWARE\\WOW6432Node\\GOG.com\\Games",
            L"SOFTWARE\\GOG.com\\Games"
        };
        std::unordered_set<std::wstring> gogVisti;
        for (const wchar_t* r : gogReg) {
            for (REGSAM vista : {(REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY}) {
                HKEY hGog = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, r, 0, KEY_READ | vista, &hGog) != ERROR_SUCCESS) continue;
                wchar_t nomeSub[256];
                for (DWORD idx = 0; ; idx++) {
                    DWORD len = 256;
                    if (RegEnumKeyExW(hGog, idx, nomeSub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    const std::wstring gameId = nomeSub;
                    if (gameId.empty() || !gogVisti.insert(minuscolo(gameId)).second) continue;
                    HKEY hSub = nullptr;
                    if (RegOpenKeyExW(hGog, nomeSub, 0, KEY_READ | vista, &hSub) != ERROR_SUCCESS) continue;
                    std::wstring dep = leggiValoreReg(hSub, L"dependsOn");
                    if (dep.empty()) dep = leggiValoreReg(hSub, L"DEPENDSON");
                    std::wstring gName = leggiValoreReg(hSub, L"gameName");
                    if (gName.empty()) gName = leggiValoreReg(hSub, L"GAMENAME");
                    std::wstring gPath = leggiValoreReg(hSub, L"path");
                    if (gPath.empty()) gPath = leggiValoreReg(hSub, L"PATH");
                    std::wstring gExe = leggiValoreReg(hSub, L"exe");
                    if (gExe.empty()) gExe = leggiValoreReg(hSub, L"EXE");
                    std::wstring gParam = leggiValoreReg(hSub, L"launchParam");
                    if (gParam.empty()) gParam = leggiValoreReg(hSub, L"LAUNCHPARAM");
                    RegCloseKey(hSub);
                    if (!dep.empty() || gName.empty()) continue;
                    gPath = normalizzaPercorso(rimuoviVirgolette(std::move(gPath)));
                    if (!esisteCartella(gPath)) continue;
                    gExe = normalizzaPercorso(rimuoviVirgolette(std::move(gExe)));
                    AppL a;
                    a.nome = gName;
                    a.cartella = gPath;
                    coperte.insert(minuscolo(gPath));
                    if (esisteFile(gExe)) {
                        a.avvio = gExe;
                        a.argomenti = gParam;
                        a.iconaFile = gExe;
                    } else {
                        a.avvio = L"goggalaxy://openGameView/" + gameId;
                        a.iconaFile = trovaExeGioco(gPath);
                    }
                    a.minuscolo = minuscolo(a.nome);
                    nuove.push_back(std::move(a));
                }
                RegCloseKey(hGog);
            }
        }
    }

    {
        std::wstring riotClient;
        if (!progData.empty()) {
            const std::wstring txt = leggiFileUtf8(progData + L"\\Riot Games\\RiotClientInstalls.json");
            if (!txt.empty()) {
                riotClient = normalizzaPercorso(valoreJson(txt, L"rc_live"));
                if (!esisteFile(riotClient)) riotClient = normalizzaPercorso(valoreJson(txt, L"rc_default"));
                if (!esisteFile(riotClient)) riotClient.clear();
            }
        }
        for (auto& [k, rc] : riotCand) {
            (void)k;
            if (rc.nome.empty() || !esisteCartella(rc.cartella)) continue;
            std::wstring clientExe = riotClient;
            if (!esisteFile(clientExe) && !rc.uninstall.empty())
                clientExe = normalizzaPercorso(estraiExeDaComando(rc.uninstall));
            if (!esisteFile(clientExe)) continue;
            AppL a;
            a.nome = rc.nome;
            a.avvio = clientExe;
            a.argomenti = L"--launch-product=" + rc.product + L" --launch-patchline=" + rc.patchline;
            a.cartella = rc.cartella;
            a.iconaFile = esisteFile(rc.iconaFile) ? rc.iconaFile : trovaExeGioco(rc.cartella);
            a.minuscolo = minuscolo(a.nome);
            coperte.insert(minuscolo(rc.cartella));
            nuove.push_back(std::move(a));
        }
    }

    {
        const wchar_t* const eaReg[] = {
            L"SOFTWARE\\WOW6432Node\\EA Games",
            L"SOFTWARE\\EA Games",
            L"SOFTWARE\\WOW6432Node\\Electronic Arts",
            L"SOFTWARE\\Electronic Arts"
        };
        static const wchar_t* const eaScarta[] = {
            L"EA Core", L"EA Desktop", L"EA app", L"Origin", L"EADM", L"EA Link", L"EA Background Service"
        };
        for (const wchar_t* r : eaReg) {
            for (REGSAM vista : {(REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY}) {
                HKEY hEa = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, r, 0, KEY_READ | vista, &hEa) != ERROR_SUCCESS) continue;
                wchar_t nomeSub[256];
                for (DWORD idx = 0; ; idx++) {
                    DWORD len = 256;
                    if (RegEnumKeyExW(hEa, idx, nomeSub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    bool salta = false;
                    for (const wchar_t* s : eaScarta) if (!_wcsicmp(nomeSub, s)) { salta = true; break; }
                    if (salta) continue;
                    HKEY hSub = nullptr;
                    if (RegOpenKeyExW(hEa, nomeSub, 0, KEY_READ | vista, &hSub) != ERROR_SUCCESS) continue;
                    std::wstring dName = leggiValoreReg(hSub, L"DisplayName");
                    std::wstring dir = leggiValoreReg(hSub, L"Install Dir");
                    if (dir.empty()) dir = leggiValoreReg(hSub, L"InstallDir");
                    RegCloseKey(hSub);
                    if (dName.empty()) dName = nomeSub;
                    dir = normalizzaPercorso(rimuoviVirgolette(std::move(dir)));
                    if (!esisteCartella(dir)) continue;
                    const std::wstring dirMin = minuscolo(dir);
                    if (coperte.count(dirMin)) continue;
                    const std::wstring exe = trovaExeGioco(dir);
                    if (exe.empty()) continue;
                    coperte.insert(dirMin);
                    AppL a;
                    a.nome = dName;
                    a.avvio = exe;
                    a.iconaFile = exe;
                    a.cartella = dir;
                    a.minuscolo = minuscolo(a.nome);
                    nuove.push_back(std::move(a));
                }
                RegCloseKey(hEa);
            }
        }
    }

    for (auto& b : blizzardCand) {
        if (!esisteCartella(b.cartella)) continue;
        const std::wstring dirMin = minuscolo(b.cartella);
        if (coperte.count(dirMin)) continue;
        std::wstring exe = trovaExeGioco(b.cartella);
        if (exe.empty() && finisceCon(b.iconaFile, L".exe") && esisteFile(b.iconaFile)) exe = b.iconaFile;
        if (exe.empty()) continue;
        coperte.insert(dirMin);
        b.avvio = exe;
        b.iconaFile = esisteFile(b.iconaFile) ? b.iconaFile : exe;
        b.minuscolo = minuscolo(b.nome);
        nuove.push_back(std::move(b));
    }

    auto scansionaCartelle = [&](const std::wstring& radice, bool filtraSteam) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((radice + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (filtraSteam && (!_wcsicmp(fd.cFileName, L"Steamworks Shared") || !_wcsicmp(fd.cFileName, L"Steam Controller Configs"))) continue;
            const std::wstring dir = normalizzaPercorso(radice + L"\\" + fd.cFileName);
            const std::wstring dirMin = minuscolo(dir);
            if (coperte.count(dirMin)) continue;
            const std::wstring exe = trovaExeGioco(dir);
            if (exe.empty()) continue;
            coperte.insert(dirMin);
            AppL a;
            a.nome = fd.cFileName;
            a.avvio = exe;
            a.iconaFile = exe;
            a.cartella = dir;
            a.minuscolo = minuscolo(a.nome);
            nuove.push_back(std::move(a));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };

    for (const auto& lib : librerie) scansionaCartelle(lib + L"\\steamapps\\common", true);
    for (const auto& xb : xboxRoots) scansionaCartelle(xb, false);

    for (const auto& [nome, percorso] : leggiGiochiAggiunti()) {
        AppL a;
        a.nome = nome;
        a.manuale = true;
        a.minuscolo = minuscolo(nome);
        if (finisceCon(percorso, L".url")) {
            wchar_t url[2048] = L"", ico[MAX_PATH * 2] = L"";
            GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, 2048, percorso.c_str());
            GetPrivateProfileStringW(L"InternetShortcut", L"IconFile", L"", ico, MAX_PATH * 2, percorso.c_str());
            a.avvio = url[0] ? std::wstring(url) : percorso;
            a.iconaFile = (ico[0] && esisteFile(ico)) ? std::wstring(ico) : percorso;
        } else {
            a.avvio = percorso;
            a.iconaFile = percorso;
            if (finisceCon(percorso, L".exe") && esisteFile(percorso)) {
                const size_t bar = percorso.find_last_of(L"\\/");
                if (bar != std::wstring::npos) a.cartella = percorso.substr(0, bar);
            }
        }
        nuove.push_back(std::move(a));
    }

    const auto nascosti = leggiGiochiNascosti();
    nuove.erase(std::remove_if(nuove.begin(), nuove.end(), [&](const AppL& a) {
        return !a.manuale && nascosti.count(chiaveGioco(a)) != 0;
    }), nuove.end());

    const auto iconePers = leggiIconeGiochi();
    const auto nomiPers = leggiNomiGiochi();
    const auto preferiti = leggiGiochiPreferiti();
    const auto recenti = leggiGiochiRecenti();
    const auto opzioni = leggiOpzioniGiochi();
    for (AppL& a : nuove) {
        const std::wstring chiave = chiaveGioco(a);
        auto it = iconePers.find(chiave);
        if (it == iconePers.end()) it = iconePers.find(a.minuscolo);
        if (it != iconePers.end() && esisteFile(it->second)) {
            a.iconaFile = it->second;
            a.iconaPers = true;
        }
        auto itN = nomiPers.find(chiave);
        if (itN != nomiPers.end() && !itN->second.empty()) {
            a.nome = itN->second;
            a.minuscolo = minuscolo(a.nome);
            a.nomePers = true;
        }
        a.preferito = preferiti.count(chiave) != 0;
        if (auto itR = recenti.find(chiave); itR != recenti.end()) a.ultimoAvvio = itR->second;
        if (auto itO = opzioni.find(chiave); itO != opzioni.end()) {
            a.comeAdmin = itO->second.first;
            a.argomentiExtra = itO->second.second;
        }
    }

    auto prio = [](const AppL& x) -> int {
        if (!x.manuale && !x.argomenti.empty() && finisceCon(minuscolo(x.avvio), L"\\riotclientservices.exe"))
            return 2;
        if (x.manuale) return (!x.argomenti.empty() || !x.argomentiExtra.empty()) ? 3 : 1;
        return 0;
    };
    std::sort(nuove.begin(), nuove.end(), [&prio](const AppL& a, const AppL& b) {
        const int c = lstrcmpiW(a.nome.c_str(), b.nome.c_str());
        if (c != 0) return c < 0;
        const int pa = prio(a), pb = prio(b);
        if (pa != pb) return pa > pb;
        if (a.iconaPers != b.iconaPers) return a.iconaPers > b.iconaPers;
        const bool ia = !a.iconaFile.empty(), ib = !b.iconaFile.empty();
        if (ia != ib) return ia > ib;
        if (a.avvio != b.avvio) return a.avvio < b.avvio;
        return a.argomenti < b.argomenti;
    });
    nuove.erase(std::unique(nuove.begin(), nuove.end(), [](const AppL& a, const AppL& b) {
        return a.minuscolo == b.minuscolo;
    }), nuove.end());
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

inline auto Motore::appartieneAiGiochi(const AppL& a) const -> bool {
    std::wstring par = a.avvio;
    if (par.rfind(L"shell:AppsFolder\\", 0) == 0) par.erase(0, 17);
    const std::wstring parMin = minuscolo(normalizzaPercorso(std::move(par)));
    const std::wstring ta = chiaveTitolo(a.nome);
    for (const auto& g : tuttiGiochi) {
        const std::wstring avvG = minuscolo(normalizzaPercorso(g.avvio));
        const std::wstring icoG = minuscolo(normalizzaPercorso(g.iconaFile));
        if (!parMin.empty() && (parMin == avvG || (!icoG.empty() && parMin == icoG))) return true;
        if (!g.cartella.empty()) {
            const std::wstring cartG = minuscolo(normalizzaPercorso(g.cartella)) + L"\\";
            if (parMin.size() > cartG.size() && parMin.compare(0, cartG.size(), cartG) == 0) return true;
        }
        if (a.minuscolo == g.minuscolo || (!ta.empty() && ta == chiaveTitolo(g.nome))) return true;
    }
    return false;
}

inline auto Motore::filtra() -> void {
    filtrate.clear();
    const std::wstring q = minuscolo(query);
    const auto& el = elencoAttivo();
    std::vector<int> livelli[3];   // iniziali, contiene, lettere in ordine
    for (int i = 0; i < (int)el.size(); i++) {
        if (!lModoGiochi && mostraGiochi && appartieneAiGiochi(el[i])) continue;
        if (q.empty()) { filtrate.push_back(i); continue; }
        const std::wstring& n = el[i].minuscolo;
        const size_t p = n.find(q);
        if (p == 0 || (p != std::wstring::npos && n[p - 1] == L' ')) filtrate.push_back(i);
        else if (q.size() >= 2 && iniziali(n, q)) livelli[0].push_back(i);
        else if (p != std::wstring::npos) livelli[1].push_back(i);
        else if (q.size() >= 3 && sottosequenza(n, q)) livelli[2].push_back(i);
    }
    for (auto& l : livelli) filtrate.insert(filtrate.end(), l.begin(), l.end());
    lSel = 0; lScrollObj = 0; lDirty = true; lSelTastiera = false;
    // file e cartelle: testo nuovo -> ricerca 0,2 s dopo l'ultimo tasto (una per parola, non per lettera); testo vuoto
    // -> via i risultati e scartata una ricerca in corso. (filtra si chiama anche quando arriva l'elenco nuovo delle
    // app: col testo uguale non si ricerca)
    lSelFile = -1; lSopraFile = -1;
    if (lModoGiochi || query.empty()) { lFile.clear(); lFileTesto.clear(); lFileGen++; if (launcher) KillTimer(launcher, TIMER_FILE_L); }
    else if (query != lFileTesto && launcher) SetTimer(launcher, TIMER_FILE_L, 200, nullptr);
}

// ---- file e cartelle sotto la griglia (issue #14) ----

// Righe di file che entrano (fino a LF_MAX), lasciando alla griglia almeno una riga di app; -1 senza testo (niente
// sezione). L'indice LF_ESPLORA della riga "Cerca in Esplora file" e' questo stesso numero
inline auto Motore::lFileRighe() const -> int {
    if (lModoGiochi || query.empty()) return -1;
    const float spazio = LH - 14 * dpi - lGrigliaTop() - lCellaH() - (LF_TESTA + LF_ESPLORA) * dpi;
    const int entrano = std::max(0, (int)std::floor(spazio / (LF_RIGA * dpi)));
    return std::min({(int)lFile.size(), LF_MAX, entrano});
}

// altezza a cui va la sezione (lFileH la insegue in renderLauncher)
inline auto Motore::lFileAlto() const -> float {
    const int r = lFileRighe();
    return r < 0 ? 0.0f : (LF_TESTA + r * LF_RIGA + LF_ESPLORA) * dpi;
}

// riga della sezione sotto il punto: 0..righe-1 i file, righe = "Cerca in Esplora file", -1 fuori
inline auto Motore::lFileIn(float x, float y) const -> int {
    const int r = lFileRighe();
    if (r < 0 || x < 16 * dpi || x > LW - 16 * dpi) return -1;
    const float top = LH - 14 * dpi - (float)lFileH, fondo = LH - 14 * dpi;
    if (y < top || y >= fondo) return -1;
    const float y0 = top + LF_TESTA * dpi;
    if (y < y0) return -1;
    const int k = (int)((y - y0) / (LF_RIGA * dpi));
    if (k < r) return lFileTesto == query ? k : -1;   // (righe di un testo vecchio: si vedono sbiadite, non si toccano)
    const float ye = y0 + r * LF_RIGA * dpi;
    return y >= ye && y < ye + LF_ESPLORA * dpi ? r : -1;
}

// la ricerca nel thread; il risultato si usa solo se e' ancora del testo scritto (generazione e testo uguali)
// Una ricerca alla volta: con Windows Search lento le ricerche vecchie si accumulavano nel pool. Arrivata la risposta,
// se il testo e' cambiato intanto ne parte una sola col testo di adesso (R20261121-002818-S10)
inline auto Motore::cercaFileLauncher() -> void {
    if (lModoGiochi || query.empty() || !lAperto) return;
    if (lFileInCorso) { lFileAttesa = true; return; }
    const std::wstring q = query;
    const unsigned g = ++lFileGen;
    auto ris = std::make_shared<std::vector<indice::File>>();
    lFileInCorso = true; lFileAttesa = false;
    inSottofondo([q, ris] { *ris = indice::cerca(q, LF_MAX, true); },
                 [this, q, g, ris] {
                     lFileInCorso = false;
                     // (anche a testo uguale se la generazione e' cambiata: svuotato e riscritto lo stesso testo, la
                     // risposta vecchia si scarta e senza nuova ricerca l'elenco restava vuoto, R20261121-115814-S1)
                     if (lFileAttesa) { lFileAttesa = false; if (!lModoGiochi && lAperto && !lChiusura && !query.empty() && (q != query || g != lFileGen)) { cercaFileLauncher(); return; } }
                     if (lModoGiochi || g != lFileGen || !lAperto || lChiusura || q != query) return;
                     // "Cerca in Esplora file" scelto con le frecce resta lui anche se arrivano file (stava all'indice
                     // delle righe, e il primo file prendeva il suo posto: Invio apriva il file, R20261121-002818-S8)
                     const bool esplora = lSelFile >= 0 && lSelFile == lFileRighe();
                     lFile = std::move(*ris); lFileTesto = q;
                     if (esplora) lSelFile = lFileRighe();
                     // (le icone di file gia' viste restano; oltre 200 la cache si svuota)
                     if (iconeFile.size() > 200) { for (auto& [p, b] : iconeFile) if (b) b->Release(); iconeFile.clear(); }
                     for (const auto& f : lFile) if (!iconeFile.count(f.percorso)) chiediIcona(f.percorso, 48);
                     lSopraFile = -1; lSelFile = std::min(lSelFile, lFileRighe());
                     lDirty = true;
                 });
}

// apre il file (o la cartella) k, oppure la ricerca completa in Esplora file (k = righe)
inline auto Motore::apriFile(int k) -> void {
    const int r = lFileRighe();
    if (r < 0 || k < 0 || k > r) return;
    if (k == r) {
        const std::wstring cerca = indice::cercaInEsplora(query);
        chiudiLauncher();
        ShellExecuteW(nullptr, L"open", cerca.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        scrivi("launcher: ricerca in Esplora file");
        return;
    }
    // (solo risultati del testo scritto: R20261119-195000-S2, prima "fattura" -> "contratto", Giu' e Invio prima dei
    // risultati nuovi apriva la fattura)
    if (k >= (int)lFile.size() || lFileTesto != query) return;
    const std::wstring p = lFile[k].percorso;
    chiudiLauncher();
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    scrivi("launcher: file aperto");   // (il percorso no: e' dell'utente)
}

inline auto Motore::menuFile(int k, POINT pt) -> void {
    if (k < 0 || k >= (int)lFile.size() || k >= lFileRighe() || lFileTesto != query) return;
    const indice::File f = lFile[k];   // (copia: il menu ha un suo ciclo dei messaggi, l'elenco puo' cambiare)
    const int s = (int)menu::mostra({menu::titolo(f.nome), menu::voce(testi::t(L"Apri", L"Open"), 1),
                                     menu::voce(testi::t(L"Apri percorso file", L"Open file location"), 2)}, pt, launcher);
    if (s == 1) { chiudiLauncher(); ShellExecuteW(nullptr, L"open", f.percorso.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    else if (s == 2) { chiudiLauncher(); ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + f.percorso + L"\"").c_str(), nullptr, SW_SHOWNORMAL); }
}

// true se un'animazione della sezione e' in corso (il launcher deve ridisegnarsi)
inline auto Motore::disegnaFileLauncher(bool scuro, D2D1_COLOR_F ink) -> bool {
    bool muove = false;
    const int r = lFileRighe();
    if (lFileH < 1) return false;
    const float top = LH - 14 * dpi - (float)lFileH, fondo = LH - 14 * dpi;
    const D2D1_COLOR_F tenue = {ink.r, ink.g, ink.b, ink.a * 0.6f};
    const D2D1_COLOR_F blu = scuro ? D2D1::ColorF(0.40f, 0.68f, 1.0f, 1) : D2D1::ColorF(0.0f, 0.42f, 0.95f, 1);
    dc->PushAxisAlignedClip({0, top, LW, fondo}, D2D1_ANTIALIAS_MODE_ALIASED);
    // linea sopra, titolo
    pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.08f));
    dc->DrawLine({24 * dpi, top + 0.5f}, {LW - 24 * dpi, top + 0.5f}, pennello, 1.0f);
    pennello->SetColor(tenue);
    scrivi(testi::t(L"File e cartelle", L"Files and folders"), fmtSotto, {28 * dpi, top + 6 * dpi, LW - 28 * dpi, top + LF_TESTA * dpi});
    auto evidenziaRiga = [&](const D2D1_RECT_F& rr, int k) {
        const bool sopra = k == lSopraFile, scelta = k == lSelFile;
        if (!sopra && !scelta) return;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, sopra ? 0.12f : 0.08f) : D2D1::ColorF(1, 1, 1, sopra ? 0.55f : 0.4f));
        dc->FillRoundedRectangle({rr, 8 * dpi, 8 * dpi}, pennello);
        if (scelta) { pennello->SetColor(D2D1::ColorF(0.2f, 0.5f, 1.0f, 0.7f)); dc->DrawRoundedRectangle({rr, 8 * dpi, 8 * dpi}, pennello, 1.5f * dpi); }
    };
    float y = top + LF_TESTA * dpi;
    // Righe di un testo vecchio (si sta scrivendo, i risultati nuovi non sono arrivati): restano al loro posto, cosi'
    // la sezione non salta a ogni lettera, ma sbiadiscono e non si possono scegliere (R20261119-195000-S2). L'opacita'
    // va verso il suo valore con dolcezza (lFileOp)
    {
        const double obj = lFileTesto == query ? 1.0 : 0.45;
        lFileOp += (obj - lFileOp) * (1 - std::exp(-dtFotogramma / 0.06));
        if (std::abs(lFileOp - obj) < 0.004) lFileOp = obj; else muove = true;
    }
    if (lFileOp < 0.999) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), (float)lFileOp), nullptr);
    for (int k = 0; k < r && k < (int)lFile.size(); k++) {
        const indice::File& f = lFile[k];
        const D2D1_RECT_F rr = {20 * dpi, y + 2 * dpi, LW - 20 * dpi, y + (LF_RIGA - 2) * dpi};
        evidenziaRiga(rr, k);
        const float cy = y + LF_RIGA * dpi / 2, ix = 30 * dpi;
        const D2D1_RECT_F ri = {ix, cy - 14 * dpi, ix + 28 * dpi, cy + 14 * dpi};
        auto it = iconeFile.find(f.percorso);
        if (it != iconeFile.end() && it->second) dc->DrawBitmap(it->second, ri, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else { pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.35f)); dc->FillRoundedRectangle({ri, 5 * dpi, 5 * dpi}, pennello); }   // (icona in arrivo)
        pennello->SetColor(ink);
        scrivi(f.nome, fmtVoce, {ix + 38 * dpi, y + 4 * dpi, LW - 32 * dpi, y + 24 * dpi});
        pennello->SetColor(tenue);
        scrivi(f.cartella, fmtSotto, {ix + 38 * dpi, y + 23 * dpi, LW - 32 * dpi, y + 40 * dpi});
        y += LF_RIGA * dpi;
    }
    if (lFileOp < 0.999) dc->PopLayer();
    // "Cerca «testo» in Esplora file": la ricerca completa di Windows
    {
        const D2D1_RECT_F rr = {20 * dpi, y + 2 * dpi, LW - 20 * dpi, y + (LF_ESPLORA - 2) * dpi};
        evidenziaRiga(rr, r);
        const float cy = y + LF_ESPLORA * dpi / 2, lx = 44 * dpi;
        pennello->SetColor(blu);
        dc->DrawEllipse({{lx - 2 * dpi, cy - 2 * dpi}, 6 * dpi, 6 * dpi}, pennello, 1.8f * dpi);
        linea(2.5f, 2.5f, 7, 7, lx, cy);
        const std::wstring t = testi::t(L"Cerca “", L"Search “") + testi::isola(query) + testi::t(L"” in Esplora file", L"” in File Explorer");
        scrivi(t, fmtVoce, {lx + 24 * dpi, y, LW - 32 * dpi, y + LF_ESPLORA * dpi});
    }
    dc->PopAxisAlignedClip();
    return muove;
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

// (la sezione dei file, quando c'e', sta sotto e la griglia si accorcia della sua altezza animata)
inline auto Motore::lGrigliaBottom() const -> float { return LH - 14 * dpi - (float)lFileH; }

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

inline auto Motore::apriLauncher(bool giochi) -> void {
    if (!attivo) return;
    if (nascostoSchermoIntero) schermoIntero(false);
    auto caricaElenco = [&](bool g) {
        if (g) {
            lOrdineRecenti = leggiOrdineRecenti();
            giroGiochiAttivi = 0;
            if (tuttiGiochi.empty()) leggiTuttiGiochi(true);
            else {
                ordinaGiochi();
                aggiornaGiochiInEsecuzione();
                if (adesso() - gLetto > 600) leggiTuttiGiochi();
            }
        } else {
            if (mostraGiochi && tuttiGiochi.empty()) leggiTuttiGiochi(true);
            if (tutteApp.empty()) leggiTutteApp(true);
            else if (adesso() - lLetto > 600) leggiTutteApp();
        }
    };
    auto aggiornaAncora = [&](bool g) {
        float ax = W / 2.0f;
        const auto pos = posizioniRiposo();
        const wchar_t* chiave = g ? AVVIO_GIOCHI : AVVIO_LAUNCHER;
        for (size_t i = 0; i < voci.size() && i < pos.size(); i++) if (voci[i].avvio == chiave) ax = pos[i] + S / 2;
        const RECT ic = aSchermo({ax - S / 2, yBaseRiposo() - S, ax + S / 2, yBaseRiposo()});
        const float icx = (ic.left + ic.right) / 2.0f - rettLauncher.left, icy = (ic.top + ic.bottom) / 2.0f - rettLauncher.top;
        switch (lato) {
            case 1: ancora = {(LONG)std::clamp(icx, 40 * dpi, LW - 40 * dpi), 0}; break;
            case 2: ancora = {0, (LONG)std::clamp(icy, 40 * dpi, LH - 40 * dpi)}; break;
            case 3: ancora = {(LONG)LW, (LONG)std::clamp(icy, 40 * dpi, LH - 40 * dpi)}; break;
            default: ancora = {(LONG)std::clamp(icx, 40 * dpi, LW - 40 * dpi), (LONG)std::min(LH + LEXTRA - 4, icy)};
        }
    };
    if (lAperto && !lChiusura && lModoGiochi == giochi) { chiudiLauncher(); return; }
    if (lAperto && !lChiusura && lModoGiochi != giochi) {
        lModoGiochi = giochi;
        lSopraOrdine = false;
        aggiornaAncora(giochi);
        caricaElenco(giochi);
        query.clear(); filtra(); lSopra = -1; lScroll = lScrollObj = 0;
        prendiFuoco(launcher);
        lDirty = true; dirty = true;
        return;
    }
    if (lAperto && lChiusura) {
        if (lModoGiochi != giochi) {
            lModoGiochi = giochi;
            lSopraOrdine = false;
            aggiornaAncora(giochi);
            caricaElenco(giochi);
            query.clear(); filtra(); lSopra = -1; lScroll = lScrollObj = 0;
            dirty = true;
        } else if (giochi) {
            giroGiochiAttivi = 0;
            aggiornaGiochiInEsecuzione();
        }
        lChiusura = false; lDirty = true; lRipreso = true;
        // una ricerca dei file arrivata durante la chiusura e' stata scartata: si rifa' (R20261119-195000-S3)
        if (!lModoGiochi && !query.empty() && lFileTesto != query) SetTimer(launcher, TIMER_FILE_L, 200, nullptr);
        prendiFuoco(launcher);
        scrivi("launcher: riaperto mentre si chiudeva");
        return;
    }
    lModoGiochi = giochi;
    lSopraOrdine = false;
    caricaElenco(giochi);
    if (!nessunaScheda()) { if (dopo != 1) { chiudiTutte(); apriDopo(1, giochi ? 1 : 0); } return; }
    primaDelLauncher = GetForegroundWindow();
    query.clear(); filtra(); lSopra = -1; lScroll = 0; lFileH = 0;   // (la sezione dei file riparte chiusa)
    lAperto = true; lChiusura = false; lScala = 0.12; lVel = 0; lAlfa = 0; lT0 = adesso(); lRegione = false; lRipreso = false;
    // sopra il dock, al centro; la finestra scende fino all'icona "App" (o "Giochi"). Col dock in alto: sotto, al centro;
    // ai lati: accanto al dock, a meta' altezza. Li' la finestra non arriva all'icona: cresce dal suo bordo piu' vicino all'icona
    const POINT o = origine();
    const D2D1_POINT_2F bi = fis(0, yBaseRiposo() - S - PAD);   // bordo interno del dock (nella finestra del dock)
    const int stacco = (int)std::round(18 * dpi);
    int x0 = schermo.left + (int)std::round((schermo.right - schermo.left - LW) / 2), y0 = 0;
    if (lato == 0) {
        const int y1 = o.y + (int)std::round(bi.y) - stacco;
        // schermi bassi (1366x768 al 125%: ~520 punti sopra il dock): il launcher si accorcia invece di uscire in alto
        LH = std::clamp((float)(y1 - schermo.top) - 12 * dpi, 300 * dpi, LHMAX);
        y0 = y1 - (int)LH;
    } else if (lato == 1) {
        y0 = o.y + (int)std::round(bi.y) + stacco;
        LH = std::clamp((float)(schermo.bottom - y0) - 12 * dpi, 300 * dpi, LHMAX);
    } else {
        LH = std::clamp((float)(schermo.bottom - schermo.top) - 24 * dpi, 300 * dpi, LHMAX);
        y0 = schermo.top + (int)std::round((schermo.bottom - schermo.top - LH) / 2);
        x0 = lato == 2 ? o.x + (int)std::round(bi.x) + stacco : o.x + (int)std::round(bi.x) - stacco - (int)LW;
        // (schermo stretto: dentro lo schermo anche a costo di coprire un poco il dock, R20261120-190507-S2)
        x0 = std::clamp(x0, (int)schermo.left + 8, std::max((int)schermo.left + 8, (int)schermo.right - 8 - (int)LW));
    }
    rettLauncher = {x0, y0, x0 + (int)LW, y0 + (int)LH};
    aggiornaAncora(giochi);
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

static inline bool processoGiocoIgnorato(const std::wstring& exeMin) {
    static const wchar_t* const scarta[] = {
        L"riotclientservices.exe", L"crashhandler", L"crashreporter", L"ue4prereq",
        L"unins", L"vcredist", L"dxsetup", L"easyanticheat_setup", L"battleye", L"webhelper"
    };
    const size_t bar = exeMin.find_last_of(L'\\');
    const std::wstring nome = (bar == std::wstring::npos) ? exeMin : exeMin.substr(bar + 1);
    for (const wchar_t* s : scarta) {
        if (nome.find(s) != std::wstring::npos) return true;
    }
    return false;
}

static inline std::vector<std::pair<DWORD, std::wstring>> elencaProcessiAttivi() {
    std::vector<std::pair<DWORD, std::wstring>> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4) continue;
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hProc) continue;
            wchar_t buf[1024];
            DWORD sz = (DWORD)(sizeof(buf) / sizeof(buf[0]));
            if (QueryFullProcessImageNameW(hProc, 0, buf, &sz) && sz > 0) {
                std::wstring p = minuscolo(normalizzaPercorso(std::wstring(buf, sz)));
                if (!processoGiocoIgnorato(p)) out.push_back({pe.th32ProcessID, std::move(p)});
            }
            CloseHandle(hProc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

inline auto Motore::registraAvvioGioco(const AppL& a) -> void {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    const ULONGLONG nowSec = ui.QuadPart >= 116444736000000000ULL
        ? (ui.QuadPart - 116444736000000000ULL) / 10000000ULL : 1ULL;
    const std::wstring chiave = chiaveGioco(a);
    auto rec = leggiGiochiRecenti();
    rec[chiave] = nowSec;
    salvaGiochiRecenti(rec);
    for (auto& g : tuttiGiochi) {
        if (chiaveGioco(g) == chiave) g.ultimoAvvio = nowSec;
    }
    ordinaGiochi();
    if (lModoGiochi) filtra();
}

inline auto Motore::lanciaGioco(const AppL& a) -> void {
    registraAvvioGioco(a);
    const std::wstring avvMin = minuscolo(a.avvio);
    const bool isSteamUri = avvMin.rfind(L"steam://rungameid/", 0) == 0;
    const bool isUri = a.avvio.find(L"://") != std::wstring::npos;
    std::wstring target = a.avvio;
    std::wstring params;
    const wchar_t* verb = L"open";
    if (isSteamUri && !a.argomentiExtra.empty()) {
        target = a.avvio + L"//" + a.argomentiExtra;
    } else {
        params = a.argomenti;
        if (!a.argomentiExtra.empty()) {
            if (!params.empty()) params.push_back(L' ');
            params += a.argomentiExtra;
        }
        if (a.comeAdmin && !isUri) verb = L"runas";
    }
    ShellExecuteW(nullptr, verb, target.c_str(),
        params.empty() ? nullptr : params.c_str(),
        a.cartella.empty() ? nullptr : a.cartella.c_str(),
        SW_SHOWNORMAL);
}

inline auto Motore::alternaPreferitoGioco(const AppL& a) -> void {
    const std::wstring chiave = chiaveGioco(a);
    auto pref = leggiGiochiPreferiti();
    const bool nuovo = pref.count(chiave) == 0;
    if (nuovo) pref.insert(chiave);
    else pref.erase(chiave);
    salvaGiochiPreferiti(pref);
    for (auto& g : tuttiGiochi) {
        if (chiaveGioco(g) == chiave) g.preferito = nuovo;
    }
    ordinaGiochi();
    if (lModoGiochi) filtra();
    lDirty = true;
}

inline auto Motore::aggiornaGiochiInEsecuzione() -> void {
    if (tuttiGiochi.empty()) return;
    const auto processi = elencaProcessiAttivi();
    bool cambiato = false;
    for (auto& g : tuttiGiochi) {
        DWORD trovato = 0;
        const bool isExe = finisceCon(g.avvio, L".exe");
        const std::wstring avvMin = isExe ? minuscolo(normalizzaPercorso(g.avvio)) : L"";
        const bool checkExe = isExe && !avvMin.empty() && !finisceCon(avvMin, L"\\riotclientservices.exe");
        std::wstring prefCart;
        if (!g.cartella.empty()) {
            prefCart = minuscolo(normalizzaPercorso(g.cartella));
            if (!prefCart.empty() && prefCart.back() != L'\\') prefCart.push_back(L'\\');
        }
        for (const auto& [pid, pExe] : processi) {
            if (checkExe && pExe == avvMin) { trovato = pid; break; }
            if (!prefCart.empty() && pExe.size() > prefCart.size() && pExe.compare(0, prefCart.size(), prefCart) == 0) {
                trovato = pid;
                break;
            }
        }
        if (g.pidInEsecuzione != trovato) {
            g.pidInEsecuzione = trovato;
            cambiato = true;
        }
    }
    if (cambiato) lDirty = true;
}

inline auto Motore::chiudiGiocoInEsecuzione(const AppL& a, bool forza) -> void {
    const auto processi = elencaProcessiAttivi();
    std::unordered_set<DWORD> pids;
    const bool isExe = finisceCon(a.avvio, L".exe");
    const std::wstring avvMin = isExe ? minuscolo(normalizzaPercorso(a.avvio)) : L"";
    const bool checkExe = isExe && !avvMin.empty() && !finisceCon(avvMin, L"\\riotclientservices.exe");
    std::wstring prefCart;
    if (!a.cartella.empty()) {
        prefCart = minuscolo(normalizzaPercorso(a.cartella));
        if (!prefCart.empty() && prefCart.back() != L'\\') prefCart.push_back(L'\\');
    }
    for (const auto& [pid, pExe] : processi) {
        if ((checkExe && pExe == avvMin) ||
            (!prefCart.empty() && pExe.size() > prefCart.size() && pExe.compare(0, prefCart.size(), prefCart) == 0)) {
            pids.insert(pid);
        }
    }
    if (pids.empty()) {
        aggiornaGiochiInEsecuzione();
        return;
    }
    int inviati = 0;
    if (!forza) {
        struct ChiudiCtx {
            const std::unordered_set<DWORD>* pids;
            int inviati = 0;
        } ctx{&pids, 0};
        EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
            auto* c = (ChiudiCtx*)lp;
            DWORD pid = 0;
            GetWindowThreadProcessId(hw, &pid);
            if (c->pids->count(pid) && GetWindow(hw, GW_OWNER) == nullptr) {
                PostMessageW(hw, WM_CLOSE, 0, 0);
                c->inviati++;
            }
            return TRUE;
        }, (LPARAM)&ctx);
        inviati = ctx.inviati;
    }
    if (forza || inviati == 0) {
        for (DWORD pid : pids) {
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProc) {
                TerminateProcess(hProc, 1);
                CloseHandle(hProc);
            }
        }
    }
    aggiornaGiochiInEsecuzione();
}

inline auto Motore::avviaApp(int i) -> void {
    if (i < 0 || i >= (int)filtrate.size()) return;
    const AppL a = elencoAttivo()[filtrate[i]];
    primaDelLauncher = nullptr;   // il primo piano va all'app che si apre
    chiudiLauncher();
    if (lModoGiochi) {
        lanciaGioco(a);
        return;
    }
    ShellExecuteW(nullptr, L"open", a.avvio.c_str(), a.argomenti.empty() ? nullptr : a.argomenti.c_str(), a.cartella.empty() ? nullptr : a.cartella.c_str(), SW_SHOWNORMAL);
}

inline auto Motore::rettEnergia() const -> D2D1_RECT_F { return {LW - 64 * dpi, 22 * dpi, LW - 24 * dpi, 62 * dpi}; }

inline auto Motore::rettOrdineGiochi() const -> D2D1_RECT_F {
    const D2D1_RECT_F re = rettEnergia();
    const float w = 70 * dpi, h = 32 * dpi, gap = 8 * dpi;
    const float cy = (re.top + re.bottom) * 0.5f;
    return {re.left - gap - w, cy - h * 0.5f, re.left - gap, cy + h * 0.5f};
}

// Menu di Windows per l'app (lo stesso dello Start), con in cima "Aggiungi al dock"
inline auto Motore::menuApp(int i, POINT pt) -> void {
    if (i < 0 || i >= (int)filtrate.size()) return;
    const AppL a = tutteApp[filtrate[i]];
    IShellItem* it = nullptr;
    if (FAILED(SHCreateItemFromParsingName(a.avvio.c_str(), nullptr, IID_PPV_ARGS(&it)))) return;
    IContextMenu* cm = nullptr;
    it->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm));
    // eseguibile e AppUserModelID dell'app, per il collegamento nel dock
    std::wstring exe, id, linkArgs;
    IShellItem2* it2 = nullptr;
    if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2)))) {
        LPWSTR s = nullptr;
        if (SUCCEEDED(it2->GetString(PKEY_Link_TargetParsingPath, &s)) && s) { exe = s; CoTaskMemFree(s); }
        s = nullptr;
        if (SUCCEEDED(it2->GetString(PKEY_AppUserModel_ID, &s)) && s) { id = s; CoTaskMemFree(s); }
        s = nullptr;
        if (SUCCEEDED(it2->GetString(PKEY_Link_Arguments, &s)) && s) { linkArgs = s; CoTaskMemFree(s); }
        it2->Release();
    }
    it->Release();
    if (!cm) return;
    const UINT ID_DOCK = 0x7F00, ID_GIOCHI = 0x7EFF;
    HMENU m = CreatePopupMenu();
    if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, ID_GIOCHI - 1, CMF_NORMAL))) {
        cm->QueryInterface(IID_PPV_ARGS(&menuShell2)); cm->QueryInterface(IID_PPV_ARGS(&menuShell3));
        // le voci di Windows, disegnate da noi; in cima il nome dell'app e "Aggiungi al dock"
        std::vector<menu::Voce> voci = {menu::titolo(a.nome), menu::voce(testi::t(L"Aggiungi al dock", L"Add to dock"), ID_DOCK)};
        if (mostraGiochi) voci.push_back(menu::voce(testi::t(L"Sposta nei giochi", L"Move to Games"), ID_GIOCHI));
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
        } else if (cmd == ID_GIOCHI) {
            auto el = leggiGiochiAggiunti();
            const std::wstring target = (linkArgs.empty() && !exe.empty() && esisteFile(exe)) ? exe : a.avvio;
            const std::wstring tMin = minuscolo(target);
            bool gia = false;
            for (const auto& kv : el) if (minuscolo(kv.second) == tMin) { gia = true; break; }
            if (!gia) {
                el.push_back({a.nome, target});
                salvaGiochiAggiunti(el);
            }
            leggiTuttiGiochi(true);
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
inline auto Motore::aggiungiAlDock(const std::wstring& nome, const std::wstring& exe, const std::wstring& id, const std::wstring& avvio, const std::wstring& icona, const std::wstring& argomenti) -> void {
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
    else {   // app dello Store o collegamento URI: il collegamento punta al PIDL
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(avvio.c_str(), nullptr, &pidl, 0, nullptr))) { ok = SUCCEEDED(sl->SetIDList(pidl)); CoTaskMemFree(pidl); }
    }
    if (ok && !argomenti.empty()) sl->SetArguments(argomenti.c_str());
    if (ok && !icona.empty() && (finisceCon(icona, L".ico") || finisceCon(icona, L".exe")) && esisteFile(icona))
        sl->SetIconLocation(icona.c_str(), 0);
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

inline auto Motore::aggiungiGiocoFile(const std::wstring& percorso) -> void {
    if (percorso.empty()) return;
    std::wstring nome;
    if (finisceCon(percorso, L".exe")) nome = nomeExe(percorso);
    if (nome.empty()) {
        const size_t bar = percorso.find_last_of(L"\\/");
        nome = bar == std::wstring::npos ? percorso : percorso.substr(bar + 1);
        const size_t dot = nome.rfind(L'.');
        if (dot != std::wstring::npos) nome = nome.substr(0, dot);
    }
    for (auto& c : nome) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    while (!nome.empty() && nome.front() == L' ') nome.erase(nome.begin());
    while (!nome.empty() && nome.back() == L' ') nome.pop_back();
    if (nome.empty()) return;
    auto el = leggiGiochiAggiunti();
    const std::wstring pMin = minuscolo(percorso);
    for (const auto& kv : el) if (minuscolo(kv.second) == pMin) return;
    el.push_back({nome, percorso});
    salvaGiochiAggiunti(el);
}

inline auto Motore::aggiungiGiocoDialogo() -> void {
    if (lDialogoAperto) return;
    lDialogoAperto = true;
    wchar_t p[MAX_PATH * 4] = L"";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = launcher;
    of.lpstrFile = p;
    of.nMaxFile = MAX_PATH * 4;
    of.lpstrFilter = L"Games (*.exe;*.lnk;*.url)\0*.exe;*.lnk;*.url\0All files (*.*)\0*.*\0";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    const bool scelto = GetOpenFileNameW(&of) != FALSE;
    lDialogoAperto = false;
    lSopraEnergia = false;
    if (lAperto && !lChiusura) prendiFuoco(launcher);
    if (scelto && p[0]) {
        aggiungiGiocoFile(p);
        leggiTuttiGiochi(true);
    }
    lDirty = true;
}

inline auto Motore::cambiaIconaGioco(const AppL& a) -> void {
    if (lDialogoAperto) return;
    lDialogoAperto = true;
    const std::wstring chiave = chiaveGioco(a);
    wchar_t p[MAX_PATH * 4] = L"";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = launcher;
    of.lpstrFile = p;
    of.nMaxFile = MAX_PATH * 4;
    of.lpstrFilter = L"Icons & images (*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.exe)\0*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.exe\0All files (*.*)\0*.*\0";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    const bool scelto = GetOpenFileNameW(&of) != FALSE;
    lDialogoAperto = false;
    if (lAperto && !lChiusura) prendiFuoco(launcher);
    if (scelto && p[0]) {
        auto ic = leggiIconeGiochi();
        ic[chiave] = p;
        salvaIconeGiochi(ic);
        iconeChieste.erase(chiaveIcona(p, 96));
        for (auto& g : tuttiGiochi) {
            if (chiaveGioco(g) == chiave) {
                if (g.icona) { g.icona->Release(); g.icona = nullptr; }
                g.provata = false;
            }
        }
        leggiTuttiGiochi(true);
    }
    lDirty = true;
}

inline auto Motore::dialogoTestoVetro(const std::wstring& titolo, const std::wstring& sottotitolo, const std::wstring& iniziale, const std::wstring& conferma, std::wstring& out, bool consentiVuoto) -> bool {
    if (lDialogoAperto) return false;
    lDialogoAperto = true;

    struct DialogoTesto {
        vetro::Finestra v;
        std::wstring titolo;
        std::wstring sottotitolo;
        std::wstring testoConferma;
        std::wstring testo;
        bool consentiVuoto = false;
        int caret = 0;
        int selIni = 0;
        int selFin = 0;
        float scrollX = 0;
        bool caretVis = true;
        bool finito = false;
        bool confermato = false;
        bool hovSave = false;
        bool hovCancel = false;
        bool trackingMouse = false;
        bool draggingSel = false;
        int pressedBtn = 0;
        ULONGLONG tFade = 0;

        IDWriteTextFormat* fTitolo = nullptr;
        IDWriteTextFormat* fSotto = nullptr;
        IDWriteTextFormat* fInput = nullptr;
        IDWriteTextFormat* fBtn = nullptr;
        ID2D1LinearGradientBrush* sheen = nullptr;
        unsigned sheenGiro = ~0u;

        static std::wstring trim(const std::wstring& s) {
            std::wstring pulita;
            pulita.reserve(s.size());
            for (wchar_t ch : s) {
                if (ch != L'\t' && ch != L'\r' && ch != L'\n') pulita.push_back(ch);
            }
            const size_t ini = pulita.find_first_not_of(L' ');
            if (ini == std::wstring::npos) return {};
            const size_t fin = pulita.find_last_not_of(L' ');
            return pulita.substr(ini, fin - ini + 1);
        }

        bool inputValido() const {
            return consentiVuoto || !trim(testo).empty();
        }

        void aggiornaFormati() {
            const float s = v.dpi;
            if (!fTitolo) fTitolo = v.formato(14.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            if (!fSotto && !sottotitolo.empty()) fSotto = v.formato(11.5f * s, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            if (!fInput) {
                fInput = v.formato(13.5f * s);
                if (fInput) {
                    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 };
                    fInput->SetTrimming(&tr, nullptr);
                }
            }
            if (!fBtn) fBtn = v.formato(13.0f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        void aggiornaSheen() {
            if (!v.rt) return;
            if (sheen && sheenGiro == v.giroRt) return;
            if (sheen) { sheen->Release(); sheen = nullptr; }
            D2D1_GRADIENT_STOP stops[2]{
                { 0.0f, D2D1::ColorF(1, 1, 1, v.scuro ? 0.10f : 0.35f) },
                { 1.0f, D2D1::ColorF(1, 1, 1, 0.0f) }
            };
            ID2D1GradientStopCollection* col = nullptr;
            if (SUCCEEDED(v.rt->CreateGradientStopCollection(stops, 2, &col)) && col) {
                v.rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, (float)v.H * 0.45f }),
                    col, &sheen);
                col->Release();
            }
            sheenGiro = v.giroRt;
        }

        void libera() {
            if (sheen) { sheen->Release(); sheen = nullptr; }
            if (fTitolo) { fTitolo->Release(); fTitolo = nullptr; }
            if (fSotto) { fSotto->Release(); fSotto = nullptr; }
            if (fInput) { fInput->Release(); fInput = nullptr; }
            if (fBtn) { fBtn->Release(); fBtn = nullptr; }
        }

        D2D1_RECT_F rectInput() const {
            const float s = v.dpi;
            const float pad = 18.0f * s;
            const float top = sottotitolo.empty() ? 50.0f * s : 84.0f * s;
            return { pad, top, (float)v.W - pad, top + 38.0f * s };
        }

        D2D1_RECT_F rectCancel() const {
            const float s = v.dpi;
            const float pad = 18.0f * s, btnW = 86.0f * s, btnH = 32.0f * s, gap = 10.0f * s;
            const float y = (float)v.H - pad - btnH;
            const float x = (float)v.W - pad - 2.0f * btnW - gap;
            return { x, y, x + btnW, y + btnH };
        }

        D2D1_RECT_F rectSave() const {
            const float s = v.dpi;
            const float pad = 18.0f * s, btnW = 86.0f * s, btnH = 32.0f * s;
            const float y = (float)v.H - pad - btnH;
            const float x = (float)v.W - pad - btnW;
            return { x, y, x + btnW, y + btnH };
        }

        static bool dentro(const D2D1_RECT_F& r, float x, float y) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        }

        IDWriteTextLayout* creaLayoutInput() const {
            if (!v.fdw || !fInput) return nullptr;
            IDWriteTextLayout* layout = nullptr;
            const D2D1_RECT_F ri = rectInput();
            v.fdw->CreateTextLayout(testo.c_str(), (UINT32)testo.size(), fInput, 10000.0f, ri.bottom - ri.top, &layout);
            return layout;
        }

        int posDaPunto(float px) {
            IDWriteTextLayout* layout = creaLayoutInput();
            if (!layout) return (int)testo.size();
            const float s = v.dpi;
            const D2D1_RECT_F ri = rectInput();
            const float textX = ri.left + 11.0f * s;
            DWRITE_HIT_TEST_METRICS htm{};
            BOOL isTrailing = FALSE, isInside = FALSE;
            layout->HitTestPoint(px - (textX - scrollX), (ri.bottom - ri.top) * 0.5f, &isTrailing, &isInside, &htm);
            layout->Release();
            int pos = (int)htm.textPosition + (isTrailing ? 1 : 0);
            return std::clamp(pos, 0, (int)testo.size());
        }

        void eliminaSelezione() {
            if (selIni == selFin) return;
            const int lo = std::min(selIni, selFin);
            const int hi = std::max(selIni, selFin);
            testo.erase((size_t)lo, (size_t)(hi - lo));
            caret = selIni = selFin = lo;
        }

        void resetBlink(HWND h) {
            caretVis = true;
            SetTimer(h, 1, 530, nullptr);
            InvalidateRect(h, nullptr, FALSE);
        }

        void copiaSelezione(HWND h) const {
            if (selIni == selFin) return;
            const int lo = std::min(selIni, selFin);
            const int hi = std::max(selIni, selFin);
            const std::wstring sub = testo.substr((size_t)lo, (size_t)(hi - lo));
            if (sub.empty() || !OpenClipboard(h)) return;
            EmptyClipboard();
            const SIZE_T bytes = (sub.size() + 1) * sizeof(wchar_t);
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hg) {
                if (void* p = GlobalLock(hg)) {
                    memcpy(p, sub.c_str(), bytes);
                    GlobalUnlock(hg);
                }
                SetClipboardData(CF_UNICODETEXT, hg);
            }
            CloseClipboard();
        }

        void incolla(HWND h) {
            if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(h)) return;
            HANDLE hd = GetClipboardData(CF_UNICODETEXT);
            std::wstring ins;
            if (hd) {
                if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) {
                    for (const wchar_t* q = p; *q; ++q) {
                        if (*q != L'\t' && *q != L'\r' && *q != L'\n') ins.push_back(*q);
                    }
                    GlobalUnlock(hd);
                }
            }
            CloseClipboard();
            if (!ins.empty()) {
                eliminaSelezione();
                testo.insert((size_t)caret, ins);
                caret += (int)ins.size();
                selIni = selFin = caret;
                resetBlink(h);
            }
        }

        void disegna() {
            if (!v.inizia(0.82f)) return;
            aggiornaFormati();
            aggiornaSheen();

            const float s = v.dpi;
            const float rad = v.raggio();
            if (sheen) {
                v.rt->FillRoundedRectangle(
                    D2D1::RoundedRect({ 0, 0, (float)v.W, (float)v.H }, rad, rad),
                    sheen);
            }

            v.pen->SetColor(D2D1::ColorF(1, 1, 1, v.scuro ? 0.18f : 0.70f));
            v.rt->DrawRoundedRectangle(
                D2D1::RoundedRect({ 0.5f, 0.5f, (float)v.W - 0.5f, (float)v.H - 0.5f },
                    std::max(0.0f, rad - 0.5f), std::max(0.0f, rad - 0.5f)),
                v.pen, 1.0f);

            const float pad = 18.0f * s;
            v.testo(titolo, fTitolo, { pad, 16.0f * s, (float)v.W - pad, 40.0f * s }, v.inchiostro());
            if (!sottotitolo.empty() && fSotto) {
                v.testo(sottotitolo, fSotto, { pad, 42.0f * s, (float)v.W - pad, 78.0f * s }, v.inchiostro(0.68f));
            }

            const D2D1_RECT_F ri = rectInput();
            const float inRad = 10.0f * s;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.68f));
            v.rt->FillRoundedRectangle(D2D1::RoundedRect(ri, inRad, inRad), v.pen);

            v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.85f));
            v.rt->DrawRoundedRectangle(D2D1::RoundedRect(ri, inRad, inRad), v.pen, 1.5f * s);

            const float textX = ri.left + 11.0f * s;
            const float textR = ri.right - 11.0f * s;
            const float innerW = std::max(1.0f, textR - textX);
            if (IDWriteTextLayout* layout = creaLayoutInput()) {
                FLOAT cx = 0, cy = 0;
                DWRITE_HIT_TEST_METRICS cm{};
                layout->HitTestTextPosition((UINT32)caret, FALSE, &cx, &cy, &cm);

                DWRITE_TEXT_METRICS tm{};
                layout->GetMetrics(&tm);
                if (tm.widthIncludingTrailingWhitespace <= innerW) {
                    scrollX = 0.0f;
                } else {
                    if (cx - scrollX > innerW - 2.0f * s) scrollX = cx - (innerW - 2.0f * s);
                    if (cx - scrollX < 0.0f) scrollX = cx;
                    scrollX = std::clamp(scrollX, 0.0f, std::max(0.0f, tm.widthIncludingTrailingWhitespace - innerW + 2.0f * s));
                }

                v.rt->PushAxisAlignedClip({ textX - 1.0f, ri.top + 2.0f * s, textR + 1.0f, ri.bottom - 2.0f * s },
                    D2D1_ANTIALIAS_MODE_ALIASED);

                if (selIni != selFin) {
                    const int lo = std::min(selIni, selFin);
                    const int hi = std::max(selIni, selFin);
                    FLOAT x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                    DWRITE_HIT_TEST_METRICS m0{}, m1{};
                    layout->HitTestTextPosition((UINT32)lo, FALSE, &x0, &y0, &m0);
                    layout->HitTestTextPosition((UINT32)hi, FALSE, &x1, &y1, &m1);
                    v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.34f));
                    v.rt->FillRectangle({ textX + x0 - scrollX, ri.top + 7.0f * s, textX + x1 - scrollX, ri.bottom - 7.0f * s }, v.pen);
                }

                v.pen->SetColor(v.inchiostro());
                v.rt->DrawTextLayout({ textX - scrollX, ri.top }, layout, v.pen, D2D1_DRAW_TEXT_OPTIONS_CLIP);

                if (caretVis) {
                    const float kx = textX + cx - scrollX;
                    v.pen->SetColor(v.inchiostro());
                    v.rt->DrawLine({ kx, ri.top + 7.5f * s }, { kx, ri.bottom - 7.5f * s }, v.pen, 1.5f * s);
                }

                v.rt->PopAxisAlignedClip();
                layout->Release();
            }

            const D2D1_RECT_F rc = rectCancel();
            const float btnRad = 9.0f * s;
            v.pen->SetColor(v.scuro
                ? D2D1::ColorF(1, 1, 1, hovCancel ? 0.18f : 0.10f)
                : D2D1::ColorF(0, 0, 0, hovCancel ? 0.10f : 0.05f));
            v.rt->FillRoundedRectangle(D2D1::RoundedRect(rc, btnRad, btnRad), v.pen);
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.10f));
            v.rt->DrawRoundedRectangle(D2D1::RoundedRect(rc, btnRad, btnRad), v.pen, 1.0f);
            v.testo(testi::t(L"Annulla", L"Cancel"), fBtn, rc, v.inchiostro());

            const D2D1_RECT_F rs = rectSave();
            const bool valido = inputValido();
            v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, !valido ? 0.40f : (hovSave ? 1.0f : 0.90f)));
            v.rt->FillRoundedRectangle(D2D1::RoundedRect(rs, btnRad, btnRad), v.pen);
            v.pen->SetColor(D2D1::ColorF(1, 1, 1, valido ? 0.28f : 0.10f));
            v.rt->DrawLine({ rs.left + btnRad, rs.top + 0.75f }, { rs.right - btnRad, rs.top + 0.75f }, v.pen, 1.0f);
            v.testo(testoConferma, fBtn, rs, D2D1::ColorF(1, 1, 1, valido ? 1.0f : 0.55f));

            v.finisci();
        }

        bool messaggio(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
            switch (m) {
                case WM_PAINT:
                    disegna();
                    return true;
                case WM_TIMER:
                    if (w == 1) {
                        caretVis = !caretVis;
                        InvalidateRect(h, nullptr, FALSE);
                        return true;
                    }
                    if (w == 2) {
                        const double p = std::clamp((double)(GetTickCount64() - tFade) / 140.0, 0.0, 1.0);
                        v.alfa(vetro::uscita(p));
                        if (p >= 1.0) KillTimer(h, 2);
                        return true;
                    }
                    return false;
                case WM_CHAR: {
                    if (w == 0x01) {
                        selIni = 0;
                        selFin = caret = (int)testo.size();
                        resetBlink(h);
                        return true;
                    }
                    if (w == 0x03) {
                        copiaSelezione(h);
                        return true;
                    }
                    if (w == 0x18) {
                        copiaSelezione(h);
                        eliminaSelezione();
                        resetBlink(h);
                        return true;
                    }
                    if (w == 0x16) {
                        incolla(h);
                        return true;
                    }
                    if (w == L'\b') {
                        if (selIni != selFin) {
                            eliminaSelezione();
                        } else if (caret > 0) {
                            testo.erase((size_t)(caret - 1), 1);
                            caret--;
                            selIni = selFin = caret;
                        }
                        resetBlink(h);
                        return true;
                    }
                    if (w >= 32) {
                        eliminaSelezione();
                        testo.insert((size_t)caret, 1, (wchar_t)w);
                        caret++;
                        selIni = selFin = caret;
                        resetBlink(h);
                        return true;
                    }
                    return false;
                }
                case WM_KEYDOWN:
                    if (w == VK_RETURN) {
                        if (inputValido()) { confermato = true; finito = true; }
                        return true;
                    }
                    if (w == VK_ESCAPE) {
                        finito = true;
                        return true;
                    }
                    if (w == VK_LEFT) {
                        if (selIni != selFin) caret = std::min(selIni, selFin);
                        else if (caret > 0) caret--;
                        selIni = selFin = caret;
                        resetBlink(h);
                        return true;
                    }
                    if (w == VK_RIGHT) {
                        if (selIni != selFin) caret = std::max(selIni, selFin);
                        else if (caret < (int)testo.size()) caret++;
                        selIni = selFin = caret;
                        resetBlink(h);
                        return true;
                    }
                    if (w == VK_HOME) {
                        caret = selIni = selFin = 0;
                        resetBlink(h);
                        return true;
                    }
                    if (w == VK_END) {
                        caret = selIni = selFin = (int)testo.size();
                        resetBlink(h);
                        return true;
                    }
                    if (w == VK_DELETE) {
                        if (selIni != selFin) {
                            eliminaSelezione();
                        } else if (caret < (int)testo.size()) {
                            testo.erase((size_t)caret, 1);
                            selIni = selFin = caret;
                        }
                        resetBlink(h);
                        return true;
                    }
                    return false;
                case WM_MOUSEMOVE: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (draggingSel) {
                        caret = selFin = posDaPunto(mx);
                        resetBlink(h);
                        return true;
                    }
                    if (!trackingMouse) {
                        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                        trackingMouse = TrackMouseEvent(&tme) != FALSE;
                    }
                    const bool hs = dentro(rectSave(), mx, my);
                    const bool hc = dentro(rectCancel(), mx, my);
                    if (hs != hovSave || hc != hovCancel) {
                        hovSave = hs;
                        hovCancel = hc;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    return true;
                }
                case WM_MOUSELEAVE:
                    trackingMouse = false;
                    if (hovSave || hovCancel) {
                        hovSave = hovCancel = false;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    return true;
                case WM_LBUTTONDOWN: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (dentro(rectInput(), mx, my)) {
                        caret = selIni = selFin = posDaPunto(mx);
                        draggingSel = true;
                        SetCapture(h);
                        resetBlink(h);
                        return true;
                    }
                    if (dentro(rectCancel(), mx, my)) {
                        pressedBtn = 1;
                        SetCapture(h);
                        return true;
                    }
                    if (dentro(rectSave(), mx, my)) {
                        pressedBtn = 2;
                        SetCapture(h);
                        return true;
                    }
                    ReleaseCapture();
                    SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    return true;
                }
                case WM_LBUTTONUP: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (draggingSel) {
                        draggingSel = false;
                        ReleaseCapture();
                        return true;
                    }
                    if (pressedBtn != 0) {
                        const int btn = pressedBtn;
                        pressedBtn = 0;
                        ReleaseCapture();
                        if (btn == 1 && dentro(rectCancel(), mx, my)) {
                            finito = true;
                        } else if (btn == 2 && dentro(rectSave(), mx, my) && inputValido()) {
                            confermato = true;
                            finito = true;
                        }
                        return true;
                    }
                    return false;
                }
                case WM_LBUTTONDBLCLK: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (dentro(rectInput(), mx, my)) {
                        selIni = 0;
                        selFin = caret = (int)testo.size();
                        resetBlink(h);
                        return true;
                    }
                    return false;
                }
                case WM_CLOSE:
                    finito = true;
                    r = 0;
                    return true;
            }
            return false;
        }
    } dlg;

    dlg.titolo = titolo;
    dlg.sottotitolo = sottotitolo;
    dlg.testoConferma = conferma;
    dlg.consentiVuoto = consentiVuoto;
    dlg.testo = iniziale;
    dlg.selIni = 0;
    dlg.selFin = dlg.caret = (int)dlg.testo.size();

    const wchar_t* clsName = L"FluidWin.DialogoTesto";
    WNDCLASSW wc{};
    if (!GetClassInfoW(GetModuleHandleW(nullptr), clsName, &wc)) {
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = vetro::procedura<DialogoTesto>;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = clsName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
    }

    bool ok = false;
    if (dlg.v.crea(clsName, vetro::procedura<DialogoTesto>, &dlg, true, 2, true, true)) {
        const int W = (int)std::lround((sottotitolo.empty() ? 360.0f : 460.0f) * dpi);
        const int H = (int)std::lround((sottotitolo.empty() ? 154.0f : 190.0f) * dpi);
        RECT rl{};
        GetWindowRect(launcher, &rl);
        int wx = rl.left + ((rl.right - rl.left) - W) / 2;
        int wy = rl.top + ((rl.bottom - rl.top) - H) / 2;
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(launcher, MONITOR_DEFAULTTONEAREST), &mi)) {
            wx = std::clamp(wx, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - W));
            wy = std::clamp(wy, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - H));
        }

        dlg.v.posiziona(wx, wy, W, H, HWND_TOPMOST, 0);
        EnableWindow(launcher, FALSE);
        dlg.tFade = GetTickCount64();
        ShowWindow(dlg.v.fin, SW_SHOW);
        SetForegroundWindow(dlg.v.fin);
        SetFocus(dlg.v.fin);
        SetTimer(dlg.v.fin, 1, 530, nullptr);
        SetTimer(dlg.v.fin, 2, 16, nullptr);
        InvalidateRect(dlg.v.fin, nullptr, FALSE);

        MSG msg{};
        while (!dlg.finito && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (dlg.confermato) {
            out = DialogoTesto::trim(dlg.testo);
            ok = true;
        }

        KillTimer(dlg.v.fin, 1);
        KillTimer(dlg.v.fin, 2);
        EnableWindow(launcher, TRUE);
    }

    dlg.libera();
    dlg.v.chiudi();

    lDialogoAperto = false;
    if (lAperto && !lChiusura) prendiFuoco(launcher);
    return ok;
}

inline auto Motore::rinominaGioco(const AppL& a) -> void {
    std::wstring nuovoNome;
    if (dialogoTestoVetro(
            testi::t(L"Rinomina gioco", L"Rename game"),
            L"",
            a.nome,
            testi::t(L"Salva", L"Save"),
            nuovoNome,
            false) && !nuovoNome.empty()) {
        auto nm = leggiNomiGiochi();
        nm[chiaveGioco(a)] = nuovoNome;
        salvaNomiGiochi(nm);
        leggiTuttiGiochi(true);
        lDirty = true;
    }
}

inline auto Motore::opzioniGioco(const AppL& a) -> void {
    lDialogoAperto = true;

    struct OpzioniGiocoDlg {
        vetro::Finestra v;
        std::wstring titolo;
        std::wstring sottotitolo;
        std::wstring placeholder;
        std::wstring testo;
        bool comeAdmin = false;
        bool adminDisabilitato = false;
        int caret = 0;
        int selIni = 0;
        int selFin = 0;
        float scrollX = 0.0f;
        bool caretVis = true;
        bool draggingSel = false;
        bool trackingMouse = false;
        bool hovSave = false;
        bool hovCancel = false;
        bool hovToggle = false;
        int pressedBtn = 0;
        bool finito = false;
        bool confermato = false;
        ULONGLONG tFade = 0;

        IDWriteTextFormat* fTitolo = nullptr;
        IDWriteTextFormat* fSotto = nullptr;
        IDWriteTextFormat* fInput = nullptr;
        IDWriteTextFormat* fRow = nullptr;
        IDWriteTextFormat* fBtn = nullptr;
        ID2D1LinearGradientBrush* sheen = nullptr;
        unsigned sheenGiro = ~0u;

        static std::wstring trim(const std::wstring& s) {
            std::wstring pulita;
            pulita.reserve(s.size());
            for (wchar_t ch : s) {
                if (ch != L'\t' && ch != L'\r' && ch != L'\n') pulita.push_back(ch);
            }
            const size_t ini = pulita.find_first_not_of(L' ');
            if (ini == std::wstring::npos) return {};
            const size_t fin = pulita.find_last_not_of(L' ');
            return pulita.substr(ini, fin - ini + 1);
        }

        void aggiornaFormati() {
            const float s = v.dpi;
            if (!fTitolo) fTitolo = v.formato(14.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            if (!fSotto) fSotto = v.formato(12.0f * s, DWRITE_FONT_WEIGHT_NORMAL);
            if (!fInput) {
                fInput = v.formato(13.5f * s);
                if (fInput) {
                    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 };
                    fInput->SetTrimming(&tr, nullptr);
                }
            }
            if (!fRow) fRow = v.formato(13.0f * s, DWRITE_FONT_WEIGHT_NORMAL);
            if (!fBtn) fBtn = v.formato(13.0f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        void aggiornaSheen() {
            if (!v.rt) return;
            if (sheen && sheenGiro == v.giroRt) return;
            if (sheen) { sheen->Release(); sheen = nullptr; }
            D2D1_GRADIENT_STOP stops[2]{
                { 0.0f, D2D1::ColorF(1, 1, 1, v.scuro ? 0.10f : 0.35f) },
                { 1.0f, D2D1::ColorF(1, 1, 1, 0.0f) }
            };
            ID2D1GradientStopCollection* col = nullptr;
            if (SUCCEEDED(v.rt->CreateGradientStopCollection(stops, 2, &col)) && col) {
                v.rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, (float)v.H * 0.45f }),
                    col, &sheen);
                col->Release();
            }
            sheenGiro = v.giroRt;
        }

        void libera() {
            if (sheen) { sheen->Release(); sheen = nullptr; }
            if (fTitolo) { fTitolo->Release(); fTitolo = nullptr; }
            if (fSotto) { fSotto->Release(); fSotto = nullptr; }
            if (fInput) { fInput->Release(); fInput = nullptr; }
            if (fRow) { fRow->Release(); fRow = nullptr; }
            if (fBtn) { fBtn->Release(); fBtn = nullptr; }
        }

        D2D1_RECT_F rectInput() const {
            const float s = v.dpi;
            const float pad = 18.0f * s;
            return { pad, 66.0f * s, (float)v.W - pad, 104.0f * s };
        }

        D2D1_RECT_F rectToggleRow() const {
            const float s = v.dpi;
            const float pad = 18.0f * s;
            return { pad, 118.0f * s, (float)v.W - pad, 154.0f * s };
        }

        D2D1_RECT_F rectTogglePill() const {
            const float s = v.dpi;
            const D2D1_RECT_F row = rectToggleRow();
            const float pw = 42.0f * s, ph = 24.0f * s;
            const float cy = (row.top + row.bottom) * 0.5f;
            return { row.right - pw, cy - ph * 0.5f, row.right, cy + ph * 0.5f };
        }

        D2D1_RECT_F rectCancel() const {
            const float s = v.dpi;
            const float pad = 18.0f * s, btnW = 86.0f * s, btnH = 32.0f * s, gap = 10.0f * s;
            const float y = (float)v.H - pad - btnH;
            const float x = (float)v.W - pad - 2.0f * btnW - gap;
            return { x, y, x + btnW, y + btnH };
        }

        D2D1_RECT_F rectSave() const {
            const float s = v.dpi;
            const float pad = 18.0f * s, btnW = 86.0f * s, btnH = 32.0f * s;
            const float y = (float)v.H - pad - btnH;
            const float x = (float)v.W - pad - btnW;
            return { x, y, x + btnW, y + btnH };
        }

        static bool dentro(const D2D1_RECT_F& r, float x, float y) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        }

        IDWriteTextLayout* creaLayoutInput() const {
            if (!v.fdw || !fInput) return nullptr;
            IDWriteTextLayout* layout = nullptr;
            const D2D1_RECT_F ri = rectInput();
            v.fdw->CreateTextLayout(testo.c_str(), (UINT32)testo.size(), fInput, 10000.0f, ri.bottom - ri.top, &layout);
            return layout;
        }

        int posDaPunto(float px) {
            IDWriteTextLayout* layout = creaLayoutInput();
            if (!layout) return (int)testo.size();
            const float s = v.dpi;
            const D2D1_RECT_F ri = rectInput();
            const float textX = ri.left + 11.0f * s;
            DWRITE_HIT_TEST_METRICS htm{};
            BOOL isTrailing = FALSE, isInside = FALSE;
            layout->HitTestPoint(px - (textX - scrollX), (ri.bottom - ri.top) * 0.5f, &isTrailing, &isInside, &htm);
            layout->Release();
            const int pos = (int)htm.textPosition + (isTrailing ? 1 : 0);
            return std::clamp(pos, 0, (int)testo.size());
        }

        void eliminaSelezione() {
            if (selIni == selFin) return;
            const int lo = std::min(selIni, selFin);
            const int hi = std::max(selIni, selFin);
            testo.erase((size_t)lo, (size_t)(hi - lo));
            caret = selIni = selFin = lo;
        }

        void resetBlink(HWND h) {
            caretVis = true;
            SetTimer(h, 1, 530, nullptr);
            InvalidateRect(h, nullptr, FALSE);
        }

        void copiaSelezione(HWND h) const {
            if (selIni == selFin) return;
            const int lo = std::min(selIni, selFin);
            const int hi = std::max(selIni, selFin);
            const std::wstring sub = testo.substr((size_t)lo, (size_t)(hi - lo));
            if (sub.empty() || !OpenClipboard(h)) return;
            EmptyClipboard();
            const SIZE_T bytes = (sub.size() + 1) * sizeof(wchar_t);
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hg) {
                if (void* p = GlobalLock(hg)) {
                    memcpy(p, sub.c_str(), bytes);
                    GlobalUnlock(hg);
                }
                SetClipboardData(CF_UNICODETEXT, hg);
            }
            CloseClipboard();
        }

        void incolla(HWND h) {
            if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(h)) return;
            HANDLE hd = GetClipboardData(CF_UNICODETEXT);
            std::wstring ins;
            if (hd) {
                if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) {
                    for (const wchar_t* q = p; *q; ++q) {
                        if (*q != L'\t' && *q != L'\r' && *q != L'\n') ins.push_back(*q);
                    }
                    GlobalUnlock(hd);
                }
            }
            CloseClipboard();
            if (!ins.empty()) {
                eliminaSelezione();
                testo.insert((size_t)caret, ins);
                caret += (int)ins.size();
                selIni = selFin = caret;
                resetBlink(h);
            }
        }

        void disegna() {
            if (!v.inizia(0.82f)) return;
            aggiornaFormati();
            aggiornaSheen();

            const float s = v.dpi;
            const float rad = v.raggio();
            if (sheen) {
                v.rt->FillRoundedRectangle(
                    D2D1::RoundedRect({ 0, 0, (float)v.W, (float)v.H }, rad, rad),
                    sheen);
            }

            v.pen->SetColor(D2D1::ColorF(1, 1, 1, v.scuro ? 0.18f : 0.70f));
            v.rt->DrawRoundedRectangle(
                D2D1::RoundedRect({ 0.5f, 0.5f, (float)v.W - 0.5f, (float)v.H - 0.5f },
                    std::max(0.0f, rad - 0.5f), std::max(0.0f, rad - 0.5f)),
                v.pen, 1.0f);

            const float pad = 18.0f * s;
            v.testo(titolo, fTitolo, { pad, 16.0f * s, (float)v.W - pad, 38.0f * s }, v.inchiostro());
            v.testo(sottotitolo, fSotto, { pad, 38.0f * s, (float)v.W - pad, 56.0f * s }, v.inchiostro(0.65f));

            const D2D1_RECT_F ri = rectInput();
            const float inRad = 10.0f * s;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.68f));
            v.rt->FillRoundedRectangle(D2D1::RoundedRect(ri, inRad, inRad), v.pen);
            v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.85f));
            v.rt->DrawRoundedRectangle(D2D1::RoundedRect(ri, inRad, inRad), v.pen, 1.5f * s);

            const float textX = ri.left + 11.0f * s;
            const float textR = ri.right - 11.0f * s;
            const float innerW = std::max(1.0f, textR - textX);
            if (testo.empty()) {
                v.testo(placeholder, fInput, { textX, ri.top, textR, ri.bottom }, v.inchiostro(0.45f));
                if (caretVis) {
                    v.pen->SetColor(v.inchiostro());
                    v.rt->DrawLine({ textX, ri.top + 7.5f * s }, { textX, ri.bottom - 7.5f * s }, v.pen, 1.5f * s);
                }
            } else if (IDWriteTextLayout* layout = creaLayoutInput()) {
                FLOAT cx = 0, cy = 0;
                DWRITE_HIT_TEST_METRICS cm{};
                layout->HitTestTextPosition((UINT32)caret, FALSE, &cx, &cy, &cm);

                DWRITE_TEXT_METRICS tm{};
                layout->GetMetrics(&tm);
                if (tm.widthIncludingTrailingWhitespace <= innerW) {
                    scrollX = 0.0f;
                } else {
                    if (cx - scrollX > innerW - 2.0f * s) scrollX = cx - (innerW - 2.0f * s);
                    if (cx - scrollX < 0.0f) scrollX = cx;
                    scrollX = std::clamp(scrollX, 0.0f, std::max(0.0f, tm.widthIncludingTrailingWhitespace - innerW + 2.0f * s));
                }

                v.rt->PushAxisAlignedClip({ textX - 1.0f, ri.top + 2.0f * s, textR + 1.0f, ri.bottom - 2.0f * s },
                    D2D1_ANTIALIAS_MODE_ALIASED);

                if (selIni != selFin) {
                    const int lo = std::min(selIni, selFin);
                    const int hi = std::max(selIni, selFin);
                    FLOAT x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                    DWRITE_HIT_TEST_METRICS m0{}, m1{};
                    layout->HitTestTextPosition((UINT32)lo, FALSE, &x0, &y0, &m0);
                    layout->HitTestTextPosition((UINT32)hi, FALSE, &x1, &y1, &m1);
                    v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.34f));
                    v.rt->FillRectangle({ textX + x0 - scrollX, ri.top + 7.0f * s, textX + x1 - scrollX, ri.bottom - 7.0f * s }, v.pen);
                }

                v.pen->SetColor(v.inchiostro());
                v.rt->DrawTextLayout({ textX - scrollX, ri.top }, layout, v.pen, D2D1_DRAW_TEXT_OPTIONS_CLIP);

                if (caretVis) {
                    const float kx = textX + cx - scrollX;
                    v.pen->SetColor(v.inchiostro());
                    v.rt->DrawLine({ kx, ri.top + 7.5f * s }, { kx, ri.bottom - 7.5f * s }, v.pen, 1.5f * s);
                }

                v.rt->PopAxisAlignedClip();
                layout->Release();
            }

            const D2D1_RECT_F row = rectToggleRow();
            const D2D1_RECT_F pill = rectTogglePill();
            const float dim = adminDisabilitato ? 0.38f : 1.0f;
            if (hovToggle && !adminDisabilitato) {
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                v.rt->FillRoundedRectangle(D2D1::RoundedRect({ row.left - 6.0f * s, row.top, row.right + 6.0f * s, row.bottom }, 8.0f * s, 8.0f * s), v.pen);
            }
            v.testo(testi::t(L"Esegui come amministratore", L"Run as administrator"), fRow,
                { row.left, row.top, pill.left - 12.0f * s, row.bottom }, v.inchiostro(dim));

            const float pr = (pill.bottom - pill.top) * 0.5f;
            const bool acceso = comeAdmin && !adminDisabilitato;
            if (acceso) {
                v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.95f * dim));
            } else {
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.18f * dim) : D2D1::ColorF(0, 0, 0, 0.16f * dim));
            }
            v.rt->FillRoundedRectangle(D2D1::RoundedRect(pill, pr, pr), v.pen);
            const float knobR = pr - 3.0f * s;
            const float knobCx = acceso ? (pill.right - pr) : (pill.left + pr);
            const float knobCy = (pill.top + pill.bottom) * 0.5f;
            v.pen->SetColor(D2D1::ColorF(1, 1, 1, 0.96f * dim));
            v.rt->FillEllipse({ { knobCx, knobCy }, knobR, knobR }, v.pen);

            const D2D1_RECT_F rc = rectCancel();
            const float btnRad = 9.0f * s;
            v.pen->SetColor(v.scuro
                ? D2D1::ColorF(1, 1, 1, hovCancel ? 0.18f : 0.10f)
                : D2D1::ColorF(0, 0, 0, hovCancel ? 0.10f : 0.05f));
            v.rt->FillRoundedRectangle(D2D1::RoundedRect(rc, btnRad, btnRad), v.pen);
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.10f));
            v.rt->DrawRoundedRectangle(D2D1::RoundedRect(rc, btnRad, btnRad), v.pen, 1.0f);
            v.testo(testi::t(L"Annulla", L"Cancel"), fBtn, rc, v.inchiostro());

            const D2D1_RECT_F rs = rectSave();
            v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, hovSave ? 1.0f : 0.90f));
            v.rt->FillRoundedRectangle(D2D1::RoundedRect(rs, btnRad, btnRad), v.pen);
            v.pen->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
            v.rt->DrawLine({ rs.left + btnRad, rs.top + 0.75f }, { rs.right - btnRad, rs.top + 0.75f }, v.pen, 1.0f);
            v.testo(testi::t(L"Salva", L"Save"), fBtn, rs, D2D1::ColorF(1, 1, 1, 1.0f));

            v.finisci();
        }

        bool messaggio(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
            switch (m) {
                case WM_PAINT:
                    disegna();
                    return true;
                case WM_TIMER:
                    if (w == 1) {
                        caretVis = !caretVis;
                        InvalidateRect(h, nullptr, FALSE);
                        return true;
                    }
                    if (w == 2) {
                        const double p = std::clamp((double)(GetTickCount64() - tFade) / 140.0, 0.0, 1.0);
                        v.alfa(vetro::uscita(p));
                        if (p >= 1.0) KillTimer(h, 2);
                        return true;
                    }
                    return false;
                case WM_CHAR: {
                    if (w == 0x01) {
                        selIni = 0;
                        selFin = caret = (int)testo.size();
                        resetBlink(h);
                        return true;
                    }
                    if (w == 0x03) {
                        copiaSelezione(h);
                        return true;
                    }
                    if (w == 0x18) {
                        copiaSelezione(h);
                        eliminaSelezione();
                        resetBlink(h);
                        return true;
                    }
                    if (w == 0x16) {
                        incolla(h);
                        return true;
                    }
                    if (w == L'\b') {
                        if (selIni != selFin) {
                            eliminaSelezione();
                        } else if (caret > 0) {
                            testo.erase((size_t)(caret - 1), 1);
                            caret--;
                            selIni = selFin = caret;
                        }
                        resetBlink(h);
                        return true;
                    }
                    if (w >= 32) {
                        eliminaSelezione();
                        testo.insert((size_t)caret, 1, (wchar_t)w);
                        caret++;
                        selIni = selFin = caret;
                        resetBlink(h);
                        return true;
                    }
                    return false;
                }
                case WM_KEYDOWN:
                    if (w == VK_RETURN) {
                        confermato = true;
                        finito = true;
                        return true;
                    }
                    if (w == VK_ESCAPE) {
                        finito = true;
                        return true;
                    }
                    if (w == VK_LEFT) {
                        if (selIni != selFin) caret = std::min(selIni, selFin);
                        else if (caret > 0) caret--;
                        selIni = selFin = caret;
                        resetBlink(h);
                        return true;
                    }
                    if (w == VK_RIGHT) {
                        if (selIni != selFin) caret = std::max(selIni, selFin);
                        else if (caret < (int)testo.size()) caret++;
                        selIni = selFin = caret;
                        resetBlink(h);
                        return true;
                    }
                    if (w == VK_HOME) {
                        caret = selIni = selFin = 0;
                        resetBlink(h);
                        return true;
                    }
                    if (w == VK_END) {
                        caret = selIni = selFin = (int)testo.size();
                        resetBlink(h);
                        return true;
                    }
                    if (w == VK_DELETE) {
                        if (selIni != selFin) {
                            eliminaSelezione();
                        } else if (caret < (int)testo.size()) {
                            testo.erase((size_t)caret, 1);
                            selIni = selFin = caret;
                        }
                        resetBlink(h);
                        return true;
                    }
                    return false;
                case WM_MOUSEMOVE: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (draggingSel) {
                        caret = selFin = posDaPunto(mx);
                        resetBlink(h);
                        return true;
                    }
                    if (!trackingMouse) {
                        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                        trackingMouse = TrackMouseEvent(&tme) != FALSE;
                    }
                    const bool hs = dentro(rectSave(), mx, my);
                    const bool hc = dentro(rectCancel(), mx, my);
                    const bool ht = !adminDisabilitato && dentro(rectToggleRow(), mx, my);
                    if (hs != hovSave || hc != hovCancel || ht != hovToggle) {
                        hovSave = hs;
                        hovCancel = hc;
                        hovToggle = ht;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    return true;
                }
                case WM_MOUSELEAVE:
                    trackingMouse = false;
                    if (hovSave || hovCancel || hovToggle) {
                        hovSave = hovCancel = hovToggle = false;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    return true;
                case WM_LBUTTONDOWN: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (dentro(rectInput(), mx, my)) {
                        caret = selIni = selFin = posDaPunto(mx);
                        draggingSel = true;
                        SetCapture(h);
                        resetBlink(h);
                        return true;
                    }
                    if (!adminDisabilitato && dentro(rectToggleRow(), mx, my)) {
                        pressedBtn = 3;
                        SetCapture(h);
                        return true;
                    }
                    if (dentro(rectCancel(), mx, my)) {
                        pressedBtn = 1;
                        SetCapture(h);
                        return true;
                    }
                    if (dentro(rectSave(), mx, my)) {
                        pressedBtn = 2;
                        SetCapture(h);
                        return true;
                    }
                    ReleaseCapture();
                    SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    return true;
                }
                case WM_LBUTTONUP: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (draggingSel) {
                        draggingSel = false;
                        ReleaseCapture();
                        return true;
                    }
                    if (pressedBtn != 0) {
                        const int btn = pressedBtn;
                        pressedBtn = 0;
                        ReleaseCapture();
                        if (btn == 1 && dentro(rectCancel(), mx, my)) {
                            finito = true;
                        } else if (btn == 2 && dentro(rectSave(), mx, my)) {
                            confermato = true;
                            finito = true;
                        } else if (btn == 3 && !adminDisabilitato && dentro(rectToggleRow(), mx, my)) {
                            comeAdmin = !comeAdmin;
                            InvalidateRect(h, nullptr, FALSE);
                        }
                        return true;
                    }
                    return false;
                }
                case WM_LBUTTONDBLCLK: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (dentro(rectInput(), mx, my)) {
                        selIni = 0;
                        selFin = caret = (int)testo.size();
                        resetBlink(h);
                        return true;
                    }
                    return false;
                }
                case WM_CLOSE:
                    finito = true;
                    r = 0;
                    return true;
            }
            return false;
        }
    } dlg;

    dlg.titolo = testi::t(L"Proprietà gioco", L"Game properties");
    dlg.sottotitolo = a.nome;
    dlg.placeholder = testi::t(L"Argomenti di avvio (es. -novid -dx12)", L"Launch arguments (e.g. -novid -dx12)");
    dlg.testo = a.argomentiExtra;
    dlg.adminDisabilitato = a.avvio.find(L"://") != std::wstring::npos;
    dlg.comeAdmin = dlg.adminDisabilitato ? false : a.comeAdmin;
    dlg.selIni = dlg.selFin = dlg.caret = (int)dlg.testo.size();

    const wchar_t* clsName = L"FluidWin.OpzioniGioco";
    WNDCLASSW wc{};
    if (!GetClassInfoW(GetModuleHandleW(nullptr), clsName, &wc)) {
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = vetro::procedura<OpzioniGiocoDlg>;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = clsName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
    }

    if (dlg.v.crea(clsName, vetro::procedura<OpzioniGiocoDlg>, &dlg, true, 2, true, true)) {
        const int W = (int)std::lround(430.0f * dpi);
        const int H = (int)std::lround(226.0f * dpi);
        RECT rl{};
        GetWindowRect(launcher, &rl);
        int wx = rl.left + ((rl.right - rl.left) - W) / 2;
        int wy = rl.top + ((rl.bottom - rl.top) - H) / 2;
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(launcher, MONITOR_DEFAULTTONEAREST), &mi)) {
            wx = std::clamp(wx, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - W));
            wy = std::clamp(wy, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - H));
        }

        dlg.v.posiziona(wx, wy, W, H, HWND_TOPMOST, 0);
        EnableWindow(launcher, FALSE);
        dlg.tFade = GetTickCount64();
        ShowWindow(dlg.v.fin, SW_SHOW);
        SetForegroundWindow(dlg.v.fin);
        SetFocus(dlg.v.fin);
        SetTimer(dlg.v.fin, 1, 530, nullptr);
        SetTimer(dlg.v.fin, 2, 16, nullptr);
        InvalidateRect(dlg.v.fin, nullptr, FALSE);

        MSG msg{};
        while (!dlg.finito && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (dlg.confermato) {
            const std::wstring chiave = chiaveGioco(a);
            const std::wstring args = OpzioniGiocoDlg::trim(dlg.testo);
            const bool admin = dlg.adminDisabilitato ? false : dlg.comeAdmin;
            auto opt = leggiOpzioniGiochi();
            if (!admin && args.empty()) opt.erase(chiave);
            else opt[chiave] = {admin, args};
            salvaOpzioniGiochi(opt);
            for (auto& g : tuttiGiochi) {
                if (chiaveGioco(g) == chiave) {
                    g.comeAdmin = admin;
                    g.argomentiExtra = args;
                }
            }
        }

        KillTimer(dlg.v.fin, 1);
        KillTimer(dlg.v.fin, 2);
        EnableWindow(launcher, TRUE);
    }

    dlg.libera();
    dlg.v.chiudi();

    lDialogoAperto = false;
    if (lAperto && !lChiusura) prendiFuoco(launcher);
}

inline auto Motore::impostaChiaveSteamGridDB() -> void {
    std::wstring k;
    if (dialogoTestoVetro(
            testi::t(L"Chiave API SteamGridDB", L"SteamGridDB API key"),
            testi::t(L"Lascia vuoto per usare solo Steam (steamgriddb.com/profile/preferences/api)",
                     L"Leave empty for Steam-only fallback (steamgriddb.com/profile/preferences/api)"),
            leggiChiaveSteamGridDB(),
            testi::t(L"Salva", L"Save"),
            k,
            true)) {
        salvaChiaveSteamGridDB(k);
    }
}

inline auto Motore::scaricaIconaGioco(const AppL& a) -> void {
    AppL copia = a;
    copia.icona = nullptr;
    const std::wstring chiave = chiaveGioco(a);
    const std::wstring apiKey = leggiChiaveSteamGridDB();
    auto ris = std::make_shared<std::wstring>();
    inSottofondo(
        [copia, apiKey, ris] { *ris = scaricaIconaGiocoOnline(copia, apiKey); },
        [this, chiave, ris] {
            if (ris->empty()) return;
            const std::wstring& p = *ris;
            auto ic = leggiIconeGiochi();
            ic[chiave] = p;
            salvaIconeGiochi(ic);
            iconeChieste.erase(chiaveIcona(p, 96));
            for (auto& g : tuttiGiochi) {
                if (chiaveGioco(g) == chiave) {
                    if (g.icona) { g.icona->Release(); g.icona = nullptr; }
                    g.iconaFile = p;
                    g.iconaPers = true;
                    g.provata = true;
                }
            }
            chiediIcona(p, 96);
            lDirty = true;
        });
}

inline auto Motore::scegliIconaOnline(const AppL& a) -> void {
    lDialogoAperto = true;

    struct IconaTrovata {
        std::wstring thumbUrl;
        std::wstring fullUrl;
        std::string rawBytes;
        std::vector<BYTE> bgra;
    };

    struct StatoWorker {
        std::mutex mtx;
        std::atomic<bool> annulla{false};
        std::atomic<int> generazione{0};
        HWND hwnd = nullptr;
        std::deque<IconaTrovata> coda;
        int totaleCandidati = 0;
    };

    struct ElementoIcona {
        std::wstring thumbUrl;
        std::wstring fullUrl;
        std::string rawBytes;
        std::vector<BYTE> bgra;
        ID2D1Bitmap* bmp = nullptr;
    };

    struct ScegliIconaDlg {
        Motore* motore = nullptr;
        AppL gioco;
        std::wstring apiKey;
        vetro::Finestra v;
        IDWriteTextFormat* fTitolo = nullptr;
        IDWriteTextFormat* fSotto = nullptr;
        IDWriteTextFormat* fStato = nullptr;
        IDWriteTextFormat* fBtn = nullptr;
        IDWriteTextFormat* fVuotoTitolo = nullptr;
        IDWriteTextFormat* fVuoto = nullptr;
        ID2D1LinearGradientBrush* sheen = nullptr;
        unsigned sheenGiro = 0;
        unsigned bmpGiro = 0;
        float fmtDpi = 0.0f;

        std::vector<ElementoIcona> elementi;
        std::shared_ptr<StatoWorker> worker;
        int genCorrente = 0;
        bool caricamento = true;
        int totaleAttesi = 0;

        float scrollY = 0.0f;
        int hovTile = -1;
        bool hovClose = false;
        bool hovCancel = false;
        bool trackingMouse = false;
        int pressedKind = 0;
        int pressedTile = -1;

        bool finito = false;
        bool sceltoOk = false;
        std::wstring sceltoThumbUrl;
        std::wstring sceltoFullUrl;
        std::string sceltoRaw;
        ULONGLONG tFade = 0;

        static bool dentro(const D2D1_RECT_F& r, float x, float y) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        }

        float headerH() const { return 56.0f * v.dpi; }
        float footerH() const { return 52.0f * v.dpi; }
        float gridTop() const { return headerH(); }
        float gridBottom() const { return (float)v.H - footerH(); }

        D2D1_RECT_F rectClose() const {
            const float s = v.dpi;
            const float pad = 20.0f * s, sz = 28.0f * s;
            return { (float)v.W - pad - sz, 14.0f * s, (float)v.W - pad, 14.0f * s + sz };
        }

        D2D1_RECT_F rectCancel() const {
            const float s = v.dpi;
            const float pad = 20.0f * s, h = 30.0f * s, w = 88.0f * s;
            const float y = gridBottom() + (footerH() - h) * 0.5f;
            return { (float)v.W - pad - w, y, (float)v.W - pad, y + h };
        }

        int slotCount() const {
            const int loaded = (int)elementi.size();
            if (!caricamento) return loaded;
            return std::max(loaded, totaleAttesi > 0 ? totaleAttesi : 8);
        }

        D2D1_RECT_F tileRect(int idx) const {
            const float s = v.dpi;
            const float tile = 92.0f * s, gap = 12.0f * s;
            const float gridW = 4.0f * tile + 3.0f * gap;
            const float x0 = ((float)v.W - gridW) * 0.5f;
            const float topPad = (caricamento ? 34.0f : 14.0f) * s;
            const int col = idx % 4;
            const int row = idx / 4;
            const float x = x0 + (float)col * (tile + gap);
            const float y = gridTop() + topPad + (float)row * (tile + gap) - scrollY;
            return { x, y, x + tile, y + tile };
        }

        float scrollMax() const {
            const int cnt = slotCount();
            if (cnt <= 0) return 0.0f;
            const float s = v.dpi;
            const float tile = 92.0f * s, gap = 12.0f * s;
            const float topPad = (caricamento ? 34.0f : 14.0f) * s;
            const float botPad = 14.0f * s;
            const int rows = (cnt + 3) / 4;
            const float contentH = topPad + (float)rows * tile + (float)std::max(0, rows - 1) * gap + botPad;
            const float viewH = std::max(1.0f, gridBottom() - gridTop());
            return std::max(0.0f, contentH - viewH);
        }

        int tileAt(float mx, float my) const {
            if (my < gridTop() || my > gridBottom()) return -1;
            for (int i = 0; i < (int)elementi.size(); i++) {
                if (dentro(tileRect(i), mx, my)) return i;
            }
            return -1;
        }

        void aggiornaFormati() {
            if (fTitolo && std::abs(fmtDpi - v.dpi) < 0.01f) return;
            for (IDWriteTextFormat** fp : { &fTitolo, &fSotto, &fStato, &fBtn, &fVuotoTitolo, &fVuoto }) {
                if (*fp) { (*fp)->Release(); *fp = nullptr; }
            }
            const float s = v.dpi;
            fTitolo = v.formato(15.0f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
            fSotto = v.formato(11.5f * s, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
            fStato = v.formato(11.5f * s, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
            fBtn = v.formato(12.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
            fVuotoTitolo = v.formato(14.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
            fVuoto = v.formato(12.5f * s, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
            fmtDpi = s;
        }

        void aggiornaSheen() {
            if (sheen && sheenGiro == v.giroRt) return;
            if (sheen) { sheen->Release(); sheen = nullptr; }
            if (!v.rt) return;
            const D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, D2D1::ColorF(1, 1, 1, v.scuro ? 0.14f : 0.42f) },
                { 1.0f, D2D1::ColorF(1, 1, 1, 0.0f) }
            };
            ID2D1GradientStopCollection* col = nullptr;
            if (SUCCEEDED(v.rt->CreateGradientStopCollection(stops, 2, &col)) && col) {
                v.rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, (float)v.H * 0.42f }),
                    col, &sheen);
                col->Release();
            }
            sheenGiro = v.giroRt;
        }

        ID2D1Bitmap* creaBitmap96(const std::vector<BYTE>& bgra) {
            if (!v.rt || bgra.size() < 96u * 96u * 4u) return nullptr;
            ID2D1Bitmap* bmp = nullptr;
            const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(v.rt->CreateBitmap(D2D1::SizeU(96, 96), bgra.data(), 96 * 4, bp, &bmp))) return nullptr;
            return bmp;
        }

        void aggiornaBitmapsRt() {
            if (bmpGiro == v.giroRt) return;
            for (auto& el : elementi) {
                if (el.bmp) { el.bmp->Release(); el.bmp = nullptr; }
                el.bmp = creaBitmap96(el.bgra);
            }
            bmpGiro = v.giroRt;
        }

        void liberaBitmaps() {
            for (auto& el : elementi) {
                if (el.bmp) { el.bmp->Release(); el.bmp = nullptr; }
            }
        }

        void libera() {
            liberaBitmaps();
            if (sheen) { sheen->Release(); sheen = nullptr; }
            for (IDWriteTextFormat** fp : { &fTitolo, &fSotto, &fStato, &fBtn, &fVuotoTitolo, &fVuoto }) {
                if (*fp) { (*fp)->Release(); *fp = nullptr; }
            }
        }

        void fermaWorker() {
            if (!worker) return;
            std::lock_guard<std::mutex> lk(worker->mtx);
            worker->annulla.store(true);
            worker->hwnd = nullptr;
        }

        void avviaRicerca() {
            fermaWorker();
            liberaBitmaps();
            elementi.clear();
            scrollY = 0.0f;
            hovTile = -1;
            pressedTile = -1;
            caricamento = true;
            totaleAttesi = 0;

            const int gen = ++genCorrente;
            auto st = std::make_shared<StatoWorker>();
            st->generazione.store(gen);
            st->hwnd = v.fin;
            worker = st;

            AppL copiaGioco = gioco;
            copiaGioco.icona = nullptr;
            const std::wstring key = apiKey;

            std::thread([st, copiaGioco = std::move(copiaGioco), key, gen]() {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const auto cands = cercaCandidatiIconaOnline(copiaGioco, key, 16);
                if (!st->annulla.load()) {
                    {
                        std::lock_guard<std::mutex> lk(st->mtx);
                        st->totaleCandidati = (int)cands.size();
                        if (st->hwnd && !st->annulla.load()) {
                            PostMessageW(st->hwnd, WM_APP + 2, (WPARAM)gen, 0);
                        }
                    }
                    for (const auto& c : cands) {
                        if (st->annulla.load()) break;
                        std::string raw = httpPrendi(c.thumbUrl, L"", 4 * 1024 * 1024);
                        if (st->annulla.load()) break;
                        if (estensioneDaMagic(raw, false).empty()) continue;
                        std::vector<BYTE> bgra;
                        if (!decodificaImmagineMemoria(raw, 96, bgra)) continue;
                        if (st->annulla.load()) break;
                        std::lock_guard<std::mutex> lk(st->mtx);
                        if (!st->hwnd || st->annulla.load()) break;
                        st->coda.push_back({ c.thumbUrl, c.fullUrl, std::move(raw), std::move(bgra) });
                        PostMessageW(st->hwnd, WM_APP + 2, (WPARAM)gen, 0);
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(st->mtx);
                    if (st->hwnd && !st->annulla.load()) {
                        PostMessageW(st->hwnd, WM_APP + 3, (WPARAM)gen, 0);
                    }
                }
                CoUninitialize();
            }).detach();

            if (v.fin) InvalidateRect(v.fin, nullptr, FALSE);
        }

        void raccogliRisultati(int gen) {
            if (gen != genCorrente || !worker) return;
            std::deque<IconaTrovata> arrivate;
            {
                std::lock_guard<std::mutex> lk(worker->mtx);
                totaleAttesi = std::max(totaleAttesi, worker->totaleCandidati);
                arrivate.swap(worker->coda);
            }
            bmpGiro = v.giroRt;
            for (auto& it : arrivate) {
                ElementoIcona el;
                el.thumbUrl = std::move(it.thumbUrl);
                el.fullUrl = std::move(it.fullUrl);
                el.rawBytes = std::move(it.rawBytes);
                el.bgra = std::move(it.bgra);
                el.bmp = creaBitmap96(el.bgra);
                if (el.bmp) elementi.push_back(std::move(el));
            }
            if (v.fin) InvalidateRect(v.fin, nullptr, FALSE);
        }

        void disegna() {
            if (!v.inizia(0.84f)) return;
            aggiornaFormati();
            aggiornaSheen();
            aggiornaBitmapsRt();

            const float s = v.dpi;
            const float rad = v.raggio();
            if (sheen) {
                v.rt->FillRoundedRectangle(
                    D2D1::RoundedRect({ 0, 0, (float)v.W, (float)v.H }, rad, rad),
                    sheen);
            }

            v.pen->SetColor(D2D1::ColorF(1, 1, 1, v.scuro ? 0.18f : 0.72f));
            v.rt->DrawRoundedRectangle(
                D2D1::RoundedRect({ 0.5f, 0.5f, (float)v.W - 0.5f, (float)v.H - 0.5f },
                    std::max(0.0f, rad - 0.5f), std::max(0.0f, rad - 0.5f)),
                v.pen, 1.0f);

            const float pad = 20.0f * s;
            const D2D1_RECT_F rClose = rectClose();
            v.testo(testi::t(L"Scegli icona online", L"Choose icon online"),
                    fTitolo, { pad, 10.0f * s, rClose.left - 8.0f * s, 32.0f * s }, v.inchiostro());
            const std::wstring sub = gioco.nome + L" · " + (apiKey.empty() ? L"Steam" : L"SteamGridDB + Steam");
            v.testo(sub, fSotto, { pad, 32.0f * s, rClose.left - 8.0f * s, 50.0f * s }, v.inchiostro(0.62f));

            const float closeRad = (rClose.right - rClose.left) * 0.5f;
            v.pen->SetColor(v.scuro
                ? D2D1::ColorF(1, 1, 1, hovClose ? 0.18f : 0.08f)
                : D2D1::ColorF(0, 0, 0, hovClose ? 0.11f : 0.05f));
            v.rt->FillRoundedRectangle(D2D1::RoundedRect(rClose, closeRad, closeRad), v.pen);
            const float cx = (rClose.left + rClose.right) * 0.5f;
            const float cy = (rClose.top + rClose.bottom) * 0.5f;
            const float arm = 4.8f * s;
            v.pen->SetColor(v.inchiostro(hovClose ? 0.95f : 0.72f));
            v.rt->DrawLine({ cx - arm, cy - arm }, { cx + arm, cy + arm }, v.pen, 1.6f * s);
            v.rt->DrawLine({ cx + arm, cy - arm }, { cx - arm, cy + arm }, v.pen, 1.6f * s);

            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.08f));
            v.rt->DrawLine({ pad, headerH() }, { (float)v.W - pad, headerH() }, v.pen, 1.0f);

            const float gTop = gridTop();
            const float gBot = gridBottom();
            v.rt->PushAxisAlignedClip({ 0.0f, gTop, (float)v.W, gBot }, D2D1_ANTIALIAS_MODE_ALIASED);

            if (caricamento) {
                const wchar_t* msg = totaleAttesi == 0
                    ? testi::t(L"Ricerca online…", L"Searching online…")
                    : testi::t(L"Caricamento icone…", L"Loading icons…");
                v.testo(msg, fStato, { pad, gTop + 8.0f * s, (float)v.W - pad, gTop + 28.0f * s }, v.inchiostro(0.62f));
            }

            if (!caricamento && elementi.empty()) {
                const float midY = (gTop + gBot) * 0.5f;
                v.testo(testi::t(L"Nessuna icona trovata online", L"No icons found online"),
                        fVuotoTitolo, { pad, midY - 28.0f * s, (float)v.W - pad, midY - 2.0f * s }, v.inchiostro(0.85f));
                v.testo(testi::t(L"Imposta una chiave API SteamGridDB o rinomina il gioco",
                                 L"Try setting a SteamGridDB API key or renaming the game"),
                        fVuoto, { pad + 16.0f * s, midY + 4.0f * s, (float)v.W - pad - 16.0f * s, midY + 48.0f * s }, v.inchiostro(0.58f));
            } else {
                const int cnt = slotCount();
                const float tileRad = 14.0f * s;
                const float iconSz = 76.0f * s;
                const float iconRad = 10.0f * s;
                for (int idx = 0; idx < cnt; idx++) {
                    const D2D1_RECT_F tr = tileRect(idx);
                    if (tr.bottom < gTop || tr.top > gBot) continue;

                    const bool loaded = idx < (int)elementi.size();
                    const bool hov = loaded && idx == hovTile;
                    v.pen->SetColor(v.scuro
                        ? D2D1::ColorF(1, 1, 1, hov ? 0.18f : (loaded ? 0.09f : 0.06f))
                        : D2D1::ColorF(1, 1, 1, hov ? 0.88f : (loaded ? 0.62f : 0.42f)));
                    v.rt->FillRoundedRectangle(D2D1::RoundedRect(tr, tileRad, tileRad), v.pen);

                    if (hov) {
                        v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.88f));
                        v.rt->DrawRoundedRectangle(D2D1::RoundedRect(tr, tileRad, tileRad), v.pen, 1.6f * s);
                    } else {
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.08f));
                        v.rt->DrawRoundedRectangle(D2D1::RoundedRect(tr, tileRad, tileRad), v.pen, 1.0f);
                    }

                    const float ix = (tr.left + tr.right - iconSz) * 0.5f;
                    const float iy = (tr.top + tr.bottom - iconSz) * 0.5f;
                    const D2D1_RECT_F ir = { ix, iy, ix + iconSz, iy + iconSz };
                    if (loaded && elementi[idx].bmp) {
                        ID2D1BitmapBrush* bb = nullptr;
                        const D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(
                            D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        const float k = iconSz / 96.0f;
                        const D2D1_BRUSH_PROPERTIES bp = D2D1::BrushProperties(
                            1.0f, D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(ix, iy));
                        if (SUCCEEDED(v.rt->CreateBitmapBrush(elementi[idx].bmp, bbp, bp, &bb)) && bb) {
                            v.rt->FillRoundedRectangle(D2D1::RoundedRect(ir, iconRad, iconRad), bb);
                            bb->Release();
                        } else {
                            v.rt->DrawBitmap(elementi[idx].bmp, ir, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        }
                    } else {
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.05f));
                        v.rt->FillRoundedRectangle(D2D1::RoundedRect(ir, iconRad, iconRad), v.pen);
                    }
                }
            }

            v.rt->PopAxisAlignedClip();

            const float sMax = scrollMax();
            if (sMax > 0.5f) {
                const float viewH = gBot - gTop;
                const float trackTop = gTop + 8.0f * s;
                const float trackH = std::max(1.0f, viewH - 16.0f * s);
                const float thumbH = std::clamp(trackH * (viewH / (viewH + sMax)), 26.0f * s, trackH);
                const float thumbY = trackTop + (trackH - thumbH) * (scrollY / sMax);
                const float bw = 4.0f * s;
                const float bx = (float)v.W - 8.0f * s - bw;
                v.pen->SetColor(v.inchiostro(0.28f));
                v.rt->FillRoundedRectangle(D2D1::RoundedRect({ bx, thumbY, bx + bw, thumbY + thumbH }, bw * 0.5f, bw * 0.5f), v.pen);
            }

            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.08f));
            v.rt->DrawLine({ pad, gBot }, { (float)v.W - pad, gBot }, v.pen, 1.0f);

            auto disegnaPill = [&](const D2D1_RECT_F& r, const wchar_t* label, bool hov) {
                const float pr = (r.bottom - r.top) * 0.5f;
                v.pen->SetColor(v.scuro
                    ? D2D1::ColorF(1, 1, 1, hov ? 0.18f : 0.10f)
                    : D2D1::ColorF(0, 0, 0, hov ? 0.10f : 0.05f));
                v.rt->FillRoundedRectangle(D2D1::RoundedRect(r, pr, pr), v.pen);
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.10f));
                v.rt->DrawRoundedRectangle(D2D1::RoundedRect(r, pr, pr), v.pen, 1.0f);
                v.testo(label, fBtn, r, v.inchiostro());
            };
            disegnaPill(rectCancel(), testi::t(L"Annulla", L"Cancel"), hovCancel);

            v.finisci();
        }

        bool messaggio(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
            switch (m) {
                case WM_PAINT:
                    disegna();
                    return true;
                case WM_TIMER:
                    if (w == 2) {
                        const double p = std::clamp((double)(GetTickCount64() - tFade) / 140.0, 0.0, 1.0);
                        v.alfa(vetro::uscita(p));
                        if (p >= 1.0) KillTimer(h, 2);
                        return true;
                    }
                    return false;
                case WM_APP + 2:
                    raccogliRisultati((int)w);
                    r = 0;
                    return true;
                case WM_APP + 3:
                    if ((int)w == genCorrente) {
                        raccogliRisultati((int)w);
                        caricamento = false;
                        scrollY = std::clamp(scrollY, 0.0f, scrollMax());
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    r = 0;
                    return true;
                case WM_MOUSEWHEEL: {
                    const float delta = (float)GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA;
                    const float next = std::clamp(scrollY - delta * 52.0f * v.dpi, 0.0f, scrollMax());
                    if (std::abs(next - scrollY) > 0.1f) {
                        scrollY = next;
                        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                        ScreenToClient(h, &pt);
                        hovTile = tileAt((float)pt.x, (float)pt.y);
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    r = 0;
                    return true;
                }
                case WM_KEYDOWN:
                    if (w == VK_ESCAPE) {
                        fermaWorker();
                        finito = true;
                        r = 0;
                        return true;
                    }
                    return false;
                case WM_MOUSEMOVE: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (!trackingMouse) {
                        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                        trackingMouse = TrackMouseEvent(&tme) != FALSE;
                    }
                    const bool hc = dentro(rectClose(), mx, my);
                    const bool ha = dentro(rectCancel(), mx, my);
                    const int ht = tileAt(mx, my);
                    if (hc != hovClose || ha != hovCancel || ht != hovTile) {
                        hovClose = hc;
                        hovCancel = ha;
                        hovTile = ht;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    return true;
                }
                case WM_MOUSELEAVE:
                    trackingMouse = false;
                    if (hovClose || hovCancel || hovTile != -1) {
                        hovClose = hovCancel = false;
                        hovTile = -1;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    return true;
                case WM_LBUTTONDOWN: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    if (dentro(rectClose(), mx, my)) {
                        pressedKind = 1;
                        SetCapture(h);
                        return true;
                    }
                    if (dentro(rectCancel(), mx, my)) {
                        pressedKind = 3;
                        SetCapture(h);
                        return true;
                    }
                    const int t = tileAt(mx, my);
                    if (t >= 0) {
                        pressedKind = 4;
                        pressedTile = t;
                        SetCapture(h);
                        return true;
                    }
                    if (my < headerH()) {
                        ReleaseCapture();
                        SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                        return true;
                    }
                    return false;
                }
                case WM_LBUTTONUP: {
                    const float mx = (float)(short)LOWORD(l);
                    const float my = (float)(short)HIWORD(l);
                    const int kind = pressedKind;
                    const int pTile = pressedTile;
                    pressedKind = 0;
                    pressedTile = -1;
                    if (GetCapture() == h) ReleaseCapture();
                    if (kind == 1 && dentro(rectClose(), mx, my)) {
                        fermaWorker();
                        finito = true;
                        return true;
                    }
                    if (kind == 3 && dentro(rectCancel(), mx, my)) {
                        fermaWorker();
                        finito = true;
                        return true;
                    }
                    if (kind == 4 && pTile >= 0 && pTile < (int)elementi.size() && tileAt(mx, my) == pTile) {
                        sceltoThumbUrl = elementi[pTile].thumbUrl;
                        sceltoFullUrl = elementi[pTile].fullUrl;
                        sceltoRaw = std::move(elementi[pTile].rawBytes);
                        sceltoOk = true;
                        fermaWorker();
                        finito = true;
                        return true;
                    }
                    return false;
                }
                case WM_CLOSE:
                    fermaWorker();
                    finito = true;
                    r = 0;
                    return true;
                case WM_DESTROY:
                    fermaWorker();
                    libera();
                    return false;
            }
            return false;
        }
    } dlg;

    dlg.motore = this;
    dlg.gioco = a;
    dlg.gioco.icona = nullptr;
    dlg.apiKey = leggiChiaveSteamGridDB();

    const wchar_t* clsName = L"FluidWin.ScegliIconaGioco";
    WNDCLASSW wc{};
    if (!GetClassInfoW(GetModuleHandleW(nullptr), clsName, &wc)) {
        wc.lpfnWndProc = vetro::procedura<ScegliIconaDlg>;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = clsName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
    }

    if (dlg.v.crea(clsName, vetro::procedura<ScegliIconaDlg>, &dlg, true, 2, true, true)) {
        const int W = (int)std::lround(472.0f * dpi);
        const int H = (int)std::lround(436.0f * dpi);
        RECT rl{};
        GetWindowRect(launcher, &rl);
        int wx = rl.left + ((rl.right - rl.left) - W) / 2;
        int wy = rl.top + ((rl.bottom - rl.top) - H) / 2;
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(launcher, MONITOR_DEFAULTTONEAREST), &mi)) {
            wx = std::clamp(wx, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - W));
            wy = std::clamp(wy, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - H));
        }

        dlg.v.posiziona(wx, wy, W, H, HWND_TOPMOST, 0);
        EnableWindow(launcher, FALSE);
        dlg.tFade = GetTickCount64();
        ShowWindow(dlg.v.fin, SW_SHOW);
        SetForegroundWindow(dlg.v.fin);
        SetFocus(dlg.v.fin);
        SetTimer(dlg.v.fin, 2, 16, nullptr);
        dlg.avviaRicerca();

        MSG msg{};
        while (!dlg.finito && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        KillTimer(dlg.v.fin, 2);
        EnableWindow(launcher, TRUE);
    }

    dlg.fermaWorker();
    dlg.libera();
    dlg.v.chiudi();

    lDialogoAperto = false;
    if (lAperto && !lChiusura) prendiFuoco(launcher);

    if (dlg.sceltoOk) {
        const std::wstring chiave = chiaveGioco(a);
        const std::wstring thumbUrl = std::move(dlg.sceltoThumbUrl);
        const std::wstring fullUrl = std::move(dlg.sceltoFullUrl);
        auto rawFallback = std::make_shared<std::string>(std::move(dlg.sceltoRaw));
        auto ris = std::make_shared<std::wstring>();
        inSottofondo(
            [chiave, thumbUrl, fullUrl, rawFallback, ris] {
                const std::wstring cacheDir = cartellaCacheIconeGiochi();
                if (cacheDir.empty()) return;
                std::string bytes;
                if (!fullUrl.empty() && fullUrl != thumbUrl) {
                    bytes = httpPrendi(fullUrl, L"", 8 * 1024 * 1024);
                }
                if (bytes.empty() || estensioneDaMagic(bytes, false).empty()) {
                    bytes = *rawFallback;
                }
                const std::wstring ext = estensioneDaMagic(bytes, false);
                if (ext.empty()) return;
                const std::wstring dest = cacheDir + L"\\" + hashChiaveGioco(chiave) + ext;
                if (scriviFileBinario(dest, bytes)) *ris = dest;
            },
            [this, chiave, ris] {
                if (ris->empty()) return;
                const std::wstring& p = *ris;
                auto ic = leggiIconeGiochi();
                ic[chiave] = p;
                salvaIconeGiochi(ic);
                iconeChieste.erase(chiaveIcona(p, 96));
                for (auto& g : tuttiGiochi) {
                    if (chiaveGioco(g) == chiave) {
                        if (g.icona) { g.icona->Release(); g.icona = nullptr; }
                        g.iconaFile = p;
                        g.iconaPers = true;
                        g.provata = true;
                    }
                }
                chiediIcona(p, 96);
                lDirty = true;
            });
    }
}

inline auto Motore::scaricaTutteIconeOnline() -> void {
    if (lScaricaIconeInCorso) return;
    std::vector<AppL> lista;
    for (const auto& g : tuttiGiochi) {
        if (!g.iconaPers) {
            AppL c = g;
            c.icona = nullptr;
            lista.push_back(std::move(c));
        }
    }
    if (lista.empty()) return;
    lScaricaIconeInCorso = true;
    const std::wstring apiKey = leggiChiaveSteamGridDB();
    auto ris = std::make_shared<std::vector<std::pair<std::wstring, std::wstring>>>();
    inSottofondo(
        [lista = std::move(lista), apiKey, ris] {
            for (const auto& g : lista) {
                const std::wstring p = scaricaIconaGiocoOnline(g, apiKey);
                if (!p.empty()) ris->push_back({chiaveGioco(g), p});
            }
        },
        [this, ris] {
            lScaricaIconeInCorso = false;
            if (ris->empty()) return;
            auto ic = leggiIconeGiochi();
            for (const auto& [chiave, p] : *ris) {
                ic[chiave] = p;
                iconeChieste.erase(chiaveIcona(p, 96));
                for (auto& g : tuttiGiochi) {
                    if (chiaveGioco(g) == chiave) {
                        if (g.icona) { g.icona->Release(); g.icona = nullptr; }
                        g.iconaFile = p;
                        g.iconaPers = true;
                        g.provata = true;
                    }
                }
                chiediIcona(p, 96);
            }
            salvaIconeGiochi(ic);
            lDirty = true;
        });
}

inline auto Motore::menuGioco(int i, POINT pt) -> void {
    if (i < 0 || i >= (int)filtrate.size()) return;
    const AppL a = tuttiGiochi[filtrate[i]];
    const bool isExe = finisceCon(a.avvio, L".exe");
    const bool isLnk = finisceCon(a.avvio, L".lnk");
    const bool isUri = a.avvio.find(L"://") != std::wstring::npos;
    const bool hasFile = esisteFile(a.avvio);
    const bool hasCartella = !a.cartella.empty() && esisteCartella(a.cartella);

    std::vector<menu::Voce> vv = {
        menu::titolo(a.nome),
        menu::voce(testi::t(L"Gioca", L"Play"), 1)
    };
    if (a.pidInEsecuzione != 0)
        vv.push_back(menu::voce(testi::t(L"Chiudi gioco", L"Close game"), 13));
    vv.push_back(menu::voce(
        a.preferito ? testi::t(L"Rimuovi dai preferiti", L"Remove from Favorites")
                    : testi::t(L"Aggiungi ai preferiti", L"Pin to Favorites"), 11));
    if (isExe || isLnk || isUri)
        vv.push_back(menu::voce(testi::t(L"Aggiungi al dock", L"Add to dock"), 2));
    if (hasCartella || hasFile)
        vv.push_back(menu::voce(testi::t(L"Apri percorso", L"Open folder"), 3));
    vv.push_back(menu::voce(testi::t(L"Cambia icona…", L"Change icon…"), 6));
    vv.push_back(menu::voce(testi::t(L"Scarica icona online…", L"Fetch icon online…"), 10));
    if (a.iconaPers)
        vv.push_back(menu::voce(testi::t(L"Ripristina icona", L"Reset icon"), 7));
    vv.push_back(menu::voce(testi::t(L"Rinomina…", L"Rename…"), 8));
    if (a.nomePers)
        vv.push_back(menu::voce(testi::t(L"Ripristina nome", L"Reset name"), 9));
    vv.push_back(menu::voce(testi::t(L"Proprietà…", L"Properties…"), 12));
    vv.push_back(menu::separatore());
    if (a.manuale)
        vv.push_back(menu::voce(testi::t(L"Rimuovi dai giochi", L"Remove from Games"), 4));
    else
        vv.push_back(menu::voce(testi::t(L"Nascondi dai giochi", L"Hide from Games"), 5));

    const UINT cmd = menu::mostra(vv, pt, launcher);
    if (cmd == 1) {
        primaDelLauncher = nullptr;
        chiudiLauncher();
        lanciaGioco(a);
    } else if (cmd == 13) {
        const bool forza = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        chiudiGiocoInEsecuzione(a, forza);
    } else if (cmd == 11) {
        alternaPreferitoGioco(a);
    } else if (cmd == 12) {
        opzioniGioco(a);
    } else if (cmd == 2) {
        if (isExe) {
            aggiungiAlDock(a.nome, a.avvio, L"", a.avvio, a.iconaFile, a.argomenti);
        } else if (isLnk) {
            std::wstring exe, id;
            IShellLinkW* sl = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
                IPersistFile* pf = nullptr;
                if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Load(a.avvio.c_str(), STGM_READ))) {
                    wchar_t dest[MAX_PATH] = L"";
                    if (SUCCEEDED(sl->GetPath(dest, MAX_PATH, nullptr, SLGP_RAWPATH)) && dest[0]) {
                        wchar_t esp[MAX_PATH] = L"";
                        ExpandEnvironmentStringsW(dest, esp, MAX_PATH);
                        exe = esp;
                    }
                    IPropertyStore* ps = nullptr;
                    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) {
                        PROPVARIANT pv; PropVariantInit(&pv);
                        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) id = pv.pwszVal;
                        PropVariantClear(&pv); ps->Release();
                    }
                }
                if (pf) pf->Release();
                sl->Release();
            }
            aggiungiAlDock(a.nome, exe, id, a.avvio, a.iconaFile, a.argomenti);
        } else if (isUri) {
            std::wstring icona = (finisceCon(a.iconaFile, L".ico") || finisceCon(a.iconaFile, L".exe")) ? a.iconaFile : L"";
            if (icona.empty() && !a.cartella.empty()) icona = trovaExeGioco(a.cartella);
            aggiungiAlDock(a.nome, L"", L"", a.avvio, icona, a.argomenti);
        }
    } else if (cmd == 3) {
        if (hasFile) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + a.avvio + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else if (hasCartella) ShellExecuteW(nullptr, L"open", a.cartella.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == 4) {
        auto el = leggiGiochiAggiunti();
        const std::wstring chiave = chiaveGioco(a);
        el.erase(std::remove_if(el.begin(), el.end(), [&](const auto& kv) {
            if (minuscolo(kv.second) == chiave) return true;
            if (finisceCon(kv.second, L".url") && minuscolo(kv.first) == a.minuscolo) {
                wchar_t u[2048] = L"";
                GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", u, 2048, kv.second.c_str());
                return minuscolo(u[0] ? std::wstring(u) : kv.second) == chiave;
            }
            return false;
        }), el.end());
        salvaGiochiAggiunti(el);
        auto nm = leggiNomiGiochi();
        if (nm.erase(chiave) > 0) salvaNomiGiochi(nm);
        auto pf = leggiGiochiPreferiti();
        if (pf.erase(chiave) > 0) salvaGiochiPreferiti(pf);
        auto rc = leggiGiochiRecenti();
        if (rc.erase(chiave) > 0) salvaGiochiRecenti(rc);
        auto op = leggiOpzioniGiochi();
        if (op.erase(chiave) > 0) salvaOpzioniGiochi(op);
        leggiTuttiGiochi(true);
    } else if (cmd == 5) {
        auto n = leggiGiochiNascosti();
        n.insert(chiaveGioco(a));
        salvaGiochiNascosti(n);
        leggiTuttiGiochi(true);
    } else if (cmd == 6) {
        cambiaIconaGioco(a);
    } else if (cmd == 7) {
        const std::wstring chiave = chiaveGioco(a);
        auto ic = leggiIconeGiochi();
        ic.erase(chiave);
        ic.erase(a.minuscolo);
        salvaIconeGiochi(ic);
        for (auto& g : tuttiGiochi) {
            if (chiaveGioco(g) == chiave) {
                if (g.icona) { g.icona->Release(); g.icona = nullptr; }
                g.provata = false;
            }
        }
        leggiTuttiGiochi(true);
    } else if (cmd == 8) {
        rinominaGioco(a);
    } else if (cmd == 9) {
        auto nm = leggiNomiGiochi();
        nm.erase(chiaveGioco(a));
        salvaNomiGiochi(nm);
        leggiTuttiGiochi(true);
    } else if (cmd == 10) {
        scegliIconaOnline(a);
    }
}

inline auto Motore::menuSpazioGiochi(POINT pt) -> void {
    const auto nascosti = leggiGiochiNascosti();
    auto v4 = menu::voce(testi::t(L"Scarica icone online", L"Fetch icons online"), 4);
    v4.disattivata = lScaricaIconeInCorso;
    std::vector<menu::Voce> vv = {
        menu::voce(testi::t(L"Aggiungi gioco…", L"Add game shortcut…"), 1),
        menu::voce(testi::t(L"Aggiorna giochi", L"Refresh games"), 2),
        menu::separatore(),
        v4,
        menu::voce(testi::t(L"Chiave API SteamGridDB…", L"SteamGridDB API key…"), 5)
    };
    if (!nascosti.empty()) {
        vv.push_back(menu::separatore());
        vv.push_back(menu::voce(testi::t(L"Mostra giochi nascosti", L"Show hidden games"), 3));
    }
    const UINT cmd = menu::mostra(vv, pt, launcher);
    if (cmd == 1) aggiungiGiocoDialogo();
    else if (cmd == 2) leggiTuttiGiochi(true);
    else if (cmd == 3) { salvaGiochiNascosti({}); leggiTuttiGiochi(true); }
    else if (cmd == 4) scaricaTutteIconeOnline();
    else if (cmd == 5) impostaChiaveSteamGridDB();
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

inline auto Motore::disegnaIconaGiochi(const D2D1_RECT_F& r, float alfa, bool scuro) -> void {
    const float d = r.right - r.left;
    pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.16f * alfa) : D2D1::ColorF(1, 1, 1, 0.82f * alfa));
    dc->FillRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello);
    pennello->SetColor(D2D1::ColorF(0, 0, 0, (scuro ? 0.3f : 0.08f) * alfa));
    dc->DrawRoundedRectangle({r, d * 0.24f, d * 0.24f}, pennello, 1.0f);

    const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const float u = d * 0.01f;
    auto P = [&](float x, float y) { return D2D1::Point2F(cx + x * u, cy + y * u); };

    const D2D1_GRADIENT_STOP fermate[2] = {
        {0, D2D1::ColorF(0.72f, 0.33f, 0.97f, alfa)},
        {1, D2D1::ColorF(0.37f, 0.36f, 0.90f, alfa)}
    };
    ID2D1GradientStopCollection* fc = nullptr;
    ID2D1LinearGradientBrush* sf = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(fermate, 2, &fc)))
        dc->CreateLinearGradientBrush({P(-30, -16), P(30, 16)}, fc, &sf);
    if (!sf) pennello->SetColor(D2D1::ColorF(0.55f, 0.35f, 0.94f, alfa));

    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f2d->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g->Open(&s))) {
            s->BeginFigure(P(-15, -16), D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(P(15, -16));
            s->AddBezier({P(23, -16), P(27, -10), P(30, 2)});
            s->AddBezier({P(33, 13), P(26, 18), P(20, 15)});
            s->AddBezier({P(15, 12), P(12, 6), P(8, 6)});
            s->AddLine(P(-8, 6));
            s->AddBezier({P(-12, 6), P(-15, 12), P(-20, 15)});
            s->AddBezier({P(-26, 18), P(-33, 13), P(-30, 2)});
            s->AddBezier({P(-27, -10), P(-23, -16), P(-15, -16)});
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            s->Release();
            dc->FillGeometry(g, sf ? (ID2D1Brush*)sf : (ID2D1Brush*)pennello);
        }
        g->Release();
    }
    if (sf) sf->Release();
    if (fc) fc->Release();

    pennello->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * alfa));
    const float lx = cx - 13.5f * u, ly = cy - 4.5f * u;
    dc->FillRoundedRectangle({{lx - 1.6f * u, ly - 5.0f * u, lx + 1.6f * u, ly + 5.0f * u}, 0.8f * u, 0.8f * u}, pennello);
    dc->FillRoundedRectangle({{lx - 5.0f * u, ly - 1.6f * u, lx + 5.0f * u, ly + 1.6f * u}, 0.8f * u, 0.8f * u}, pennello);

    const float rx = cx + 13.5f * u, ry = cy - 4.5f * u, off = 4.2f * u, rad = 1.75f * u;
    dc->FillEllipse({{rx, ry - off}, rad, rad}, pennello);
    dc->FillEllipse({{rx, ry + off}, rad, rad}, pennello);
    dc->FillEllipse({{rx - off, ry}, rad, rad}, pennello);
    dc->FillEllipse({{rx + off, ry}, rad, rad}, pennello);
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
    // sezione dei file: l'altezza insegue quella giusta (compare e sparisce scorrendo, la griglia si accorcia con lei)
    {
        const double fh = verso(lFileH, lFileAlto(), 0.07);
        if (fh != lFileH) {
            lFileH = fh; anim = true;
            // La griglia si accorcia: l'app scelta con le frecce resta in vista (R20261119-195000-S4; prima solo il
            // limite dello scorrimento, e poteva finire sotto la sezione). Posizione assoluta, calcolata sull'obiettivo
            // dello scorrimento e non sulla posizione animata: chiamata a ogni fotogramma, selezionaVisibile sommava
            // ogni volta il ritardo dell'animazione, e lo scorrimento andava oltre e rimbalzava (R20261119-195720-S2).
            // Solo per una scelta fatta con la tastiera: rotella e barra hanno la precedenza (R20261119-195720-S1)
            if (lSelTastiera && !query.empty() && lSelFile < 0 && lSel >= 0 && lSel < (int)filtrate.size()) {
                const int numeroRiga = lSel / 6;   // (divisione intera voluta: la riga della griglia, 6 app per riga)
                const double alto = lGrigliaBottom() - lGrigliaTop(), riga = numeroRiga * (double)lCellaH();
                lScrollObj = std::max(riga + lCellaH() - alto, std::min(lScrollObj, riga));   // (non clamp: con una griglia piu' bassa di una riga il minimo supererebbe il massimo)
            }
            lScrollObj = std::clamp(lScrollObj, 0.0, lScrollMax());
        }
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
        if (lModoGiochi) {
            linea(-6, 0, 6, 0, cx, cy, 2.0f);
            linea(0, -6, 0, 6, cx, cy, 2.0f);
        } else {
            simboloEnergia(cx, cy);
        }
    }
    if (lModoGiochi) {
        const D2D1_RECT_F ro = rettOrdineGiochi();
        const float pr = (ro.bottom - ro.top) * 0.5f;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, lSopraOrdine ? 0.18f : 0.10f) : D2D1::ColorF(1, 1, 1, lSopraOrdine ? 0.85f : 0.6f));
        dc->FillRoundedRectangle({ro, pr, pr}, pennello);
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, lSopraOrdine ? 0.24f : 0.14f) : D2D1::ColorF(0, 0, 0, 0.08f));
        dc->DrawRoundedRectangle({ro, pr, pr}, pennello, 1.0f);
        pennello->SetColor(ink);
        scrivi(lOrdineRecenti ? testi::t(L"Recenti", L"Recent") : L"A-Z", testo, ro);
    }
    // campo di ricerca
    const D2D1_RECT_F rc = {24 * dpi, 22 * dpi, (lModoGiochi ? rettOrdineGiochi().left : rettEnergia().left) - 10 * dpi, 62 * dpi};
    pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.6f));
    dc->FillRoundedRectangle({rc, 10 * dpi, 10 * dpi}, pennello);
    pennello->SetColor({ink.r, ink.g, ink.b, 0.55f});
    const float lx = rc.left + 22 * dpi, ly = (rc.top + rc.bottom) / 2;
    dc->DrawEllipse({{lx - 2 * dpi, ly - 2 * dpi}, 6 * dpi, 6 * dpi}, pennello, 1.8f * dpi);
    linea(2.5f, 2.5f, 7, 7, lx, ly);
    const D2D1_RECT_F rt = {lx + 18 * dpi, rc.top, rc.right - 14 * dpi, rc.bottom};
    if (query.empty()) scrivi(lModoGiochi ? testi::t(L"Cerca un gioco", L"Search for a game") : testi::t(L"Cerca un'app", L"Search for an app"), fmtTitolo, rt);
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
    auto& el = elencoAttivo();
    for (int i = 0; i < (int)filtrate.size(); i++) {
        const auto r = lCella(i);
        if (r.bottom < lGrigliaTop() - lCellaH() || r.top > lGrigliaBottom() + lCellaH()) continue;
        AppL& a = el[filtrate[i]];
        const bool sopra = i == lSopra, scelta = i == lSel && !query.empty() && lSelFile < 0;   // (con una riga dei file scelta, solo quella)
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
        if (lModoGiochi && a.preferito) {
            const float sx = ri.left + 6 * dpi, sy = ri.top + 6 * dpi;
            const float rOut = 6.2f * dpi, rIn = 2.7f * dpi;
            pennello->SetColor(D2D1::ColorF(0, 0, 0, 0.42f));
            dc->FillEllipse({{sx, sy}, rOut + 1.4f * dpi, rOut + 1.4f * dpi}, pennello);
            ID2D1PathGeometry* stella = nullptr;
            if (SUCCEEDED(f2d->CreatePathGeometry(&stella)) && stella) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(stella->Open(&sink)) && sink) {
                    const float pi = 3.14159265f;
                    for (int k = 0; k < 10; k++) {
                        const float ang = -pi * 0.5f + k * (pi / 5.0f);
                        const float rad = (k % 2 == 0) ? rOut : rIn;
                        const D2D1_POINT_2F pt = D2D1::Point2F(sx + std::cos(ang) * rad, sy + std::sin(ang) * rad);
                        if (k == 0) sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED);
                        else sink->AddLine(pt);
                    }
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    sink->Release();
                    pennello->SetColor(D2D1::ColorF(1.0f, 0.78f, 0.16f, 0.98f));
                    dc->FillGeometry(stella, pennello);
                }
                stella->Release();
            }
        }
        if (lModoGiochi && a.pidInEsecuzione != 0) {
            const float rx = ri.right - 5 * dpi, ry = ri.top + 5 * dpi;
            pennello->SetColor(D2D1::ColorF(0.18f, 0.85f, 0.40f, 0.34f));
            dc->FillEllipse({{rx, ry}, 7.0f * dpi, 7.0f * dpi}, pennello);
            pennello->SetColor(D2D1::ColorF(0.18f, 0.86f, 0.42f, 1.0f));
            dc->FillEllipse({{rx, ry}, 4.5f * dpi, 4.5f * dpi}, pennello);
            pennello->SetColor(scuro ? D2D1::ColorF(0.08f, 0.10f, 0.12f, 0.85f) : D2D1::ColorF(1, 1, 1, 0.92f));
            dc->DrawEllipse({{rx, ry}, 4.5f * dpi, 4.5f * dpi}, pennello, 1.2f * dpi);
        }
        pennello->SetColor(ink);
        scrivi(a.nome, fmtNome, {r.left + 6 * dpi, ri.bottom + 6 * dpi, r.right - 6 * dpi, ri.bottom + 24 * dpi});
    }
    if (filtrate.empty()) {
        pennello->SetColor({ink.r, ink.g, ink.b, 0.5f});
        scrivi(lModoGiochi ? testi::t(L"Nessun gioco trovato · clicca + o trascina qui un gioco", L"No games found · click + or drop a game here")
                           : testi::t(L"Nessuna app trovata", L"No apps found"),
               fmtNome, {0, lGrigliaTop() + 40 * dpi, LW, lGrigliaTop() + 70 * dpi});
    }
    dc->PopAxisAlignedClip();
    if (disegnaFileLauncher(scuro, ink)) anim = true;
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
                    d->lScrollObj = std::clamp(d->lScrollObj + (y < c.top ? -pagina : pagina), 0.0, d->lScrollMax()); d->lSelTastiera = false;
                } else {
                    d->lTrascinoBarra = true; d->lPresaY = y; d->lPresaScroll = d->lScroll; d->lSelTastiera = false;
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
            const int sf = d->lFileIn(x, y);
            if (sf != d->lSopraFile) { d->lSopraFile = sf; d->lDirty = true; }
            const D2D1_RECT_F re = d->rettEnergia();
            const bool e = x >= re.left && x < re.right && y >= re.top && y < re.bottom;
            if (e != d->lSopraEnergia) { d->lSopraEnergia = e; d->lDirty = true; }
            const D2D1_RECT_F ro = d->rettOrdineGiochi();
            const bool o = d->lModoGiochi && x >= ro.left && x < ro.right && y >= ro.top && y < ro.bottom;
            if (o != d->lSopraOrdine) { d->lSopraOrdine = o; d->lDirty = true; }
            return 0;
        }
        case WM_MOUSELEAVE: d->lMouseDentro = false; d->lSopra = -1; d->lSopraFile = -1; d->lSopraEnergia = false; d->lSopraOrdine = false; d->lSopraBarra = false; d->lDirty = true; return 0;
        case WM_TIMER:
            if (w == TIMER_FILE_L) { KillTimer(h, TIMER_FILE_L); d->cercaFileLauncher(); return 0; }
            break;
        case WM_LBUTTONUP: {
            if (d->lTrascinoBarra) { d->lTrascinoBarra = false; ReleaseCapture(); d->lDirty = true; return 0; }
            { const D2D1_RECT_F b = d->lRettBarra(); if (d->lScrollMax() > 0 && x >= b.left && x < b.right && y >= b.top && y < b.bottom) return 0; }
            if (y > d->LH) { d->chiudiLauncher(); return 0; }   // clic sotto (durante l'animazione): chiude
            if (d->lModoGiochi) {
                const D2D1_RECT_F ro = d->rettOrdineGiochi();
                if (x >= ro.left && x < ro.right && y >= ro.top && y < ro.bottom) {
                    d->lOrdineRecenti = !d->lOrdineRecenti;
                    salvaOrdineRecenti(d->lOrdineRecenti);
                    d->ordinaGiochi();
                    d->filtra();
                    d->lDirty = true;
                    return 0;
                }
            }
            if (d->lSopraEnergia) {
                if (d->lModoGiochi) { d->aggiungiGiocoDialogo(); return 0; }
                POINT p{(LONG)x, (LONG)y}; ClientToScreen(h, &p); d->menuEnergia(p); return 0;
            }
            const int i = d->lCellaIn(x, y);
            if (i >= 0) d->avviaApp(i);
            else { const int k = d->lFileIn(x, y); if (k >= 0) d->apriFile(k); }
            return 0;
        }
        case WM_RBUTTONUP: {   // il menu di Windows dell'app (amministratore, percorso, disinstalla...)
            const int i = d->lCellaIn(x, y);
            POINT p{(LONG)x, (LONG)y}; ClientToScreen(h, &p);
            if (d->lModoGiochi) {
                if (i >= 0) d->menuGioco(i, p);
                else if (y < d->LH) d->menuSpazioGiochi(p);
            } else if (i >= 0) {
                d->menuApp(i, p);
            } else {
                const int k = d->lFileIn(x, y);
                if (k >= 0) d->menuFile(k, p);
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP hd = (HDROP)w;
            const UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            bool aggiunto = false;
            for (UINT i = 0; i < n; i++) {
                wchar_t p[MAX_PATH * 4] = L"";
                if (!DragQueryFileW(hd, i, p, MAX_PATH * 4)) continue;
                const std::wstring s = p;
                if (finisceCon(s, L".exe") || finisceCon(s, L".lnk") || finisceCon(s, L".url")) {
                    d->aggiungiGiocoFile(s);
                    aggiunto = true;
                }
            }
            DragFinish(hd);
            if (aggiunto) {
                if (!d->lModoGiochi) {
                    d->lModoGiochi = true;
                    float ax = d->W / 2.0f;
                    const auto pos = d->posizioniRiposo();
                    for (size_t i = 0; i < d->voci.size() && i < pos.size(); i++) if (d->voci[i].avvio == AVVIO_GIOCHI) ax = pos[i] + d->S / 2;
                    const RECT ic = d->aSchermo({ax - d->S / 2, d->yBaseRiposo() - d->S, ax + d->S / 2, d->yBaseRiposo()});
                    const float icx = (ic.left + ic.right) / 2.0f - d->rettLauncher.left, icy = (ic.top + ic.bottom) / 2.0f - d->rettLauncher.top;
                    switch (d->lato) {
                        case 1: d->ancora = {(LONG)std::clamp(icx, 40 * d->dpi, d->LW - 40 * d->dpi), 0}; break;
                        case 2: d->ancora = {0, (LONG)std::clamp(icy, 40 * d->dpi, d->LH - 40 * d->dpi)}; break;
                        case 3: d->ancora = {(LONG)d->LW, (LONG)std::clamp(icy, 40 * d->dpi, d->LH - 40 * d->dpi)}; break;
                        default: d->ancora = {(LONG)std::clamp(icx, 40 * d->dpi, d->LW - 40 * d->dpi), (LONG)std::min(d->LH + d->LEXTRA - 4, icy)};
                    }
                    d->query.clear(); d->lFile.clear(); d->lFileTesto.clear(); d->lFileGen++; d->lSelFile = -1; d->lSopraFile = -1; d->lFileH = 0;
                    d->lSopra = -1; d->lScroll = d->lScrollObj = 0;
                    d->dirty = true;
                }
                d->leggiTuttiGiochi(true);
                prendiFuoco(h);
            }
            return 0;
        }
        // sottomenu del menu di Windows ("Invia a", "Apri con"...): li disegna lui
        case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR:
            if (d->menuShell3) { LRESULT r = 0; if (SUCCEEDED(d->menuShell3->HandleMenuMsg2(m, w, l, &r))) return r; }
            else if (d->menuShell2) { if (SUCCEEDED(d->menuShell2->HandleMenuMsg(m, w, l))) return m == WM_INITMENUPOPUP ? 0 : TRUE; }
            break;
        case WM_MOUSEWHEEL:
            d->lSelTastiera = false;   // (la rotella ha la precedenza sulla scelta da tastiera)
            d->lScrollObj = std::clamp(d->lScrollObj - GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA * d->lCellaH() * 0.9, 0.0, d->lScrollMax());
            d->lDirty = true; return 0;
        case WM_CHAR:
            if (w == 27) d->chiudiLauncher();
            // Invio: con una ricerca la prima trovata (o quella scelta con le frecce); senza, quella scelta. Una riga dei
            // file scelta con le frecce: quella. Nessuna app trovata: il primo file, se i risultati sono del testo
            // scritto (altrimenti la ricerca completa in Esplora file, non un file di un testo vecchio)
            else if (w == 13) {
                if (d->lSelFile >= 0) d->apriFile(d->lSelFile);
                else if (!d->lModoGiochi && d->filtrate.empty() && !d->query.empty()) {
                    const int r = d->lFileRighe();
                    d->apriFile(r > 0 && d->lFileTesto == d->query ? 0 : r);
                }
                else d->avviaApp(!d->query.empty() || d->lSopra == d->lSel ? d->lSel : -1);
            }
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
            // Frecce su e giu' fra griglia e file: dall'ultima riga di app (o senza app) la freccia giu' entra nei file;
            // nei file su e giu' scorrono le righe, fino a "Cerca in Esplora file"; su dalla prima torna alla griglia.
            // Gli altri tasti di spostamento riportano nella griglia
            {
                const int r = d->lFileRighe();
                if (r >= 0 && (w == VK_UP || w == VK_DOWN)) {
                    // (con righe di un testo vecchio si sceglie solo "Cerca in Esplora file": R20261119-195000-S2)
                    const bool validi = d->lFileTesto == d->query;
                    if (d->lSelFile >= 0) {
                        if (w == VK_DOWN) d->lSelFile = validi ? std::min(d->lSelFile + 1, r) : r;
                        else if (d->lSelFile > 0 && validi) d->lSelFile--;
                        else if (!d->filtrate.empty()) d->lSelFile = -1;
                        d->lDirty = true;
                        return 0;
                    }
                    if (w == VK_DOWN && (d->filtrate.empty() || d->lSel + 6 >= (int)d->filtrate.size())) { d->lSelFile = validi ? 0 : r; d->lDirty = true; return 0; }
                }
                if (d->lSelFile >= 0 && (w == VK_LEFT || w == VK_RIGHT || w == VK_PRIOR || w == VK_NEXT || w == VK_HOME || w == VK_END)) {
                    d->lSelFile = -1;
                    if (d->filtrate.empty()) return 0;
                }
            }
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
            d->lSelTastiera = true; d->selezionaVisibile(); d->lDirty = true;
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) {
                if (d->lDialogoAperto) return 0;
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
