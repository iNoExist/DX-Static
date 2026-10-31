# dxstatic — static colour for the DX Light PC SYNC backlight

Keeps a Robobloq **DX Light "PC SYNC Backlight"** strip on one fixed colour (default
orange, RGB 255,100,0) without the official DX Light app.

* `dxstatic.exe` (34 KB): tray app. It applies the colour at start, after Windows
  resumes from sleep, and whenever the strip is plugged in again. It uses 0 CPU while
  idle and about 1.7 MB of private memory.
* `dxctl.exe` (26 KB): command-line tool. It sends one command and exits.

Both are plain C against Windows' own `hid.dll` / `setupapi.dll`. No runtime, driver or
service is needed, and nothing touches the firmware.

---

## Findings

### Hardware (read from the device)

| | |
|---|---|
| USB VID:PID | `1A86:FE07` (QinHeng/WCH vendor ID; the MCU itself isn't identified by the descriptors) |
| Strings | manufacturer `ROBOBLOQ`, product `USBHID`, serial `0123456789` |
| Interface MI_00 | HID, vendor usage page `0xFF00`, 64-byte input/output reports (65 with report ID 0). **This is the control channel.** |
| Interface MI_01 | HID boot keyboard (the app's "open URL" feature). Not used. |
| Device info reply | id `000603`, firmware **1.8.2**, **54 LEDs**, 24" display setting |

### Software

DX Light 3.11.0 (`C:\Program Files\DX Light`) is an Electron app (`quiklight-desktop`). Its
main-process bundle `resources\app.asar` → `.webpack\main\index.js` contains:

* `LIGHT_VID = 6790 (0x1A86)`, `LIGHT_PID = 65031 (0xFE07)`. For this product id it uses
  **HID via `node-hid`**. Serial (`1A86:FE0C`, 115200 baud) and BLE belong to other models.
* The protocol class (webpack module `60895`):

  ```
  'R' 'B' len seq action payload...  checksum      checksum = (sum of all previous bytes) & 0xFF
  ```
  sent as an output report with report ID 0, in 64-byte chunks (module `5786`).

| action | name in app | used here |
|---|---|---|
| `0x86` | setSectionLED | **yes**: set static colour |
| `0x82` | readDeviceInfo | yes: read LED count / firmware, as the app does on every connect |
| `0x87` | setBrightness | yes: one byte, sent before the colour |
| `0x97` | turnOffLight | yes: `--off`, as the app does on quit |
| `0x80 0x81 0x83 0x85 0x89 0x8A 0x8B 0x91 0x93 0x95 0x96 0x98` | sync screen, **writeDeviceInfo**, uuid, effects, speed, … | no |

* **Static colour**, as built in `getUsbProtocol()`: runs of `[first LED, R, G, B, last LED]`,
  1-based. With 54 LEDs the app sends `[1,R,G,B,54, 55,0,0,0,254]`.
* **Colour limit** (`lowLight`, setting `isLightCompression`, on by default): if R+G+B > 255,
  all channels are scaled so the total is about 255. dxstatic does the same, so
  RGB(255,100,0) goes out as **(183,71,0)**, exactly what DX Light sends for that colour.
  This looks like a current limit for the USB supply, so there's no option to disable it.

  Packet for orange on this strip:
  `52 42 10 02 86 01 b7 47 00 36 37 00 00 00 fe 96`
* **Brightness** is a separate hardware setting. The app's slider value `a` (0–1, saved as
  `brightnessColor.a` in its `config.json`) goes out as `setBrightness(max(5, floor(255·a)))`.
  For a USB strip in static mode, the app's brightness handler (module `88492`) sends
  `setBrightness`, waits about 220 ms (its 200 ms write delay plus 20 ms), then re-sends
  the colour frame. dxstatic does the same on every apply. The app sends brightness on
  every connect, so the controller most likely keeps it in RAM only; dxstatic sends it
  rather than relying on it. Brightness works on top of the R+G+B ≤ 255 cap, so 100% is
  as bright as DX Light itself goes.
  Example, 100%: `52 42 07 05 87 ff 26`; DX Light's old 59%: `52 42 07 05 87 96 bd`.

### Why the strip changed when DX Light closed

The controller doesn't time out. DX Light's quit handler (`turnOffLights`, module `51772`)
sends `turnOffLight` (0x97) four times before it exits. DX Light's log
(`%APPDATA%\quiklight-desktop\logs\main.log`) shows those four sends on every quit. That
command switches the strip **off**. Rainbow is the firmware's built-in effect, which you
get by pressing the strip's button.

### Can the colour persist without software?

**Partly, and that part was tested on your hardware:**

* **Yes, while the strip has power.** After one `setSectionLED`, the controller keeps
  showing the colour after the program has closed its handle and exited. With DX Light
  closed, `dxctl --apply` turned the strip orange, the process exited, and the strip
  stayed orange with no Robobloq or dxstatic process running.
* **There is no save-to-flash command.** None of the app's protocol actions stores a
  colour. The only write command, `writeDeviceInfo`, stores configuration such as the LED
  count. Writing the colour into firmware would mean modifying or reflashing it, which is
  out of scope. So after the controller loses power or resets, it shows its own effect
  until something sends the colour again.

That is why the tray app exists. It sits idle and re-sends the colour only at login,
after resume from sleep, and when the strip is plugged in again.

### How the packets were validated

USBPcap/Wireshark aren't installed, and I didn't install a capture driver. Instead:

1. `tools/refpackets.js` runs **DX Light's own modules** (`60895`, `72299`, `79539`,
   `42127`) taken unchanged from `app.asar` to generate reference packets.
   `dxctl --dump` matched them byte for byte for 14 colours × 5 LED counts (70/70), and
   with brightness added for 9 colours × 3 LED counts × 12 levels, including the
   minimum-5 clamp (324/324).
2. On the live strip, `dxctl --info` returned exactly the values DX Light logged for it
   (id, firmware 1.8.2, 54 LEDs, 24", and the device UUID).
3. Visual checks on the strip: `--off` switched it off, the button then gave rainbow, and
   `--apply` gave orange that held after the process exited, and `--brightness 10`
   visibly dimmed the strip.

---

## Build

Needs MinGW-w64 gcc and windres (MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-gcc`).

```bat
set PATH=C:\msys64\ucrt64\bin;%PATH%
build.cmd
```

The output goes to `bin\`. The only dependencies are Windows system DLLs and the
Universal CRT, which ships with Windows 10 and 11.

## Install / startup

```bat
install.cmd
```

This copies both exes to `%LOCALAPPDATA%\Programs\DXStatic`, adds
`HKCU\...\CurrentVersion\Run\DXStatic` (per-user, no admin) and starts the tray app.

Turn startup on or off with the tray menu item **Start with Windows**, or:

```bat
"%LOCALAPPDATA%\Programs\DXStatic\dxctl.exe" --startup off
"%LOCALAPPDATA%\Programs\DXStatic\dxctl.exe" --startup on
```

## Use

Tray icon (a dot in the current colour):

* **double-click / Apply colour now**: re-send the colour
* **Choose colour…**: pick a colour. It is sent to the strip straight away so you can
  see it, then you're asked whether to keep it (No restores the previous colour).
* **Brightness**: 10 / 25 / 50 / 75 / 100 %. Any other value set with
  `dxctl --brightness` shows ticked here too.
* **Start with Windows**: toggles the Run entry
* **Turn strip off and exit**: the same command DX Light sends when it quits
* **Exit (keep colour)**: closes the app. The strip stays on its colour until it loses power.

Command line (`dxctl.exe`; `dxstatic.exe` accepts the same options):

```
dxctl --apply [RRGGBB]   send saved colour (or RRGGBB, not saved) once
dxctl --set RRGGBB       save and send
dxctl --brightness PCT   save brightness 1-100 % and send
dxctl --off              turn strip off (as DX Light does on quit)
dxctl --info             device id / firmware / LED count / settings
dxctl --startup on|off
dxctl --dump RRGGBB [N] [PCT]  show packets, send nothing
```

Settings are stored in `HKCU\Software\DXStatic`: `Color` (DWORD 0xRRGGBB, default
0xFF6400) and `Brightness` (DWORD percent, default 100). Running `dxstatic.exe` again
while it's already running makes the running copy reload them and re-apply.

## Don't run both

If you start DX Light, it takes over the strip. When it quits it turns the strip
**off**. After that, double-click the tray icon (or run `dxctl --apply`) to bring the
colour back. Make sure DX Light isn't set to start with Windows (it wasn't on the PC
this was developed on).

## Rollback

To go back to normal DX Light operation:

1. Run `uninstall.cmd`. It stops the tray app, removes the Run entry and the
   `HKCU\Software\DXStatic` key, and deletes `%LOCALAPPDATA%\Programs\DXStatic`.
2. Start DX Light as before. It reconnects and applies its own settings. Nothing in
   DX Light's install or its `config.json` was changed.
3. To get the built-in rainbow without DX Light, run `dxctl --off` (before uninstalling)
   and press the strip's button, or unplug and replug the strip.

Nothing was written to the controller other than colour, info-query and turn-off
packets, none of which persist. There is nothing to undo on the device.

## Not yet verified on hardware

* Re-applying after **sleep/resume** and after a **USB replug**. The code is in place
  (`WM_POWERBROADCAST` resume, HID arrival for `vid_1a86&pid_fe07&mi_00`, retries every
  2 s for 20 s), but it hasn't been exercised. Test: sleep the PC, wake it, and check
  the strip turns orange within a few seconds.
* That the strip shows rainbow after a cold power-up. This is expected from your
  original description but wasn't tested here.
* The **Choose colour** dialog flow has only been built, not clicked through.

## Files

```
src/dxstatic.c         tray app + CLI (one source, CLI_ONLY builds dxctl.exe)
src/dxstatic.rc/.manifest  DPI-aware manifest, visual styles
build.cmd              build both exes
install.cmd / uninstall.cmd
tools/hidprobe.c       read-only HID descriptor/caps dump
tools/refpackets.js    reference packets from DX Light's own code
```

## Licence

MIT, see [LICENSE](LICENSE). This project is not affiliated with or endorsed by
Robobloq. "DX Light" is their product name, used here only to say what this works with.
No DX Light code is included; `tools/refpackets.js` reads it from your own installation.
