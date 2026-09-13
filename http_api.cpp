#include "http_api.h"
#include "crypto.h"
#include "database.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace {



std::string JsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b;
                } else o += (char)c;
        }
    }
    return o;
}


bool JsonString(const std::string& body, const std::string& key, std::string& out) {
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

bool JsonBool(const std::string& body, const std::string& key, bool def) {
    std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return def;
    p = body.find(':', p + pat.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < body.size() && isspace((unsigned char)body[p])) ++p;
    if (body.compare(p, 4, "true") == 0) return true;
    if (body.compare(p, 5, "false") == 0) return false;
    return def;
}

std::string JsonStrOr(const std::string& body, const std::string& key,
                      const std::string& def = "") {
    std::string v; return JsonString(body, key, v) ? v : def;
}

std::string ToLowerHdr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool RecvAll(SOCKET s, char* buf, int len) {
    int got = 0;
    while (got < len) {
        int n = recv(s, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

bool SendAll(SOCKET s, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

void SendJson(SOCKET conn, int status, const std::string& body) {
    const char* statusText = "OK";
    if (status == 400) statusText = "Bad Request";
    else if (status == 401) statusText = "Unauthorized";
    else if (status == 403) statusText = "Forbidden";
    else if (status == 404) statusText = "Not Found";
    else if (status == 409) statusText = "Conflict";
    else if (status == 500) statusText = "Internal Server Error";
    std::ostringstream h;
    h << "HTTP/1.1 " << status << " " << statusText << "\r\n"
      << "Content-Type: application/json; charset=utf-8\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
      << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
      << "Connection: close\r\n\r\n";
    std::string head = h.str();
    SendAll(conn, head.data(), head.size());
    if (!body.empty()) SendAll(conn, body.data(), body.size());
}

void SendError(SOCKET conn, int status, const std::string& msg) {
    SendJson(conn, status, "{\"error\":\"" + JsonEscape(msg) + "\"}");
}

std::string BearerToken(const std::string& headers) {

    std::string low = ToLowerHdr(headers);
    size_t p = low.find("authorization:");
    if (p == std::string::npos) return "";
    size_t eol = headers.find("\r\n", p);
    std::string line = headers.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
    size_t b = ToLowerHdr(line).find("bearer ");
    if (b == std::string::npos) return "";
    std::string tok = line.substr(b + 7);

    size_t a = 0;
    while (a < tok.size() && isspace((unsigned char)tok[a])) ++a;
    size_t z = tok.size();
    while (z > a && isspace((unsigned char)tok[z - 1])) --z;
    return tok.substr(a, z - a);
}

std::optional<int64_t> RequireAuth(const std::string& headers, SOCKET conn) {
    std::string tok = BearerToken(headers);
    if (tok.empty()) { SendError(conn, 401, "unauthorized"); return std::nullopt; }
    auto uid = Database::instance().validateSession(tok);
    if (!uid) { SendError(conn, 401, "invalid token"); return std::nullopt; }
    return uid;
}

}

HttpApi::HttpApi() = default;

HttpApi::~HttpApi() { stop(); }

bool HttpApi::start(const std::string& host, int port) {
    if (running_.load()) return false;
    running_ = true;
    thread_ = std::thread(&HttpApi::acceptLoop, this, host, port);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return true;
}

void HttpApi::stop() {
    if (!running_.exchange(false)) return;
    SOCKET s = listenSock_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
    if (thread_.joinable()) thread_.join();
}

void HttpApi::acceptLoop(std::string host, int port) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host == "0.0.0.0" ? "0.0.0.0" : host.c_str(),
                    portStr.c_str(), &hints, &res) != 0) {
        std::cerr << "[HTTP] getaddrinfo failed" << std::endl;
        running_ = false;
        return;
    }

    SOCKET ls = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (ls == INVALID_SOCKET) {
        std::cerr << "[HTTP] socket failed" << std::endl;
        freeaddrinfo(res);
        running_ = false;
        return;
    }
    int reuse = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    if (bind(ls, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        std::cerr << "[HTTP] bind " << host << ":" << port
                  << " failed: " << WSAGetLastError() << std::endl;
        closesocket(ls);
        freeaddrinfo(res);
        running_ = false;
        return;
    }
    freeaddrinfo(res);
    if (listen(ls, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[HTTP] listen failed" << std::endl;
        closesocket(ls);
        running_ = false;
        return;
    }
    listenSock_.store(ls);
    std::cout << "[HTTP] Listening on " << host << ":" << port << std::endl;


    while (running_.load()) {
        fd_set rf; FD_ZERO(&rf); FD_SET(ls, &rf);
        timeval tv{0, 200 * 1000};
        int r = select(0, &rf, nullptr, nullptr, &tv);
        if (!running_.load()) break;
        if (r <= 0) continue;
        SOCKET conn = accept(ls, nullptr, nullptr);
        if (conn == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        std::thread(&HttpApi::handleConnection, this, conn).detach();
    }
    SOCKET cur = listenSock_.exchange(INVALID_SOCKET);
    if (cur != INVALID_SOCKET && cur != ls) closesocket(cur);
    closesocket(ls);
    std::cout << "[HTTP] Stopped" << std::endl;
}

void HttpApi::handleConnection(SOCKET conn) {
    DWORD to = 5000;
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
    setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof(to));


    std::string req;
    req.reserve(4096);
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while (req.size() < 256 * 1024) {
        int n = recv(conn, buf, sizeof(buf), 0);
        if (n <= 0) { closesocket(conn); return; }
        req.append(buf, n);
        headerEnd = req.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
    }
    if (headerEnd == std::string::npos) { closesocket(conn); return; }

    std::string head = req.substr(0, headerEnd);
    std::string body = req.substr(headerEnd + 4);


    size_t eol = head.find("\r\n");
    std::string reqLine = head.substr(0, eol);
    std::istringstream rl(reqLine);
    std::string method, target, version;
    rl >> method >> target >> version;
    for (auto& c : method) c = (char)std::toupper((unsigned char)c);


    size_t contentLen = 0;
    {
        std::string low = ToLowerHdr(head);
        size_t p = low.find("content-length:");
        if (p != std::string::npos) {
            size_t e = head.find("\r\n", p);
            std::string v = head.substr(p + 15, e == std::string::npos ? std::string::npos : e - p - 15);
            try { contentLen = (size_t)std::stoul(v); } catch (...) {}
        }
    }
    while (body.size() < contentLen && body.size() < 1024 * 1024) {
        int n = recv(conn, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, n);
    }
    if (body.size() > contentLen) body.resize(contentLen);


    size_t q = target.find('?');
    if (q != std::string::npos) target.resize(q);


    std::string peerIp;
    {
        sockaddr_in a{}; int al = sizeof(a);
        if (getpeername(conn, (sockaddr*)&a, &al) == 0) {
            char tmp[64]; inet_ntop(AF_INET, &a.sin_addr, tmp, sizeof(tmp));
            peerIp = tmp;
        }
    }
    std::string userAgent;
    {
        std::string low = ToLowerHdr(head);
        size_t p = low.find("user-agent:");
        if (p != std::string::npos) {
            size_t e = head.find("\r\n", p);
            userAgent = head.substr(p + 11, e == std::string::npos ? std::string::npos : e - p - 11);

            size_t a = 0;
            while (a < userAgent.size() && isspace((unsigned char)userAgent[a])) ++a;
            size_t z = userAgent.size();
            while (z > a && isspace((unsigned char)userAgent[z-1])) --z;
            userAgent = userAgent.substr(a, z - a);
            if (userAgent.size() > 256) userAgent.resize(256);
        }
    }

    try {
        if (method == "OPTIONS") {
            SendJson(conn, 200, "{}");
        } else if (method == "GET" && target == "/api/health") {
            SendJson(conn, 200, "{\"ok\":true}");
        } else if (method == "POST" && target == "/api/auth/register") {
            std::string username = JsonStrOr(body, "username");
            std::string email = JsonStrOr(body, "email");
            std::string password = JsonStrOr(body, "password");
            std::string displayName = JsonStrOr(body, "displayName", username);
            if (username.empty() || password.empty()) {
                SendError(conn, 400, "username and password required"); closesocket(conn); return;
            }
            if (username.size() < 3 || username.size() > 32) {
                SendError(conn, 400, "username must be 3..32 chars"); closesocket(conn); return;
            }
            if (password.size() < 4) {
                SendError(conn, 400, "password too short (min 4)"); closesocket(conn); return;
            }
            if (Database::instance().findUserByUsername(username)) {
                SendError(conn, 409, "username already exists"); closesocket(conn); return;
            }
            if (!email.empty() && Database::instance().findUserByEmail(email)) {
                SendError(conn, 409, "email already exists"); closesocket(conn); return;
            }
            std::string ph = Crypto::hashPassword(password);
            int64_t uid = Database::instance().createUser(username, email, ph, displayName);
            std::ostringstream o;
            o << "{\"userId\":" << uid << ",\"username\":\"" << JsonEscape(username)
              << "\",\"displayName\":\"" << JsonEscape(displayName.empty() ? username : displayName) << "\"}";
            SendJson(conn, 200, o.str());
        } else if (method == "POST" && target == "/api/auth/login") {
            std::string username = JsonStrOr(body, "username");
            std::string password = JsonStrOr(body, "password");
            bool remember = JsonBool(body, "remember", false);
            if (username.empty() || password.empty()) {
                SendError(conn, 400, "username and password required"); closesocket(conn); return;
            }
            auto user = Database::instance().findUserByUsername(username);
            if (!user || !user->isActive || !Crypto::verifyPassword(user->passwordHash, password)) {
                SendError(conn, 401, "invalid credentials"); closesocket(conn); return;
            }
            std::string token = Database::instance().createSession(user->id, remember, userAgent, peerIp);
            std::ostringstream o;
            o << "{\"token\":\"" << JsonEscape(token) << "\",\"userId\":" << user->id
              << ",\"username\":\"" << JsonEscape(user->username)
              << "\",\"displayName\":\"" << JsonEscape(user->displayName) << "\"}";
            SendJson(conn, 200, o.str());
        } else if (method == "POST" && target == "/api/auth/logout") {
            std::string tok = BearerToken(head);
            if (tok.empty()) { SendError(conn, 401, "unauthorized"); closesocket(conn); return; }
            Database::instance().revokeSession(tok);
            SendJson(conn, 200, "{\"success\":true}");
        } else if (method == "GET" && target == "/api/auth/me") {
            auto uid = RequireAuth(head, conn);
            if (!uid) { closesocket(conn); return; }
            auto user = Database::instance().findUserById(*uid);
            if (!user) { SendError(conn, 401, "invalid token"); closesocket(conn); return; }
            std::ostringstream o;
            o << "{\"userId\":" << user->id << ",\"username\":\"" << JsonEscape(user->username)
              << "\",\"displayName\":\"" << JsonEscape(user->displayName)
              << "\",\"email\":\"" << JsonEscape(user->email) << "\"}";
            SendJson(conn, 200, o.str());
        } else if (method == "GET" && target == "/api/rooms") {
            auto uid = RequireAuth(head, conn);
            if (!uid) { closesocket(conn); return; }
            auto rooms = Database::instance().getUserRooms(*uid);
            std::ostringstream o; o << "[";
            for (size_t i = 0; i < rooms.size(); ++i) {
                if (i) o << ",";
                o << "{\"id\":" << rooms[i].id << ",\"name\":\"" << JsonEscape(rooms[i].name)
                  << "\",\"description\":\"" << JsonEscape(rooms[i].description)
                  << "\",\"createdBy\":" << rooms[i].createdBy
                  << ",\"createdAt\":" << rooms[i].createdAt << "}";
            }
            o << "]";
            SendJson(conn, 200, o.str());
        } else if (method == "POST" && target == "/api/rooms") {
            auto uid = RequireAuth(head, conn);
            if (!uid) { closesocket(conn); return; }
            std::string name = JsonStrOr(body, "name");
            std::string desc = JsonStrOr(body, "description");
            if (name.empty()) { SendError(conn, 400, "name required"); closesocket(conn); return; }
            int64_t id = Database::instance().createRoom(*uid, name, desc);
            std::ostringstream o;
            o << "{\"id\":" << id << ",\"name\":\"" << JsonEscape(name)
              << "\",\"description\":\"" << JsonEscape(desc) << "\"}";
            SendJson(conn, 200, o.str());
        } else if ((method == "POST") && target.rfind("/api/rooms/", 0) == 0) {
            auto uid = RequireAuth(head, conn);
            if (!uid) { closesocket(conn); return; }

            std::string rest = target.substr(strlen("/api/rooms/"));
            size_t slash = rest.find('/');
            if (slash == std::string::npos) { SendError(conn, 404, "not found"); closesocket(conn); return; }
            int64_t roomId = 0;
            try { roomId = std::stoll(rest.substr(0, slash)); } catch (...) {
                SendError(conn, 400, "bad room id"); closesocket(conn); return;
            }
            std::string action = rest.substr(slash + 1);
            auto room = Database::instance().findRoomById(roomId);
            if (!room || !room->isActive) { SendError(conn, 404, "room not found"); closesocket(conn); return; }
            if (action == "join") {
                Database::instance().joinRoom(roomId, *uid);
                SendJson(conn, 200, "{\"success\":true}");
            } else if (action == "leave") {
                Database::instance().leaveRoom(roomId, *uid);
                SendJson(conn, 200, "{\"success\":true}");
            } else { SendError(conn, 404, "not found"); }
        } else if (method == "DELETE" && target.rfind("/api/rooms/", 0) == 0) {
            auto uid = RequireAuth(head, conn);
            if (!uid) { closesocket(conn); return; }
            std::string rest = target.substr(strlen("/api/rooms/"));
            int64_t roomId = 0;
            try { roomId = std::stoll(rest); } catch (...) {
                SendError(conn, 400, "bad room id"); closesocket(conn); return;
            }
            auto room = Database::instance().findRoomById(roomId);
            if (!room || !room->isActive) { SendError(conn, 404, "room not found"); closesocket(conn); return; }
            if (room->createdBy != *uid) { SendError(conn, 403, "only owner can delete"); closesocket(conn); return; }
            Database::instance().deleteRoom(roomId);
            SendJson(conn, 200, "{\"success\":true}");
        } else {
            SendError(conn, 404, "not found");
        }
    } catch (const std::exception& e) {
        SendError(conn, 500, e.what());
    } catch (...) {
        SendError(conn, 500, "internal error");
    }
    closesocket(conn);
}
