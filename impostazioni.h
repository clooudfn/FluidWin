// Impostazioni di FluidWin: valori salvati in HKCU\Software\FluidWin\Impostazioni e la finestra per cambiarli
// (dal menu dell'icona di FluidWin o dal centro di controllo). Ogni cambio si applica subito.
#pragma once
#include "vetro.h"
#include "testi.h"
#include "cursori.h"
#include "pool.h"
#include <shellapi.h>   // DragAcceptFiles: il pacchetto di cursori si trascina nella finestra
#include <windowsx.h>
#include <commdlg.h>   // ChooseColorW: il colore del dock scelto a piacere
#include <vector>
#include <functional>
#include <string>

namespace impostazioni {

struct Valori {
    bool avvio = false;           // FluidWin si apre all'accesso a Windows (sta nel registro di Windows, vedi avvioAttivo)
    bool gelatina = true;         // finestre "di gelatina" quando si trascinano
    bool genio = true;            // riduzione e ripristino con l'effetto genio
    bool chiusura = true;         // finestre che si rimpiccioliscono e svaniscono quando si chiudono
    bool apertura = true;         // finestre aperte fuori dal dock (Start, ricerca...) che crescono dal centro
    bool onda = true;             // onda di luce multicolore sui bordi dello schermo quando si apre la ricerca
    bool volume = true;           // indicatore del volume di FluidWin al posto di quello di Windows
    bool angolo = true;           // panoramica col mouse nell'angolo in alto a sinistra
    bool winTab = true;           // Alt+Tab apre la panoramica di FluidWin (fino alla 1.2.0 era Win+Tab: il nome e' rimasto)
    bool winS = true;             // Win+Tab apre la ricerca di FluidWin (il nome nel registro e' rimasto WinS)
    bool banner = false;          // banner delle notifiche di FluidWin (spegne quelli di Windows, app per app)
    bool menuDestro = true;       // menu del tasto destro di FluidWin su desktop ed Esplora file (destro.h)
    bool notch = true;            // il notch in alto al centro (spento: volume e notifiche col vetro, niente musica e timer)
    bool oraDock = false;         // ora e data in fondo al dock, dopo il centro di controllo
    bool dockAuto = true;         // dock a scomparsa sotto le finestre ingrandite o che lo coprono
    int dockOpacita = 100;        // opacita' del fondo del dock, in percento (OPACITA_MIN..100)
    int dockIcone = 42;           // lato delle icone del dock a riposo, px a 100% (ICONE_MIN..ICONE_MAX); l'altezza segue
    int dockDistanza = 6;         // distanza fra le icone del dock, px a 100% (DISTANZA_MIN..DISTANZA_MAX)
    DWORD dockColore = 0;         // colore del fondo del dock: 0 automatico (tema), altrimenti 0x01RRGGBB
    bool dockSpazio = false;      // le finestre ingrandite si fermano sopra il dock (spazio riservato, come la barra di Windows)
    int lingua = 0;               // lingua dell'interfaccia (testi::ITALIANO o testi::INGLESE)
    std::wstring cursore;         // pacchetto di cursori di FluidWin in uso ("" = quelli di Windows; vedi cursori.h)
};
constexpr int OPACITA_MIN = 20, OPACITA_PASSO = 5;
constexpr int ICONE_MIN = 32, ICONE_MAX = 64, ICONE_PASSO = 2;
constexpr int DISTANZA_MIN = 2, DISTANZA_MAX = 24, DISTANZA_PASSO = 2;
constexpr DWORD COLORE_SCELTO = 0x01000000;   // segno di "colore scelto" (senza: automatico)

inline Valori& valori() { static Valori v; return v; }

inline const wchar_t* CHIAVE = L"Software\\FluidWin\\Impostazioni";

inline DWORD leggiDword(const wchar_t* nome, DWORD predefinito) {
    DWORD v = predefinito, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE, nome, RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS) return predefinito;
    return v;
}

inline void scriviDword(const wchar_t* nome, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE, nome, REG_DWORD, &v, sizeof(v));
}

inline std::wstring leggiTesto(const wchar_t* nome) {
    wchar_t b[256] = L""; DWORD n = sizeof(b);
    return RegGetValueW(HKEY_CURRENT_USER, CHIAVE, nome, RRF_RT_REG_SZ, nullptr, b, &n) == ERROR_SUCCESS ? b : L"";
}
inline void scriviTesto(const wchar_t* nome, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE, nome, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

// Avvio con Windows: il valore "FluidWin" nella chiave Run dell'utente (come le altre app che si aprono all'accesso;
// niente amministratore). Lo stato vero e' quello di Windows, non una nostra impostazione: l'avvio si puo' spegnere
// anche da Gestione attivita' > App di avvio, che lo segna in StartupApproved (primo byte dispari = spento)
inline const wchar_t* CHIAVE_RUN = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline const wchar_t* CHIAVE_APPROVATO = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

inline std::wstring comandoAvvio() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    return L"\"" + std::wstring(p) + L"\"";
}

// comando registrato ("" se non c'e')
inline std::wstring comandoRegistrato() {
    wchar_t b[MAX_PATH + 16] = L""; DWORD n = sizeof(b);
    return RegGetValueW(HKEY_CURRENT_USER, CHIAVE_RUN, L"FluidWin", RRF_RT_REG_SZ, nullptr, b, &n) == ERROR_SUCCESS ? b : L"";
}

inline bool avvioAttivo() {
    if (comandoRegistrato().empty()) return false;
    BYTE a[12] = {}; DWORD n = sizeof(a);
    if (RegGetValueW(HKEY_CURRENT_USER, CHIAVE_APPROVATO, L"FluidWin", RRF_RT_REG_BINARY, nullptr, a, &n) == ERROR_SUCCESS && (a[0] & 1)) return false;
    return true;
}

inline void impostaAvvio(bool si) {
    if (si) {
        const std::wstring c = comandoAvvio();
        RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_RUN, L"FluidWin", REG_SZ, c.c_str(), (DWORD)((c.size() + 1) * sizeof(wchar_t)));
        // se era stato spento da Gestione attivita', torna acceso anche li'
        const BYTE acceso[12] = {2};
        RegSetKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPROVATO, L"FluidWin", REG_BINARY, acceso, sizeof(acceso));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_RUN, L"FluidWin");
        RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE_APPROVATO, L"FluidWin");
    }
}

inline void carica() {
    Valori& v = valori();
    v.avvio = avvioAttivo();
    // cartella di FluidWin spostata: l'avvio punta al FluidWin di adesso
    if (v.avvio && comandoRegistrato() != comandoAvvio()) impostaAvvio(true);
    v.gelatina = leggiDword(L"Gelatina", 1); v.genio = leggiDword(L"Genio", 1); v.chiusura = leggiDword(L"Chiusura", 1);
    v.apertura = leggiDword(L"Apertura", 1); v.onda = leggiDword(L"OndaRicerca", 1);
    v.volume = leggiDword(L"Volume", 1); v.angolo = leggiDword(L"AngoloPanoramica", 1); v.winTab = leggiDword(L"WinTab", 1);
    v.winS = leggiDword(L"WinS", 1); v.banner = leggiDword(L"Banner", 0); v.menuDestro = leggiDword(L"MenuDestro", 1);
    v.notch = leggiDword(L"Notch", 1); v.oraDock = leggiDword(L"OraDock", 0);
    v.dockAuto = leggiDword(L"DockAutoNascosto", 1);
    v.dockOpacita = std::clamp((int)leggiDword(L"DockOpacita", 100), OPACITA_MIN, 100);
    v.dockIcone = std::clamp((int)leggiDword(L"DockIcone", 42), ICONE_MIN, ICONE_MAX);
    v.dockDistanza = std::clamp((int)leggiDword(L"DockDistanza", 6), DISTANZA_MIN, DISTANZA_MAX);
    v.dockColore = leggiDword(L"DockColore", 0) & (COLORE_SCELTO | 0xFFFFFF);
    v.dockSpazio = leggiDword(L"DockSpazio", 0);
    v.lingua = leggiDword(L"Lingua", (DWORD)testi::predefinita()) == testi::INGLESE ? testi::INGLESE : testi::ITALIANO;
    testi::lingua() = v.lingua;
    v.cursore = leggiTesto(L"Cursore");
    if (!cursori::esiste(v.cursore)) v.cursore.clear();   // (pacchetto cancellato a mano dalla cartella)
    // la capsula dei tasti non c'e' piu' (1.1.1): via i suoi valori rimasti dalle versioni prima
    RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE, L"Capsula");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, CHIAVE, L"CapsulaMisura");
}

inline void salva() {
    const Valori& v = valori();
    scriviDword(L"Gelatina", v.gelatina); scriviDword(L"Genio", v.genio); scriviDword(L"Chiusura", v.chiusura);
    scriviDword(L"Apertura", v.apertura); scriviDword(L"OndaRicerca", v.onda);
    scriviDword(L"Volume", v.volume); scriviDword(L"AngoloPanoramica", v.angolo); scriviDword(L"WinTab", v.winTab);
    scriviDword(L"WinS", v.winS); scriviDword(L"Banner", v.banner); scriviDword(L"MenuDestro", v.menuDestro); scriviDword(L"Lingua", (DWORD)v.lingua);
    scriviDword(L"Notch", v.notch); scriviDword(L"OraDock", v.oraDock);
    scriviDword(L"DockAutoNascosto", v.dockAuto); scriviDword(L"DockOpacita", (DWORD)v.dockOpacita);
    scriviDword(L"DockIcone", (DWORD)v.dockIcone); scriviDword(L"DockColore", v.dockColore);
    scriviDword(L"DockDistanza", (DWORD)v.dockDistanza);
    scriviDword(L"DockSpazio", v.dockSpazio);
    scriviTesto(L"Cursore", v.cursore);
}

// ---- la finestra ----
// A sinistra la barra delle sezioni (come le Impostazioni di Windows 11), a destra la sezione scelta: titolo, voci in
// riquadri arrotondati (nome, spiegazione, interruttore) e righe informative. Il cambio di sezione e' animato.
class Finestra {
public:
    std::function<void()> cambiato;   // dopo ogni cambio (gia' salvato)
    int raggioWindhawk = 0;           // raggio della mod di Windhawk (0 = nessuna): riga informativa sugli angoli
    void (*registro)(const char*) = nullptr;
    std::function<bool()> copiaDiagnostica;   // mette negli appunti la diagnostica (fluidwin.cpp); true se riuscito
    std::function<void()> tornaAlCentro;      // aperte dal centro di controllo e richiuse nel suo rettangolo: lo riapre
    std::function<void(bool)> dockSu;         // aperte dal centro di controllo: il dock resta su finche' ci sono
    std::function<RECT()> iconaCentro;        // icona del centro di controllo nel dock, sullo schermo (clic altrove)

    // da: rettangolo sullo schermo del vetro del centro di controllo. Le impostazioni non compaiono a parte: il vetro
    // del centro sale e si allarga fino a diventare la loro finestra, al centro dello schermo (dove sono sempre state),
    // e alla chiusura ci rientra, e il centro ricompare. Un clic fuori (un'altra app prende il primo piano) le chiude
    // del tutto: il vetro torna nell'icona del centro, come il pannello. Senza (scorciatoia, menu): compaiono al centro
    void apri(const RECT* da = nullptr) {
        if (!v.fin && !v.crea(L"FluidWin.Impostazioni", vetro::procedura<Finestra>, this, true, 2, true, false)) return;
        if (aperta) {   // gia' aperta: viene davanti (se stava rientrando nel centro, torna a crescere)
            if (morph < 0) { giraMorph(1); vetro::battito(v.fin, 1); }
            SetForegroundWindow(v.fin); return;
        }
        v.aggiornaTema();
        RECT a = vetro::areaPrincipale(true);
        if (da) {
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromRect(da, MONITOR_DEFAULTTONEAREST), &mi)) a = mi.rcWork;
        }
        v.dpi = vetro::dpiMonitor(MonitorFromPoint({a.left, a.top}, MONITOR_DEFAULTTOPRIMARY));
        const float d = v.dpi;
        // misura fissa; su schermi piccoli quanto ci sta (le sezioni sono corte, entrano comunque)
        const int w = (int)std::lround(std::min(LARGO * d, (float)(a.right - a.left) - 40 * d));
        const int h = (int)std::lround(std::min(ALTO * d, (float)(a.bottom - a.top) - 40 * d));
        daCentro = da != nullptr; morph = 0; mx = 1; rientra = false; fuori = false; fuoriDopo = false;
        const int x = (a.left + a.right - w) / 2, y = (a.top + a.bottom - h) / 2;   // al centro, come sempre
        if (daCentro) {
            rDa = *da; rA = {x, y, x + w, y + h};
            morph = 1; mx = 0; tM = vetro::ms();
            v.posiziona(rDa.left, rDa.top, rDa.right - rDa.left, rDa.bottom - rDa.top, HWND_TOP, SWP_SHOWWINDOW);
            if (dockSu) dockSu(true);
            if (registro) { char b[120]; snprintf(b, 120, "impostazioni: crescono dal centro di controllo (%ld,%ld-%ld,%ld -> %d,%d %dx%d)", rDa.left, rDa.top, rDa.right, rDa.bottom, x, y, w, h); registro(b); }
        } else v.posiziona(x, y, w, h, HWND_TOP, SWP_SHOWWINDOW);
        if (!fTitolo) {
            fTitolo = v.formato(22 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fVoce = v.formato(13.5f * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fSotto = v.formato(11.5f * d);
            fSezione = v.formato(13 * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fCentro = v.formato(12.5f * d, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
            fMarchio = v.formato(16 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
        valori().avvio = avvioAttivo();   // (puo' essere cambiato da Gestione attivita' mentre FluidWin era aperto)
        DragAcceptFiles(v.fin, TRUE);
        rileggiPacchetti();
        anim.clear();
        aperta = true; sopra = -1; t0 = vetro::ms(); k = 0; kSez = 1;
        if (daCentro) { t0 -= 1000; k = 1; }   // il vetro c'e' gia' (prende il posto di quello del centro): niente dissolvenza
        v.alfa(daCentro ? 1 : 0);
        vetro::battito(v.fin, 1);
        SetForegroundWindow(v.fin);
        disegna();
    }

    // aperte dal centro di controllo: rientrano nel suo rettangolo (poi lo riaprono); altrimenti si nascondono
    void chiudi() {
        if (aperta && daCentro && tornaAlCentro && v.fin && IsWindowVisible(v.fin)) {
            if (morph == 0) { GetWindowRect(v.fin, &rA); giraMorph(-1); }   // (spostata trascinandola: rientra da li')
            else if (morph > 0) giraMorph(-1);
            if (trascina >= 0) { trascina = -1; ReleaseCapture(); salva(); }
            vetro::battito(v.fin, 1);
            return;
        }
        nascondi();
    }

    // clic altrove (un'altra app ha preso il primo piano): si chiudono del tutto. Il vetro rientra rimpicciolendosi
    // nell'icona del centro di controllo e svanisce (come il pannello quando si chiude), senza riaprire il centro:
    // riaprendolo gli si ridarebbe il primo piano, togliendolo all'app appena cliccata
    void chiudiFuori() {
        if (!aperta || !daCentro || fuori || morph < 0) return;
        if (morph > 0) { fuoriDopo = true; return; }   // sta ancora crescendo: si chiude appena arrivata (niente salti)
        fuori = true;
        const RECT ic = iconaCentro ? iconaCentro() : RECT{};
        const LONG pw = (rDa.right - rDa.left) / 10, ph = (rDa.bottom - rDa.top) / 10;
        const LONG cx = IsRectEmpty(&ic) ? (rDa.left + rDa.right) / 2 : std::clamp((ic.left + ic.right) / 2, rDa.left + pw / 2, rDa.right - pw / 2);
        rDa = {cx - pw / 2, rDa.bottom - ph, cx + pw / 2, rDa.bottom};   // piccolo sopra l'icona, come il pannello che si chiude
        if (registro) registro("impostazioni: clic fuori, si chiudono");
        // l'app cliccata passa davanti e copriva tutta la chiusura (ripresa a 60 fps): sopra a tutto finche' svanisce
        // (finestra nostra; torna normale in nascondi)
        SetWindowPos(v.fin, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        chiudi();
    }

    bool messaggio(HWND, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
        r = 0;
        switch (m) {
            case WM_TIMER:
                if (w == 1 && morph) {   // crescita dal centro di controllo o rientro (vedi apri)
                    const ULONGLONG ora = vetro::ms();
                    const double dt = (double)std::min<ULONGLONG>(ora - tM, 50); tM = ora;
                    mx = std::clamp(mx + morph * dt / (morph > 0 ? DURATA_CRESCE : DURATA_RIENTRA), 0.0, 1.0);
                    applicaForma();
                    // rientrata: ricompare il centro di controllo (sopra) e questa sparisce. Si finisce a forma < 1%
                    // (pochi px dal rettangolo del centro): la coda di x^3 lasciava il vetro vuoto quasi fermo per ~100 ms
                    if (morph < 0 && (mx <= 0 || forma() < 0.01)) {
                        if (registro) registro(fuori ? "impostazioni: chiuse nell'icona" : "impostazioni: rientrate nel centro di controllo");
                        if (tornaAlCentro && !fuori) tornaAlCentro();
                        nascondi();
                        return true;
                    }
                    if (morph > 0 && mx >= 1) { morph = 0; if (fuoriDopo) { fuoriDopo = false; chiudiFuori(); } }
                }
                if (w == 1) {
                    k = std::min(1.0, (vetro::ms() - t0) / 180.0);
                    v.alfa(fuori && morph < 0 ? std::min(1.0, forma() * 2.2) : vetro::uscita(k));   // (verso l'icona: svanisce)
                    bool ancora = k < 1 || morph != 0;
                    if (kSez < 1) { kSez = std::min(1.0, (vetro::ms() - tSez) / 180.0); ancora = true; }
                    if (copiataAlle && vetro::ms() - copiataAlle > 2500) copiataAlle = 0;   // "Copiata" torna "Copia"
                    else if (copiataAlle) ancora = true;
                    for (size_t i = 0; i < voci.size(); i++) {
                        const double obj = stato(voci[i]) ? 1.0 : 0.0;
                        if (i < anim.size() && std::abs(anim[i] - obj) > 0.01) { anim[i] += (obj - anim[i]) * 0.3; ancora = true; }
                        else if (i < anim.size()) anim[i] = obj;
                    }
                    disegna();
                    if (!ancora) vetro::fermaBattito(v.fin, 1);
                }
                return true;
            case WM_PAINT: disegna(); return true;
            // un'altra app prende il primo piano (clic altrove): aperte dal centro di controllo, si chiudono da sole.
            // (le finestre di FluidWin, come quella dei colori, non contano: l'app resta FluidWin)
            // Nella sezione Cursore no: per trascinarci un pacchetto si clicca prima il file in Esplora file, e quel clic
            // le chiudeva (si chiudono con la x o con Esc)
            case WM_ACTIVATEAPP: if (!w && sezione != S_CURSORE) chiudiFuori(); return false;
            case WM_ACTIVATE: {   // il centro di controllo riaperto dalla sua icona mentre queste erano aperte: lascia a lui
                wchar_t c[32] = L"";
                if (LOWORD(w) == WA_INACTIVE && l && daCentro && morph == 0 && GetClassNameW((HWND)l, c, 32) && !wcscmp(c, L"FluidWin.Pannello")) {
                    if (registro) registro("impostazioni: riaperto il centro di controllo, si nascondono");
                    nascondi();
                }
                return false;
            }
            case WM_MOUSEMOVE: {
                if (morph) return true;   // mentre cresce o rientra non si tocca niente
                if (trascina >= 0) { valoreDa(trascina, (float)GET_X_LPARAM(l)); return true; }
                if (!dentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, v.fin, 0}; TrackMouseEvent(&tm); dentro = true; }
                const int s = zonaIn((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
                if (s != sopra) { sopra = s; disegna(); }
                return true;
            }
            case WM_MOUSELEAVE: dentro = false; if (trascina < 0) sopra = -1; disegna(); return true;
            case WM_LBUTTONDOWN: {   // su un cursore: si trascina (anche fuori dal binario, finche' non si lascia)
                if (morph) return true;
                const int z = zonaIn((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
                if (z >= Z_CURSORE && z < Z_CURSORE + (int)cursori.size()) {
                    trascina = z - Z_CURSORE; SetCapture(v.fin); valoreDa(trascina, (float)GET_X_LPARAM(l));
                }
                return true;
            }
            case WM_LBUTTONUP:
                if (trascina >= 0) { trascina = -1; ReleaseCapture(); salva(); disegna(); return true; }
                if (morph) return true;
                clic(zonaIn((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l)));
                return true;
            case WM_CAPTURECHANGED: if (trascina >= 0) { trascina = -1; salva(); disegna(); } return true;
            case WM_KEYDOWN:
                if (w == VK_ESCAPE) chiudi();
                else if (w == VK_DOWN || w == VK_UP) vaiASezione((sezione + (w == VK_DOWN ? 1 : SEZIONI - 1)) % SEZIONI);   // frecce: sezioni
                return true;
            case WM_CLOSE: chiudi(); return true;
            case WM_MOUSEWHEEL:   // sezione Cursore: l'elenco dei pacchetti scorre (non ci stanno tutti: R20261114-141207-S7)
                if (sezione == S_CURSORE && !morph) {
                    // (si sommano: ogni WHEEL_DELTA una riga; prima un delta di 60 per volta non scorreva mai, R20261114-142613)
                    rotella += GET_WHEEL_DELTA_WPARAM(w);
                    scorriCursori -= rotella / WHEEL_DELTA; rotella %= WHEEL_DELTA;
                    disegna();
                }
                return true;
            case WM_DROPFILES: {   // pacchetto di cursori trascinato nella finestra (anche fuori dalla sezione: ci si va)
                HDROP hd = (HDROP)w;
                wchar_t p[MAX_PATH] = L"";
                const bool uno = DragQueryFileW(hd, 0, p, MAX_PATH) > 0;
                DragFinish(hd);
                if (uno) { vaiASezione(S_CURSORE); installa(p); }
                return true;
            }
            case WM_CURSORI_INSTALLATI: {
                cursori::Esito* e = (cursori::Esito*)l;
                installando = false;
                if (e->ok) {
                    statoCursori = testi::t(L"Installato: ", L"Installed: ") + e->nome + L" (" + std::to_wstring(e->quanti) + testi::t(L" cursori)", L" cursors)");
                    erroreCursori = false;
                    valori().cursore = e->nome;   // si usa subito
                    salva();
                    if (cambiato) cambiato();
                } else {
                    const size_t bar = e->errore.find(L'|');
                    statoCursori = testi::t(L"Non installato: ", L"Not installed: ") + (bar == std::wstring::npos ? e->errore : testi::t(e->errore.substr(0, bar).c_str(), e->errore.substr(bar + 1).c_str()));
                    erroreCursori = true;
                }
                delete e;
                rileggiPacchetti();
                vetro::battito(v.fin, 1);
                disegna();
                return true;
            }
            case WM_NCHITTEST: {   // si trascina dalla fascia in alto (non dalla x) e dalla testata della barra laterale
                if (morph) return false;
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(v.fin, &p);
                const float d = v.dpi;
                if ((p.y < 60 * d && p.x < v.W - 56 * d && zonaIn((float)p.x, (float)p.y) < 0)) { r = HTCAPTION; return true; }
                return false;
            }
        }
        return false;
    }

private:
    static constexpr float LARGO = 780, ALTO = 500, BARRA = 240;
    enum { S_GENERALE, S_DOCK, S_ANIMAZIONI, S_NOTCH, S_TASTIERA, S_CURSORE, SEZIONI };
    // nome e spiegazione in italiano e in inglese, sezione
    struct Voce { const wchar_t *nome, *name, *spiega, *explain; bool Valori::*campo; int sezione; };
    const std::vector<Voce> voci = {
        {L"Nascondi automaticamente", L"Auto-hide",
         L"Spento: resta su anche sopra le finestre ingrandite", L"Off: it stays up over maximized windows too", &Valori::dockAuto, S_DOCK},
        {L"Finestre ingrandite sopra il dock", L"Maximized windows above the dock",
         L"Come la barra di Windows: si fermano sopra. Spento: fino in fondo", L"Like the Windows taskbar: they stop above. Off: full screen", &Valori::dockSpazio, S_DOCK},
        {L"Finestre di gelatina", L"Jelly windows",
         L"Le finestre si deformano mentre le trascini", L"Windows wobble while you drag them", &Valori::gelatina, S_ANIMAZIONI},
        {L"Avvia con Windows", L"Start with Windows",
         L"FluidWin si apre da solo quando accedi", L"FluidWin opens by itself when you sign in", &Valori::avvio, S_GENERALE},
        {L"Effetto genio", L"Genie effect",
         L"Riduzione e ripristino che entrano ed escono dal dock", L"Minimize into the dock and come back out of it", &Valori::genio, S_ANIMAZIONI},
        {L"Animazione di apertura", L"Open animation",
         L"Le finestre aperte da Start o dalla ricerca crescono dal centro", L"Windows opened from Start or search grow from the center", &Valori::apertura, S_ANIMAZIONI},
        {L"Animazione di chiusura", L"Close animation",
         L"La finestra si rimpicciolisce e svanisce sul posto", L"The window shrinks and fades away in place", &Valori::chiusura, S_ANIMAZIONI},
        {L"Onda di luce della ricerca", L"Search light wave",
         L"I bordi dello schermo si accendono di colori", L"The edges of the screen light up with color", &Valori::onda, S_ANIMAZIONI},
        {L"Notch", L"Notch",
         L"In alto al centro: ora, volume, notifiche, musica e timer", L"At the top center: time, volume, notifications, music, timer", &Valori::notch, S_NOTCH},
        {L"Ora e data nel dock", L"Time and date in the dock",
         L"In fondo al dock, dopo il centro di controllo", L"At the end of the dock, after the control center", &Valori::oraDock, S_NOTCH},
        {L"Notifiche di FluidWin", L"FluidWin notifications",
         L"Escono dal notch, con il numero sulle icone del dock", L"They come out of the notch, with badges on the dock", &Valori::banner, S_NOTCH},
        {L"Volume nel notch", L"Volume in the notch",
         L"I tasti del volume mostrano il livello nel notch", L"The volume keys show the level in the notch", &Valori::volume, S_NOTCH},
        {L"Alt+Tab apre la panoramica", L"Alt+Tab opens the overview",
         L"Tieni Alt e premi Tab per scegliere la finestra, come prima", L"Hold Alt and press Tab to pick a window, as always", &Valori::winTab, S_TASTIERA},
        {L"Win+Tab apre la ricerca", L"Win+Tab opens search",
         L"App, file, impostazioni e calcoli (al posto della Visualizzazione attività)", L"Apps, files, settings and math (instead of Task View)", &Valori::winS, S_TASTIERA},
        {L"Panoramica dall'angolo", L"Overview from the corner",
         L"Mouse nell'angolo in alto a sinistra", L"Move the mouse to the top-left corner", &Valori::angolo, S_TASTIERA},
        {L"Menu del tasto destro", L"Right-click menu",
         L"Su desktop ed Esplora file. Spento o con Maiusc: quello di Windows",
         L"On the desktop and in File Explorer. Off or with Shift: the Windows one", &Valori::menuDestro, S_TASTIERA},
    };
    vetro::Finestra v;
    IDWriteTextFormat *fTitolo = nullptr, *fVoce = nullptr, *fSotto = nullptr, *fSezione = nullptr, *fCentro = nullptr, *fMarchio = nullptr;
    bool aperta = false, dentro = false;
    // crescita dal centro di controllo e rientro (vedi apri): morph 1 cresce, -1 rientra, 0 fermo; mx 0..1 avanzamento
    // (0 = rettangolo del centro rDa, 1 = impostazioni rA); rientra: curva del rientro (vedi forma)
    bool daCentro = false, rientra = false, fuori = false, fuoriDopo = false;   // fuori: si chiudono nell'icona (vedi chiudiFuori)
    int morph = 0;
    double mx = 1;
    ULONGLONG tM = 0;
    RECT rDa{}, rA{};
    static constexpr double DURATA_CRESCE = 340, DURATA_RIENTRA = 260;   // ms
    int sopra = -1, sezione = S_GENERALE;
    ULONGLONG t0 = 0, tSez = 0, copiataAlle = 0;
    double k = 0, kSez = 1;
    std::vector<double> anim;
    // zone: 0..n-1 le voci, 100+i la lingua, 200 chiudi, 300+i le sezioni, 400 diagnostica, 500 opacita' del dock
    struct Zona { D2D1_RECT_F r; int id; };
    std::vector<Zona> zone;
    // cursori (valori con un minimo e un massimo): 500+i. Il dock cambia mentre si trascina; si salva quando si lascia
    struct Cursore { const wchar_t *nome, *name, *spiega, *explain, *unita; int Valori::*campo; int min, max, passo; int sezione; };
    const std::vector<Cursore> cursori = {
        {L"Dimensione delle icone", L"Icon size", L"Anche l'altezza del dock segue", L"The dock's height follows",
         L" px", &Valori::dockIcone, ICONE_MIN, ICONE_MAX, ICONE_PASSO, S_DOCK},
        {L"Distanza fra le icone", L"Space between icons", L"Anche i separatori seguono", L"Separators follow too",
         L" px", &Valori::dockDistanza, DISTANZA_MIN, DISTANZA_MAX, DISTANZA_PASSO, S_DOCK},
        {L"Opacità", L"Opacity", L"Il vetro del dock; le icone restano piene", L"The dock's glass; icons stay solid",
         L"%", &Valori::dockOpacita, OPACITA_MIN, 100, OPACITA_PASSO, S_DOCK},
    };
    static constexpr int Z_CURSORE = 500;
    std::vector<D2D1_RECT_F> binari = std::vector<D2D1_RECT_F>(cursori.size());   // dove si e' disegnato ogni cursore
    int trascina = -1;                                               // cursore che si sta trascinando

    void valoreDa(int i, float x) {
        const Cursore& c = cursori[i]; const D2D1_RECT_F& b = binari[i];
        const float p = std::clamp((x - b.left) / std::max(1.0f, b.right - b.left), 0.0f, 1.0f);
        const int val = std::clamp((int)std::lround((c.min + p * (c.max - c.min)) / c.passo) * c.passo, c.min, c.max);
        if (val == valori().*(c.campo)) return;
        valori().*(c.campo) = val;
        if (cambiato) cambiato();
        disegna();
    }

    // colore del dock: 600 automatico, 600+k i colori pronti, Z_COLORE_MIO quello scelto con la finestra dei colori
    static constexpr int Z_COLORE = 600;
    static constexpr DWORD COLORI[] = {0x2C2C2E, 0xF2F2F7, 0x0A84FF, 0x5E5CE6, 0xBF5AF2, 0xFF375F, 0xFF9F0A, 0x30D158};
    static constexpr int N_COLORI = sizeof(COLORI) / sizeof(COLORI[0]);
    static constexpr int Z_COLORE_MIO = Z_COLORE + 1 + N_COLORI;
    void scegliColore(int id) {
        DWORD& c = valori().dockColore;
        if (id == Z_COLORE) c = 0;
        else if (id < Z_COLORE_MIO) c = COLORE_SCELTO | COLORI[id - Z_COLORE - 1];
        else {   // la finestra dei colori di Windows
            static COLORREF propri[16] = {};
            const DWORD prima = c & 0xFFFFFF;
            CHOOSECOLORW cc{}; cc.lStructSize = sizeof(cc); cc.hwndOwner = v.fin; cc.lpCustColors = propri;
            cc.rgbResult = RGB((prima >> 16) & 255, (prima >> 8) & 255, prima & 255);
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (!ChooseColorW(&cc)) return;
            c = COLORE_SCELTO | ((DWORD)GetRValue(cc.rgbResult) << 16) | ((DWORD)GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
        }
    }
    static D2D1_COLOR_F colore(DWORD rgb, float a = 1) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a); }

    static bool stato(const Voce& x) { return valori().*(x.campo); }

    // ---- cursori (sezione Cursore): 700 la zona del pacchetto, 710 quelli di Windows, 720+i i pacchetti, 760+i rimuovi
    static constexpr UINT WM_CURSORI_INSTALLATI = WM_APP + 50;
    static constexpr int Z_PACCHETTO = 700, Z_WINDOWS = 710, Z_SCEGLI = 720, Z_RIMUOVI = 760, MAX_PACCHETTI = 30;
    std::vector<cursori::Pacchetto> pacchetti;
    std::vector<ID2D1Bitmap*> anteprime;   // la freccia di ogni pacchetto, fatta per il render target di giroAnteprime
    unsigned giroAnteprime = ~0u;
    bool installando = false, erroreCursori = false;
    int scorriCursori = 0;   // prima riga mostrata dell'elenco (0 = "Cursori di Windows"): rotella del mouse
    int rotella = 0;         // scatti piccoli della rotella (touchpad, rotelle ad alta precisione) messi da parte
    std::wstring statoCursori;

    void liberaAnteprime() { for (auto* b : anteprime) if (b) b->Release(); anteprime.clear(); }
    void rileggiPacchetti() { pacchetti = cursori::elenco(); if ((int)pacchetti.size() > MAX_PACCHETTI) pacchetti.resize(MAX_PACCHETTI); liberaAnteprime(); }

    // la freccia del pacchetto come bitmap (pixel premoltiplicati); nullptr se non si legge
    ID2D1Bitmap* anteprima(const std::wstring& file, int lato) {
        if (file.empty() || !v.rt) return nullptr;
        HCURSOR c = (HCURSOR)LoadImageW(nullptr, file.c_str(), IMAGE_CURSOR, lato, lato, LR_LOADFROMFILE);
        if (!c) c = LoadCursorFromFileW(file.c_str());
        if (!c) return nullptr;
        ICONINFO ii{}; ID2D1Bitmap* out = nullptr;
        if (GetIconInfo(c, &ii) && ii.hbmColor) {
            BITMAP bm{}; GetObject(ii.hbmColor, sizeof(bm), &bm);
            const int w = bm.bmWidth, h = bm.bmHeight;
            if (w > 0 && h > 0 && w <= 256 && h <= 256) {
                std::vector<UINT> px((size_t)w * h);
                BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
                HDC scr = GetDC(nullptr); GetDIBits(scr, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, scr);
                // solo la parte disegnata (i cursori stanno in un angolo di un quadrato molto piu' grande: misurato su
                // OutLier, la freccia occupa 15 x 15 px di 32 x 32)
                int x0 = w, y0 = h, x1 = -1, y1 = -1;
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (px[(size_t)y * w + x] >> 24) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
                if (x1 >= x0 && y1 >= y0) {
                    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
                    std::vector<UINT> taglio((size_t)cw * ch);
                    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
                        const UINT p = px[(size_t)(y0 + y) * w + x0 + x], a = p >> 24;   // Direct2D vuole i colori premoltiplicati
                        taglio[(size_t)y * cw + x] = (a << 24) | ((((p >> 16) & 255) * a / 255) << 16) | ((((p >> 8) & 255) * a / 255) << 8) | ((p & 255) * a / 255);
                    }
                    v.rt->CreateBitmap(D2D1::SizeU(cw, ch), taglio.data(), cw * 4,
                                       D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &out);
                }
            }
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        DestroyCursor(c);
        return out;
    }

    // installazione in un thread (tar puo' metterci qualche secondo); la risposta torna con WM_CURSORI_INSTALLATI
    void installa(const std::wstring& file) {
        if (installando) return;
        installando = true; erroreCursori = false;
        statoCursori = testi::t(L"Installazione di ", L"Installing ") + cursori::nomeFile(file) + L"…";
        struct Lavoro { std::wstring file; HWND fin; };
        Lavoro* l = new Lavoro{file, v.fin};
        if (!pool::lancia([](LPVOID p) -> DWORD {
                Lavoro* l = (Lavoro*)p;
                cursori::Esito* e = new cursori::Esito(cursori::installa(l->file));
                if (!PostMessageW(l->fin, WM_CURSORI_INSTALLATI, 0, (LPARAM)e)) delete e;
                delete l;
                return 0;
            }, l, THREAD_PRIORITY_BELOW_NORMAL, true)) {
            delete l; installando = false; erroreCursori = true;
            statoCursori = testi::t(L"Non installato: lavoro non partito", L"Not installed: job did not start");
        }
        vetro::battito(v.fin, 1);
        disegna();
    }

    // clic sulla zona del pacchetto: lo si sceglie da una finestra (oltre che trascinandolo)
    void scegliPacchetto() {
        // (la finestra di scelta ha il suo ciclo dei messaggi: un clic che arriva mentre e' aperta non ne apre una
        // seconda; misurato: un clic solo ne apriva due, una sopra l'altra)
        static bool aperta = false;
        if (aperta) return;
        aperta = true;
        if (registro) registro("impostazioni: scelta del pacchetto di cursori");
        wchar_t p[MAX_PATH] = L"";
        OPENFILENAMEW of{}; of.lStructSize = sizeof(of); of.hwndOwner = v.fin; of.lpstrFile = p; of.nMaxFile = MAX_PATH;
        of.lpstrFilter = L"Cursori (.zip, .rar, .7z)\0*.zip;*.rar;*.7z\0\0";
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        const bool scelto = GetOpenFileNameW(&of) != FALSE;
        aperta = false;
        if (scelto) installa(p);
    }

    int zonaIn(float x, float y) const {
        for (const auto& z : zone) if (x >= z.r.left && x < z.r.right && y >= z.r.top && y < z.r.bottom) return z.id;
        return -1;
    }

    void vaiASezione(int s) {
        if (s == sezione) return;
        sezione = s; kSez = 0; tSez = vetro::ms();
        vetro::battito(v.fin, 1);
        disegna();
    }

    void clic(int id) {
        if (id < 0) return;
        if (id == 200) { chiudi(); return; }
        if (id >= Z_CURSORE && id < Z_COLORE) return;   // (i cursori si usano trascinando: WM_LBUTTONDOWN)
        if (id >= Z_COLORE && id <= Z_COLORE_MIO) {
            scegliColore(id);
            salva();
            if (cambiato) cambiato();
            disegna();
            return;
        }
        if (id == Z_PACCHETTO) { if (!installando) scegliPacchetto(); return; }
        if (id >= Z_WINDOWS && id < Z_RIMUOVI + MAX_PACCHETTI) {
            if (id == Z_WINDOWS) valori().cursore.clear();
            else if (id < Z_RIMUOVI) { const int i = id - Z_SCEGLI; if (i >= 0 && i < (int)pacchetti.size()) valori().cursore = pacchetti[i].nome; }
            else {
                const int i = id - Z_RIMUOVI;
                if (i >= 0 && i < (int)pacchetti.size()) {
                    const std::wstring n = pacchetti[i].nome;
                    if (valori().cursore == n) { valori().cursore.clear(); salva(); if (cambiato) cambiato(); }   // prima via dallo schermo
                    cursori::rimuovi(n);
                    statoCursori = testi::t(L"Rimosso: ", L"Removed: ") + n; erroreCursori = false;
                    rileggiPacchetti();
                }
            }
            salva();
            if (cambiato) cambiato();
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id == 400) {   // diagnostica negli appunti
            if (copiaDiagnostica && copiaDiagnostica()) { copiataAlle = vetro::ms(); vetro::battito(v.fin, 1); }
            disegna();
            return;
        }
        if (id >= 300 && id < 300 + SEZIONI) { vaiASezione(id - 300); return; }
        if (id >= 100) { valori().lingua = id - 100; testi::lingua() = valori().lingua; }
        else if (id < (int)voci.size()) {
            bool& b = valori().*(voci[id].campo); b = !b;
            if (voci[id].campo == &Valori::avvio) impostaAvvio(b);   // (sta nel registro di Windows, non in salva)
        }
        salva();
        if (cambiato) cambiato();
        vetro::battito(v.fin, 1);
        disegna();
    }

    // versione di FluidWin, dalle risorse dell'eseguibile
    static std::wstring versione() {
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
        DWORD h = 0, n = GetFileVersionInfoSizeW(p, &h);
        std::wstring r;
        if (n) {
            std::vector<BYTE> b(n);
            VS_FIXEDFILEINFO* f = nullptr; UINT lf = 0;
            if (GetFileVersionInfoW(p, 0, n, b.data()) && VerQueryValueW(b.data(), L"\\", (void**)&f, &lf) && f) {
                wchar_t s[32]; swprintf(s, 32, L"%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS), HIWORD(f->dwFileVersionLS));
                r = s;
            }
        }
        return r;
    }

    static const wchar_t* nomeSezione(int s) {
        switch (s) {
            case S_GENERALE: return testi::t(L"Generale", L"General");
            case S_DOCK: return L"Dock";
            case S_ANIMAZIONI: return testi::t(L"Animazioni", L"Animations");
            case S_NOTCH: return testi::t(L"Notch e notifiche", L"Notch & notifications");
            case S_TASTIERA: return testi::t(L"Tastiera e mouse", L"Keyboard and mouse");
            case S_CURSORE: return testi::t(L"Cursore", L"Cursor");
        }
        return L"";
    }

    // icone della barra laterale, a linee
    void iconaSezione(int s, float cx, float cy, D2D1_COLOR_F c) {
        const float d = v.dpi, sp = 1.6f * d;
        v.pen->SetColor(c);
        auto l = [&](float x1, float y1, float x2, float y2) { v.rt->DrawLine({cx + x1 * d, cy + y1 * d}, {cx + x2 * d, cy + y2 * d}, v.pen, sp); };
        switch (s) {
            case S_GENERALE:   // ingranaggio semplice: cerchio con i denti
                v.rt->DrawEllipse({{cx, cy}, 4.5f * d, 4.5f * d}, v.pen, sp);
                for (int i = 0; i < 8; i++) { const float a = i * 3.14159265f / 4; l(6 * std::cos(a), 6 * std::sin(a), 8.5f * std::cos(a), 8.5f * std::sin(a)); }
                break;
            case S_DOCK:       // il dock: barra arrotondata in basso con tre icone
                v.rt->DrawRoundedRectangle({{cx - 9 * d, cy + 1 * d, cx + 9 * d, cy + 7 * d}, 2.5f * d, 2.5f * d}, v.pen, sp);
                for (int i = 0; i < 3; i++) v.rt->FillRoundedRectangle({{cx + (-6.5f + i * 5) * d, cy + 2.5f * d, cx + (-3.5f + i * 5) * d, cy + 5.5f * d}, 0.8f * d, 0.8f * d}, v.pen);
                l(-7, -6, 7, -6); break;
            case S_ANIMAZIONI: // scintilla e una piccola
                l(-2, -8, -2, 4); l(-8, -2, 4, -2); l(-5.5f, -5.5f, 1.5f, 1.5f); l(1.5f, -5.5f, -5.5f, 1.5f);
                l(5, 3, 5, 8); l(2.5f, 5.5f, 7.5f, 5.5f); break;
            case S_NOTCH:      // pillola del notch attaccata al bordo
                l(-9, -7, 9, -7);
                v.rt->FillRoundedRectangle({{cx - 6 * d, cy - 7 * d, cx + 6 * d, cy - 2 * d}, 2.5f * d, 2.5f * d}, v.pen);
                v.rt->DrawRoundedRectangle({{cx - 8 * d, cy + 1 * d, cx + 8 * d, cy + 7 * d}, 2 * d, 2 * d}, v.pen, sp); break;
            case S_TASTIERA:   // tastiera
                v.rt->DrawRoundedRectangle({{cx - 9 * d, cy - 5.5f * d, cx + 9 * d, cy + 5.5f * d}, 2 * d, 2 * d}, v.pen, sp);
                for (int i = 0; i < 4; i++) v.rt->FillEllipse({{cx + (-5.5f + i * 3.7f) * d, cy - 1.5f * d}, 0.9f * d, 0.9f * d}, v.pen);
                l(-4, 2.5f, 4, 2.5f); break;
            case S_CURSORE:    // freccia del mouse
                l(-5, -8, -5, 6); l(-5, 6, -1.5f, 2.5f); l(-1.5f, 2.5f, 1.5f, 8.5f); l(1.5f, 8.5f, 4, 7.3f); l(4, 7.3f, 1.2f, 1.8f);
                l(1.2f, 1.8f, 6, 1.8f); l(6, 1.8f, -5, -8); break;
        }
    }

    void nascondi() {
        if (v.fin) { vetro::fermaBattito(v.fin, 1); ShowWindow(v.fin, SW_HIDE); }
        if (v.fin && fuori) SetWindowPos(v.fin, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (daCentro && dockSu) dockSu(false);   // (dopo tornaAlCentro: col pannello gia' aperto il dock non scende)
        aperta = false; daCentro = false; morph = 0; mx = 1; rientra = false; fuori = false; fuoriDopo = false;
    }

    // forma del vetro 0..1 (0 = rettangolo del centro di controllo, 1 = impostazioni). Tutte e due le direzioni
    // rallentano all'arrivo: crescendo 1-(1-x)^3, rientrando x^3 (con x che scende)
    double forma() const { return rientra ? mx * mx * mx : 1 - std::pow(1 - mx, 3); }
    // cambio di direzione a meta': stessa forma di adesso, curva dell'altra direzione (niente salti)
    void giraMorph(int dir) {
        const double e = forma();
        morph = dir; rientra = dir < 0; tM = vetro::ms();
        mx = rientra ? std::cbrt(e) : 1 - std::cbrt(1 - e);
    }
    void applicaForma() {
        const double e = forma();
        auto mix = [e](LONG a, LONG b) { return (int)std::lround(a + (b - a) * e); };
        const int x = mix(rDa.left, rA.left), y = mix(rDa.top, rA.top);
        v.posiziona(x, y, std::max(1, mix(rDa.right, rA.right) - x), std::max(1, mix(rDa.bottom, rA.bottom) - y), nullptr, SWP_NOZORDER);
    }

    void disegna() {
        if (!aperta || !v.inizia()) return;
        float W = (float)v.W, H = (float)v.H;
        // mentre cresce o rientra: il contenuto con le misure finali, rimpicciolito per stare nel vetro, e compare
        // solo nell'ultima parte della crescita (prima il vetro e' troppo stretto per leggerlo)
        const bool strato = morph != 0 && rA.right > rA.left && rA.bottom > rA.top;
        if (strato) {
            const float fw = (float)(rA.right - rA.left), fh = (float)(rA.bottom - rA.top);
            const float s = std::min({1.0f, W / fw, H / fh});
            // rientrando la forma cambia quasi tutta subito (curva x^3): legato a lei il contenuto spariva nei primi
            // 30 ms e restava il vetro vuoto (ripresa a 60 fps); qui svanisce col tempo, fino a ~1/3 del rientro
            const float x = (float)(rientra ? std::clamp((mx - 0.35) / 0.5, 0.0, 1.0) : std::clamp((forma() - 0.55) / 0.4, 0.0, 1.0));
            const float op = x * x * (3 - 2 * x);
            v.rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation((W - fw * s) / 2, (H - fh * s) / 2));
            v.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), op), nullptr);
            W = fw; H = fh;
        }
        const float d = v.dpi, B = BARRA * d;
        const D2D1_COLOR_F ink = v.inchiostro(), tenue = v.inchiostro(0.6f);
        const D2D1_COLOR_F blu = v.scuro ? D2D1::ColorF(0.40f, 0.68f, 1.0f, 1) : D2D1::ColorF(0.0f, 0.42f, 0.95f, 1);
        zone.clear();
        if (anim.size() != voci.size()) { anim.clear(); for (const auto& x : voci) anim.push_back(stato(x) ? 1 : 0); }
        const float rr = std::max(8.0f, raggio::vetro() - 12.0f) * d;

        // ---- barra laterale ----
        v.pen->SetColor(v.scuro ? D2D1::ColorF(0, 0, 0, 0.18f) : D2D1::ColorF(0, 0, 0, 0.035f));
        v.rt->FillRectangle({0, 0, B, H}, v.pen);
        v.testo(L"FluidWin", fMarchio, {24 * d, 22 * d, B - 12 * d, 44 * d}, ink);
        const std::wstring ver = versione();
        v.testo(ver.empty() ? std::wstring(testi::t(L"Impostazioni", L"Settings")) : testi::t(L"Impostazioni · ", L"Settings · ") + ver,
                fSotto, {24 * d, 44 * d, B - 12 * d, 62 * d}, tenue);
        for (int s = 0; s < SEZIONI; s++) {
            const D2D1_RECT_F r = {12 * d, (84 + s * 44) * d, B - 12 * d, (84 + s * 44 + 38) * d};
            zone.push_back({r, 300 + s});
            const bool scelta = s == sezione;
            if (scelta || sopra == 300 + s) {
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, scelta ? 0.10f : 0.05f) : D2D1::ColorF(0, 0, 0, scelta ? 0.06f : 0.035f));
                v.rt->FillRoundedRectangle({r, 8 * d, 8 * d}, v.pen);
            }
            if (scelta) {   // segno blu a sinistra, come in Windows 11
                v.pen->SetColor(blu);
                v.rt->FillRoundedRectangle({{r.left + 2 * d, r.top + 11 * d, r.left + 5 * d, r.bottom - 11 * d}, 1.5f * d, 1.5f * d}, v.pen);
            }
            iconaSezione(s, r.left + 26 * d, (r.top + r.bottom) / 2, scelta ? blu : ink);
            v.testo(nomeSezione(s), fSezione, {r.left + 48 * d, r.top, r.right - 6 * d, r.bottom}, ink);
        }

        // ---- chiudi ----
        {
            const D2D1_RECT_F r = {W - 52 * d, 18 * d, W - 20 * d, 50 * d};
            zone.push_back({r, 200});
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, sopra == 200 ? 0.16f : 0.08f) : D2D1::ColorF(0, 0, 0, sopra == 200 ? 0.10f : 0.05f));
            v.rt->FillEllipse({{(r.left + r.right) / 2, (r.top + r.bottom) / 2}, 15 * d, 15 * d}, v.pen);
            v.pen->SetColor(ink);
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            v.rt->DrawLine({cx - 4.5f * d, cy - 4.5f * d}, {cx + 4.5f * d, cy + 4.5f * d}, v.pen, 1.6f * d);
            v.rt->DrawLine({cx + 4.5f * d, cy - 4.5f * d}, {cx - 4.5f * d, cy + 4.5f * d}, v.pen, 1.6f * d);
        }

        // ---- sezione (entra con una piccola scivolata) ----
        const double e = vetro::uscita(kSez);
        const float sx = (float)((1 - e) * 14 * d);
        const D2D1_COLOR_F inkS = {ink.r, ink.g, ink.b, ink.a * (float)e}, tenueS = {tenue.r, tenue.g, tenue.b, tenue.a * (float)e};
        const float L = B + 28 * d + sx, R = W - 28 * d + sx;
        v.testo(nomeSezione(sezione), fTitolo, {L, 20 * d, R - 50 * d, 54 * d}, inkS);
        float y = 72 * d;
        auto riquadro = [&](const D2D1_RECT_F& r, bool evid) {
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (evid ? 0.10f : 0.06f) * (float)e) : D2D1::ColorF(1, 1, 1, (evid ? 0.75f : 0.55f) * (float)e));
            v.rt->FillRoundedRectangle({r, rr, rr}, v.pen);
        };
        auto riga = [&](const std::wstring& titolo, const std::wstring& testo) {   // riga informativa (senza interruttore)
            const D2D1_RECT_F r = {L, y, R, y + 54 * d};
            riquadro(r, false);
            v.testo(titolo, fVoce, {r.left + 16 * d, r.top + 7 * d, r.right - 16 * d, r.top + 29 * d}, inkS);
            v.testo(testo, fSotto, {r.left + 16 * d, r.top + 28 * d, r.right - 16 * d, r.top + 48 * d}, tenueS);
            y += 60 * d;
        };
        for (size_t i = 0; i < voci.size(); i++) {
            if (voci[i].sezione != sezione) continue;
            const D2D1_RECT_F r = {L, y, R, y + 54 * d};
            zone.push_back({r, (int)i});
            riquadro(r, sopra == (int)i);
            v.testo(testi::t(voci[i].nome, voci[i].name), fVoce, {r.left + 16 * d, r.top + 7 * d, r.right - 74 * d, r.top + 29 * d}, inkS);
            v.testo(testi::t(voci[i].spiega, voci[i].explain), fSotto, {r.left + 16 * d, r.top + 28 * d, r.right - 74 * d, r.top + 48 * d}, tenueS);
            levetta(r.right - 58 * d, (r.top + r.bottom) / 2, (float)anim[i], (float)e);
            y += 60 * d;
        }
        if (sezione == S_GENERALE) {
            // lingua: due scelte (i nomi restano ciascuno nella sua lingua)
            const D2D1_RECT_F rl = {L, y, R, y + 64 * d};
            riquadro(rl, false);
            v.testo(testi::t(L"Lingua", L"Language"), fVoce, {rl.left + 16 * d, rl.top, rl.left + 160 * d, rl.bottom}, inkS);
            const float x0 = rl.left + 170 * d, larg = rl.right - 16 * d - x0, h = 34 * d, yl = rl.top + 15 * d;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)e) : D2D1::ColorF(0, 0, 0, 0.06f * (float)e));
            v.rt->FillRoundedRectangle({{x0, yl, x0 + larg, yl + h}, h / 2, h / 2}, v.pen);
            const wchar_t* nomi[2] = {L"Italiano", L"English"};
            for (int i = 0; i < 2; i++) {
                const D2D1_RECT_F r = {x0 + larg * i / 2 + 3 * d, yl + 3 * d, x0 + larg * (i + 1) / 2 - 3 * d, yl + h - 3 * d};
                zone.push_back({r, 100 + i});
                if (valori().lingua == i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * (float)e) : D2D1::ColorF(1, 1, 1, 0.95f * (float)e));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                } else if (sopra == 100 + i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                }
                v.testo(nomi[i], fCentro, r, inkS);
            }
            y += 70 * d;
            riga(testi::t(L"Angoli delle finestre", L"Window corners"), raggioWindhawk > 0
                ? std::wstring(testi::t(L"Arrotondati a ", L"Rounded to ")) + std::to_wstring(raggioWindhawk) + testi::t(L" px dalla mod di Windhawk", L" px by the Windhawk mod")
                : std::wstring(testi::t(L"Quelli di Windows (piu' tondi con la mod di Windhawk)", L"The Windows ones (rounder with the Windhawk mod)")));
            riga(L"FluidWin" + (ver.empty() ? std::wstring() : L" " + ver), testi::t(L"Windows reimmaginato · github.com/clooudfn/FluidWin", L"Windows reimagined · github.com/clooudfn/FluidWin"));
            // diagnostica: versione, Windows, schermi, scheda video, impostazioni (niente dati personali), per chi
            // segnala un problema; il pulsante "Copia" a destra
            {
                const float yr = y;
                riga(testi::t(L"Diagnostica", L"Diagnostics"), testi::t(L"Sistema, schermi, scheda video e impostazioni, da incollare in una segnalazione",
                                                                         L"System, screens, graphics card and settings, to paste into a report"));
                const std::wstring tb = copiataAlle ? testi::t(L"Copiata", L"Copied") : testi::t(L"Copia", L"Copy");
                const float lw = v.larghezza(tb, fCentro) + 30 * d;
                const D2D1_RECT_F rb = {R - 16 * d - lw, yr + 12 * d, R - 16 * d, yr + 42 * d};
                zone.push_back({rb, 400});
                v.pen->SetColor(copiataAlle ? D2D1::ColorF(0.20f, 0.78f, 0.35f, (float)e) : D2D1::ColorF(blu.r, blu.g, blu.b, (sopra == 400 ? 1.0f : 0.88f) * (float)e));
                v.rt->FillRoundedRectangle({rb, 15 * d, 15 * d}, v.pen);
                v.testo(tb, fCentro, rb, D2D1::ColorF(1, 1, 1, (float)e));
            }
        }
        // cursori della sezione: nome e spiegazione a sinistra, binario a destra, il valore accanto
        for (size_t i = 0; i < cursori.size(); i++) {
            const Cursore& c = cursori[i];
            if (c.sezione != sezione) continue;
            const int id = Z_CURSORE + (int)i;
            const D2D1_RECT_F r = {L, y, R, y + 54 * d};
            riquadro(r, sopra == id || trascina == (int)i);
            const float x1 = r.right - 64 * d, x0 = x1 - 190 * d, cy = (r.top + r.bottom) / 2;
            v.testo(testi::t(c.nome, c.name), fVoce, {r.left + 16 * d, r.top + 7 * d, x0 - 16 * d, r.top + 29 * d}, inkS);
            v.testo(testi::t(c.spiega, c.explain), fSotto, {r.left + 16 * d, r.top + 28 * d, x0 - 16 * d, r.top + 48 * d}, tenueS);
            binari[i] = {x0, cy - 12 * d, x1, cy + 12 * d};
            zone.push_back({{x0 - 10 * d, r.top, x1 + 10 * d, r.bottom}, id});
            const int val = valori().*(c.campo);
            const float p = (val - c.min) / (float)(c.max - c.min), xm = x0 + (x1 - x0) * p;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.18f * (float)e) : D2D1::ColorF(0, 0, 0, 0.12f * (float)e));
            v.rt->FillRoundedRectangle({{x0, cy - 2 * d, x1, cy + 2 * d}, 2 * d, 2 * d}, v.pen);
            v.pen->SetColor({blu.r, blu.g, blu.b, (float)e});
            v.rt->FillRoundedRectangle({{x0, cy - 2 * d, xm, cy + 2 * d}, 2 * d, 2 * d}, v.pen);
            const float rk = (trascina == (int)i ? 10.0f : 9.0f) * d;   // pallino (un po' piu' grande mentre si trascina)
            v.pen->SetColor(D2D1::ColorF(0, 0, 0, (v.scuro ? 0.35f : 0.18f) * (float)e));
            v.rt->FillEllipse({{xm, cy + 0.8f * d}, rk + 0.6f * d, rk + 0.6f * d}, v.pen);
            v.pen->SetColor(D2D1::ColorF(1, 1, 1, (float)e));
            v.rt->FillEllipse({{xm, cy}, rk, rk}, v.pen);
            v.testo(std::to_wstring(val) + c.unita, fCentro, {x1 + 8 * d, r.top, r.right - 8 * d, r.bottom}, inkS);
            y += 60 * d;
        }
        if (sezione == S_DOCK) {
            // colore del dock: automatico (meta' chiaro e meta' scuro), i colori pronti, e "+" per sceglierne uno
            const D2D1_RECT_F r = {L, y, R, y + 54 * d};
            riquadro(r, false);
            const float rc = 10 * d, passo = 28 * d, cy = (r.top + r.bottom) / 2;
            const float xFine = r.right - 16 * d - rc, xInizio = xFine - passo * (N_COLORI + 1);
            v.testo(testi::t(L"Colore", L"Color"), fVoce, {r.left + 16 * d, r.top + 7 * d, xInizio - 16 * d, r.top + 29 * d}, inkS);
            v.testo(testi::t(L"Del vetro del dock", L"Of the dock's glass"), fSotto, {r.left + 16 * d, r.top + 28 * d, xInizio - 16 * d, r.top + 48 * d}, tenueS);
            const DWORD scelto = valori().dockColore;
            bool pronto = false;
            for (int k = 0; k < N_COLORI; k++) if (scelto == (COLORE_SCELTO | COLORI[k])) pronto = true;
            for (int k = 0; k <= N_COLORI + 1; k++) {
                const int id = Z_COLORE + k;
                const float cx = xInizio + passo * k;
                const D2D1_ELLIPSE el = {{cx, cy}, rc, rc};
                zone.push_back({{cx - passo / 2, r.top, cx + passo / 2, r.bottom}, id});
                const bool attivo = k == 0 ? !(scelto & COLORE_SCELTO)
                                  : k <= N_COLORI ? scelto == (COLORE_SCELTO | COLORI[k - 1])
                                                  : ((scelto & COLORE_SCELTO) && !pronto);
                if (k == 0) {   // automatico: meta' chiaro e meta' scuro
                    v.pen->SetColor(D2D1::ColorF(0.96f, 0.96f, 0.97f, (float)e)); v.rt->FillEllipse(el, v.pen);
                    v.pen->SetColor(D2D1::ColorF(0.17f, 0.17f, 0.18f, (float)e));
                    v.rt->PushAxisAlignedClip({cx, cy - rc, cx + rc, cy + rc}, D2D1_ANTIALIAS_MODE_ALIASED);
                    v.rt->FillEllipse(el, v.pen); v.rt->PopAxisAlignedClip();
                } else if (k <= N_COLORI) {
                    v.pen->SetColor(colore(COLORI[k - 1], (float)e)); v.rt->FillEllipse(el, v.pen);
                } else {        // il proprio: il colore scelto, oppure un "+"
                    if (attivo) { v.pen->SetColor(colore(scelto, (float)e)); v.rt->FillEllipse(el, v.pen); }
                    else {
                        v.pen->SetColor(tenueS); v.rt->DrawEllipse(el, v.pen, 1.4f * d);
                        v.rt->DrawLine({cx - 4.5f * d, cy}, {cx + 4.5f * d, cy}, v.pen, 1.6f * d);
                        v.rt->DrawLine({cx, cy - 4.5f * d}, {cx, cy + 4.5f * d}, v.pen, 1.6f * d);
                    }
                }
                // bordo sottile (i colori chiari si vedono anche sul riquadro chiaro), anello blu su quello scelto
                v.pen->SetColor(D2D1::ColorF(0, 0, 0, 0.15f * (float)e)); v.rt->DrawEllipse(el, v.pen, 1.0f);
                if (attivo || sopra == id) {
                    v.pen->SetColor(attivo ? D2D1::ColorF(blu.r, blu.g, blu.b, (float)e) : tenueS);
                    v.rt->DrawEllipse({{cx, cy}, rc + 3.5f * d, rc + 3.5f * d}, v.pen, 2.0f * d);
                }
            }
            y += 60 * d;
        }
        if (sezione == S_NOTCH)
            riga(testi::t(L"Nel notch, sempre", L"Always in the notch"),
                 testi::t(L"Musica, timer, scaffale dei file, microfono e webcam, batteria e cuffie", L"Music, timer, file shelf, microphone and camera, battery and headphones"));
        if (sezione == S_TASTIERA) {
            riga(testi::t(L"Ctrl+Win+T", L"Ctrl+Win+T"), testi::t(L"Tiene la finestra attiva sempre in primo piano (di nuovo per toglierlo)", L"Keeps the active window always on top (again to undo)"));
            riga(testi::t(L"Maiusc+clic destro", L"Shift+right-click"), testi::t(L"Il menu di Windows, anche con il menu di FluidWin acceso", L"The Windows menu, even with the FluidWin menu on"));
        }
        if (sezione == S_CURSORE) disegnaCursori(L, R, y, e, ink, tenue, blu, rr, H);
        if (strato) { v.rt->PopLayer(); v.rt->SetTransform(D2D1::Matrix3x2F::Identity()); }
        v.finisci();
    }

    // Sezione Cursore: la zona dove trascinare il pacchetto (o cliccarla per sceglierlo), poi "Cursori di Windows" e i
    // pacchetti installati (anteprima della freccia, nome, quanti cursori, pallino della scelta, Rimuovi)
    void disegnaCursori(float L, float R, float y, double e, D2D1_COLOR_F ink, D2D1_COLOR_F tenue, D2D1_COLOR_F blu, float rr, float H) {
        const float d = v.dpi, fe = (float)e;
        const D2D1_COLOR_F inkS = {ink.r, ink.g, ink.b, ink.a * fe}, tenueS = {tenue.r, tenue.g, tenue.b, tenue.a * fe};
        if (giroAnteprime != v.giroRt) { liberaAnteprime(); giroAnteprime = v.giroRt; }
        // zona del pacchetto: bordo tratteggiato
        {
            const D2D1_RECT_F r = {L, y, R, y + 92 * d};
            zone.push_back({r, Z_PACCHETTO});
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (sopra == Z_PACCHETTO ? 0.09f : 0.05f) * fe) : D2D1::ColorF(1, 1, 1, (sopra == Z_PACCHETTO ? 0.75f : 0.5f) * fe));
            v.rt->FillRoundedRectangle({r, rr, rr}, v.pen);
            v.pen->SetColor(erroreCursori ? D2D1::ColorF(0.95f, 0.30f, 0.30f, 0.8f * fe) : D2D1::ColorF(blu.r, blu.g, blu.b, (installando ? 0.9f : 0.55f) * fe));
            const float m = 6 * d, x0 = r.left + m, x1 = r.right - m, y0 = r.top + m, y1 = r.bottom - m, tr = 6 * d, sp = 8 * d;
            const int nx = (int)((x1 - x0 - 2 * tr) / (2 * sp)) + 1, ny = (int)((y1 - y0 - 2 * tr) / (2 * sp)) + 1;
            for (int k = 0; k < nx; k++) { const float x = x0 + tr + k * 2 * sp, xe = std::min(x + sp, x1 - tr); v.rt->DrawLine({x, y0}, {xe, y0}, v.pen, 1.4f * d); v.rt->DrawLine({x, y1}, {xe, y1}, v.pen, 1.4f * d); }
            for (int k = 0; k < ny; k++) { const float yy = y0 + tr + k * 2 * sp, ye = std::min(yy + sp, y1 - tr); v.rt->DrawLine({x0, yy}, {x0, ye}, v.pen, 1.4f * d); v.rt->DrawLine({x1, yy}, {x1, ye}, v.pen, 1.4f * d); }
            const std::wstring titolo = installando ? statoCursori : std::wstring(testi::t(L"Trascina qui un pacchetto di cursori", L"Drop a cursor pack here"));
            const std::wstring sotto = installando ? std::wstring(testi::t(L"Un momento…", L"One moment…"))
                                     : !statoCursori.empty() ? statoCursori
                                     : std::wstring(testi::t(L"File .zip, .rar o .7z, oppure clicca per sceglierlo. Valgono solo con FluidWin aperto",
                                                             L".zip, .rar or .7z file, or click to choose it. Used only while FluidWin is open"));
            v.testo(titolo, fVoce, {r.left + 22 * d, r.top + 20 * d, r.right - 22 * d, r.top + 44 * d}, inkS);
            v.testo(sotto, fSotto, {r.left + 22 * d, r.top + 46 * d, r.right - 22 * d, r.top + 70 * d},
                    erroreCursori && !installando ? D2D1::ColorF(0.95f, 0.30f, 0.30f, fe) : tenueS);
            y += 100 * d;
        }
        // scelte: Windows e i pacchetti, quanti ne stanno; gli altri con la rotella (accanto al titolo quali si vedono)
        const float riga = 50 * d, passo = 56 * d;
        const int posto = std::max(1, (int)((H - 14 * d - y) / passo)), tutte = 1 + (int)pacchetti.size();
        scorriCursori = std::clamp(scorriCursori, 0, std::max(0, tutte - posto));
        if (tutte > posto) {
            const std::wstring quali = std::to_wstring(scorriCursori + 1) + L"–" + std::to_wstring(std::min(tutte, scorriCursori + posto)) +
                                       testi::t(L" di ", L" of ") + std::to_wstring(tutte) + testi::t(L" · scorri", L" · scroll");
            v.testo(quali, fSotto, {R - 220 * d, 26 * d, R - 60 * d, 50 * d}, tenueS);
        }
        const std::wstring& scelto = valori().cursore;
        for (int i = scorriCursori - 1; i < (int)pacchetti.size() && i + 1 < scorriCursori + posto; i++) {
            const D2D1_RECT_F r = {L, y, R, y + riga};
            const int id = i < 0 ? Z_WINDOWS : Z_SCEGLI + i;
            const bool attivo = i < 0 ? scelto.empty() : scelto == pacchetti[i].nome;
            zone.push_back({{r.left, r.top, r.right - (i < 0 ? 0 : 96 * d), r.bottom}, id});
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (sopra == id ? 0.10f : 0.06f) * fe) : D2D1::ColorF(1, 1, 1, (sopra == id ? 0.75f : 0.55f) * fe));
            v.rt->FillRoundedRectangle({r, rr, rr}, v.pen);
            // pallino della scelta
            const float cx = r.left + 22 * d, cy = (r.top + r.bottom) / 2;
            v.pen->SetColor(attivo ? D2D1::ColorF(blu.r, blu.g, blu.b, fe) : tenueS);
            v.rt->DrawEllipse({{cx, cy}, 8 * d, 8 * d}, v.pen, 1.6f * d);
            if (attivo) v.rt->FillEllipse({{cx, cy}, 4.5f * d, 4.5f * d}, v.pen);
            float xt = r.left + 42 * d;
            if (i >= 0) {   // anteprima della freccia
                if ((int)anteprime.size() < (int)pacchetti.size()) anteprime.resize(pacchetti.size(), nullptr);
                if (!anteprime[i]) anteprime[i] = anteprima(pacchetti[i].freccia, (int)std::lround(64 * d));
                // su un riquadro chiaro: i cursori hanno quasi sempre il bordo scuro, sul tema scuro non si vedrebbero
                const float q = 34 * d;
                const D2D1_RECT_F rq = {xt, cy - q / 2, xt + q, cy + q / 2};
                v.pen->SetColor(D2D1::ColorF(0.93f, 0.94f, 0.96f, (v.scuro ? 0.92f : 0.75f) * fe));
                v.rt->FillRoundedRectangle({rq, 8 * d, 8 * d}, v.pen);
                if (anteprime[i]) {
                    const D2D1_SIZE_F s = anteprime[i]->GetSize();
                    const float lato = 24 * d, k = std::min(lato / std::max(s.width, s.height), 1.5f * d);
                    const float bw = s.width * k, bh = s.height * k, bx = (rq.left + rq.right - bw) / 2, by = (rq.top + rq.bottom - bh) / 2;
                    v.rt->DrawBitmap(anteprime[i], {bx, by, bx + bw, by + bh}, fe, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                xt += 46 * d;
            }
            const std::wstring nome = i < 0 ? std::wstring(testi::t(L"Cursori di Windows", L"Windows cursors")) : pacchetti[i].nome;
            const std::wstring info = i < 0 ? std::wstring(testi::t(L"Quelli scelti nelle impostazioni di Windows", L"The ones chosen in Windows settings"))
                                            : std::to_wstring(pacchetti[i].quanti) + testi::t(L" cursori", L" cursors");
            v.testo(nome, fVoce, {xt, r.top + 5 * d, r.right - 104 * d, r.top + 26 * d}, inkS);
            v.testo(info, fSotto, {xt, r.top + 25 * d, r.right - 104 * d, r.top + 45 * d}, tenueS);
            if (i >= 0) {   // Rimuovi
                const std::wstring tr = testi::t(L"Rimuovi", L"Remove");
                const float lw = v.larghezza(tr, fCentro) + 24 * d;
                const D2D1_RECT_F rb = {r.right - 12 * d - lw, cy - 14 * d, r.right - 12 * d, cy + 14 * d};
                zone.push_back({rb, Z_RIMUOVI + i});
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (sopra == Z_RIMUOVI + i ? 0.16f : 0.08f) * fe) : D2D1::ColorF(0, 0, 0, (sopra == Z_RIMUOVI + i ? 0.10f : 0.05f) * fe));
                v.rt->FillRoundedRectangle({rb, 14 * d, 14 * d}, v.pen);
                v.testo(tr, fCentro, rb, inkS);
            }
            y += passo;
        }
    }

    // interruttore in stile iOS, k = 0 spento .. 1 acceso; a = trasparenza
    void levetta(float x, float cy, float k, float a = 1) {
        const float d = v.dpi, w = 42 * d, h = 24 * d;
        const D2D1_RECT_F r = {x, cy - h / 2, x + w, cy + h / 2};
        const D2D1_COLOR_F spento = v.scuro ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.14f);
        const D2D1_COLOR_F acceso = D2D1::ColorF(0.20f, 0.78f, 0.35f, 1);
        v.pen->SetColor(D2D1::ColorF(spento.r + (acceso.r - spento.r) * k, spento.g + (acceso.g - spento.g) * k,
                                     spento.b + (acceso.b - spento.b) * k, (spento.a + (acceso.a - spento.a) * k) * a));
        v.rt->FillRoundedRectangle({r, h / 2, h / 2}, v.pen);
        v.pen->SetColor(D2D1::ColorF(1, 1, 1, a));
        v.rt->FillEllipse({{x + h / 2 + (w - h) * k, cy}, h / 2 - 2.5f * d, h / 2 - 2.5f * d}, v.pen);
    }
};

}  // namespace impostazioni
