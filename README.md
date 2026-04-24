# Ultra-Minimal Keyboard/Mouse Switcher

Zero-overhead keyboard/mouse sharing between two Windows PCs.

## Key Features

- **ZERO overhead when not in use** - Only hotkey monitoring runs
- **Ultra-low latency** - Direct UDP packets, no buffering
- **Clipboard sync** - Text and images synced automatically on toggle (up to 1GB)
- **Multi-monitor support** - Per-monitor DPI-aware, works across any monitor layout
- **UIAccess enabled** - Mouse works even through UAC elevation prompts
- **Minimal dependencies** - Pure Win32 API, no external libraries
- **Configurable hotkey** - Default ALT+1, configurable via CLI or config file

## Architecture

When inactive (99% of the time):
- NO hooks installed (apart from the hotkey's)
- NO Raw Input registration
- NO capture/clipboard worker threads
- NO network activity
- NO polling loops
- Essentially zero CPU/memory usage

When active (after pressing hotkey):
- Low-level hook captures keyboard input
- Raw Input API captures mouse movement/buttons/wheel
- Low-level mouse hook only blocks local mouse input
- Direct UDP transmission to target PC
- Input is blocked on source PC
- Clipboard is synced via short-lived TCP only at toggle boundaries

## Building

Requires MinGW with `gcc` and `windres` on PATH.

```batch
build.bat
```

Creates:
- `sender.exe` - Run on gaming PC
- `receiver.exe` - Run on streaming PC (windowless)

## Usage

**On Streaming PC:**
```
receiver.exe
```
The receiver runs silently with no console window.

**On Gaming PC:**
```
sender.exe 192.168.1.100
sender.exe 192.168.1.100 --hotkey=ALT+2
sender.exe 192.168.1.100 --hotkey=CTRL+SHIFT+K
```
(Replace with actual IP of streaming PC)

Clipboard sync runs only at toggle boundaries: sender clipboard is pushed when capture turns on, and receiver clipboard is pulled back when capture turns off. There is no clipboard monitor, persistent TCP socket, or clipboard/network activity while idle.

You can also set the hotkey in a `sender.cfg` file next to the exe:
```
HOTKEY=ALT+1
```

## Controls

- **ALT+1** (default) - Toggle control between PCs
- `--hotkey=ALT+C` - Example custom toggle hotkey

## Network

- Port 7777 (UDP) - keyboard/mouse input
- Port 7778 (TCP) - boundary clipboard sync
- Ensure Windows Firewall allows both ports
- Both PCs must be on same network

## Deployment (receiver)

For the receiver to work through UAC prompts, it needs UIAccess which requires:
1. The embedded manifest (`receiver.manifest`) with `uiAccess="true"`
2. The exe to be Authenticode-signed (self-signed works)
3. Installed in a trusted location (e.g. `C:\Program Files\KMReceiver\`)

The included `deploy_receiver.ps1` automates this: creates a self-signed cert, signs the exe, copies to Program Files, and sets up a startup shortcut.

## Performance

- In idle state: ~0% CPU, <1MB RAM (0.7MB when I tested it)
- When active: <1% CPU for input capture/transmission
- Network: ~10KB/s during active mouse movement
- Latency: <1ms on local network

## Notes

This program does NOT encrypt the content of the packets. This means that if you use this inside an unsafe network, this functionally becomes a keylogger that can be potentially sniffed by a malicious attacker on the network. Use at your own risk, if you understand what this does and who it's for.

## License

This project is licensed under the [PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0/).

You may use, modify, and share this code for personal or educational
(non-commercial) purposes.
Commercial use of any kind requires my explicit permission.
