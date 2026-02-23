# Agent Guidelines

Ultra-minimal keyboard/mouse sharing over UDP between two PCs. Pure Win32/POSIX — no external dependencies.

## Architecture

- **sender** (runs on the source PC): Captures keyboard via low-level hook, mouse via Raw Input API. Sends relative deltas as UDP packets. Blocks local input while capturing.
- **receiver** (runs on the target PC): Listens for UDP packets and replays input via `SendInput`. Runs windowless (`WinMain`, no console). Uses a dedicated high-priority thread for cursor movement with coalesced deltas and absolute positioning (`MOUSEEVENTF_VIRTUALDESK`).
- **sender_linux**: Linux port of the sender using `/dev/uinput` and X11.

## Key files

| File | Purpose |
|---|---|
| `sender.cpp` | Windows sender — Raw Input mouse, LL keyboard hook, hotkey toggle |
| `receiver.cpp` | Windows receiver — UDP listener, `SendInput` replay, DPI-aware, UIAccess |
| `sender_linux.cpp` | Linux sender |
| `receiver.manifest` | Embedded manifest: `uiAccess="true"` + PerMonitorV2 DPI awareness |
| `receiver.rc` | Resource file that embeds the manifest into receiver.exe |
| `build.bat` | Build script (requires GCC/MinGW) |

## Build

`build.bat` builds both sender and receiver. Requires MinGW with `gcc` and `windres` on PATH. No external libraries — links only against `ws2_32`, `user32`, and standard C++ runtime.

The receiver build embeds `receiver.manifest` via `receiver.rc` → `windres` → object file.

## Protocol

Single `InputPacket` struct sent as raw UDP on port 7777. Four packet types: `MOUSE_MOVE` (relative dx/dy), `MOUSE_BUTTON`, `KEYBOARD`, `MOUSE_WHEEL`. No encryption — not safe on untrusted networks.

## Important constraints

- **Sender performance is sacred.** The sender runs on a competitive gaming PC. When not actively capturing (which is 99% of the time), it must use absolute zero CPU and minimal RAM (<1MB). No polling loops, no timers, no background threads, no periodic network activity. The only thing running idle is a single hotkey listener. Any change to the sender must preserve this — if it adds overhead while inactive, it is unacceptable.
- **No external dependencies.** Everything uses Win32 API or POSIX directly.
- **Receiver must stay windowless.** It uses `WinMain` + `-mwindows`, no console.
- **UIAccess manifest is required.** Without it, `SendInput` cannot reach elevated windows (UAC prompts). The exe must be Authenticode-signed and installed in a trusted location (e.g. Program Files) for UIAccess to take effect.
- **DPI awareness is required.** Without per-monitor DPI awareness, `GetCursorPos`/`GetSystemMetrics`/`SendInput` use different coordinate spaces on multi-monitor setups, causing incorrect cursor positioning.
- **The cursor thread runs at `THREAD_PRIORITY_TIME_CRITICAL`.** This is intentional — it needs to win the race against other programs that might reposition the cursor.
