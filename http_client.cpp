#include "http_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cctype>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace {

HttpResult DoRequest(const std::string& host, int port, const std::string& method,
                     const std::string& target, const std::string& body,
                     const std::string& bearer) {
    HttpResult r;
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        r.error = "DNS/connect failed: " + host;
        return r;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        r.error = "socket() failed";
        return r;
    }
    DWORD to = 6000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof(to));

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        r.error = "connect() failed to " + host + ":" + portStr + " (is relay_server running?)";
        closesocket(s);
        freeaddrinfo(res);
        return r;
    }
    freeaddrinfo(res);

    std::ostringstream req;
    req << method << " " << target << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Connection: close\r\n";
    if (!bearer.empty()) req << "Authorization: Bearer " << bearer << "\r\n";
    req << "Content-Length: " << body.size() << "\r\n\r\n";
    std::string head = req.str();
    auto sendAll = [&](const char* p, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            int k = send(s, p + sent, (int)(n - sent), 0);
            if (k <= 0) return false;
            sent += (size_t)k;
        }
        return true;
    };
    if (!sendAll(head.data(), head.size()) || (!body.empty() && !sendAll(body.data(), body.size()))) {
        r.error = "send failed";
        closesocket(s);
        return r;
    }

    std::string resp;
    char buf[8192];
    while (true) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, n);
        if (resp.size() > 4 * 1024 * 1024) break;
    }
    closesocket(s);

    size_t eol = resp.find("\r\n");
    if (eol == std::string::npos) { r.error = "bad HTTP response"; return r; }

    size_t sp1 = resp.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : resp.find(' ', sp1 + 1);
    if (sp1 == std::string::npos) { r.error = "bad status line"; return r; }
    try {
        r.status = std::stoi(resp.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1));
    } catch (...) { r.error = "bad status code"; return r; }
    size_t hb = resp.find("\r\n\r\n");
    r.body = hb == std::string::npos ? "" : resp.substr(hb + 4);
    r.ok = true;
    if (r.status < 200 || r.status >= 300) {
        r.error = HttpJsonError(r.body);
        if (r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    }
    return r;
}

}

HttpResult HttpPost(const std::string& host, int port, const std::string& target,
                    const std::string& jsonBody, const std::string& bearer) {
    return DoRequest(host, port, "POST", target, jsonBody, bearer);
}

HttpResult HttpGet(const std::string& host, int port, const std::string& target,
                   const std::string& bearer) {
    return DoRequest(host, port, "GET", target, "", bearer);
}

HttpResult HttpDelete(const std::string& host, int port, const std::string& target,
                      const std::string& bearer) {
    return DoRequest(host, port, "DELETE", target, "", bearer);
}

bool HttpJsonString(const std::string& body, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return false;
    p = body.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < body.size() && isspace((unsigned char)body[p])) ++p;
    if (p >= body.size() || body[p] != '"') return false;
    ++p;
    std::string v;
    while (p < body.size()) {
        char c = body[p];
        if (c == '\\' && p + 1 < body.size()) {
            char n = body[p + 1];
            if (n == '"') v += '"';
            else if (n == '\\') v += '\\';
            else if (n == 'n') v += '\n';
            else if (n == 'r') v += '\r';
            else if (n == 't') v += '\t';
            else if (n == '/') v += '/';
            else v += n;
            p += 2;
        } else if (c == '"') { out = v; return true; }
        else { v += c; ++p; }
    }
    return false;
}

int64_t HttpJsonInt(const std::string& body, const std::string& key, int64_t def) {
    std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return def;
    p = body.find(':', p + pat.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < body.size() && isspace((unsigned char)body[p])) ++p;
    size_t e = p;
    if (e < body.size() && (body[e] == '-' || body[e] == '+')) ++e;
    while (e < body.size() && isdigit((unsigned char)body[e])) ++e;
    if (e == p) return def;
    try { return std::stoll(body.substr(p, e - p)); } catch (...) { return def; }
}

std::string HttpJsonError(const std::string& body) {
    std::string e;
    if (HttpJsonString(body, "error", e)) return e;
    return "";
}
