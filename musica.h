// Musica in riproduzione, letta con l'API ufficiale dei controlli multimediali di Windows
// (Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager): e' la stessa che usa il riquadro
// multimediale di Windows sopra il volume, quindi funziona con Spotify, i browser, Lettore multimediale ecc.
// MinGW non ha le intestazioni di questa API: come in notifiche.h gli IID li si chiede all'oggetto (GetIids) e i
// metodi si chiamano per posizione, nell'ordine dei metadati ufficiali (Windows.Media.winmd). Ogni oggetto avuto
// si controlla col nome della sua classe prima di usarlo: se un aggiornamento di Windows cambiasse qualcosa, la
// musica semplicemente non compare (niente chiamate a caso).
// Tutto gira in un thread a parte (le chiamate aspettano Windows); il thread principale legge solo una copia.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <shcore.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>

namespace musica {

struct Brano {
    bool c = false;          // c'e' una sessione multimediale
    bool suona = false;      // in riproduzione (non in pausa)
    std::wstring titolo, artista, aumid;
    std::vector<UINT> copertina;   // pixel BGRA premoltiplicati, LATO x LATO (vuoto se non c'e')
    UINT versione = 0;             // cambia quando cambiano titolo o copertina
};

constexpr int LATO = 96;   // misura della copertina tenuta in memoria

namespace abi {
template <class F> inline F metodo(void* o, int i) { return (F)(*(void***)o)[i]; }
inline void rilascia(void* o) { if (o) ((IUnknown*)o)->Release(); }
typedef HRESULT (STDMETHODCALLTYPE *FnPtr)(void*, void**);
typedef HRESULT (STDMETHODCALLTYPE *FnStr)(void*, HSTRING*);

inline std::wstring testo(HSTRING s) {
    UINT32 n = 0; const wchar_t* p = WindowsGetStringRawBuffer(s, &n);
    std::wstring r(p ? p : L"", p ? n : 0);
    WindowsDeleteString(s);
    return r;
}

// nome della classe WinRT dell'oggetto (per controllare di avere quello giusto)
inline bool classe(void* o, const wchar_t* nome) {
    if (!o) return false;
    HSTRING s = nullptr;
    if (FAILED(((IInspectable*)o)->GetRuntimeClassName(&s))) return false;
    const bool ok = testo(s) == nome;
    return ok;
}

// aspetta un'operazione asincrona (al massimo 'ms'): true se finita bene
inline bool attendi(void* op, int ms) {
    if (!op) return false;
    static const IID IID_Async = {0x00000036, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};   // IAsyncInfo
    void* info = nullptr; ((IUnknown*)op)->QueryInterface(IID_Async, &info);
    int stato = 0;
    for (int i = 0; i < ms && info; i++) {
        metodo<HRESULT (STDMETHODCALLTYPE*)(void*, int*)>(info, 7)(info, &stato);   // get_Status
        if (stato != 0) break;
        Sleep(1);
    }
    rilascia(info);
    return stato == 1;
}

// aspetta un'operazione asincrona che da' un oggetto e lo prende (GetResults); l'operazione si rilascia
inline void* risultato(void* op, int ms = 1500) {
    void* r = nullptr;
    if (attendi(op, ms)) metodo<FnPtr>(op, 8)(op, &r);
    rilascia(op);
    return r;
}
}  // namespace abi

class Lettore {
public:
    ~Lettore() { ferma(); }

    void avvia() {
        if (lavoro.joinable()) return;
        basta = false;
        lavoro = std::thread([this] { ciclo(); });
    }

    void ferma() {
        basta = true;
        if (lavoro.joinable()) lavoro.join();
    }

    // copia dello stato attuale (thread principale)
    Brano stato() { std::lock_guard<std::mutex> g(mx); return brano; }
    UINT versione() { std::lock_guard<std::mutex> g(mx); return brano.versione ^ (brano.suona ? 0x80000000u : 0) ^ (brano.c ? 0x40000000u : 0); }

    // comandi: li esegue il thread di lettura (che possiede la sessione)
    enum { NIENTE, PLAY_PAUSA, AVANTI, INDIETRO };
    void comando(int c) { daFare = c; }

private:
    std::thread lavoro;
    std::atomic<bool> basta{false};
    std::atomic<int> daFare{NIENTE};
    std::mutex mx;
    Brano brano;
    void* gestore = nullptr;   // IGlobalSystemMediaTransportControlsSessionManager
    std::wstring chiaveCopertina;
    int tentativi = 0;

    static constexpr const wchar_t* NS = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSession";

    void ciclo() {
        RoInitialize(RO_INIT_MULTITHREADED);
        while (!basta) {
            // musica che sembra fermarsi o sparire: si rilegge poco dopo per conferma. Le app (i browser soprattutto)
            // passano per stati di un attimo ("cambiando", sessione assente, titolo non ancora pronto): presi per buoni,
            // il notch si chiudeva e riapriva, e le barrette si fermavano (sembrava sfarfallare)
            if (!leggi(false) && !basta) { Sleep(300); leggi(true); }
            // un comando si esegue subito; poi si rilegge presto (lo stato cambia in poco tempo)
            for (int i = 0; i < 10 && !basta; i++) {
                const int c = daFare.exchange(NIENTE);
                if (c != NIENTE) { esegui(c); Sleep(150); break; }
                Sleep(100);
            }
        }
        abi::rilascia(gestore); gestore = nullptr;
        RoUninitialize();
    }

    bool prepara() {
        if (gestore) return true;
        static ULONGLONG tProva = 0;
        if (tProva && GetTickCount64() - tProva < 10000) return false;   // API assente: si riprova ogni 10 s
        tProva = GetTickCount64();
        HSTRING cl = nullptr; const wchar_t* nome = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        WindowsCreateString(nome, (UINT32)wcslen(nome), &cl);
        IInspectable* fab = nullptr;
        const HRESULT hr = RoGetActivationFactory(cl, __uuidof(IInspectable), (void**)&fab);
        WindowsDeleteString(cl);
        if (FAILED(hr) || !fab) return false;
        static const IID IID_Fabbrica = {0x00000035, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
        ULONG n = 0; IID* ids = nullptr; fab->GetIids(&n, &ids);
        void* stat = nullptr;
        for (ULONG i = 0; i < n && !stat; i++) if (!IsEqualIID(ids[i], IID_Fabbrica)) fab->QueryInterface(ids[i], &stat);
        CoTaskMemFree(ids); fab->Release();
        if (!stat) return false;
        void* op = nullptr;
        abi::metodo<abi::FnPtr>(stat, 6)(stat, &op);   // RequestAsync
        abi::rilascia(stat);
        void* g = abi::risultato(op, 3000);
        if (!abi::classe(g, L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager")) { abi::rilascia(g); return false; }
        gestore = g;
        return true;
    }

    // la sessione attuale (quella che Windows mostra nel suo riquadro), controllata
    void* sessione() {
        if (!prepara()) return nullptr;
        void* s = nullptr;
        abi::metodo<abi::FnPtr>(gestore, 6)(gestore, &s);   // GetCurrentSession
        if (s && !abi::classe(s, NS)) { abi::rilascia(s); s = nullptr; }
        return s;
    }

    // false: stato da confermare (la musica sembra fermarsi o sparire), non ancora preso
    bool leggi(bool conferma) {
        Brano b;
        int st = -1;
        bool proprieta = false;
        void* s = sessione();
        if (s) {
            b.c = true;
            HSTRING a = nullptr;
            if (SUCCEEDED(abi::metodo<abi::FnStr>(s, 6)(s, &a))) b.aumid = abi::testo(a);   // SourceAppUserModelId
            void* info = nullptr;
            abi::metodo<abi::FnPtr>(s, 9)(s, &info);   // GetPlaybackInfo
            if (abi::classe(info, L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionPlaybackInfo")) {
                abi::metodo<HRESULT (STDMETHODCALLTYPE*)(void*, int*)>(info, 7)(info, &st);   // PlaybackStatus
                b.suona = st == 4;   // Playing (1 aperta, 2 cambiando, 3 fermata, 5 in pausa)
            }
            abi::rilascia(info);
            void* op = nullptr;
            abi::metodo<abi::FnPtr>(s, 7)(s, &op);   // TryGetMediaPropertiesAsync
            void* p = abi::risultato(op);
            if (abi::classe(p, L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties")) {
                proprieta = true;
                HSTRING t = nullptr;
                if (SUCCEEDED(abi::metodo<abi::FnStr>(p, 6)(p, &t))) b.titolo = abi::testo(t);    // Title
                t = nullptr;
                if (SUCCEEDED(abi::metodo<abi::FnStr>(p, 9)(p, &t))) b.artista = abi::testo(t);   // Artist
                const std::wstring chiave = b.aumid + L"|" + b.titolo + L"|" + b.artista;
                // la copertina si legge quando cambia il brano; se non c'e' ancora (le app la mettono un po' dopo il
                // titolo) si riprova alle letture successive, al massimo 5 volte
                if (chiave != chiaveCopertina) { chiaveCopertina = chiave; tentativi = 0; std::lock_guard<std::mutex> g(mx); brano.copertina.clear(); brano.versione++; }
                if (tentativi < 5 && brano.copertina.empty()) {
                    tentativi++;
                    std::vector<UINT> px;
                    void* rif = nullptr;
                    abi::metodo<abi::FnPtr>(p, 15)(p, &rif);   // Thumbnail
                    if (rif) { copertina(rif, px); abi::rilascia(rif); }
                    if (!px.empty()) { std::lock_guard<std::mutex> g(mx); brano.copertina = std::move(px); brano.versione++; }
                }
            }
            abi::rilascia(p);
            abi::rilascia(s);
        }
        std::lock_guard<std::mutex> g(mx);
        if (b.c && brano.c && b.aumid == brano.aumid) {
            if (st == 1 || st == 2) b.suona = brano.suona;   // stato di passaggio: resta quello di prima
            if (!proprieta) { b.titolo = brano.titolo; b.artista = brano.artista; }   // titolo non arrivato in tempo
        }
        if (!conferma && ((brano.suona && !b.suona) || (brano.c && !b.c))) return false;
        if (!b.c) { if (brano.c || !brano.copertina.empty()) brano.versione++; brano.copertina.clear(); chiaveCopertina.clear(); }
        if (b.titolo != brano.titolo || b.artista != brano.artista) brano.versione++;
        b.copertina = std::move(brano.copertina);
        b.versione = brano.versione;
        brano = std::move(b);
        return true;
    }

    void esegui(int c) {
        void* s = sessione();
        if (!s) return;
        const int i = c == PLAY_PAUSA ? 20 : c == AVANTI ? 16 : 17;   // TryTogglePlayPause / TrySkipNext / TrySkipPrevious
        void* op = nullptr;
        abi::metodo<abi::FnPtr>(s, i)(s, &op);
        abi::attendi(op, 1000);   // da' un booleano, non un oggetto: niente GetResults
        abi::rilascia(op);
        abi::rilascia(s);
    }

    // copertina: flusso WinRT -> IStream -> WIC, ridotta a LATO x LATO (ritaglio quadrato al centro)
    static void copertina(void* rif, std::vector<UINT>& px) {
        void* op = nullptr;
        abi::metodo<abi::FnPtr>(rif, 6)(rif, &op);   // OpenReadAsync
        void* flusso = abi::risultato(op, 2000);
        if (!flusso) return;
        IStream* is = nullptr;
        CreateStreamOverRandomAccessStream((IUnknown*)flusso, IID_PPV_ARGS(&is));
        abi::rilascia(flusso);
        if (!is) return;
        IWICImagingFactory* w = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&w));
        IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
        IWICBitmapClipper* cl = nullptr; IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
        UINT fw = 0, fh = 0;
        if (w && SUCCEEDED(w->CreateDecoderFromStream(is, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&fw, &fh)) && fw && fh &&
            SUCCEEDED(w->CreateBitmapClipper(&cl))) {
            const UINT q = std::min(fw, fh);
            const WICRect r = {(INT)(fw - q) / 2, (INT)(fh - q) / 2, (INT)q, (INT)q};
            if (SUCCEEDED(cl->Initialize(fr, &r)) && SUCCEEDED(w->CreateBitmapScaler(&sc)) &&
                SUCCEEDED(sc->Initialize(cl, LATO, LATO, WICBitmapInterpolationModeFant)) &&
                SUCCEEDED(w->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                px.resize((size_t)LATO * LATO);
                if (FAILED(cv->CopyPixels(nullptr, LATO * 4, LATO * LATO * 4, (BYTE*)px.data()))) px.clear();
            }
        }
        for (IUnknown* u : {(IUnknown*)cv, (IUnknown*)sc, (IUnknown*)cl, (IUnknown*)fr, (IUnknown*)dec, (IUnknown*)w, (IUnknown*)is}) if (u) u->Release();
    }
};

}  // namespace musica
