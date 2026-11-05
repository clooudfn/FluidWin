// Fabbriche Direct2D e DirectWrite, una sola per tutto FluidWin (finestre di vetro, menu). Ogni fabbrica Direct2D
// crea il suo motore grafico per le finestre che disegna, e ogni motore avvia ~20 thread del driver video: con una
// fabbrica a testa, aprire impostazioni, panoramica e ricerca aggiungeva 20 thread ciascuna. Si usano dal thread
// principale e non si liberano mai (servono fino all'uscita)
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

namespace fabbriche {

inline ID2D1Factory* d2d() {
    static ID2D1Factory* f = [] { ID2D1Factory* x = nullptr; D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &x); return x; }();
    return f;
}

inline IDWriteFactory* testo() {
    static IDWriteFactory* f = [] { IDWriteFactory* x = nullptr; DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&x); return x; }();
    return f;
}

}  // namespace fabbriche
