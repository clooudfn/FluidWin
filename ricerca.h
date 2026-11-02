// Ricerca rapida (come Spotlight): Alt+Tab apre una barra di vetro al centro dello schermo. Mentre si scrive mostra
// app, pagine delle impostazioni, file (dall'indice di ricerca di Windows, in un thread a parte), il risultato dei
// calcoli e la ricerca sul web. Frecce e Invio per scegliere, Esc per chiudere.
#pragma once
#include "vetro.h"
#include "onda.h"
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <structuredquerycondition.h>
#include <propsys.h>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <cwctype>

namespace ricerca {

enum Tipo { T_CALCOLO, T_APP, T_IMPOSTAZIONE, T_FILE, T_WEB };

struct Risultato {
    Tipo tipo = T_APP;
    std::wstring titolo, sotto, azione, icona;   // icona: percorso per la shell ("" = simbolo disegnato)
};

// ---- calcolatrice: + - * / ^ % e parentesi, virgola o punto per i decimali ----
class Calcolo {
public:
    static bool valuta(const std::wstring& s, double& out) {
        bool cifra = false, operatore = false;
        for (wchar_t c : s) {
            if (iswdigit(c)) cifra = true;
            else if (wcschr(L"+-*/^%x()", c)) operatore = true;
            else if (c != L' ' && c != L',' && c != L'.') return false;
        }
        if (!cifra || !operatore) return false;
        Calcolo c(s);
        out = c.somma();
        c.spazi();
        return c.ok && c.p == c.s.size() && std::isfinite(out);
    }
private:
    std::wstring s; size_t p = 0; bool ok = true;
    explicit Calcolo(const std::wstring& t) : s(t) {}
    void spazi() { while (p < s.size() && s[p] == L' ') p++; }
    double somma() {
        double v = prodotto();
        for (;;) { spazi(); if (p < s.size() && (s[p] == L'+' || s[p] == L'-')) { const wchar_t o = s[p++]; const double b = prodotto(); v = o == L'+' ? v + b : v - b; } else return v; }
    }
    double prodotto() {
        double v = potenza();
        for (;;) {
            spazi();
            if (p < s.size() && (s[p] == L'*' || s[p] == L'x' || s[p] == L'/')) { const wchar_t o = s[p++]; const double b = potenza(); v = o == L'/' ? v / b : v * b; }
            else if (p < s.size() && s[p] == L'%') { p++; v /= 100; }
            else return v;
        }
    }
    double potenza() { const double b = unario(); spazi(); if (p < s.size() && s[p] == L'^') { p++; return std::pow(b, potenza()); } return b; }
    double unario() { spazi(); if (p < s.size() && s[p] == L'-') { p++; return -unario(); } if (p < s.size() && s[p] == L'+') { p++; return unario(); } return atomo(); }
    double atomo() {
        spazi();
        if (p < s.size() && s[p] == L'(') { p++; const double v = somma(); spazi(); if (p < s.size() && s[p] == L')') p++; else ok = false; return v; }
        std::wstring n;
        while (p < s.size() && (iswdigit(s[p]) || s[p] == L'.' || s[p] == L',')) n += s[p] == L',' ? L'.' : s[p], p++;
        if (n.empty()) { ok = false; return 0; }
        return wcstod(n.c_str(), nullptr);
    }
};

class Ricerca {
public:
    std::function<std::vector<std::pair<std::wstring, std::wstring>>()> elencoApp;   // nome, comando d'avvio
    std::function<void()> apriImpostazioni;                                          // impostazioni di FluidWin
    std::function<bool()> conOnda;                                                   // onda di luce accesa nelle impostazioni
    void (*registro)(const char*) = nullptr;

    bool aperta() const { return apertaOra; }

    void apri() {
        if (apertaOra) { chiudi(); return; }
        if (!v.fin) {
            if (!v.crea(L"FluidWin.Ricerca", vetro::procedura<Ricerca>, this, true)) return;
            InitializeCriticalSection(&cs);
            evento = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            CloseHandle(CreateThread(nullptr, 0, lavoratore, this, 0, nullptr));
        }
        v.aggiornaTema();
        const RECT a = vetro::areaPrincipale();
        v.dpi = vetro::dpiMonitor(MonitorFromPoint({a.left, a.top}, MONITOR_DEFAULTTOPRIMARY));
        const float d = v.dpi;
        if (!fTesto) {
            fTesto = v.formato(21 * d);
            fTitolo = v.formato(14 * d, DWRITE_FONT_WEIGHT_MEDIUM);
            fSotto = v.formato(11.5f * d);
            fSezione = v.formato(11 * d, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
        app = elencoApp ? elencoApp() : std::vector<std::pair<std::wstring, std::wstring>>{};
        testo.clear(); risultati.clear(); file.clear(); sel = 0; sopra = -1;
        larg = (int)std::lround(680 * d);
        x = (a.left + a.right - larg) / 2; y = a.top + (int)std::lround((a.bottom - a.top) * 0.22);
        primaDi = GetForegroundWindow();
        apertaOra = true; chiudendo = false; tAnim = adesso(); ripreso = false;
        scalaW = 0.80; spostaY = 16 * d;   // come al primo fotogramma dell'animazione
        v.alfa(0);
        ridimensiona();
        ShowWindow(v.fin, SW_SHOW);
        prendiFuoco();
        // onda di luce dai bordi, appena sotto la ricerca; i suoi fotogrammi danno anche il ritmo al riquadro
        if (!conOnda || conOnda()) {
            onda.avvisa = v.fin; onda.battito = true;
            onda.apri(vetro::areaPrincipale(), {x + larg / 2, y + (int)std::lround(ALTO_BARRA * d / 2)}, d, v.fin);
        }
        SetTimer(v.fin, 1, 15, nullptr);    // animazione del riquadro (se l'onda non c'e', o tra un battito e l'altro)
        SetTimer(v.fin, 2, 530, nullptr);   // cursore lampeggiante
        passoRiquadro();
    }

    void chiudi(bool restituisciFuoco = true) {
        if (!apertaOra) return;
        apertaOra = false; chiudendo = true; tAnim = adesso();
        KillTimer(v.fin, 2);
        onda.chiudi();
        // il riquadro sfuma salendo un poco e restringendosi (sotto nessun clic: e' gia' chiusa)
        SetTimer(v.fin, 1, 15, nullptr);
        if (restituisciFuoco && primaDi && IsWindow(primaDi) && IsWindowVisible(primaDi)) SetForegroundWindow(primaDi);
        passoRiquadro();
    }

    bool messaggio(HWND, UINT m, WPARAM w, LPARAM l, LRESULT& r) {
        r = 0;
        if (m == onda.messaggio) {
            if (w == 1) { onda.battitoRicevuto(); passoRiquadro(); } else onda.controlla();
            return true;
        }
        // mentre sfuma e' gia' chiusa: niente clic ne' tasti
        if (chiudendo && ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_KEYDOWN || m == WM_CHAR)) return true;
        switch (m) {
            case WM_TIMER:
                if (w == 1) { passoRiquadro(); return true; }
                if (w == 2) cursore = !cursore;
                if (w == 3) { KillTimer(v.fin, 3); chiediFile(); }
                if (w == 4) { raccogliIcone(); }
                disegna();
                return true;
            case WM_PAINT: disegna(); return true;
            case WM_APP + 1: {   // risultati dei file dal thread
                auto* lista = (std::vector<Risultato>*)l;
                if ((LONG)w == generazione && apertaOra) { file = std::move(*lista); componi(); }
                delete lista;
                return true;
            }
            case WM_CHAR:
                if (w >= 32 && w != 127 && !(GetKeyState(VK_CONTROL) & 0x8000)) { testo += (wchar_t)w; cambiato(); }
                return true;
            case WM_KEYDOWN: tasto(w); return true;
            case WM_MOUSEMOVE: {
                const int s = rigaIn(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                if (s != sopra) { sopra = s; if (s >= 0) sel = s; disegna(); }
                return true;
            }
            case WM_LBUTTONUP: { const int s = rigaIn(GET_X_LPARAM(l), GET_Y_LPARAM(l)); if (s >= 0) esegui(s); return true; }
            case WM_ACTIVATE:
                if (LOWORD(w) == WA_INACTIVE && apertaOra) {
                    // Appena aperta, il primo piano preso agganciandosi all'app di prima a volte torna a lei subito
                    // dopo lo sgancio (una volta su due nelle prove): non e' l'utente che ha cliccato altrove, si
                    // riprende (una volta sola) invece di chiudersi
                    if (agganciata && !ripreso && adesso() - tAnim < 0.3 && GetForegroundWindow() == primaDi) {
                        ripreso = true;
                        if (registro) registro("ricerca: primo piano tornato all'app di prima, lo riprende");
                        PostMessageW(v.fin, WM_APP + 2, 0, 0);
                        return true;
                    }
                    if (registro) {
                        char b[200]; wchar_t c[64] = L"", c2[64] = L""; GetClassNameW((HWND)l, c, 64);
                        HWND fg = GetForegroundWindow(); GetClassNameW(fg, c2, 64);
                        snprintf(b, sizeof(b), "ricerca: chiusa, attivata %p (%ls), in primo piano %p (%ls), primaDi %p, aggancio %d",
                                 (void*)l, c, (void*)fg, c2, (void*)primaDi, (int)agganciata);
                        registro(b);
                    }
                    chiudi(false);
                }
                return true;
            case WM_CLOSE: chiudi(); return true;
            case WM_APP + 2: if (apertaOra) prendiFuoco(); return true;
        }
        return false;
    }

private:
    vetro::Finestra v;
    IDWriteTextFormat *fTesto = nullptr, *fTitolo = nullptr, *fSotto = nullptr, *fSezione = nullptr;
    std::wstring testo;
    std::vector<std::pair<std::wstring, std::wstring>> app;
    std::vector<Risultato> risultati, file;
    int sel = 0, sopra = -1, larg = 0, x = 0, y = 0;
    bool apertaOra = false, cursore = true;
    HWND primaDi = nullptr;
    onda::Onda onda;
    // animazione del riquadro: all'apertura si allarga da una pillola piu' stretta con un piccolo rimbalzo, salendo
    // di poco e apparendo; alla chiusura sfuma salendo e restringendosi, poi si nasconde
    bool chiudendo = false;
    double tAnim = 0;
    double scalaW = 1, spostaY = 0;   // larghezza (frazione) e spostamento verticale (px) di adesso
    static constexpr double DURATA_APRI = 0.42, DURATA_CHIUDI = 0.16;

    static double adesso() {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart;
    }

    void passoRiquadro() {
        if (!apertaOra && !chiudendo) return;
        const float d = v.dpi;
        const double e = adesso() - tAnim;
        double a;
        bool finita;
        if (apertaOra) {
            const double m = vetro::molla(e / DURATA_APRI);
            scalaW = 0.80 + 0.20 * m;
            spostaY = (1 - m) * 16 * d;
            a = vetro::uscita(e / 0.15);
            finita = e >= DURATA_APRI;
            if (finita) { scalaW = 1; spostaY = 0; a = 1; }
        } else {
            const double k = vetro::uscita(e / DURATA_CHIUDI);
            scalaW = 1 - 0.06 * k;
            spostaY = -8 * d * k;
            a = 1 - k;
            finita = e >= DURATA_CHIUDI;
        }
        ridimensiona();
        v.alfa(a);
        disegna();
        if (!finita) return;
        KillTimer(v.fin, 1);
        onda.battito = false;
        if (chiudendo) { chiudendo = false; ShowWindow(v.fin, SW_HIDE); scalaW = 1; spostaY = 0; }
    }
    volatile LONG generazione = 0;
    // thread per i file e le icone
    CRITICAL_SECTION cs{};
    HANDLE evento = nullptr;
    std::wstring richiesta; LONG genRichiesta = -1;
    std::deque<std::wstring> iconeDaFare;
    struct Pixel { std::wstring chiave; int w = 0, h = 0; std::vector<UINT> px; };
    std::vector<Pixel> iconePronte;
    std::unordered_map<std::wstring, ID2D1Bitmap*> icone;   // nullptr = in arrivo o non disponibile

    static constexpr float ALTO_BARRA = 64, ALTO_RIGA = 48;

    bool agganciata = false, ripreso = false;
    void prendiFuoco() {
        if (!ripreso) agganciata = false;
        SetForegroundWindow(v.fin);
        if (GetForegroundWindow() == v.fin) return;
        agganciata = true;
        HWND fg = GetForegroundWindow();
        const DWORD a = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, b = GetCurrentThreadId();
        // (mai agganciati a un'app che non risponde: il suo input bloccato bloccherebbe anche quello di FluidWin)
        if (a && a != b && !IsHungAppWindow(fg) && AttachThreadInput(b, a, TRUE)) { SetForegroundWindow(v.fin); SetFocus(v.fin); AttachThreadInput(b, a, FALSE); }
    }

    void ridimensiona() {
        const float d = v.dpi;
        const int righe = (int)risultati.size();
        const int h = (int)std::lround((ALTO_BARRA + (righe ? 10 + righe * ALTO_RIGA + 8 : 0)) * d);
        const int w = (int)std::lround(larg * scalaW);
        v.posiziona(x + (larg - w) / 2, y + (int)std::lround(spostaY), w, h, HWND_TOPMOST);
    }

    void tasto(WPARAM k) {
        const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        if (k == VK_ESCAPE) { if (testo.empty()) chiudi(); else { testo.clear(); cambiato(); } }
        else if (k == VK_BACK && !testo.empty()) {
            if (ctrl) { while (!testo.empty() && testo.back() == L' ') testo.pop_back(); while (!testo.empty() && testo.back() != L' ') testo.pop_back(); }
            else testo.pop_back();
            cambiato();
        }
        else if (k == VK_DOWN && !risultati.empty()) { sel = (sel + 1) % (int)risultati.size(); disegna(); }
        else if (k == VK_UP && !risultati.empty()) { sel = (sel - 1 + (int)risultati.size()) % (int)risultati.size(); disegna(); }
        else if (k == VK_RETURN) esegui(sel);
        else if (ctrl && k == 'V' && OpenClipboard(v.fin)) {
            if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) {
                for (; *p; p++) if (*p >= 32) testo += *p;
                GlobalUnlock(hd);
            }
            CloseClipboard(); cambiato();
        }
    }

    void cambiato() {
        cursore = true; SetTimer(v.fin, 2, 530, nullptr);
        InterlockedIncrement(&generazione);
        file.clear();
        componi();
        // i file si cercano quando si smette di scrivere per un attimo
        KillTimer(v.fin, 3);
        if (testo.size() >= 2) SetTimer(v.fin, 3, 180, nullptr);
    }

    static std::wstring minuscolo(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

    static int punteggio(const std::wstring& nome, const std::wstring& q) {
        const std::wstring n = minuscolo(nome);
        const size_t p = n.find(q);
        if (p == std::wstring::npos) return -1;
        if (p == 0) return 3;
        if (n[p - 1] == L' ' || n[p - 1] == L'-') return 2;
        return 1;
    }

    void componi() {
        risultati.clear();
        std::wstring q = testo;
        while (!q.empty() && q.front() == L' ') q.erase(q.begin());
        while (!q.empty() && q.back() == L' ') q.pop_back();
        if (!q.empty()) {
            const std::wstring ql = minuscolo(q);
            double val;
            if (Calcolo::valuta(q, val)) {
                wchar_t b[64]; swprintf(b, 64, L"%.10g", val);
                std::wstring s = b; for (auto& c : s) if (c == L'.') c = L',';
                risultati.push_back({T_CALCOLO, L"= " + s, L"Invio per copiare il risultato", s, L""});
            }
            // app: prima chi inizia con il testo
            std::vector<std::pair<int, size_t>> trovate;
            for (size_t i = 0; i < app.size(); i++) { const int p = punteggio(app[i].first, ql); if (p > 0) trovate.push_back({p, i}); }
            std::stable_sort(trovate.begin(), trovate.end(), [](auto& a, auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < trovate.size() && i < 5; i++) {
                const auto& a = app[trovate[i].second];
                risultati.push_back({T_APP, a.first, L"App", a.second, a.second});
            }
            int n = 0;
            for (const auto& s : pagine()) {
                if (n >= 3) break;
                if (punteggio(s.titolo, ql) > 0 || minuscolo(s.sotto).find(ql) != std::wstring::npos) { Risultato r = s; r.sotto = L"Impostazioni"; risultati.push_back(r); n++; }
            }
            for (const auto& f : file) risultati.push_back(f);
            risultati.push_back({T_WEB, L"Cerca “" + q + L"” sul web", L"Nel browser predefinito", q, L""});
        }
        sel = std::clamp(sel, 0, std::max(0, (int)risultati.size() - 1));
        if (risultati.empty()) sel = 0;
        sopra = -1;
        // icone che mancano: al thread
        bool nuove = false;
        EnterCriticalSection(&cs);
        for (const auto& r : risultati) if (!r.icona.empty() && !icone.count(r.icona)) { icone[r.icona] = nullptr; iconeDaFare.push_back(r.icona); nuove = true; }
        LeaveCriticalSection(&cs);
        if (nuove) { SetEvent(evento); SetTimer(v.fin, 4, 50, nullptr); }
        ridimensiona();
        disegna();
    }

    void chiediFile() {
        if (!apertaOra || testo.size() < 2) return;
        EnterCriticalSection(&cs); richiesta = testo; genRichiesta = generazione; LeaveCriticalSection(&cs);
        SetEvent(evento);
    }

    void raccogliIcone() {
        std::vector<Pixel> arrivate;
        EnterCriticalSection(&cs); arrivate.swap(iconePronte); const bool finite = iconeDaFare.empty(); LeaveCriticalSection(&cs);
        for (auto& p : arrivate) {
            if (p.px.empty() || !v.rt) continue;
            ID2D1Bitmap* b = nullptr;
            v.rt->CreateBitmap(D2D1::SizeU(p.w, p.h), p.px.data(), p.w * 4,
                               D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
            icone[p.chiave] = b;
        }
        if (finite && arrivate.empty()) KillTimer(v.fin, 4);
    }

    void esegui(int i) {
        if (i < 0 || i >= (int)risultati.size()) return;
        const Risultato r = risultati[i];
        chiudi(false);
        switch (r.tipo) {
            case T_CALCOLO:
                if (OpenClipboard(v.fin)) {
                    EmptyClipboard();
                    const size_t n = (r.azione.size() + 1) * sizeof(wchar_t);
                    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(g), r.azione.c_str(), n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                    CloseClipboard();
                }
                if (primaDi && IsWindow(primaDi)) SetForegroundWindow(primaDi);
                break;
            case T_WEB: {
                std::wstring u = L"https://www.google.com/search?q=";
                for (wchar_t c : r.azione) {
                    if (iswalnum(c) && c < 128) u += c;
                    else if (c == L' ') u += L'+';
                    else {   // UTF-8 in percentuale
                        char b[8]; const int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, b, 8, nullptr, nullptr);
                        for (int j = 0; j < n; j++) { wchar_t h[4]; swprintf(h, 4, L"%%%02X", (unsigned char)b[j]); u += h; }
                    }
                }
                ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            default:
                if (r.azione == L"*fluidwin") { if (apriImpostazioni) apriImpostazioni(); }
                else ShellExecuteW(nullptr, L"open", r.azione.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    int rigaIn(int px, int py) const {
        const float d = v.dpi, y0 = (ALTO_BARRA + 10) * d;
        if (py < y0 || px < 8 * d || px > v.W - 8 * d) return -1;
        const int i = (int)((py - y0) / (ALTO_RIGA * d));
        return i < (int)risultati.size() ? i : -1;
    }

    void disegna() {
        if ((!apertaOra && !chiudendo) || !v.inizia()) return;
        const float d = v.dpi, W = (float)v.W;
        const D2D1_COLOR_F ink = v.inchiostro(), tenue = v.inchiostro(0.55f);
        // lente
        v.pen->SetColor(tenue);
        const float lx = 30 * d, ly = ALTO_BARRA * d / 2;
        v.rt->DrawEllipse({{lx - 1.5f * d, ly - 1.5f * d}, 7.5f * d, 7.5f * d}, v.pen, 2.0f * d);
        v.rt->DrawLine({lx + 4 * d, ly + 4 * d}, {lx + 9 * d, ly + 9 * d}, v.pen, 2.2f * d);
        const D2D1_RECT_F rt = {54 * d, 0, W - 24 * d, ALTO_BARRA * d};
        if (testo.empty()) v.testo(L"Cerca app, file, impostazioni…", fTesto, rt, tenue);
        else v.testo(testo, fTesto, rt, ink);
        if (cursore) {
            const float cx = rt.left + (testo.empty() ? 0 : std::min(rt.right - rt.left, v.larghezza(testo, fTesto))) + 1.5f * d;
            v.pen->SetColor(D2D1::ColorF(0.25f, 0.6f, 1.0f, 1));
            v.rt->FillRectangle({cx, ly - 13 * d, cx + 2 * d, ly + 13 * d}, v.pen);
        }
        if (!risultati.empty()) {
            v.pen->SetColor(v.scuro ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.08f));
            v.rt->FillRectangle({0, ALTO_BARRA * d, W, ALTO_BARRA * d + 1}, v.pen);
        }
        const float rr = std::max(8.0f, raggio::vetro() - 10.0f) * d;
        for (int i = 0; i < (int)risultati.size(); i++) {
            const Risultato& r = risultati[i];
            const float y0 = (ALTO_BARRA + 10 + i * ALTO_RIGA) * d;
            const D2D1_RECT_F riga = {8 * d, y0, W - 8 * d, y0 + ALTO_RIGA * d};
            const bool s = i == sel;
            if (s) {
                v.pen->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1));
                v.rt->FillRoundedRectangle({riga, std::min(rr, ALTO_RIGA * d / 2), std::min(rr, ALTO_RIGA * d / 2)}, v.pen);
            }
            const D2D1_COLOR_F c = s ? D2D1::ColorF(1, 1, 1, 1) : ink, c2 = s ? D2D1::ColorF(1, 1, 1, 0.75f) : tenue;
            // icona
            const float ix = riga.left + 12 * d, iy = y0 + (ALTO_RIGA - 30) * d / 2, il = 30 * d;
            auto it = icone.find(r.icona);
            if (!r.icona.empty() && it != icone.end() && it->second) v.rt->DrawBitmap(it->second, {ix, iy, ix + il, iy + il});
            else simbolo(r.tipo, ix + il / 2, iy + il / 2, c);
            const float tx = ix + il + 12 * d;
            v.testo(r.titolo, fTitolo, {tx, y0 + 5 * d, riga.right - 12 * d, y0 + 26 * d}, c);
            v.testo(r.sotto, fSotto, {tx, y0 + 25 * d, riga.right - 12 * d, y0 + 43 * d}, c2);
        }
        v.finisci();
    }

    void simbolo(Tipo t, float cx, float cy, D2D1_COLOR_F c) {
        const float d = v.dpi;
        v.pen->SetColor({c.r, c.g, c.b, c.a * 0.85f});
        switch (t) {
            case T_CALCOLO:
                v.rt->DrawRoundedRectangle({{cx - 9 * d, cy - 11 * d, cx + 9 * d, cy + 11 * d}, 3 * d, 3 * d}, v.pen, 1.6f * d);
                v.rt->DrawLine({cx - 5 * d, cy - 5 * d}, {cx + 5 * d, cy - 5 * d}, v.pen, 1.6f * d);
                for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) v.rt->FillEllipse({{cx - 4.5f * d + j * 4.5f * d, cy + 1 * d + i * 5 * d}, 1.2f * d, 1.2f * d}, v.pen);
                break;
            case T_WEB:
                v.rt->DrawEllipse({{cx, cy}, 10 * d, 10 * d}, v.pen, 1.6f * d);
                v.rt->DrawEllipse({{cx, cy}, 4.5f * d, 10 * d}, v.pen, 1.4f * d);
                v.rt->DrawLine({cx - 10 * d, cy}, {cx + 10 * d, cy}, v.pen, 1.4f * d);
                break;
            case T_IMPOSTAZIONE:
                v.rt->DrawEllipse({{cx, cy}, 6 * d, 6 * d}, v.pen, 2.2f * d);
                for (int i = 0; i < 8; i++) {
                    const float a = i * 3.14159265f / 4 + 0.39f, co = std::cos(a), si = std::sin(a);
                    v.rt->DrawLine({cx + co * 7 * d, cy + si * 7 * d}, {cx + co * 9.5f * d, cy + si * 9.5f * d}, v.pen, 2.6f * d);
                }
                break;
            default:   // file o app senza icona
                v.rt->DrawRoundedRectangle({{cx - 8 * d, cy - 11 * d, cx + 8 * d, cy + 11 * d}, 2.5f * d, 2.5f * d}, v.pen, 1.6f * d);
        }
    }

    static const std::vector<Risultato>& pagine() {
        static const std::vector<Risultato> p = [] {
            const wchar_t* t[][3] = {
                {L"Schermo", L"display risoluzione monitor scala", L"ms-settings:display"},
                {L"Audio", L"suono volume microfono altoparlanti", L"ms-settings:sound"},
                {L"Notifiche", L"avvisi non disturbare", L"ms-settings:notifications"},
                {L"Bluetooth e dispositivi", L"cuffie mouse tastiera", L"ms-settings:bluetooth"},
                {L"Wi-Fi", L"wireless rete internet", L"ms-settings:network-wifi"},
                {L"Rete e Internet", L"ethernet connessione proxy vpn", L"ms-settings:network-status"},
                {L"Personalizzazione", L"aspetto", L"ms-settings:personalization"},
                {L"Sfondo", L"wallpaper desktop", L"ms-settings:personalization-background"},
                {L"Colori e tema scuro", L"modalità scura chiara colore", L"ms-settings:colors"},
                {L"App installate", L"disinstalla programmi", L"ms-settings:appsfeatures"},
                {L"App predefinite", L"browser apri con", L"ms-settings:defaultapps"},
                {L"App all'avvio", L"esecuzione automatica", L"ms-settings:startupapps"},
                {L"Account", L"utente profilo", L"ms-settings:yourinfo"},
                {L"Data e ora", L"orologio fuso orario", L"ms-settings:dateandtime"},
                {L"Lingua e area geografica", L"italiano formato", L"ms-settings:regionlanguage"},
                {L"Tastiera e digitazione", L"layout", L"ms-settings:typing"},
                {L"Mouse", L"puntatore velocità touchpad", L"ms-settings:mousetouchpad"},
                {L"Energia", L"batteria sospensione spegnimento", L"ms-settings:powersleep"},
                {L"Archiviazione", L"spazio disco pulizia", L"ms-settings:storagesense"},
                {L"Windows Update", L"aggiornamenti", L"ms-settings:windowsupdate"},
                {L"Sicurezza di Windows", L"antivirus defender firewall", L"windowsdefender:"},
                {L"Privacy e sicurezza", L"autorizzazioni fotocamera posizione", L"ms-settings:privacy"},
                {L"Stampanti e scanner", L"stampa", L"ms-settings:printers"},
                {L"Informazioni sul sistema", L"pc specifiche nome versione", L"ms-settings:about"},
                {L"Gioco", L"game bar modalità gioco", L"ms-settings:gaming-gamebar"},
                {L"Accessibilità", L"testo grande contrasto narratore", L"ms-settings:easeofaccess"},
                {L"Pannello di controllo", L"classico", L"control"},
                {L"Gestione attività", L"processi task manager", L"taskmgr"},
                {L"Impostazioni di FluidWin", L"dock capsula gelatina genio panoramica", L"*fluidwin"},
            };
            std::vector<Risultato> v;
            for (auto& x : t) v.push_back({T_IMPOSTAZIONE, x[0], x[1], x[2], L""});
            return v;
        }();
        return p;
    }

    // ---- thread: file dall'indice di ricerca di Windows e icone ----
    static DWORD WINAPI lavoratore(LPVOID p) {
        Ricerca* r = (Ricerca*)p;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            WaitForSingleObject(r->evento, INFINITE);
            for (;;) {
                std::wstring q, ic; LONG g = -1;
                EnterCriticalSection(&r->cs);
                if (r->genRichiesta >= 0) { q = r->richiesta; g = r->genRichiesta; r->genRichiesta = -1; }
                else if (!r->iconeDaFare.empty()) { ic = r->iconeDaFare.front(); r->iconeDaFare.pop_front(); }
                LeaveCriticalSection(&r->cs);
                if (g >= 0) {
                    auto* lista = new std::vector<Risultato>(cercaFile(q, 5));   // al massimo 5, dopo aver saltato quelli di sistema
                    if (g != r->generazione || !PostMessageW(r->v.fin, WM_APP + 1, (WPARAM)g, (LPARAM)lista)) delete lista;
                } else if (!ic.empty()) {
                    Pixel px; px.chiave = ic;
                    pixelIcona(ic, 64, px.w, px.h, px.px);
                    EnterCriticalSection(&r->cs); r->iconePronte.push_back(std::move(px)); LeaveCriticalSection(&r->cs);
                } else break;
            }
        }
        return 0;
    }

    static std::vector<Risultato> cercaFile(const std::wstring& q, int quanti) {
        std::vector<Risultato> out;
        static const CLSID CLSID_Fabbrica = {0x14010e02, 0xbbbd, 0x41f0, {0x88, 0xe3, 0xed, 0xa3, 0x71, 0x21, 0x65, 0x84}};   // SearchFolderItemFactory
        static const IID IID_Fabbrica = {0xa0ffbc28, 0x5482, 0x4366, {0xbe, 0x27, 0x3e, 0x81, 0xe7, 0x8e, 0x06, 0xc2}};
        static const CLSID CLSID_Condizioni = {0xe03e85b0, 0x7be3, 0x4000, {0xba, 0x98, 0x6c, 0x13, 0xde, 0x9f, 0xa4, 0x86}};   // ConditionFactory
        static const IID IID_Condizioni2 = {0x71d222e1, 0x432f, 0x429e, {0x8c, 0x13, 0xb6, 0xda, 0xfd, 0xe5, 0x07, 0x7a}};
        static const IID IID_Condizione = {0x0fc988d4, 0xc935, 0x4b97, {0xa9, 0x73, 0x46, 0x28, 0x2e, 0xa1, 0x75, 0xc8}};
        static const PROPERTYKEY NOME = {{0xb725f130, 0x47ef, 0x101a, {0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac}}, 10};   // ItemNameDisplay
        ISearchFolderItemFactory* sf = nullptr; IUnknown* cf = nullptr; ICondition* cond = nullptr;
        if (FAILED(CoCreateInstance(CLSID_Fabbrica, nullptr, CLSCTX_INPROC_SERVER, IID_Fabbrica, (void**)&sf))) return out;
        if (SUCCEEDED(CoCreateInstance(CLSID_Condizioni, nullptr, CLSCTX_INPROC_SERVER, IID_Condizioni2, (void**)&cf)) &&
            // IConditionFactory2::CreateStringLeaf (MinGW non ha structuredquery.h: si chiama per posizione, IUnknown 3 +
            // IConditionFactory 4 + CreateTrueFalse, CreateNegation, CreateCompoundFromObjectArray, CreateCompoundFromArray)
            SUCCEEDED(((HRESULT (STDMETHODCALLTYPE*)(void*, const PROPERTYKEY&, CONDITION_OPERATION, LPCWSTR, LPCWSTR, int, REFIID, void**))(*(void***)cf)[11])(
                cf, NOME, COP_VALUE_CONTAINS, q.c_str(), nullptr, 0 /* CONDITION_CREATION_DEFAULT */, IID_Condizione, (void**)&cond)) && cond) {
            sf->SetDisplayName(L"FluidWin");
            sf->SetCondition(cond);
            PIDLIST_ABSOLUTE profilo = nullptr;
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Profile, 0, nullptr, &profilo))) {
                IShellItemArray* scopo = nullptr;
                if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(1, (PCIDLIST_ABSOLUTE_ARRAY)&profilo, &scopo))) { sf->SetScope(scopo); scopo->Release(); }
                CoTaskMemFree(profilo);
            }
            IShellItem* ris = nullptr;
            if (SUCCEEDED(sf->GetShellItem(IID_PPV_ARGS(&ris)))) {
                IEnumShellItems* en = nullptr;
                if (SUCCEEDED(ris->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
                    IShellItem* it = nullptr;
                    while ((int)out.size() < quanti && en->Next(1, &it, nullptr) == S_OK) {
                        LPWSTR n = nullptr, perc = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &perc)) && n && perc) {
                            std::wstring p = perc, cartella = p.substr(0, p.find_last_of(L'\\'));
                            // cartelle di sistema e nascoste (AppData, .cache, .vscode...): non sono cose che si cercano
                            std::wstring pl = p; for (auto& ch : pl) ch = (wchar_t)towlower(ch);
                            const bool sistema = pl.find(L"\\appdata\\") != std::wstring::npos || pl.find(L"\\.") != std::wstring::npos;
                            if (sistema) { CoTaskMemFree(n); CoTaskMemFree(perc); it->Release(); continue; }
                            wchar_t prof[MAX_PATH] = L""; ExpandEnvironmentStringsW(L"%USERPROFILE%", prof, MAX_PATH);
                            if (_wcsnicmp(cartella.c_str(), prof, wcslen(prof)) == 0) cartella = L"~" + cartella.substr(wcslen(prof));
                            out.push_back({T_FILE, n, cartella, p, p});
                        }
                        CoTaskMemFree(n); CoTaskMemFree(perc); it->Release();
                    }
                    en->Release();
                }
                ris->Release();
            }
        }
        if (cond) cond->Release();
        if (cf) cf->Release();
        sf->Release();
        return out;
    }

    // pixel dell'icona (premoltiplicati) dalla shell
    static bool pixelIcona(const std::wstring& percorso, int lato, int& w, int& h, std::vector<UINT>& px) {
        IShellItemImageFactory* fab = nullptr;
        if (FAILED(SHCreateItemFromParsingName(percorso.c_str(), nullptr, IID_PPV_ARGS(&fab)))) return false;
        HBITMAP hb = nullptr;
        const HRESULT hr = fab->GetImage({lato, lato}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb);
        fab->Release();
        if (FAILED(hr) || !hb) return false;
        BITMAP bm{}; GetObject(hb, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight;
        px.assign((size_t)w * h, 0);
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        HDC scr = GetDC(nullptr); GetDIBits(scr, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, scr);
        DeleteObject(hb);
        bool alfa = false;
        for (UINT p : px) if (p >> 24) { alfa = true; break; }
        for (UINT& p : px) {
            const UINT a = alfa ? (p >> 24) : 255;
            p = (a << 24) | ((((p >> 16) & 255) * a / 255) << 16) | ((((p >> 8) & 255) * a / 255) << 8) | ((p & 255) * a / 255);
        }
        return true;
    }
};

}  // namespace ricerca
