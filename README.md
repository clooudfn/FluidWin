# FluidWin

**Windows, ridisegnato da capo. Da una persona sola, per passione.**

FluidWin è un mio piccolo progetto personale: un giorno mi sono chiesto come sarebbe Windows se fosse più morbido,
più vivo, più bello da usare. E ho cominciato a ridisegnarlo, un pezzo alla volta.

Il risultato è un'app che si appoggia sopra Windows 11 e lo cambia nell'aspetto e nelle sensazioni:

- 🧊 un **dock** al posto della barra delle applicazioni;
- 🪄 finestre che si **riducono nel dock** con l'effetto genio e ne riescono;
- 🫧 finestre che **si aprono e si chiudono** con un piccolo rimbalzo, e diventano "di gelatina" quando le trascini;
- 🌈 una **ricerca** (Alt+Tab) che accende i bordi dello schermo con un'onda di colori;
- 🔲 una **panoramica** di tutte le finestre (Win+Tab);
- 🪟 pannelli di vetro per volume, notifiche, meteo e impostazioni.

Quando lo chiudi, Windows torna esattamente com'era.

## Come provarlo

1. Vai su **[Releases](../../releases)** e scarica `FluidWin-1.0.0.zip`.
2. Estrailo dove vuoi e apri `FluidWin.exe`.

Fine. Non c'è niente da installare e niente altro da scaricare.

> Windows potrebbe dirti "Windows ha protetto il PC": succede con le app nuove senza firma digitale. Clicca
> **Ulteriori informazioni → Esegui comunque**.

Serve **Windows 11**.

## Come si chiude

Clic destro sull'icona di FluidWin vicino all'orologio → **Esci**. Da lì trovi anche le **Impostazioni**, per
accendere o spegnere ogni effetto.

Per toglierlo del tutto, chiudilo e cancella la cartella.

## Due parole su come è nato

FluidWin è in parte **vibe-coded**: buona parte del codice l'ha scritta un'intelligenza artificiale (Claude),
**sempre sotto la mia supervisione**. Io decidevo cosa fare e come doveva essere, provavo ogni cosa sul mio PC,
segnalavo ogni difetto e approvavo ogni modifica, finché il risultato non era quello che avevo in mente.

## Da sapere

- È la **prima versione**: l'ho provata a fondo sul mio PC, ma non su tutti i PC del mondo. Se qualcosa non va,
  raccontamelo nelle [Issues](../../issues).
- È tutta **in italiano**.
- Non chiede permessi di amministratore e non tocca i file di Windows.
- Va su internet solo per il meteo ([Open-Meteo](https://open-meteo.com)) e le notizie ([ANSA](https://www.ansa.it)).

## Per chi vuole guardare il codice

È tutto qui, in C++. Per compilarlo serve [MSYS2](https://www.msys2.org) con `mingw-w64-ucrt-x86_64-gcc`, poi
basta lanciare `compila.ps1`.

## Licenza

[MIT](LICENSE): puoi usarlo, cambiarlo e condividerlo liberamente. © 2026 Clooudy.
FluidWin è un progetto indipendente, non affiliato a Microsoft.
