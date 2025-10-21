# Ultra-Minimal Keyboard/Mouse Switcher

Zero-overhead keyboard/mouse sharing between two Windows PCs.

## Key Features

- **ZERO overhead when not in use** - Only hotkey monitoring runs
- **Ultra-low latency** - Direct UDP packets, no buffering
- **Minimal dependencies** - Pure Win32 API, no external libraries
- **Simple toggle** - Press CTRL+SHIFT+K to toggle

## Architecture

When inactive (99% of the time):
- NO hooks installed (apart from the hotkey's)
- NO network activity
- NO polling loops
- Essentially zero CPU/memory usage

When active (after pressing hotkey):
- Low-level hooks capture input
- Direct UDP transmission to target PC
- Input is blocked on source PC

## Building

```batch
build.bat
```

Creates:
- `sender.exe` - Run on gaming PC
- `receiver.exe` - Run on streaming PC

## Usage

**On Streaming PC:**
```
receiver.exe
```

**On Gaming PC:**
```
sender.exe 192.168.1.100
```
(Replace with actual IP of streaming PC)

## Controls

- **CTRL+SHIFT+K** - Toggle control between PCs

## Network

- Uses port 7777 (UDP)
- Ensure Windows Firewall allows this port
- Both PCs must be on same network

## Performance

- In idle state: ~0% CPU, <1MB RAM (0.7MB when I tested it)
- When active: <1% CPU for input capture/transmission
- Network: ~10KB/s during active mouse movement
- Latency: <1ms on local network


## Notes

This program does NOT encrypt the content of the packets. This means that if you use this inside an unsafe network, this functionally becomes a keylogger that can be potentially sniffed by a malicious attacker on the network. Use at your own risk, if you understand what this does and who it's for.