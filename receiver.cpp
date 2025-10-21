#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

constexpr int PORT = 7777;

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
        } mouse_move;
        struct
        {
            uint8_t button;
            bool down;
        } mouse_button;
        struct
        {
            uint16_t vkCode;
            bool down;
        } keyboard;
        struct
        {
            int delta;
        } mouse_wheel;
    } data;
};

// ---- globals ----
static std::atomic<LONG> g_accumDx{0}, g_accumDy{0};
static std::atomic<bool> g_movePending{false};
static DWORD g_cursorThreadId = 0;

static inline LONG to_abs(LONG pixel, int origin, int span)
{
    if (span <= 1)
        return 0;
    double norm = (double)(pixel - origin) * 65535.0 / (double)(span - 1);
    if (norm < 0.0)
        norm = 0.0;
    if (norm > 65535.0)
        norm = 65535.0;
    return (LONG)(norm + 0.5);
}

static void send_abs_si(LONG tx, LONG ty)
{
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // clamp to virtual desktop (handles negative origins)
    if (vw > 0)
        tx = std::max<LONG>(vx, std::min<LONG>(tx, vx + vw - 1));
    if (vh > 0)
        ty = std::max<LONG>(vy, std::min<LONG>(ty, vy + vh - 1));

    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = to_abs(tx, vx, vw);
    in.mi.dy = to_abs(ty, vy, vh);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    // assert multiple times to win ordering
    SendInput(1, &in, sizeof(in));
    SendInput(1, &in, sizeof(in));
}

static LRESULT cursor_move_once()
{
    // 1) coalesced deltas
    LONG dx = g_accumDx.exchange(0);
    LONG dy = g_accumDy.exchange(0);
    if (dx == 0 && dy == 0)
        return 0;

    // 2) compute target pixel
    POINT p;
    GetCursorPos(&p);
    LONG tx = p.x + dx;
    LONG ty = p.y + dy;

    // 3) first assert + yield + re-assert (barrier-style)
    send_abs_si(tx, ty);
    Sleep(0);
    send_abs_si(tx, ty);

    // 4) stickiness window ~ 8–12ms (tune 1 frame)
    DWORD end = GetTickCount() + 10;

    while (GetTickCount() < end)
    {
        Sleep(0);            // let others fire first…
        send_abs_si(tx, ty); // …then we land last
    }

    return 0;
}
static DWORD WINAPI CursorThreadProc(LPVOID)
{
    MSG msg;
    g_cursorThreadId = GetCurrentThreadId();

    // bump just this thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // prime the message queue (PeekMessage creates it before PostThreadMessage hits)
    PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (msg.message == WM_USER)
        {
            cursor_move_once();
            g_movePending.store(false, std::memory_order_release);
            if (g_accumDx.load(std::memory_order_relaxed) ||
                g_accumDy.load(std::memory_order_relaxed))
            {
                g_movePending.store(true, std::memory_order_relaxed);
                PostThreadMessage(g_cursorThreadId, WM_USER, 0, 0);
            }
        }
    }
    return 0;
}

std::atomic<bool> g_running(true);

void ProcessPacket(const InputPacket &packet)
{
    switch (packet.type)
    {
    case InputPacket::MOUSE_MOVE:
    {
        g_accumDx.fetch_add(packet.data.mouse_move.x, std::memory_order_relaxed);
        g_accumDy.fetch_add(packet.data.mouse_move.y, std::memory_order_relaxed);
        bool expected = false;
        if (g_movePending.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            // if the cursor thread hasn’t created its queue yet, this fails; mitigate by:
            //  - starting cursor thread before receiver (you already do),
            //  - calling PeekMessage early (we added), or
            //  - retry once if PostThreadMessage returns 0.
            if (!PostThreadMessage(g_cursorThreadId, WM_USER, 0, 0))
            {
                // retry once
                Sleep(0);
                PostThreadMessage(g_cursorThreadId, WM_USER, 0, 0);
            }
        }
        break;
    }

    case InputPacket::MOUSE_BUTTON:
    {
        INPUT input;
        ZeroMemory(&input, sizeof(INPUT));
        input.type = INPUT_MOUSE;
        input.mi.dx = 0;
        input.mi.dy = 0;
        input.mi.mouseData = 0;
        input.mi.time = 0;
        input.mi.dwExtraInfo = 0;
        if (packet.data.mouse_button.button == 0)
        {
            input.mi.dwFlags = packet.data.mouse_button.down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        }
        else if (packet.data.mouse_button.button == 1)
        {
            input.mi.dwFlags = packet.data.mouse_button.down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        }
        else if (packet.data.mouse_button.button == 2)
        {
            input.mi.dwFlags = packet.data.mouse_button.down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        }
        SendInput(1, &input, sizeof(INPUT));
        break;
    }

    case InputPacket::KEYBOARD:
    {
        INPUT input;
        ZeroMemory(&input, sizeof(INPUT));
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = packet.data.keyboard.vkCode;
        input.ki.wScan = 0;
        input.ki.dwFlags = packet.data.keyboard.down ? 0 : KEYEVENTF_KEYUP;
        input.ki.time = 0;
        input.ki.dwExtraInfo = 0;
        SendInput(1, &input, sizeof(INPUT));
        break;
    }

    case InputPacket::MOUSE_WHEEL:
    {
        INPUT input;
        ZeroMemory(&input, sizeof(INPUT));
        input.type = INPUT_MOUSE;
        input.mi.dx = 0;
        input.mi.dy = 0;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = packet.data.mouse_wheel.delta;
        input.mi.time = 0;
        input.mi.dwExtraInfo = 0;
        SendInput(1, &input, sizeof(INPUT));
        break;
    }
    }
}

void ReceiverThread()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed" << std::endl;
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        std::cerr << "Socket creation failed" << std::endl;
        WSACleanup();
        return;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "Bind failed on port " << PORT << std::endl;
        closesocket(sock);
        WSACleanup();
        return;
    }

    std::cout << "Receiver listening on port " << PORT << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    InputPacket packet;
    sockaddr_in senderAddr;
    int senderAddrSize = sizeof(senderAddr);

    while (g_running)
    {
        int received = recvfrom(sock, (char *)&packet, sizeof(packet), 0,
                                (sockaddr *)&senderAddr, &senderAddrSize);

        if (received == sizeof(packet))
        {
            ProcessPacket(packet);
        }
        else if (received == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINTR)
            {
                std::cerr << "recvfrom failed: " << error << std::endl;
            }
        }
    }

    closesocket(sock);
    WSACleanup();
}

BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT)
    {
        std::cout << "\nShutting down..." << std::endl;
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int main()
{
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::cout << "Keyboard/Mouse Receiver Starting..." << std::endl;

    HANDLE hCursorThread = CreateThread(nullptr, 0, CursorThreadProc, nullptr, 0, nullptr);
    std::thread receiverThread(ReceiverThread);
    receiverThread.join();

    return 0;
}