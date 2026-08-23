#pragma once

#include <string>
#include <optional>

struct JwtPayload
{
    int user_id{0};
    std::string role;
    long exp{0};
};

namespace auth
{
    std::string hash_password(const std::string &password);
    bool verify_password(const std::string &password, const std::string &hash);

    std::string issue_jwt(int user_id, const std::string &role, const std::string &secret, int ttl_seconds);
    std::optional<JwtPayload> verify_jwt(const std::string &token, const std::string &secret);
}
