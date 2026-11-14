// Cursori personalizzati di FluidWin: l'utente trascina nelle impostazioni un pacchetto di cursori (.zip, .rar, .7z,
// come quelli di DeviantArt, o una cartella), FluidWin lo estrae, capisce quale file va in quale posto (freccia, mano,
// testo...) e lo salva in %LOCALAPPDATA%\FluidWin\Cursori\<nome>. Il pacchetto scelto vale solo mentre FluidWin e'
// aperto: si applica con SetSystemCursor (i cursori di Windows restano quelli dell'utente nel registro) e all'uscita
// SPI_SETCURSORS rimette quelli di Windows. Se FluidWin cade, il segno CursoriAttivi nel registro dice al guardiano (e
// al prossimo avvio) di rimetterli.
// Perche' cosi': su alcune build di Windows 11 (Insider 10.0.26300, misurato) cinque posti (freccia, mano, attesa,
// mirino, aiuto) ignorano lo schema dei cursori e restano su quelli di base, qualunque file si scelga, anche quelli di
// Windows; SetSystemCursor invece funziona.
// Estrazione: tar.exe di Windows (libarchive: zip, e su Windows 11 recenti anche rar e 7z), niente librerie in piu'.
// Il pacchetto e' un file scaricato: prima si elenca il contenuto (numero di file e dimensione totale con un tetto),
// si estrae in una cartella nuova e vuota, si leggono solo .cur/.ani (validati da Windows con LoadCursorFromFile) e il
// file .inf (testo, solo letto: niente si esegue), e alla fine restano solo i cursori scelti.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace cursori {

inline void (*registro)(const char*) = nullptr;   // riga nel registro di FluidWin (solo con --log)

struct Posto { const wchar_t* nome; UINT id; };
// nell'ordine dei valori di "Control Panel\Cursors\Schemes" (quello dei file .inf dei pacchetti)
inline const Posto POSTI[] = {
    {L"Arrow", 32512}, {L"Help", 32651}, {L"AppStarting", 32650}, {L"Wait", 32514}, {L"Crosshair", 32515},
    {L"IBeam", 32513}, {L"NWPen", 32631}, {L"No", 32648}, {L"SizeNS", 32645}, {L"SizeWE", 32644},
    {L"SizeNWSE", 32642}, {L"SizeNESW", 32643}, {L"SizeAll", 32646}, {L"UpArrow", 32516}, {L"Hand", 32649},
    {L"Pin", 32671}, {L"Person", 32672}};
constexpr int N_POSTI = sizeof(POSTI) / sizeof(POSTI[0]);
constexpr ULONGLONG MAX_ARCHIVIO = 100ull << 20, MAX_CURSORE = 4ull << 20;
constexpr int MAX_VOCI = 2000;
inline const wchar_t* SEGNO = L"CursoriAttivi";   // in HKCU\Software\FluidWin: cursori di FluidWin applicati

inline std::wstring cartella() {
    wchar_t p[MAX_PATH] = L"";
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\FluidWin\\Cursori", p, MAX_PATH);
    return p;
}

inline std::wstring minuscolo(std::wstring s) { for (auto& c : s) c = (wchar_t)std::towlower(c); return s; }
inline std::wstring nomeFile(const std::wstring& p) { const size_t i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? p : p.substr(i + 1); }
inline std::wstring cartellaDi(const std::wstring& p) { const size_t i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? L"" : p.substr(0, i); }
// d\n (cartella e nome) senza stringhe temporanee
inline std::wstring percorso(const std::wstring& d, const std::wstring& n) { std::wstring p; p.reserve(d.size() + n.size() + 1); p += d; p += L'\\'; p += n; return p; }
inline bool finisceCon(const std::wstring& s, const wchar_t* e) { const size_t n = wcslen(e); return s.size() >= n && minuscolo(s.substr(s.size() - n)) == e; }

// cancella una cartella e il suo contenuto (i collegamenti si tolgono, non si seguono)
inline void cancellaCartella(const std::wstring& d) {
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = f.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = percorso(d, n);
            if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) cancellaCartella(p);
            else if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(d.c_str());
}

// tutti i file sotto d (senza seguire i collegamenti)
inline void fileSotto(const std::wstring& d, std::vector<std::pair<std::wstring, ULONGLONG>>& out, int prof = 0) {
    if (prof > 8) return;
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = f.cFileName;
        if (n == L"." || n == L".." || (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring p = percorso(d, n);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) fileSotto(p, out, prof + 1);
        else out.push_back({p, ((ULONGLONG)f.nFileSizeHigh << 32) | f.nFileSizeLow});
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

// tar.exe di Windows con un limite di tempo e un tetto all'uscita: oltre maxUscita byte tar si ferma e la risposta e'
// false (un file "estratto" sullo standard output non puo' superarlo). stderr va a parte (non si mescola ai byte di un
// file). tar sta in un Job Object che lo chiude se FluidWin finisce (anche di colpo): non resta a scrivere da solo
// (R20261114-141207-S8). false: non partito, scaduto, troppo grande o uscito con errore
inline bool tar(const std::wstring& argomenti, std::string& uscita, DWORD limiteMs, size_t maxUscita = 4u << 20) {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = sys; exe += L"\\tar.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { uscita = "tar.exe assente"; return false; }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE leggi = nullptr, scrivi = nullptr;
    if (!CreatePipe(&leggi, &scrivi, &sa, 0)) return false;
    SetHandleInformation(leggi, HANDLE_FLAG_INHERIT, 0);
    HANDLE nulla = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = scrivi; si.hStdError = nulla != INVALID_HANDLE_VALUE ? nulla : nullptr; si.hStdInput = nullptr;
    std::wstring cmd = L"\"" + exe + L"\" " + argomenti;
    // tar nasce gia' dentro un Job Object che lo chiude quando FluidWin finisce (anche di colpo): prima lo si assegnava
    // dopo la creazione, e fra le due cose (o se l'assegnazione falliva) poteva restare fuori (R20261114-142613, resto
    // di S8). Senza job tar non parte
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SIZE_T dimLista = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &dimLista);
    std::vector<BYTE> lista(dimLista);
    auto attr = (LPPROC_THREAD_ATTRIBUTE_LIST)lista.data();
    const bool pronto = job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li)) &&
                        dimLista && InitializeProcThreadAttributeList(attr, 1, 0, &dimLista) &&
                        UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof(job), nullptr, nullptr);
    STARTUPINFOEXW six{}; six.StartupInfo = si; six.StartupInfo.cb = sizeof(six); six.lpAttributeList = attr;
    PROCESS_INFORMATION pi{};
    const BOOL ok = pronto && CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &six.StartupInfo, &pi);
    if (dimLista && pronto) DeleteProcThreadAttributeList(attr);
    CloseHandle(scrivi);
    if (nulla != INVALID_HANDLE_VALUE) CloseHandle(nulla);
    if (!ok) { CloseHandle(leggi); if (job) CloseHandle(job); uscita = "tar.exe non avviato"; return false; }
    const ULONGLONG t0 = GetTickCount64();
    bool finito = false, troppo = false;
    for (;;) {
        DWORD disp = 0;
        while (PeekNamedPipe(leggi, nullptr, 0, nullptr, &disp, nullptr) && disp) {
            char b[16384]; DWORD n = 0;
            if (!ReadFile(leggi, b, std::min<DWORD>(disp, sizeof(b)), &n, nullptr) || !n) break;
            if (uscita.size() + n > maxUscita) { troppo = true; break; }
            uscita.append(b, n);
        }
        if (troppo) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 2000); break; }
        if (finito) break;
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) { finito = true; continue; }   // (l'ultima lettura)
        if (GetTickCount64() - t0 > limiteMs) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 2000); break; }
    }
    DWORD codice = 1; GetExitCodeProcess(pi.hProcess, &codice);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(leggi);
    if (job) CloseHandle(job);
    return finito && !troppo && codice == 0;
}

// ---- file .inf dei pacchetti ----
// [Strings] con le variabili, e la riga che scrive "Control Panel\Cursors\Schemes": l'elenco dei file nell'ordine di
// POSTI (con %variabili%). Anche le righe che scrivono un valore di "Control Panel\Cursors" alla volta (Arrow=...)
inline std::wstring leggiTesto(const std::wstring& p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    std::string b((size_t)std::min<LONGLONG>(sz.QuadPart, 1 << 20), '\0');
    DWORD n = 0; ReadFile(f, b.data(), (DWORD)b.size(), &n, nullptr); CloseHandle(f); b.resize(n);
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) return std::wstring((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
    size_t ini = (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) ? 3 : 0;
    UINT cp = CP_UTF8;
    int k = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, b.data() + ini, (int)(b.size() - ini), nullptr, 0);
    if (k <= 0) { cp = CP_ACP; k = MultiByteToWideChar(cp, 0, b.data() + ini, (int)(b.size() - ini), nullptr, 0); }
    std::wstring w(k > 0 ? k : 0, L'\0');
    if (k > 0) MultiByteToWideChar(cp, 0, b.data() + ini, (int)(b.size() - ini), w.data(), k);
    return w;
}
inline std::wstring pulito(std::wstring s) {   // senza spazi ai lati e senza virgolette
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && std::iswspace(s[i])) i++;
    s = s.substr(i);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}
// campi separati da virgole fuori dalle virgolette (le virgolette restano: si tolgono con pulito)
inline std::vector<std::wstring> campi(const std::wstring& r) {
    std::vector<std::wstring> v; std::wstring c; bool q = false;
    for (wchar_t ch : r) { if (ch == L'"') q = !q; if (ch == L',' && !q) { v.push_back(c); c.clear(); } else c += ch; }
    v.push_back(c);
    return v;
}
// %variabili% sostituite (al massimo 4 giri, 256 sostituzioni e 4096 caratteri): un .inf con Nome="%Nome%" faceva
// girare il ciclo per sempre (si ricercava dall'inizio dopo ogni sostituzione: R20261114-141207-S1)
inline std::wstring risolvi(std::wstring s, const std::vector<std::pair<std::wstring, std::wstring>>& var) {
    int sostituzioni = 0;
    for (int giro = 0; giro < 4; giro++) {
        bool cambiato = false;
        for (const auto& [k, v] : var) {
            std::wstring t = L"%"; t += k; t += L'%';
            const std::wstring tm = minuscolo(t);
            size_t da = 0;
            for (size_t i; (i = minuscolo(s).find(tm, da)) != std::wstring::npos;) {
                s.replace(i, t.size(), v); cambiato = true;
                da = i + v.size();   // (si riprende dopo il testo appena messo)
                if (++sostituzioni > 256 || s.size() > 4096) return s;
            }
        }
        if (!cambiato) break;
    }
    return s;
}
// dal .inf: per ogni posto il nome del file (vuoto se non c'e'), e il nome dello schema
inline bool leggiInf(const std::wstring& p, std::wstring (&file)[N_POSTI], std::wstring& nome) {
    const std::wstring t = leggiTesto(p);
    std::vector<std::wstring> righe; { std::wstring r; for (wchar_t c : t) { if (c == L'\n') { righe.push_back(r); r.clear(); } else if (c != L'\r') r += c; } righe.push_back(r); }
    std::vector<std::pair<std::wstring, std::wstring>> var;
    std::wstring sezione;
    auto senzaCommento = [](const std::wstring& r) { bool q = false; for (size_t i = 0; i < r.size(); i++) { if (r[i] == L'"') q = !q; if (r[i] == L';' && !q) return r.substr(0, i); } return r; };
    for (const auto& r0 : righe) {
        const std::wstring r = pulito(senzaCommento(r0));
        if (r.empty()) continue;
        if (r.front() == L'[') { sezione = minuscolo(r); continue; }
        if (sezione == L"[strings]") { const size_t u = r.find(L'='); if (u != std::wstring::npos) var.push_back({pulito(r.substr(0, u)), pulito(r.substr(u + 1))}); }
    }
    bool trovato = false;
    for (const auto& r0 : righe) {
        const std::wstring r = senzaCommento(r0), m = minuscolo(r);
        if (m.find(L"control panel\\cursors") == std::wstring::npos) continue;
        const std::vector<std::wstring> c = campi(r);
        if (c.size() < 3) continue;
        if (m.find(L"control panel\\cursors\\schemes") != std::wstring::npos) {
            nome = pulito(risolvi(pulito(c[2]), var));
            const std::vector<std::wstring> el = campi(risolvi(pulito(c.back()), var));
            for (int i = 0; i < N_POSTI && i < (int)el.size(); i++) { file[i] = nomeFile(pulito(el[i])); trovato = trovato || !file[i].empty(); }
        } else {
            const std::wstring chiave = minuscolo(pulito(c[2]));
            for (int i = 0; i < N_POSTI; i++) if (chiave == minuscolo(POSTI[i].nome)) { file[i] = nomeFile(pulito(risolvi(pulito(c.back()), var))); trovato = true; }
        }
    }
    return trovato;
}

// senza .inf (o se non dice niente): il posto dal nome del file. L'ordine conta: "link select" prima di "select"
inline int postoDaNome(const std::wstring& file) {
    std::wstring n = minuscolo(nomeFile(file));
    n = n.substr(0, n.find_last_of(L'.'));
    auto ha = [&](const wchar_t* x) { return n.find(x) != std::wstring::npos; };
    auto idx = [](const wchar_t* nome) { for (int i = 0; i < N_POSTI; i++) if (!wcscmp(POSTI[i].nome, nome)) return i; return -1; };
    if (ha(L"help")) return idx(L"Help");
    if (ha(L"work") || ha(L"background") || ha(L"appstart") || ha(L"progress")) return idx(L"AppStarting");
    if (ha(L"busy") || ha(L"wait") || ha(L"load")) return idx(L"Wait");
    if (ha(L"precis") || ha(L"cross")) return idx(L"Crosshair");
    if (ha(L"text") || ha(L"beam")) return idx(L"IBeam");
    if (ha(L"handwrit") || ha(L"pen")) return idx(L"NWPen");
    if (ha(L"unavail") || ha(L"forbid") || ha(L"block") || n == L"no") return idx(L"No");
    if (ha(L"vert") || ha(L"sizens") || ha(L"ns_") || ha(L"_ns")) return idx(L"SizeNS");
    if (ha(L"horz") || ha(L"horiz") || ha(L"sizewe") || ha(L"ew_") || ha(L"_ew")) return idx(L"SizeWE");
    // diagonali: "dgn1", "diag1", ma anche "Diagonal Resize 1" (i nomi delle Impostazioni di Windows)
    const bool diag = ha(L"dgn") || ha(L"diag");
    if (ha(L"nwse") || (diag && n.find(L'1') != std::wstring::npos)) return idx(L"SizeNWSE");
    if (ha(L"nesw") || (diag && n.find(L'2') != std::wstring::npos)) return idx(L"SizeNESW");
    if (ha(L"move") || ha(L"sizeall")) return idx(L"SizeAll");
    if (ha(L"altern") || ha(L"uparrow")) return idx(L"UpArrow");
    if (ha(L"link") || ha(L"hand") || (ha(L"point") && !ha(L"pointer"))) return idx(L"Hand");
    if (ha(L"pin") || ha(L"location")) return idx(L"Pin");
    if (ha(L"person")) return idx(L"Person");
    if (ha(L"pointer") || ha(L"arrow") || ha(L"normal") || ha(L"select") || ha(L"default")) return idx(L"Arrow");
    return -1;
}

// ---- pacchetti installati ----
struct Pacchetto { std::wstring nome; int quanti = 0; std::wstring freccia; };   // freccia: file del posto Arrow

// schema.txt nella cartella del pacchetto: una riga "Posto=file" per posto
inline bool leggiSchema(const std::wstring& nome, std::wstring (&file)[N_POSTI]) {
    const std::wstring t = leggiTesto(cartella() + L"\\" + nome + L"\\schema.txt");
    if (t.empty()) return false;
    std::wstring r; bool qualcuno = false;
    auto riga = [&](const std::wstring& x) {
        const size_t u = x.find(L'='); if (u == std::wstring::npos) return;
        const std::wstring k = x.substr(0, u), v = nomeFile(x.substr(u + 1));
        for (int i = 0; i < N_POSTI; i++) if (k == POSTI[i].nome && !v.empty()) { file[i] = percorso(percorso(cartella(), nome), v); qualcuno = true; }
    };
    for (wchar_t c : t) { if (c == L'\n') { riga(r); r.clear(); } else if (c != L'\r') r += c; }
    riga(r);
    return qualcuno;
}

inline std::vector<Pacchetto> elenco() {
    std::vector<Pacchetto> v;
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW((cartella() + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        const std::wstring n = f.cFileName;
        if (n == L"." || n == L".." || n[0] == L'_' || !(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring file[N_POSTI];
        if (!leggiSchema(n, file)) continue;
        Pacchetto p; p.nome = n; p.freccia = file[0];
        for (const auto& x : file) if (!x.empty()) p.quanti++;
        v.push_back(p);
    } while (FindNextFileW(h, &f));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](const Pacchetto& a, const Pacchetto& b) { return minuscolo(a.nome) < minuscolo(b.nome); });
    return v;
}

inline std::atomic<unsigned> gRevisione{0};   // cresce a ogni installazione riuscita (thread dell'installazione)
inline bool esiste(const std::wstring& nome) { std::wstring f[N_POSTI]; return !nome.empty() && leggiSchema(nome, f); }
inline void rimuovi(const std::wstring& nome) { if (!nome.empty() && nome.find_first_of(L"\\/.") == std::wstring::npos) cancellaCartella(cartella() + L"\\" + nome); }

// Avanzi di un'installazione interrotta (FluidWin chiuso o caduto a meta'): _nuovo... (estrazione) si cancella;
// _pronto-Nome (pacchetto nuovo gia' completo) e _vecchio-Nome (quello di prima, messo di lato durante lo scambio)
// servono a recuperare: se Nome manca, prima si completa col nuovo, altrimenti si rimette il vecchio; il resto si
// cancella. Prima si cancellava tutto e uno scambio interrotto perdeva tutte e due le versioni (R20261114-142613-S1).
// All'avvio, prima di leggere le impostazioni (che scartano un pacchetto che non c'e')
inline void pulisciAvanzi() {
    const std::wstring base = cartella();
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW(percorso(base, L"_*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> nuovi, pronti, vecchi;
    do {
        const std::wstring n = f.cFileName;
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (n.rfind(L"_nuovo", 0) == 0) nuovi.push_back(n);
        else if (n.rfind(L"_pronto-", 0) == 0) pronti.push_back(n.substr(8));
        else if (n.rfind(L"_vecchio-", 0) == 0) vecchi.push_back(n.substr(9));
    } while (FindNextFileW(h, &f));
    FindClose(h);
    for (const auto& n : nuovi) cancellaCartella(percorso(base, n));
    auto manca = [&](const std::wstring& n) { return !n.empty() && GetFileAttributesW(percorso(base, n).c_str()) == INVALID_FILE_ATTRIBUTES; };
    for (const auto& n : pronti) {
        const std::wstring p = percorso(base, L"_pronto-" + n);
        if (manca(n) && esiste(L"_pronto-" + n) && MoveFileW(p.c_str(), percorso(base, n).c_str())) {
            if (registro) { char b[200]; snprintf(b, sizeof(b), "cursori: installazione interrotta di \"%ls\" completata", n.c_str()); registro(b); }
        } else cancellaCartella(p);
    }
    for (const auto& n : vecchi) {
        const std::wstring p = percorso(base, L"_vecchio-" + n);
        if (manca(n) && MoveFileW(p.c_str(), percorso(base, n).c_str())) {
            if (registro) { char b[200]; snprintf(b, sizeof(b), "cursori: \"%ls\" rimesso com'era (sostituzione interrotta)", n.c_str()); registro(b); }
        } else cancellaCartella(p);
    }
}

// ---- installazione (in un thread: tar puo' metterci qualche secondo) ----
struct Esito { bool ok = false; std::wstring nome, errore; int quanti = 0; };

inline Esito installa(const std::wstring& sorgente) {
    Esito e;
    // errori come "italiano|inglese": la finestra delle impostazioni sceglie la lingua
    auto no = [&](const wchar_t* testo) { e.errore = testo; return e; };
    const std::wstring base = cartella();
    CreateDirectoryW(cartellaDi(base).c_str(), nullptr); CreateDirectoryW(base.c_str(), nullptr);
    const DWORD attr = GetFileAttributesW(sorgente.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return no(L"file non trovato|file not found");
    const std::wstring tmp = base + L"\\_nuovo" + std::to_wstring(GetTickCount64());
    std::wstring dove;   // cartella da cui prendere i cursori
    if (attr & FILE_ATTRIBUTE_DIRECTORY) dove = sorgente;
    else {
        if (!finisceCon(sorgente, L".zip") && !finisceCon(sorgente, L".rar") && !finisceCon(sorgente, L".7z"))
            return no(L"serve un file .zip, .rar o .7z|a .zip, .rar or .7z file is needed");
        WIN32_FILE_ATTRIBUTE_DATA fa{}; GetFileAttributesExW(sorgente.c_str(), GetFileExInfoStandard, &fa);
        if ((((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) > MAX_ARCHIVIO) return no(L"pacchetto troppo grande (oltre 100 MB)|package too large (over 100 MB)");
        // Solo i file che servono (.cur, .ani, .inf), uno alla volta e con un tetto ciascuno: tar -xO manda il file sullo
        // standard output, che tar() taglia oltre il tetto. Dall'archivio non si scrive altro sul disco. Prima si
        // estraeva tutto, con un tetto calcolato dall'elenco di tar, aggirabile (nomi utente con spazi spostavano la
        // colonna della dimensione: R20261114-141207-S2)
        std::string lista;
        if (!tar(L"-tf \"" + sorgente + L"\"", lista, 20000, 4u << 20)) {
            if (registro) registro("cursori: elenco dell'archivio non riuscito");
            return no(L"archivio non leggibile (questo Windows legge sicuramente lo .zip)|unreadable archive (this Windows surely reads .zip)");
        }
        auto largo = [](const std::string& x) {   // nomi dall'elenco di tar: UTF-8 se valido, altrimenti la code page di Windows
            UINT cp = CP_UTF8; int k = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, x.data(), (int)x.size(), nullptr, 0);
            if (k <= 0) { cp = CP_ACP; k = MultiByteToWideChar(cp, 0, x.data(), (int)x.size(), nullptr, 0); }
            std::wstring w(k > 0 ? k : 0, L'\0'); if (k > 0) MultiByteToWideChar(cp, 0, x.data(), (int)x.size(), w.data(), k);
            return w;
        };
        std::vector<std::wstring> utili;
        int voci = 0;
        for (size_t i = 0; i < lista.size();) {
            size_t j = lista.find('\n', i); if (j == std::string::npos) j = lista.size();
            std::string r = lista.substr(i, j - i); i = j + 1;
            while (!r.empty() && (r.back() == '\r' || r.back() == '/')) r.pop_back();
            if (r.empty()) continue;
            if (++voci > MAX_VOCI) return no(L"troppi file nel pacchetto|too many files in the package");
            const std::wstring n = largo(r);
            if (!finisceCon(n, L".cur") && !finisceCon(n, L".ani") && !finisceCon(n, L".inf")) continue;
            // nomi strani: niente percorsi assoluti, risalite, virgolette o caratteri jolly (tar li tratta come schemi)
            if (n[0] == L'/' || n[0] == L'\\' || n.find(L':') != std::wstring::npos || n.find(L"..") != std::wstring::npos || n.find_first_of(L"\"*?[") != std::wstring::npos) continue;
            utili.push_back(n);
        }
        if (utili.size() > 200) return no(L"troppi cursori nel pacchetto|too many cursors in the package");
        CreateDirectoryW(tmp.c_str(), nullptr);
        for (const auto& n : utili) {
            std::string dati;
            const size_t tetto = finisceCon(n, L".inf") ? (1u << 20) : (size_t)MAX_CURSORE;
            std::wstring arg = L"-xOf \""; arg += sorgente; arg += L"\" -- \""; arg += n; arg += L'"';
            if (!tar(arg, dati, 10000, tetto) || dati.empty()) continue;   // (troppo grande o illeggibile: si salta)
            // stesso percorso relativo dentro la cartella temporanea (le cartelle servono a tenere insieme le varianti)
            std::wstring dest = tmp, parte;
            for (wchar_t c : n) {
                if (c == L'/' || c == L'\\') { if (!parte.empty()) { dest = percorso(dest, parte); CreateDirectoryW(dest.c_str(), nullptr); } parte.clear(); }
                else if (c >= 32 && !wcschr(L"<>|", c)) parte += c;
            }
            if (parte.empty()) continue;
            dest = percorso(dest, parte);
            HANDLE f = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) continue;
            DWORD scritti = 0; WriteFile(f, dati.data(), (DWORD)dati.size(), &scritti, nullptr); CloseHandle(f);
        }
        dove = tmp;
    }
    // file trovati: cursori e .inf
    std::vector<std::pair<std::wstring, ULONGLONG>> tutti; fileSotto(dove, tutti);
    std::vector<std::wstring> cur, inf;
    for (const auto& [p, dim] : tutti) {
        if ((finisceCon(p, L".cur") || finisceCon(p, L".ani")) && dim <= MAX_CURSORE) cur.push_back(p);
        else if (finisceCon(p, L".inf") && dim <= (1u << 20)) inf.push_back(p);
    }
    std::sort(cur.begin(), cur.end()); std::sort(inf.begin(), inf.end());
    std::wstring scelti[N_POSTI], nome;
    // il .inf del pacchetto (il primo): per ogni posto il file, cercato prima accanto al .inf
    for (const auto& i : inf) {
        std::wstring dalInf[N_POSTI], n;
        if (!leggiInf(i, dalInf, n)) continue;
        const std::wstring vicino = minuscolo(cartellaDi(i));
        for (int k = 0; k < N_POSTI; k++) {
            if (dalInf[k].empty()) continue;
            const std::wstring cerca = minuscolo(dalInf[k]);
            for (const auto& c : cur) if (minuscolo(nomeFile(c)) == cerca && (scelti[k].empty() || minuscolo(cartellaDi(c)) == vicino)) scelti[k] = c;
        }
        nome = n;
        break;
    }
    // posti rimasti vuoti: dal nome del file (solo file della stessa cartella del primo trovato, per non mischiare varianti)
    const std::wstring cartellaPrincipale = !scelti[0].empty() ? minuscolo(cartellaDi(scelti[0])) : (cur.empty() ? L"" : minuscolo(cartellaDi(cur[0])));
    for (const auto& c : cur) {
        if (minuscolo(cartellaDi(c)) != cartellaPrincipale) continue;
        const int k = postoDaNome(c);
        if (k >= 0 && scelti[k].empty()) scelti[k] = c;
    }
    // validi per Windows (si caricano davvero)
    for (auto& s : scelti) {
        if (s.empty()) continue;
        HCURSOR h = LoadCursorFromFileW(s.c_str());
        if (!h) { if (registro) { char b[300]; snprintf(b, sizeof(b), "cursori: file scartato (Windows non lo carica): %ls", nomeFile(s).c_str()); registro(b); } s.clear(); continue; }
        DestroyCursor(h);
        e.quanti++;
    }
    if (!e.quanti) { if (!tmp.empty()) cancellaCartella(tmp); return no(L"nessun cursore valido nel pacchetto|no valid cursor in the package"); }
    // nome: quello dello schema, altrimenti quello del file; solo caratteri sicuri per una cartella
    if (nome.empty()) { nome = nomeFile(sorgente); if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) nome = nome.substr(0, nome.find_last_of(L'.')); }
    std::wstring ok;
    for (wchar_t c : nome) if (c >= 32 && !wcschr(L"\\/:*?\"<>|.%", c)) ok += c;   // (% : variabili del .inf non risolte)
    while (!ok.empty() && (ok.back() == L' ' || ok.front() == L' ')) { if (ok.back() == L' ') ok.pop_back(); else ok.erase(0, 1); }
    if (ok.size() > 48) ok.resize(48);
    if (ok.empty() || ok[0] == L'_') ok = L"Cursori" + ok;
    e.nome = ok;
    // Il pacchetto si prepara in una cartella a parte (_pronto...) e prende il posto di quello vecchio solo se tutte le
    // copie e lo schema sono riusciti. Prima si cancellava subito la cartella finale: trascinando di nuovo un pacchetto
    // gia' installato (sorgente = destinazione) se ne perdevano i file, e una copia fallita passava per riuscita
    // (R20261114-141207-S4)
    const std::wstring fin = percorso(base, ok), pronto = percorso(base, L"_pronto-" + ok);
    cancellaCartella(pronto);   // (avanzo di un'altra installazione dello stesso nome)
    CreateDirectoryW(pronto.c_str(), nullptr);
    std::wstring schema;
    bool copie = true;
    for (int k = 0; k < N_POSTI && copie; k++) {
        if (scelti[k].empty()) continue;
        const std::wstring nf = nomeFile(scelti[k]), dest = percorso(pronto, nf);
        if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(scelti[k].c_str(), dest.c_str(), TRUE)) copie = false;
        schema += POSTI[k].nome; schema += L'='; schema += nf; schema += L"\r\n";
    }
    if (copie) {
        HANDLE f = CreateFileW(percorso(pronto, L"schema.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD n1 = 0, n2 = 0;
        const WORD bom = 0xFEFF;
        copie = f != INVALID_HANDLE_VALUE && WriteFile(f, &bom, 2, &n1, nullptr) &&
                WriteFile(f, schema.data(), (DWORD)(schema.size() * sizeof(wchar_t)), &n2, nullptr) && n2 == schema.size() * sizeof(wchar_t);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    if (dove == tmp) cancellaCartella(tmp);
    if (!copie) { cancellaCartella(pronto); return no(L"non salvato nella cartella di FluidWin|could not be saved in the FluidWin folder"); }
    // al posto di quello vecchio (se c'era): prima di lato, poi il nuovo al suo nome, poi via il vecchio
    const std::wstring vecchio = percorso(base, L"_vecchio-" + ok);
    cancellaCartella(vecchio);
    const bool cera = GetFileAttributesW(fin.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (cera && !MoveFileW(fin.c_str(), vecchio.c_str())) { cancellaCartella(pronto); return no(L"il pacchetto con questo nome e' in uso|the package with this name is in use"); }
    if (!MoveFileW(pronto.c_str(), fin.c_str())) {
        if (cera) MoveFileW(vecchio.c_str(), fin.c_str());   // (rimesso com'era)
        cancellaCartella(pronto);
        return no(L"non salvato nella cartella di FluidWin|could not be saved in the FluidWin folder");
    }
    if (cera) cancellaCartella(vecchio);
    e.ok = esiste(ok);
    if (!e.ok) e.errore = L"non salvato nella cartella di FluidWin|could not be saved in the FluidWin folder";
    gRevisione++;   // (anche se ha lo stesso nome di quello in uso: va riapplicato, vedi applicaSeCambiato)
    if (registro) { char b[300]; snprintf(b, sizeof(b), "cursori: installato \"%ls\" (%d posti su %d)", ok.c_str(), e.quanti, N_POSTI); registro(b); }
    return e;
}

// ---- applicazione (thread principale) ----
inline HKEY gChiave = nullptr;          // "Control Panel\Cursors", osservata: Windows ha cambiato i cursori
inline HANDLE gAvviso = nullptr;
inline std::wstring gApplicato;         // pacchetto applicato adesso ("" nessuno)
inline unsigned long long gFirma = 0;   // impronta della freccia di sistema appena applicata (0: non si controlla)

// impronta dei pixel di un cursore (misura, punto attivo, maschera e colori): per capire se la freccia di sistema e'
// ancora la nostra. Windows ricarica i suoi cursori anche senza cambiare il registro (Applica sullo stesso schema)
inline unsigned long long firma(HCURSOR c) {
    ICONINFO ii{};
    if (!c || !GetIconInfo(c, &ii)) return 0;
    unsigned long long h = 1469598103934665603ull;
    auto mescola = [&](unsigned long long x) { h = (h ^ x) * 1099511628211ull; };
    mescola((unsigned)ii.xHotspot); mescola((unsigned)ii.yHotspot);
    for (HBITMAP b : {ii.hbmMask, ii.hbmColor}) {
        if (!b) continue;
        BITMAP bm{}; GetObject(b, sizeof(bm), &bm);
        mescola((unsigned)bm.bmWidth); mescola((unsigned)bm.bmHeight);
        if (bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 256 && bm.bmHeight <= 512) {
            std::vector<BYTE> px((size_t)bm.bmWidthBytes * bm.bmHeight);
            GetBitmapBits(b, (LONG)px.size(), px.data());
            for (size_t i = 0; i < px.size(); i += 4) mescola(px[i] | (px[std::min(i + 1, px.size() - 1)] << 8));
        }
    }
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return h;
}

inline void segno(bool si) {
    if (si) { const DWORD uno = 1; RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", SEGNO, REG_DWORD, &uno, sizeof(uno)); }
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", SEGNO);
}

// osserva le impostazioni dei cursori di Windows: se l'utente le cambia, Windows ricarica i suoi e i nostri vanno
// rimessi (vedi controlla)
inline void osserva() {
    if (!gAvviso) gAvviso = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gChiave && RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_NOTIFY, &gChiave) != ERROR_SUCCESS) gChiave = nullptr;
    if (gChiave && gAvviso) RegNotifyChangeKeyValue(gChiave, FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, gAvviso, TRUE);
}
inline void smettiDiOsservare() {
    if (gChiave) { RegCloseKey(gChiave); gChiave = nullptr; }   // (chiudere la chiave annulla l'avviso)
    if (gAvviso) { CloseHandle(gAvviso); gAvviso = nullptr; }
}

// di nuovo i cursori di Windows (lo schema dell'utente). Il segno si toglie solo se Windows li ha ricaricati davvero:
// se no resta, e ci riprova il guardiano o il prossimo avvio (R20261114-141207-S6)
inline bool ricaricaWindows(const char* dove) {
    if (SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0)) { segno(false); return true; }
    if (registro) { char b[160]; snprintf(b, sizeof(b), "cursori: %s, Windows non ha ricaricato i suoi (errore %lu): segno lasciato", dove, GetLastError()); registro(b); }
    return false;
}

inline bool gAttivi = false;            // cursori di FluidWin a schermo (anche durante un cambio di pacchetto)
inline unsigned gRevisioneApplicata = 0;

// i cursori del pacchetto al posto di quelli di sistema; quanti posti. Se c'erano gia' quelli di un altro pacchetto
// prima tornano quelli di Windows: i posti che il pacchetto nuovo non ha non devono restare del vecchio
// (R20261114-141207-S3)
inline int applica(const std::wstring& nome) {
    // la revisione si prende prima di leggere i file: un'installazione che finisce intanto (stesso nome) resta "nuova"
    // e si riapplica (R20261114-142613, resto di S5)
    const unsigned rev = gRevisione;
    std::wstring file[N_POSTI];
    if (!leggiSchema(nome, file)) return 0;
    // c'erano gia' i cursori di un pacchetto: prima quelli di Windows. Se Windows non li ricarica, il cambio non si fa
    // (i posti che il nuovo non ha resterebbero del vecchio): si riprova al prossimo controllo (resto di S3)
    if (gAttivi && !SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0)) {
        if (registro) { char b[160]; snprintf(b, sizeof(b), "cursori: cambio rimandato, Windows non ha ricaricato i suoi (errore %lu)", GetLastError()); registro(b); }
        return -1;
    }
    segno(true);   // prima di cambiarli: se FluidWin cade, il guardiano sa che deve rimetterli
    gAttivi = true;
    int n = 0;
    for (int k = 0; k < N_POSTI; k++) {
        if (file[k].empty()) continue;
        HCURSOR c = LoadCursorFromFileW(file[k].c_str());
        if (!c) continue;
        if (SetSystemCursor(c, POSTI[k].id)) n++;   // (riuscito: il cursore ora e' di Windows, che lo distruggera')
        else DestroyCursor(c);
    }
    gApplicato = nome; gRevisioneApplicata = rev;
    // impronta della freccia (non se e' animata: cambierebbe a ogni fotogramma)
    gFirma = !file[0].empty() && !finisceCon(file[0], L".ani") ? firma(LoadCursorW(nullptr, MAKEINTRESOURCEW(POSTI[0].id))) : 0;
    osserva();
    if (registro) { char b[200]; snprintf(b, sizeof(b), "cursori: \"%ls\" applicato (%d posti)", nome.c_str(), n); registro(b); }
    return n;
}

// di nuovo i cursori di Windows (quelli dello schema dell'utente)
inline void togli() {
    smettiDiOsservare();
    if (gAttivi && ricaricaWindows("ritorno")) { if (registro) registro("cursori: di nuovo quelli di Windows"); gAttivi = false; }
    gApplicato.clear(); gFirma = 0;
}

// impostazione cambiata: il pacchetto scelto ("" = Windows). Anche lo stesso nome se e' stato reinstallato nel frattempo
// (file nuovi: prima restavano a schermo quelli vecchi, R20261114-141207-S5)
inline std::wstring gVoluto;            // pacchetto scelto nelle impostazioni (controlla riprova finche' non e' a schermo)
inline void applicaSeCambiato(const std::wstring& scelto) {
    gVoluto = scelto;
    if (scelto.empty()) { if (gAttivi || !gApplicato.empty()) togli(); return; }
    if (scelto != gApplicato || gRevisione != gRevisioneApplicata) applica(scelto);
}

// avvio e guardiano: FluidWin caduto con i suoi cursori applicati
inline void ripristinaSeServe() {
    DWORD v = 0, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FluidWin", SEGNO, RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS || !v) return;
    ricaricaWindows("ripristino");
}

// ogni tanto (timer di FluidWin): Windows ha ricaricato i suoi cursori (schema cambiato: avviso del registro; Applica
// sullo stesso schema: nessun avviso, ma la freccia di sistema non e' piu' la nostra)? Allora si rimettono i nostri
inline void controlla() {
    // scelto ma non a schermo (cambio rimandato) o reinstallato intanto: si riprova
    if (!gVoluto.empty() && (gVoluto != gApplicato || gRevisione != gRevisioneApplicata)) { applica(gVoluto); return; }
    if (gApplicato.empty()) return;
    const bool avviso = gAvviso && WaitForSingleObject(gAvviso, 0) == WAIT_OBJECT_0;
    const bool cambiata = !avviso && gFirma && firma(LoadCursorW(nullptr, MAKEINTRESOURCEW(POSTI[0].id))) != gFirma;
    if (!avviso && !cambiata) return;
    if (registro) registro(avviso ? "cursori: cambiati da Windows, si rimettono quelli di FluidWin" : "cursori: Windows ha ricaricato i suoi, si rimettono quelli di FluidWin");
    const std::wstring n = gApplicato;
    applica(n);   // (riarma anche l'osservazione)
}

}  // namespace cursori
