# FoloToy AI Passport · Voice Input & AI Control Surface

[简体中文](README.md) | [English](README.en.md)

**FoloToy AI Passport** is an AI companion device (ESP32-C3) — but not just "another keyboard". Hold the device button to talk: recognized candidates appear **live in a floating window on your Mac/Windows desktop**, and when you release, the final text is **injected into the focused input box automatically** (WeChat-style voice input). At the same time, the agent's thinking, execution and approval flow are all visualized on the device screen, and critical actions require your **physical button confirmation**.

In one sentence: it moves AI from the browser to your desktop input box, and puts the AI's **ears** (voice input), **eyes** (state visualization) and **handbrake** (physical approval) into a device you hold.

```text
Hold VOL+ to talk ──► device records ──► desktop relay ──► Volcano streaming ASR
                        │  over BLE / USB
                        ▼  live partials
            Floating window shows candidates
                        │
                        ▼  release → final
            Inject into focused input box (once)
```

> The repository contains both the **firmware** (ESP-IDF, device side) and the **desktop companion** (`companion/`, macOS/Windows relay), which work together over two channels: BLE / USB.

## Architecture

| Layer | Component | Responsibility |
| --- | --- | --- |
| Device | ESP32-C3 firmware (ESP-IDF 5.5 + LVGL 9.5) | State machine, buttons/recording, UI rendering, dual-channel transport, power management |
| Desktop | companion (macOS / Windows) | Dual-channel access, Volcano streaming ASR forwarding, floating window, clipboard injection, wizard, tray |
| Cloud | Volcano Engine large-model streaming ASR | Audio → text (partial full-accumulation / final) |

Data flow: the device captures 16 kHz audio → streams 100 ms frames up over **BLE / USB** (any channel) → the desktop relay feeds them into Volcano ASR in streaming mode → partial results echo live to the floating window and the device screen → on release the final text is injected once and the window closes. The reverse channel carries agent status, approval requests and button verdicts.

## Highlights

- **Dual-channel redundant transport**: one audio/event protocol runs over two physical channels — BLE (direct to Mac, GATT service `0xA2B0`) and USB-Serial-JTAG (wired debugging + full console command surface). Any channel failure converges the session back to READY and keeps approvals pending for reconnect — a broken link never bricks the device.
- **State-machine driven + snapshot rendering**: the core state machine is a pure-C reducer (`state + event → action`) with zero ESP-IDF dependencies; 7 host test suites run directly on a PC. UI rendering is driven by snapshot diffs, fully decoupling logic from hardware — testable and portable.
- **Low-resource streaming audio pipeline**: on an ESP32-C3 with only **400 KB SRAM**, audio streams up in 3200-byte/100 ms frames — static ring buffers (no dynamic allocation), source-side frame dropping (never buffers a full utterance), drop reconciliation against silent loss; long sentences never stall.
- **Physical security approval**: agent permission requests (modify files, run commands) are not push notifications — they are an approval page on the device screen. **OK approve / UP reject** — a real physical press counts; the approval state never sleeps.
- **Two-level power management**: 20 s idle → backlight off (rendering skipped, panel frozen on the last frame); 60 s idle → panel SLPIN sleep at μA level; any key wakes instantly with zero repaint (ST7789 DRAM survives sleep).
- **Polished desktop experience**: the floating window **never steals focus** (Windows `WS_EX_NOACTIVATE` + macOS zero WindowServer sync on the hot path — no spinning beachball while talking), 120 ms frame merging renders only the latest frame, a 5-step wizard verifies the ASR key with a zero-audio handshake, tray residency, and dual-platform injection (CJK goes through the clipboard channel).

## Features

### Device firmware

- **Push-to-talk**: hold **VOL+** in READY state to record (start beep), release to send — no cancel window, no timeout residue
- **Exit transcribing**: click VOL+ in TRANSCRIBING to exit back to READY immediately; late recognition results are not shown
- **LISTENING page**: microphone icon + elapsed timer while recording (replaces the classic REC dot / level bar)
- **Agent workflow visualization**: THINKING / RUNNING / DONE states, task echo, offline banner — see what the AI is doing
- **Physical approval**: agent approval requests show on-device — OK approve / UP reject / DOWN view diff (disabled in the GUI by default)
- **Three-button interaction**: VOL+ hold-to-talk, VOL+ click exits transcribing, DOWN = Enter, OK double-click clears the input box (global)
- **Full state machine**: HOME → READY → LISTENING → TRANSCRIBING → AGENT_RUNNING → APPROVAL → DONE
- **Tones**: start / send / approval / success / reject / error
- **Dual-channel transport**: BLE / USB-Serial-JTAG (full console command surface)
- **Low-memory audio pipeline**: 3200-byte/100 ms frames, static ring buffers, source-side frame dropping, drop reconciliation
- **Two-level screen-off**: 20 s idle → backlight off; 60 s idle → panel SLPIN power-off; any key wakes; approval state stays on
- **Console commands**: `st` (heap/stack watermarks, link state, drop stats), `mode` (ble/usb), `logs`, `system`, `factory reset`, `reboot`

### Desktop companion (macOS + Windows)

- **Candidate floating window** (core): partial ASR results — **full accumulated text** — appear live in a borderless always-on-top window anchored at the bottom center of the screen; auto-wraps and grows upward as you speak; on release the final text is injected once and the window disappears
- **Never steals focus**: the window never calls focus; Windows uses `WS_EX_NOACTIVATE` + `SWP_NOACTIVATE`, macOS hot path avoids WindowServer syncs (no spinning beachball while talking)
- **High-frequency frame merging**: 120 ms merge window keeps only the latest frame, first frame renders immediately — no per-frame redraw stutter
- **5-step wizard**: welcome → auto-discover device (BLE → USB) → Volcano ASR key config (zero-audio handshake test, never echoes the key) → system permission guide (macOS) → status page
- **System tray**: stays in the menu bar / tray after connecting — status rows, diagnostics, settings
- **Diagnostics page**: full device console command surface over USB; read-only runtime state over BLE
- **Injection**: macOS clipboard + Cmd+V (requires Accessibility permission; CJK must go through the clipboard channel); standalone Windows injector

## Controls

| Button | Context | Action |
| --- | --- | --- |
| **VOL+ hold** | READY | Start recording (PTT, start beep), release to send |
| **VOL+ click** | TRANSCRIBING | Exit the transcribing scene, back to READY (late results dropped) |
| OK **double-click** | anywhere | Clear all text in the input box |
| DOWN click | HOME / READY | Press Enter in the input box (submit) |
| OK click | HOME | Enter READY (workflow ready) |
| OK click | APPROVAL | Approve the agent request |
| UP click | APPROVAL | Reject the agent request |
| DOWN click | APPROVAL | View diff details |

> Long-press thresholds: VOL+ 300 ms starts recording; OK 500 ms locks/unlocks the screen (locked = power-saving off-screen; keys still execute but do not wake the display).

## Quick start

### Desktop companion (Mac / Windows)

**Use the packaged client (recommended — no terminal needed for end users)**:

- **macOS**: `companion/dist/AI Passport.app` (built with `companion/build/pack.py`, or use a release build) — double-click to launch
- **Windows**: run `python3 companion/build/pack.py` on a Windows build machine to produce `dist/AI Passport.exe` (or use a release build)

On first launch the 5-step wizard opens: welcome → auto-discover the device (BLE → USB) → Volcano ASR key config → system permission guide (macOS) → status page; the app then stays in the system tray. First run writes `companion/config.local.json` (Volcano API key, **never committed**).

**Run from source (development)**:

```bash
pip install -r companion/requirements.txt
cp companion/config.example.json companion/config.local.json   # fill in the Volcano API key
companion/.venv/bin/python companion/fre_app.py                 # wizard GUI (--dry-run walks the flow with a fake link)
companion/.venv/bin/python companion/relay.py                   # CLI relay (auto-scans "AI Passport" over BLE)
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
main/                    ESP32-C3 firmware: state machine, UI, dual-channel transport, audio streaming, console commands
components/bsp/          Board drivers: display / buttons / audio / battery / shared I2C (bsp_pins.h is the single source of truth)
companion/               Desktop side: relay (streaming ASR + injection), floating window, wizard, tray, dual-channel transport
tests/                   Hardware-free firmware logic tests (pure C, ctest)
docs/                    Hardware development guide and acceptance docs
sdkconfig.defaults       ESP32-C3, USB console, Flash, LVGL defaults
partitions.csv           Custom partition table (factory 4 MB)
```

## How it works

1. Hold VOL+ in READY → start beep → `voice.start` → 3200-byte/100 ms audio frames stream up over BLE (GATT NOTIFY) / WS / USB
2. The desktop relay streams frames into the Volcano Engine ASR (`bigmodel_async`; every result packet carries the **full accumulated text**)
3. Partial results → the floating window updates live (120 ms frame merging; with the GUI attached, the device no longer previews candidates)
4. Release OK → `voice.end` → final result → **injected into the focused input box once** (clipboard + Cmd+V) → window closes
5. The final text also echoes back on the device screen; agent status (THINKING / RUNNING / DONE) and approval requests stream down
6. An approval request → the device enters the approval page → OK/UP physical press decides → the verdict streams up → the agent continues

The injection target is whatever window the user is focused on — the floating window never steals focus. Preview failures are logged only and never block injection.

## Design decisions

- **Why two channels**: BLE covers Macs with Bluetooth; USB is both the debugger and the last-resort wired link — any single link can fail and the other keeps working. Disconnect events converge the session uniformly, leaving no half-open sessions.
- **Why a pure-C reducer state machine**: the `state + event → action` pattern keeps all transition logic **hardware-free**; 7 ctest suites cover state transitions, protocol codec, audio framing and UI pixel math. UI renders from snapshot diffs — adding a page adds no state coupling.
- **Why a static ring-buffer audio pipeline**: the ESP32-C3 has only 400 KB SRAM — dynamic allocation plus buffering a full utterance would blow the heap. 3200-byte/100 ms frames, source-side frame dropping and drop reconciliation are what let an entry-level MCU act as an AI input device.
- **Why physical approval**: AI auto-modifying files or running commands is risky — approvals don't live in a notification banner, they live on the device screen. A real button press counts, and the approval state never sleeps.
- **Why two-level screen-off**: 20 s kills the backlight (saving the backlight LED's mA); 60 s puts the panel into SLPIN (saving the internal oscillator and driver, μA) — different goals, same wake experience: any key lights up instantly with zero repaint (ST7789 DRAM survives sleep).

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

The full checklist for when hardware arrives is in [`docs/ON_DEVICE.md`](docs/ON_DEVICE.md). Current status: **Build PASS, host tests PASS, device tests NOT RUN (awaiting hardware)**. Key items: real floating-window sessions (hold PTT → live candidates → single injection on release), Windows focus behavior, no dropped words over ~15 s of continuous speech (if drops occur, raise `AUDIO_Q_MAX` from 20 to 30–40 in `companion/relay.py`), BLE throughput/drop rate, USB unplug recovery, battery readings, two-level screen-off timing and wake content restoration.

## Extensibility

Voice input is the **first application** on this pipeline: the same audio stream + event protocol (shared by both channels) can naturally carry more — transcription, translation, agent commands, remote control. Both the firmware and the desktop side are open under MIT; reskinning, adding pages and integrating new services all start from clean architectural boundaries.

## License

MIT © 2026 FoloToy, see [LICENSE](LICENSE).

Third-party components (LVGL, esp_lvgl_port, NimBLE, cJSON, etc.) are copyright their respective authors.
