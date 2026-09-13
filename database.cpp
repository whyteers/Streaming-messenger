#include "database.h"
#include "crypto.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

int64_t NowSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}


std::string Esc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '\t': o += "\\t"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            default: o += c; break;
        }
    }
    return o;
}

std::string Unesc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == '\\') { o += '\\'; ++i; }
            else if (n == 't') { o += '\t'; ++i; }
            else if (n == 'n') { o += '\n'; ++i; }
            else if (n == 'r') { o += '\r'; ++i; }
            else { o += s[i]; }
        } else o += s[i];
    }
    return o;
}

std::vector<std::string> SplitTab(const std::string& line) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\t') { parts.push_back(cur); cur.clear(); }
        else if (line[i] == '\\' && i + 1 < line.size() &&
                 (line[i+1] == 't' || line[i+1] == '\\' ||
                  line[i+1] == 'n' || line[i+1] == 'r')) {

            cur += line[i]; cur += line[i+1]; ++i;
        } else cur += line[i];
    }
    parts.push_back(cur);
    return parts;
}

int64_t ToI64(const std::string& s, int64_t def = 0) {
    try { return std::stoll(s); } catch (...) { return def; }
}

}

Database& Database::instance() {
    static Database inst;
    return inst;
}

bool Database::init(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    dbPath_ = dbPath;
    if (!loadFromFile()) {

        std::cout << "[DB] Fresh database: " << dbPath_ << std::endl;
    } else {
        std::cout << "[DB] Loaded: " << dbPath_ << " users=" << users_.size()
                  << " rooms=" << rooms_.size() << std::endl;
    }
    return true;
}

bool Database::loadFromFile() {
    users_.clear(); sessions_.clear(); rooms_.clear(); members_.clear();
    nextUserId_ = 1; nextRoomId_ = 1;

    std::ifstream f(dbPath_, std::ios::binary);
    if (!f) return false;

    std::string line;

    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto p = SplitTab(line);
        if (p.empty()) continue;
        const std::string& kind = p[0];
        try {
            if (kind == "NEXT" && p.size() >= 3) {
                nextUserId_ = std::max<int64_t>(1, ToI64(p[1], 1));
                nextRoomId_ = std::max<int64_t>(1, ToI64(p[2], 1));
            } else if (kind == "USER" && p.size() >= 9) {
                User u;
                u.id = ToI64(p[1]); u.username = Unesc(p[2]); u.email = Unesc(p[3]);
                u.passwordHash = Unesc(p[4]); u.displayName = Unesc(p[5]);
                u.avatarUrl = Unesc(p[6]); u.createdAt = ToI64(p[7]);
                u.isActive = p[8] == "1";
                users_.push_back(std::move(u));
                nextUserId_ = std::max(nextUserId_, users_.back().id + 1);
            } else if (kind == "SESSION" && p.size() >= 9) {
                Session s;
                s.userId = ToI64(p[1]); s.tokenHash = Unesc(p[2]);
                s.userAgent = Unesc(p[3]); s.ip = Unesc(p[4]);
                s.remember = p[5] == "1"; s.createdAt = ToI64(p[6]);
                s.expiresAt = ToI64(p[7]); s.revokedAt = ToI64(p[8], -1);
                sessions_.push_back(std::move(s));
            } else if (kind == "ROOM" && p.size() >= 7) {
                Room r;
                r.id = ToI64(p[1]); r.name = Unesc(p[2]); r.description = Unesc(p[3]);
                r.createdBy = ToI64(p[4]); r.createdAt = ToI64(p[5]);
                r.isActive = p[6] == "1";
                rooms_.push_back(std::move(r));
                nextRoomId_ = std::max(nextRoomId_, rooms_.back().id + 1);
            } else if (kind == "MEMBER" && p.size() >= 5) {
                Membership m;
                m.roomId = ToI64(p[1]); m.userId = ToI64(p[2]);
                m.role = Unesc(p[3]); m.joinedAt = ToI64(p[4]);
                members_.push_back(std::move(m));
            }
        } catch (...) {  }
    }
    return true;
}

bool Database::saveToFileLocked() {
    if (dbPath_.empty()) return false;
    std::string tmp = dbPath_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "# StreamService DB v1\n";
        f << "NEXT\t" << nextUserId_ << "\t" << nextRoomId_ << "\n";
        for (const auto& u : users_)
            f << "USER\t" << u.id << "\t" << Esc(u.username) << "\t" << Esc(u.email)
              << "\t" << Esc(u.passwordHash) << "\t" << Esc(u.displayName)
              << "\t" << Esc(u.avatarUrl) << "\t" << u.createdAt
              << "\t" << (u.isActive ? "1" : "0") << "\n";
        for (const auto& s : sessions_)
            f << "SESSION\t" << s.userId << "\t" << Esc(s.tokenHash)
              << "\t" << Esc(s.userAgent) << "\t" << Esc(s.ip)
              << "\t" << (s.remember ? "1" : "0") << "\t" << s.createdAt
              << "\t" << s.expiresAt << "\t" << s.revokedAt << "\n";
        for (const auto& r : rooms_)
            f << "ROOM\t" << r.id << "\t" << Esc(r.name) << "\t" << Esc(r.description)
              << "\t" << r.createdBy << "\t" << r.createdAt
              << "\t" << (r.isActive ? "1" : "0") << "\n";
        for (const auto& m : members_)
            f << "MEMBER\t" << m.roomId << "\t" << m.userId << "\t" << Esc(m.role)
              << "\t" << m.joinedAt << "\n";
        f.flush();
        if (!f) return false;
    }

    std::remove(dbPath_.c_str());
    if (std::rename(tmp.c_str(), dbPath_.c_str()) != 0) return false;
    return true;
}

int64_t Database::createUser(const std::string& username, const std::string& email,
                             const std::string& passwordHash, const std::string& displayName) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string ul = ToLower(username), el = ToLower(email);
    for (const auto& u : users_) {
        if (ToLower(u.username) == ul)
            throw std::runtime_error("username already exists");
        if (!el.empty() && !u.email.empty() && ToLower(u.email) == el)
            throw std::runtime_error("email already exists");
    }
    User u;
    u.id = nextUserId_++;
    u.username = username; u.email = email;
    u.passwordHash = passwordHash;
    u.displayName = displayName.empty() ? username : displayName;
    u.createdAt = NowSeconds(); u.isActive = true;
    users_.push_back(u);
    saveToFileLocked();
    return u.id;
}

std::optional<User> Database::findUserByUsername(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string ul = ToLower(username);
    for (const auto& u : users_)
        if (ToLower(u.username) == ul) return u;
    return std::nullopt;
}

std::optional<User> Database::findUserByEmail(const std::string& email) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string el = ToLower(email);
    for (const auto& u : users_)
        if (ToLower(u.email) == el) return u;
    return std::nullopt;
}

std::optional<User> Database::findUserById(int64_t userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& u : users_)
        if (u.id == userId) return u;
    return std::nullopt;
}

size_t Database::userCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return users_.size();
}

std::string Database::createSession(int64_t userId, bool remember,
                                    const std::string& userAgent, const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string token = Crypto::generateToken(32);
    std::string tokenHash = Crypto::sha256(token);
    int64_t now = NowSeconds();
    int64_t expiresIn = remember ? (30LL * 24 * 3600) : (24LL * 3600);
    Session s;
    s.userId = userId; s.tokenHash = tokenHash;
    s.userAgent = userAgent; s.ip = ip;
    s.remember = remember; s.createdAt = now;
    s.expiresAt = now + expiresIn; s.revokedAt = -1;
    sessions_.push_back(std::move(s));
    saveToFileLocked();
    return token;
}

std::optional<int64_t> Database::validateSession(const std::string& token) {
    if (token.size() < 16) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    std::string h = Crypto::sha256(token);
    int64_t now = NowSeconds();
    for (const auto& s : sessions_) {
        if (s.tokenHash == h && s.revokedAt < 0 && s.expiresAt > now) {

            for (const auto& u : users_)
                if (u.id == s.userId) return u.isActive ? std::optional<int64_t>(u.id) : std::nullopt;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void Database::revokeSession(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string h = Crypto::sha256(token);
    int64_t now = NowSeconds();
    for (auto& s : sessions_)
        if (s.tokenHash == h && s.revokedAt < 0) s.revokedAt = now;
    saveToFileLocked();
}

void Database::revokeAllSessions(int64_t userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = NowSeconds();
    for (auto& s : sessions_)
        if (s.userId == userId && s.revokedAt < 0) s.revokedAt = now;
    saveToFileLocked();
}

int64_t Database::createRoom(int64_t ownerId, const std::string& name,
                             const std::string& description) {
    if (name.empty()) throw std::runtime_error("name required");
    std::lock_guard<std::mutex> lock(mutex_);
    Room r;
    r.id = nextRoomId_++;
    r.name = name; r.description = description;
    r.createdBy = ownerId; r.createdAt = NowSeconds(); r.isActive = true;
    rooms_.push_back(r);
    Membership m;
    m.roomId = r.id; m.userId = ownerId; m.role = "owner"; m.joinedAt = r.createdAt;
    members_.push_back(std::move(m));
    saveToFileLocked();
    return r.id;
}

std::optional<Room> Database::findRoomById(int64_t roomId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : rooms_)
        if (r.id == roomId) return r;
    return std::nullopt;
}

std::vector<Room> Database::getUserRooms(int64_t userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Room> out;
    for (const auto& m : members_) {
        if (m.userId != userId) continue;
        for (const auto& r : rooms_)
            if (r.id == m.roomId && r.isActive) { out.push_back(r); break; }
    }
    std::sort(out.begin(), out.end(),
              [](const Room& a, const Room& b) { return a.createdAt > b.createdAt; });
    return out;
}

std::vector<Room> Database::getAllRooms() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Room> out;
    for (const auto& r : rooms_)
        if (r.isActive) out.push_back(r);
    return out;
}

bool Database::joinRoom(int64_t roomId, int64_t userId, const std::string& role) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool roomOk = false;
    for (const auto& r : rooms_)
        if (r.id == roomId && r.isActive) { roomOk = true; break; }
    if (!roomOk) return false;
    for (const auto& m : members_)
        if (m.roomId == roomId && m.userId == userId) return true;
    Membership m;
    m.roomId = roomId; m.userId = userId;
    m.role = role.empty() ? "member" : role; m.joinedAt = NowSeconds();
    members_.push_back(std::move(m));
    saveToFileLocked();
    return true;
}

bool Database::isRoomMember(int64_t roomId, int64_t userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& m : members_)
        if (m.roomId == roomId && m.userId == userId) return true;
    return false;
}

bool Database::leaveRoom(int64_t roomId, int64_t userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = members_.begin(); it != members_.end(); ++it) {
        if (it->roomId == roomId && it->userId == userId) {
            members_.erase(it);
            saveToFileLocked();
            return true;
        }
    }
    return false;
}

bool Database::deleteRoom(int64_t roomId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : rooms_) {
        if (r.id == roomId) {
            r.isActive = false;
            saveToFileLocked();
            return true;
        }
    }
    return false;
}
