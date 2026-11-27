// Impostazioni di FluidWin: valori salvati in HKCU\Software\FluidWin\Impostazioni e la finestra per cambiarli
// (dal menu dell'icona di FluidWin o dal centro di controllo). Ogni cambio si applica subito.
#pragma once
#include "vetro.h"
#include "testi.h"
#include "cursori.h"
#include "pool.h"
#include "sfondi.h"
#include "widget_plugin.h"
#include <shellapi.h>   // DragAcceptFiles: il pacchetto di cursori si trascina nella finestra
#include <windowsx.h>
#include <commdlg.h>   // ChooseColorW: il colore del dock scelto a piacere
#include <vector>
#include <functional>
#include <string>
#include <utility>

namespace impostazioni {

struct Valori {
    bool avvio = false;           // FluidWin si apre all'accesso a Windows (sta nel registro di Windows, vedi avvioAttivo)
    bool gelatina = true;         // finestre "di gelatina" quando si trascinano
    bool genio = true;            // riduzione e ripristino animati (con il genio o con la caduta, vedi riduzione)
    int riduzione = 0;            // 0 genio (la finestra si deforma), 1 caduta nel dock (riquadro che cade dietro le icone)
    int angoli = 0;               // angoli di dock, notch, pannelli e finestre di FluidWin: 0 arrotondati, 1 poco, 2 squadrati (raggio.h)
    bool chiusura = true;         // finestre che si rimpiccioliscono e svaniscono quando si chiudono
    bool apertura = true;         // finestre aperte fuori dal dock (Start, ricerca...) che crescono dal centro
    bool onda = true;             // onda di luce multicolore sui bordi dello schermo quando si apre la ricerca
    bool volume = true;           // indicatore del volume di FluidWin al posto di quello di Windows
    bool angolo = true;           // panoramica col mouse nell'angolo in alto a sinistra
    bool tastoWin = true;         // il tasto Win da solo apre il launcher delle app (al posto dello Start)
    bool winTab = true;           // Alt+Tab apre la panoramica di FluidWin (fino alla 1.2.0 era Win+Tab: il nome e' rimasto)
    bool winS = true;             // Win+Tab apre la ricerca di FluidWin (il nome nel registro e' rimasto WinS)
    bool banner = false;          // banner delle notifiche di FluidWin (spegne quelli di Windows, app per app)
    bool menuDestro = true;       // menu del tasto destro di FluidWin su desktop ed Esplora file (destro.h)
    bool notch = true;            // il notch in alto al centro sempre visibile (spento: a scomparsa, scende solo per volume, notifiche e avvisi; musica e timer nel dock)
    bool oraDock = false;         // ora e data in fondo al dock, dopo il centro di controllo
    int dockLato = 0;             // bordo del dock: 0 in basso, 1 in alto, 2 a sinistra, 3 a destra (1.3.0, Fase 3; il notch va in basso col dock in alto)
    bool dockAuto = true;         // dock a scomparsa sotto le finestre ingrandite o che lo coprono
    int dockOpacita = 100;        // opacita' del fondo del dock, in percento (OPACITA_MIN..100)
    int dockIcone = 42;           // lato delle icone del dock a riposo, px a 100% (ICONE_MIN..ICONE_MAX); l'altezza segue
    int dockDistanza = 6;         // distanza fra le icone del dock, px a 100% (DISTANZA_MIN..DISTANZA_MAX)
    DWORD dockColore = 0;         // colore del fondo del dock: 0 automatico (tema), altrimenti 0x01RRGGBB
    bool dockSpazio = false;      // le finestre ingrandite si fermano sopra il dock (spazio riservato, come la barra di Windows)
    bool dockSchermi = true;      // il dock segue il mouse sul bordo in basso degli altri schermi (spento: resta sul principale)
    bool dockAnteprime = true;    // anteprime delle finestre col mouse fermo su un'icona del dock (issue #24)
    bool dockAI = false;          // icona Monitor AI nel dock
    bool dockLogi = false;        // widget Logitech G HUB nel dock
    bool logiAnimCarica = true;   // animazione di ricarica sull'anello Logitech
    DWORD logiGlowColore = 0;     // colore bagliore ricarica Logitech: 0 automatico (ciano), altrimenti 0x00RRGGBB
    bool aggiornamenti = true;    // all'avvio chiede a GitHub se c'e' una versione nuova (aggiorna.h)
    bool affianca = true;         // affiancamento di FluidWin al posto dello Snap di Windows (affianca.h)
    int lingua = 0;               // lingua dell'interfaccia (testi::Lingua, 0..N_LINGUE-1)
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
    v.riduzione = leggiDword(L"Riduzione", 0) == 1 ? 1 : 0;
    { const DWORD a = leggiDword(L"Angoli", 0); v.angoli = a <= 2 ? (int)a : 0; }
    v.volume = leggiDword(L"Volume", 1); v.angolo = leggiDword(L"AngoloPanoramica", 1); v.winTab = leggiDword(L"WinTab", 1);
    v.winS = leggiDword(L"WinS", 1); v.tastoWin = leggiDword(L"TastoWin", 1); v.banner = leggiDword(L"Banner", 0); v.menuDestro = leggiDword(L"MenuDestro", 1);
    v.notch = leggiDword(L"Notch", 1); v.oraDock = leggiDword(L"OraDock", 0);
    v.dockAuto = leggiDword(L"DockAutoNascosto", 1);
    { const DWORD l = leggiDword(L"DockLato", 0); v.dockLato = l <= 3 ? (int)l : 0; }
    v.dockOpacita = std::clamp((int)leggiDword(L"DockOpacita", 100), OPACITA_MIN, 100);
    v.dockIcone = std::clamp((int)leggiDword(L"DockIcone", 42), ICONE_MIN, ICONE_MAX);
    v.dockDistanza = std::clamp((int)leggiDword(L"DockDistanza", 6), DISTANZA_MIN, DISTANZA_MAX);
    v.dockColore = leggiDword(L"DockColore", 0) & (COLORE_SCELTO | 0xFFFFFF);
    v.dockSpazio = leggiDword(L"DockSpazio", 0);
    v.dockSchermi = leggiDword(L"DockAltriSchermi", 1);
    v.dockAnteprime = leggiDword(L"DockAnteprime", 1);
    v.dockAI = leggiDword(L"DockAI", 0);
    v.dockLogi = leggiDword(L"DockLogi", 0);
    v.logiAnimCarica = leggiDword(L"LogiAnimCarica", 1);
    v.logiGlowColore = leggiDword(L"LogiGlowColore", 0) & 0xFFFFFFu;
    v.aggiornamenti = leggiDword(L"Aggiornamenti", 1);
    v.affianca = leggiDword(L"Affiancamento", 1);
    { const DWORD l = leggiDword(L"Lingua", (DWORD)testi::predefinita()); v.lingua = l < (DWORD)testi::N_LINGUE ? (int)l : testi::INGLESE; }
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
    scriviDword(L"Apertura", v.apertura); scriviDword(L"OndaRicerca", v.onda); scriviDword(L"Riduzione", (DWORD)v.riduzione); scriviDword(L"Angoli", (DWORD)v.angoli);
    scriviDword(L"Volume", v.volume); scriviDword(L"AngoloPanoramica", v.angolo); scriviDword(L"WinTab", v.winTab);
    scriviDword(L"WinS", v.winS); scriviDword(L"TastoWin", v.tastoWin); scriviDword(L"Banner", v.banner); scriviDword(L"MenuDestro", v.menuDestro); scriviDword(L"Lingua", (DWORD)v.lingua);
    scriviDword(L"Notch", v.notch); scriviDword(L"OraDock", v.oraDock);
    scriviDword(L"DockAutoNascosto", v.dockAuto); scriviDword(L"DockLato", (DWORD)v.dockLato); scriviDword(L"DockOpacita", (DWORD)v.dockOpacita);
    scriviDword(L"DockIcone", (DWORD)v.dockIcone); scriviDword(L"DockColore", v.dockColore);
    scriviDword(L"DockDistanza", (DWORD)v.dockDistanza);
    scriviDword(L"DockSpazio", v.dockSpazio); scriviDword(L"DockAltriSchermi", v.dockSchermi); scriviDword(L"DockAnteprime", v.dockAnteprime); scriviDword(L"DockAI", v.dockAI); scriviDword(L"DockLogi", v.dockLogi); scriviDword(L"LogiAnimCarica", v.logiAnimCarica); scriviDword(L"LogiGlowColore", v.logiGlowColore & 0xFFFFFFu); scriviDword(L"Aggiornamenti", v.aggiornamenti); scriviDword(L"Affiancamento", v.affianca);
    scriviTesto(L"Cursore", v.cursore);
}

// ---- la finestra ----
// A sinistra la barra delle sezioni (come le Impostazioni di Windows 11), a destra la sezione scelta: titolo, voci in
// riquadri arrotondati (nome, spiegazione, interruttore) e righe informative. Il cambio di sezione e' animato.
class Finestra {
public:
    std::function<void()> cambiato;   // dopo ogni cambio (gia' salvato)
    void (*registro)(const char*) = nullptr;
    std::function<bool()> copiaDiagnostica;   // mette negli appunti la diagnostica (fluidwin.cpp); true se riuscito
    std::function<void()> esci;               // "Esci da FluidWin": l'uscita normale, che rimette Windows com'era (#16)
    std::function<void()> tornaAlCentro;      // aperte dal centro di controllo e richiuse nel suo rettangolo: lo riapre
    std::function<void(bool)> dockSu;         // aperte dal centro di controllo: il dock resta su finche' ci sono
    std::function<RECT()> iconaCentro;        // icona del centro di controllo nel dock, sullo schermo (clic altrove)
    std::function<std::vector<std::pair<std::wstring, std::wstring>>()> elencoNascoste;
    std::function<void(const std::wstring&)> mostraNascosta;
    std::function<std::vector<std::pair<std::wstring, std::pair<std::wstring, bool>>>()> elencoWPlugin;
    std::function<void(const std::wstring&, bool)> impostaWPlugin;
    std::function<void()> apriCartellaWPlugin;
    std::function<void()> creaEsempioWPlugin;
    std::function<bool()> leggiAIAvvisi;
    std::function<void(bool)> impostaAIAvvisi;
    std::function<bool()> hookAgyStato;
    std::function<void(bool)> hookAgyImposta;
    std::function<bool()> hookClaudeStato;
    std::function<void(bool)> hookClaudeImposta;
    std::function<void()> apriCartellaHookAI;
    std::function<void()> apriGHub;
    std::function<bool(const std::wstring&, wplugin::Widget&)> dettagliWPlugin;
    std::function<void(const std::wstring&, const std::wstring&, bool)> impostaConfigWPluginBool;
    std::function<void(const std::wstring&, const std::wstring&, const std::wstring&)> impostaConfigWPluginStr;
    std::function<void(const std::wstring&, const std::wstring&, double)> impostaConfigWPluginNum;

    // da: rettangolo sullo schermo del vetro del centro di controllo. Le impostazioni non compaiono a parte: il vetro
    // del centro sale e si allarga fino a diventare la loro finestra, al centro dello schermo (dove sono sempre state),
    // e alla chiusura ci rientra, e il centro ricompare. Un clic fuori (un'altra app prende il primo piano) le chiude
    // del tutto: il vetro torna nell'icona del centro, come il pannello. Senza (scorciatoia, menu): compaiono al centro
    // Da dove crescono: 'da' (il pannello del centro di controllo, vedi sopra); senza, dal punto del mouse ('dalMouse':
    // menu del dock e dell'icona nell'area di notifica, dove si e' appena cliccato) o dal centro dello schermo (ricerca).
    // Prima crescevano solo dal centro di controllo: dagli altri menu comparivano e basta (segnalato da chi li apriva dal
    // menu dell'icona, la via spiegata nel README)
    void apri(const RECT* da = nullptr, bool dalMouse = false) {
        if (!v.fin && !v.crea(L"FluidWin.Impostazioni", vetro::procedura<Finestra>, this, true, 2, true, false)) return;
        if (aperta) {   // gia' aperta: viene davanti (se stava rientrando nel centro, torna a crescere)
            if (morph < 0) { if (daPunto) fuori = false; giraMorph(1); vetro::battito(v.fin, 1); }
            SetForegroundWindow(v.fin); return;
        }
        v.aggiornaTema();
        RECT a = vetro::areaPrincipale(true);
        POINT mouse{}; dalMouse = dalMouse && !da && GetCursorPos(&mouse);
        if (da || dalMouse) {   // sullo schermo da cui si aprono
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            const HMONITOR m = da ? MonitorFromRect(da, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(mouse, MONITOR_DEFAULTTONEAREST);
            if (GetMonitorInfoW(m, &mi)) a = mi.rcWork;
        }
        v.dpi = vetro::dpiMonitor(MonitorFromPoint({a.left, a.top}, MONITOR_DEFAULTTOPRIMARY));
        const float d = v.dpi;
        // misura fissa; su schermi piccoli quanto ci sta (le sezioni sono corte, entrano comunque)
        const int w = (int)std::lround(std::min(LARGO * d, (float)(a.right - a.left) - 40 * d));
        const int h = (int)std::lround(std::min(ALTO * d, (float)(a.bottom - a.top) - 40 * d));
        daCentro = da != nullptr; daPunto = !daCentro; morph = 0; mx = 1; rientra = false; fuori = false; fuoriDopo = false;
        const int x = (a.left + a.right - w) / 2, y = (a.top + a.bottom - h) / 2;   // al centro, come sempre
        rA = {x, y, x + w, y + h};
        if (daCentro) rDa = *da;
        else {   // un vetro piccolo (un decimo) attorno al mouse o al centro dello schermo, dentro lo schermo
            const int pw = std::max(1, w / 10), ph = std::max(1, h / 10);
            const POINT c = dalMouse ? mouse : POINT{(rA.left + rA.right) / 2, (rA.top + rA.bottom) / 2};
            const int px = std::clamp((int)c.x - pw / 2, (int)a.left, (int)a.right - pw), py = std::clamp((int)c.y - ph / 2, (int)a.top, (int)a.bottom - ph);
            rDa = {px, py, px + pw, py + ph};
        }
        morph = 1; mx = 0; tM = vetro::ms();
        v.posiziona(rDa.left, rDa.top, rDa.right - rDa.left, rDa.bottom - rDa.top, HWND_TOP, SWP_SHOWWINDOW);
        if (daCentro && dockSu) dockSu(true);
        if (registro) { char b[140]; snprintf(b, 140, "impostazioni: crescono da%s (%ld,%ld-%ld,%ld -> %d,%d %dx%d)", daCentro ? "l centro di controllo" : dalMouse ? "l mouse" : "l centro dello schermo", rDa.left, rDa.top, rDa.right, rDa.bottom, x, y, w, h); registro(b); }
        // (rifatti se lo schermo ha un'altra scala: prima restavano quelli della prima apertura, S15)
        if (fTitolo && fDpi != d) for (IDWriteTextFormat** f : {&fTitolo, &fVoce, &fSotto, &fSezione, &fCentro, &fMarchio, &fPiccolo}) if (*f) { (*f)->Release(); *f = nullptr; }
        if (!fTitolo) {
            fDpi = d;
            fTitolo = v.formato(22 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fVoce = v.formato(13.5f * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fSotto = v.formato(11.5f * d);
            fSezione = v.formato(13 * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fCentro = v.formato(12.5f * d, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
            fMarchio = v.formato(16 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fPiccolo = v.formato(10.5f * d, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);   // ("Coming soon" della lingua)
        }
        valori().avvio = avvioAttivo();   // (puo' essere cambiato da Gestione attivita' mentre FluidWin era aperto)
        DragAcceptFiles(v.fin, TRUE);
        rileggiPacchetti();
        anim.clear();
        widgetAperto.clear();
        sliderWPlugDrag = -1;
        aperta = true; sopra = -1; t0 = vetro::ms(); k = 0; kSez = 1;
        // (lo sfondo puo' essere cambiato da fuori: si rilegge, e la miniatura del tuo si rifa')
        sfondoAttuale = -2; sfondoErrore = false; provataMini[0] = false; if (miniSfondi[0]) { miniSfondi[0]->Release(); miniSfondi[0] = nullptr; }
        if (sfondoInCorso != -2 && vetro::ms() - sfondoDa >= 15000) sfondoInCorso = -2;   // (risposta persa: vedi scegliSfondo)
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
            if (sliderWPlugDrag >= 0) { sliderWPlugDrag = -1; ReleaseCapture(); }
            vetro::battito(v.fin, 1);
            return;
        }
        // aperte da un menu o dalla ricerca: rientrano nel punto da cui sono cresciute, svanendo (come chiudiFuori)
        if (aperta && daPunto && v.fin && IsWindowVisible(v.fin)) {
            fuori = true;
            if (morph == 0) { GetWindowRect(v.fin, &rA); giraMorph(-1); }
            else if (morph > 0) giraMorph(-1);
            if (trascina >= 0) { trascina = -1; ReleaseCapture(); salva(); }
            if (sliderWPlugDrag >= 0) { sliderWPlugDrag = -1; ReleaseCapture(); }
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

    bool messaggio(HWND hw, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
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
                        if (registro) registro(daPunto ? "impostazioni: rientrate nel punto da cui sono cresciute" : fuori ? "impostazioni: chiuse nell'icona" : "impostazioni: rientrate nel centro di controllo");
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
                if (sliderWPlugDrag >= 0) { aggiornaSliderWPlug(sliderWPlugDrag, (float)GET_X_LPARAM(l)); return true; }
                if (!dentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, v.fin, 0}; TrackMouseEvent(&tm); dentro = true; }
                const int s = zonaIn((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
                if (s != sopra) { sopra = s; disegna(); }
                return true;
            }
            case WM_MOUSELEAVE: dentro = false; if (trascina < 0 && sliderWPlugDrag < 0) sopra = -1; disegna(); return true;
            case WM_LBUTTONDOWN: {   // su un cursore: si trascina (anche fuori dal binario, finche' non si lascia)
                if (morph) return true;
                const int z = zonaIn((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
                if (z >= Z_CURSORE && z < Z_CURSORE + (int)cursori.size()) {
                    trascina = z - Z_CURSORE; SetCapture(v.fin); valoreDa(trascina, (float)GET_X_LPARAM(l));
                } else if (z >= Z_W_CFG_SLIDER && z < Z_W_CFG_SLIDER + (int)wplugin::MAX_CONFIG) {
                    sliderWPlugDrag = z - Z_W_CFG_SLIDER;
                    SetCapture(v.fin);
                    aggiornaSliderWPlug(sliderWPlugDrag, (float)GET_X_LPARAM(l));
                    vetro::battito(v.fin, 1);
                    disegna();
                }
                return true;
            }
            case WM_LBUTTONUP:
                if (trascina >= 0) { trascina = -1; ReleaseCapture(); salva(); disegna(); return true; }
                if (sliderWPlugDrag >= 0) { sliderWPlugDrag = -1; ReleaseCapture(); disegna(); return true; }
                if (morph) return true;
                clic(zonaIn((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l)));
                return true;
            case WM_CAPTURECHANGED:
                if (trascina >= 0) { trascina = -1; salva(); disegna(); }
                if (sliderWPlugDrag >= 0) { sliderWPlugDrag = -1; disegna(); }
                return true;
            case WM_KEYDOWN:
                if (w == VK_ESCAPE) chiudi();
                else if (w == VK_DOWN || w == VK_UP) vaiASezione((sezione + (w == VK_DOWN ? 1 : SEZIONI - 1)) % SEZIONI);   // frecce: sezioni
                return true;
            case WM_CLOSE: chiudi(); return true;
            case WM_MOUSEWHEEL:   // sezione Cursore: l'elenco dei pacchetti scorre (non ci stanno tutti: R20261114-141207-S7)
                if (sezione != S_CURSORE && !morph) {   // le altre: tutta la sezione, una riga (60 px) per scatto
                    const float prima = scorriSez;
                    scorriSez = std::min(std::max(scorriSez - GET_WHEEL_DELTA_WPARAM(w) * 60.0f * v.dpi / WHEEL_DELTA, 0.0f), massimoSez);
                    if (scorriSez != prima) {
                        disegna();   // (rifa' le zone allo scorrimento nuovo)
                        POINT pm; GetCursorPos(&pm); ScreenToClient(v.fin, &pm);
                        const int s = dentro ? zonaIn((float)pm.x, (float)pm.y) : -1;
                        if (s != sopra) { sopra = s; disegna(); }   // sotto il mouse ora c'e' un'altra riga
                    }
                } else if (sezione == S_CURSORE && !morph) {
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
            case WM_SFONDO_FATTO: {   // lo sfondo scelto e' stato messo (o no): vedi scegliSfondo
                if (!consegna::riprendi(hw, m, (const void*)l)) return true;   // (non consegnato da sfondi.h: ignorato)
                auto* cb = (std::function<void(bool)>*)l;
                (*cb)(w != 0); delete cb;
                return true;
            }
            case WM_CURSORI_INSTALLATI: {
                if (!consegna::riprendi(hw, m, (const void*)l)) return true;   // (non consegnato dal lavoro di installazione)
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
    enum { S_GENERALE, S_DOCK, S_WIDGET, S_ANIMAZIONI, S_NOTCH, S_TASTIERA, S_CURSORE, SEZIONI };
    // nome e spiegazione in italiano e in inglese, sezione
    struct Voce { const wchar_t *nome, *name, *spiega, *explain; bool Valori::*campo; int sezione; };
    const std::vector<Voce> voci = {
        {L"Nascondi automaticamente", L"Auto-hide",
         L"Spento: resta su anche sopra le finestre ingrandite", L"Off: it stays up over maximized windows too", &Valori::dockAuto, S_DOCK},
        {L"Finestre ingrandite sopra il dock", L"Maximized windows above the dock",
         L"Come la barra di Windows: si fermano sopra. Spento: fino in fondo", L"Like the Windows taskbar: they stop above. Off: full screen", &Valori::dockSpazio, S_DOCK},
        {L"Segue il mouse sugli altri schermi", L"Follows the mouse to other screens",
         L"Mouse in basso su un altro schermo. Spento: solo sul principale",
         L"Mouse on the bottom edge of another screen. Off: main only", &Valori::dockSchermi, S_DOCK},
        {L"Anteprime delle finestre", L"Window previews",
         L"Mouse fermo su un'icona: le sue finestre dal vivo", L"Hover an icon: its windows, live", &Valori::dockAnteprime, S_DOCK},
        {L"Monitor AI", L"AI Monitor",
         L"Widget nel dock per stato, contesto e avvio rapido degli agenti AI", L"Dock widget for live AI agent status, context usage and quick launch", &Valori::dockAI, S_WIDGET},
        {L"Logitech", L"Logitech",
         L"Widget nel dock per batteria e stato dei dispositivi Logitech G HUB", L"Dock widget for Logitech G HUB battery levels and connected gear", &Valori::dockLogi, S_WIDGET},
        {L"Finestre di gelatina", L"Jelly windows",
         L"Le finestre si deformano mentre le trascini", L"Windows wobble while you drag them", &Valori::gelatina, S_ANIMAZIONI},
        {L"Avvia con Windows", L"Start with Windows",
         L"FluidWin si apre da solo quando accedi", L"FluidWin opens by itself when you sign in", &Valori::avvio, S_GENERALE},
        {L"Cerca aggiornamenti", L"Check for updates",
         L"All'avvio chiede a GitHub se c'è una versione nuova", L"At startup it asks GitHub for a new version", &Valori::aggiornamenti, S_GENERALE},
        {L"Riduzione a icona animata", L"Animated minimize",
         L"Le finestre entrano nel dock ed escono da lì", L"Windows go into the dock and come back out of it", &Valori::genio, S_ANIMAZIONI},
        {L"Animazione di apertura", L"Open animation",
         L"Le finestre aperte da Start o dalla ricerca crescono dal centro", L"Windows opened from Start or search grow from the center", &Valori::apertura, S_ANIMAZIONI},
        {L"Animazione di chiusura", L"Close animation",
         L"La finestra si rimpicciolisce e svanisce sul posto", L"The window shrinks and fades away in place", &Valori::chiusura, S_ANIMAZIONI},
        {L"Affiancamento di FluidWin", L"FluidWin snapping",
         L"Bordi dello schermo e Win+frecce, al posto dello Snap di Windows", L"Screen edges and Win+arrows, instead of Windows Snap", &Valori::affianca, S_ANIMAZIONI},
        {L"Onda di luce della ricerca", L"Search light wave",
         L"I bordi dello schermo si accendono di colori", L"The edges of the screen light up with color", &Valori::onda, S_ANIMAZIONI},
        {L"Notch", L"Notch",
         L"Sempre visibile. Spento: compare solo quando serve", L"Always visible. Off: it appears only when needed", &Valori::notch, S_NOTCH},
        {L"Ora e data nel dock", L"Time and date in the dock",
         L"In fondo al dock, dopo il centro di controllo", L"At the end of the dock, after the control center", &Valori::oraDock, S_NOTCH},
        {L"Notifiche di FluidWin", L"FluidWin notifications",
         L"Escono dal notch, con il numero sulle icone del dock", L"They come out of the notch, with badges on the dock", &Valori::banner, S_NOTCH},
        {L"Volume nel notch", L"Volume in the notch",
         L"I tasti del volume mostrano il livello nel notch", L"The volume keys show the level in the notch", &Valori::volume, S_NOTCH},
        {L"Il tasto Win apre le app", L"Win key opens the launcher",
         L"Al posto del menu Start di Windows", L"Instead of the Windows Start menu", &Valori::tastoWin, S_TASTIERA},
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
    IDWriteTextFormat *fTitolo = nullptr, *fVoce = nullptr, *fSotto = nullptr, *fSezione = nullptr, *fCentro = nullptr, *fMarchio = nullptr, *fPiccolo = nullptr;
    float fDpi = 0;   // scala con cui sono stati fatti i formati qui sopra
    bool aperta = false, dentro = false;
    // crescita dal centro di controllo e rientro (vedi apri): morph 1 cresce, -1 rientra, 0 fermo; mx 0..1 avanzamento
    // (0 = rettangolo del centro rDa, 1 = impostazioni rA); rientra: curva del rientro (vedi forma)
    bool daCentro = false, rientra = false, fuori = false, fuoriDopo = false;   // fuori: si chiudono nell'icona (vedi chiudiFuori)
    bool daPunto = false;   // cresciute da un punto (menu, ricerca): si chiudono rientrandoci (vedi chiudi)
    int morph = 0;
    double mx = 1;
    ULONGLONG tM = 0;
    RECT rDa{}, rA{};
    static constexpr double DURATA_CRESCE = 340, DURATA_RIENTRA = 260;   // ms
    int sopra = -1, sezione = S_GENERALE;
    std::wstring widgetAperto;
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

    // app nascoste dal dock (PR #20): "Mostra tutte", poi una riga per app da Z_MOSTRA_APP in su, senza limite (prima 651..699,
    // e dalla cinquantesima le righe non c'erano: R20261120-022133-S4)
    static constexpr int Z_MOSTRA_TUTTE = 650, Z_WPLUG_CARTELLA = 800, Z_WPLUG_ESEMPIO = 801, Z_MOSTRA_APP = 10000, Z_WPLUG_APP = 20000;
    static constexpr int Z_W_INDIETRO = 900, Z_W_APRI_AI = 901, Z_W_APRI_LOGI = 902, Z_W_MOSTRA_DOCK = 903,
                         Z_W_AI_AVVISI = 904, Z_W_AI_HOOK_AGY = 905, Z_W_AI_HOOK_CLAUDE = 906, Z_W_AI_CARTELLA = 907,
                         Z_W_LOGI_ANIM = 908, Z_W_LOGI_GHUB = 909, Z_W_WPLUG_CARTELLA = 910, Z_W_WPLUG_JSON = 911,
                         Z_W_LOGI_GLOW = 912,
                         Z_W_APRI_WPLUG = 920, Z_W_CFG_TOGGLE = 960, Z_W_CFG_OPT = 1000, Z_W_CFG_SLIDER = 1160;
    struct GlowSwatch { DWORD rgb; const wchar_t* nome; };
    static constexpr GlowSwatch LOGI_GLOW_PALETTE[] = {
        {0x000000, L"Auto"},
        {0x30D158, L"Emerald"},
        {0x32D2FF, L"Cyan"},
        {0x0A84FF, L"Blue"},
        {0xBF5AF2, L"Purple"},
        {0xFF9F0A, L"Amber"},
        {0xFF453A, L"Coral"},
    };
    static constexpr int N_LOGI_GLOW = (int)(sizeof(LOGI_GLOW_PALETTE) / sizeof(LOGI_GLOW_PALETTE[0]));
    int sliderWPlugDrag = -1;
    D2D1_RECT_F sliderWPlugTrack[wplugin::MAX_CONFIG]{};
    void aggiornaSliderWPlug(int idx, float mouseX) {
        if (idx < 0 || idx >= (int)wplugin::MAX_CONFIG) return;
        if (widgetAperto.rfind(L"wplugin:", 0) != 0 || !dettagliWPlugin) return;
        const std::wstring wid = widgetAperto.substr(8);
        wplugin::Widget w;
        if (!dettagliWPlugin(wid, w) || (size_t)idx >= w.config.size()) return;
        const wplugin::ConfigVoce& cfg = w.config[(size_t)idx];
        if (cfg.tipo != L"slider") return;
        const D2D1_RECT_F& tr = sliderWPlugTrack[idx];
        const float width = tr.right - tr.left;
        if (width <= 1.0f) return;
        const double frac = std::clamp((double)(mouseX - tr.left) / (double)width, 0.0, 1.0);
        const double rawVal = cfg.minNum + frac * (cfg.maxNum - cfg.minNum);
        const double val = wplugin::dettagli::snapSlider(rawVal, cfg.minNum, cfg.maxNum, cfg.stepNum);
        if (std::abs(val - cfg.valNum) > 1e-6 && impostaConfigWPluginNum) {
            impostaConfigWPluginNum(wid, cfg.chiave, val);
            disegna();
        }
    }
    // ---- cursori (sezione Cursore): 700 la zona del pacchetto, 710 quelli di Windows, 720+i i pacchetti, 760+i rimuovi
    static constexpr UINT WM_CURSORI_INSTALLATI = WM_APP + 50;
    // ---- sfondi (Generale, Fase 3): Z_SFONDO "Il tuo sfondo", Z_SFONDO + 1 + i gli sfondi di FluidWin (sfondi.h)
    static constexpr UINT WM_SFONDO_FATTO = WM_APP + 51;
    static constexpr int Z_SFONDO = 170;
    ID2D1Bitmap* miniSfondi[sfondi::N + 1] = {};   // [0] il tuo, poi i nostri; fatte per il render target di giroSfondi
    unsigned giroSfondi = ~0u;
    int sfondoAttuale = -2;   // -2 da leggere, -1 il tuo (o un altro), 0..N-1 uno di FluidWin
    int sfondoInCorso = -2;   // quello che si sta mettendo (-2 nessuno)
    ULONGLONG sfondoDa = 0;   // da quando (vedi scegliSfondo) 
    unsigned sfondoRichiesta = 0;   // numero dell'ultima scelta (vedi scegliSfondo)
    bool sfondoErrore = false;
    bool provataMini[sfondi::N + 1] = {};      // miniatura gia' provata (anche senza riuscirci)
    bool tuoTinta = false; DWORD coloreTuo = 0;   // il tuo sfondo e' una tinta unita: il suo colore
    void liberaSfondi() { for (auto*& b : miniSfondi) if (b) { b->Release(); b = nullptr; } for (bool& p : provataMini) p = false; }
    // sceglie lo sfondo i (-1 il tuo); lo mette un thread (sfondi::scegli), la risposta arriva con WM_SFONDO_FATTO.
    // Uno alla volta; se la risposta si perde (finestra distrutta, coda piena) dopo 15 s se ne puo' scegliere un altro: i
    // lavori restano in fila (sfondi::scegli) e conta solo la risposta dell'ultimo
    // (R20261120-210024-S5)
    void scegliSfondo(int i) {
        if (registro) { char b[96]; snprintf(b, 96, "sfondo: scelto %d (attuale %d, in corso %d)", i, sfondoAttuale, sfondoInCorso); registro(b); }
        if ((sfondoInCorso != -2 && vetro::ms() - sfondoDa < 15000) || i == sfondoAttuale) return;   // (quello che c'e' gia': niente)
        if (i == -1 && !tuoDisponibile()) return;
        sfondoInCorso = i; sfondoErrore = false; sfondoDa = vetro::ms();
        const unsigned n = ++sfondoRichiesta;   // (la risposta di una richiesta superata non cambia lo stato: R20261120-211010-S3)
        if (!sfondi::scegli(i, v.fin, WM_SFONDO_FATTO, [this, i, n](bool ok) {
                if (n != sfondoRichiesta) { if (registro) registro("sfondo: risposta di una scelta superata"); return; }
                sfondoInCorso = -2;
                if (registro) { char b[64]; snprintf(b, 64, "sfondo: %d %s", i, ok ? "messo" : "NON messo"); registro(b); }
                if (ok) sfondoAttuale = i; else sfondoErrore = true;
                if (ok && i >= 0) { provataMini[0] = false; if (miniSfondi[0]) { miniSfondi[0]->Release(); miniSfondi[0] = nullptr; } }   // ("il tuo" ora e' quello ricordato)
                vetro::battito(v.fin, 1);
                disegna();
            })) { sfondoInCorso = -2; sfondoErrore = true; }
        disegna();
    }
    // il "tuo sfondo": quello ricordato, o quello di adesso se non e' di FluidWin. Immagine per la miniatura (vuota:
    // nessuna) e, se e' una tinta unita, il colore
    sfondi::Stato statoTuo() const {
        sfondi::Stato t = sfondi::tuo();
        if (t.tipo == sfondi::NESSUNO && sfondoAttuale == -1) sfondi::leggi(t);
        return t;
    }
    std::wstring fileTuo() const {
        const sfondi::Stato t = statoTuo();
        std::wstring f = t.immagine();
        // (il file di partenza puo' non esserci piu': Windows ne tiene una copia dello sfondo mostrato)
        if (f.empty() && t.tipo == sfondi::IMMAGINI && sfondoAttuale == -1) f = sfondi::copiaDiWindows();
        return f;
    }
    bool tuoDisponibile() const { return sfondi::tuo().tipo != sfondi::NESSUNO || sfondoAttuale == -1; }
    static constexpr int Z_PACCHETTO = 700, Z_WINDOWS = 710, Z_SCEGLI = 720, Z_RIMUOVI = 760, MAX_PACCHETTI = 30;
    std::vector<cursori::Pacchetto> pacchetti;
    std::vector<ID2D1Bitmap*> anteprime;   // la freccia di ogni pacchetto, fatta per il render target di giroAnteprime
    unsigned giroAnteprime = ~0u;
    bool installando = false, erroreCursori = false;
    int scorriCursori = 0;   // prima riga mostrata dell'elenco (0 = "Cursori di Windows"): rotella del mouse
    int rotella = 0;         // scatti piccoli della rotella (touchpad, rotelle ad alta precisione) messi da parte
    // le altre sezioni scorrono intere quando non ci stanno (Dock: 4 interruttori, 3 cursori e il colore; con
    // l'interruttore delle anteprime l'ultima riga usciva dalla finestra). Pixel scrollati e quanti se ne possono
    // scrollare, misurati all'ultimo disegno della sezione
    float scorriSez = 0, massimoSez = 0;
    std::wstring statoCursori;

    void liberaAnteprime() { for (auto* b : anteprime) if (b) b->Release(); anteprime.clear(); }
    // (oltre il limite resta comunque quello in uso: prima un pacchetto installato per ultimo in ordine alfabetico era
    // attivo ma non compariva, e non si poteva togliere, R20261121-002419-S16)
    void rileggiPacchetti() {
        pacchetti = cursori::elenco();
        if ((int)pacchetti.size() > MAX_PACCHETTI) {
            const auto it = std::find_if(pacchetti.begin(), pacchetti.end(), [](const cursori::Pacchetto& p) { return p.nome == valori().cursore; });
            const bool fuori = it != pacchetti.end() && it - pacchetti.begin() >= MAX_PACCHETTI;
            cursori::Pacchetto attivo; if (fuori) attivo = *it;
            pacchetti.resize(MAX_PACCHETTI);
            if (fuori) pacchetti.back() = std::move(attivo);
        }
        liberaAnteprime();
    }

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
                if (!consegna::posta(l->fin, WM_CURSORI_INSTALLATI, 0, e)) delete e;
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

    void apriPaginaWidget(std::wstring id) {
        widgetAperto = std::move(id);
        sliderWPlugDrag = -1;
        kSez = 0; tSez = vetro::ms(); scorriSez = 0; massimoSez = 0;
        vetro::battito(v.fin, 1);
        disegna();
    }

    void vaiASezione(int s) {
        if (s == sezione && widgetAperto.empty()) return;
        sezione = s; widgetAperto.clear(); sliderWPlugDrag = -1; kSez = 0; tSez = vetro::ms(); scorriSez = 0; massimoSez = 0;
        vetro::battito(v.fin, 1);
        disegna();
    }

    void clic(int id) {
        if (id < 0) return;
        if (id == 200) { chiudi(); return; }
        if (id >= Z_CURSORE && id < Z_COLORE) return;   // (i cursori si usano trascinando: WM_LBUTTONDOWN)
        if (id >= Z_W_CFG_SLIDER && id < Z_W_CFG_SLIDER + (int)wplugin::MAX_CONFIG) return;
        if (id >= Z_COLORE && id <= Z_COLORE_MIO) {
            scegliColore(id);
            salva();
            if (cambiato) cambiato();
            disegna();
            return;
        }
        if (id == Z_W_INDIETRO) { apriPaginaWidget(L""); return; }
        if (id == Z_W_APRI_AI) { apriPaginaWidget(L"ai"); return; }
        if (id == Z_W_APRI_LOGI) { apriPaginaWidget(L"logitech"); return; }
        if (id >= Z_W_APRI_WPLUG && id < Z_W_APRI_WPLUG + 30) {
            const auto wp = elencoWPlugin ? elencoWPlugin() : std::vector<std::pair<std::wstring, std::pair<std::wstring, bool>>>{};
            const size_t k = (size_t)(id - Z_W_APRI_WPLUG);
            if (k < wp.size()) apriPaginaWidget(L"wplugin:" + wp[k].first);
            return;
        }
        if (id == Z_W_MOSTRA_DOCK) {
            if (widgetAperto == L"ai") {
                valori().dockAI = !valori().dockAI;
                salva();
                if (cambiato) cambiato();
            } else if (widgetAperto == L"logitech") {
                valori().dockLogi = !valori().dockLogi;
                salva();
                if (cambiato) cambiato();
            } else if (widgetAperto.rfind(L"wplugin:", 0) == 0) {
                const std::wstring wid = widgetAperto.substr(8);
                wplugin::Widget w;
                if (dettagliWPlugin && dettagliWPlugin(wid, w) && impostaWPlugin) impostaWPlugin(wid, !w.abilitato);
            }
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id == Z_W_AI_AVVISI) {
            if (leggiAIAvvisi && impostaAIAvvisi) impostaAIAvvisi(!leggiAIAvvisi());
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id == Z_W_AI_HOOK_AGY) {
            if (hookAgyStato && hookAgyImposta) hookAgyImposta(!hookAgyStato());
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id == Z_W_AI_HOOK_CLAUDE) {
            if (hookClaudeStato && hookClaudeImposta) hookClaudeImposta(!hookClaudeStato());
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id == Z_W_AI_CARTELLA) {
            if (apriCartellaHookAI) apriCartellaHookAI();
            return;
        }
        if (id == Z_W_LOGI_ANIM) {
            valori().logiAnimCarica = !valori().logiAnimCarica;
            salva();
            if (cambiato) cambiato();
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id >= Z_W_LOGI_GLOW && id < Z_W_LOGI_GLOW + N_LOGI_GLOW) {
            valori().logiGlowColore = LOGI_GLOW_PALETTE[id - Z_W_LOGI_GLOW].rgb;
            salva();
            if (cambiato) cambiato();
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id == Z_W_LOGI_GHUB) {
            if (apriGHub) apriGHub();
            return;
        }
        if (id == Z_W_WPLUG_CARTELLA) {
            if (apriCartellaWPlugin) apriCartellaWPlugin();
            return;
        }
        if (id == Z_W_WPLUG_JSON) {
            if (widgetAperto.rfind(L"wplugin:", 0) == 0 && dettagliWPlugin) {
                wplugin::Widget w;
                if (dettagliWPlugin(widgetAperto.substr(8), w) && !w.filePath.empty()) {
                    ShellExecuteW(nullptr, L"open", w.filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return;
        }
        if (id >= Z_W_CFG_TOGGLE && id < Z_W_CFG_TOGGLE + (int)wplugin::MAX_CONFIG) {
            if (widgetAperto.rfind(L"wplugin:", 0) == 0 && dettagliWPlugin && impostaConfigWPluginBool) {
                const std::wstring wid = widgetAperto.substr(8);
                wplugin::Widget w;
                const size_t idx = (size_t)(id - Z_W_CFG_TOGGLE);
                if (dettagliWPlugin(wid, w) && idx < w.config.size() && w.config[idx].tipo == L"toggle") {
                    impostaConfigWPluginBool(wid, w.config[idx].chiave, !w.config[idx].valBool);
                }
            }
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id >= Z_W_CFG_OPT && id < Z_W_CFG_OPT + (int)wplugin::MAX_CONFIG * 8) {
            if (widgetAperto.rfind(L"wplugin:", 0) == 0 && dettagliWPlugin && impostaConfigWPluginStr) {
                const std::wstring wid = widgetAperto.substr(8);
                wplugin::Widget w;
                const int rel = id - Z_W_CFG_OPT;
                const size_t idx = (size_t)(rel / 8);
                const size_t j = (size_t)(rel % 8);
                if (dettagliWPlugin(wid, w) && idx < w.config.size() &&
                    (w.config[idx].tipo == L"choice" || w.config[idx].tipo == L"color") &&
                    j < w.config[idx].opzioni.size()) {
                    impostaConfigWPluginStr(wid, w.config[idx].chiave, w.config[idx].opzioni[j]);
                }
            }
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id == Z_WPLUG_CARTELLA) {
            if (apriCartellaWPlugin) apriCartellaWPlugin();
            return;
        }
        if (id == Z_WPLUG_ESEMPIO) {
            if (creaEsempioWPlugin) creaEsempioWPlugin();
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id >= Z_WPLUG_APP) {
            const auto wp = elencoWPlugin ? elencoWPlugin() : std::vector<std::pair<std::wstring, std::pair<std::wstring, bool>>>{};
            const size_t k = (size_t)(id - Z_WPLUG_APP);
            if (k < wp.size() && impostaWPlugin) impostaWPlugin(wp[k].first, !wp[k].second.second);
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id == Z_MOSTRA_TUTTE) {
            if (mostraNascosta) mostraNascosta(L"");
            vetro::battito(v.fin, 1);
            disegna();
            return;
        }
        if (id >= Z_MOSTRA_APP && id < Z_WPLUG_APP) {
            const auto nasc = elencoNascoste ? elencoNascoste() : std::vector<std::pair<std::wstring, std::wstring>>{};
            const size_t k = (size_t)(id - Z_MOSTRA_APP);
            if (k < nasc.size() && mostraNascosta) mostraNascosta(nasc[k].first);
            vetro::battito(v.fin, 1);
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
        if (id >= Z_SFONDO && id <= Z_SFONDO + sfondi::N) { scegliSfondo(id - Z_SFONDO - 1); return; }   // sfondo (Fase 3)
        if (id == 401) { if (esci) esci(); return; }   // "Esci da FluidWin" (#16)
        if (id == 400) {   // diagnostica negli appunti
            if (copiaDiagnostica && copiaDiagnostica()) { copiataAlle = vetro::ms(); vetro::battito(v.fin, 1); }
            disegna();
            return;
        }
        if (id >= 300 && id < 300 + SEZIONI) { vaiASezione(id - 300); return; }
        if (id == 120 || id == 121) valori().riduzione = id - 120;   // stile della riduzione a icona (1.3.0)
        else if (id >= 130 && id <= 132) valori().angoli = id - 130;   // angoli (1.3.0, Fase 3)
        else if (id >= 140 && id <= 143) valori().dockLato = id - 140;   // posizione del dock (1.3.0, Fase 3)
        else if (id >= 100 && id < 100 + testi::N_LINGUE) { valori().lingua = id - 100; testi::lingua() = valori().lingua; }
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
            case S_DOCK: return testi::t(L"Dock", L"Dock");
            case S_WIDGET: return testi::t(L"Widget", L"Widgets");
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
            case S_WIDGET:     // griglia 2x2 di widget
                v.rt->DrawRoundedRectangle({{cx - 8 * d, cy - 8 * d, cx - 1.5f * d, cy - 1.5f * d}, 2 * d, 2 * d}, v.pen, sp);
                v.rt->DrawRoundedRectangle({{cx + 1.5f * d, cy - 8 * d, cx + 8 * d, cy - 1.5f * d}, 2 * d, 2 * d}, v.pen, sp);
                v.rt->DrawRoundedRectangle({{cx - 8 * d, cy + 1.5f * d, cx - 1.5f * d, cy + 8 * d}, 2 * d, 2 * d}, v.pen, sp);
                v.rt->DrawRoundedRectangle({{cx + 1.5f * d, cy + 1.5f * d, cx + 8 * d, cy + 8 * d}, 2 * d, 2 * d}, v.pen, sp);
                break;
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
        aperta = false; daCentro = false; daPunto = false; morph = 0; mx = 1; rientra = false; fuori = false; fuoriDopo = false; sliderWPlugDrag = -1;
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
        wplugin::Widget wpSub;
        bool widgetSubAperto = false;
        if (sezione == S_WIDGET && !widgetAperto.empty()) {
            if (widgetAperto == L"ai" || widgetAperto == L"logitech") {
                widgetSubAperto = true;
            } else if (widgetAperto.rfind(L"wplugin:", 0) == 0) {
                const std::wstring wid = widgetAperto.substr(8);
                if (dettagliWPlugin && dettagliWPlugin(wid, wpSub)) widgetSubAperto = true;
                else { widgetAperto.clear(); sliderWPlugDrag = -1; }
            } else {
                widgetAperto.clear();
            }
        }
        if (widgetSubAperto) {
            const std::wstring tbBack = L"\u2039 " + std::wstring(testi::t(L"Widget", L"Widgets"));
            const float lwBack = v.larghezza(tbBack, fCentro) + 22 * d;
            const D2D1_RECT_F rBack = {L, 21 * d, L + lwBack, 49 * d};
            zone.push_back({rBack, Z_W_INDIETRO});
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (sopra == Z_W_INDIETRO ? 0.16f : 0.08f) * (float)e)
                                    : D2D1::ColorF(0, 0, 0, (sopra == Z_W_INDIETRO ? 0.10f : 0.05f) * (float)e));
            v.rt->FillRoundedRectangle({rBack, 14 * d, 14 * d}, v.pen);
            v.testo(tbBack, fCentro, rBack, inkS);

            std::wstring subTit, subSotto;
            if (widgetAperto == L"ai") {
                subTit = testi::t(L"Monitor AI", L"AI Monitor");
                subSotto = testi::t(L"Stato, notifiche e hook degli agenti AI", L"AI agent status, notifications and CLI hooks");
            } else if (widgetAperto == L"logitech") {
                subTit = L"Logitech";
                subSotto = testi::t(L"Dispositivi Logitech G HUB nel dock", L"Logitech G HUB devices in the dock");
            } else {
                subTit = wpSub.nome;
                subSotto = !wpSub.sottotitolo.empty() ? wpSub.sottotitolo : (wpSub.id + L".json");
            }
            v.testo(subTit, fTitolo, {rBack.right + 12 * d, 18 * d, R - 50 * d, 50 * d}, inkS);
            v.testo(subSotto, fSotto, {L, 52 * d, R - 16 * d, 70 * d}, tenueS);
        } else {
            v.testo(nomeSezione(sezione), fTitolo, {L, 20 * d, R - 50 * d, 54 * d}, inkS);
        }
        // le righe scorrono sotto il titolo: ritaglio da qui in giu', e le zone tagliate allo stesso modo (piu' sotto).
        // (la sezione Cursore ha il suo scorrimento, a righe dell'elenco dei pacchetti)
        const bool scorre = sezione != S_CURSORE;
        const float cimaSez = widgetSubAperto ? 74 * d : 62 * d, y0 = widgetSubAperto ? 82 * d : 72 * d;
        const size_t zona0 = zone.size();
        if (scorre) v.rt->PushAxisAlignedClip({B, cimaSez, W, H}, D2D1_ANTIALIAS_MODE_ALIASED);
        float y = y0 - (scorre ? scorriSez : 0);
        // app nascoste dal dock (PR #20): il loro elenco in fondo alla sezione Dock
        const auto nascDock = (sezione == S_DOCK && elencoNascoste) ? elencoNascoste() : std::vector<std::pair<std::wstring, std::wstring>>{};
        const int nNascVis = (int)nascDock.size();
        auto aggiungiZona = [&](D2D1_RECT_F r, int id) { zone.push_back({r, id}); };   // (le taglia lo scorrimento, piu' sotto)
        auto riquadro = [&](const D2D1_RECT_F& r, bool evid) {
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (evid ? 0.10f : 0.06f) * (float)e) : D2D1::ColorF(1, 1, 1, (evid ? 0.75f : 0.55f) * (float)e));
            v.rt->FillRoundedRectangle({r, rr, rr}, v.pen);
        };
        auto frecciaWidget = [&](float cx, float cy, bool evid) {
            v.pen->SetColor(evid ? inkS : tenueS);
            v.rt->DrawLine({cx - 2.5f * d, cy - 4.5f * d}, {cx + 2.0f * d, cy}, v.pen, 1.5f * d);
            v.rt->DrawLine({cx + 2.0f * d, cy}, {cx - 2.5f * d, cy + 4.5f * d}, v.pen, 1.5f * d);
        };
        auto pulsantePill = [&](const D2D1_RECT_F& rb, int zid, const std::wstring& testo, bool primario) {
            aggiungiZona(rb, zid);
            if (primario) {
                v.pen->SetColor(D2D1::ColorF(blu.r, blu.g, blu.b, (sopra == zid ? 1.0f : 0.88f) * (float)e));
            } else {
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (sopra == zid ? 0.16f : 0.08f) * (float)e)
                                        : D2D1::ColorF(0, 0, 0, (sopra == zid ? 0.10f : 0.05f) * (float)e));
            }
            v.rt->FillRoundedRectangle({rb, 15 * d, 15 * d}, v.pen);
            v.testo(testo, fCentro, rb, primario ? D2D1::ColorF(1, 1, 1, (float)e) : inkS);
        };
        auto rigaLevetta = [&](int zid, const std::wstring& tit, const std::wstring& sub, float kAnim) {
            const D2D1_RECT_F r = {L, y, R, y + 54 * d};
            aggiungiZona(r, zid);
            riquadro(r, sopra == zid);
            v.testo(tit, fVoce, {r.left + 16 * d, r.top + 7 * d, r.right - 74 * d, r.top + 29 * d}, inkS);
            v.testo(sub, fSotto, {r.left + 16 * d, r.top + 28 * d, r.right - 74 * d, r.top + 48 * d}, tenueS);
            levetta(r.right - 58 * d, (r.top + r.bottom) / 2, kAnim, (float)e);
            y += 60 * d;
        };
        // riga informativa (senza interruttore); spazio: quanto lasciare libero a destra per un pulsante
        auto riga = [&](const std::wstring& titolo, const std::wstring& testo, float spazio = 0) {
            const D2D1_RECT_F r = {L, y, R, y + 54 * d};
            riquadro(r, false);
            v.testo(titolo, fVoce, {r.left + 16 * d, r.top + 7 * d, r.right - 16 * d - spazio, r.top + 29 * d}, inkS);
            v.testo(testo, fSotto, {r.left + 16 * d, r.top + 28 * d, r.right - 16 * d - spazio, r.top + 48 * d}, tenueS);
            y += 60 * d;
        };
        auto rigaConPulsante = [&](int zid, const std::wstring& tit, const std::wstring& sub, const std::wstring& btnTesto, bool primario) {
            const float yr = y;
            const float lw = v.larghezza(btnTesto, fCentro) + 26 * d;
            riga(tit, sub, lw + 12 * d);
            const D2D1_RECT_F rb = {R - 16 * d - lw, yr + 12 * d, R - 16 * d, yr + 42 * d};
            pulsantePill(rb, zid, btnTesto, primario);
        };
        auto animCampo = [&](bool Valori::*campo) -> float {
            for (size_t i = 0; i < voci.size(); i++) {
                if (voci[i].campo == campo && i < anim.size()) return (float)anim[i];
            }
            return (valori().*campo) ? 1.0f : 0.0f;
        };
        if (!widgetSubAperto) {
            for (size_t i = 0; i < voci.size(); i++) {
                if (voci[i].sezione != sezione) continue;
                const D2D1_RECT_F r = {L, y, R, y + 54 * d};
                if (sezione == S_WIDGET) {
                    const int openId = (voci[i].campo == &Valori::dockAI) ? Z_W_APRI_AI
                                     : (voci[i].campo == &Valori::dockLogi) ? Z_W_APRI_LOGI : -1;
                    const float toggleL = r.right - 66 * d;
                    if (openId >= 0) aggiungiZona({r.left, r.top, toggleL, r.bottom}, openId);
                    aggiungiZona({openId >= 0 ? toggleL : r.left, r.top, r.right, r.bottom}, (int)i);
                    riquadro(r, sopra == (int)i || (openId >= 0 && sopra == openId));
                    const float textR = openId >= 0 ? (r.right - 94 * d) : (r.right - 74 * d);
                    v.testo(testi::t(voci[i].nome, voci[i].name), fVoce, {r.left + 16 * d, r.top + 7 * d, textR, r.top + 29 * d}, inkS);
                    v.testo(testi::t(voci[i].spiega, voci[i].explain), fSotto, {r.left + 16 * d, r.top + 28 * d, textR, r.top + 48 * d}, tenueS);
                    if (openId >= 0) frecciaWidget(r.right - 76 * d, (r.top + r.bottom) / 2, sopra == openId);
                    levetta(r.right - 58 * d, (r.top + r.bottom) / 2, (float)anim[i], (float)e);
                } else {
                    aggiungiZona(r, (int)i);
                    riquadro(r, sopra == (int)i);
                    v.testo(testi::t(voci[i].nome, voci[i].name), fVoce, {r.left + 16 * d, r.top + 7 * d, r.right - 74 * d, r.top + 29 * d}, inkS);
                    v.testo(testi::t(voci[i].spiega, voci[i].explain), fSotto, {r.left + 16 * d, r.top + 28 * d, r.right - 74 * d, r.top + 48 * d}, tenueS);
                    levetta(r.right - 58 * d, (r.top + r.bottom) / 2, (float)anim[i], (float)e);
                }
                y += 60 * d;
            }
        }
        if (sezione == S_ANIMAZIONI) {
            // stile della riduzione a icona (1.3.0): il genio, o il riquadro che cade dietro le icone del dock. Con la
            // riduzione animata spenta la scelta resta, un po' spenta, per quando la si riaccende
            const D2D1_RECT_F rl = {L, y, R, y + 64 * d};
            riquadro(rl, false);
            const float ks = valori().genio ? 1.0f : 0.45f;
            const D2D1_COLOR_F inkK = {inkS.r, inkS.g, inkS.b, inkS.a * ks};
            v.testo(testi::t(L"Stile della riduzione", L"Minimize style"), fVoce, {rl.left + 16 * d, rl.top, rl.left + 200 * d, rl.bottom}, inkK);
            const float x0 = rl.left + 210 * d, larg = rl.right - 16 * d - x0, h = 34 * d, yl = rl.top + 15 * d;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)e) : D2D1::ColorF(0, 0, 0, 0.06f * (float)e));
            v.rt->FillRoundedRectangle({{x0, yl, x0 + larg, yl + h}, h / 2, h / 2}, v.pen);
            const wchar_t* nomi[2] = {testi::t(L"Genio", L"Genie"), testi::t(L"Caduta nel dock", L"Drop into the dock")};
            for (int i = 0; i < 2; i++) {
                const D2D1_RECT_F r = {x0 + larg * i / 2 + 3 * d, yl + 3 * d, x0 + larg * (i + 1) / 2 - 3 * d, yl + h - 3 * d};
                zone.push_back({r, 120 + i});
                if (valori().riduzione == i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * (float)e * ks) : D2D1::ColorF(1, 1, 1, 0.95f * (float)e * ks));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                } else if (sopra == 120 + i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                }
                v.testo(nomi[i], fCentro, r, inkK);
            }
            y += 70 * d;
        }
        if (sezione == S_GENERALE) {
            // lingua: sette scelte in un'unica tendina di due file da quattro (i nomi restano ciascuno nella sua lingua);
            // l'ottavo posto dice che altre lingue arriveranno
            const D2D1_RECT_F rl = {L, y, R, y + 98 * d};
            riquadro(rl, false);
            v.testo(testi::t(L"Lingua", L"Language"), fVoce, {rl.left + 16 * d, rl.top, rl.left + 160 * d, rl.bottom}, inkS);
            const float x0 = rl.left + 170 * d, larg = rl.right - 16 * d - x0, h = 34 * d, y0 = rl.top + 15 * d;
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)e) : D2D1::ColorF(0, 0, 0, 0.06f * (float)e));
            v.rt->FillRoundedRectangle({{x0, y0, x0 + larg, y0 + 2 * h}, h / 2, h / 2}, v.pen);
            v.testo(L"Coming soon", fPiccolo, {x0 + larg * 3 / 4 + 3 * d, y0 + h + 3 * d, x0 + larg - 3 * d, y0 + 2 * h - 3 * d}, tenueS);
            for (int i = 0; i < testi::N_LINGUE; i++) {
                const int col = i % 4;
                const int fila = i / 4;
                const float yl = y0 + (float)fila * h;
                const D2D1_RECT_F r = {x0 + larg * col / 4 + 3 * d, yl + 3 * d, x0 + larg * (col + 1) / 4 - 3 * d, yl + h - 3 * d};
                const wchar_t* const nomeL = testi::nome(i);
                zone.push_back({r, 100 + i});
                if (valori().lingua == i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * (float)e) : D2D1::ColorF(1, 1, 1, 0.95f * (float)e));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                } else if (sopra == 100 + i) {
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                    v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                }
                v.testo(nomeL, fCentro, r, inkS);
            }
            y += 104 * d;
            {   // angoli (1.3.0, Fase 3): dock, notch, pannelli, menu e finestre di FluidWin; tre scelte
                const D2D1_RECT_F ra = {L, y, R, y + 64 * d};
                riquadro(ra, false);
                v.testo(testi::t(L"Angoli", L"Corners"), fVoce, {ra.left + 16 * d, ra.top, ra.left + 160 * d, ra.bottom}, inkS);
                const float x0 = ra.left + 170 * d, larg = ra.right - 16 * d - x0, h = 34 * d, yl = ra.top + 15 * d;
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)e) : D2D1::ColorF(0, 0, 0, 0.06f * (float)e));
                v.rt->FillRoundedRectangle({{x0, yl, x0 + larg, yl + h}, h / 2, h / 2}, v.pen);
                const wchar_t* nomi[3] = {testi::t(L"Arrotondati", L"Rounded"), testi::t(L"Poco arrotondati", L"Slightly rounded"), testi::t(L"Squadrati", L"Square")};
                for (int i = 0; i < 3; i++) {
                    const D2D1_RECT_F r = {x0 + larg * i / 3 + 3 * d, yl + 3 * d, x0 + larg * (i + 1) / 3 - 3 * d, yl + h - 3 * d};
                    zone.push_back({r, 130 + i});
                    if (valori().angoli == i) {
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * (float)e) : D2D1::ColorF(1, 1, 1, 0.95f * (float)e));
                        v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                    } else if (sopra == 130 + i) {
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                        v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                    }
                    v.testo(nomi[i], fCentro, r, inkS);
                }
                y += 70 * d;
            }
            {   // sfondo (1.3.0, Fase 3): il tuo e i tre di FluidWin, in fila; un clic lo mette (sfondi.h)
                if (sfondoAttuale == -2) sfondoAttuale = sfondi::attualeNostro();
                if (giroSfondi != v.giroRt) { liberaSfondi(); giroSfondi = v.giroRt; }
                const int n = sfondi::N + 1;
                const float gap = 12 * d, x0 = L + 16 * d, larg = (R - L - 32 * d - gap * (n - 1)) / n, alto = std::round(larg * 9 / 16);
                const D2D1_RECT_F rs = {L, y, R, y + (44 + 30) * d + alto};
                riquadro(rs, false);
                v.testo(testi::t(L"Sfondo", L"Wallpaper"), fVoce, {rs.left + 16 * d, rs.top + 8 * d, rs.left + 200 * d, rs.top + 38 * d}, inkS);
                if (sfondoErrore) v.testo(testi::t(L"Non messo: riprova", L"Not set: try again"), fSotto, {rs.left + 200 * d, rs.top + 8 * d, rs.right - 16 * d, rs.top + 38 * d}, tenueS);
                const float yc = rs.top + 44 * d, rr = std::max(2.0f, 8 * d * raggio::fattore());
                const bool haTuo = tuoDisponibile();
                for (int k = 0; k < n; k++) {
                    const int i = k - 1;   // -1 il tuo, poi 0..N-1
                    const D2D1_RECT_F c = {x0 + k * (larg + gap), yc, x0 + k * (larg + gap) + larg, yc + alto};
                    const bool usabile = i >= 0 || haTuo;
                    if (usabile) zone.push_back({c, Z_SFONDO + k});
                    if (!miniSfondi[k] && !provataMini[k] && usabile) {   // (una prova sola per apertura: niente letture a ogni fotogramma)
                        provataMini[k] = true;
                        const UINT pw = (UINT)std::lround(larg), ph = (UINT)std::lround(alto);
                        if (i >= 0) { const void* dati = nullptr; DWORD nb = 0; if (sfondi::risorsa(i, dati, nb)) miniSfondi[k] = sfondi::miniatura(v.rt, dati, nb, L"", pw, ph); }
                        else {
                            const sfondi::Stato t = statoTuo();
                            tuoTinta = t.tipo == sfondi::TINTA; coloreTuo = t.colore;
                            const std::wstring f = fileTuo(); if (!f.empty()) miniSfondi[k] = sfondi::miniatura(v.rt, nullptr, 0, f, pw, ph);
                        }
                    }
                    const float ea = (float)e * (usabile ? 1.0f : 0.4f);
                    if (miniSfondi[k]) {
                        ID2D1BitmapBrush* bb = nullptr;
                        if (SUCCEEDED(v.rt->CreateBitmapBrush(miniSfondi[k], &bb)) && bb) {
                            bb->SetTransform(D2D1::Matrix3x2F::Translation(c.left, c.top));
                            bb->SetOpacity(ea);
                            v.rt->FillRoundedRectangle({c, rr, rr}, bb);
                            bb->Release();
                        }
                    } else if (i < 0 && tuoTinta) {   // il tuo e' una tinta unita: quel colore
                        v.pen->SetColor(D2D1::ColorF(GetRValue(coloreTuo) / 255.0f, GetGValue(coloreTuo) / 255.0f, GetBValue(coloreTuo) / 255.0f, ea));
                        v.rt->FillRoundedRectangle({c, rr, rr}, v.pen);
                    } else {   // (nessuna immagine: un riquadro vuoto)
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f * ea) : D2D1::ColorF(0, 0, 0, 0.06f * ea));
                        v.rt->FillRoundedRectangle({c, rr, rr}, v.pen);
                    }
                    // scelto: anello blu; sotto il mouse: anello chiaro; mentre si mette: anello tratteggiato... qui pieno e tenue
                    const bool scelto = sfondoAttuale == i, inCorso = sfondoInCorso == i;
                    if (scelto || inCorso || (sopra == Z_SFONDO + k && usabile)) {
                        v.pen->SetColor(scelto ? D2D1::ColorF(blu.r, blu.g, blu.b, (float)e) : inCorso ? D2D1::ColorF(blu.r, blu.g, blu.b, 0.45f * (float)e) : tenueS);
                        const float o = 3 * d;
                        v.rt->DrawRoundedRectangle({{c.left - o, c.top - o, c.right + o, c.bottom + o}, rr + o, rr + o}, v.pen, 2.0f * d);
                    }
                    const wchar_t* nome = i < 0 ? testi::t(L"Il tuo sfondo", L"Your wallpaper") : testi::t(sfondi::ELENCO[i].it, sfondi::ELENCO[i].en);
                    v.testo(nome, fCentro, {c.left, c.bottom + 4 * d, c.right, c.bottom + 26 * d}, usabile ? inkS : tenueS);
                }
                y += rs.bottom - rs.top + 6 * d;
            }
            riga(L"FluidWin" + (ver.empty() ? std::wstring() : L" " + ver), testi::t(L"Windows reimmaginato · github.com/clooudfn/FluidWin", L"Windows reimagined · github.com/clooudfn/FluidWin"));
            // diagnostica: versione, Windows, schermi, scheda video, impostazioni (niente dati personali), per chi
            // segnala un problema; il pulsante "Copia" a destra
            {
                const float yr = y;
                const std::wstring tb = copiataAlle ? testi::t(L"Copiata", L"Copied") : testi::t(L"Copia", L"Copy");
                const float lw = v.larghezza(tb, fCentro) + 30 * d;
                // (il testo si ferma prima del pulsante: prima ci finiva sotto)
                riga(testi::t(L"Diagnostica", L"Diagnostics"), testi::t(L"Sistema, schermi, scheda video e impostazioni, da incollare in una segnalazione",
                                                                         L"System, screens, graphics card and settings, to paste into a report"), lw + 12 * d);
                const D2D1_RECT_F rb = {R - 16 * d - lw, yr + 12 * d, R - 16 * d, yr + 42 * d};
                zone.push_back({rb, 400});
                v.pen->SetColor(copiataAlle ? D2D1::ColorF(0.20f, 0.78f, 0.35f, (float)e) : D2D1::ColorF(blu.r, blu.g, blu.b, (sopra == 400 ? 1.0f : 0.88f) * (float)e));
                v.rt->FillRoundedRectangle({rb, 15 * d, 15 * d}, v.pen);
                v.testo(tb, fCentro, rb, D2D1::ColorF(1, 1, 1, (float)e));
            }
            // uscita (issue #16): con la barra di Windows nascosta l'icona nell'area di notifica non si vede. La
            // scorciatoia d'emergenza la tiene il guardiano e funziona anche con FluidWin bloccato
            {
                const float yr = y;
                const std::wstring tb = testi::t(L"Esci", L"Quit");
                const float lw = v.larghezza(tb, fCentro) + 30 * d;
                riga(testi::t(L"Esci da FluidWin", L"Quit FluidWin"),
                     testi::t(L"Windows torna com'era. Bloccato? Ctrl+Alt+Maiusc+F12",
                              L"Windows goes back as it was. Stuck? Ctrl+Alt+Shift+F12"), lw + 12 * d);
                const D2D1_RECT_F rb = {R - 16 * d - lw, yr + 12 * d, R - 16 * d, yr + 42 * d};
                zone.push_back({rb, 401});
                v.pen->SetColor(D2D1::ColorF(0.92f, 0.26f, 0.22f, (sopra == 401 ? 1.0f : 0.88f) * (float)e));
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
            aggiungiZona({x0 - 10 * d, r.top, x1 + 10 * d, r.bottom}, id);
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
            v.testo(testi::isola(std::to_wstring(val) + c.unita), fCentro, {x1 + 8 * d, r.top, r.right - 8 * d, r.bottom}, inkS);
            y += 60 * d;
        }
        if (sezione == S_DOCK) {
            {   // posizione (1.3.0, Fase 3): il bordo dello schermo dove sta il dock; quattro scelte
                const D2D1_RECT_F ra = {L, y, R, y + 64 * d};
                riquadro(ra, false);
                v.testo(testi::t(L"Posizione", L"Position"), fVoce, {ra.left + 16 * d, ra.top, ra.left + 160 * d, ra.bottom}, inkS);
                const float x0 = ra.left + 170 * d, larg = ra.right - 16 * d - x0, h = 34 * d, yl = ra.top + 15 * d;
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)e) : D2D1::ColorF(0, 0, 0, 0.06f * (float)e));
                v.rt->FillRoundedRectangle({{x0, yl, x0 + larg, yl + h}, h / 2, h / 2}, v.pen);
                const wchar_t* nomi[4] = {testi::t(L"In basso", L"Bottom"), testi::t(L"In alto", L"Top"), testi::t(L"A sinistra", L"Left"), testi::t(L"A destra", L"Right")};
                for (int i = 0; i < 4; i++) {
                    const D2D1_RECT_F r = {x0 + larg * i / 4 + 3 * d, yl + 3 * d, x0 + larg * (i + 1) / 4 - 3 * d, yl + h - 3 * d};
                    zone.push_back({r, 140 + i});
                    if (valori().dockLato == i) {
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * (float)e) : D2D1::ColorF(1, 1, 1, 0.95f * (float)e));
                        v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                    } else if (sopra == 140 + i) {
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                        v.rt->FillRoundedRectangle({r, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                    }
                    v.testo(nomi[i], fCentro, r, inkS);
                }
                y += 70 * d;
            }
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
                aggiungiZona({cx - passo / 2, r.top, cx + passo / 2, r.bottom}, id);
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
            if (nascDock.empty()) {
                riga(testi::t(L"App nascoste dal dock", L"Hidden dock apps"),
                     testi::t(L"Nessuna. Clic destro su un'app aperta nel dock per nasconderla", L"None. Right-click an open app in the dock to hide it"));
            } else {
                const std::wstring subTit = std::to_wstring(nascDock.size()) + testi::t(nascDock.size() == 1 ? L" app nascosta" : L" app nascoste",
                                                                                        nascDock.size() == 1 ? L" hidden app" : L" hidden apps");
                const D2D1_RECT_F rh = {L, y, R, y + 54 * d};
                riquadro(rh, false);
                float limTit = rh.right - 16 * d;
                if (nascDock.size() > 1) {
                    const std::wstring tb = testi::t(L"Mostra tutte", L"Show all");
                    const float lw = v.larghezza(tb, fCentro) + 30 * d;
                    const D2D1_RECT_F rb = {R - 16 * d - lw, y + 12 * d, R - 16 * d, y + 42 * d};
                    aggiungiZona(rb, Z_MOSTRA_TUTTE);
                    v.pen->SetColor(D2D1::ColorF(blu.r, blu.g, blu.b, (sopra == Z_MOSTRA_TUTTE ? 1.0f : 0.88f) * (float)e));
                    v.rt->FillRoundedRectangle({rb, 15 * d, 15 * d}, v.pen);
                    v.testo(tb, fCentro, rb, D2D1::ColorF(1, 1, 1, (float)e));
                    limTit = rb.left - 12 * d;
                }
                v.testo(testi::t(L"App nascoste dal dock", L"Hidden dock apps"), fVoce, {rh.left + 16 * d, rh.top + 7 * d, limTit, rh.top + 29 * d}, inkS);
                v.testo(subTit, fSotto, {rh.left + 16 * d, rh.top + 28 * d, limTit, rh.top + 48 * d}, tenueS);
                y += 60 * d;
                for (int k = 0; k < nNascVis; k++) {
                    const D2D1_RECT_F ra = {L, y, R, y + 54 * d};
                    const int id = Z_MOSTRA_APP + k;
                    riquadro(ra, false);
                    const std::wstring tb = testi::t(L"Mostra", L"Show");
                    const float lw = v.larghezza(tb, fCentro) + 24 * d;
                    const D2D1_RECT_F rb = {ra.right - 16 * d - lw, y + 12 * d, ra.right - 16 * d, y + 42 * d};
                    aggiungiZona(rb, id);
                    v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, (sopra == id ? 0.16f : 0.08f) * (float)e)
                                            : D2D1::ColorF(0, 0, 0, (sopra == id ? 0.10f : 0.05f) * (float)e));
                    v.rt->FillRoundedRectangle({rb, 15 * d, 15 * d}, v.pen);
                    v.testo(tb, fCentro, rb, inkS);
                    v.testo(nascDock[k].second, fVoce, {ra.left + 16 * d, ra.top + 7 * d, rb.left - 12 * d, ra.top + 29 * d}, inkS);
                    v.testo(testi::t(L"Nascosta nel dock", L"Hidden in dock"), fSotto, {ra.left + 16 * d, ra.top + 28 * d, rb.left - 12 * d, ra.top + 48 * d}, tenueS);
                    y += 60 * d;
                }
            }
        }
        if (sezione == S_WIDGET && widgetSubAperto) {
            struct SwatchVoce {
                int zid = -1;
                bool isAuto = false;
                bool attivo = false;
                D2D1_COLOR_F col{};
            };
            auto rigaSwatch = [&](const std::wstring& titolo, const std::wstring& sub, const std::vector<SwatchVoce>& swatches) {
                if (swatches.empty()) return;
                const float fe = (float)e;
                const float cardH = (!sub.empty() ? 78.0f : 68.0f) * d;
                const D2D1_RECT_F ra = {L, y, R, y + cardH};
                bool anyHover = false;
                for (const auto& sw : swatches) if (sopra == sw.zid) { anyHover = true; break; }
                riquadro(ra, anyHover);

                if (!sub.empty()) {
                    v.testo(titolo, fVoce, {ra.left + 16 * d, ra.top + 8 * d, ra.right - 16 * d, ra.top + 28 * d}, inkS);
                    v.testo(sub, fSotto, {ra.left + 16 * d, ra.top + 27 * d, ra.right - 16 * d, ra.top + 45 * d}, tenueS);
                } else {
                    v.testo(titolo, fVoce, {ra.left + 16 * d, ra.top + 9 * d, ra.right - 16 * d, ra.top + 31 * d}, inkS);
                }

                const float rBase = 10.0f * d;
                const float passo = 28.0f * d;
                const float xStart = ra.left + 16.0f * d + rBase;
                const float cy = ra.bottom - 18.0f * d;
                const float zTop = std::max(ra.top, cy - 16.0f * d);
                const float zBot = std::min(ra.bottom, cy + 16.0f * d);

                for (size_t j = 0; j < swatches.size(); j++) {
                    const SwatchVoce& sw = swatches[j];
                    const float cx = xStart + passo * (float)j;
                    aggiungiZona({cx - passo * 0.5f, zTop, cx + passo * 0.5f, zBot}, sw.zid);

                    const bool hovered = (sopra == sw.zid);
                    const float r = hovered ? (rBase + 1.0f * d) : rBase;
                    if (hovered) {
                        const D2D1_COLOR_F halo = sw.isAuto
                            ? D2D1::ColorF(0.20f, 0.85f, 0.90f, 0.24f * fe)
                            : D2D1::ColorF(sw.col.r, sw.col.g, sw.col.b, 0.26f * fe);
                        v.pen->SetColor(halo);
                        v.rt->FillEllipse({{cx, cy}, r + 4.5f * d, r + 4.5f * d}, v.pen);
                    }

                    const D2D1_ELLIPSE el = {{cx, cy}, r, r};
                    if (sw.isAuto) {
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.14f * fe) : D2D1::ColorF(0, 0, 0, 0.08f * fe));
                        v.rt->FillEllipse(el, v.pen);
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.32f * fe) : D2D1::ColorF(0, 0, 0, 0.24f * fe));
                        v.rt->DrawEllipse(el, v.pen, 1.0f);
                        v.pen->SetColor(D2D1::ColorF(0.20f, 0.85f, 1.0f, 0.82f * fe));
                        v.rt->DrawEllipse({{cx, cy}, r * 0.62f, r * 0.62f}, v.pen, 1.5f * d);
                        v.pen->SetColor(D2D1::ColorF(0.19f, 0.82f, 0.35f, 0.92f * fe));
                        v.rt->FillEllipse({{cx, cy}, r * 0.30f, r * 0.30f}, v.pen);
                    } else {
                        v.pen->SetColor(D2D1::ColorF(sw.col.r, sw.col.g, sw.col.b, fe));
                        v.rt->FillEllipse(el, v.pen);
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * fe) : D2D1::ColorF(0, 0, 0, 0.16f * fe));
                        v.rt->DrawEllipse(el, v.pen, 1.0f);
                    }

                    if (sw.attivo) {
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.96f * fe) : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.90f * fe));
                        v.rt->DrawEllipse({{cx, cy}, r + 3.0f * d, r + 3.0f * d}, v.pen, 2.0f * d);
                    }
                }
                y += cardH + 6 * d;
            };
            if (widgetAperto == L"ai") {
                rigaLevetta(Z_W_MOSTRA_DOCK, testi::t(L"Mostra nel dock", L"Show in dock"),
                            testi::t(L"Aggiunge l'icona di Monitor AI al dock", L"Add the AI Monitor icon to the dock"), animCampo(&Valori::dockAI));
                rigaLevetta(Z_W_AI_AVVISI, testi::t(L"Avvisi nel notch", L"Notch alerts"),
                            testi::t(L"Mostra gli avvisi degli agenti AI nel notch", L"Show AI agent status alerts in the notch"),
                            (leggiAIAvvisi ? leggiAIAvvisi() : true) ? 1.0f : 0.0f);
                rigaLevetta(Z_W_AI_HOOK_AGY, testi::t(L"Hook Antigravity CLI", L"Antigravity CLI hooks"),
                            testi::t(L"Registra gli hook in ~/.gemini/config/hooks.json", L"Register lifecycle hooks in ~/.gemini/config/hooks.json"),
                            (hookAgyStato && hookAgyStato()) ? 1.0f : 0.0f);
                rigaLevetta(Z_W_AI_HOOK_CLAUDE, testi::t(L"Hook Claude Code", L"Claude Code hooks"),
                            testi::t(L"Registra gli hook in ~/.claude/settings.json", L"Register lifecycle hooks in ~/.claude/settings.json"),
                            (hookClaudeStato && hookClaudeStato()) ? 1.0f : 0.0f);
                rigaConPulsante(Z_W_AI_CARTELLA, testi::t(L"Cartella hook", L"Hooks folder"),
                                L"%LOCALAPPDATA%\\FluidWin\\ai-agents",
                                testi::t(L"Apri cartella hook", L"Open hooks folder"), false);
            } else if (widgetAperto == L"logitech") {
                rigaLevetta(Z_W_MOSTRA_DOCK, testi::t(L"Mostra nel dock", L"Show in dock"),
                            testi::t(L"Aggiunge il widget batteria Logitech G HUB al dock", L"Add the Logitech G HUB battery widget to the dock"), animCampo(&Valori::dockLogi));
                rigaLevetta(Z_W_LOGI_ANIM, testi::t(L"Animazione di ricarica", L"Charging animation"),
                            testi::t(L"Bagliore animato e impulso sull'anello della batteria durante la ricarica",
                                     L"Animated glow and pulse on the battery ring while charging"),
                            valori().logiAnimCarica ? 1.0f : 0.0f);
                {
                    std::vector<SwatchVoce> logiSwatches;
                    logiSwatches.reserve(N_LOGI_GLOW);
                    for (int j = 0; j < N_LOGI_GLOW; j++) {
                        const DWORD rgb = LOGI_GLOW_PALETTE[j].rgb;
                        logiSwatches.push_back({Z_W_LOGI_GLOW + j, rgb == 0, valori().logiGlowColore == rgb, colore(rgb)});
                    }
                    rigaSwatch(testi::t(L"Colore del bagliore", L"Glow color"),
                               testi::t(L"Colore dell'anello e del bagliore durante la ricarica",
                                        L"Accent color for the charging glow and shimmer"),
                               logiSwatches);
                }
                rigaConPulsante(Z_W_LOGI_GHUB, L"Logitech G HUB",
                                testi::t(L"Avvia o porta in primo piano Logitech G HUB", L"Launch or focus Logitech G HUB"),
                                testi::t(L"Apri Logitech G HUB", L"Open Logitech G HUB"), false);
            } else {
                rigaLevetta(Z_W_MOSTRA_DOCK, testi::t(L"Mostra nel dock", L"Show in dock"),
                            testi::t(L"Mostra questo widget personalizzato nel dock", L"Show this custom widget in the dock"),
                            wpSub.abilitato ? 1.0f : 0.0f);
                for (size_t i = 0; i < wpSub.config.size() && i < wplugin::MAX_CONFIG; i++) {
                    const wplugin::ConfigVoce& cv = wpSub.config[i];
                    if (cv.tipo == L"toggle") {
                        rigaLevetta(Z_W_CFG_TOGGLE + (int)i, cv.titolo, !cv.spiega.empty() ? cv.spiega : cv.chiave, cv.valBool ? 1.0f : 0.0f);
                    } else if (cv.tipo == L"choice" && !cv.opzioni.empty()) {
                        const D2D1_RECT_F ra = {L, y, R, y + 64 * d};
                        riquadro(ra, false);
                        if (!cv.spiega.empty()) {
                            v.testo(cv.titolo, fVoce, {ra.left + 16 * d, ra.top + 10 * d, ra.left + 168 * d, ra.top + 32 * d}, inkS);
                            v.testo(cv.spiega, fSotto, {ra.left + 16 * d, ra.top + 32 * d, ra.left + 168 * d, ra.top + 54 * d}, tenueS);
                        } else {
                            v.testo(cv.titolo, fVoce, {ra.left + 16 * d, ra.top, ra.left + 168 * d, ra.bottom}, inkS);
                        }
                        const float x0 = ra.left + 176 * d, larg = ra.right - 16 * d - x0, h = 34 * d, yl = ra.top + 15 * d;
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)e) : D2D1::ColorF(0, 0, 0, 0.06f * (float)e));
                        v.rt->FillRoundedRectangle({{x0, yl, x0 + larg, yl + h}, h / 2, h / 2}, v.pen);
                        const int nOpt = std::min<int>((int)cv.opzioni.size(), (int)wplugin::MAX_CONFIG_OPTIONS);
                        for (int j = 0; j < nOpt; j++) {
                            const int zid = Z_W_CFG_OPT + (int)i * 8 + j;
                            const D2D1_RECT_F rOpt = {x0 + larg * j / nOpt + 3 * d, yl + 3 * d, x0 + larg * (j + 1) / nOpt - 3 * d, yl + h - 3 * d};
                            aggiungiZona(rOpt, zid);
                            if (cv.valStr == cv.opzioni[j]) {
                                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * (float)e) : D2D1::ColorF(1, 1, 1, 0.95f * (float)e));
                                v.rt->FillRoundedRectangle({rOpt, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                            } else if (sopra == zid) {
                                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.04f));
                                v.rt->FillRoundedRectangle({rOpt, (h - 6 * d) / 2, (h - 6 * d) / 2}, v.pen);
                            }
                            v.testo(cv.opzioni[j], fCentro, rOpt, inkS);
                        }
                        y += 70 * d;
                    } else if (cv.tipo == L"color" && !cv.opzioni.empty()) {
                        const int nOpt = std::min<int>((int)cv.opzioni.size(), (int)wplugin::MAX_CONFIG_COLOR_OPTIONS);
                        std::vector<SwatchVoce> cfgSwatches;
                        cfgSwatches.reserve((size_t)nOpt);
                        for (int j = 0; j < nOpt; j++) {
                            const std::wstring& opt = cv.opzioni[(size_t)j];
                            const bool isAuto = (opt == L"auto");
                            float cr = 0, cg = 0, cb = 0;
                            if (!isAuto) wplugin::parseColore(wplugin::dettagli::inUtf8(opt), &cr, &cg, &cb);
                            cfgSwatches.push_back({Z_W_CFG_OPT + (int)i * 8 + j, isAuto, cv.valStr == opt, D2D1::ColorF(cr, cg, cb, 1.0f)});
                        }
                        rigaSwatch(cv.titolo, cv.spiega, cfgSwatches);
                    } else if (cv.tipo == L"slider") {
                        const int zid = Z_W_CFG_SLIDER + (int)i;
                        const bool dragging = (sliderWPlugDrag == (int)i);
                        const bool hovered = (sopra == zid);
                        const float cardH = (!cv.spiega.empty() ? 78.0f : 68.0f) * d;
                        const D2D1_RECT_F ra = {L, y, R, y + cardH};
                        riquadro(ra, dragging || hovered);

                        wchar_t buf[64]{};
                        const double rVal = std::round(cv.valNum);
                        std::wstring valText;
                        if (std::abs(cv.valNum - rVal) < 1e-4) {
                            swprintf(buf, 64, L"%.0f", rVal == 0.0 ? 0.0 : rVal);
                            valText = buf;
                        } else {
                            swprintf(buf, 64, L"%.2f", cv.valNum);
                            valText = buf;
                            while (!valText.empty() && valText.back() == L'0') valText.pop_back();
                            if (!valText.empty() && valText.back() == L'.') valText.pop_back();
                            if (valText == L"-0") valText = L"0";
                        }
                        valText += cv.unita;
                        const std::wstring valDisp = testi::isola(valText);
                        const float valW = v.larghezza(valDisp, fVoce);
                        const float valLeft = std::max(ra.left + 16 * d, ra.right - 16 * d - valW - 2 * d);
                        const D2D1_COLOR_F iosBlu = v.scuro
                            ? D2D1::ColorF(0.04f, 0.52f, 1.0f, 1.0f)
                            : D2D1::ColorF(0.0f, 0.48f, 1.0f, 1.0f);
                        const D2D1_COLOR_F valCol = (dragging || hovered)
                            ? D2D1::ColorF(iosBlu.r, iosBlu.g, iosBlu.b, (float)e)
                            : D2D1::ColorF(ink.r, ink.g, ink.b, 0.78f * (float)e);

                        v.testo(cv.titolo, fVoce, {ra.left + 16 * d, ra.top + 10 * d, std::max(ra.left + 16 * d, valLeft - 10 * d), ra.top + 32 * d}, inkS);
                        v.testo(valDisp, fVoce, {valLeft, ra.top + 10 * d, ra.right - 14 * d, ra.top + 32 * d}, valCol);
                        if (!cv.spiega.empty()) {
                            v.testo(cv.spiega, fSotto, {ra.left + 16 * d, ra.top + 30 * d, ra.right - 16 * d, ra.top + 48 * d}, tenueS);
                        }

                        const float trackL = ra.left + 16 * d, trackR = ra.right - 16 * d;
                        const float trackH = 4.5f * d;
                        const float trackCy = ra.bottom - 17.0f * d;
                        const float trackTop = trackCy - trackH * 0.5f, trackBot = trackCy + trackH * 0.5f;
                        sliderWPlugTrack[i] = {trackL, trackTop, trackR, trackBot};
                        aggiungiZona({ra.left, std::max(ra.top, trackCy - 14 * d), ra.right, std::min(ra.bottom, trackCy + 14 * d)}, zid);

                        const double span = (cv.maxNum > cv.minNum) ? (cv.maxNum - cv.minNum) : 1.0;
                        const float frac = (float)std::clamp((cv.valNum - cv.minNum) / span, 0.0, 1.0);
                        const float thumbX = trackL + (trackR - trackL) * frac;

                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.16f * (float)e) : D2D1::ColorF(0, 0, 0, 0.12f * (float)e));
                        v.rt->FillRoundedRectangle({{trackL, trackTop, trackR, trackBot}, trackH * 0.5f, trackH * 0.5f}, v.pen);
                        if (thumbX > trackL + 0.5f) {
                            v.pen->SetColor(D2D1::ColorF(iosBlu.r, iosBlu.g, iosBlu.b, (float)e));
                            v.rt->FillRoundedRectangle({{trackL, trackTop, thumbX, trackBot}, trackH * 0.5f, trackH * 0.5f}, v.pen);
                        }

                        const float thumbR = (dragging ? 10.5f : 9.5f) * d;
                        v.pen->SetColor(D2D1::ColorF(0, 0, 0, 0.08f * (float)e));
                        v.rt->FillEllipse({{thumbX, trackCy + 1.5f * d}, thumbR + 3.0f * d, thumbR + 3.0f * d}, v.pen);
                        v.pen->SetColor(D2D1::ColorF(0, 0, 0, (v.scuro ? 0.34f : 0.18f) * (float)e));
                        v.rt->FillEllipse({{thumbX, trackCy + 1.5f * d}, thumbR + 1.5f * d, thumbR + 1.5f * d}, v.pen);
                        v.pen->SetColor(D2D1::ColorF(1, 1, 1, (float)e));
                        v.rt->FillEllipse({{thumbX, trackCy}, thumbR, thumbR}, v.pen);
                        v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.25f * (float)e) : D2D1::ColorF(0, 0, 0, 0.12f * (float)e));
                        v.rt->DrawEllipse({{thumbX, trackCy}, thumbR, thumbR}, v.pen, 1.0f);

                        y += cardH + 6 * d;
                    }
                }
                {
                    const D2D1_RECT_F rbRow = {L, y, R, y + 54 * d};
                    riquadro(rbRow, false);
                    const float yb = rbRow.top + 12 * d;
                    const std::wstring tb1 = testi::t(L"Apri cartella widget", L"Open widgets folder");
                    const float lw1 = v.larghezza(tb1, fCentro) + 26 * d;
                    const D2D1_RECT_F rb1 = {rbRow.left + 16 * d, yb, std::min(rbRow.right - 16 * d, rbRow.left + 16 * d + lw1), yb + 30 * d};
                    pulsantePill(rb1, Z_W_WPLUG_CARTELLA, tb1, false);
                    if (!wpSub.filePath.empty()) {
                        const std::wstring tb2 = testi::t(L"Modifica JSON", L"Edit JSON");
                        const float lw2 = v.larghezza(tb2, fCentro) + 26 * d;
                        const D2D1_RECT_F rb2 = {rb1.right + 10 * d, yb, std::min(rbRow.right - 16 * d, rb1.right + 10 * d + lw2), yb + 30 * d};
                        if (rb2.right > rb2.left + 10 * d) pulsantePill(rb2, Z_W_WPLUG_JSON, tb2, true);
                    }
                    y += 60 * d;
                }
            }
        } else if (sezione == S_WIDGET) {
            const D2D1_RECT_F rh = {L, y, R, y + 92 * d};
            riquadro(rh, false);
            v.testo(testi::t(L"Widget personalizzati", L"Custom widgets"), fVoce, {rh.left + 16 * d, rh.top + 7 * d, rh.right - 16 * d, rh.top + 29 * d}, inkS);
            v.testo(L"%LOCALAPPDATA%\\FluidWin\\widgets\\*.json", fSotto, {rh.left + 16 * d, rh.top + 27 * d, rh.right - 16 * d, rh.top + 47 * d}, tenueS);
            const float yb = rh.top + 52 * d;
            const std::wstring tb1 = testi::t(L"Apri cartella widget", L"Open widgets folder");
            const float lw1 = v.larghezza(tb1, fCentro) + 26 * d;
            const D2D1_RECT_F rb1 = {rh.left + 16 * d, yb, rh.left + 16 * d + lw1, yb + 30 * d};
            pulsantePill(rb1, Z_WPLUG_CARTELLA, tb1, false);

            const std::wstring tb2 = testi::t(L"Crea widget di esempio", L"Create sample widget");
            const float lw2 = v.larghezza(tb2, fCentro) + 26 * d;
            const D2D1_RECT_F rb2 = {rb1.right + 10 * d, yb, std::min(rh.right - 16 * d, rb1.right + 10 * d + lw2), yb + 30 * d};
            pulsantePill(rb2, Z_WPLUG_ESEMPIO, tb2, true);
            y += 98 * d;

            const auto wplugs = elencoWPlugin ? elencoWPlugin() : std::vector<std::pair<std::wstring, std::pair<std::wstring, bool>>>{};
            if (wplugs.empty()) {
                riga(testi::t(L"Nessun widget personalizzato", L"No custom widgets found"),
                     testi::t(L"Crea un esempio o aggiungi file .json nella cartella widget", L"Create a sample or drop .json files into the widgets folder"));
            } else {
                for (int k = 0; k < (int)wplugs.size(); k++) {
                    const D2D1_RECT_F ra = {L, y, R, y + 54 * d};
                    const int openId = Z_W_APRI_WPLUG + k;
                    const int toggleId = Z_WPLUG_APP + k;
                    const float toggleL = ra.right - 66 * d;
                    if (k < 30) aggiungiZona({ra.left, ra.top, toggleL, ra.bottom}, openId);
                    aggiungiZona({k < 30 ? toggleL : ra.left, ra.top, ra.right, ra.bottom}, toggleId);
                    riquadro(ra, sopra == toggleId || (k < 30 && sopra == openId));
                    v.testo(wplugs[k].second.first, fVoce, {ra.left + 16 * d, ra.top + 7 * d, ra.right - 94 * d, ra.top + 29 * d}, inkS);
                    v.testo(wplugs[k].first + L".json", fSotto, {ra.left + 16 * d, ra.top + 28 * d, ra.right - 94 * d, ra.top + 48 * d}, tenueS);
                    if (k < 30) frecciaWidget(ra.right - 76 * d, (ra.top + ra.bottom) / 2, sopra == openId);
                    levetta(ra.right - 58 * d, (ra.top + ra.bottom) / 2, wplugs[k].second.second ? 1.0f : 0.0f, (float)e);
                    y += 60 * d;
                }
            }
        }
        if (sezione == S_NOTCH)
            riga(testi::t(L"Nel notch, sempre", L"Always in the notch"),
                 testi::t(L"Musica, timer, scaffale dei file, microfono e webcam, batteria e cuffie", L"Music, timer, file shelf, microphone and camera, battery and headphones"));
        if (sezione == S_TASTIERA) {
            riga(testi::t(L"Ctrl+Win+T", L"Ctrl+Win+T"), testi::t(L"Tiene la finestra attiva sempre in primo piano (di nuovo per toglierlo)", L"Keeps the active window always on top (again to undo)"));
            riga(testi::t(L"Maiusc+clic destro", L"Shift+right-click"), testi::t(L"Il menu di Windows, anche con il menu di FluidWin acceso", L"The Windows menu, even with the FluidWin menu on"));
        }
        if (sezione == S_CURSORE) disegnaCursori(L, R, y, e, ink, tenue, blu, rr, H);
        if (scorre) {
            v.rt->PopAxisAlignedClip();
            // quanto si puo' scorrere: l'altezza del contenuto (y senza lo scorrimento) meno lo spazio visibile, con un
            // margine sul fondo. Se la finestra e' cresciuta (o la sezione accorciata) lo scorrimento rientra al
            // prossimo scatto; qui si disegna comunque con quello che c'e'
            const float alto = y + scorriSez - y0, visibile = H - 12 * d - y0;
            massimoSez = std::max(0.0f, alto - visibile);
            scorriSez = std::min(scorriSez, massimoSez);   // (vale dal prossimo disegno)
            // zone della sezione: solo la parte visibile (sotto il titolo e dentro la finestra); le altre via
            for (size_t k = zona0; k < zone.size();) {
                D2D1_RECT_F& z = zone[k].r;
                z.top = std::max(z.top, cimaSez); z.bottom = std::min(z.bottom, H);
                if (z.bottom <= z.top) zone.erase(zone.begin() + (std::ptrdiff_t)k); else k++;
            }
            if (massimoSez > 0) {   // barretta a destra: dove si e' e quanto c'e'
                const float hb = visibile * visibile / alto, yb = y0 + (visibile - hb) * std::min(scorriSez / massimoSez, 1.0f);
                v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.22f * (float)e) : D2D1::ColorF(0, 0, 0, 0.18f * (float)e));
                v.rt->FillRoundedRectangle({{W - 10 * d, yb, W - 6 * d, yb + hb}, 2 * d, 2 * d}, v.pen);
            }
        }
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
            // (l'intervallo isolato: in arabo "1–6" fra due numeri si leggerebbe "6–1")
            const std::wstring quali = testi::isola(std::to_wstring(scorriCursori + 1) + L"–" + std::to_wstring(std::min(tutte, scorriCursori + posto))) +
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
