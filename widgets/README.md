# FluidWin Starter Custom Widgets

FluidWin watches `%LOCALAPPDATA%\FluidWin\widgets\*.json` and renders each file as a native Liquid Glass dock icon + flyout panel with zero arbitrary code execution inside FluidWin.

## Starter Widget: Tailscale (`tailscale-widget.ps1`)

A complete, zero-dependency Tailscale dock widget that demonstrates every custom widget capability:

- **Ringless animated dock icon:** Uses the built-in 3x3 `tailscale` dot glyph with `"animate": "wave"` (or `"pulse"`) while connected and `"none"` when stopped.
- **Interactive flyout header toggle:** Declares `"toggle": { "key": "connected", "on": true|false }`. Flipping the switch writes `tailscale.action.json`, and the script runs `tailscale up` or `tailscale down` and refreshes immediately.
- **Click-to-copy peer IPs:** Lists this PC plus your tailnet devices with online/active/exit-node badges. Clicking any row copies its Tailscale IP to the clipboard.
- **Native Settings config page:** Declares `"config"` controls (**Online devices only**, **Show this PC first**, **Animate icon**, **Animation style**, **Glow color**, and **Refresh interval**) rendered under **Settings > Widgets > Tailscale** and persisted to `tailscale.config.json`.

### Running the Tailscale Widget

Run once to generate `tailscale.json`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\widgets\tailscale-widget.ps1 -Once
```

Run hidden in the background so status, flyout toggles, and Settings changes update live:

```powershell
Start-Process powershell.exe -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"$PWD\widgets\tailscale-widget.ps1"
```

Then open **Settings > Widgets** in FluidWin and enable **Tailscale**.

## Building Your Own Widget

Drop any UTF-8 JSON file (max 256 KB, up to 32 items) into `%LOCALAPPDATA%\FluidWin\widgets\`:

- `icon`: `{ glyph, text, color, glowColor, ring, animate }`
  - `glyph`: `ring` | `battery` | `spark` | `cpu` | `thermometer` | `tailscale` | `dot` | `text`
  - `color`: `#RRGGBB` hex accent color
  - `glowColor`: optional `#RRGGBB` hex override for animated glow halo (defaults to `color`)
  - `ring`: `0..100` (omit to render a clean ringless icon)
  - `animate`: `none` | `pulse` | `breathe` | `wave` | `spin`
- `toggle`: `{ key, on }` (renders an interactive switch in the flyout header that writes `<id>.action.json`)
- `items`: `[{ title, subtitle, badge, color, progress, copy, url }]`
- `config`: `[{ key, label, description, type, default, options, min, max, step, unit }]` (rendered in **Settings > Widgets > <widget>** and saved to `<id>.config.json`)
  - `type`: `toggle` (boolean default) | `choice` (string default + `options` array, max 5) | `color` (swatch picker: `"auto"` or `#RRGGBB` string default + `options` array, max 8) | `slider` (numeric default)
  - `slider` fields: `min` (number, default `0`), `max` (number, default `100`), `step` (number, default `1`), `default` (number, default `min`), `unit` (optional string, max 8 chars)
- `footer`: `{ label, url }`
