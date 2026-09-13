
















#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>

#include <iostream>
#include <vector>
#include <array>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <string>
#include <optional>

#include "crypto.h"
#include "database.h"
#include "http_api.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

#define VIDEO_VIEWER_PORT    "8080"
#define VIDEO_BROADCAST_PORT "8082"
#define CHAT_PORT            "8081"

static const size_t RING_BUFFER_SIZE = 30;
static const uint32_t MAX_FRAME_SIZE = 4 * 1024 * 1024;

std::atomic<bool> g_Running{ true };


std::atomic<SOCKET> g_ViewerListenSocket{ INVALID_SOCKET };
std::atomic<SOCKET> g_BroadcastListenSocket{ INVALID_SOCKET };
std::atomic<SOCKET> g_ChatListenSocket{ INVALID_SOCKET };



struct StoredFrame
{
    std::vector<uint8_t> data;
    bool is_key_frame = false;
};

std::array<std::shared_ptr<StoredFrame>, RING_BUFFER_SIZE> g_Ring;
std::atomic<uint64_t> g_RingHead{ 0 };

std::mutex g_RingMutex;
std::condition_variable g_RingCV;



std::atomic<bool> g_BroadcasterActive{ false };
std::atomic<int> g_ViewerCount{ 0 };

std::atomic<uint64_t> g_StatReceivedBytes{ 0 };
std::atomic<uint64_t> g_StatSentBytes{ 0 };

std::atomic<uint32_t> g_StatReceivedFrames{ 0 };
std::atomic<uint32_t> g_StatSentFrames{ 0 };
std::atomic<uint32_t> g_StatViewerDrops{ 0 };



static bool RecvAll(SOCKET sock, void* buf, int len)
{
    char* ptr = static_cast<char*>(buf);
    int received = 0;

    while (received < len)
    {
        int n = recv(sock, ptr + received, len - received, 0);
        if (n <= 0)
            return false;

        received += n;
    }

    return true;
}

static void CloseListenSocket(std::atomic<SOCKET>& sockAtomic)
{
    SOCKET s = sockAtomic.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET)
        closesocket(s);
}

static SOCKET CreateListener(const char* port)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result = nullptr;

    if (getaddrinfo("0.0.0.0", port, &hints, &result) != 0)
    {
        return INVALID_SOCKET;
    }

    SOCKET listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);

    if (listenSocket == INVALID_SOCKET)
    {
        freeaddrinfo(result);
        return INVALID_SOCKET;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));

    if (bind(listenSocket, result->ai_addr, static_cast<int>(result->ai_addrlen)) == SOCKET_ERROR)
    {
        closesocket(listenSocket);
        freeaddrinfo(result);
        return INVALID_SOCKET;
    }

    freeaddrinfo(result);

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        closesocket(listenSocket);
        return INVALID_SOCKET;
    }

    return listenSocket;
}





static bool SendLineRaw(SOCKET s, const std::string& line)
{
    size_t sent = 0;
    while (sent < line.size())
    {
        int n = send(s, line.data() + sent, static_cast<int>(line.size() - sent), 0);
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}


static std::optional<int64_t> ServerAuthHandshake(SOCKET sock, const char* role)
{

    DWORD hsTimeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&hsTimeout), sizeof(hsTimeout));

    char peek[6] = {};
    int pn = recv(sock, peek, 6, MSG_PEEK);
    bool hasTokenPrefix = (pn >= 6 && memcmp(peek, "TOKEN ", 6) == 0);

    std::optional<int64_t> result;

    if (hasTokenPrefix)
    {

        std::string line;
        char c = 0;
        while (line.size() < 512)
        {
            int n = recv(sock, &c, 1, 0);
            if (n <= 0)
                break;
            if (c == '\n')
                break;
            if (c != '\r')
                line += c;
        }

        std::string token;
        if (line.rfind("TOKEN ", 0) == 0)
            token = line.substr(6);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.pop_back();

        auto uid = token.empty() ? std::nullopt : Database::instance().validateSession(token);
        if (uid)
        {
            SendLineRaw(sock, "OK\n");
            std::cout << "[Auth] " << role << " authenticated user " << *uid << std::endl;
            result = uid;
        }
        else
        {
            SendLineRaw(sock, "ERR invalid token\n");
            std::cout << "[Auth] " << role << " rejected: invalid token" << std::endl;
            result = std::nullopt;
        }
    }
    else
    {

        if (Database::instance().userCount() == 0)
        {
            std::cout << "[Auth] " << role << " anonymous (bootstrap, no users yet)" << std::endl;
            result = std::optional<int64_t>(0);
        }
        else
        {
            SendLineRaw(sock, "ERR auth required\n");
            std::cout << "[Auth] " << role << " rejected: auth required" << std::endl;
            result = std::nullopt;
        }
    }


    DWORD noTimeout = 0;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&noTimeout), sizeof(noTimeout));
    return result;
}



static bool ContainsH264IDR(const std::vector<uint8_t>& data)
{
    const uint8_t* p = data.data();
    size_t n = data.size();

    size_t limit = std::min(n, static_cast<size_t>(1024 * 1024));
    size_t i = 0;

    while (i + 3 < limit)
    {

        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
        {
            if (i + 3 < n)
            {
                uint8_t nal_type = p[i + 3] & 0x1F;
                if (nal_type == 5)
                    return true;
            }

            i += 4;
        }

        else if (i + 4 < n &&
            p[i] == 0 &&
            p[i + 1] == 0 &&
            p[i + 2] == 0 &&
            p[i + 3] == 1)
        {
            uint8_t nal_type = p[i + 4] & 0x1F;
            if (nal_type == 5)
                return true;

            i += 5;
        }
        else
        {
            ++i;
        }
    }

    return false;
}



static uint64_t FindLatestKeyTail(bool& needKeyframe)
{
    uint64_t head = g_RingHead.load(std::memory_order_acquire);

    needKeyframe = true;

    if (head == 0)
        return 0;

    uint64_t begin = (head > RING_BUFFER_SIZE) ? (head - RING_BUFFER_SIZE) : 0;

    for (uint64_t idx = head; idx > begin; --idx)
    {
        std::shared_ptr<StoredFrame> frame;

        {
            std::lock_guard<std::mutex> lock(g_RingMutex);
            frame = g_Ring[(idx - 1) % RING_BUFFER_SIZE];
        }

        if (frame && frame->is_key_frame)
        {
            needKeyframe = false;
            return idx - 1;
        }
    }

    return head;
}



void ClientThread(SOCKET clientSocket)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    int flag = 1;
    setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&flag), sizeof(flag));

    DWORD sendTimeout = 5000;
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&sendTimeout), sizeof(sendTimeout));

    int sndbuf = 512 * 1024;
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sndbuf), sizeof(sndbuf));

    g_ViewerCount.fetch_add(1);

    bool needKeyframe = true;
    uint64_t tail = FindLatestKeyTail(needKeyframe);

    std::cout << "[Relay] Viewer connected. Total viewers: " << g_ViewerCount.load() << std::endl;

    while (g_Running.load())
    {
        uint64_t head = g_RingHead.load(std::memory_order_acquire);

        if (tail == head)
        {
            std::unique_lock<std::mutex> lock(g_RingMutex);

            g_RingCV.wait_for(
                lock,
                std::chrono::milliseconds(5),
                [&]
                {
                    return g_RingHead.load(std::memory_order_acquire) != tail || !g_Running.load();
                }
            );

            continue;
        }


        if (head >= tail + RING_BUFFER_SIZE)
        {
            g_StatViewerDrops.fetch_add(static_cast<uint32_t>(head - tail));
            tail = FindLatestKeyTail(needKeyframe);
            continue;
        }

        std::shared_ptr<StoredFrame> frame;

        {
            std::lock_guard<std::mutex> lock(g_RingMutex);
            frame = g_Ring[tail % RING_BUFFER_SIZE];
        }

        if (!frame)
        {
            ++tail;
            continue;
        }

        if (needKeyframe)
        {
            if (!frame->is_key_frame)
            {
                ++tail;
                continue;
            }

            needKeyframe = false;
        }

        uint32_t payloadSize = static_cast<uint32_t>(frame->data.size());

        WSABUF wsaBufs[2];

        wsaBufs[0].buf = reinterpret_cast<char*>(&payloadSize);
        wsaBufs[0].len = 4;

        wsaBufs[1].buf = reinterpret_cast<char*>(frame->data.data());
        wsaBufs[1].len = static_cast<ULONG>(frame->data.size());

        DWORD bytesSent = 0;

        int res = WSASend(clientSocket, wsaBufs, 2, &bytesSent, 0, nullptr, nullptr);

        if (res == SOCKET_ERROR || bytesSent != payloadSize + 4)
        {
            std::cout << "[Relay] Viewer disconnected or send failed." << std::endl;
            break;
        }

        g_StatSentBytes.fetch_add(bytesSent);
        g_StatSentFrames.fetch_add(1);

        ++tail;
    }

    closesocket(clientSocket);

    g_ViewerCount.fetch_sub(1);

    std::cout << "[Relay] Viewer disconnected. Total viewers: " << g_ViewerCount.load() << std::endl;
}



void HandleBroadcaster(SOCKET broadcasterSocket)
{
    bool expected = false;

    if (!g_BroadcasterActive.compare_exchange_strong(expected, true))
    {
        std::cout << "[Relay] Rejected second broadcaster." << std::endl;
        closesocket(broadcasterSocket);
        return;
    }

    int flag = 1;
    setsockopt(broadcasterSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&flag), sizeof(flag));

    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(broadcasterSocket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rcvbuf), sizeof(rcvbuf));

    std::cout << "[Relay] Broadcaster connected." << std::endl;

    while (g_Running.load())
    {
        uint32_t frameSize = 0;

        if (!RecvAll(broadcasterSocket, &frameSize, 4))
        {
            std::cout << "[Relay] Broadcaster disconnected (header)." << std::endl;
            break;
        }

        if (frameSize == 0)
            continue;

        if (frameSize > MAX_FRAME_SIZE)
        {
            std::cout << "[Relay] Broadcaster sent too large frame: " << frameSize << std::endl;
            break;
        }

        auto frame = std::make_shared<StoredFrame>();
        frame->data.resize(frameSize);

        if (!RecvAll(broadcasterSocket, frame->data.data(), static_cast<int>(frameSize)))
        {
            std::cout << "[Relay] Broadcaster disconnected (payload)." << std::endl;
            break;
        }

        frame->is_key_frame = ContainsH264IDR(frame->data);

        uint64_t head = g_RingHead.load(std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(g_RingMutex);
            g_Ring[head % RING_BUFFER_SIZE] = frame;
        }

        g_RingHead.store(head + 1, std::memory_order_release);
        g_RingCV.notify_all();

        g_StatReceivedBytes.fetch_add(frameSize);
        g_StatReceivedFrames.fetch_add(1);
    }

    closesocket(broadcasterSocket);
    g_BroadcasterActive.store(false);

    std::cout << "[Relay] Broadcaster disconnected." << std::endl;
}



void ViewerAcceptThread()
{
    SOCKET listenSocket = CreateListener(VIDEO_VIEWER_PORT);

    if (listenSocket == INVALID_SOCKET)
    {
        std::cerr << "[Relay] Failed to listen on viewer port " << VIDEO_VIEWER_PORT << std::endl;
        return;
    }

    g_ViewerListenSocket.store(listenSocket);

    std::cout << "[Relay] Waiting for viewers on port " << VIDEO_VIEWER_PORT << std::endl;

    while (g_Running.load())
    {
        SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);

        if (clientSocket != INVALID_SOCKET)
        {
            std::thread(ClientThread, clientSocket).detach();
        }
        else
        {
            if (!g_Running.load())
                break;

            Sleep(10);
        }
    }

    CloseListenSocket(g_ViewerListenSocket);
}

void BroadcasterAcceptThread()
{
    SOCKET listenSocket = CreateListener(VIDEO_BROADCAST_PORT);

    if (listenSocket == INVALID_SOCKET)
    {
        std::cerr << "[Relay] Failed to listen on broadcaster port " << VIDEO_BROADCAST_PORT << std::endl;
        return;
    }

    g_BroadcastListenSocket.store(listenSocket);

    std::cout << "[Relay] Waiting for broadcaster on port " << VIDEO_BROADCAST_PORT << std::endl;

    while (g_Running.load())
    {
        SOCKET broadcasterSocket = accept(listenSocket, nullptr, nullptr);

        if (broadcasterSocket != INVALID_SOCKET)
        {
            std::thread(HandleBroadcaster, broadcasterSocket).detach();
        }
        else
        {
            if (!g_Running.load())
                break;

            Sleep(10);
        }
    }

    CloseListenSocket(g_BroadcastListenSocket);
}



struct ChatClientInfo
{
    SOCKET socket;
    std::string username;
};

std::vector<ChatClientInfo> g_ChatClients;
std::mutex g_ChatClientsMutex;

static void ForwardRawToOthers(const char* data, int len, SOCKET senderSocket)
{
    std::vector<SOCKET> targets;

    {
        std::lock_guard<std::mutex> lock(g_ChatClientsMutex);

        for (const auto& client : g_ChatClients)
        {
            if (client.socket != senderSocket)
                targets.push_back(client.socket);
        }
    }

    for (SOCKET sock : targets)
    {
        send(sock, data, len, 0);
    }
}

static void BroadcastChatMessage(const std::string& message, SOCKET senderSocket)
{
    ForwardRawToOthers(message.data(), static_cast<int>(message.size()), senderSocket);
}

void HandleChatClient(SOCKET clientSocket)
{
    int flag = 1;
    setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&flag), sizeof(flag));

    DWORD sendTimeout = 2000;
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&sendTimeout), sizeof(sendTimeout));

    char buffer[16384];


    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

    if (bytesReceived <= 0)
    {
        closesocket(clientSocket);
        return;
    }

    buffer[bytesReceived] = '\0';

    bool firstIsControl =
        bytesReceived > 0 &&
        (
            static_cast<unsigned char>(buffer[0]) == 0x01 ||
            static_cast<unsigned char>(buffer[0]) == 0x02 ||
            static_cast<unsigned char>(buffer[0]) == 0x03
            );

    std::string username;

    if (firstIsControl)
    {
        username = "User";
    }
    else
    {
        username = std::string(buffer, bytesReceived);
    }

    while (!username.empty() &&
        (username.back() == '\n' || username.back() == '\r' || username.back() == ' '))
    {
        username.pop_back();
    }

    if (username.empty())
        username = "User";

    {
        std::lock_guard<std::mutex> lock(g_ChatClientsMutex);
        g_ChatClients.push_back({ clientSocket, username });
    }

    std::cout << "[Chat] " << username << " connected." << std::endl;

    BroadcastChatMessage("\x01[Server] " + username + " joined the room.\n", clientSocket);

    auto ProcessPacket = [&](const char* buf, int len)
        {
            if (len <= 0)
                return;

            unsigned char type = static_cast<unsigned char>(buf[0]);

            if (type == 0x01 && len > 1)
            {
                std::string text(buf + 1, len - 1);

                std::string outMessage = "\x01[" + username + "]: " + text;

                std::cout << "[" << username << "]: " << text << std::endl;

                BroadcastChatMessage(outMessage, clientSocket);
            }
            else if (type == 0x02 || type == 0x03)
            {


                ForwardRawToOthers(buf, len, clientSocket);
            }
        };

    if (firstIsControl)
    {
        ProcessPacket(buffer, bytesReceived);
    }

    while (g_Running.load())
    {
        bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);

        if (bytesReceived <= 0)
        {
            std::cout << "[Chat] " << username << " disconnected." << std::endl;
            break;
        }

        ProcessPacket(buffer, bytesReceived);
    }

    {
        std::lock_guard<std::mutex> lock(g_ChatClientsMutex);

        for (auto it = g_ChatClients.begin(); it != g_ChatClients.end(); ++it)
        {
            if (it->socket == clientSocket)
            {
                g_ChatClients.erase(it);
                break;
            }
        }
    }

    BroadcastChatMessage("\x01[Server] " + username + " left the room.\n", clientSocket);

    closesocket(clientSocket);
}

void ChatServerThread()
{
    SOCKET listenSocket = CreateListener(CHAT_PORT);

    if (listenSocket == INVALID_SOCKET)
    {
        std::cerr << "[Relay] Failed to listen on chat port " << CHAT_PORT << std::endl;
        return;
    }

    g_ChatListenSocket.store(listenSocket);

    std::cout << "[Relay] Waiting for chat/voice clients on port " << CHAT_PORT << std::endl;

    while (g_Running.load())
    {
        SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);

        if (clientSocket != INVALID_SOCKET)
        {
            std::thread(HandleChatClient, clientSocket).detach();
        }
        else
        {
            if (!g_Running.load())
                break;

            Sleep(10);
        }
    }

    CloseListenSocket(g_ChatListenSocket);
}



void StatsThread()
{
    while (g_Running.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t rxBytes = g_StatReceivedBytes.exchange(0);
        uint64_t txBytes = g_StatSentBytes.exchange(0);

        uint32_t rxFps = g_StatReceivedFrames.exchange(0);
        uint32_t txFps = g_StatSentFrames.exchange(0);
        uint32_t drops = g_StatViewerDrops.exchange(0);

        uint64_t rxBps = rxBytes * 8;
        uint64_t txBps = txBytes * 8;

        std::cout
            << "[Stats] Broadcaster: " << (g_BroadcasterActive.load() ? "yes" : "no")
            << " | Viewers: " << g_ViewerCount.load()
            << " | Rx fps: " << rxFps
            << " | Tx fps: " << txFps
            << " | Rx kbps: " << (rxBps / 1000)
            << " | Tx kbps: " << (txBps / 1000)
            << " | Drops: " << drops
            << std::endl;
    }
}



static BOOL WINAPI ConsoleCtrlHandler(DWORD)
{
    g_Running.store(false);

    CloseListenSocket(g_ViewerListenSocket);
    CloseListenSocket(g_BroadcastListenSocket);
    CloseListenSocket(g_ChatListenSocket);

    return TRUE;
}



int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    std::cout << "==============================================" << std::endl;
    std::cout << " Relay Server                                 " << std::endl;
    std::cout << " Viewer port:      " << VIDEO_VIEWER_PORT << std::endl;
    std::cout << " Broadcaster port: " << VIDEO_BROADCAST_PORT << std::endl;
    std::cout << " Chat/voice port:  " << CHAT_PORT << std::endl;
    std::cout << "==============================================" << std::endl;

    std::thread(StatsThread).detach();

    std::thread(BroadcasterAcceptThread).detach();
    std::thread(ViewerAcceptThread).detach();
    std::thread(ChatServerThread).detach();

    while (g_Running.load())
    {
        Sleep(200);
    }

    CloseListenSocket(g_ViewerListenSocket);
    CloseListenSocket(g_BroadcastListenSocket);
    CloseListenSocket(g_ChatListenSocket);

    WSACleanup();
    timeEndPeriod(1);

    return 0;
}