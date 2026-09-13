#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H




#include <cstdint>
#include <string>

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};


HttpResult HttpPost(const std::string& host, int port, const std::string& target,
                    const std::string& jsonBody, const std::string& bearer = "");
HttpResult HttpGet(const std::string& host, int port, const std::string& target,
                   const std::string& bearer = "");
HttpResult HttpDelete(const std::string& host, int port, const std::string& target,
                      const std::string& bearer = "");


bool HttpJsonString(const std::string& body, const std::string& key, std::string& out);
int64_t HttpJsonInt(const std::string& body, const std::string& key, int64_t def = 0);


std::string HttpJsonError(const std::string& body);

#endif
