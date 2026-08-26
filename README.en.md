# FoloToy AI Passport · Voice Input

[简体中文](README.md) | [English](README.en.md)

A voice input solution for the **FoloToy AI Passport** (ESP32-C3): hold the device button to talk — recognized candidates appear **live in a floating window on your Mac/Windows desktop**; release to have the final text **injected into the focused input box automatically**. WeChat-style voice input for the desktop.

```text
Hold OK to talk ──► device records ──► BLE / WiFi / USB ──► desktop relay ──► Volcano streaming ASR
                                                                                    │
  focused input box ◄── inject final text (once) ◄── release ──► floating window shows candidates ◄─┘
```

> The repository contains both the **firmware** (ESP-IDF, device side) and the **desktop companion** (`companion/`, macOS/Windows relay), which work together over three channels: BLE / WiFi / USB.

## Features

### Device firmware

- **Push-to-talk**: hold OK in READY state to record (start beep), release to send — no cancel window, no timeout residue
- **LISTENING page**: microphone icon + elapsed timer while recording (replaces the classic REC dot / level bar)
- **Three-button interaction**: OK hold-to-talk, DOWN = Enter, OK double-click clears the input box (global)
- **Full state machine**: HOME → READY → LISTENING → TRANSCRIBING → AGENT_RUNNING → APPROVAL → DONE
- **Physical approval**: agent approval requests are shown on-device — OK approve / UP reject / DOWN view diff (disabled in the GUI by default)
- **Tones**: start / send / approval / success / reject / error
- **Three transport channels**: BLE (direct to Mac, GATT service `0xA2B0`) / WiFi+WebSocket+mDNS (Windows PCs without Bluetooth) / USB (USB-Serial-JTAG wired, full console command surface)
- **Low-memory audio pipeline**: 3200-byte/100 ms frames, static ring buffers, source-side frame dropping (never buffers a full utterance), drop reconciliation
- **Console commands**: `st` (heap/stack watermarks, link state, drop stats), `mode`, `wifi set` (password never echoed), `ws`, `mdns`, `logs`, `system`, `factory reset`, `reboot`

### Desktop companion (macOS + Windows)

- **Candidate floating window** (core): partial ASR results — **full accumulated text** — appear live in a borderless always-on-top window anchored at the bottom center of the screen; auto-wraps and grows upward as you speak; on release the final text is injected once and the window disappears
- **Never steals focus**: the window never calls focus; Windows uses `WS_EX_NOACTIVATE` + `SWP_NOACTIVATE`, macOS hot path avoids WindowServer syncs (no spinning beachball while talking)
- **High-frequency frame merging**: 120 ms merge window keeps only the latest frame, first frame renders immediately — no per-frame redraw stutter
- **5-step wizard**: welcome → auto-discover device (BLE → WiFi → USB) → Volcano ASR key config (zero-audio handshake test, never echoes the key) → system permission guide (macOS) → status page
- **System tray**: stays in the menu bar / tray after connecting — status rows, diagnostics, settings
- **Diagnostics page**: full device console command surface over USB; read-only runtime state over BLE/WiFi
- **Injection**: macOS clipboard + Cmd+V (requires Accessibility permission; CJK must go through the clipboard channel); standalone Windows injector

## Controls

| Button | Context | Action |
| --- | --- | --- |
| OK **hold** | READY | Start recording (PTT), release to send |
| OK **double-click** | anywhere | Clear all text in the input box |
| DOWN click | HOME / READY | Press Enter in the input box (submit) |
| OK click | HOME | Enter READY (workflow ready) |
| OK click | APPROVAL | Approve the agent request |
| UP click | APPROVAL | Reject the agent request |
| DOWN click | APPROVAL | View diff details |

## Quick start

### Desktop companion (Mac / Windows)

1. Install dependencies (a venv is recommended): `pip install -r companion/requirements.txt`
2. Configure the key (**never committed**): `cp companion/config.example.json companion/config.local.json` and fill in your Volcano Engine API key; or use the `VOLCANO_API_KEY` environment variable
3. Run the wizard (GUI):
   ```bash
   companion/.venv/bin/python companion/fre_app.py
   ```
   Or use the CLI relay (auto-scans and connects to "AI Passport" over BLE):
   ```bash
   companion/.venv/bin/python companion/relay.py
   ```

On macOS, grant **Accessibility** permission (required for clipboard + Cmd+V injection) and Bluetooth permission on first use. Windows instructions: [`companion/WINDOWS.md`](companion/WINDOWS.md).

### Firmware (ESP32-C3)

Requires ESP-IDF 5.5.x (known environment: 5.5.3):

```bash
source "$HOME/esp/esp-idf-v5.5.3/export.sh"   # or your installation path
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

The first build pulls LVGL, `esp_lvgl_port`, `button`, `esp_codec_dev` and other dependencies via the ESP-IDF Component Manager. The console runs over USB-Serial-JTAG (GPIO18/19; the default UART0 TX on GPIO21 conflicts with this board's backlight).

## Repository layout

```text
main/                    ESP32-C3 firmware: state machine, UI, three-channel transport, audio streaming, console commands
components/bsp/          Board drivers: display / buttons / audio / battery / shared I2C (bsp_pins.h is the single source of truth)
companion/               Desktop side: relay (streaming ASR + injection), floating window, wizard, tray, three-channel transport
tests/                   Hardware-free firmware logic tests (pure C, ctest)
docs/                    Hardware development guide and acceptance docs
sdkconfig.defaults       ESP32-C3, USB console, Flash, LVGL defaults
partitions.csv           Custom partition table (factory 4 MB)
```

## How it works

1. Hold OK in READY → start beep → `voice.start` → 3200-byte/100 ms audio frames stream up over BLE (GATT NOTIFY) / WS / USB
2. The desktop relay streams frames into the Volcano Engine ASR (`bigmodel_async`; every result packet carries the **full accumulated text**)
3. Partial results → the floating window updates live (120 ms frame merging; with the GUI attached, the device no longer previews candidates)
4. Release OK → `voice.end` → final result → **injected into the focused input box once** (clipboard + Cmd+V) → window closes

The injection target is whatever window the user is focused on — the floating window never steals focus. Preview failures are logged only and never block injection.

## Development

Firmware logic tests (hardware-free, cmake + ctest):

```bash
cd tests && cmake -B build && cmake --build build && ctest --test-dir build
```

Desktop unit tests (pytest):

```bash
companion/.venv/bin/python -m pytest companion/tests/ -q -o asyncio_mode=auto
```

- State machine, protocol, audio framing and UI pixel math all have host-side test coverage; a passing build is not hardware validation
- Keep hardware logic in `components/bsp` and application logic in `main`; separate testable pure logic from ESP-IDF/LVGL
- LVGL is not thread-safe; button callbacks only dispatch lightweight events; slow operations belong in worker tasks

### On-device acceptance status

The full checklist for when hardware arrives is in [`docs/ON_DEVICE.md`](docs/ON_DEVICE.md). Current status: **Build PASS, host tests PASS, device tests NOT RUN (awaiting hardware)**. Key items: real floating-window sessions (hold PTT → live candidates → single injection on release), Windows focus behavior, no dropped words over ~15 s of continuous speech (if drops occur, raise `AUDIO_Q_MAX` from 20 to 30–40 in `companion/relay.py`), BLE throughput/drop rate, USB unplug recovery, battery readings.

## Security

- Volcano API keys / WiFi passwords are **never committed**: `companion/config.local.json` is gitignored; `VOLCANO_API_KEY` env var is supported
- The device `wifi set` command **never echoes the password**; credentials live only in device NVS
- Logs never contain keys; session/diagnostic payloads carry no credentials

## License

MIT © 2026 FoloToy, see [LICENSE](LICENSE).

Third-party components (LVGL, esp_lvgl_port, NimBLE, cJSON, etc.) are copyright their respective authors.
