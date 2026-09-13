#ifndef CRYPTO_H
#define CRYPTO_H








#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Crypto {


bool init();


std::string sha256(const std::string& input);
std::string sha256(const uint8_t* data, size_t len);



std::string generateToken(size_t bytesCount = 32);


std::string hashPassword(const std::string& password);



bool verifyPassword(const std::string& passwordHash, const std::string& password) noexcept;

}

#endif
