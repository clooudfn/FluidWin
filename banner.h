// Banner delle notifiche di FluidWin: quando arriva una notifica, sopra l'isola sale un banner di vetro con icona,
// app, titolo e testo; resta qualche secondo (fermo finche' c'e' sopra il mouse), clic per aprire l'app, la x per
// toglierla. Le notifiche si leggono con l'API ufficiale (notifiche.h), ogni secondo e mezzo, in un thread a parte.
//
// Per non vedere due banner, quelli di Windows si spengono app per app con la stessa opzione delle Impostazioni
// ("Mostra banner di notifica", valore ShowBanner in HKCU\...\Notifications\Settings\<app>). Il valore di prima si
// salva in HKCU\Software\FluidWin\Banner e si rimette com'era quando l'opzione si spegne. Le notifiche restano
// nel centro notifiche e nel pannello dell'isola: cambia solo chi le mostra a comparsa.
// Windows legge ShowBanner solo all'accesso (provato il 12/11/2026 su 10.0.26300: valore a 0 scritto con Windows
// acceso, il banner di Windows esce lo stesso). Per questo i valori restano spenti finche' l'opzione e' accesa, anche
// a FluidWin chiuso, allo spegnimento del PC e dopo un crash: rimettendoli a ogni uscita (correzione 84) all'accesso
// Windows li trovava accesi e le notifiche uscivano doppie. Si rimettono solo spegnendo l'opzione (accendi(false), e
// all'avvio se e' spenta); vale dal prossimo accesso.
#pragma once
#include "vetro.h"
#include "notifiche.h"
#include <windowsx.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <vector>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <roapi.h>
#include "testi.h"

namespace banner {

inline const wchar_t* IMPOST = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\";
inline const wchar_t* COPIA = L"Software\\FluidWin\\Banner";

// spegne il banner di Windows per questa app, ricordando com'era (una volta sola per app). ShowBanner si cambia solo
// se la copia del valore di prima e' stata scritta: senza copia non si saprebbe piu' come rimetterlo (audit FW-AUDIT-009)
// Si va avanti solo su esiti certi: copia assente (ERROR_FILE_NOT_FOUND) e ShowBanner DWORD o assente. Una copia
// illeggibile o di un altro tipo, o un ShowBanner illeggibile o non DWORD, lasciano tutto com'e': prima ogni errore di
// lettura valeva "assente" e poteva sovrascrivere la copia buona o cancellare un valore originale (revisione di Codex
// R20261112-190918-S1)
inline void spegniBannerWindows(const std::wstring& aumid) {
    if (aumid.empty()) return;
    DWORD x = 0, n = sizeof(x);
    const LSTATUS ec = RegGetValueW(HKEY_CURRENT_USER, COPIA, aumid.c_str(), RRF_RT_REG_DWORD, nullptr, &x, &n);
    if (ec != ERROR_FILE_NOT_FOUND) return;   // gia' fatto (ERROR_SUCCESS), o copia che non si sa leggere: non si tocca
    const std::wstring k = IMPOST + aumid;
    DWORD prima = 0xFFFFFFFF; n = sizeof(prima);   // 0xFFFFFFFF = il valore non c'era
    const LSTATUS es = RegGetValueW(HKEY_CURRENT_USER, k.c_str(), L"ShowBanner", RRF_RT_REG_DWORD, nullptr, &prima, &n);
    if (es == ERROR_FILE_NOT_FOUND) prima = 0xFFFFFFFF;
    else if (es != ERROR_SUCCESS) return;   // ShowBanner illeggibile o non DWORD: non e' nostro da cambiare
    if (RegSetKeyValueW(HKEY_CURRENT_USER, COPIA, aumid.c_str(), REG_DWORD, &prima, sizeof(prima)) != ERROR_SUCCESS) return;
    DWORD zero = 0; RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), L"ShowBanner", REG_DWORD, &zero, sizeof(zero));
}

// notifiche di sistema e di sicurezza (Defender, Sicurezza di Windows, centro operativo): restano a Windows, che le
// mostra col suo banner come sempre; FluidWin non le spegne e non le doppia
inline bool diSistema(const std::wstring& aumid) {
    for (const wchar_t* p : {L"Windows.SystemToast", L"Defender", L"SecHealthUI", L"SecurityAndMaintenance"})
        if (aumid.find(p) != std::wstring::npos) return true;
    return false;
}

// spegne in anticipo i banner di Windows per tutte le app che hanno gia' mandato notifiche (le sottochiavi di
// Notifications\Settings): senza, la prima notifica di ogni app compariva due volte, una di Windows e una di FluidWin
// Gli AUMID con un percorso (PowerShell: "{GUID}\WindowsPowerShell\v1.0\powershell.exe") nel registro sono chiavi
// annidate: si scende finche' si trova la chiave dell'app (con dei valori, o senza sottochiavi). Prima si leggeva solo
// il primo livello e ShowBanner finiva su "{GUID}", che non e' un'app: il banner restava acceso fino alla sua prima
// notifica letta da FluidWin
inline void appSotto(const std::wstring& rel, std::vector<std::wstring>& app, int livello) {
    HKEY c = nullptr;
    std::wstring radice = IMPOST; radice.pop_back();
    std::wstring percorso = radice;
    if (!rel.empty()) { percorso += L'\\'; percorso += rel; }
    if (RegOpenKeyExW(HKEY_CURRENT_USER, percorso.c_str(), 0, KEY_READ, &c) != ERROR_SUCCESS) return;
    DWORD sotto = 0, valori = 0;
    RegQueryInfoKeyW(c, nullptr, nullptr, nullptr, &sotto, nullptr, nullptr, &valori, nullptr, nullptr, nullptr, nullptr);
    std::vector<std::wstring> figli;
    for (DWORD i = 0; i < sotto; i++) {
        wchar_t nome[512]; DWORD ln = 512;
        if (RegEnumKeyExW(c, i, nome, &ln, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        figli.push_back(nome);
    }
    RegCloseKey(c);
    if (!rel.empty() && (valori > 0 || figli.empty())) app.push_back(rel);
    if (livello >= 6) return;   // (i percorsi veri sono corti)
    for (const auto& f : figli) {
        std::wstring p = rel;
        if (!p.empty()) p += L'\\';
        p += f;
        appSotto(p, app, livello + 1);
    }
}

inline void spegniBannerTutte() {
    std::vector<std::wstring> app;
    appSotto(L"", app, 0);
    for (const auto& a : app) if (!diSistema(a)) spegniBannerWindows(a);
}

// rimette i banner di Windows come erano, per tutte le app toccate. Torna quante copie restano (0 = tutto rimesso).
// Ogni copia si cancella solo dopo che il suo ripristino e' riuscito, e la chiave delle copie solo quando e' vuota:
// prima un errore a meta' (un nome lungo che non entrava nel buffer da 512, un valore non riscritto) fermava l'elenco e
// poi si cancellavano comunque tutte le copie, perdendo per sempre lo stato di partenza (audit FW-AUDIT-009)
// -1: la chiave delle copie c'e' ma non si apre (niente ripristinato: prima tornava 0, cioe' "tutto rimesso", R20261112-190918-S2)
inline int ripristinaBannerWindows() {
    HKEY c = nullptr;
    const LSTATUS eo = RegOpenKeyExW(HKEY_CURRENT_USER, COPIA, 0, KEY_READ | KEY_SET_VALUE, &c);
    if (eo == ERROR_FILE_NOT_FOUND) return 0;   // nessuna copia: niente da rimettere
    if (eo != ERROR_SUCCESS) return -1;
    std::vector<std::pair<std::wstring, DWORD>> lista;
    int altre = 0;   // valori che non sono copie leggibili: restano dove sono
    std::vector<wchar_t> nome(16384);   // (la misura massima di un nome di valore nel registro)
    for (DWORD i = 0;; i++) {
        DWORD ln = (DWORD)nome.size(), tipo = 0, val = 0, lv = sizeof(val);
        const LSTATUS e = RegEnumValueW(c, i, nome.data(), &ln, nullptr, &tipo, (BYTE*)&val, &lv);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS || tipo != REG_DWORD || lv != sizeof(val)) { altre++; if (e != ERROR_SUCCESS && e != ERROR_MORE_DATA) break; continue; }
        lista.push_back({std::wstring(nome.data(), ln), val});
    }
    int restano = altre;
    for (const auto& [aumid, prima] : lista) {
        const std::wstring k = IMPOST + aumid;
        LSTATUS e;
        if (prima == 0xFFFFFFFF) { e = RegDeleteKeyValueW(HKEY_CURRENT_USER, k.c_str(), L"ShowBanner"); if (e == ERROR_FILE_NOT_FOUND) e = ERROR_SUCCESS; }
        else e = RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), L"ShowBanner", REG_DWORD, &prima, sizeof(prima));
        if (e == ERROR_SUCCESS && RegDeleteValueW(c, aumid.c_str()) == ERROR_SUCCESS) continue;
        restano++;   // la copia resta: ci si riprova la prossima volta
    }
    DWORD valori = 1, sotto = 1;
    const bool vuota = RegQueryInfoKeyW(c, nullptr, nullptr, nullptr, &sotto, nullptr, nullptr, &valori, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && !valori && !sotto;
    RegCloseKey(c);
    if (vuota) RegDeleteKeyW(HKEY_CURRENT_USER, COPIA);
    return restano;
}

// icona (64 px) in pixel BGRA premoltiplicati di un elemento della shell (un .exe, shell:AppsFolder\...); false se non c'e'
inline bool iconaDa(const std::wstring& nome, std::vector<UINT>& px, int& w, int& h) {
    px.clear(); w = h = 0;
    if (nome.empty()) return false;
    IShellItemImageFactory* fab = nullptr;
    if (FAILED(SHCreateItemFromParsingName(nome.c_str(), nullptr, IID_PPV_ARGS(&fab)))) return false;
    HBITMAP hb = nullptr;
    if (SUCCEEDED(fab->GetImage({64, 64}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
        BITMAP bm{}; GetObject(hb, sizeof(bm), &bm);
        if (bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 1024 && bm.bmHeight <= 1024) {
            px.resize((size_t)bm.bmWidth * bm.bmHeight);
            BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
            HDC scr = GetDC(nullptr); GetDIBits(scr, hb, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, scr);
            bool alfa = false; for (UINT p : px) if (p >> 24) { alfa = true; break; }
            for (UINT& p : px) {
                const UINT a = alfa ? (p >> 24) : 255;
                p = (a << 24) | ((((p >> 16) & 255) * a / 255) << 16) | ((((p >> 8) & 255) * a / 255) << 8) | ((p & 255) * a / 255);
            }
            w = bm.bmWidth; h = bm.bmHeight;
        }
        DeleteObject(hb);
    }
    fab->Release();
    return !px.empty();
}

// icona dell'app di una notifica dal suo AppUserModelID
inline bool iconaApp(const std::wstring& aumid, std::vector<UINT>& px, int& w, int& h) {
    return !aumid.empty() && iconaDa(L"shell:AppsFolder\\" + aumid, px, w, h);
}

class Banner {
public:
    // eseguibile dell'app di una notifica, se il dock la conosce (aumid, nome): da li' l'icona vera. Discord e altre
    // app installate "a mano" hanno un AppUserModelID che la shell non sa risolvere: l'icona era quella generica
    std::function<std::wstring(const std::wstring&, const std::wstring&)> exeApp;
    bool iconaPer(const notifiche::Notifica& n, std::vector<UINT>& px, int& w, int& h) {
        const std::wstring exe = exeApp ? exeApp(n.aumid, n.app) : L"";
        return (!exe.empty() && iconaDa(exe, px, w, h)) || iconaApp(n.aumid, px, w, h);
    }
    std::function<RECT()> isola;              // dove sta l'isola (il banner sale da sopra di lei)
    std::function<bool()> silenzio;           // vero con un gioco o un video a tutto schermo: niente banner sopra
    // mostra la notifica nel notch (false se il notch non c'e': allora il banner di vetro); quando il notch ha
    // finito chiama notchFinito()
    std::function<bool(const notifiche::Notifica&, const std::vector<UINT>&, int, int)> nelNotch;
    std::function<bool()> notchOccupato;   // il notch sta ancora mostrando la notifica che gli abbiamo dato
    // quante notifiche ha ogni app (AppUserModelID minuscolo), a ogni lettura: i numeri sulle icone del dock
    std::function<void(const std::unordered_map<std::wstring, int>&)> conteggi;
    bool attivo = false;
    std::function<void(const char*)> registro;   // righe per il registro (diagnosi)
    int statoOra() const { return stato; }
    size_t inCoda() const { return coda.size(); }
    ULONGLONG ultimaLettura() const { return tLettura; }

    // dal notch: la notifica mostrata e' sparita (si passa alla prossima), clic per aprirla, x per toglierla
    void notchFinito() { if (stato == 4) { stato = 0; if (attivo && !coda.empty()) entra(); } }
    void apriAttuale() {
        if (!attuale.aumid.empty()) ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + attuale.aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        lettore.rimuovi(attuale.id);
    }
    void togliAttuale() { lettore.rimuovi(attuale.id); }
    // all'uscita, dal thread principale (quello che usa 'lettore') e prima della fine di COM (R20261113-010114-S5)
    void chiudiLettore() { lettore.chiudi(); }

    ~Banner() { fermaLettura(); }

    void accendi(bool si) {
        if (si == attivo) return;
        attivo = si;
        if (si) {
            if (!v.fin && !v.crea(L"FluidWin.Banner", vetro::procedura<Banner>, this, false)) { attivo = false; return; }
            primaLettura = true;
            spegniBannerTutte(); tScansione = GetTickCount64();
            avviaLettura();
            SetTimer(v.fin, 2, 250, nullptr);
        } else {
            if (v.fin) { KillTimer(v.fin, 2); nascondi(); }
            fermaLettura();
            coda.clear();
            const int restano = ripristinaBannerWindows();
            if (restano && registro) {
                char t[120];
                if (restano < 0) snprintf(t, sizeof t, "banner di Windows: chiave delle copie non apribile, niente ripristinato");
                else snprintf(t, sizeof t, "banner di Windows: %d copie non ripristinate, restano per il prossimo giro", restano);
                registro(t);
            }
        }
    }

    bool messaggio(HWND, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
        r = 0;
        switch (m) {
            case WM_TIMER:
                if (w == 1) passo();
                if (w == 2) controlla();
                return true;
            case WM_PAINT: disegna(); return true;
            case WM_MOUSEACTIVATE: r = MA_NOACTIVATE; return true;
            case WM_CLOSE: nascondi(); return true;
            case WM_MOUSEMOVE: {
                if (!dentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, v.fin, 0}; TrackMouseEvent(&tm); dentro = true; disegna(); }
                const bool x = sullaX(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                if (x != sopraX) { sopraX = x; disegna(); }
                return true;
            }
            case WM_MOUSELEAVE: dentro = false; sopraX = false; tFine = vetro::ms() + 2500; disegna(); return true;
            case WM_LBUTTONUP:
                if (stato == 0) return true;
                if (sullaX(GET_X_LPARAM(l), GET_Y_LPARAM(l))) lettore.rimuovi(attuale.id);
                else {
                    if (!attuale.aumid.empty()) ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + attuale.aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    lettore.rimuovi(attuale.id);
                }
                esci();
                return true;
        }
        return false;
    }

private:
    vetro::Finestra v;
    notifiche::Lettore lettore;
    std::unordered_set<UINT32> viste;
    std::deque<notifiche::Notifica> coda;
    notifiche::Notifica attuale;
    ID2D1Bitmap* icona = nullptr;
    IDWriteTextFormat *fApp = nullptr, *fTitolo = nullptr, *fTesto = nullptr;
    bool primaLettura = true, dentro = false, sopraX = false;
    int stato = 0;   // 0 nascosto, 1 entra, 2 fermo, 3 esce, 4 la mostra il notch
    ULONGLONG t0 = 0, tFine = 0;
    double k = 0;
    int x = 0, y = 0;
    ULONGLONG tScansione = 0;

    // La lettura delle notifiche aspetta Windows (decine di millisecondi): la fa un thread a parte, ogni secondo e
    // mezzo, cosi' il thread principale (che disegna le animazioni) non si ferma mai
    std::thread lavoro;
    std::atomic<bool> basta{false};
    std::mutex mx;
    std::vector<notifiche::Notifica> letta;
    bool nuova = false;
    std::atomic<ULONGLONG> tLettura{0};   // ultima lettura riuscita (il thread di lettura e' vivo)

    void avviaLettura() {
        if (lavoro.joinable()) return;
        basta = false;
        lavoro = std::thread([this] {
            RoInitialize(RO_INIT_MULTITHREADED);
            {
                notifiche::Lettore l;
                while (!basta) {
                    std::vector<notifiche::Notifica> lista;
                    if (l.leggi(lista)) { std::lock_guard<std::mutex> g(mx); letta = std::move(lista); nuova = true; tLettura = GetTickCount64(); }
                    for (int i = 0; i < 15 && !basta; i++) Sleep(100);
                }
                l.chiudi();   // prima di RoUninitialize (FW-AUDIT-011)
            }
            RoUninitialize();
        });
    }

    void fermaLettura() {
        basta = true;
        if (lavoro.joinable()) lavoro.join();
        nuova = false;
    }

    void controlla() {
        sblocca();
        std::vector<notifiche::Notifica> lista;
        {
            std::lock_guard<std::mutex> g(mx);
            if (!nuova) return;
            lista = std::move(letta); nuova = false;
        }
        // le app nuove (installate o che notificano per la prima volta) ogni mezzo minuto
        if (GetTickCount64() - tScansione > 30000) { spegniBannerTutte(); tScansione = GetTickCount64(); }
        // sopra un gioco o un video a tutto schermo un banner in primo piano lo fa uscire dal tutto schermo (o lo
        // riduce a icona): li' le notifiche non compaiono, restano nel centro notifiche come fa Windows
        const bool zitto = silenzio && silenzio();
        if (conteggi) {
            std::unordered_map<std::wstring, int> c;
            for (const auto& n : lista) if (!n.aumid.empty()) { std::wstring k = n.aumid; for (auto& ch : k) ch = (wchar_t)towlower(ch); c[k]++; }
            conteggi(c);
        }
        std::unordered_set<UINT32> ora;
        for (auto it = lista.rbegin(); it != lista.rend(); ++it) {   // dalla piu' vecchia: in coda nell'ordine di arrivo
            const auto& n = *it;
            ora.insert(n.id);
            if (diSistema(n.aumid)) continue;   // le mostra Windows
            spegniBannerWindows(n.aumid);   // da qui in poi le notifiche di quest'app le mostra FluidWin
            // alla prima lettura quelle gia' presenti non si mostrano
            if (!primaLettura && !zitto && !viste.count(n.id)) {
                coda.push_back(n);
                if (registro) { char t[160]; snprintf(t, sizeof t, "notifica nuova (id %u), stato %d, in coda %zu", n.id, stato, coda.size()); registro(t); }
            }
        }
        viste = std::move(ora);
        primaLettura = false;
        if (zitto) { coda.clear(); if (stato != 0 && stato != 4) nascondi(); return; }
        if (stato == 0 && !coda.empty()) entra();
    }

    // Sicurezze: se la notifica in mostra resta "in corso" senza esserlo, la coda si fermava per sempre (le notifiche
    // smettevano di uscire finche' FluidWin non veniva riavviato). Il notch che non la mostra piu', il banner di vetro
    // senza animazione da piu' di 5 s o fermo da molto oltre la sua scadenza: si passa alla prossima
    void sblocca() {
        const ULONGLONG ora = vetro::ms();
        const char* perche = nullptr;
        if (stato == 4) { if (notchOccupato && !notchOccupato()) perche = "il notch non la mostra piu'"; }
        else if (stato == 1 || stato == 3) { if (ora - t0 > 5000) perche = "animazione del banner ferma"; }
        else if (stato == 2) { if (!IsWindowVisible(v.fin)) perche = "banner nascosto"; else if (ora > tFine + 60000) perche = "banner fermo da un minuto"; }
        if (!perche) return;
        if (registro) { char t[120]; snprintf(t, sizeof t, "notifiche: sbloccate (stato %d: %s)", stato, perche); registro(t); }
        if (stato == 4) stato = 0; else { dentro = false; nascondi(); }
        if (attivo && !coda.empty()) entra();
    }

    void entra() {
        attuale = coda.front(); coda.pop_front();
        if (nelNotch) {   // dal notch, se si vede
            std::vector<UINT> px; int iw = 0, ih = 0;
            iconaPer(attuale, px, iw, ih);
            if (nelNotch(attuale, px, iw, ih)) { stato = 4; if (registro) registro("notifica: nel notch"); return; }
        }
        v.aggiornaTema();
        const RECT is = isola ? isola() : vetro::areaPrincipale(true);
        v.dpi = vetro::dpiMonitor(MonitorFromPoint({is.left, is.top}, MONITOR_DEFAULTTOPRIMARY));
        const float d = v.dpi;
        const int w = (int)std::lround(360 * d), h = (int)std::lround(84 * d);
        x = is.right - w; y = is.top - (int)std::lround(12 * d) - h;
        v.posiziona(x, y + (int)std::lround(16 * d), w, h, HWND_TOPMOST, SWP_SHOWWINDOW);
        if (!fApp) {
            fApp = v.formato(11.5f * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fTitolo = v.formato(13.5f * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fTesto = v.formato(12.5f * d);
        }
        if (icona) { icona->Release(); icona = nullptr; }
        caricaIcona();
        stato = 1; t0 = vetro::ms(); tFine = t0 + 5500; k = 0;
        if (registro) registro("notifica: banner di vetro");
        vetro::battito(v.fin, 1);
        disegna();
    }

    void esci() { if (stato == 1 || stato == 2) { stato = 3; t0 = vetro::ms(); } }

    void nascondi() { if (stato == 4) return; stato = 0; vetro::fermaBattito(v.fin, 1); ShowWindow(v.fin, SW_HIDE); }

    void passo() {
        const ULONGLONG ora = vetro::ms();
        if (stato == 1) { k = std::min(1.0, (ora - t0) / 260.0); if (k >= 1) stato = 2; }
        else if (stato == 2 && !dentro && ora >= tFine) esci();
        else if (stato == 3) {
            k = 1 - std::min(1.0, (ora - t0) / 220.0);
            if (k <= 0) { nascondi(); if (!coda.empty()) entra(); return; }
        }
        const double e = stato == 3 ? vetro::uscita(k) : vetro::molla(k);
        SetWindowPos(v.fin, HWND_TOPMOST, x, y + (int)std::lround((1 - e) * 16 * v.dpi), 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
        v.alfa(std::min(1.0, k * 1.6));
    }

    bool sullaX(int px, int py) const { const float d = v.dpi; return px > v.W - 34 * d && py < 34 * d; }

    void caricaIcona() {
        if (!v.rt) return;
        std::vector<UINT> px; int w = 0, h = 0;
        if (!iconaPer(attuale, px, w, h)) return;
        v.rt->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4,
                           D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &icona);
        giroIcona = v.giroRt;
    }
    unsigned giroIcona = 0;   // giro del render target con cui e' stata fatta l'icona (FW-AUDIT-012)

    void disegna() {
        if (stato == 0) return;
        if (icona && giroIcona != v.giroRt) { icona->Release(); icona = nullptr; caricaIcona(); }   // target ricreato: icona rifatta
        if (!v.inizia()) return;
        const float d = v.dpi, W = (float)v.W, H = (float)v.H;
        const D2D1_COLOR_F ink = v.inchiostro(), tenue = v.inchiostro(0.6f);
        const float il = 38 * d, ix = 16 * d, iy = (H - il) / 2;
        if (icona) v.rt->DrawBitmap(icona, {ix, iy, ix + il, iy + il});
        else {
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.15f) : D2D1::ColorF(0, 0, 0, 0.08f));
            v.rt->FillRoundedRectangle({{ix, iy, ix + il, iy + il}, 9 * d, 9 * d}, v.pen);
        }
        const float tx = ix + il + 14 * d, tr = W - 40 * d;
        v.testo(attuale.app.empty() ? std::wstring(testi::t(L"Notifica", L"Notification")) : attuale.app, fApp, {tx, 12 * d, tr, 28 * d}, tenue);
        v.testo(attuale.titolo, fTitolo, {tx, 28 * d, tr + 24 * d, 48 * d}, ink);
        v.testo(attuale.testo, fTesto, {tx, 48 * d, tr + 24 * d, 68 * d}, v.inchiostro(0.8f));
        if (dentro) {   // la x per toglierla, solo col mouse sopra
            const float cx = W - 18 * d, cy = 17 * d;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, sopraX ? 0.25f : 0.12f) : D2D1::ColorF(0, 0, 0, sopraX ? 0.14f : 0.07f));
            v.rt->FillEllipse({{cx, cy}, 10 * d, 10 * d}, v.pen);
            v.pen->SetColor(ink);
            v.rt->DrawLine({cx - 3.5f * d, cy - 3.5f * d}, {cx + 3.5f * d, cy + 3.5f * d}, v.pen, 1.5f * d);
            v.rt->DrawLine({cx + 3.5f * d, cy - 3.5f * d}, {cx - 3.5f * d, cy + 3.5f * d}, v.pen, 1.5f * d);
        }
        v.finisci();
    }
};

}  // namespace banner
