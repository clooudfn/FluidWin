// Onda di luce multicolore della ricerca: quando si apre, un'onda parte dal riquadro della ricerca e si allarga
// fino ai bordi dello schermo, che restano accesi con un bagliore colorato che scorre piano finche' la ricerca e'
// aperta; alla chiusura sfuma. La disegna la GPU (un solo triangolo a tutto schermo, tutto nello shader) in una
// finestra trasparente ai clic, appena sotto la ricerca. Il disegno e' in un thread suo, a ritmo con lo schermo:
// resta fluida anche quando FluidWin e' occupato, e il resto di FluidWin non aspetta i suoi fotogrammi.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace onda {

static const char* SHADER = R"(
cbuffer C : register(b0) { float2 dim; float t; float apri; float2 centro; float spegni; float dpi; };
static const float3 TINTE[5] = { float3(0.24, 0.52, 1.00), float3(0.66, 0.36, 1.00), float3(1.00, 0.34, 0.66),
                                 float3(1.00, 0.60, 0.24), float3(0.30, 0.86, 1.00) };
float4 vs(uint id : SV_VertexID) : SV_POSITION {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
// tinta ciclica: blu, viola, rosa, arancio, azzurro e di nuovo blu
float3 tinta(float h) {
    float x = frac(h) * 5.0;
    int i = (int)floor(x);
    float f = smoothstep(0.0, 1.0, frac(x));
    return lerp(TINTE[i % 5], TINTE[(i + 1) % 5], f);
}
// luce dei quattro bordi sommata (con la distanza dal bordo piu' vicino gli angoli avevano pieghe diagonali; cosi'
// si raccordano morbidi e un po' piu' luminosi, come due luci che si sommano)
float bordi(float2 p, float w) {
    return saturate(exp(-p.x / w) + exp(-p.y / w) + exp(-(dim.x - p.x) / w) + exp(-(dim.y - p.y) / w));
}
float4 ps(float4 pos : SV_POSITION) : SV_TARGET {
    float2 p = pos.xy;
    // la tinta gira lenta attorno allo schermo
    float2 q = p - dim * 0.5;
    float ang = atan2(q.y / dim.y, q.x / dim.x) / 6.2831853;
    float h = ang + t * 0.05 + 0.035 * sin(t * 0.8 + ang * 12.566);
    // onda: un anello che parte dal centro della ricerca e arriva oltre l'angolo piu' lontano
    float2 r = p - centro;
    float dist = length(r);
    float lontano = length(max(abs(centro), abs(dim - centro)));
    float R = apri * lontano * 1.06;
    float spess = (36.0 + 150.0 * apri) * dpi;
    float x = (dist - R) / spess;
    float anello = exp(-x * x) * (1.0 - smoothstep(0.7, 1.0, apri)) * smoothstep(0.0, 0.06, apri);
    float dietro = saturate(-x) * exp(-max(-x - 0.4, 0.0) * 1.6) * 0.22 * (1.0 - apri);   // scia tenue dentro l'anello
    // i bordi si accendono dove l'onda e' gia' passata, e respirano piano
    float passata = saturate((R - dist) / (220.0 * dpi));
    float respiro = 1.0 + 0.20 * sin(t * 1.6 + ang * 18.85) + 0.12 * sin(t * 2.7 - ang * 31.4);
    float alone = 0.78 * bordi(p, 46.0 * dpi * respiro) + 0.60 * bordi(p, 8.0 * dpi) + 0.16 * bordi(p, 150.0 * dpi);
    float bordo = alone * passata;
    float onda = anello * 0.60 + dietro;
    float a = saturate(bordo + onda) * spegni;
    if (a <= 0.002) return float4(0, 0, 0, 0);
    float3 cb = tinta(h);
    float3 cw = tinta(atan2(r.y, r.x) / 6.2831853 + t * 0.08);
    float3 c = (cb * bordo + cw * onda) / max(bordo + onda, 0.0001);
    c = lerp(c, float3(1, 1, 1), 0.30 * bordi(p, 2.5 * dpi) * passata);   // filo quasi bianco sul bordo
    return float4(c * a, a);
}
)";

struct Costanti { float dim[2]; float t; float apri; float centro[2]; float spegni; float dpi; };

class Onda {
public:
    // crea la finestra (nel thread di chi chiama: e' quello che poi la mostra e la nasconde) e il thread di disegno
    bool inizializza() {
        if (fin) return true;
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FluidWin.Onda";
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT { return m == WM_CLOSE ? 0 : DefWindowProcW(h, m, w, l); };
        RegisterClassW(&wc);
        fin = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                              WS_EX_TRANSPARENT | WS_EX_LAYERED,
                              wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (!fin) return false;
        SetLayeredWindowAttributes(fin, 0, 255, LWA_ALPHA);
        BOOL niente = TRUE; DwmSetWindowAttribute(fin, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        sveglia = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        InitializeCriticalSection(&cs);
        HANDLE t = sveglia ? CreateThread(nullptr, 0, giro, this, 0, nullptr) : nullptr;
        if (!t) { DestroyWindow(fin); fin = nullptr; return false; }
        CloseHandle(t);
        return true;
    }

    // si apre: zona = schermo (coordinate schermo), da = punto da cui parte l'onda, sotto = finestra sopra di lei
    void apri(const RECT& zona, POINT da, float dpi, HWND sopraDi) {
        if (!inizializza() || rotta) return;
        EnterCriticalSection(&cs);
        z = zona; centro = {(float)(da.x - zona.left), (float)(da.y - zona.top)}; scala = dpi;
        t0 = adesso(); tChiudi = -1; aperta = true; mostrata = false;
        LeaveCriticalSection(&cs);
        sotto = sopraDi;
        SetEvent(sveglia);
    }

    // sfuma (e poi sparisce da sola)
    void chiudi() {
        if (!fin) return;
        EnterCriticalSection(&cs);
        if (aperta && tChiudi < 0) tChiudi = adesso();
        LeaveCriticalSection(&cs);
    }

    // dal thread della finestra, a ogni giro di messaggi utile: la mostra quando il primo fotogramma e' pronto e la
    // nasconde quando ha finito (mostrare e nascondere si fa nel thread che l'ha creata)
    void controlla() {
        if (!fin) return;
        const LONG s = InterlockedExchange(&richiesta, 0);
        if (s == 1) SetWindowPos(fin, sotto && IsWindowVisible(sotto) ? sotto : HWND_TOPMOST, z.left, z.top, z.right - z.left, z.bottom - z.top,
                                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
        else if (s == 2) ShowWindow(fin, SW_HIDE);
    }

    HWND finestra() const { return fin; }
    // Il thread avvisa con questo messaggio la finestra 'avvisa' (dello stesso thread di fin): wParam 0 = mostrare o
    // nascondere l'onda (controlla()), 1 = battito, un fotogramma dello schermo appena passato. I battiti arrivano
    // solo se 'battito' e' acceso, e mai piu' di uno in attesa: le animazioni della finestra vanno al ritmo dello
    // schermo invece che dei timer di Windows (~64 al secondo)
    UINT messaggio = RegisterWindowMessageW(L"FluidWin.Onda");
    HWND avvisa = nullptr;
    volatile bool battito = false;
    void battitoRicevuto() { InterlockedExchange(&battitoInViaggio, 0); }
    bool attiva() const { return fin && !rotta && aperta; }

private:
    HWND fin = nullptr, sotto = nullptr;
    HANDLE sveglia = nullptr;
    CRITICAL_SECTION cs{};
    RECT z{};
    struct { float x, y; } centro{};
    float scala = 1;
    double t0 = 0, tChiudi = -1;
    volatile bool aperta = false, mostrata = false;
    volatile bool rotta = false;
    volatile LONG richiesta = 0;   // 1 mostra, 2 nascondi
    volatile LONG battitoInViaggio = 0;

    static constexpr double DURATA_ONDA = 1.05, DURATA_SFUMA = 0.32;

    static double adesso() {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart;
    }

    void chiedi(LONG s) { InterlockedExchange(&richiesta, s); if (avvisa) PostMessageW(avvisa, messaggio, 0, 0); }

    static DWORD WINAPI giro(LPVOID p) { ((Onda*)p)->disegnaSempre(); return 0; }

    void disegnaSempre() {
        ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
        IDXGISwapChain1* sc = nullptr; ID3D11RenderTargetView* rtv = nullptr;
        IDCompositionDevice* dc = nullptr; IDCompositionTarget* tg = nullptr; IDCompositionVisual* vis = nullptr;
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11Buffer* cb = nullptr; ID3D11BlendState* bl = nullptr;
        int W = 0, H = 0;
        auto rilascia = [&]() {
            for (IUnknown** u : {(IUnknown**)&rtv, (IUnknown**)&sc, (IUnknown**)&vis, (IUnknown**)&tg, (IUnknown**)&dc, (IUnknown**)&vs,
                                 (IUnknown**)&ps, (IUnknown**)&cb, (IUnknown**)&bl, (IUnknown**)&ctx, (IUnknown**)&dev})
                if (*u) { (*u)->Release(); *u = nullptr; }
            W = H = 0;
        };
        auto prepara = [&](int w, int h) -> bool {
            if (dev && w == W && h == H) return true;
            if (dev && sc) {   // schermo cambiato: superficie nuova della misura giusta
                if (rtv) { rtv->Release(); rtv = nullptr; }
                ctx->OMSetRenderTargets(0, nullptr, nullptr); ctx->Flush();
                if (FAILED(sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return false;
            } else {
                D3D_FEATURE_LEVEL fl;
                if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) return false;
                IDXGIDevice* dd = nullptr; dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd);
                { IDXGIDevice1* d1 = nullptr; if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d1))) { d1->SetMaximumFrameLatency(1); d1->Release(); } }
                IDXGIFactory2* fab = nullptr;
                if (!dd || FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&fab))) { if (dd) dd->Release(); return false; }
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                const HRESULT hr = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &sc);
                fab->Release();
                if (FAILED(hr) || FAILED(DCompositionCreateDevice(dd, __uuidof(IDCompositionDevice), (void**)&dc))) { dd->Release(); return false; }
                dd->Release();
                if (FAILED(dc->CreateTargetForHwnd(fin, TRUE, &tg)) || FAILED(dc->CreateVisual(&vis))) return false;
                vis->SetContent(sc); tg->SetRoot(vis); dc->Commit();
                ID3DBlob *bv = nullptr, *bp = nullptr, *err = nullptr;
                if (FAILED(D3DCompile(SHADER, strlen(SHADER), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &bv, &err))) { if (err) err->Release(); return false; }
                if (FAILED(D3DCompile(SHADER, strlen(SHADER), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &bp, &err))) { if (err) err->Release(); bv->Release(); return false; }
                dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &vs);
                dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &ps);
                bv->Release(); bp->Release();
                D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(Costanti); bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                dev->CreateBuffer(&bd, nullptr, &cb);
                D3D11_BLEND_DESC bs{}; bs.RenderTarget[0].BlendEnable = FALSE; bs.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                dev->CreateBlendState(&bs, &bl);
                if (!vs || !ps || !cb || !bl) return false;
            }
            ID3D11Texture2D* bb = nullptr;
            if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
            dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();
            W = w; H = h;
            return rtv != nullptr;
        };
        for (;;) {
            EnterCriticalSection(&cs);
            const bool attiva = aperta;
            const RECT zona = z; const float cx = centro.x, cy = centro.y, d = scala;
            const double inizio = t0, fine = tChiudi;
            LeaveCriticalSection(&cs);
            if (!attiva) { WaitForSingleObject(sveglia, INFINITE); continue; }
            const int w = zona.right - zona.left, h = zona.bottom - zona.top;
            if (w <= 0 || h <= 0 || !prepara(w, h)) {   // GPU non disponibile: niente onda (la ricerca funziona lo stesso)
                rilascia(); rotta = true;
                EnterCriticalSection(&cs); aperta = false; LeaveCriticalSection(&cs);
                chiedi(2);
                continue;
            }
            const double t = adesso();
            const double x = std::clamp((t - inizio) / DURATA_ONDA, 0.0, 1.0);
            const double sp = fine < 0 ? 1.0 : 1.0 - std::clamp((t - fine) / DURATA_SFUMA, 0.0, 1.0);
            Costanti k{{(float)w, (float)h}, (float)(t - inizio), (float)(1 - std::pow(1 - x, 2.4)), {cx, cy},
                       (float)(sp * sp * (3 - 2 * sp)), d};
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &k, sizeof(k)); ctx->Unmap(cb, 0); }
            D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->OMSetBlendState(bl, nullptr, 0xFFFFFFFF);
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetInputLayout(nullptr);
            ctx->VSSetShader(vs, nullptr, 0);
            ctx->PSSetShader(ps, nullptr, 0);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ctx->Draw(3, 0);
            // finita l'onda il bagliore si muove piano: basta un fotogramma ogni ~16 ms anche sugli schermi veloci
            int intervallo = 1;
            if (x >= 1 && fine < 0) {
                DWM_TIMING_INFO ti{}; ti.cbSize = sizeof(ti);
                if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator)
                    intervallo = std::clamp((int)std::lround((double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator / 60.0), 1, 4);
            }
            const HRESULT hr = sc->Present(intervallo, 0);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { rilascia(); continue; }   // si ricrea al prossimo giro
            if (!mostrata) { mostrata = true; chiedi(1); }   // primo fotogramma pronto: ora si puo' mostrare
            if (battito && avvisa && InterlockedCompareExchange(&battitoInViaggio, 1, 0) == 0) PostMessageW(avvisa, messaggio, 1, 0);
            if (sp <= 0) {   // sfumata: sparisce e il thread si riaddormenta
                const float zero[4] = {0, 0, 0, 0};
                ctx->ClearRenderTargetView(rtv, zero); sc->Present(1, 0);
                EnterCriticalSection(&cs);
                if (tChiudi >= 0 && tChiudi == fine) aperta = false;   // (riaperta nel frattempo: si continua)
                LeaveCriticalSection(&cs);
                chiedi(2);
            }
        }
    }
};

}  // namespace onda
