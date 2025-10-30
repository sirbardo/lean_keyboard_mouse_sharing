#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <atomic>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

// ------------------------- config -------------------------
static constexpr int PORT = 7777;
static constexpr int HOTKEY_ID = 1;

// hotkey configuration (defaults to Alt+1)
struct HotkeyConfig {
    UINT modifiers = MOD_ALT;
    UINT key = '1';
    std::wstring description = L"ALT+1";
};
static HotkeyConfig g_hotkey;

// ------------------------- protocol -----------------------
struct InputPacket
{
    enum Type : uint8_t
    {
        MOUSE_MOVE,
        MOUSE_BUTTON,
        KEYBOARD,
        MOUSE_WHEEL
    } type;
    union
    {
        struct
        {
            int x, y;
        } mouse_move; // relative dx,dy
        struct
        {
            uint8_t button;
            bool down;
        } mouse_button; // 0=L,1=R,2=M
        struct
        {
            uint16_t vkCode;
            bool down;
        } keyboard; // virtual key
        struct
        {
            int delta;
        } mouse_wheel; // +120/-120
    } data;
};

// ------------------------- globals ------------------------
static std::atomic<bool> g_capturing{false};
static std::atomic<bool> is_shift_pressed_down{false};
static std::atomic<bool> is_ctrl_pressed_down{false};
static std::atomic<bool> is_alt_pressed_down{false};
static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_recvAddr{};
static HWND g_msgWnd = nullptr;
constexpr UINT WM_APP_TOGGLE = WM_APP + 1; // used by LL hook to toggle when capturing

// hooks used only to BLOCK local input while capturing
static HHOOK g_mouseHook = nullptr;
static HHOOK g_keyHook = nullptr;

// whether we currently have the cursor clipped
static bool g_cursorClipped = false;

// ------------------------- net ----------------------------
static inline void SendPacket(const InputPacket &p)
{
    sendto(g_sock, reinterpret_cast<const char *>(&p), sizeof(p), 0,
           reinterpret_cast<const sockaddr *>(&g_recvAddr), sizeof(g_recvAddr));
}

// ------------------------- hotkey parsing -----------------
static bool ParseHotkey(const std::wstring& hotkeyStr, HotkeyConfig& config)
{
    std::wstring str = hotkeyStr;
    // convert to uppercase for case-insensitive parsing
    std::transform(str.begin(), str.end(), str.begin(), ::towupper);

    UINT mods = 0;
    UINT key = 0;

    // parse modifiers
    if (str.find(L"CTRL") != std::wstring::npos || str.find(L"CONTROL") != std::wstring::npos)
        mods |= MOD_CONTROL;
    if (str.find(L"SHIFT") != std::wstring::npos)
        mods |= MOD_SHIFT;
    if (str.find(L"ALT") != std::wstring::npos)
        mods |= MOD_ALT;
    if (str.find(L"WIN") != std::wstring::npos)
        mods |= MOD_WIN;

    // find the key (last character that's not '+')
    size_t lastPlus = str.rfind(L'+');
    if (lastPlus != std::wstring::npos && lastPlus < str.length() - 1)
    {
        std::wstring keyStr = str.substr(lastPlus + 1);
        // trim whitespace
        keyStr.erase(0, keyStr.find_first_not_of(L" \t"));
        keyStr.erase(keyStr.find_last_not_of(L" \t") + 1);

        if (keyStr.length() == 1)
        {
            wchar_t c = keyStr[0];
            if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z'))
            {
                key = (UINT)c;
            }
        }
        else if (keyStr.length() == 2 && keyStr[0] == L'F' && keyStr[1] >= L'1' && keyStr[1] <= L'9')
        {
            // F1-F9
            key = VK_F1 + (keyStr[1] - L'1');
        }
        else if (keyStr.length() == 3 && keyStr[0] == L'F' && keyStr[1] == L'1' && keyStr[2] >= L'0' && keyStr[2] <= L'2')
        {
            // F10-F12
            key = VK_F10 + (keyStr[2] - L'0');
        }
    }

    if (key == 0)
        return false;

    config.modifiers = mods;
    config.key = key;
    config.description = hotkeyStr;
    return true;
}

static void ReadConfigFile(HotkeyConfig& config)
{
    std::wifstream file(L"sender.cfg");
    if (!file.is_open())
        return;

    std::wstring line;
    while (std::getline(file, line))
    {
        // skip empty lines and comments
        if (line.empty() || line[0] == L'#' || line[0] == L';')
            continue;

        // look for HOTKEY=value
        size_t eqPos = line.find(L'=');
        if (eqPos != std::wstring::npos)
        {
            std::wstring key = line.substr(0, eqPos);
            std::wstring value = line.substr(eqPos + 1);

            // trim whitespace
            key.erase(0, key.find_first_not_of(L" \t"));
            key.erase(key.find_last_not_of(L" \t") + 1);
            value.erase(0, value.find_first_not_of(L" \t"));
            value.erase(value.find_last_not_of(L" \t") + 1);

            std::transform(key.begin(), key.end(), key.begin(), ::towupper);

            if (key == L"HOTKEY")
            {
                if (ParseHotkey(value, config))
                {
                    std::wcout << L"Config file: Using hotkey " << config.description << L"\n";
                }
                break;
            }
        }
    }
}

static bool IsHotkeyPressed(DWORD vk)
{
    // check key matches
    if (vk != g_hotkey.key)
        return false;

    // check that required modifiers are pressed (using tracked state)
    if (g_hotkey.modifiers & MOD_CONTROL)
        if (!is_ctrl_pressed_down.load())
            return false;

    if (g_hotkey.modifiers & MOD_SHIFT)
        if (!is_shift_pressed_down.load())
            return false;

    if (g_hotkey.modifiers & MOD_ALT)
        if (!is_alt_pressed_down.load())
            return false;

    return true;
}

static LRESULT CALLBACK LLKbdHook(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    if (!g_capturing.load(std::memory_order_relaxed))
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const KBDLLHOOKSTRUCT *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const DWORD vk = k->vkCode;

    // track ctrl/shift/alt state
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)
        is_shift_pressed_down.store(isDown, std::memory_order_relaxed);
    else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
        is_ctrl_pressed_down.store(isDown, std::memory_order_relaxed);
    else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)
        is_alt_pressed_down.store(isDown, std::memory_order_relaxed);

    // dynamic hotkey toggle
    if (isDown && IsHotkeyPressed(vk))
    {
        PostMessageW(g_msgWnd, WM_APP_TOGGLE, 0, 0);
        return 1;
    }

    // Send keyboard packet directly from the hook since RIDEV_INPUTSINK doesn't work for keyboards
    // Note: this WOULD be better over Raw Input, but no matter what, I could NOT get the Raw Input messages for the keyboard to fire.
    // So for now, LL hook it is.
    InputPacket p{};
    p.type = InputPacket::KEYBOARD;
    p.data.keyboard.vkCode = (uint16_t)vk;
    p.data.keyboard.down = isDown;
    SendPacket(p);

    // block locally
    return 1;
}

static LRESULT CALLBACK LLMouHook(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    if (!g_capturing.load(std::memory_order_relaxed))
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    // swallow all local mouse events during capture (cursor won't move / no clicks locally)
    return 1;
}

// ------------------------- capture on/off -----------------
static void ClipCursor1x1AtCurrent(bool enable)
{
    if (enable && !g_cursorClipped)
    {
        POINT c;
        GetCursorPos(&c);
        RECT r{c.x, c.y, c.x + 1, c.y + 1};
        ClipCursor(&r);
        g_cursorClipped = true;
    }
    else if (!enable && g_cursorClipped)
    {
        ClipCursor(nullptr);
        g_cursorClipped = false;
    }
}

static void StartCapture()
{
    if (g_capturing.exchange(true))
        return;

    RAWINPUTDEVICE rid[1]{};
    // mouse
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = g_msgWnd;

    if (!RegisterRawInputDevices(rid, 1, sizeof(RAWINPUTDEVICE)))
    {
        std::wcerr << L"RegisterRawInputDevices failed: " << GetLastError() << L"\n";
        g_capturing = false;
        return;
    }

    // to make sure that ctrl, shift, and alt don't get stuck down when toggling capture while holding them
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);

    // install LL hooks (eat local input)
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LLMouHook, GetModuleHandleW(nullptr), 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKbdHook, GetModuleHandleW(nullptr), 0);

    // optional: pin the cursor so there's no flicker
    ClipCursor1x1AtCurrent(true);

    std::wcout << L"capture: ON (local input is blocked)\n";
}

static void StopCapture()
{
    if (!g_capturing.exchange(false))
        return;

    // Send key-up events for Ctrl, Shift, and Alt to the receiver before disconnecting
    // This ensures they don't get stuck down on the remote machine
    if (is_ctrl_pressed_down.load())
    {
        InputPacket p{};
        p.type = InputPacket::KEYBOARD;
        p.data.keyboard.vkCode = VK_CONTROL;
        p.data.keyboard.down = false;
        SendPacket(p);
        is_ctrl_pressed_down = false;
    }
    if (is_shift_pressed_down.load())
    {
        InputPacket p{};
        p.type = InputPacket::KEYBOARD;
        p.data.keyboard.vkCode = VK_SHIFT;
        p.data.keyboard.down = false;
        SendPacket(p);
        is_shift_pressed_down = false;
    }
    if (is_alt_pressed_down.load())
    {
        InputPacket p{};
        p.type = InputPacket::KEYBOARD;
        p.data.keyboard.vkCode = VK_MENU;
        p.data.keyboard.down = false;
        SendPacket(p);
        is_alt_pressed_down = false;
    }

    // remove LL hooks
    if (g_mouseHook)
    {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_keyHook)
    {
        UnhookWindowsHookEx(g_keyHook);
        g_keyHook = nullptr;
    }

    RAWINPUTDEVICE rid[1]{};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02; // mouse
    rid[0].dwFlags = RIDEV_REMOVE;
    rid[0].hwndTarget = nullptr;

    RegisterRawInputDevices(rid, 1, sizeof(RAWINPUTDEVICE));

    ClipCursor1x1AtCurrent(false);

    std::wcout << L"capture: OFF (local input restored)\n";
}

// ------------------------- window proc --------------------
static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_APP_TOGGLE:
        if (g_capturing.load())
            StopCapture();
        else
            StartCapture();
        break;
    case WM_INPUT:
    {
        if (!g_capturing.load(std::memory_order_relaxed))
            break;

        UINT size = 0;
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0)
            break;

        BYTE buf[1024];
        if (size > sizeof(buf))
            break;
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size)
            break;

        RAWINPUT *ri = reinterpret_cast<RAWINPUT *>(buf);

        if (ri->header.dwType == RIM_TYPEMOUSE)
        {
            const RAWMOUSE &m = ri->data.mouse;

            // relative motion only
            if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE))
            {
                int dx = (int)m.lLastX;
                int dy = (int)m.lLastY;
                if ((dx | dy) != 0)
                {
                    InputPacket p{};
                    p.type = InputPacket::MOUSE_MOVE;
                    p.data.mouse_move.x = dx;
                    p.data.mouse_move.y = dy;
                    SendPacket(p);
                }
            }

            // buttons
            const USHORT f = m.usButtonFlags;
            if (f & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP))
            {
                InputPacket p{};
                p.type = InputPacket::MOUSE_BUTTON;
                p.data.mouse_button.button = 0;
                p.data.mouse_button.down = (f & RI_MOUSE_LEFT_BUTTON_DOWN) != 0;
                SendPacket(p);
            }
            if (f & (RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP))
            {
                InputPacket p{};
                p.type = InputPacket::MOUSE_BUTTON;
                p.data.mouse_button.button = 1;
                p.data.mouse_button.down = (f & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0;
                SendPacket(p);
            }
            if (f & (RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_UP))
            {
                InputPacket p{};
                p.type = InputPacket::MOUSE_BUTTON;
                p.data.mouse_button.button = 2;
                p.data.mouse_button.down = (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0;
                SendPacket(p);
            }

            // wheel
            if (f & RI_MOUSE_WHEEL)
            {
                SHORT delta = (SHORT)m.usButtonData;
                InputPacket p{};
                p.type = InputPacket::MOUSE_WHEEL;
                p.data.mouse_wheel.delta = (int)delta;
                SendPacket(p);
            }
        }
        break;
    }

    case WM_HOTKEY:
        if (wParam == HOTKEY_ID)
        {
            if (g_capturing.load())
                StopCapture();
            else
                StartCapture();
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ------------------------- entry --------------------------
int wmain(int argc, wchar_t *argv[])
{
    if (argc < 2)
    {
        std::wcout << L"usage: sender.exe <receiver_ip> [--hotkey=ALT+1]\n"
                      L"example: sender.exe 192.168.1.100\n"
                      L"         sender.exe 192.168.1.100 --hotkey=ALT+2\n"
                      L"         sender.exe 192.168.1.100 --hotkey=CTRL+SHIFT+K\n";
        return 1;
    }

    // read config file for default hotkey
    ReadConfigFile(g_hotkey);

    // parse CLI arguments for hotkey override
    for (int i = 2; i < argc; i++)
    {
        std::wstring arg = argv[i];
        if (arg.find(L"--hotkey=") == 0)
        {
            std::wstring hotkeyStr = arg.substr(9);
            HotkeyConfig tempConfig;
            if (ParseHotkey(hotkeyStr, tempConfig))
            {
                g_hotkey = tempConfig;
                std::wcout << L"CLI override: Using hotkey " << g_hotkey.description << L"\n";
            }
            else
            {
                std::wcerr << L"Invalid hotkey format: " << hotkeyStr << L"\n";
                return 1;
            }
        }
    }

    // init sockets
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET)
    {
        std::cerr << "socket() failed\n";
        WSACleanup();
        return 1;
    }
    g_recvAddr.sin_family = AF_INET;
    g_recvAddr.sin_port = htons(PORT);
    {
        char ipA[64];
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, ipA, 64, nullptr, nullptr);
        if (inet_pton(AF_INET, ipA, &g_recvAddr.sin_addr) <= 0)
        {
            std::cerr << "invalid ip\n";
            closesocket(g_sock);
            WSACleanup();
            return 1;
        }
    }

    // window class + hidden message-only window
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KMRawInputSenderWnd";
    if (!RegisterClassExW(&wc))
    {
        std::cerr << "RegisterClassExW failed\n";
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    g_msgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_msgWnd)
    {
        std::cerr << "CreateWindowExW failed\n";
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    // register dynamic hotkey (toggle capture)
    if (!RegisterHotKey(g_msgWnd, HOTKEY_ID, g_hotkey.modifiers, g_hotkey.key))
    {
        std::wcerr << L"RegisterHotKey failed (hotkey may be in use by another application)\n";
        DestroyWindow(g_msgWnd);
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    std::wcout << L"sender running → target " << argv[1] << L":" << PORT << L"\n";
    std::wcout << L"press " << g_hotkey.description << L" to toggle capture (blocks local input when ON)\n";

    // message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // cleanup
    UnregisterHotKey(g_msgWnd, HOTKEY_ID);
    StopCapture(); // ensures hooks uninstalled, raw input unregistered, cursor unclipped
    if (g_msgWnd)
        DestroyWindow(g_msgWnd);
    if (g_sock != INVALID_SOCKET)
        closesocket(g_sock);
    WSACleanup();
    return 0;
}
