#include "http/HttpRedisPolicy.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message, int& failures) {
    if (condition) {
        return true;
    }
    std::cerr << message << '\n';
    ++failures;
    return false;
}

}

int main() {
    int failures = 0;

    expect(is_valid_upload_idempotency_key(""),
        "an omitted idempotency key should remain optional", failures);
    expect(is_valid_upload_idempotency_key("abcd-123"),
        "an eight-character key should be accepted", failures);
    expect(is_valid_upload_idempotency_key("550e8400-e29b-41d4_a716.446655440000:v1"),
        "safe UUID-style key characters should be accepted", failures);
    expect(!is_valid_upload_idempotency_key("abc-123"),
        "a seven-character key should be rejected", failures);
    expect(!is_valid_upload_idempotency_key(std::string(129, 'a')),
        "a 129-character key should be rejected", failures);
    expect(!is_valid_upload_idempotency_key("upload key"),
        "spaces should be rejected", failures);
    expect(!is_valid_upload_idempotency_key("upload/../../key"),
        "path separators should be rejected", failures);
    expect(!is_valid_upload_idempotency_key("upload\nkey"),
        "control characters should be rejected", failures);

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 14\r\n\r\n"
        "{\"ok\":true}\n";
    expect(http_response_body(response) == "{\"ok\":true}\n",
        "the exact JSON body should be extracted for idempotent replay", failures);
    expect(http_response_body("HTTP/1.1 500 Error\r\n") == "",
        "a malformed header-only response should not produce a replay body", failures);

    if (failures != 0) {
        return 1;
    }
    std::cout << "HTTP Redis policy test passed\n";
    return 0;
}
