#include "auth.h"

#include <cassert>
#include <iostream>

int main()
{
    const std::string pwd = "correct horse battery staple";
    const std::string hash = auth::hash_password(pwd);
    assert(!hash.empty());
    assert(auth::verify_password(pwd, hash));
    assert(!auth::verify_password("wrong", hash));

    const std::string secret = "test-secret-key";
    const std::string token = auth::issue_jwt(42, "customer", secret, 60);
    auto payload = auth::verify_jwt(token, secret);
    assert(payload.has_value());
    assert(payload->user_id == 42);
    assert(payload->role == "customer");

    std::cout << "auth tests passed" << std::endl;
    return 0;
}
