// Cose del PC che il notch e il centro di controllo mostrano (letti dal thread principale, a giri leggeri):
//  - microfono in uso: le sessioni audio attive sui dispositivi di registrazione (WASAPI, API documentata), con il
//    nome dell'app dall'istantanea dei processi (senza aprirli); muto del microfono predefinito;
//  - webcam in uso: il registro della privacy di Windows (ConsentStore\webcam, LastUsedTimeStart > LastUsedTimeStop).
//    Non tutte le versioni di Windows lo scrivono: se non c'e', la webcam semplicemente non risulta mai in uso;
//  - batteria (portatili): alimentatore attaccato o staccato, batteria scarica (20% e 10%);
//  - cuffie Bluetooth collegate: un'uscita audio nuova il cui dispositivo (stesso "contenitore") e' Bluetooth; la
//    batteria se Windows la conosce (proprieta' {104EA319-...},2 del dispositivo Bluetooth).
// Gli avvisi (batteria, cuffie) li manda 'avviso'.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <setupapi.h>
#include <devpropdef.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <algorithm>
#include <cwctype>
#include "testi.h"

namespace sensori {

enum Simbolo { S_PUNTINA, S_BATTERIA, S_CUFFIE };

class Sensori {
public:
    static Sensori& io() { static Sensori s; return s; }

    std::vector<std::wstring> micApp, camApp;   // app che usano microfono e webcam
    bool micMuto = false, micC = false;          // muto; c'e' un microfono
    int batteria = -1;                           // percentuale, -1 senza batteria
    bool inCarica = false;                       // alimentatore collegato
    bool caricando = false;                      // ... e la batteria si sta davvero caricando (non gia' piena)
    int minutiRestanti = -1;                     // a batteria: quanto manca secondo Windows, -1 se non si sa
    UINT versione = 0;                           // cambia quando cambia qualcosa di quanto sopra
    // avviso per il notch: testo, simbolo, valore (batteria %, -1 se non si sa), acceso (in carica)
    std::function<void(const std::wstring&, int, int, bool)> avviso;

    // dal timer principale (500 ms): ogni cosa al suo ritmo
    void controlla() {
        giro++;
        if (giro % 2 == 0) { microfono(); webcam(); }
        if (giro % 4 == 1) batteriaPC();
        if (giro % 4 == 3) uscite();
    }

    void muta(bool si) {
        IAudioEndpointVolume* v = volumeMic();
        if (!v) return;
        v->SetMute(si, nullptr); v->Release();
        micMuto = si; versione++;
    }

private:
    unsigned giro = 0;
    IMMDeviceEnumerator* en = nullptr;
    std::set<std::wstring> usciteViste;
    bool primaUscite = true, primaBatteria = true;
    bool caricaPrima = false; int sogliaAvvisata = 101;

    IMMDeviceEnumerator* enumeratore() {
        if (!en) CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&en));
        return en;
    }

    IAudioEndpointVolume* volumeMic() {
        IMMDeviceEnumerator* e = enumeratore();
        IMMDevice* d = nullptr; IAudioEndpointVolume* v = nullptr;
        if (e && SUCCEEDED(e->GetDefaultAudioEndpoint(eCapture, eConsole, &d)) && d) {
            d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, (void**)&v);
            d->Release();
        }
        return v;
    }

    static std::wstring nomeProcesso(DWORD pid, HANDLE snap) {
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (pe.th32ProcessID == pid) return bello(pe.szExeFile);
        return L"";
    }

    // "discord.exe" -> "Discord"
    static std::wstring bello(std::wstring s) {
        const size_t p = s.rfind(L'.'); if (p != std::wstring::npos && p > 0) s.resize(p);
        if (!s.empty()) s[0] = (wchar_t)towupper(s[0]);
        return s;
    }

    void microfono() {
        std::vector<std::wstring> app;
        bool c = false;
        IMMDeviceEnumerator* e = enumeratore();
        IMMDeviceCollection* col = nullptr;
        HANDLE snap = INVALID_HANDLE_VALUE;
        if (e && SUCCEEDED(e->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT n = 0; col->GetCount(&n);
            c = n > 0;
            for (UINT i = 0; i < n; i++) {
                IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d)) || !d) continue;
                IAudioSessionManager2* m = nullptr;
                if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr, (void**)&m)) && m) {
                    IAudioSessionEnumerator* se = nullptr;
                    if (SUCCEEDED(m->GetSessionEnumerator(&se)) && se) {
                        int k = 0; se->GetCount(&k);
                        for (int j = 0; j < k; j++) {
                            IAudioSessionControl* sc = nullptr; if (FAILED(se->GetSession(j, &sc)) || !sc) continue;
                            AudioSessionState st = AudioSessionStateInactive; sc->GetState(&st);
                            IAudioSessionControl2* s2 = nullptr;
                            if (st == AudioSessionStateActive && SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s2))) && s2) {
                                DWORD pid = 0; s2->GetProcessId(&pid);
                                if (pid && pid != GetCurrentProcessId() && s2->IsSystemSoundsSession() != S_OK) {
                                    if (snap == INVALID_HANDLE_VALUE) snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                                    std::wstring nome = snap != INVALID_HANDLE_VALUE ? nomeProcesso(pid, snap) : L"";
                                    if (nome.empty()) nome = testi::t(L"Un'app", L"An app");
                                    if (std::find(app.begin(), app.end(), nome) == app.end()) app.push_back(nome);
                                }
                                s2->Release();
                            }
                            sc->Release();
                        }
                        se->Release();
                    }
                    m->Release();
                }
                d->Release();
            }
            col->Release();
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        bool muto = false;
        if (IAudioEndpointVolume* v = volumeMic()) { BOOL b = FALSE; v->GetMute(&b); muto = b != FALSE; v->Release(); }
        if (app != micApp || c != micC || muto != micMuto) { micApp = std::move(app); micC = c; micMuto = muto; versione++; }
    }

    // registro della privacy: un'app usa la webcam se l'ultimo inizio e' dopo l'ultima fine
    static void inUso(HKEY radice, const wchar_t* chiave, bool nonPacchetto, std::vector<std::wstring>& out) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(radice, chiave, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
        for (DWORD i = 0; i < 500; i++) {
            wchar_t n[512]; DWORD ln = 512;
            if (RegEnumKeyExW(k, i, n, &ln, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            ULONGLONG a = 0, b = 0; DWORD s = sizeof(a);
            if (RegGetValueW(k, n, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &a, &s) != ERROR_SUCCESS) continue;
            s = sizeof(b); RegGetValueW(k, n, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &b, &s);
            if (a && a > b) {
                std::wstring x = n;
                if (nonPacchetto) { const size_t p = x.rfind(L'#'); if (p != std::wstring::npos) x = x.substr(p + 1); x = bello(x); }
                else { const size_t u = x.find(L'_'); if (u != std::wstring::npos) x.resize(u); const size_t p = x.rfind(L'.'); if (p != std::wstring::npos) x = x.substr(p + 1); }
                if (std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x);
            }
        }
        RegCloseKey(k);
    }

    void webcam() {
        std::vector<std::wstring> app;
        const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
        inUso(HKEY_CURRENT_USER, base, false, app);
        inUso(HKEY_CURRENT_USER, (std::wstring(base) + L"\\NonPackaged").c_str(), true, app);
        if (app != camApp) { camApp = std::move(app); versione++; }
    }

    void batteriaPC() {
        SYSTEM_POWER_STATUS ps{};
        if (!GetSystemPowerStatus(&ps)) return;
        const bool c = !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100;
        const int pc = c ? ps.BatteryLifePercent : -1;
        const bool carica = ps.ACLineStatus == 1;
        const bool sta = c && carica && (ps.BatteryFlag & 8);   // 8: in carica
        const int minuti = c && !carica && ps.BatteryLifeTime != (DWORD)-1 ? (int)(ps.BatteryLifeTime / 60) : -1;
        if (pc != batteria || carica != inCarica || sta != caricando || minuti != minutiRestanti) {
            batteria = pc; inCarica = carica; caricando = sta; minutiRestanti = minuti; versione++;
        }
        if (!c) return;
        if (primaBatteria) { primaBatteria = false; caricaPrima = carica; sogliaAvvisata = pc <= 10 ? 10 : pc <= 20 ? 20 : 101; return; }
        wchar_t b[96];
        if (carica != caricaPrima) {
            caricaPrima = carica;
            swprintf(b, 96, carica ? testi::t(L"In carica · %d%%", L"Charging · %d%%") : testi::t(L"Alimentatore scollegato · %d%%", L"Unplugged · %d%%"), pc);
            if (avviso) avviso(b, S_BATTERIA, pc, carica);
            if (carica) sogliaAvvisata = 101;
        }
        if (!carica && pc <= 20 && sogliaAvvisata > (pc <= 10 ? 10 : 20)) {
            sogliaAvvisata = pc <= 10 ? 10 : 20;
            swprintf(b, 96, testi::t(L"Batteria scarica · %d%%", L"Low battery · %d%%"), pc);
            if (avviso) avviso(b, S_BATTERIA, pc, false);
        }
    }

    // uscite audio: una nuova che e' Bluetooth = cuffie collegate
    void uscite() {
        IMMDeviceEnumerator* e = enumeratore();
        IMMDeviceCollection* col = nullptr;
        if (!e || FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) || !col) return;
        std::set<std::wstring> ora;
        std::vector<IMMDevice*> nuove;
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d)) || !d) continue;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) {
                ora.insert(id);
                if (!primaUscite && !usciteViste.count(id)) { nuove.push_back(d); d = nullptr; }
                CoTaskMemFree(id);
            }
            if (d) d->Release();
        }
        col->Release();
        usciteViste = std::move(ora);
        primaUscite = false;
        for (IMMDevice* d : nuove) { cuffie(d); d->Release(); }
    }

    void cuffie(IMMDevice* d) {
        IPropertyStore* ps = nullptr;
        if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)) || !ps) return;
        GUID contenitore{}; bool cc = false;
        std::wstring nome;
        static const PROPERTYKEY CONTENITORE = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(CONTENITORE, &v)) && v.vt == VT_CLSID && v.puuid) { contenitore = *v.puuid; cc = true; }
        PropVariantClear(&v);
        static const PROPERTYKEY NOME = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        if (SUCCEEDED(ps->GetValue(NOME, &v)) && v.vt == VT_LPWSTR && v.pwszVal) nome = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
        if (!cc) return;
        // "Cuffie (AirPods Pro)" -> "AirPods Pro"
        const size_t a = nome.find(L'('), b = nome.rfind(L')');
        if (a != std::wstring::npos && b != std::wstring::npos && b > a + 1) nome = nome.substr(a + 1, b - a - 1);
        bool bt = false; int bat = -1;
        static const DEVPROPKEY BATTERIA = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
        static const DEVPROPKEY CONT_DEV = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
        HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (h == INVALID_HANDLE_VALUE) return;
        SP_DEVINFO_DATA di{}; di.cbSize = sizeof(di);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &di) && i < 5000; i++) {
            DEVPROPTYPE t = 0; GUID g{};
            if (!SetupDiGetDevicePropertyW(h, &di, &CONT_DEV, &t, (BYTE*)&g, sizeof(g), nullptr, 0) || t != DEVPROP_TYPE_GUID || g != contenitore) continue;
            wchar_t ist[512] = L"";
            SetupDiGetDeviceInstanceIdW(h, &di, ist, 512, nullptr);
            if (!_wcsnicmp(ist, L"BTHENUM", 7) || !_wcsnicmp(ist, L"BTHLE", 5) || !_wcsnicmp(ist, L"BTHHFENUM", 9)) bt = true;
            BYTE b8 = 0;
            if (SetupDiGetDevicePropertyW(h, &di, &BATTERIA, &t, &b8, sizeof(b8), nullptr, 0) && t == DEVPROP_TYPE_BYTE && b8 <= 100) bat = b8;
        }
        SetupDiDestroyDeviceInfoList(h);
        if (!bt || !avviso) return;
        wchar_t t[160];
        if (bat >= 0) swprintf(t, 160, L"%ls · %d%%", nome.c_str(), bat);
        else swprintf(t, 160, testi::t(L"%ls · connesse", L"%ls · connected"), nome.c_str());
        avviso(t, S_CUFFIE, bat, false);
    }
};

}  // namespace sensori
