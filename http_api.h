#ifndef HTTP_API_H
#define HTTP_API_H














#include <atomic>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#endif

class HttpApi {
public:
    HttpApi();
    ~HttpApi();

    HttpApi(const HttpApi&) = delete;
    HttpApi& operator=(const HttpApi&) = delete;

    bool start(const std::string& host, int port);
    void stop();
    bool isRunning() const { return running_.load(); }

private:
    void acceptLoop(std::string host, int port);
    void handleConnection(
#ifdef _WIN32
        SOCKET conn
#else
        int conn
#endif
    );

    std::thread thread_;
    std::atomic<bool> running_{false};
#ifdef _WIN32
    std::atomic<SOCKET> listenSock_{INVALID_SOCKET};
#else
    std::atomic<int> listenSock_{-1};
#endif
};

#endif
