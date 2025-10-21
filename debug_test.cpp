#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")

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

const char* GetPacketTypeName(InputPacket::Type type)
{
    switch(type)
    {
        case InputPacket::MOUSE_MOVE: return "MOUSE_MOVE";
        case InputPacket::MOUSE_BUTTON: return "MOUSE_BUTTON";
        case InputPacket::KEYBOARD: return "KEYBOARD";
        case InputPacket::MOUSE_WHEEL: return "MOUSE_WHEEL";
        default: return "UNKNOWN";
    }
}

const char* GetKeyName(uint16_t vkCode)
{
    static char buffer[32];

    if (vkCode >= 'A' && vkCode <= 'Z')
    {
        sprintf_s(buffer, "%c", (char)vkCode);
        return buffer;
    }
    else if (vkCode >= '0' && vkCode <= '9')
    {
        sprintf_s(buffer, "%c", (char)vkCode);
        return buffer;
    }
    else if (vkCode == VK_SPACE) return "SPACE";
    else if (vkCode == VK_RETURN) return "ENTER";
    else if (vkCode == VK_BACK) return "BACKSPACE";
    else if (vkCode == VK_TAB) return "TAB";
    else if (vkCode == VK_ESCAPE) return "ESC";
    else if (vkCode == VK_SHIFT) return "SHIFT";
    else if (vkCode == VK_CONTROL) return "CTRL";
    else if (vkCode == VK_MENU) return "ALT";
    else
    {
        sprintf_s(buffer, "VK_0x%02X", vkCode);
        return buffer;
    }
}

int main()
{
    std::cout << "Packet Sniffer - Listening on port " << PORT << std::endl;
    std::cout << "This will show all packets received on the network" << std::endl;
    std::cout << "Press Ctrl+C to exit\n" << std::endl;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        std::cerr << "Socket creation failed" << std::endl;
        WSACleanup();
        return 1;
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
        return 1;
    }

    InputPacket packet;
    sockaddr_in senderAddr;
    int senderAddrSize = sizeof(senderAddr);
    int packetCount = 0;

    while (true)
    {
        int received = recvfrom(sock, (char *)&packet, sizeof(packet), 0,
                                (sockaddr *)&senderAddr, &senderAddrSize);

        if (received == sizeof(packet))
        {
            packetCount++;
            char senderIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, senderIP, INET_ADDRSTRLEN);

            std::cout << "[#" << std::setw(4) << packetCount << "] From " << senderIP
                      << " - Type: " << std::setw(12) << GetPacketTypeName(packet.type);

            switch(packet.type)
            {
                case InputPacket::MOUSE_MOVE:
                    std::cout << " - Delta: (" << packet.data.mouse_move.x
                              << ", " << packet.data.mouse_move.y << ")";
                    break;
                case InputPacket::MOUSE_BUTTON:
                    std::cout << " - Button: " << (int)packet.data.mouse_button.button
                              << " " << (packet.data.mouse_button.down ? "DOWN" : "UP");
                    break;
                case InputPacket::KEYBOARD:
                    std::cout << " - Key: " << GetKeyName(packet.data.keyboard.vkCode)
                              << " (" << packet.data.keyboard.vkCode << ") "
                              << (packet.data.keyboard.down ? "DOWN" : "UP");
                    break;
                case InputPacket::MOUSE_WHEEL:
                    std::cout << " - Delta: " << packet.data.mouse_wheel.delta;
                    break;
            }

            std::cout << std::endl;
        }
        else if (received == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINTR)
            {
                std::cerr << "recvfrom failed: " << error << std::endl;
                break;
            }
        }
        else
        {
            std::cout << "Received packet with unexpected size: " << received
                      << " bytes (expected " << sizeof(packet) << ")" << std::endl;
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}