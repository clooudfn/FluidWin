// Effetto "genio" in stile macOS: l'immagine della finestra viene disegnata dalla GPU come una griglia
// che si deforma verso la barra delle applicazioni. Overlay trasparente con Direct3D 11 + DirectComposition.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include "stampa.h"
#include "gpu.h"

namespace genio {

// Immagine della finestra catturata, gia' ritagliata alla parte visibile (senza bordi invisibili)
struct Immagine {
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* tex = nullptr;
    int w = 0, h = 0;
    void libera() { if (srv) srv->Release(); if (tex) tex->Release(); srv = nullptr; tex = nullptr; }
};

struct Vertice { float x, y, u, v; };

// Cosa disegnare in un fotogramma
struct Elemento {
    const Immagine* img;
    RECT finestra;     // rettangolo visibile della finestra a schermo (coordinate schermo)
    RECT icona;        // dove entra nella barra (coordinate schermo)
    double p;          // 0 = finestra intera, 1 = dentro la barra
    double alfa;       // opacita' complessiva
    // in alternativa alla curva del genio: una griglia gia' deformata (finestre gelatinose), colonne x righe vertici
    const Vertice* maglia = nullptr;
    int colonne = 0, righe = 0;
    bool ombra = false;   // disegna solo l'ombra morbida attorno al rettangolo della foto (uv fuori da 0..1)
    double alza = 0;      // ombra: 0 = finestra posata (come l'ombra di Windows), 1 = sollevata (piu' morbida e raccolta)
    double raggio = -1;   // angoli in pixel della foto (< 0: raggioAngoli)
    double raggioY = -1;  // ... in verticale, se la foto e' stirata diversamente nei due versi (< 0: come raggio)
};

static const int RIGHE = 160;   // righe della griglia: piu' righe = curve piu' morbide

struct Costanti { float origine[2]; float dimensione[2]; float alfa; float raggio; float texDim[2]; float ombra; float pad[3]; };

static const char* SHADER = R"(
cbuffer C : register(b0) { float2 origine; float2 dimensione; float alfa; float raggio; float2 texDim; float ombra; float alza; float raggioY; };
Texture2D tex : register(t0);
SamplerState camp : register(s0);
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VO vs(float2 p : POSITION, float2 uv : TEXCOORD0) {
    VO o;
    o.pos = float4((p.x - origine.x) / dimensione.x * 2.0 - 1.0, 1.0 - (p.y - origine.y) / dimensione.y * 2.0, 0, 1);
    o.uv = uv;
    return o;
}
float4 ps(VO i) : SV_TARGET {
    float4 c = tex.Sample(camp, i.uv);
    float2 px = i.uv * texDim;
    if (ombra > 0.5) {
        // ombra attorno al rettangolo arrotondato della foto. Posata somiglia a quella di Windows 11 (piu' scura e
        // lunga sotto); sollevata si fa piu' chiara, piu' sfumata e quasi uguale su tutti i lati
        float a0 = lerp(0.20, 0.13, alza), tau = lerp(10.0, 16.0, alza), giu = lerp(7.0, 3.0, alza);
        float2 c = texDim * 0.5;
        float2 q = abs(px - float2(0, giu) - c) - (c - raggio);
        float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - raggio;
        float s = alfa * a0 * exp(-max(d, 0) / tau);
        return float4(0, 0, 0, s);
    }
    // angoli arrotondati come le finestre di Windows 11 (ellittici nella foto se e' stirata: tondi sullo schermo)
    float2 q = min(px, texDim - px);
    float2 r2 = float2(raggio, raggioY > 0 ? raggioY : raggio);
    float m = 1.0;
    if (q.x < r2.x && q.y < r2.y) {
        float2 d = (r2 - q) / max(r2, 0.001);
        // distanza dal bordo dell'ellisse, in pixel a schermo circa (il raggio piu' piccolo fa da scala)
        m = saturate((1.0 - length(d)) * min(r2.x, r2.y) + 0.5);
    }
    // le foto sono opache (alfa 1); le immagini vive di Windows.Graphics.Capture sono premoltiplicate, con il bordo
    // sottile di DWM semitrasparente e gli angoli gia' tondi: si rispetta la loro trasparenza
    float a = m * alfa;
    return float4(c.rgb * a, c.a * a);
}
)";

class Motore {
public:
    HWND finestra = nullptr;
    POINT origine{};
    int W = 0, H = 0;
    bool visibile = false;
    // ritocco della foto prima di caricarla (FluidWin ci disegna sopra la capsula dei tasti): px = BGRA w x h
    void (*ritocco)(HWND, const RECT& visibile, int w, int h, unsigned int* px) = nullptr;
    HWND sotto = nullptr;   // finestra (topmost) sotto cui mettere l'overlay: la piu' bassa del dock
    float raggioAngoli = 8;  // px: angoli arrotondati delle foto (come le finestre)
    // la parte visibile della finestra si vede tutta sullo schermo, senza nulla sopra (deciso da FluidWin)
    bool (*scoperta)(HWND, const RECT& visibile) = nullptr;
    bool fotoDaSchermo = false;  // l'ultima foto e' stata presa dallo schermo (non da PrintWindow)

    bool inizializza(HINSTANCE inst) {
        WNDCLASSW wc{}; wc.hInstance = inst; wc.lpszClassName = L"FluidWin.Genio";
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT { return m == WM_CLOSE ? 0 : DefWindowProcW(h, m, w, l); };   // mai distrutta da WM_CLOSE
        RegisterClassW(&wc);
        origine = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
        W = GetSystemMetrics(SM_CXVIRTUALSCREEN); H = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        finestra = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                   WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                   wc.lpszClassName, L"", WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, inst, nullptr);
        if (!finestra) return false;
        SetLayeredWindowAttributes(finestra, 0, 255, LWA_ALPHA);
        BOOL niente = TRUE;   // nessuna animazione nativa di comparsa per l'overlay
        DwmSetWindowAttribute(finestra, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
        ShowWindow(finestra, SW_SHOWNOACTIVATE);   // sempre mostrata, ma 1x1 fuori schermo

        // la device di Direct3D condivisa con il dock (gpu.h): una sola per tutto FluidWin
        dev = gpu::condivisa().dev; ctx = gpu::condivisa().ctx;
        if (!dev) return false;
        dev->AddRef(); ctx->AddRef();
        // Ogni passo puo' fallire (memoria video finita, driver che si riavvia): un errore qui deve dare false (il
        // chiamante ripiega sull'animazione a molla), mai un puntatore nullo usato alla riga dopo (audit FW-AUDIT-004).
        // Quelli locali si rilasciano qui; quelli gia' salvati nei membri restano fino all'uscita (succede una volta,
        // all'avvio, e senza genio non si riprova)
        IDXGIDevice* dd = nullptr;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) || !dd) return false;
        { IDXGIDevice1* d1 = nullptr; if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d1))) { d1->SetMaximumFrameLatency(1); d1->Release(); } }   // latenza minima: un solo fotogramma in coda
        IDXGIFactory2* fab = nullptr;
        if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&fab))) { dd->Release(); return false; }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        const HRESULT hsc = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &sc);
        fab->Release();
        if (FAILED(hsc)) { dd->Release(); return false; }
        const HRESULT hdc = DCompositionCreateDevice(dd, __uuidof(IDCompositionDevice), (void**)&dcomp);
        dd->Release();
        if (FAILED(hdc)) return false;
        if (FAILED(dcomp->CreateTargetForHwnd(finestra, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual))) return false;
        visual->SetContent(sc);
        target->SetRoot(visual);
        dcomp->Commit();

        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
        const HRESULT hrt = dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();
        if (FAILED(hrt)) return false;

        ID3DBlob *bvs = nullptr, *bps = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(SHADER, strlen(SHADER), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &bvs, &err))) { if (err) err->Release(); return false; }
        if (FAILED(D3DCompile(SHADER, strlen(SHADER), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &bps, &err))) { if (err) err->Release(); bvs->Release(); return false; }
        dev->CreateVertexShader(bvs->GetBufferPointer(), bvs->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(bps->GetBufferPointer(), bps->GetBufferSize(), nullptr, &ps);
        D3D11_INPUT_ELEMENT_DESC el[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        dev->CreateInputLayout(el, 2, bvs->GetBufferPointer(), bvs->GetBufferSize(), &layout);
        bvs->Release(); bps->Release();

        D3D11_BUFFER_DESC vb{}; vb.ByteWidth = sizeof(Vertice) * RIGHE * 2; vb.Usage = D3D11_USAGE_DYNAMIC;
        vb.BindFlags = D3D11_BIND_VERTEX_BUFFER; vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&vb, nullptr, &vbuf);
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = sizeof(Costanti); cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&cb, nullptr, &cbuf);
        D3D11_SAMPLER_DESC sm{}; sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sm.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sm, &camp);
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bd, &blend);
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
        dev->CreateRasterizerState(&rd, &rast);
        // (le Create* qui sopra lasciano nullptr se falliscono: il disegno li usa tutti senza altri controlli)
        return vs && ps && layout && vbuf && cbuf && camp && blend && rast;
    }

    // Fotografa la finestra (anche app DirectX/Chromium) e la carica sulla GPU, ritagliata alla parte visibile
    // daSchermo: se la finestra si vede intera e scoperta la foto si prende dallo schermo, senza PrintWindow
    // (che la fa ridisegnare tutta: su Chromium e simili si vede un lampo)
    bool cattura(HWND h, const RECT& visibile, Immagine& out, bool daSchermo = false) {
        RECT wr; if (!GetWindowRect(h, &wr)) return false;
        const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        if (ww <= 0 || wh <= 0) return false;
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = ww; bi.bmiHeader.biHeight = -wh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC scr = GetDC(nullptr);
        HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) { if (dib) DeleteObject(dib); ReleaseDC(nullptr, scr); return false; }   // finestra enorme o memoria finita
        HDC mem = CreateCompatibleDC(scr);
        HGDIOBJ vecchio = SelectObject(mem, dib);
        BOOL ok = FALSE;
        if (daSchermo && scoperta && scoperta(h, visibile) && BitBlt(mem, 0, 0, ww, wh, scr, wr.left, wr.top, SRCCOPY)) { ok = TRUE; fotoDaSchermo = true; }
        else {
            // PrintWindow con un limite di tempo (un'app bloccata non deve fermare FluidWin)
            std::vector<UINT> st;
            if (stampa::finestra(h, ww, wh, st)) { memcpy(bits, st.data(), st.size() * sizeof(UINT)); ok = TRUE; }
        }
        GdiFlush();
        // foto nera o non riuscita (alcune app non si lasciano fotografare): se la finestra e' davvero visibile in
        // primo piano la si copia dallo schermo, com'e' adesso
        auto quasiNera = [&]() {
            size_t c = 0, n = 0;
            for (int y = 0; y < wh; y += 16) for (int x = 0; x < ww; x += 16) { n++; c += (((const UINT*)bits)[(size_t)y * ww + x] & 0xFFFFFF) != 0; }
            return c * 200 < n * 1 + 1 || !ok;
        };
        if (quasiNera()) {
            const POINT centro = {(visibile.left + visibile.right) / 2, (visibile.top + visibile.bottom) / 2};
            HWND sopra = WindowFromPoint(centro);
            if (sopra && GetAncestor(sopra, GA_ROOT) == h && BitBlt(mem, 0, 0, ww, wh, scr, wr.left, wr.top, SRCCOPY)) { GdiFlush(); ok = TRUE; fotoDaSchermo = true; }
        }
        const bool fatto = ok && carica(h, visibile, wr, (const UINT*)bits, out);
        SelectObject(mem, vecchio); DeleteDC(mem); DeleteObject(dib); ReleaseDC(nullptr, scr);
        return fatto;
    }

    // Pixel di tutta la finestra (rettangolo wr, BGRA riga per riga) -> foto della parte visibile sulla GPU, con il
    // ritocco (la capsula). false se e' quasi tutta nera (la finestra non era ancora disegnata)
    bool carica(HWND h, const RECT& visibile, const RECT& wr, const UINT* bits, Immagine& out) {
        const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        const int sx = std::clamp((int)(visibile.left - wr.left), 0, ww), sy = std::clamp((int)(visibile.top - wr.top), 0, wh);
        const int vw = std::min((int)(visibile.right - visibile.left), ww - sx), vh = std::min((int)(visibile.bottom - visibile.top), wh - sy);
        if (!bits || vw <= 0 || vh <= 0) return false;
        std::vector<UINT> px((size_t)vw * vh);
        size_t colorati = 0;
        for (int y = 0; y < vh; y++) {
            const UINT* riga = bits + (size_t)(sy + y) * ww + sx;
            for (int x = 0; x < vw; x++) { px[(size_t)y * vw + x] = riga[x] | 0xFF000000u; colorati += (riga[x] & 0xFFFFFF) != 0; }   // opaca
        }
        // foto tutta nera: la finestra non era ancora disegnata, meglio l'animazione a molla
        if (colorati * 200 < px.size()) return false;
        // il bordo di 1 pixel lo disegna DWM: nella foto resta nero, e animando si vedeva una riga scura attorno
        // alla finestra. Diventa il colore appena dentro, un po' piu' scuro (come il bordo vero, grigio)
        if (vw > 2 && vh > 2) {
            auto bordo = [&](size_t i, size_t dentro) {
                if ((px[i] & 0xF8F8F8u) != 0) return;   // non nero: lo ha disegnato l'app
                const UINT c = px[dentro];
                px[i] = 0xFF000000u | ((((c >> 16) & 0xFF) * 13 / 16) << 16) | ((((c >> 8) & 0xFF) * 13 / 16) << 8) | ((c & 0xFF) * 13 / 16);
            };
            for (int x = 0; x < vw; x++) {
                bordo((size_t)x, (size_t)vw + x);
                bordo((size_t)(vh - 1) * vw + x, (size_t)(vh - 2) * vw + x);
            }
            for (int y = 0; y < vh; y++) {
                bordo((size_t)y * vw, (size_t)y * vw + 1);
                bordo((size_t)y * vw + vw - 1, (size_t)y * vw + vw - 2);
            }
        }
        if (ritocco) ritocco(h, visibile, vw, vh, (unsigned int*)px.data());
        out.libera();
        D3D11_TEXTURE2D_DESC td{}; td.Width = vw; td.Height = vh; td.MipLevels = 0; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &out.tex))) return false;
        ctx->UpdateSubresource(out.tex, 0, nullptr, px.data(), vw * 4, 0);
        dev->CreateShaderResourceView(out.tex, nullptr, &out.srv);
        ctx->GenerateMips(out.srv);   // rimpicciolita resta morbida, senza sfarfallio
        out.w = vw; out.h = vh;
        return true;
    }

    // Fuori dalle animazioni la finestra e' 1x1 e fuori schermo: una finestra in primo piano grande quanto lo schermo
    // farebbe credere a Windows che sia aperta un'app a schermo intero (e per esempio silenzierebbe le notifiche).
    // Resta pero' sempre "mostrata": cosi' per attivarla basta allargarla, senza il ritardo di una nuova comparsa.
    void mostra(bool si) {
        if (si == visibile) return;
        visibile = si;
        // sotto al dock (se c'e'): la finestra che si riduce gli passa dietro e sparisce sotto il suo vetro
        if (si) SetWindowPos(finestra, sotto ? sotto : HWND_TOPMOST, origine.x, origine.y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        else SetWindowPos(finestra, nullptr, -32000, -32000, 1, 1, SWP_NOACTIVATE | SWP_NOZORDER);
    }

    // Schermi cambiati (risoluzione, monitor collegato o tolto, rotazione): l'overlay torna a coprire tutto lo schermo
    // virtuale, con una superficie della misura nuova (prima restava quella vecchia e il genio usciva deformato)
    void aggiornaSchermo() {
        origine = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
        const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), hh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if ((w != W || hh != H) && sc && w > 0 && hh > 0) {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            if (rtv) { rtv->Release(); rtv = nullptr; }
            ctx->Flush();
            if (SUCCEEDED(sc->ResizeBuffers(0, (UINT)w, (UINT)hh, DXGI_FORMAT_UNKNOWN, 0))) { W = w; H = hh; }
            ID3D11Texture2D* bb = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) { dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release(); }
        }
        if (visibile) SetWindowPos(finestra, sotto ? sotto : HWND_TOPMOST, origine.x, origine.y, W, H, SWP_NOACTIVATE);
    }

    // la GPU e' stata persa (driver aggiornato o riavviato, errore della scheda video): niente si disegna piu' finche'
    // il motore non si ricrea da capo (FluidWin si riavvia da solo)
    bool perso = false;
    void controllaPresent(HRESULT hr) { if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) perso = true; }
    bool gpuPersa() { if (!perso && dev && dev->GetDeviceRemovedReason() != S_OK) perso = true; return perso; }

    // Disegna un fotogramma con tutti gli elementi e lo presenta (attende il vblank)
    void disegna(const std::vector<Elemento>& el) {
        if (!rtv || perso) return;
        const float zero[4] = {0, 0, 0, 0};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, zero);
        D3D11_VIEWPORT vp{0, 0, (float)W, (float)H, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rast);
        ctx->IASetInputLayout(layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        UINT passo = sizeof(Vertice), off = 0;
        ctx->IASetVertexBuffers(0, 1, &vbuf, &passo, &off);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &camp);
        ctx->VSSetConstantBuffers(0, 1, &cbuf);
        ctx->PSSetConstantBuffers(0, 1, &cbuf);
        const float bf[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(blend, bf, 0xFFFFFFFF);
        for (const Elemento& e : el) {
            if (!e.img || !e.img->srv || e.alfa <= 0.0) continue;
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                Costanti* c = (Costanti*)m.pData;
                c->origine[0] = (float)origine.x; c->origine[1] = (float)origine.y;
                c->dimensione[0] = (float)W; c->dimensione[1] = (float)H;
                c->alfa = (float)e.alfa; c->raggio = e.raggio >= 0 ? (float)e.raggio : raggioAngoli;
                c->texDim[0] = (float)e.img->w; c->texDim[1] = (float)e.img->h;
                c->ombra = e.ombra ? 1.0f : 0.0f;
                c->pad[0] = (float)e.alza;
                c->pad[1] = (float)e.raggioY;
                ctx->Unmap(cbuf, 0);
            }
            ctx->PSSetShaderResources(0, 1, &e.img->srv);
            if (e.maglia) {
                if (e.colonne * 2 > RIGHE * 2) continue;   // il buffer dei vertici ha posto per RIGHE * 2 vertici per striscia
                // una striscia di triangoli per ogni coppia di righe della griglia
                for (int r = 0; r + 1 < e.righe; r++) {
                    if (FAILED(ctx->Map(vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) break;
                    Vertice* o = (Vertice*)m.pData;
                    for (int c = 0; c < e.colonne; c++) { o[c * 2] = e.maglia[r * e.colonne + c]; o[c * 2 + 1] = e.maglia[(r + 1) * e.colonne + c]; }
                    ctx->Unmap(vbuf, 0);
                    ctx->Draw(e.colonne * 2, 0);
                }
                continue;
            }
            if (FAILED(ctx->Map(vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
            griglia(e, (Vertice*)m.pData);
            ctx->Unmap(vbuf, 0);
            ctx->Draw(RIGHE * 2, 0);
        }
        controllaPresent(sc->Present(1, 0));
    }

    // GPU e contesto del genio (le immagini vive della gelatina arrivano qui, senza passare dal processore)
    ID3D11Device* dispositivo() const { return dev; }
    ID3D11DeviceContext* contesto() const { return ctx; }

    // dst diventa una copia di src (stessa misura, stessi livelli di dettaglio), pronta a ricevere le immagini vive
    bool copia(const Immagine& src, Immagine& dst) {
        dst.libera();
        if (!src.tex) return false;
        D3D11_TEXTURE2D_DESC td{}; src.tex->GetDesc(&td);
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &dst.tex))) return false;
        ctx->CopyResource(dst.tex, src.tex);
        if (FAILED(dev->CreateShaderResourceView(dst.tex, nullptr, &dst.srv))) { dst.libera(); return false; }
        dst.w = src.w; dst.h = src.h;
        return true;
    }

    // dopo aver cambiato il livello principale di un'immagine: si rifanno quelli piu' piccoli (rimpicciolita resta morbida)
    void rifaiMips(Immagine& i) { if (i.srv) ctx->GenerateMips(i.srv); }

    // Pulisce l'overlay (nessun contenuto) senza nasconderlo
    void svuota() {
        if (!rtv || perso) return;
        const float zero[4] = {0, 0, 0, 0};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, zero);
        controllaPresent(sc->Present(1, 0));
    }

private:
    static double morbido(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }

    // La curva del genio. Due movimenti che si sovrappongono:
    //  - i lati si piegano verso l'icona, di piu' quanto piu' la riga e' vicina alla barra
    //  - la finestra scivola verso la barra; le righe che la raggiungono si schiacciano nell'icona
    static void griglia(const Elemento& e, Vertice* out) {
        const double L = e.finestra.left, T = e.finestra.top, R = e.finestra.right, B = e.finestra.bottom;
        // la "bocca" e' meta' dell'icona: la finestra si stringe piu' a punta prima di entrarci
        const double ic = (e.icona.left + e.icona.right) / 2.0, mezza = (e.icona.right - e.icona.left) * 0.25;
        const double il = ic - mezza, ir = ic + mezza;
        double ty = e.icona.top;
        if (ty < B + 1) ty = B + 1;                         // la finestra tocca gia' la barra: la "bocca" e' appena sotto
        const double piega = morbido(e.p / 0.45);           // i lati si curvano
        const double scivola = morbido((e.p - 0.22) / 0.78); // poi la finestra scende nella barra
        const double D = ty - T;
        for (int k = 0; k < RIGHE; k++) {
            const double v = (double)k / (RIGHE - 1);
            const double y0 = T + v * (B - T);
            const double y = std::min(y0 + scivola * D, ty);
            const double f = morbido((y - T) / std::max(1.0, D));   // 0 in alto, 1 alla barra
            const double s = f * piega;
            const double xl = L + (il - L) * s, xr = R + (ir - R) * s;
            out[k * 2 + 0] = {(float)xl, (float)y, 0.0f, (float)v};
            out[k * 2 + 1] = {(float)xr, (float)y, 1.0f, (float)v};
        }
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* sc = nullptr;
    IDCompositionDevice* dcomp = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vbuf = nullptr;
    ID3D11Buffer* cbuf = nullptr;
    ID3D11SamplerState* camp = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* rast = nullptr;
};

} // namespace genio
