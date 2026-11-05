# FluidWin

**Windows, redesigned from scratch. By one person, for the love of it.**

FluidWin is my small personal project. One day I wondered what Windows would feel like if it were softer, more
alive and more pleasant to use. So I started redesigning it, one piece at a time.

The result is an app that sits on top of Windows 11 and changes how it looks and feels:

- a **dock** instead of the taskbar, with **App** on the left and the **control center** on the right. It follows you
  to another screen when you bring the mouse to its bottom edge;
- a **control center** where everything opens inside the same glass panel: background apps, notifications,
  weather and news;
- the time and date in a small **notch** at the top of the screen;
- new round **window buttons** that sit exactly where the original ones were;
- windows that **minimize into the dock** with a genie effect, and come back out of it;
- windows that **open and close** with a little bounce, and turn "jelly-like" while you drag them (with their content
  still alive);
- a **search** (Alt+Tab) that lights up the edges of the screen with a wave of color;
- an **overview** of all your open windows (Win+Tab).

When you close it, Windows goes back exactly to how it was.

## How to try it

1. Go to the **[latest release](../../releases/latest)** and download the `FluidWin-x.y.z.zip` file (under
   **Assets**).
2. Extract it anywhere and open `FluidWin.exe`.

That's it! There is nothing to install and nothing else to download.

> Windows may say "Windows protected your PC": this happens with new apps that have no digital signature. Click
> **More info -> Run anyway**.

You need **Windows 11**.

## How to close it

Open the **control center** (the last icon on the right of the dock) -> **In background** -> **FluidWin** -> **Esci**
(Exit). From the same menu you can open the **settings** and turn each effect on or off.

To remove it completely, close it and delete its folder.

## A few words on how it was made

FluidWin is partly **vibe-coded**: much of the code was written by an AI (Claude), **always under my supervision**.
I decided what to build and how it should look and feel, tested everything on my own PC, reported every flaw and
approved every change, until the result matched what I had in mind.

## Good to know

- This is still an **early version**. I tested it thoroughly on my PC, but not on every PC in the world. If something
  doesn't work, tell me in the [Issues](../../issues).
- The interface is in **Italian** for now.
- It doesn't ask for administrator rights and doesn't touch Windows files.
- It only goes online for the weather ([Open-Meteo](https://open-meteo.com)) and the news
  ([ANSA](https://www.ansa.it)).

## For those who want to look at the code

It's all here, in C++. To build it you need [MSYS2](https://www.msys2.org) with `mingw-w64-ucrt-x86_64-gcc`, then
just run `compila.ps1`.

## License

[MIT](LICENSE): you can use it, change it and share it freely. (c) 2026 Clooudy.
FluidWin is an independent project, not affiliated with Microsoft.
