#ifndef DATABASE_H
#define DATABASE_H








#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct User {
    int64_t id = 0;
    std::string username;
    std::string email;
    std::string passwordHash;
    std::string displayName;
    std::string avatarUrl;
    int64_t createdAt = 0;
    bool isActive = true;
};

struct Room {
    int64_t id = 0;
    std::string name;
    std::string description;
    int64_t createdBy = 0;
    int64_t createdAt = 0;
    bool isActive = true;
};

class Database {
public:
    static Database& instance();


    bool init(const std::string& dbPath);

    int64_t createUser(const std::string& username, const std::string& email,
                       const std::string& passwordHash, const std::string& displayName);
    std::optional<User> findUserByUsername(const std::string& username);
    std::optional<User> findUserByEmail(const std::string& email);
    std::optional<User> findUserById(int64_t userId);
    size_t userCount();

    std::string createSession(int64_t userId, bool remember,
                              const std::string& userAgent = "",
                              const std::string& ip = "");
    std::optional<int64_t> validateSession(const std::string& token);
    void revokeSession(const std::string& token);
    void revokeAllSessions(int64_t userId);

    int64_t createRoom(int64_t ownerId, const std::string& name,
                       const std::string& description = "");
    std::optional<Room> findRoomById(int64_t roomId);
    std::vector<Room> getUserRooms(int64_t userId);
    std::vector<Room> getAllRooms();
    bool joinRoom(int64_t roomId, int64_t userId, const std::string& role = "member");
    bool isRoomMember(int64_t roomId, int64_t userId);
    bool leaveRoom(int64_t roomId, int64_t userId);
    bool deleteRoom(int64_t roomId);

private:
    Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    struct Session {
        int64_t userId = 0;
        std::string tokenHash;
        std::string userAgent;
        std::string ip;
        bool remember = false;
        int64_t createdAt = 0;
        int64_t expiresAt = 0;
        int64_t revokedAt = -1;
    };

    struct Membership {
        int64_t roomId = 0;
        int64_t userId = 0;
        std::string role = "member";
        int64_t joinedAt = 0;
    };

    bool loadFromFile();
    bool saveToFileLocked();

    std::string dbPath_;
    std::mutex mutex_;
    std::vector<User> users_;
    std::vector<Session> sessions_;
    std::vector<Room> rooms_;
    std::vector<Membership> members_;
    int64_t nextUserId_ = 1;
    int64_t nextRoomId_ = 1;
};

#endif
