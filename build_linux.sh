#!/bin/bash
echo "Building Linux Sender for Keyboard/Mouse Switcher..."

# Check for required dependencies
if ! pkg-config --exists x11 2>/dev/null; then
    echo "Warning: libX11 development files not found."
    echo "Install with: sudo pacman -S libx11  (Arch) or sudo apt install libx11-dev (Debian/Ubuntu)"
fi

# Build the Linux sender
g++ -O3 -o sender_linux sender_linux.cpp -lX11 -lpthread 2>&1

if [ $? -ne 0 ]; then
    echo "Failed to build sender_linux"
    exit 1
fi

echo ""
echo "Build successful!"
echo ""
echo "File created:"
echo "  - sender_linux (run on Linux PC to control remote Windows receiver)"
echo ""
echo "IMPORTANT: You need permission to read /dev/input/* devices."
echo "Either run as root or add your user to the 'input' group:"
echo "  sudo usermod -a -G input \$USER"
echo "  (then log out and back in)"
echo ""
echo "Usage: ./sender_linux <receiver_ip> [--hotkey=ALT+1]"
echo "Example: ./sender_linux 192.168.1.100"
