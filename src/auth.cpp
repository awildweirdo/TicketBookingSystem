#include "auth.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <ctime>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <cassert>

static std::string hex_encode(const unsigned char *data, size_t len)
{
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    return oss.str();
}

static std::vector<unsigned char> hex_decode(const std::string &hex)
{
    std::vector<unsigned char> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        std::string byte = hex.substr(i, 2);
        unsigned int val = 0;
        std::stringstream ss;
        ss << std::hex << byte;
        ss >> val;
        out.push_back(static_cast<unsigned char>(val));
    }
    return out;
}

std::string auth::hash_password(const std::string &password)
{
    unsigned char salt[16];
    if (RAND_bytes(salt, sizeof(salt)) != 1)
    {
        // fallback to zero salt (unlikely)
        std::memset(salt, 0, sizeof(salt));
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, sizeof(salt));
    SHA256_Update(&ctx, reinterpret_cast<const unsigned char *>(password.data()), password.size());
    SHA256_Final(digest, &ctx);
    return hex_encode(salt, sizeof(salt)) + "$" + hex_encode(digest, sizeof(digest));
}

bool auth::verify_password(const std::string &password, const std::string &hash)
{
    const auto pos = hash.find('$');
    if (pos == std::string::npos)
        return false;
    const std::string salt_hex = hash.substr(0, pos);
    const std::string digest_hex = hash.substr(pos + 1);
    const auto salt = hex_decode(salt_hex);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt.data(), salt.size());
    SHA256_Update(&ctx, reinterpret_cast<const unsigned char *>(password.data()), password.size());
    SHA256_Final(digest, &ctx);
    const std::string computed = hex_encode(digest, sizeof(digest));
    return computed == digest_hex;
}

static std::string base64url_encode(const unsigned char *data, size_t len)
{
    // use OpenSSL EVP_EncodeBlock
    size_t olen = 4 * ((len + 2) / 3);
    std::vector<unsigned char> out(olen + 1);
    int written = EVP_EncodeBlock(out.data(), data, static_cast<int>(len));
    std::string s(reinterpret_cast<char *>(out.data()), written);
    // url-safe and remove padding
    for (auto &c : s)
    {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

static std::string hmac_sha256_base64url(const std::string &key, const std::string &data)
{
    unsigned int len = EVP_MAX_MD_SIZE;
    std::vector<unsigned char> out(len);
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(data.data()), data.size(), out.data(), &len);
    return base64url_encode(out.data(), len);
}

std::string auth::issue_jwt(int user_id, const std::string &role, const std::string &secret, int ttl_seconds)
{
    std::string header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    const long exp = std::time(nullptr) + ttl_seconds;
    std::ostringstream payload;
    payload << "{\"sub\":" << user_id << ",\"role\":\"" << role << "\",\"exp\":" << exp << "}";

    const std::string b64h = base64url_encode(reinterpret_cast<const unsigned char *>(header.data()), header.size());
    const std::string b64p = base64url_encode(reinterpret_cast<const unsigned char *>(payload.str().data()), payload.str().size());
    const std::string signing_input = b64h + "." + b64p;
    const std::string sig = hmac_sha256_base64url(secret, signing_input);
    return signing_input + "." + sig;
}

std::optional<JwtPayload> auth::verify_jwt(const std::string &token, const std::string &secret)
{
    const size_t p1 = token.find('.');
    const size_t p2 = token.find('.', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos)
        return std::nullopt;
    const std::string signing_input = token.substr(0, p2);
    const std::string sig = token.substr(p2 + 1);
    const std::string expected = hmac_sha256_base64url(secret, signing_input);
    if (sig != expected) return std::nullopt;

    // decode payload (simplified parsing, not robust JSON)
    const std::string b64p = token.substr(p1 + 1, p2 - p1 - 1);
    // restore padding
    std::string b64 = b64p;
    while (b64.size() % 4) b64.push_back('=');
    for (auto &c : b64) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    // use EVP_DecodeBlock
    std::vector<unsigned char> out((b64.size()*3)/4 + 1);
    int outlen = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char *>(b64.data()), static_cast<int>(b64.size()));
    if (outlen < 0) return std::nullopt;
    std::string payload(reinterpret_cast<char *>(out.data()), outlen);

    // parse sub, role, exp with naive search
    JwtPayload res;
    auto pos_sub = payload.find("\"sub\":");
    if (pos_sub != std::string::npos)
    {
        pos_sub += 6;
        res.user_id = std::stoi(payload.substr(pos_sub));
    }
    const std::string role_key = "\"role\":\"";
    auto pos_role = payload.find(role_key);
    if (pos_role != std::string::npos)
    {
        pos_role += role_key.size();
        auto end = payload.find('"', pos_role);
        if (end != std::string::npos) res.role = payload.substr(pos_role, end - pos_role);
    }
    auto pos_exp = payload.find("\"exp\":");
    if (pos_exp != std::string::npos)
    {
        pos_exp += 6;
        res.exp = std::stol(payload.substr(pos_exp));
        if (std::time(nullptr) > res.exp) return std::nullopt;
    }
    return res;
}
