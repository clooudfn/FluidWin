// Centro di controllo di FluidWin: i pannelli in stile iOS (controllo, notifiche e calendario, widget, app in
// background) e le viste nella stessa finestra. Metodi di dock::Motore, dichiarati in dock.h (che include questo file).
#pragma once

namespace dock {

// ---- pannelli in stile iOS (al posto di quelli di Windows, che si agganciano alla barra e la fanno risalire) ----

// ---- due schede, come carte: 0 = centro di controllo (anche widget e app in background), 1 = notifiche ----

// ---- viste del centro di controllo, tutte nella stessa finestra ----

inline auto Motore::scambiaScheda() -> void {
    Scheda& a = altra;
    std::swap(pannello, a.pannello); std::swap(vetroPannello, a.vetroPannello); std::swap(scP, a.scP); std::swap(targetP, a.targetP);
    std::swap(visualP, a.visualP); std::swap(effettoP, a.effettoP); std::swap(bersaglioP, a.bersaglioP);
    std::swap(pannelloTipo, a.pannelloTipo); std::swap(pannelloChiuso, a.pannelloChiuso);
    std::swap(pDirty, a.pDirty); std::swap(pAnimazione, a.pAnimazione); std::swap(pChiusura, a.pChiusura); std::swap(pMouseDentro, a.pMouseDentro);
    std::swap(trascinaVolume, a.trascinaVolume); std::swap(trascinaLum, a.trascinaLum);
    std::swap(pT0, a.pT0); std::swap(pChiusoT, a.pChiusoT); std::swap(pK, a.pK); std::swap(PW, a.PW); std::swap(PH, a.PH);
    std::swap(rettPannello, a.rettPannello); std::swap(elementi, a.elementi); std::swap(evidP, a.evidP);
    std::swap(pSopra, a.pSopra); std::swap(pPremuto, a.pPremuto);
    std::swap(posDietro, a.posDietro); std::swap(posObj, a.posObj); std::swap(gira, a.gira); std::swap(davanti, a.davanti); std::swap(chiudiAlle, a.chiudiAlle);
}

inline auto Motore::usa(int slot) -> void { if (slot != slotCorrente) { scambiaScheda(); slotCorrente = slot; } }

// una sola finestra per tutto il centro di controllo: le notifiche non sono piu' una carta a parte
inline auto Motore::slotDiTipo(int) -> int { return 0; }

inline auto Motore::slotDiFinestra(HWND h) const -> int { return (h == pannello || h == vetroPannello) ? slotCorrente : 1 - slotCorrente; }

inline auto Motore::schedaFinestra(HWND h) const -> bool { return h && (h == pannello || h == vetroPannello || h == altra.pannello || h == altra.vetroPannello); }

inline auto Motore::nessunaScheda() const -> bool { return pannelloTipo == P_NESSUNO && altra.pannelloTipo == P_NESSUNO; }

inline auto Motore::adesso() -> double {
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart / f;
}

inline auto Motore::pannelloDaDisegnare() const -> bool {
    return (pannelloTipo != P_NESSUNO && (pDirty || pAnimazione)) || (altra.pannelloTipo != P_NESSUNO && (altra.pDirty || altra.pAnimazione));
}

inline auto Motore::creaPannello(HINSTANCE inst) -> bool {
    PWMAX = std::ceil(336 * dpi); PHMAX = std::ceil(760 * dpi);
    WNDCLASSW wp{}; wp.lpfnWndProc = procPannello; wp.hInstance = inst; wp.lpszClassName = L"FluidWin.Pannello";
    wp.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wp);
    pannello = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wp.lpszClassName, L"FluidWin",
                               WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
    vetroPannello = creaVetro(inst);
    BOOL niente = TRUE; DwmSetWindowAttribute(pannello, DWMWA_TRANSITIONS_FORCEDISABLED, &niente, sizeof(niente));
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fab = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd); dd->GetAdapter(&ad); ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fab);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)PWMAX; sd.Height = (UINT)PHMAX; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    const HRESULT hr = fab->CreateSwapChainForComposition(dev, &sd, nullptr, &scP);
    fab->Release(); ad->Release(); dd->Release();
    if (FAILED(hr)) return false;
    dcomp->CreateTargetForHwnd(pannello, TRUE, &targetP);
    dcomp->CreateVisual(&visualP); visualP->SetContent(scP);
    dcomp->CreateEffectGroup(&effettoP); visualP->SetEffect(effettoP);   // dissolvenza in apertura e chiusura
    targetP->SetRoot(visualP); dcomp->Commit();
    IDXGISurface* sup = nullptr; scP->GetBuffer(0, __uuidof(IDXGISurface), (void**)&sup);
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc->CreateBitmapFromDxgiSurface(sup, &bp, &bersaglioP); sup->Release();
    return bersaglioP != nullptr;
}

inline auto Motore::apriPannello(int tipo) -> void {
    const int slot = slotDiTipo(tipo);
    usa(1 - slot);
    const bool altraAperta = pannelloTipo != P_NESSUNO && !pChiusura;
    usa(slot);
    if (pannelloTipo == tipo && !pChiusura) {
        if (!davanti && altraAperta) { mescola(slot); return; }   // era dietro: torna davanti girando da sinistra
        chiudiPannello(); return;                                   // era davanti: si chiude
    }
    // si sta chiudendo: torna indietro dal punto in cui e' (prima ripartiva da zero, e per 0,3 s dopo la chiusura i
    // clic si ignoravano). Stessa curva dell'apertura, ripresa dalla stessa altezza
    if (pannelloTipo == tipo && pChiusura) {
        const double k = std::clamp(pK < 0 ? 0.0 : pK, 0.0, 1.0);
        const double x = 1 - std::pow(1 - k, 1.0 / 4);
        pT0 = adesso() - x * 0.25;
        pChiusura = false; pAnimazione = true; pDirty = true;
        SetForegroundWindow(pannello);
        ordinaSchede(); usa(slot);
        return;
    }
    if (tipo == P_CONTROLLO) { leggiUscita(); sis.avvia(); sis.rileggi(); }
    pannelloTipo = tipo; pChiusura = false; pT0 = adesso(); pK = -1;
    pSopra = pPremuto = -1; evidP.clear(); trascinaVolume = false; pMouseDentro = false;
    PW = PWMAX;
    // spazio vero sopra l'icona del centro di controllo (schermi bassi, scala al 150%: 1080 px sono 720 punti):
    // nessuna vista lo supera
    const D2D1_RECT_F rc = rettCentro();
    const int y1 = schermo.bottom - H + (int)std::round(rc.top - PAD - 10 * dpi);
    phDisponibile = std::max(200 * dpi, (float)(y1 - schermo.top) - 12 * dpi);
    preparaVista(tipo);
    PH = altezzaVista(tipo);
    phVis = PH; phVel = 0; tVista = -1; vistaPrima = P_NESSUNO; pRipreso = false; pFuocoAvuto = false;
    // sopra l'icona, allineato al suo bordo destro (dentro lo schermo)
    const int x1 = std::clamp(schermo.left + (int)std::round(rc.right + PAD), schermo.left + (int)PW + 8, schermo.right - 8);
    rettPannello = {x1 - (int)PW, y1 - (int)PH, x1, y1};
    posDietro = posObj = 0; gira = false; davanti = true; chiudiAlle = -1;
    posizionaPannello(0);
    ShowWindow(vetroPannello, SW_SHOWNOACTIVATE);
    ShowWindow(pannello, SW_SHOW);
    if (registro) { char b[120]; snprintf(b, 120, "pannello %d aperto: visibile %d, rett %ld,%ld-%ld,%ld", tipo, (int)IsWindowVisible(pannello), rettPannello.left, rettPannello.top, rettPannello.right, rettPannello.bottom); scrivi(b); }
    // l'altra carta, se e' aperta, va dietro: un po' piu' piccola, se ne vede solo il bordo sinistro
    if (altraAperta) { usa(1 - slot); davanti = false; posObj = 1; gira = false; chiudiAlle = -1; pAnimazione = true; usa(slot); }
    prendiFuoco(pannello);   // (come il launcher: senza il primo piano il controllo di sicurezza lo richiudeva dopo 0,6 s)
    ordinaSchede(); usa(slot);
    pDirty = true;
}

// i dati che servono a una vista, letti quando ci si entra
inline auto Motore::preparaVista(int tipo) -> void {
    if (tipo == P_SFONDO) leggiAppSfondo();   // l'elenco nuovo arriva dal thread e l'altezza si adatta
    else if (tipo == P_WIDGET) { widD.aggiorna(); wid = widD.copia(); }
    else if (tipo == P_NOTIFICHE) { Cronometro cr(this, "notifiche"); permessoN = lettoreN.leggi(listaN); }
    else if (tipo == P_CONTROLLO) { leggiUscita(); sis.avvia(); sis.rileggi(); }
    else if (tipo == P_MIXER) mix.leggi();
}

// altezza di tutta la vista (con la testata per quelle interne), mai oltre lo spazio sopra il dock
inline auto Motore::altezzaVista(int tipo) const -> float {
    const float testa = std::round(TESTA * dpi);
    float h;
    if (tipo == P_SFONDO) h = altezzaSfondo() + testa;
    else if (tipo == P_WIDGET) h = std::round(560 * dpi) + testa;
    else if (tipo == P_NOTIFICHE) h = altezzaNotifiche() + testa;
    else if (tipo == P_MIXER) h = altezzaMixer() + testa;
    else h = altezzaControllo();
    return std::min(h, altezzaMassimaP());
}

// Passa a un'altra vista dentro la stessa finestra. origine: il punto da cui nasce (quello toccato), in coordinate
// del pannello. Verso il centro di controllo e' un ritorno: il cerchio si richiude verso il pulsante di partenza
inline auto Motore::cambiaVista(int tipo, D2D1_POINT_2F origine) -> void {
    if (pannelloTipo == P_NESSUNO || pChiusura || tipo == pannelloTipo) return;
    vistaPrima = pannelloTipo; phPrima = PH;
    vistaAvanti = tipo != P_CONTROLLO;
    if (vistaAvanti) origineVista = origineRitorno = origine;
    preparaVista(tipo);
    pannelloTipo = tipo;
    PH = altezzaVista(tipo);
    tVista = adesso();
    if (trascinaVolume || trascinaLum || trascinaMixer >= 0) { trascinaVolume = trascinaLum = false; trascinaMixer = -1; ReleaseCapture(); }
    pSopra = pPremuto = -1; evidP.clear();
    pAnimazione = true; pDirty = true;
}

inline auto Motore::tornaIndietro() -> void { if (pannelloTipo != P_CONTROLLO && pannelloTipo != P_NESSUNO) cambiaVista(P_CONTROLLO, origineRitorno); }

inline auto Motore::inTransizione() const -> bool { return tVista >= 0; }

// La carta dietro torna davanti: esce a sinistra, passa sopra l'altra a meta' strada e si rimette al suo posto
// (in senso antiorario); l'altra intanto scivola dietro
inline auto Motore::mescola(int slot) -> void {
    usa(slot);
    if (pannelloTipo == P_NESSUNO || pChiusura || davanti) return;
    posObj = 0; gira = true; pAnimazione = true; pDirty = true; chiudiAlle = -1;
    usa(1 - slot); posObj = 1; gira = false; pAnimazione = true;
    usa(slot);
    SetForegroundWindow(pannello);
    ordinaSchede(); usa(slot);   // l'attivazione la porterebbe subito in cima: resta sotto fino a meta' giro
}

// ordine delle finestre: la carta davanti (contenuto, poi il suo vetro), poi quella dietro
inline auto Motore::ordinaSchede() -> void {
    usa(0); const bool primaZero = davanti || altra.pannelloTipo == P_NESSUNO;
    HWND c0 = pannello, v0 = vetroPannello, c1 = altra.pannello, v1 = altra.vetroPannello;
    HWND a = primaZero ? c0 : c1, av = primaZero ? v0 : v1, b = primaZero ? c1 : c0, bv = primaZero ? v1 : v0;
    const UINT f = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    SetWindowPos(a, HWND_TOPMOST, 0, 0, 0, 0, f);
    SetWindowPos(av, a, 0, 0, 0, 0, f);
    SetWindowPos(b, av, 0, 0, 0, 0, f);
    SetWindowPos(bv, b, 0, 0, 0, 0, f);
}

// a ogni fotogramma: le carte vanno verso la loro posa, partono le chiusure ritardate, chi gira passa sopra
inline auto Motore::animaSchede() -> void {
    const double ora = adesso();
    for (int s : {0, 1}) {
        usa(s);
        if (pannelloTipo == P_NESSUNO) continue;
        if (chiudiAlle > 0 && ora >= chiudiAlle) { chiudiAlle = -1; chiudiPannello(); usa(s); }
        if (chiudiAlle > 0) pAnimazione = true;   // in attesa della sua chiusura
        if (posDietro != posObj) {
            const double passo = dtFotogramma / 0.30;
            posDietro = std::abs(posObj - posDietro) <= passo ? posObj : posDietro + (posObj > posDietro ? passo : -passo);
            pAnimazione = true;
            if (posDietro == posObj) gira = false;
        }
    }
    for (int s : {0, 1}) {
        usa(s);
        if (pannelloTipo == P_NESSUNO || !gira || davanti || posDietro > 0.5) continue;
        davanti = true; usa(1 - s); davanti = false;   // a meta' giro passa sopra
        ordinaSchede();
        break;
    }
}

// chiusura di tutte e due (clic altrove, Esc): prima quella davanti, l'altra poco dopo, ognuna con la sua animazione
inline auto Motore::chiudiTutte() -> void {
    int fronte = -1;
    for (int s : {0, 1}) { usa(s); if (pannelloTipo != P_NESSUNO && !pChiusura && davanti) fronte = s; }
    for (int s : {0, 1}) {
        usa(s);
        if (pannelloTipo == P_NESSUNO || pChiusura || s == fronte) continue;
        if (fronte < 0) chiudiPannello(); else { chiudiAlle = adesso() + 0.08; pAnimazione = true; }
    }
    if (fronte >= 0) { usa(fronte); chiudiPannello(); }
}

inline auto Motore::nascondiSchede() -> void { for (int s : {0, 1}) { usa(s); nascondiPannello(); } }

inline auto Motore::chiudiPannello() -> void {
    if (pannelloTipo == P_NESSUNO || pChiusura) return;
    // chiuso mentre si apriva: la chiusura parte dall'altezza a cui e' arrivato, non da tutto aperto
    // (tutte le viste crescono dall'icona del centro di controllo e ci rientrano)
    const double k = std::clamp(pK < 0 ? 0.0 : pK, 0.0, 1.0);
    pChiusura = true; pT0 = adesso() - std::sqrt(1 - k) * 0.17; pDirty = true; chiudiAlle = -1;
    if (trascinaVolume || trascinaLum || trascinaMixer >= 0) { trascinaVolume = trascinaLum = false; trascinaMixer = -1; ReleaseCapture(); }
    // si chiude quella davanti e l'altra resta: torna davanti al suo posto (senza giro)
    const int s = slotCorrente;
    usa(1 - s);
    if (pannelloTipo != P_NESSUNO && !pChiusura && chiudiAlle < 0 && !davanti) {
        posObj = 0; gira = false; davanti = true; pAnimazione = true;
        SetForegroundWindow(pannello);
    }
    usa(s);
}

inline auto Motore::altezzaControllo() const -> float { return std::round((518 + (sis.luminosita >= 0 ? 52 : 0)) * dpi); }

inline auto Motore::nascondiPannello() -> void {
    if (pannello) ShowWindow(pannello, SW_HIDE);
    if (vetroPannello) ShowWindow(vetroPannello, SW_HIDE);
    if (pannelloTipo != P_NESSUNO) { pannelloChiuso = pannelloTipo; pChiusoT = adesso(); }
    if (registro && pannelloTipo != P_NESSUNO) { char b[80]; snprintf(b, 80, "pannello %d nascosto%s", pannelloTipo, pChiusura ? " (fine chiusura)" : ""); scrivi(b); }
    pannelloTipo = P_NESSUNO; pChiusura = false; pAnimazione = false; pDirty = false;
    phVis = 0; phVel = 0; tVista = -1; vistaPrima = P_NESSUNO;
    posDietro = posObj = 0; gira = false; davanti = true; chiudiAlle = -1;
}

// k = 0..1 (apertura). Il centro di controllo nasce piccolo sopra la sua pillola e cresce (senza piegarsi), le
// notifiche entrano da destra. Da carta dietro: un po' piu' piccola e spostata a sinistra (se ne vede il bordo);
// mentre torna davanti gira larga da sinistra. Il vetro copre esattamente il contenuto trasformato.
inline auto Motore::posizionaPannello(double k) -> void {
    const RECT& r = rettPannello;
    const float pw = (float)(r.right - r.left), ph = (float)(r.bottom - r.top);
    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
    float tx = 0;
    double alfa = std::min(1.0, k * 2.2);
    {   // nasce piccolo sopra l'icona del centro di controllo nel dock e cresce (senza piegarsi)
        const float s = (float)(0.10 + 0.90 * k);
        const D2D1_RECT_F rc = rettCentro();
        const float ax = std::clamp((float)(schermo.left + (rc.left + rc.right) / 2 - r.left), 0.0f, pw);
        m = D2D1::Matrix3x2F::Scale(s, s, {ax, ph});   // dal bordo in basso, sopra l'icona
    }
    const float pd = (float)(posDietro * posDietro * (3 - 2 * posDietro));
    const float sb = 1 - 0.06f * pd;
    m = m * D2D1::Matrix3x2F::Scale(sb, sb, {pw / 2, ph / 2});
    tx += -40 * dpi * pd;
    if (gira) tx += -(float)(pw * 0.62 * std::sin(M_PI * posDietro));
    alfa *= 1 - 0.18 * pd;
    const int x = r.left + (int)std::lround(tx);
    SetWindowPos(pannello, nullptr, x, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_NOZORDER);
    visualP->SetTransform(m);
    const D2D1_POINT_2F a = m.TransformPoint({0, 0}), b = m.TransformPoint({pw, ph});
    SetWindowPos(vetroPannello, nullptr, x + (int)std::lround(a.x), r.top + (int)std::lround(a.y),
                 std::max(1, (int)std::lround(b.x - a.x)), std::max(1, (int)std::lround(b.y - a.y)), SWP_NOACTIVATE | SWP_NOZORDER);
    SetLayeredWindowAttributes(vetroPannello, 0, (BYTE)std::lround(std::clamp(alfa, 0.0, 1.0) * 255), LWA_ALPHA);
    effettoP->SetOpacity((float)std::clamp(alfa, 0.0, 1.0));
    dcomp->Commit();
}

// restituisce false se il pannello si e' appena chiuso (niente da presentare)
inline auto Motore::renderPannello() -> bool {
    const double ora = adesso();
    double k;
    const bool posaInMoto = posDietro != posObj || chiudiAlle > 0;
    if (!pChiusura) {
        // (niente rimbalzo oltre 1: il contenuto uscirebbe dalla sua finestra e verrebbe tagliato)
        const double x = std::clamp((ora - pT0) / 0.25, 0.0, 1.0);
        k = 1 - std::pow(1 - x, 4);
        pAnimazione = x < 1 || posaInMoto;
    } else {
        const double x = std::clamp((ora - pT0) / 0.17, 0.0, 1.0);
        k = 1 - x * x; pAnimazione = x < 1 || posaInMoto;
        if (x >= 1) { nascondiPannello(); return false; }
    }
    // altezza: va verso quella della vista con una molla appena morbida (il bordo in basso resta fermo, sopra il dock)
    if (phVis <= 0) phVis = PH;
    if (molla(phVis, phVel, PH, 210, 0.86, 0.3)) pAnimazione = true;
    const LONG alto = (LONG)std::lround(phVis);
    if (rettPannello.bottom - rettPannello.top != alto) rettPannello.top = rettPannello.bottom - alto;
    posizionaPannello(k); pK = k;

    // cambio di vista in corso: 0..1
    double e = 1;
    if (tVista >= 0) {
        const double x = std::clamp((ora - tVista) / DURATA_VISTA, 0.0, 1.0);
        e = 1 - std::pow(1 - x, 3);
        if (x >= 1) { tVista = -1; vistaPrima = P_NESSUNO; } else pAnimazione = true;
    }

    const bool scuro = temaScuro();
    const D2D1_COLOR_F ink = scuro ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.88f);
    dc->SetTarget(bersaglioP);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    const float hv = (float)phVis;
    disegnaFondo({0.5f, 0.5f, PW - 0.5f, hv - 0.5f}, scuro);
    elementi.clear();
    dc->PushAxisAlignedClip({0, 0, PW, hv}, D2D1_ANTIALIAS_MODE_ALIASED);
    if (tVista < 0) disegnaVista(pannelloTipo, PH, scuro, ink, D2D1::Matrix3x2F::Identity());
    else {
        const float E = (float)e;
        const D2D1_POINT_2F o = origineVista;
        // il cerchio deve arrivare all'angolo piu' lontano della finestra, alla sua altezza piu' grande
        const float hm = std::max(phPrima, PH);
        float rMax = 0;
        for (D2D1_POINT_2F c : {D2D1_POINT_2F{0, 0}, D2D1_POINT_2F{PW, 0}, D2D1_POINT_2F{0, hm}, D2D1_POINT_2F{PW, hm}})
            rMax = std::max(rMax, std::hypot(c.x - o.x, c.y - o.y));
        rMax += 8 * dpi;
        const D2D1_POINT_2F centro = {PW / 2, hv * 0.35f};
        // sotto: quella che resta indietro (andando avanti la vecchia si allontana; tornando, il centro di controllo
        // torna avanti); sopra: quella nel cerchio, che si apre (avanti) o si richiude verso il suo pulsante (indietro)
        const int sotto = vistaAvanti ? vistaPrima : pannelloTipo, sopra = vistaAvanti ? pannelloTipo : vistaPrima;
        const float hSotto = vistaAvanti ? phPrima : PH, hSopra = vistaAvanti ? PH : phPrima;
        const float lontano = vistaAvanti ? E : 1 - E;   // 0 = davanti, 1 = lontano
        const float rCerchio = rMax * (vistaAvanti ? E : 1 - E);
        {
            const float s = 1 - 0.07f * lontano;
            ID2D1Layer* l = nullptr;
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), 1 - 0.82f * lontano), l);
            disegnaVista(sotto, hSotto, scuro, ink, D2D1::Matrix3x2F::Scale(s, s, centro));
            dc->PopLayer();
        }
        ID2D1EllipseGeometry* cerchio = nullptr;
        f2d->CreateEllipseGeometry({o, rCerchio, rCerchio}, &cerchio);
        if (cerchio && rCerchio > 0.5f) {
            // il contenuto nel cerchio arriva appena piu' grande e si posa (come se emergesse dal punto toccato)
            const float s = 1 + 0.05f * (1 - (vistaAvanti ? E : 1 - E));
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), cerchio, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), 1.0f), nullptr);
            disegnaFondo({0.5f, 0.5f, PW - 0.5f, hv - 0.5f}, scuro);   // il vetro del cerchio copre la vista sotto
            disegnaVista(sopra, hSopra, scuro, ink, D2D1::Matrix3x2F::Scale(s, s, o));
            dc->PopLayer();
            // il bordo del cerchio: un anello di luce che si spegne mentre si allarga
            const float a = 0.28f * (1 - E);
            if (a > 0.01f) {
                pennello->SetColor(D2D1::ColorF(0.35f, 0.62f, 1.0f, a));
                dc->DrawEllipse({o, rCerchio, rCerchio}, pennello, 2.0f * dpi);
            }
        }
        if (cerchio) cerchio->Release();
        elementi.clear();   // durante il cambio niente clic: le zone si ricalcolano a fine animazione
        pDirty = true;
    }
    dc->PopAxisAlignedClip();
    controllaGpu(dc->EndDraw());
    dc->SetTarget(bersaglio);
    if (tVista < 0) pDirty = false;
    return true;
}

// Una vista intera con la trasformazione t (scala del cambio di vista): il centro di controllo, oppure una vista
// interna con la sua testata ("<" e titolo) e sotto il contenuto. Le zone cliccabili del contenuto si spostano
// sotto la testata. h = altezza di tutta la vista
inline auto Motore::disegnaVista(int tipo, float h, bool scuro, D2D1_COLOR_F ink, const D2D1::Matrix3x2F& t) -> void {
    const float phSalvato = PH;
    PH = h; baseVista = t; dc->SetTransform(t);
    if (tipo == P_CONTROLLO) disegnaControllo(scuro, ink);
    else {
        const float testa = std::round(TESTA * dpi);
        disegnaTesta(tipo, scuro, ink);
        const size_t n = elementi.size();
        PH = h - testa;
        baseVista = D2D1::Matrix3x2F::Translation(0, testa) * t; dc->SetTransform(baseVista);
        if (tipo == P_SFONDO) disegnaSfondo(scuro, ink);
        else if (tipo == P_NOTIFICHE) disegnaNotifiche(scuro, ink);
        else if (tipo == P_WIDGET) disegnaWidget(scuro, ink);
        else if (tipo == P_MIXER) disegnaMixer(scuro, ink);
        for (size_t i = n; i < elementi.size(); i++) { elementi[i].r.top += testa; elementi[i].r.bottom += testa; }
    }
    baseVista = D2D1::Matrix3x2F::Identity(); dc->SetTransform(baseVista);
    PH = phSalvato;
}

// testata delle viste interne: "<" con il nome del centro di controllo (si torna li') e il titolo della vista
inline auto Motore::disegnaTesta(int tipo, bool scuro, D2D1_COLOR_F ink) -> void {
    const float testa = std::round(TESTA * dpi), m = 12 * dpi;
    const D2D1_RECT_F ri = {6 * dpi, 6 * dpi, 104 * dpi, testa - 4 * dpi};
    const int e = aggiungi(ri, A_INDIETRO, 0);
    evidenzia(e, ri, scuro, 10 * dpi);
    const float s = (e == pPremuto) ? 0.94f : 1.0f;
    const D2D1_COLOR_F blu = scuro ? D2D1::ColorF(0.40f, 0.68f, 1.0f, 1) : D2D1::ColorF(0.0f, 0.42f, 0.95f, 1);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {ri.left, (ri.top + ri.bottom) / 2}) * baseVista);
    pennello->SetColor(blu);
    const float cy = (ri.top + ri.bottom) / 2, cx = m + 6 * dpi;
    dc->DrawLine({cx + 4 * dpi, cy - 7 * dpi}, {cx - 3 * dpi, cy}, pennello, 2.2f * dpi, tratto);
    dc->DrawLine({cx - 3 * dpi, cy}, {cx + 4 * dpi, cy + 7 * dpi}, pennello, 2.2f * dpi, tratto);
    scrivi(testi::t(L"Controllo", L"Control"), fmtVoce, {cx + 12 * dpi, ri.top, ri.right, ri.bottom});
    dc->SetTransform(baseVista);
    // titolo al centro vero della testata (il formato dei titoli e' allineato a sinistra: si misura)
    const std::wstring titolo = tipo == P_SFONDO ? testi::t(L"In background", L"In background") : tipo == P_WIDGET ? L"Widget"
                              : tipo == P_MIXER ? testi::t(L"Mixer", L"Mixer") : testi::t(L"Notifiche", L"Notifications");
    const float lt = larghezzaTesto(titolo, fmtTitolo);
    pennello->SetColor(ink);
    scrivi(titolo, fmtTitolo, {std::max(110 * dpi, PW / 2 - lt / 2), ri.top, PW - m, ri.bottom});
    pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.08f));
    dc->DrawLine({m, testa - 0.5f}, {PW - m, testa - 0.5f}, pennello, 1.0f);
}

inline auto Motore::aggiungi(const D2D1_RECT_F& r, Azione a, int indice) -> int { elementi.push_back({r, a, indice}); return (int)elementi.size() - 1; }

// Menu sopra una scheda. Durante il suo ciclo dei messaggi l'altra scheda puo' ricevere messaggi e scambiare i
// membri dei pannelli (usa): alla fine si torna sulla scheda di partenza
inline auto Motore::menuScheda(const std::vector<menu::Voce>& v, POINT pt, HWND h) -> UINT {
    const int s = slotCorrente;
    const UINT r = menu::mostra(v, pt, h);
    usa(s);
    return r;
}

// sfondo morbido dell'elemento sotto il mouse (e piu' scuro mentre e' premuto)
inline auto Motore::evidenzia(int e, const D2D1_RECT_F& r, bool scuro, float raggio) -> double {
    if ((int)evidP.size() <= e) evidP.resize(e + 1, 0.0);
    const double o = (e == pPremuto) ? 1.6 : (e == pSopra && pMouseDentro) ? 1.0 : 0.0;
    const double v = evidP[e] + (o - evidP[e]) * (1 - std::exp(-dtFotogramma / 0.05));
    evidP[e] = std::abs(v - o) < 0.01 ? o : v;
    if (evidP[e] != o) pAnimazione = true;
    if (evidP[e] > 0.01) {
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.08f * (float)evidP[e]) : D2D1::ColorF(0, 0, 0, 0.06f * (float)evidP[e]));
        dc->FillRoundedRectangle({r, raggio, raggio}, pennello);
    }
    return evidP[e];
}

inline auto Motore::scrivi(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r) -> void {
    dc->DrawText(s.c_str(), (UINT32)s.size(), f, r, pennello, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

inline auto Motore::chevronDestra(float x, float cy) -> void { linea(-2, -4, 2, 0, x, cy, 1.8f); linea(2, 0, -2, 4, x, cy, 1.8f); }

// ---- mixer del volume (riga "Uscita" del centro di controllo) ----
// Per ogni uscita (la predefinita in cima, le altre solo se ci suona un'app): titolo, la riga dell'uscita e le righe
// delle app. Ogni riga: simbolo o icona (clic = muto), nome, cursore sottile con il pallino, percentuale.
static constexpr float MIX_TESTA = 30, MIX_RIGA = 46, MIX_SPAZIO = 12, MIX_PIEDE = 52;

inline auto Motore::altezzaMixer() const -> float {
    float h = 8;
    for (const auto& u : mix.uscite) h += MIX_TESTA + MIX_RIGA * (1 + u.app.size()) + MIX_SPAZIO;
    h += MIX_PIEDE;
    return std::min(altezzaMassimaP() - std::round(TESTA * dpi), std::round(h * dpi));
}

inline auto Motore::rileggiMixer() -> void {
    mix.leggi();
    const int s = slotCorrente;
    usa(0);
    if (pannelloTipo == P_MIXER && !pChiusura) {
        const float nuova = altezzaVista(P_MIXER);
        if (nuova != PH) { PH = nuova; pAnimazione = true; }
        pDirty = true;
    }
    usa(s);
}

inline auto Motore::mixerDaMouse(float x) -> void {
    for (auto& e : elementi) if (e.azione == A_MIXER_VOL && e.indice == trascinaMixer) {
        const float l = e.r.left + 36 * dpi, r = e.r.right - 44 * dpi;   // il binario dentro la riga
        mix.volume(e.indice / 1000, e.indice % 1000 - 1, (x - l) / std::max(1.0f, r - l));
        if (e.indice % 1000 == 0) leggiVolume();   // l'uscita: anche il volume del dock e del centro di controllo
        pDirty = true; dirty = true;
        return;
    }
}

// le uscite in un menu (si sceglie la predefinita) e le impostazioni audio
inline auto Motore::menuUscite(const D2D1_RECT_F& r) -> void {
    const auto lista = sistema::Sistema::uscite();
    std::vector<menu::Voce> voci = {menu::titolo(testi::t(L"Uscita audio", L"Sound output"))};
    for (size_t i = 0; i < lista.size(); i++) { auto x = menu::voce(lista[i].nome, (UINT)(i + 1)); x.spuntata = lista[i].predefinita; voci.push_back(x); }
    voci.push_back(menu::separatore());
    voci.push_back(menu::voce(testi::t(L"Mixer di Windows", L"Windows volume mixer"), 1001));
    voci.push_back(menu::voce(testi::t(L"Impostazioni audio", L"Sound settings"), 1000));
    POINT pt = {(LONG)r.left, (LONG)r.top}; ClientToScreen(pannello, &pt);
    const UINT s = menuScheda(voci, pt, pannello);
    if (s == 1000) { chiudiPannello(); apriPagina(L"ms-settings:sound"); }
    else if (s == 1001) { chiudiPannello(); apriPagina(L"ms-settings:apps-volume"); }
    else if (s >= 1 && s <= lista.size()) { sistema::Sistema::impostaUscita(lista[s - 1].id); leggiUscita(); leggiVolume(); rileggiMixer(); dirty = true; }
}

inline auto Motore::disegnaMixer(bool scuro, D2D1_COLOR_F ink) -> void {
    const float m = 16 * dpi, limite = PH - MIX_PIEDE * dpi;
    const D2D1_COLOR_F tenue = {ink.r, ink.g, ink.b, ink.a * 0.6f};
    const D2D1_COLOR_F blu = scuro ? D2D1::ColorF(0.40f, 0.68f, 1.0f, 1) : D2D1::ColorF(0.0f, 0.42f, 0.95f, 1);
    float y = 8 * dpi;
    auto riga = [&](size_t u, int a, const std::wstring& nome, float vol, bool muto, ID2D1Bitmap* icona, bool uscita, bool sistema) {
        const D2D1_RECT_F r = {m - 6 * dpi, y, PW - m + 6 * dpi, y + MIX_RIGA * dpi};
        const int indice = (int)u * 1000 + a + 1;
        const int ev = aggiungi(r, A_MIXER_VOL, indice);
        evidenzia(ev, r, scuro, 10 * dpi);
        // simbolo o icona: clic = muto
        const float cx = m + 12 * dpi, cy = y + MIX_RIGA * dpi / 2;
        const D2D1_RECT_F ri = {cx - 15 * dpi, cy - 15 * dpi, cx + 15 * dpi, cy + 15 * dpi};
        const int em = aggiungi(ri, A_MIXER_MUTO, indice);
        evidenzia(em, ri, scuro, 15 * dpi);
        const float op = muto ? 0.4f : 1.0f;
        if (icona) dc->DrawBitmap(icona, {cx - 11 * dpi, cy - 11 * dpi, cx + 11 * dpi, cy + 11 * dpi}, op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else if (uscita) { pennello->SetColor({ink.r, ink.g, ink.b, ink.a * op}); simboloVolume(cx, cy); }
        else if (sistema) { pennello->SetColor({ink.r, ink.g, ink.b, ink.a * op}); simboloCampana(cx, cy); }
        else {
            pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.6f * op));
            dc->FillRoundedRectangle({{cx - 11 * dpi, cy - 11 * dpi, cx + 11 * dpi, cy + 11 * dpi}, 5 * dpi, 5 * dpi}, pennello);
            pennello->SetColor(D2D1::ColorF(1, 1, 1, op));
            scrivi(std::wstring(1, nome.empty() ? L'?' : (wchar_t)towupper(nome[0])), fmtNome, {cx - 11 * dpi, cy - 11 * dpi, cx + 11 * dpi, cy + 11 * dpi});
        }
        if (muto) { pennello->SetColor(D2D1::ColorF(1.0f, 0.27f, 0.23f, 1)); linea(-10, -10, 10, 10, cx, cy, 1.8f); }
        // nome sopra, binario sotto
        const float l = r.left + 36 * dpi, rr = r.right - 44 * dpi;
        pennello->SetColor(muto ? tenue : ink);
        scrivi(nome, uscita ? fmtVoce : fmtSotto, {l, y + 4 * dpi, rr + 40 * dpi, y + 22 * dpi});
        const float ty = y + 32 * dpi, v = muto ? 0.0f : std::clamp(vol, 0.0f, 1.0f);
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.10f));
        dc->FillRoundedRectangle({{l, ty - 2 * dpi, rr, ty + 2 * dpi}, 2 * dpi, 2 * dpi}, pennello);
        pennello->SetColor(blu);
        if (v > 0) dc->FillRoundedRectangle({{l, ty - 2 * dpi, l + (rr - l) * v, ty + 2 * dpi}, 2 * dpi, 2 * dpi}, pennello);
        const float kx = l + (rr - l) * v;
        pennello->SetColor(D2D1::ColorF(1, 1, 1, 1));
        dc->FillEllipse({{kx, ty}, 7 * dpi, 7 * dpi}, pennello);
        pennello->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
        dc->DrawEllipse({{kx, ty}, 7 * dpi, 7 * dpi}, pennello, 1.0f);
        wchar_t pc[16]; swprintf(pc, 16, L"%d", (int)std::lround(vol * 100));
        pennello->SetColor(tenue);
        scrivi(pc, fmtNome, {rr + 8 * dpi, ty - 9 * dpi, r.right, ty + 9 * dpi});
        y += MIX_RIGA * dpi;
    };
    for (size_t u = 0; u < mix.uscite.size() && y < limite; u++) {
        const mixer::Uscita& x = mix.uscite[u];
        // titolo dell'uscita: "Predefinita" oppure il pulsante per renderla predefinita
        pennello->SetColor(tenue);
        scrivi(x.predefinita ? testi::t(L"USCITA PREDEFINITA", L"DEFAULT OUTPUT") : testi::t(L"ALTRA USCITA", L"OTHER OUTPUT"), fmtSotto, {m, y, PW / 2, y + MIX_TESTA * dpi});
        if (!x.predefinita) {
            const std::wstring t = testi::t(L"Rendi predefinita", L"Make default");
            const float lw = larghezzaTesto(t, fmtSotto) + 16 * dpi;
            const D2D1_RECT_F rp = {PW - m - lw, y + 4 * dpi, PW - m, y + MIX_TESTA * dpi - 4 * dpi};
            evidenzia(aggiungi(rp, A_MIXER_PRED, (int)u * 1000), rp, scuro, 10 * dpi);
            pennello->SetColor(blu);
            scrivi(t, fmtSotto, {rp.left + 8 * dpi, rp.top, rp.right, rp.bottom});
        }
        y += MIX_TESTA * dpi;
        riga(u, -1, x.nome, x.volume, x.muto, nullptr, true, false);
        for (size_t a = 0; a < x.app.size() && y < limite; a++) {
            const mixer::App& p = x.app[a];
            ID2D1Bitmap* ic = p.exe.empty() ? nullptr : iconaSfondo(p.exe, nullptr, L"");
            riga(u, (int)a, p.nome, p.volume, p.muto, ic, false, p.sistema);
        }
        y += MIX_SPAZIO * dpi;
    }
    if (mix.uscite.empty()) {
        pennello->SetColor(tenue);
        scrivi(testi::t(L"Nessuna uscita audio", L"No audio output"), fmtSotto, {m, y, PW - m, y + 40 * dpi});
    }
    // piede: le altre uscite e le impostazioni
    const float py = PH - 46 * dpi;
    pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.10f));
    dc->DrawLine({m, py}, {PW - m, py}, pennello, 1.0f);
    const D2D1_RECT_F rp = {m - 8 * dpi, py + 6 * dpi, PW - m + 8 * dpi, PH - 8 * dpi};
    evidenzia(aggiungi(rp, A_ALTRE_USCITE, 0), rp, scuro, 6 * dpi);
    pennello->SetColor(ink);
    scrivi(testi::t(L"Uscite e impostazioni audio", L"Outputs and sound settings"), fmtVoce, {m, rp.top, PW - m - 20 * dpi, rp.bottom});
    chevronDestra(PW - m - 4 * dpi, (rp.top + rp.bottom) / 2);
}

// "App in background": icone di notifica delle app in esecuzione, in una griglia come su iOS
inline auto Motore::disegnaSfondo(bool scuro, D2D1_COLOR_F ink) -> void {
    const float m = 16 * dpi;
    pennello->SetColor(ink);
    // (il titolo e' nella testata della vista)
    const float tw = (PW - 2 * m) / 4, th = 86 * dpi, top = 10 * dpi;
    if (appSfondo.empty()) {
        pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.6f});
        scrivi(testi::t(L"Nessuna app in background", L"No apps in the background"), fmtSotto, {m, top, PW - m, top + 40 * dpi});
    }
    for (size_t i = 0; i < appSfondo.size() && i < 24; i++) {
        const float x = m + (i % 4) * tw, y = top + (i / 4) * th;
        const D2D1_RECT_F r = {x + 2 * dpi, y, x + tw - 2 * dpi, y + th - 6 * dpi};
        const int e = aggiungi(r, A_APP, (int)i);
        evidenzia(e, r, scuro, 6 * dpi);
        const float s = (e == pPremuto) ? 0.9f : 1.0f;   // si "schiaccia" alla pressione
        const float cx = (r.left + r.right) / 2, cy = r.top + 30 * dpi, l = 22 * dpi * s;
        if (appSfondo[i].icona) dc->DrawBitmap(appSfondo[i].icona, D2D1::RectF(cx - l, cy - l, cx + l, cy + l), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else { pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.9f)); dc->FillRoundedRectangle({{cx - l, cy - l, cx + l, cy + l}, 10 * dpi, 10 * dpi}, pennello); }
        pennello->SetColor(ink);
        scrivi(appSfondo[i].nome, fmtNome, {r.left + 3 * dpi, cy + 26 * dpi, r.right - 3 * dpi, cy + 44 * dpi});
    }
    // piede: il pannello di Windows, per i menu col tasto destro delle singole app
    const float py = PH - 46 * dpi;
    pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.10f));
    dc->DrawLine({m, py}, {PW - m, py}, pennello, 1.0f);
    const D2D1_RECT_F rp = {m - 8 * dpi, py + 6 * dpi, PW - m + 8 * dpi, PH - 8 * dpi};
    evidenzia(aggiungi(rp, A_TUTTE, 0), rp, scuro, 6 * dpi);
    pennello->SetColor(ink);
    scrivi(testi::t(L"Pannello di Windows", L"Windows panel"), fmtVoce, {m, rp.top, PW - m - 20 * dpi, rp.bottom});
    chevronDestra(PW - m - 4 * dpi, (rp.top + rp.bottom) / 2);
}

// ---- notifiche e calendario (clic sull'ora) ----

// altezza massima di un pannello adesso: quella della sua superficie e lo spazio sopra l'isola
inline auto Motore::altezzaMassimaP() const -> float { return std::min(PHMAX, phDisponibile > 0 ? phDisponibile : PHMAX); }

inline auto Motore::schedeVisibili() const -> int {
    const int spazio = (int)((altezzaMassimaP() / dpi - TESTA - N_TESTA - N_CAL - N_TITOLO - N_PIEDE - 10) / N_SCHEDA);
    return std::max(1, std::min((int)listaN.size(), spazio));
}

inline auto Motore::altezzaNotifiche() const -> float {
    const float schede = (!permessoN || listaN.empty()) ? 56 : schedeVisibili() * N_SCHEDA + ((int)listaN.size() > schedeVisibili() ? 22 : 0);
    return std::min(altezzaMassimaP() - std::round(TESTA * dpi), std::round((N_TESTA + N_CAL + N_TITOLO + schede + N_PIEDE) * dpi));
}

// dopo aver tolto notifiche: elenco nuovo e pannello della nuova altezza (il bordo in basso resta fermo)
inline auto Motore::ricaricaNotifiche() -> void {
    permessoN = lettoreN.leggi(listaN);
    const float nuova = altezzaVista(P_NOTIFICHE);
    if (nuova != PH) { PH = nuova; pAnimazione = true; }   // l'altezza ci arriva con la molla (il bordo in basso resta fermo)
    pSopra = pPremuto = -1; evidP.clear(); pDirty = true;
}

inline auto Motore::quandoN(INT64 t) -> std::wstring {
    FILETIME f; GetSystemTimeAsFileTime(&f);
    const INT64 ora = ((INT64)f.dwHighDateTime << 32) | f.dwLowDateTime;
    const INT64 s = std::max<INT64>(0, (ora - t) / 10000000);
    wchar_t b[32];
    if (s < 60) return testi::t(L"adesso", L"now");
    if (s < 3600) { swprintf(b, 32, testi::t(L"%d min fa", L"%d min ago"), (int)(s / 60)); return b; }
    if (s < 86400) { swprintf(b, 32, testi::t(L"%d h fa", L"%d h ago"), (int)(s / 3600)); return b; }
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)}, lt; SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
    GetDateFormatEx(testi::locale(), 0, &st, testi::t(L"d MMM", L"MMM d"), b, 32, nullptr);
    return b;
}

inline auto Motore::disegnaNotifiche(bool scuro, D2D1_COLOR_F ink) -> void {
    const float m = 16 * dpi;
    const D2D1_COLOR_F tenue = {ink.r, ink.g, ink.b, ink.a * 0.6f};
    const D2D1_COLOR_F scheda = scuro ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(1, 1, 1, 0.5f);
    // ora grande e data
    pennello->SetColor(ink);
    scrivi(testoOra, fmtGrande, {m, 8 * dpi, PW - m, 52 * dpi});
    std::wstring data = testoDataLunga; if (!data.empty()) data[0] = (wchar_t)towupper(data[0]);
    pennello->SetColor(tenue);
    scrivi(data, fmtVoce, {m + 2 * dpi, 52 * dpi, PW - m, 72 * dpi});

    // calendario del mese, la settimana inizia di lunedi', oggi in un cerchio blu
    const float ct = N_TESTA * dpi, cb = ct + (N_CAL - 10) * dpi;
    pennello->SetColor(scheda);
    dc->FillRoundedRectangle({{m, ct, PW - m, cb}, 12 * dpi, 12 * dpi}, pennello);
    SYSTEMTIME oggi; GetLocalTime(&oggi);
    wchar_t mese[64] = L""; GetDateFormatEx(testi::locale(), 0, &oggi, L"MMMM yyyy", mese, 64, nullptr);
    if (mese[0]) mese[0] = (wchar_t)towupper(mese[0]);
    pennello->SetColor(ink);
    scrivi(mese, fmtVoce, {m + 12 * dpi, ct + 8 * dpi, PW - m, ct + 28 * dpi});
    SYSTEMTIME primo = oggi; primo.wDay = 1;
    FILETIME fp; SystemTimeToFileTime(&primo, &fp); FileTimeToSystemTime(&fp, &primo);   // giorno della settimana del primo
    const int inizio = (primo.wDayOfWeek + 6) % 7;   // 0 = lunedi'
    static const int giorniMese[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int giorni = giorniMese[oggi.wMonth - 1];
    if (oggi.wMonth == 2 && ((oggi.wYear % 4 == 0 && oggi.wYear % 100 != 0) || oggi.wYear % 400 == 0)) giorni = 29;
    const float cw = (PW - 2 * m - 16 * dpi) / 7, x0 = m + 8 * dpi, rh = 18 * dpi, y0 = ct + 54 * dpi;
    for (int g = 0; g < 7; g++) {   // iniziali dei giorni nella lingua dell'utente (lunedi' 1/1/2024)
        SYSTEMTIME s{}; s.wYear = 2024; s.wMonth = 1; s.wDay = (WORD)(1 + g);
        wchar_t n[16] = L""; GetDateFormatEx(testi::locale(), 0, &s, L"ddd", n, 16, nullptr);
        std::wstring iniziale(1, (wchar_t)towupper(n[0]));
        pennello->SetColor(tenue);
        scrivi(iniziale, fmtNome, {x0 + g * cw, ct + 32 * dpi, x0 + (g + 1) * cw, ct + 48 * dpi});
    }
    for (int d = 1; d <= giorni; d++) {
        const int cella = inizio + d - 1;
        const float x = x0 + (cella % 7) * cw, y = y0 + (cella / 7) * rh;
        if (d == oggi.wDay) {
            pennello->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1));
            dc->FillEllipse({{x + cw / 2, y + rh / 2}, 9 * dpi, 9 * dpi}, pennello);
            pennello->SetColor(D2D1::ColorF(1, 1, 1, 1));
        } else pennello->SetColor((cella % 7) >= 5 ? tenue : ink);
        scrivi(std::to_wstring(d), fmtNome, {x, y, x + cw, y + rh});
    }

    // notifiche
    const float nt = (N_TESTA + N_CAL) * dpi;
    pennello->SetColor(ink);
    scrivi(testi::t(L"Notifiche", L"Notifications"), fmtTitolo, {m, nt, PW - m, nt + 28 * dpi});
    if (permessoN && !listaN.empty()) {
        const float lw = larghezzaTesto(testi::t(L"Cancella tutto", L"Clear all"), fmtSotto) + 16 * dpi;
        const D2D1_RECT_F rc = {PW - m - lw, nt + 3 * dpi, PW - m, nt + 25 * dpi};
        evidenzia(aggiungi(rc, A_CANCELLA, 0), rc, scuro, 11 * dpi);
        pennello->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1));
        scrivi(testi::t(L"Cancella tutto", L"Clear all"), fmtSotto, {rc.left + 8 * dpi, rc.top, rc.right, rc.bottom});
    }
    float y = nt + N_TITOLO * dpi;
    if (!permessoN || listaN.empty()) {
        pennello->SetColor(scheda);
        dc->FillRoundedRectangle({{m, y, PW - m, y + 46 * dpi}, 12 * dpi, 12 * dpi}, pennello);
        pennello->SetColor(tenue);
        scrivi(permessoN ? testi::t(L"Nessuna notifica", L"No notifications")
                         : testi::t(L"Consenti l'accesso alle notifiche nelle impostazioni", L"Allow access to notifications in Settings"),
               fmtSotto, {m + 14 * dpi, y, PW - m - 10 * dpi, y + 46 * dpi});
    } else {
        const int quante = schedeVisibili();
        for (int i = 0; i < quante; i++) {
            const notifiche::Notifica& n = listaN[i];
            const D2D1_RECT_F r = {m, y, PW - m, y + (N_SCHEDA - 8) * dpi};
            pennello->SetColor(scheda);
            dc->FillRoundedRectangle({r, 12 * dpi, 12 * dpi}, pennello);
            const int e = aggiungi(r, A_NOTIFICA, i);
            evidenzia(e, r, scuro, 12 * dpi);
            // icona dell'app (dal suo AppUserModelID): la prepara il thread delle icone, intanto l'iniziale
            auto it = iconeN.find(n.aumid);
            if (it == iconeN.end()) {
                it = iconeN.emplace(n.aumid, nullptr).first;
                if (!n.aumid.empty()) chiediIcona(L"shell:AppsFolder\\" + n.aumid, 64);
            }
            const D2D1_RECT_F ri = {r.left + 12 * dpi, r.top + 12 * dpi, r.left + 40 * dpi, r.top + 40 * dpi};
            if (it != iconeN.end() && it->second) dc->DrawBitmap(it->second, ri, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else {
                pennello->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.6f));
                dc->FillRoundedRectangle({ri, 7 * dpi, 7 * dpi}, pennello);
                pennello->SetColor(D2D1::ColorF(1, 1, 1, 1));
                scrivi(std::wstring(1, n.app.empty() ? L'?' : (wchar_t)towupper(n.app[0])), fmtNome, ri);
            }
            const float tx = r.left + 50 * dpi;
            const std::wstring q = quandoN(n.quando);
            const float qw = larghezzaTesto(q, fmtSotto);
            const bool sopra = pMouseDentro && (pSopra == e || (pSopra == e + 1));
            pennello->SetColor(tenue);
            scrivi(n.app, fmtSotto, {tx, r.top + 8 * dpi, r.right - qw - 34 * dpi, r.top + 24 * dpi});
            if (!sopra) scrivi(q, fmtSotto, {r.right - qw - 12 * dpi, r.top + 8 * dpi, r.right, r.top + 24 * dpi});
            pennello->SetColor(ink);
            scrivi(n.titolo.empty() ? n.app : n.titolo, fmtVoce, {tx, r.top + 24 * dpi, r.right - 12 * dpi, r.top + 42 * dpi});
            pennello->SetColor(tenue);
            scrivi(n.testo, fmtSotto, {tx, r.top + 42 * dpi, r.right - 12 * dpi, r.bottom - 4 * dpi});
            // X per togliere la notifica, solo col mouse sopra
            const D2D1_RECT_F rx = {r.right - 30 * dpi, r.top + 6 * dpi, r.right - 6 * dpi, r.top + 30 * dpi};
            const int ex = aggiungi(rx, A_RIMUOVI, i);
            if (sopra) {
                const double h = evidenzia(ex, rx, scuro, 12 * dpi);
                pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.14f + 0.1f * (float)h) : D2D1::ColorF(0, 0, 0, 0.08f + 0.06f * (float)h));
                dc->FillEllipse({{(rx.left + rx.right) / 2, (rx.top + rx.bottom) / 2}, 10 * dpi, 10 * dpi}, pennello);
                pennello->SetColor(ink);
                linea(-3.5f, -3.5f, 3.5f, 3.5f, (rx.left + rx.right) / 2, (rx.top + rx.bottom) / 2, 1.5f);
                linea(3.5f, -3.5f, -3.5f, 3.5f, (rx.left + rx.right) / 2, (rx.top + rx.bottom) / 2, 1.5f);
            }
            y += N_SCHEDA * dpi;
        }
        if ((int)listaN.size() > quante) {
            wchar_t b[48]; swprintf(b, 48, testi::t(L"+ altre %d", L"+ %d more"), (int)listaN.size() - quante);
            pennello->SetColor(tenue);
            scrivi(b, fmtSotto, {m + 4 * dpi, y - 4 * dpi, PW - m, y + 16 * dpi});
        }
    }

    // piede: impostazioni delle notifiche
    const float py = PH - 46 * dpi;
    pennello->SetColor(D2D1::ColorF(scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, scuro ? 1.f : 0.f, 0.10f));
    dc->DrawLine({m, py}, {PW - m, py}, pennello, 1.0f);
    const D2D1_RECT_F rp = {m - 8 * dpi, py + 6 * dpi, PW - m + 8 * dpi, PH - 8 * dpi};
    evidenzia(aggiungi(rp, A_IMPOST_NOTIFICHE, 0), rp, scuro, 6 * dpi);
    pennello->SetColor(ink);
    scrivi(testi::t(L"Impostazioni notifiche", L"Notification settings"), fmtVoce, {m, rp.top, PW - m - 20 * dpi, rp.bottom});
    chevronDestra(PW - m - 4 * dpi, (rp.top + rp.bottom) / 2);
}

// "Centro di controllo": rete, Bluetooth, volume a capsula, uscita audio, scorciatoie
inline auto Motore::disegnaControllo(bool scuro, D2D1_COLOR_F ink) -> void {
    const float m = 16 * dpi, larg = PW - 2 * m, mezzo = (larg - 10 * dpi) / 2;
    float y = m;
    // Wi-Fi: interruttore vero se c'e' la scheda; senza (PC col cavo) lo stato della rete, e il clic apre le impostazioni
    const int wf = sis.wifi, bt = sis.bluetooth;
    if (wf >= 0) piastrella({m, y, m + mezzo, y + 64 * dpi}, A_RETE, wf == 1, L"Wi-Fi",
                            wf == 1 ? (rete == 2 ? nomeRete : testi::t(L"Acceso", L"On")) : testi::t(L"Spento", L"Off"), scuro, ink);
    else piastrella({m, y, m + mezzo, y + 64 * dpi}, A_RETE, rete != 0, rete == 2 ? L"Wi-Fi" : rete == 1 ? L"Ethernet" : testi::t(L"Rete", L"Network"),
                    rete ? testi::t(L"Connessa", L"Connected") : testi::t(L"Non connessa", L"Not connected"), scuro, ink);
    piastrella({m + mezzo + 10 * dpi, y, PW - m, y + 64 * dpi}, A_BLUETOOTH, bt == 1, L"Bluetooth",
               bt < 0 ? testi::t(L"Non disponibile", L"Unavailable") : bt == 1 ? testi::t(L"Acceso", L"On") : testi::t(L"Spento", L"Off"), scuro, ink);
    y += 64 * dpi + 10 * dpi;
    const bool tScuro = sistema::Sistema::temaScuro();
    piastrella({m, y, m + mezzo, y + 64 * dpi}, A_TEMA, tScuro, testi::t(L"Tema scuro", L"Dark theme"), tScuro ? testi::t(L"Attivo", L"On") : testi::t(L"Spento", L"Off"), scuro, ink);
    piastrella({m + mezzo + 10 * dpi, y, PW - m, y + 64 * dpi}, A_FLUIDWIN, false, L"FluidWin", testi::t(L"Impostazioni", L"Settings"), scuro, ink);
    // tieni sveglio e non disturbare (comodita.h): il clic apre il menu delle durate
    y += 64 * dpi + 10 * dpi;
    piastrella({m, y, m + mezzo, y + 64 * dpi}, A_SVEGLIO, comodita::sveglio(), testi::t(L"Non dormire", L"Keep awake"),
               comodita::descrivi(comodita::stato().svegliaFino), scuro, ink);
    piastrella({m + mezzo + 10 * dpi, y, PW - m, y + 64 * dpi}, A_SILENZIO, comodita::silenzio(), testi::t(L"Silenzioso", L"Silent"),
               comodita::descrivi(comodita::stato().silenzioFino), scuro, ink);
    // timer e microfono (sensori.h): il clic apre un menu
    y += 64 * dpi + 10 * dpi;
    {
        std::wstring st = testi::t(L"Spento", L"Off");
        if (comodita::timerSuona()) st = testi::t(L"Finito", L"Done");
        else if (comodita::timerAttivo()) st = comodita::durata(comodita::timerResto()) + testi::t(L" rimasti", L" left");
        piastrella({m, y, m + mezzo, y + 64 * dpi}, A_TIMER, comodita::timerAttivo() || comodita::timerSuona(), L"Timer", st, scuro, ink);
        const sensori::Sensori& se = sensori::Sensori::io();
        std::wstring sm = !se.micC ? std::wstring(testi::t(L"Nessuno", L"None")) : se.micMuto ? std::wstring(testi::t(L"Muto", L"Muted"))
                        : se.micApp.empty() ? std::wstring(testi::t(L"Non in uso", L"Not in use")) : se.micApp[0];
        piastrella({m + mezzo + 10 * dpi, y, PW - m, y + 64 * dpi}, A_MICROFONO, !se.micApp.empty() || !se.camApp.empty(),
                   testi::t(L"Microfono", L"Microphone"), sm, scuro, ink);
    }

    // volume: capsula riempita di bianco come su iOS; si trascina, la rotella regola, l'altoparlante disattiva
    y += 64 * dpi + 14 * dpi;
    const D2D1_RECT_F rv = {m, y, PW - m, y + 44 * dpi};
    const float rr = 22 * dpi, v = (audioOk && !muto) ? volume : 0.0f;
    aggiungi(rv, A_VOLUME, 0);
    pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.08f));
    dc->FillRoundedRectangle({rv, rr, rr}, pennello);
    dc->PushAxisAlignedClip({rv.left, rv.top, rv.left + larg * v, rv.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    pennello->SetColor(D2D1::ColorF(1, 1, 1, scuro ? 0.92f : 0.97f));
    dc->FillRoundedRectangle({rv, rr, rr}, pennello);
    dc->PopAxisAlignedClip();
    const D2D1_RECT_F rm = {rv.left, rv.top, rv.left + 48 * dpi, rv.bottom};
    const int em = aggiungi(rm, A_MUTO, 0);
    const float sm = (em == pPremuto) ? 0.88f : 1.0f;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(sm, sm, {rv.left + 26 * dpi, (rv.top + rv.bottom) / 2}) * baseVista);
    pennello->SetColor(scuro && v < 0.12f ? ink : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.85f));
    simboloVolume(rv.left + 26 * dpi, (rv.top + rv.bottom) / 2);
    dc->SetTransform(baseVista);
    wchar_t pc[16]; swprintf(pc, 16, muto ? testi::t(L"muto", L"muted") : L"%d%%", (int)std::lround(volume * 100));
    pennello->SetColor(scuro && v < 0.9f ? ink : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.7f));
    scrivi(pc, fmtNome, {rv.right - 56 * dpi, rv.top, rv.right - 8 * dpi, rv.bottom});

    // luminosita' del monitor (solo se si puo' regolare via DDC/CI)
    const int lum = sis.luminosita;
    if (lum >= 0) {
        y += 44 * dpi + 8 * dpi;
        const D2D1_RECT_F rl = {m, y, PW - m, y + 44 * dpi};
        const float vl = lum / 100.0f;
        aggiungi(rl, A_LUMINOSITA, 0);
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.08f));
        dc->FillRoundedRectangle({rl, rr, rr}, pennello);
        dc->PushAxisAlignedClip({rl.left, rl.top, rl.left + larg * vl, rl.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        pennello->SetColor(D2D1::ColorF(1, 1, 1, scuro ? 0.92f : 0.97f));
        dc->FillRoundedRectangle({rl, rr, rr}, pennello);
        dc->PopAxisAlignedClip();
        pennello->SetColor(scuro && vl < 0.12f ? ink : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.85f));
        simboloSole(rl.left + 26 * dpi, (rl.top + rl.bottom) / 2);
        wchar_t pl[16]; swprintf(pl, 16, L"%d%%", lum);
        pennello->SetColor(scuro && vl < 0.9f ? ink : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.7f));
        scrivi(pl, fmtNome, {rl.right - 56 * dpi, rl.top, rl.right - 8 * dpi, rl.bottom});
    }

    // uscita audio
    y += 44 * dpi + 8 * dpi;
    const D2D1_RECT_F ru = {m - 8 * dpi, y, PW - m + 8 * dpi, y + 36 * dpi};
    evidenzia(aggiungi(ru, A_AUDIO, 0), ru, scuro, 6 * dpi);
    pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.62f});
    scrivi(testi::t(L"Uscita", L"Output"), fmtSotto, {m, y, m + 60 * dpi, y + 36 * dpi});
    pennello->SetColor(ink);
    scrivi(nomeUscita.empty() ? std::wstring(testi::t(L"Nessun dispositivo", L"No device")) : nomeUscita, fmtVoce, {m + 52 * dpi, y, PW - m - 20 * dpi, y + 36 * dpi});
    chevronDestra(PW - m - 4 * dpi, y + 18 * dpi);

    // scorciatoie tonde
    y += 36 * dpi + 16 * dpi;
    const wchar_t* nomi[4] = {L"Widget", L"In background", testi::t(L"Notifiche", L"Notifications"), testi::t(L"Impostazioni", L"Settings")};
    const Azione az[4] = {A_WIDGET, A_SFONDO, A_NOTIFICHE, A_IMPOSTAZIONI};
    for (int i = 0; i < 4; i++) {
        const float cx = m + larg * (i + 0.5f) / 4, cy = y + 26 * dpi;
        const D2D1_RECT_F r = {cx - 36 * dpi, y - 4 * dpi, cx + 36 * dpi, y + 76 * dpi};
        const int e = aggiungi(r, az[i], 0);
        const double h = evidenzia(e, {0, 0, 0, 0}, scuro, 0);   // solo il valore: l'evidenziazione e' il cerchio
        const float s = (e == pPremuto) ? 0.9f : 1.0f;
        pennello->SetColor(scuro ? D2D1::ColorF(1, 1, 1, 0.12f + 0.06f * (float)h) : D2D1::ColorF(0, 0, 0, 0.06f + 0.05f * (float)h));
        dc->FillEllipse({{cx, cy}, 26 * dpi * s, 26 * dpi * s}, pennello);
        pennello->SetColor(ink);
        if (i == 0) simboloWidget(cx, cy); else if (i == 1) simboloFreccia(cx, cy); else if (i == 2) simboloCampana(cx, cy); else simboloIngranaggio(cx, cy);
        pennello->SetColor({ink.r, ink.g, ink.b, ink.a * 0.75f});
        scrivi(nomi[i], fmtNome, {cx - 40 * dpi, cy + 30 * dpi, cx + 40 * dpi, cy + 48 * dpi});
    }
}

// ---- widget: meteo e notizie ----

inline auto Motore::disegnaWidget(bool scuro, D2D1_COLOR_F ink) -> void {
    const float m = 16 * dpi, rp = std::max(6.0f, raggio::vetro() - 10.0f) * dpi;
    const D2D1_COLOR_F tenue = {ink.r, ink.g, ink.b, ink.a * 0.62f};
    // meteo: una scheda azzurra come l'app Meteo di iOS
    const D2D1_RECT_F rm = {m, m, PW - m, m + 168 * dpi};
    ID2D1GradientStopCollection* stop = nullptr;
    const D2D1_GRADIENT_STOP gs[2] = {{0, D2D1::ColorF(0.24f, 0.52f, 0.92f, 1)}, {1, D2D1::ColorF(0.42f, 0.70f, 0.98f, 1)}};
    dc->CreateGradientStopCollection(gs, 2, &stop);
    ID2D1LinearGradientBrush* sf = nullptr;
    if (stop) dc->CreateLinearGradientBrush({{rm.left, rm.top}, {rm.left, rm.bottom}}, stop, &sf);
    if (sf) { dc->FillRoundedRectangle({rm, rp, rp}, sf); sf->Release(); }
    if (stop) stop->Release();
    const D2D1_COLOR_F bianco = D2D1::ColorF(1, 1, 1, 1), biancoT = D2D1::ColorF(1, 1, 1, 0.8f);
    pennello->SetColor(bianco);
    scrivi(widD.citta, fmtVoce, {rm.left + 16 * dpi, rm.top + 12 * dpi, rm.right - 16 * dpi, rm.top + 32 * dpi});
    if (wid.meteoOk) {
        wchar_t t[16]; swprintf(t, 16, L"%d°", (int)std::lround(wid.temp));
        scrivi(t, fmtGrande, {rm.left + 16 * dpi, rm.top + 30 * dpi, rm.left + 140 * dpi, rm.top + 74 * dpi});
        pennello->SetColor(bianco);
        simboloMeteo(wid.codice, rm.right - 40 * dpi, rm.top + 44 * dpi, 1.4f);
        pennello->SetColor(biancoT);
        wchar_t d[96]; swprintf(d, 96, testi::t(L"%ls · umidità %d%% · %d km/h", L"%ls · humidity %d%% · %d km/h"), widget::descrizione(wid.codice), (int)std::lround(wid.umidita), (int)std::lround(wid.vento));
        scrivi(d, fmtSotto, {rm.left + 16 * dpi, rm.top + 76 * dpi, rm.right - 16 * dpi, rm.top + 94 * dpi});
        // i prossimi giorni
        const size_t n = std::min<size_t>(5, wid.giorni.size());
        for (size_t i = 0; i < n; i++) {
            const float cw = (rm.right - rm.left - 16 * dpi) / 5, cx = rm.left + 8 * dpi + cw * (i + 0.5f), y = rm.top + 104 * dpi;
            const auto& g = wid.giorni[i];
            pennello->SetColor(biancoT);
            scrivi(widget::nomeGiorno(g), fmtNome, {cx - cw / 2, y, cx + cw / 2, y + 14 * dpi});
            pennello->SetColor(bianco);
            simboloMeteo(g.codice, cx, y + 27 * dpi, 0.8f);
            wchar_t mm[32]; swprintf(mm, 32, L"%d°  %d°", (int)std::lround(g.max), (int)std::lround(g.min));
            scrivi(mm, fmtNome, {cx - cw / 2, y + 40 * dpi, cx + cw / 2, y + 56 * dpi});
        }
    } else {
        pennello->SetColor(biancoT);
        scrivi(testi::t(L"Meteo in arrivo…", L"Loading weather…"), fmtSotto, {rm.left + 16 * dpi, rm.top + 40 * dpi, rm.right - 16 * dpi, rm.top + 60 * dpi});
    }
    // notizie
    float y = rm.bottom + 16 * dpi;
    pennello->SetColor(ink);
    scrivi(testi::t(L"Notizie", L"News"), fmtTitolo, {m, y, PW - m, y + 22 * dpi});
    pennello->SetColor(tenue);
    scrivi(L"ANSA", fmtSotto, {PW - m - 36 * dpi, y + 2 * dpi, PW - m, y + 20 * dpi});
    y += 28 * dpi;
    if (wid.notizie.empty()) { pennello->SetColor(tenue); scrivi(testi::t(L"Notizie in arrivo…", L"Loading news…"), fmtSotto, {m, y, PW - m, y + 20 * dpi}); }
    for (size_t i = 0; i < wid.notizie.size() && y + 56 * dpi < PH - m; i++) {
        const auto& nz = wid.notizie[i];
        const D2D1_RECT_F r = {m - 8 * dpi, y, PW - m + 8 * dpi, y + 56 * dpi};
        evidenzia(aggiungi(r, A_NOTIZIA, (int)i), r, scuro, std::min(rp, 12 * dpi));
        pennello->SetColor(ink);
        // due righe al massimo: quello che non entra finisce con i puntini
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(fdw->CreateTextLayout(nz.titolo.c_str(), (UINT32)nz.titolo.size(), fmtNotizia, PW - 2 * m - 44 * dpi, 34 * dpi, &lay))) {
            dc->DrawTextLayout({m, y + 6 * dpi}, lay, pennello, D2D1_DRAW_TEXT_OPTIONS_CLIP); lay->Release();
        }
        pennello->SetColor(tenue);
        scrivi(nz.quando, fmtSotto, {PW - m - 36 * dpi, y + 6 * dpi, PW - m, y + 22 * dpi});
        y += 58 * dpi;
    }
}

// simboli del meteo (scala 1 = circa 24 px)
inline auto Motore::simboloMeteo(int c, float cx, float cy, float s) -> void {
    const float d = dpi * s;
    auto sole = [&](float x, float y, float r) {
        dc->FillEllipse({{x, y}, r * d, r * d}, pennello);
        for (int i = 0; i < 8; i++) {
            const float a = i * (float)M_PI / 4;
            dc->DrawLine({x + std::cos(a) * (r + 2.5f) * d, y + std::sin(a) * (r + 2.5f) * d}, {x + std::cos(a) * (r + 5) * d, y + std::sin(a) * (r + 5) * d}, pennello, 1.6f * d, tratto);
        }
    };
    auto nuvola = [&](float x, float y) {
        dc->FillEllipse({{x - 5 * d, y + 2 * d}, 6 * d, 5 * d}, pennello);
        dc->FillEllipse({{x + 1 * d, y - 2 * d}, 7 * d, 7 * d}, pennello);
        dc->FillEllipse({{x + 7 * d, y + 2 * d}, 5.5f * d, 5 * d}, pennello);
        dc->FillRoundedRectangle({{x - 11 * d, y + 1 * d, x + 12.5f * d, y + 7 * d}, 3 * d, 3 * d}, pennello);
    };
    if (c == 0) { sole(cx, cy, 6); return; }
    if (c <= 2) { sole(cx - 4 * d, cy - 4 * d, 4.5f); nuvola(cx + 2 * d, cy + 3 * d); return; }   // sole e nuvola
    nuvola(cx, cy - 2 * d);
    if (c == 45 || c == 48) { for (int i = 0; i < 2; i++) dc->DrawLine({cx - 9 * d, cy + (9 + i * 4) * d}, {cx + 9 * d, cy + (9 + i * 4) * d}, pennello, 1.6f * d, tratto); return; }
    if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) { for (int i = -1; i <= 1; i++) dc->DrawLine({cx + i * 5 * d, cy + 8 * d}, {cx + i * 5 * d - 2 * d, cy + 13 * d}, pennello, 1.6f * d, tratto); return; }
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) { for (int i = -1; i <= 1; i++) dc->FillEllipse({{cx + i * 5 * d, cy + 11 * d}, 1.5f * d, 1.5f * d}, pennello); return; }
    if (c >= 95) {   // fulmine
        dc->DrawLine({cx + 1 * d, cy + 6 * d}, {cx - 2 * d, cy + 11 * d}, pennello, 1.8f * d, tratto);
        dc->DrawLine({cx - 2 * d, cy + 11 * d}, {cx + 2 * d, cy + 11 * d}, pennello, 1.8f * d, tratto);
        dc->DrawLine({cx + 2 * d, cy + 11 * d}, {cx - 1 * d, cy + 16 * d}, pennello, 1.8f * d, tratto);
    }
}

inline auto Motore::simboloBluetooth(float cx, float cy) -> void {
    linea(-4, -4.5f, 4, 3.5f, cx, cy, 1.6f); linea(4, 3.5f, 0, 7.5f, cx, cy, 1.6f); linea(0, 7.5f, 0, -7.5f, cx, cy, 1.6f);
    linea(0, -7.5f, 4, -3.5f, cx, cy, 1.6f); linea(4, -3.5f, -4, 4.5f, cx, cy, 1.6f);
}

// Sole che diventa luna (e viceversa) quando cambia il tema: i raggi rientrano, un'ombra rotonda scivola sul
// disco fino a lasciare una falce, e il tutto ruota un poco
inline auto Motore::simboloTema(float cx, float cy) -> void {
    const double obj = sistema::Sistema::temaScuro() ? 1.0 : 0.0;
    if (temaK < 0) temaK = obj;
    const double passo = dtFotogramma / 0.5;   // mezzo secondo da un capo all'altro, a velocita' costante (la curva la fa k)
    temaK = std::abs(obj - temaK) <= passo ? obj : temaK + (obj > temaK ? passo : -passo);
    if (temaK != obj) pAnimazione = true;
    const float k = (float)(temaK * temaK * (3 - 2 * temaK));   // morbida all'inizio e alla fine
    dc->SetTransform(D2D1::Matrix3x2F::Rotation(-90.0f * (1 - k), {cx, cy}) * baseVista);
    // raggi del sole: si accorciano fino a sparire
    if (k < 0.98f) {
        const float r0 = (6.6f + 1.5f * k) * dpi, r1 = (9.2f - 2.6f * k) * dpi;
        for (int i = 0; i < 8; i++) {
            const float a = i * (float)M_PI / 4, c = std::cos(a), s = std::sin(a);
            if (r1 > r0 + 0.3f * dpi) dc->DrawLine({cx + c * r0, cy + s * r0}, {cx + c * r1, cy + s * r1}, pennello, 1.8f * dpi, tratto);
        }
    }
    // disco: piccolo da sole, piu' grande da luna; l'ombra arriva da in alto a destra
    const float r = (4.2f + 3.3f * k) * dpi;
    const float ox = cx + (r * 2.2f) - (r * 2.2f - 4.5f * dpi) * k, oy = cy - (r * 1.6f) + (r * 1.6f - 3.5f * dpi) * k;
    ID2D1EllipseGeometry *a = nullptr, *b = nullptr; ID2D1PathGeometry* g = nullptr;
    f2d->CreateEllipseGeometry({{cx, cy}, r, r}, &a);
    f2d->CreateEllipseGeometry({{ox, oy}, r * 0.86f, r * 0.86f}, &b);
    if (a && b && SUCCEEDED(f2d->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr; g->Open(&s);
        a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, s);
        s->Close(); s->Release();
        dc->FillGeometry(g, pennello);
    }
    for (IUnknown* o : std::initializer_list<IUnknown*>{a, b, g}) if (o) o->Release();
    dc->SetTransform(baseVista);
}

// timer: quadrante con una lancetta e il pulsante in cima
inline auto Motore::simboloTimer(float cx, float cy) -> void {
    const float d = dpi;
    dc->DrawEllipse({{cx, cy + 1 * d}, 7.5f * d, 7.5f * d}, pennello, 1.6f * d);
    linea(0, 1, 0, -3.5f, cx, cy, 1.6f);
    linea(0, 1, 3, 3, cx, cy, 1.6f);
    linea(-2.5f, -9, 2.5f, -9, cx, cy, 1.6f);
}

// microfono: capsula, archetto e asta (rosso barrato se e' muto)
inline auto Motore::simboloMicrofono(float cx, float cy, bool) -> void {
    const float d = dpi;
    dc->FillRoundedRectangle({{cx - 3.5f * d, cy - 9 * d, cx + 3.5f * d, cy + 2 * d}, 3.5f * d, 3.5f * d}, pennello);
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f2d->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr; g->Open(&s);
        s->BeginFigure({cx - 6.5f * d, cy - 1 * d}, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc(D2D1::ArcSegment({cx + 6.5f * d, cy - 1 * d}, {6.5f * d, 6.5f * d}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
        dc->DrawGeometry(g, pennello, 1.5f * d, tratto); g->Release();
    }
    linea(0, 5.5f, 0, 9, cx, cy, 1.5f);
    if (sensori::Sensori::io().micMuto) linea(-7, -8, 7, 8, cx, cy, 1.6f);
}

// tazzina di caffe' (tieni sveglio): tazza, manico e un filo di vapore
inline auto Motore::simboloCaffe(float cx, float cy) -> void {
    const float d = dpi;
    dc->FillRoundedRectangle({{cx - 6.5f * d, cy - 1.5f * d, cx + 4.5f * d, cy + 7.5f * d}, 2.5f * d, 2.5f * d}, pennello);
    dc->DrawEllipse({{cx + 5.5f * d, cy + 2.5f * d}, 2.6f * d, 2.6f * d}, pennello, 1.5f * d);
    linea(-3.5f, -4.5f, -3.5f, -8.5f, cx, cy, 1.4f);
    linea(0.5f, -4.0f, 0.5f, -9.0f, cx, cy, 1.4f);
}

inline auto Motore::simboloLuna(float cx, float cy) -> void {
    // falce: un cerchio meno un cerchio spostato in alto a destra
    const float r = 7.5f * dpi;
    ID2D1EllipseGeometry *a = nullptr, *b = nullptr; ID2D1PathGeometry* g = nullptr;
    f2d->CreateEllipseGeometry({{cx, cy}, r, r}, &a);
    f2d->CreateEllipseGeometry({{cx + 4.5f * dpi, cy - 3.5f * dpi}, r * 0.85f, r * 0.85f}, &b);
    if (a && b && SUCCEEDED(f2d->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr; g->Open(&s);
        a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, s);
        s->Close(); s->Release();
        dc->FillGeometry(g, pennello);
    }
    for (IUnknown* o : std::initializer_list<IUnknown*>{a, b, g}) if (o) o->Release();
}

// goccia: il simbolo di FluidWin
inline auto Motore::simboloGoccia(float cx, float cy) -> void {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(f2d->CreatePathGeometry(&g))) return;
    ID2D1GeometrySink* s = nullptr; g->Open(&s);
    const float r = 6 * dpi;
    s->BeginFigure({cx, cy - 10 * dpi}, D2D1_FIGURE_BEGIN_FILLED);
    s->AddBezier(D2D1::BezierSegment({cx + 2 * dpi, cy - 6 * dpi}, {cx + r, cy - 2 * dpi}, {cx + r, cy + 2.5f * dpi}));
    s->AddArc(D2D1::ArcSegment({cx - r, cy + 2.5f * dpi}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    s->AddBezier(D2D1::BezierSegment({cx - r, cy - 2 * dpi}, {cx - 2 * dpi, cy - 6 * dpi}, {cx, cy - 10 * dpi}));
    s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
    dc->FillGeometry(g, pennello); g->Release();
}

inline auto Motore::simboloSole(float cx, float cy) -> void {
    dc->DrawEllipse({{cx, cy}, 3.8f * dpi, 3.8f * dpi}, pennello, 1.8f * dpi);
    for (int i = 0; i < 8; i++) {
        const float a = i * (float)M_PI / 4, c = std::cos(a), s = std::sin(a);
        linea(c * 6.8f, s * 6.8f, c * 8.8f, s * 8.8f, cx, cy, 1.8f);
    }
}

inline auto Motore::simboloEnergia(float cx, float cy) -> void {
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f2d->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr; g->Open(&s);
        const float r = 7 * dpi, a0 = -(float)M_PI / 2 + 0.65f, a1 = -(float)M_PI / 2 - 0.65f;
        s->BeginFigure({cx + r * std::cos(a0), cy + r * std::sin(a0)}, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc(D2D1::ArcSegment({cx + r * std::cos(a1), cy + r * std::sin(a1)}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
        dc->DrawGeometry(g, pennello, 1.9f * dpi, tratto);
        g->Release();
    }
    linea(0, -9, 0, -2, cx, cy, 1.9f);
}

inline auto Motore::simboloIngranaggio(float cx, float cy) -> void {
    dc->DrawEllipse({{cx, cy}, 5.6f * dpi, 5.6f * dpi}, pennello, 2.2f * dpi);
    dc->DrawEllipse({{cx, cy}, 2.0f * dpi, 2.0f * dpi}, pennello, 1.4f * dpi);
    for (int i = 0; i < 8; i++) {
        const float a = i * (float)M_PI / 4 + (float)M_PI / 8, c = std::cos(a), s = std::sin(a);
        linea(c * 6.6f, s * 6.6f, c * 8.6f, s * 8.6f, cx, cy, 2.6f);
    }
}

// campanella delle notifiche: cupola, bordo in basso e batacchio
inline auto Motore::simboloCampana(float cx, float cy) -> void {
    ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
    if (SUCCEEDED(f2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
        const float d = dpi;
        s->BeginFigure({cx - 7.5f * d, cy + 4.5f * d}, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLine({cx - 6 * d, cy + 2.5f * d});
        s->AddLine({cx - 6 * d, cy - 1.5f * d});
        s->AddArc({{cx + 6 * d, cy - 1.5f * d}, {6 * d, 6.5f * d}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        s->AddLine({cx + 6 * d, cy + 2.5f * d});
        s->AddLine({cx + 7.5f * d, cy + 4.5f * d});
        s->AddLine({cx - 7.5f * d, cy + 4.5f * d});
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close(); s->Release();
        dc->DrawGeometry(g, pennello, 1.9f * d, tratto);
    }
    if (g) g->Release();
    linea(-2.2f, 7.2f, 2.2f, 7.2f, cx, cy, 2.0f);
    linea(0, -8.6f, 0, -7.6f, cx, cy, 2.0f);
}

// ---- dati dei pannelli ----

// "{GUID cartella nota}\resto" -> percorso vero (cosi' Windows salva i percorsi delle icone di notifica)
inline auto Motore::risolviCartella(const std::wstring& p) -> std::wstring {
    if (p.size() < 39 || p[0] != L'{') return p;
    const size_t fine = p.find(L'}');
    if (fine == std::wstring::npos) return p;
    GUID g; if (FAILED(CLSIDFromString(p.substr(0, fine + 1).c_str(), &g))) return p;
    PWSTR base = nullptr; std::wstring r = p;
    if (SUCCEEDED(SHGetKnownFolderPath(g, 0, nullptr, &base))) r = std::wstring(base) + p.substr(fine + 1);
    CoTaskMemFree(base);
    return r;
}

inline auto Motore::bitmapDaPng(const BYTE* d, DWORD n) -> ID2D1Bitmap* {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    IWICStream* s = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    ID2D1Bitmap1* out = nullptr;
    if (SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromMemory((BYTE*)d, n)) &&
        SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        dc->CreateBitmapFromWicBitmap(cv, (const D2D1_BITMAP_PROPERTIES1*)nullptr, &out);
    for (IUnknown* u : std::initializer_list<IUnknown*>{cv, fr, dec, s, f}) if (u) u->Release();
    return out;
}

inline auto Motore::iconaSfondo(const std::wstring& exe, HKEY, const wchar_t* sotto) -> ID2D1Bitmap* {
    chiaviSfondo[exe] = sotto;
    auto it = cacheIcone.find(exe);
    if (it != cacheIcone.end()) return it->second;
    chiediIcona(exe, 128);
    return nullptr;
}

inline auto Motore::fotoIconaSfondo(const std::wstring& exe) -> ID2D1Bitmap* {
    auto k = chiaviSfondo.find(exe);
    if (k == chiaviSfondo.end()) return nullptr;
    HKEY chiave = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &chiave) != ERROR_SUCCESS) return nullptr;
    ID2D1Bitmap* b = nullptr;
    DWORD n = 0;
    if (RegGetValueW(chiave, k->second.c_str(), L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &n) == ERROR_SUCCESS && n && n < 4 * 1024 * 1024) {
        std::vector<BYTE> png(n);
        if (RegGetValueW(chiave, k->second.c_str(), L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, png.data(), &n) == ERROR_SUCCESS) b = bitmapDaPng(png.data(), n);
    }
    RegCloseKey(chiave);
    return b;
}

// app dello Store: dal percorso ...\WindowsApps\Nome_versione_arch__editore\... all'AppUserModelID
// (solo dal lavoro in sottofondo delle app in background: uno alla volta, quindi la cache non e' contesa)
inline auto Motore::avvioPacchetto(const std::wstring& exe) -> std::wstring {
    static std::unordered_map<std::wstring, std::wstring> aumidPacchetti;   // famiglia del pacchetto -> AppUserModelID
    const size_t p = exe.find(L"\\windowsapps\\");
    if (p == std::wstring::npos) return L"";
    const size_t a = p + 13, b = exe.find(L'\\', a);
    const std::wstring pac = exe.substr(a, b - a);
    const size_t u = pac.find(L'_'), e = pac.rfind(L"__");
    if (u == std::wstring::npos || e == std::wstring::npos) return L"";
    const std::wstring famiglia = pac.substr(0, u) + L"_" + pac.substr(e + 2);
    if (aumidPacchetti.empty()) {   // elenco delle app installate (una volta sola)
        IShellItem* cart = nullptr;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&cart)))) {
            IEnumShellItems* en = nullptr;
            if (SUCCEEDED(cart->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
                IShellItem* it = nullptr;
                while (en->Next(1, &it, nullptr) == S_OK) {
                    LPWSTR n = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &n))) {
                        const std::wstring id = n; const size_t k = id.find(L'!');
                        if (k != std::wstring::npos) aumidPacchetti.emplace(minuscolo(id.substr(0, k)), id);
                        CoTaskMemFree(n);
                    }
                    it->Release();
                }
                en->Release();
            }
            cart->Release();
        }
    }
    auto it = aumidPacchetti.find(famiglia);
    return it == aumidPacchetti.end() ? L"" : L"shell:AppsFolder\\" + it->second;
}

// il tooltip e' il nome che l'app mostra di se': si tolgono stato ("Steam - sincronizzazione") e versioni ("B26.0730.1")
inline auto Motore::nomeDaTooltip(std::wstring t) -> std::wstring {
    for (const wchar_t* sep : {L" - ", L"\n", L"\r"}) { const size_t p = t.find(sep); if (p != std::wstring::npos) t = t.substr(0, p); }
    // ultime parole con cifre (versioni) e spazi in fondo
    for (;;) {
        while (!t.empty() && t.back() == L' ') t.pop_back();
        const size_t sp = t.rfind(L' ');
        if (sp == std::wstring::npos || t.find_first_of(L"0123456789", sp) == std::wstring::npos) break;
        t = t.substr(0, sp);
    }
    return t;
}

inline auto Motore::leggiAppSfondo() -> void {
    if (lavoroSfondo) return;
    lavoroSfondo = true;
    auto ris = std::make_shared<std::vector<AppSfondo>>();
    inSottofondo([ris] { *ris = elencaAppSfondo(); }, [this, ris] {
        lavoroSfondo = false;
        for (auto& a : *ris) a.icona = iconaSfondo(a.exe, nullptr, a.chiaveReg.c_str());
        appSfondo = std::move(*ris);
        // pannello aperto: si allunga o si accorcia con le righe di icone (il bordo in basso resta fermo)
        const int s = slotCorrente;
        usa(0);
        if (pannelloTipo == P_SFONDO && !pChiusura) {
            const float nuova = altezzaVista(P_SFONDO);
            if (nuova != PH) { PH = nuova; pAnimazione = true; }   // con la molla, dal bordo in basso
            pSopra = pPremuto = -1; evidP.clear(); pDirty = true;
        }
        usa(s);
    });
}

inline auto Motore::altezzaSfondo() const -> float {
    // righe di app: quante ce ne sono (massimo 6) e quante ne entrano nello spazio sopra l'isola
    const int entrano = std::max(1, (int)((altezzaMassimaP() / dpi - TESTA - 10 - 52) / 86));
    const int righe = std::max(1, std::min({(int)std::min<size_t>(6, (appSfondo.size() + 3) / 4), entrano}));
    return std::round((10 + righe * 86 + 52) * dpi);
}

inline auto Motore::elencaAppSfondo() -> std::vector<AppSfondo> {
    std::unordered_set<std::wstring> attivi;
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (sn != INVALID_HANDLE_VALUE && Process32FirstW(sn, &pe)) do {
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t b[MAX_PATH]; DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(p, 0, b, &n)) attivi.insert(minuscolo(b));
            CloseHandle(p);
        }
    } while (Process32NextW(sn, &pe));
    if (sn != INVALID_HANDLE_VALUE) CloseHandle(sn);
    wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH); const std::wstring proprio = minuscolo(buf);
    GetWindowsDirectoryW(buf, MAX_PATH); const std::wstring dirWin = minuscolo(buf) + L"\\";

    std::vector<AppSfondo> nuove;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t sub[256];
        for (DWORD i = 0;; i++) {
            DWORD n = 256;
            if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t percorso[1024]; DWORD sz = sizeof(percorso);
            if (RegGetValueW(k, sub, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, percorso, &sz) != ERROR_SUCCESS) continue;
            const std::wstring exe = minuscolo(risolviCartella(percorso));
            if (!attivi.count(exe) || exe == dirWin + L"explorer.exe") continue;   // explorer: volume, rete... gia' nell'isola
            if (std::any_of(nuove.begin(), nuove.end(), [&](const AppSfondo& a) { return a.exe == exe; })) continue;
            AppSfondo a; a.exe = exe;
            wchar_t tip[256] = L""; DWORD ts = sizeof(tip);
            RegGetValueW(k, sub, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, tip, &ts);
            a.nome = exe == proprio ? L"FluidWin" : exe.find(L"securityhealthsystray") != std::wstring::npos ? testi::t(L"Sicurezza di Windows", L"Windows Security") : nomeDaTooltip(tip);
            if (a.nome.empty()) a.nome = nomeExe(exe);
            a.chiaveReg = sub;   // l'icona la chiede poi il thread principale
            // come si apre: rilanciarle e' il modo ufficiale (le app da area di notifica mostrano la loro finestra)
            if (exe == proprio) a.avvio = L"*fluidwin";
            else if (exe.find(L"securityhealthsystray") != std::wstring::npos) a.avvio = L"windowsdefender:";
            else if (exe.rfind(dirWin, 0) == 0) a.avvio = L"";                    // componenti di Windows: pannello nativo
            else if (exe.find(L"\\windowsapps\\") != std::wstring::npos) a.avvio = avvioPacchetto(exe);
            else a.avvio = exe;
            nuove.push_back(std::move(a));
        }
        RegCloseKey(k);
    }
    std::sort(nuove.begin(), nuove.end(), [](const AppSfondo& a, const AppSfondo& b) { return minuscolo(a.nome) < minuscolo(b.nome); });
    return nuove;
}

inline auto Motore::leggiUscita() -> void {
    static const PROPERTYKEY NOME_DISPOSITIVO = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    nomeUscita.clear();
    IMMDevice* d = nullptr;
    if (!audio || FAILED(audio->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(NOME_DISPOSITIVO, &v)) && v.vt == VT_LPWSTR && v.pwszVal) nomeUscita = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    d->Release();
}

inline auto Motore::impostaVolume(float l, bool togliMuto) -> void {
    IAudioEndpointVolume* v = volumeDispositivo();
    if (!v) return;
    v->SetMasterVolumeLevelScalar(std::clamp(l, 0.0f, 1.0f), nullptr);
    if (togliMuto) v->SetMute(FALSE, nullptr);
    v->Release();
    leggiVolume(); dirty = true; pDirty = true;
}

inline auto Motore::volumeDaMouse(float x) -> void {
    for (auto& e : elementi) if (e.azione == A_VOLUME) {
        impostaVolume((x - e.r.left) / (e.r.right - e.r.left), true);
        return;
    }
}

inline auto Motore::luminositaDaMouse(float x) -> void {
    for (auto& e : elementi) if (e.azione == A_LUMINOSITA) {
        sis.impostaLuminosita((int)std::lround(std::clamp((x - e.r.left) / (e.r.right - e.r.left), 0.0f, 1.0f) * 100));
        pDirty = true;
        return;
    }
}

// tema cambiato (dal centro di controllo o dalle Impostazioni): i vetri seguono il chiaro/scuro
inline auto Motore::aggiornaTemaVetri() -> void {
    BOOL s = temaScuro(true);
    for (HWND v : {vetro, vetroStato, vetroOra, vetroPannello, altra.vetroPannello, vetroLauncher}) if (v) DwmSetWindowAttribute(v, DWMWA_USE_IMMERSIVE_DARK_MODE, &s, sizeof(s));
    dirty = true; pDirty = true; altra.pDirty = true; lDirty = true;
}

inline auto Motore::apriPagina(const wchar_t* uri) -> void { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

// il pannello delle icone nascoste di Windows (fa risalire la barra): resta per i menu delle singole app
inline auto Motore::pannelloWindows() -> void { tastiWin('B'); SetTimer(icone, TIMER_INVIO, 220, nullptr); }

inline auto Motore::apriAppSfondo(const AppSfondo& a) -> void {
    if (a.avvio == L"*fluidwin") {   // FluidWin stesso: il suo menu (pausa, esci)
        nascondiPannello();   // il menu blocca il disegno finche' resta aperto: niente dissolvenza a meta'
        PostMessageW(FindWindowW(L"FluidWin.Messaggi", nullptr), WM_APP + 1, 1, WM_LBUTTONUP);
    } else if (a.avvio.empty()) {
        pannelloWindows();
    } else {
        const std::wstring dir = a.avvio.find(L'\\') != std::wstring::npos && a.avvio.rfind(L"shell:", 0) != 0
                                 ? a.avvio.substr(0, a.avvio.find_last_of(L'\\')) : L"";
        ShellExecuteW(nullptr, L"open", a.avvio.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
    }
}

inline auto Motore::eseguiPannello(const Elemento& e) -> void {
    switch (e.azione) {
        case A_APP:          chiudiPannello(); if (e.indice < (int)appSfondo.size()) apriAppSfondo(appSfondo[e.indice]); break;
        case A_TUTTE:        chiudiPannello(); pannelloWindows(); break;
        // Wi-Fi e Bluetooth: interruttori (se la scheda c'e'); il tasto destro apre comunque le impostazioni
        case A_RETE:
            if (sis.wifi >= 0) { sis.impostaRadio(1, sis.wifi != 1); pDirty = true; }
            else { chiudiPannello(); apriPagina(L"ms-settings:network-status"); }
            break;
        case A_BLUETOOTH:
            if (sis.bluetooth >= 0) { sis.impostaRadio(3, sis.bluetooth != 1); pDirty = true; }
            else { chiudiPannello(); apriPagina(L"ms-settings:bluetooth"); }
            break;
        case A_TEMA:
            sistema::Sistema::impostaTema(!sistema::Sistema::temaScuro());
            aggiornaTemaVetri(); dirty = true; pDirty = true;
            break;
        case A_FLUIDWIN:     chiudiPannello(); if (impostazioni) impostazioni(); break;
        case A_TIMER: {
            std::vector<menu::Voce> voci = {menu::titolo(L"Timer")};
            const int minuti[] = {1, 3, 5, 10, 15, 25, 30, 45, 60};
            for (int i = 0; i < 9; i++) {
                wchar_t b[32]; swprintf(b, 32, minuti[i] == 60 ? testi::t(L"1 ora", L"1 hour") : testi::t(L"%d minuti", L"%d minutes"), minuti[i]);
                if (minuti[i] == 1) wcscpy(b, testi::t(L"1 minuto", L"1 minute"));
                voci.push_back(menu::voce(b, (UINT)(i + 1)));
            }
            if (comodita::timerAttivo() || comodita::timerSuona()) { voci.push_back(menu::separatore()); voci.push_back(menu::voce(testi::t(L"Ferma il timer", L"Stop the timer"), 100)); }
            POINT pt = {(LONG)e.r.left, (LONG)e.r.bottom}; ClientToScreen(pannello, &pt);
            const UINT s = menuScheda(voci, pt, pannello);
            if (s == 100) comodita::fermaTimer();
            else if (s >= 1 && s <= 9) { comodita::avviaTimer(minuti[s - 1] * 60000ULL); if (registro) { char b[48]; snprintf(b, 48, "timer: %d min", minuti[s - 1]); scrivi(b); } }
            pDirty = true;
            break;
        }
        case A_MICROFONO: {   // chi usa microfono e webcam, muto del microfono, privacy
            sensori::Sensori& se = sensori::Sensori::io();
            auto elenco = [](const std::vector<std::wstring>& v) { std::wstring r; for (auto& x : v) r += (r.empty() ? L"" : L", ") + x; return r; };
            std::vector<menu::Voce> voci = {menu::titolo(testi::t(L"Microfono e webcam", L"Microphone and camera"))};
            auto riga = [&](const wchar_t* chi, const std::vector<std::wstring>& v) {
                menu::Voce x = menu::voce(std::wstring(chi) + L": " + (v.empty() ? std::wstring(testi::t(L"non in uso", L"not in use")) : elenco(v)), 0);
                x.disattivata = true; voci.push_back(x);
            };
            riga(testi::t(L"Microfono", L"Microphone"), se.micApp);
            riga(L"Webcam", se.camApp);
            voci.push_back(menu::separatore());
            if (se.micC) voci.push_back(menu::voce(se.micMuto ? testi::t(L"Riattiva il microfono", L"Unmute the microphone") : testi::t(L"Silenzia il microfono", L"Mute the microphone"), 1));
            voci.push_back(menu::voce(testi::t(L"Privacy del microfono", L"Microphone privacy"), 2));
            voci.push_back(menu::voce(testi::t(L"Privacy della webcam", L"Camera privacy"), 3));
            POINT pt = {(LONG)e.r.left, (LONG)e.r.bottom}; ClientToScreen(pannello, &pt);
            const UINT s = menuScheda(voci, pt, pannello);
            if (s == 1) se.muta(!se.micMuto);
            else if (s == 2) { chiudiPannello(); apriPagina(L"ms-settings:privacy-microphone"); }
            else if (s == 3) { chiudiPannello(); apriPagina(L"ms-settings:privacy-webcam"); }
            pDirty = true;
            break;
        }
        case A_SVEGLIO:
        case A_SILENZIO: {   // durate in un menu
            const bool sv = e.azione == A_SVEGLIO;
            const bool acceso = sv ? comodita::sveglio() : comodita::silenzio();
            std::vector<menu::Voce> voci = {menu::titolo(sv ? testi::t(L"Tieni sveglio il PC", L"Keep the PC awake") : testi::t(L"Non disturbare", L"Do not disturb")),
                                            menu::voce(testi::t(L"Per 1 ora", L"For 1 hour"), 1)};
            if (sv) {
                voci.push_back(menu::voce(testi::t(L"Per 2 ore", L"For 2 hours"), 2));
                voci.push_back(menu::voce(testi::t(L"Finché non lo spengo", L"Until I turn it off"), 3));
            } else {
                voci.push_back(menu::voce(testi::t(L"Per 3 ore", L"For 3 hours"), 2));
                voci.push_back(menu::voce(testi::t(L"Fino a domani mattina", L"Until tomorrow morning"), 3));
            }
            if (acceso) { voci.push_back(menu::separatore()); voci.push_back(menu::voce(testi::t(L"Spegni", L"Turn off"), 4)); }
            POINT pt = {(LONG)e.r.left, (LONG)e.r.bottom}; ClientToScreen(pannello, &pt);
            const UINT s = menuScheda(voci, pt, pannello);
            if (s >= 1 && s <= 4) {
                const ULONGLONG ora = 3600000ULL;
                const ULONGLONG ms = s == 4 ? 0 : s == 1 ? ora : s == 2 ? (sv ? 2 : 3) * ora : (sv ? comodita::SEMPRE : comodita::finoADomani(8));
                if (sv) comodita::tieniSveglio(ms); else comodita::nonDisturbare(ms);
                if (registro) { char b[96]; snprintf(b, 96, "%s: %llu ms", sv ? "tieni sveglio" : "non disturbare", (unsigned long long)ms); scrivi(b); }
                pDirty = true;
            }
            break;
        }
        // le viste interne si aprono dentro la stessa finestra, dal pulsante toccato
        case A_WIDGET:       cambiaVista(P_WIDGET, {(e.r.left + e.r.right) / 2, e.r.top + 30 * dpi}); break;
        case A_SFONDO:       cambiaVista(P_SFONDO, {(e.r.left + e.r.right) / 2, e.r.top + 30 * dpi}); break;
        case A_NOTIFICHE:    cambiaVista(P_NOTIFICHE, {(e.r.left + e.r.right) / 2, e.r.top + 30 * dpi}); break;
        case A_INDIETRO:     tornaIndietro(); break;
        case A_NOTIZIA:      if (e.indice < (int)wid.notizie.size()) { chiudiPannello(); apriPagina(wid.notizie[e.indice].link.c_str()); } break;
        case A_LUMINOSITA:   break;
        case A_MIXER_VOL:    break;   // si trascina (procPannello)
        // la riga dell'uscita apre il mixer (volume per uscita e per app)
        case A_AUDIO:        cambiaVista(P_MIXER, {(e.r.left + e.r.right) / 2, (e.r.top + e.r.bottom) / 2}); break;
        case A_ALTRE_USCITE: menuUscite(e.r); break;
        case A_MIXER_MUTO:   mix.muto(e.indice / 1000, e.indice % 1000 - 1); leggiVolume(); pDirty = true; dirty = true; break;
        case A_MIXER_PRED: {
            const size_t u = e.indice / 1000;
            if (u < mix.uscite.size()) { sistema::Sistema::impostaUscita(mix.uscite[u].id); leggiUscita(); leggiVolume(); rileggiMixer(); dirty = true; }
            break;
        }
        case A_SCHERMO:      chiudiPannello(); apriPagina(L"ms-settings:display"); break;
        case A_ENERGIA:      chiudiPannello(); apriPagina(L"ms-settings:powersleep"); break;
        case A_IMPOSTAZIONI: chiudiPannello(); apriPagina(L"ms-settings:"); break;
        case A_NOTIFICA:
            // come toccare una notifica: si apre l'app e la notifica se ne va
            if (e.indice < (int)listaN.size()) {
                const notifiche::Notifica n = listaN[e.indice];
                chiudiPannello();
                if (!n.aumid.empty()) ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + n.aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                lettoreN.rimuovi(n.id);
            }
            break;
        case A_RIMUOVI:
            if (e.indice < (int)listaN.size()) { lettoreN.rimuovi(listaN[e.indice].id); ricaricaNotifiche(); }
            break;
        case A_CANCELLA: lettoreN.cancellaTutte(); ricaricaNotifiche(); break;
        case A_IMPOST_NOTIFICHE: chiudiPannello(); apriPagina(permessoN ? L"ms-settings:notifications" : L"ms-settings:privacy-notifications"); break;
        case A_MUTO: {
            IAudioEndpointVolume* v = volumeDispositivo();
            if (v) { BOOL m = FALSE; v->GetMute(&m); v->SetMute(!m, nullptr); v->Release(); }
            leggiVolume(); dirty = true; pDirty = true;
            break;
        }
        case A_VOLUME: break;
    }
}

inline auto Motore::elementoIn(float x, float y) const -> int {
    if (tVista >= 0) return -1;   // cambio di vista in corso: le zone sono quelle della vista di prima
    for (int i = (int)elementi.size() - 1; i >= 0; i--) {   // gli ultimi aggiunti stanno sopra (es. l'altoparlante sul volume)
        const auto& r = elementi[i].r;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

inline LRESULT CALLBACK Motore::procPannello(HWND h, UINT m, WPARAM w, LPARAM l) {
    Motore* d = gIstanza();
    if (!d) return DefWindowProcW(h, m, w, l);
    // da qui in poi si lavora sulla scheda di questa finestra. Una finestra non ancora registrata (in creazione:
    // CreateWindowEx manda gia' dei messaggi) non deve scambiare niente
    if (h == d->altra.pannello) d->usa(1 - d->slotCorrente);
    else if (h != d->pannello) return DefWindowProcW(h, m, w, l);
    const float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
    // carta dietro: un clic la riporta davanti (rimescolando), il resto del mouse non le arriva
    if (!d->davanti && d->pannelloTipo != P_NESSUNO && !d->pChiusura && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) {
        if (m == WM_LBUTTONUP) d->mescola(d->slotCorrente);
        return 0;
    }
    switch (m) {
        case WM_MOUSEMOVE:
            if (!d->pMouseDentro) { TRACKMOUSEEVENT tm{sizeof(tm), TME_LEAVE, h, 0}; TrackMouseEvent(&tm); d->pMouseDentro = true; }
            if (d->trascinaMixer >= 0) d->mixerDaMouse(x);
            else if (d->trascinaVolume) d->volumeDaMouse(x);
            else if (d->trascinaLum) d->luminositaDaMouse(x);
            else { const int s = d->elementoIn(x, y); if (s != d->pSopra) { d->pSopra = s; d->pDirty = true; } }
            return 0;
        case WM_MOUSELEAVE: d->pMouseDentro = false; d->pSopra = -1; d->pDirty = true; return 0;
        case WM_LBUTTONDOWN: {
            const int e = d->elementoIn(x, y);
            d->pPremuto = e; d->pDirty = true;
            if (e >= 0 && d->elementi[e].azione == A_VOLUME) { d->trascinaVolume = true; SetCapture(h); d->volumeDaMouse(x); }
            if (e >= 0 && d->elementi[e].azione == A_LUMINOSITA) { d->trascinaLum = true; SetCapture(h); d->luminositaDaMouse(x); }
            if (e >= 0 && d->elementi[e].azione == A_MIXER_VOL) { d->trascinaMixer = d->elementi[e].indice; SetCapture(h); d->mixerDaMouse(x); }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (d->trascinaVolume || d->trascinaLum || d->trascinaMixer >= 0) { d->trascinaVolume = d->trascinaLum = false; d->trascinaMixer = -1; ReleaseCapture(); }
            else { const int e = d->elementoIn(x, y); if (e >= 0 && e == d->pPremuto) d->eseguiPannello(d->elementi[e]); }
            d->pPremuto = -1; d->pDirty = true;
            return 0;
        }
        case WM_RBUTTONUP: {   // app in background: menu con l'alternativa del pannello di Windows
            const int e = d->elementoIn(x, y);
            if (e >= 0 && d->elementi[e].azione == A_RETE) { d->chiudiPannello(); apriPagina(L"ms-settings:network-status"); return 0; }
            if (e >= 0 && d->elementi[e].azione == A_BLUETOOTH) { d->chiudiPannello(); apriPagina(L"ms-settings:bluetooth"); return 0; }
            if (e < 0 || d->elementi[e].azione != A_APP) return 0;
            POINT pt{(LONG)x, (LONG)y}; ClientToScreen(h, &pt);
            const int indice = d->elementi[e].indice;
            const std::wstring exe = indice >= 0 && indice < (int)d->appSfondo.size() ? d->appSfondo[indice].exe : L"";
            const int s = (int)d->menuScheda({menu::voce(testi::t(L"Apri", L"Open"), 1), menu::separatore(),
                                               menu::voce(testi::t(L"Altre opzioni (pannello di Windows)", L"More options (Windows panel)"), 2)}, pt, h);
            // nel frattempo l'elenco puo' essere stato riletto (in ordine di nome): si ritrova la stessa app
            if (s == 1) {
                for (size_t k = 0; k < d->appSfondo.size(); k++)
                    if (d->appSfondo[k].exe == exe) { d->eseguiPannello(Elemento{{}, A_APP, (int)k}); break; }
            }
            else if (s == 2) { d->chiudiPannello(); d->pannelloWindows(); }
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (d->pannelloTipo == P_CONTROLLO) d->cambiaVolume(GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA * 0.02f), d->pDirty = true;
            return 0;
        // Esc chiude tutto; Backspace (o Alt+freccia sinistra) torna al centro di controllo da una vista interna
        case WM_APP + 4: if (d->pannelloTipo != P_NESSUNO && !d->pChiusura) prendiFuoco(h); return 0;
        case WM_KEYDOWN: if (w == VK_ESCAPE) d->chiudiTutte(); else if (w == VK_BACK) d->tornaIndietro(); return 0;
        case WM_SYSKEYDOWN: if (w == VK_LEFT) { d->tornaIndietro(); return 0; } break;
        case WM_CLOSE: d->chiudiTutte(); return 0;   // Alt+F4: come Esc (la finestra non si distrugge, si riusa)
        // il fuoco va altrove (non all'altra carta): si chiudono tutte e due, una dopo l'altra
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE && h == d->pannello) d->pFuocoAvuto = true;
            if (LOWORD(w) == WA_INACTIVE && !d->schedaFinestra((HWND)l)) {
                // primo piano perso appena aperto e senza nessun clic: e' Windows che lo rimbalza, non l'utente che va altrove
                // (su alcuni PC succede piu' volte di fila): lo si riprende. Un clic altrove invece chiude, qui o con controllaClicFuori
                const bool clic = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
                if (d->pannelloTipo != P_NESSUNO && !d->pChiusura && !clic && adesso() - d->pT0 < 1.0) {
                    d->pRipreso = true; d->scrivi("centro di controllo: primo piano perso appena aperto, lo riprende");
                    PostMessageW(h, WM_APP + 4, 0, 0);
                    return 0;
                }
                // il fuoco va via perche' si e' cliccata l'isola: decide quel clic (apre, chiude o cambia pannello).
                // Chiudendo qui, il clic subito dopo riaprirebbe quello appena chiuso
                POINT pt; GetCursorPos(&pt); const RECT is = d->rettIsolaSchermo();
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) && PtInRect(&is, pt)) return 0;
                d->chiudiTutte();
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// tasti del volume (intercettati da FluidWin): +1 alza, -1 abbassa (passi del 2%, come Windows), 0 muto
inline auto Motore::volumeTasto(int verso, float& livello, bool& silenzio) -> void {
    if (verso) cambiaVolume(verso * 0.02f);
    else { IAudioEndpointVolume* v = volumeDispositivo(); if (v) { BOOL m = FALSE; v->GetMute(&m); v->SetMute(!m, nullptr); v->Release(); } leggiVolume(); dirty = true; }
    usa(0); if (pannelloTipo == P_CONTROLLO) pDirty = true;
    livello = volume; silenzio = muto;
}

// tutte le app (nome, comando d'avvio), per la ricerca rapida
inline auto Motore::elencoApp() -> std::vector<std::pair<std::wstring, std::wstring>> {
    if (tutteApp.empty()) leggiTutteApp(true);              // la prima volta serve subito
    else if (adesso() - lLetto > 600) leggiTutteApp();      // poi si rinfresca in sottofondo
    std::vector<std::pair<std::wstring, std::wstring>> r;
    for (const auto& a : tutteApp) r.push_back({a.nome, a.avvio});
    return r;
}

// l'icona del centro di controllo a riposo, in coordinate della finestra del dock (non si muove con l'ingrandimento:
// e' il punto da cui il centro di controllo nasce e a cui torna)
inline auto Motore::rettCentro() const -> D2D1_RECT_F {
    const auto pos = posizioniRiposo();
    float x = W - S - 12 * dpi;
    for (size_t i = 0; i < voci.size() && i < pos.size(); i++) if (voci[i].avvio == AVVIO_CONTROLLO) x = pos[i];
    const float yb = yBaseRiposo();
    return {x, yb - S, x + S, yb};
}

// ... sullo schermo (per i banner delle notifiche, che escono da li', e per i clic sull'icona a pannello aperto)
inline auto Motore::rettIsolaSchermo() const -> RECT {
    const D2D1_RECT_F r = rettCentro();
    return {schermo.left + (LONG)r.left, schermo.bottom - H + (LONG)(r.top - PAD), schermo.left + (LONG)r.right, schermo.bottom - H + (LONG)(r.bottom + PAD)};
}

inline auto Motore::isolaVisibile() const -> bool { return attivo && !nascostoSchermoIntero; }

// apre il centro di controllo da fuori (1 app in background, 2 centro di controllo, 3 notifiche, 4 widget)
inline auto Motore::apriPannelloIsola(int tipo) -> void { if (attivo && tipo >= P_SFONDO && tipo <= P_MIXER) apriCentro(tipo); }

// Il centro di controllo (icona a destra nel dock, notch, Win+N...): si apre sulla vista chiesta. Gia' aperto: la
// stessa vista lo chiude, un'altra ci passa dentro la stessa finestra. Col launcher aperto prima si chiude lui
inline auto Motore::apriCentro(int tipo) -> void {
    if (registro) { char b[120]; snprintf(b, 120, "centro di controllo: chiesta vista %d (attivo %d, schermo intero %d, launcher %d, vista ora %d%s)", tipo, (int)attivo, (int)nascostoSchermoIntero, (int)lAperto, pannelloTipo, pChiusura ? ", si chiude" : ""); scrivi(b); }
    if (!attivo || nascostoSchermoIntero) return;
    if (lAperto) { if (!lChiusura) chiudiLauncher(); if (dopo != 2) apriDopo(2, tipo); return; }
    usa(0);
    if (pannelloTipo != P_NESSUNO && !pChiusura) {
        if (pannelloTipo == tipo || tipo == P_CONTROLLO) { chiudiPannello(); return; }
        cambiaVista(tipo, {PW / 2, PH / 2});   // (da fuori: la vista nuova si apre dal centro)
        return;
    }
    apriPannello(tipo);
}

inline auto Motore::apriDopo(int cosa, int tipo) -> void { dopo = cosa; dopoTipo = tipo; dopoT0 = adesso(); SetTimer(icone, TIMER_DOPO, 15, nullptr); }

inline auto Motore::suTimerDopo() -> void {
    const bool pronto = dopo == 1 ? nessunaScheda() : !lAperto;
    if (!pronto && adesso() - dopoT0 < 1.0) return;   // (al massimo un secondo: poi si apre comunque)
    KillTimer(icone, TIMER_DOPO);
    const int cosa = dopo; dopo = 0;
    if (cosa == 1) apriLauncher();
    else if (cosa == 2) apriCentro(dopoTipo);
}

}  // namespace dock
