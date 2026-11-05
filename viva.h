// Contenuto vivo di una finestra con Windows.Graphics.Capture (API ufficiale di Windows 10 1903+, la stessa di
// Strumento di cattura e OBS): mentre la si trascina la gelatina mostra la finestra com'e' adesso (video, testo che
// scorre) invece di una foto ferma. Nessuna iniezione nell'app: e' DWM a passarci le immagini della finestra.
// Senza bordo giallo (Windows 11) e senza puntatore. Le immagini arrivano sulla stessa GPU del genio e si copiano
// nella texture della gelatina, senza passare dalla memoria del processore.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace viva {

namespace WGC = ABI::Windows::Graphics::Capture;
namespace WD3D = ABI::Windows::Graphics::DirectX::Direct3D11;

template <class T> inline void rilascia(T*& p) { if (p) { p->Release(); p = nullptr; } }

inline HRESULT fabbrica(const wchar_t* nome, REFIID iid, void** out) {
    HSTRING s = nullptr;
    HRESULT hr = WindowsCreateString(nome, (UINT32)wcslen(nome), &s);
    if (SUCCEEDED(hr)) { hr = RoGetActivationFactory(s, iid, out); WindowsDeleteString(s); }
    return hr;
}

// chiude un oggetto WinRT che lo prevede (sessione, pool, fotogramma): libera subito le sue risorse
inline void chiudi(IUnknown* o) {
    if (!o) return;
    ABI::Windows::Foundation::IClosable* c = nullptr;
    if (SUCCEEDED(o->QueryInterface(__uuidof(ABI::Windows::Foundation::IClosable), (void**)&c))) { c->Close(); c->Release(); }
}

// Windows.Graphics.Capture c'e' su questo sistema (una volta sola)
inline bool supportata() {
    static int s = -1;
    if (s < 0) {
        s = 0;
        WGC::IGraphicsCaptureSessionStatics* st = nullptr;
        if (SUCCEEDED(fabbrica(L"Windows.Graphics.Capture.GraphicsCaptureSession", __uuidof(WGC::IGraphicsCaptureSessionStatics), (void**)&st))) {
            boolean si = false;
            if (SUCCEEDED(st->IsSupported(&si)) && si) s = 1;
            st->Release();
        }
    }
    return s == 1;
}

class Cattura {
public:
    ~Cattura() { ferma(); }

    // Avvia la cattura della finestra h con la GPU dev. false: non supportata o rifiutata (resta la foto)
    bool avvia(ID3D11Device* dev, HWND h) {
        ferma();
        if (!dev || !IsWindow(h) || !supportata()) return false;
        IDXGIDevice* dd = nullptr;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd))) return false;
        IInspectable* ins = nullptr;
        HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dd, &ins);
        dd->Release();
        if (FAILED(hr)) return false;
        hr = ins->QueryInterface(__uuidof(WD3D::IDirect3DDevice), (void**)&disp);
        ins->Release();
        if (FAILED(hr)) return fallita();

        IGraphicsCaptureItemInterop* interop = nullptr;
        if (FAILED(fabbrica(L"Windows.Graphics.Capture.GraphicsCaptureItem", __uuidof(IGraphicsCaptureItemInterop), (void**)&interop))) return fallita();
        hr = interop->CreateForWindow(h, __uuidof(WGC::IGraphicsCaptureItem), (void**)&item);
        interop->Release();
        if (FAILED(hr) || !item) return fallita();
        ABI::Windows::Graphics::SizeInt32 dim{};
        if (FAILED(item->get_Size(&dim)) || dim.Width <= 0 || dim.Height <= 0) return fallita();
        w = dim.Width; hh = dim.Height;

        WGC::IDirect3D11CaptureFramePoolStatics2* ps = nullptr;
        if (FAILED(fabbrica(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", __uuidof(WGC::IDirect3D11CaptureFramePoolStatics2), (void**)&ps))) return fallita();
        // "free threaded": niente coda di messaggi WinRT sul thread; i fotogrammi si prendono quando servono
        hr = ps->CreateFreeThreaded(disp, ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, dim, &pool);
        ps->Release();
        if (FAILED(hr) || !pool) return fallita();
        if (FAILED(pool->CreateCaptureSession(item, &sessione)) || !sessione) return fallita();
        // senza puntatore e senza il bordo giallo (le app desktop non impacchettate possono farlo senza chiedere)
        WGC::IGraphicsCaptureSession2* s2 = nullptr;
        if (SUCCEEDED(sessione->QueryInterface(__uuidof(WGC::IGraphicsCaptureSession2), (void**)&s2))) { s2->put_IsCursorCaptureEnabled(false); s2->Release(); }
        WGC::IGraphicsCaptureSession3* s3 = nullptr;
        if (SUCCEEDED(sessione->QueryInterface(__uuidof(WGC::IGraphicsCaptureSession3), (void**)&s3))) { s3->put_IsBorderRequired(false); s3->Release(); }
        if (FAILED(sessione->StartCapture())) return fallita();
        // si avvia in un thread a parte e si usa dal thread principale: gli oggetti devono essere "agili"
        // (utilizzabili da qualsiasi thread). Quelli di Windows.Graphics.Capture lo sono; se un giorno non lo fossero,
        // niente contenuto vivo invece di usarli dal thread sbagliato
        IUnknown* ag = nullptr;
        for (IUnknown* o : {(IUnknown*)pool, (IUnknown*)sessione, (IUnknown*)disp}) {
            if (FAILED(o->QueryInterface(__uuidof(IAgileObject), (void**)&ag))) return fallita();
            ag->Release();
        }
        finestra = h;
        stato.store(1, std::memory_order_release);
        return true;
    }

    // 0 in avvio, 1 attiva (le immagini arrivano), -1 non riuscita
    int statoAvvio() const { return stato.load(std::memory_order_acquire); }
    bool attiva() const { return statoAvvio() == 1; }
    int larghezza() const { return w; }
    int altezza() const { return hh; }

    // Prende l'ultimo fotogramma arrivato (scarta i piu' vecchi). Se c'e', copia il suo rettangolo src (coordinate
    // del fotogramma) in dst a (dx, dy) e restituisce true. cw/ch: misura del contenuto del fotogramma
    bool copia(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, int dx, int dy, const RECT& src, int* cw = nullptr, int* ch = nullptr) {
        if (!attiva() || !pool) return false;
        WGC::IDirect3D11CaptureFrame* ultimo = nullptr;
        for (int i = 0; i < 4; i++) {
            WGC::IDirect3D11CaptureFrame* f = nullptr;
            if (FAILED(pool->TryGetNextFrame(&f)) || !f) break;
            if (ultimo) { chiudi(ultimo); ultimo->Release(); }
            ultimo = f;
        }
        if (!ultimo) return false;
        bool fatto = false;
        ABI::Windows::Graphics::SizeInt32 dim{};
        ultimo->get_ContentSize(&dim);
        if (cw) *cw = dim.Width;
        if (ch) *ch = dim.Height;
        WD3D::IDirect3DSurface* sup = nullptr;
        if (SUCCEEDED(ultimo->get_Surface(&sup)) && sup) {
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess* acc = nullptr;
            if (SUCCEEDED(sup->QueryInterface(__uuidof(::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess), (void**)&acc))) {
                ID3D11Texture2D* t = nullptr;
                if (SUCCEEDED(acc->GetInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
                    D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
                    // solo la parte che c'e' davvero (il fotogramma puo' essere piu' piccolo del pool)
                    const LONG r = std::min<LONG>(src.right, std::min<LONG>((LONG)d.Width, dim.Width));
                    const LONG b = std::min<LONG>(src.bottom, std::min<LONG>((LONG)d.Height, dim.Height));
                    if (r > src.left && b > src.top) {
                        D3D11_BOX box{(UINT)src.left, (UINT)src.top, 0, (UINT)r, (UINT)b, 1};
                        ctx->CopySubresourceRegion(dst, 0, (UINT)dx, (UINT)dy, 0, t, 0, &box);
                        fatto = true;
                    }
                    t->Release();
                }
                acc->Release();
            }
            sup->Release();
        }
        // la finestra ha cambiato misura: il pool si rifa' della misura nuova (i prossimi fotogrammi la avranno)
        if ((dim.Width != w || dim.Height != hh) && dim.Width > 0 && dim.Height > 0 && disp) {
            w = dim.Width; hh = dim.Height;
            pool->Recreate(disp, ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, dim);
        }
        chiudi(ultimo); ultimo->Release();
        return fatto;
    }

    void ferma() {
        if (sessione) { chiudi(sessione); rilascia(sessione); }
        if (pool) { chiudi(pool); rilascia(pool); }
        rilascia(item); rilascia(disp);
        finestra = nullptr; w = hh = 0;
    }

private:
    bool fallita() { ferma(); stato.store(-1, std::memory_order_release); return false; }
    std::atomic<int> stato{0};
    HWND finestra = nullptr;
    WD3D::IDirect3DDevice* disp = nullptr;
    WGC::IGraphicsCaptureItem* item = nullptr;
    WGC::IDirect3D11CaptureFramePool* pool = nullptr;
    WGC::IGraphicsCaptureSession* sessione = nullptr;
    int w = 0, hh = 0;
};

// Avvio in sottofondo: crearla costa 35-55 ms (tutto dentro Windows), che sul thread principale bloccavano la
// gelatina proprio quando si afferra la finestra. Un thread solo, sempre lo stesso, con il suo "appartamento" WinRT
// multithread che resta vivo (gli oggetti creati li' restano validi). Finche' non e' pronta si vede la foto.
// La cattura e' condivisa: se la gelatina finisce prima, il thread la libera lui quando ha finito.
inline void avviaInSottofondo(std::shared_ptr<Cattura> c, ID3D11Device* dev, HWND h) {
    struct Coda {
        std::mutex mx; std::condition_variable cv;
        struct Lavoro { std::shared_ptr<Cattura> c; ID3D11Device* dev; HWND h; };
        std::deque<Lavoro> lavori;
        Coda() {
            std::thread([this] {
                RoInitialize(RO_INIT_MULTITHREADED);
                for (;;) {
                    Lavoro l;
                    {
                        std::unique_lock<std::mutex> lk(mx);
                        cv.wait(lk, [this] { return !lavori.empty(); });
                        l = std::move(lavori.front()); lavori.pop_front();
                    }
                    l.c->avvia(l.dev, l.h);
                    l.c.reset();   // se la gelatina e' gia' finita, la cattura si chiude qui
                }
            }).detach();
        }
    };
    static Coda* coda = new Coda;   // mai distrutta: il thread vive quanto FluidWin
    {
        std::lock_guard<std::mutex> lk(coda->mx);
        coda->lavori.push_back({std::move(c), dev, h});
    }
    coda->cv.notify_one();
}

// La GPU del genio si usa anche dal pool di Windows.Graphics.Capture, dai suoi thread: con questa protezione il
// contesto della GPU non viene mai usato da due thread nello stesso istante (una volta, all'avvio)
inline void proteggi(ID3D11DeviceContext* ctx) {
    ID3D10Multithread* mt = nullptr;
    if (ctx && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
}

}  // namespace viva
