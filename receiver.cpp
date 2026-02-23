#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <vector>

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

// ------------------------- clipboard protocol -------------
static constexpr int CLIP_PORT = 7778;
static constexpr uint32_t CLIP_MAX_SIZE = 1024 * 1024 * 1024; // 1 GB

#pragma pack(push, 1)
struct ClipHeader
{
    uint8_t msg_type;      // 0=CLIP_DATA, 1=CLIP_REQUEST
    uint8_t content_type;  // 0=empty, 1=text(UTF-16LE), 2=image(DIB)
    uint32_t data_length;  // followed by this many bytes
};
#pragma pack(pop)

static constexpr uint8_t CLIP_MSG_DATA = 0;
static constexpr uint8_t CLIP_MSG_REQUEST = 1;
static constexpr uint8_t CLIP_CONTENT_EMPTY = 0;
static constexpr uint8_t CLIP_CONTENT_TEXT = 1;
static constexpr uint8_t CLIP_CONTENT_IMAGE = 2;

static bool TcpSendAll(SOCKET s, const char *buf, int len)
{
    while (len > 0)
    {
        int sent = send(s, buf, len, 0);
        if (sent <= 0) return false;
        buf += sent;
        len -= sent;
    }
    return true;
}

static bool TcpRecvAll(SOCKET s, char *buf, int len)
{
    while (len > 0)
    {
        int got = recv(s, buf, len, 0);
        if (got <= 0) return false;
        buf += got;
        len -= got;
    }
    return true;
}

static void ReadClipboard(uint8_t &content_type, std::vector<char> &data)
{
    content_type = CLIP_CONTENT_EMPTY;
    data.clear();

    if (!OpenClipboard(nullptr))
        return;

    // try image first (CF_DIB)
    HANDLE hDib = GetClipboardData(CF_DIB);
    if (hDib)
    {
        SIZE_T sz = GlobalSize(hDib);
        if (sz > 0 && sz <= CLIP_MAX_SIZE)
        {
            void *ptr = GlobalLock(hDib);
            if (ptr)
            {
                data.assign(static_cast<char *>(ptr), static_cast<char *>(ptr) + sz);
                GlobalUnlock(hDib);
                content_type = CLIP_CONTENT_IMAGE;
                CloseClipboard();
                return;
            }
        }
    }

    // fall back to text (CF_UNICODETEXT)
    HANDLE hText = GetClipboardData(CF_UNICODETEXT);
    if (hText)
    {
        SIZE_T sz = GlobalSize(hText);
        if (sz > 0 && sz <= CLIP_MAX_SIZE)
        {
            void *ptr = GlobalLock(hText);
            if (ptr)
            {
                data.assign(static_cast<char *>(ptr), static_cast<char *>(ptr) + sz);
                GlobalUnlock(hText);
                content_type = CLIP_CONTENT_TEXT;
            }
        }
    }

    CloseClipboard();
}

static void WriteClipboard(uint8_t content_type, const std::vector<char> &data)
{
    if (content_type == CLIP_CONTENT_EMPTY || data.empty())
        return;

    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();

    UINT fmt = (content_type == CLIP_CONTENT_IMAGE) ? CF_DIB : CF_UNICODETEXT;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (hMem)
    {
        void *ptr = GlobalLock(hMem);
        if (ptr)
        {
            memcpy(ptr, data.data(), data.size());
            GlobalUnlock(hMem);
            SetClipboardData(fmt, hMem);
        }
        else
        {
            GlobalFree(hMem);
        }
    }

    CloseClipboard();
}

std::atomic<bool> g_running(true);
static SOCKET g_clipListenSock = INVALID_SOCKET;

static void ClipboardThread()
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET)
        return;

    // allow quick restart
    int optval = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char *>(&optval), sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(CLIP_PORT);

    if (bind(listenSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(listenSock);
        return;
    }

    if (listen(listenSock, 1) == SOCKET_ERROR)
    {
        closesocket(listenSock);
        return;
    }

    g_clipListenSock = listenSock;

    while (g_running)
    {
        // use select so we can check g_running periodically
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listenSock, &rset);
        timeval tv{1, 0}; // 1s timeout

        int sel = select(0, &rset, nullptr, nullptr, &tv);
        if (sel <= 0)
            continue;

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;

        // serve this connection until it closes (use select to avoid
        // blocking forever so we can check g_running periodically)
        while (g_running)
        {
            fd_set cset;
            FD_ZERO(&cset);
            FD_SET(client, &cset);
            timeval ctv{2, 0};

            int csel = select(0, &cset, nullptr, nullptr, &ctv);
            if (csel < 0) break;   // error
            if (csel == 0) continue; // timeout — check g_running and wait again

            ClipHeader hdr{};
            if (!TcpRecvAll(client, reinterpret_cast<char *>(&hdr), sizeof(hdr)))
                break;

            if (hdr.msg_type == CLIP_MSG_DATA)
            {
                // sender is pushing its clipboard to us
                if (hdr.data_length > CLIP_MAX_SIZE)
                    break;

                std::vector<char> clipData(hdr.data_length);
                if (hdr.data_length > 0 && !TcpRecvAll(client, clipData.data(), (int)hdr.data_length))
                    break;

                WriteClipboard(hdr.content_type, clipData);
            }
            else if (hdr.msg_type == CLIP_MSG_REQUEST)
            {
                // sender wants our clipboard
                uint8_t ct;
                std::vector<char> clipData;
                ReadClipboard(ct, clipData);

                ClipHeader resp{};
                resp.msg_type = CLIP_MSG_DATA;
                resp.content_type = ct;
                resp.data_length = (uint32_t)clipData.size();

                if (!TcpSendAll(client, reinterpret_cast<char *>(&resp), sizeof(resp)))
                    break;
                if (!clipData.empty() && !TcpSendAll(client, clipData.data(), (int)clipData.size()))
                    break;
            }
            else
            {
                break; // unknown message
            }
        }

        closesocket(client);
    }

    closesocket(listenSock);
    g_clipListenSock = INVALID_SOCKET;
}

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
        else if (packet.data.mouse_button.button == 3)
        {
            input.mi.dwFlags = packet.data.mouse_button.down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = XBUTTON1;
        }
        else if (packet.data.mouse_button.button == 4)
        {
            input.mi.dwFlags = packet.data.mouse_button.down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = XBUTTON2;
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
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
        return;

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    { closesocket(sock); WSACleanup(); return; }

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
                break;
        }
    }

    closesocket(sock);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // Per-monitor DPI awareness: ensures GetCursorPos, GetSystemMetrics, and
    // SendInput all use the same physical coordinate space across monitors.
    // Without this, coordinates are DPI-virtualized and multi-monitor absolute
    // positioning breaks.
    {
        // Try Per-Monitor V2 first (Win10 1703+), fall back gracefully.
        using PFN = BOOL(WINAPI *)(HANDLE);
        auto fn = reinterpret_cast<PFN>(
            GetProcAddress(GetModuleHandleA("user32.dll"),
                           "SetProcessDpiAwarenessContext"));
        if (fn)
            fn(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        else
            SetProcessDPIAware(); // Vista+ fallback (system-DPI aware)
    }

    // Initialize Winsock before any threads that need sockets
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return 1;

    HANDLE hCursorThread = CreateThread(nullptr, 0, CursorThreadProc, nullptr, 0, nullptr);

    // start clipboard sync thread
    std::thread clipThread(ClipboardThread);
    clipThread.detach();

    std::thread receiverThread(ReceiverThread);
    receiverThread.join();

    // cleanup
    g_running = false;
    if (g_clipListenSock != INVALID_SOCKET)
        closesocket(g_clipListenSock);

    return 0;
}