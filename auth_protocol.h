#ifndef AUTH_PROTOCOL_H
#define AUTH_PROTOCOL_H

















#include <cstdint>
#include <optional>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#endif

#define HTTP_API_DEFAULT_PORT 8085
#define AUTH_TOKEN_PREFIX "TOKEN "

namespace AuthProto {


inline bool ClientSideHello(
#ifdef _WIN32
    SOCKET sock,
#else
    int sock,
#endif
    const std::string& token, std::string* errOut = nullptr) {
    if (token.empty()) {
        if (errOut) *errOut = "not logged in";
        return false;
    }
    std::string hello = std::string(AUTH_TOKEN_PREFIX) + token + "\n";
    size_t sent = 0;
    while (sent < hello.size()) {
#ifdef _WIN32
        int n = send(sock, hello.data() + sent, (int)(hello.size() - sent), 0);
#else
        ssize_t n = send(sock, hello.data() + sent, hello.size() - sent, 0);
#endif
        if (n <= 0) {
            if (errOut) *errOut = "send failed";
            return false;
        }
        sent += (size_t)n;
    }

    std::string reply;
    char c = 0;
    while (reply.size() < 256) {
#ifdef _WIN32
        int n = recv(sock, &c, 1, 0);
#else
        ssize_t n = recv(sock, &c, 1, 0);
#endif
        if (n <= 0) {
            if (errOut) *errOut = "no reply from server";
            return false;
        }
        if (c == '\n') break;
        if (c != '\r') reply += c;
    }
    if (reply == "OK") return true;
    if (errOut) {
        if (reply.rfind("ERR", 0) == 0 && reply.size() > 4) *errOut = reply.substr(4);
        else if (!reply.empty()) *errOut = reply;
        else *errOut = "rejected";
    }
    return false;
}

}

#endif
