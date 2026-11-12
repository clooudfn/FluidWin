// Compila gli shader di genio.h e onda.h e scrive shader_compilati.h con il loro bytecode. Lo usa compila.ps1 prima
// di compilare FluidWin: cosi' FluidWin non li ricompila a ogni avvio (genio ~19 ms all'avvio, onda ~60 ms alla prima
// ricerca) e non carica nemmeno d3dcompiler_47.dll. Senza shader_compilati.h (per esempio compilando a mano)
// FluidWin li compila da se' come prima.
// Uso: genera_shader.exe shader_compilati.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define FLUIDWIN_GENERA_SHADER   // (genio.h e onda.h non includono uno shader_compilati.h rimasto da prima)
#include <windows.h>
#include <cstdio>
#include "genio.h"
#include "onda.h"

static bool scrivi(FILE* f, const char* nome, const char* sorgente, const char* entrata, const char* profilo) {
    ID3DBlob *b = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(sorgente, strlen(sorgente), nullptr, nullptr, nullptr, entrata, profilo, 0, 0, &b, &err))) {
        fprintf(stderr, "%s: %s\n", nome, err ? (const char*)err->GetBufferPointer() : "errore");
        if (err) err->Release();
        return false;
    }
    const unsigned char* p = (const unsigned char*)b->GetBufferPointer();
    const size_t n = b->GetBufferSize();
    fprintf(f, "inline const unsigned char %s[%zu] = {", nome, n);
    for (size_t i = 0; i < n; i++) fprintf(f, "%s%u,", i % 24 ? "" : "\n    ", p[i]);
    fprintf(f, "\n};\n");
    b->Release();
    return true;
}

int main(int argc, char** argv) {
    const char* uscita = argc > 1 ? argv[1] : "shader_compilati.h";
    FILE* f = fopen(uscita, "wb");
    if (!f) return 1;
    fprintf(f, "// Generato da genera_shader.cpp (compila.ps1): bytecode degli shader di genio.h e onda.h. Non modificare.\n");
    fprintf(f, "#pragma once\n#define FLUIDWIN_SHADER_COMPILATI 1\nnamespace shader_compilati {\n");
    const bool ok = scrivi(f, "GENIO_VS", genio::SHADER, "vs", "vs_4_0") && scrivi(f, "GENIO_PS", genio::SHADER, "ps", "ps_4_0") &&
                    scrivi(f, "ONDA_VS", onda::SHADER, "vs", "vs_4_0") && scrivi(f, "ONDA_PS", onda::SHADER, "ps", "ps_4_0");
    fprintf(f, "}\n");
    fclose(f);
    if (!ok) { remove(uscita); return 1; }   // (meglio nessun file che uno a meta': FluidWin li compila da se')
    return 0;
}
