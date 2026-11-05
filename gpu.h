// Il motore Direct3D condiviso: una sola "device" per dock (con i suoi pannelli e il launcher), genio, gelatina e
// contenuto vivo. Ogni device avvia ~20 thread del driver video e la sua memoria: prima dock e genio ne avevano una a
// testa. Tutti la usano dal thread principale; il contenuto vivo (Windows.Graphics.Capture) anche dai suoi thread,
// per questo e' protetta per l'uso da piu' thread (ID3D10Multithread). L'onda della ricerca resta a parte: disegna in
// un thread suo mentre Direct2D del dock disegna su questa, e i due si pesterebbero lo stato della pipeline.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_3.h>

namespace gpu {

struct Condivisa { ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; };

// Creata alla prima richiesta; mai liberata (vive quanto FluidWin; se la GPU si perde FluidWin si riavvia da capo)
inline const Condivisa& condivisa() {
    static Condivisa g = [] {
        Condivisa c;
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &c.dev, &fl, &c.ctx))) return Condivisa{};
        // latenza minima: un solo fotogramma in coda (come facevano dock e genio)
        IDXGIDevice1* d1 = nullptr;
        if (SUCCEEDED(c.dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d1))) { d1->SetMaximumFrameLatency(1); d1->Release(); }
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(c.ctx->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
        return c;
    }();
    return g;
}

}  // namespace gpu
